/*! @file fast_decode.c
 *
 *  @brief Ultra-fast VC5 decoder: pre-index + parallel band decode + wavelet reconstruct.
 *
 *  Replaces the serial tag-by-tag decoder in decoder.c with:
 *  1. Pre-index: single pass over VC5 tags → band offset table (< 1ms)
 *  2. Parallel decode: N threads decode independent ANS bands simultaneously
 *  3. Direct lowpass decode: byte-swap 16-bit big-endian coefficients (no bitstream)
 *  4. Wavelet reconstruction: intermediate levels + final output (parallel per channel)
 *
 *  Entry point: DecodeFastImage() — drop-in replacement for DecodeImage() in vc5_decoder_process.
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *  Licensed under Apache-2.0 or MIT at your option.
 */

#include "headers.h"
#include "ans_joint.h"
#include "logcurve.h"
#include "inverse.h"
#ifndef _WIN32
#include <pthread.h>
#endif

/* When FAST_SINGLE_THREAD is defined, all operations run single-threaded.
   Used for benchmarking single-threaded performance on embedded targets. */
#ifdef FAST_SINGLE_THREAD
#define FAST_NO_THREADS 1
#else
#define FAST_NO_THREADS 0
#endif
#include <string.h>
#include <stdlib.h>

#if ENABLED(NEON)
#include <arm_neon.h>
#endif

/* ================================================================
   Pre-indexer: single fast pass over VC5 tag stream
   ================================================================ */

#define FAST_MAX_BANDS 48  /* 4 channels × 12 bands max */

typedef struct {
    int channel;
    int subband;
    int coding_method;    /* 0=VLC, 1/2=ANS, 3/4=ANS interleaved */
    int quantization;
    int lowpass_precision;
    size_t data_offset;   /* Offset from start of VC5 blob */
    size_t data_size;     /* Codeblock size in bytes */
} FAST_BAND_INFO;

typedef struct {
    int image_width;
    int image_height;
    int channel_count;
    int bits_per_component;
    int max_bits_per_component;
    int prescale_shift;   /* Raw tag 109 value for unpacking */
    int band_count;
    FAST_BAND_INFO bands[FAST_MAX_BANDS];
} FAST_INDEX;

/* StartMarker: "VC-5" = 0x56432D35 */
static const uint32_t VC5_START_MARKER = ((0x56 << 24) | (0x43 << 16) | (0x2D << 8) | 0x35);

/*! Scan the VC5 tag stream and build an index of all band codeblocks.
    Also extracts image metadata needed to set up the decoder. */
static int fast_preindex(const uint8_t *buf, size_t size, FAST_INDEX *idx)
{
    memset(idx, 0, sizeof(*idx));
    idx->channel_count = 4; /* Bayer default */
    idx->bits_per_component = 14; /* Default */

    size_t pos = 0;
    int cur_channel = 0, cur_subband = 0, cur_quant = 1, cur_coding = 0;
    int cur_lowpass_prec = 16;

    /* Skip start marker */
    if (size >= 4) {
        uint32_t marker = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                          ((uint32_t)buf[2] << 8) | buf[3];
        if (marker == VC5_START_MARKER)
            pos = 4;
    }

    while (pos + 4 <= size) {
        uint32_t word = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos+1] << 16) |
                        ((uint32_t)buf[pos+2] << 8) | buf[pos+3];
        pos += 4;

        int16_t raw_tag = (int16_t)(word >> 16);
        int16_t value = (int16_t)(word & 0xFFFF);

        /* Handle optional (negative) tags */
        int tag = raw_tag;
        if (tag < 0) tag = -tag;

        /* VC5 tag classification (after removing optional bit):
           Bit 14 (0x4000) = small chunk (payload follows, e.g., UUID)
           Bit 13 (0x2000) = large chunk section header (channel/header section - parse inline)
           Both (0x6000) = codeblock (band data)
           Neither = regular tag-value pair */

        /* Codeblock: both chunk bits set (0x6000) */
        if ((tag & 0x6000) == 0x6000) {
            /* Compute 24-bit size: (tag & 0xFF) << 16 | value, in 32-bit words */
            size_t chunk_words = ((size_t)(tag & 0xFF) << 16) | ((size_t)(uint16_t)value);
            size_t chunk_bytes = chunk_words * 4;

            if (idx->band_count < FAST_MAX_BANDS) {
                FAST_BAND_INFO *b = &idx->bands[idx->band_count];
                b->channel = cur_channel;
                b->subband = cur_subband;
                b->coding_method = cur_coding;
                b->quantization = cur_quant;
                b->lowpass_precision = cur_lowpass_prec;
                b->data_offset = pos;
                b->data_size = chunk_bytes;
                idx->band_count++;
            }
            cur_coding = 0; /* Reset for next band */
            pos += chunk_bytes;
            continue;
        }

        /* Small chunk (bit 14 only, 0x4000): UUID, inverse transform, etc.
           These have inline payload data that is NOT tags — must skip. */
        if ((tag & 0x4000) && !(tag & 0x2000)) {
            size_t chunk_bytes = (size_t)(uint16_t)value * 4;
            pos += chunk_bytes;
            continue;
        }

        /* Large non-codeblock sections (bit 13 only, 0x2000): channel/header sections.
           These are containers whose content is more tags and codeblocks.
           Do NOT skip — just continue parsing the inline content. */

        /* Parse metadata tags (non-chunk tag-value pairs and section headers) */
        switch (tag) {
            case 20: idx->image_width = value; break;          /* ImageWidth */
            case 21: idx->image_height = value; break;         /* ImageHeight */
            case 12: idx->channel_count = value; break;        /* ChannelCount */
            case 62: cur_channel = value; break;               /* ChannelNumber */
            case 48: cur_subband = value; break;               /* SubbandNumber */
            case 53: cur_quant = value; break;                 /* Quantization */
            case 35: cur_lowpass_prec = value; break;          /* LowpassPrecision */
            case 101: idx->bits_per_component = value; break;  /* BitsPerComponent */
            case 102: idx->max_bits_per_component = value; break; /* MaxBitsPerComponent */
            case 109: idx->prescale_shift = value; break;      /* PrescaleShift */
            case 200: cur_coding = value; break;               /* BandCodingMethod */
            default: break;
        }
    }

    return (idx->band_count > 0) ? 0 : -1;
}

/* ================================================================
   Lowpass band decoder: direct byte-swap from raw buffer
   ================================================================ */

/*! Decode the lowpass band (subband 0) directly from codeblock data.
    Lowpass coefficients are stored as 16-bit big-endian values. */
static void fast_decode_lowpass(const uint8_t *data, size_t data_size,
                                PIXEL *output, int width, int height, int pitch_pixels,
                                int precision)
{
    const uint8_t *src = data;

    if (precision == 16) {
        for (int row = 0; row < height; row++) {
            PIXEL *row_ptr = output + row * pitch_pixels;
            for (int col = 0; col < width; col++) {
                /* Big-endian 16-bit UNSIGNED → int32_t PIXEL.
                   The encoder writes the lowpass coefficient clamped to
                   [0, UINT16_MAX] (see EncodeLowpassBand in encoder.c),
                   so it must be read as unsigned. A previous (int16_t)
                   cast here sign-extended values >= 32768 into negative
                   numbers, which corrupted reconstruction of midpoint-
                   centered channels (GS, RG, BG, GD) whose deepest LL
                   coefficients commonly sit above 32767 due to the
                   cumulative 8× gain of the 3-level wavelet sum-of-sums. */
                row_ptr[col] = (COEFFICIENT)((uint16_t)(src[0] << 8) | src[1]);
                src += 2;
            }
        }
    } else {
        /* Generic: read 'precision' bits per coefficient
           For non-16-bit, fall back to bit-by-bit (rare) */
        for (int row = 0; row < height; row++) {
            PIXEL *row_ptr = output + row * pitch_pixels;
            for (int col = 0; col < width; col++) {
                /* Assume byte-aligned for simplicity */
                int val = 0;
                int bits_left = precision;
                while (bits_left >= 8) {
                    val = (val << 8) | *src++;
                    bits_left -= 8;
                }
                if (bits_left > 0) {
                    val = (val << bits_left) | (*src >> (8 - bits_left));
                }
                row_ptr[col] = (COEFFICIENT)val;
            }
        }
    }
}

/* ================================================================
   Parallel ANS band decode
   ================================================================ */

typedef struct {
    const uint8_t *vc5_base;     /* Base of VC5 blob */
    FAST_BAND_INFO *band;        /* Band to decode */
    PIXEL *output;               /* Pre-allocated wavelet band buffer */
    int width;                   /* Band width */
    int height;                  /* Band height */
    int pitch;                   /* Output pitch in bytes */
    int error;                   /* 0=ok, -1=fatal */
} FAST_ANS_ARG;

static void *fast_ans_decode_thread(void *arg)
{
    FAST_ANS_ARG *a = (FAST_ANS_ARG *)arg;
    FAST_BAND_INFO *b = a->band;
    a->error = 0;

    const uint8_t *cblock = a->vc5_base + b->data_offset;
    size_t cblock_size = b->data_size;

    if (cblock_size < 4) { a->error = -1; return NULL; }

    /* First 4 bytes of codeblock = ANS blob size (big-endian uint32) */
    uint32_t ans_size = ((uint32_t)cblock[0] << 24) | ((uint32_t)cblock[1] << 16) |
                        ((uint32_t)cblock[2] << 8) | cblock[3];
    const uint8_t *ans_blob = cblock + 4;

    if (4 + ans_size > cblock_size) { a->error = -1; return NULL; }

    int rc;
    if (b->coding_method >= 3)
        rc = jans_decode_band_x4(ans_blob, ans_size,
                                  (int32_t *)a->output,
                                  a->width, a->height, a->pitch);
    else
        rc = jans_decode_band(ans_blob, ans_size,
                               (int32_t *)a->output,
                               a->width, a->height, a->pitch);

    if (rc != 0) a->error = -1;
    return NULL;
}

/* ================================================================
   VLC band decode (serial, using existing codebook)
   ================================================================ */

/* Use the standard decoder codebook for VLC fallback */
#include "codebooks.h"

