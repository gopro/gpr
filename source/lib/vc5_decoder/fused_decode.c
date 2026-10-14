/*! @file fused_decode.c
 *
 *  @brief Implementation of the fused decoder.
 *
 *  See fused_decode.h for the contract. The flow:
 *    1. Parse FUSED_HEADER and band-size table.
 *    2. rANS-decode all 40 bands (4 channels × 10 slots) via
 *       jans_decode_band_x4.
 *    3. Per channel: inverse wavelet at level 3, then 2, then 1.
 *       Level 1 uses InvertSpatialQuantDescale16s with descale=1 to
 *       undo the encoder's prescale=2. Levels 2/3 use the plain
 *       InvertSpatialQuant16s (no descale).
 *    4. Reverse the GS/RG/BG/GD color transform per pixel:
 *         R  = (RG-midpoint)<<1 + GS
 *         B  = (BG-midpoint)<<1 + GS
 *         G1 = GS + (GD-midpoint)
 *         G2 = GS - (GD-midpoint)
 *       Clamp to [0, log_max], apply DecoderLogCurve, shift to the
 *       caller's output bit depth, and write to the RGGB/GBRG Bayer
 *       pattern.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L  /* needed for CLOCK_MONOTONIC on Pi gcc */
#endif

#include "headers.h"
#include "ans_joint.h"
#include "fused_decode.h"
#include "../vc5_encoder/fused_encode.h"  /* FUSED_HEADER, FUSED_MAGIC */
#include "logcurve.h"
#include "inverse.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* GPR_DECODE_TIMING=1 prints per-stage decoder timing. */
static double _decode_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

/* Mirror of the encoder's quality_tables (private to fused_encode.c).
   Keep these in sync if they change. */
static const QUANT FUSED_QUALITY_TABLES[12][10] = {
    {1, 24, 24, 12, 64, 64, 48, 512, 512, 768},  /* 0 Low */
    {1, 24, 24, 12, 48, 48, 32, 256, 256, 384},  /* 1 Medium */
    {1, 24, 24, 12, 32, 32, 24, 128, 128, 192},  /* 2 High */
    {1, 24, 24, 12, 24, 24, 12,  96,  96, 144},  /* 3 FS1 default */
    {1, 24, 24, 12, 24, 24, 12,  64,  64,  96},  /* 4 FSX */
    {1, 24, 24, 12, 24, 24, 12,  32,  32,  48},  /* 5 FS2 (peak on real content) */
    {1, 12, 12,  6, 12, 12,  6,  16,  16,  24},  /* 6 FS3 */
    {1,  6,  6,  4, 12, 12,  6,  16,  16,  24},  /* 7 FS4 */
    {1,  4,  4,  2, 10, 10,  6,  16,  16,  24},  /* 8 FS5 */
    {1,  4,  4,  2, 10, 10,  6,  16,  16,  24},  /* 9 Reserved (mirrors FS5) */
    {1,  4,  4,  2, 10, 10,  6,  16,  16,  24},  /* 10 Reserved (mirrors FS5) */
    {1, 48, 48, 48, 48, 48, 24, 192, 192, 576},  /* 11 CNN-aware (single+multi cranked) */
};

/* Apply GPR_QUANT_OVERRIDE to a dequant table copy (task #158).
   Mirror of fused_encode.c::apply_quant_override — same parser, same
   semantics. Decoder MUST run with the same env var the encoder used. */
static void apply_quant_override(QUANT *qt /* size 10 */)
{
    const char *spec = getenv("GPR_QUANT_OVERRIDE");
    if (!spec || !*spec) return;
    const char *p = spec;
    while (*p) {
        int slot = -1, value = -1;
        char *end = NULL;
        slot = (int)strtol(p, &end, 10);
        if (end == p || *end != ':') break;
        p = end + 1;
        value = (int)strtol(p, &end, 10);
        if (end == p) break;
        if (slot >= 0 && slot < 10 && value > 0) qt[slot] = (QUANT)value;
        p = end;
        if (*p == ',') p++;
        else break;
    }
}

/* Resolve the per-channel quant table for hdr.quality, honoring the override
   env var. Returns a pointer to a thread-local buffer; valid until the next
   call from the same thread. */
static const QUANT *get_quant_table(int quality)
{
    static _Thread_local QUANT qt[10];
    memcpy(qt, FUSED_QUALITY_TABLES[(quality >= 0 && quality < 12) ? quality : 3],
           sizeof(qt));
    apply_quant_override(qt);
    return qt;
}

static void fused_scale_pixels(PIXEL *p, size_t n, int scale)
{
    if (!p || scale == 1) return;
    for (size_t i = 0; i < n; i++) {
        p[i] *= scale;
    }
}

static int fused_ll_extra_divisor(int levels)
{
    if (levels == 2) {
        const char *env = getenv("FUSED_LL2_DIVISOR");
        if (env && *env) {
            int v = atoi(env);
            if (v >= 1 && v <= 64) return v;
        }
    }
    if (levels == 3) {
        const char *env = getenv("FUSED_LL3_DIVISOR");
        if (env && *env) {
            int v = atoi(env);
            if (v >= 1 && v <= 64) return v;
        }
    }
    return 16;
}

/* Allocator wrapper for the decoder primitives. They expect a
   gpr_allocator* — provide one backed by libc malloc. */
static void *fd_alloc(size_t n) { return malloc(n); }
static void  fd_free(void *p)   { free(p); }

/* Per-decode-call arena for reused buffers. Eliminates the per-frame
   malloc/free churn that was hidden in the decode timeline:
   - 16 band buffers (4 channels × {LL, LH, HL, HH}, each bw*bh*4 bytes)
   - 4 channel scratch buffers (ch_w*ch_h*4 each)
   At 50 MP / 2x2 decimate: bw=1035 bh=690 → ~2.8 MB per band × 16 = 45 MB;
   ch_w=2070 ch_h=1380 → ~11 MB per channel × 4 = 45 MB. ~90 MB total per
   frame previously alloc/freed; now reused via a TLS arena. */
typedef struct {
    PIXEL *band[4][4];   /* per-channel band buffers, size band_cap bytes each */
    size_t band_cap;
    PIXEL *chan[4];      /* per-channel scratch, size chan_cap bytes each */
    size_t chan_cap;
} FUSED_DECODE_ARENA;

static _Thread_local FUSED_DECODE_ARENA fd_arena;

static int arena_ensure(size_t need_band_bytes, size_t need_chan_bytes)
{
    if (need_band_bytes > fd_arena.band_cap) {
        for (int ch = 0; ch < 4; ch++) {
            for (int s = 0; s < 4; s++) {
                if (fd_arena.band[ch][s]) free(fd_arena.band[ch][s]);
                fd_arena.band[ch][s] = (PIXEL *)malloc(need_band_bytes);
                if (!fd_arena.band[ch][s]) { fd_arena.band_cap = 0; return -1; }
            }
        }
        fd_arena.band_cap = need_band_bytes;
    }
    if (need_chan_bytes > fd_arena.chan_cap) {
        for (int ch = 0; ch < 4; ch++) {
            if (fd_arena.chan[ch]) free(fd_arena.chan[ch]);
            fd_arena.chan[ch] = (PIXEL *)malloc(need_chan_bytes);
            if (!fd_arena.chan[ch]) { fd_arena.chan_cap = 0; return -1; }
        }
        fd_arena.chan_cap = need_chan_bytes;
    }
    return 0;
}

/* Worker for parallel per-channel inverse wavelet in the single-level+LL
   decode path. Each task owns its 4 bands + output channel buffer. */
typedef struct {
    PIXEL *ll, *lh, *hl, *hh;
    PIXEL *out_channel;
    int bw, bh, ch_w, ch_h;
    int descale;
    QUANT *q;
    CODEC_ERROR err;
} FUSED_INV_TASK;

/* Per-strip color transform task. Each thread handles a contiguous slice
   of channel rows and writes the corresponding Bayer rows. */
typedef struct {
    int y_start, y_end;             /* channel row range [y_start, y_end) */
    int ch_w, ch_h;
    int log_max, midpoint, shift, is_rggb;
    const uint16_t *log_table;
    const PIXEL *gs_row, *rg_row, *bg_row, *gd_row;  /* base of channels */
    uint8_t *bayer_out;
    size_t bayer_pitch_bytes;
} FUSED_COLOR_TASK;

typedef struct {
    int block_y_start, block_y_end;  /* preview 2x2 Bayer-block rows */
    int ch_w, ch_h;
    int log_max, midpoint, shift, is_rggb;
    const uint16_t *log_table;
    const PIXEL *gs_row, *rg_row, *bg_row, *gd_row;
    uint8_t *bayer_out;
    size_t bayer_pitch_bytes;
} FUSED_PREVIEW4X_TASK;

static inline uint16_t fused_log_to_u16(const uint16_t *log_table, int v, int log_max, int shift)
{
    if (v < 0) v = 0;
    if (v > log_max) v = log_max;
    return (uint16_t)(log_table[v] >> shift);
}

static void *fused_preview4x_runner(void *arg)
{
    FUSED_PREVIEW4X_TASK *t = (FUSED_PREVIEW4X_TASK *)arg;
    const int preview_w = t->ch_w / 2;
    const int log_max = t->log_max;
    const int midpoint = t->midpoint;
    const int shift = t->shift;
    const uint16_t *log_table = t->log_table;

    for (int by = t->block_y_start; by < t->block_y_end; by++) {
        uint16_t *row0 = (uint16_t *)(t->bayer_out + (size_t)(2 * by) * t->bayer_pitch_bytes);
        uint16_t *row1 = (uint16_t *)(t->bayer_out + (size_t)(2 * by + 1) * t->bayer_pitch_bytes);
        int cy0 = by * 4;
        for (int bx = 0; bx < preview_w / 2; bx++) {
            int cx0 = bx * 4;
#if defined(__ARM_NEON) && defined(__aarch64__)
            int32x4_t gs_v = vdupq_n_s32(0);
            int32x4_t rg_v = vdupq_n_s32(0);
            int32x4_t bg_v = vdupq_n_s32(0);
            int32x4_t gd_v = vdupq_n_s32(0);
            for (int ky = 0; ky < 4; ky++) {
                size_t base = (size_t)(cy0 + ky) * (size_t)t->ch_w + (size_t)cx0;
                gs_v = vaddq_s32(gs_v, vld1q_s32(t->gs_row + base));
                rg_v = vaddq_s32(rg_v, vld1q_s32(t->rg_row + base));
                bg_v = vaddq_s32(bg_v, vld1q_s32(t->bg_row + base));
                gd_v = vaddq_s32(gd_v, vld1q_s32(t->gd_row + base));
            }
            int gs = (vaddvq_s32(gs_v) + 8) >> 4;
            int rg = (vaddvq_s32(rg_v) + 8) >> 4;
            int bg = (vaddvq_s32(bg_v) + 8) >> 4;
            int gd = (vaddvq_s32(gd_v) + 8) >> 4;
#else
            int64_t gs_sum = 0, rg_sum = 0, bg_sum = 0, gd_sum = 0;
            for (int ky = 0; ky < 4; ky++) {
                size_t base = (size_t)(cy0 + ky) * (size_t)t->ch_w + (size_t)cx0;
                for (int kx = 0; kx < 4; kx++) {
                    gs_sum += t->gs_row[base + kx];
                    rg_sum += t->rg_row[base + kx];
                    bg_sum += t->bg_row[base + kx];
                    gd_sum += t->gd_row[base + kx];
                }
            }
            int gs = (int)((gs_sum + 8) >> 4);
            int rg = (int)((rg_sum + 8) >> 4);
            int bg = (int)((bg_sum + 8) >> 4);
            int gd = (int)((gd_sum + 8) >> 4);
#endif
            if (gs < 0) gs = 0; if (gs > log_max) gs = log_max;
            if (rg < 0) rg = 0; if (rg > log_max) rg = log_max;
            if (bg < 0) bg = 0; if (bg > log_max) bg = log_max;
            if (gd < 0) gd = 0; if (gd > log_max) gd = log_max;
            rg -= midpoint; bg -= midpoint; gd -= midpoint;

            uint16_t r = fused_log_to_u16(log_table, (rg << 1) + gs, log_max, shift);
            uint16_t b = fused_log_to_u16(log_table, (bg << 1) + gs, log_max, shift);
            uint16_t g1 = fused_log_to_u16(log_table, gs + gd, log_max, shift);
            uint16_t g2 = fused_log_to_u16(log_table, gs - gd, log_max, shift);
            int ox = bx * 2;
            if (t->is_rggb) {
                row0[ox] = r;      row0[ox + 1] = g1;
                row1[ox] = g2;     row1[ox + 1] = b;
            } else {
                row0[ox] = g1;     row0[ox + 1] = b;
                row1[ox] = r;      row1[ox + 1] = g2;
            }
        }
    }
    return NULL;
}

