/*
 * fused_encode_arm64.S — ARM64 assembly for fused encoder hot loops
 *
 * Targets:
 *   - Mac ARM64 (Apple Silicon) as fast iteration testbed
 *   - GoPro GP3 (Cortex-A78) as production target
 *   - Raspberry Pi 5 (Cortex-A76) as intermediate dev platform
 *
 * Bit-exact replacement of the NEON inner body of
 * unpack_channel_row_decimate_2x2(). Falls back to C for any input
 * that isn't an exact multiple of 4 output samples — the caller
 * passes only the rounded count and runs the existing scalar tail.
 *
 * (C) Copyright 2026 GoPro Inc. Licensed under Apache-2.0 or MIT.
 */

#if defined(__aarch64__) && !defined(FUSED_DISABLE_ASM)

#if defined(__APPLE__)
# define ASYM(name) _##name
#else
# define ASYM(name) name
#endif

/* On ELF we need to declare the symbol explicitly so non-text
 * sections (.eh_frame, .note.GNU-stack) don't get pulled in by gas. */
#if !defined(__APPLE__)
.section .note.GNU-stack,"",%progbits
#endif

/* ================================================================
 * Function: fused_unpack_dec2x2_neon_asm
 *
 * Bit-exact ARM64 NEON implementation of the inner body of
 * unpack_channel_row_decimate_2x2() for the 4-aligned head; the
 * scalar tail in the C caller still runs for any odd remainder.
 *
 * void fused_unpack_dec2x2_neon_asm(
 *     int channel,                 // w0  channel id (0..3)
 *     int is_rggb,                 // w1  bayer pattern flag
 *     const uint16_t *log_tbl,     // x2  log curve LUT (>= 16K u16)
 *     int log_max,                 // w3  max LUT index (= 1<<log_bits - 1)
 *     int32_t mid2,                // w4  2 * midpoint
 *     const uint16_t *row1a,       // x5  Bayer row 1 of pair A
 *     const uint16_t *row2a,       // x6  Bayer row 2 of pair A
 *     const uint16_t *row1b,       // x7  Bayer row 1 of pair B
 *     const uint16_t *row2b,       // [sp+0]  Bayer row 2 of pair B
 *     int32_t *output,             // [sp+8]  output channel row
 *     int ch_width_out_4);         // [sp+16] iterations × 4 = output count
 *
 * Per iteration produces 4 output samples; the C caller is expected
 * to pass the floored-to-4 count and run the scalar tail itself.
 *
 * Techniques applied (from FFmpeg libswscale/libavcodec aarch64):
 *   1. uaddlp + uadalp 4-way reduction (swscale_unscaled_neon.S
 *      `nv12_to_yuv420p` pattern): pair-add widening, then
 *      accumulate the second row's pair-add — collapses the
 *      previous {vpaddlq + vaddq} pair into a tighter dep chain.
 *   2. umov→ldrh→ins direct NEON-lane LUT gather. Skip the C
 *      version's stack scratch entirely: the compiler always spills
 *      the gathered values to the stack and reloads via vld1q, but
 *      ARM64 has a 1-cycle `ins` that lets us populate a NEON
 *      register directly from a GPR. Saves 4 store/load pairs per
 *      iter and eliminates the store→load forwarding window.
 *   3. Software-pipelined input prefetch (videodsp.S pattern):
 *      `prfm pldl1strm` two cache lines ahead on every input row.
 *      The Bayer rows are read once and never again this frame;
 *      `strm` hint tells the cache not to evict useful data.
 *
 * Not used:
 *   - `tbl` for the LUT — table is 32 KB (way too big for q-regs).
 *   - 8-wide unrolling — the inner gather chain saturates the 2
 *     load ports at 4-wide; doubling the unroll quadruples register
 *     pressure without raising throughput.
 *
 * ----------------------------------------------------------------
 * Per-iteration data flow (RGGB, channel 0 = GS):
 *
 *   vld2 row1a → e_a.0, e_a.1   (8 R, 8 G1 lanes)
 *   vld2 row2a → o_a.0, o_a.1   (8 G2, 8 B lanes)
 *   vld2 row1b → e_b.0, e_b.1
 *   vld2 row2b → o_b.0, o_b.1
 *
 *   v_g1_a = umin(e_a.1, v_log_max)    ; v_g2_a = umin(o_a.0, ...)
 *   v_g1_b = umin(e_b.1, v_log_max)    ; v_g2_b = umin(o_b.0, ...)
 *   (for channel != 3 we also need v_x_a, v_x_b from R or B lanes)
 *
 *   For each of the 16 lanes of v_g1, gather via direct
 *   umov-ldrh-ins into v_g1log (u16x8 per row half). Same for v_g2,
 *   v_x. Then 4-way pair-add reduce:
 *
 *     v_g1sum = uaddlp(v_g1log_a)
 *     v_g1sum = uadalp(v_g1sum, v_g1log_b)
 *     vg1     = (v_g1sum + 2) >> 2     ; 4-lane u32, max 16383
 *
 *   Final combine per channel:
 *     ch 0 GS: result = (vg1 + vg2) >> 1
 *     ch 1 RG: vgs = (vg1+vg2)>>1; result = ((vx - vgs) + mid2) >> 1
 *     ch 2 BG: vgs = (vg1+vg2)>>1; result = ((vx - vgs) + mid2) >> 1
 *     ch 3 GD: result = ((vg1 - vg2) + mid2) >> 1
 *
 *   st1 result, [output]
 * ----------------------------------------------------------------
 */