static int fast_decode_vlc_band(const uint8_t *vc5_buf, FAST_BAND_INFO *band,
                                PIXEL *output, int width, int height, int pitch)
{
    /* Set up a memory stream at the band's codeblock offset */
    STREAM stream;
    BITSTREAM bitstream;

    /* Create a memory stream starting at the codeblock data */
    const uint8_t *cblock_start = vc5_buf + band->data_offset;
    size_t cblock_size = band->data_size;

    memset(&stream, 0, sizeof(stream));
    stream.type = STREAM_TYPE_MEMORY;
    stream.location.memory.buffer = (void *)cblock_start;
    stream.location.memory.size = cblock_size;
    stream.byte_count = 0;

    InitBitstream(&bitstream);
    AttachBitstream(&bitstream, &stream);

    CODEBOOK *codebook = (CODEBOOK *)decoder_codeset_17.codebook;
    if (!codebook) return -1;

    CODEC_ERROR err = DecodeBandRuns(&bitstream, codebook, output,
                                     (DIMENSION)width, (DIMENSION)height, (DIMENSION)pitch);

    ReleaseBitstream(&bitstream);
    return (err == CODEC_ERROR_OKAY) ? 0 : -1;
}

/* ================================================================
   Wavelet tree construction and intermediate reconstruction
   ================================================================ */

/* Thread argument for full per-channel wavelet reconstruction.
   Each thread handles ALL 3 levels of inverse transform for one channel:
   wavelet[2] → wavelet[1].LL → wavelet[0].LL → output */
typedef struct {
    gpr_allocator *allocator;
    WAVELET *wavelets[MAX_WAVELET_COUNT]; /* All wavelet levels for this channel */
    PRESCALE prescale_table[MAX_WAVELET_COUNT];
    COMPONENT_VALUE *output_data;
    DIMENSION output_width;
    DIMENSION output_height;
    size_t output_pitch;
    CODEC_ERROR error;
} FAST_CHANNEL_RECON_ARG;

static void *fast_channel_recon_thread(void *arg)
{
    FAST_CHANNEL_RECON_ARG *a = (FAST_CHANNEL_RECON_ARG *)arg;
    a->error = CODEC_ERROR_OKAY;

    /* Level 2 → Level 1 lowpass */
    if (BandsAllValid(a->wavelets[2])) {
        TransformInverseSpatialQuantLowpass(a->allocator,
                                            a->wavelets[2], a->wavelets[1],
                                            a->prescale_table[2]);
        UpdateWaveletValidBandMask(a->wavelets[1], 0);
    }

    /* Level 1 → Level 0 lowpass */
    if (BandsAllValid(a->wavelets[1])) {
        TransformInverseSpatialQuantLowpass(a->allocator,
                                            a->wavelets[1], a->wavelets[0],
                                            a->prescale_table[1]);
        UpdateWaveletValidBandMask(a->wavelets[0], 0);
    }

    /* Level 0 → final output */
    if (BandsAllValid(a->wavelets[0])) {
        a->error = TransformInverseSpatialQuantArray(a->allocator,
                                                      a->wavelets[0],
                                                      a->output_data,
                                                      a->output_width,
                                                      a->output_height,
                                                      a->output_pitch,
                                                      a->prescale_table[0]);
    }

    return NULL;
}

/* ================================================================
   Piecewise linear log curve approximation for NEON
   ================================================================ */

/* 256-segment piecewise linear approximation of the decoder log curve.
   Max error: 19 out of 65535 (0.03%). 2KB table fits in L1 cache.
   Enables full NEON vectorization of the log curve (no scalar LUT lookups). */
typedef struct { int32_t offset; int32_t slope; } LOG_APPROX_SEG;
static LOG_APPROX_SEG log_approx_14[256];
static LOG_APPROX_SEG log_approx_16[1024];
static int log_approx_initialized = 0;

static void init_log_approx(void)
{
    if (log_approx_initialized) return;

    /* 14-bit: 256 segments, 64-entry spacing, 2KB table */
    {
        int max_in = (1 << 14) - 1;
        int seg_size = (max_in + 1) / 256;
        for (int i = 0; i < 256; i++) {
            int x0 = i * seg_size;
            int x1 = x0 + seg_size;
            if (x1 > max_in) x1 = max_in;
            int y0 = DecoderLogCurve14[x0];
            int y1 = DecoderLogCurve14[x1];
            log_approx_14[i].offset = y0;
            log_approx_14[i].slope = (int32_t)(((int64_t)(y1 - y0) * 65536 + seg_size/2) / seg_size);
        }
    }

    /* 16-bit: 1024 segments, 64-entry spacing, 8KB table, max_err=5 */
    {
        int max_in = (1 << 16) - 1;
        int seg_size = (max_in + 1) / 1024;
        for (int i = 0; i < 1024; i++) {
            int x0 = i * seg_size;
            int x1 = x0 + seg_size;
            if (x1 > max_in) x1 = max_in;
            int y0 = DecoderLogCurve16[x0];
            int y1 = DecoderLogCurve16[x1];
            log_approx_16[i].offset = y0;
            log_approx_16[i].slope = (int32_t)(((int64_t)(y1 - y0) * 65536 + seg_size/2) / seg_size);
        }
    }

    log_approx_initialized = 1;
}

#if ENABLED(NEON)
/* NEON log curve evaluation using piecewise linear approx (14-bit). */
static INLINE int32x4_t log_curve_neon_pwl(int32x4_t x, const LOG_APPROX_SEG *table, int max_seg)
{
    int32_t idx[4], f[4];
    vst1q_s32(idx, vshrq_n_s32(x, 6));
    vst1q_s32(f, vandq_s32(x, vdupq_n_s32(63)));

    int32_t results[4];
    for (int k = 0; k < 4; k++) {
        int i = idx[k];
        if (i < 0) i = 0;
        if (i >= max_seg) i = max_seg - 1;
        results[k] = table[i].offset +
                     (int32_t)(((int64_t)table[i].slope * f[k] + 32768) >> 16);
    }
    return vld1q_s32(results);
}

/* NEON log curve via 6th-order float32 polynomial (16-bit).
   Max error: 27 out of 65535 (0.04%). Fully vectorized — no scalar LUT.
   Horner's method: ((((c6*x+c5)*x+c4)*x+c3)*x+c2)*x+c1)*x+c0 */
static INLINE int32x4_t log_curve_neon_poly16(int32x4_t xi)
{
    /* Normalize to [0, 1] range as float */
    float32x4_t x = vcvtq_f32_s32(xi);
    const float32x4_t inv_max = vdupq_n_f32(1.0f / 65535.0f);
    x = vmulq_f32(x, inv_max);

    /* 6th-order polynomial (max_err=27/65535 = 0.04%) — Horner evaluation */
    float32x4_t y = vdupq_n_f32(115930.82f);         /* c6 */
    y = vmlaq_f32(vdupq_n_f32(-196522.70f), y, x);   /* c6*x + c5 */
    y = vmlaq_f32(vdupq_n_f32(183116.82f), y, x);    /* *x + c4 */
    y = vmlaq_f32(vdupq_n_f32(-58613.95f), y, x);    /* *x + c3 */
    y = vmlaq_f32(vdupq_n_f32(19850.53f), y, x);     /* *x + c2 */
    y = vmlaq_f32(vdupq_n_f32(1727.87f), y, x);      /* *x + c1 */
    y = vmlaq_f32(vdupq_n_f32(19.28f), y, x);        /* *x + c0 */

    /* Clamp and convert back to int32 */
    y = vmaxq_f32(y, vdupq_n_f32(0.0f));
    y = vminq_f32(y, vdupq_n_f32(65535.0f));
    return vcvtq_s32_f32(vaddq_f32(y, vdupq_n_f32(0.5f)));
}
#endif

/* ================================================================
   Parallel image packing (row-striped)
   ================================================================ */

typedef struct {
    const UNPACKED_IMAGE *image;
    PACKED_IMAGE *packed;
    const DECODER_PARAMETERS *params;
    int row_start;    /* First row (in Bayer half-rows) */
    int row_end;      /* Last row (exclusive) */
} FAST_PACK_ARG;

