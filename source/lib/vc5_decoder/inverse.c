/*! @file inverse.c
 *
 *  @brief Implementation of the inverse wavelet transforms.
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "headers.h"

#if ENABLED(NEON)
#include <arm_neon.h>
#endif

//! Rounding adjustment used by the inverse wavelet transforms
static const int32_t rounding = 4;

#if ENABLED(NEON)

/*! @brief Inline dequantize 4 pixels for ANS raw mode (negative quant).
    Returns abs(input) * |quant| with original sign restored. */
static INLINE int32x4_t DequantInline4_NEON(const PIXEL *src, int col, int32x4_t quant_vec)
{
    int32x4_t v = vld1q_s32(&src[col]);
    int32x4_t abs_v = vabsq_s32(v);
    int32x4_t dequant = vmulq_s32(abs_v, quant_vec);
    uint32x4_t neg_mask = vcltq_s32(v, vdupq_n_s32(0));
    return vbslq_s32(neg_mask, vnegq_s32(dequant), dequant);
}

/*!
 @brief NEON helper: vertical inverse middle-row filter for 4 columns
 Computes even and odd outputs from 3 rows of lowpass and 1 row of highpass.
 even = ((row0 - row2 + 4) >> 3 + row1 + hp) >> 1
 odd  = ((-row0 + row2 + 4) >> 3 + row1 - hp) >> 1
 */
static INLINE void InvertVerticalMiddle4_NEON(
    const PIXEL *row0, const PIXEL *row1, const PIXEL *row2,
    const PIXEL *hp, PIXEL *even_out, PIXEL *odd_out, int col)
{
    const int32x4_t four = vdupq_n_s32(4);

    int32x4_t r0 = vld1q_s32(&row0[col]);
    int32x4_t r1 = vld1q_s32(&row1[col]);
    int32x4_t r2 = vld1q_s32(&row2[col]);
    int32x4_t h  = vld1q_s32(&hp[col]);

    // even path: (r0 - r2 + 4) >> 3 + r1 + h, then >> 1
    int32x4_t diff = vsubq_s32(r0, r2);
    diff = vaddq_s32(diff, four);
    diff = vshrq_n_s32(diff, 3);
    int32x4_t even_v = vaddq_s32(vaddq_s32(diff, r1), h);
    even_v = vshrq_n_s32(even_v, 1);
    vst1q_s32(&even_out[col], even_v);

    // odd path: (-r0 + r2 + 4) >> 3 + r1 - h, then >> 1
    int32x4_t diff_odd = vsubq_s32(r2, r0);
    diff_odd = vaddq_s32(diff_odd, four);
    diff_odd = vshrq_n_s32(diff_odd, 3);
    int32x4_t odd_v = vsubq_s32(vaddq_s32(diff_odd, r1), h);
    odd_v = vshrq_n_s32(odd_v, 1);
    vst1q_s32(&odd_out[col], odd_v);
}

/*!
 @brief 8-wide vertical filter: process 8 columns per call (2x throughput).
 Uses 20 registers — fits in ARM64's 32-register file with room to spare.
 */
static INLINE void InvertVerticalMiddle8_NEON(
    const PIXEL *row0, const PIXEL *row1, const PIXEL *row2,
    const PIXEL *hp, PIXEL *even_out, PIXEL *odd_out, int col)
{
    const int32x4_t four = vdupq_n_s32(4);

    /* Load 8 values from each source (2 × 4-wide) */
    int32x4_t r0a = vld1q_s32(&row0[col]);
    int32x4_t r0b = vld1q_s32(&row0[col + 4]);
    int32x4_t r1a = vld1q_s32(&row1[col]);
    int32x4_t r1b = vld1q_s32(&row1[col + 4]);
    int32x4_t r2a = vld1q_s32(&row2[col]);
    int32x4_t r2b = vld1q_s32(&row2[col + 4]);
    int32x4_t ha  = vld1q_s32(&hp[col]);
    int32x4_t hb  = vld1q_s32(&hp[col + 4]);

    /* Even path: (r0 - r2 + 4) >> 3 + r1 + h, then >> 1 */
    int32x4_t da = vshrq_n_s32(vaddq_s32(vsubq_s32(r0a, r2a), four), 3);
    int32x4_t db = vshrq_n_s32(vaddq_s32(vsubq_s32(r0b, r2b), four), 3);
    int32x4_t ea = vshrq_n_s32(vaddq_s32(vaddq_s32(da, r1a), ha), 1);
    int32x4_t eb = vshrq_n_s32(vaddq_s32(vaddq_s32(db, r1b), hb), 1);
    vst1q_s32(&even_out[col], ea);
    vst1q_s32(&even_out[col + 4], eb);

    /* Odd path: (-r0 + r2 + 4) >> 3 + r1 - h, then >> 1 */
    int32x4_t oa = vshrq_n_s32(vaddq_s32(vsubq_s32(r2a, r0a), four), 3);
    int32x4_t ob = vshrq_n_s32(vaddq_s32(vsubq_s32(r2b, r0b), four), 3);
    int32x4_t ova = vshrq_n_s32(vsubq_s32(vaddq_s32(oa, r1a), ha), 1);
    int32x4_t ovb = vshrq_n_s32(vsubq_s32(vaddq_s32(ob, r1b), hb), 1);
    vst1q_s32(&odd_out[col], ova);
    vst1q_s32(&odd_out[col + 4], ovb);
}

/*!
 @brief 8-wide fused dequantize + vertical filter for ANS raw mode.
 */
static INLINE void InvertVerticalMiddle8_Fused_NEON(
    const PIXEL *row0, const PIXEL *row1, const PIXEL *row2,
    const PIXEL *hp_raw, int32x4_t hp_quant,
    PIXEL *even_out, PIXEL *odd_out, int col)
{
    const int32x4_t four = vdupq_n_s32(4);

    int32x4_t r0a = vld1q_s32(&row0[col]);
    int32x4_t r0b = vld1q_s32(&row0[col + 4]);
    int32x4_t r1a = vld1q_s32(&row1[col]);
    int32x4_t r1b = vld1q_s32(&row1[col + 4]);
    int32x4_t r2a = vld1q_s32(&row2[col]);
    int32x4_t r2b = vld1q_s32(&row2[col + 4]);
    int32x4_t ha  = DequantInline4_NEON(hp_raw, col, hp_quant);
    int32x4_t hb  = DequantInline4_NEON(hp_raw, col + 4, hp_quant);

    int32x4_t da = vshrq_n_s32(vaddq_s32(vsubq_s32(r0a, r2a), four), 3);
    int32x4_t db = vshrq_n_s32(vaddq_s32(vsubq_s32(r0b, r2b), four), 3);
    int32x4_t ea = vshrq_n_s32(vaddq_s32(vaddq_s32(da, r1a), ha), 1);
    int32x4_t eb = vshrq_n_s32(vaddq_s32(vaddq_s32(db, r1b), hb), 1);
    vst1q_s32(&even_out[col], ea);
    vst1q_s32(&even_out[col + 4], eb);

    int32x4_t oa = vshrq_n_s32(vaddq_s32(vsubq_s32(r2a, r0a), four), 3);
    int32x4_t ob = vshrq_n_s32(vaddq_s32(vsubq_s32(r2b, r0b), four), 3);
    int32x4_t ova = vshrq_n_s32(vsubq_s32(vaddq_s32(oa, r1a), ha), 1);
    int32x4_t ovb = vshrq_n_s32(vsubq_s32(vaddq_s32(ob, r1b), hb), 1);
    vst1q_s32(&odd_out[col], ova);
    vst1q_s32(&odd_out[col + 4], ovb);
}

