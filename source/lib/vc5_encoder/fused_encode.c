/*! @file fused_encode.c
 *
 *  @brief Fused encoder: Bayer→Wavelet→Quantize→FreqCount in one streaming pass.
 *
 *  Pass 1: For each row pair of the input image:
 *    1. Load Bayer pixels, apply log curve → GS, RG, BG, GD
 *    2. Horizontal wavelet filter into 6-row circular buffer
 *    3. When 6 rows available: vertical filter + quantize → band output
 *    4. Inline: count symbol frequencies for ANS encoding
 *
 *  Pass 2: For each band:
 *    1. Build rANS table from counted frequencies
 *    2. Re-scan band data for tokens, rANS encode
 *
 *  This eliminates 4 × 11MB intermediate component arrays and fuses
 *  the 144ms tokenization pass into the wavelet output path.
 *
 *  (C) Copyright 2018 GoPro Inc. Licensed under Apache-2.0 or MIT.
 */

/* POSIX feature test must precede any system header so clock_gettime
   and CLOCK_MONOTONIC are exposed on Linux (Pi). */
#if !defined(__APPLE__)
#  ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 199309L
#  endif
#  ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1   /* for pthread_setaffinity_np on glibc */
#  endif
#endif

#include "headers.h"
#include "fused_encode.h"
#include "ans_joint.h"
#include "denoise.h"
#include <pthread.h>
#include <limits.h>
#include <unistd.h>  /* for sysconf */
#if defined(__linux__)
#include <sched.h>
#endif

/* FUSED_PIN / GPR_PIN_AFFINITY: pin channel thread N to core N (modulo nproc).
   Set at runtime via FUSED_PIN=1 or GPR_PIN_AFFINITY=1. Default OFF.

   Pi 5 measurement (Aug 2026): the kernel scheduler exhibits bimodal
   placement of the 4 channel threads — sometimes lands them on separate
   cores (median 46 ms), sometimes 2 share a core (median 64 ms). Pinning
   each channel thread to its own core eliminates the bimodal — gates p90
   down. */
#if defined(__linux__)
static int _fused_pin_enabled(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    const char *e = getenv("FUSED_PIN");
    if (!e) e = getenv("GPR_PIN_AFFINITY");
    cached = (e && *e == '1') ? 1 : 0;
    return cached;
}
static int _fused_pin_p2_enabled(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    const char *e = getenv("FUSED_PIN_P2");
    cached = (e && *e == '1') ? 1 : 0;
    return cached;
}
static void _fused_pin_self(int core) {
    if (!_fused_pin_enabled()) return;
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 4;
    int target = core % n;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(target, &mask);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
}
#else
static void _fused_pin_self(int core) { (void)core; }
static int _fused_pin_p2_enabled(void) { return 0; }
#endif

/* Per-frame timing prints. Comment out for clean micro-benchmarks. */
/* #define FUSED_TIMING */
/* #define FUSED_TIMING_DETAIL */

#if defined(FUSED_TIMING) || defined(FUSED_TIMING_DETAIL)
#if defined(__APPLE__)
#include <mach/mach_time.h>
static double _fused_ms(void) {
    static double s = 0;
    if (!s) { mach_timebase_info_data_t i; mach_timebase_info(&i); s = (double)i.numer/i.denom/1e6; }
    return mach_absolute_time() * s;
}
#else
#include <time.h>
static double _fused_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}
#endif
#endif

#if ENABLED(NEON)
#include <arm_neon.h>
#endif

/* ================================================================
   FUSED_LOG_POLYNOMIAL — NEON polynomial log curve.

   Re-introduced at bb37c76 (originally landed in 8fe3638 and promoted
   to default ON in 58577fe; got dropped during fused-LP T13/T14/T15/T16
   work). The CMake option was still defining the macro but no source
   was reading it, so the LUT scalar-gather path was always running.

   Historical Pi 5 (Cortex-A76) sweeps showed the polynomial path saving time
   on an older unpack shape. Current half-res Labs capture measurements rule it
   out for the active T13/T14/T15/T16 path: on 2026-06-15, FUSED_LOG_POLYNOMIAL
   slowed normal write-all output and severely regressed the highpass lower-
   bound diagnostic. Keep the CMake default OFF unless a new target-specific
   receipt proves otherwise.

   Polynomial: log2(m) for m in [1, 2) via series in u = (m-1)/(m+1),
   |u| <= 1/3, with 6 terms. Validated to <=1 LSB max error vs LUT across
   the 14- and 16-bit input ranges. We have ~32 LSB of headroom in the
   LL quantizer regardless.

   Default OFF via CMake; gated by FUSED_LOG_POLYNOMIAL. With the macro
   off, the LUT scalar-gather paths below remain unchanged byte-for-byte.
   ================================================================ */
#if defined(FUSED_LOG_POLYNOMIAL) && ENABLED(NEON)
#include <math.h>  /* log2f */

/* log2(m) for m in [1, 2) via series in u = (m-1)/(m+1), |u| <= 1/3. */
static inline float32x4_t fused_vlog2q_mant(float32x4_t m) {
    float32x4_t mm1 = vsubq_f32(m, vdupq_n_f32(1.0f));
    float32x4_t mp1 = vaddq_f32(m, vdupq_n_f32(1.0f));
    /* 1 / (m+1) via 2 Newton-Raphson refinements on vrecpeq estimate */
    float32x4_t inv = vrecpeq_f32(mp1);
    inv = vmulq_f32(inv, vrecpsq_f32(mp1, inv));
    inv = vmulq_f32(inv, vrecpsq_f32(mp1, inv));
    float32x4_t u  = vmulq_f32(mm1, inv);
    float32x4_t u2 = vmulq_f32(u, u);
    /* 1 + u^2/3 + u^4/5 + u^6/7 + u^8/9 + u^10/11 */
    float32x4_t p = vfmaq_f32(vdupq_n_f32(1.0f / 9.0f),  u2, vdupq_n_f32(1.0f / 11.0f));
    p = vfmaq_f32(vdupq_n_f32(1.0f / 7.0f),  u2, p);
    p = vfmaq_f32(vdupq_n_f32(1.0f / 5.0f),  u2, p);
    p = vfmaq_f32(vdupq_n_f32(1.0f / 3.0f),  u2, p);
    p = vfmaq_f32(vdupq_n_f32(1.0f),         u2, p);
    return vmulq_n_f32(vmulq_f32(u, p), 2.0f / 0.6931471805599453f);
}

static inline float32x4_t fused_vlog2q_approx(float32x4_t x) {
    int32x4_t bits  = vreinterpretq_s32_f32(x);
    int32x4_t expo  = vsubq_s32(vshrq_n_s32(bits, 23), vdupq_n_s32(127));
    int32x4_t mbits = vorrq_s32(vandq_s32(bits, vdupq_n_s32(0x007FFFFF)),
                                 vdupq_n_s32(0x3F800000));
    float32x4_t m = vreinterpretq_f32_s32(mbits);
    return vaddq_f32(vcvtq_f32_s32(expo), fused_vlog2q_mant(m));
}

/* Bayer-pixel log curve, 4-wide: y = max_v * log10(x/max_v * 112 + 1) / log10(113)
   Equivalent to LUT[x] with <=1 LSB error. Input must already be clamped to max_v.
   Output is clamped to [0, max_v] internally. */
static inline int32x4_t fused_log_curve_neon4(uint16x4_t x_u16, int max_v) {
    float scale = 112.0f / (float)max_v;
    float K     = (float)max_v / log2f(113.0f);
    float32x4_t fx   = vcvtq_f32_u32(vmovl_u16(x_u16));
    float32x4_t norm = vfmaq_n_f32(vdupq_n_f32(1.0f), fx, scale);
    float32x4_t ln   = fused_vlog2q_approx(norm);
    float32x4_t y    = vmulq_n_f32(ln, K);
    int32x4_t   yi   = vcvtq_s32_f32(y);  /* truncate, like the LUT */
    int32x4_t   lo   = vmaxq_s32(yi, vdupq_n_s32(0));
    return vminq_s32(lo, vdupq_n_s32(max_v));
}

/* 8-wide u16 -> u16 wrapper. Replaces a 16-element scalar-gather sequence
   (vst1q_u16(idx, vMin); for k... arr[k] = log_tbl[idx[k]]) plus the
   subsequent vld1q_u16(arr) reload. Produces u16 in [0, max_v]. */
static inline uint16x8_t fused_log_curve_neon8_u16(uint16x8_t x_u16, int max_v) {
    int32x4_t lo = fused_log_curve_neon4(vget_low_u16(x_u16),  max_v);
    int32x4_t hi = fused_log_curve_neon4(vget_high_u16(x_u16), max_v);
    /* Narrow s32 (already clamped to [0, max_v]) to u16 via vqmovun_s32. */
    return vcombine_u16(vqmovun_s32(lo), vqmovun_s32(hi));
}
#endif /* FUSED_LOG_POLYNOMIAL && NEON */

/* ================================================================
   Constants and quant tables
   ================================================================ */

#define FUSED_MAX_CHANNELS 4
#define FUSED_MAX_WAVELETS 3
#define FUSED_MAX_BANDS    4
#define FUSED_ROW_BUFS     6

/* Cache-line size: 64 B on Cortex-A76 (Pi 5), Apple M1 firestorm/icestorm,
   and most modern ARMv8 / x86_64. 128 B exists on some Apple cores' L2
   prefetch granule but L1 line is 64 B everywhere we care about. */
#define CACHE_LINE_SIZE 64

/* aligned_alloc()/posix_memalign() wrapper. Always returns either NULL or
   a pointer aligned to CACHE_LINE_SIZE. Used for hot-path scratch
   (per-row band buffers, ring slot buffers, per-channel row workspaces)
   where the C plain malloc's 16-byte alignment can split NEON loads
   across cache lines. Modeled on FFTW's fftw_malloc(). */
static inline void *fused_aligned_alloc(size_t bytes) {
    if (bytes == 0) return NULL;
    /* posix_memalign requires size be any multiple, alignment ≥ sizeof(void*)
       and a power of two. aligned_alloc requires size be a multiple of
       alignment. Round up the size for portability. */
    size_t rounded = (bytes + CACHE_LINE_SIZE - 1) & ~((size_t)(CACHE_LINE_SIZE - 1));
    void *p = NULL;
#if defined(_ISOC11_SOURCE) || (__STDC_VERSION__ >= 201112L)
    p = aligned_alloc(CACHE_LINE_SIZE, rounded);
#else
    if (posix_memalign(&p, CACHE_LINE_SIZE, rounded) != 0) p = NULL;
#endif
    return p;
}

/* As above but zero-initialised, replacement for calloc() at hot-path sites. */
static inline void *fused_aligned_calloc(size_t nelt, size_t eltsz) {
    size_t bytes = nelt * eltsz;
    void *p = fused_aligned_alloc(bytes);
    if (p) memset(p, 0, bytes);
    return p;
}

/* Quality presets: quant divisors per subband [LL, LH, HL, HH for each level]
   Slot mapping:
     0     = LL3 (always 1 — never quantize the deepest lowpass)
     1,2,3 = LH3, HL3, HH3 (level-3 highpass) — also LH1/HL1/HH1 in single-ll mode
     4,5,6 = LH2, HL2, HH2 (level-2 highpass)
     7,8,9 = LH1, HL1, HH1 (level-1 highpass) in multi-level mode
   See docs/quant_calibration_findings.md for the per-subband sweep data. */
static const QUANT quality_tables[12][10] = {
    {1, 24, 24, 12, 64, 64, 48, 512, 512, 768},  /* 0: Low */
    {1, 24, 24, 12, 48, 48, 32, 256, 256, 384},  /* 1: Medium */
    {1, 24, 24, 12, 32, 32, 24, 128, 128, 192},  /* 2: High */
    {1, 24, 24, 12, 24, 24, 12,  96,  96, 144},  /* 3: Filmscan-1 (default) */
    {1, 24, 24, 12, 24, 24, 12,  64,  64,  96},  /* 4: Filmscan-X */
    {1, 24, 24, 12, 24, 24, 12,  32,  32,  48},  /* 5: Filmscan-2 (empirical peak on real content) */
    {1, 12, 12,  6, 12, 12,  6,  16,  16,  24},  /* 6: Filmscan-3 */
    {1,  6,  6,  4, 12, 12,  6,  16,  16,  24},  /* 7: Filmscan-4 */
    {1,  4,  4,  2, 10, 10,  6,  16,  16,  24},  /* 8: Filmscan-5 */
    {1,  4,  4,  2, 10, 10,  6,  16,  16,  24},  /* 9: Reserved (mirrors FS5) */
    {1,  4,  4,  2, 10, 10,  6,  16,  16,  24},  /* 10: Reserved (mirrors FS5) */
    /* 11: CNN-aware. Turn it up to 11.
       Cranks the highpass slots across single-level, ML-2, and ML-3:
         SL reads slots 1/2/3 (LH/HL/HH)    — {24,24,12} → {48,48,48}
         ML reads slots 4/5/6 (LH2/HL2/HH2) — {24,24,12} → {48,48,24}
                  slots 7/8/9 (LH1/HL1/HH1) — {96,96,144} → {192,192,576}
       ML-2 L2-highpass cranking (slots 4/5/6) gives 13% savings on top
       of L1-highpass cranking — measured on Z8 50MP, ~zero bayer-PSNR
       loss. Slot 0 (LL) untouched; LL2 has its own ×16 internal
       divisor and the multiplied-out quant must stay within rANS class
       range. */
    {1, 48, 48, 48, 48, 48, 24, 192, 192, 576},  /* 11: CNN-aware */
};

/* Apply GPR_QUANT_OVERRIDE env var to a quant table copy (task #158).
   Format: "slot:value,slot:value,..." e.g. "7:144,8:144,9:216" sets slots
   7/8/9 (LH1/HL1/HH1, the level-1 highpass). Used by the per-subband quant
   calibration sweep to explore rate-distortion tradeoffs that the 9 fixed
   presets can't reach. The decoder reads the SAME env var so encode and
   decode must run in the same environment; mismatches produce garbage.
   No-op when env var is unset (the normal path). */
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

/* ================================================================
   Quantization helpers (same math as forward.c QuantizeValue)
   ================================================================ */

static inline int32_t get_multiplier(int divisor) {
    return (divisor > 0) ? ((1 << 16) / divisor) : 0;
}

static inline int32_t get_midpoint(int divisor) {
    return (divisor > 1) ? (divisor >> 1) - 1 : 0;
}

static inline int32_t quantize_scalar(int32_t value, int32_t midpoint, int32_t multiplier) {
    int32_t mag = (value < 0) ? -value : value;
    int32_t q = (int32_t)(((int64_t)(mag + midpoint) * multiplier) >> 16);
    return (value < 0) ? -q : q;
}

static inline int fused_reference_horizontal_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    const char *e = getenv("FUSED_REFERENCE_HORIZONTAL");
    cached = (e && *e == '0') ? 0 : 1;
    return cached;
}

static inline PIXEL fused_hpf_ref_middle(const PIXEL *input, int prescale_rounding, int prescale)
{
    int32_t sum;
    if (prescale == 0) {
        sum = -input[0] - input[1] + (input[2] << 3) - (input[3] << 3) + input[4] + input[5];
    } else {
        sum = 0;
        sum -=  (input[0] + prescale_rounding) >> prescale;
        sum -=  (input[1] + prescale_rounding) >> prescale;
        sum += ((input[2] + prescale_rounding) >> prescale) << 3;
        sum -= ((input[3] + prescale_rounding) >> prescale) << 3;
        sum +=  (input[4] + prescale_rounding) >> prescale;
        sum +=  (input[5] + prescale_rounding) >> prescale;
    }
    sum += 4;
    sum = DivideByShift(sum, 3);
    return ClampPixel(sum);
}

static inline PIXEL fused_hpf_ref_coeff(const PIXEL *input, const PIXEL *coeffs,
                                        int prescale_rounding, int prescale)
{
    int32_t sum = 0;
    for (int i = 0; i < 6; i++) {
        int32_t v = (prescale == 0) ? input[i] : ((input[i] + prescale_rounding) >> prescale);
        sum += coeffs[i] * v;
    }
    sum += 4;
    sum = DivideByShift(sum, 3);
    return ClampPixel(sum);
}

static void horizontal_filter_reference(const PIXEL *input, PIXEL *lowpass, PIXEL *highpass,
                                        int width, int prescale)
{
    int column = 2;
    int prescale_rounding = (1 << prescale) - 1;
    const int last_input_column = ((width % 2) == 0) ? width - 2 : width - 1;
    PIXEL left_coeffs[6] = { 5, -11, 4, 4, -1, -1 };
    PIXEL right_coeffs[6] = { 1, 1, -4, -4, 11, -5 };

    if (prescale != 0 && prescale != 2 && prescale != 3) {
        prescale = (prescale > 3) ? 3 : ((prescale > 0) ? 2 : 0);
        prescale_rounding = (1 << prescale) - 1;
    }

    lowpass[0] = (input[0] + input[1] + prescale_rounding) >> prescale;
    highpass[0] = fused_hpf_ref_coeff(input, left_coeffs, prescale_rounding, prescale);

#if ENABLED(NEON)
    if (prescale == 2) {
        const int32x4_t vround_lp = vdupq_n_s32(3);
        const int32x4_t vround_hp = vdupq_n_s32(4);
        const int32x4_t vprescale = vdupq_n_s32(3);
        for (; column + 6 < last_input_column && column + 9 < width; column += 8) {
            const int idx = column;
            int32x4_t raw0 = vld1q_s32(&input[idx - 2]);  /* c-2 .. c+1 */
            int32x4_t raw1 = vld1q_s32(&input[idx + 2]);  /* c+2 .. c+5 */
            int32x4_t raw2 = vld1q_s32(&input[idx + 6]);  /* c+6 .. c+9 */

            int32x4_t cur_pair = vextq_s32(raw0, raw1, 2); /* c .. c+3 */
            int32x4_t nxt_pair = vextq_s32(raw1, raw2, 2); /* c+4 .. c+7 */
            int32x4x2_t raw_cur = vuzpq_s32(cur_pair, nxt_pair);
            int32x4_t lp_sum = vaddq_s32(raw_cur.val[0], raw_cur.val[1]);
            vst1q_s32(&lowpass[column / 2], vshrq_n_s32(vaddq_s32(lp_sum, vround_lp), 2));

            int32x4_t ps0 = vshrq_n_s32(vaddq_s32(raw0, vprescale), 2);
            int32x4_t ps1 = vshrq_n_s32(vaddq_s32(raw1, vprescale), 2);
            int32x4_t ps2 = vshrq_n_s32(vaddq_s32(raw2, vprescale), 2);

            int32x4x2_t prev_pairs = vuzpq_s32(ps0, ps1);
            int32x4x2_t next_pairs = vuzpq_s32(ps1, ps2);
            int32x4_t prev_sum = vaddq_s32(prev_pairs.val[0], prev_pairs.val[1]);
            int32x4_t next_sum = vaddq_s32(next_pairs.val[0], next_pairs.val[1]);

            int32x4_t ps_cur_pair = vextq_s32(ps0, ps1, 2);
            int32x4_t ps_nxt_pair = vextq_s32(ps1, ps2, 2);
            int32x4x2_t ps_cur = vuzpq_s32(ps_cur_pair, ps_nxt_pair);
            int32x4_t center_delta = vsubq_s32(ps_cur.val[0], ps_cur.val[1]);

            int32x4_t hp = vsubq_s32(next_sum, prev_sum);
            hp = vaddq_s32(hp, vshlq_n_s32(center_delta, 3));
            hp = vshrq_n_s32(vaddq_s32(hp, vround_hp), 3);
            vst1q_s32(&highpass[column / 2], hp);
        }
    }
    if (prescale == 3) {
        const int32x4_t vround_lp = vdupq_n_s32(7);
        const int32x4_t vround_hp = vdupq_n_s32(4);
        const int32x4_t vprescale = vdupq_n_s32(7);
        for (; column + 6 < last_input_column && column + 9 < width; column += 8) {
            const int idx = column;
            int32x4_t raw0 = vld1q_s32(&input[idx - 2]);  /* c-2 .. c+1 */
            int32x4_t raw1 = vld1q_s32(&input[idx + 2]);  /* c+2 .. c+5 */
            int32x4_t raw2 = vld1q_s32(&input[idx + 6]);  /* c+6 .. c+9 */

            int32x4_t cur_pair = vextq_s32(raw0, raw1, 2); /* c .. c+3 */
            int32x4_t nxt_pair = vextq_s32(raw1, raw2, 2); /* c+4 .. c+7 */
            int32x4x2_t raw_cur = vuzpq_s32(cur_pair, nxt_pair);
            int32x4_t lp_sum = vaddq_s32(raw_cur.val[0], raw_cur.val[1]);
            vst1q_s32(&lowpass[column / 2], vshrq_n_s32(vaddq_s32(lp_sum, vround_lp), 3));

            int32x4_t ps0 = vshrq_n_s32(vaddq_s32(raw0, vprescale), 3);
            int32x4_t ps1 = vshrq_n_s32(vaddq_s32(raw1, vprescale), 3);
            int32x4_t ps2 = vshrq_n_s32(vaddq_s32(raw2, vprescale), 3);

            int32x4x2_t prev_pairs = vuzpq_s32(ps0, ps1);
            int32x4x2_t next_pairs = vuzpq_s32(ps1, ps2);
            int32x4_t prev_sum = vaddq_s32(prev_pairs.val[0], prev_pairs.val[1]);
            int32x4_t next_sum = vaddq_s32(next_pairs.val[0], next_pairs.val[1]);

            int32x4_t ps_cur_pair = vextq_s32(ps0, ps1, 2);
            int32x4_t ps_nxt_pair = vextq_s32(ps1, ps2, 2);
            int32x4x2_t ps_cur = vuzpq_s32(ps_cur_pair, ps_nxt_pair);
            int32x4_t center_delta = vsubq_s32(ps_cur.val[0], ps_cur.val[1]);

            int32x4_t hp = vsubq_s32(next_sum, prev_sum);
            hp = vaddq_s32(hp, vshlq_n_s32(center_delta, 3));
            hp = vshrq_n_s32(vaddq_s32(hp, vround_hp), 3);
            vst1q_s32(&highpass[column / 2], hp);
        }
    }
#endif

    for (; column < last_input_column; column += 2) {
        if (column & 1) column--;
        lowpass[column / 2] = (input[column] + input[column + 1] + prescale_rounding) >> prescale;
        if ((column + 3) < width) {
            highpass[column / 2] = fused_hpf_ref_middle(input + column - 2, prescale_rounding, prescale);
        } else {
            int32_t sum = 0;
            sum -= (input[column - 2] + prescale_rounding) >> prescale;
            sum -= (input[column - 1] + prescale_rounding) >> prescale;
            sum += (input[column + 2] + prescale_rounding) >> prescale;
            sum += (input[column + 2] + prescale_rounding) >> prescale;
            sum += 4;
            sum = DivideByShift(sum, 3);
            sum += (input[column] + prescale_rounding) >> prescale;
            sum -= (input[column + 1] + prescale_rounding) >> prescale;
            highpass[column / 2] = ClampPixel(sum);
        }
    }

    column = last_input_column;
    if ((column + 1) < width) {
        highpass[column / 2] = fused_hpf_ref_coeff(input + column - 4, right_coeffs,
                                                   prescale_rounding, prescale);
        lowpass[column / 2] = (input[column] + input[column + 1] + prescale_rounding) >> prescale;
    } else {
        int32_t sum = 0;
        sum -= 5 * ((input[column] + prescale_rounding) >> prescale);
        sum += 11 * ((input[column] + prescale_rounding) >> prescale);
        sum -= 4 * ((input[column - 1] + prescale_rounding) >> prescale);
        sum -= 4 * ((input[column - 2] + prescale_rounding) >> prescale);
        sum += (input[column - 3] + prescale_rounding) >> prescale;
        sum += (input[column - 4] + prescale_rounding) >> prescale;
        sum += 4;
        sum = DivideByShift(sum, 3);
        highpass[column / 2] = ClampPixel(sum);
        lowpass[column / 2] = (input[column] + input[column] + prescale_rounding) >> prescale;
    }
}

/* ================================================================
   Horizontal wavelet filter (simplified from forward.c)
   ================================================================ */

static inline __attribute__((always_inline))
void horizontal_filter(const PIXEL *input, PIXEL *lowpass, PIXEL *highpass,
                               int width, int prescale)
{
    if (fused_reference_horizontal_enabled()) {
        horizontal_filter_reference(input, lowpass, highpass, width, prescale);
        return;
    }

    int prescale_rounding = (1 << prescale) - 1;
    int half = width / 2;

    /* Prescale helper */
    #define PS(v) (((v) + prescale_rounding) >> prescale)

    /* Left boundary */
    lowpass[0] = PS(input[0]) + PS(input[1]);
    highpass[0] = PS(input[0]) - PS(input[1]);

    /* Interior */
    {
        int i = 1;
#if ENABLED(NEON)
        const int32x4_t vround = vdupq_n_s32(prescale_rounding);
        const int32x4_t four = vdupq_n_s32(4);
        const int32x4_t neg_ps = vdupq_n_s32(-prescale);

        for (; i + 3 < half - 1; i += 4) {
            /* Output i covers input indices [2i-2 .. 2i+3] (6 inputs per output) */
            /* For 4 outputs (i..i+3): need inputs [2i-2 .. 2i+9] = 12 consecutive inputs */
            int idx = 2 * i;

            /* Load 12 consecutive inputs, prescale them all */
            int32x4_t in_lo  = vshlq_s32(vaddq_s32(vld1q_s32(&input[idx-2]), vround), neg_ps); /* [2i-2 .. 2i+1] */
            int32x4_t in_md  = vshlq_s32(vaddq_s32(vld1q_s32(&input[idx+2]), vround), neg_ps); /* [2i+2 .. 2i+5] */
            int32x4_t in_hi  = vshlq_s32(vaddq_s32(vld1q_s32(&input[idx+6]), vround), neg_ps); /* [2i+6 .. 2i+9] */

            /* Extract even and odd samples for the 4 outputs.
               Output i uses inputs[2i], inputs[2i+1] for current pair.
               Output i+1 uses [2i+2], [2i+3]. Output i+2 uses [2i+4], [2i+5]. Output i+3 uses [2i+6], [2i+7].
               So evens = inputs at offsets 2,4,6,8 from idx-2 = [2i, 2i+2, 2i+4, 2i+6]
                   odds = inputs at offsets 3,5,7,9 from idx-2 = [2i+1, 2i+3, 2i+5, 2i+7] */
            /* Use VEXT to slide and pick: */
            /* in_lo = [2i-2, 2i-1, 2i, 2i+1], in_md = [2i+2, 2i+3, 2i+4, 2i+5], in_hi = [2i+6, 2i+7, 2i+8, 2i+9] */
            /* evens[0..3] = [2i, 2i+2, 2i+4, 2i+6] */
            /* odds[0..3]  = [2i+1, 2i+3, 2i+5, 2i+7] */
            int32x4_t cur_pair = vextq_s32(in_lo, in_md, 2);  /* [2i, 2i+1, 2i+2, 2i+3] */
            int32x4_t nxt_pair = vextq_s32(in_md, in_hi, 2);  /* [2i+4, 2i+5, 2i+6, 2i+7] */
            /* Deinterleave cur_pair and nxt_pair to get evens/odds */
            int32x4x2_t cn = vuzpq_s32(cur_pair, nxt_pair);
            int32x4_t evens = cn.val[0];  /* [2i, 2i+2, 2i+4, 2i+6] */
            int32x4_t odds  = cn.val[1];  /* [2i+1, 2i+3, 2i+5, 2i+7] */

            /* Lowpass = even + odd */
            vst1q_s32(&lowpass[i], vaddq_s32(evens, odds));

            /* For highpass: prev_sum[k] = input[2(i+k)-2] + input[2(i+k)-1]
                              next_sum[k] = input[2(i+k)+2] + input[2(i+k)+3]
               prev_sum_vec = [pair_sum at 2i-2, 2i, 2i+2, 2i+4]
               next_sum_vec = [pair_sum at 2i+2, 2i+4, 2i+6, 2i+8]

               in_lo split: [2i-2, 2i-1, 2i, 2i+1]
                  in_lo even/odd via vuzp:
                    evens_lo = [2i-2, 2i]   odds_lo = [2i-1, 2i+1]
               in_md split: [2i+2, 2i+3, 2i+4, 2i+5]
                    evens_md = [2i+2, 2i+4] odds_md = [2i+3, 2i+5]
               in_hi split: [2i+6, 2i+7, 2i+8, 2i+9]
                    evens_hi = [2i+6, 2i+8] odds_hi = [2i+7, 2i+9]
            */
            int32x4x2_t ulo = vuzpq_s32(in_lo, in_md);  /* even[0..3] = [2i-2,2i,2i+2,2i+4]; odd[0..3] = [2i-1,2i+1,2i+3,2i+5] */
            int32x4x2_t uhi = vuzpq_s32(in_md, in_hi);  /* even[0..3] = [2i+2,2i+4,2i+6,2i+8]; odd[0..3] = [2i+3,2i+5,2i+7,2i+9] */
            int32x4_t prev_sum = vaddq_s32(ulo.val[0], ulo.val[1]);  /* pair_sum at 2i-2, 2i, 2i+2, 2i+4 */
            int32x4_t next_sum = vaddq_s32(uhi.val[0], uhi.val[1]);  /* pair_sum at 2i+2, 2i+4, 2i+6, 2i+8 */

            /* Highpass = ((next_sum - prev_sum + 4) >> 3) + (even - odd) */
            int32x4_t diff = vsubq_s32(next_sum, prev_sum);
            int32x4_t hp = vshrq_n_s32(vaddq_s32(diff, four), 3);
            hp = vaddq_s32(hp, vsubq_s32(evens, odds));
            vst1q_s32(&highpass[i], hp);
        }
#endif
        for (; i < half - 1; i++) {
            int idx = 2 * i;
            int32_t e0 = PS(input[idx]);
            int32_t o0 = PS(input[idx + 1]);
            lowpass[i] = e0 + o0;
            int32_t e_prev = PS(input[idx - 2]) + PS(input[idx - 1]);
            int32_t e_next = PS(input[idx + 2]) + PS(input[idx + 3]);
            highpass[i] = ((e_next - e_prev + 4) >> 3) + (e0 - o0);
        }
    }

    /* Right boundary */
    {
        int idx = 2 * (half - 1);
        lowpass[half - 1] = PS(input[idx]) + PS(input[idx + 1]);
        highpass[half - 1] = PS(input[idx]) - PS(input[idx + 1]);
    }

    /* Odd-width tail: produce one extra output for the unpaired last
       column by treating it as a pair with itself (replicated). Without
       this, every wavelet level whose input width is odd silently drops
       the right-most column — visible as a chunk of dead pixels in the
       reconstructed image. */
    if (width & 1) {
        int idx = width - 1;
        PIXEL last = PS(input[idx]);
        lowpass[half]  = last + last;
        highpass[half] = 0;
    }

    #undef PS
}