static void *fast_pack_thread(void *arg)
{
    FAST_PACK_ARG *a = (FAST_PACK_ARG *)arg;
    const UNPACKED_IMAGE *image = a->image;
    PACKED_IMAGE *packed = a->packed;

    DIMENSION width = packed->width / 2;   /* Bayer pattern units */
    size_t output_pitch = packed->pitch * 2;
    PIXEL_FORMAT output_format = packed->format;
    int output_bit_depth = 14;

    /* Determine output bit depth from format */
    switch (output_format) {
        case PIXEL_FORMAT_RAW_RGGB_12: case PIXEL_FORMAT_RAW_GBRG_12: output_bit_depth = 12; break;
        case PIXEL_FORMAT_RAW_RGGB_14: case PIXEL_FORMAT_RAW_GBRG_14: output_bit_depth = 14; break;
        case PIXEL_FORMAT_RAW_RGGB_16: case PIXEL_FORMAT_RAW_GBRG_16: output_bit_depth = 16; break;
        default: break;
    }

    int rggb_order = (output_format == PIXEL_FORMAT_RAW_RGGB_12 ||
                      output_format == PIXEL_FORMAT_RAW_RGGB_14 ||
                      output_format == PIXEL_FORMAT_RAW_RGGB_16) ? 1 : 0;

    int log_bits = image->component_array_list[0].bits_per_component;
    if (log_bits <= 0) log_bits = 12;
    if (log_bits > 16) log_bits = 16;

    /* Use appropriate log curve table */
    uint16_t *log_table;
    int log_max;
    if (log_bits <= 12) { log_table = DecoderLogCurve12; log_max = (1 << 12) - 1; }
    else if (log_bits <= 14) { log_table = DecoderLogCurve14; log_max = (1 << 14) - 1; }
    else { log_table = DecoderLogCurve16; log_max = (1 << 16) - 1; }

    const int32_t midpoint = 1 << (log_bits - 1);
    const int shift = 16 - output_bit_depth;
    size_t output_half_pitch = output_pitch / 2;
    int bypass = vc5_logcurve_bypass();

    for (int row = a->row_start; row < a->row_end; row++)
    {
        COMPONENT_VALUE *GS_row = (COMPONENT_VALUE *)((uintptr_t)image->component_array_list[0].data + row * image->component_array_list[0].pitch);
        COMPONENT_VALUE *RG_row = (COMPONENT_VALUE *)((uintptr_t)image->component_array_list[1].data + row * image->component_array_list[1].pitch);
        COMPONENT_VALUE *BG_row = (COMPONENT_VALUE *)((uintptr_t)image->component_array_list[2].data + row * image->component_array_list[2].pitch);
        COMPONENT_VALUE *GD_row = (COMPONENT_VALUE *)((uintptr_t)image->component_array_list[3].data + row * image->component_array_list[3].pitch);

        uint8_t *output_row_ptr = (uint8_t *)packed->buffer + row * output_pitch;
        uint16_t *out_row1 = (uint16_t *)output_row_ptr;
        uint16_t *out_row2 = (uint16_t *)(output_row_ptr + output_half_pitch);

        int col = 0;

#if ENABLED(NEON)
        {
            const int32x4_t v_zero = vdupq_n_s32(0);
            const int32x4_t v_max = vdupq_n_s32(log_max);
            const int32x4_t v_mid = vdupq_n_s32(midpoint);
            const int width_m4 = ((int)width / 4) * 4;

            for (; col < width_m4; col += 4)
            {
                /* Load 4 pixels from each component */
                int32x4_t gs = vld1q_s32(&GS_row[col]);
                int32x4_t rg = vld1q_s32(&RG_row[col]);
                int32x4_t bg = vld1q_s32(&BG_row[col]);
                int32x4_t gd = vld1q_s32(&GD_row[col]);

                /* Clamp to [0, log_max] */
                gs = vmaxq_s32(vminq_s32(gs, v_max), v_zero);
                rg = vmaxq_s32(vminq_s32(rg, v_max), v_zero);
                bg = vmaxq_s32(vminq_s32(bg, v_max), v_zero);
                gd = vmaxq_s32(vminq_s32(gd, v_max), v_zero);

                /* Subtract midpoint from color differences */
                rg = vsubq_s32(rg, v_mid);
                bg = vsubq_s32(bg, v_mid);
                gd = vsubq_s32(gd, v_mid);

                /* Color conversion: R=(RG<<1)+GS, B=(BG<<1)+GS, G1=GS+GD, G2=GS-GD */
                int32x4_t r  = vaddq_s32(vshlq_n_s32(rg, 1), gs);
                int32x4_t b  = vaddq_s32(vshlq_n_s32(bg, 1), gs);
                int32x4_t g1 = vaddq_s32(gs, gd);
                int32x4_t g2 = vsubq_s32(gs, gd);

                /* Clamp results to [0, log_max] */
                r  = vmaxq_s32(vminq_s32(r,  v_max), v_zero);
                g1 = vmaxq_s32(vminq_s32(g1, v_max), v_zero);
                g2 = vmaxq_s32(vminq_s32(g2, v_max), v_zero);
                b  = vmaxq_s32(vminq_s32(b,  v_max), v_zero);

                /* Apply log curve: NEON piecewise linear for 14-bit, scalar LUT fallback */
                int32_t ra[4], g1a[4], g2a[4], ba[4];
                if (!bypass && log_bits == 16) {
                    /* NEON 6th-order float polynomial — fully vectorized, no LUT */
                    int32x4_t neg_shift = vdupq_n_s32(-shift);
                    int32x4_t r_log  = vshlq_s32(log_curve_neon_poly16(r), neg_shift);
                    int32x4_t g1_log = vshlq_s32(log_curve_neon_poly16(g1), neg_shift);
                    int32x4_t g2_log = vshlq_s32(log_curve_neon_poly16(g2), neg_shift);
                    int32x4_t b_log  = vshlq_s32(log_curve_neon_poly16(b), neg_shift);
                    vst1q_s32(ra, r_log); vst1q_s32(g1a, g1_log);
                    vst1q_s32(g2a, g2_log); vst1q_s32(ba, b_log);
                } else if (!bypass && log_bits == 14) {
                    /* NEON piecewise linear approximation — 14-bit only (2KB table) */
                    const LOG_APPROX_SEG *tbl = log_approx_14;
                    int max_seg = 256;
                    int32x4_t neg_shift = vdupq_n_s32(-shift);
                    int32x4_t r_log  = vshlq_s32(log_curve_neon_pwl(r, tbl, max_seg), neg_shift);
                    int32x4_t g1_log = vshlq_s32(log_curve_neon_pwl(g1, tbl, max_seg), neg_shift);
                    int32x4_t g2_log = vshlq_s32(log_curve_neon_pwl(g2, tbl, max_seg), neg_shift);
                    int32x4_t b_log  = vshlq_s32(log_curve_neon_pwl(b, tbl, max_seg), neg_shift);
                    vst1q_s32(ra, r_log); vst1q_s32(g1a, g1_log);
                    vst1q_s32(g2a, g2_log); vst1q_s32(ba, b_log);
                } else if (!bypass) {
                    /* Scalar LUT for non-14-bit */
                    vst1q_s32(ra, r); vst1q_s32(g1a, g1); vst1q_s32(g2a, g2); vst1q_s32(ba, b);
                    for (int k = 0; k < 4; k++) {
                        ra[k] = log_table[ra[k]] >> shift; g1a[k] = log_table[g1a[k]] >> shift;
                        g2a[k] = log_table[g2a[k]] >> shift; ba[k] = log_table[ba[k]] >> shift;
                    }
                } else {
                    vst1q_s32(ra, r); vst1q_s32(g1a, g1); vst1q_s32(g2a, g2); vst1q_s32(ba, b);
                }

                for (int k = 0; k < 4; k++) {
                    int c = col + k;
                    if (rggb_order) {
                        out_row1[2*c]   = (uint16_t)ra[k];
                        out_row1[2*c+1] = (uint16_t)g1a[k];
                        out_row2[2*c]   = (uint16_t)g2a[k];
                        out_row2[2*c+1] = (uint16_t)ba[k];
                    } else {
                        out_row1[2*c]   = (uint16_t)g1a[k];
                        out_row1[2*c+1] = (uint16_t)ba[k];
                        out_row2[2*c]   = (uint16_t)ra[k];
                        out_row2[2*c+1] = (uint16_t)g2a[k];
                    }
                }
            }
        }
#endif

        /* Scalar cleanup */
        for (; col < (int)width; col++)
        {
            int32_t GS = GS_row[col], RG = RG_row[col], BG = BG_row[col], GD = GD_row[col];
            if (GS < 0) GS = 0; else if (GS > log_max) GS = log_max;
            if (RG < 0) RG = 0; else if (RG > log_max) RG = log_max;
            if (BG < 0) BG = 0; else if (BG > log_max) BG = log_max;
            if (GD < 0) GD = 0; else if (GD > log_max) GD = log_max;
            GD -= midpoint; RG -= midpoint; BG -= midpoint;
            int32_t R  = (RG << 1) + GS, B  = (BG << 1) + GS;
            int32_t G1 = GS + GD,        G2 = GS - GD;
            if (R < 0) R = 0; else if (R > log_max) R = log_max;
            if (G1 < 0) G1 = 0; else if (G1 > log_max) G1 = log_max;
            if (G2 < 0) G2 = 0; else if (G2 > log_max) G2 = log_max;
            if (B < 0) B = 0; else if (B > log_max) B = log_max;
            if (!bypass) { R = log_table[R]>>shift; G1 = log_table[G1]>>shift; G2 = log_table[G2]>>shift; B = log_table[B]>>shift; }
            if (rggb_order) {
                out_row1[2*col] = (uint16_t)R; out_row1[2*col+1] = (uint16_t)G1;
                out_row2[2*col] = (uint16_t)G2; out_row2[2*col+1] = (uint16_t)B;
            } else {
                out_row1[2*col] = (uint16_t)G1; out_row1[2*col+1] = (uint16_t)B;
                out_row2[2*col] = (uint16_t)R; out_row2[2*col+1] = (uint16_t)G2;
            }
        }
    }
    return NULL;
}

/* ================================================================
   Fused pack helper: color convert + log curve + Bayer store (NEON)
   ================================================================ */

