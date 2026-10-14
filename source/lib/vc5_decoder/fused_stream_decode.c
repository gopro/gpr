/*! @file fused_stream_decode.c
 *
 *  @brief Fused streaming decoder — row-strip parallel band → wavelet → color
 *         pipeline that mirrors the encoder's Pass 1 fused design.
 *
 *  Band entropy decoding completes first. Each row-strip worker then
 *  reconstructs wavelet samples and converts them directly to Bayer output,
 *  avoiding full-frame intermediate channel buffers. The reference decoder
 *  remains available for comparison; actual throughput depends on the profile
 *  and target platform.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "headers.h"
#include "ans_joint.h"
#include "fused_decode.h"
#include "../vc5_encoder/fused_encode.h"
#include "logcurve.h"
#include "inverse.h"
#include "dequantize.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* =========================================================================
   ARCHITECTURE OVERVIEW
   =========================================================================

   Decode flow:

       1) band_decode (4 channels parallel, unchanged from current decoder)
          rANS state chain doesn't naturally stream, so this stays as the
          "decode all tokens into SoA arrays, then scatter to band buffers"
          design. Wall ~38-48 ms on Pi 5.

       2) FUSED STREAM (4 row-strips parallel — the new piece):
          For each output row pair (2*by, 2*by+1) in this strip:
            a. wavelet inverse for each of 4 channels using LL/LH/HL/HH
               rows in [by-1, by+1] window (3-row LH sliding cache)
            b. color transform reads the 4 channel rows and writes 4
               Bayer rows directly (no intermediate channel buffer)

       Each strip thread maintains its own per-channel LH dequant cache.
       No cross-strip sync inside the loop. Strip boundaries handled by
       reading band rows from the global bands array (band_decode has
       completed before any strip starts).

   What's preserved:
       - Band decode stays as the existing parallel-per-channel runner
       - HP-synth (when enabled) inserts BEFORE the fused stream (already
         per-channel parallel)
       - All quant/dequant logic uses the existing DequantizeBandRow16s
       - Color transform math is identical to fused_color_runner in
         fused_decode.c

   What changes:
       - InvertSpatialQuantDescale16s is no longer called whole-channel
       - The 4-channel-parallel wavelet_inv is replaced by 4 row-strip
         workers that each do all 4 channels of wavelet + color
       - Intermediate "channels[ch]" full-buffer is gone (rows are
         live in per-thread scratch only)

   ========================================================================= */


/* Forward declarations from inverse.c — internal but reused inline here.
   We don't actually call InvertHorizontalDescale16s row-by-row directly
   because it's already a stand-alone API; we inline its logic.

   The decoder uses descale=2 → descale_shift=1, which matches the
   single-level + LL fused path. */

/* Helper: dequantize one band row (returns through `out`).
   For raw-mode quant (q<0): out[i] = sign(in[i]) * abs(in[i]) * (-q)
   For companded quant (q>0): out[i] = sign(uc) * abs(uc) * q where
   uc = UncompandedValueFast(in[i]). The fused encoder uses negative
   quants exclusively for the single-level+LL path. We just delegate
   to the existing DequantizeBandRow16s which already handles both
   modes (scalar + NEON inside). */
static inline void dequant_band_row(PIXEL *in, PIXEL *out, int width, QUANT q)
{
    DequantizeBandRow16s(in, width, q, out);
}


/* --------------------------------------------------------------------
   invert_row_pair_one_channel

   Produces 2 channel rows (even_out, odd_out) of width = ch_w (=2*bw),
   from a 3-row window of LL/LH bands and a 1-row HL/HH band, all in
   *dequantized* form (no quant divisors here — caller dequantizes).

   Matches InvertSpatialQuantDescale16s's per-row body with descale=2.
   Boundaries:
       by == 0           → top-border filter (11, -4, +1 / 5, +4, -1)
       by == bh-1        → bottom-border filter (5, +4, -1 / 11, -4, +1
                                                 referenced to row 2 down)
       interior          → middle filter (+1, +0, -1 with /8 + row1)

   Inputs:
       lh0, lh1, lh2 — dequantized LH rows at by-1, by, by+1 (replicated
                       at strip boundaries by caller)
       hl_dq, hh_dq  — dequantized HL/HH at current row by
       ll_pitch_pix  — pitch (in PIXEL) of the lowlow row pointer
       ll0/ll1/ll2   — pointers to LL rows at by-1, by, by+1 (replicated
                       at strip boundaries by caller — NOT dequantized;
                       LL is raw — same convention as the existing
                       inverse fn, which uses lowlow_band straight from
                       the bands array, only dequantizing the HP bands)
   Outputs:
       even_out, odd_out — 2 rows of ch_w PIXELs each

   Scratch (caller-allocated, sized for one row of bw int32):
       even_lp, odd_lp, even_hp, odd_hp — vertical-pass outputs

   The vertical pass produces 4 intermediate rows
   {even_lp, odd_lp, even_hp, odd_hp} each at width bw, then the
   horizontal pass on (even_lp, even_hp) → even_out and (odd_lp, odd_hp)
   → odd_out, each at width ch_w = 2*bw.

   The filter arithmetic matches the reference implementation in inverse.c.
   -------------------------------------------------------------------- */
static const int32_t INV_ROUNDING = 4;
/* INV_DESCALE_SHIFT must be a true constant (used by vshlq_n_s32 immediate).
   descale=2 → shift left by 1. */
#define INV_DESCALE_SHIFT 1

/* Inline horizontal inverse with descale, matching InvertHorizontalDescale16s.
   NEON 4-wide on interior columns (column ∈ [1, last_column-3]); scalar
   borders + scalar tail. Constant descale_shift=1 lets us use vshlq_n_s32
   instead of a variable-shift vector, saving one register and a load. */
static inline void invert_horizontal_descale_row(
    const PIXEL *lp, const PIXEL *hp, PIXEL *out,
    int input_width, int output_width)
{
    const int last_column = input_width - 1;
    int column = 0;
    int32_t even, odd;

    /* Left border */
    even = 11 * lp[0] - 4 * lp[1] + lp[2] + INV_ROUNDING;
    even = even >> 3;
    even = (even + hp[0]) << INV_DESCALE_SHIFT;

    odd = 5 * lp[0] + 4 * lp[1] - lp[2] + INV_ROUNDING;
    odd = odd >> 3;
    odd = (odd - hp[0]) << INV_DESCALE_SHIFT;

    out[0] = (PIXEL)even;
    out[1] = (PIXEL)odd;
    column = 1;

#if defined(__ARM_NEON)
    {
        const int32x4_t four = vdupq_n_s32(INV_ROUNDING);
        /* 4-wide interior. Match InvertHorizontalDescale16s NEON path,
           but with constant descale_shift=1. */
        for (; column + 3 < last_column; column += 4) {
            int32x4_t lp_left   = vld1q_s32(&lp[column - 1]);
            int32x4_t lp_center = vld1q_s32(&lp[column]);
            int32x4_t lp_right  = vld1q_s32(&lp[column + 1]);
            int32x4_t hp_center = vld1q_s32(&hp[column]);

            int32x4_t diff_e = vsubq_s32(lp_left, lp_right);
            diff_e = vaddq_s32(diff_e, four);
            diff_e = vshrq_n_s32(diff_e, 3);
            int32x4_t even_v = vaddq_s32(vaddq_s32(diff_e, lp_center), hp_center);
            even_v = vshlq_n_s32(even_v, INV_DESCALE_SHIFT);

            int32x4_t diff_o = vsubq_s32(lp_right, lp_left);
            diff_o = vaddq_s32(diff_o, four);
            diff_o = vshrq_n_s32(diff_o, 3);
            int32x4_t odd_v = vsubq_s32(vaddq_s32(diff_o, lp_center), hp_center);
            odd_v = vshlq_n_s32(odd_v, INV_DESCALE_SHIFT);

            int32x4x2_t interleaved = { .val = { even_v, odd_v } };
            vst2q_s32(&out[2 * column], interleaved);
        }
    }
#endif

    /* Scalar middle columns (tail) */
    for (; column < last_column; column++) {
        even = lp[column - 1] - lp[column + 1] + 4;
        even >>= 3;
        even += lp[column];
        even = (even + hp[column]) << INV_DESCALE_SHIFT;
        out[2 * column] = (PIXEL)even;

        odd = -lp[column - 1] + lp[column + 1] + 4;
        odd >>= 3;
        odd += lp[column];
        odd = (odd - hp[column]) << INV_DESCALE_SHIFT;
        out[2 * column + 1] = (PIXEL)odd;
    }

    /* Right border */
    even = 5 * lp[column] + 4 * lp[column - 1] - lp[column - 2] + INV_ROUNDING;
    even = even >> 3;
    even = (even + hp[column]) << INV_DESCALE_SHIFT;
    out[2 * column] = (PIXEL)even;

    if (2 * column + 1 < output_width) {
        odd = 11 * lp[column] - 4 * lp[column - 1] + lp[column - 2] + INV_ROUNDING;
        odd = odd >> 3;
        odd = (odd - hp[column]) << INV_DESCALE_SHIFT;
        out[2 * column + 1] = (PIXEL)odd;
    }
}