/* LP-only variant: produces only the lowpass output, skips all HP arithmetic.
   Used when GPR_DROP_HIGHPASS=1 (HP bands are discarded anyway, no point
   computing them). Removes ~half the per-row horizontal-filter work. */
static void horizontal_filter_lp_only(const PIXEL *input, PIXEL *lowpass,
                                       int width, int prescale)
{
    int prescale_rounding = (1 << prescale) - 1;
    int half = width / 2;
    #define PS(v) (((v) + prescale_rounding) >> prescale)

    lowpass[0] = PS(input[0]) + PS(input[1]);

    {
        int i = 1;
#if ENABLED(NEON)
        const int32x4_t vround = vdupq_n_s32(prescale_rounding);
        const int32x4_t neg_ps = vdupq_n_s32(-prescale);
        for (; i + 3 < half - 1; i += 4) {
            int idx = 2 * i;
            /* Need inputs [2i .. 2i+7] for 4 LP outputs (no halo). */
            int32x4_t in_lo = vshlq_s32(vaddq_s32(vld1q_s32(&input[idx + 0]), vround), neg_ps);
            int32x4_t in_hi = vshlq_s32(vaddq_s32(vld1q_s32(&input[idx + 4]), vround), neg_ps);
            /* vuzpq splits into evens/odds: evens=[2i,2i+2,2i+4,2i+6], odds=[2i+1,2i+3,2i+5,2i+7] */
            int32x4x2_t u = vuzpq_s32(in_lo, in_hi);
            vst1q_s32(&lowpass[i], vaddq_s32(u.val[0], u.val[1]));
        }
#endif
        for (; i < half - 1; i++) {
            int idx = 2 * i;
            lowpass[i] = PS(input[idx]) + PS(input[idx + 1]);
        }
    }

    {
        int idx = 2 * (half - 1);
        lowpass[half - 1] = PS(input[idx]) + PS(input[idx + 1]);
    }

    if (width & 1) {
        int idx = width - 1;
        PIXEL last = PS(input[idx]);
        lowpass[half] = last + last;
    }
    #undef PS
}

/* ================================================================
   Vertical filter + quantize (simplified from forward.c)
   ================================================================ */

#if ENABLED(NEON)
/* Quantize 4 int32 lanes: out = sign(v) * ((|v| + midpoint) * multiplier) >> 16.
   A76 tuning: fuses (>>16) + narrow into one `vshrn_n_s64` (shrn) per half
   instead of `vshrq_n_s64 + vmovn_s64`. Saves a dispatch slot; shrn issues
   on V1 freeing V0 for the next iteration's smull. Byte-exact result. */
static inline int32x4_t quantize_neon4(int32x4_t values, int32_t midpoint, int32_t multiplier) {
    int32x4_t abs_v = vabsq_s32(values);
    abs_v = vaddq_s32(abs_v, vdupq_n_s32(midpoint));
    int32x2_t mul_v = vdup_n_s32(multiplier);
    int64x2_t plo = vmull_s32(vget_low_s32(abs_v), mul_v);
    int64x2_t phi = vmull_s32(vget_high_s32(abs_v), mul_v);
    int32x4_t scaled = vcombine_s32(vshrn_n_s64(plo, 16), vshrn_n_s64(phi, 16));
    uint32x4_t neg = vcltq_s32(values, vdupq_n_s32(0));
    return vbslq_s32(neg, vnegq_s32(scaled), scaled);
}
#endif

/* Inline-mode BayesShrink-style soft-threshold for a quantized band row.
 *
 * Standard soft-thresholding: x_out = sign(x_in) * max(|x_in| - T, 0)
 *
 * T is the BayesShrink threshold in QUANTIZED coefficient units (caller has
 * already divided continuous T by the band's quantization step).
 *
 * Intentionally placed BETWEEN vertical_filter_quantize_row and
 * jans_inline_row — those are the only two places the inline path touches
 * each band row, so the band is still hot in L1 here. Per-row cost on
 * Pi 5 NEON is ~width/4 ops + a few setup ops → trivial relative to
 * tokenize cost. */
static inline void soft_threshold_row(PIXEL *row, int width, int32_t T)
{
    if (T <= 0) return;
    int i = 0;
#if ENABLED(NEON)
    const int32x4_t vT    = vdupq_n_s32(T);
    const int32x4_t vNegT = vdupq_n_s32(-T);
    const int32x4_t vZero = vdupq_n_s32(0);
    int w_m4 = (width / 4) * 4;
    for (; i < w_m4; i += 4) {
        int32x4_t v   = vld1q_s32(&row[i]);
        /* positive branch: max(v - T, 0)
           negative branch: min(v + T, 0) */
        int32x4_t vp  = vmaxq_s32(vsubq_s32(v, vT),    vZero);
        int32x4_t vn  = vminq_s32(vaddq_s32(v, vT),    vZero);
        /* pos mask: v > 0 → use vp; otherwise use vn (handles both
           negative AND in-deadzone cases since vn collapses to 0 then too). */
        uint32x4_t pos = vcgtq_s32(v, vZero);
        int32x4_t res = vbslq_s32(pos, vp, vn);
        vst1q_s32(&row[i], res);
    }
#endif
    for (; i < width; i++) {
        PIXEL v = row[i];
        if      (v >  T) row[i] = v - T;
        else if (v < -T) row[i] = v + T;
        else             row[i] = 0;
    }
}

/* Hard dead-zone threshold for quantized highpass coefficients:
   values inside [-T, T] become zero; larger signal coefficients are preserved.
   This is a more conservative noise-removal probe than soft-thresholding,
   which shrinks every surviving coefficient. */
static inline void hard_threshold_row(PIXEL *row, int width, int32_t T)
{
    if (T <= 0) return;
    int i = 0;
#if ENABLED(NEON)
    const int32x4_t vT    = vdupq_n_s32(T);
    const int32x4_t vZero = vdupq_n_s32(0);
    int w_m4 = (width / 4) * 4;
    for (; i < w_m4; i += 4) {
        int32x4_t v = vld1q_s32(&row[i]);
        uint32x4_t keep = vcgtq_s32(vabsq_s32(v), vT);
        vst1q_s32(&row[i], vbslq_s32(keep, v, vZero));
    }
#endif
    for (; i < width; i++) {
        PIXEL v = row[i];
        if (v <= T && v >= -T) row[i] = 0;
    }
}

static void vertical_filter_quantize_row(
    PIXEL *rows[6],
    int width,
    int32_t mid_lo, int32_t mul_lo,
    int32_t mid_hi, int32_t mul_hi,
    int32_t mid_unused1, int32_t mul_unused1,
    int32_t mid_unused2, int32_t mul_unused2,
    PIXEL *out_lo, PIXEL *out_hi, PIXEL *unused1, PIXEL *unused2,
    int is_top, int is_bottom)
{
    (void)mid_unused1; (void)mul_unused1; (void)mid_unused2; (void)mul_unused2;
    (void)unused1; (void)unused2;

    int col = 0;

#if ENABLED(NEON)
    if (!is_top && !is_bottom) {
        /* NEON middle row: 8-wide vertical filter + quantize.
           A76/A78 tuning: 8 outputs per iter doubles the in-flight
           independent loads/ops so the OOO core can keep both NEON pipes
           saturated. Restrict-locals + post-inc loads emit ldr q,[x],#16
           (single dispatch slot) vs indexed ldr q,[x,x] (5-cycle).
           4-wide tail handles width%8. */
        const int32x4_t four = vdupq_n_s32(4);
        const int width_m8 = (width / 8) * 8;
        const int width_m4 = (width / 4) * 4;
        const PIXEL *__restrict__ p0 = rows[0];
        const PIXEL *__restrict__ p1 = rows[1];
        const PIXEL *__restrict__ p2 = rows[2];
        const PIXEL *__restrict__ p3 = rows[3];
        const PIXEL *__restrict__ p4 = rows[4];
        const PIXEL *__restrict__ p5 = rows[5];
        PIXEL *__restrict__ qlo = out_lo;
        PIXEL *__restrict__ qhi = out_hi;

        for (int c = 0; c < width_m8; c += 8) {
            int32x4_t r0a = vld1q_s32(p0); p0 += 4;
            int32x4_t r0b = vld1q_s32(p0); p0 += 4;
            int32x4_t r1a = vld1q_s32(p1); p1 += 4;
            int32x4_t r1b = vld1q_s32(p1); p1 += 4;
            int32x4_t r2a = vld1q_s32(p2); p2 += 4;
            int32x4_t r2b = vld1q_s32(p2); p2 += 4;
            int32x4_t r3a = vld1q_s32(p3); p3 += 4;
            int32x4_t r3b = vld1q_s32(p3); p3 += 4;
            int32x4_t r4a = vld1q_s32(p4); p4 += 4;
            int32x4_t r4b = vld1q_s32(p4); p4 += 4;
            int32x4_t r5a = vld1q_s32(p5); p5 += 4;
            int32x4_t r5b = vld1q_s32(p5); p5 += 4;

            int32x4_t low_a  = vaddq_s32(r2a, r3a);
            int32x4_t low_b  = vaddq_s32(r2b, r3b);
            int32x4_t high_a = vsubq_s32(vaddq_s32(r4a, r5a), vaddq_s32(r0a, r1a));
            int32x4_t high_b = vsubq_s32(vaddq_s32(r4b, r5b), vaddq_s32(r0b, r1b));
            high_a = vshrq_n_s32(vaddq_s32(high_a, four), 3);
            high_b = vshrq_n_s32(vaddq_s32(high_b, four), 3);
            high_a = vaddq_s32(high_a, vsubq_s32(r2a, r3a));
            high_b = vaddq_s32(high_b, vsubq_s32(r2b, r3b));

            vst1q_s32(qlo, quantize_neon4(low_a,  mid_lo, mul_lo)); qlo += 4;
            vst1q_s32(qlo, quantize_neon4(low_b,  mid_lo, mul_lo)); qlo += 4;
            vst1q_s32(qhi, quantize_neon4(high_a, mid_hi, mul_hi)); qhi += 4;
            vst1q_s32(qhi, quantize_neon4(high_b, mid_hi, mul_hi)); qhi += 4;
        }
        /* 4-wide tail */
        for (int c = width_m8; c < width_m4; c += 4) {
            int32x4_t r0 = vld1q_s32(p0); p0 += 4;
            int32x4_t r1 = vld1q_s32(p1); p1 += 4;
            int32x4_t r2 = vld1q_s32(p2); p2 += 4;
            int32x4_t r3 = vld1q_s32(p3); p3 += 4;
            int32x4_t r4 = vld1q_s32(p4); p4 += 4;
            int32x4_t r5 = vld1q_s32(p5); p5 += 4;
            int32x4_t low  = vaddq_s32(r2, r3);
            int32x4_t high = vsubq_s32(vaddq_s32(r4, r5), vaddq_s32(r0, r1));
            high = vshrq_n_s32(vaddq_s32(high, four), 3);
            high = vaddq_s32(high, vsubq_s32(r2, r3));
            vst1q_s32(qlo, quantize_neon4(low,  mid_lo, mul_lo)); qlo += 4;
            vst1q_s32(qhi, quantize_neon4(high, mid_hi, mul_hi)); qhi += 4;
        }
        col = width_m4;
    }
#endif

    /* Scalar fallback (boundaries + cleanup).

       Legacy production code (forward.c:FilterVerticalTopBottom_1x) uses
       BOUNDARY-SPECIFIC 6-tap filters at top/bottom rows, NOT the middle-row
       formula. Without this, every wavelet level's boundary rows are
       reconstructed with the wrong filter, and in a 3-level cascade the
       boundary errors propagate up through the inverse cascade — this is
       a major contributor to the multi-level regression (task #172).

       legacy top filter coefficients:    {  5, -11,  4,  4, -1, -1 } / 8
       legacy bottom filter coefficients: {  1,   1, -4, -4, 11, -5 } / 8
       middle filter equivalent:          { -1,  -1,  8, -8,  1,  1 } / 8
                                         (after expanding ((X+4)>>3)+(r2-r3)) */
    for (; col < width; col++) {
        int32_t r0 = rows[0][col], r1 = rows[1][col], r2 = rows[2][col];
        int32_t r3 = rows[3][col], r4 = rows[4][col], r5 = rows[5][col];
        int32_t low, high;
        if (is_top) {
            low = r0 + r1;
            high = (5*r0 - 11*r1 + 4*r2 + 4*r3 - r4 - r5 + 4) >> 3;
        } else if (is_bottom) {
            low = r4 + r5;
            high = (r0 + r1 - 4*r2 - 4*r3 + 11*r4 - 5*r5 + 4) >> 3;
        } else {
            low = r2 + r3;
            high = ((r4 + r5 - r0 - r1 + 4) >> 3) + (r2 - r3);
        }
        out_lo[col] = quantize_scalar(low, mid_lo, mul_lo);
        out_hi[col] = quantize_scalar(high, mid_hi, mul_hi);
    }
}


/* LL-only variant: produces only the LP output, skips HP arithmetic.
   Used in GPR_DROP_HIGHPASS=1 mode to skip the LH-row computation when
   processing LP-input rows. Saves ~50% of the per-row vertical filter work
   for the LP-side call (only the lo side computes; hi side is skipped).
   Note: this is for the FIRST vertical_filter_quantize_row call (LP rows
   → LL + LH). The SECOND call (HP rows → HL + HH) gets skipped entirely
   at the caller. */
static void vertical_filter_quantize_row_lo_only(
    PIXEL *rows[6],
    int width,
    int32_t mid_lo, int32_t mul_lo,
    PIXEL *out_lo,
    int is_top, int is_bottom)
{
    int col = 0;

#if ENABLED(NEON)
    if (!is_top && !is_bottom) {
        const int width_m8 = (width / 8) * 8;
        const int width_m4 = (width / 4) * 4;
        const PIXEL *__restrict__ p2 = rows[2];
        const PIXEL *__restrict__ p3 = rows[3];
        PIXEL *__restrict__ qlo = out_lo;

        for (int c = 0; c < width_m8; c += 8) {
            int32x4_t r2a = vld1q_s32(p2); p2 += 4;
            int32x4_t r2b = vld1q_s32(p2); p2 += 4;
            int32x4_t r3a = vld1q_s32(p3); p3 += 4;
            int32x4_t r3b = vld1q_s32(p3); p3 += 4;
            int32x4_t low_a = vaddq_s32(r2a, r3a);
            int32x4_t low_b = vaddq_s32(r2b, r3b);
            vst1q_s32(qlo, quantize_neon4(low_a, mid_lo, mul_lo)); qlo += 4;
            vst1q_s32(qlo, quantize_neon4(low_b, mid_lo, mul_lo)); qlo += 4;
        }
        for (int c = width_m8; c < width_m4; c += 4) {
            int32x4_t r2 = vld1q_s32(p2); p2 += 4;
            int32x4_t r3 = vld1q_s32(p3); p3 += 4;
            int32x4_t low = vaddq_s32(r2, r3);
            vst1q_s32(qlo, quantize_neon4(low, mid_lo, mul_lo)); qlo += 4;
        }
        col = width_m4;
    }
#endif

    for (; col < width; col++) {
        int32_t r0 = rows[0][col], r1 = rows[1][col];
        int32_t r2 = rows[2][col], r3 = rows[3][col];
        int32_t r4 = rows[4][col], r5 = rows[5][col];
        int32_t low;
        if (is_top) { low = r0 + r1; }
        else if (is_bottom) { low = r4 + r5; }
        else { low = r2 + r3; }
        out_lo[col] = quantize_scalar(low, mid_lo, mul_lo);
    }
}


/* ================================================================
   Log curve setup
   ================================================================ */

extern uint16_t EncoderLogCurve14[];
extern uint16_t EncoderLogCurve16[];
extern void SetupEncoderLogCurve(void);

static inline uint16_t apply_log_curve(uint16_t value, int bits) {
    if (bits <= 14) {
        if (value > 16383) value = 16383;
        return EncoderLogCurve14[value];
    } else {
        return EncoderLogCurve16[value];
    }
}

/* ================================================================
   Multi-level helper: apply a full wavelet decomposition to a PIXEL
   buffer. Used for levels 2 and 3 after Pass 1 fills the level-1
   LL band. Sequential (not fused with the unpack/log stage that
   only exists for the raw Bayer input).
   ================================================================ */

/* Decompose `input` (in_width × in_height PIXEL) into 4 quantized subbands
   of (in_width/2 × in_height/2). prescale=2 — without this the LL magnitude
   grows 4× per level and quickly overflows the rANS encoder's mag-class
   ceiling (2047). Matches the production encoder's {0,2,2} prescale table:
   level 1 (this encoder's level 1) uses prescale=2 already, and so do
   levels 2 and 3 here, keeping LL bounded at every level.
   LL is NOT quantized here even if mid[0]/mul[0] are passed —
   divisor=1 in the quant table produces a no-op. */
static void wavelet_decompose_buffer(
    const PIXEL *input, int in_width, int in_height,
    const int32_t mid[4], const int32_t mul[4],
    PIXEL *out_ll, PIXEL *out_lh, PIXEL *out_hl, PIXEL *out_hh)
{
    int out_width = in_width / 2;
    int out_height = in_height / 2;

    PIXEL *lp_rows[FUSED_ROW_BUFS];
    PIXEL *hp_rows[FUSED_ROW_BUFS];
    for (int r = 0; r < FUSED_ROW_BUFS; r++) {
        lp_rows[r] = (PIXEL *)fused_aligned_alloc(out_width * sizeof(PIXEL));
        hp_rows[r] = (PIXEL *)fused_aligned_alloc(out_width * sizeof(PIXEL));
        if (!lp_rows[r] || !hp_rows[r]) {
            for (int q = 0; q <= r; q++) {
                if (lp_rows[q]) free(lp_rows[q]);
                if (hp_rows[q]) free(hp_rows[q]);
            }
            return;
        }
    }

    int out_row = 0;
    int buf_filled = 0;

    /* Per the FUSED_L2_L3_PRESCALE env hook, allow swapping the L2/L3
       wavelet prescale. Default=2 (current code); experimental 0 matches
       the inverse decoder's documented "no descale" for L2/L3. */
    const char *_l23ps = getenv("FUSED_L2_L3_PRESCALE");
    int l23_prescale = (_l23ps && *_l23ps) ? atoi(_l23ps) : 2;

    for (int row = 0; row < in_height; row++) {
        int slot = row % FUSED_ROW_BUFS;
        horizontal_filter(input + row * in_width,
                          lp_rows[slot], hp_rows[slot],
                          in_width, l23_prescale);
        buf_filled++;

        /* The biorthogonal 5/3 forward needs two outputs from the first 6-row
           window: out_row=0 (top-boundary, LP = r0+r1, content from inputs
           {0,1}) AND out_row=1 (interior, LP = r2+r3, content from inputs
           {2,3}). Subsequent windows slide by 2 and emit one row each.
           Matches the reference encoder (encoder.c:1296-1336). */
        if (buf_filled >= 6 && (buf_filled % 2) == 0) {
            int base = (row + 1 - 6) % FUSED_ROW_BUFS;
            if (base < 0) base += FUSED_ROW_BUFS;
            PIXEL *lprefs[6], *hprefs[6];
            for (int r = 0; r < 6; r++) {
                int idx = (base + r) % FUSED_ROW_BUFS;
                lprefs[r] = lp_rows[idx];
                hprefs[r] = hp_rows[idx];
            }
            int n_emits = (out_row == 0) ? 2 : 1;
            for (int e = 0; e < n_emits; e++) {
                if (out_row >= out_height) break;
                int is_top = (out_row == 0);
                int is_bottom = (out_row == out_height - 1);

                vertical_filter_quantize_row(lprefs, out_width,
                    mid[0], mul[0],  mid[2], mul[2],
                    0,0, 0,0,
                    out_ll + out_row * out_width,
                    out_hl + out_row * out_width,
                    NULL, NULL,
                    is_top, is_bottom);

                vertical_filter_quantize_row(hprefs, out_width,
                    mid[1], mul[1],  mid[3], mul[3],
                    0,0, 0,0,
                    out_lh + out_row * out_width,
                    out_hh + out_row * out_width,
                    NULL, NULL,
                    is_top, is_bottom);

                out_row++;
            }
            if (out_row >= out_height) break;
        }
    }

    /* Bottom-edge handling: the 6-tap vertical filter under-runs by 2 rows.
       Emit the missing last rows by re-running the filter with is_bottom=true
       to clamp the bottom boundary. Without this, the last 2 rows stay zero,
       which contributes to the multi-level regression (task #172) — for
       period-N stripes the 2 zero rows are conspicuous, and the inverse
       cascade propagates the loss to L2 → L1 reconstruction. */
    while (out_row < out_height) {
        /* Use the last 6 rows we have buffered. Pad by reusing the last
           row when buf_filled < 6 (only happens for tiny inputs). */
        int last_input_row = in_height - 1;
        int base = (last_input_row + 1 - 6) % FUSED_ROW_BUFS;
        if (base < 0) base += FUSED_ROW_BUFS;
        PIXEL *lprefs[6], *hprefs[6];
        for (int r = 0; r < 6; r++) {
            int idx = (base + r) % FUSED_ROW_BUFS;
            lprefs[r] = lp_rows[idx];
            hprefs[r] = hp_rows[idx];
        }
        vertical_filter_quantize_row(lprefs, out_width,
            mid[0], mul[0],  mid[2], mul[2],
            0,0, 0,0,
            out_ll + out_row * out_width,
            out_hl + out_row * out_width,
            NULL, NULL,
            /*is_top=*/0, /*is_bottom=*/1);
        vertical_filter_quantize_row(hprefs, out_width,
            mid[1], mul[1],  mid[3], mul[3],
            0,0, 0,0,
            out_lh + out_row * out_width,
            out_hh + out_row * out_width,
            NULL, NULL,
            /*is_top=*/0, /*is_bottom=*/1);
        out_row++;
    }

    for (int r = 0; r < FUSED_ROW_BUFS; r++) {
        free(lp_rows[r]);
        free(hp_rows[r]);
    }
}

/* ================================================================
   Pass 1: Fused Unpack → Wavelet → Quantize → FreqCount
   ================================================================ */

typedef struct {
    /* Per-channel wavelet state (level 1 only — see below for higher levels) */
    PIXEL *lowpass_buf[FUSED_ROW_BUFS];   /* Horizontal lowpass 6-row circular buffer */
    PIXEL *highpass_buf[FUSED_ROW_BUFS];  /* Horizontal highpass 6-row circular buffer */
    int buf_row;                           /* Current position in circular buffer */

    /* Level-1 (largest) per-band output. band_data[0] = LL1 — fed into the
       level-2 wavelet pass post-Pass-1 in multi-level mode, discarded
       otherwise. band_data[1..3] = LH1/HL1/HH1 (encoded). */
    PIXEL *band_data[FUSED_MAX_BANDS];    /* Quantized band buffers (NULL in inline mode) */
    PIXEL *row_scratch[FUSED_MAX_BANDS];  /* Per-row scratch (~5KB × 4) used in inline mode */
    int band_width, band_height;
    int band_pitch;                        /* In pixels */
    int band_out_row;                      /* Current output row */

    /* Quantization parameters per band (level 1) */
    int32_t midpoint[FUSED_MAX_BANDS];
    int32_t multiplier[FUSED_MAX_BANDS];

    /* ANS frequency tables per band (legacy/unused since the freq-removal commit) */
    uint16_t freq[FUSED_MAX_BANDS][160];
    int run_state[FUSED_MAX_BANDS];

    /* Inline-tokenize state per highpass band (NULL in split-pass mode).
       Owned by FUSED_ENCODER; reset each frame. */
    JANS_INLINE_STATE *inline_state[FUSED_MAX_BANDS];

    /* ---- Multi-level (3-level wavelet) extension. NULL when multi_level off.
       Sequential mode: band_data_l2[0] holds the entire LL2 band (input to
       level-3). Streaming mode: band_data_l2[0] is NULL — LL2 rows feed
       directly into the level-3 horizontal filter via lp_buf_l3.
       LH/HL/HH at each level are always allocated (they are Pass 2 inputs).
       LL3 is always allocated and encoded. */
    PIXEL *band_data_l2[FUSED_MAX_BANDS];
    PIXEL *band_data_l3[FUSED_MAX_BANDS];
    int band_width_l2, band_height_l2;
    int band_width_l3, band_height_l3;
    int32_t midpoint_l2[FUSED_MAX_BANDS], multiplier_l2[FUSED_MAX_BANDS];
    int32_t midpoint_l3[FUSED_MAX_BANDS], multiplier_l3[FUSED_MAX_BANDS];

    /* Streaming pyramid: per-level 6-row horizontal filter buffer fed by
       the previous level's vertical-filter output. NULL when not in
       streaming mode. */
    PIXEL *lp_buf_l2[FUSED_ROW_BUFS], *hp_buf_l2[FUSED_ROW_BUFS];
    int buf_row_l2;
    int band_out_row_l2;
    PIXEL *ll2_row_scratch;  /* 1-row buffer for the unused LL2 result */

    PIXEL *lp_buf_l3[FUSED_ROW_BUFS], *hp_buf_l3[FUSED_ROW_BUFS];
    int buf_row_l3;
    int band_out_row_l3;

    int streaming_active;  /* 1 = run cascade in pass1; 0 = sequential post-pass */
    int levels;            /* Wavelet depth: 1 = single, 2 = 2-level (LL2 emitted),
                              3 = 3-level (LL3 emitted via cascade). */

    /* Anti-alias prefilter history for L2 input. When FUSED_L2_PREFILTER=3,
       stream_cascade_higher_levels keeps the last 2 ll1 rows here so it can
       compute a 3-tap [1,2,1]/4 vertical LP on the LL1 stream before the
       L2 wavelet sees it. NULL when the prefilter is off. */
    PIXEL *ll1_prev1;
    PIXEL *ll1_prev2;
    int ll1_history_count;

    /* Inline-mode BayesShrink threshold per band, in QUANTIZED coefficient
       units. 0 = no thresholding. Bands [LL=0, LH=1, HL=2, HH=3]. LL is left
       at 0 (DC content shouldn't be soft-thresholded). Per-channel because
       different Bayer color planes have different noise characteristics. */
    int32_t inline_denoise_T[FUSED_MAX_BANDS];
    int inline_denoise_hard;

    /* Per-context mode bits captured at encoder creation. Pass1 runs on
       worker threads, so these must not be read through function-static
       getenv caches. */
    int row_decimate;
    int col_decimate;
    int decimate_aa;
    int aa_luma_only;
    int drop_hp;
    int fuse_lp_off;
    int luma_fused_off;
} FUSED_CHANNEL_STATE;

/* ================================================================
   Per-channel Pass 1 (one of 4 parallel threads)
   ================================================================ */

/* Unpack ONE channel from a Bayer row pair (called per output row by a channel thread).
   GS/GD need G1+G2 only. RG/BG also need R or B and compute GS as an intermediate.
   NEON path: 4-wide arithmetic with scalar LUT lookups (gather phase + NEON compute). */