static void fused_pack_row(uint16_t *out_r1, uint16_t *out_r2,
                           const PIXEL *gs_row, const PIXEL *rg_row,
                           const PIXEL *bg_row, const PIXEL *gd_row,
                           int width, int log_max, int32_t midpoint,
                           int shift, int bypass, int rggb_order,
                           int log_bits, uint16_t *log_table)
{
    int col = 0;
#if ENABLED(NEON)
    {
        const int32x4_t v_zero = vdupq_n_s32(0);
        const int32x4_t v_max = vdupq_n_s32(log_max);
        const int32x4_t v_mid = vdupq_n_s32(midpoint);
        const int width_m4 = (width / 4) * 4;
        for (; col < width_m4; col += 4) {
            int32x4_t vgs = vmaxq_s32(vminq_s32(vld1q_s32(&gs_row[col]), v_max), v_zero);
            int32x4_t vrg = vsubq_s32(vmaxq_s32(vminq_s32(vld1q_s32(&rg_row[col]), v_max), v_zero), v_mid);
            int32x4_t vbg = vsubq_s32(vmaxq_s32(vminq_s32(vld1q_s32(&bg_row[col]), v_max), v_zero), v_mid);
            int32x4_t vgd = vsubq_s32(vmaxq_s32(vminq_s32(vld1q_s32(&gd_row[col]), v_max), v_zero), v_mid);
            int32x4_t r  = vmaxq_s32(vminq_s32(vaddq_s32(vshlq_n_s32(vrg, 1), vgs), v_max), v_zero);
            int32x4_t b  = vmaxq_s32(vminq_s32(vaddq_s32(vshlq_n_s32(vbg, 1), vgs), v_max), v_zero);
            int32x4_t g1 = vmaxq_s32(vminq_s32(vaddq_s32(vgs, vgd), v_max), v_zero);
            int32x4_t g2 = vmaxq_s32(vminq_s32(vsubq_s32(vgs, vgd), v_max), v_zero);
            int32_t ra[4], g1a[4], g2a[4], ba[4];
            if (!bypass && log_bits == 16) {
                /* Dual polynomial evaluation: process R+G1 and G2+B simultaneously
                   to keep the FMLA pipeline saturated with independent chains */
                float32x4_t inv = vdupq_n_f32(1.0f / 65535.0f);
                float32x4_t xr  = vmulq_f32(vcvtq_f32_s32(r), inv);
                float32x4_t xg1 = vmulq_f32(vcvtq_f32_s32(g1), inv);
                float32x4_t xg2 = vmulq_f32(vcvtq_f32_s32(g2), inv);
                float32x4_t xb  = vmulq_f32(vcvtq_f32_s32(b), inv);

                /* Interleaved Horner: process (r,g2) and (g1,b) in alternating FMLA */
                float32x4_t yr  = vdupq_n_f32(115930.82f);
                float32x4_t yg2 = vdupq_n_f32(115930.82f);
                float32x4_t yg1 = vdupq_n_f32(115930.82f);
                float32x4_t yb  = vdupq_n_f32(115930.82f);

                #define STEP(c) \
                    yr  = vmlaq_f32(vdupq_n_f32(c), yr, xr); \
                    yg2 = vmlaq_f32(vdupq_n_f32(c), yg2, xg2); \
                    yg1 = vmlaq_f32(vdupq_n_f32(c), yg1, xg1); \
                    yb  = vmlaq_f32(vdupq_n_f32(c), yb, xb);
                STEP(-196522.70f)
                STEP(183116.82f)
                STEP(-58613.95f)
                STEP(19850.53f)
                STEP(1727.87f)
                STEP(19.28f)
                #undef STEP

                float32x4_t fmax = vdupq_n_f32(65535.0f);
                float32x4_t fzero = vdupq_n_f32(0.0f);
                float32x4_t fhalf = vdupq_n_f32(0.5f);
                int32x4_t ns = vdupq_n_s32(-shift);
                vst1q_s32(ra,  vshlq_s32(vcvtq_s32_f32(vaddq_f32(vminq_f32(vmaxq_f32(yr,fzero),fmax),fhalf)), ns));
                vst1q_s32(g1a, vshlq_s32(vcvtq_s32_f32(vaddq_f32(vminq_f32(vmaxq_f32(yg1,fzero),fmax),fhalf)), ns));
                vst1q_s32(g2a, vshlq_s32(vcvtq_s32_f32(vaddq_f32(vminq_f32(vmaxq_f32(yg2,fzero),fmax),fhalf)), ns));
                vst1q_s32(ba,  vshlq_s32(vcvtq_s32_f32(vaddq_f32(vminq_f32(vmaxq_f32(yb,fzero),fmax),fhalf)), ns));
            } else if (!bypass && log_bits == 14) {
                int32x4_t ns = vdupq_n_s32(-shift);
                vst1q_s32(ra,  vshlq_s32(log_curve_neon_pwl(r, log_approx_14, 256), ns));
                vst1q_s32(g1a, vshlq_s32(log_curve_neon_pwl(g1, log_approx_14, 256), ns));
                vst1q_s32(g2a, vshlq_s32(log_curve_neon_pwl(g2, log_approx_14, 256), ns));
                vst1q_s32(ba,  vshlq_s32(log_curve_neon_pwl(b, log_approx_14, 256), ns));
            } else if (!bypass) {
                vst1q_s32(ra, r); vst1q_s32(g1a, g1); vst1q_s32(g2a, g2); vst1q_s32(ba, b);
                for (int k = 0; k < 4; k++) {
                    ra[k] = log_table[ra[k]] >> shift;
                    g1a[k] = log_table[g1a[k]] >> shift;
                    g2a[k] = log_table[g2a[k]] >> shift;
                    ba[k] = log_table[ba[k]] >> shift;
                }
            } else {
                vst1q_s32(ra, r); vst1q_s32(g1a, g1); vst1q_s32(g2a, g2); vst1q_s32(ba, b);
            }
            for (int k = 0; k < 4; k++) {
                int c = col + k;
                if (rggb_order) {
                    out_r1[2*c] = (uint16_t)ra[k]; out_r1[2*c+1] = (uint16_t)g1a[k];
                    out_r2[2*c] = (uint16_t)g2a[k]; out_r2[2*c+1] = (uint16_t)ba[k];
                } else {
                    out_r1[2*c] = (uint16_t)g1a[k]; out_r1[2*c+1] = (uint16_t)ba[k];
                    out_r2[2*c] = (uint16_t)ra[k]; out_r2[2*c+1] = (uint16_t)g2a[k];
                }
            }
        }
    }
#endif
    for (; col < width; col++) {
        int32_t GS = gs_row[col], RG = rg_row[col], BG = bg_row[col], GD = gd_row[col];
        if (GS < 0) GS = 0; else if (GS > log_max) GS = log_max;
        if (RG < 0) RG = 0; else if (RG > log_max) RG = log_max;
        if (BG < 0) BG = 0; else if (BG > log_max) BG = log_max;
        if (GD < 0) GD = 0; else if (GD > log_max) GD = log_max;
        RG -= midpoint; BG -= midpoint; GD -= midpoint;
        int32_t R = (RG << 1) + GS, B = (BG << 1) + GS;
        int32_t G1 = GS + GD, G2 = GS - GD;
        if (R < 0) R = 0; else if (R > log_max) R = log_max;
        if (G1 < 0) G1 = 0; else if (G1 > log_max) G1 = log_max;
        if (G2 < 0) G2 = 0; else if (G2 > log_max) G2 = log_max;
        if (B < 0) B = 0; else if (B > log_max) B = log_max;
        if (!bypass) { R = log_table[R]>>shift; G1 = log_table[G1]>>shift; G2 = log_table[G2]>>shift; B = log_table[B]>>shift; }
        if (rggb_order) {
            out_r1[2*col] = (uint16_t)R; out_r1[2*col+1] = (uint16_t)G1;
            out_r2[2*col] = (uint16_t)G2; out_r2[2*col+1] = (uint16_t)B;
        } else {
            out_r1[2*col] = (uint16_t)G1; out_r1[2*col+1] = (uint16_t)B;
            out_r2[2*col] = (uint16_t)R; out_r2[2*col+1] = (uint16_t)G2;
        }
    }
}

/* ================================================================
   Fused wavelet level-0 reconstruct + color convert + pack
   ================================================================ */

/*!
    @brief Fused final-level wavelet reconstruction + color convert + log curve + Bayer pack.

    Instead of reconstructing 4 channels to intermediate arrays and then packing them,
    this function processes all 4 channels row-by-row: for each output row pair, it runs
    the vertical + horizontal inverse wavelet filters for all 4 channels, then immediately
    color-converts, applies the log curve, and writes directly to the packed Bayer output.

    This eliminates the separate pack phase entirely and halves L2 cache pressure.
*/
/* NEON helpers for fused path vertical filter */
#if ENABLED(NEON)
static INLINE int32x4_t FusedDequant4(const PIXEL *src, int col, int32x4_t quant_vec) {
    int32x4_t v = vld1q_s32(&src[col]);
    int32x4_t abs_v = vabsq_s32(v);
    int32x4_t dq = vmulq_s32(abs_v, quant_vec);
    uint32x4_t neg = vcltq_s32(v, vdupq_n_s32(0));
    return vbslq_s32(neg, vnegq_s32(dq), dq);
}
static INLINE void FusedVertMid8(
    const PIXEL *r0, const PIXEL *r1, const PIXEL *r2,
    const PIXEL *hp, PIXEL *ev_out, PIXEL *od_out, int col) {
    const int32x4_t four = vdupq_n_s32(4);
    int32x4_t r0a=vld1q_s32(&r0[col]),r0b=vld1q_s32(&r0[col+4]);
    int32x4_t r1a=vld1q_s32(&r1[col]),r1b=vld1q_s32(&r1[col+4]);
    int32x4_t r2a=vld1q_s32(&r2[col]),r2b=vld1q_s32(&r2[col+4]);
    int32x4_t ha=vld1q_s32(&hp[col]),hb=vld1q_s32(&hp[col+4]);
    int32x4_t da=vshrq_n_s32(vaddq_s32(vsubq_s32(r0a,r2a),four),3);
    int32x4_t db=vshrq_n_s32(vaddq_s32(vsubq_s32(r0b,r2b),four),3);
    vst1q_s32(&ev_out[col],vshrq_n_s32(vaddq_s32(vaddq_s32(da,r1a),ha),1));
    vst1q_s32(&ev_out[col+4],vshrq_n_s32(vaddq_s32(vaddq_s32(db,r1b),hb),1));
    int32x4_t oa=vshrq_n_s32(vaddq_s32(vsubq_s32(r2a,r0a),four),3);
    int32x4_t ob=vshrq_n_s32(vaddq_s32(vsubq_s32(r2b,r0b),four),3);
    vst1q_s32(&od_out[col],vshrq_n_s32(vsubq_s32(vaddq_s32(oa,r1a),ha),1));
    vst1q_s32(&od_out[col+4],vshrq_n_s32(vsubq_s32(vaddq_s32(ob,r1b),hb),1));
}
static INLINE void FusedVertMid8_Dq(
    const PIXEL *r0, const PIXEL *r1, const PIXEL *r2,
    const PIXEL *hp_raw, int32x4_t hq, PIXEL *ev_out, PIXEL *od_out, int col) {
    const int32x4_t four = vdupq_n_s32(4);
    int32x4_t r0a=vld1q_s32(&r0[col]),r0b=vld1q_s32(&r0[col+4]);
    int32x4_t r1a=vld1q_s32(&r1[col]),r1b=vld1q_s32(&r1[col+4]);
    int32x4_t r2a=vld1q_s32(&r2[col]),r2b=vld1q_s32(&r2[col+4]);
    int32x4_t ha=FusedDequant4(hp_raw,col,hq),hb=FusedDequant4(hp_raw,col+4,hq);
    int32x4_t da=vshrq_n_s32(vaddq_s32(vsubq_s32(r0a,r2a),four),3);
    int32x4_t db=vshrq_n_s32(vaddq_s32(vsubq_s32(r0b,r2b),four),3);
    vst1q_s32(&ev_out[col],vshrq_n_s32(vaddq_s32(vaddq_s32(da,r1a),ha),1));
    vst1q_s32(&ev_out[col+4],vshrq_n_s32(vaddq_s32(vaddq_s32(db,r1b),hb),1));
    int32x4_t oa=vshrq_n_s32(vaddq_s32(vsubq_s32(r2a,r0a),four),3);
    int32x4_t ob=vshrq_n_s32(vaddq_s32(vsubq_s32(r2b,r0b),four),3);
    vst1q_s32(&od_out[col],vshrq_n_s32(vsubq_s32(vaddq_s32(oa,r1a),ha),1));
    vst1q_s32(&od_out[col+4],vshrq_n_s32(vsubq_s32(vaddq_s32(ob,r1b),hb),1));
}
#endif

