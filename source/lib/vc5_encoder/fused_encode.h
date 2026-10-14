/*! @file fused_encode.h
 *
 *  @brief Fused encoder: Bayer→Wavelet→Quantize→FreqCount in one streaming pass.
 *
 *  Replaces the 4-stage serial pipeline with a 2-pass fused design:
 *  Pass 1: Stream Bayer pixels through log curve → horizontal filter → vertical
 *          filter + quantize, with optional inline tokenization (embedded mode).
 *  Pass 2: rANS encode using per-band (or per-stripe) frequency tables.
 *
 *  Designed for GoPro ARM (Cortex-A78), testable on Mac ARM64.
 *
 *  ## Known limitation: single-level wavelet only
 *
 *  This encoder applies ONLY the level-0 wavelet decomposition. The
 *  production GPR encoder (encoder.c) applies 3 levels (LL → LL2 → LL3)
 *  which removes more inter-band correlation. As a result, the fused
 *  encoder produces ~2× larger compressed output than the standard
 *  GPR encoder on the same input — about 24 % of raw vs. 12 % for the
 *  same quality preset.
 *
 *  Trade-off: the fused encoder is much faster and uses far less RAM,
 *  so it's the right choice for:
 *    - Live preview / monitoring streams
 *    - Embedded targets that can't afford the 3-level memory cost
 *    - Burst photography where throughput beats minimum size
 *
 *  For archival or "best compression" workflows, the production encoder
 *  is still preferred.
 *
 *  (C) Copyright 2018 GoPro Inc. Licensed under Apache-2.0 or MIT.
 */

#ifndef FUSED_ENCODE_H
#define FUSED_ENCODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fused-codec self-describing wrapper format. Output starts with this
   fixed header followed by a per-band length table and the band data.
   This is NOT a legacy VC5 bitstream; use the matching FUSED decoder.

   Layout:
     FUSED_HEADER  header
     uint32_t      band_size[num_bands]    -- byte length of each band
     uint8_t       band_data[band_size[0]]
     uint8_t       band_data[band_size[1]]
     ...
*/
#define FUSED_MAGIC    0x44535546u   /* 'FUSD' little-endian */
#define FUSED_VERSION  1

typedef struct {
    uint32_t magic;          /* FUSED_MAGIC */
    uint32_t version;        /* FUSED_VERSION */
    uint32_t width;          /* pixel width  (Bayer pattern width)  */
    uint32_t height;         /* pixel height (Bayer pattern height) */
    uint32_t pixel_format;   /* same encoding as gpr_encode_fused()  */
    uint32_t quality;        /* 0..11 */
    uint32_t is_rggb;        /* 1 = RGGB, 0 = GBRG */
    uint32_t log_bits;       /* 14 for 12/14-bit input, 16 for 16-bit */
    uint32_t prescale;       /* level-1 shift: default 3 for 12/14-bit, 2 for 16-bit */
    uint32_t multi_level;    /* 0 = single; 2/3 = depth; legacy 1 means 3 levels */
    uint32_t num_bands;      /* 12 (single-level no LL), 16 (single-level + LL),
                                28 (two levels), 40 (three levels) */
    uint32_t decimate;       /* Channel-space decimation factor.
                                0 or 1 = none. 2 = 2x2 channel-space
                                decimation (bands and output Bayer are
                                effectively at hdr.width/decimate ×
                                hdr.height/decimate). */
} FUSED_HEADER;

/*!
    @brief Fused encode: raw Bayer pixels to a FUSED payload.

    Two-pass design:
    Pass 1: Fused unpack→wavelet→quantize→frequency count (no intermediate arrays)
    Pass 2: rANS encode using counted frequencies

    @param raw_bayer    Raw Bayer pixel data (uint16_t interleaved)
    @param raw_size     Size of raw data in bytes
    @param width        Image width in pixels
    @param height       Image height in pixels
    @param pixel_format 0=RGGB12, 1=RGGB14, 2=GBRG12, 3=GBRG14, 4=RGGB16, 5=GBRG16.
                        The live FUSED/.gvid path currently supports this
                        RGGB/GBRG unpacked set only. The standard still/GPR
                        encoder path supports GRBG/BGGR separately.
    @param quality      Quality preset (0-11, default 3 = Filmscan-1)
    @param vc5_out      Output: allocated VC5 bitstream (caller must free)
    @param vc5_size     Output: size of VC5 bitstream
    @return 0 on success, -1 on error
*/
int gpr_encode_fused(
    const uint8_t *raw_bayer,
    size_t raw_size,
    int width, int height,
    int pixel_format,
    int quality,
    uint8_t **vc5_out,
    size_t *vc5_size
);