/*!
 @brief Fused dequantize + vertical filter for middle rows (ANS raw mode).
 Reads raw highpass band data directly and dequantizes inline during the filter,
 eliminating the separate DequantizeBandRow16s call and its temp buffer writes.
 */
static INLINE void InvertVerticalMiddle4_Fused_NEON(
    const PIXEL *row0, const PIXEL *row1, const PIXEL *row2,
    const PIXEL *hp_raw, int32x4_t hp_quant,
    PIXEL *even_out, PIXEL *odd_out, int col)
{
    const int32x4_t four = vdupq_n_s32(4);

    int32x4_t r0 = vld1q_s32(&row0[col]);
    int32x4_t r1 = vld1q_s32(&row1[col]);
    int32x4_t r2 = vld1q_s32(&row2[col]);
    /* Fused dequantize: load raw hp, abs*quant, restore sign */
    int32x4_t h = DequantInline4_NEON(hp_raw, col, hp_quant);

    int32x4_t diff = vsubq_s32(r0, r2);
    diff = vaddq_s32(diff, four);
    diff = vshrq_n_s32(diff, 3);
    int32x4_t even_v = vaddq_s32(vaddq_s32(diff, r1), h);
    even_v = vshrq_n_s32(even_v, 1);
    vst1q_s32(&even_out[col], even_v);

    int32x4_t diff_odd = vsubq_s32(r2, r0);
    diff_odd = vaddq_s32(diff_odd, four);
    diff_odd = vshrq_n_s32(diff_odd, 3);
    int32x4_t odd_v = vsubq_s32(vaddq_s32(diff_odd, r1), h);
    odd_v = vshrq_n_s32(odd_v, 1);
    vst1q_s32(&odd_out[col], odd_v);
}

/*!
 @brief NEON helper: vertical inverse middle-row filter for Descale variant
 Same as above but uses DivideByShift(x,1) = x >> 1 instead of >> 1
 (which is the same operation, so the only diff is conceptual)
 */
static INLINE void InvertVerticalMiddle4Descale_NEON(
    const PIXEL *row0, const PIXEL *row1, const PIXEL *row2,
    const PIXEL *hp, PIXEL *even_out, PIXEL *odd_out, int col)
{
    const int32x4_t four = vdupq_n_s32(4);

    int32x4_t r0 = vld1q_s32(&row0[col]);
    int32x4_t r1 = vld1q_s32(&row1[col]);
    int32x4_t r2 = vld1q_s32(&row2[col]);
    int32x4_t h  = vld1q_s32(&hp[col]);

    int32x4_t diff = vsubq_s32(r0, r2);
    diff = vaddq_s32(diff, four);
    diff = vshrq_n_s32(diff, 3);
    int32x4_t even_v = vaddq_s32(vaddq_s32(diff, r1), h);
    even_v = vshrq_n_s32(even_v, 1);
    vst1q_s32(&even_out[col], even_v);

    int32x4_t diff_odd = vsubq_s32(r2, r0);
    diff_odd = vaddq_s32(diff_odd, four);
    diff_odd = vshrq_n_s32(diff_odd, 3);
    int32x4_t odd_v = vsubq_s32(vaddq_s32(diff_odd, r1), h);
    odd_v = vshrq_n_s32(odd_v, 1);
    vst1q_s32(&odd_out[col], odd_v);
}
#endif

/*!
 @brief Apply the inverse horizontal wavelet transform
 */
CODEC_ERROR InvertHorizontal16s(PIXEL *lowpass, PIXEL *highpass, PIXEL *output,
                                DIMENSION input_width, DIMENSION output_width)
{
    const int last_column = input_width - 1;
    int32_t even;
    int32_t odd;
    int column = 0;

    // Left border
    even = 11 * lowpass[0] - 4 * lowpass[1] + lowpass[2] + rounding;
    even = DivideByShift(even, 3);
    even = (even + highpass[0]) >> 1;

    odd = 5 * lowpass[0] + 4 * lowpass[1] - lowpass[2] + rounding;
    odd = DivideByShift(odd, 3);
    odd = (odd - highpass[0]) >> 1;

    output[0] = ClampPixel(even);
    output[1] = ClampPixel(odd);
    column = 1;

#if ENABLED(NEON)
    {
        const int32x4_t four = vdupq_n_s32(4);

        /* 8-wide: process 8 input columns → 16 output pixels per iteration */
        for (; column + 7 < last_column; column += 8)
        {
            /* First 4 columns */
            int32x4_t lp_la = vld1q_s32(&lowpass[column - 1]);
            int32x4_t lp_ca = vld1q_s32(&lowpass[column]);
            int32x4_t lp_ra = vld1q_s32(&lowpass[column + 1]);
            int32x4_t hp_ca = vld1q_s32(&highpass[column]);
            /* Second 4 columns */
            int32x4_t lp_lb = vld1q_s32(&lowpass[column + 3]);
            int32x4_t lp_cb = vld1q_s32(&lowpass[column + 4]);
            int32x4_t lp_rb = vld1q_s32(&lowpass[column + 5]);
            int32x4_t hp_cb = vld1q_s32(&highpass[column + 4]);

            int32x4_t ea = vshrq_n_s32(vaddq_s32(vaddq_s32(vshrq_n_s32(vaddq_s32(vsubq_s32(lp_la, lp_ra), four), 3), lp_ca), hp_ca), 1);
            int32x4_t oa = vshrq_n_s32(vsubq_s32(vaddq_s32(vshrq_n_s32(vaddq_s32(vsubq_s32(lp_ra, lp_la), four), 3), lp_ca), hp_ca), 1);
            int32x4_t eb = vshrq_n_s32(vaddq_s32(vaddq_s32(vshrq_n_s32(vaddq_s32(vsubq_s32(lp_lb, lp_rb), four), 3), lp_cb), hp_cb), 1);
            int32x4_t ob = vshrq_n_s32(vsubq_s32(vaddq_s32(vshrq_n_s32(vaddq_s32(vsubq_s32(lp_rb, lp_lb), four), 3), lp_cb), hp_cb), 1);

            int32x4x2_t ia = { .val = { ea, oa } };
            int32x4x2_t ib = { .val = { eb, ob } };
            vst2q_s32(&output[2 * column], ia);
            vst2q_s32(&output[2 * (column + 4)], ib);
        }

        /* 4-wide cleanup */
        for (; column + 3 < last_column; column += 4)
        {
            int32x4_t lp_left   = vld1q_s32(&lowpass[column - 1]);
            int32x4_t lp_center = vld1q_s32(&lowpass[column]);
            int32x4_t lp_right  = vld1q_s32(&lowpass[column + 1]);
            int32x4_t hp_center = vld1q_s32(&highpass[column]);

            int32x4_t diff_e = vsubq_s32(lp_left, lp_right);
            diff_e = vaddq_s32(diff_e, four);
            diff_e = vshrq_n_s32(diff_e, 3);
            int32x4_t even_v = vaddq_s32(diff_e, lp_center);
            even_v = vaddq_s32(even_v, hp_center);
            even_v = vshrq_n_s32(even_v, 1);

            int32x4_t diff_o = vsubq_s32(lp_right, lp_left);
            diff_o = vaddq_s32(diff_o, four);
            diff_o = vshrq_n_s32(diff_o, 3);
            int32x4_t odd_v = vaddq_s32(diff_o, lp_center);
            odd_v = vsubq_s32(odd_v, hp_center);
            odd_v = vshrq_n_s32(odd_v, 1);

            int32x4x2_t interleaved;
            interleaved.val[0] = even_v;
            interleaved.val[1] = odd_v;
            vst2q_s32(&output[2 * column], interleaved);
        }
    }
#endif

    // Scalar middle columns
    for (; column < last_column; column++)
    {
        even = lowpass[column - 1] - lowpass[column + 1] + 4;
        even >>= 3;
        even += lowpass[column];
        even = (even + highpass[column]) >> 1;
        output[2 * column] = ClampPixel(even);

        odd = -lowpass[column - 1] + lowpass[column + 1] + 4;
        odd >>= 3;
        odd += lowpass[column];
        odd = (odd - highpass[column]) >> 1;
        output[2 * column + 1] = ClampPixel(odd);
    }

    assert(column == last_column);

    // Right border
    even = 5 * lowpass[column] + 4 * lowpass[column - 1] - lowpass[column - 2] + rounding;
    even = DivideByShift(even, 3);
    even = (even + highpass[column]) >> 1;
    output[2 * column] = ClampPixel(even);

    if (2 * column + 1 < output_width)
    {
        odd = 11 * lowpass[column] - 4 * lowpass[column - 1] + lowpass[column - 2] + rounding;
        odd = DivideByShift(odd, 3);
        odd = (odd - highpass[column]) >> 1;
        output[2 * column + 1] = ClampPixel(odd);
    }

    return CODEC_ERROR_OKAY;
}