static void unpack_channel_row(
    int channel, int is_rggb,
    const uint16_t *log_tbl, int log_max, int32_t mid2,
    const uint16_t *row1, const uint16_t *row2,
    PIXEL *output, int ch_width)
{
    int col = 0;

#if ENABLED(NEON)
    const int ch_width_m8 = (ch_width / 8) * 8;
    const int ch_width_m4 = (ch_width / 4) * 4;
    const int32x4_t vmid2 = vdupq_n_s32(mid2);
    const uint16x8_t v_log_max = vdupq_n_u16((uint16_t)log_max);

    /* Prefetch the LAST cache line of row1/row2 to warm L1 against the
       HW prefetcher's startup latency. The body of the loop reads
       sequentially so the stride prefetcher catches up quickly, but the
       first iter sees a cold front. PLDL1KEEP locality hint = stay in L1.
       Tried also prefetching next-iter rows 4 bayer rows ahead — regressed
       (+2 ms) because the extra prefetch hints competed with the actual
       reads for LSU dispatch. */
    __builtin_prefetch(&row1[ch_width * 2 - 32], 0, 3);
    __builtin_prefetch(&row2[ch_width * 2 - 32], 0, 3);

    /* 8-wide path: load + branchless clip via NEON; LUT gather stays scalar
       (ARM has no arbitrary u16 gather). Outputs are emitted in two 4-wide
       NEON chunks so the downstream arithmetic stays identical to the
       4-wide path. */
    for (; col < ch_width_m8; col += 8) {
        /* vld2q_u16 deinterleaves the strided Bayer reads:
             RGGB: row1 = R G1 R G1 ... → (R[0..7], G1[0..7])
                   row2 = G2 B G2 B ... → (G2[0..7], B[0..7])
             GBRG: row1 = G1 B G1 B ... → (G1[0..7], B[0..7])
                   row2 = R G2 R G2 ... → (R[0..7], G2[0..7])
           One vld2q replaces 8 strided u16 loads + an 8-way clip. */
        uint16x8x2_t r1d = vld2q_u16(&row1[2*col]);
        uint16x8x2_t r2d = vld2q_u16(&row2[2*col]);

        uint16x8_t vR, vG1, vG2, vB;
        if (is_rggb) {
            vR  = vminq_u16(r1d.val[0], v_log_max);
            vG1 = vminq_u16(r1d.val[1], v_log_max);
            vG2 = vminq_u16(r2d.val[0], v_log_max);
            vB  = vminq_u16(r2d.val[1], v_log_max);
        } else {
            vG1 = vminq_u16(r1d.val[0], v_log_max);
            vB  = vminq_u16(r1d.val[1], v_log_max);
            vR  = vminq_u16(r2d.val[0], v_log_max);
            vG2 = vminq_u16(r2d.val[1], v_log_max);
        }

        uint16_t Rs[8], G1s[8], G2s[8], Bs[8];
        if (channel == 1) vst1q_u16(Rs, vR);
        if (channel == 2) vst1q_u16(Bs, vB);
        vst1q_u16(G1s, vG1);
        vst1q_u16(G2s, vG2);

        /* Emit two 4-wide NEON output chunks per 8-pixel iter. */
        for (int half = 0; half < 2; half++) {
            int32_t g1a[4], g2a[4];
            for (int k = 0; k < 4; k++) {
                g1a[k] = log_tbl[G1s[half*4 + k]];
                g2a[k] = log_tbl[G2s[half*4 + k]];
            }
            int32x4_t vg1 = vld1q_s32(g1a);
            int32x4_t vg2 = vld1q_s32(g2a);

            int32x4_t result;
            if (channel == 0) {
                result = vshrq_n_s32(vaddq_s32(vg1, vg2), 1);
            } else if (channel == 3) {
                result = vshrq_n_s32(vaddq_s32(vsubq_s32(vg1, vg2), vmid2), 1);
            } else {
                int32_t xa[4];
                if (channel == 1) {
                    for (int k = 0; k < 4; k++) xa[k] = log_tbl[Rs[half*4 + k]];
                } else {
                    for (int k = 0; k < 4; k++) xa[k] = log_tbl[Bs[half*4 + k]];
                }
                int32x4_t vx = vld1q_s32(xa);
                int32x4_t vgs = vshrq_n_s32(vaddq_s32(vg1, vg2), 1);
                result = vshrq_n_s32(vaddq_s32(vsubq_s32(vx, vgs), vmid2), 1);
            }
            vst1q_s32(&output[col + half*4], result);
        }
    }

    /* 4-wide tail when ch_width isn't a multiple of 8 (rare — almost
       always 0 cols for the common Z8/X2D widths). */
    for (; col < ch_width_m4; col += 4) {
        int32_t ra[4], ba[4], g1a[4], g2a[4];
        for (int k = 0; k < 4; k++) {
            int c = col + k;
            uint16_t R, G1, G2, B;
            if (is_rggb) {
                R  = row1[2*c]; G1 = row1[2*c+1]; G2 = row2[2*c]; B  = row2[2*c+1];
            } else {
                G1 = row1[2*c]; B  = row1[2*c+1]; R  = row2[2*c]; G2 = row2[2*c+1];
            }
            if (R  > log_max) R  = log_max;
            if (G1 > log_max) G1 = log_max;
            if (G2 > log_max) G2 = log_max;
            if (B  > log_max) B  = log_max;
            ra[k] = log_tbl[R]; g1a[k] = log_tbl[G1];
            g2a[k] = log_tbl[G2]; ba[k] = log_tbl[B];
        }
        int32x4_t vg1 = vld1q_s32(g1a), vg2 = vld1q_s32(g2a);
        int32x4_t result;
        if (channel == 0) {
            result = vshrq_n_s32(vaddq_s32(vg1, vg2), 1);
        } else if (channel == 3) {
            result = vshrq_n_s32(vaddq_s32(vsubq_s32(vg1, vg2), vmid2), 1);
        } else {
            int32x4_t vx = vld1q_s32(channel == 1 ? ra : ba);
            int32x4_t vgs = vshrq_n_s32(vaddq_s32(vg1, vg2), 1);
            result = vshrq_n_s32(vaddq_s32(vsubq_s32(vx, vgs), vmid2), 1);
        }
        vst1q_s32(&output[col], result);
    }
#endif

    /* Scalar cleanup for tail columns */
    for (; col < ch_width; col++) {
        uint16_t Rv, G1v, G2v, Bv;
        if (is_rggb) {
            Rv = row1[2*col]; G1v = row1[2*col+1]; G2v = row2[2*col]; Bv = row2[2*col+1];
        } else {
            G1v = row1[2*col]; Bv = row1[2*col+1]; Rv = row2[2*col]; G2v = row2[2*col+1];
        }
        if (G1v > log_max) G1v = log_max;
        if (G2v > log_max) G2v = log_max;
        int32_t g1 = log_tbl[G1v], g2 = log_tbl[G2v];

        switch (channel) {
            case 0: output[col] = (g1 + g2) >> 1; break;
            case 1: {
                if (Rv > log_max) Rv = log_max;
                int32_t r = log_tbl[Rv], gs = (g1 + g2) >> 1;
                output[col] = ((r - gs) + mid2) >> 1;
            } break;
            case 2: {
                if (Bv > log_max) Bv = log_max;
                int32_t b = log_tbl[Bv], gs = (g1 + g2) >> 1;
                output[col] = ((b - gs) + mid2) >> 1;
            } break;
            case 3: output[col] = ((g1 - g2) + mid2) >> 1; break;
        }
    }
}

/* No-decimate luma specialization for channels 0=GS and 3=GD.
   The generic unpack path also supports chroma and therefore carries channel
   branches plus R/B scratch handling inside the row loop. Luma needs only G1
   and G2 log values, so keep this path smaller and branch outside the loop. */
static void unpack_luma_row(
    int channel, int is_rggb,
    const uint16_t *log_tbl, int log_max, int32_t mid2,
    const uint16_t *row1, const uint16_t *row2,
    PIXEL *output, int ch_width)
{
    int col = 0;
    const int is_gd = (channel == 3);

#if ENABLED(NEON)
    const int ch_width_m8 = (ch_width / 8) * 8;
    const int ch_width_m4 = (ch_width / 4) * 4;
    const int32x4_t vmid2 = vdupq_n_s32(mid2);
    const uint16x8_t v_log_max = vdupq_n_u16((uint16_t)log_max);

    __builtin_prefetch(&row1[ch_width * 2 - 32], 0, 3);
    __builtin_prefetch(&row2[ch_width * 2 - 32], 0, 3);

    for (; col < ch_width_m8; col += 8) {
        uint16x8x2_t r1d = vld2q_u16(&row1[2*col]);
        uint16x8x2_t r2d = vld2q_u16(&row2[2*col]);
        uint16x8_t vG1, vG2;
        if (is_rggb) {
            vG1 = vminq_u16(r1d.val[1], v_log_max);
            vG2 = vminq_u16(r2d.val[0], v_log_max);
        } else {
            vG1 = vminq_u16(r1d.val[0], v_log_max);
            vG2 = vminq_u16(r2d.val[1], v_log_max);
        }

        uint16_t G1s[8], G2s[8];
        vst1q_u16(G1s, vG1);
        vst1q_u16(G2s, vG2);

        for (int half = 0; half < 2; half++) {
            int32_t g1a[4], g2a[4];
            for (int k = 0; k < 4; k++) {
                g1a[k] = log_tbl[G1s[half*4 + k]];
                g2a[k] = log_tbl[G2s[half*4 + k]];
            }
            int32x4_t vg1 = vld1q_s32(g1a);
            int32x4_t vg2 = vld1q_s32(g2a);
            int32x4_t result;
            if (is_gd) {
                result = vshrq_n_s32(vaddq_s32(vsubq_s32(vg1, vg2), vmid2), 1);
            } else {
                result = vshrq_n_s32(vaddq_s32(vg1, vg2), 1);
            }
            vst1q_s32(&output[col + half*4], result);
        }
    }

    for (; col < ch_width_m4; col += 4) {
        int32_t g1a[4], g2a[4];
        for (int k = 0; k < 4; k++) {
            int c = col + k;
            uint16_t G1, G2;
            if (is_rggb) {
                G1 = row1[2*c+1]; G2 = row2[2*c];
            } else {
                G1 = row1[2*c];   G2 = row2[2*c+1];
            }
            if (G1 > log_max) G1 = log_max;
            if (G2 > log_max) G2 = log_max;
            g1a[k] = log_tbl[G1];
            g2a[k] = log_tbl[G2];
        }
        int32x4_t vg1 = vld1q_s32(g1a);
        int32x4_t vg2 = vld1q_s32(g2a);
        int32x4_t result;
        if (is_gd) {
            result = vshrq_n_s32(vaddq_s32(vsubq_s32(vg1, vg2), vmid2), 1);
        } else {
            result = vshrq_n_s32(vaddq_s32(vg1, vg2), 1);
        }
        vst1q_s32(&output[col], result);
    }
#endif

    for (; col < ch_width; col++) {
        uint16_t G1v, G2v;
        if (is_rggb) {
            G1v = row1[2*col+1]; G2v = row2[2*col];
        } else {
            G1v = row1[2*col];   G2v = row2[2*col+1];
        }
        if (G1v > log_max) G1v = log_max;
        if (G2v > log_max) G2v = log_max;
        int32_t g1 = log_tbl[G1v];
        int32_t g2 = log_tbl[G2v];
        if (is_gd) output[col] = ((g1 - g2) + mid2) >> 1;
        else       output[col] = (g1 + g2) >> 1;
    }
}

/* Fused unpack + 2x2 channel-space decimation.
   Reads 4 Bayer rows (2 RGGB pairs) and produces 1 channel output row at
   ch_width_out = ch_width/2 (half the post-unpack channel width).

   Per output sample, averages 4 same-color values IN LOG SPACE — i.e. apply
   the log curve to each of the 4 raw values then average. Averaging raw
   values BEFORE the log curve is *wrong* because the log curve is steeply
   nonlinear (especially at low values): e.g. log((2+8)/2)=log(5)≈2.3 but
   (log(2)+log(8))/2 = (1+3)/2 = 2. That ~15% bias differed per channel and
   produced a visible orange cast + banding in shadows. Correct math now.

   Cost: 4 LUT lookups per output per color (vs 1 if we could average raw),
   but still saves vs the naive "unpack at full width + NEON pair-average"
   path because we do half as many output rows (row decimation absorbs the
   would-be-skipped Bayer pairs as the second pair to average with). */
static void unpack_channel_row_decimate_2x2(
    int channel, int is_rggb,
    const uint16_t *log_tbl, int log_max, int32_t mid2,
    const uint16_t *row1a, const uint16_t *row2a,  /* first RGGB pair */
    const uint16_t *row1b, const uint16_t *row2b,  /* second RGGB pair */
    PIXEL *output, int ch_width_out)
{
    int o = 0;
    const uint16_t lm = (uint16_t)log_max;

#if ENABLED(NEON)
    const int o_m4 = (ch_width_out / 4) * 4;
    const uint16x8_t v_log_max = vdupq_n_u16(lm);
    const int32x4_t vmid2 = vdupq_n_s32(mid2);

#if defined(__aarch64__) && !defined(FUSED_DISABLE_ASM)
    /* Hand-tuned ARM64 NEON path. Bit-exact with the C intrinsics
       body below — verified via SHA256 of encoded .gpr output on Pi 5.
       Runtime-gated via FUSED_USE_ASM=1 (DEFAULT OFF: measured slower
       than the C path on Cortex-A76, see commit message for details).
       Kept here as scaffolding for future architecture-specific tuning
       (e.g. A78 / X1 where NEON-GPR move throughput is higher). */
    extern int fused_unpack_dec2x2_neon_asm(
        int channel, int is_rggb,
        const uint16_t *log_tbl, int log_max, int32_t mid2,
        const uint16_t *row1a, const uint16_t *row2a,
        const uint16_t *row1b, const uint16_t *row2b,
        int32_t *output, int count4);
    static int asm_on = -1;
    if (asm_on < 0) {
        const char *e = getenv("FUSED_USE_ASM");
        asm_on = (e && *e == '1') ? 1 : 0;
    }
    if (asm_on && o_m4 > 0) {
        if (fused_unpack_dec2x2_neon_asm(channel, is_rggb,
                                          log_tbl, log_max, mid2,
                                          row1a, row2a, row1b, row2b,
                                          output, o_m4) == 0) {
            o = o_m4;
            goto _asm_done;
        }
    }
#endif

    /* Per iter: 4 output samples; reads 16 Bayer cols from each of 4 rows. */
    for (; o < o_m4; o += 4) {
        int bc = o * 4;  /* starting Bayer column */

        /* Load 8 deinterleaved pairs from each row. */
        uint16x8x2_t e_a = vld2q_u16(&row1a[bc]);
        uint16x8x2_t o_a = vld2q_u16(&row2a[bc]);
        uint16x8x2_t e_b = vld2q_u16(&row1b[bc]);
        uint16x8x2_t o_b = vld2q_u16(&row2b[bc]);

        /* Identify which deinterleaved lane carries which color. */
        uint16x8_t vR_a, vR_b, vG1_a, vG1_b, vG2_a, vG2_b, vB_a, vB_b;
        if (is_rggb) {
            vR_a  = vminq_u16(e_a.val[0], v_log_max);
            vG1_a = vminq_u16(e_a.val[1], v_log_max);
            vG2_a = vminq_u16(o_a.val[0], v_log_max);
            vB_a  = vminq_u16(o_a.val[1], v_log_max);
            vR_b  = vminq_u16(e_b.val[0], v_log_max);
            vG1_b = vminq_u16(e_b.val[1], v_log_max);
            vG2_b = vminq_u16(o_b.val[0], v_log_max);
            vB_b  = vminq_u16(o_b.val[1], v_log_max);
        } else {
            vG1_a = vminq_u16(e_a.val[0], v_log_max);
            vB_a  = vminq_u16(e_a.val[1], v_log_max);
            vR_a  = vminq_u16(o_a.val[0], v_log_max);
            vG2_a = vminq_u16(o_a.val[1], v_log_max);
            vG1_b = vminq_u16(e_b.val[0], v_log_max);
            vB_b  = vminq_u16(e_b.val[1], v_log_max);
            vR_b  = vminq_u16(o_b.val[0], v_log_max);
            vG2_b = vminq_u16(o_b.val[1], v_log_max);
        }

        /* For output k (k=0..3), the 4 same-color source values are at
           lanes (2k, 2k+1) of both row_a and row_b. Lay them out flat in
           per-color 16-entry arrays so the 16 LUT lookups per color are
           all data-independent — the compiler / OoO can issue them via
           both LSU ports in parallel. After the lookups, NEON does the
           4-way pair-sum to produce 4 averaged log values. */
#if defined(FUSED_LOG_POLYNOMIAL)
        /* Polynomial log curve replaces the 32+ scalar gathers below. */
        (void)log_tbl;
        uint16x8_t g1_a16 = fused_log_curve_neon8_u16(vG1_a, log_max);
        uint16x8_t g1_b16 = fused_log_curve_neon8_u16(vG1_b, log_max);
        uint16x8_t g2_a16 = fused_log_curve_neon8_u16(vG2_a, log_max);
        uint16x8_t g2_b16 = fused_log_curve_neon8_u16(vG2_b, log_max);
        uint16x8_t x_a16_poly = vdupq_n_u16(0), x_b16_poly = vdupq_n_u16(0);
        if (channel == 1) {
            x_a16_poly = fused_log_curve_neon8_u16(vR_a, log_max);
            x_b16_poly = fused_log_curve_neon8_u16(vR_b, log_max);
        } else if (channel == 2) {
            x_a16_poly = fused_log_curve_neon8_u16(vB_a, log_max);
            x_b16_poly = fused_log_curve_neon8_u16(vB_b, log_max);
        }
#else
        uint16_t G1_idx[16], G2_idx[16], R_idx[16], B_idx[16];
        vst1q_u16(&G1_idx[0], vG1_a);
        vst1q_u16(&G1_idx[8], vG1_b);
        vst1q_u16(&G2_idx[0], vG2_a);
        vst1q_u16(&G2_idx[8], vG2_b);
        if (channel == 1) {
            vst1q_u16(&R_idx[0], vR_a);
            vst1q_u16(&R_idx[8], vR_b);
        }
        if (channel == 2) {
            vst1q_u16(&B_idx[0], vB_a);
            vst1q_u16(&B_idx[8], vB_b);
        }

        /* For each output k, we want the avg of the 4 same-color samples
           at flat indices {2k, 2k+1, 8+2k, 8+2k+1}. We store the 16 LUT
           outputs as u16 (the encoder log curve fits in 14 or 16 bits) and
           pair-add via vpaddlq_u16 (u16 lanes → u32 lanes in 1 op).
           Compared to the prior i32 scratch: half the store/load bandwidth,
           no STLF size-mismatch (u16 store ↔ u16 load forwards on A76),
           and 1 fewer ALU op per color (vpaddlq does pair-add + widen in
           one instruction). 4-way sum max value = 4 × 16383 = 65532 (fits
           in u16 sum after pair-add gives u32, so no overflow). */
        uint16_t G1log[16], G2log[16], Xlog[16];
        G1log[ 0] = log_tbl[G1_idx[ 0]]; G1log[ 1] = log_tbl[G1_idx[ 1]];
        G1log[ 2] = log_tbl[G1_idx[ 2]]; G1log[ 3] = log_tbl[G1_idx[ 3]];
        G1log[ 4] = log_tbl[G1_idx[ 4]]; G1log[ 5] = log_tbl[G1_idx[ 5]];
        G1log[ 6] = log_tbl[G1_idx[ 6]]; G1log[ 7] = log_tbl[G1_idx[ 7]];
        G1log[ 8] = log_tbl[G1_idx[ 8]]; G1log[ 9] = log_tbl[G1_idx[ 9]];
        G1log[10] = log_tbl[G1_idx[10]]; G1log[11] = log_tbl[G1_idx[11]];
        G1log[12] = log_tbl[G1_idx[12]]; G1log[13] = log_tbl[G1_idx[13]];
        G1log[14] = log_tbl[G1_idx[14]]; G1log[15] = log_tbl[G1_idx[15]];
        G2log[ 0] = log_tbl[G2_idx[ 0]]; G2log[ 1] = log_tbl[G2_idx[ 1]];
        G2log[ 2] = log_tbl[G2_idx[ 2]]; G2log[ 3] = log_tbl[G2_idx[ 3]];
        G2log[ 4] = log_tbl[G2_idx[ 4]]; G2log[ 5] = log_tbl[G2_idx[ 5]];
        G2log[ 6] = log_tbl[G2_idx[ 6]]; G2log[ 7] = log_tbl[G2_idx[ 7]];
        G2log[ 8] = log_tbl[G2_idx[ 8]]; G2log[ 9] = log_tbl[G2_idx[ 9]];
        G2log[10] = log_tbl[G2_idx[10]]; G2log[11] = log_tbl[G2_idx[11]];
        G2log[12] = log_tbl[G2_idx[12]]; G2log[13] = log_tbl[G2_idx[13]];
        G2log[14] = log_tbl[G2_idx[14]]; G2log[15] = log_tbl[G2_idx[15]];
        if (channel == 1 || channel == 2) {
            const uint16_t *idx = (channel == 1) ? R_idx : B_idx;
            Xlog[ 0] = log_tbl[idx[ 0]]; Xlog[ 1] = log_tbl[idx[ 1]];
            Xlog[ 2] = log_tbl[idx[ 2]]; Xlog[ 3] = log_tbl[idx[ 3]];
            Xlog[ 4] = log_tbl[idx[ 4]]; Xlog[ 5] = log_tbl[idx[ 5]];
            Xlog[ 6] = log_tbl[idx[ 6]]; Xlog[ 7] = log_tbl[idx[ 7]];
            Xlog[ 8] = log_tbl[idx[ 8]]; Xlog[ 9] = log_tbl[idx[ 9]];
            Xlog[10] = log_tbl[idx[10]]; Xlog[11] = log_tbl[idx[11]];
            Xlog[12] = log_tbl[idx[12]]; Xlog[13] = log_tbl[idx[13]];
            Xlog[14] = log_tbl[idx[14]]; Xlog[15] = log_tbl[idx[15]];
        }

        /* NEON reduce: vpaddlq_u16 pair-adds adjacent u16 lanes within one
           q-vector, widening result to u32. For each 8-lane row-half:
             [a0..a7] → [a0+a1, a2+a3, a4+a5, a6+a7] (4 × u32)
           Sum the two row-halves (row_a + row_b) lane-wise → 4-way avg. */
        uint16x8_t g1_a16 = vld1q_u16(&G1log[ 0]);
        uint16x8_t g1_b16 = vld1q_u16(&G1log[ 8]);
        uint16x8_t g2_a16 = vld1q_u16(&G2log[ 0]);
        uint16x8_t g2_b16 = vld1q_u16(&G2log[ 8]);
#endif
        uint32x4_t g1_sum4u = vaddq_u32(vpaddlq_u16(g1_a16), vpaddlq_u16(g1_b16));
        uint32x4_t g2_sum4u = vaddq_u32(vpaddlq_u16(g2_a16), vpaddlq_u16(g2_b16));
        int32x4_t  g1_sum4  = vreinterpretq_s32_u32(g1_sum4u);
        int32x4_t  g2_sum4  = vreinterpretq_s32_u32(g2_sum4u);
        /* (sum + 2) >> 2 = average of 4 */
        int32x4_t two = vdupq_n_s32(2);
        int32x4_t vg1 = vshrq_n_s32(vaddq_s32(g1_sum4, two), 2);
        int32x4_t vg2 = vshrq_n_s32(vaddq_s32(g2_sum4, two), 2);

        int32x4_t result;
        if (channel == 0) {
            result = vshrq_n_s32(vaddq_s32(vg1, vg2), 1);
        } else if (channel == 3) {
            result = vshrq_n_s32(vaddq_s32(vsubq_s32(vg1, vg2), vmid2), 1);
        } else {
#if defined(FUSED_LOG_POLYNOMIAL)
            uint16x8_t x_a16 = x_a16_poly;
            uint16x8_t x_b16 = x_b16_poly;
#else
            uint16x8_t x_a16 = vld1q_u16(&Xlog[ 0]);
            uint16x8_t x_b16 = vld1q_u16(&Xlog[ 8]);
#endif
            uint32x4_t x_sum4u = vaddq_u32(vpaddlq_u16(x_a16), vpaddlq_u16(x_b16));
            int32x4_t  x_sum4  = vreinterpretq_s32_u32(x_sum4u);
            int32x4_t vx = vshrq_n_s32(vaddq_s32(x_sum4, two), 2);
            int32x4_t vgs = vshrq_n_s32(vaddq_s32(vg1, vg2), 1);
            result = vshrq_n_s32(vaddq_s32(vsubq_s32(vx, vgs), vmid2), 1);
        }
        vst1q_s32(&output[o], result);
    }
#if defined(__aarch64__) && !defined(FUSED_DISABLE_ASM)
_asm_done:;
#endif
#endif
    /* Scalar tail, same log-space averaging semantics. */
    for (; o < ch_width_out; o++) {
        int bc = o * 4;
        /* Pull raw values per color from the 4 rows, 2 cols each. */
        uint16_t R0,R1,R2,R3, G10,G11,G12,G13, G20,G21,G22,G23, B0,B1,B2,B3;
        if (is_rggb) {
            R0=row1a[bc];   R1=row1a[bc+2]; R2=row1b[bc];   R3=row1b[bc+2];
            G10=row1a[bc+1];G11=row1a[bc+3];G12=row1b[bc+1];G13=row1b[bc+3];
            G20=row2a[bc];  G21=row2a[bc+2];G22=row2b[bc];  G23=row2b[bc+2];
            B0=row2a[bc+1]; B1=row2a[bc+3]; B2=row2b[bc+1]; B3=row2b[bc+3];
        } else {
            G10=row1a[bc];  G11=row1a[bc+2];G12=row1b[bc];  G13=row1b[bc+2];
            B0=row1a[bc+1]; B1=row1a[bc+3]; B2=row1b[bc+1]; B3=row1b[bc+3];
            R0=row2a[bc];   R1=row2a[bc+2]; R2=row2b[bc];   R3=row2b[bc+2];
            G20=row2a[bc+1];G21=row2a[bc+3];G22=row2b[bc+1];G23=row2b[bc+3];
        }
        if (R0  > lm) R0  = lm; if (R1  > lm) R1  = lm; if (R2  > lm) R2  = lm; if (R3  > lm) R3  = lm;
        if (G10 > lm) G10 = lm; if (G11 > lm) G11 = lm; if (G12 > lm) G12 = lm; if (G13 > lm) G13 = lm;
        if (G20 > lm) G20 = lm; if (G21 > lm) G21 = lm; if (G22 > lm) G22 = lm; if (G23 > lm) G23 = lm;
        if (B0  > lm) B0  = lm; if (B1  > lm) B1  = lm; if (B2  > lm) B2  = lm; if (B3  > lm) B3  = lm;
        int32_t g1 = ((int32_t)log_tbl[G10] + log_tbl[G11] + log_tbl[G12] + log_tbl[G13] + 2) >> 2;
        int32_t g2 = ((int32_t)log_tbl[G20] + log_tbl[G21] + log_tbl[G22] + log_tbl[G23] + 2) >> 2;
        switch (channel) {
            case 0: output[o] = (g1 + g2) >> 1; break;
            case 1: {
                int32_t r = ((int32_t)log_tbl[R0] + log_tbl[R1] + log_tbl[R2] + log_tbl[R3] + 2) >> 2;
                int32_t gs = (g1 + g2) >> 1;
                output[o] = ((r - gs) + mid2) >> 1;
            } break;
            case 2: {
                int32_t b = ((int32_t)log_tbl[B0] + log_tbl[B1] + log_tbl[B2] + log_tbl[B3] + 2) >> 2;
                int32_t gs = (g1 + g2) >> 1;
                output[o] = ((b - gs) + mid2) >> 1;
            } break;
            case 3: output[o] = ((g1 - g2) + mid2) >> 1; break;
        }
    }
}