/*!
    @brief Reusable encoder context for video / batch encoding.

    Pre-allocates band buffers, row buffers, and the output stream buffer.
    Subsequent encode_frame calls touch already-faulted pages, eliminating
    the ~5ms-per-frame calloc+page-fault cost.

    Single-thread is the embedded baseline; the same context works with
    parallel mode (the FUSED_THREADS=1 env var still selects serial).

    Lifetime:
        ctx = gpr_encode_fused_create(width, height, pixel_format, quality);
        for each frame: gpr_encode_fused_frame(ctx, raw, ...);
        gpr_encode_fused_destroy(ctx);
*/
typedef struct FUSED_ENCODER FUSED_ENCODER;

FUSED_ENCODER *gpr_encode_fused_create(
    int width, int height,
    int pixel_format,
    int quality);

/*! Encode one frame using a pre-allocated context.
    @param ctx        Encoder context from gpr_encode_fused_create().
    @param raw_bayer  Raw input pixels (uint16_t interleaved).
    @param raw_size   Size of raw data in bytes.
    @param vc5_out    Output pointer (points into ctx-owned buffer; do not free).
    @param vc5_size   Output: size of VC5 bitstream.
    @return 0 on success, -1 on error.
*/
int gpr_encode_fused_frame(
    FUSED_ENCODER *ctx,
    const uint8_t *raw_bayer,
    size_t raw_size,
    uint8_t **vc5_out,
    size_t *vc5_size);

void gpr_encode_fused_destroy(FUSED_ENCODER *ctx);

/*!
    @brief Enable wavelet-domain BayesShrink denoise on the fused encoder.

    Operates on each highpass band between Pass 1 (wavelet+quantize) and
    Pass 2 (tokenize+rANS), thresholding noise-dominated coefficients.
    Measured on the production-encoder equivalent: 3-38% file-size win
    at SSIM 0.9998, signal-detail preserved.

    Enabling denoise forces split-pass mode (the band buffer must exist
    for per-band threshold estimation). With strength=0 (default) the
    encoder behaves as before.

    @param ctx           Encoder context from gpr_encode_fused_create().
    @param noise_scale   Poisson noise component in raw-pixel units
                         (multiply DNG NoiseProfile scale by max_val).
                         Pass 0 to use MAD-based auto-estimation per band.
    @param noise_offset  Gaussian noise component in raw-pixel units
                         (multiply DNG NoiseProfile offset by max_val^2).
    @param strength      Threshold multiplier [0.0-1.0]. 0 disables denoise.
                         1.0 = full BayesShrink strength.
*/
void gpr_encode_fused_set_denoise(FUSED_ENCODER *ctx,
                                  double noise_scale,
                                  double noise_offset,
                                  double strength);

/*  Multiplicative scale on level-1 quant divisors. The video encoder's
    rate controller in gpr_video.c calls this each frame to track a
    bitrate target. 1.0 = preset's nominal divisors. Higher = more
    aggressive quantization (smaller, lower quality). Lower = less
    aggressive. Clamped to [0.25, 16.0].

    Only level-1 (LL/LH/HL/HH) quants are adjusted, matching the legacy
    encoder's behavior. In multi-level mode the L2/L3 quants stay at
    their preset values regardless of RC scale.
*/
void gpr_encode_fused_set_quant_scale(FUSED_ENCODER *ctx, double scale);

#ifdef __cplusplus
}
#endif

#endif /* FUSED_ENCODE_H */