/*!
 @brief Apply the inverse horizontal wavelet transform with descaling
 */
CODEC_ERROR InvertHorizontalDescale16s(PIXEL *lowpass, PIXEL *highpass, PIXEL *output,
                                       DIMENSION input_width, DIMENSION output_width,
                                       int descale)
{
    const int last_column = input_width - 1;
    int column = 0;
    int descale_shift = 0;
    int32_t even, odd;

    if (descale == 2) descale_shift = 1;
    else if (descale == 3) descale_shift = 2;
    assert(descale_shift >= 0);

    // Left border
    even = 11 * lowpass[0] - 4 * lowpass[1] + lowpass[2] + rounding;
    even = DivideByShift(even, 3);
    even = (even + highpass[0]) << descale_shift;

    odd = 5 * lowpass[0] + 4 * lowpass[1] - lowpass[2] + rounding;
    odd = DivideByShift(odd, 3);
    odd = (odd - highpass[0]) << descale_shift;

    output[0] = ClampPixel(even);
    output[1] = ClampPixel(odd);
    column = 1;

#if ENABLED(NEON)
    {
        const int32x4_t four = vdupq_n_s32(4);
        const int32x4_t descale_vec = vdupq_n_s32(descale_shift);

        for (; column + 3 < last_column; column += 4)
        {
            int32x4_t lp_left   = vld1q_s32(&lowpass[column - 1]);
            int32x4_t lp_center = vld1q_s32(&lowpass[column]);
            int32x4_t lp_right  = vld1q_s32(&lowpass[column + 1]);
            int32x4_t hp_center = vld1q_s32(&highpass[column]);

            int32x4_t diff_e = vsubq_s32(lp_left, lp_right);
            diff_e = vaddq_s32(diff_e, four);
            diff_e = vshrq_n_s32(diff_e, 3);
            int32x4_t even_v = vaddq_s32(diff_e, lp_center);
            even_v = vaddq_s32(even_v, hp_center);
            even_v = vshlq_s32(even_v, descale_vec);

            int32x4_t diff_o = vsubq_s32(lp_right, lp_left);
            diff_o = vaddq_s32(diff_o, four);
            diff_o = vshrq_n_s32(diff_o, 3);
            int32x4_t odd_v = vaddq_s32(diff_o, lp_center);
            odd_v = vsubq_s32(odd_v, hp_center);
            odd_v = vshlq_s32(odd_v, descale_vec);

            int32x4x2_t interleaved;
            interleaved.val[0] = even_v;
            interleaved.val[1] = odd_v;
            vst2q_s32(&output[2 * column], interleaved);
        }
    }
#endif

    // Scalar middle columns
    for (; column < last_column; column++)
    {
        even = lowpass[column - 1] - lowpass[column + 1] + 4;
        even >>= 3;
        even += lowpass[column];
        even = (even + highpass[column]) << descale_shift;
        output[2 * column] = ClampPixel(even);

        odd = -lowpass[column - 1] + lowpass[column + 1] + 4;
        odd >>= 3;
        odd += lowpass[column];
        odd = (odd - highpass[column]) << descale_shift;
        output[2 * column + 1] = ClampPixel(odd);
    }

    assert(column == last_column);

    // Right border
    even = 5 * lowpass[column] + 4 * lowpass[column - 1] - lowpass[column - 2] + rounding;
    even = DivideByShift(even, 3);
    even = (even + highpass[column]) << descale_shift;
    output[2 * column] = ClampPixel(even);

    if (2 * column + 1 < output_width)
    {
        odd = 11 * lowpass[column] - 4 * lowpass[column - 1] + lowpass[column - 2] + rounding;
        odd = DivideByShift(odd, 3);
        odd = (odd - highpass[column]) << descale_shift;
        output[2 * column + 1] = ClampPixel(odd);
    }

    return CODEC_ERROR_OKAY;
}

/*!
	@brief Apply the inverse spatial wavelet filter (no descaling)
 */