.text
.globl ASYM(fused_unpack_dec2x2_neon_asm)
.p2align 4

ASYM(fused_unpack_dec2x2_neon_asm):
        /* AAPCS64 prologue. We allocate 96 bytes of stack scratch
         * (3 × 32 for {G1, G2, X} indices). Each gather pass writes
         * its own 32-byte slot so they don't conflict if we want to
         * overlap gathers in the future. */
        stp     x29, x30, [sp, #-112]!
        mov     x29, sp
        /* Stack layout (relative to sp):
         *   0..15    x29/x30
         *   16..47   G1 idx scratch (16 u16 = 32 B)
         *   48..79   G2 idx scratch (16 u16 = 32 B)
         *   80..111  X  idx scratch (16 u16 = 32 B)
         *
         * Caller args (original frame, now at sp + 112):
         *   [sp + 112 + 0]  row2b  (x8)
         *   [sp + 112 + 8]  output (x9)
         *   [sp + 112 + 16] count4 (w10)  — already a multiple of 4
         */
        ldr     x8,  [sp, #112]         /* row2b */
        ldr     x9,  [sp, #120]         /* output */
        ldr     w10, [sp, #128]         /* iteration count */

        /* Quick-out: nothing to do. */
        cbz     w10, .Lasm_done

        /* Pre-form the clamp vector v_log_max (u16x8) and the mid2
         * dup (s32x4). v30/v31 will hold them across iters. */
        dup     v30.8h, w3              /* v30 = v_log_max */
        dup     v31.4s, w4              /* v31 = vmid2 */
        movi    v29.4s, #2              /* v29 = (s32)2 — for round before >>2 */

        /* Branch into channel-specific loop. Cheaper than testing
         * inside the loop body. */
        cmp     w0, #0
        b.eq    .Lasm_loop_ch0
        cmp     w0, #1
        b.eq    .Lasm_loop_ch1
        cmp     w0, #2
        b.eq    .Lasm_loop_ch2
        /* channel == 3 */
        b       .Lasm_loop_ch3

/* ----------------------------------------------------------------
 * Macro: GATHER16 — gather 16 log_tbl[lane] values from a u16x8 src
 * Inputs:
 *   src_a, src_b: u16x8 NEON regs holding the 16 indices
 *   dst_a, dst_b: u16x8 NEON regs that will hold the 16 gathered
 *                 LUT values (one per lane)
 *
 * Implementation note: we keep ONE round-trip through the stack
 * for the 16 indices (avoiding 16 serialized NEON→GPR `umov`s — A76
 * has only one issue port for that move), and gather LUT values
 * DIRECTLY INTO NEON via `ldr h, [x2, w<n>, lsl #1]`. Then `mov
 * v.h[lane], h<n>.h[0]` packs them back. This matches the pattern
 * the compiler generates for the C reference; the win has to come
 * from elsewhere. (Kept here to make the assembly drop-in compatible
 * with the C path's data-flow and to provide a place to attack
 * other angles like cross-color interleaving.)
 *
 * Clobbers: w11-w14 (4 GPRs allows 2 independent index-load chains
 * to overlap); v24-v27 as h-reg load destinations.
 * ---------------------------------------------------------------- */
.macro GATHER8_HALF q_src, src, dst, stack_off
        /* Store 8 indices to stack so we can use cheap `ldrh w, [sp,#k]`
         * for each scalar pickup — A76's NEON→GPR pipe is single-issue,
         * but stack ldrh's go through 2 LSU ports.
         * q_src must be the q-reg name matching v_src (e.g. q0 / v0). */
        str     \q_src, [sp, #\stack_off]
        ldrh    w11, [sp, #(\stack_off + 0)]
        ldrh    w12, [sp, #(\stack_off + 2)]
        ldr     h24, [x2, w11, uxtw #1]
        ldr     h25, [x2, w12, uxtw #1]
        ldrh    w13, [sp, #(\stack_off + 4)]
        ldrh    w14, [sp, #(\stack_off + 6)]
        mov     \dst\().h[0], v24.h[0]
        mov     \dst\().h[1], v25.h[0]
        ldr     h24, [x2, w13, uxtw #1]
        ldr     h25, [x2, w14, uxtw #1]
        ldrh    w11, [sp, #(\stack_off + 8)]
        ldrh    w12, [sp, #(\stack_off + 10)]
        mov     \dst\().h[2], v24.h[0]
        mov     \dst\().h[3], v25.h[0]
        ldr     h24, [x2, w11, uxtw #1]
        ldr     h25, [x2, w12, uxtw #1]
        ldrh    w13, [sp, #(\stack_off + 12)]
        ldrh    w14, [sp, #(\stack_off + 14)]
        mov     \dst\().h[4], v24.h[0]
        mov     \dst\().h[5], v25.h[0]
        ldr     h24, [x2, w13, uxtw #1]
        ldr     h25, [x2, w14, uxtw #1]
        mov     \dst\().h[6], v24.h[0]
        mov     \dst\().h[7], v25.h[0]
.endm

.macro GATHER16 q_a, q_b, src_a, src_b, dst_a, dst_b, base_off
        GATHER8_HALF \q_a, \src_a, \dst_a, (\base_off + 0)
        GATHER8_HALF \q_b, \src_b, \dst_b, (\base_off + 16)
.endm

/* ----------------------------------------------------------------
 * Macro: LOAD_AND_CLAMP_ROWS  (computes the 8 G1, G2 (+R or B)
 *                              lanes from one row-pair).
 *
 * Loads vld2 from row1, row2 with post-increment, then picks the
 * G1/G2 (and R or B if requested) lanes per RGGB or GBRG order.
 *
 * Args:
 *   ptr1, ptr2: row pointers (x-regs, advanced by #32 after load)
 *   pat:        immediate 0=GBRG, 1=RGGB
 *   want_x:     0 = only G1/G2 (channel 0 or 3)
 *               1 = also load X = R lanes  (channel 1, RG)
 *               2 = also load X = B lanes  (channel 2, BG)
 *   v_g1, v_g2, v_x: dst u16x8 NEON regs (clamped to v_log_max)
 *
 * Uses v20, v21, v22, v23 as deinterleave scratch.
 * ---------------------------------------------------------------- */
.macro LOAD_AND_CLAMP_ROWS ptr1, ptr2, pat, want_x, v_g1, v_g2, v_x
        ld2     {v20.8h, v21.8h}, [\ptr1], #32  /* even row even/odd cols */
        ld2     {v22.8h, v23.8h}, [\ptr2], #32  /* odd  row even/odd cols */
        prfm    pldl1strm, [\ptr1, #128]
        prfm    pldl1strm, [\ptr2, #128]
        .if \pat == 1   /* RGGB: even-row even=R, even-row odd=G1
                                 odd-row  even=G2, odd-row odd=B */
                umin    \v_g1\().8h, v21.8h, v30.8h
                umin    \v_g2\().8h, v22.8h, v30.8h
                .if \want_x == 1
                        umin    \v_x\().8h, v20.8h, v30.8h     /* R */
                .endif
                .if \want_x == 2
                        umin    \v_x\().8h, v23.8h, v30.8h     /* B */
                .endif
        .else           /* GBRG: even-row even=G1, even-row odd=B
                                 odd-row  even=R,  odd-row odd=G2 */
                umin    \v_g1\().8h, v20.8h, v30.8h
                umin    \v_g2\().8h, v23.8h, v30.8h
                .if \want_x == 1
                        umin    \v_x\().8h, v22.8h, v30.8h     /* R */
                .endif
                .if \want_x == 2
                        umin    \v_x\().8h, v21.8h, v30.8h     /* B */
                .endif
        .endif
.endm

/* ----------------------------------------------------------------
 * Macro: REDUCE4WAY — 4-way log-space avg, (sum + 2) >> 2.
 * In: log_a, log_b: u16x8 NEON regs (gathered LUT values)
 * Out: dst_s4: int32x4 holding the 4 averaged values
 * Uses: v25 scratch (u32x4 sum).
 *
 * uaddlp v25, log_a  ; widening pair-add: u16x8 → u32x4
 * uadalp v25, log_b  ; accumulate-pair-add (same widening) — saves
 *                      one vector op vs vpaddlq+vaddq
 * add    v_dst, v25, v29  ; +2 round
 * ushr   v_dst, v_dst, #2 ; >>2
 * ---------------------------------------------------------------- */
.macro REDUCE4WAY log_a, log_b, dst_s4
        uaddlp  v25.4s, \log_a\().8h
        uadalp  v25.4s, \log_b\().8h
        add     v25.4s, v25.4s, v29.4s
        ushr    \dst_s4\().4s, v25.4s, #2
.endm

/* ================================================================
 * Channel 0 (GS): result = (vg1 + vg2) >> 1
 * ================================================================ */
.Lasm_loop_ch0:
        cmp     w1, #1
        b.ne    .Lasm_loop_ch0_gbrg

.Lasm_loop_ch0_rggb:
        LOAD_AND_CLAMP_ROWS x5, x6, 1, 0, v0, v1, v2
        LOAD_AND_CLAMP_ROWS x7, x8, 1, 0, v3, v4, v5
        /* v0=G1_a, v1=G2_a, v3=G1_b, v4=G2_b (each u16x8) */
        GATHER16 q0, q3, v0, v3, v8, v9, 16         /* v8/v9 = log[G1_a]/log[G1_b] */
        GATHER16 q1, q4, v1, v4, v10, v11, 48       /* v10/v11 = log[G2_a]/log[G2_b] */
        REDUCE4WAY v8, v9, v16          /* v16 = vg1 (4 × s32) */
        REDUCE4WAY v10, v11, v17        /* v17 = vg2 */
        add     v18.4s, v16.4s, v17.4s
        sshr    v18.4s, v18.4s, #1
        st1     {v18.4s}, [x9], #16
        subs    w10, w10, #4
        b.gt    .Lasm_loop_ch0_rggb
        b       .Lasm_done

.Lasm_loop_ch0_gbrg:
        LOAD_AND_CLAMP_ROWS x5, x6, 0, 0, v0, v1, v2
        LOAD_AND_CLAMP_ROWS x7, x8, 0, 0, v3, v4, v5
        GATHER16 q0, q3, v0, v3, v8, v9, 16
        GATHER16 q1, q4, v1, v4, v10, v11, 48
        REDUCE4WAY v8, v9, v16
        REDUCE4WAY v10, v11, v17
        add     v18.4s, v16.4s, v17.4s
        sshr    v18.4s, v18.4s, #1
        st1     {v18.4s}, [x9], #16
        subs    w10, w10, #4
        b.gt    .Lasm_loop_ch0_gbrg
        b       .Lasm_done

/* ================================================================
 * Channel 1 (RG): vgs = (vg1+vg2)>>1; result = ((vx - vgs) + mid2)>>1
 * vx loaded from R lanes
 * ================================================================ */
.Lasm_loop_ch1:
        cmp     w1, #1
        b.ne    .Lasm_loop_ch1_gbrg

.Lasm_loop_ch1_rggb:
        LOAD_AND_CLAMP_ROWS x5, x6, 1, 1, v0, v1, v2
        LOAD_AND_CLAMP_ROWS x7, x8, 1, 1, v3, v4, v5
        GATHER16 q0, q3, v0, v3, v8,  v9, 16
        GATHER16 q1, q4, v1, v4, v10, v11, 48
        GATHER16 q2, q5, v2, v5, v12, v13, 80
        REDUCE4WAY v8,  v9,  v16        /* vg1 */
        REDUCE4WAY v10, v11, v17        /* vg2 */
        REDUCE4WAY v12, v13, v18        /* vx (R) */
        add     v19.4s, v16.4s, v17.4s  /* vg1+vg2 */
        sshr    v19.4s, v19.4s, #1      /* vgs = >>1 */
        sub     v18.4s, v18.4s, v19.4s  /* vx - vgs */
        add     v18.4s, v18.4s, v31.4s  /* + mid2 */
        sshr    v18.4s, v18.4s, #1      /* >>1 */
        st1     {v18.4s}, [x9], #16
        subs    w10, w10, #4
        b.gt    .Lasm_loop_ch1_rggb
        b       .Lasm_done

.Lasm_loop_ch1_gbrg:
        LOAD_AND_CLAMP_ROWS x5, x6, 0, 1, v0, v1, v2
        LOAD_AND_CLAMP_ROWS x7, x8, 0, 1, v3, v4, v5
        GATHER16 q0, q3, v0, v3, v8,  v9, 16
        GATHER16 q1, q4, v1, v4, v10, v11, 48
        GATHER16 q2, q5, v2, v5, v12, v13, 80
        REDUCE4WAY v8,  v9,  v16
        REDUCE4WAY v10, v11, v17
        REDUCE4WAY v12, v13, v18
        add     v19.4s, v16.4s, v17.4s
        sshr    v19.4s, v19.4s, #1
        sub     v18.4s, v18.4s, v19.4s
        add     v18.4s, v18.4s, v31.4s
        sshr    v18.4s, v18.4s, #1
        st1     {v18.4s}, [x9], #16
        subs    w10, w10, #4
        b.gt    .Lasm_loop_ch1_gbrg
        b       .Lasm_done

/* ================================================================
 * Channel 2 (BG): like ch1 but vx from B lanes
 * ================================================================ */
.Lasm_loop_ch2:
        cmp     w1, #1
        b.ne    .Lasm_loop_ch2_gbrg

.Lasm_loop_ch2_rggb:
        LOAD_AND_CLAMP_ROWS x5, x6, 1, 2, v0, v1, v2
        LOAD_AND_CLAMP_ROWS x7, x8, 1, 2, v3, v4, v5
        GATHER16 q0, q3, v0, v3, v8,  v9, 16
        GATHER16 q1, q4, v1, v4, v10, v11, 48
        GATHER16 q2, q5, v2, v5, v12, v13, 80
        REDUCE4WAY v8,  v9,  v16
        REDUCE4WAY v10, v11, v17
        REDUCE4WAY v12, v13, v18
        add     v19.4s, v16.4s, v17.4s
        sshr    v19.4s, v19.4s, #1
        sub     v18.4s, v18.4s, v19.4s
        add     v18.4s, v18.4s, v31.4s
        sshr    v18.4s, v18.4s, #1
        st1     {v18.4s}, [x9], #16
        subs    w10, w10, #4
        b.gt    .Lasm_loop_ch2_rggb
        b       .Lasm_done

.Lasm_loop_ch2_gbrg:
        LOAD_AND_CLAMP_ROWS x5, x6, 0, 2, v0, v1, v2
        LOAD_AND_CLAMP_ROWS x7, x8, 0, 2, v3, v4, v5
        GATHER16 q0, q3, v0, v3, v8,  v9, 16
        GATHER16 q1, q4, v1, v4, v10, v11, 48
        GATHER16 q2, q5, v2, v5, v12, v13, 80
        REDUCE4WAY v8,  v9,  v16
        REDUCE4WAY v10, v11, v17
        REDUCE4WAY v12, v13, v18
        add     v19.4s, v16.4s, v17.4s
        sshr    v19.4s, v19.4s, #1
        sub     v18.4s, v18.4s, v19.4s
        add     v18.4s, v18.4s, v31.4s
        sshr    v18.4s, v18.4s, #1
        st1     {v18.4s}, [x9], #16
        subs    w10, w10, #4
        b.gt    .Lasm_loop_ch2_gbrg
        b       .Lasm_done

/* ================================================================
 * Channel 3 (GD): result = ((vg1 - vg2) + mid2) >> 1
 * ================================================================ */
.Lasm_loop_ch3:
        cmp     w1, #1
        b.ne    .Lasm_loop_ch3_gbrg

.Lasm_loop_ch3_rggb:
        LOAD_AND_CLAMP_ROWS x5, x6, 1, 0, v0, v1, v2
        LOAD_AND_CLAMP_ROWS x7, x8, 1, 0, v3, v4, v5
        GATHER16 q0, q3, v0, v3, v8, v9, 16
        GATHER16 q1, q4, v1, v4, v10, v11, 48
        REDUCE4WAY v8, v9, v16
        REDUCE4WAY v10, v11, v17
        sub     v18.4s, v16.4s, v17.4s
        add     v18.4s, v18.4s, v31.4s
        sshr    v18.4s, v18.4s, #1
        st1     {v18.4s}, [x9], #16
        subs    w10, w10, #4
        b.gt    .Lasm_loop_ch3_rggb
        b       .Lasm_done

.Lasm_loop_ch3_gbrg:
        LOAD_AND_CLAMP_ROWS x5, x6, 0, 0, v0, v1, v2
        LOAD_AND_CLAMP_ROWS x7, x8, 0, 0, v3, v4, v5
        GATHER16 q0, q3, v0, v3, v8, v9, 16
        GATHER16 q1, q4, v1, v4, v10, v11, 48
        REDUCE4WAY v8, v9, v16
        REDUCE4WAY v10, v11, v17
        sub     v18.4s, v16.4s, v17.4s
        add     v18.4s, v18.4s, v31.4s
        sshr    v18.4s, v18.4s, #1
        st1     {v18.4s}, [x9], #16
        subs    w10, w10, #4
        b.gt    .Lasm_loop_ch3_gbrg
        b       .Lasm_done

.Lasm_done:
        mov     w0, #0          /* success */
        ldp     x29, x30, [sp], #112
        ret

#endif /* __aarch64__ && !FUSED_DISABLE_ASM */