/* T15: Fully-fused unpack_row + horizontal_filter_lp_only for the AA-2x2
   decimate path. Reads 4 Bayer rows (2 RGGB pairs), produces ch_width/2
   prescaled+pair-summed LP-only outputs directly to lp_out, completely
   bypassing the intermediate unpack_row buffer.

   On the Pi 5 LL-only-fast hot path the existing AA-2x2 unpack writes
   ~8 KB/row to unpack_row, then horizontal_filter_lp_only reads it and
   writes ~4 KB to lp_out. This fused path saves ~16 KB/row of L1 cache
   traffic per channel = ~24 MB/frame across 4 channels at 50 MP.

   Only used when prescale==2 (hard-coded into the pair-sum/PS path) and
   GPR_DROP_HIGHPASS=1 (no HP band needed). Bit-exact with the two-pass
   path: same arithmetic, same operation ordering.

   Arithmetic per output (i):
     For each of the 2 unpack positions (col=2i, col=2i+1):
       g1 = avg-of-4 G1 log values (row1a,2a,1b,2b at col)
       g2 = avg-of-4 G2 log values
       val = (g1+g2)>>1 (ch 0), or ((g1-g2)+mid2)>>1 (ch 3), or
             ((x-gs)+mid2)>>1 for chroma where x=R/B avg, gs=(g1+g2)>>1
       PS(val) = (val + 3) >> 2
     lp_out[i] = PS(unpack[2i]) + PS(unpack[2i+1])
*/
static void unpack_channel_row_decimate_2x2_fused_lp(
    int channel, int is_rggb,
    const uint16_t *log_tbl, int log_max, int32_t mid2,
    const uint16_t *row1a, const uint16_t *row2a,
    const uint16_t *row1b, const uint16_t *row2b,
    PIXEL *lp_out, int ch_width_out)
{
    /* lp_out length = ch_width_out / 2.  ch_width_out is the original
       unpack width (multiple of 4 enforced by upstream layout). */
    int o = 0;
    const uint16_t lm = (uint16_t)log_max;
    const int half = ch_width_out / 2;
    const int half_m2 = (half / 2) * 2;  /* even half so we can emit 2 LP per iter */

#if ENABLED(NEON)
    /* Each "iter" handles 4 unpack values → 2 LP outputs. Outer i steps by 2
       LP outputs per iter (= 4 unpack outputs = 16 Bayer columns per row). */
    const uint16x8_t v_log_max = vdupq_n_u16(lm);
    const int32x4_t vmid2 = vdupq_n_s32(mid2);
    const int32x4_t vthree = vdupq_n_s32(3);

    for (int i = 0; i < half_m2; i += 2) {
        int u = i * 2;       /* unpack col base (4 unpack outputs from u..u+3) */
        int bc = u * 4;       /* Bayer col base = 16 cols per iter */

        uint16x8x2_t e_a = vld2q_u16(&row1a[bc]);
        uint16x8x2_t o_a = vld2q_u16(&row2a[bc]);
        uint16x8x2_t e_b = vld2q_u16(&row1b[bc]);
        uint16x8x2_t o_b = vld2q_u16(&row2b[bc]);

        uint16x8_t vR_a, vR_b, vG1_a, vG1_b, vG2_a, vG2_b, vB_a, vB_b;
        if (is_rggb) {
            vR_a  = vminq_u16(e_a.val[0], v_log_max);
            vG1_a = vminq_u16(e_a.val[1], v_log_max);
            vG2_a = vminq_u16(o_a.val[0], v_log_max);
            vB_a  = vminq_u16(o_a.val[1], v_log_max);
            vR_b  = vminq_u16(e_b.val[0], v_log_max);
            vG1_b = vminq_u16(e_b.val[1], v_log_max);
            vG2_b = vminq_u16(o_b.val[0], v_log_max);
            vB_b  = vminq_u16(o_b.val[1], v_log_max);
        } else {
            vG1_a = vminq_u16(e_a.val[0], v_log_max);
            vB_a  = vminq_u16(e_a.val[1], v_log_max);
            vR_a  = vminq_u16(o_a.val[0], v_log_max);
            vG2_a = vminq_u16(o_a.val[1], v_log_max);
            vG1_b = vminq_u16(e_b.val[0], v_log_max);
            vB_b  = vminq_u16(e_b.val[1], v_log_max);
            vR_b  = vminq_u16(o_b.val[0], v_log_max);
            vG2_b = vminq_u16(o_b.val[1], v_log_max);
        }

#if defined(FUSED_LOG_POLYNOMIAL)
        /* Polynomial log curve: bypass the scalar LUT gather entirely.
           Produces the same u16 results within <=1 LSB error. */
        (void)log_tbl;
        uint16x8_t g1_a16 = fused_log_curve_neon8_u16(vG1_a, log_max);
        uint16x8_t g1_b16 = fused_log_curve_neon8_u16(vG1_b, log_max);
        uint16x8_t g2_a16 = fused_log_curve_neon8_u16(vG2_a, log_max);
        uint16x8_t g2_b16 = fused_log_curve_neon8_u16(vG2_b, log_max);
        uint16x8_t x_a16_poly = vdupq_n_u16(0), x_b16_poly = vdupq_n_u16(0);
        if (channel == 1) {
            x_a16_poly = fused_log_curve_neon8_u16(vR_a, log_max);
            x_b16_poly = fused_log_curve_neon8_u16(vR_b, log_max);
        } else if (channel == 2) {
            x_a16_poly = fused_log_curve_neon8_u16(vB_a, log_max);
            x_b16_poly = fused_log_curve_neon8_u16(vB_b, log_max);
        }
#else
        uint16_t G1_idx[16], G2_idx[16], R_idx[16], B_idx[16];
        vst1q_u16(&G1_idx[0], vG1_a);
        vst1q_u16(&G1_idx[8], vG1_b);
        vst1q_u16(&G2_idx[0], vG2_a);
        vst1q_u16(&G2_idx[8], vG2_b);
        if (channel == 1) {
            vst1q_u16(&R_idx[0], vR_a);
            vst1q_u16(&R_idx[8], vR_b);
        }
        if (channel == 2) {
            vst1q_u16(&B_idx[0], vB_a);
            vst1q_u16(&B_idx[8], vB_b);
        }

        uint16_t G1log[16], G2log[16], Xlog[16];
        for (int k = 0; k < 16; k++) {
            G1log[k] = log_tbl[G1_idx[k]];
            G2log[k] = log_tbl[G2_idx[k]];
        }
        if (channel == 1 || channel == 2) {
            const uint16_t *idx = (channel == 1) ? R_idx : B_idx;
            for (int k = 0; k < 16; k++) Xlog[k] = log_tbl[idx[k]];
        }

        uint16x8_t g1_a16 = vld1q_u16(&G1log[ 0]);
        uint16x8_t g1_b16 = vld1q_u16(&G1log[ 8]);
        uint16x8_t g2_a16 = vld1q_u16(&G2log[ 0]);
        uint16x8_t g2_b16 = vld1q_u16(&G2log[ 8]);
#endif
        uint32x4_t g1_sum4u = vaddq_u32(vpaddlq_u16(g1_a16), vpaddlq_u16(g1_b16));
        uint32x4_t g2_sum4u = vaddq_u32(vpaddlq_u16(g2_a16), vpaddlq_u16(g2_b16));
        int32x4_t  g1_sum4  = vreinterpretq_s32_u32(g1_sum4u);
        int32x4_t  g2_sum4  = vreinterpretq_s32_u32(g2_sum4u);
        int32x4_t two = vdupq_n_s32(2);
        int32x4_t vg1 = vshrq_n_s32(vaddq_s32(g1_sum4, two), 2);
        int32x4_t vg2 = vshrq_n_s32(vaddq_s32(g2_sum4, two), 2);

        int32x4_t result;
        if (channel == 0) {
            result = vshrq_n_s32(vaddq_s32(vg1, vg2), 1);
        } else if (channel == 3) {
            result = vshrq_n_s32(vaddq_s32(vsubq_s32(vg1, vg2), vmid2), 1);
        } else {
#if defined(FUSED_LOG_POLYNOMIAL)
            uint16x8_t x_a16 = x_a16_poly;
            uint16x8_t x_b16 = x_b16_poly;
#else
            uint16x8_t x_a16 = vld1q_u16(&Xlog[ 0]);
            uint16x8_t x_b16 = vld1q_u16(&Xlog[ 8]);
#endif
            uint32x4_t x_sum4u = vaddq_u32(vpaddlq_u16(x_a16), vpaddlq_u16(x_b16));
            int32x4_t  x_sum4  = vreinterpretq_s32_u32(x_sum4u);
            int32x4_t vx = vshrq_n_s32(vaddq_s32(x_sum4, two), 2);
            int32x4_t vgs = vshrq_n_s32(vaddq_s32(vg1, vg2), 1);
            result = vshrq_n_s32(vaddq_s32(vsubq_s32(vx, vgs), vmid2), 1);
        }

        /* Fused horizontal_filter_lp_only(prescale=2):
             PS(v) = (v + 3) >> 2
             lp_out[i+k] = PS(result[2k]) + PS(result[2k+1])  for k=0,1
           NEON: 4 PS-values → vpaddq_s32 → 2 lp outputs in low half. */
        int32x4_t ps = vshrq_n_s32(vaddq_s32(result, vthree), 2);
        int32x4_t pairs = vpaddq_s32(ps, ps);  /* lanes 0,1 hold lp[i], lp[i+1] */
        vst1_s32(&lp_out[i], vget_low_s32(pairs));
        (void)o;
    }
    o = half_m2 * 2;  /* tail starts at unpack col = remaining LPs * 2 */
#endif

    /* Scalar tail for any remaining LP outputs (covers half-half_m2 LPs, each
       reading 2 unpack columns, each unpack col reading 4 same-color samples
       from the 2 RGGB pairs). */
    for (int i = (half_m2); i < half; i++) {
        int u_base = i * 2;
        int32_t ps_pair[2];
        for (int p = 0; p < 2; p++) {
            int u = u_base + p;
            int bc = u * 4;
            uint16_t R0,R1,R2,R3, G10,G11,G12,G13, G20,G21,G22,G23, B0,B1,B2,B3;
            if (is_rggb) {
                R0=row1a[bc];   R1=row1a[bc+2]; R2=row1b[bc];   R3=row1b[bc+2];
                G10=row1a[bc+1];G11=row1a[bc+3];G12=row1b[bc+1];G13=row1b[bc+3];
                G20=row2a[bc];  G21=row2a[bc+2];G22=row2b[bc];  G23=row2b[bc+2];
                B0=row2a[bc+1]; B1=row2a[bc+3]; B2=row2b[bc+1]; B3=row2b[bc+3];
            } else {
                G10=row1a[bc];  G11=row1a[bc+2];G12=row1b[bc];  G13=row1b[bc+2];
                B0=row1a[bc+1]; B1=row1a[bc+3]; B2=row1b[bc+1]; B3=row1b[bc+3];
                R0=row2a[bc];   R1=row2a[bc+2]; R2=row2b[bc];   R3=row2b[bc+2];
                G20=row2a[bc+1];G21=row2a[bc+3];G22=row2b[bc+1];G23=row2b[bc+3];
            }
            if (R0  > lm) R0  = lm; if (R1  > lm) R1  = lm; if (R2  > lm) R2  = lm; if (R3  > lm) R3  = lm;
            if (G10 > lm) G10 = lm; if (G11 > lm) G11 = lm; if (G12 > lm) G12 = lm; if (G13 > lm) G13 = lm;
            if (G20 > lm) G20 = lm; if (G21 > lm) G21 = lm; if (G22 > lm) G22 = lm; if (G23 > lm) G23 = lm;
            if (B0  > lm) B0  = lm; if (B1  > lm) B1  = lm; if (B2  > lm) B2  = lm; if (B3  > lm) B3  = lm;
            int32_t g1 = ((int32_t)log_tbl[G10] + log_tbl[G11] + log_tbl[G12] + log_tbl[G13] + 2) >> 2;
            int32_t g2 = ((int32_t)log_tbl[G20] + log_tbl[G21] + log_tbl[G22] + log_tbl[G23] + 2) >> 2;
            int32_t val;
            switch (channel) {
                case 0: val = (g1 + g2) >> 1; break;
                case 1: {
                    int32_t r = ((int32_t)log_tbl[R0] + log_tbl[R1] + log_tbl[R2] + log_tbl[R3] + 2) >> 2;
                    int32_t gs = (g1 + g2) >> 1;
                    val = ((r - gs) + mid2) >> 1;
                } break;
                case 2: {
                    int32_t b = ((int32_t)log_tbl[B0] + log_tbl[B1] + log_tbl[B2] + log_tbl[B3] + 2) >> 2;
                    int32_t gs = (g1 + g2) >> 1;
                    val = ((b - gs) + mid2) >> 1;
                } break;
                default: val = ((g1 - g2) + mid2) >> 1; break;  /* ch 3 */
            }
            ps_pair[p] = (val + 3) >> 2;
        }
        lp_out[i] = ps_pair[0] + ps_pair[1];
    }
}

/* T16: chroma col-decimate + fused horizontal-LP. Combines T13b with the
   prescale=2 pair-sum from T15: walks 2 Bayer rows, produces ch_width/2
   prescaled+pair-summed LP outputs directly to lp_out, skipping the
   unpack_row intermediate buffer entirely.

   Saves ~12 MB/frame across 2 chroma channels on the AA_LUMA_ONLY=1 +
   DROP_HIGHPASS path. Bit-exact with chroma T13b + horizontal_filter_lp_only.
*/
static void unpack_channel_row_col_decimate_2x1_fused_lp(
    int channel, int is_rggb,
    const uint16_t *log_tbl, int log_max, int32_t mid2,
    const uint16_t *row1, const uint16_t *row2,
    PIXEL *lp_out, int ch_width_out)
{
    int o = 0;
    const uint16_t lm = (uint16_t)log_max;
    const int half = ch_width_out / 2;
    const int half_m2 = (half / 2) * 2;

#if ENABLED(NEON)
    if (ch_width_out >= 8) {
        __builtin_prefetch(&row1[ch_width_out * 4 - 32], 0, 3);
        __builtin_prefetch(&row2[ch_width_out * 4 - 32], 0, 3);
    }

    const uint16x8_t v_log_max = vdupq_n_u16(lm);
    const int32x4_t vmid2 = vdupq_n_s32(mid2);
    const int32x4_t vthree = vdupq_n_s32(3);

    /* Each iter handles 4 unpack outputs from 8 same-color samples
       (col_decimate=2 means 2 unpack outputs per 4 Bayer cols of same color).
       That produces 2 LP outputs (i, i+1). half_m2 must be a multiple of 2 — half
       is rarely odd, but we use even-aligned bound and let scalar tail clean up. */
    for (int i = 0; i < half_m2; i += 2) {
        int u = i * 2;       /* 4 unpack outputs starting at u */
        int bc = u * 2;       /* Bayer col base; col_decimate=2 → 8 cols per 4 outputs */

        uint16x8x2_t r1d = vld2q_u16(&row1[2 * bc]);
        uint16x8x2_t r2d = vld2q_u16(&row2[2 * bc]);

        uint16x8_t vR, vG1, vG2, vB;
        if (is_rggb) {
            vR  = vminq_u16(r1d.val[0], v_log_max);
            vG1 = vminq_u16(r1d.val[1], v_log_max);
            vG2 = vminq_u16(r2d.val[0], v_log_max);
            vB  = vminq_u16(r2d.val[1], v_log_max);
        } else {
            vG1 = vminq_u16(r1d.val[0], v_log_max);
            vB  = vminq_u16(r1d.val[1], v_log_max);
            vR  = vminq_u16(r2d.val[0], v_log_max);
            vG2 = vminq_u16(r2d.val[1], v_log_max);
        }

#if defined(FUSED_LOG_POLYNOMIAL)
        /* Polynomial path: produce 8-wide u16 log values directly, then
           split into 2 × s32 4-wide tiles for the existing arithmetic. */
        (void)log_tbl;
        uint16x8_t vG1log = fused_log_curve_neon8_u16(vG1, log_max);
        uint16x8_t vG2log = fused_log_curve_neon8_u16(vG2, log_max);
        uint16x8_t vXlog  = fused_log_curve_neon8_u16((channel == 1) ? vR : vB, log_max);
        int32x4_t G1v_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vG1log)));
        int32x4_t G1v_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(vG1log)));
        int32x4_t G2v_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vG2log)));
        int32x4_t G2v_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(vG2log)));
        int32x4_t Xv_lo  = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vXlog)));
        int32x4_t Xv_hi  = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(vXlog)));

        int32x4_t val_lo, val_hi;
        {
            int32x4_t vgs = vshrq_n_s32(vaddq_s32(G1v_lo, G2v_lo), 1);
            val_lo = vshrq_n_s32(vaddq_s32(vsubq_s32(Xv_lo, vgs), vmid2), 1);
        }
        {
            int32x4_t vgs = vshrq_n_s32(vaddq_s32(G1v_hi, G2v_hi), 1);
            val_hi = vshrq_n_s32(vaddq_s32(vsubq_s32(Xv_hi, vgs), vmid2), 1);
        }
#else
        uint16_t Xs[8], G1s[8], G2s[8];
        vst1q_u16(G1s, vG1);
        vst1q_u16(G2s, vG2);
        if (channel == 1) vst1q_u16(Xs, vR);
        else              vst1q_u16(Xs, vB);

        uint16_t Xlog[8], G1log[8], G2log[8];
        for (int k = 0; k < 8; k++) {
            Xlog[k]  = log_tbl[Xs[k]];
            G1log[k] = log_tbl[G1s[k]];
            G2log[k] = log_tbl[G2s[k]];
        }

        /* Two 4-wide tiles → 8 intermediates → pair-avg + prescale + pair-sum
           gives 2 LP outs. The intra-tile pair-avg uses vuzp1q/vuzp2q like T13b. */
        uint16x8_t xlog8 = vld1q_u16(Xlog);
        uint16x8_t g1log8 = vld1q_u16(G1log);
        uint16x8_t g2log8 = vld1q_u16(G2log);
        int32x4_t x_lo  = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(xlog8)));
        int32x4_t x_hi  = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(xlog8)));
        int32x4_t g1_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(g1log8)));
        int32x4_t g1_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(g1log8)));
        int32x4_t g2_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(g2log8)));
        int32x4_t g2_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(g2log8)));
        int32x4_t gs_lo = vshrq_n_s32(vaddq_s32(g1_lo, g2_lo), 1);
        int32x4_t gs_hi = vshrq_n_s32(vaddq_s32(g1_hi, g2_hi), 1);
        int32x4_t val_lo = vshrq_n_s32(vaddq_s32(vsubq_s32(x_lo, gs_lo), vmid2), 1);
        int32x4_t val_hi = vshrq_n_s32(vaddq_s32(vsubq_s32(x_hi, gs_hi), vmid2), 1);
#endif
        /* Pair-avg across val_lo/val_hi → 4 unpack outputs in `avg`. */
        int32x4_t e = vuzp1q_s32(val_lo, val_hi);
        int32x4_t d = vuzp2q_s32(val_lo, val_hi);
        int32x4_t avg = vshrq_n_s32(vaddq_s32(e, d), 1);
        /* Prescale + pair-sum → 2 LP outputs in low half. */
        int32x4_t ps = vshrq_n_s32(vaddq_s32(avg, vthree), 2);
        int32x4_t pairs = vpaddq_s32(ps, ps);
        vst1_s32(&lp_out[i], vget_low_s32(pairs));
        (void)o;
    }
    o = half_m2 * 2;
#endif

    /* Scalar tail. */
    for (int i = half_m2; i < half; i++) {
        int u_base = i * 2;
        int32_t ps_pair[2];
        for (int p = 0; p < 2; p++) {
            int u = u_base + p;
            /* Each u reads 2 Bayer pair-cols (= 4 same-color samples). */
            int32_t s0 = 0, s1 = 0;
            for (int pp = 0; pp < 2; pp++) {
                int c = u * 2 + pp;
                uint16_t Rv, G1v, G2v, Bv;
                if (is_rggb) {
                    Rv = row1[2*c];   G1v = row1[2*c+1];
                    G2v = row2[2*c];  Bv  = row2[2*c+1];
                } else {
                    G1v = row1[2*c];  Bv  = row1[2*c+1];
                    Rv  = row2[2*c];  G2v = row2[2*c+1];
                }
                if (Rv > lm) Rv = lm; if (G1v > lm) G1v = lm;
                if (G2v > lm) G2v = lm; if (Bv > lm) Bv = lm;
                int32_t r  = log_tbl[Rv];
                int32_t g1 = log_tbl[G1v];
                int32_t g2 = log_tbl[G2v];
                int32_t b  = log_tbl[Bv];
                int32_t gs = (g1 + g2) >> 1;
                int32_t val;
                if (channel == 1) val = ((r - gs) + mid2) >> 1;
                else              val = ((b - gs) + mid2) >> 1;
                if (pp == 0) s0 = val; else s1 = val;
            }
            int32_t unpack_val = (s0 + s1) >> 1;
            ps_pair[p] = (unpack_val + 3) >> 2;
        }
        lp_out[i] = ps_pair[0] + ps_pair[1];
    }
}

/* T13b: fused col-decimate variant of unpack_channel_row for chroma. Reads
   2 Bayer rows, produces ch_width=ch_width_full/2 outputs (each = average of
   two adjacent same-channel samples). This replaces the unpack_channel_row →
   unpack_full → pair-avg → unpack_row two-pass dance in mode-1, eliminating
   ~64 KB/row of intermediate buffer traffic. Bit-exact with the two-pass
   path (same arithmetic semantics: log lookup → arithmetic → pair-avg).
   Only used for chroma (channel 1, 2) since luma has the AA-2x2 path. */
static void unpack_channel_row_col_decimate_2x1(
    int channel, int is_rggb,
    const uint16_t *log_tbl, int log_max, int32_t mid2,
    const uint16_t *row1, const uint16_t *row2,
    PIXEL *output, int ch_width_out)
{
    int o = 0;
    const uint16_t lm = (uint16_t)log_max;

#if ENABLED(NEON)
    if (ch_width_out >= 8) {
        __builtin_prefetch(&row1[ch_width_out * 4 - 32], 0, 3);
        __builtin_prefetch(&row2[ch_width_out * 4 - 32], 0, 3);
    }

    const int o_m4 = (ch_width_out / 4) * 4;
    const uint16x8_t v_log_max = vdupq_n_u16(lm);
    const int32x4_t vmid2 = vdupq_n_s32(mid2);

    /* Per iter: 4 output samples = 8 same-color samples from 2 Bayer rows.
       Each Bayer row supplies 16 u16s (8 R + 8 G1 from row1, etc).
       After log lookup and color arithmetic, pair-avg adjacent samples. */
    for (; o < o_m4; o += 4) {
        int bc = o * 2;  /* starting Bayer column (col_decimate=2 means 8 Bayer cols per 4 outs) */

        /* Load 16 u16s from each row → deinterleaved into 8 R + 8 G1 (or G2/B). */
        uint16x8x2_t r1d = vld2q_u16(&row1[2 * bc]);
        uint16x8x2_t r2d = vld2q_u16(&row2[2 * bc]);

        uint16x8_t vR, vG1, vG2, vB;
        if (is_rggb) {
            vR  = vminq_u16(r1d.val[0], v_log_max);
            vG1 = vminq_u16(r1d.val[1], v_log_max);
            vG2 = vminq_u16(r2d.val[0], v_log_max);
            vB  = vminq_u16(r2d.val[1], v_log_max);
        } else {
            vG1 = vminq_u16(r1d.val[0], v_log_max);
            vB  = vminq_u16(r1d.val[1], v_log_max);
            vR  = vminq_u16(r2d.val[0], v_log_max);
            vG2 = vminq_u16(r2d.val[1], v_log_max);
        }

        /* For chroma (ch 1: RG, ch 2: BG): need R or B and G1+G2.
           Spill, lookup, recombine. */
        uint16_t Xs[8], G1s[8], G2s[8];
        vst1q_u16(G1s, vG1);
        vst1q_u16(G2s, vG2);
        if (channel == 1) vst1q_u16(Xs, vR);
        else              vst1q_u16(Xs, vB);

        /* 8 LUT lookups each for X/G1/G2; the log curve fits in u16, so keep
           scratch at half width and widen directly into NEON lanes. This is
           bit-exact with the prior int32 scratch path but cuts stack
           store/load traffic in the active half-res col-decimate kernel. */
        uint16_t Xlog[8], G1log[8], G2log[8];
        for (int k = 0; k < 8; k++) {
            Xlog[k]  = log_tbl[Xs[k]];
            G1log[k] = log_tbl[G1s[k]];
            G2log[k] = log_tbl[G2s[k]];
        }

        uint16x8_t xlog8 = vld1q_u16(Xlog);
        uint16x8_t g1log8 = vld1q_u16(G1log);
        uint16x8_t g2log8 = vld1q_u16(G2log);
        int32x4_t x_lo  = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(xlog8)));
        int32x4_t x_hi  = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(xlog8)));
        int32x4_t g1_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(g1log8)));
        int32x4_t g1_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(g1log8)));
        int32x4_t g2_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(g2log8)));
        int32x4_t g2_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(g2log8)));

        int32x4_t gs_lo = vshrq_n_s32(vaddq_s32(g1_lo, g2_lo), 1);
        int32x4_t gs_hi = vshrq_n_s32(vaddq_s32(g1_hi, g2_hi), 1);
        int32x4_t val_lo = vshrq_n_s32(vaddq_s32(vsubq_s32(x_lo, gs_lo), vmid2), 1);
        int32x4_t val_hi = vshrq_n_s32(vaddq_s32(vsubq_s32(x_hi, gs_hi), vmid2), 1);

        int32x4_t e = vuzp1q_s32(val_lo, val_hi);
        int32x4_t d = vuzp2q_s32(val_lo, val_hi);
        int32x4_t avg = vshrq_n_s32(vaddq_s32(e, d), 1);
        vst1q_s32(&output[o], avg);
    }
#endif

    /* Scalar tail. */
    for (; o < ch_width_out; o++) {
        int bc = o * 2;  /* 2 input pairs per output */
        int32_t s0 = 0, s1 = 0;
        for (int p = 0; p < 2; p++) {
            int c = bc + p;
            uint16_t Rv, G1v, G2v, Bv;
            if (is_rggb) {
                Rv = row1[2*c];   G1v = row1[2*c+1];
                G2v = row2[2*c];  Bv  = row2[2*c+1];
            } else {
                G1v = row1[2*c];  Bv  = row1[2*c+1];
                Rv  = row2[2*c];  G2v = row2[2*c+1];
            }
            if (Rv > lm) Rv = lm; if (G1v > lm) G1v = lm;
            if (G2v > lm) G2v = lm; if (Bv > lm) Bv = lm;
            int32_t r  = log_tbl[Rv];
            int32_t g1 = log_tbl[G1v];
            int32_t g2 = log_tbl[G2v];
            int32_t b  = log_tbl[Bv];
            int32_t gs = (g1 + g2) >> 1;
            int32_t val;
            if (channel == 1) val = ((r - gs) + mid2) >> 1;
            else              val = ((b - gs) + mid2) >> 1;
            if (p == 0) s0 = val; else s1 = val;
        }
        output[o] = (s0 + s1) >> 1;
    }
}

/* T16-luma: luma col-decimate + fused horizontal-LP. Like T14 but writes
   directly to lp_out (prescale=2 + pair-sum applied to pair-averaged outputs). */
static void unpack_luma_row_col_decimate_2x1_fused_lp(
    int channel, int is_rggb,
    const uint16_t *log_tbl, int log_max, int32_t mid2,
    const uint16_t *row1, const uint16_t *row2,
    PIXEL *lp_out, int ch_width_out)
{
    int o = 0;
    const uint16_t lm = (uint16_t)log_max;
    const int half = ch_width_out / 2;
    const int half_m2 = (half / 2) * 2;

#if ENABLED(NEON)
    if (ch_width_out >= 8) {
        __builtin_prefetch(&row1[ch_width_out * 4 - 32], 0, 3);
        __builtin_prefetch(&row2[ch_width_out * 4 - 32], 0, 3);
    }

    const uint16x8_t v_log_max = vdupq_n_u16(lm);
    const int32x4_t vmid2 = vdupq_n_s32(mid2);
    const int32x4_t vthree = vdupq_n_s32(3);

    for (int i = 0; i < half_m2; i += 2) {
        int u = i * 2;
        int bc = u * 2;

        uint16x8x2_t r1d = vld2q_u16(&row1[2 * bc]);
        uint16x8x2_t r2d = vld2q_u16(&row2[2 * bc]);

        uint16x8_t vG1, vG2;
        if (is_rggb) {
            vG1 = vminq_u16(r1d.val[1], v_log_max);
            vG2 = vminq_u16(r2d.val[0], v_log_max);
        } else {
            vG1 = vminq_u16(r1d.val[0], v_log_max);
            vG2 = vminq_u16(r2d.val[1], v_log_max);
        }

        uint16_t G1s[8], G2s[8];
        vst1q_u16(G1s, vG1);
        vst1q_u16(G2s, vG2);
        uint16_t G1log[8], G2log[8];
        for (int k = 0; k < 8; k++) {
            G1log[k] = log_tbl[G1s[k]];
            G2log[k] = log_tbl[G2s[k]];
        }

        uint16x8_t g1log8 = vld1q_u16(G1log);
        uint16x8_t g2log8 = vld1q_u16(G2log);
        int32x4_t g1_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(g1log8)));
        int32x4_t g1_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(g1log8)));
        int32x4_t g2_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(g2log8)));
        int32x4_t g2_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(g2log8)));
        int32x4_t val_lo, val_hi;
        if (channel == 0) {
            val_lo = vshrq_n_s32(vaddq_s32(g1_lo, g2_lo), 1);
            val_hi = vshrq_n_s32(vaddq_s32(g1_hi, g2_hi), 1);
        } else {
            val_lo = vshrq_n_s32(vaddq_s32(vsubq_s32(g1_lo, g2_lo), vmid2), 1);
            val_hi = vshrq_n_s32(vaddq_s32(vsubq_s32(g1_hi, g2_hi), vmid2), 1);
        }
        int32x4_t e = vuzp1q_s32(val_lo, val_hi);
        int32x4_t d = vuzp2q_s32(val_lo, val_hi);
        int32x4_t avg = vshrq_n_s32(vaddq_s32(e, d), 1);
        int32x4_t ps = vshrq_n_s32(vaddq_s32(avg, vthree), 2);
        int32x4_t pairs = vpaddq_s32(ps, ps);
        vst1_s32(&lp_out[i], vget_low_s32(pairs));
        (void)o;
    }
    o = half_m2 * 2;
#endif

    for (int i = half_m2; i < half; i++) {
        int u_base = i * 2;
        int32_t ps_pair[2];
        for (int p = 0; p < 2; p++) {
            int u = u_base + p;
            int32_t s0 = 0, s1 = 0;
            for (int pp = 0; pp < 2; pp++) {
                int c = u * 2 + pp;
                uint16_t G1v, G2v;
                if (is_rggb) {
                    G1v = row1[2*c+1];
                    G2v = row2[2*c];
                } else {
                    G1v = row1[2*c];
                    G2v = row2[2*c+1];
                }
                if (G1v > lm) G1v = lm;
                if (G2v > lm) G2v = lm;
                int32_t g1 = log_tbl[G1v];
                int32_t g2 = log_tbl[G2v];
                int32_t val;
                if (channel == 0) val = (g1 + g2) >> 1;
                else              val = ((g1 - g2) + mid2) >> 1;
                if (pp == 0) s0 = val; else s1 = val;
            }
            int32_t unpack_val = (s0 + s1) >> 1;
            ps_pair[p] = (unpack_val + 3) >> 2;
        }
        lp_out[i] = ps_pair[0] + ps_pair[1];
    }
}

/* T14: fused col-decimate variant of unpack_channel_row for LUMA (ch 0=GS, 3=GD).
   Reads 2 Bayer rows, produces ch_width=ch_width_full/2 outputs (each = average
   of two adjacent same-channel samples after the per-pixel GS/GD arithmetic).
   This replaces the unpack_channel_row → unpack_full → NEON pair-avg → unpack_row
   two-pass dance for the luma channels when col_decimate=2 (no row_decimate).
   Cuts ~32 KB/row of intermediate buffer traffic per luma channel, ~24 MB/frame
   per channel × 2 luma channels = ~48 MB/frame of L1/L2 evictions saved on the
   LL-only-fast path. Bit-exact with the two-pass path. */
static void unpack_luma_row_col_decimate_2x1(
    int channel, int is_rggb,
    const uint16_t *log_tbl, int log_max, int32_t mid2,
    const uint16_t *row1, const uint16_t *row2,
    PIXEL *output, int ch_width_out)
{
    int o = 0;
    const uint16_t lm = (uint16_t)log_max;

#if ENABLED(NEON)
    if (ch_width_out >= 8) {
        __builtin_prefetch(&row1[ch_width_out * 4 - 32], 0, 3);
        __builtin_prefetch(&row2[ch_width_out * 4 - 32], 0, 3);
    }

    const int o_m4 = (ch_width_out / 4) * 4;
    const uint16x8_t v_log_max = vdupq_n_u16(lm);
    const int32x4_t vmid2 = vdupq_n_s32(mid2);

    /* Per iter: 4 output samples = 8 same-color samples (G1 + G2) from 2 Bayer rows.
       Each Bayer row supplies 16 u16s (8 R + 8 G1 from row1, 8 G2 + 8 B from row2). */
    for (; o < o_m4; o += 4) {
        int bc = o * 2;  /* starting Bayer column */

        uint16x8x2_t r1d = vld2q_u16(&row1[2 * bc]);
        uint16x8x2_t r2d = vld2q_u16(&row2[2 * bc]);

        uint16x8_t vG1, vG2;
        if (is_rggb) {
            /* row1 = R G1 R G1 ... ; row2 = G2 B G2 B ... */
            vG1 = vminq_u16(r1d.val[1], v_log_max);
            vG2 = vminq_u16(r2d.val[0], v_log_max);
        } else {
            /* row1 = G1 B G1 B ... ; row2 = R G2 R G2 ... */
            vG1 = vminq_u16(r1d.val[0], v_log_max);
            vG2 = vminq_u16(r2d.val[1], v_log_max);
        }

        /* Luma only needs G1, G2 log lookups. 8 entries each, then NEON
           arithmetic produces 8 per-column intermediates which we pair-avg
           into 4 outputs. */
        uint16_t G1s[8], G2s[8];
        vst1q_u16(G1s, vG1);
        vst1q_u16(G2s, vG2);

        uint16_t G1log[8], G2log[8];
        for (int k = 0; k < 8; k++) {
            G1log[k] = log_tbl[G1s[k]];
            G2log[k] = log_tbl[G2s[k]];
        }

        uint16x8_t g1log8 = vld1q_u16(G1log);
        uint16x8_t g2log8 = vld1q_u16(G2log);
        int32x4_t g1_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(g1log8)));
        int32x4_t g1_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(g1log8)));
        int32x4_t g2_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(g2log8)));
        int32x4_t g2_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(g2log8)));

        int32x4_t val_lo, val_hi;
        if (channel == 0) {
            val_lo = vshrq_n_s32(vaddq_s32(g1_lo, g2_lo), 1);
            val_hi = vshrq_n_s32(vaddq_s32(g1_hi, g2_hi), 1);
        } else {  /* channel == 3 */
            val_lo = vshrq_n_s32(vaddq_s32(vsubq_s32(g1_lo, g2_lo), vmid2), 1);
            val_hi = vshrq_n_s32(vaddq_s32(vsubq_s32(g1_hi, g2_hi), vmid2), 1);
        }
        int32x4_t e = vuzp1q_s32(val_lo, val_hi);
        int32x4_t d = vuzp2q_s32(val_lo, val_hi);
        int32x4_t avg = vshrq_n_s32(vaddq_s32(e, d), 1);
        vst1q_s32(&output[o], avg);
    }
#endif

    /* Scalar tail. */
    for (; o < ch_width_out; o++) {
        int bc = o * 2;
        int32_t s0 = 0, s1 = 0;
        for (int p = 0; p < 2; p++) {
            int c = bc + p;
            uint16_t G1v, G2v;
            if (is_rggb) {
                G1v = row1[2*c+1];
                G2v = row2[2*c];
            } else {
                G1v = row1[2*c];
                G2v = row2[2*c+1];
            }
            if (G1v > lm) G1v = lm;
            if (G2v > lm) G2v = lm;
            int32_t g1 = log_tbl[G1v];
            int32_t g2 = log_tbl[G2v];
            int32_t val;
            if (channel == 0) val = (g1 + g2) >> 1;
            else              val = ((g1 - g2) + mid2) >> 1;
            if (p == 0) s0 = val; else s1 = val;
        }
        output[o] = (s0 + s1) >> 1;
    }
}

/* Streaming cascade: feed one newly-produced LL1 row into level-2's
   horizontal filter, then (if enough rows are queued) run level-2's
   vertical filter, then cascade the LL2 row into level-3's horizontal
   filter, then (if enough rows) run level-3's vertical filter.

   This eliminates the LL1 and LL2 full-image buffers (saves ~57 MB at
   50 MP), keeping only the small 6-row horizontal buffers per level. */