CODEC_ERROR InvertSpatialQuant16s(gpr_allocator *allocator,
                                  PIXEL *lowlow_band, int lowlow_pitch,
                                  PIXEL *lowhigh_band, int lowhigh_pitch,
                                  PIXEL *highlow_band, int highlow_pitch,
                                  PIXEL *highhigh_band, int highhigh_pitch,
                                  PIXEL *output_image, int output_pitch,
                                  DIMENSION input_width, DIMENSION input_height,
                                  DIMENSION output_width, DIMENSION output_height,
                                  QUANT quantization[])
{
    PIXEL *lowlow = (PIXEL *)lowlow_band;
    PIXEL *lowhigh = lowhigh_band;
    PIXEL *highlow = highlow_band;
    PIXEL *highhigh = highhigh_band;
    PIXEL *output = output_image;
    PIXEL *even_lowpass, *even_highpass, *odd_lowpass, *odd_highpass;
    PIXEL *even_output, *odd_output;
    size_t buffer_row_size;
    int last_row = input_height - 1;
    int row, column;
    PIXEL *lowhigh_row[3];
    PIXEL *lowhigh_line[3];
    PIXEL *highlow_line, *highhigh_line;

    QUANT highlow_quantization = quantization[HL_BAND];
    QUANT lowhigh_quantization = quantization[LH_BAND];
    QUANT highhigh_quantization = quantization[HH_BAND];

    buffer_row_size = input_width * sizeof(PIXEL);

    /* Single arena allocation for all 9 row buffers (1 malloc instead of 9) */
    size_t aligned_row = (buffer_row_size + 15) & ~(size_t)15;
    uint8_t *arena = (uint8_t *)allocator->Alloc(aligned_row * 9);
    if (!arena) return CODEC_ERROR_OUTOFMEMORY;
    even_lowpass    = (PIXEL *)(arena + aligned_row * 0);
    even_highpass   = (PIXEL *)(arena + aligned_row * 1);
    odd_lowpass     = (PIXEL *)(arena + aligned_row * 2);
    odd_highpass    = (PIXEL *)(arena + aligned_row * 3);
    lowhigh_line[0] = (PIXEL *)(arena + aligned_row * 4);
    lowhigh_line[1] = (PIXEL *)(arena + aligned_row * 5);
    lowhigh_line[2] = (PIXEL *)(arena + aligned_row * 6);
    highlow_line    = (PIXEL *)(arena + aligned_row * 7);
    highhigh_line   = (PIXEL *)(arena + aligned_row * 8);

    lowlow_pitch   /= sizeof(PIXEL);
    lowhigh_pitch  /= sizeof(PIXEL);
    highlow_pitch  /= sizeof(PIXEL);
    highhigh_pitch /= sizeof(PIXEL);
    output_pitch   /= sizeof(PIXEL);

    even_output = output;
    odd_output  = output + output_pitch;

    // First row (top border)
    row = 0;
    lowhigh_row[0] = lowhigh + 0 * lowhigh_pitch;
    lowhigh_row[1] = lowhigh + 1 * lowhigh_pitch;
    lowhigh_row[2] = lowhigh + 2 * lowhigh_pitch;

    DequantizeBandRow16s(lowhigh_row[0], input_width, lowhigh_quantization, lowhigh_line[0]);
    DequantizeBandRow16s(lowhigh_row[1], input_width, lowhigh_quantization, lowhigh_line[1]);
    DequantizeBandRow16s(lowhigh_row[2], input_width, lowhigh_quantization, lowhigh_line[2]);
    DequantizeBandRow16s(highlow,  input_width, highlow_quantization,  highlow_line);
    DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);

    column = 0;
#if ENABLED(NEON)
    {
        const int width_m4 = (input_width / 4) * 4;
        const int32x4_t four = vdupq_n_s32(4);
        const int32x4_t c11 = vdupq_n_s32(11);
        const int32x4_t c5  = vdupq_n_s32(5);
        const int32x4_t c4  = vdupq_n_s32(4);

        for (; column < width_m4; column += 4)
        {
            /* Left bands (lowlow + highlow) - top border */
            int32x4_t r0 = vld1q_s32(&lowlow[column + 0 * lowlow_pitch]);
            int32x4_t r1 = vld1q_s32(&lowlow[column + 1 * lowlow_pitch]);
            int32x4_t r2 = vld1q_s32(&lowlow[column + 2 * lowlow_pitch]);
            int32x4_t hp = vld1q_s32(&highlow_line[column]);

            /* even = (11*r0 - 4*r1 + r2 + 4) >> 3 + hp, then >> 1 */
            int32x4_t even_v = vmulq_s32(c11, r0);
            even_v = vmlsq_s32(even_v, c4, r1);
            even_v = vaddq_s32(even_v, r2);
            even_v = vaddq_s32(even_v, four);
            even_v = vshrq_n_s32(even_v, 3);
            even_v = vaddq_s32(even_v, hp);
            even_v = vshrq_n_s32(even_v, 1);
            vst1q_s32(&even_lowpass[column], even_v);

            /* odd = (5*r0 + 4*r1 - r2 + 4) >> 3 - hp, then >> 1 */
            int32x4_t odd_v = vmulq_s32(c5, r0);
            odd_v = vmlaq_s32(odd_v, c4, r1);
            odd_v = vsubq_s32(odd_v, r2);
            odd_v = vaddq_s32(odd_v, four);
            odd_v = vshrq_n_s32(odd_v, 3);
            odd_v = vsubq_s32(odd_v, hp);
            odd_v = vshrq_n_s32(odd_v, 1);
            vst1q_s32(&odd_lowpass[column], odd_v);

            /* Right bands (lowhigh + highhigh) - top border */
            r0 = vld1q_s32(&lowhigh_line[0][column]);
            r1 = vld1q_s32(&lowhigh_line[1][column]);
            r2 = vld1q_s32(&lowhigh_line[2][column]);
            hp = vld1q_s32(&highhigh_line[column]);

            even_v = vmulq_s32(c11, r0);
            even_v = vmlsq_s32(even_v, c4, r1);
            even_v = vaddq_s32(even_v, r2);
            even_v = vaddq_s32(even_v, four);
            even_v = vshrq_n_s32(even_v, 3);
            even_v = vaddq_s32(even_v, hp);
            even_v = vshrq_n_s32(even_v, 1);
            vst1q_s32(&even_highpass[column], even_v);

            odd_v = vmulq_s32(c5, r0);
            odd_v = vmlaq_s32(odd_v, c4, r1);
            odd_v = vsubq_s32(odd_v, r2);
            odd_v = vaddq_s32(odd_v, four);
            odd_v = vshrq_n_s32(odd_v, 3);
            odd_v = vsubq_s32(odd_v, hp);
            odd_v = vshrq_n_s32(odd_v, 1);
            vst1q_s32(&odd_highpass[column], odd_v);
        }
    }
#endif
    for (; column < input_width; column++)
    {
        int32_t even = 0, odd = 0;

        // Left bands (lowlow + highlow) - top border filter
        even += 11 * lowlow[column + 0 * lowlow_pitch];
        even -=  4 * lowlow[column + 1 * lowlow_pitch];
        even +=  1 * lowlow[column + 2 * lowlow_pitch];
        even += rounding;
        even = DivideByShift(even, 3);
        even += highlow_line[column];
        even >>= 1;
        even_lowpass[column] = ClampPixel(even);

        odd += 5 * lowlow[column + 0 * lowlow_pitch];
        odd += 4 * lowlow[column + 1 * lowlow_pitch];
        odd -= 1 * lowlow[column + 2 * lowlow_pitch];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        odd -= highlow_line[column];
        odd >>= 1;
        odd_lowpass[column] = ClampPixel(odd);

        // Right bands (lowhigh + highhigh) - top border filter
        even = 0; odd = 0;
        even += 11 * lowhigh_line[0][column];
        even -=  4 * lowhigh_line[1][column];
        even +=  1 * lowhigh_line[2][column];
        even += rounding;
        even = DivideByShift(even, 3);
        even += highhigh_line[column];
        even >>= 1;
        even_highpass[column] = ClampPixel(even);

        odd += 5 * lowhigh_line[0][column];
        odd += 4 * lowhigh_line[1][column];
        odd -= 1 * lowhigh_line[2][column];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        odd -= highhigh_line[column];
        odd >>= 1;
        odd_highpass[column] = ClampPixel(odd);
    }

    InvertHorizontal16s(even_lowpass, even_highpass, even_output, input_width, output_width);
    InvertHorizontal16s(odd_lowpass,  odd_highpass,  odd_output,  input_width, output_width);

    even_output += 2 * output_pitch;
    odd_output  += 2 * output_pitch;
    highlow  += highlow_pitch;
    highhigh += highhigh_pitch;
    row++;

    // Middle rows
    /* Detect fused-dequantize eligibility: negative quant = ANS raw mode.
       In fused mode, the vertical filter reads raw band data and dequantizes
       inline, eliminating the separate DequantizeBandRow16s + temp buffer. */
    int fuse_highlow  = (highlow_quantization < 0);
    int fuse_highhigh = (highhigh_quantization < 0);