/* Vertical-pass border helpers, mirroring the InvertSpatialQuantDescale16s
   "first row" and "last row" code paths. The 'middle' helper handles all
   interior rows (by ∈ [1, bh-2]). */

/* Top border: by == 0. The 3-row window for LL/LH is rows {0, 1, 2}.
   For LH the dequantized rows are already in lh0/lh1/lh2 — for LL we
   pass pointers to raw rows {0, 1, 2}. */
static inline void inv_vert_top(
    const PIXEL *ll0, const PIXEL *ll1, const PIXEL *ll2,
    const PIXEL *lh0, const PIXEL *lh1, const PIXEL *lh2,
    const PIXEL *hl_dq, const PIXEL *hh_dq,
    PIXEL *even_lp, PIXEL *odd_lp, PIXEL *even_hp, PIXEL *odd_hp,
    int bw)
{
    int x = 0;
#if defined(__ARM_NEON)
    const int32x4_t four = vdupq_n_s32(INV_ROUNDING);
    const int bw_m4 = (bw / 4) * 4;
    for (; x < bw_m4; x += 4) {
        int32x4_t a0 = vld1q_s32(&ll0[x]);
        int32x4_t a1 = vld1q_s32(&ll1[x]);
        int32x4_t a2 = vld1q_s32(&ll2[x]);
        int32x4_t b0 = vld1q_s32(&lh0[x]);
        int32x4_t b1 = vld1q_s32(&lh1[x]);
        int32x4_t b2 = vld1q_s32(&lh2[x]);
        int32x4_t hl = vld1q_s32(&hl_dq[x]);
        int32x4_t hh = vld1q_s32(&hh_dq[x]);

        /* LL: even = (11*a0 - 4*a1 + a2 + 4) >> 3 + hl, then >>1
              odd  = (5*a0  + 4*a1 - a2 + 4) >> 3 - hl, then >>1 */
        int32x4_t e_acc = vmlaq_n_s32(a2, a0, 11);   /* a0*11 + a2 */
        e_acc = vmlsq_n_s32(e_acc, a1, 4);            /* - a1*4 */
        e_acc = vaddq_s32(e_acc, four);
        e_acc = vshrq_n_s32(e_acc, 3);
        int32x4_t e_lp = vshrq_n_s32(vaddq_s32(e_acc, hl), 1);
        vst1q_s32(&even_lp[x], e_lp);

        int32x4_t o_acc = vmlaq_n_s32(vmulq_n_s32(a0, 5), a1, 4); /* 5*a0 + 4*a1 */
        o_acc = vsubq_s32(o_acc, a2);
        o_acc = vaddq_s32(o_acc, four);
        o_acc = vshrq_n_s32(o_acc, 3);
        int32x4_t o_lp = vshrq_n_s32(vsubq_s32(o_acc, hl), 1);
        vst1q_s32(&odd_lp[x], o_lp);

        /* LH side, same shape with b's and hh */
        int32x4_t e_acc2 = vmlaq_n_s32(b2, b0, 11);
        e_acc2 = vmlsq_n_s32(e_acc2, b1, 4);
        e_acc2 = vaddq_s32(e_acc2, four);
        e_acc2 = vshrq_n_s32(e_acc2, 3);
        int32x4_t e_hp = vshrq_n_s32(vaddq_s32(e_acc2, hh), 1);
        vst1q_s32(&even_hp[x], e_hp);

        int32x4_t o_acc2 = vmlaq_n_s32(vmulq_n_s32(b0, 5), b1, 4);
        o_acc2 = vsubq_s32(o_acc2, b2);
        o_acc2 = vaddq_s32(o_acc2, four);
        o_acc2 = vshrq_n_s32(o_acc2, 3);
        int32x4_t o_hp = vshrq_n_s32(vsubq_s32(o_acc2, hh), 1);
        vst1q_s32(&odd_hp[x], o_hp);
    }
#endif
    for (; x < bw; x++) {
        int32_t even, odd;

        /* Left bands (LL + HL) — top border */
        even = 11 * ll0[x] - 4 * ll1[x] + ll2[x] + INV_ROUNDING;
        even = even >> 3;
        even += hl_dq[x];
        even = even >> 1;
        even_lp[x] = (PIXEL)even;

        odd = 5 * ll0[x] + 4 * ll1[x] - ll2[x] + INV_ROUNDING;
        odd = odd >> 3;
        odd -= hl_dq[x];
        odd = odd >> 1;
        odd_lp[x] = (PIXEL)odd;

        /* Right bands (LH + HH) — top border */
        even = 11 * lh0[x] - 4 * lh1[x] + lh2[x] + INV_ROUNDING;
        even = even >> 3;
        even += hh_dq[x];
        even = even >> 1;
        even_hp[x] = (PIXEL)even;

        odd = 5 * lh0[x] + 4 * lh1[x] - lh2[x] + INV_ROUNDING;
        odd = odd >> 3;
        odd -= hh_dq[x];
        odd = odd >> 1;
        odd_hp[x] = (PIXEL)odd;
    }
}

/* Middle (interior): by ∈ [1, bh-2]. 3-row window centered at by.
   NEON 4-wide; mirrors InvertVerticalMiddle4Descale_NEON in inverse.c but
   processes the LL-side and LH-side together (8 vector ops per pixel pair).
   Hot path: bh-2 ≈ 688 rows of interior per channel × 4 channels × bw=1035
   = ~2.85 M pixel-ops per frame. */