static CODEC_ERROR ReconstructAndPackDirect(
    gpr_allocator *allocator,
    WAVELET *wav[4],                 /* wavelet[0] for each of 4 channels (GS,RG,BG,GD) */
    PRESCALE prescale,               /* prescale for level 0 */
    IMAGE *packed_image,             /* output packed Bayer image */
    int bits_per_component,          /* e.g. 14 or 16 */
    PIXEL_FORMAT output_format)      /* e.g. PIXEL_FORMAT_RAW_RGGB_14 */
{
    DIMENSION input_width  = wav[0]->width;
    DIMENSION input_height = wav[0]->height;
    DIMENSION output_width = input_width * 2;
    DIMENSION output_height_ch = input_height * 2;  /* per-channel output height */

    int last_row = input_height - 1;

    /* Determine output params */
    int output_bit_depth = 14;
    switch (output_format) {
        case PIXEL_FORMAT_RAW_RGGB_12: case PIXEL_FORMAT_RAW_GBRG_12: output_bit_depth = 12; break;
        case PIXEL_FORMAT_RAW_RGGB_14: case PIXEL_FORMAT_RAW_GBRG_14: output_bit_depth = 14; break;
        case PIXEL_FORMAT_RAW_RGGB_16: case PIXEL_FORMAT_RAW_GBRG_16: output_bit_depth = 16; break;
        default: break;
    }
    int rggb_order = (output_format == PIXEL_FORMAT_RAW_RGGB_12 ||
                      output_format == PIXEL_FORMAT_RAW_RGGB_14 ||
                      output_format == PIXEL_FORMAT_RAW_RGGB_16) ? 1 : 0;

    int log_bits = bits_per_component;
    if (log_bits <= 0) log_bits = 12;
    if (log_bits > 16) log_bits = 16;

    uint16_t *log_table;
    int log_max;
    if (log_bits <= 12) { log_table = DecoderLogCurve12; log_max = (1 << 12) - 1; }
    else if (log_bits <= 14) { log_table = DecoderLogCurve14; log_max = (1 << 14) - 1; }
    else { log_table = DecoderLogCurve16; log_max = (1 << 16) - 1; }

    const int32_t midpoint = 1 << (log_bits - 1);
    const int shift = 16 - output_bit_depth;
    int bypass = vc5_logcurve_bypass();

    /* Bayer output geometry: output_width*2 wide, output_height_ch*2 tall (full sensor) */
    size_t bayer_pitch = packed_image->pitch * 2;  /* pitch is per Bayer-row-pair */
    size_t bayer_half_pitch = bayer_pitch / 2;

    /* Validate quant values */
    for (int ch = 0; ch < 4; ch++) {
        for (int i = 0; i < 4; i++) {
            if (wav[ch]->quant[i] == 0)
                wav[ch]->quant[i] = 1;
        }
    }

    /* Check if we need descale */
    int use_descale = (prescale > 1);
    int descale_shift = 0;
    if (use_descale) {
        if (prescale == 2) descale_shift = 1;
        else if (prescale == 3) descale_shift = 2;
    }

    /* Allocate row buffers: 4 channels x 4 buffers (even_lp, even_hp, odd_lp, odd_hp)
       + 4 channels x (3 lowhigh_line + 1 highlow_line + 1 highhigh_line) = 4*9 = 36 buffers
       + 4 channels x 2 output rows = 8 buffers
       Total: 44 row buffers. All from one arena. */
    size_t buffer_row_size = input_width * sizeof(PIXEL);
    size_t aligned_row = (buffer_row_size + 15) & ~(size_t)15;
    size_t horiz_row_size = output_width * sizeof(PIXEL);
    size_t aligned_horiz = (horiz_row_size + 15) & ~(size_t)15;

    /* Per-channel: 9 temp rows (same as InvertSpatialQuant16s) + 2 horizontal output rows */
    size_t per_ch = aligned_row * 9 + aligned_horiz * 2;
    uint8_t *arena = (uint8_t *)allocator->Alloc(per_ch * 4);
    if (!arena) return CODEC_ERROR_OUTOFMEMORY;

    /* Set up per-channel state */
    typedef struct {
        PIXEL *even_lowpass, *even_highpass, *odd_lowpass, *odd_highpass;
        PIXEL *lowhigh_line[3];
        PIXEL *highlow_line, *highhigh_line;
        PIXEL *even_row, *odd_row;   /* full-width horizontal output */
        PIXEL *lowlow, *lowhigh, *highlow, *highhigh;
        int lowlow_pitch, lowhigh_pitch, highlow_pitch, highhigh_pitch;
        QUANT hl_quant, lh_quant, hh_quant;
        int fuse_highlow, fuse_highhigh;
    } CH_STATE;

    CH_STATE cs[4];
    for (int ch = 0; ch < 4; ch++) {
        uint8_t *base = arena + ch * per_ch;
        cs[ch].even_lowpass    = (PIXEL *)(base + aligned_row * 0);
        cs[ch].even_highpass   = (PIXEL *)(base + aligned_row * 1);
        cs[ch].odd_lowpass     = (PIXEL *)(base + aligned_row * 2);
        cs[ch].odd_highpass    = (PIXEL *)(base + aligned_row * 3);
        cs[ch].lowhigh_line[0] = (PIXEL *)(base + aligned_row * 4);
        cs[ch].lowhigh_line[1] = (PIXEL *)(base + aligned_row * 5);
        cs[ch].lowhigh_line[2] = (PIXEL *)(base + aligned_row * 6);
        cs[ch].highlow_line    = (PIXEL *)(base + aligned_row * 7);
        cs[ch].highhigh_line   = (PIXEL *)(base + aligned_row * 8);
        cs[ch].even_row        = (PIXEL *)(base + aligned_row * 9);
        cs[ch].odd_row         = (PIXEL *)(base + aligned_row * 9 + aligned_horiz);

        cs[ch].lowlow   = (PIXEL *)wav[ch]->data[LL_BAND];
        cs[ch].lowhigh  = (PIXEL *)wav[ch]->data[LH_BAND];
        cs[ch].highlow  = (PIXEL *)wav[ch]->data[HL_BAND];
        cs[ch].highhigh = (PIXEL *)wav[ch]->data[HH_BAND];

        cs[ch].lowlow_pitch   = wav[ch]->pitch / sizeof(PIXEL);
        cs[ch].lowhigh_pitch  = wav[ch]->pitch / sizeof(PIXEL);
        cs[ch].highlow_pitch  = wav[ch]->pitch / sizeof(PIXEL);
        cs[ch].highhigh_pitch = wav[ch]->pitch / sizeof(PIXEL);

        cs[ch].hl_quant = wav[ch]->quant[HL_BAND];
        cs[ch].lh_quant = wav[ch]->quant[LH_BAND];
        cs[ch].hh_quant = wav[ch]->quant[HH_BAND];

        cs[ch].fuse_highlow  = (cs[ch].hl_quant < 0);
        cs[ch].fuse_highhigh = (cs[ch].hh_quant < 0);
    }

    /* ---- Macro to run vertical filter for one channel, writing to even_lp/hp and odd_lp/hp ---- */
    /* Then run horizontal filter to produce even_row and odd_row */

    /* Helper: run the vertical top-border filter for one channel */
#define VERT_TOP(ch) do { \
    CH_STATE *s = &cs[ch]; \
    DequantizeBandRow16s(s->lowhigh + 0 * s->lowhigh_pitch, input_width, s->lh_quant, s->lowhigh_line[0]); \
    DequantizeBandRow16s(s->lowhigh + 1 * s->lowhigh_pitch, input_width, s->lh_quant, s->lowhigh_line[1]); \
    DequantizeBandRow16s(s->lowhigh + 2 * s->lowhigh_pitch, input_width, s->lh_quant, s->lowhigh_line[2]); \
    DequantizeBandRow16s(s->highlow,  input_width, s->hl_quant, s->highlow_line); \
    DequantizeBandRow16s(s->highhigh, input_width, s->hh_quant, s->highhigh_line); \
    int col = 0; \
    for (; col < (int)input_width; col++) { \
        int32_t ev, od; \
        ev = 11 * s->lowlow[col + 0 * s->lowlow_pitch] \
           -  4 * s->lowlow[col + 1 * s->lowlow_pitch] \
           +  1 * s->lowlow[col + 2 * s->lowlow_pitch] + 4; \
        ev >>= 3; ev += s->highlow_line[col]; ev >>= 1; \
        s->even_lowpass[col] = ev; \
        od = 5 * s->lowlow[col + 0 * s->lowlow_pitch] \
           + 4 * s->lowlow[col + 1 * s->lowlow_pitch] \
           - 1 * s->lowlow[col + 2 * s->lowlow_pitch] + 4; \
        od >>= 3; od -= s->highlow_line[col]; od >>= 1; \
        s->odd_lowpass[col] = od; \
        ev = 11 * s->lowhigh_line[0][col] - 4 * s->lowhigh_line[1][col] + s->lowhigh_line[2][col] + 4; \
        ev >>= 3; ev += s->highhigh_line[col]; ev >>= 1; \
        s->even_highpass[col] = ev; \
        od = 5 * s->lowhigh_line[0][col] + 4 * s->lowhigh_line[1][col] - s->lowhigh_line[2][col] + 4; \
        od >>= 3; od -= s->highhigh_line[col]; od >>= 1; \
        s->odd_highpass[col] = od; \
    } \
} while(0)

    /* Helper: run the vertical middle-row filter for one channel */
#if ENABLED(NEON)
#define NEON_VERT_MID_BODY(ch) { \
    CH_STATE *_s = &cs[ch]; \
    int _w8 = ((int)input_width / 8) * 8; \
    int32x4_t _hlq = vdupq_n_s32(_s->fuse_highlow ? -_s->hl_quant : 1); \
    int32x4_t _hhq = vdupq_n_s32(_s->fuse_highhigh ? -_s->hh_quant : 1); \
    for (; col < _w8; col += 8) { \
        if (_s->fuse_highlow) \
            FusedVertMid8_Dq(_s->lowlow + 0 * _s->lowlow_pitch, _s->lowlow + 1 * _s->lowlow_pitch, \
                _s->lowlow + 2 * _s->lowlow_pitch, _s->highlow, _hlq, \
                _s->even_lowpass, _s->odd_lowpass, col); \
        else \
            FusedVertMid8(_s->lowlow + 0 * _s->lowlow_pitch, _s->lowlow + 1 * _s->lowlow_pitch, \
                _s->lowlow + 2 * _s->lowlow_pitch, _s->highlow_line, \
                _s->even_lowpass, _s->odd_lowpass, col); \
        if (_s->fuse_highhigh) \
            FusedVertMid8_Dq(_s->lowhigh_line[0], _s->lowhigh_line[1], _s->lowhigh_line[2], \
                _s->highhigh, _hhq, _s->even_highpass, _s->odd_highpass, col); \
        else \
            FusedVertMid8(_s->lowhigh_line[0], _s->lowhigh_line[1], _s->lowhigh_line[2], \
                _s->highhigh_line, _s->even_highpass, _s->odd_highpass, col); \
    } \
}
#else
#define NEON_VERT_MID_BODY(ch) /* no NEON */
#endif