#if ENABLED(NEON)
    int32x4_t hl_quant_vec = vdupq_n_s32(fuse_highlow  ? -highlow_quantization  : 1);
    int32x4_t hh_quant_vec = vdupq_n_s32(fuse_highhigh ? -highhigh_quantization : 1);
#endif

    for (; row < last_row; row++)
    {
        /* Only dequantize separately if NOT fusing (VLC mode with uncompanding) */
        if (!fuse_highlow)
            DequantizeBandRow16s(highlow,  input_width, highlow_quantization,  highlow_line);
        if (!fuse_highhigh)
            DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);

        column = 0;
#if ENABLED(NEON)
        {
            /* 8-wide loop: process 8 columns per iteration for 2x throughput */
            const int width_m8 = (input_width / 8) * 8;
            for (; column < width_m8; column += 8)
            {
                if (fuse_highlow)
                    InvertVerticalMiddle8_Fused_NEON(
                        lowlow + 0 * lowlow_pitch, lowlow + 1 * lowlow_pitch, lowlow + 2 * lowlow_pitch,
                        highlow, hl_quant_vec, even_lowpass, odd_lowpass, column);
                else
                    InvertVerticalMiddle8_NEON(
                        lowlow + 0 * lowlow_pitch, lowlow + 1 * lowlow_pitch, lowlow + 2 * lowlow_pitch,
                        highlow_line, even_lowpass, odd_lowpass, column);

                if (fuse_highhigh)
                    InvertVerticalMiddle8_Fused_NEON(
                        lowhigh_line[0], lowhigh_line[1], lowhigh_line[2],
                        highhigh, hh_quant_vec, even_highpass, odd_highpass, column);
                else
                    InvertVerticalMiddle8_NEON(
                        lowhigh_line[0], lowhigh_line[1], lowhigh_line[2],
                        highhigh_line, even_highpass, odd_highpass, column);
            }
            /* 4-wide cleanup for remaining 4-7 columns */
            const int width_m4 = (input_width / 4) * 4;
            for (; column < width_m4; column += 4)
            {
                if (fuse_highlow)
                    InvertVerticalMiddle4_Fused_NEON(
                        lowlow + 0 * lowlow_pitch, lowlow + 1 * lowlow_pitch, lowlow + 2 * lowlow_pitch,
                        highlow, hl_quant_vec, even_lowpass, odd_lowpass, column);
                else
                    InvertVerticalMiddle4_NEON(
                        lowlow + 0 * lowlow_pitch, lowlow + 1 * lowlow_pitch, lowlow + 2 * lowlow_pitch,
                        highlow_line, even_lowpass, odd_lowpass, column);

                if (fuse_highhigh)
                    InvertVerticalMiddle4_Fused_NEON(
                        lowhigh_line[0], lowhigh_line[1], lowhigh_line[2],
                        highhigh, hh_quant_vec, even_highpass, odd_highpass, column);
                else
                    InvertVerticalMiddle4_NEON(
                        lowhigh_line[0], lowhigh_line[1], lowhigh_line[2],
                        highhigh_line, even_highpass, odd_highpass, column);
            }
        }
#endif
        /* Scalar cleanup for remaining columns (< 4).
           For fused mode, dequantize the tail columns inline. */
        for (; column < input_width; column++)
        {
            int32_t even = 0, odd = 0;
            int32_t hl_val, hh_val;

            /* Inline dequantize for fused ANS mode, or read from pre-dequantized buffer */
            if (fuse_highlow) {
                int32_t v = highlow[column];
                hl_val = (v > 0) ? v * (-highlow_quantization) : (v < 0) ? -((-v) * (-highlow_quantization)) : 0;
            } else {
                hl_val = highlow_line[column];
            }
            if (fuse_highhigh) {
                int32_t v = highhigh[column];
                hh_val = (v > 0) ? v * (-highhigh_quantization) : (v < 0) ? -((-v) * (-highhigh_quantization)) : 0;
            } else {
                hh_val = highhigh_line[column];
            }

            even += lowlow[column + 0 * lowlow_pitch];
            even -= lowlow[column + 2 * lowlow_pitch];
            even += 4; even >>= 3;
            even += lowlow[column + 1 * lowlow_pitch];
            even += hl_val;
            even >>= 1;
            even_lowpass[column] = ClampPixel(even);

            odd -= lowlow[column + 0 * lowlow_pitch];
            odd += lowlow[column + 2 * lowlow_pitch];
            odd += 4; odd >>= 3;
            odd += lowlow[column + 1 * lowlow_pitch];
            odd -= hl_val;
            odd >>= 1;
            odd_lowpass[column] = ClampPixel(odd);

            even = 0; odd = 0;
            even += lowhigh_line[0][column];
            even -= lowhigh_line[2][column];
            even += 4; even >>= 3;
            even += lowhigh_line[1][column];
            even += hh_val;
            even >>= 1;
            even_highpass[column] = ClampPixel(even);

            odd -= lowhigh_line[0][column];
            odd += lowhigh_line[2][column];
            odd += 4; odd >>= 3;
            odd += lowhigh_line[1][column];
            odd -= hh_val;
            odd >>= 1;
            odd_highpass[column] = ClampPixel(odd);
        }

        InvertHorizontal16s(even_lowpass, even_highpass, even_output, input_width, output_width);
        InvertHorizontal16s(odd_lowpass,  odd_highpass,  odd_output,  input_width, output_width);

        lowlow   += lowlow_pitch;
        lowhigh  += lowhigh_pitch;
        highlow  += highlow_pitch;
        highhigh += highhigh_pitch;
        even_output += 2 * output_pitch;
        odd_output  += 2 * output_pitch;

        if (row < last_row - 1)
        {
            PIXEL *lowhigh_row_ptr = (lowhigh + 2 * lowhigh_pitch);
            PIXEL *temp = lowhigh_line[0];
            lowhigh_line[0] = lowhigh_line[1];
            lowhigh_line[1] = lowhigh_line[2];
            lowhigh_line[2] = temp;
            DequantizeBandRow16s(lowhigh_row_ptr, input_width, lowhigh_quantization, lowhigh_line[2]);
        }
    }

    assert(row == last_row);
    lowlow += lowlow_pitch;

    assert(lowlow == (lowlow_band + last_row * lowlow_pitch));
    assert(highlow == (highlow_band + last_row * highlow_pitch));
    assert(highhigh == (highhigh_band + last_row * highhigh_pitch));

    DequantizeBandRow16s(highlow,  input_width, highlow_quantization,  highlow_line);
    DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);

    // Last row (bottom border)
    column = 0;