static inline void inv_vert_mid(
    const PIXEL *ll_m1, const PIXEL *ll_0, const PIXEL *ll_p1,
    const PIXEL *lh_m1, const PIXEL *lh_0, const PIXEL *lh_p1,
    const PIXEL *hl_dq, const PIXEL *hh_dq,
    PIXEL *even_lp, PIXEL *odd_lp, PIXEL *even_hp, PIXEL *odd_hp,
    int bw)
{
    int x = 0;
#if defined(__ARM_NEON)
    const int32x4_t four = vdupq_n_s32(4);
    const int bw_m4 = (bw / 4) * 4;
    for (; x < bw_m4; x += 4) {
        /* Load 3-row window of LL and LH plus HL/HH dequant rows */
        int32x4_t llm = vld1q_s32(&ll_m1[x]);
        int32x4_t ll0 = vld1q_s32(&ll_0[x]);
        int32x4_t llp = vld1q_s32(&ll_p1[x]);
        int32x4_t lhm = vld1q_s32(&lh_m1[x]);
        int32x4_t lh0 = vld1q_s32(&lh_0[x]);
        int32x4_t lhp = vld1q_s32(&lh_p1[x]);
        int32x4_t hl  = vld1q_s32(&hl_dq[x]);
        int32x4_t hh  = vld1q_s32(&hh_dq[x]);

        /* LL even/odd */
        int32x4_t diff_e = vshrq_n_s32(vaddq_s32(vsubq_s32(llm, llp), four), 3);
        int32x4_t e_lp = vshrq_n_s32(vaddq_s32(vaddq_s32(diff_e, ll0), hl), 1);
        int32x4_t diff_o = vshrq_n_s32(vaddq_s32(vsubq_s32(llp, llm), four), 3);
        int32x4_t o_lp = vshrq_n_s32(vsubq_s32(vaddq_s32(diff_o, ll0), hl), 1);
        vst1q_s32(&even_lp[x], e_lp);
        vst1q_s32(&odd_lp[x],  o_lp);

        /* LH even/odd */
        int32x4_t diff_e2 = vshrq_n_s32(vaddq_s32(vsubq_s32(lhm, lhp), four), 3);
        int32x4_t e_hp = vshrq_n_s32(vaddq_s32(vaddq_s32(diff_e2, lh0), hh), 1);
        int32x4_t diff_o2 = vshrq_n_s32(vaddq_s32(vsubq_s32(lhp, lhm), four), 3);
        int32x4_t o_hp = vshrq_n_s32(vsubq_s32(vaddq_s32(diff_o2, lh0), hh), 1);
        vst1q_s32(&even_hp[x], e_hp);
        vst1q_s32(&odd_hp[x],  o_hp);
    }
#endif
    /* Scalar tail */
    for (; x < bw; x++) {
        int32_t even, odd;

        /* Left bands (LL + HL) — middle row */
        even = ll_m1[x] - ll_p1[x] + 4;
        even >>= 3;
        even += ll_0[x];
        even += hl_dq[x];
        even = even >> 1;
        even_lp[x] = (PIXEL)even;

        odd = ll_p1[x] - ll_m1[x] + 4;
        odd >>= 3;
        odd += ll_0[x];
        odd -= hl_dq[x];
        odd = odd >> 1;
        odd_lp[x] = (PIXEL)odd;

        /* Right bands (LH + HH) — middle row */
        even = lh_m1[x] - lh_p1[x] + 4;
        even >>= 3;
        even += lh_0[x];
        even += hh_dq[x];
        even = even >> 1;
        even_hp[x] = (PIXEL)even;

        odd = lh_p1[x] - lh_m1[x] + 4;
        odd >>= 3;
        odd += lh_0[x];
        odd -= hh_dq[x];
        odd = odd >> 1;
        odd_hp[x] = (PIXEL)odd;
    }
}

/* Bottom border: by == bh-1. 3-row window {bh-3, bh-2, bh-1}. Note the
   ordering convention from inverse.c — the "current" row pointer is
   advanced to row bh-1 and references row[0], row[-1], row[-2]. To keep
   our caller's life easier we receive {ll_m2, ll_m1, ll_0} = {bh-3, bh-2,
   bh-1} directly, and similarly for LH. */
static inline void inv_vert_bot(
    const PIXEL *ll_m2, const PIXEL *ll_m1, const PIXEL *ll_0,
    const PIXEL *lh_m2, const PIXEL *lh_m1, const PIXEL *lh_0,
    const PIXEL *hl_dq, const PIXEL *hh_dq,
    PIXEL *even_lp, PIXEL *odd_lp, PIXEL *even_hp, PIXEL *odd_hp,
    int bw)
{
    int x = 0;
#if defined(__ARM_NEON)
    const int32x4_t four = vdupq_n_s32(INV_ROUNDING);
    const int bw_m4 = (bw / 4) * 4;
    for (; x < bw_m4; x += 4) {
        int32x4_t a0 = vld1q_s32(&ll_m2[x]);
        int32x4_t a1 = vld1q_s32(&ll_m1[x]);
        int32x4_t a2 = vld1q_s32(&ll_0[x]);
        int32x4_t b0 = vld1q_s32(&lh_m2[x]);
        int32x4_t b1 = vld1q_s32(&lh_m1[x]);
        int32x4_t b2 = vld1q_s32(&lh_0[x]);
        int32x4_t hl = vld1q_s32(&hl_dq[x]);
        int32x4_t hh = vld1q_s32(&hh_dq[x]);

        /* LL: even = (5*a2 + 4*a1 - a0 + 4) >> 3 + hl, then >>1
              odd  = (11*a2 - 4*a1 + a0 + 4) >> 3 - hl, then >>1 */
        int32x4_t e_acc = vmlaq_n_s32(vmulq_n_s32(a2, 5), a1, 4);
        e_acc = vsubq_s32(e_acc, a0);
        e_acc = vaddq_s32(e_acc, four);
        e_acc = vshrq_n_s32(e_acc, 3);
        int32x4_t e_lp = vshrq_n_s32(vaddq_s32(e_acc, hl), 1);
        vst1q_s32(&even_lp[x], e_lp);

        int32x4_t o_acc = vmlaq_n_s32(a0, a2, 11);
        o_acc = vmlsq_n_s32(o_acc, a1, 4);
        o_acc = vaddq_s32(o_acc, four);
        o_acc = vshrq_n_s32(o_acc, 3);
        int32x4_t o_lp = vshrq_n_s32(vsubq_s32(o_acc, hl), 1);
        vst1q_s32(&odd_lp[x], o_lp);

        /* LH side */
        int32x4_t e_acc2 = vmlaq_n_s32(vmulq_n_s32(b2, 5), b1, 4);
        e_acc2 = vsubq_s32(e_acc2, b0);
        e_acc2 = vaddq_s32(e_acc2, four);
        e_acc2 = vshrq_n_s32(e_acc2, 3);
        int32x4_t e_hp = vshrq_n_s32(vaddq_s32(e_acc2, hh), 1);
        vst1q_s32(&even_hp[x], e_hp);

        int32x4_t o_acc2 = vmlaq_n_s32(b0, b2, 11);
        o_acc2 = vmlsq_n_s32(o_acc2, b1, 4);
        o_acc2 = vaddq_s32(o_acc2, four);
        o_acc2 = vshrq_n_s32(o_acc2, 3);
        int32x4_t o_hp = vshrq_n_s32(vsubq_s32(o_acc2, hh), 1);
        vst1q_s32(&odd_hp[x], o_hp);
    }
#endif
    for (; x < bw; x++) {
        int32_t even, odd;

        /* Left bands (LL + HL) — bottom border */
        even = 5 * ll_0[x] + 4 * ll_m1[x] - ll_m2[x] + INV_ROUNDING;
        even = even >> 3;
        even += hl_dq[x];
        even = even >> 1;
        even_lp[x] = (PIXEL)even;

        odd = 11 * ll_0[x] - 4 * ll_m1[x] + ll_m2[x] + INV_ROUNDING;
        odd = odd >> 3;
        odd -= hl_dq[x];
        odd = odd >> 1;
        odd_lp[x] = (PIXEL)odd;

        /* Right bands (LH + HH) — bottom border */
        even = 5 * lh_0[x] + 4 * lh_m1[x] - lh_m2[x] + INV_ROUNDING;
        even = even >> 3;
        even += hh_dq[x];
        even = even >> 1;
        even_hp[x] = (PIXEL)even;

        odd = 11 * lh_0[x] - 4 * lh_m1[x] + lh_m2[x] + INV_ROUNDING;
        odd = odd >> 3;
        odd -= hh_dq[x];
        odd = odd >> 1;
        odd_hp[x] = (PIXEL)odd;
    }
}


