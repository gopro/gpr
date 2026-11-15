/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef VC5_PREVIEW_H
#define VC5_PREVIEW_H
#include <stdint.h>
#include <stddef.h>
/* Borrowed host-endian quantized encoder coefficients, before codebook
 * companding. Upper levels 0/1 here correspond to encoder stages 1/2.
 * All extents/strides are in int16_t samples. The lower level's LL is unused.
 * This is an in-process view, not a serialized or compressed file format. */
typedef struct {
    uint32_t width,height,pitch_samples;
    const int16_t *bands[4];
    size_t band_samples[4];
    uint32_t quant[4],prescale;
} vc5_preview_level;
typedef struct {
    uint32_t sensor_width,sensor_height;
    vc5_preview_level channels[3][2]; /* GS/RG/BG, lower/upper level */
} vc5_preview_coefficients;
typedef void (*vc5_encoder_preview_fn)(void *user,const vc5_preview_coefficients *coefficients);
/* Synchronous row handoff.
 * level0/1 correspond to encoder stages1/2. Every band points at this one
 * quantized row and is borrowed only until the callback returns. */
typedef struct {
    uint32_t channel,level,row,width,height,prescale;
    const int16_t *bands[4];
    uint32_t quant[4];
    size_t band_samples[4]; /* at least width for each used band */
} vc5_preview_row;
typedef void (*vc5_encoder_preview_row_fn)(void *user,const vc5_preview_row *row);
#endif