#if ENABLED(NEON)
    {
        const int width_m4 = (input_width / 4) * 4;
        const int32x4_t four = vdupq_n_s32(4);
        const int32x4_t c11 = vdupq_n_s32(11);
        const int32x4_t c5  = vdupq_n_s32(5);
        const int32x4_t c4  = vdupq_n_s32(4);

        for (; column < width_m4; column += 4)
        {
            /* Left bands - bottom border:
               even = (5*r0 + 4*r_m1 - r_m2 + 4) >> 3 + hp, then >> 1
               odd  = (11*r0 - 4*r_m1 + r_m2 + 4) >> 3 - hp, then >> 1 */
            int32x4_t r0  = vld1q_s32(&lowlow[column + 0 * lowlow_pitch]);
            int32x4_t rm1 = vld1q_s32(&lowlow[column - 1 * lowlow_pitch]);
            int32x4_t rm2 = vld1q_s32(&lowlow[column - 2 * lowlow_pitch]);
            int32x4_t hp  = vld1q_s32(&highlow_line[column]);

            int32x4_t even_v = vmulq_s32(c5, r0);
            even_v = vmlaq_s32(even_v, c4, rm1);
            even_v = vsubq_s32(even_v, rm2);
            even_v = vaddq_s32(even_v, four);
            even_v = vshrq_n_s32(even_v, 3);
            even_v = vaddq_s32(even_v, hp);
            even_v = vshrq_n_s32(even_v, 1);
            vst1q_s32(&even_lowpass[column], even_v);

            int32x4_t odd_v = vmulq_s32(c11, r0);
            odd_v = vmlsq_s32(odd_v, c4, rm1);
            odd_v = vaddq_s32(odd_v, rm2);
            odd_v = vaddq_s32(odd_v, four);
            odd_v = vshrq_n_s32(odd_v, 3);
            odd_v = vsubq_s32(odd_v, hp);
            odd_v = vshrq_n_s32(odd_v, 1);
            vst1q_s32(&odd_lowpass[column], odd_v);

            /* Right bands - bottom border */
            r0  = vld1q_s32(&lowhigh_line[2][column]);
            rm1 = vld1q_s32(&lowhigh_line[1][column]);
            rm2 = vld1q_s32(&lowhigh_line[0][column]);
            hp  = vld1q_s32(&highhigh_line[column]);

            even_v = vmulq_s32(c5, r0);
            even_v = vmlaq_s32(even_v, c4, rm1);
            even_v = vsubq_s32(even_v, rm2);
            even_v = vaddq_s32(even_v, four);
            even_v = vshrq_n_s32(even_v, 3);
            even_v = vaddq_s32(even_v, hp);
            even_v = vshrq_n_s32(even_v, 1);
            vst1q_s32(&even_highpass[column], even_v);

            odd_v = vmulq_s32(c11, r0);
            odd_v = vmlsq_s32(odd_v, c4, rm1);
            odd_v = vaddq_s32(odd_v, rm2);
            odd_v = vaddq_s32(odd_v, four);
            odd_v = vshrq_n_s32(odd_v, 3);
            odd_v = vsubq_s32(odd_v, hp);
            odd_v = vshrq_n_s32(odd_v, 1);
            vst1q_s32(&odd_highpass[column], odd_v);
        }
    }
#endif
    for (; column < input_width; column++)
    {
        int32_t even = 0, odd = 0;

        // Left bands - bottom border
        even += 5  * lowlow[column + 0 * lowlow_pitch];
        even += 4  * lowlow[column - 1 * lowlow_pitch];
        even -= 1  * lowlow[column - 2 * lowlow_pitch];
        even += 4;
        even = DivideByShift(even, 3);
        even += highlow_line[column];
        even >>= 1;
        even_lowpass[column] = ClampPixel(even);

        odd += 11 * lowlow[column + 0 * lowlow_pitch];
        odd -=  4 * lowlow[column - 1 * lowlow_pitch];
        odd +=  1 * lowlow[column - 2 * lowlow_pitch];
        odd += 4;
        odd = DivideByShift(odd, 3);
        odd -= highlow_line[column];
        odd >>= 1;
        odd_lowpass[column] = ClampPixel(odd);

        // Right bands - bottom border
        even = 0; odd = 0;
        even += 5  * lowhigh_line[2][column];
        even += 4  * lowhigh_line[1][column];
        even -= 1  * lowhigh_line[0][column];
        even += 4;
        even = DivideByShift(even, 3);
        even += highhigh_line[column];
        even >>= 1;
        even_highpass[column] = ClampPixel(even);

        odd += 11 * lowhigh_line[2][column];
        odd -=  4 * lowhigh_line[1][column];
        odd +=  1 * lowhigh_line[0][column];
        odd += 4;
        odd = DivideByShift(odd, 3);
        odd -= highhigh_line[column];
        odd >>= 1;
        odd_highpass[column] = ClampPixel(odd);
    }

    InvertHorizontal16s(even_lowpass, even_highpass, even_output, input_width, output_width);
    if (2 * row + 1 < output_height)
        InvertHorizontal16s(odd_lowpass, odd_highpass, odd_output, input_width, output_width);

    allocator->Free(arena);

    return CODEC_ERROR_OKAY;
}

/*!
	@brief Apply the inverse spatial transform with descaling
 */
CODEC_ERROR InvertSpatialQuantDescale16s(gpr_allocator *allocator,
                                         PIXEL *lowlow_band, int lowlow_pitch,
                                         PIXEL *lowhigh_band, int lowhigh_pitch,
                                         PIXEL *highlow_band, int highlow_pitch,
                                         PIXEL *highhigh_band, int highhigh_pitch,
                                         PIXEL *output_image, int output_pitch,
                                         DIMENSION input_width, DIMENSION input_height,
                                         DIMENSION output_width, DIMENSION output_height,
                                         int descale, QUANT quantization[])
{
    PIXEL *lowlow = lowlow_band;
    PIXEL *lowhigh = lowhigh_band;
    PIXEL *highlow = highlow_band;
    PIXEL *highhigh = highhigh_band;
    PIXEL *output = output_image;
    PIXEL *even_lowpass, *even_highpass, *odd_lowpass, *odd_highpass;
    PIXEL *even_output, *odd_output;
    size_t buffer_row_size;
    int last_row = input_height - 1;
    int row, column;
    PIXEL *lowhigh_row[3];
    PIXEL *lowhigh_line[3];
    PIXEL *highlow_line, *highhigh_line;

    QUANT highlow_quantization = quantization[HL_BAND];
    QUANT lowhigh_quantization = quantization[LH_BAND];
    QUANT highhigh_quantization = quantization[HH_BAND];

    buffer_row_size = input_width * sizeof(PIXEL);

    /* Single arena allocation for all 9 row buffers (1 malloc instead of 9) */
    size_t aligned_row = (buffer_row_size + 15) & ~(size_t)15;
    uint8_t *arena = (uint8_t *)allocator->Alloc(aligned_row * 9);
    if (!arena) return CODEC_ERROR_OUTOFMEMORY;
    even_lowpass    = (PIXEL *)(arena + aligned_row * 0);
    even_highpass   = (PIXEL *)(arena + aligned_row * 1);
    odd_lowpass     = (PIXEL *)(arena + aligned_row * 2);
    odd_highpass    = (PIXEL *)(arena + aligned_row * 3);
    lowhigh_line[0] = (PIXEL *)(arena + aligned_row * 4);
    lowhigh_line[1] = (PIXEL *)(arena + aligned_row * 5);
    lowhigh_line[2] = (PIXEL *)(arena + aligned_row * 6);
    highlow_line    = (PIXEL *)(arena + aligned_row * 7);
    highhigh_line   = (PIXEL *)(arena + aligned_row * 8);

    lowlow_pitch   /= sizeof(PIXEL);
    lowhigh_pitch  /= sizeof(PIXEL);
    highlow_pitch  /= sizeof(PIXEL);
    highhigh_pitch /= sizeof(PIXEL);
    output_pitch   /= sizeof(PIXEL);

    even_output = output;
    odd_output  = output + output_pitch;

    // First row (top border)
    row = 0;
    lowhigh_row[0] = lowhigh + 0 * lowhigh_pitch;
    lowhigh_row[1] = lowhigh + 1 * lowhigh_pitch;
    lowhigh_row[2] = lowhigh + 2 * lowhigh_pitch;

    DequantizeBandRow16s(lowhigh_row[0], input_width, lowhigh_quantization, lowhigh_line[0]);
    DequantizeBandRow16s(lowhigh_row[1], input_width, lowhigh_quantization, lowhigh_line[1]);
    DequantizeBandRow16s(lowhigh_row[2], input_width, lowhigh_quantization, lowhigh_line[2]);
    DequantizeBandRow16s(highlow,  input_width, highlow_quantization,  highlow_line);
    DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);

    column = 0;