/* ----- HP-zero fast-path variants -----
   When LH/HL/HH bands are all-zero, the inverse wavelet math collapses:
     even_lp = ((ll_m1 - ll_p1 + 4) >> 3 + ll_0) >> 1   // no HL
     odd_lp  = ((ll_p1 - ll_m1 + 4) >> 3 + ll_0) >> 1
     even_hp = ((lh_m1 - lh_p1 + 4) >> 3 + lh_0) >> 1 = 0   (since lh = 0)
     odd_hp  = 0
   So even_hp/odd_hp are 0 throughout, and the horizontal pass becomes
     out[2*col]   = (((lp[col-1] - lp[col+1] + 4) >> 3) + lp[col]) << 1
     out[2*col+1] = (((lp[col+1] - lp[col-1] + 4) >> 3) + lp[col]) << 1
   (with appropriate border handling) — i.e. an LP-only 2x upsample.
   We can also skip writing even_hp/odd_hp entirely. */

static inline void inv_vert_mid_hpzero(
    const PIXEL *ll_m1, const PIXEL *ll_0, const PIXEL *ll_p1,
    PIXEL *even_lp, PIXEL *odd_lp,
    int bw)
{
    int x = 0;
#if defined(__ARM_NEON)
    const int32x4_t four = vdupq_n_s32(4);
    const int bw_m4 = (bw / 4) * 4;
    for (; x < bw_m4; x += 4) {
        int32x4_t llm = vld1q_s32(&ll_m1[x]);
        int32x4_t ll0 = vld1q_s32(&ll_0[x]);
        int32x4_t llp = vld1q_s32(&ll_p1[x]);

        int32x4_t diff_e = vshrq_n_s32(vaddq_s32(vsubq_s32(llm, llp), four), 3);
        int32x4_t e_lp = vshrq_n_s32(vaddq_s32(diff_e, ll0), 1);
        int32x4_t diff_o = vshrq_n_s32(vaddq_s32(vsubq_s32(llp, llm), four), 3);
        int32x4_t o_lp = vshrq_n_s32(vaddq_s32(diff_o, ll0), 1);
        vst1q_s32(&even_lp[x], e_lp);
        vst1q_s32(&odd_lp[x],  o_lp);
    }
#endif
    for (; x < bw; x++) {
        int32_t even, odd;
        even = ll_m1[x] - ll_p1[x] + 4;
        even >>= 3;
        even += ll_0[x];
        even = even >> 1;
        even_lp[x] = (PIXEL)even;

        odd = ll_p1[x] - ll_m1[x] + 4;
        odd >>= 3;
        odd += ll_0[x];
        odd = odd >> 1;
        odd_lp[x] = (PIXEL)odd;
    }
}

static inline void inv_vert_top_hpzero(
    const PIXEL *ll0, const PIXEL *ll1, const PIXEL *ll2,
    PIXEL *even_lp, PIXEL *odd_lp,
    int bw)
{
    int x = 0;
#if defined(__ARM_NEON)
    const int32x4_t four = vdupq_n_s32(INV_ROUNDING);
    const int bw_m4 = (bw / 4) * 4;
    for (; x < bw_m4; x += 4) {
        int32x4_t a0 = vld1q_s32(&ll0[x]);
        int32x4_t a1 = vld1q_s32(&ll1[x]);
        int32x4_t a2 = vld1q_s32(&ll2[x]);
        int32x4_t e_acc = vmlaq_n_s32(a2, a0, 11);
        e_acc = vmlsq_n_s32(e_acc, a1, 4);
        e_acc = vaddq_s32(e_acc, four);
        e_acc = vshrq_n_s32(e_acc, 3);
        int32x4_t e_lp = vshrq_n_s32(e_acc, 1);
        vst1q_s32(&even_lp[x], e_lp);

        int32x4_t o_acc = vmlaq_n_s32(vmulq_n_s32(a0, 5), a1, 4);
        o_acc = vsubq_s32(o_acc, a2);
        o_acc = vaddq_s32(o_acc, four);
        o_acc = vshrq_n_s32(o_acc, 3);
        int32x4_t o_lp = vshrq_n_s32(o_acc, 1);
        vst1q_s32(&odd_lp[x], o_lp);
    }
#endif
    for (; x < bw; x++) {
        int32_t even, odd;
        even = 11 * ll0[x] - 4 * ll1[x] + ll2[x] + INV_ROUNDING;
        even = even >> 3;
        even = even >> 1;
        even_lp[x] = (PIXEL)even;

        odd = 5 * ll0[x] + 4 * ll1[x] - ll2[x] + INV_ROUNDING;
        odd = odd >> 3;
        odd = odd >> 1;
        odd_lp[x] = (PIXEL)odd;
    }
}

static inline void inv_vert_bot_hpzero(
    const PIXEL *ll_m2, const PIXEL *ll_m1, const PIXEL *ll_0,
    PIXEL *even_lp, PIXEL *odd_lp,
    int bw)
{
    int x = 0;
#if defined(__ARM_NEON)
    const int32x4_t four = vdupq_n_s32(INV_ROUNDING);
    const int bw_m4 = (bw / 4) * 4;
    for (; x < bw_m4; x += 4) {
        int32x4_t a0 = vld1q_s32(&ll_m2[x]);
        int32x4_t a1 = vld1q_s32(&ll_m1[x]);
        int32x4_t a2 = vld1q_s32(&ll_0[x]);

        int32x4_t e_acc = vmlaq_n_s32(vmulq_n_s32(a2, 5), a1, 4);
        e_acc = vsubq_s32(e_acc, a0);
        e_acc = vaddq_s32(e_acc, four);
        e_acc = vshrq_n_s32(e_acc, 3);
        int32x4_t e_lp = vshrq_n_s32(e_acc, 1);
        vst1q_s32(&even_lp[x], e_lp);

        int32x4_t o_acc = vmlaq_n_s32(a0, a2, 11);
        o_acc = vmlsq_n_s32(o_acc, a1, 4);
        o_acc = vaddq_s32(o_acc, four);
        o_acc = vshrq_n_s32(o_acc, 3);
        int32x4_t o_lp = vshrq_n_s32(o_acc, 1);
        vst1q_s32(&odd_lp[x], o_lp);
    }
#endif
    for (; x < bw; x++) {
        int32_t even, odd;
        even = 5 * ll_0[x] + 4 * ll_m1[x] - ll_m2[x] + INV_ROUNDING;
        even = even >> 3;
        even = even >> 1;
        even_lp[x] = (PIXEL)even;

        odd = 11 * ll_0[x] - 4 * ll_m1[x] + ll_m2[x] + INV_ROUNDING;
        odd = odd >> 3;
        odd = odd >> 1;
        odd_lp[x] = (PIXEL)odd;
    }
}