/* Env: FUSED_L2_PREFILTER selects an anti-alias LP on the LL1 stream
   before the L2 wavelet sees it. Values:
     0 (default)  — off
     3 (h-only)   — 3-tap [1,2,1]/4 horizontal LP
     33 (hv)      — 3-tap LP in both X and Y directions
   The vertical LP needs a 2-row history per channel; storage allocated
   on demand. Off-default since this is lossy by design. */
#define FUSED_L2_PREFILTER_MAX_W 8192
static int fused_l2_prefilter_taps(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    const char *e = getenv("FUSED_L2_PREFILTER");
    if (!e || !*e) { cached = 0; return 0; }
    int v = atoi(e);
    cached = (v == 3 || v == 33) ? v : 0;
    return cached;
}

static void stream_cascade_higher_levels(FUSED_CHANNEL_STATE *cs,
                                         const PIXEL *ll1_row)
{
    int bw_l2 = cs->band_width_l2;
    int bw_l3 = cs->band_width_l3;

    int prefilter_mode = fused_l2_prefilter_taps();
    PIXEL prefilter_scratch[FUSED_L2_PREFILTER_MAX_W];
    PIXEL vfilter_scratch[FUSED_L2_PREFILTER_MAX_W];
    const PIXEL *l2_input = ll1_row;
    int bw = cs->band_width;

    if (prefilter_mode != 0 && bw <= FUSED_L2_PREFILTER_MAX_W && bw >= 3) {
        const PIXEL *vfilt = ll1_row;
        /* Vertical 3-tap [1,2,1]/4 LP on the LL1 row stream. */
        if (prefilter_mode == 33) {
            if (cs->ll1_prev1 == NULL) {
                cs->ll1_prev1 = (PIXEL *)fused_aligned_calloc(bw, sizeof(PIXEL));
                cs->ll1_prev2 = (PIXEL *)fused_aligned_calloc(bw, sizeof(PIXEL));
                cs->ll1_history_count = 0;
            }
            if (cs->ll1_history_count >= 2) {
                /* Compute LP on (prev2, prev1, current). Output corresponds to
                   the prev1 row's time index — i.e. we emit a 1-row-delayed
                   smoothed value. The L2 cascade sees fewer rows by 1; the
                   bottom-edge tail handler picks up the missing rows. */
                for (int i = 0; i < bw; i++) {
                    vfilter_scratch[i] = (PIXEL)((cs->ll1_prev2[i]
                                                  + 2 * cs->ll1_prev1[i]
                                                  + ll1_row[i] + 2) >> 2);
                }
                vfilt = vfilter_scratch;
            } else {
                /* Warm-up: not enough history yet — pass through unfiltered. */
                vfilt = ll1_row;
            }
            /* Shift history */
            if (cs->ll1_history_count >= 1) {
                memcpy(cs->ll1_prev2, cs->ll1_prev1, bw * sizeof(PIXEL));
            }
            memcpy(cs->ll1_prev1, ll1_row, bw * sizeof(PIXEL));
            if (cs->ll1_history_count < 2) cs->ll1_history_count++;
        }
        /* Horizontal 3-tap LP on whichever row we'll feed the L2 horizontal_filter. */
        prefilter_scratch[0] = vfilt[0];
        for (int i = 1; i < bw - 1; i++) {
            prefilter_scratch[i] = (PIXEL)((vfilt[i-1] + 2*vfilt[i] + vfilt[i+1] + 2) >> 2);
        }
        prefilter_scratch[bw-1] = vfilt[bw-1];
        l2_input = prefilter_scratch;
    }

    /* ---- Level 2 horizontal filter ---- */
    int slot2 = cs->buf_row_l2 % FUSED_ROW_BUFS;
    horizontal_filter(l2_input,
                      cs->lp_buf_l2[slot2], cs->hp_buf_l2[slot2],
                      cs->band_width, /*prescale=*/2);
    cs->buf_row_l2++;

    /* ---- Level 2 vertical filter ----
       First trigger emits TWO L2 rows from the same 6-row window (top
       boundary + first interior); subsequent triggers emit one row each.
       Each L2 row cascades into the L3 horizontal+vertical pipeline. */
    if (cs->buf_row_l2 >= 6 && (cs->buf_row_l2 % 2) == 0) {
        int n_emits_l2 = (cs->band_out_row_l2 == 0) ? 2 : 1;
        int base = (cs->buf_row_l2 - 6) % FUSED_ROW_BUFS;
        if (base < 0) base += FUSED_ROW_BUFS;
        PIXEL *lp_rows[6], *hp_rows[6];
        for (int r = 0; r < 6; r++) {
            int idx = (base + r) % FUSED_ROW_BUFS;
            lp_rows[r] = cs->lp_buf_l2[idx];
            hp_rows[r] = cs->hp_buf_l2[idx];
        }

        for (int e2 = 0; e2 < n_emits_l2; e2++) {
            int out_row_l2 = cs->band_out_row_l2;
            /* Allow writing up to band_height_l2 + 4 (extra scratch rows). */
            if (out_row_l2 >= cs->band_height_l2 + 4) break;
            int is_top = (out_row_l2 == 0);
            int is_bottom = (out_row_l2 == cs->band_height_l2 - 1);

            /* In 2-level mode (cs->levels==2): write LL2 directly to the
               output band buffer (band_data_l2[0]) — LL2 IS the deepest
               emitted band. In 3-level mode (cs->levels==3): keep LL2 in
               row scratch so it can cascade into L3. */
            PIXEL *ll2 = (cs->levels == 2)
                       ? (cs->band_data_l2[0] + out_row_l2 * bw_l2)
                       : cs->ll2_row_scratch;
            PIXEL *lh2 = cs->band_data_l2[1] + out_row_l2 * bw_l2;
            PIXEL *hl2 = cs->band_data_l2[2] + out_row_l2 * bw_l2;
            PIXEL *hh2 = cs->band_data_l2[3] + out_row_l2 * bw_l2;

            vertical_filter_quantize_row(lp_rows, bw_l2,
                cs->midpoint_l2[0], cs->multiplier_l2[0],
                cs->midpoint_l2[2], cs->multiplier_l2[2],
                0,0, 0,0,
                ll2, hl2, NULL, NULL,
                is_top, is_bottom);

            vertical_filter_quantize_row(hp_rows, bw_l2,
                cs->midpoint_l2[1], cs->multiplier_l2[1],
                cs->midpoint_l2[3], cs->multiplier_l2[3],
                0,0, 0,0,
                lh2, hh2, NULL, NULL,
                is_top, is_bottom);

            cs->band_out_row_l2++;

            /* L3 cascade only runs when cs->levels==3. 2-level stops here. */
            if (cs->levels != 3) continue;

            /* ---- Cascade the LL2 row into level 3 ---- */
            int slot3 = cs->buf_row_l3 % FUSED_ROW_BUFS;
            horizontal_filter(ll2,
                              cs->lp_buf_l3[slot3], cs->hp_buf_l3[slot3],
                              bw_l2, /*prescale=*/2);
            cs->buf_row_l3++;

            /* ---- Level 3 vertical filter (same dual-emit-on-first pattern) ---- */
            if (cs->buf_row_l3 >= 6 && (cs->buf_row_l3 % 2) == 0) {
                int n_emits_l3 = (cs->band_out_row_l3 == 0) ? 2 : 1;
                int base3 = (cs->buf_row_l3 - 6) % FUSED_ROW_BUFS;
                if (base3 < 0) base3 += FUSED_ROW_BUFS;
                PIXEL *lp_rows3[6], *hp_rows3[6];
                for (int r = 0; r < 6; r++) {
                    int idx = (base3 + r) % FUSED_ROW_BUFS;
                    lp_rows3[r] = cs->lp_buf_l3[idx];
                    hp_rows3[r] = cs->hp_buf_l3[idx];
                }
                for (int e3 = 0; e3 < n_emits_l3; e3++) {
                    int out_row_l3 = cs->band_out_row_l3;
                    if (out_row_l3 >= cs->band_height_l3 + 4) break;
                    int is_top3 = (out_row_l3 == 0);
                    int is_bottom3 = (out_row_l3 == cs->band_height_l3 - 1);

                    PIXEL *ll3 = cs->band_data_l3[0] + out_row_l3 * bw_l3;
                    PIXEL *lh3 = cs->band_data_l3[1] + out_row_l3 * bw_l3;
                    PIXEL *hl3 = cs->band_data_l3[2] + out_row_l3 * bw_l3;
                    PIXEL *hh3 = cs->band_data_l3[3] + out_row_l3 * bw_l3;

                    vertical_filter_quantize_row(lp_rows3, bw_l3,
                        cs->midpoint_l3[0], cs->multiplier_l3[0],
                        cs->midpoint_l3[2], cs->multiplier_l3[2],
                        0,0, 0,0,
                        ll3, hl3, NULL, NULL,
                        is_top3, is_bottom3);

                    vertical_filter_quantize_row(hp_rows3, bw_l3,
                        cs->midpoint_l3[1], cs->multiplier_l3[1],
                        cs->midpoint_l3[3], cs->multiplier_l3[3],
                        0,0, 0,0,
                        lh3, hh3, NULL, NULL,
                        is_top3, is_bottom3);

                    cs->band_out_row_l3++;
                }
            }
        }
    }
}

/* Run the entire Pass 1 pipeline for a single channel: unpack → horiz → vert+quant → freq.
   Each invocation owns its 6-row buffer and freq tables in *cs. */
static void pass1_run_channel(
    int channel,
    const uint8_t *raw_bayer, int width, int height,
    int log_bits, int is_rggb, int prescale,
    FUSED_CHANNEL_STATE *cs)
{
    int ch_width = width / 2;
    int ch_height = height / 2;
    /* GPR_ROW_DECIMATE=2: skip alternate Bayer row pairs.
       GPR_COL_DECIMATE=2: average pairs of channel columns post-unpack.
       Combined, these give 2x2 channel-space decimation (1/4 area) for the
       50 MP→ ~5.7 MP-equivalent encode path. ch_width and ch_height are
       halved; downstream band sizes match setup_channel_state. */
    int row_stride_pairs = (cs->row_decimate == 2) ? 4 : 2;
    if (row_stride_pairs == 4) ch_height /= 2;
    int col_decimate = (cs->col_decimate == 2) ? 2 : 1;
    int ch_width_full = ch_width;          /* width of intermediate unpack */
    if (col_decimate == 2) ch_width /= 2;  /* output to horiz */
    const uint16_t *bayer = (const uint16_t *)raw_bayer;
    int bayer_pitch = width;

    int32_t mid2 = 2 * (1 << (log_bits - 1));
    uint16_t *log_tbl = (log_bits <= 14) ? EncoderLogCurve14 : EncoderLogCurve16;
    int log_max = (log_bits <= 14) ? 16383 : 65535;

    PIXEL *unpack_row = (PIXEL *)fused_aligned_alloc(ch_width * sizeof(PIXEL));
    if (!unpack_row) return;
    /* Full-width scratch for the post-unpack column-average path. Needed
       whenever col_decimate=2, since either:
         - the fused 4-row AA function is OFF (default: GPR_DECIMATE_AA unset)
           → fast row-skip path calls regular unpack at full ch_width into
             unpack_full, then NEON pair-averages to half-width unpack_row
         - col_decimate alone (no row_decimate) → same fallback path. */
    PIXEL *unpack_full = NULL;
    if (col_decimate == 2) {
        unpack_full = (PIXEL *)fused_aligned_alloc(ch_width_full * sizeof(PIXEL));
        if (!unpack_full) { free(unpack_row); return; }
    }

#ifdef FUSED_TIMING_DETAIL
    double t_unpack = 0, t_horiz = 0, t_vert = 0, t_freq = 0;
    double _td;
    double _ch_start = _fused_ms();
#endif

    /* Bottom-edge handling: the 6-tap vertical filter under-runs by 2
       rows per level. To produce the missing bottom outputs, we run
       extra "tail" iterations beyond ch_height that replicate the last
       horizontal-filter result instead of consuming new input. The number
       of extras needed depends on how many levels are stacked. */
    int tail_extras = 4;  /* enough for pass1 alone */
    if (cs->streaming_active) {
        /* Each cascade level needs 4 extra inputs to emit its 2 missing
           bottom-edge outputs. Three levels stacked needs
             tail = 2*(2*(2*band_h_l3+4)+4)+4 - 2*band_h_l1 = 28.
           Derivation: L_in(L_out)=2*L_out+4 per level, applied 3 times. */
        tail_extras = 28;
    }
    int total_rows = ch_height + tail_extras;

    /* Hoist env-driven decisions OUT of the row loop. Each env lookup uses
       a function-static cache, but the per-row `do_aa = (...)` expression
       and the conditional dispatch still cost branches. Compute the per-
       channel mode once. */
    const int hf_drop_hp = cs->drop_hp;
    int do_aa_ch = (col_decimate == 2 && row_stride_pairs == 4 && cs->decimate_aa);
    if (do_aa_ch && cs->aa_luma_only && (channel == 1 || channel == 2)) {
        do_aa_ch = 0;
    }
    /* Per-row dispatch mode: 0 = AA-2x2, 1 = full-width unpack + pair-avg,
       2 = direct unpack to ch_width. */
    int unpack_mode = do_aa_ch ? 0 : ((col_decimate == 2) ? 1 : 2);

    for (int row = 0; row < total_rows; row++) {
        int buf_idx = cs->buf_row % FUSED_ROW_BUFS;

        if (row < ch_height) {
            const uint16_t *row1 = bayer + (row * row_stride_pairs) * bayer_pitch;
            const uint16_t *row2 = row1 + bayer_pitch;

#ifdef FUSED_TIMING_DETAIL
            _td = _fused_ms();
#endif

            /* unpack_mode dispatch hoisted out of the row loop. See above. */
            if (unpack_mode == 0) {
                /* Slow / quality: average across two row pairs in log space. */
                const uint16_t *row1b = row1 + 2 * bayer_pitch;
                const uint16_t *row2b = row2 + 2 * bayer_pitch;
                /* T15: fully fused AA-2x2 unpack + horizontal_filter_lp_only.
                   Active when GPR_DROP_HIGHPASS=1 AND prescale==2 (level 1
                   wavelet input prescale, the only callsite). Writes
                   directly to lowpass_buf, skipping the 8 KB unpack_row
                   intermediate. Gated by FUSED_FUSE_LP_OFF=1 in case
                   regression. */
                if (hf_drop_hp && prescale == 2 && !cs->fuse_lp_off) {
                    unpack_channel_row_decimate_2x2_fused_lp(channel, is_rggb,
                        log_tbl, log_max, mid2,
                        row1, row2, row1b, row2b,
                        cs->lowpass_buf[buf_idx], ch_width);
                } else {
                    unpack_channel_row_decimate_2x2(channel, is_rggb,
                        log_tbl, log_max, mid2,
                        row1, row2, row1b, row2b, unpack_row, ch_width);
                }
            } else if (unpack_mode == 1) {
                /* T13b (chroma): fused col-decimate for channel 1, 2.
                   T14 (luma):    fused col-decimate for channel 0, 3.
                   T16:           further fused with horizontal-LP when
                                  hf_drop_hp+prescale==2; writes directly to
                                  cs->lowpass_buf, skipping unpack_row.
                   Gated by FUSED_LUMA_FUSED_OFF=1 / FUSED_FUSE_LP_OFF=1. */
                int use_fused_lp = (hf_drop_hp && prescale == 2 && !cs->fuse_lp_off);
                if (channel == 1 || channel == 2) {
                    if (use_fused_lp) {
                        unpack_channel_row_col_decimate_2x1_fused_lp(channel, is_rggb,
                            log_tbl, log_max, mid2,
                            row1, row2, cs->lowpass_buf[buf_idx], ch_width);
                    } else {
                        unpack_channel_row_col_decimate_2x1(channel, is_rggb,
                            log_tbl, log_max, mid2,
                            row1, row2, unpack_row, ch_width);
                    }
                } else if (!cs->luma_fused_off) {
                    if (use_fused_lp) {
                        unpack_luma_row_col_decimate_2x1_fused_lp(channel, is_rggb,
                            log_tbl, log_max, mid2,
                            row1, row2, cs->lowpass_buf[buf_idx], ch_width);
                    } else {
                        unpack_luma_row_col_decimate_2x1(channel, is_rggb,
                            log_tbl, log_max, mid2,
                            row1, row2, unpack_row, ch_width);
                    }
                } else {
                    /* Luma legacy: full-width unpack + post-pair-avg. */
                    unpack_channel_row(channel, is_rggb, log_tbl, log_max, mid2,
                                       row1, row2, unpack_full, ch_width_full);
#if ENABLED(NEON)
                    int o = 0;
                    int o_m4 = (ch_width / 4) * 4;
                    for (; o < o_m4; o += 4) {
                        int32x4_t a = vld1q_s32(&unpack_full[2 * o]);
                        int32x4_t b = vld1q_s32(&unpack_full[2 * o + 4]);
                        int32x4_t e = vuzp1q_s32(a, b);
                        int32x4_t d = vuzp2q_s32(a, b);
                        int32x4_t avg = vshrq_n_s32(vaddq_s32(e, d), 1);
                        vst1q_s32(&unpack_row[o], avg);
                    }
                    for (; o < ch_width; o++)
                        unpack_row[o] = (unpack_full[2*o] + unpack_full[2*o + 1]) >> 1;
#else
                    for (int o = 0; o < ch_width; o++)
                        unpack_row[o] = (unpack_full[2*o] + unpack_full[2*o + 1]) >> 1;
#endif
                }
            } else {
                if (channel == 0 || channel == 3) {
                    unpack_luma_row(channel, is_rggb, log_tbl, log_max, mid2,
                                    row1, row2, unpack_row, ch_width);
                } else {
                    unpack_channel_row(channel, is_rggb, log_tbl, log_max, mid2,
                                       row1, row2, unpack_row, ch_width);
                }
            }

#ifdef FUSED_TIMING_DETAIL
            t_unpack += _fused_ms() - _td; _td = _fused_ms();
#endif

            /* hf_drop_hp hoisted; saves ~30-40% on LL-only-fast path.
               T15: when unpack_mode==0 + hf_drop_hp, the fused-LP path
               wrote directly to lowpass_buf and skipped unpack_row.
               T16: mode 1 (col-decimate) also fuses with LP — both chroma
               and luma paths support it. Skip the separate horizontal_filter
               call when any fused-LP path was taken. */
            int skip_h = 0;
            if (hf_drop_hp && prescale == 2 && !cs->fuse_lp_off) {
                if (unpack_mode == 0) skip_h = 1;
                else if (unpack_mode == 1) {
                    /* mode 1 fused-LP is only used when the channel's
                       fused-col-decimate path was taken (chroma always; luma
                       unless FUSED_LUMA_FUSED_OFF). */
                    if (channel == 1 || channel == 2) skip_h = 1;
                    else if (!cs->luma_fused_off)    skip_h = 1;
                }
            }
            if (skip_h) {
                /* LP already in cs->lowpass_buf[buf_idx]; nothing to do. */
            } else if (hf_drop_hp) {
                horizontal_filter_lp_only(unpack_row,
                                          cs->lowpass_buf[buf_idx],
                                          ch_width, prescale);
            } else {
                horizontal_filter(unpack_row,
                                  cs->lowpass_buf[buf_idx],
                                  cs->highpass_buf[buf_idx],
                                  ch_width, prescale);
            }
        } else {
            /* Tail: replicate the previous slot's lp/hp. */
            int prev = (cs->buf_row - 1) % FUSED_ROW_BUFS;
            if (prev != buf_idx) {
                memcpy(cs->lowpass_buf[buf_idx], cs->lowpass_buf[prev],
                       (size_t)(ch_width / 2) * sizeof(PIXEL));
                memcpy(cs->highpass_buf[buf_idx], cs->highpass_buf[prev],
                       (size_t)(ch_width / 2) * sizeof(PIXEL));
            }
        }
        cs->buf_row++;

#ifdef FUSED_TIMING_DETAIL
        t_horiz += _fused_ms() - _td; _td = _fused_ms();
#endif

        if (cs->buf_row >= 6 && (cs->buf_row % 2) == 0) {
            /* The biorthogonal 5/3 forward emits TWO outputs from the first
               6-row window: out_row=0 (top boundary, LP=r0+r1) and out_row=1
               (interior, LP=r2+r3). Subsequent windows slide by 2 and emit
               one row each. Matches encoder.c:1296-1336 reference. */
            int n_emits = (cs->band_out_row == 0) ? 2 : 1;
            int base = (cs->buf_row - 6) % FUSED_ROW_BUFS;
            if (base < 0) base += FUSED_ROW_BUFS;
            PIXEL *lp_rows[6], *hp_rows[6];
            for (int r = 0; r < 6; r++) {
                int idx = (base + r) % FUSED_ROW_BUFS;
                lp_rows[r] = cs->lowpass_buf[idx];
                hp_rows[r] = cs->highpass_buf[idx];
            }
            /* inline_state[1] may be NULL when GPR_DROP_HIGHPASS=1 (HP bands
               are unused). Fall back to state[0] (LL band, allocated when
               GPR_INCLUDE_LL=1) to distinguish inline vs split mode. */
            const int inline_mode = (cs->inline_state[0] != NULL ||
                                     cs->inline_state[1] != NULL);
            const int streaming = cs->streaming_active;
            int bw = cs->band_width;

            for (int e = 0; e < n_emits; e++) {
                int out_row = cs->band_out_row;
                /* Allow the tail to write into the +12 scratch rows so the
                   streaming cascade can drive level 3 all the way to
                   band_height_l3. Split single-level mode owns only
                   band_height rows in band_data and must not write past it. */
                int max_out_rows = streaming ? (cs->band_height + 12) : cs->band_height;
                if (out_row >= max_out_rows) break;

                int is_top = (out_row == 0);
                int is_bottom = (out_row == cs->band_height - 1);

                /* Output destinations: per-row scratch for inline/streaming,
                   full band buffers for split mode. Hybrid raw-LL inline has
                   HP inline states but no LL inline state, so LL writes to its
                   buffered sideband source. */
                PIXEL *ll_row = (inline_mode && cs->inline_state[0])  ? cs->row_scratch[0]
                              : (inline_mode && cs->band_data[0])     ? (cs->band_data[0] + out_row * cs->band_pitch)
                              : inline_mode                           ? cs->row_scratch[0]
                              : streaming                             ? cs->row_scratch[0]
                                                                      : (cs->band_data[0] + out_row * cs->band_pitch);
                PIXEL *lh_row = inline_mode ? cs->row_scratch[1]
                                            : (cs->band_data[1] + out_row * cs->band_pitch);
                PIXEL *hl_row = inline_mode ? cs->row_scratch[2]
                                            : (cs->band_data[2] + out_row * cs->band_pitch);
                PIXEL *hh_row = inline_mode ? cs->row_scratch[3]
                                            : (cs->band_data[3] + out_row * cs->band_pitch);

                /* GPR_DROP_HIGHPASS=1 → skip LH/HL/HH wavelet arithmetic entirely.
                   Reuse the hoisted hf_drop_hp from outside the row loop. */
                if (hf_drop_hp) {
                    vertical_filter_quantize_row_lo_only(lp_rows, bw,
                        cs->midpoint[0], cs->multiplier[0],
                        ll_row,
                        is_top, is_bottom);
                } else {
                    vertical_filter_quantize_row(lp_rows, bw,
                        cs->midpoint[0], cs->multiplier[0],
                        cs->midpoint[2], cs->multiplier[2],
                        0, 0, 0, 0,
                        ll_row, hl_row, NULL, NULL,
                        is_top, is_bottom);

                    vertical_filter_quantize_row(hp_rows, bw,
                        cs->midpoint[1], cs->multiplier[1],
                        cs->midpoint[3], cs->multiplier[3],
                        0, 0, 0, 0,
                        lh_row, hh_row, NULL, NULL,
                        is_top, is_bottom);
                }

#ifdef FUSED_TIMING_DETAIL
                t_vert += _fused_ms() - _td; _td = _fused_ms();
#endif

                /* Inline-mode: tokenize each highpass band's row immediately while
                   it's still hot in L1. Pass 2 will only do rANS encode.

                   GPR_DROP_HIGHPASS=1 skips highpass tokenization entirely —
                   for measuring "wavelet-as-downsample" budget. Output is NOT a
                   valid GPR file in this mode (decoder will see empty highpass
                   bands), but the timing tells us if dropping HL/LH/HH bands
                   is sufficient to hit 24 fps on 50 MP input. */
                if (inline_mode) {
                    if (cs->inline_state[0]) {
                        jans_inline_row(cs->inline_state[0], ll_row, bw);
                    }
	                    if (!hf_drop_hp) {
	                        /* Inline-mode BayesShrink-style soft-threshold on
	                           highpass bands. Applied while bands are still hot
	                           in L1, before the tokenize loop reads them. LL is
	                           intentionally NOT thresholded — it carries DC. */
	                        int32_t T_lh = cs->inline_denoise_T[1];
	                        int32_t T_hl = cs->inline_denoise_T[2];
	                        int32_t T_hh = cs->inline_denoise_T[3];
	                        if (cs->inline_denoise_hard) {
	                            if (T_lh > 0) hard_threshold_row(lh_row, bw, T_lh);
	                            if (T_hl > 0) hard_threshold_row(hl_row, bw, T_hl);
	                            if (T_hh > 0) hard_threshold_row(hh_row, bw, T_hh);
	                        } else {
	                            if (T_lh > 0) soft_threshold_row(lh_row, bw, T_lh);
	                            if (T_hl > 0) soft_threshold_row(hl_row, bw, T_hl);
	                            if (T_hh > 0) soft_threshold_row(hh_row, bw, T_hh);
	                        }
	                        jans_inline_row(cs->inline_state[1], lh_row, bw);
	                        jans_inline_row(cs->inline_state[2], hl_row, bw);
	                        jans_inline_row(cs->inline_state[3], hh_row, bw);
	                    }
	                }

                /* Multi-level streaming: cascade the fresh LL1 row through
                   level-2 and level-3 wavelets while it's hot in cache. */
                if (streaming) {
                    stream_cascade_higher_levels(cs, ll_row);
                }

#ifdef FUSED_TIMING_DETAIL
                t_freq += _fused_ms() - _td;
#endif

                cs->band_out_row++;
            }
        }
    }

    /* The 6-tap vertical filter underflows by 2 rows at the bottom — the last
       2 band rows are never produced by the loop. The split-pass encoder's
       jans_encode_band_x4 still tokenizes the full band (those untouched
       trailing rows are calloc'd zero), so inline mode must do the same to
       stay bit-identical: emit zero-row tokens for the missing rows. */
    /* Inline-mode detection: state[1] may be NULL when GPR_DROP_HIGHPASS=1,
       but state[0] is still set when GPR_INCLUDE_LL=1. Take either. */
    if (cs->inline_state[0] != NULL || cs->inline_state[1] != NULL) {
        int bw = cs->band_width;
        PIXEL *zero_row = (PIXEL *)calloc(bw, sizeof(PIXEL));
        if (zero_row) {
            while (cs->band_out_row < cs->band_height) {
                if (cs->inline_state[0]) {
                    jans_inline_row(cs->inline_state[0], zero_row, bw);
                }
                if (!hf_drop_hp && cs->inline_state[1]) {
                    jans_inline_row(cs->inline_state[1], zero_row, bw);
                    jans_inline_row(cs->inline_state[2], zero_row, bw);
                    jans_inline_row(cs->inline_state[3], zero_row, bw);
                }
                cs->band_out_row++;
            }
            free(zero_row);
        }
    }

#ifdef FUSED_TIMING_DETAIL
    double _ch_end = _fused_ms();
    double _ch_total = _ch_end - _ch_start;
    double _ch_other = _ch_total - t_unpack - t_horiz - t_vert - t_freq;
    fprintf(stderr, "    ch%d: unpack=%.1f, horiz=%.1f, vert+quant=%.1f, tokenize=%.1f, other=%.1f, TOTAL=%.1f\n",
            channel, t_unpack, t_horiz, t_vert, t_freq, _ch_other, _ch_total);
#endif

    free(unpack_row);
    if (unpack_full) free(unpack_full);
}

/* Sync structure: shared between Pass 1 and Pass 2 threads to overlap them.
   Pass 2 band threads wait on their channel's p1_done flag before encoding. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    volatile int p1_done[4];
} CHANNEL_SYNC;

typedef struct {
    int channel;
    const uint8_t *raw_bayer;
    int width, height;
    int log_bits, is_rggb, prescale;
    FUSED_CHANNEL_STATE *cs;
    CHANNEL_SYNC *sync;
} PASS1_CHANNEL_TASK;

static void *pass1_channel_thread(void *arg) {
    PASS1_CHANNEL_TASK *t = (PASS1_CHANNEL_TASK *)arg;
    /* Pin to channel-index core (FUSED_PIN=1). Prevents kernel migration
       between cores during the row loop, eliminating the bimodal slowdown
       where 2 channel threads end up sharing a core. */
    _fused_pin_self(t->channel);
    pass1_run_channel(t->channel, t->raw_bayer, t->width, t->height,
                      t->log_bits, t->is_rggb, t->prescale, t->cs);

    /* Signal that this channel's Pass 1 is complete — unblocks its 3 P2 bands */
    if (t->sync) {
        pthread_mutex_lock(&t->sync->lock);
        t->sync->p1_done[t->channel] = 1;
        pthread_cond_broadcast(&t->sync->cv);
        pthread_mutex_unlock(&t->sync->lock);
    }
    return NULL;
}

/* One-time channel state setup. In inline_mode, skips the per-band full-image
   buffers and allocates per-row scratch instead — ~85 MB savings at 23 MP,
   ~200 MB at 50 MP. When multi_level is on, also allocates level-2 and
   level-3 band buffers (each is 1/4 and 1/16 the level-1 band size). */