#if ENABLED(NEON)
    {
        const int width_m4 = (input_width / 4) * 4;
        const int32x4_t four = vdupq_n_s32(4);
        const int32x4_t c11 = vdupq_n_s32(11);
        const int32x4_t c5  = vdupq_n_s32(5);
        const int32x4_t c4  = vdupq_n_s32(4);

        for (; column < width_m4; column += 4)
        {
            /* Left bands - top border (descale: DivideByShift(x,1) = >> 1) */
            int32x4_t r0 = vld1q_s32(&lowlow[column + 0 * lowlow_pitch]);
            int32x4_t r1 = vld1q_s32(&lowlow[column + 1 * lowlow_pitch]);
            int32x4_t r2 = vld1q_s32(&lowlow[column + 2 * lowlow_pitch]);
            int32x4_t hp = vld1q_s32(&highlow_line[column]);

            int32x4_t even_v = vmulq_s32(c11, r0);
            even_v = vmlsq_s32(even_v, c4, r1);
            even_v = vaddq_s32(even_v, r2);
            even_v = vaddq_s32(even_v, four);
            even_v = vshrq_n_s32(even_v, 3);
            even_v = vaddq_s32(even_v, hp);
            even_v = vshrq_n_s32(even_v, 1);
            vst1q_s32(&even_lowpass[column], even_v);

            int32x4_t odd_v = vmulq_s32(c5, r0);
            odd_v = vmlaq_s32(odd_v, c4, r1);
            odd_v = vsubq_s32(odd_v, r2);
            odd_v = vaddq_s32(odd_v, four);
            odd_v = vshrq_n_s32(odd_v, 3);
            odd_v = vsubq_s32(odd_v, hp);
            odd_v = vshrq_n_s32(odd_v, 1);
            vst1q_s32(&odd_lowpass[column], odd_v);

            /* Right bands - top border */
            r0 = vld1q_s32(&lowhigh_line[0][column]);
            r1 = vld1q_s32(&lowhigh_line[1][column]);
            r2 = vld1q_s32(&lowhigh_line[2][column]);
            hp = vld1q_s32(&highhigh_line[column]);

            even_v = vmulq_s32(c11, r0);
            even_v = vmlsq_s32(even_v, c4, r1);
            even_v = vaddq_s32(even_v, r2);
            even_v = vaddq_s32(even_v, four);
            even_v = vshrq_n_s32(even_v, 3);
            even_v = vaddq_s32(even_v, hp);
            even_v = vshrq_n_s32(even_v, 1);
            vst1q_s32(&even_highpass[column], even_v);

            odd_v = vmulq_s32(c5, r0);
            odd_v = vmlaq_s32(odd_v, c4, r1);
            odd_v = vsubq_s32(odd_v, r2);
            odd_v = vaddq_s32(odd_v, four);
            odd_v = vshrq_n_s32(odd_v, 3);
            odd_v = vsubq_s32(odd_v, hp);
            odd_v = vshrq_n_s32(odd_v, 1);
            vst1q_s32(&odd_highpass[column], odd_v);
        }
    }
#endif
    for (; column < input_width; column++)
    {
        int32_t even = 0, odd = 0;

        // Left bands - top border (descale uses DivideByShift(x,1) instead of >>1)
        even += 11 * lowlow[column + 0 * lowlow_pitch];
        even -=  4 * lowlow[column + 1 * lowlow_pitch];
        even +=  1 * lowlow[column + 2 * lowlow_pitch];
        even += rounding;
        even = DivideByShift(even, 3);
        even += highlow_line[column];
        even = DivideByShift(even, 1);
        even_lowpass[column] = ClampPixel(even);

        odd += 5 * lowlow[column + 0 * lowlow_pitch];
        odd += 4 * lowlow[column + 1 * lowlow_pitch];
        odd -= 1 * lowlow[column + 2 * lowlow_pitch];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        odd -= highlow_line[column];
        odd = DivideByShift(odd, 1);
        odd_lowpass[column] = ClampPixel(odd);

        // Right bands - top border
        even = 0; odd = 0;
        even += 11 * lowhigh_line[0][column];
        even -=  4 * lowhigh_line[1][column];
        even +=  1 * lowhigh_line[2][column];
        even += rounding;
        even = DivideByShift(even, 3);
        even += highhigh_line[column];
        even = DivideByShift(even, 1);
        even_highpass[column] = ClampPixel(even);

        odd += 5 * lowhigh_line[0][column];
        odd += 4 * lowhigh_line[1][column];
        odd -= 1 * lowhigh_line[2][column];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        odd -= highhigh_line[column];
        odd = DivideByShift(odd, 1);
        odd_highpass[column] = ClampPixel(odd);
    }

    InvertHorizontalDescale16s(even_lowpass, even_highpass, even_output, input_width, output_width, descale);
    InvertHorizontalDescale16s(odd_lowpass,  odd_highpass,  odd_output,  input_width, output_width, descale);

    even_output += 2 * output_pitch;
    odd_output  += 2 * output_pitch;
    highlow  += highlow_pitch;
    highhigh += highhigh_pitch;
    row++;

    // Middle rows
    for (; row < last_row; row++)
    {
        DequantizeBandRow16s(highlow,  input_width, highlow_quantization,  highlow_line);
        DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);

        column = 0;
#if ENABLED(NEON)
        {
            const int width_m4 = (input_width / 4) * 4;
            for (; column < width_m4; column += 4)
            {
                // Left bands
                InvertVerticalMiddle4Descale_NEON(
                    lowlow + 0 * lowlow_pitch, lowlow + 1 * lowlow_pitch, lowlow + 2 * lowlow_pitch,
                    highlow_line, even_lowpass, odd_lowpass, column);
                // Right bands
                InvertVerticalMiddle4Descale_NEON(
                    lowhigh_line[0], lowhigh_line[1], lowhigh_line[2],
                    highhigh_line, even_highpass, odd_highpass, column);
            }
        }