/* HP-zero horizontal: omit the (lp + hp) terms entirely since hp == 0. */
static inline void invert_horizontal_descale_row_hpzero(
    const PIXEL *lp, PIXEL *out,
    int input_width, int output_width)
{
    const int last_column = input_width - 1;
    int column = 0;
    int32_t even, odd;

    even = 11 * lp[0] - 4 * lp[1] + lp[2] + INV_ROUNDING;
    even = even >> 3;
    even = even << INV_DESCALE_SHIFT;

    odd = 5 * lp[0] + 4 * lp[1] - lp[2] + INV_ROUNDING;
    odd = odd >> 3;
    odd = odd << INV_DESCALE_SHIFT;

    out[0] = (PIXEL)even;
    out[1] = (PIXEL)odd;
    column = 1;

#if defined(__ARM_NEON)
    {
        const int32x4_t four = vdupq_n_s32(INV_ROUNDING);
        for (; column + 3 < last_column; column += 4) {
            int32x4_t lp_left   = vld1q_s32(&lp[column - 1]);
            int32x4_t lp_center = vld1q_s32(&lp[column]);
            int32x4_t lp_right  = vld1q_s32(&lp[column + 1]);

            int32x4_t diff_e = vsubq_s32(lp_left, lp_right);
            diff_e = vaddq_s32(diff_e, four);
            diff_e = vshrq_n_s32(diff_e, 3);
            int32x4_t even_v = vshlq_n_s32(vaddq_s32(diff_e, lp_center), INV_DESCALE_SHIFT);

            int32x4_t diff_o = vsubq_s32(lp_right, lp_left);
            diff_o = vaddq_s32(diff_o, four);
            diff_o = vshrq_n_s32(diff_o, 3);
            int32x4_t odd_v = vshlq_n_s32(vaddq_s32(diff_o, lp_center), INV_DESCALE_SHIFT);

            int32x4x2_t interleaved = { .val = { even_v, odd_v } };
            vst2q_s32(&out[2 * column], interleaved);
        }
    }
#endif

    for (; column < last_column; column++) {
        even = lp[column - 1] - lp[column + 1] + 4;
        even >>= 3;
        even += lp[column];
        even = even << INV_DESCALE_SHIFT;
        out[2 * column] = (PIXEL)even;

        odd = -lp[column - 1] + lp[column + 1] + 4;
        odd >>= 3;
        odd += lp[column];
        odd = odd << INV_DESCALE_SHIFT;
        out[2 * column + 1] = (PIXEL)odd;
    }

    even = 5 * lp[column] + 4 * lp[column - 1] - lp[column - 2] + INV_ROUNDING;
    even = even >> 3;
    even = even << INV_DESCALE_SHIFT;
    out[2 * column] = (PIXEL)even;

    if (2 * column + 1 < output_width) {
        odd = 11 * lp[column] - 4 * lp[column - 1] + lp[column - 2] + INV_ROUNDING;
        odd = odd >> 3;
        odd = odd << INV_DESCALE_SHIFT;
        out[2 * column + 1] = (PIXEL)odd;
    }
}


/* Per-strip task descriptor. Each of N worker threads owns one of these. */
typedef struct {
    int by_start, by_end;       /* band-row range owned [by_start, by_end) */
    int bw, bh, ch_w, ch_h;

    /* Pointers into bands. Per channel × band-slot {LL, LH, HL, HH}.
       Each is bw*bh PIXEL, contiguous, [by * bw + bx]. */
    PIXEL *bands[4][4];

    /* Per-channel dequant divisors for LL, LH, HL, HH. For the
       single-level+LL fused path the LL band has already been pre-
       dequantized into raw scale by the caller (multiplied by
       qt[0] * 16). So we only need LH/HL/HH quants here. */
    QUANT q[4];   /* {LL=1 marker, LH=-qt[1], HL=-qt[2], HH=-qt[3]} */

    /* When non-zero, LH/HL/HH bands are known to be all-zero. Strip
       worker skips all HP dequant + uses HP-zero fast paths for the
       vertical and horizontal inverse wavelet (omits HP terms entirely).
       Net effect on Pi 5 in measured LL-only-fast streaming: wavelet
       work roughly halves because we don't touch 3 of the 4 band
       buffers and the inner loops shrink to LL-only math. */
    int hp_zero;
    int descale;

    /* Per-strip scratch (caller-allocated, sized for one row pair × 4
       channels). Layout per channel:
         lh_dq[3][bw]   — 3-row sliding LH dequant cache (rotating)
         hl_dq[bw], hh_dq[bw]
         even_lp, odd_lp, even_hp, odd_hp[bw]
         even_out, odd_out[ch_w]
       Total per channel: (3 + 2 + 4) * bw + 2 * ch_w = 9*bw + 2*ch_w
                        = 9*bw + 4*bw = 13*bw PIXELs ≈ 52 KB at bw=1035
                        × 4 channels ≈ 210 KB per strip. */
    PIXEL *scratch;
    size_t scratch_bytes;

    /* Color transform inputs */
    int log_max, midpoint, shift, is_rggb;
    const uint16_t *log_table;

    /* Bayer output */
    uint8_t *bayer_out;
    size_t bayer_pitch_bytes;

    /* Status */
    int err;
} FUSED_STREAM_TASK;


/* Color transform: mirror of fused_color_runner. Reads 4 channel rows
   (gs/rg/bg/gd) and writes 4 Bayer rows (top + bottom of one Bayer pair
   per channel-row). This is the scalar version. NEON version lives in
   the existing fused_color_runner; we can call into that or copy it
   later. Keeping scalar here to minimize lines while correctness is
   being proven. */
static inline void color_xform_row(
    const PIXEL *gs_row, const PIXEL *rg_row,
    const PIXEL *bg_row, const PIXEL *gd_row,
    uint8_t *bayer_row1_bytes, uint8_t *bayer_row2_bytes,
    int ch_w,
    int log_max, int midpoint, int shift, int is_rggb,
    const uint16_t *log_table)
{
    uint16_t *bayer_row1 = (uint16_t *)bayer_row1_bytes;
    uint16_t *bayer_row2 = (uint16_t *)bayer_row2_bytes;

    int x = 0;

#if defined(__ARM_NEON)
    /* NEON 4-wide — same arithmetic as fused_color_runner. */
    const int32x4_t vlog_max = vdupq_n_s32(log_max);
    const int32x4_t vzero    = vdupq_n_s32(0);
    const int32x4_t vmid     = vdupq_n_s32(midpoint);
    const int ch_w_m4 = (ch_w / 4) * 4;
    for (; x < ch_w_m4; x += 4) {
        int32x4_t gs = vld1q_s32(gs_row + x);
        int32x4_t rg = vld1q_s32(rg_row + x);
        int32x4_t bg = vld1q_s32(bg_row + x);
        int32x4_t gd = vld1q_s32(gd_row + x);
        gs = vmaxq_s32(vminq_s32(gs, vlog_max), vzero);
        rg = vmaxq_s32(vminq_s32(rg, vlog_max), vzero);
        bg = vmaxq_s32(vminq_s32(bg, vlog_max), vzero);
        gd = vmaxq_s32(vminq_s32(gd, vlog_max), vzero);
        int32x4_t rgc = vsubq_s32(rg, vmid);
        int32x4_t bgc = vsubq_s32(bg, vmid);
        int32x4_t gdc = vsubq_s32(gd, vmid);
        int32x4_t r  = vaddq_s32(vshlq_n_s32(rgc, 1), gs);
        int32x4_t b  = vaddq_s32(vshlq_n_s32(bgc, 1), gs);
        int32x4_t g1 = vaddq_s32(gs, gdc);
        int32x4_t g2 = vsubq_s32(gs, gdc);
        r  = vmaxq_s32(vminq_s32(r,  vlog_max), vzero);
        g1 = vmaxq_s32(vminq_s32(g1, vlog_max), vzero);
        g2 = vmaxq_s32(vminq_s32(g2, vlog_max), vzero);
        b  = vmaxq_s32(vminq_s32(b,  vlog_max), vzero);
        int32_t rs[4], g1s[4], g2s[4], bs[4];
        vst1q_s32(rs,  r); vst1q_s32(g1s, g1);
        vst1q_s32(g2s, g2); vst1q_s32(bs,  b);
        uint16_t r_l[4], g1_l[4], g2_l[4], b_l[4];
        for (int k = 0; k < 4; k++) {
            r_l[k]  = log_table[rs[k]]  >> shift;
            g1_l[k] = log_table[g1s[k]] >> shift;
            g2_l[k] = log_table[g2s[k]] >> shift;
            b_l[k]  = log_table[bs[k]]  >> shift;
        }
        uint16x4x2_t v_r1, v_r2;
        if (is_rggb) {
            v_r1.val[0] = vld1_u16(r_l);
            v_r1.val[1] = vld1_u16(g1_l);
            v_r2.val[0] = vld1_u16(g2_l);
            v_r2.val[1] = vld1_u16(b_l);
        } else {
            v_r1.val[0] = vld1_u16(g1_l);
            v_r1.val[1] = vld1_u16(b_l);
            v_r2.val[0] = vld1_u16(r_l);
            v_r2.val[1] = vld1_u16(g2_l);
        }
        vst2_u16(bayer_row1 + 2*x, v_r1);
        vst2_u16(bayer_row2 + 2*x, v_r2);
    }
#endif

    /* Scalar tail */
    for (; x < ch_w; x++) {
        int gs = gs_row[x], rg = rg_row[x], bg = bg_row[x], gd = gd_row[x];
        if (gs < 0) gs = 0; if (gs > log_max) gs = log_max;
        if (rg < 0) rg = 0; if (rg > log_max) rg = log_max;
        if (bg < 0) bg = 0; if (bg > log_max) bg = log_max;
        if (gd < 0) gd = 0; if (gd > log_max) gd = log_max;
        rg -= midpoint; bg -= midpoint; gd -= midpoint;
        int r  = (rg << 1) + gs;
        int b  = (bg << 1) + gs;
        int g1 = gs + gd;
        int g2 = gs - gd;
        if (r  < 0) r  = 0; if (r  > log_max) r  = log_max;
        if (g1 < 0) g1 = 0; if (g1 > log_max) g1 = log_max;
        if (g2 < 0) g2 = 0; if (g2 > log_max) g2 = log_max;
        if (b  < 0) b  = 0; if (b  > log_max) b  = log_max;
        int r_lin  = log_table[r]  >> shift;
        int g1_lin = log_table[g1] >> shift;
        int g2_lin = log_table[g2] >> shift;
        int b_lin  = log_table[b]  >> shift;
        if (is_rggb) {
            bayer_row1[2*x]   = (uint16_t)r_lin;
            bayer_row1[2*x+1] = (uint16_t)g1_lin;
            bayer_row2[2*x]   = (uint16_t)g2_lin;
            bayer_row2[2*x+1] = (uint16_t)b_lin;
        } else {
            bayer_row1[2*x]   = (uint16_t)g1_lin;
            bayer_row1[2*x+1] = (uint16_t)b_lin;
            bayer_row2[2*x]   = (uint16_t)r_lin;
            bayer_row2[2*x+1] = (uint16_t)g2_lin;
        }
    }
}