static void *fused_ll_preview2x_runner(void *arg)
{
    FUSED_PREVIEW4X_TASK *t = (FUSED_PREVIEW4X_TASK *)arg;
    const int preview_w = t->ch_w;
    const int log_max = t->log_max;
    const int midpoint = t->midpoint;
    const int shift = t->shift;
    const uint16_t *log_table = t->log_table;

    for (int by = t->block_y_start; by < t->block_y_end; by++) {
        uint16_t *row0 = (uint16_t *)(t->bayer_out + (size_t)(2 * by) * t->bayer_pitch_bytes);
        uint16_t *row1 = (uint16_t *)(t->bayer_out + (size_t)(2 * by + 1) * t->bayer_pitch_bytes);
        int cy0 = by * 2;
        for (int bx = 0; bx < preview_w / 2; bx++) {
            int cx0 = bx * 2;
            size_t base0 = (size_t)cy0 * (size_t)t->ch_w + (size_t)cx0;
            size_t base1 = base0 + (size_t)t->ch_w;
            int gs = (t->gs_row[base0] + t->gs_row[base0 + 1] +
                      t->gs_row[base1] + t->gs_row[base1 + 1] + 2) >> 2;
            int rg = (t->rg_row[base0] + t->rg_row[base0 + 1] +
                      t->rg_row[base1] + t->rg_row[base1 + 1] + 2) >> 2;
            int bg = (t->bg_row[base0] + t->bg_row[base0 + 1] +
                      t->bg_row[base1] + t->bg_row[base1 + 1] + 2) >> 2;
            int gd = (t->gd_row[base0] + t->gd_row[base0 + 1] +
                      t->gd_row[base1] + t->gd_row[base1 + 1] + 2) >> 2;
            if (gs < 0) gs = 0; if (gs > log_max) gs = log_max;
            if (rg < 0) rg = 0; if (rg > log_max) rg = log_max;
            if (bg < 0) bg = 0; if (bg > log_max) bg = log_max;
            if (gd < 0) gd = 0; if (gd > log_max) gd = log_max;
            rg -= midpoint; bg -= midpoint; gd -= midpoint;

            uint16_t r = fused_log_to_u16(log_table, (rg << 1) + gs, log_max, shift);
            uint16_t b = fused_log_to_u16(log_table, (bg << 1) + gs, log_max, shift);
            uint16_t g1 = fused_log_to_u16(log_table, gs + gd, log_max, shift);
            uint16_t g2 = fused_log_to_u16(log_table, gs - gd, log_max, shift);
            int ox = bx * 2;
            if (t->is_rggb) {
                row0[ox] = r;      row0[ox + 1] = g1;
                row1[ox] = g2;     row1[ox + 1] = b;
            } else {
                row0[ox] = g1;     row0[ox + 1] = b;
                row1[ox] = r;      row1[ox + 1] = g2;
            }
        }
    }
    return NULL;
}