#define VERT_MID(ch) do { \
    CH_STATE *s = &cs[ch]; \
    if (!s->fuse_highlow) \
        DequantizeBandRow16s(s->highlow, input_width, s->hl_quant, s->highlow_line); \
    if (!s->fuse_highhigh) \
        DequantizeBandRow16s(s->highhigh, input_width, s->hh_quant, s->highhigh_line); \
    int col = 0; \
    NEON_VERT_MID_BODY(ch) \
    for (; col < (int)input_width; col++) { \
        int32_t ev, od, hl_val, hh_val; \
        if (s->fuse_highlow) { \
            int32_t v = s->highlow[col]; \
            hl_val = (v > 0) ? v * (-s->hl_quant) : (v < 0) ? -((-v) * (-s->hl_quant)) : 0; \
        } else { hl_val = s->highlow_line[col]; } \
        if (s->fuse_highhigh) { \
            int32_t v = s->highhigh[col]; \
            hh_val = (v > 0) ? v * (-s->hh_quant) : (v < 0) ? -((-v) * (-s->hh_quant)) : 0; \
        } else { hh_val = s->highhigh_line[col]; } \
        ev = s->lowlow[col + 0 * s->lowlow_pitch] - s->lowlow[col + 2 * s->lowlow_pitch] + 4; \
        ev >>= 3; ev += s->lowlow[col + 1 * s->lowlow_pitch]; ev += hl_val; ev >>= 1; \
        s->even_lowpass[col] = ev; \
        od = -s->lowlow[col + 0 * s->lowlow_pitch] + s->lowlow[col + 2 * s->lowlow_pitch] + 4; \
        od >>= 3; od += s->lowlow[col + 1 * s->lowlow_pitch]; od -= hl_val; od >>= 1; \
        s->odd_lowpass[col] = od; \
        ev = s->lowhigh_line[0][col] - s->lowhigh_line[2][col] + 4; \
        ev >>= 3; ev += s->lowhigh_line[1][col]; ev += hh_val; ev >>= 1; \
        s->even_highpass[col] = ev; \
        od = -s->lowhigh_line[0][col] + s->lowhigh_line[2][col] + 4; \
        od >>= 3; od += s->lowhigh_line[1][col]; od -= hh_val; od >>= 1; \
        s->odd_highpass[col] = od; \
    } \
} while(0)

    /* Helper: run the vertical bottom-border filter for one channel */
#define VERT_BOT(ch) do { \
    CH_STATE *s = &cs[ch]; \
    DequantizeBandRow16s(s->highlow, input_width, s->hl_quant, s->highlow_line); \
    DequantizeBandRow16s(s->highhigh, input_width, s->hh_quant, s->highhigh_line); \
    int col = 0; \
    for (; col < (int)input_width; col++) { \
        int32_t ev, od; \
        ev = 5 * s->lowlow[col + 0 * s->lowlow_pitch] \
           + 4 * s->lowlow[col - 1 * s->lowlow_pitch] \
           - 1 * s->lowlow[col - 2 * s->lowlow_pitch] + 4; \
        ev >>= 3; ev += s->highlow_line[col]; ev >>= 1; \
        s->even_lowpass[col] = ev; \
        od = 11 * s->lowlow[col + 0 * s->lowlow_pitch] \
           -  4 * s->lowlow[col - 1 * s->lowlow_pitch] \
           +  1 * s->lowlow[col - 2 * s->lowlow_pitch] + 4; \
        od >>= 3; od -= s->highlow_line[col]; od >>= 1; \
        s->odd_lowpass[col] = od; \
        ev = 5 * s->lowhigh_line[2][col] + 4 * s->lowhigh_line[1][col] - s->lowhigh_line[0][col] + 4; \
        ev >>= 3; ev += s->highhigh_line[col]; ev >>= 1; \
        s->even_highpass[col] = ev; \
        od = 11 * s->lowhigh_line[2][col] - 4 * s->lowhigh_line[1][col] + s->lowhigh_line[0][col] + 4; \
        od >>= 3; od -= s->highhigh_line[col]; od >>= 1; \
        s->odd_highpass[col] = od; \
    } \
} while(0)

    /* Helper: run horizontal filter for one channel (using descale or not) */
#define HORIZ(ch) do { \
    CH_STATE *s = &cs[ch]; \
    if (use_descale) { \
        InvertHorizontalDescale16s(s->even_lowpass, s->even_highpass, s->even_row, input_width, output_width, prescale); \
        InvertHorizontalDescale16s(s->odd_lowpass, s->odd_highpass, s->odd_row, input_width, output_width, prescale); \
    } else { \
        InvertHorizontal16s(s->even_lowpass, s->even_highpass, s->even_row, input_width, output_width); \
        InvertHorizontal16s(s->odd_lowpass, s->odd_highpass, s->odd_row, input_width, output_width); \
    } \
} while(0)

    /* Helper: color convert + log curve + pack two output rows from 4 channels */
#define PACK_ROWS(out_row) do { \
    for (int _sub = 0; _sub < 2; _sub++) { \
        uint8_t *_ob = (uint8_t *)packed_image->buffer + ((out_row) + _sub) * bayer_pitch; \
        fused_pack_row((uint16_t *)_ob, (uint16_t *)(_ob + bayer_half_pitch), \
            (_sub == 0) ? cs[0].even_row : cs[0].odd_row, \
            (_sub == 0) ? cs[1].even_row : cs[1].odd_row, \
            (_sub == 0) ? cs[2].even_row : cs[2].odd_row, \
            (_sub == 0) ? cs[3].even_row : cs[3].odd_row, \
            (int)output_width, log_max, midpoint, shift, bypass, rggb_order, log_bits, log_table); \
    } \
} while(0)

    /* ---- Process rows ---- */
    int out_bayer_row = 0;  /* counts in Bayer row-pairs (each has 2 rows) */

    /* Top border row (row 0) */
    for (int ch = 0; ch < 4; ch++) { VERT_TOP(ch); HORIZ(ch); }
    PACK_ROWS(out_bayer_row);
    out_bayer_row += 2;  /* each wavelet row produces 2 output rows = 1 Bayer pair */

    /* Advance pointers for all channels */
    for (int ch = 0; ch < 4; ch++) {
        cs[ch].highlow  += cs[ch].highlow_pitch;
        cs[ch].highhigh += cs[ch].highhigh_pitch;
    }

    /* Middle rows */
    int row;
    for (row = 1; row < last_row; row++) {
        for (int ch = 0; ch < 4; ch++) { VERT_MID(ch); HORIZ(ch); }
        PACK_ROWS(out_bayer_row);
        out_bayer_row += 2;

        /* Advance band pointers */
        for (int ch = 0; ch < 4; ch++) {
            cs[ch].lowlow  += cs[ch].lowlow_pitch;
            cs[ch].lowhigh += cs[ch].lowhigh_pitch;
            cs[ch].highlow += cs[ch].highlow_pitch;
            cs[ch].highhigh += cs[ch].highhigh_pitch;

            /* Rotate lowhigh_line ring buffer */
            if (row < last_row - 1) {
                PIXEL *temp = cs[ch].lowhigh_line[0];
                cs[ch].lowhigh_line[0] = cs[ch].lowhigh_line[1];
                cs[ch].lowhigh_line[1] = cs[ch].lowhigh_line[2];
                cs[ch].lowhigh_line[2] = temp;
                DequantizeBandRow16s(cs[ch].lowhigh + 2 * cs[ch].lowhigh_pitch,
                                     input_width, cs[ch].lh_quant, cs[ch].lowhigh_line[2]);
            }
        }
    }

    /* Bottom border row */
    assert(row == last_row);
    for (int ch = 0; ch < 4; ch++) {
        cs[ch].lowlow += cs[ch].lowlow_pitch;
    }
    for (int ch = 0; ch < 4; ch++) { VERT_BOT(ch); HORIZ(ch); }

    /* Pack even row */
    {
        uint8_t *out_base = (uint8_t *)packed_image->buffer + out_bayer_row * bayer_pitch;
        fused_pack_row((uint16_t *)out_base, (uint16_t *)(out_base + bayer_half_pitch),
                       cs[0].even_row, cs[1].even_row, cs[2].even_row, cs[3].even_row,
                       (int)output_width, log_max, midpoint, shift, bypass, rggb_order, log_bits, log_table);
    }
    /* Pack odd row if it fits */
    if (2 * last_row + 1 < (int)output_height_ch) {
        uint8_t *out_base = (uint8_t *)packed_image->buffer + (out_bayer_row + 1) * bayer_pitch;
        fused_pack_row((uint16_t *)out_base, (uint16_t *)(out_base + bayer_half_pitch),
                       cs[0].odd_row, cs[1].odd_row, cs[2].odd_row, cs[3].odd_row,
                       (int)output_width, log_max, midpoint, shift, bypass, rggb_order, log_bits, log_table);
    }

#undef VERT_TOP
#undef VERT_MID
#undef VERT_BOT
#undef HORIZ
#undef PACK_ROWS

    allocator->Free(arena);
    return CODEC_ERROR_OKAY;
}

/* ================================================================
   Main entry: DecodeFastImage — drop-in replacement for DecodeImage
   ================================================================ */

/* Optional timing (compile with -DFAST_DECODE_TIMING to enable) */
#ifdef FAST_DECODE_TIMING
#include <mach/mach_time.h>
static double _fd_ms(void) {
    static double s = 0;
    if (!s) { mach_timebase_info_data_t i; mach_timebase_info(&i); s = (double)i.numer/i.denom/1e6; }
    return mach_absolute_time() * s;
}
#define FD_T(name) double _t_##name = _fd_ms()
#define FD_P(name, prev) fprintf(stderr, "  %-25s %.1fms\n", #name, _fd_ms() - _t_##prev)
#else
#define FD_T(name)
#define FD_P(name, prev)
#endif