/* Per-strip worker. Walks band rows [by_start, by_end) in lock-step
   across all 4 channels, producing 2*(by_end-by_start) channel rows
   and the corresponding Bayer output. */
static void *fused_stream_runner(void *arg)
{
    FUSED_STREAM_TASK *t = (FUSED_STREAM_TASK *)arg;
    const int bw    = t->bw;
    const int bh    = t->bh;
    const int ch_w  = t->ch_w;
    const int ch_h  = t->ch_h;
    const int last_by = bh - 1;
    (void)ch_h;

    /* Scratch layout per channel — sliced from t->scratch. */
    /* Per channel: lh_dq[3][bw], hl_dq[bw], hh_dq[bw],
                    even_lp[bw], odd_lp[bw], even_hp[bw], odd_hp[bw],
                    even_out[ch_w], odd_out[ch_w]
       = 9*bw + 2*ch_w = 13*bw PIXELs */
    const size_t per_ch = (size_t)(9 * bw + 2 * ch_w);
    if (t->scratch_bytes < per_ch * 4 * sizeof(PIXEL)) {
        t->err = -50;
        return NULL;
    }
    PIXEL *base = t->scratch;
    PIXEL *lh_dq[4][3];
    PIXEL *hl_dq[4], *hh_dq[4];
    PIXEL *even_lp[4], *odd_lp[4], *even_hp[4], *odd_hp[4];
    PIXEL *even_out[4], *odd_out[4];
    for (int ch = 0; ch < 4; ch++) {
        PIXEL *p = base + (size_t)ch * per_ch;
        lh_dq[ch][0] = p;          p += bw;
        lh_dq[ch][1] = p;          p += bw;
        lh_dq[ch][2] = p;          p += bw;
        hl_dq[ch]    = p;          p += bw;
        hh_dq[ch]    = p;          p += bw;
        even_lp[ch]  = p;          p += bw;
        odd_lp[ch]   = p;          p += bw;
        even_hp[ch]  = p;          p += bw;
        odd_hp[ch]   = p;          p += bw;
        even_out[ch] = p;          p += ch_w;
        odd_out[ch]  = p;          p += ch_w;
    }

    const int hp_zero = t->hp_zero;
    const int descale = t->descale;

    /* Init LH cache for strip start. Cache holds dequantized LH rows
       {by_start-1, by_start, by_start+1}. Top-of-image strip uses
       replicate at by=-1 (= row 0). Internal strips read the real
       by_start-1 row from the bands array. Similarly for by_start+1
       being past the bottom (only matters when strip is a single row).
       HP-zero fast path: skip the LH cache prime entirely (cache is
       unused in that mode). */
    if (!hp_zero) {
        for (int ch = 0; ch < 4; ch++) {
            QUANT lhq = t->q[1];
            PIXEL *lh_band = t->bands[ch][1];

            /* Row at by_start-1 (or replicate to 0 if at top of image) */
            int rm1 = t->by_start - 1;
            if (rm1 < 0) rm1 = 0;
            dequant_band_row(lh_band + (size_t)rm1 * bw, lh_dq[ch][0], bw, lhq);

            /* Row at by_start (or replicate to bh-1 if past bottom) */
            int r0 = t->by_start;
            if (r0 > last_by) r0 = last_by;
            dequant_band_row(lh_band + (size_t)r0 * bw, lh_dq[ch][1], bw, lhq);

            /* Row at by_start+1 (replicate at bottom edge) */
            int rp1 = t->by_start + 1;
            if (rp1 > last_by) rp1 = last_by;
            dequant_band_row(lh_band + (size_t)rp1 * bw, lh_dq[ch][2], bw, lhq);
        }
    }

    /* Walk band rows. Each band row → 1 row of channel rows × 2 (even, odd)
       → 1 Bayer row pair (2 bayer rows = top+bottom of pair). */
    for (int by = t->by_start; by < t->by_end; by++) {
        /* For each channel, vertical pass + horizontal pass. */
        for (int ch = 0; ch < 4; ch++) {
            PIXEL *ll_band = t->bands[ch][0];

            /* LL rows (raw, no dequant — the caller pre-scaled the LL
               band by ll_dequant). The 3-row window. */
            int rm1 = by - 1;
            int rp1 = by + 1;
            int top    = (by == 0);
            int bottom = (by == last_by);

            /* ---- HP-zero fast path ---- */
            if (hp_zero) {
                if (top) {
                    const PIXEL *ll0 = ll_band + 0 * bw;
                    const PIXEL *ll1 = ll_band + 1 * bw;
                    const PIXEL *ll2 = ll_band + (bh > 2 ? 2 : last_by) * bw;
                    inv_vert_top_hpzero(ll0, ll1, ll2,
                                         even_lp[ch], odd_lp[ch], bw);
                } else if (bottom) {
                    int m2 = last_by - 2; if (m2 < 0) m2 = 0;
                    int m1 = last_by - 1; if (m1 < 0) m1 = 0;
                    const PIXEL *ll_m2 = ll_band + (size_t)m2 * bw;
                    const PIXEL *ll_m1 = ll_band + (size_t)m1 * bw;
                    const PIXEL *ll_0  = ll_band + (size_t)last_by * bw;
                    inv_vert_bot_hpzero(ll_m2, ll_m1, ll_0,
                                         even_lp[ch], odd_lp[ch], bw);
                } else {
                    const PIXEL *ll_m1p = ll_band + (size_t)rm1 * bw;
                    const PIXEL *ll_0   = ll_band + (size_t)by  * bw;
                    const PIXEL *ll_p1p = ll_band + (size_t)rp1 * bw;
                    inv_vert_mid_hpzero(ll_m1p, ll_0, ll_p1p,
                                         even_lp[ch], odd_lp[ch], bw);
                }
                if (descale == 2) {
                    invert_horizontal_descale_row_hpzero(even_lp[ch],
                                                         even_out[ch], bw, ch_w);
                    invert_horizontal_descale_row_hpzero(odd_lp[ch],
                                                         odd_out[ch],  bw, ch_w);
                } else {
                    memset(even_hp[ch], 0, (size_t)bw * sizeof(PIXEL));
                    if (descale == 0) {
                        InvertHorizontal16s(even_lp[ch], even_hp[ch],
                                            even_out[ch], bw, ch_w);
                        InvertHorizontal16s(odd_lp[ch], even_hp[ch],
                                            odd_out[ch],  bw, ch_w);
                    } else {
                        InvertHorizontalDescale16s(even_lp[ch], even_hp[ch],
                                                   even_out[ch], bw, ch_w, descale);
                        InvertHorizontalDescale16s(odd_lp[ch], even_hp[ch],
                                                   odd_out[ch],  bw, ch_w, descale);
                    }
                }
                continue;
            }

            /* ---- Full HP path ---- */
            QUANT hlq = t->q[2];
            QUANT hhq = t->q[3];
            PIXEL *hl_band = t->bands[ch][2];
            PIXEL *hh_band = t->bands[ch][3];

            /* Dequantize HL and HH at current row by (raw mode). */
            dequant_band_row(hl_band + (size_t)by * bw, hl_dq[ch], bw, hlq);
            dequant_band_row(hh_band + (size_t)by * bw, hh_dq[ch], bw, hhq);

            if (top) {
                /* {ll0, ll1, ll2} = LL rows {0, 1, 2} */
                const PIXEL *ll0 = ll_band + 0 * bw;
                const PIXEL *ll1 = ll_band + 1 * bw;
                const PIXEL *ll2 = ll_band + (bh > 2 ? 2 : last_by) * bw;
                /* {lh0, lh1, lh2} are already in the 3-row cache (entries
                   0,1,2 == rows 0,1,2 because at strip top, by_start may
                   not be 0; but this branch only fires when by==0, which
                   means the strip starts at 0 and the cache already
                   reflects rows {0,0,1} or {0,1,2} as primed above —
                   actually NO: when by==0 and strip start is 0, we primed
                   cache with rows {0,0,1} above. We need {0,1,2}. Rebuild. */
                /* Reload LH cache to {0, 1, 2} explicitly. */
                QUANT lhq = t->q[1];
                PIXEL *lh_band = t->bands[ch][1];
                dequant_band_row(lh_band + 0 * bw,           lh_dq[ch][0], bw, lhq);
                dequant_band_row(lh_band + 1 * bw,           lh_dq[ch][1], bw, lhq);
                dequant_band_row(lh_band + (size_t)(bh > 2 ? 2 : last_by) * bw,
                                  lh_dq[ch][2], bw, lhq);

                inv_vert_top(ll0, ll1, ll2,
                             lh_dq[ch][0], lh_dq[ch][1], lh_dq[ch][2],
                             hl_dq[ch], hh_dq[ch],
                             even_lp[ch], odd_lp[ch], even_hp[ch], odd_hp[ch],
                             bw);
            } else if (bottom) {
                /* {ll_m2, ll_m1, ll_0} = LL rows {bh-3, bh-2, bh-1} */
                int m2 = last_by - 2; if (m2 < 0) m2 = 0;
                int m1 = last_by - 1; if (m1 < 0) m1 = 0;
                const PIXEL *ll_m2 = ll_band + (size_t)m2 * bw;
                const PIXEL *ll_m1 = ll_band + (size_t)m1 * bw;
                const PIXEL *ll_0  = ll_band + (size_t)last_by * bw;
                /* Reload LH cache to {bh-3, bh-2, bh-1}. */
                QUANT lhq = t->q[1];
                PIXEL *lh_band = t->bands[ch][1];
                dequant_band_row(lh_band + (size_t)m2 * bw,      lh_dq[ch][0], bw, lhq);
                dequant_band_row(lh_band + (size_t)m1 * bw,      lh_dq[ch][1], bw, lhq);
                dequant_band_row(lh_band + (size_t)last_by * bw, lh_dq[ch][2], bw, lhq);

                inv_vert_bot(ll_m2, ll_m1, ll_0,
                             lh_dq[ch][0], lh_dq[ch][1], lh_dq[ch][2],
                             hl_dq[ch], hh_dq[ch],
                             even_lp[ch], odd_lp[ch], even_hp[ch], odd_hp[ch],
                             bw);
            } else {
                /* Interior — cache holds {by-1, by, by+1}. */
                const PIXEL *ll_m1 = ll_band + (size_t)rm1 * bw;
                const PIXEL *ll_0  = ll_band + (size_t)by  * bw;
                const PIXEL *ll_p1 = ll_band + (size_t)rp1 * bw;
                inv_vert_mid(ll_m1, ll_0, ll_p1,
                             lh_dq[ch][0], lh_dq[ch][1], lh_dq[ch][2],
                             hl_dq[ch], hh_dq[ch],
                             even_lp[ch], odd_lp[ch], even_hp[ch], odd_hp[ch],
                             bw);
            }

            /* Horizontal pass — produces 2 channel rows at width ch_w. */
            if (descale == 2) {
                invert_horizontal_descale_row(even_lp[ch], even_hp[ch],
                                              even_out[ch], bw, ch_w);
                invert_horizontal_descale_row(odd_lp[ch],  odd_hp[ch],
                                              odd_out[ch],  bw, ch_w);
            } else if (descale == 0) {
                InvertHorizontal16s(even_lp[ch], even_hp[ch],
                                    even_out[ch], bw, ch_w);
                InvertHorizontal16s(odd_lp[ch],  odd_hp[ch],
                                    odd_out[ch],  bw, ch_w);
            } else {
                InvertHorizontalDescale16s(even_lp[ch], even_hp[ch],
                                           even_out[ch], bw, ch_w, descale);
                InvertHorizontalDescale16s(odd_lp[ch],  odd_hp[ch],
                                           odd_out[ch],  bw, ch_w, descale);
            }
        }

        /* Color transform on the 4-channel × 2 row tuple → 4 Bayer rows.
           Channel-row indices are 2*by (even) and 2*by+1 (odd). Bayer
           rows are 2*(2*by) = 4*by and 4*by+1 for the even-row pair,
           4*by+2 and 4*by+3 for the odd-row pair. */

        /* Even channel row (channel-row index 2*by). Writes Bayer rows
           4*by and 4*by+1. */
        int cr_even = 2 * by;
        uint8_t *b_row1 = t->bayer_out + (size_t)(2 * cr_even)     * t->bayer_pitch_bytes;
        uint8_t *b_row2 = t->bayer_out + (size_t)(2 * cr_even + 1) * t->bayer_pitch_bytes;
        color_xform_row(even_out[0], even_out[1], even_out[2], even_out[3],
                        b_row1, b_row2, ch_w,
                        t->log_max, t->midpoint, t->shift, t->is_rggb,
                        t->log_table);

        /* Odd channel row (channel-row index 2*by+1). Writes Bayer rows
           4*by+2 and 4*by+3 — but only if the row exists. The encoder's
           layout always pairs even+odd at this level. */
        int cr_odd = 2 * by + 1;
        if (cr_odd < ch_h) {
            uint8_t *o_row1 = t->bayer_out + (size_t)(2 * cr_odd)     * t->bayer_pitch_bytes;
            uint8_t *o_row2 = t->bayer_out + (size_t)(2 * cr_odd + 1) * t->bayer_pitch_bytes;
            color_xform_row(odd_out[0], odd_out[1], odd_out[2], odd_out[3],
                            o_row1, o_row2, ch_w,
                            t->log_max, t->midpoint, t->shift, t->is_rggb,
                            t->log_table);
        }

        /* Slide LH cache for next iteration.

           Convention (mirrors reference inverse.c):
             - After processing by=0 (top border): cache holds rows {0,1,2}.
               These are ALSO the right rows for by=1's interior filter
               (which wants {by-1, by, by+1} = {0, 1, 2}). So skip slide.
             - After processing by=k for k ≥ 1 (interior): cache held
               {k-1, k, k+1}. Slide to {k, k+1, k+2} for by=k+1.
             - After processing by = last_by (bottom border): no next row.

           Equivalently: slide iff `by >= 1` AND `by+1 <= last_by - 1`.
           Note bottom-border (by == last_by) is handled by its own
           rebuild branch the next time we hit it, but in practice the
           loop terminates at last_by, so we never enter "slide after
           bottom-border" anyway.

           Subtle case: by=0 AND we then jump straight to by=last_by
           (image with bh=2). In that case the bottom-border branch
           will rebuild the cache, so the (now-stale) {0,1,2} state is
           harmless. */
        int next_by = by + 1;
        if (!hp_zero && by >= 1 && next_by <= last_by - 1) {
            for (int ch = 0; ch < 4; ch++) {
                /* Rotate: 0←1, 1←2, 2←new */
                PIXEL *tmp = lh_dq[ch][0];
                lh_dq[ch][0] = lh_dq[ch][1];
                lh_dq[ch][1] = lh_dq[ch][2];
                lh_dq[ch][2] = tmp;
                int rp2 = next_by + 1;
                if (rp2 > last_by) rp2 = last_by;
                QUANT lhq = t->q[1];
                PIXEL *lh_band = t->bands[ch][1];
                dequant_band_row(lh_band + (size_t)rp2 * bw,
                                  lh_dq[ch][2], bw, lhq);
            }
        }
    }

    t->err = 0;
    return NULL;
}