static int setup_channel_state(
    FUSED_CHANNEL_STATE ch_state[4],
    int width, int height, int quality,
    int inline_mode, int multi_level, int levels, int streaming,
    int *out_is_rggb, int *out_log_bits)
{
    int ch_width = width / 2;
    int ch_height = height / 2;
    /* GPR_ROW_DECIMATE=2 — halve ch_height (row skip).
       GPR_COL_DECIMATE=2 — halve ch_width (post-unpack pair average).
       Combined: 2x2 channel-space decimation; bands/buffers shrink to match. */
    const char *_rdec_env = getenv("GPR_ROW_DECIMATE");
    int row_decimate = (_rdec_env && *_rdec_env == '2') ? 2 : 1;
    if (row_decimate == 2) ch_height /= 2;
    const char *_cdec_env = getenv("GPR_COL_DECIMATE");
    int col_decimate = (_cdec_env && *_cdec_env == '2') ? 2 : 1;
    if (col_decimate == 2) ch_width /= 2;
    /* GPR_INCLUDE_LL: in single-level mode, the LL band IS the final lowpass
       output (not a cascade intermediate). Its magnitude after the 5/3 wavelet
       can reach ~16k for 14-bit input, which overflows the rANS class-15
       ceiling of 2047. We need the same extra divisor trick the multi-level
       LL3 path uses to keep LL bounded. */
    const char *_ll_env = getenv("GPR_INCLUDE_LL");
    int single_level_include_ll = (_ll_env && *_ll_env == '1') ? 1 : 0;
    const char *_raw_ll_env_setup = getenv("FUSED_RAW_LL");
    int single_level_raw_ll = (!multi_level && single_level_include_ll &&
                               _raw_ll_env_setup && *_raw_ll_env_setup == '1');
    QUANT qt_buf[10];
    memcpy(qt_buf, quality_tables[(quality >= 0 && quality < 12) ? quality : 3],
           sizeof(qt_buf));
    apply_quant_override(qt_buf);
    const QUANT *qt = qt_buf;

    for (int ch = 0; ch < 4; ch++) {
        ch_state[ch].levels = levels;
        ch_state[ch].row_decimate = row_decimate;
        ch_state[ch].col_decimate = col_decimate;
        /* Level-1 quant: qt[0]=LL1 (effectively no-op divisor=1),
           qt[7,8,9]=LH1,HL1,HH1 in multi-level mode (coarsest quantization
           on the largest bands). Single-level mode uses qt[1..3]. */
        int q_ll1 = 0, q_lh1 = 1, q_hl1 = 2, q_hh1 = 3;
        if (multi_level) {
            q_ll1 = 0; q_lh1 = 7; q_hl1 = 8; q_hh1 = 9;
        }
        /* When emitting LL in single-level (multi_level=0), divide LL by
           the same 16× factor multi-level uses for LL3. Otherwise LL1 mag
           exceeds the rANS class-15 ceiling and gets clipped at ±2047. */
        int q_ll1_eff = qt[q_ll1];
        if (!multi_level && single_level_include_ll) {
            int ll1_extra = 16;
            const char *_ll1_env = getenv("FUSED_LL1_DIVISOR");
            if (_ll1_env && *_ll1_env) {
                int v = atoi(_ll1_env);
                if (v >= 1 && v <= 64) ll1_extra = v;
            }
            const char *_raw_ll_env = getenv("FUSED_RAW_LL");
            if (_raw_ll_env && *_raw_ll_env == '1') ll1_extra = 1;
            q_ll1_eff = qt[q_ll1] * ll1_extra;
        }
        ch_state[ch].midpoint[0] = get_midpoint(q_ll1_eff);
        ch_state[ch].multiplier[0] = get_multiplier(q_ll1_eff);
        ch_state[ch].midpoint[1] = get_midpoint(qt[q_lh1]);
        ch_state[ch].multiplier[1] = get_multiplier(qt[q_lh1]);
        ch_state[ch].midpoint[2] = get_midpoint(qt[q_hl1]);
        ch_state[ch].multiplier[2] = get_multiplier(qt[q_hl1]);
        ch_state[ch].midpoint[3] = get_midpoint(qt[q_hh1]);
        ch_state[ch].multiplier[3] = get_multiplier(qt[q_hh1]);

        ch_state[ch].band_width = ch_width / 2;
        ch_state[ch].band_height = ch_height / 2;
        ch_state[ch].band_pitch = ch_state[ch].band_width;
        ch_state[ch].band_out_row = 0;
        ch_state[ch].buf_row = 0;
        memset(ch_state[ch].freq, 0, sizeof(ch_state[ch].freq));
        memset(ch_state[ch].run_state, 0, sizeof(ch_state[ch].run_state));

        int bw = ch_state[ch].band_width;
        int bh = ch_state[ch].band_height;
        for (int band = 0; band < 4; band++) {
            if (inline_mode && single_level_raw_ll && band == 0) {
                /* Hybrid inline/raw-LL path: highpass bands tokenize inline,
                   but LL must stay buffered so Pass 2 can emit the FLL1 raw
                   sideband instead of feeding oversized LL coefficients into
                   the rANS inline tokenizer. */
                ch_state[ch].band_data[band] = (PIXEL *)fused_aligned_calloc((size_t)bw * (bh + 12), sizeof(PIXEL));
                ch_state[ch].row_scratch[band] = NULL;
                if (!ch_state[ch].band_data[band]) return -1;
            } else if (inline_mode && !multi_level) {
                /* Need only a small row scratch instead of the full band */
                ch_state[ch].band_data[band] = NULL;
                ch_state[ch].row_scratch[band] = (PIXEL *)fused_aligned_calloc(bw, sizeof(PIXEL));
                if (!ch_state[ch].row_scratch[band]) return -1;
            } else if (multi_level && streaming && band == 0) {
                /* Streaming multi-level: LL1 is consumed inline as it's
                   produced — no need to buffer the full band. Use a 1-row
                   scratch instead. */
                ch_state[ch].band_data[band] = NULL;
                ch_state[ch].row_scratch[band] = (PIXEL *)fused_aligned_calloc(bw, sizeof(PIXEL));
                if (!ch_state[ch].row_scratch[band]) return -1;
            } else {
                /* +12 extra rows for the bottom-edge tail handler.
                   Derivation: each cascade level under-runs by 2 outputs;
                   to fill 2 extra L3 outputs we need 4 extra L2 outputs
                   (= 8 extra L1 outputs). Plus L1's own under-run = 12
                   extras. Pass 2 still only encodes band_height rows.
                   64-B aligned so per-band-row pointers (band_data + row*pitch
                   stays line-aligned when pitch is a multiple of 16 PIXELs)
                   stay aligned for NEON quantize stores. */
                ch_state[ch].band_data[band] = (PIXEL *)fused_aligned_calloc((size_t)bw * (bh + 12), sizeof(PIXEL));
                ch_state[ch].row_scratch[band] = NULL;
                if (!ch_state[ch].band_data[band]) return -1;
            }
        }
        for (int r = 0; r < FUSED_ROW_BUFS; r++) {
            /* Per-channel circular row buffers — the level-1 horizontal-filter
               output, then read by vertical_filter on the very next row pair.
               Hottest scratch in the encoder (one read + one write per pixel
               per row). 64-B aligned to keep NEON int32 loads/stores from
               splitting on every cache line. */
            ch_state[ch].lowpass_buf[r] = (PIXEL *)fused_aligned_calloc(ch_width / 2, sizeof(PIXEL));
            ch_state[ch].highpass_buf[r] = (PIXEL *)fused_aligned_calloc(ch_width / 2, sizeof(PIXEL));
            if (!ch_state[ch].lowpass_buf[r] || !ch_state[ch].highpass_buf[r]) return -1;
        }

        /* Multi-level: allocate level-2 (and level-3 when levels==3) band
           buffers + quant. Use ceil at each step so odd-width inputs don't
           silently drop the unpaired column.

           levels==2: 7 bands/channel — LH1/HL1/HH1, LH2/HL2/HH2, LL2.
                      LL2 is the deepest emitted band; band_data_l2[0]
                      always allocated regardless of streaming.
           levels==3: 10 bands/channel (existing); band_data_l3[*] allocated
                      and L3 cascade runs. */
        if (multi_level) {
            int bw2 = (bw + 1) / 2, bh2 = (bh + 1) / 2;
            int bw3 = (bw2 + 1) / 2, bh3 = (bh2 + 1) / 2;
            ch_state[ch].band_width_l2 = bw2;
            ch_state[ch].band_height_l2 = bh2;
            ch_state[ch].band_width_l3 = bw3;
            ch_state[ch].band_height_l3 = bh3;
            for (int band = 0; band < 4; band++) {
                /* L2 LL band (band==0) only needs full storage when LL2 is
                   emitted (levels==2) or in sequential 3-level mode.
                   Streaming 3-level uses ll2_row_scratch instead. */
                int need_l2 = (levels == 2) || !(streaming && band == 0);
                if (need_l2) {
                    /* +4 extra rows so the bottom-edge tail cascade can
                       write past band_height without overflowing. */
                    ch_state[ch].band_data_l2[band] = (PIXEL *)fused_aligned_calloc(
                        (size_t)bw2 * (bh2 + 4), sizeof(PIXEL));
                    if (!ch_state[ch].band_data_l2[band]) return -1;
                } else {
                    ch_state[ch].band_data_l2[band] = NULL;
                }
                /* L3 bands only allocated for true 3-level mode. */
                if (levels == 3) {
                    ch_state[ch].band_data_l3[band] = (PIXEL *)fused_aligned_calloc(
                        (size_t)bw3 * (bh3 + 4), sizeof(PIXEL));
                    if (!ch_state[ch].band_data_l3[band]) return -1;
                } else {
                    ch_state[ch].band_data_l3[band] = NULL;
                }
            }
            /* Streaming buffers — only in streaming mode and only for the
               levels actually needed. L3 streaming row buffers only when
               levels==3. */
            if (streaming) {
                for (int r = 0; r < FUSED_ROW_BUFS; r++) {
                    ch_state[ch].lp_buf_l2[r] = (PIXEL *)fused_aligned_calloc(bw2, sizeof(PIXEL));
                    ch_state[ch].hp_buf_l2[r] = (PIXEL *)fused_aligned_calloc(bw2, sizeof(PIXEL));
                    if (!ch_state[ch].lp_buf_l2[r] || !ch_state[ch].hp_buf_l2[r])
                        return -1;
                    if (levels == 3) {
                        ch_state[ch].lp_buf_l3[r] = (PIXEL *)fused_aligned_calloc(bw3, sizeof(PIXEL));
                        ch_state[ch].hp_buf_l3[r] = (PIXEL *)fused_aligned_calloc(bw3, sizeof(PIXEL));
                        if (!ch_state[ch].lp_buf_l3[r] || !ch_state[ch].hp_buf_l3[r])
                            return -1;
                    } else {
                        ch_state[ch].lp_buf_l3[r] = NULL;
                        ch_state[ch].hp_buf_l3[r] = NULL;
                    }
                }
                ch_state[ch].ll2_row_scratch = (PIXEL *)fused_aligned_calloc(bw2, sizeof(PIXEL));
                if (!ch_state[ch].ll2_row_scratch) return -1;
                ch_state[ch].buf_row_l2 = 0;
                ch_state[ch].band_out_row_l2 = 0;
                ch_state[ch].buf_row_l3 = 0;
                ch_state[ch].band_out_row_l3 = 0;
                ch_state[ch].streaming_active = 1;
            } else {
                ch_state[ch].streaming_active = 0;
            }
            /* Level-2 quant.
               levels==3: qt[0]=LL2 (no-op divisor=1; LL2 is intermediate)
               levels==2: qt[0] × LL2_EXTRA_DIVISOR (16) to keep LL2 mag
                          under the rANS class-15 ceiling — LL2 IS encoded. */
            #define FUSED_LL2_EXTRA_DIVISOR_DEFAULT 16
            /* Env override for the LL2 extra divisor. Experiment-controlled.
               Lower values preserve more LL2 fidelity at file-size cost.
               Must stay high enough to keep LL2 magnitudes < rANS class-15
               ceiling (2047) for typical content. */
            int ll2_extra = FUSED_LL2_EXTRA_DIVISOR_DEFAULT;
            const char *_ll2_env = getenv("FUSED_LL2_DIVISOR");
            if (_ll2_env && *_ll2_env) {
                int v = atoi(_ll2_env);
                if (v >= 1 && v <= 64) ll2_extra = v;
            }
            int ll2_div = (levels == 2) ? (qt[0] * ll2_extra) : qt[0];
            ch_state[ch].midpoint_l2[0] = get_midpoint(ll2_div);
            ch_state[ch].multiplier_l2[0] = get_multiplier(ll2_div);
            ch_state[ch].midpoint_l2[1] = get_midpoint(qt[4]);
            ch_state[ch].multiplier_l2[1] = get_multiplier(qt[4]);
            ch_state[ch].midpoint_l2[2] = get_midpoint(qt[5]);
            ch_state[ch].multiplier_l2[2] = get_multiplier(qt[5]);
            ch_state[ch].midpoint_l2[3] = get_midpoint(qt[6]);
            ch_state[ch].multiplier_l2[3] = get_multiplier(qt[6]);
            /* Level-3 quant — only meaningful when levels==3. For
               levels==2 these stay at zero (L3 cascade is skipped). */
            if (levels == 3) {
                /* qt[0]=LL3 (encoded), qt[1,2,3]=LH3,HL3,HH3.
                   LL3 divisor 16× to keep magnitudes under rANS class-15
                   ceiling (2047). */
                int ll3_extra = 16;
                const char *_ll3_env = getenv("FUSED_LL3_DIVISOR");
                if (_ll3_env && *_ll3_env) {
                    int v = atoi(_ll3_env);
                    if (v >= 1 && v <= 64) ll3_extra = v;
                }
                int ll3_div = qt[0] * ll3_extra;
                ch_state[ch].midpoint_l3[0] = get_midpoint(ll3_div);
                ch_state[ch].multiplier_l3[0] = get_multiplier(ll3_div);
                ch_state[ch].midpoint_l3[1] = get_midpoint(qt[1]);
                ch_state[ch].multiplier_l3[1] = get_multiplier(qt[1]);
                ch_state[ch].midpoint_l3[2] = get_midpoint(qt[2]);
                ch_state[ch].multiplier_l3[2] = get_multiplier(qt[2]);
                ch_state[ch].midpoint_l3[3] = get_midpoint(qt[3]);
                ch_state[ch].multiplier_l3[3] = get_multiplier(qt[3]);
            }
        } else {
            for (int band = 0; band < 4; band++) {
                ch_state[ch].band_data_l2[band] = NULL;
                ch_state[ch].band_data_l3[band] = NULL;
            }
        }
    }
    (void)ch_height;
    *out_is_rggb = 0; *out_log_bits = 14; /* caller sets actual values */
    return 0;
}


/* ================================================================
   Pass 2: rANS Encode using pre-counted frequencies
   ================================================================ */


typedef struct {
    int channel;        /* Which channel's P1 we depend on (0..3) */
    int slot;           /* LL/LH/HL/HH slot within the channel */
    /* Split mode: scan band_data here in Pass 2's tokenize+rANS */
    PIXEL *band_data;
    int width, height, pitch;
    /* Inline mode: pre-built tokens/freq/bitbuf from Pass 1, just rANS-encode */
    JANS_INLINE_STATE *inline_state;
    /* Common */
    uint8_t *enc_buf;
    size_t enc_cap;
    int enc_size;
    CHANNEL_SYNC *sync;
    /* Optional wavelet-domain denoise (split mode only; ignored if inline). */
    double denoise_strength;
    double noise_scale;
    double noise_offset;
    /* LL sideband config captured at encoder-create time. */
    int raw_ll_sideband;
    int pred_ll_sideband;
    int ll_predictor;
    int ll_rice_k;
    int ll_fast_rice;
    int ll_assume_u16;
} PASS2_BAND_TASK;

#define FUSED_RAW_LL_MAGIC 0x314C4C46u  /* "FLL1", little-endian */
#define FUSED_PRED_LL_MAGIC 0x324C4C46u /* "FLL2", little-endian */

static void fused_denoise_band(PIXEL *band_data, int width, int height,
                               int pitch_bytes,
                               double noise_scale, double noise_offset,
                               double strength);

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    uint64_t acc;
    int bits;
} FUSED_LL_BITWRITER;

static inline int fused_ll_bw_flush_byte(FUSED_LL_BITWRITER *bw)
{
    if (bw->pos >= bw->cap) return -1;
    bw->buf[bw->pos++] = (uint8_t)(bw->acc & 0xffu);
    bw->acc >>= 8;
    bw->bits -= 8;
    return 0;
}

static inline int fused_ll_bw_flush32(FUSED_LL_BITWRITER *bw)
{
    if (bw->pos + 4 > bw->cap) return -1;
    uint32_t word = (uint32_t)bw->acc;
    memcpy(bw->buf + bw->pos, &word, sizeof(word));
    bw->pos += sizeof(word);
    bw->acc >>= 32;
    bw->bits -= 32;
    return 0;
}

static inline int fused_ll_bw_put_bits(FUSED_LL_BITWRITER *bw, uint32_t value, int bits)
{
    if (bits < 0 || bits > 32) return -1;
    if (bits == 0) return 0;
    while (bw->bits + bits > 56) {
        if (bw->bits >= 32) {
            if (fused_ll_bw_flush32(bw) != 0) return -1;
        } else {
            if (fused_ll_bw_flush_byte(bw) != 0) return -1;
        }
    }
    uint64_t mask = (bits == 32) ? 0xffffffffull : ((1ull << bits) - 1ull);
    bw->acc |= ((uint64_t)value & mask) << bw->bits;
    bw->bits += bits;
    while (bw->bits >= 32) {
        if (fused_ll_bw_flush32(bw) != 0) return -1;
    }
    return 0;
}

static inline int fused_ll_bw_put_bits64(FUSED_LL_BITWRITER *bw, uint64_t value, int bits)
{
    if (bits < 0 || bits > 56) return -1;
    if (bits == 0) return 0;
    while (bw->bits + bits > 56) {
        if (bw->bits >= 32) {
            if (fused_ll_bw_flush32(bw) != 0) return -1;
        } else {
            if (fused_ll_bw_flush_byte(bw) != 0) return -1;
        }
    }
    uint64_t mask = (bits == 64) ? UINT64_MAX : ((1ull << bits) - 1ull);
    bw->acc |= (value & mask) << bw->bits;
    bw->bits += bits;
    while (bw->bits >= 32) {
        if (fused_ll_bw_flush32(bw) != 0) return -1;
    }
    return 0;
}

static inline int fused_ll_bw_put_unary(FUSED_LL_BITWRITER *bw, uint32_t q)
{
    while (q >= 32) {
        if (fused_ll_bw_put_bits(bw, 0xffffffffu, 32) != 0) return -1;
        q -= 32;
    }
    uint32_t tail = (q == 32) ? 0xffffffffu : ((q == 0) ? 0u : ((1u << q) - 1u));
    return fused_ll_bw_put_bits(bw, tail, (int)q + 1);
}

static inline int fused_ll_bw_put_rice_fast(FUSED_LL_BITWRITER *bw, uint32_t q, uint32_t r, int rice_k)
{
    if (__builtin_expect(q <= 40, 1)) {
        uint64_t unary = (q == 0) ? 0ull : ((1ull << q) - 1ull);
        uint64_t packed = unary | ((uint64_t)r << (q + 1));
        return fused_ll_bw_put_bits64(bw, packed, (int)q + 1 + rice_k);
    }
    if (fused_ll_bw_put_unary(bw, q) != 0) return -1;
    if (rice_k > 0 && fused_ll_bw_put_bits(bw, r, rice_k) != 0) return -1;
    return 0;
}

static int fused_ll_bw_flush(FUSED_LL_BITWRITER *bw)
{
    while (bw->bits >= 32) {
        if (fused_ll_bw_flush32(bw) != 0) return -1;
    }
    while (bw->bits > 0) {
        if (fused_ll_bw_flush_byte(bw) != 0) return -1;
    }
    bw->acc = 0;
    bw->bits = 0;
    return 0;
}

static int fused_write_pred_ll_band(uint8_t *dst, size_t cap,
                                    const PIXEL *src, int width, int height,
                                    int pitch_elems, int channel,
                                    int predictor, int rice_k,
                                    int fast_rice, int assume_u16)
{
    if (!dst || !src || width <= 0 || height <= 0) return -1;
    (void)channel;

    const size_t header_size = 16;
    if (cap < header_size) return -1;
    uint32_t magic = FUSED_PRED_LL_MAGIC;
    uint32_t meta = (uint32_t)(rice_k & 0xff) | ((uint32_t)(predictor & 0xff) << 8);
    uint32_t w = (uint32_t)width;
    uint32_t h = (uint32_t)height;
    memcpy(dst + 0, &magic, sizeof(magic));
    memcpy(dst + 4, &meta, sizeof(meta));
    memcpy(dst + 8, &w, sizeof(w));
    memcpy(dst + 12, &h, sizeof(h));

    FUSED_LL_BITWRITER bw = {
        dst + header_size,
        cap - header_size,
        0,
        0,
        0,
    };
    if (predictor == 1 && fast_rice && assume_u16 && rice_k > 0) {
        const uint32_t r_mask = (1u << rice_k) - 1u;
        const PIXEL *row = src;
        int32_t left = 0;
        for (int x = 0; x < width; x++) {
            int32_t v = row[x];
            int32_t residual = v - left;
            left = v;
            uint32_t zz = ((uint32_t)residual << 1) ^ (uint32_t)(residual >> 31);
            if (fused_ll_bw_put_rice_fast(&bw, zz >> rice_k, zz & r_mask, rice_k) != 0) return -1;
        }
        for (int y = 1; y < height; y++) {
            const PIXEL *prev = row;
            row += pitch_elems;
            left = 0;
            for (int x = 0; x < width; x++) {
                int32_t v = row[x];
                int32_t pred = (left + prev[x]) >> 1;
                int32_t residual = v - pred;
                left = v;
                uint32_t zz = ((uint32_t)residual << 1) ^ (uint32_t)(residual >> 31);
                if (fused_ll_bw_put_rice_fast(&bw, zz >> rice_k, zz & r_mask, rice_k) != 0) return -1;
            }
        }
        if (fused_ll_bw_flush(&bw) != 0) return -1;
        return (int)(header_size + bw.pos);
    }
    for (int y = 0; y < height; y++) {
        const PIXEL *row = src + (size_t)y * (size_t)pitch_elems;
        const PIXEL *prev = (y > 0) ? (src + (size_t)(y - 1) * (size_t)pitch_elems) : NULL;
        int32_t left = 0;
        for (int x = 0; x < width; x++) {
            int32_t v = row[x];
            if (!assume_u16) {
                if (v < 0) v = 0;
                if (v > 65535) v = 65535;
            }
            int32_t up = prev ? prev[x] : left;
            int32_t pred = (predictor == 1) ? ((left + up) >> 1) : left;
            int32_t residual = v - pred;
            left = v;
            uint32_t zz = ((uint32_t)residual << 1) ^ (uint32_t)(residual >> 31);
            uint32_t q = zz >> rice_k;
            uint32_t r = zz & ((rice_k == 0) ? 0u : ((1u << rice_k) - 1u));
            if (fast_rice) {
                if (fused_ll_bw_put_rice_fast(&bw, q, r, rice_k) != 0) return -1;
            } else {
                if (fused_ll_bw_put_unary(&bw, q) != 0) return -1;
                if (rice_k > 0 && fused_ll_bw_put_bits(&bw, r, rice_k) != 0) return -1;
            }
        }
    }
    if (fused_ll_bw_flush(&bw) != 0) return -1;
    return (int)(header_size + bw.pos);
}

static void *pass2_band_thread(void *arg) {
    PASS2_BAND_TASK *t = (PASS2_BAND_TASK *)arg;

    /* Wait for our channel's Pass 1 to complete (overlap with other channels' P1) */
    if (t->sync) {
        pthread_mutex_lock(&t->sync->lock);
        while (!t->sync->p1_done[t->channel]) {
            pthread_cond_wait(&t->sync->cv, &t->sync->lock);
        }
        pthread_mutex_unlock(&t->sync->lock);
    }

    if (t->inline_state) {
        /* Inline mode: Pass 1 already tokenized; just do rANS encode + emit */
        t->enc_size = jans_inline_finalize(t->enc_buf, t->enc_cap, t->inline_state);
    } else {
        if (t->slot == 0 && t->raw_ll_sideband) {
            if (t->pred_ll_sideband) {
                int pitch_elems = t->pitch / (int)sizeof(PIXEL);
#ifdef FUSED_TIMING_DETAIL
                double t_ll0 = _fused_ms();
#endif
                t->enc_size = fused_write_pred_ll_band(t->enc_buf, t->enc_cap,
                                                       t->band_data,
                                                       t->width, t->height,
                                                       pitch_elems,
                                                       t->channel,
                                                       t->ll_predictor,
                                                       t->ll_rice_k,
                                                       t->ll_fast_rice,
                                                       t->ll_assume_u16);
#ifdef FUSED_TIMING_DETAIL
                fprintf(stderr, "  FUSED pred_ll ch%d: %.3fms size=%d\n",
                        t->channel, _fused_ms() - t_ll0, t->enc_size);
#endif
                return NULL;
            }
            size_t n = (size_t)t->width * (size_t)t->height;
            size_t need = sizeof(uint32_t) + n * sizeof(uint16_t);
            if (need > t->enc_cap) {
                t->enc_size = -1;
                return NULL;
            }
            uint32_t magic = FUSED_RAW_LL_MAGIC;
            memcpy(t->enc_buf, &magic, sizeof(magic));
            uint16_t *dst = (uint16_t *)(t->enc_buf + sizeof(uint32_t));
            const PIXEL *src = t->band_data;
            int pitch_elems = t->pitch / (int)sizeof(PIXEL);
            for (int y = 0; y < t->height; y++) {
                for (int x = 0; x < t->width; x++) {
                    int32_t v = src[(size_t)y * (size_t)pitch_elems + (size_t)x];
                    if (v < 0) v = 0;
                    if (v > 65535) v = 65535;
                    dst[(size_t)y * (size_t)t->width + (size_t)x] = (uint16_t)v;
                }
            }
            t->enc_size = (int)need;
            return NULL;
        }
        /* Split mode: optional wavelet-domain denoise, then tokenize + rANS */
        if (t->denoise_strength > 0.0) {
            fused_denoise_band(t->band_data, t->width, t->height, t->pitch,
                               t->noise_scale, t->noise_offset,
                               t->denoise_strength);
        }
        t->enc_size = jans_encode_band_x4(t->enc_buf, t->enc_cap,
                                           (const int32_t *)t->band_data,
                                           t->width, t->height, t->pitch);
    }
    return NULL;
}

/* ---- Worker-pool for multi-level Pass 2 ----
   Multi-level emits 40 bands (4 channels × 10 bands). On a 4-core Pi 5,
   spawning 40 pthreads inflates the per-band scheduler overhead and
   pthread_create cost (4×) dominates the actual jans_encode work for the
   small L3/LL3 bands. Use a worker pool sized to the CPU count instead:
   workers pull from a shared atomic index, and tasks are pre-ordered so
   the L1 bands (largest) dispatch first (longest-job-first).

   On a wide host (8+ cores) the pool grows so we don't artificially serialize
   work; on Pi 5 the pool is 4 (one per core). */
typedef struct {
    PASS2_BAND_TASK *tasks;
    const int *order;
    int num_tasks;
    int next;
    pthread_mutex_t lock;
} PASS2_WORK_QUEUE;

typedef struct {
    PASS2_WORK_QUEUE *wq;
    int worker_id;
} PASS2_WORKER_ARG;

static void *pass2_worker_thread(void *arg) {
    PASS2_WORKER_ARG *wa = (PASS2_WORKER_ARG *)arg;
    PASS2_WORK_QUEUE *wq = wa->wq;
    if (_fused_pin_p2_enabled() && wa->worker_id >= 0) {
        _fused_pin_self(wa->worker_id);
    }
    for (;;) {
        pthread_mutex_lock(&wq->lock);
        int idx = wq->next++;
        pthread_mutex_unlock(&wq->lock);
        if (idx >= wq->num_tasks) return NULL;
        if (wq->order) idx = wq->order[idx];
        pass2_band_thread(&wq->tasks[idx]);
    }
}

/* Pool size cap. Tuned per-platform from observed scaling:
   - Pi 5 (Cortex-A76 ×4) saturates around 4 workers; the per-task pthread
     create / join cost (40× ~50 µs = 2 ms) is a measurable fraction of
     Pass 2 wall time (~22 ms baseline), so the pool wins.
   - Mac (Apple Silicon, 8+ cores) is faster with one worker per band
     because thread_create is cheap and the OS scheduler distributes
     across the wider core array; the pool would underutilize.
   The choice is automatic via sysconf(_SC_NPROCESSORS_ONLN) at runtime;
   override with -DPASS2_POOL_FORCE=1/0 for benchmarking. PASS2_POOL_MAX
   bounds the pool size when the pool path is used. */
#ifndef PASS2_POOL_MAX
#  define PASS2_POOL_MAX 8
#endif

static void fused_dump_coeff_band(const char *dir,
                                  int ch,
                                  int slot,
                                  int width,
                                  int height,
                                  const PIXEL *data)
{
    if (!dir || !*dir || !data || width <= 0 || height <= 0) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/ch%d_s%d_w%d_h%d.s32",
             dir, ch, slot, width, height);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, sizeof(PIXEL), (size_t)width * (size_t)height, f);
    fclose(f);
}

static void fused_dump_multilevel_encode_coeffs(int levels,
                                                const PASS2_BAND_TASK *tasks,
                                                int task_count)
{
    const char *dir = getenv("GPR_DUMP_ENCODE_COEFFS");
    if (!dir || !*dir || !tasks || task_count <= 0) return;

    const int bands_per_ch = (levels == 2) ? 7 : 10;
    if (task_count < bands_per_ch * 4) return;
    for (int ch = 0; ch < 4; ch++) {
        for (int s = 0; s < bands_per_ch; s++) {
            const PASS2_BAND_TASK *pt = &tasks[ch * bands_per_ch + s];
            fused_dump_coeff_band(dir, ch, s, pt->width, pt->height, pt->band_data);
        }
    }
}

static int fused_pass2(
    FUSED_CHANNEL_STATE ch_state[4],
    uint8_t *output_buf, size_t output_cap, size_t *output_written
)
{
    size_t pos = 0;

    /* Parallel encode: 12 independent band tasks (4 channels × 3 highpass bands).
       Allocate buffers and dispatch threads. */
    PASS2_BAND_TASK tasks[12];
    pthread_t threads[12];
    int task_count = 0;
    int created[12];

    for (int ch = 0; ch < 4; ch++) {
        FUSED_CHANNEL_STATE *cs = &ch_state[ch];
        for (int band = 1; band < 4; band++) {
            PASS2_BAND_TASK *t = &tasks[task_count];
            t->band_data = cs->band_data[band];
            t->width = cs->band_width;
            t->height = cs->band_height;
            t->pitch = cs->band_width * sizeof(int32_t);
            t->enc_cap = (size_t)t->width * t->height * 4 + 8192;
            t->enc_buf = (uint8_t *)malloc(t->enc_cap);
            t->enc_size = 0;
            if (!t->enc_buf) return -1;

            /* Spawn thread */
            created[task_count] = (pthread_create(&threads[task_count], NULL,
                                                    pass2_band_thread, t) == 0);
            if (!created[task_count]) pass2_band_thread(t);
            task_count++;
        }
    }

    /* Wait for all threads, then concatenate outputs in order */
    for (int i = 0; i < task_count; i++) {
        if (created[i]) pthread_join(threads[i], NULL);
    }

    /* Sequential output concat (preserves band order) */
    for (int i = 0; i < task_count; i++) {
        PASS2_BAND_TASK *t = &tasks[i];
        if (t->enc_size > 0 && pos + t->enc_size <= output_cap) {
            memcpy(output_buf + pos, t->enc_buf, t->enc_size);
            pos += t->enc_size;
        }
        free(t->enc_buf);
    }

    *output_written = pos;
    return 0;
}