CODEC_ERROR DecodeFastImage(const uint8_t *vc5_buf, size_t vc5_size,
                            IMAGE *packed_image, RGB_IMAGE *rgb_image,
                            DECODER_PARAMETERS *parameters)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    FD_T(start);

    /* Initialize LUTs (same as DecodeImage) */
    SetupDecoderLogCurve();
    InitUncompandTable();
    init_log_approx();

    /* Check env var for log curve bypass (for benchmarking raw component output) */
    {
        static int checked = 0;
        if (!checked) {
            if (getenv("GPR_LOG_BYPASS")) {
                extern void vc5_logcurve_set_bypass(int);
                vc5_logcurve_set_bypass(1);
            }
            checked = 1;
        }
    }

    /* ---- Step 1: Pre-index the VC5 bitstream ---- */
    FAST_INDEX idx;
    if (fast_preindex(vc5_buf, vc5_size, &idx) != 0)
        return CODEC_ERROR_MISSING_START_MARKER;

    int channel_count = idx.channel_count;
    int channel_width = idx.image_width / 2;   /* Bayer: each channel is half the image */
    int channel_height = idx.image_height / 2;

    /* Determine bits per component */
    int bits_per_component = idx.max_bits_per_component;
    if (bits_per_component == 0)
        bits_per_component = idx.bits_per_component;
    if (bits_per_component == 0)
        bits_per_component = 14; /* Default */

    /* Unpack prescale table from tag 109 value */
    PRESCALE prescale_table[MAX_WAVELET_COUNT];
    for (int i = 0; i < MAX_WAVELET_COUNT; i++) {
        prescale_table[i] = (idx.prescale_shift >> (14 - i * 2)) & 0x03;
    }


    FD_P(preindex, start); FD_T(alloc);
    /* ---- Step 2: Create wavelets for all channels ---- */
    gpr_allocator *allocator = &parameters->allocator;
    WAVELET *wavelets[MAX_CHANNEL_COUNT][MAX_WAVELET_COUNT];
    memset(wavelets, 0, sizeof(wavelets));

    for (int ch = 0; ch < channel_count; ch++) {
        int w = channel_width;
        int h = channel_height;
        for (int level = 0; level < MAX_WAVELET_COUNT; level++) {
            /* Pad to even */
            if (w % 2 != 0) w++;
            if (h % 2 != 0) h++;
            w /= 2;
            h /= 2;

            wavelets[ch][level] = CreateWavelet(allocator, w, h);
            if (!wavelets[ch][level]) {
                error = CODEC_ERROR_OUTOFMEMORY;
                goto cleanup;
            }
        }
    }


    FD_P(wavelet_alloc, alloc); FD_T(bands);
    /* ---- Step 3: Map pre-indexed bands to wavelet tree positions ---- */
    /* Subband mapping: same as SubbandWaveletIndex/SubbandBandIndex */
    static const int subband_wavelet[] = {2, 2, 2, 2, 1, 1, 1, 0, 0, 0};
    static const int subband_band[]    = {0, 1, 2, 3, 1, 2, 3, 1, 2, 3};

    /* Count of ANS bands for parallel dispatch */
    int ans_band_count = 0;
    FAST_ANS_ARG ans_args[FAST_MAX_BANDS];

    for (int i = 0; i < idx.band_count; i++) {
        FAST_BAND_INFO *b = &idx.bands[i];
        int ch = b->channel;
        int sub = b->subband;

        if (ch < 0 || ch >= channel_count || sub < 0 || sub >= MAX_SUBBAND_COUNT)
            continue;

        int wlevel = subband_wavelet[sub];
        int wband = subband_band[sub];
        WAVELET *wav = wavelets[ch][wlevel];

        if (sub == 0) {
            /* ---- Lowpass band: direct byte-swap decode ---- */
            fast_decode_lowpass(vc5_buf + b->data_offset, b->data_size,
                                wav->data[0], wav->width, wav->height,
                                wav->pitch / sizeof(PIXEL),
                                b->lowpass_precision);
            UpdateWaveletValidBandMask(wav, 0);
            wav->quant[0] = 1; /* Lowpass has no quantization */
        }
        else if (b->coding_method >= 1) {
            /* ---- ANS band: queue for parallel decode ---- */
            FAST_ANS_ARG *a = &ans_args[ans_band_count];
            a->vc5_base = vc5_buf;
            a->band = b;
            a->output = wav->data[wband];
            a->width = wav->width;
            a->height = wav->height;
            a->pitch = wav->pitch;
            a->error = 0;
            ans_band_count++;

            wav->quant[wband] = b->quantization;
            /* ANS modes 2/4: negate quant to skip uncompanding in dequantizer */
            if (b->coding_method == 2 || b->coding_method == 4) {
                wav->quant[wband] = -wav->quant[wband];
            }
            UpdateWaveletValidBandMask(wav, wband);
        }
        else {
            /* ---- VLC band: decode serially ---- */
            int rc = fast_decode_vlc_band(vc5_buf, b,
                                           wav->data[wband],
                                           wav->width, wav->height,
                                           wav->pitch);
            if (rc != 0) {
                error = CODEC_ERROR_DECODING_SUBBAND;
                goto cleanup;
            }
            wav->quant[wband] = b->quantization;
            UpdateWaveletValidBandMask(wav, wband);
        }
    }

    FD_P(band_decode, bands); FD_T(ans);
    /* ---- Step 4: Parallel ANS decode ---- */
#if !defined(_WIN32) && !FAST_NO_THREADS
    {
        pthread_t threads[FAST_MAX_BANDS];
        int thread_count = 0;

        for (int i = 0; i < ans_band_count; i++) {
            if (pthread_create(&threads[thread_count], NULL,
                               fast_ans_decode_thread, &ans_args[i]) == 0) {
                thread_count++;
            } else {
                /* Thread creation failed: decode inline */
                fast_ans_decode_thread(&ans_args[i]);
            }
        }

        for (int i = 0; i < thread_count; i++) {
            pthread_join(threads[i], NULL);
        }
    }
#else
    for (int i = 0; i < ans_band_count; i++) {
        fast_ans_decode_thread(&ans_args[i]);
    }