/* ============================================================
   Public entry point — invoked from fused_decode.c when the env
   knob GPR_DECODE_FUSED_STREAM=1 is set. Replaces the
   inverse-wavelet + color-xform stages with N-strip parallel
   workers. Caller has already done:
     - band_decode (16 bands populated)
     - optional HP-synth
     - LL pre-multiply by qt[0]*16

   On entry:
     bands[ch][s]   — 16 band buffers, each bw*bh PIXEL
     qt             — quality table {qt[0]..qt[3]}
     bayer_out      — output Bayer plane, sized for ch_w*2 × ch_h*2
     bayer_pitch    — row stride in bytes (typically ch_w*2*2 = ch_w*4)

   Returns 0 on success, negative on failure.

   ============================================================ */
int gpr_decode_fused_stream(PIXEL *bands[4][4],
                            int bw, int bh, int ch_w, int ch_h,
                            const QUANT qt[4],
                            int log_max, int midpoint, int shift, int is_rggb,
                            const uint16_t *log_table,
                            uint8_t *bayer_out, size_t bayer_pitch_bytes,
                            int descale,
                            int hp_zero)
{
    /* Pick number of strips: 1, 2, or 4. Default 4. Allow override via
       GPR_DECODE_FUSED_STREAM_STRIPS=N for tuning. */
    int n_strips = 4;
    {
        const char *e = getenv("GPR_DECODE_FUSED_STREAM_STRIPS");
        if (e && *e) {
            int v = atoi(e);
            if (v == 1 || v == 2 || v == 4) n_strips = v;
        }
    }
    if (n_strips > bh) n_strips = bh > 0 ? bh : 1;

    /* Quants are stored with the negative-quant convention so that
       DequantizeBandRow16s skips uncompanding (raw mode). */
    QUANT q[4] = { -qt[0], -qt[1], -qt[2], -qt[3] };
    (void)q;

    /* Allocate strip scratches. Per strip: 4 channels * (9*bw + 2*ch_w).
       Per-frame malloc/free of 4 × ~250 KB was measurable on Pi 5.
       Promote to a process-wide cache (one persistent buffer per strip
       slot, grown as needed). Threads use distinct slots so no locking
       is needed. 64-byte aligned to match cache lines and avoid false
       sharing across strip workers. */
    const size_t per_ch = (size_t)(9 * bw + 2 * ch_w);
    const size_t per_strip_bytes = per_ch * 4 * sizeof(PIXEL);
    static PIXEL *scratch_cache[4] = {0};
    static size_t scratch_cache_bytes[4] = {0};

    FUSED_STREAM_TASK tasks[4];
    pthread_t threads[4];
    int created[4] = {0};
    PIXEL *scratches[4] = {0};

    int rc = 0;
    for (int i = 0; i < n_strips; i++) {
        if (scratch_cache_bytes[i] < per_strip_bytes) {
            if (scratch_cache[i]) free(scratch_cache[i]);
            /* posix_memalign needs <stdlib.h> already included; use it for
               64-B alignment so the per-channel scratch slices land on
               cache-line boundaries. */
            void *p = NULL;
            if (posix_memalign(&p, 64, per_strip_bytes) != 0 || !p) {
                rc = -60; break;
            }
            scratch_cache[i] = (PIXEL *)p;
            scratch_cache_bytes[i] = per_strip_bytes;
        }
        scratches[i] = scratch_cache[i];
    }
    if (rc == 0) {
        for (int i = 0; i < n_strips; i++) {
            tasks[i].by_start = (bh * i)       / n_strips;
            tasks[i].by_end   = (bh * (i + 1)) / n_strips;
            tasks[i].bw = bw; tasks[i].bh = bh;
            tasks[i].ch_w = ch_w; tasks[i].ch_h = ch_h;
            for (int ch = 0; ch < 4; ch++)
                for (int s = 0; s < 4; s++)
                    tasks[i].bands[ch][s] = bands[ch][s];
            tasks[i].q[0] = -qt[0];  /* unused — LL is pre-scaled */
            tasks[i].q[1] = -qt[1];
            tasks[i].q[2] = -qt[2];
            tasks[i].q[3] = -qt[3];
            tasks[i].scratch = scratches[i];
            tasks[i].scratch_bytes = per_strip_bytes;
            tasks[i].log_max = log_max;
            tasks[i].midpoint = midpoint;
            tasks[i].shift = shift;
            tasks[i].is_rggb = is_rggb;
            tasks[i].log_table = log_table;
            tasks[i].bayer_out = bayer_out;
            tasks[i].bayer_pitch_bytes = bayer_pitch_bytes;
            tasks[i].descale = descale;
            tasks[i].hp_zero = hp_zero;
            tasks[i].err = 0;
        }
        for (int i = 0; i < n_strips; i++) {
            created[i] = (pthread_create(&threads[i], NULL,
                                          fused_stream_runner, &tasks[i]) == 0);
            if (!created[i]) fused_stream_runner(&tasks[i]);
        }
        for (int i = 0; i < n_strips; i++) {
            if (created[i]) pthread_join(threads[i], NULL);
            if (tasks[i].err != 0) rc = tasks[i].err;
        }
    }

    /* Scratches live in the process-wide cache; do not free per-frame. */

    return rc;
}