static void *fused_color_runner(void *arg) {
    FUSED_COLOR_TASK *t = (FUSED_COLOR_TASK *)arg;
    int log_max = t->log_max, midpoint = t->midpoint, shift = t->shift;
    const uint16_t *log_table = t->log_table;
    int ch_w = t->ch_w;
    /* A/B debug knob: GPR_COLOR_XFORM_SCALAR=1 forces the scalar path
       (skipping the NEON block) so we can verify NEON ≡ scalar bit-for-bit. */
    static int force_scalar = -1;
    if (force_scalar < 0) {
        const char *e = getenv("GPR_COLOR_XFORM_SCALAR");
        force_scalar = (e && *e == '1') ? 1 : 0;
    }
    for (int y = t->y_start; y < t->y_end; y++) {
        const PIXEL *gs_row = t->gs_row + (size_t)y * ch_w;
        const PIXEL *rg_row = t->rg_row + (size_t)y * ch_w;
        const PIXEL *bg_row = t->bg_row + (size_t)y * ch_w;
        const PIXEL *gd_row = t->gd_row + (size_t)y * ch_w;
        uint8_t *r1b = t->bayer_out + (size_t)(2*y) * t->bayer_pitch_bytes;
        uint8_t *r2b = t->bayer_out + (size_t)(2*y + 1) * t->bayer_pitch_bytes;
        uint16_t *bayer_row1 = (uint16_t *)r1b;
        uint16_t *bayer_row2 = (uint16_t *)r2b;

        int x = 0;

#if defined(__ARM_NEON)
        if (!force_scalar) {
        /* NEON 4-wide: load 4 lanes per channel, do all arithmetic vectored,
           scatter-load the log table per lane (NEON has no arbitrary-index
           u16 gather), then interleave into Bayer rows.

           Encoder mirror: unpack_channel_row uses exactly this pattern in
           reverse (vld2q_u16 deinterleave, scalar LUT gather, NEON arith).
           This brings color_xform from ~43ms to ~12ms on Pi 5. */
        const int32x4_t vlog_max = vdupq_n_s32(log_max);
        const int32x4_t vzero    = vdupq_n_s32(0);
        const int32x4_t vmid     = vdupq_n_s32(midpoint);
        const int32x4_t vshift   = vdupq_n_s32(-shift);  /* vshlq with neg = right shift */
        (void)vshift;
        const int ch_w_m4 = (ch_w / 4) * 4;
        for (; x < ch_w_m4; x += 4) {
            int32x4_t gs = vld1q_s32(gs_row + x);
            int32x4_t rg = vld1q_s32(rg_row + x);
            int32x4_t bg = vld1q_s32(bg_row + x);
            int32x4_t gd = vld1q_s32(gd_row + x);
            /* Clamp inputs to [0, log_max] */
            gs = vmaxq_s32(vminq_s32(gs, vlog_max), vzero);
            rg = vmaxq_s32(vminq_s32(rg, vlog_max), vzero);
            bg = vmaxq_s32(vminq_s32(bg, vlog_max), vzero);
            gd = vmaxq_s32(vminq_s32(gd, vlog_max), vzero);
            /* Center the difference channels */
            int32x4_t rgc = vsubq_s32(rg, vmid);
            int32x4_t bgc = vsubq_s32(bg, vmid);
            int32x4_t gdc = vsubq_s32(gd, vmid);
            /* r = (rgc << 1) + gs; b = (bgc << 1) + gs; g1 = gs + gdc; g2 = gs - gdc */
            int32x4_t r  = vaddq_s32(vshlq_n_s32(rgc, 1), gs);
            int32x4_t b  = vaddq_s32(vshlq_n_s32(bgc, 1), gs);
            int32x4_t g1 = vaddq_s32(gs, gdc);
            int32x4_t g2 = vsubq_s32(gs, gdc);
            r  = vmaxq_s32(vminq_s32(r,  vlog_max), vzero);
            g1 = vmaxq_s32(vminq_s32(g1, vlog_max), vzero);
            g2 = vmaxq_s32(vminq_s32(g2, vlog_max), vzero);
            b  = vmaxq_s32(vminq_s32(b,  vlog_max), vzero);
            /* LUT gather: spill 4 lanes to stack then gather */
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
            /* Interleave into Bayer rows. RGGB: row1 = [r, g1, r, g1, ...],
               row2 = [g2, b, g2, b, ...]. GBRG: row1 = [g1, b], row2 = [r, g2]. */
            uint16x4x2_t v_r1, v_r2;
            if (t->is_rggb) {
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
            if (t->is_rggb) {
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
    return NULL;
}

/* Per-channel band decode task. 4 of these — each decodes a channel's
   4 bands sequentially in one thread. 16-thread per-band variant was
   tested: did NOT help (rANS decode has memory bandwidth + libc malloc
   contention; more threads beyond core count = diminishing returns). */
typedef struct {
    const uint8_t *enc_start;
    const uint32_t *band_sizes;     /* 4 sizes */
    PIXEL *out[4];
    int bw, bh;
    int raw_band_mask;
} FUSED_BAND_TASK;

typedef struct {
    const uint8_t *enc;
    const uint32_t *band_sizes;
    const size_t *band_offsets;
    PIXEL **bands;
    const int *slot_w;
    const int *slot_h;
    int n_slots;
    int ch;
    int half_res;
    int l2_hp_mask;
    int rc;
} FUSED_LEVEL2_BAND_TASK;

#define FUSED_RAW_LL_MAGIC 0x314C4C46u  /* "FLL1", little-endian */
#define FUSED_PRED_LL_MAGIC 0x324C4C46u /* "FLL2", little-endian */

/* HP-synth per-channel task. Hoisted to file scope (was a GCC nested
   function inside decode_fused_single_level_ll — clang doesn't support
   the nested-fn extension, so the file failed to build on macOS). */
typedef struct {
    const PIXEL *LL;
    PIXEL *LH, *HL, *HH;
    int bw, bh;
    double scale;
    double dq_lh, dq_hl, dq_hh;
    uint32_t seed;
    int do_synth;  /* 0 = skip, 1 = run */
} HP_SYNTH_TASK;

static void synthesize_hp_bandpass_band(const PIXEL *LL,
                                        int bw, int bh,
                                        PIXEL *LH, PIXEL *HL, PIXEL *HH,
                                        double scale,
                                        double peak_pct,
                                        double sigma_pct,
                                        uint32_t seed,
                                        double dq_lh, double dq_hl, double dq_hh);

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
    uint64_t acc;
    int bits;
} FUSED_LL_BITREADER;

static int fused_ll_br_get_bit(FUSED_LL_BITREADER *br, uint32_t *bit)
{
    if (br->bits == 0) {
        if (br->pos >= br->size) return -1;
        br->acc = br->buf[br->pos++];
        br->bits = 8;
    }
    *bit = (uint32_t)(br->acc & 1u);
    br->acc >>= 1;
    br->bits--;
    return 0;
}

static int fused_ll_br_get_bits(FUSED_LL_BITREADER *br, int bits, uint32_t *value)
{
    uint32_t out = 0;
    for (int i = 0; i < bits; i++) {
        uint32_t bit = 0;
        if (fused_ll_br_get_bit(br, &bit) != 0) return -1;
        out |= bit << i;
    }
    *value = out;
    return 0;
}

static int fused_read_pred_ll_band(const uint8_t *src, size_t size,
                                   PIXEL *dst, int width, int height)
{
    if (!src || !dst || size < 16 || width <= 0 || height <= 0) return -1;
    uint32_t meta = 0, enc_w = 0, enc_h = 0;
    memcpy(&meta, src + 4, sizeof(meta));
    memcpy(&enc_w, src + 8, sizeof(enc_w));
    memcpy(&enc_h, src + 12, sizeof(enc_h));
    int rice_k = (int)(meta & 0xffu);
    int predictor = (int)((meta >> 8) & 0xffu);
    if ((predictor != 0 && predictor != 1) || rice_k < 0 || rice_k > 15) return -1;
    if ((int)enc_w != width || (int)enc_h != height) return -1;

    FUSED_LL_BITREADER br = { src + 16, size - 16, 0, 0, 0 };
    for (int y = 0; y < height; y++) {
        PIXEL *row = dst + (size_t)y * (size_t)width;
        const PIXEL *prev = (y > 0) ? (dst + (size_t)(y - 1) * (size_t)width) : NULL;
        int32_t left = 0;
        for (int x = 0; x < width; x++) {
            uint32_t q = 0;
            for (;;) {
                uint32_t bit = 0;
                if (fused_ll_br_get_bit(&br, &bit) != 0) return -1;
                if (!bit) break;
                q++;
                if (q > 0x1ffffu) return -1;
            }
            uint32_t r = 0;
            if (rice_k > 0 && fused_ll_br_get_bits(&br, rice_k, &r) != 0) return -1;
            uint32_t zz = (q << rice_k) | r;
            int32_t residual = (int32_t)((zz >> 1) ^ (uint32_t)-(int32_t)(zz & 1u));
            int32_t up = prev ? prev[x] : left;
            int32_t pred = (predictor == 1) ? ((left + up) >> 1) : left;
            int32_t v = pred + residual;
            if (v < 0) v = 0;
            if (v > 65535) v = 65535;
            row[x] = (PIXEL)v;
            left = v;
        }
    }
    return 0;
}

static void *hp_synth_runner(void *arg) {
    HP_SYNTH_TASK *t = (HP_SYNTH_TASK *)arg;
    if (t->do_synth) {
        synthesize_hp_bandpass_band(t->LL, t->bw, t->bh,
                                    t->LH, t->HL, t->HH,
                                    t->scale, 45.0, 15.0,
                                    t->seed,
                                    t->dq_lh, t->dq_hl, t->dq_hh);
    }
    return NULL;
}

/* Implemented in fused_stream_decode.c. Replaces the wavelet_inv +
   color_xform stages with N-strip parallel pipeline. Caller has already
   done band_decode + optional HP-synth + LL pre-multiply by qt[0]*16.
   hp_zero: when non-zero, signals that LH/HL/HH bands are entirely zero
   (LL-only-fast mode). The strip workers skip per-row HP dequant and
   use a simplified vertical/horizontal filter that omits HP terms,
   typically saving ~30% on the wavelet stage. Set hp_zero=0 for the
   general case to use the full HP-aware path. */
extern int gpr_decode_fused_stream(PIXEL *bands[4][4],
                                   int bw, int bh, int ch_w, int ch_h,
                                   const QUANT qt[4],
                                   int log_max, int midpoint, int shift, int is_rggb,
                                   const uint16_t *log_table,
                                   uint8_t *bayer_out, size_t bayer_pitch_bytes,
                                   int descale,
                                   int hp_zero);

static void *fused_band_decode_runner(void *arg) {
    FUSED_BAND_TASK *t = (FUSED_BAND_TASK *)arg;
    size_t off = 0;
    t->raw_band_mask = 0;
    for (int s = 0; s < 4; s++) {
        uint32_t sz = t->band_sizes[s];
        if (sz < 64) {
            memset(t->out[s], 0, (size_t)t->bw * t->bh * sizeof(PIXEL));
        } else if (sz >= sizeof(uint32_t)) {
            uint32_t magic = 0;
            memcpy(&magic, t->enc_start + off, sizeof(magic));
            if (magic == FUSED_RAW_LL_MAGIC) {
                size_t n = (size_t)t->bw * (size_t)t->bh;
                size_t need = sizeof(uint32_t) + n * sizeof(uint16_t);
                if (sz >= need) {
                    const uint16_t *src = (const uint16_t *)(t->enc_start + off + sizeof(uint32_t));
                    for (size_t i = 0; i < n; i++) t->out[s][i] = (PIXEL)src[i];
                    t->raw_band_mask |= (1 << s);
                } else {
                    memset(t->out[s], 0, n * sizeof(PIXEL));
                }
            } else if (magic == FUSED_PRED_LL_MAGIC) {
                if (fused_read_pred_ll_band(t->enc_start + off, sz,
                                            t->out[s], t->bw, t->bh) == 0) {
                    t->raw_band_mask |= (1 << s);
                } else {
                    memset(t->out[s], 0, (size_t)t->bw * t->bh * sizeof(PIXEL));
                }
            } else {
                int drc = jans_decode_band_x4(t->enc_start + off, sz, t->out[s],
                                              t->bw, t->bh, t->bw * (int)sizeof(PIXEL));
                if (drc != 0) memset(t->out[s], 0, (size_t)t->bw * t->bh * sizeof(PIXEL));
            }
        } else {
            int drc = jans_decode_band_x4(t->enc_start + off, sz, t->out[s],
                                          t->bw, t->bh, t->bw * (int)sizeof(PIXEL));
            if (drc != 0) memset(t->out[s], 0, (size_t)t->bw * t->bh * sizeof(PIXEL));
        }
        off += sz;
    }
    return NULL;
}

static int fused_decode_one_band(const uint8_t *src, uint32_t sz,
                                 PIXEL *out, int bw, int bh,
                                 int *raw_band)
{
    if (!src || !out || !raw_band) return -1;
    *raw_band = 0;
    size_t n = (size_t)bw * (size_t)bh;
    if (sz < 64) {
        memset(out, 0, n * sizeof(PIXEL));
        return 0;
    }
    if (sz >= sizeof(uint32_t)) {
        uint32_t magic = 0;
        memcpy(&magic, src, sizeof(magic));
        if (magic == FUSED_RAW_LL_MAGIC) {
            size_t need = sizeof(uint32_t) + n * sizeof(uint16_t);
            if (sz < need) return -2;
            const uint16_t *raw = (const uint16_t *)(src + sizeof(uint32_t));
            for (size_t i = 0; i < n; i++) out[i] = (PIXEL)raw[i];
            *raw_band = 1;
            return 0;
        }
        if (magic == FUSED_PRED_LL_MAGIC) {
            if (fused_read_pred_ll_band(src, sz, out, bw, bh) != 0) return -3;
            *raw_band = 1;
            return 0;
        }
    }
    int drc = jans_decode_band_x4(src, sz, out, bw, bh, bw * (int)sizeof(PIXEL));
    return (drc == 0) ? 0 : -4;
}

static void *fused_level2_band_decode_runner(void *arg) {
    FUSED_LEVEL2_BAND_TASK *t = (FUSED_LEVEL2_BAND_TASK *)arg;
    int base = t->ch * t->n_slots;
    for (int s = 0; s < t->n_slots; s++) {
        if (t->half_res && (s <= 2 || (s >= 3 && s <= 5 && !(t->l2_hp_mask & (1 << (s - 3)))))) {
            continue;
        }
        PIXEL *out = t->bands[s];
        if (!out) {
            t->rc = -11;
            return NULL;
        }
        uint32_t sz = t->band_sizes[base + s];
        if (sz < 64) {
            memset(out, 0, (size_t)t->slot_w[s] * t->slot_h[s] * sizeof(PIXEL));
            continue;
        }
        int drc = jans_decode_band_x4(t->enc + t->band_offsets[base + s], sz,
                                       out, t->slot_w[s], t->slot_h[s],
                                       t->slot_w[s] * (int)sizeof(PIXEL));
        if (drc != 0) {
            t->rc = -12;
            return NULL;
        }
    }
    return NULL;
}

/* ============================================================
   HP-synth deblock polish (decoder-side, optional)
   ------------------------------------------------------------
   When the encoder is run with GPR_DROP_HIGHPASS=1, or when the decoder
   is called with GPR_DECODE_LL_ONLY=1, the LH/HL/HH wavelet bands arrive
   as zero. The inverse 5/3 with HP=0 produces a characteristically soft,
   slightly blocky output (the LP synthesis impulse response on the LL
   grid). The user complained about this as "fundamental noise of the
   process".

   The fix synthesizes plausible HP coefficients from the LL band itself,
   before running the inverse wavelet. Algorithm:

     1. Sobel gradient magnitude of LL
     2. Local std (5x5 box variance) of LL
     3. Per-pixel weight = Gaussian on gradient centered at the 45th
        percentile (so noise PEAKS at moderate edges where LP-only
        synthesis loses real detail, and ROLLS OFF at both flat regions
        and hard edges → no speckle on silhouettes, no spurious grain on
        sky)
     4. Generate per-pixel Gaussian noise scaled by weight * std
     5. Write into LH, HL, HH band buffers (with HH at 0.5× scale)
     6. Inverse wavelet runs as normal, now with non-zero HP

   Per-plane RNG uses the SAME seed across R/G1/G2/B (intentional —
   correlated noise per Bayer position means no chroma fringing after
   demosaic). Each band uses a different seed-stream so LH ≠ HL ≠ HH.

   This is the C port of tools/hp_synth_polish.py committed at 03c81e0.
   ============================================================ */

/* Box-Muller Gaussian generator with simple xorshift32 RNG. Standalone so
   we don't need libc rand(). Produces approximately N(0, 1) doubles. */
typedef struct {
    uint32_t state;
    int has_cached;
    double cached;
} xorshift_gauss_t;

static inline uint32_t xorshift32(xorshift_gauss_t *r) {
    uint32_t x = r->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->state = x;
    return x;
}

static inline double xorshift_uniform(xorshift_gauss_t *r) {
    /* (0, 1) — never exactly 0 to keep log safe */
    return ((double)(xorshift32(r) + 1u)) / 4294967297.0;
}

static double xorshift_randn(xorshift_gauss_t *r) {
    if (r->has_cached) { r->has_cached = 0; return r->cached; }
    double u1 = xorshift_uniform(r);
    double u2 = xorshift_uniform(r);
    double rad = sqrt(-2.0 * log(u1));
    double ang = 2.0 * 3.14159265358979323846 * u2;
    r->cached = rad * sin(ang);
    r->has_cached = 1;
    return rad * cos(ang);
}

/* Histogram-based percentile. O(n) one pass instead of O(n log n) qsort.
   Two passes over the data: first finds min/max, second bins. Within the
   target bin, linear-interpolates. ~50× faster than qsort for this n. */
#define PCT_BINS 1024
static double pct_via_histogram(const float *src, size_t n, double pct)
{
    if (n == 0) return 0.0;
    /* Range scan with NEON 4-wide min/max reduction. */
    float vmin =  1e30f, vmax = -1e30f;
    size_t i = 0;
#if defined(__ARM_NEON)
    float32x4_t vmn = vdupq_n_f32(vmin);
    float32x4_t vmx = vdupq_n_f32(vmax);
    for (; i + 4 <= n; i += 4) {
        float32x4_t v = vld1q_f32(src + i);
        vmn = vminq_f32(vmn, v);
        vmx = vmaxq_f32(vmx, v);
    }
    float lo[4], hi[4];
    vst1q_f32(lo, vmn); vst1q_f32(hi, vmx);
    vmin = fminf(fminf(lo[0], lo[1]), fminf(lo[2], lo[3]));
    vmax = fmaxf(fmaxf(hi[0], hi[1]), fmaxf(hi[2], hi[3]));
#endif
    for (; i < n; i++) {
        if (src[i] < vmin) vmin = src[i];
        if (src[i] > vmax) vmax = src[i];
    }
    if (vmax <= vmin) return vmin;

    /* Histogram bin. */
    uint32_t hist[PCT_BINS] = {0};
    double scale = (PCT_BINS - 1) / (double)(vmax - vmin);
    for (size_t k = 0; k < n; k++) {
        int b = (int)((src[k] - vmin) * scale);
        if (b < 0) b = 0;
        else if (b >= PCT_BINS) b = PCT_BINS - 1;
        hist[b]++;
    }

    /* Find target bin. */
    size_t target = (size_t)((n - 1) * pct / 100.0 + 0.5);
    if (target >= n) target = n - 1;
    size_t cum = 0;
    int target_bin = PCT_BINS - 1;
    for (int b = 0; b < PCT_BINS; b++) {
        cum += hist[b];
        if (cum > target) { target_bin = b; break; }
    }
    /* Bin-center value. (Linear interp within the bin would need a second
       pass over the values in that bin; the bin-center is accurate enough
       for our application — 1024 bins → ~0.1% resolution.) */
    return vmin + ((double)target_bin + 0.5) / scale;
}

/* Synthesize HP bands from an LL band via bandpass-on-edge noise.
   Optimized C port of tools/hp_synth_polish.py with three perf wins:
     1. Histogram-based percentile (O(n)) — replaces qsort
     2. Separable running-sum box variance (2 ops/pixel after setup) —
        replaces nested 5x5 (25 ops/pixel)
     3. NEON Sobel + weight math (4-wide float32) — replaces scalar
   On Pi 5 these together bring per-channel time from ~1.6s to ~40ms.

   Same seed for all 3 bands → noise correlates → no chroma fringing. */
static void synthesize_hp_bandpass_band(const PIXEL *LL,
                                        int bw, int bh,
                                        PIXEL *LH, PIXEL *HL, PIXEL *HH,
                                        double scale,
                                        double peak_pct,
                                        double sigma_pct,
                                        uint32_t seed,
                                        /* quant divisors so we can place
                                           synth values in the *quantized*
                                           coefficient space — inverse
                                           wavelet then dequantizes back to
                                           natural scale. Without these, the
                                           inverse dequant multiplies our
                                           noise by qt[1..3] = 24,24,12 and
                                           produces int16 overflow speckle. */
                                        double dq_lh, double dq_hl, double dq_hh)
{
    size_t n = (size_t)bw * bh;
    float *grad = (float *)malloc(n * sizeof(float));
    float *std_eff = (float *)malloc(n * sizeof(float));
    float *col_sum = (float *)malloc((bw + 4) * sizeof(float));
    float *col_sumsq = (float *)malloc((bw + 4) * sizeof(float));
    if (!grad || !std_eff || !col_sum || !col_sumsq) {
        if (grad) free(grad); if (std_eff) free(std_eff);
        if (col_sum) free(col_sum); if (col_sumsq) free(col_sumsq);
        return;
    }

    /* ===== 1) Sobel gradient magnitude with NEON int32 inner loop ===== */
    for (int y = 0; y < bh; y++) {
        int ym = y > 0 ? y - 1 : 0;
        int yp = y < bh - 1 ? y + 1 : bh - 1;
        const PIXEL *r0 = LL + ym * bw;
        const PIXEL *r1 = LL + y  * bw;
        const PIXEL *r2 = LL + yp * bw;
        float *out = grad + y * bw;

        /* x=0 boundary scalar (xm clamps to 0) */
        {
            int xp = bw > 1 ? 1 : 0;
            float gx = (float)(r0[xp] - r0[0])
                    + 2.0f * (r1[xp] - r1[0])
                    +        (r2[xp] - r2[0]);
            float gy = (float)(r2[0]  - r0[0])
                    + 2.0f * (r2[0]  - r0[0])
                    +        (r2[xp] - r0[xp]);
            out[0] = sqrtf(gx * gx + gy * gy);
        }

        int x = 1;
#if defined(__ARM_NEON)
        /* PIXEL is int32_t; use vld1q_s32 to load 4 wide. Interior pixels
           only: x in [1, bw-1), processing 4 at a time with neighbors at
           x-1 and x+1. The same load address for r0+x-1 etc. gives the
           4 neighbor values for output lanes x, x+1, x+2, x+3. */
        for (; x + 4 <= bw - 1; x += 4) {
            int32x4_t r0m = vld1q_s32((const int32_t *)(r0 + x - 1));
            int32x4_t r0c = vld1q_s32((const int32_t *)(r0 + x));
            int32x4_t r0p = vld1q_s32((const int32_t *)(r0 + x + 1));
            int32x4_t r1m = vld1q_s32((const int32_t *)(r1 + x - 1));
            int32x4_t r1p = vld1q_s32((const int32_t *)(r1 + x + 1));
            int32x4_t r2m = vld1q_s32((const int32_t *)(r2 + x - 1));
            int32x4_t r2c = vld1q_s32((const int32_t *)(r2 + x));
            int32x4_t r2p = vld1q_s32((const int32_t *)(r2 + x + 1));

            /* gx = (r0p - r0m) + 2*(r1p - r1m) + (r2p - r2m) */
            int32x4_t dx0 = vsubq_s32(r0p, r0m);
            int32x4_t dx1 = vsubq_s32(r1p, r1m);
            int32x4_t dx2 = vsubq_s32(r2p, r2m);
            int32x4_t gx = vaddq_s32(vaddq_s32(dx0, dx2),
                                     vshlq_n_s32(dx1, 1));
            /* gy = (r2m + 2*r2c + r2p) - (r0m + 2*r0c + r0p) */
            int32x4_t top = vaddq_s32(vaddq_s32(r0m, r0p), vshlq_n_s32(r0c, 1));
            int32x4_t bot = vaddq_s32(vaddq_s32(r2m, r2p), vshlq_n_s32(r2c, 1));
            int32x4_t gy = vsubq_s32(bot, top);

            float32x4_t fgx = vcvtq_f32_s32(gx);
            float32x4_t fgy = vcvtq_f32_s32(gy);
            float32x4_t mag = vsqrtq_f32(vmlaq_f32(vmulq_f32(fgx, fgx), fgy, fgy));
            vst1q_f32(out + x, mag);
        }
#endif
        /* Scalar tail (rightmost pixels including x=bw-1 boundary) */
        for (; x < bw; x++) {
            int xp = x < bw - 1 ? x + 1 : bw - 1;
            float gx = (float)(r0[xp] - r0[x - 1])
                    + 2.0f * (r1[xp] - r1[x - 1])
                    +        (r2[xp] - r2[x - 1]);
            float gy = (float)(r2[x - 1] - r0[x - 1])
                    + 2.0f * (r2[x]     - r0[x])
                    +        (r2[xp]    - r0[xp]);
            out[x] = sqrtf(gx * gx + gy * gy);
        }
    }

    double grad_peak  = pct_via_histogram(grad, n, peak_pct);
    double grad_top   = pct_via_histogram(grad, n, peak_pct + sigma_pct);
    double grad_sigma = (grad_top > grad_peak) ? grad_top - grad_peak : 1e-6;

    /* ===== 2) Separable 5x5 box variance via running sums ===== */
    /* For each column, maintain running sum & sum-of-squares of LL.
       Then for each output row, sliding-window sum-of-cols gives 5x5
       box statistics in 2 ops/pixel after the per-column work. */
    /* col_sum[x] = sum of LL values in current 5-row window at column x.
       col_sumsq[x] = same for squared values.
       At the start, we prime col_sum with the first 5 rows (rows 0..4),
       using row replication for rows -2..-1 (which equal row 0). */
    for (int x = 0; x < bw; x++) {
        float s = 0, s2 = 0;
        for (int dy = -2; dy <= 2; dy++) {
            int yy = dy < 0 ? 0 : (dy < bh ? dy : bh - 1);
            float v = (float)LL[yy * bw + x];
            s += v; s2 += v * v;
        }
        col_sum[x] = s; col_sumsq[x] = s2;
    }
    for (int y = 0; y < bh; y++) {
        /* Now compute per-pixel 5-col-window variance via running sum
           across x. Window size = 25 = 5x5. */
        float win_s = 0, win_s2 = 0;
        for (int dx = -2; dx <= 2; dx++) {
            int xx = dx < 0 ? 0 : (dx < bw ? dx : bw - 1);
            win_s += col_sum[xx];
            win_s2 += col_sumsq[xx];
        }
        float *out = std_eff + y * bw;
        for (int x = 0; x < bw; x++) {
            float mean = win_s * (1.0f / 25.0f);
            float var = win_s2 * (1.0f / 25.0f) - mean * mean;
            out[x] = var > 0.0f ? sqrtf(var) : 0.0f;
            /* slide x window: drop col x-2, add col x+3 */
            int drop = x - 2 < 0 ? 0 : x - 2;
            int add  = x + 3 < bw ? x + 3 : bw - 1;
            win_s  += col_sum[add]  - col_sum[drop];
            win_s2 += col_sumsq[add] - col_sumsq[drop];
        }
        /* slide y: prepare col_sum for row y+1: drop row y-2, add row y+3 */
        int drop_row = y - 2 < 0 ? 0 : y - 2;
        int add_row  = y + 3 < bh ? y + 3 : bh - 1;
        for (int x = 0; x < bw; x++) {
            float vdrop = (float)LL[drop_row * bw + x];
            float vadd  = (float)LL[add_row  * bw + x];
            col_sum[x]   += vadd - vdrop;
            col_sumsq[x] += vadd*vadd - vdrop*vdrop;
        }
    }
    free(col_sum); free(col_sumsq);

    double std_cap = pct_via_histogram(std_eff, n, 90.0);

    /* ===== 3) Combine: weight = Gaussian(grad - peak, sigma) * min(std, cap) * scale
                NEON 4-wide computation including approximate exp(). ===== */
    float fpeak  = (float)grad_peak;
    float finvsig = 1.0f / (float)grad_sigma;
    float fcap   = (float)std_cap;
    float fscale = (float)scale;
    size_t i = 0;
#if defined(__ARM_NEON)
    float32x4_t vpeak  = vdupq_n_f32(fpeak);
    float32x4_t vinvsig = vdupq_n_f32(finvsig);
    float32x4_t vcap   = vdupq_n_f32(fcap);
    float32x4_t vscale = vdupq_n_f32(fscale);
    float32x4_t vmhalf = vdupq_n_f32(-0.5f);
    for (; i + 4 <= n; i += 4) {
        float32x4_t g = vld1q_f32(grad + i);
        float32x4_t s = vld1q_f32(std_eff + i);
        float32x4_t d = vmulq_f32(vsubq_f32(g, vpeak), vinvsig);
        float32x4_t arg = vmulq_f32(vmhalf, vmulq_f32(d, d));
        /* Polynomial approximation to exp(arg) for arg in [-10, 0]. Outside
           that range result is ~0 anyway. exp(x) ≈ ((((x/16)+1)^16) using
           repeated squaring — exact-enough for our weight purpose. */
        float32x4_t e = vmlaq_f32(vdupq_n_f32(1.0f), arg, vdupq_n_f32(1.0f/16.0f));
        e = vmulq_f32(e, e); e = vmulq_f32(e, e);
        e = vmulq_f32(e, e); e = vmulq_f32(e, e);
        /* Clip to [0, 1] for safety. */
        e = vmaxq_f32(e, vdupq_n_f32(0.0f));
        e = vminq_f32(e, vdupq_n_f32(1.0f));
        float32x4_t sclip = vminq_f32(s, vcap);
        float32x4_t out = vmulq_f32(vmulq_f32(e, sclip), vscale);
        vst1q_f32(std_eff + i, out);
    }
#endif
    for (; i < n; i++) {
        float d = (grad[i] - fpeak) * finvsig;
        float e = expf(-0.5f * d * d);
        float s = std_eff[i] < fcap ? std_eff[i] : fcap;
        std_eff[i] = e * s * fscale;
    }
    free(grad);

    /* ===== 4) Generate per-pixel noise (scalar — Box-Muller + RNG state
                is hard to vectorize; Pi cost ~5ms at this stage). ===== */
    xorshift_gauss_t rng_lh = { seed ^ 0xA5A5A5A5u, 0, 0.0 };
    xorshift_gauss_t rng_hl = { seed ^ 0x5A5A5A5Au, 0, 0.0 };
    xorshift_gauss_t rng_hh = { seed ^ 0xCAFEBABEu, 0, 0.0 };
    /* Divide synth output by the band's quant divisor so the inverse
       wavelet's internal dequant multiplies it BACK to natural scale.
       Without this, qt = {24,24,12} produces int16 overflow speckle. */
    double inv_dq_lh = 1.0 / (dq_lh > 0 ? dq_lh : 1.0);
    double inv_dq_hl = 1.0 / (dq_hl > 0 ? dq_hl : 1.0);
    double inv_dq_hh = 1.0 / (dq_hh > 0 ? dq_hh : 1.0);
    for (size_t k = 0; k < n; k++) {
        float e = std_eff[k];
        double lh = xorshift_randn(&rng_lh) * e * inv_dq_lh;
        double hl = xorshift_randn(&rng_hl) * e * inv_dq_hl;
        double hh = xorshift_randn(&rng_hh) * e * 0.5 * inv_dq_hh;
        /* Clamp to int16 range to prevent overflow if a Gaussian tail event
           coincides with a high-weight pixel. */
        if (lh >  32767.0) lh =  32767.0; else if (lh < -32768.0) lh = -32768.0;
        if (hl >  32767.0) hl =  32767.0; else if (hl < -32768.0) hl = -32768.0;
        if (hh >  32767.0) hh =  32767.0; else if (hh < -32768.0) hh = -32768.0;
        LH[k] = (PIXEL)((int32_t)(lh + (lh >= 0 ? 0.5 : -0.5)));
        HL[k] = (PIXEL)((int32_t)(hl + (hl >= 0 ? 0.5 : -0.5)));
        HH[k] = (PIXEL)((int32_t)(hh + (hh >= 0 ? 0.5 : -0.5)));
    }
    free(std_eff);
}

static void *fused_inv_wavelet_runner(void *arg) {
    FUSED_INV_TASK *t = (FUSED_INV_TASK *)arg;
    gpr_allocator alloc = { fd_alloc, fd_free };
    if (t->descale == 0) {
        t->err = InvertSpatialQuant16s(&alloc,
            t->ll, t->bw * (int)sizeof(PIXEL),
            t->lh, t->bw * (int)sizeof(PIXEL),
            t->hl, t->bw * (int)sizeof(PIXEL),
            t->hh, t->bw * (int)sizeof(PIXEL),
            t->out_channel, t->ch_w * (int)sizeof(PIXEL),
            (DIMENSION)t->bw, (DIMENSION)t->bh,
            (DIMENSION)t->ch_w, (DIMENSION)t->ch_h,
            t->q);
    } else {
        t->err = InvertSpatialQuantDescale16s(&alloc,
            t->ll, t->bw * (int)sizeof(PIXEL),
            t->lh, t->bw * (int)sizeof(PIXEL),
            t->hl, t->bw * (int)sizeof(PIXEL),
            t->hh, t->bw * (int)sizeof(PIXEL),
            t->out_channel, t->ch_w * (int)sizeof(PIXEL),
            (DIMENSION)t->bw, (DIMENSION)t->bh,
            (DIMENSION)t->ch_w, (DIMENSION)t->ch_h,
            t->descale, t->q);
    }
    return NULL;
}

/* Single-level + LL decode path (hdr.num_bands == 16, hdr.multi_level == 0).
   Band layout per channel (4 slots):
     0 = LL1   (bw × bh)
     1 = LH1   (bw × bh)
     2 = HL1   (bw × bh)
     3 = HH1   (bw × bh)
   Channels: GS=0, RG=1, BG=2, GD=3.

   Handles optional channel-space decimation via hdr.decimate: when set
   to 2, the encoded bands represent a (hdr.width/2 × hdr.height/2) Bayer
   image. Output bayer dims are adjusted accordingly. */
static int decode_fused_single_level_ll(const FUSED_HEADER *hdr,
                                        const uint8_t *enc, size_t enc_size,
                                        uint16_t *bayer_out, size_t bayer_pitch_bytes,
                                        int *out_width, int *out_height);

/* Internal worker for the multi-level decode. half_res=0 returns full-resolution
   bayer (hdr.width × hdr.height); half_res=1 stops one wavelet level early and
   returns a half-resolution bayer (hdr.width/2 × hdr.height/2). The half-res
   path is the playback-pipeline default for FUSED video (matches the legacy
   GPRCodec topology that fed the CNN at codec-half-res). */
static int gpr_decode_fused_impl(const uint8_t *enc, size_t enc_size,
                                  uint16_t *bayer_out, size_t bayer_pitch_bytes,
                                  int *out_width, int *out_height,
                                  int half_res)
{
    if (!enc || !bayer_out) return -1;
    if (enc_size < sizeof(FUSED_HEADER)) return -2;

    FUSED_HEADER hdr;
    memcpy(&hdr, enc, sizeof(hdr));
    if (hdr.magic != FUSED_MAGIC) return -3;
    if (hdr.version != FUSED_VERSION) return -4;
    /* Route single-level-with-LL files to the 16-band path. half_res isn't
       defined for single-level streams (only one wavelet level exists), so we
       just decode at native dims. */
    if (!hdr.multi_level && hdr.num_bands == 16) {
        return decode_fused_single_level_ll(&hdr, enc, enc_size,
                                            bayer_out, bayer_pitch_bytes,
                                            out_width, out_height);
    }
    if (!hdr.multi_level) return -5;  /* single-level-without-LL: not decodable */
    /* hdr.multi_level encodes the wavelet depth (2 or 3 in the new format;
       legacy bool=1 streams equal 3-level via num_bands==40 check). */
    int levels = (int)hdr.multi_level;
    if (levels == 1) levels = 3;  /* legacy bool=1 means 3-level */
    if (levels == 2) {
        if (hdr.num_bands != 28) return -6;
    } else if (levels == 3) {
        if (hdr.num_bands != 40) return -6;
    } else {
        return -6;
    }
    if (hdr.quality >= 12) return -7;

    /* Apply hdr.decimate from the bitstream: when set to 2, the encoded
       bands represent a (hdr.width/2 × hdr.height/2) Bayer-equivalent image
       (the encoder applied channel-space decimation). Bayer output dims
       and per-level band dims all shrink by `dec` accordingly. */
    int dec = (hdr.decimate == 2) ? 2 : 1;
    int bayer_w = (int)hdr.width  / dec;
    int bayer_h = (int)hdr.height / dec;
    if (out_width)  *out_width  = half_res ? bayer_w  / 2 : bayer_w;
    if (out_height) *out_height = half_res ? bayer_h / 2 : bayer_h;

    SetupDecoderLogCurve();

    /* Band-size table */
    size_t off = sizeof(FUSED_HEADER);
    uint32_t band_sizes[40];
    memcpy(band_sizes, enc + off, sizeof(uint32_t) * hdr.num_bands);
    off += sizeof(uint32_t) * hdr.num_bands;

    /* Dimensions per level — ceil at each step to match the encoder's
       odd-width handling (otherwise odd intermediate widths drop the
       last column on the way down the pyramid). */
    int ch_w = bayer_w / 2;
    int ch_h = bayer_h / 2;
    int bw1  = ch_w / 2, bh1 = ch_h / 2;
    int bw2  = (bw1 + 1) / 2, bh2 = (bh1 + 1) / 2;
    int bw3  = (bw2 + 1) / 2, bh3 = (bh2 + 1) / 2;

    /* Slot widths/heights — matches encoder write order:
         levels==3 (10 slots):
           0..2 = LH1/HL1/HH1   (bw1×bh1)
           3..5 = LH2/HL2/HH2   (bw2×bh2)
           6..8 = LH3/HL3/HH3   (bw3×bh3)
           9    = LL3           (bw3×bh3)
         levels==2 (7 slots):
           0..2 = LH1/HL1/HH1   (bw1×bh1)
           3..5 = LH2/HL2/HH2   (bw2×bh2)
           6    = LL2           (bw2×bh2) */
    int slot_w[10], slot_h[10];
    int n_slots = (levels == 2) ? 7 : 10;
    int dbg_timing = 0;
    {
        const char *e = getenv("GPR_DECODE_TIMING");
        if (e && *e == '1') dbg_timing = 1;
    }
    int l2_hp_mask = 7;
    {
        const char *drop = getenv("GPR_DECODE_HALFRES_DROP_L2_HP");
        const char *mask = getenv("GPR_DECODE_HALFRES_L2_MASK");
        if (half_res && drop && *drop == '1') l2_hp_mask = 0;
        if (half_res && mask && *mask) {
            char *end = NULL;
            long v = strtol(mask, &end, 0);
            if (end != mask && v >= 0 && v <= 7) l2_hp_mask = (int)v;
        }
    }
    if (levels == 2) {
        slot_w[0]=bw1; slot_w[1]=bw1; slot_w[2]=bw1;
        slot_w[3]=bw2; slot_w[4]=bw2; slot_w[5]=bw2;
        slot_w[6]=bw2;
        slot_h[0]=bh1; slot_h[1]=bh1; slot_h[2]=bh1;
        slot_h[3]=bh2; slot_h[4]=bh2; slot_h[5]=bh2;
        slot_h[6]=bh2;
    } else {
        slot_w[0]=bw1; slot_w[1]=bw1; slot_w[2]=bw1;
        slot_w[3]=bw2; slot_w[4]=bw2; slot_w[5]=bw2;
        slot_w[6]=bw3; slot_w[7]=bw3; slot_w[8]=bw3; slot_w[9]=bw3;
        slot_h[0]=bh1; slot_h[1]=bh1; slot_h[2]=bh1;
        slot_h[3]=bh2; slot_h[4]=bh2; slot_h[5]=bh2;
        slot_h[6]=bh3; slot_h[7]=bh3; slot_h[8]=bh3; slot_h[9]=bh3;
    }

    /* Decode all bands into per-channel, per-slot int32 buffers. */
    PIXEL *bands[4][10];
    for (int ch = 0; ch < 4; ch++)
        for (int s = 0; s < 10; s++)
            bands[ch][s] = NULL;

    int band_idx = 0;
    int rc = 0;
    double dt_band0 = _decode_ms();
    if (half_res && levels == 2) {
        size_t band_offsets[4 * 10];
        for (int ch = 0; ch < 4 && rc == 0; ch++) {
            for (int s = 0; s < n_slots && rc == 0; s++) {
                int bw = slot_w[s], bh = slot_h[s];
                uint32_t sz = band_sizes[band_idx];
                if (off + sz > enc_size) { rc = -10; break; }
                band_offsets[band_idx] = off;
                if (!(s <= 2 || (s >= 3 && s <= 5 && !(l2_hp_mask & (1 << (s - 3)))))) {
                    bands[ch][s] = (PIXEL *)malloc((size_t)bw * bh * sizeof(PIXEL));
                    if (!bands[ch][s]) { rc = -11; break; }
                }
                off += sz;
                band_idx++;
            }
        }
        if (rc == 0) {
            FUSED_LEVEL2_BAND_TASK tasks[4];
            pthread_t threads[4];
            int created[4] = {0};
            for (int ch = 0; ch < 4; ch++) {
                tasks[ch].enc = enc;
                tasks[ch].band_sizes = band_sizes;
                tasks[ch].band_offsets = band_offsets;
                tasks[ch].bands = bands[ch];
                tasks[ch].slot_w = slot_w;
                tasks[ch].slot_h = slot_h;
                tasks[ch].n_slots = n_slots;
                tasks[ch].ch = ch;
                tasks[ch].half_res = half_res;
                tasks[ch].l2_hp_mask = l2_hp_mask;
                tasks[ch].rc = 0;
            }
            for (int ch = 0; ch < 4; ch++) {
                created[ch] = (pthread_create(&threads[ch], NULL,
                                               fused_level2_band_decode_runner,
                                               &tasks[ch]) == 0);
                if (!created[ch]) fused_level2_band_decode_runner(&tasks[ch]);
            }
            for (int ch = 0; ch < 4; ch++) {
                if (created[ch]) pthread_join(threads[ch], NULL);
                if (tasks[ch].rc != 0 && rc == 0) rc = tasks[ch].rc;
            }
        }
    } else {
        for (int ch = 0; ch < 4 && rc == 0; ch++) {
            for (int s = 0; s < n_slots && rc == 0; s++) {
                int bw = slot_w[s], bh = slot_h[s];
                uint32_t sz = band_sizes[band_idx];
                if (off + sz > enc_size) { rc = -10; break; }
                bands[ch][s] = (PIXEL *)malloc((size_t)bw * bh * sizeof(PIXEL));
                if (!bands[ch][s]) { rc = -11; break; }
                int drc = jans_decode_band_x4(enc + off, sz,
                                               bands[ch][s], bw, bh,
                                               bw * (int)sizeof(PIXEL));
                if (drc != 0) { rc = -12; break; }
                off += sz;
                band_idx++;
            }
        }
    }
    if (dbg_timing) fprintf(stderr, "  decode band_decode: %.1f ms\n", _decode_ms() - dt_band0);

    /* Per-channel inverse wavelet: level 3 → level 2 → level 1. */
    gpr_allocator alloc = { fd_alloc, fd_free };
    PIXEL *channels[4] = { NULL, NULL, NULL, NULL };

    if (rc == 0) {
        const QUANT *qt = get_quant_table(hdr.quality);
        /* The decoder's DequantizeBandRow16s interprets a positive QUANT as
           "VC5 companded + quantized" (apply UncompandedValueFast then
           multiply by quant). The fused encoder doesn't compand, so we use
           the negative-quant convention to tell DequantizeBandRow16s to
           just multiply by |q| without uncompanding. */
        /* Level-3 quant: {LL3=qt[0], LH3=qt[1], HL3=qt[2], HH3=qt[3]} */
        QUANT q_l3[4] = { -qt[0], -qt[1], -qt[2], -qt[3] };
        /* Level-2 quant: {LL=1 (intermediate, not encoded), LH2=qt[4], HL2=qt[5], HH2=qt[6]} */
        QUANT q_l2[4] = { -1, -qt[4], -qt[5], -qt[6] };
        /* Level-1 quant: {LL=1 (intermediate), LH1=qt[7], HL1=qt[8], HH1=qt[9]} */
        QUANT q_l1[4] = { -1, -qt[7], -qt[8], -qt[9] };

        /* Manually dequantize the deepest LL band (the inverse function
           only dequantizes LH/HL/HH; LL gets passed straight through).
           rANS class-15 ceiling. LL2 can be lowered for native-12MP quality
           experiments through FUSED_LL2_DIVISOR; LL3 remains fixed at 16 for
           existing three-level streams. */
        #define FUSED_LL3_EXTRA_DIVISOR 16  /* kept for back-compat string */
        int ll_dequant = qt[0] * fused_ll_extra_divisor(levels);
        int deepest_slot = (levels == 2) ? 6 : 9;
        int deepest_bw = (levels == 2) ? bw2 : bw3;
        int deepest_bh = (levels == 2) ? bh2 : bh3;
#ifdef GPR_DEBUG_COEFF_IO
        {
            const char *_dump_quant_dir = getenv("GPR_DUMP_DECODE_QUANT_COEFFS");
            if (_dump_quant_dir && *_dump_quant_dir) {
                for (int ch = 0; ch < 4; ch++) {
                    for (int s = 0; s <= deepest_slot; s++) {
                        if (!bands[ch][s]) continue;
                        char path[1024];
                        snprintf(path, sizeof(path), "%s/ch%d_s%d_w%d_h%d.s32",
                                 _dump_quant_dir, ch, s, slot_w[s], slot_h[s]);
                        FILE *f = fopen(path, "wb");
                        if (f) {
                            size_t n = (size_t)slot_w[s] * (size_t)slot_h[s];
                            fwrite(bands[ch][s], sizeof(PIXEL), n, f);
                            fclose(f);
                        }
                    }
                }
            }
        }
#endif /* GPR_DEBUG_COEFF_IO */
        double dt_ll0 = _decode_ms();
        for (int ch = 0; ch < 4; ch++) {
            PIXEL *p = bands[ch][deepest_slot];
            if (!p) continue;
            size_t n = (size_t)deepest_bw * deepest_bh;
            for (size_t i = 0; i < n; i++) p[i] *= ll_dequant;
        }
        if (dbg_timing) fprintf(stderr, "  decode ll_dequant: %.1f ms\n", _decode_ms() - dt_ll0);

        /* Coefficient I/O hooks for codec-anchored refinement experiments
           (task #233). Compile-guarded so production builds get zero code
           and zero overhead. At this point bands[ch][s] holds:
             - quantized int32 coefficients for s=0..(deepest_slot-1)
             - dequantized LL for s=deepest_slot
           Zero in the quantized HF buffers means "quantized away" — a
           trustable signal for the data-consistency projection idea.
           Build with -DGPR_DEBUG_COEFF_IO to enable. */
#ifdef GPR_DEBUG_COEFF_IO
        {
            const char *_dump_dir = getenv("GPR_DUMP_COEFFS");
            const char *_load_dir = getenv("GPR_LOAD_COEFFS");
            if (_dump_dir && *_dump_dir) {
                for (int ch = 0; ch < 4; ch++) {
                    for (int s = 0; s <= deepest_slot; s++) {
                        if (!bands[ch][s]) continue;
                        char path[1024];
                        snprintf(path, sizeof(path), "%s/ch%d_s%d_w%d_h%d.s32",
                                 _dump_dir, ch, s, slot_w[s], slot_h[s]);
                        FILE *f = fopen(path, "wb");
                        if (f) {
                            size_t n = (size_t)slot_w[s] * slot_h[s];
                            fwrite(bands[ch][s], sizeof(PIXEL), n, f);
                            fclose(f);
                        }
                    }
                }
            }
            if (_load_dir && *_load_dir) {
                for (int ch = 0; ch < 4; ch++) {
                    for (int s = 0; s <= deepest_slot; s++) {
                        if (!bands[ch][s]) continue;
                        char path[1024];
                        snprintf(path, sizeof(path), "%s/ch%d_s%d_w%d_h%d.s32",
                                 _load_dir, ch, s, slot_w[s], slot_h[s]);
                        FILE *f = fopen(path, "rb");
                        if (f) {
                            size_t n = (size_t)slot_w[s] * slot_h[s];
                            size_t r = fread(bands[ch][s], sizeof(PIXEL), n, f);
                            fclose(f);
                            if (r != n) {
                                fprintf(stderr,
                                    "GPR_LOAD_COEFFS: short read for %s (got %zu, want %zu)\n",
                                    path, r, n);
                            }
                        }
                    }
                }
            }
        }
#endif /* GPR_DEBUG_COEFF_IO */

        if (half_res && levels == 2) {
            int use_halfres_stream = 1;
            {
                const char *e = getenv("GPR_DECODE_HALFRES_STREAM");
                if (e && *e == '0') use_halfres_stream = 0;
            }
            if (use_halfres_stream) {
                PIXEL *stream_bands[4][4];
                memset(stream_bands, 0, sizeof(stream_bands));
                PIXEL *zero_hp = NULL;
                if (l2_hp_mask != 7) {
                    zero_hp = (PIXEL *)calloc((size_t)bw2 * bh2, sizeof(PIXEL));
                    if (!zero_hp) rc = -24;
                }
                for (int ch = 0; ch < 4 && rc == 0; ch++) {
                    stream_bands[ch][0] = bands[ch][6];
                    stream_bands[ch][1] = bands[ch][3] ? bands[ch][3] : zero_hp;
                    stream_bands[ch][2] = bands[ch][4] ? bands[ch][4] : zero_hp;
                    stream_bands[ch][3] = bands[ch][5] ? bands[ch][5] : zero_hp;
                }
                if (rc == 0) {
                    int log_max  = (1 << (int)hdr.log_bits) - 1;
                    int midpoint = 1 << ((int)hdr.log_bits - 1);
                    const uint16_t *log_table =
                        (hdr.log_bits <= 14) ? DecoderLogCurve14 : DecoderLogCurve16;
                    int output_bit_depth = (int)hdr.log_bits;
                    int shift = 16 - output_bit_depth;
                    int is_rggb = (int)hdr.is_rggb;
                    QUANT qt_l2_pos[4] = { 1, qt[4], qt[5], qt[6] };
                    double dt_fs0 = _decode_ms();
                    int frc = gpr_decode_fused_stream(stream_bands, bw2, bh2, bw1, bh1,
                                                       qt_l2_pos,
                                                       log_max, midpoint, shift, is_rggb,
                                                       log_table,
                                                       (uint8_t *)bayer_out, bayer_pitch_bytes,
                                                       2,
                                                       l2_hp_mask == 0);
                    if (frc != 0) rc = -30;
                    if (dbg_timing) fprintf(stderr, "  decode fused_stream_l2: %.1f ms\n",
                                            _decode_ms() - dt_fs0);
                }
                if (zero_hp) free(zero_hp);
                for (int ch = 0; ch < 4; ch++)
                    for (int s = 0; s < 10; s++)
                        if (bands[ch][s]) free(bands[ch][s]);
                return rc;
            }
        }

        double dt_wave0 = _decode_ms();
        if (half_res && levels == 2) {
            FUSED_INV_TASK tasks[4];
            pthread_t threads[4];
            int created[4] = {0};
            PIXEL *zero_hp = NULL;
            if (l2_hp_mask != 7) {
                zero_hp = (PIXEL *)calloc((size_t)bw2 * bh2, sizeof(PIXEL));
                if (!zero_hp) rc = -24;
            }
            for (int ch = 0; ch < 4 && rc == 0; ch++) {
                channels[ch] = (PIXEL *)malloc((size_t)bw1 * bh1 * sizeof(PIXEL));
                if (!channels[ch]) { rc = -24; break; }
                tasks[ch].ll = bands[ch][6];
                tasks[ch].lh = bands[ch][3] ? bands[ch][3] : zero_hp;
                tasks[ch].hl = bands[ch][4] ? bands[ch][4] : zero_hp;
                tasks[ch].hh = bands[ch][5] ? bands[ch][5] : zero_hp;
                tasks[ch].out_channel = channels[ch];
                tasks[ch].bw = bw2;
                tasks[ch].bh = bh2;
                tasks[ch].ch_w = bw1;
                tasks[ch].ch_h = bh1;
                tasks[ch].q = q_l2;
                tasks[ch].err = CODEC_ERROR_OKAY;
            }
            if (rc == 0) {
                for (int ch = 0; ch < 4; ch++) {
                    created[ch] = (pthread_create(&threads[ch], NULL,
                        fused_inv_wavelet_runner, &tasks[ch]) == 0);
                    if (!created[ch]) fused_inv_wavelet_runner(&tasks[ch]);
                }
                for (int ch = 0; ch < 4; ch++) {
                    if (created[ch]) pthread_join(threads[ch], NULL);
                    if (tasks[ch].err != CODEC_ERROR_OKAY) rc = -23;
                }
            }
            if (zero_hp) free(zero_hp);
        } else {
        for (int ch = 0; ch < 4 && rc == 0; ch++) {
            /* Per-level descale values default to descale=2 everywhere
               (mirroring the encoder's prescale=2 at every level). Override
               via FUSED_INVERSE_DESCALE="l1,l2,l3" for experimentation. */
            const char *_dsenv = getenv("FUSED_INVERSE_DESCALE");
            int ds_l1 = (int)hdr.prescale, ds_l2 = 2, ds_l3 = 2;
            if (_dsenv && *_dsenv) {
                int a = 0, b = 0, c = 0;
                if (sscanf(_dsenv, "%d,%d,%d", &a, &b, &c) == 3) {
                    ds_l1 = a; ds_l2 = b; ds_l3 = c;
                }
            }
            const char *_isenv = getenv("FUSED_INTERLEVEL_SCALE");
            int scale_l3_to_l2 = 1, scale_l2_to_l1 = 1;
            if (_isenv && *_isenv) {
                int a = 1, b = 1;
                if (sscanf(_isenv, "%d,%d", &a, &b) == 2) {
                    if (a >= 1 && a <= 16) scale_l3_to_l2 = a;
                    if (b >= 1 && b <= 16) scale_l2_to_l1 = b;
                }
            }

            PIXEL *ll2;
            CODEC_ERROR e;
            if (levels == 3) {
                /* Level 3 inverse: bands[ch][9] = LL3, [6/7/8] = LH3/HL3/HH3 → LL2 */
                ll2 = (PIXEL *)malloc((size_t)bw2 * bh2 * sizeof(PIXEL));
                if (!ll2) { rc = -20; break; }
                e = InvertSpatialQuantDescale16s(&alloc,
                    bands[ch][9], bw3 * (int)sizeof(PIXEL),
                    bands[ch][6], bw3 * (int)sizeof(PIXEL),
                    bands[ch][7], bw3 * (int)sizeof(PIXEL),
                    bands[ch][8], bw3 * (int)sizeof(PIXEL),
                    ll2, bw2 * (int)sizeof(PIXEL),
                    (DIMENSION)bw3, (DIMENSION)bh3,
                    (DIMENSION)bw2, (DIMENSION)bh2,
                    /*descale=*/ds_l3, q_l3);
                if (e != CODEC_ERROR_OKAY) { free(ll2); rc = -21; break; }
                fused_scale_pixels(ll2, (size_t)bw2 * (size_t)bh2, scale_l3_to_l2);
            } else {
                /* 2-level: LL2 is read directly from bands[ch][6]. We need
                   to PASS THROUGH that pointer as-is, but the function
                   below frees ll2 at the end of L2 inverse. So make a copy
                   we can free safely. */
                ll2 = (PIXEL *)malloc((size_t)bw2 * bh2 * sizeof(PIXEL));
                if (!ll2) { rc = -20; break; }
                memcpy(ll2, bands[ch][6], (size_t)bw2 * bh2 * sizeof(PIXEL));
            }

            /* Level 2 inverse: ll2 + LH2/HL2/HH2 → LL1 */
            PIXEL *ll1 = (PIXEL *)malloc((size_t)bw1 * bh1 * sizeof(PIXEL));
            if (!ll1) { free(ll2); rc = -22; break; }
            e = InvertSpatialQuantDescale16s(&alloc,
                ll2,          bw2 * (int)sizeof(PIXEL),
                bands[ch][3], bw2 * (int)sizeof(PIXEL),
                bands[ch][4], bw2 * (int)sizeof(PIXEL),
                bands[ch][5], bw2 * (int)sizeof(PIXEL),
                ll1, bw1 * (int)sizeof(PIXEL),
                (DIMENSION)bw2, (DIMENSION)bh2,
                (DIMENSION)bw1, (DIMENSION)bh1,
                /*descale=*/ds_l2, q_l2);
            free(ll2);
            if (e != CODEC_ERROR_OKAY) { free(ll1); rc = -23; break; }
            fused_scale_pixels(ll1, (size_t)bw1 * (size_t)bh1, scale_l2_to_l1);

            if (half_res) {
                /* Half-res output: LL1 is the channel. Skip level-1 inverse,
                   color-untransform at LL1 dims below. The level-1 highpass
                   bands (LH1/HL1/HH1) are left in the bitstream — they cost
                   bits but aren't reconstructed here. (Bitrate optimization
                   waits on the encoder-side decimate work; see task #158.) */
                channels[ch] = ll1;
            } else {
                /* Level 1 inverse: ll1 + LH1/HL1/HH1 → channel */
                channels[ch] = (PIXEL *)malloc((size_t)ch_w * ch_h * sizeof(PIXEL));
                if (!channels[ch]) { free(ll1); rc = -24; break; }
                e = InvertSpatialQuantDescale16s(&alloc,
                    ll1,          bw1 * (int)sizeof(PIXEL),
                    bands[ch][0], bw1 * (int)sizeof(PIXEL),
                    bands[ch][1], bw1 * (int)sizeof(PIXEL),
                    bands[ch][2], bw1 * (int)sizeof(PIXEL),
                    channels[ch], ch_w * (int)sizeof(PIXEL),
                    (DIMENSION)bw1, (DIMENSION)bh1,
                    (DIMENSION)ch_w, (DIMENSION)ch_h,
                    /*descale=*/ds_l1, q_l1);
                free(ll1);
                if (e != CODEC_ERROR_OKAY) { rc = -25; break; }
            }
        }
        }
        if (dbg_timing) fprintf(stderr, "  decode wavelet inv: %.1f ms\n", _decode_ms() - dt_wave0);
    }

    /* Free band buffers; we've consumed them into the channel planes. */
    for (int ch = 0; ch < 4; ch++)
        for (int s = 0; s < 10; s++)
            if (bands[ch][s]) free(bands[ch][s]);

    /* Reverse the GS/RG/BG/GD color transform and apply the inverse
       log curve. Output bit-depth: match log_bits (12, 14, or 16).
       Channel dimensions depend on half_res: full-res uses ch_w × ch_h,
       half-res uses bw1 × bh1 (LL1 was kept as the channel buffer). */
    int chan_w = half_res ? bw1 : ch_w;
    int chan_h = half_res ? bh1 : ch_h;
    if (rc == 0) {
        {
            const char *_dump_channels = getenv("GPR_DUMP_DECODE_CHANNELS");
            if (_dump_channels && *_dump_channels) {
                for (int ch = 0; ch < 4; ch++) {
                    if (!channels[ch]) continue;
                    char path[1024];
                    snprintf(path, sizeof(path), "%s/ch%d_w%d_h%d.s32",
                             _dump_channels, ch, chan_w, chan_h);
                    FILE *f = fopen(path, "wb");
                    if (f) {
                        fwrite(channels[ch], sizeof(PIXEL),
                               (size_t)chan_w * (size_t)chan_h, f);
                        fclose(f);
                    }
                }
            }
        }
        double dt_color0 = _decode_ms();
        int log_max  = (1 << (int)hdr.log_bits) - 1;
        int midpoint = 1 << ((int)hdr.log_bits - 1);
        const uint16_t *log_table =
            (hdr.log_bits <= 14) ? DecoderLogCurve14 : DecoderLogCurve16;
        int output_bit_depth = (int)hdr.log_bits;
        int shift = 16 - output_bit_depth;
        int is_rggb = (int)hdr.is_rggb;

        FUSED_COLOR_TASK ct[4];
        pthread_t cth[4];
        int created[4] = {0};
        int nthreads = 4;
        int rows_per = (chan_h + nthreads - 1) / nthreads;
        for (int i = 0; i < nthreads; i++) {
            int y0 = i * rows_per;
            int y1 = y0 + rows_per;
            if (y1 > chan_h) y1 = chan_h;
            ct[i].y_start = y0;
            ct[i].y_end = y1;
            ct[i].ch_w = chan_w;
            ct[i].ch_h = chan_h;
            ct[i].log_max = log_max;
            ct[i].midpoint = midpoint;
            ct[i].shift = shift;
            ct[i].is_rggb = is_rggb;
            ct[i].log_table = log_table;
            ct[i].gs_row = channels[0];
            ct[i].rg_row = channels[1];
            ct[i].bg_row = channels[2];
            ct[i].gd_row = channels[3];
            ct[i].bayer_out = (uint8_t *)bayer_out;
            ct[i].bayer_pitch_bytes = bayer_pitch_bytes;
            created[i] = (pthread_create(&cth[i], NULL, fused_color_runner, &ct[i]) == 0);
            if (!created[i]) fused_color_runner(&ct[i]);
        }
        for (int i = 0; i < nthreads; i++)
            if (created[i]) pthread_join(cth[i], NULL);
        if (dbg_timing) fprintf(stderr, "  decode color_xform: %.1f ms\n", _decode_ms() - dt_color0);
    }

    for (int ch = 0; ch < 4; ch++)
        if (channels[ch]) free(channels[ch]);

    return rc;
}

/* Public entry point — full-resolution decode. */
int gpr_decode_fused(const uint8_t *enc, size_t enc_size,
                     uint16_t *bayer_out, size_t bayer_pitch_bytes,
                     int *out_width, int *out_height)
{
    return gpr_decode_fused_impl(enc, enc_size, bayer_out, bayer_pitch_bytes,
                                 out_width, out_height, /*half_res=*/0);
}

/* Public entry point — half-resolution decode for playback. Skips the level-1
   inverse wavelet, outputs bayer at hdr.width/2 × hdr.height/2. Matches the
   pre-FUSED GPRCodec topology that fed the CNN at codec-half-res. */
int gpr_decode_fused_halfres(const uint8_t *enc, size_t enc_size,
                             uint16_t *bayer_out, size_t bayer_pitch_bytes,
                             int *out_width, int *out_height)
{
    return gpr_decode_fused_impl(enc, enc_size, bayer_out, bayer_pitch_bytes,
                                 out_width, out_height, /*half_res=*/1);
}

typedef struct {
    const uint8_t *src;
    uint32_t size;
    PIXEL *out;
    int bw, bh;
    int raw_band;
    int rc;
} FUSED_LL_PREVIEW_TASK;

static void *fused_ll_preview_decode_runner(void *arg)
{
    FUSED_LL_PREVIEW_TASK *t = (FUSED_LL_PREVIEW_TASK *)arg;
    t->rc = fused_decode_one_band(t->src, t->size, t->out,
                                  t->bw, t->bh, &t->raw_band);
    return NULL;
}

int gpr_decode_fused_ll_preview(const uint8_t *enc, size_t enc_size,
                                uint16_t *bayer_out, size_t bayer_pitch_bytes,
                                int *out_width, int *out_height)
{
    if (!enc || !bayer_out) return -1;
    if (enc_size < sizeof(FUSED_HEADER)) return -2;

    FUSED_HEADER hdr;
    memcpy(&hdr, enc, sizeof(hdr));
    if (hdr.magic != FUSED_MAGIC) return -3;
    if (hdr.version != FUSED_VERSION) return -4;
    if (hdr.multi_level || hdr.num_bands != 16) return -5;
    if (hdr.quality >= 12) return -7;

    int dec = (hdr.decimate == 2) ? 2 : 1;
    int bayer_w = (int)hdr.width / dec;
    int bayer_h = (int)hdr.height / dec;
    int ch_w = bayer_w / 2;
    int ch_h = bayer_h / 2;
    int bw = ch_w / 2;
    int bh = ch_h / 2;
    int preview_w = bw;
    int preview_h = bh;
    if (out_width) *out_width = preview_w;
    if (out_height) *out_height = preview_h;

    SetupDecoderLogCurve();

    size_t off = sizeof(FUSED_HEADER);
    if (off + 16 * sizeof(uint32_t) > enc_size) return -8;
    uint32_t band_sizes[16];
    memcpy(band_sizes, enc + off, sizeof(band_sizes));
    off += sizeof(band_sizes);

    size_t band_offsets[16];
    for (int i = 0; i < 16; i++) {
        uint32_t sz = band_sizes[i];
        if (off + sz > enc_size) return -10;
        band_offsets[i] = off;
        off += sz;
    }

    PIXEL *ll[4] = { NULL, NULL, NULL, NULL };
    size_t ll_bytes = (size_t)bw * (size_t)bh * sizeof(PIXEL);
    for (int ch = 0; ch < 4; ch++) {
        ll[ch] = (PIXEL *)malloc(ll_bytes);
        if (!ll[ch]) {
            for (int k = 0; k < ch; k++) free(ll[k]);
            return -11;
        }
    }

    int dbg_timing = 0;
    {
        const char *e = getenv("GPR_DECODE_TIMING");
        if (e && *e == '1') dbg_timing = 1;
    }
    double dt_band0 = _decode_ms();

    FUSED_LL_PREVIEW_TASK tasks[4];
    pthread_t threads[4];
    int created[4] = {0};
    int rc = 0;
    for (int ch = 0; ch < 4; ch++) {
        int band_idx = ch * 4;
        tasks[ch].src = enc + band_offsets[band_idx];
        tasks[ch].size = band_sizes[band_idx];
        tasks[ch].out = ll[ch];
        tasks[ch].bw = bw;
        tasks[ch].bh = bh;
        tasks[ch].raw_band = 0;
        tasks[ch].rc = 0;
        created[ch] = (pthread_create(&threads[ch], NULL,
                                      fused_ll_preview_decode_runner,
                                      &tasks[ch]) == 0);
        if (!created[ch]) fused_ll_preview_decode_runner(&tasks[ch]);
    }
    for (int ch = 0; ch < 4; ch++) {
        if (created[ch]) pthread_join(threads[ch], NULL);
        if (tasks[ch].rc != 0 && rc == 0) rc = tasks[ch].rc;
    }
    if (dbg_timing) fprintf(stderr, "  decode ll_preview_band_decode: %.1f ms\n",
                            _decode_ms() - dt_band0);

    if (rc == 0) {
        const QUANT *qt = get_quant_table(hdr.quality);
        int ll_dequant = qt[0] * 16;
        {
            const char *e = getenv("FUSED_LL1_DIVISOR");
            if (e && *e) {
                int v = atoi(e);
                if (v >= 1 && v <= 64) ll_dequant = qt[0] * v;
            }
        }
        double dt_ll0 = _decode_ms();
        for (int ch = 0; ch < 4; ch++) {
            if (tasks[ch].raw_band) continue;
            size_t n = (size_t)bw * (size_t)bh;
            PIXEL *p = ll[ch];
            for (size_t i = 0; i < n; i++) p[i] *= ll_dequant;
        }
        if (dbg_timing) fprintf(stderr, "  decode ll_preview_dequant: %.1f ms\n",
                                _decode_ms() - dt_ll0);
    }

    if (rc == 0) {
        double dt_color0 = _decode_ms();
        int log_max  = (1 << (int)hdr.log_bits) - 1;
        int midpoint = 1 << ((int)hdr.log_bits - 2);
        const uint16_t *log_table =
            (hdr.log_bits <= 14) ? DecoderLogCurve14 : DecoderLogCurve16;
        int shift = 16 - (int)hdr.log_bits;
        int is_rggb = (int)hdr.is_rggb;

        FUSED_PREVIEW4X_TASK ct[4];
        pthread_t cth[4];
        int ccr[4] = {0};
        int preview_blocks_h = bh / 2;
        for (int i = 0; i < 4; i++) {
            ct[i].block_y_start = (preview_blocks_h * i) / 4;
            ct[i].block_y_end = (preview_blocks_h * (i + 1)) / 4;
            ct[i].ch_w = bw;
            ct[i].ch_h = bh;
            ct[i].log_max = log_max;
            ct[i].midpoint = midpoint;
            ct[i].shift = shift;
            ct[i].is_rggb = is_rggb;
            ct[i].log_table = log_table;
            ct[i].gs_row = ll[0];
            ct[i].rg_row = ll[1];
            ct[i].bg_row = ll[2];
            ct[i].gd_row = ll[3];
            ct[i].bayer_out = (uint8_t *)bayer_out;
            ct[i].bayer_pitch_bytes = bayer_pitch_bytes;
            ccr[i] = (pthread_create(&cth[i], NULL, fused_ll_preview2x_runner, &ct[i]) == 0);
            if (!ccr[i]) fused_ll_preview2x_runner(&ct[i]);
        }
        for (int i = 0; i < 4; i++)
            if (ccr[i]) pthread_join(cth[i], NULL);
        if (dbg_timing) fprintf(stderr, "  decode ll_preview_color_xform: %.1f ms\n",
                                _decode_ms() - dt_color0);
    }

    for (int ch = 0; ch < 4; ch++) free(ll[ch]);
    return rc;
}

/* ============================================================
   Single-level + LL decode path
   ============================================================ */
static int decode_fused_single_level_ll(const FUSED_HEADER *hdr,
                                        const uint8_t *enc, size_t enc_size,
                                        uint16_t *bayer_out, size_t bayer_pitch_bytes,
                                        int *out_width, int *out_height)
{
    if (hdr->quality >= 12) return -7;

    /* When channel-space decimation was applied, the encoded bands
       represent a (hdr.width/dec × hdr.height/dec) Bayer-equivalent image.
       The output Bayer is at those reduced dims. */
    int dec = (hdr->decimate == 2) ? 2 : 1;
    int bayer_w = (int)hdr->width  / dec;
    int bayer_h = (int)hdr->height / dec;
    int ch_w = bayer_w / 2;
    int ch_h = bayer_h / 2;
    int bw = ch_w / 2;
    int bh = ch_h / 2;
    int preview4x = 0;
    {
        const char *e = getenv("GPR_DECODE_PREVIEW4X");
        if (e && *e == '1') preview4x = 1;
    }

    if (out_width)  *out_width  = preview4x ? bayer_w / 4 : bayer_w;
    if (out_height) *out_height = preview4x ? bayer_h / 4 : bayer_h;

    SetupDecoderLogCurve();

    size_t off = sizeof(FUSED_HEADER);
    if (off + 16 * sizeof(uint32_t) > enc_size) return -8;
    uint32_t band_sizes[16];
    memcpy(band_sizes, enc + off, sizeof(band_sizes));
    off += sizeof(band_sizes);

    /* Decode all 16 bands: 4 channels × {LL, LH, HL, HH}. */
    PIXEL *bands[4][4];
    for (int ch = 0; ch < 4; ch++)
        for (int s = 0; s < 4; s++)
            bands[ch][s] = NULL;

    int rc = 0;
    int band_idx = 0;
    int dbg_timing = 0;
    {
        const char *e = getenv("GPR_DECODE_TIMING");
        if (e && *e == '1') dbg_timing = 1;
    }
    /* GPR_DECODE_LL_ONLY=1: discard HP bands at decode (treat as zero),
       producing the same output as encoding with GPR_DROP_HIGHPASS=1, but
       from a full LL+HP-encoded stream. This lets a single archived stream
       serve two consumers: a fast streaming decoder that drops HP for
       speed, and a quality decoder that consumes the HP for full fidelity
       (with optional CNN polish layered on the LL-only output). */
    int ll_only_decode = 0;
    {
        const char *e = getenv("GPR_DECODE_LL_ONLY");
        if (e && *e == '1') ll_only_decode = 1;
    }
    double dt0 = _decode_ms();
    /* Pre-allocate buffers + compute per-channel offsets, then dispatch
       4 threads (one per channel × 4 bands each). When ll_only_decode is
       set, we still allocate HP band buffers but zero them and skip the
       rANS decode for the HP slots (their offsets are still consumed
       so the byte cursor walks past them). */
    /* Use the TLS arena instead of per-frame malloc. ~5-10 ms saved at
       50 MP, plus better cache locality across frames. */
    size_t band_bytes = (size_t)bw * bh * sizeof(PIXEL);
    size_t chan_bytes = (size_t)ch_w * ch_h * sizeof(PIXEL);
    if (arena_ensure(band_bytes, chan_bytes) != 0) {
        rc = -11;
    }
    FUSED_BAND_TASK bt[4];
    size_t band_off = off;
    for (int ch = 0; ch < 4 && rc == 0; ch++) {
        bt[ch].enc_start = enc + band_off;
        bt[ch].band_sizes = &band_sizes[ch * 4];
        bt[ch].bw = bw;
        bt[ch].bh = bh;
        bt[ch].raw_band_mask = 0;
        for (int s = 0; s < 4; s++) {
            bands[ch][s] = fd_arena.band[ch][s];   /* reused from TLS arena */
            bt[ch].out[s] = bands[ch][s];
            uint32_t sz = band_sizes[ch * 4 + s];
            if (band_off + sz > enc_size) { rc = -10; break; }
            band_off += sz;
            /* When ll_only_decode is set, zero the HP band buffers and
               trigger the existing "sz < 64 → memset" fast path in the
               band runner by zeroing the size locally. We do this AFTER
               offset accumulation so the byte cursor walks the real
               sizes correctly. */
        }
    }
    /* If LL-only decode requested, point the runner at a zeroed-size
       table per channel so HP bands are memset to zero, not rANS-decoded. */
    static uint32_t zero_sizes[4][4];
    if (ll_only_decode && rc == 0) {
        for (int ch = 0; ch < 4; ch++) {
            zero_sizes[ch][0] = band_sizes[ch * 4 + 0];  /* keep LL */
            zero_sizes[ch][1] = 0;   /* triggers memset fast path */
            zero_sizes[ch][2] = 0;
            zero_sizes[ch][3] = 0;
            bt[ch].band_sizes = zero_sizes[ch];
        }
    }
    if (rc == 0) {
        pthread_t bth[4]; int bcr[4] = {0};
        for (int ch = 0; ch < 4; ch++) {
            bcr[ch] = (pthread_create(&bth[ch], NULL,
                       fused_band_decode_runner, &bt[ch]) == 0);
            if (!bcr[ch]) fused_band_decode_runner(&bt[ch]);
        }
        for (int ch = 0; ch < 4; ch++) {
            if (bcr[ch]) pthread_join(bth[ch], NULL);
        }
    }
    off = band_off;
    (void)band_idx;

    double dt_band = _decode_ms() - dt0;
    if (dbg_timing) fprintf(stderr, "  decode band_decode: %.1f ms\n", dt_band);
    double dt_wav0 = _decode_ms();

    PIXEL *channels[4] = { NULL, NULL, NULL, NULL };

    if (rc == 0) {
        const QUANT *qt = get_quant_table(hdr->quality);
        QUANT q_l1[4] = { -qt[0], -qt[1], -qt[2], -qt[3] };

        /* HP-synth deblock polish (C port of tools/hp_synth_polish.py).
           Activated by GPR_DECODE_HPSYNTH=<scale> env var (default 0 = off,
           recommended 0.3-0.7). When HP bands are zero (LL-only stream or
           DECODE_LL_ONLY), synthesizes plausible HP coefficients from LL
           gradients before the inverse wavelet runs. Per-band noise scaled
           by a Gaussian bandpass on LL gradient (peak at the 45th
           percentile, σ=15 pct) — peaks at moderate edges, rolls off at
           flat regions AND at hard edges (avoids chroma speckle on
           silhouettes). Same seed for all 4 Bayer planes → correlated
           noise → no chroma fringing.

           CRITICAL: runs BEFORE the ll_dequant multiply below, so the LL
           coefficients are in the encoder's quantized range (small) — same
           scale the inverse wavelet expects HP bands to be in. Running it
           AFTER would inject noise at 16× too large a scale, producing
           int16 wrap-around speckles. */
        double hpsynth_scale = 0.0;
        {
            const char *e = getenv("GPR_DECODE_HPSYNTH");
            if (e && *e) hpsynth_scale = atof(e);
        }
        if (hpsynth_scale > 0.0) {
            double dt_hp = _decode_ms();
            /* Per-channel parallel: launch 4 pthread tasks. Each task
               processes one channel's LL→LH/HL/HH synthesis independently
               (no shared writes). On Pi 5's 4 cores this gives ~4× wall
               speedup over the serial loop. */
            HP_SYNTH_TASK tasks[4];
            for (int ch = 0; ch < 4; ch++) {
                tasks[ch].do_synth = 0;
                if (!bands[ch][0]) continue;
                /* Only synthesize when HP bands are missing (LL-only mode).
                   When HP is real, leave them alone — refining real HP is
                   the CNN's job, not this deterministic synth. */
                PIXEL *lh = bands[ch][1];
                PIXEL *hl = bands[ch][2];
                PIXEL *hh = bands[ch][3];
                if (!lh || !hl || !hh) continue;
                size_t n = (size_t)bw * bh;
                size_t nonzero = 0;
                for (size_t i = 0; i < n && nonzero < 64; i++) {
                    if (lh[i] || hl[i] || hh[i]) nonzero++;
                }
                if (nonzero >= 64) continue;
                tasks[ch].LL = bands[ch][0];
                tasks[ch].LH = lh; tasks[ch].HL = hl; tasks[ch].HH = hh;
                tasks[ch].bw = bw; tasks[ch].bh = bh;
                tasks[ch].scale = hpsynth_scale;
                tasks[ch].dq_lh = (double)qt[1];
                tasks[ch].dq_hl = (double)qt[2];
                tasks[ch].dq_hh = (double)qt[3];
                tasks[ch].seed = 0xDEADBEEFu;  /* same across channels →
                                                  correlated noise → no
                                                  chroma fringing */
                tasks[ch].do_synth = 1;
            }
            pthread_t th[4]; int cr[4] = {0};
            for (int ch = 0; ch < 4; ch++) {
                cr[ch] = (pthread_create(&th[ch], NULL, hp_synth_runner, &tasks[ch]) == 0);
                if (!cr[ch]) hp_synth_runner(&tasks[ch]);
            }
            for (int ch = 0; ch < 4; ch++)
                if (cr[ch]) pthread_join(th[ch], NULL);
            if (dbg_timing)
                fprintf(stderr, "  decode hp_synth: %.1f ms\n", _decode_ms() - dt_hp);
        }

        /* The encoder normally divides single-level LL by 16× the natural quant to
           keep magnitudes under the rANS class-15 ceiling (matches the
           multi-level LL3 trick). Pre-multiply LL bands to undo that.
           Raw-LL sideband streams are self-describing and already carry the
           dequantized LL coefficients, so those bands must not be scaled.
           NEON 4-wide; ~4× faster than scalar on Pi 5 (4 cores × NEON pipes
           give plenty of throughput; on M1 the scalar loop is bound by
           memory bandwidth so the speedup is smaller). */
        int ll_extra = 16;
        {
            const char *e = getenv("FUSED_LL1_DIVISOR");
            if (e && *e) {
                int v = atoi(e);
                if (v >= 1 && v <= 64) ll_extra = v;
            }
        }
        const int ll_dequant = qt[0] * ll_extra;
        for (int ch = 0; ch < 4; ch++) {
            if (bt[ch].raw_band_mask & 1) continue;
            PIXEL *p = bands[ch][0];
            if (!p) continue;
            size_t n = (size_t)bw * bh;
            size_t i = 0;
#if defined(__ARM_NEON)
            int32x4_t vq = vdupq_n_s32(ll_dequant);
            size_t n_m8 = (n / 8) * 8;
            for (; i < n_m8; i += 8) {
                int32x4_t v0 = vld1q_s32(&p[i]);
                int32x4_t v1 = vld1q_s32(&p[i + 4]);
                vst1q_s32(&p[i],     vmulq_s32(v0, vq));
                vst1q_s32(&p[i + 4], vmulq_s32(v1, vq));
            }
#endif
            for (; i < n; i++) p[i] *= ll_dequant;
        }

        const char *dbg = getenv("FUSED_DECODE_DEBUG");
        if (dbg && *dbg == '1') {
            for (int ch = 0; ch < 4; ch++) {
                for (int s = 0; s < 4; s++) {
                    PIXEL *p = bands[ch][s];
                    long mn = 1<<30, mx = -(1<<30); long long sum = 0;
                    long n = (long)bw * bh;
                    for (long i = 0; i < n; i++) {
                        if (p[i] < mn) mn = p[i];
                        if (p[i] > mx) mx = p[i];
                        sum += p[i];
                    }
                    fprintf(stderr, "  ch%d band%d (%s): min=%ld max=%ld mean=%.1f\n",
                            ch, s, s==0?"LL":s==1?"LH":s==2?"HL":"HH",
                            mn, mx, (double)sum/n);
                }
            }
        }

        /* Fused row-strip pipeline (band → wavelet → color in one pass per
           strip) is the default for the LL-only-fast path. Set
           GPR_DECODE_FUSED_STREAM=0 to opt out and use the legacy
           per-channel whole-buffer pipeline (kept for A/B verification).
           Default ON measured Pi 5: wavelet(45)+color(31)=76ms → ~max(stage)
           ≈ 45ms wall time (per fused_stream_decode.c file header note),
           and now with NEON'd inverse wavelet in fused_stream the gap is
           even tighter. M1 measured byte-identical. */
        int use_fused_stream = preview4x ? 0 : 1;
        {
            const char *e = getenv("GPR_DECODE_FUSED_STREAM");
            if (e && *e == '0') use_fused_stream = 0;
        }
        if (use_fused_stream) {
            int log_max  = (1 << (int)hdr->log_bits) - 1;
            int midpoint = 1 << ((int)hdr->log_bits - 1);
            const uint16_t *log_table =
                (hdr->log_bits <= 14) ? DecoderLogCurve14 : DecoderLogCurve16;
            int output_bit_depth = (int)hdr->log_bits;
            int shift = 16 - output_bit_depth;
            int is_rggb = (int)hdr->is_rggb;

            /* Detect HP-zero: when all HP band sizes from the encoded
               stream are below the rANS "small-band memset" threshold
               (<64 bytes), or when caller set GPR_DECODE_LL_ONLY=1, the
               LH/HL/HH bands are guaranteed zero post-band_decode. The
               LL-only-fast encoder writes 0-byte HP sizes, so this is the
               common case for the streaming playback path. Detecting it
               lets fused_stream skip per-row HP dequant entirely. */
            int hp_zero = 1;
            for (int ch = 0; ch < 4 && hp_zero; ch++) {
                for (int s = 1; s <= 3; s++) {
                    if (band_sizes[ch * 4 + s] >= 64) { hp_zero = 0; break; }
                }
            }
            /* When HP-synth was actually applied (scale > 0), HP is NOT
               zero — the synth wrote into the band buffers. */
            if (hpsynth_scale > 0.0) hp_zero = 0;
            double dt_fs0 = _decode_ms();
            int frc = gpr_decode_fused_stream(bands, bw, bh, ch_w, ch_h,
                                               qt,
                                               log_max, midpoint, shift, is_rggb,
                                               log_table,
                                               (uint8_t *)bayer_out, bayer_pitch_bytes,
                                               (int)hdr->prescale,
                                               hp_zero);
            if (frc != 0) rc = -30;
            if (dbg_timing) fprintf(stderr, "  decode fused_stream: %.1f ms\n",
                                    _decode_ms() - dt_fs0);
            /* Bypass legacy wavelet/color stages — fused stream already
               wrote the Bayer output. */
            return rc;
        }

        /* Parallel inverse wavelet: 4 channels, 4 threads on Pi 5's 4 cores.
           Each channel's inverse is independent — own band buffers, own
           output channel. InvertSpatialQuantDescale16s uses the allocator
           which wraps libc malloc (thread-safe). Expected ~3-4× wall
           reduction on this stage. */
        FUSED_INV_TASK tasks[4];
        pthread_t threads[4];
        int created[4] = {0};
        int single_descale = (int)hdr->prescale;
        {
            const char *e = getenv("FUSED_SINGLE_INVERSE_DESCALE");
            if (e && *e) {
                int v = atoi(e);
                if (v >= 0 && v <= 4) single_descale = v;
            }
        }
        for (int ch = 0; ch < 4 && rc == 0; ch++) {
            channels[ch] = fd_arena.chan[ch];   /* reused from TLS arena */
            if (!channels[ch]) { rc = -24; break; }
            tasks[ch].ll = bands[ch][0];
            tasks[ch].lh = bands[ch][1];
            tasks[ch].hl = bands[ch][2];
            tasks[ch].hh = bands[ch][3];
            tasks[ch].out_channel = channels[ch];
            tasks[ch].bw = bw;
            tasks[ch].bh = bh;
            tasks[ch].ch_w = ch_w;
            tasks[ch].ch_h = ch_h;
            tasks[ch].descale = single_descale;
            tasks[ch].q = q_l1;
            tasks[ch].err = CODEC_ERROR_OKAY;
        }
        if (rc == 0) {
            for (int ch = 0; ch < 4; ch++) {
                created[ch] = (pthread_create(&threads[ch], NULL,
                    fused_inv_wavelet_runner, &tasks[ch]) == 0);
                if (!created[ch]) fused_inv_wavelet_runner(&tasks[ch]);
            }
            for (int ch = 0; ch < 4; ch++) {
                if (created[ch]) pthread_join(threads[ch], NULL);
                if (tasks[ch].err != CODEC_ERROR_OKAY) rc = -25;
            }
        }
    }

    double dt_wav = _decode_ms() - dt_wav0;
    if (dbg_timing) fprintf(stderr, "  decode wavelet inv: %.1f ms\n", dt_wav);
    double dt_color0_sl = _decode_ms();

    /* Bands stay in the TLS arena for reuse; no per-frame free. */

    if (preview4x && rc == 0) {
        double dt_preview0 = _decode_ms();
        int log_max  = (1 << (int)hdr->log_bits) - 1;
        int midpoint = 1 << ((int)hdr->log_bits - 1);
        const uint16_t *log_table =
            (hdr->log_bits <= 14) ? DecoderLogCurve14 : DecoderLogCurve16;
        int output_bit_depth = (int)hdr->log_bits;
        int shift = 16 - output_bit_depth;
        int is_rggb = (int)hdr->is_rggb;
        int preview_blocks_h = (ch_h / 4);

        FUSED_PREVIEW4X_TASK pt[4];
        pthread_t pth[4];
        int pcr[4] = {0};
        for (int i = 0; i < 4; i++) {
            pt[i].block_y_start = (preview_blocks_h * i) / 4;
            pt[i].block_y_end = (preview_blocks_h * (i + 1)) / 4;
            pt[i].ch_w = ch_w;
            pt[i].ch_h = ch_h;
            pt[i].log_max = log_max;
            pt[i].midpoint = midpoint;
            pt[i].shift = shift;
            pt[i].is_rggb = is_rggb;
            pt[i].log_table = log_table;
            pt[i].gs_row = channels[0];
            pt[i].rg_row = channels[1];
            pt[i].bg_row = channels[2];
            pt[i].gd_row = channels[3];
            pt[i].bayer_out = (uint8_t *)bayer_out;
            pt[i].bayer_pitch_bytes = bayer_pitch_bytes;
            pcr[i] = (pthread_create(&pth[i], NULL, fused_preview4x_runner, &pt[i]) == 0);
            if (!pcr[i]) fused_preview4x_runner(&pt[i]);
        }
        for (int i = 0; i < 4; i++)
            if (pcr[i]) pthread_join(pth[i], NULL);
        if (dbg_timing) fprintf(stderr, "  decode preview4x: %.1f ms\n",
                                _decode_ms() - dt_preview0);
        return rc;
    }

    {
        const char *_dump_channels = getenv("GPR_DUMP_DECODE_CHANNELS");
        if (_dump_channels && *_dump_channels) {
            for (int ch = 0; ch < 4; ch++) {
                if (!channels[ch]) continue;
                char path[1024];
                snprintf(path, sizeof(path), "%s/ch%d_w%d_h%d.s32",
                         _dump_channels, ch, ch_w, ch_h);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(channels[ch], sizeof(PIXEL),
                           (size_t)ch_w * (size_t)ch_h, f);
                    fclose(f);
                }
            }
        }
    }

    /* Parallel color transform: split channel rows across 4 threads. Each
       row is independent — output Bayer rows for row y are at 2y, 2y+1. */
    if (rc == 0) {
        int log_max  = (1 << (int)hdr->log_bits) - 1;
        int midpoint = 1 << ((int)hdr->log_bits - 1);
        const uint16_t *log_table =
            (hdr->log_bits <= 14) ? DecoderLogCurve14 : DecoderLogCurve16;
        int output_bit_depth = (int)hdr->log_bits;
        int shift = 16 - output_bit_depth;
        int is_rggb = (int)hdr->is_rggb;

        FUSED_COLOR_TASK ct[4];
        pthread_t cth[4]; int ccr[4] = {0};
        for (int i = 0; i < 4; i++) {
            ct[i].y_start = (ch_h * i) / 4;
            ct[i].y_end   = (ch_h * (i + 1)) / 4;
            ct[i].ch_w = ch_w;
            ct[i].ch_h = ch_h;
            ct[i].log_max = log_max;
            ct[i].midpoint = midpoint;
            ct[i].shift = shift;
            ct[i].is_rggb = is_rggb;
            ct[i].log_table = log_table;
            ct[i].gs_row = channels[0];
            ct[i].rg_row = channels[1];
            ct[i].bg_row = channels[2];
            ct[i].gd_row = channels[3];
            ct[i].bayer_out = (uint8_t *)bayer_out;
            ct[i].bayer_pitch_bytes = bayer_pitch_bytes;
            ccr[i] = (pthread_create(&cth[i], NULL,
                                     fused_color_runner, &ct[i]) == 0);
            if (!ccr[i]) fused_color_runner(&ct[i]);
        }
        for (int i = 0; i < 4; i++) {
            if (ccr[i]) pthread_join(cth[i], NULL);
        }
    }

    /* Channels stay in the TLS arena for reuse; no per-frame free. */

    if (dbg_timing) fprintf(stderr, "  decode color_xform: %.1f ms\n",
                            _decode_ms() - dt_color0_sl);
    return rc;
}
