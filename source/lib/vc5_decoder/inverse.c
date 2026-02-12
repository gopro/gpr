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
STATIC CODEC_ERROR InvertHorizontal16s(PIXEL *lowpass, PIXEL *highpass, PIXEL *output,
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
STATIC CODEC_ERROR InvertHorizontalDescale16s(PIXEL *lowpass, PIXEL *highpass, PIXEL *output,
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

    even_lowpass  = (PIXEL *)allocator->Alloc(buffer_row_size);
    even_highpass = (PIXEL *)allocator->Alloc(buffer_row_size);
    odd_lowpass   = (PIXEL *)allocator->Alloc(buffer_row_size);
    odd_highpass  = (PIXEL *)allocator->Alloc(buffer_row_size);

    lowhigh_line[0] = (PIXEL *)allocator->Alloc(buffer_row_size);
    lowhigh_line[1] = (PIXEL *)allocator->Alloc(buffer_row_size);
    lowhigh_line[2] = (PIXEL *)allocator->Alloc(buffer_row_size);
    highlow_line    = (PIXEL *)allocator->Alloc(buffer_row_size);
    highhigh_line   = (PIXEL *)allocator->Alloc(buffer_row_size);

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

    for (column = 0; column < input_width; column++)
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
                InvertVerticalMiddle4_NEON(
                    lowlow + 0 * lowlow_pitch, lowlow + 1 * lowlow_pitch, lowlow + 2 * lowlow_pitch,
                    highlow_line, even_lowpass, odd_lowpass, column);
                // Right bands
                InvertVerticalMiddle4_NEON(
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
            even >>= 1;
            even_lowpass[column] = ClampPixel(even);

            odd -= lowlow[column + 0 * lowlow_pitch];
            odd += lowlow[column + 2 * lowlow_pitch];
            odd += 4; odd >>= 3;
            odd += lowlow[column + 1 * lowlow_pitch];
            odd -= highlow_line[column];
            odd >>= 1;
            odd_lowpass[column] = ClampPixel(odd);

            even = 0; odd = 0;
            even += lowhigh_line[0][column];
            even -= lowhigh_line[2][column];
            even += 4; even >>= 3;
            even += lowhigh_line[1][column];
            even += highhigh_line[column];
            even >>= 1;
            even_highpass[column] = ClampPixel(even);

            odd -= lowhigh_line[0][column];
            odd += lowhigh_line[2][column];
            odd += 4; odd >>= 3;
            odd += lowhigh_line[1][column];
            odd -= highhigh_line[column];
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
    for (column = 0; column < input_width; column++)
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

    allocator->Free(even_lowpass);
    allocator->Free(even_highpass);
    allocator->Free(odd_lowpass);
    allocator->Free(odd_highpass);
    allocator->Free(lowhigh_line[0]);
    allocator->Free(lowhigh_line[1]);
    allocator->Free(lowhigh_line[2]);
    allocator->Free(highlow_line);
    allocator->Free(highhigh_line);

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

    even_lowpass  = (PIXEL *)allocator->Alloc(buffer_row_size);
    even_highpass = (PIXEL *)allocator->Alloc(buffer_row_size);
    odd_lowpass   = (PIXEL *)allocator->Alloc(buffer_row_size);
    odd_highpass  = (PIXEL *)allocator->Alloc(buffer_row_size);

    lowhigh_line[0] = (PIXEL *)allocator->Alloc(buffer_row_size);
    lowhigh_line[1] = (PIXEL *)allocator->Alloc(buffer_row_size);
    lowhigh_line[2] = (PIXEL *)allocator->Alloc(buffer_row_size);
    highlow_line    = (PIXEL *)allocator->Alloc(buffer_row_size);
    highhigh_line   = (PIXEL *)allocator->Alloc(buffer_row_size);

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

    for (column = 0; column < input_width; column++)
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
    for (column = 0; column < input_width; column++)
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

    allocator->Free(even_lowpass);
    allocator->Free(even_highpass);
    allocator->Free(odd_lowpass);
    allocator->Free(odd_highpass);
    allocator->Free(lowhigh_line[0]);
    allocator->Free(lowhigh_line[1]);
    allocator->Free(lowhigh_line[2]);
    allocator->Free(highlow_line);
    allocator->Free(highhigh_line);

    return CODEC_ERROR_OKAY;
}