#endif
        for (; column < input_width; column++)
        {
            int32_t even = 0, odd = 0;

            even += lowlow[column + 0 * lowlow_pitch];
            even -= lowlow[column + 2 * lowlow_pitch];
            even += 4; even >>= 3;
            even += lowlow[column + 1 * lowlow_pitch];
            even += highlow_line[column];
            even = DivideByShift(even, 1);
            even_lowpass[column] = ClampPixel(even);

            odd -= lowlow[column + 0 * lowlow_pitch];
            odd += lowlow[column + 2 * lowlow_pitch];
            odd += 4; odd >>= 3;
            odd += lowlow[column + 1 * lowlow_pitch];
            odd -= highlow_line[column];
            odd = DivideByShift(odd, 1);
            odd_lowpass[column] = ClampPixel(odd);

            even = 0; odd = 0;
            even += lowhigh_line[0][column];
            even -= lowhigh_line[2][column];
            even += 4; even >>= 3;
            even += lowhigh_line[1][column];
            even += highhigh_line[column];
            even = DivideByShift(even, 1);
            even_highpass[column] = ClampPixel(even);

            odd -= lowhigh_line[0][column];
            odd += lowhigh_line[2][column];
            odd += 4; odd >>= 3;
            odd += lowhigh_line[1][column];
            odd -= highhigh_line[column];
            odd = DivideByShift(odd, 1);
            odd_highpass[column] = ClampPixel(odd);
        }

        InvertHorizontalDescale16s(even_lowpass, even_highpass, even_output, input_width, output_width, descale);
        InvertHorizontalDescale16s(odd_lowpass,  odd_highpass,  odd_output,  input_width, output_width, descale);

        lowlow   += lowlow_pitch;
        lowhigh  += lowhigh_pitch;
        highlow  += highlow_pitch;
        highhigh += highhigh_pitch;
        even_output += 2 * output_pitch;
        odd_output  += 2 * output_pitch;

        if (row < last_row - 1)
        {
            PIXEL *lowhigh_row_ptr = (lowhigh + 2 * lowhigh_pitch);
            PIXEL *temp = lowhigh_line[0];
            lowhigh_line[0] = lowhigh_line[1];
            lowhigh_line[1] = lowhigh_line[2];
            lowhigh_line[2] = temp;
            DequantizeBandRow16s(lowhigh_row_ptr, input_width, lowhigh_quantization, lowhigh_line[2]);
        }
    }

    assert(row == last_row);
    lowlow += lowlow_pitch;

    assert(lowlow == (lowlow_band + last_row * lowlow_pitch));
    assert(highlow == (highlow_band + last_row * highlow_pitch));
    assert(highhigh == (highhigh_band + last_row * highhigh_pitch));

    DequantizeBandRow16s(highlow,  input_width, highlow_quantization,  highlow_line);
    DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);

    // Last row (bottom border)
    column = 0;
#if ENABLED(NEON)
    {
        const int width_m4 = (input_width / 4) * 4;
        const int32x4_t four = vdupq_n_s32(4);
        const int32x4_t c11 = vdupq_n_s32(11);
        const int32x4_t c5  = vdupq_n_s32(5);
        const int32x4_t c4  = vdupq_n_s32(4);

        for (; column < width_m4; column += 4)
        {
            /* Left bands - bottom border (descale) */
            int32x4_t r0  = vld1q_s32(&lowlow[column + 0 * lowlow_pitch]);
            int32x4_t rm1 = vld1q_s32(&lowlow[column - 1 * lowlow_pitch]);
            int32x4_t rm2 = vld1q_s32(&lowlow[column - 2 * lowlow_pitch]);
            int32x4_t hp  = vld1q_s32(&highlow_line[column]);

            int32x4_t even_v = vmulq_s32(c5, r0);
            even_v = vmlaq_s32(even_v, c4, rm1);
            even_v = vsubq_s32(even_v, rm2);
            even_v = vaddq_s32(even_v, four);
            even_v = vshrq_n_s32(even_v, 3);
            even_v = vaddq_s32(even_v, hp);
            even_v = vshrq_n_s32(even_v, 1);
            vst1q_s32(&even_lowpass[column], even_v);

            int32x4_t odd_v = vmulq_s32(c11, r0);
            odd_v = vmlsq_s32(odd_v, c4, rm1);
            odd_v = vaddq_s32(odd_v, rm2);
            odd_v = vaddq_s32(odd_v, four);
            odd_v = vshrq_n_s32(odd_v, 3);
            odd_v = vsubq_s32(odd_v, hp);
            odd_v = vshrq_n_s32(odd_v, 1);
            vst1q_s32(&odd_lowpass[column], odd_v);

            /* Right bands - bottom border (descale) */
            r0  = vld1q_s32(&lowhigh_line[2][column]);
            rm1 = vld1q_s32(&lowhigh_line[1][column]);
            rm2 = vld1q_s32(&lowhigh_line[0][column]);
            hp  = vld1q_s32(&highhigh_line[column]);

            even_v = vmulq_s32(c5, r0);
            even_v = vmlaq_s32(even_v, c4, rm1);
            even_v = vsubq_s32(even_v, rm2);
            even_v = vaddq_s32(even_v, four);
            even_v = vshrq_n_s32(even_v, 3);
            even_v = vaddq_s32(even_v, hp);
            even_v = vshrq_n_s32(even_v, 1);
            vst1q_s32(&even_highpass[column], even_v);

            odd_v = vmulq_s32(c11, r0);
            odd_v = vmlsq_s32(odd_v, c4, rm1);
            odd_v = vaddq_s32(odd_v, rm2);
            odd_v = vaddq_s32(odd_v, four);
            odd_v = vshrq_n_s32(odd_v, 3);
            odd_v = vsubq_s32(odd_v, hp);
            odd_v = vshrq_n_s32(odd_v, 1);
            vst1q_s32(&odd_highpass[column], odd_v);
        }
    }
#endif
    for (; column < input_width; column++)
    {
        int32_t even = 0, odd = 0;

        // Left bands - bottom border
        even += 5  * lowlow[column + 0 * lowlow_pitch];
        even += 4  * lowlow[column - 1 * lowlow_pitch];
        even -= 1  * lowlow[column - 2 * lowlow_pitch];
        even += rounding;
        even = DivideByShift(even, 3);
        even += highlow_line[column];
        even = DivideByShift(even, 1);
        even_lowpass[column] = ClampPixel(even);

        odd += 11 * lowlow[column + 0 * lowlow_pitch];
        odd -=  4 * lowlow[column - 1 * lowlow_pitch];
        odd +=  1 * lowlow[column - 2 * lowlow_pitch];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        odd -= highlow_line[column];
        odd = DivideByShift(odd, 1);
        odd_lowpass[column] = ClampPixel(odd);

        // Right bands - bottom border
        even = 0; odd = 0;
        even += 5  * lowhigh_line[2][column];
        even += 4  * lowhigh_line[1][column];
        even -= 1  * lowhigh_line[0][column];
        even += rounding;
        even = DivideByShift(even, 3);
        even += highhigh_line[column];
        even = DivideByShift(even, 1);
        even_highpass[column] = ClampPixel(even);

        odd += 11 * lowhigh_line[2][column];
        odd -=  4 * lowhigh_line[1][column];
        odd +=  1 * lowhigh_line[0][column];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        odd -= highhigh_line[column];
        odd = DivideByShift(odd, 1);
        odd_highpass[column] = ClampPixel(odd);
    }

    InvertHorizontalDescale16s(even_lowpass, even_highpass, even_output, input_width, output_width, descale);
    if (2 * row + 1 < output_height)
        InvertHorizontalDescale16s(odd_lowpass, odd_highpass, odd_output, input_width, output_width, descale);

    allocator->Free(arena);

    return CODEC_ERROR_OKAY;
}