/* OLD serial version kept for reference */
static int fused_pass2_serial(
    FUSED_CHANNEL_STATE ch_state[4],
    uint8_t *output_buf, size_t output_cap, size_t *output_written
)
{
    size_t pos = 0;

    /* For each channel, for each highpass band: rANS encode */
    for (int ch = 0; ch < 4; ch++) {
        FUSED_CHANNEL_STATE *cs = &ch_state[ch];

        for (int band = 1; band < 4; band++) {
            PIXEL *data = cs->band_data[band];
            int bw = cs->band_width;
            int bh = cs->band_height;
            int pitch = bw * sizeof(int32_t);

            /* Encode band using existing jans_encode_band_x4 */
            size_t buf_cap = (size_t)bw * bh * 4 + 8192;
            uint8_t *enc_buf = (uint8_t *)malloc(buf_cap);
            if (!enc_buf) return -1;

            int enc_size = jans_encode_band_x4(enc_buf, buf_cap,
                                                (const int32_t *)data,
                                                bw, bh, pitch);
            if (enc_size > 0 && pos + enc_size <= output_cap) {
                memcpy(output_buf + pos, enc_buf, enc_size);
                pos += enc_size;
            }
            free(enc_buf);
        }
    }

    *output_written = pos;
    return 0;
}

/* ================================================================
   Reusable encoder context (for video / batch frames)
   ================================================================ */

struct FUSED_ENCODER {
    int width, height;
    int pixel_format;
    int quality;
    int is_rggb;
    int log_bits;
    int prescale;
    int inline_mode;  /* 1 = tokenize inline in Pass 1 (low DRAM, embedded);
                          0 = split-pass (band_data buffer + Pass 2 tokenize) */
    int multi_level;  /* 1 = multi-level wavelet (see `levels` for depth);
                          0 = single-level (3 bands × 4 ch, legacy) */
    int levels;       /* When multi_level=1: 2 (7 bands/ch × 4 = 28 total;
                          production-validated, no cascade ringing) or 3
                          (10 bands/ch × 4 = 40 total; current default,
                          suffers from biorthogonal 5/3 cascade ringing). */
    int include_ll;   /* 1 = single-level + LL (16 bands total, decodable);
                          0 = highpass-only (12 bands, undecodable except via
                          multi_level path). GPR_INCLUDE_LL=1 to enable. */
    int raw_ll_sideband;  /* FUSED_RAW_LL=1 captured at create time. */
    int pred_ll_sideband; /* FUSED_LL_PREDICT=1 captured at create time. */
    int ll_predictor;     /* 0 = left, 1 = average(left, up). */
    int ll_rice_k[4];     /* Per-channel FLL2 Rice K values. */
    int ll_fast_rice;     /* FUSED_LL_RICE_FAST=1 captured at create time. */
    int ll_assume_u16;    /* FUSED_LL_ASSUME_U16=1 captured at create time. */
    int drop_hp;      /* 1 = GPR_DROP_HIGHPASS=1 at create time. Skips
                          inline_state[1..3] allocation and the Pass-2
                          finalize dispatch for HP bands, since none of them
                          get any rows written in Pass 1 anyway. Saves
                          allocation + thread-create + finalize call per
                          frame; decoder treats size-0 bands as zeros. */
    int32_t inline_denoise_T[FUSED_MAX_BANDS]; /* Per-band threshold captured at create time. */
    int32_t inline_denoise_T_ch[FUSED_MAX_CHANNELS][FUSED_MAX_BANDS];
    int inline_denoise_hard; /* 1 = hard dead-zone threshold, 0 = soft threshold. */
    int streaming;    /* When multi_level=1: 1 = stream level-2/3 inline with
                          level-1 (no LL1/LL2 band buffers, embedded-friendly);
                          0 = sequential (full LL1/LL2 buffers, simpler). */

    FUSED_CHANNEL_STATE ch_state[4];

    /* Persistent Pass 2 enc buffers (one per band) */
    uint8_t *enc_bufs[16];   /* 16 = 4 channels × 4 bands (with optional LL) */
    size_t   enc_caps[16];

    /* Multi-level: persistent Pass 2 enc buffers for all 40 bands
       (4 channels × {3 L1 highpass + 3 L2 + 3 L3 + LL3}) and the
       per-frame task scratch. Allocated only when multi_level=1.
       Without these, each frame paid 40 × {alloc + page-fault on first
       write + free} for buffers totalling ~50 MB at 50 MP — visible
       in median-frame jitter on LPDDR4x. */
    uint8_t *enc_bufs_ml[40];
    size_t   enc_caps_ml[40];

    /* Persistent output stream buffer */
    uint8_t *stream_buf;
    size_t   stream_cap;

    /* Last-frame scatter view for direct container writers. This avoids the
       final band-buffer-to-stream-buffer memcpy when the caller can write a
       `.gvid` frame with writev-style chunks. Valid until the next encode. */
    struct {
        FUSED_HEADER hdr;
        uint32_t band_sizes[16];
    } scatter_prefix;
    const uint8_t *scatter_parts[18];
    size_t scatter_part_sizes[18];
    int scatter_part_count;

    /* Wavelet-domain denoise (BayesShrink). When denoise_strength > 0,
       inline mode is disabled (split-pass only — denoise needs the band
       buffer to compute per-band thresholds). noise_scale/noise_offset
       are in raw-pixel units (multiply DNG NoiseProfile by max_val and
       max_val^2 respectively before passing in). */
    double   noise_scale;
    double   noise_offset;
    double   denoise_strength;

    /* Rate-controller scale on level-1 quant divisors. 1.0 = identity.
       Updated by gpr_encode_fused_set_quant_scale(); read-only no-ops
       when the new value equals the cached one. */
    double quant_scale;

};

static void fused_dump_singlelevel_encode_coeffs(FUSED_ENCODER *ctx)
{
    const char *dir = getenv("GPR_DUMP_ENCODE_COEFFS");
    if (!dir || !*dir || !ctx) return;
    for (int ch = 0; ch < 4; ch++) {
        FUSED_CHANNEL_STATE *cs = &ctx->ch_state[ch];
        for (int s = 0; s < 4; s++) {
            if (!cs->band_data[s]) continue;
            fused_dump_coeff_band(dir, ch, s,
                                  cs->band_width, cs->band_height,
                                  cs->band_data[s]);
        }
    }
}

/* Decide which mode to use. Default: inline mode when CPU count is ≤6
   (typical embedded camera SoC like GP3 with 4× A78). M1 / large servers
   default to split-pass for the wider Pass 2 parallelism. Override via env. */
static int fused_choose_inline_mode(void) {
    const char *env = getenv("FUSED_INLINE_TOKENIZE");
    if (env) {
        if (env[0] == '0') return 0;
        if (env[0] == '1') return 1;
    }
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    return (ncpu > 0 && ncpu <= 6) ? 1 : 0;
}

/* Multi-level mode is off by default; opt in via FUSED_MULTI_LEVEL=1. */
static int fused_choose_multi_level(void) {
    const char *env = getenv("FUSED_MULTI_LEVEL");
    if (env) {
        if (env[0] == '0') return 0;
        if (env[0] == '1') return 1;
    }
    return 0;
}

/* When multi_level is on, choose 2 or 3 wavelet levels.
   Default = 3 (current behavior, kept for compatibility).
   Set FUSED_WAVELET_LEVELS=2 for the production-validated 2-level path
   that ships 24 fps × 50 MP × UHS-II V90 microSD without cascade ringing
   (task #172 — see docs/REGRESSION_2026-05-25.md). */
static int fused_choose_levels(int multi_level) {
    if (!multi_level) return 1;
    const char *env = getenv("FUSED_WAVELET_LEVELS");
    if (env) {
        int v = atoi(env);
        if (v == 2) return 2;
        if (v == 3) return 3;
    }
    return 3;
}

/* Multi-level streaming: when set (and multi_level=1), runs levels 2 and 3
   inline inside each channel's Pass 1 thread instead of as a separate
   post-Pass-1 stage. Saves the LL1 and LL2 full-image buffers
   (~57 MB at 50 MP) at the cost of slightly more complex bookkeeping.
   Default: 1 (streaming is the embedded-friendly choice; sequential
   is kept as a debug/comparison path). */
static int fused_choose_streaming(void) {
    const char *env = getenv("FUSED_MULTI_LEVEL_STREAMING");
    if (env) {
        if (env[0] == '0') return 0;
        if (env[0] == '1') return 1;
    }
    return 1;
}

static int fused_getenv_nonnegative_int(const char *name, int fallback)
{
    const char *env = getenv(name);
    if (!env || !*env) return fallback;
    int v = atoi(env);
    return v < 0 ? 0 : v;
}

static void fused_parse_ll_sideband_config(FUSED_ENCODER *ctx)
{
    const char *e;
    if (!ctx) return;

    e = getenv("FUSED_RAW_LL");
    ctx->raw_ll_sideband = (e && *e == '1') ? 1 : 0;
    e = getenv("FUSED_LL_PREDICT");
    ctx->pred_ll_sideband = (e && *e == '1') ? 1 : 0;

    ctx->ll_predictor = 0;
    e = getenv("FUSED_LL_PREDICTOR");
    if (e && *e) {
        if (strcmp(e, "1") == 0 || strcmp(e, "avg") == 0 || strcmp(e, "average") == 0) {
            ctx->ll_predictor = 1;
        }
    }

    ctx->ll_rice_k[0] = 8;
    ctx->ll_rice_k[1] = 7;
    ctx->ll_rice_k[2] = 7;
    ctx->ll_rice_k[3] = 7;
    e = getenv("FUSED_LL_RICE_KS");
    if (e && *e) {
        const char *p = e;
        for (int i = 0; i < 4 && *p; i++) {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end != p && v >= 0 && v <= 15) {
                ctx->ll_rice_k[i] = (int)v;
            }
            if (end == p) break;
            p = end;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ',') p++;
            while (*p == ' ' || *p == '\t') p++;
        }
    }
    e = getenv("FUSED_LL_RICE_K");
    if (e && *e) {
        int v = atoi(e);
        if (v >= 0 && v <= 15) {
            for (int i = 0; i < 4; i++) ctx->ll_rice_k[i] = v;
        }
    }

    e = getenv("FUSED_LL_RICE_FAST");
    ctx->ll_fast_rice = (e && *e == '1') ? 1 : 0;
    e = getenv("FUSED_LL_ASSUME_U16");
    ctx->ll_assume_u16 = (e && *e == '1') ? 1 : 0;
}

/* Reset just the per-frame state on a context (counters, run state, freq tables,
   inline-tokenize state). Band buffers / row scratch are overwritten as work
   progresses so no per-frame zeroing needed. */
static void fused_reset_frame_state(FUSED_ENCODER *ctx) {
    for (int ch = 0; ch < 4; ch++) {
        FUSED_CHANNEL_STATE *cs = &ctx->ch_state[ch];
        cs->band_out_row = 0;
        cs->buf_row = 0;
        cs->buf_row_l2 = 0;
        cs->band_out_row_l2 = 0;
        cs->buf_row_l3 = 0;
        cs->band_out_row_l3 = 0;
        memset(cs->freq, 0, sizeof(cs->freq));
        memset(cs->run_state, 0, sizeof(cs->run_state));
        for (int band = 0; band < 4; band++) {
            if (cs->inline_state[band]) jans_inline_reset(cs->inline_state[band]);
        }
        /* LL band intentionally NOT thresholded (band 0). HP bands can use the
           global threshold or band-specific overrides captured at create time. */
        cs->inline_denoise_T[0] = 0;
        cs->inline_denoise_T[1] = ctx->inline_denoise_T_ch[ch][1];
        cs->inline_denoise_T[2] = ctx->inline_denoise_T_ch[ch][2];
        cs->inline_denoise_T[3] = ctx->inline_denoise_T_ch[ch][3];
        cs->inline_denoise_hard = ctx->inline_denoise_hard;
    }
}

static int fused_getenv_channel_band_threshold(int ch, const char *band, int fallback)
{
    char name[64];
    snprintf(name, sizeof(name), "GPR_INLINE_DENOISE_T_CH%d_%s", ch, band);
    return fused_getenv_nonnegative_int(name, fallback);
}