#endif

    /* Check for ANS decode errors */
    for (int i = 0; i < ans_band_count; i++) {
        if (ans_args[i].error != 0) {
            error = CODEC_ERROR_DECODING_SUBBAND;
            goto cleanup;
        }
    }

    FD_P(ans_decode, ans); FD_T(wavelet);

    /* ---- Fused path: wavelet level 2→1→0 + color convert + pack in one pass ---- */
    /* Use fused path ONLY in single-threaded mode (multi-threaded uses parallel channels).
       Fused processes all 4 channels per-row which can't be easily parallelized. */
    if (FAST_NO_THREADS &&
        parameters->rgb_resolution == GPR_RGB_RESOLUTION_NONE &&
        channel_count == 4 &&
        !(parameters->variance_stabilize && parameters->noise_scale > 0.0))
    {
        /* Step 5a: Intermediate wavelet levels (2→1 and 1→0) per channel */
        for (int ch = 0; ch < channel_count; ch++) {
            if (BandsAllValid(wavelets[ch][2])) {
                TransformInverseSpatialQuantLowpass(allocator,
                    wavelets[ch][2], wavelets[ch][1], prescale_table[2]);
                UpdateWaveletValidBandMask(wavelets[ch][1], 0);
            }
            if (BandsAllValid(wavelets[ch][1])) {
                TransformInverseSpatialQuantLowpass(allocator,
                    wavelets[ch][1], wavelets[ch][0], prescale_table[1]);
                UpdateWaveletValidBandMask(wavelets[ch][0], 0);
            }
        }

        FD_P(wavelet_intermediate, wavelet); FD_T(pack);

        /* Step 5b: Allocate and fused level-0 reconstruct + pack */
        DIMENSION packed_width = idx.image_width;
        DIMENSION packed_height = idx.image_height;
        PIXEL_FORMAT packed_format = parameters->output.format;

        AllocImage(allocator, packed_image, packed_width, packed_height, packed_format);

        WAVELET *wav0[4] = { wavelets[0][0], wavelets[1][0], wavelets[2][0], wavelets[3][0] };
        error = ReconstructAndPackDirect(allocator, wav0, prescale_table[0],
                                         packed_image, bits_per_component, packed_format);

        FD_P(fused_recon_pack, pack);
    }
    else
    {
    /* ---- Legacy path: separate wavelet recon + pack ---- */
    /* Step 5+6: Full wavelet reconstruction (all levels) in parallel per channel */
    {
        UNPACKED_IMAGE unpacked_image;
        InitUnpackedImage(&unpacked_image);

        /* Allocate component arrays */
        size_t comp_size = channel_count * sizeof(COMPONENT_ARRAY);
        unpacked_image.component_array_list = allocator->Alloc(comp_size);
        if (!unpacked_image.component_array_list) {
            error = CODEC_ERROR_OUTOFMEMORY;
            goto cleanup;
        }
        unpacked_image.component_count = channel_count;
        memset(unpacked_image.component_array_list, 0, comp_size);

        for (int ch = 0; ch < channel_count; ch++) {
            error = AllocateComponentArray(allocator,
                                            &unpacked_image.component_array_list[ch],
                                            channel_width, channel_height,
                                            bits_per_component);
            if (error != CODEC_ERROR_OKAY) goto cleanup;
        }

        /* Run ALL wavelet reconstruction levels in parallel (one thread per channel).
           Each thread does: level 2→1, level 1→0, level 0→output */
#if !defined(_WIN32) && !FAST_NO_THREADS
        {
            pthread_t threads[MAX_CHANNEL_COUNT];
            FAST_CHANNEL_RECON_ARG recon_args[MAX_CHANNEL_COUNT];
            int thread_created[MAX_CHANNEL_COUNT];

            for (int ch = 0; ch < channel_count; ch++) {
                recon_args[ch].allocator = allocator;
                for (int lvl = 0; lvl < MAX_WAVELET_COUNT; lvl++) {
                    recon_args[ch].wavelets[lvl] = wavelets[ch][lvl];
                    recon_args[ch].prescale_table[lvl] = prescale_table[lvl];
                }
                recon_args[ch].output_data  = unpacked_image.component_array_list[ch].data;
                recon_args[ch].output_width = channel_width;
                recon_args[ch].output_height = channel_height;
                recon_args[ch].output_pitch = unpacked_image.component_array_list[ch].pitch;
                recon_args[ch].error        = CODEC_ERROR_OKAY;

                thread_created[ch] = (pthread_create(&threads[ch], NULL,
                                                      fast_channel_recon_thread,
                                                      &recon_args[ch]) == 0);
                if (!thread_created[ch]) {
                    fast_channel_recon_thread(&recon_args[ch]);
                }
            }

            for (int ch = 0; ch < channel_count; ch++) {
                if (thread_created[ch])
                    pthread_join(threads[ch], NULL);
                if (recon_args[ch].error != CODEC_ERROR_OKAY)
                    error = recon_args[ch].error;
            }
        }
#else
        for (int ch = 0; ch < channel_count; ch++) {
            FAST_CHANNEL_RECON_ARG recon_arg;
            recon_arg.allocator = allocator;
            for (int lvl = 0; lvl < MAX_WAVELET_COUNT; lvl++) {
                recon_arg.wavelets[lvl] = wavelets[ch][lvl];
                recon_arg.prescale_table[lvl] = prescale_table[lvl];
            }
            recon_arg.output_data  = unpacked_image.component_array_list[ch].data;
            recon_arg.output_width = channel_width;
            recon_arg.output_height = channel_height;
            recon_arg.output_pitch = unpacked_image.component_array_list[ch].pitch;
            recon_arg.error        = CODEC_ERROR_OKAY;
            fast_channel_recon_thread(&recon_arg);
            if (recon_arg.error != CODEC_ERROR_OKAY)
                error = recon_arg.error;
        }
#endif

        if (error != CODEC_ERROR_OKAY) {
            ReleaseComponentArrays(allocator, &unpacked_image, channel_count);
            goto cleanup;
        }


        FD_P(wavelet_recon, wavelet); FD_T(pack);
        /* ---- Step 7: Apply inverse variance-stabilizing transform if needed ---- */
        if (parameters->variance_stabilize && parameters->noise_scale > 0.0)
        {
            /* Defined in decoder.c as static — we inline the same logic */
            for (int ch = 0; ch < channel_count; ch++)
            {
                COMPONENT_VALUE *data = unpacked_image.component_array_list[ch].data;
                DIMENSION w = unpacked_image.component_array_list[ch].width;
                DIMENSION h = unpacked_image.component_array_list[ch].height;
                size_t pitch = unpacked_image.component_array_list[ch].pitch;
                double alpha = parameters->noise_scale;
                double sigma_sq = parameters->noise_offset;

                if (alpha > 0.0) {
                    double half_alpha = alpha / 2.0;
                    double offset = 3.0 / 8.0 * alpha * alpha + sigma_sq;
                    int pitch_elems = (int)(pitch / sizeof(COMPONENT_VALUE));
                    for (int row = 0; row < (int)h; row++) {
                        COMPONENT_VALUE *row_ptr = data + row * pitch_elems;
                        for (int col = 0; col < (int)w; col++) {
                            double d = (double)row_ptr[col];
                            double val = half_alpha * d;
                            val = val * val;
                            val = (val - offset) / alpha;
                            row_ptr[col] = (COMPONENT_VALUE)(val + 0.5);
                        }
                    }
                }
            }
        }

        /* ---- Step 8: Pack output image ---- */
        switch (parameters->rgb_resolution) {
            case GPR_RGB_RESOLUTION_NONE:
            {
                /* Determine output format from parameters */
                DIMENSION packed_width = idx.image_width;
                DIMENSION packed_height = idx.image_height;
                PIXEL_FORMAT packed_format = parameters->output.format;

                AllocImage(allocator, packed_image, packed_width, packed_height, packed_format);

                /* Parallel image packing: split rows across threads */
                {
                    int half_height = packed_height / 2;
                    int num_pack_threads = 4;
                    if (num_pack_threads > half_height) num_pack_threads = half_height;

#if !defined(_WIN32) && !FAST_NO_THREADS
                    {
                        pthread_t pack_threads[8];
                        FAST_PACK_ARG pack_args[8];
                        int pack_created[8];
                        int rows_per = half_height / num_pack_threads;

                        for (int t = 0; t < num_pack_threads; t++) {
                            pack_args[t].image = &unpacked_image;
                            pack_args[t].packed = packed_image;
                            pack_args[t].params = parameters;
                            pack_args[t].row_start = t * rows_per;
                            pack_args[t].row_end = (t == num_pack_threads - 1) ? half_height : (t + 1) * rows_per;

                            pack_created[t] = (pthread_create(&pack_threads[t], NULL,
                                                               fast_pack_thread, &pack_args[t]) == 0);
                            if (!pack_created[t])
                                fast_pack_thread(&pack_args[t]);
                        }

                        for (int t = 0; t < num_pack_threads; t++) {
                            if (pack_created[t])
                                pthread_join(pack_threads[t], NULL);
                        }
                    }
#else
                    ImageRepackingProcess(&unpacked_image, packed_image, parameters);
#endif
                }
                break;
            }

            case GPR_RGB_RESOLUTION_HALF:
            {
                WaveletToRGB(*allocator,
                             (PIXEL*)unpacked_image.component_array_list[0].data,
                             (PIXEL*)unpacked_image.component_array_list[1].data,
                             (PIXEL*)unpacked_image.component_array_list[2].data,
                             unpacked_image.component_array_list[2].width,
                             unpacked_image.component_array_list[2].height,
                             unpacked_image.component_array_list[2].pitch / sizeof(COMPONENT_VALUE),
                             rgb_image, bits_per_component,
                             parameters->rgb_bits, &parameters->rgb_gain);
                break;
            }

            case GPR_RGB_RESOLUTION_QUARTER:
            {
                WaveletToRGB(*allocator,
                             wavelets[0][0]->data[0], wavelets[1][0]->data[0],
                             wavelets[2][0]->data[0],
                             wavelets[2][0]->width, wavelets[2][0]->height,
                             wavelets[2][0]->width,
                             rgb_image, bits_per_component,
                             parameters->rgb_bits, &parameters->rgb_gain);
                break;
            }

            case GPR_RGB_RESOLUTION_EIGHTH:
            {
                WaveletToRGB(*allocator,
                             wavelets[0][1]->data[0], wavelets[1][1]->data[0],
                             wavelets[2][1]->data[0],
                             wavelets[2][1]->width, wavelets[2][1]->height,
                             wavelets[2][1]->width,
                             rgb_image, bits_per_component,
                             parameters->rgb_bits, &parameters->rgb_gain);
                break;
            }

            case GPR_RGB_RESOLUTION_SIXTEENTH:
            {
                WaveletToRGB(*allocator,
                             wavelets[0][2]->data[0], wavelets[1][2]->data[0],
                             wavelets[2][2]->data[0],
                             wavelets[2][2]->width, wavelets[2][2]->height,
                             wavelets[2][2]->width,
                             rgb_image, bits_per_component,
                             parameters->rgb_bits, &parameters->rgb_gain);
                break;
            }

            default:
                error = CODEC_ERROR_UNSUPPORTED_FORMAT;
                break;
        }


        FD_P(pack_output, pack);
        ReleaseComponentArrays(allocator, &unpacked_image, channel_count);
    }
    } /* end legacy path */

cleanup:
    /* Free all wavelets */
    for (int ch = 0; ch < channel_count; ch++) {
        for (int level = 0; level < MAX_WAVELET_COUNT; level++) {
            if (wavelets[ch][level]) {
                DeleteWavelet(allocator, wavelets[ch][level]);
                wavelets[ch][level] = NULL;
            }
        }
    }

    return error;
}

/* ================================================================
   Direct GPR decode: bypass DNG SDK entirely
   ================================================================ */

/* Lightweight GPR parser (defined in fast_gpr.c) */
extern int fast_gpr_extract_vc5(const uint8_t *gpr_data, size_t gpr_size,
                                 size_t *vc5_offset, size_t *vc5_size,
                                 int *image_width, int *image_height);

/*!
    @brief Decode a GPR file directly to raw pixels, bypassing the DNG SDK.

    This is the fastest possible GPR decode path:
    1. Lightweight TIFF parse to find VC5 blob (< 0.1ms)
    2. Fast parallel VC5 decode (pre-index + parallel ANS + parallel wavelet)
    3. Parallel image packing

    @param gpr_data     GPR file data in memory
    @param gpr_size     GPR file size
    @param raw_output   Output: pointer to allocated raw pixel buffer (caller must free)
    @param raw_size     Output: size of raw pixel buffer
    @param pixel_format Output pixel format (e.g., VC5_DECODER_PIXEL_FORMAT_RGGB_14)
    @return 0 on success
*/
int gpr_fast_decode(const uint8_t *gpr_data, size_t gpr_size,
                    void **raw_output, size_t *raw_size,
                    int pixel_format)
{
    /* Step 1: Extract VC5 blob from GPR container */
    size_t vc5_offset, vc5_size;
    int img_width, img_height;

    if (fast_gpr_extract_vc5(gpr_data, gpr_size, &vc5_offset, &vc5_size,
                              &img_width, &img_height) != 0)
        return -1;

    const uint8_t *vc5_buf = gpr_data + vc5_offset;

    /* Step 2: Set up minimal decoder parameters */
    DECODER_PARAMETERS parameters;
    InitDecoderParameters(&parameters);
    parameters.allocator.Alloc = malloc;
    parameters.allocator.Free = free;

    switch (pixel_format) {
        case 0: parameters.output.format = PIXEL_FORMAT_RAW_RGGB_12; break;
        case 1: parameters.output.format = PIXEL_FORMAT_RAW_RGGB_14; break;
        case 2: parameters.output.format = PIXEL_FORMAT_RAW_GBRG_12; break;
        case 3: parameters.output.format = PIXEL_FORMAT_RAW_GBRG_14; break;
        case 4: parameters.output.format = PIXEL_FORMAT_RAW_RGGB_16; break;
        case 5: parameters.output.format = PIXEL_FORMAT_RAW_GBRG_16; break;
        default: parameters.output.format = PIXEL_FORMAT_RAW_RGGB_14; break;
    }
    parameters.rgb_resolution = GPR_RGB_RESOLUTION_NONE;

    /* Step 3: Fast parallel VC5 decode */
    IMAGE output_image;
    InitImage(&output_image);
    RGB_IMAGE rgb_image;
    InitRGBImage(&rgb_image);

    CODEC_ERROR error = DecodeFastImage(vc5_buf, vc5_size,
                                         &output_image, &rgb_image, &parameters);
    if (error != CODEC_ERROR_OKAY)
        return (int)error;

    /* Return the decoded image */
    *raw_output = output_image.buffer;
    *raw_size = output_image.size;

    return 0;
}