FUSED_ENCODER *gpr_encode_fused_create(int width, int height, int pixel_format, int quality) {
    FUSED_ENCODER *ctx = (FUSED_ENCODER *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->width = width;
    ctx->height = height;
    ctx->pixel_format = pixel_format;
    ctx->quality = quality;
    ctx->is_rggb = (pixel_format == 1 || pixel_format == 0 || pixel_format == 4);
    ctx->log_bits = (pixel_format >= 4) ? 16 : 14;
    ctx->prescale = (ctx->log_bits >= 15) ? 2 : 3;
    {
        const char *e = getenv("FUSED_L1_PRESCALE");
        if (e && *e) {
            int v = atoi(e);
            if (v == 0 || v == 2 || v == 3) ctx->prescale = v;
        }
    }
    ctx->quant_scale = 1.0;
    ctx->inline_mode = fused_choose_inline_mode();
    ctx->multi_level = fused_choose_multi_level();
    ctx->levels = fused_choose_levels(ctx->multi_level);
    ctx->streaming = ctx->multi_level ? fused_choose_streaming() : 0;
    {
        int base_T = fused_getenv_nonnegative_int("GPR_INLINE_DENOISE_T", 0);
        ctx->inline_denoise_T[0] = 0;
        ctx->inline_denoise_T[1] = fused_getenv_nonnegative_int("GPR_INLINE_DENOISE_T_LH", base_T);
        ctx->inline_denoise_T[2] = fused_getenv_nonnegative_int("GPR_INLINE_DENOISE_T_HL", base_T);
        ctx->inline_denoise_T[3] = fused_getenv_nonnegative_int("GPR_INLINE_DENOISE_T_HH", base_T);
        for (int ch = 0; ch < FUSED_MAX_CHANNELS; ch++) {
            ctx->inline_denoise_T_ch[ch][0] = 0;
            ctx->inline_denoise_T_ch[ch][1] =
                fused_getenv_channel_band_threshold(ch, "LH", ctx->inline_denoise_T[1]);
            ctx->inline_denoise_T_ch[ch][2] =
                fused_getenv_channel_band_threshold(ch, "HL", ctx->inline_denoise_T[2]);
            ctx->inline_denoise_T_ch[ch][3] =
                fused_getenv_channel_band_threshold(ch, "HH", ctx->inline_denoise_T[3]);
        }
        {
            const char *mode = getenv("GPR_INLINE_DENOISE_MODE");
            const char *hard = getenv("GPR_INLINE_DENOISE_HARD");
            ctx->inline_denoise_hard =
                (mode && strcmp(mode, "hard") == 0) ||
                (hard && *hard == '1');
        }
    }
    /* Multi-level requires split mode (we need LL1/LL2 buffers in memory). */
    if (ctx->multi_level && ctx->inline_mode) {
        ctx->inline_mode = 0;
    }

    SetupEncoderLogCurve();

    int dummy_is_rggb, dummy_log_bits;
    if (setup_channel_state(ctx->ch_state, width, height, quality,
                             ctx->inline_mode, ctx->multi_level, ctx->levels, ctx->streaming,
                             &dummy_is_rggb, &dummy_log_bits) != 0) {
        gpr_encode_fused_destroy(ctx);
        return NULL;
    }

    /* Pre-allocate persistent enc buffers + (if inline) inline-tokenize state */
    /* GPR_INCLUDE_LL=1 — also allocate inline_state[0] for the LL band in
       single-level inline mode. Without this the encoder produces only the
       3 highpass bands per channel and the fused decoder can't reconstruct
       the image (see fused_decode.c return -5). */
    {
        const char *e = getenv("GPR_INCLUDE_LL");
        ctx->include_ll = (e && *e == '1') ? 1 : 0;
    }
    fused_parse_ll_sideband_config(ctx);
    {
        const char *e = getenv("GPR_DROP_HIGHPASS");
        ctx->drop_hp = (e && *e == '1') ? 1 : 0;
    }
    int raw_ll_sideband_inline = 0;
    {
        raw_ll_sideband_inline = (ctx->inline_mode && !ctx->multi_level &&
                                  ctx->include_ll && ctx->raw_ll_sideband);
    }
    {
        const char *e = getenv("GPR_DECIMATE_AA");
        int decimate_aa = (e && *e == '1') ? 1 : 0;
        e = getenv("GPR_AA_LUMA_ONLY");
        int aa_luma_only = (e && *e == '1') ? 1 : 0;
        e = getenv("FUSED_FUSE_LP_OFF");
        int fuse_lp_off = (e && *e == '1') ? 1 : 0;
        e = getenv("FUSED_LUMA_FUSED_OFF");
        int luma_fused_off = (e && *e == '1') ? 1 : 0;
        for (int ch = 0; ch < 4; ch++) {
            FUSED_CHANNEL_STATE *cs = &ctx->ch_state[ch];
            cs->decimate_aa = decimate_aa;
            cs->aa_luma_only = aa_luma_only;
            cs->drop_hp = ctx->drop_hp;
            cs->fuse_lp_off = fuse_lp_off;
            cs->luma_fused_off = luma_fused_off;
        }
    }
    int band_start = ctx->include_ll ? 0 : 1;

    int p2_idx = 0;
    for (int ch = 0; ch < 4; ch++) {
        FUSED_CHANNEL_STATE *cs = &ctx->ch_state[ch];
        size_t band_coeffs = (size_t)cs->band_width * cs->band_height;
        for (int band = band_start; band < 4; band++) {
            size_t cap = band_coeffs * 4 + 8192;
            ctx->enc_caps[p2_idx] = cap;
            ctx->enc_bufs[p2_idx] = (uint8_t *)malloc(cap);
            if (!ctx->enc_bufs[p2_idx]) {
                gpr_encode_fused_destroy(ctx);
                return NULL;
            }
            /* GPR_DROP_HIGHPASS=1: bands 1..3 (LH/HL/HH) receive no rows in
               Pass 1, so skip their inline_state allocation. Band 0 (LL) is
               still needed when GPR_INCLUDE_LL=1. */
            if (ctx->inline_mode && !(ctx->drop_hp && band > 0) &&
                !(raw_ll_sideband_inline && band == 0)) {
                /* Stripe encoding: adaptive default based on band height,
                   overridable via env vars.

                   Sweep data:
                     23 MP HERO10 (band_height=1044): 128 wins (4.36 MB vs
                       4.39 MB at 64 — 0.7% better)
                     50 MP MISSION 1 (band_height=1450): 64 wins (9.50 MB vs
                       10.55 MB at 128 — 10% better)
                     50 MP X2D (band_height=1536) on Pi 5 A76: 32 wins on
                       sustained throughput (52.9 → 51.9 ms median over
                       50 frames) AND 2.3% smaller files vs 64. Tighter
                       per-stripe freq fit improves at high-MP sources.
                   Larger bands benefit from more stripes (tighter local
                   freq fit per stripe outweighs per-stripe table overhead).

                   Heuristic: 128 below 1200 rows, 32 above. Env overrides
                   take precedence. */
                int rows = (cs->band_height < 1200) ? 128 : 32;
                const char *e_global = getenv("FUSED_STRIPE_ROWS");
                if (e_global) { int v = atoi(e_global); if (v > 0) rows = v; }
                const char *band_env = NULL;
                if (band == 0) band_env = getenv("FUSED_STRIPE_ROWS_LL");
                if (band == 1) band_env = getenv("FUSED_STRIPE_ROWS_LH");
                if (band == 2) band_env = getenv("FUSED_STRIPE_ROWS_HL");
                if (band == 3) band_env = getenv("FUSED_STRIPE_ROWS_HH");
                if (band_env) { int v = atoi(band_env); if (v > 0) rows = v; }

                /* FUSED_DEFER_RANS=1 moves the rANS encode out of Pass 1's
                   stripe-flush path and into Pass 2's finalize. In defer mode
                   the tokens[] and resid_buf[] arenas must hold the full
                   band, not just a single stripe. */
                const char *_defer_env = getenv("FUSED_DEFER_RANS");
                int defer_rans = (_defer_env && *_defer_env == '1') ? 1 : 0;

                size_t alloc_coeffs;
                if (defer_rans) {
                    alloc_coeffs = band_coeffs;
                } else {
                    alloc_coeffs = (size_t)cs->band_width * (size_t)rows;
                    if (alloc_coeffs > band_coeffs) alloc_coeffs = band_coeffs;
                }
                cs->inline_state[band] = jans_inline_create(alloc_coeffs);
                if (!cs->inline_state[band]) {
                    gpr_encode_fused_destroy(ctx);
                    return NULL;
                }
                {
                    char profile_label[16];
                    snprintf(profile_label, sizeof(profile_label), "ch%d_b%d", ch, band);
                    jans_inline_set_profile_label(cs->inline_state[band], profile_label);
                }
                jans_inline_set_stripe_rows(cs->inline_state[band], rows);
                if (defer_rans) {
                    jans_inline_set_defer_rans(cs->inline_state[band], 1);
                }
            }
            p2_idx++;
        }
    }

    /* Pre-allocate output stream buffer */
    ctx->stream_cap = (size_t)width * height * 2;
    ctx->stream_buf = (uint8_t *)malloc(ctx->stream_cap);
    if (!ctx->stream_buf) {
        gpr_encode_fused_destroy(ctx);
        return NULL;
    }

    /* Pre-allocate Pass-2 enc buffers for multi-level. Slot layout per channel
       depends on ctx->levels:
         levels==2: 0..2=L1 hp, 3..5=L2 hp, 6=LL2 (7 slots)
         levels==3: 0..2=L1 hp, 3..5=L2 hp, 6..8=L3 hp, 9=LL3 (10 slots)
       Capacity = band_w * band_h * 4 + 8K (jans_encode_band_x4 upper bound). */
    if (ctx->multi_level) {
        FUSED_CHANNEL_STATE *cs0 = &ctx->ch_state[0];
        const int slots_per_ch = (ctx->levels == 2) ? 7 : 10;
        size_t per_slot_cap[10] = {0};
        per_slot_cap[0] = (size_t)cs0->band_width    * cs0->band_height    * 4 + 8192;
        per_slot_cap[1] = per_slot_cap[0];
        per_slot_cap[2] = per_slot_cap[0];
        per_slot_cap[3] = (size_t)cs0->band_width_l2 * cs0->band_height_l2 * 4 + 8192;
        per_slot_cap[4] = per_slot_cap[3];
        per_slot_cap[5] = per_slot_cap[3];
        if (ctx->levels == 2) {
            /* slot 6 is LL2 (same dims as L2 hp) */
            per_slot_cap[6] = per_slot_cap[3];
        } else {
            /* slots 6..9 are L3-sized */
            size_t l3cap = (size_t)cs0->band_width_l3 * cs0->band_height_l3 * 4 + 8192;
            per_slot_cap[6] = l3cap;
            per_slot_cap[7] = l3cap;
            per_slot_cap[8] = l3cap;
            per_slot_cap[9] = l3cap;
        }
        const int total_tasks = slots_per_ch * 4;
        for (int i = 0; i < total_tasks; i++) {
            size_t cap = per_slot_cap[i % slots_per_ch];
            ctx->enc_caps_ml[i] = cap;
            ctx->enc_bufs_ml[i] = (uint8_t *)malloc(cap);
            if (!ctx->enc_bufs_ml[i]) {
                gpr_encode_fused_destroy(ctx);
                return NULL;
            }
        }
    }

    /* Lazy paging strategy: skip pre-fault on stream_buf and enc_bufs (both
       are heavily over-sized for the typical encoded output ~10% of capacity).
       Only band_data is pre-faulted in split mode because vert+quant writes
       every pixel; inline mode doesn't allocate band_data at all.

       This pushes ~150 MB of RSS off the books at 50 MP, paid only for the
       actually-written portion of each buffer. */
    for (int ch = 0; ch < 4; ch++) {
        FUSED_CHANNEL_STATE *cs = &ctx->ch_state[ch];
        for (int band = 0; band < 4; band++) {
            if (cs->band_data[band]) {
                memset(cs->band_data[band], 0,
                       (size_t)cs->band_width * cs->band_height * sizeof(PIXEL));
            }
        }
    }

    return ctx;
}

void gpr_encode_fused_set_denoise(FUSED_ENCODER *ctx,
                                  double noise_scale,
                                  double noise_offset,
                                  double strength)
{
    if (!ctx) return;
    if (strength > 0.0 && ctx->inline_mode) {
        fprintf(stderr, "gpr_encode_fused_set_denoise: denoise requires split mode; "
                        "set FUSED_INLINE_TOKENIZE=0 before gpr_encode_fused_create. "
                        "Denoise NOT enabled.\n");
        return;
    }
    ctx->noise_scale = noise_scale;
    ctx->noise_offset = noise_offset;
    ctx->denoise_strength = strength;
}

void gpr_encode_fused_set_quant_scale(FUSED_ENCODER *ctx, double scale)
{
    if (!ctx) return;
    if (scale < 0.25) scale = 0.25;
    if (scale > 16.0) scale = 16.0;
    if (scale == ctx->quant_scale) return;   /* no-op */
    ctx->quant_scale = scale;

    /* Reproduce the level-1 quant init from setup_channel_state(), then
       apply `scale` to the per-band divisor before computing midpoint/
       multiplier. Matches the legacy encoder's per-frame rate-controller hook. */
    const int quality = ctx->quality;
    const QUANT *qt = quality_tables[(quality >= 0 && quality < 12) ? quality : 3];
    int q_ll1 = 0, q_lh1 = 1, q_hl1 = 2, q_hh1 = 3;
    if (ctx->multi_level) {
        q_ll1 = 0; q_lh1 = 7; q_hl1 = 8; q_hh1 = 9;
    }
    int q_ll1_base = qt[q_ll1];
    if (!ctx->multi_level && ctx->include_ll) {
        int ll1_extra = 16;
        const char *_ll1_env = getenv("FUSED_LL1_DIVISOR");
        if (_ll1_env && *_ll1_env) {
            int v = atoi(_ll1_env);
            if (v >= 1 && v <= 64) ll1_extra = v;
        }
        const char *_raw_ll_env = getenv("FUSED_RAW_LL");
        if (_raw_ll_env && *_raw_ll_env == '1') ll1_extra = 1;
        q_ll1_base *= ll1_extra;
    }
    int divs[4] = { q_ll1_base, qt[q_lh1], qt[q_hl1], qt[q_hh1] };

    for (int ch = 0; ch < 4; ch++) {
        FUSED_CHANNEL_STATE *cs = &ctx->ch_state[ch];
        for (int band = 0; band < 4; band++) {
            int eff = (int)((double)divs[band] * scale + 0.5);
            if (eff < 1) eff = 1;
            cs->midpoint[band]   = get_midpoint(eff);
            cs->multiplier[band] = get_multiplier(eff);
        }
    }
}

/* Per-band BayesShrink denoise: estimate noise sigma from this band (MAD on
   the band itself, since the fused encoder doesn't keep an LL band around for
   calibrated sigma), compute the threshold, then noise-aware-requantize. Falls
   back to soft-thresholding when the band is pure noise (no signal energy). */
static void fused_denoise_band(PIXEL *band_data, int width, int height,
                               int pitch_bytes,
                               double noise_scale, double noise_offset,
                               double strength)
{
    if (!band_data || strength <= 0.0) return;

    /* Estimate noise sigma. Prefer calibrated noise model when available
       (operates on the band's local mean signal), otherwise MAD. */
    double sigma_noise;
    if (noise_scale > 0.0) {
        sigma_noise = CalibratedNoiseSigma(band_data, width, height,
                                            pitch_bytes,
                                            noise_scale, noise_offset);
    } else {
        sigma_noise = EstimateNoiseSigma(band_data, width, height, pitch_bytes);
    }
    if (sigma_noise <= 0.0) return;

    /* Sample band variance for the BayesShrink threshold. Stride directly
       across band_data — old code iterated every pixel just to sample every
       Nth, wasting ~99% of the loop body on a modulo check. */
    int pitch_pixels = pitch_bytes / (int)sizeof(PIXEL);
    int N = width * height;
    int step = (N > 10000) ? (N / 10000) : 1;
    int sample_target = (N + step - 1) / step;
    if (sample_target > 10000) sample_target = 10000;
    double sum_sq = 0.0;
    int sample_count = 0;
    for (int s = 0; s < sample_target; s++) {
        int idx = s * step;
        int row = idx / width;
        if (row >= height) break;
        int col = idx - row * width;
        double v = (double)band_data[row * pitch_pixels + col];
        sum_sq += v * v;
        sample_count++;
    }
    double sigma_band_sq = (sample_count > 0) ? sum_sq / sample_count : 0.0;
    double sigma_noise_sq = sigma_noise * sigma_noise;
    double sigma_signal_sq = sigma_band_sq - sigma_noise_sq;

    if (sigma_signal_sq > sigma_noise_sq * 0.01) {
        /* Signal present: noise-aware requantize at sigma_noise step. */
        NoiseAwareRequantize(band_data, width, height, pitch_bytes,
                             sigma_noise, strength);
    } else {
        /* Pure noise band: soft threshold at the VisuShrink scale. */
        double T = sigma_noise * sqrt(2.0 * log((double)N)) * strength;
        SoftThresholdBand(band_data, width, height, pitch_bytes, T);
    }
}

void gpr_encode_fused_destroy(FUSED_ENCODER *ctx) {
    if (!ctx) return;
    for (int i = 0; i < 16; i++) if (ctx->enc_bufs[i]) free(ctx->enc_bufs[i]);
    for (int i = 0; i < 40; i++) if (ctx->enc_bufs_ml[i]) free(ctx->enc_bufs_ml[i]);
    if (ctx->stream_buf) free(ctx->stream_buf);
    for (int ch = 0; ch < 4; ch++) {
        FUSED_CHANNEL_STATE *cs = &ctx->ch_state[ch];
        for (int band = 0; band < 4; band++) {
            if (cs->band_data[band])   free(cs->band_data[band]);
            if (cs->row_scratch[band]) free(cs->row_scratch[band]);
            if (cs->inline_state[band]) jans_inline_destroy(cs->inline_state[band]);
            if (cs->band_data_l2[band]) free(cs->band_data_l2[band]);
            if (cs->band_data_l3[band]) free(cs->band_data_l3[band]);
        }
        for (int r = 0; r < FUSED_ROW_BUFS; r++) {
            if (cs->lowpass_buf[r])  free(cs->lowpass_buf[r]);
            if (cs->highpass_buf[r]) free(cs->highpass_buf[r]);
            if (cs->lp_buf_l2[r])    free(cs->lp_buf_l2[r]);
            if (cs->hp_buf_l2[r])    free(cs->hp_buf_l2[r]);
            if (cs->lp_buf_l3[r])    free(cs->lp_buf_l3[r]);
            if (cs->hp_buf_l3[r])    free(cs->hp_buf_l3[r]);
        }
        if (cs->ll2_row_scratch) free(cs->ll2_row_scratch);
        if (cs->ll1_prev1) free(cs->ll1_prev1);
        if (cs->ll1_prev2) free(cs->ll1_prev2);
    }
    free(ctx);
}

/* Forward decl — multi-level path is implemented below. */
static int gpr_encode_fused_frame_multilevel(FUSED_ENCODER *ctx,
                                              const uint8_t *raw_bayer,
                                              uint8_t **vc5_out,
                                              size_t *vc5_size);

static int gpr_encode_fused_frame_single(FUSED_ENCODER *ctx,
                            const uint8_t *raw_bayer, size_t raw_size,
                            uint8_t **vc5_out, size_t *vc5_size,
                            int scatter_mode)
{
    (void)raw_size;
    if (!ctx || !raw_bayer || !vc5_out || !vc5_size) return -1;
    ctx->scatter_part_count = 0;
    if (ctx->multi_level) {
        if (scatter_mode) return -1;
        return gpr_encode_fused_frame_multilevel(ctx, raw_bayer, vc5_out, vc5_size);
    }
    int rc = 0;
    PASS2_BAND_TASK p2_tasks[16];   /* 12 (no LL) or 16 (with LL) */
    memset(p2_tasks, 0, sizeof(p2_tasks));
    pthread_t p2_threads[16];
    int p2_created[16] = {0};
    PASS2_WORK_QUEUE p2_wq;
    int p2_order[16];
    PASS2_WORKER_ARG p2_worker_args[PASS2_POOL_MAX];
    pthread_t p2_workers[PASS2_POOL_MAX];
    int p2_worker_created[PASS2_POOL_MAX] = {0};
    int p2_worker_count = 0;
    int p2_use_pool = 0;
    pthread_t p1_threads[4];
    int p1_created[4] = {0};

#ifdef FUSED_TIMING
    double t0 = _fused_ms();
#endif

    const char *threads_env = getenv("FUSED_THREADS");
    int run_serial = (threads_env && threads_env[0] == '1' && threads_env[1] == '\0');
    if (!run_serial) {
#ifdef PASS2_POOL_FORCE
        p2_use_pool = PASS2_POOL_FORCE;
#else
        long _nproc = sysconf(_SC_NPROCESSORS_ONLN);
        if (_nproc < 1) _nproc = 4;
        p2_use_pool = (_nproc <= 4);
#endif
    }

    fused_reset_frame_state(ctx);

    CHANNEL_SYNC sync;
    pthread_mutex_init(&sync.lock, NULL);
    pthread_cond_init(&sync.cv, NULL);
    for (int i = 0; i < 4; i++) sync.p1_done[i] = 0;

    PASS1_CHANNEL_TASK p1_tasks[4];
    for (int ch = 0; ch < 4; ch++) {
        p1_tasks[ch].channel = ch;
        p1_tasks[ch].raw_bayer = raw_bayer;
        p1_tasks[ch].width = ctx->width;
        p1_tasks[ch].height = ctx->height;
        p1_tasks[ch].log_bits = ctx->log_bits;
        p1_tasks[ch].is_rggb = ctx->is_rggb;
        p1_tasks[ch].prescale = ctx->prescale;
        p1_tasks[ch].cs = &ctx->ch_state[ch];
        p1_tasks[ch].sync = &sync;
    }
    if (run_serial) {
        for (int ch = 0; ch < 4; ch++) {
            pass1_channel_thread(&p1_tasks[ch]);
            p1_created[ch] = 0;
        }
    } else {
        for (int ch = 0; ch < 4; ch++) {
            p1_created[ch] = (pthread_create(&p1_threads[ch], NULL,
                                              pass1_channel_thread, &p1_tasks[ch]) == 0);
        }
    }

    int p2_count = 0;
    int band_start = ctx->include_ll ? 0 : 1;
    int bands_per_channel = 4 - band_start;
    for (int ch = 0; ch < 4; ch++) {
        FUSED_CHANNEL_STATE *cs = &ctx->ch_state[ch];
        for (int band = band_start; band < 4; band++) {
            PASS2_BAND_TASK *pt = &p2_tasks[p2_count];
            pt->channel = ch;
            pt->slot = band;
            pt->band_data    = cs->band_data[band];     /* NULL in inline mode */
            pt->inline_state = cs->inline_state[band];  /* NULL in split mode */
            pt->width = cs->band_width;
            pt->height = cs->band_height;
            pt->pitch = cs->band_width * sizeof(int32_t);
            pt->enc_cap = ctx->enc_caps[p2_count];
            pt->enc_buf = ctx->enc_bufs[p2_count];  /* persistent */
            pt->enc_size = 0;
            pt->sync = &sync;
            pt->denoise_strength = ctx->denoise_strength;
            pt->noise_scale = ctx->noise_scale;
            pt->noise_offset = ctx->noise_offset;
            pt->raw_ll_sideband = ctx->raw_ll_sideband;
            pt->pred_ll_sideband = ctx->pred_ll_sideband;
            pt->ll_predictor = ctx->ll_predictor;
            pt->ll_rice_k = ctx->ll_rice_k[ch];
            pt->ll_fast_rice = ctx->ll_fast_rice;
            pt->ll_assume_u16 = ctx->ll_assume_u16;
            /* GPR_DROP_HIGHPASS=1: skip Pass-2 dispatch for HP bands (1..3).
               Their inline_state is NULL (3a-style skip in create); enc_size
               stays 0 and the band-size manifest records 0 → decoder fills
               that band with zeros (see fused_band_decode_runner: sz<64 →
               memset). LL band (0) still runs. */
            if (ctx->drop_hp && band > 0) {
                p2_created[p2_count] = 0;
                p2_count++;
                continue;
            }
            if (run_serial) {
                pass2_band_thread(pt);
                p2_created[p2_count] = 0;
            } else if (p2_use_pool) {
                p2_created[p2_count] = 0;
            } else {
                p2_created[p2_count] = (pthread_create(&p2_threads[p2_count], NULL,
                                                       pass2_band_thread, pt) == 0);
            }
            p2_count++;
        }
    }

    if (!run_serial && p2_use_pool) {
        int o = 0;
        for (int band = band_start; band < 4; band++) {
            /* Omitted bands retain manifest slots but have no encoding work. */
            if (ctx->drop_hp && band > 0) continue;
            for (int ch = 0; ch < 4; ch++) {
                p2_order[o++] = ch * bands_per_channel + (band - band_start);
            }
        }
        p2_wq.tasks = p2_tasks;
        p2_wq.order = p2_order;
        p2_wq.num_tasks = o;
        p2_wq.next = 0;
        pthread_mutex_init(&p2_wq.lock, NULL);
#ifdef PASS2_POOL_FORCE
        p2_worker_count = 4;
#else
        long _nproc = sysconf(_SC_NPROCESSORS_ONLN);
        if (_nproc < 1) _nproc = 4;
        p2_worker_count = (int)_nproc;
#endif
        if (p2_worker_count > PASS2_POOL_MAX) p2_worker_count = PASS2_POOL_MAX;
        if (p2_worker_count > p2_count) p2_worker_count = p2_count;
        if (p2_worker_count < 1) p2_worker_count = 1;
        for (int w = 0; w < p2_worker_count; w++) {
            p2_worker_args[w].wq = &p2_wq;
            p2_worker_args[w].worker_id = w;
            p2_worker_created[w] = (pthread_create(&p2_workers[w], NULL,
                                                   pass2_worker_thread,
                                                   &p2_worker_args[w]) == 0);
        }
    }

    if (!run_serial) {
        for (int ch = 0; ch < 4; ch++) {
            if (!p1_created[ch]) pass1_channel_thread(&p1_tasks[ch]);
        }
    }
    for (int ch = 0; ch < 4; ch++) {
        if (p1_created[ch]) pthread_join(p1_threads[ch], NULL);
    }

    fused_dump_singlelevel_encode_coeffs(ctx);

#ifdef FUSED_TIMING
    double t1 = _fused_ms();
    fprintf(stderr, "  FUSED Pass1 (parallel, signals P2):      %.1fms\n", t1 - t0);
#endif

    if (p2_use_pool) {
        for (int w = 0; w < p2_worker_count; w++) {
            if (!p2_worker_created[w]) {
                PASS2_WORKER_ARG wa = { &p2_wq, w };
                pass2_worker_thread(&wa);
                break;
            }
        }
        for (int w = 0; w < p2_worker_count; w++) {
            if (p2_worker_created[w]) pthread_join(p2_workers[w], NULL);
        }
        pthread_mutex_destroy(&p2_wq.lock);
    } else {
        for (int i = 0; i < p2_count; i++) {
            if (p2_created[i]) pthread_join(p2_threads[i], NULL);
        }
    }

#ifdef FUSED_TIMING
    double t2 = _fused_ms();
    fprintf(stderr, "  FUSED Pass2 (overlapped w/ P1):          %.1fms (since P1 end)\n", t2 - t1);
#endif

    pthread_mutex_destroy(&sync.lock);
    pthread_cond_destroy(&sync.cv);

    /* Emit header + band manifest + band data into stream_buf. */
    const int num_bands = p2_count;  /* 12 (highpass only) or 16 (with LL) */
    FUSED_HEADER hdr;
    hdr.magic = FUSED_MAGIC;
    hdr.version = FUSED_VERSION;
    hdr.width = ctx->width;
    hdr.height = ctx->height;
    hdr.pixel_format = ctx->pixel_format;
    hdr.quality = ctx->quality;
    hdr.is_rggb = ctx->is_rggb;
    hdr.log_bits = ctx->log_bits;
    hdr.prescale = ctx->prescale;
    hdr.multi_level = 0;
    hdr.num_bands = num_bands;
    /* If row/col decimate were applied during encode, the bands and the
       output Bayer are at half dims. Tell the decoder via this field;
       decoder treats the file as a hdr.width/dec × hdr.height/dec image. */
    hdr.decimate = (ctx->ch_state[0].row_decimate == 2 &&
                    ctx->ch_state[0].col_decimate == 2) ? 2 : 0;

    size_t pos = sizeof(hdr) + (size_t)num_bands * sizeof(uint32_t);
    if (scatter_mode) {
        ctx->scatter_prefix.hdr = hdr;
        ctx->scatter_part_count = 0;
        for (int i = 0; i < num_bands; i++) {
            ctx->scatter_prefix.band_sizes[i] = (uint32_t)p2_tasks[i].enc_size;
        }
        ctx->scatter_parts[ctx->scatter_part_count] = (const uint8_t *)&ctx->scatter_prefix;
        ctx->scatter_part_sizes[ctx->scatter_part_count++] =
            sizeof(ctx->scatter_prefix.hdr) + (size_t)num_bands * sizeof(uint32_t);
        for (int i = 0; i < num_bands; i++) {
            PASS2_BAND_TASK *pt = &p2_tasks[i];
            if (pt->enc_size > 0) {
                ctx->scatter_parts[ctx->scatter_part_count] = pt->enc_buf;
                ctx->scatter_part_sizes[ctx->scatter_part_count++] = pt->enc_size;
                pos += pt->enc_size;
            }
        }
    } else {
        size_t out_pos = 0;
        memcpy(ctx->stream_buf + out_pos, &hdr, sizeof(hdr)); out_pos += sizeof(hdr);
        /* Band-size table */
        for (int i = 0; i < num_bands; i++) {
            uint32_t sz = (uint32_t)p2_tasks[i].enc_size;
            memcpy(ctx->stream_buf + out_pos, &sz, sizeof(sz)); out_pos += sizeof(sz);
        }
        /* Band data */
        for (int i = 0; i < num_bands; i++) {
            PASS2_BAND_TASK *pt = &p2_tasks[i];
            if (pt->enc_size > 0 && out_pos + pt->enc_size <= ctx->stream_cap) {
                memcpy(ctx->stream_buf + out_pos, pt->enc_buf, pt->enc_size);
                out_pos += pt->enc_size;
            }
        }
        pos = out_pos;
    }

#ifdef FUSED_TIMING
    double t3 = _fused_ms();
    fprintf(stderr, "  FUSED Total:                             %.1fms\n", t3 - t0);
#endif

    *vc5_out = scatter_mode ? NULL : ctx->stream_buf;  /* context-owned; caller must NOT free */
    *vc5_size = pos;
    return rc;
}

int gpr_encode_fused_frame(FUSED_ENCODER *ctx,
                            const uint8_t *raw_bayer, size_t raw_size,
                            uint8_t **vc5_out, size_t *vc5_size)
{
    return gpr_encode_fused_frame_single(ctx, raw_bayer, raw_size,
                                         vc5_out, vc5_size, 0);
}

int gpr_encode_fused_frame_scatter(FUSED_ENCODER *ctx,
                                   const uint8_t *raw_bayer, size_t raw_size,
                                   const uint8_t ***parts,
                                   const size_t **part_sizes,
                                   int *part_count,
                                   size_t *vc5_size)
{
    uint8_t *unused = NULL;
    if (!ctx || !parts || !part_sizes || !part_count || !vc5_size) return -1;
    int rc = gpr_encode_fused_frame_single(ctx, raw_bayer, raw_size,
                                           &unused, vc5_size, 1);
    if (rc != 0) return rc;
    *parts = ctx->scatter_parts;
    *part_sizes = ctx->scatter_part_sizes;
    *part_count = ctx->scatter_part_count;
    return 0;
}

/* ================================================================
   Multi-level path (3-level wavelet decomposition)
   ================================================================ */

/* Run level-2 and level-3 wavelets for a single channel. Called after Pass 1
   completes (so band_data[0] = LL1 is populated). Sequential — the two passes
   chain (LL2 from level-2 is input to level-3). */
typedef struct {
    FUSED_CHANNEL_STATE *cs;
} LEVEL23_TASK;

static void *level23_run_channel(void *arg) {
    LEVEL23_TASK *t = (LEVEL23_TASK *)arg;
    FUSED_CHANNEL_STATE *cs = t->cs;

    /* Level 2: decompose LL1 → LL2/LH2/HL2/HH2 */
    wavelet_decompose_buffer(
        cs->band_data[0], cs->band_width, cs->band_height,
        cs->midpoint_l2, cs->multiplier_l2,
        cs->band_data_l2[0], cs->band_data_l2[1],
        cs->band_data_l2[2], cs->band_data_l2[3]);

    /* Level 3 only runs in 3-level mode. In 2-level mode, LL2 from the
       L2 decomposition above IS the deepest band (emitted directly). */
    if (cs->levels == 3) {
        wavelet_decompose_buffer(
            cs->band_data_l2[0], cs->band_width_l2, cs->band_height_l2,
            cs->midpoint_l3, cs->multiplier_l3,
            cs->band_data_l3[0], cs->band_data_l3[1],
            cs->band_data_l3[2], cs->band_data_l3[3]);
    }

    return NULL;
}

/* Layout of the 40 Pass-2 tasks in multi-level mode:
     For each channel (0..3):
       0: LH1   1: HL1   2: HH1     (level-1 highpass)
       3: LH2   4: HL2   5: HH2     (level-2 highpass)
       6: LH3   7: HL3   8: HH3     (level-3 highpass)
       9: LL3                       (level-3 lowpass — encoded)
   Index = channel * 10 + slot. */
static int gpr_encode_fused_frame_multilevel(FUSED_ENCODER *ctx,
                                              const uint8_t *raw_bayer,
                                              uint8_t **vc5_out,
                                              size_t *vc5_size)
{
    int rc = 0;
    /* Per-channel band count depends on wavelet depth.
       levels==2: 7 (LH1/HL1/HH1, LH2/HL2/HH2, LL2)
       levels==3: 10 (+ LH3/HL3/HH3, LL3 instead of LL2) */
    const int bands_per_ch = (ctx->levels == 2) ? 7 : 10;
    const int p2_tasks_total = bands_per_ch * 4;
    enum { p2_tasks_max = 40 };  /* upper bound for buffer sizing */
    PASS2_BAND_TASK p2_tasks[p2_tasks_max];
    pthread_t       p2_threads[p2_tasks_max];
    int             p2_created[p2_tasks_max] = {0};
    memset(p2_tasks, 0, sizeof(p2_tasks));

    pthread_t p1_threads[4];
    int p1_created[4] = {0};

#ifdef FUSED_TIMING
    double t0 = _fused_ms();
#endif

    const char *threads_env = getenv("FUSED_THREADS");
    int run_serial = (threads_env && threads_env[0] == '1' && threads_env[1] == '\0');

    fused_reset_frame_state(ctx);

    CHANNEL_SYNC sync;
    pthread_mutex_init(&sync.lock, NULL);
    pthread_cond_init(&sync.cv, NULL);
    for (int i = 0; i < 4; i++) sync.p1_done[i] = 0;

    /* ---- Dispatch Pass 1 (level-1 wavelet) ---- */
    PASS1_CHANNEL_TASK p1_tasks[4];
    for (int ch = 0; ch < 4; ch++) {
        p1_tasks[ch].channel = ch;
        p1_tasks[ch].raw_bayer = raw_bayer;
        p1_tasks[ch].width = ctx->width;
        p1_tasks[ch].height = ctx->height;
        p1_tasks[ch].log_bits = ctx->log_bits;
        p1_tasks[ch].is_rggb = ctx->is_rggb;
        p1_tasks[ch].prescale = ctx->prescale;
        p1_tasks[ch].cs = &ctx->ch_state[ch];
        p1_tasks[ch].sync = &sync;
    }
    if (run_serial) {
        for (int ch = 0; ch < 4; ch++) {
            pass1_channel_thread(&p1_tasks[ch]);
        }
    } else {
        for (int ch = 0; ch < 4; ch++) {
            p1_created[ch] = (pthread_create(&p1_threads[ch], NULL,
                                              pass1_channel_thread, &p1_tasks[ch]) == 0);
            if (!p1_created[ch]) pass1_channel_thread(&p1_tasks[ch]);
        }
    }
    for (int ch = 0; ch < 4; ch++) {
        if (p1_created[ch]) pthread_join(p1_threads[ch], NULL);
    }

#ifdef FUSED_TIMING
    double t1 = _fused_ms();
    fprintf(stderr, "  FUSED ML Pass1 (level-1, parallel):       %.1fms\n", t1 - t0);
#endif

    /* ---- Level 2 + Level 3 wavelets per channel (parallel).
       Streaming mode: skip — already run inline in pass1 per channel. */
    if (!ctx->streaming) {
        LEVEL23_TASK l23_tasks[4];
        pthread_t l23_threads[4];
        int l23_created[4] = {0};
        for (int ch = 0; ch < 4; ch++) {
            l23_tasks[ch].cs = &ctx->ch_state[ch];
            if (run_serial) {
                level23_run_channel(&l23_tasks[ch]);
            } else {
                l23_created[ch] = (pthread_create(&l23_threads[ch], NULL,
                                                   level23_run_channel, &l23_tasks[ch]) == 0);
                if (!l23_created[ch]) level23_run_channel(&l23_tasks[ch]);
            }
        }
        for (int ch = 0; ch < 4; ch++) {
            if (l23_created[ch]) pthread_join(l23_threads[ch], NULL);
        }
    }

#ifdef FUSED_TIMING
    double t2 = _fused_ms();
    fprintf(stderr, "  FUSED ML Level23 (%s):     %.1fms\n",
            ctx->streaming ? "streamed in pass1" : "post-pass1 per ch",
            t2 - t1);
#endif

    /* ---- Dispatch Pass 2 for all 40 bands ----
       Layout: per-channel, slot index goes LH1, HL1, HH1, LH2, HL2, HH2,
       LH3, HL3, HH3, LL3. Across channels (4×10 = 40 entries) the L1 highpass
       bands (largest, ~16× the L3 area) are dispatched first, which gives
       longest-job-first scheduling for free in the worker-pool branch below. */
    int p2_count = 0;
    for (int ch = 0; ch < 4; ch++) {
        FUSED_CHANNEL_STATE *cs = &ctx->ch_state[ch];

        typedef struct { PIXEL *data; int width, height; } BAND_SLOT;
        BAND_SLOT slots[10];
        int n_slots;
        if (ctx->levels == 2) {
            /* 2-level: 7 bands per channel — LL2 is the deepest emitted band. */
            slots[0] = (BAND_SLOT){ cs->band_data[1],    cs->band_width,    cs->band_height    };
            slots[1] = (BAND_SLOT){ cs->band_data[2],    cs->band_width,    cs->band_height    };
            slots[2] = (BAND_SLOT){ cs->band_data[3],    cs->band_width,    cs->band_height    };
            slots[3] = (BAND_SLOT){ cs->band_data_l2[1], cs->band_width_l2, cs->band_height_l2 };
            slots[4] = (BAND_SLOT){ cs->band_data_l2[2], cs->band_width_l2, cs->band_height_l2 };
            slots[5] = (BAND_SLOT){ cs->band_data_l2[3], cs->band_width_l2, cs->band_height_l2 };
            slots[6] = (BAND_SLOT){ cs->band_data_l2[0], cs->band_width_l2, cs->band_height_l2 };
            n_slots = 7;
        } else {
            /* 3-level: 10 bands per channel — LL3 is the deepest. */
            slots[0] = (BAND_SLOT){ cs->band_data[1],    cs->band_width,    cs->band_height    };
            slots[1] = (BAND_SLOT){ cs->band_data[2],    cs->band_width,    cs->band_height    };
            slots[2] = (BAND_SLOT){ cs->band_data[3],    cs->band_width,    cs->band_height    };
            slots[3] = (BAND_SLOT){ cs->band_data_l2[1], cs->band_width_l2, cs->band_height_l2 };
            slots[4] = (BAND_SLOT){ cs->band_data_l2[2], cs->band_width_l2, cs->band_height_l2 };
            slots[5] = (BAND_SLOT){ cs->band_data_l2[3], cs->band_width_l2, cs->band_height_l2 };
            slots[6] = (BAND_SLOT){ cs->band_data_l3[1], cs->band_width_l3, cs->band_height_l3 };
            slots[7] = (BAND_SLOT){ cs->band_data_l3[2], cs->band_width_l3, cs->band_height_l3 };
            slots[8] = (BAND_SLOT){ cs->band_data_l3[3], cs->band_width_l3, cs->band_height_l3 };
            slots[9] = (BAND_SLOT){ cs->band_data_l3[0], cs->band_width_l3, cs->band_height_l3 };
            n_slots = 10;
        }

        for (int s = 0; s < n_slots; s++) {
            PASS2_BAND_TASK *pt = &p2_tasks[p2_count];
            pt->channel = ch;
            pt->slot = s;
            pt->band_data = slots[s].data;
            pt->inline_state = NULL;
            pt->width = slots[s].width;
            pt->height = slots[s].height;
            pt->pitch = slots[s].width * sizeof(int32_t);
            pt->enc_cap = ctx->enc_caps_ml[p2_count];
            pt->enc_buf = ctx->enc_bufs_ml[p2_count];
            pt->enc_size = 0;
            pt->sync = NULL;  /* Pass 1 already waited */
            pt->denoise_strength = 0.0;
            pt->noise_scale = 0;
            pt->noise_offset = 0;
            pt->raw_ll_sideband = ctx->raw_ll_sideband;
            pt->pred_ll_sideband = ctx->pred_ll_sideband;
            pt->ll_predictor = ctx->ll_predictor;
            pt->ll_rice_k = ctx->ll_rice_k[ch];
            pt->ll_fast_rice = ctx->ll_fast_rice;
            pt->ll_assume_u16 = ctx->ll_assume_u16;
            p2_count++;
        }
    }

    fused_dump_multilevel_encode_coeffs(ctx->levels, p2_tasks, p2_count);

    /* Two dispatch strategies:
       - Narrow host (<= 4 cores, e.g. Pi 5): use a worker pool sized to nproc,
         so we pay 4× pthread_create + 4× join instead of 40× and the per-task
         scheduler overhead drops. Pass 2 measures ~17% faster on Pi 5.
       - Wide host (> 4 cores, e.g. Apple Silicon): spawn 1 thread per band so
         the OS scheduler spreads work across the full core array. pthread_create
         is cheap on macOS, and the wider parallelism dominates the overhead.
       Override at compile time with -DPASS2_POOL_FORCE=1 (force pool) or =0
       (force spawn-per-task) for benchmarking. */
    if (run_serial) {
        for (int i = 0; i < p2_count; i++) pass2_band_thread(&p2_tasks[i]);
    } else {
#ifdef PASS2_POOL_FORCE
        const int use_pool = PASS2_POOL_FORCE;
#else
        long _nproc = sysconf(_SC_NPROCESSORS_ONLN);
        if (_nproc < 1) _nproc = 4;
        const int use_pool = (_nproc <= 4);
#endif
        if (use_pool) {
            PASS2_WORK_QUEUE wq;
            wq.tasks = p2_tasks;
            wq.order = NULL;
            wq.num_tasks = p2_count;
            wq.next = 0;
            pthread_mutex_init(&wq.lock, NULL);

            int nworkers = (int)
#ifdef PASS2_POOL_FORCE
                4
#else
                _nproc
#endif
                ;
            if (nworkers > PASS2_POOL_MAX) nworkers = PASS2_POOL_MAX;
            if (nworkers > p2_count) nworkers = p2_count;
            if (nworkers < 1) nworkers = 1;

            pthread_t workers[PASS2_POOL_MAX];
            PASS2_WORKER_ARG worker_args[PASS2_POOL_MAX];
            int wcreated[PASS2_POOL_MAX] = {0};
            for (int w = 0; w < nworkers; w++) {
                worker_args[w].wq = &wq;
                worker_args[w].worker_id = w;
                wcreated[w] = (pthread_create(&workers[w], NULL,
                                              pass2_worker_thread,
                                              &worker_args[w]) == 0);
            }
            for (int w = 0; w < nworkers; w++) {
                if (!wcreated[w]) {
                    PASS2_WORKER_ARG wa = { &wq, w };
                    pass2_worker_thread(&wa);  /* drain on this thread */
                    break;
                }
            }
            for (int w = 0; w < nworkers; w++) {
                if (wcreated[w]) pthread_join(workers[w], NULL);
            }
            pthread_mutex_destroy(&wq.lock);
        } else {
            /* Spawn-per-task (wide host). */
            for (int i = 0; i < p2_count; i++) {
                p2_created[i] = (pthread_create(&p2_threads[i], NULL,
                                                pass2_band_thread, &p2_tasks[i]) == 0);
                if (!p2_created[i]) pass2_band_thread(&p2_tasks[i]);
            }
            for (int i = 0; i < p2_count; i++) {
                if (p2_created[i]) pthread_join(p2_threads[i], NULL);
            }
        }
    }

#ifdef FUSED_TIMING
    double t3 = _fused_ms();
    fprintf(stderr, "  FUSED ML Pass2 (40 bands, parallel):      %.1fms\n", t3 - t2);
#endif

    /* ---- Emit header + band manifest + band data ---- */
    FUSED_HEADER hdr;
    hdr.magic = FUSED_MAGIC;
    hdr.version = FUSED_VERSION;
    hdr.width = ctx->width;
    hdr.height = ctx->height;
    hdr.pixel_format = ctx->pixel_format;
    hdr.quality = ctx->quality;
    hdr.is_rggb = ctx->is_rggb;
    hdr.log_bits = ctx->log_bits;
    hdr.prescale = ctx->prescale;
    /* Header records actual wavelet depth (1, 2, or 3). Decoder uses this
       to dispatch the matching inverse cascade. (Backward compat: when
       only single/3-level existed, this field was a bool 0/1; new
       decoders accept 2 here for the 2-level path.) */
    hdr.multi_level = ctx->levels;
    hdr.num_bands = p2_tasks_total;
    /* Decimate flag mirrors the single-level path: when GPR_ROW_DECIMATE=2 AND
       GPR_COL_DECIMATE=2 are set, setup_channel_state has already halved
       ch_width/ch_height, every band is at the smaller dims, and the decoder
       needs hdr.decimate=2 to size buffers correctly. */
    hdr.decimate = (ctx->ch_state[0].row_decimate == 2 &&
                    ctx->ch_state[0].col_decimate == 2) ? 2 : 0;

    size_t pos = 0;
    memcpy(ctx->stream_buf + pos, &hdr, sizeof(hdr)); pos += sizeof(hdr);
    for (int i = 0; i < p2_tasks_total; i++) {
        uint32_t sz = (uint32_t)p2_tasks[i].enc_size;
        memcpy(ctx->stream_buf + pos, &sz, sizeof(sz)); pos += sizeof(sz);
    }
    for (int i = 0; i < p2_tasks_total; i++) {
        PASS2_BAND_TASK *pt = &p2_tasks[i];
        if (pt->enc_size > 0 && pos + pt->enc_size <= ctx->stream_cap) {
            memcpy(ctx->stream_buf + pos, pt->enc_buf, pt->enc_size);
            pos += pt->enc_size;
        }
    }

#ifdef FUSED_TIMING
    double t4 = _fused_ms();
    fprintf(stderr, "  FUSED ML Total:                           %.1fms\n", t4 - t0);
#endif

    *vc5_out = ctx->stream_buf;
    *vc5_size = pos;

cleanup:
    pthread_mutex_destroy(&sync.lock);
    pthread_cond_destroy(&sync.cv);
    /* enc_buf is owned by ctx->enc_bufs_ml — do not free per-frame.
       p2_tasks/threads/created are stack-allocated — no free needed. */
    return rc;
}

/* ================================================================
   One-shot entry point (back-compat: creates context per call)
   ================================================================ */

int gpr_encode_fused(
    const uint8_t *raw_bayer,
    size_t raw_size,
    int width, int height,
    int pixel_format,
    int quality,
    uint8_t **vc5_out,
    size_t *vc5_size)
{
    FUSED_ENCODER *ctx = gpr_encode_fused_create(width, height, pixel_format, quality);
    if (!ctx) return -1;
    uint8_t *internal_out = NULL;
    size_t internal_size = 0;
    int rc = gpr_encode_fused_frame(ctx, raw_bayer, raw_size, &internal_out, &internal_size);
    if (rc == 0) {
        /* Copy the context-owned bytes into a fresh malloc'd buffer for the caller */
        *vc5_out = (uint8_t *)malloc(internal_size);
        if (!*vc5_out) { rc = -1; goto out; }
        memcpy(*vc5_out, internal_out, internal_size);
        *vc5_size = internal_size;
    }
out:
    gpr_encode_fused_destroy(ctx);
    return rc;
}
