/*! @file forward.c
 *
 *  @brief Implementation of the forward wavelet transform functions.
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

//! Rounding added to the highpass sum before division
static const int32_t rounding = 4;

STATIC_INLINE PIXEL QuantizeValue(int32_t value, int32_t midpoint, int32_t multiplier )
{
    int less_than_zero = 0;
    int negate_less_than_zero = 0;
    
    int32_t x = abs(value) + midpoint;
    
    if( value < 0 )
    {
        less_than_zero = -1;
        negate_less_than_zero = 1;
    }
    
    x = ((int64_t)x * multiplier) >> 16;
    
    x = x ^ less_than_zero;
    x = x + negate_less_than_zero;
    
    return ClampPixel(x);
}

static void FilterVerticalTopBottom_Core_8x_C_(PIXEL *coefficients[], int column, PIXEL* highpass, PIXEL* lowpass, bool top )
{
    const int filter_coeffs_top[]     = { 5, -11, 4, 4, -1, -1 };
    const int filter_coeffs_bottom[]  = { 1, 1, -4, -4, 11, -5 };
    
    int low_band_index = top ? 0 : 4;
    const int* filter_coeffs = top ? filter_coeffs_top : filter_coeffs_bottom;
    
    int i, f;
    
    for (i = 0; i < 8; i++)
    {
        lowpass[i] = coefficients[low_band_index + 0][column + i] + coefficients[low_band_index + 1][column + i];
    }

    for (i = 0; i < 8; i++)
    {
        int32_t sum = 0;

        for (f = 0; f < 6; f++)
        {
            sum += filter_coeffs[f] * coefficients[f][column + i];
        }

        sum += rounding;
        sum = DivideByShift(sum, 3);
        highpass[i] = sum;
    }
}

#if ENABLED(NEON)

#define HorizontalFilter_Prescale2_4x HorizontalFilter_Prescale2_4x_NEON_
void HorizontalFilter_Prescale2_4x_NEON_(PIXEL *input, PIXEL* lowpass, PIXEL* highpass )
{
    const int prescale_rounding = 3;
    int32_t scaled[12];

    for (int i = 0; i < 12; ++i)
    {
        scaled[i] = (input[i] + prescale_rounding) >> 2;
    }

    int32_t sum_prev_vals[4] = {
        scaled[0] + scaled[1],
        scaled[2] + scaled[3],
        scaled[4] + scaled[5],
        scaled[6] + scaled[7]
    };

    int32_t sum_next_vals[4] = {
        scaled[4] + scaled[5],
        scaled[6] + scaled[7],
        scaled[8] + scaled[9],
        scaled[10] + scaled[11]
    };

    int32_t diff_vals[4] = {
        scaled[2] - scaled[3],
        scaled[4] - scaled[5],
        scaled[6] - scaled[7],
        scaled[8] - scaled[9]
    };

    int32x4_t sum_prev = vld1q_s32(sum_prev_vals);
    int32x4_t sum_next = vld1q_s32(sum_next_vals);
    int32x4_t diffs = vld1q_s32(diff_vals);

    int32x4_t hp = vsubq_s32(sum_next, sum_prev);
    hp = vaddq_s32(hp, vdupq_n_s32(rounding));
    hp = vshrq_n_s32(hp, 3);
    hp = vaddq_s32(hp, diffs);
    vst1q_s32(highpass, hp);

    int32_t low_vals[4] = {
        (input[2] + input[3] + prescale_rounding) >> 2,
        (input[4] + input[5] + prescale_rounding) >> 2,
        (input[6] + input[7] + prescale_rounding) >> 2,
        (input[8] + input[9] + prescale_rounding) >> 2
    };
    vst1q_s32(lowpass, vld1q_s32(low_vals));
}

#define HorizontalFilter_Prescale3_4x HorizontalFilter_Prescale3_4x_NEON_
void HorizontalFilter_Prescale3_4x_NEON_(PIXEL *input, PIXEL* lowpass, PIXEL* highpass )
{
    const int prescale_rounding = 7;
    int32_t scaled[12];

    for (int i = 0; i < 12; ++i)
    {
        scaled[i] = (input[i] + prescale_rounding) >> 3;
    }

    int32_t sum_prev_vals[4] = {
        scaled[0] + scaled[1],
        scaled[2] + scaled[3],
        scaled[4] + scaled[5],
        scaled[6] + scaled[7]
    };

    int32_t sum_next_vals[4] = {
        scaled[4] + scaled[5],
        scaled[6] + scaled[7],
        scaled[8] + scaled[9],
        scaled[10] + scaled[11]
    };

    int32_t diff_vals[4] = {
        scaled[2] - scaled[3],
        scaled[4] - scaled[5],
        scaled[6] - scaled[7],
        scaled[8] - scaled[9]
    };

    int32x4_t sum_prev = vld1q_s32(sum_prev_vals);
    int32x4_t sum_next = vld1q_s32(sum_next_vals);
    int32x4_t diffs = vld1q_s32(diff_vals);

    int32x4_t hp = vsubq_s32(sum_next, sum_prev);
    hp = vaddq_s32(hp, vdupq_n_s32(rounding));
    hp = vshrq_n_s32(hp, 3);
    hp = vaddq_s32(hp, diffs);
    vst1q_s32(highpass, hp);

    int32_t low_vals[4] = {
        (input[2] + input[3] + prescale_rounding) >> 3,
        (input[4] + input[5] + prescale_rounding) >> 3,
        (input[6] + input[7] + prescale_rounding) >> 3,
        (input[8] + input[9] + prescale_rounding) >> 3
    };
    vst1q_s32(lowpass, vld1q_s32(low_vals));
}

#define HorizontalFilter_Prescale0_4x HorizontalFilter_Prescale0_4x_NEON_
void HorizontalFilter_Prescale0_4x_NEON_(PIXEL *input, PIXEL* lowpass, PIXEL* highpass )
{
    int32_t sum_prev_vals[4] = {
        input[0] + input[1],
        input[2] + input[3],
        input[4] + input[5],
        input[6] + input[7]
    };

    int32_t sum_next_vals[4] = {
        input[4] + input[5],
        input[6] + input[7],
        input[8] + input[9],
        input[10] + input[11]
    };

    int32_t diff_vals[4] = {
        input[2] - input[3],
        input[4] - input[5],
        input[6] - input[7],
        input[8] - input[9]
    };

    int32x4_t sum_prev = vld1q_s32(sum_prev_vals);
    int32x4_t sum_next = vld1q_s32(sum_next_vals);
    int32x4_t diffs = vld1q_s32(diff_vals);

    int32x4_t hp = vsubq_s32(sum_next, sum_prev);
    hp = vaddq_s32(hp, vdupq_n_s32(rounding));
    hp = vshrq_n_s32(hp, 3);
    hp = vaddq_s32(hp, diffs);
    vst1q_s32(highpass, hp);

    int32_t low_vals[4] = {
        input[2] + input[3],
        input[4] + input[5],
        input[6] + input[7],
        input[8] + input[9]
    };
    vst1q_s32(lowpass, vld1q_s32(low_vals));
}

static INLINE int32x4_t QuantizeVector(int32x4_t values, int32_t midpoint, int32_t multiplier)
{
    const int32x4_t midpoint_vec = vdupq_n_s32(midpoint);
    int32x4_t abs_vals = vabsq_s32(values);
    abs_vals = vaddq_s32(abs_vals, midpoint_vec);

    const int32x2_t multiplier_vec = vdup_n_s32(multiplier);
    int64x2_t prod_low = vmull_s32(vget_low_s32(abs_vals), multiplier_vec);
    int64x2_t prod_high = vmull_s32(vget_high_s32(abs_vals), multiplier_vec);

    prod_low = vshrq_n_s64(prod_low, 16);
    prod_high = vshrq_n_s64(prod_high, 16);

    int32x4_t scaled = vcombine_s32(vmovn_s64(prod_low), vmovn_s64(prod_high));

    const int32x4_t zero = vdupq_n_s32(0);
    uint32x4_t neg_mask = vcltq_s32(values, zero);
    int32x4_t negated = vnegq_s32(scaled);
    return vbslq_s32(neg_mask, negated, scaled);
}

void QuantizeBand_8x_NEON_(const PIXEL* wavelet_band, int32_t midpoint, int32_t multiplier, PIXEL *output )
{
    int32x4_t v0 = vld1q_s32(wavelet_band);
    int32x4_t v1 = vld1q_s32(wavelet_band + 4);
    int32x4_t q0 = QuantizeVector(v0, midpoint, multiplier);
    int32x4_t q1 = QuantizeVector(v1, midpoint, multiplier);
    vst1q_s32(output, q0);
    vst1q_s32(output + 4, q1);
}

static INLINE void FilterVerticalCoreBlock(int32x4_t row0,
                                           int32x4_t row1,
                                           int32x4_t row2,
                                           int32x4_t row3,
                                           int32x4_t row4,
                                           int32x4_t row5,
                                           PIXEL *high_dest,
                                           PIXEL *low_dest)
{
    int32x4_t high = vsubq_s32(row5, row0);
    high = vaddq_s32(high, vsubq_s32(row4, row1));
    high = vaddq_s32(high, vdupq_n_s32(rounding));
    high = vshrq_n_s32(high, 3);
    high = vaddq_s32(high, vsubq_s32(row2, row3));

    int32x4_t low = vaddq_s32(row2, row3);

    vst1q_s32(high_dest, high);
    vst1q_s32(low_dest, low);
}

void FilterVerticalMiddle_Core_8x_NEON_(PIXEL *coefficients[], int column, PIXEL* highpass, PIXEL* lowpass )
{
    int32x4_t row0_lo = vld1q_s32(&coefficients[0][column]);
    int32x4_t row0_hi = vld1q_s32(&coefficients[0][column + 4]);
    int32x4_t row1_lo = vld1q_s32(&coefficients[1][column]);
    int32x4_t row1_hi = vld1q_s32(&coefficients[1][column + 4]);
    int32x4_t row2_lo = vld1q_s32(&coefficients[2][column]);
    int32x4_t row2_hi = vld1q_s32(&coefficients[2][column + 4]);
    int32x4_t row3_lo = vld1q_s32(&coefficients[3][column]);
    int32x4_t row3_hi = vld1q_s32(&coefficients[3][column + 4]);
    int32x4_t row4_lo = vld1q_s32(&coefficients[4][column]);
    int32x4_t row4_hi = vld1q_s32(&coefficients[4][column + 4]);
    int32x4_t row5_lo = vld1q_s32(&coefficients[5][column]);
    int32x4_t row5_hi = vld1q_s32(&coefficients[5][column + 4]);

    FilterVerticalCoreBlock(row0_lo, row1_lo, row2_lo, row3_lo, row4_lo, row5_lo, highpass, lowpass);
    FilterVerticalCoreBlock(row0_hi, row1_hi, row2_hi, row3_hi, row4_hi, row5_hi, highpass + 4, lowpass + 4);
}

#define FilterVerticalMiddle_8x FilterVerticalMiddle_8x_NEON_
void FilterVerticalMiddle_8x_NEON_(PIXEL *lowpass[], PIXEL *highpass[], int column, int32_t* midpoints, int32_t* multipliers, PIXEL *result[])
{
    PIXEL LOW[8];
    PIXEL HIGH[8];

    FilterVerticalMiddle_Core_8x_NEON_( highpass, column, HIGH, LOW);
    QuantizeBand_8x_NEON_( LOW, midpoints[LH_BAND], multipliers[LH_BAND], result[LH_BAND] + column );
    QuantizeBand_8x_NEON_( HIGH, midpoints[HH_BAND], multipliers[HH_BAND], result[HH_BAND] + column );

    FilterVerticalMiddle_Core_8x_NEON_( lowpass, column, HIGH, LOW);
    QuantizeBand_8x_NEON_( LOW, midpoints[LL_BAND], multipliers[LL_BAND], result[LL_BAND] + column );
    QuantizeBand_8x_NEON_( HIGH, midpoints[HL_BAND], multipliers[HL_BAND], result[HL_BAND] + column );
}

#define FilterVerticalTopBottom_8x FilterVerticalTopBottom_8x_NEON_
void FilterVerticalTopBottom_8x_NEON_(PIXEL *lowpass[], PIXEL *highpass[], int column, int32_t* midpoints, int32_t* multipliers, PIXEL *result[], bool top )
{
    PIXEL LOW[8];
    PIXEL HIGH[8];

    FilterVerticalTopBottom_Core_8x_C_( highpass, column, HIGH, LOW, top );
    QuantizeBand_8x_NEON_( LOW, midpoints[LH_BAND], multipliers[LH_BAND], result[LH_BAND] + column );
    QuantizeBand_8x_NEON_( HIGH, midpoints[HH_BAND], multipliers[HH_BAND], result[HH_BAND] + column );

    FilterVerticalTopBottom_Core_8x_C_( lowpass, column, HIGH, LOW, top );
    QuantizeBand_8x_NEON_( LOW, midpoints[LL_BAND], multipliers[LL_BAND], result[LL_BAND] + column );
    QuantizeBand_8x_NEON_( HIGH, midpoints[HL_BAND], multipliers[HL_BAND], result[HL_BAND] + column );
}


#else

#define HorizontalFilter_Prescale2_4x HorizontalFilter_Prescale2_4x_C_
void HorizontalFilter_Prescale2_4x_C_(PIXEL *input, PIXEL* lowpass, PIXEL* highpass )
{
    int i;
    PIXEL input_c[16];
    for ( i = 0; i < 12; i++)
    {
        input_c[i] = (input[i] + 3) >> 2;
    }
    
    int32_t diff_23 = input_c[2] - input_c[3];
    int32_t diff_45 = input_c[4] - input_c[5];
    int32_t diff_67 = input_c[6] - input_c[7];
    int32_t diff_89 = input_c[8] - input_c[9];
    
    int32_t sum_01  	= input_c[0]  + input_c[1];
    int32_t sum_23  	= input_c[2]  + input_c[3];
    int32_t sum_45  	= input_c[4]  + input_c[5];
    int32_t sum_67  	= input_c[6]  + input_c[7];
    int32_t sum_89  	= input_c[8]  + input_c[9];
    int32_t sum_1011  	= input_c[10] + input_c[11];
    
    {
        int32_t sum = sum_45 - sum_01;
        
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_23;
        
        highpass[0] = ClampPixel(sum);
    }
    
    {
        int32_t sum = sum_67 - sum_23;
        
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_45;
        
        highpass[1] = ClampPixel(sum);
    }
    
    {
        int32_t sum = sum_89 - sum_45;
        
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_67;
        
        highpass[2] = ClampPixel(sum);
    }
    
    {
        int32_t sum = sum_1011 - sum_67;
        
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_89;
        
        highpass[3] = ClampPixel(sum);
    }
    
    lowpass[0] = (input[2] + input[3] + 3) >> 2;
    lowpass[1] = (input[4] + input[5] + 3) >> 2;
    lowpass[2] = (input[6] + input[7] + 3) >> 2;
    lowpass[3] = (input[8] + input[9] + 3) >> 2;
}

#define HorizontalFilter_Prescale3_4x HorizontalFilter_Prescale3_4x_C_
void HorizontalFilter_Prescale3_4x_C_(PIXEL *input, PIXEL* lowpass, PIXEL* highpass )
{
    int i;
    PIXEL input_c[16];
    for ( i = 0; i < 12; i++)
    {
        input_c[i] = (input[i] + 7) >> 3;
    }

    int32_t diff_23 = input_c[2] - input_c[3];
    int32_t diff_45 = input_c[4] - input_c[5];
    int32_t diff_67 = input_c[6] - input_c[7];
    int32_t diff_89 = input_c[8] - input_c[9];

    int32_t sum_01   = input_c[0]  + input_c[1];
    int32_t sum_23   = input_c[2]  + input_c[3];
    int32_t sum_45   = input_c[4]  + input_c[5];
    int32_t sum_67   = input_c[6]  + input_c[7];
    int32_t sum_89   = input_c[8]  + input_c[9];
    int32_t sum_1011 = input_c[10] + input_c[11];

    {
        int32_t sum = sum_45 - sum_01;
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_23;
        highpass[0] = ClampPixel(sum);
    }

    {
        int32_t sum = sum_67 - sum_23;
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_45;
        highpass[1] = ClampPixel(sum);
    }

    {
        int32_t sum = sum_89 - sum_45;
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_67;
        highpass[2] = ClampPixel(sum);
    }

    {
        int32_t sum = sum_1011 - sum_67;
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_89;
        highpass[3] = ClampPixel(sum);
    }

    lowpass[0] = (input[2] + input[3] + 7) >> 3;
    lowpass[1] = (input[4] + input[5] + 7) >> 3;
    lowpass[2] = (input[6] + input[7] + 7) >> 3;
    lowpass[3] = (input[8] + input[9] + 7) >> 3;
}

#define HorizontalFilter_Prescale0_4x HorizontalFilter_Prescale0_4x_C_
void HorizontalFilter_Prescale0_4x_C_(PIXEL *input, PIXEL* lowpass, PIXEL* highpass )
{
    PIXEL input_c[16];
    
    memcpy(input_c, input, sizeof(PIXEL) * 12);
    
    int32_t diff_23 = input_c[2] - input_c[3];
    int32_t diff_45 = input_c[4] - input_c[5];
    int32_t diff_67 = input_c[6] - input_c[7];
    int32_t diff_89 = input_c[8] - input_c[9];
    
    int32_t sum_01  	= input_c[0]  + input_c[1];
    int32_t sum_23  	= input_c[2]  + input_c[3];
    int32_t sum_45  	= input_c[4]  + input_c[5];
    int32_t sum_67  	= input_c[6]  + input_c[7];
    int32_t sum_89  	= input_c[8]  + input_c[9];
    int32_t sum_1011  	= input_c[10] + input_c[11];
    
    {
        int32_t sum = sum_45 - sum_01;
        
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_23;
        
        highpass[0] = ClampPixel(sum);
    }
    
    {
        int32_t sum = sum_67 - sum_23;
        
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_45;
        
        highpass[1] = ClampPixel(sum);
    }
    
    {
        int32_t sum = sum_89 - sum_45;
        
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_67;
        
        highpass[2] = ClampPixel(sum);
    }
    
    {
        int32_t sum = sum_1011 - sum_67;
        
        sum += rounding;
        sum = DivideByShift(sum, 3);
        sum += diff_89;
        
        highpass[3] = ClampPixel(sum);
    }
    
    lowpass[0] = input[2] + input[3];
    lowpass[1] = input[4] + input[5];
    lowpass[2] = input[6] + input[7];
    lowpass[3] = input[8] + input[9];
}

void FilterVerticalMiddle_Core_8x_C_(PIXEL *coefficients[], int column, PIXEL* highpass, PIXEL* lowpass )
{
    PIXEL row_0[8];
    PIXEL row_1[8];
    PIXEL row_2[8];
    PIXEL row_3[8];
    PIXEL row_4[8];
    PIXEL row_5[8];
    
    int i;
    
    memcpy( row_0, &coefficients[0][column], sizeof(PIXEL) * 8 );
    memcpy( row_1, &coefficients[1][column], sizeof(PIXEL) * 8 );
    memcpy( row_2, &coefficients[2][column], sizeof(PIXEL) * 8 );
    memcpy( row_3, &coefficients[3][column], sizeof(PIXEL) * 8 );
    memcpy( row_4, &coefficients[4][column], sizeof(PIXEL) * 8 );
    memcpy( row_5, &coefficients[5][column], sizeof(PIXEL) * 8 );
    
    for (i = 0; i < 8; i++)
        highpass[i]  = (-1 * row_0[i] + row_5[i]);
    
    for (i = 0; i < 8; i++)
        highpass[i] += (-1 * row_1[i] + row_4[i]);
    
    for (i = 0; i < 8; i++)
        highpass[i] += rounding;
    
    for (i = 0; i < 8; i++)
        highpass[i] = DivideByShift(highpass[i], 3);
    
    for (i = 0; i < 8; i++)
    {
        highpass[i] += (row_2[i] - row_3[i]);
        lowpass[i]   = (row_2[i] + row_3[i]);
    }
}


#define FilterVerticalMiddle_8x FilterVerticalMiddle_8x_C_
void FilterVerticalMiddle_8x_C_(PIXEL *lowpass[], PIXEL *highpass[], int column, int32_t* midpoints, int32_t* multipliers, PIXEL *result[])
{
    int i;
    
    PIXEL LL[8];
    PIXEL HL[8];
    PIXEL LH[8];
    PIXEL HH[8];
    
    FilterVerticalMiddle_Core_8x_C_( highpass, column, HH, LH);
    FilterVerticalMiddle_Core_8x_C_( lowpass, column, HL, LL);
    
    for (i = 0; i < 8; i++)
    {
        result[LL_BAND][column + i] = QuantizeValue( LL[i], midpoints[LL_BAND], multipliers[LL_BAND] );
        result[LH_BAND][column + i] = QuantizeValue( LH[i], midpoints[LH_BAND], multipliers[LH_BAND] );
        result[HL_BAND][column + i] = QuantizeValue( HL[i], midpoints[HL_BAND], multipliers[HL_BAND] );
        result[HH_BAND][column + i] = QuantizeValue( HH[i], midpoints[HH_BAND], multipliers[HH_BAND] );
    }
}

#define FilterVerticalTopBottom_8x FilterVerticalTopBottom_8x_C_
void FilterVerticalTopBottom_8x_C_(PIXEL *lowpass[], PIXEL *highpass[], int column, int32_t* midpoints, int32_t* multipliers, PIXEL *result[], bool top )
{
    int i;
    
    PIXEL LL[8];
    PIXEL HL[8];
    PIXEL LH[8];
    PIXEL HH[8];
    
    FilterVerticalTopBottom_Core_8x_C_( highpass, column, HH, LH, top );
    FilterVerticalTopBottom_Core_8x_C_( lowpass, column, HL, LL, top );
    
    for (i = 0; i < 8; i++)
    {
        result[LL_BAND][column + i] = QuantizeValue( LL[i], midpoints[LL_BAND], multipliers[LL_BAND] );
        result[LH_BAND][column + i] = QuantizeValue( LH[i], midpoints[LH_BAND], multipliers[LH_BAND] );
        result[HL_BAND][column + i] = QuantizeValue( HL[i], midpoints[HL_BAND], multipliers[HL_BAND] );
        result[HH_BAND][column + i] = QuantizeValue( HH[i], midpoints[HH_BAND], multipliers[HH_BAND] );
    }
}

#endif

static PIXEL HorizontalHighPassFilter_Middle(PIXEL *input, int prescale_rounding, int prescale)
{
    int32_t sum;
    
    if( prescale == 0 )
    {
        sum = -input[0] - input[1] + (input[2] << 3) - (input[3] << 3) + input[4] + input[5];
    }
    else
    {
        sum = 0;
        sum -=  (input[0] + prescale_rounding) >> prescale;
        sum -=  (input[1] + prescale_rounding) >> prescale;
        sum += ((input[2] + prescale_rounding) >> prescale) << 3;
        sum -= ((input[3] + prescale_rounding) >> prescale) << 3;
        sum +=  (input[4] + prescale_rounding) >> prescale;
        sum +=  (input[5] + prescale_rounding) >> prescale;
    }
    
    sum += rounding;
    sum = DivideByShift(sum, 3);
    return ClampPixel(sum);
}

static PIXEL HorizontalHighPassFilter(PIXEL *input, PIXEL *multipliers, int prescale_rounding, int prescale)
{
    int i;
    int32_t sum = 0;
    
    if( prescale == 0 )
    {
        for (i = 0; i < 6; i++)
        {
            sum += multipliers[i] * input[i];
        }
        
    }
    else
    {
        for (i = 0; i < 6; i++)
        {
            sum += multipliers[i] * ( (input[i] + prescale_rounding) >> prescale);
        }
    }
    
    sum += rounding;
    sum = DivideByShift(sum, 3);
    return ClampPixel(sum);
}

/*!
	@brief Apply the horizontal wavelet filter to a row of pixels
*/
CODEC_ERROR FilterHorizontalRow(PIXEL *input, PIXEL *lowpass, PIXEL *highpass, int width, int prescale)
{
	int column = 2;

	//uint16_t *input = (uint16_t *)input_buffer;

	//TODO: Check that the rounding is correct for all prescale values
	int prescale_rounding = (1 << prescale) - 1;

	const int last_input_column = ((width % 2) == 0) ? width - 2 : width - 1;
    
    int last_input_column_tight = ( (last_input_column - 4) / 8) * 8;

    // Clamp prescale to supported values (0, 2, or 3)
    if (prescale != 0 && prescale != 2 && prescale != 3)
    {
        prescale = (prescale > 3) ? 3 : ((prescale > 0) ? 2 : 0);
        prescale_rounding = (1 << prescale) - 1;
    }
    
	/***** Process the left border using the formula for boundary conditions *****/
    
	// Compute the lowpass coefficient
	lowpass[0] = (input[0] + input[1] + prescale_rounding) >> prescale;

    {
        PIXEL coefficients[6] = { 5, -11, 4, 4, -1, -1 };
        highpass[0] = HorizontalHighPassFilter(input, coefficients, prescale_rounding, prescale );
    }
    
    if( prescale == 2 )
    {
        /***** Process the internal pixels using the normal wavelet formula *****/
        for (; column < last_input_column_tight; column += 8) //
        {
            if (column & 1) column--;
            
            HorizontalFilter_Prescale2_4x( input + column - 2, &lowpass[column/2], &highpass[column/2] );
        }
    }
    else if( prescale == 0 )
    {
        /***** Process the internal pixels using the normal wavelet formula *****/
        for (; column < last_input_column_tight; column += 8) //
        {
            if (column & 1) column--;
            
            HorizontalFilter_Prescale0_4x( input + column - 2, &lowpass[column/2], &highpass[column/2] );
        }
    }
    else if( prescale == 3 )
    {
        /***** Process the internal pixels using the normal wavelet formula *****/
        for (; column < last_input_column_tight; column += 8)
        {
            if (column & 1) column--;

            HorizontalFilter_Prescale3_4x( input + column - 2, &lowpass[column/2], &highpass[column/2] );
        }
    }
    
    for (; column < last_input_column; column += 2)
    {
        if (column & 1) column--;
        
        // Compute the lowpass coefficient
        lowpass[column/2] = (input[column + 0] + input[column + 1] + prescale_rounding) >> prescale;
        
        // Initialize the sum for computing the highpass coefficient
        if ((column + 3) < width)
        {
            highpass[column/2] = HorizontalHighPassFilter_Middle(input + column - 2, prescale_rounding, prescale );
        }
        else
        {
            int32_t sum = 0;
            
            sum -= (input[column - 2] + prescale_rounding) >> prescale;
            sum -= (input[column - 1] + prescale_rounding) >> prescale;
            sum += (input[column + 2] + prescale_rounding) >> prescale;

            // Duplicate the value in the last column
            sum += (input[column + 2] + prescale_rounding) >> prescale;
            sum += rounding;
            sum = DivideByShift(sum, 3);
            sum += (input[column + 0] + prescale_rounding) >> prescale;
            sum -= (input[column + 1] + prescale_rounding) >> prescale;
            highpass[column/2] = ClampPixel(sum);
        }
    }
    
    column = last_input_column;

	/***** Process the right border using the formula for boundary conditions *****/

	// Compute the lowpass coefficient
	if ((column + 1) < width)
	{
        PIXEL coefficients[6] = { 1, 1, -4, -4, 11, -5 };
        highpass[column/2] = HorizontalHighPassFilter(input + column - 4, coefficients, prescale_rounding, prescale );

        // Use the value in the last column
		lowpass[column/2] = (input[column + 0] + input[column + 1] + prescale_rounding) >> prescale;
	}
	else
	{
        int32_t sum = 0;
        // Duplicate the value in the last column
        sum -=  5 * ((input[column + 0] + prescale_rounding) >> prescale);
        
        sum += 11 * ((input[column + 0] + prescale_rounding) >> prescale);
        sum -=  4 * ((input[column - 1] + prescale_rounding) >> prescale);
        sum -=  4 * ((input[column - 2] + prescale_rounding) >> prescale);
        sum +=  1 * ((input[column - 3] + prescale_rounding) >> prescale);
        sum +=  1 * ((input[column - 4] + prescale_rounding) >> prescale);
        sum += rounding;
        sum = DivideByShift(sum, 3);
        
        highpass[column/2] = ClampPixel(sum);

        // Duplicate the value in the last column
		lowpass[column/2] = (input[column + 0] + input[column + 0] + prescale_rounding) >> prescale;
	}

	return CODEC_ERROR_OKAY;
}

static void FilterVerticalMiddle_1x(PIXEL *lowpass[], PIXEL *highpass[], int column, int32_t* midpoints, int32_t* multipliers, PIXEL *result[])
{
    {
        const PIXEL coefficients_01 = lowpass[0][column] + lowpass[1][column];
        const PIXEL coefficients_2  = lowpass[2][column];
        const PIXEL coefficients_3  = lowpass[3][column];
        const PIXEL coefficients_45 = lowpass[4][column] + lowpass[5][column];
        
        int32_t sum =  coefficients_45 - coefficients_01;
        sum +=  8 * (coefficients_2 - coefficients_3);
        sum = DivideByShift(sum + rounding, 3);
        
        result[LL_BAND][column] = QuantizeValue( coefficients_2 + coefficients_3, midpoints[LL_BAND], multipliers[LL_BAND] );
        result[HL_BAND][column] = QuantizeValue( sum, midpoints[HL_BAND], multipliers[HL_BAND] );
    }
    
    {
        const PIXEL coefficients_01 = highpass[0][column] + highpass[1][column];
        const PIXEL coefficients_2  = highpass[2][column];
        const PIXEL coefficients_3  = highpass[3][column];
        const PIXEL coefficients_45 = highpass[4][column] + highpass[5][column];
        
        int32_t sum =  coefficients_45 - coefficients_01;
        sum +=  8 * (coefficients_2 - coefficients_3);
        sum = DivideByShift(sum + rounding, 3);
        
        result[LH_BAND][column] = QuantizeValue( coefficients_2 + coefficients_3, midpoints[LH_BAND], multipliers[LH_BAND] );
        result[HH_BAND][column] = QuantizeValue( sum, midpoints[HH_BAND], multipliers[HH_BAND] );
    }
}

static void FilterVerticalTopBottom_1x(PIXEL *lowpass[], PIXEL *highpass[], int column, int32_t* midpoints, int32_t* multipliers, PIXEL *result[], bool top )
{
    const int filter_coeffs_top[]     = { 5, -11, 4, 4, -1, -1 };
    const int filter_coeffs_bottom[]  = { 1, 1, -4, -4, 11, -5 };
    
    int low_band_index = top ? 0 : 4;
    const int* filter_coeffs = top ? filter_coeffs_top : filter_coeffs_bottom;

    int i;
    int32_t sum_L = 0;
    int32_t sum_H = 0;
    
    // Apply the lowpass vertical filter to the lowpass horizontal results
    result[LL_BAND][column] = QuantizeValue( lowpass[low_band_index + 0][column] + lowpass[low_band_index + 1][column], midpoints[LL_BAND], multipliers[LL_BAND] );

    // Apply the lowpass vertical filter to the highpass horizontal results
    result[LH_BAND][column] = QuantizeValue( highpass[low_band_index + 0][column] + highpass[low_band_index + 1][column], midpoints[LH_BAND], multipliers[LH_BAND] ); 
    
    for (i = 0; i < 6; i++)
    {
        sum_L += filter_coeffs[i] * lowpass[i][column];
        sum_H += filter_coeffs[i] * highpass[i][column];
    }
    
    sum_L += rounding;
    sum_L = DivideByShift(sum_L, 3);
    result[HL_BAND][column] = QuantizeValue( sum_L, midpoints[HL_BAND], multipliers[HL_BAND] );
    
    sum_H += rounding;
    sum_H = DivideByShift(sum_H, 3);
    result[HH_BAND][column] = QuantizeValue( sum_H, midpoints[HH_BAND], multipliers[HH_BAND] );
}

/*!
	@brief Apply the vertical wavelet filter to the first row

	This routine uses the wavelet formulas for the top row of an image

	The midpoint prequant argument is not the offset that is added to the
	value prior to quantization.  It is a setting indicating which midpoint
	offset to use.

	@todo Change the midpoint_prequant argument to midpoint_setting?
*/
CODEC_ERROR FilterVerticalTopRow(PIXEL **lowpass, PIXEL **highpass, PIXEL **output, int wavelet_width, int wavelet_pitch, int32_t midpoints[], int32_t multipliers[], int input_row )
{
	int column = 0;
    const int wavelet_width_m8 = (wavelet_width / 8) * 8;

	assert(input_row == 0);
    
	for (; column < wavelet_width_m8; column += 8)
	{
        FilterVerticalTopBottom_8x( lowpass, highpass, column, midpoints, multipliers, output, true );

        if (output[LL_BAND][column] < 0)
            output[LL_BAND][column] = 0;
	}

    for (; column < wavelet_width; column++)
    {
        FilterVerticalTopBottom_1x( lowpass, highpass, column, midpoints, multipliers, output, true );

        if (output[LL_BAND][column] < 0)
            output[LL_BAND][column] = 0;
    }

	return CODEC_ERROR_OKAY;
}


CODEC_ERROR FilterVerticalBottomRow(PIXEL **lowpass, PIXEL **highpass, PIXEL **output, int wavelet_width, int wavelet_pitch, int32_t midpoints[], int32_t multipliers[], int input_row )
{
    PIXEL *result[MAX_BAND_COUNT];
    int column = 0;
    const int wavelet_width_m8 = (wavelet_width / 8) * 8;
    
    int band;
    
    //uint16_t **lowpass = (uint16_t **)lowpass_buffer;
    
    int output_row = input_row / 2;
    
    // Compute the address of each output row
    for (band = 0; band < MAX_BAND_COUNT; band++)
    {
        uint8_t *band_row_ptr = (uint8_t *)output[band];
        band_row_ptr += output_row * wavelet_pitch;
        result[band] = (PIXEL *)band_row_ptr;
    }
    
    for (; column < wavelet_width_m8; column += 8)
    {
        FilterVerticalTopBottom_8x( lowpass, highpass, column, midpoints, multipliers, result, false );
        
        if (result[LL_BAND][column] < 0)
            result[LL_BAND][column] = 0;
    }

    for (; column < wavelet_width; column++)
    {
        FilterVerticalTopBottom_1x( lowpass, highpass, column, midpoints, multipliers, result, false );
        
        if (result[LL_BAND][column] < 0)
            result[LL_BAND][column] = 0;
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Apply the vertical wavelet filter to a middle row

	This routine uses the wavelet formulas for the middle rows of an image
*/
CODEC_ERROR FilterVerticalMiddleRow(PIXEL **lowpass, PIXEL **highpass, PIXEL **output, int wavelet_width, int wavelet_pitch, int32_t midpoints[], int32_t multipliers[], int input_row )
{
	PIXEL *result[MAX_BAND_COUNT];

    int column = 0;
	int band;

    const int wavelet_width_m8 = (wavelet_width / 8) * 8;
    
	//uint16_t **lowpass = (uint16_t **)lowpass_buffer;

	int output_row = input_row / 2;

	// Compute the address of each output row
	for (band = 0; band < MAX_BAND_COUNT; band++)
	{
		uint8_t *band_row_ptr = (uint8_t *)output[band];
		band_row_ptr += output_row * wavelet_pitch;
		result[band] = (PIXEL *)band_row_ptr;
	}

        for (; column < wavelet_width_m8; column += 8)
        {
            FilterVerticalMiddle_8x(lowpass, highpass, column, midpoints, multipliers, result);

            for (int k = 0; k < 8; ++k)
            {
                if (result[LL_BAND][column + k] < 0)
                    result[LL_BAND][column + k] = 0;
            }
        }

        for (; column < wavelet_width; column += 1)
        {
            FilterVerticalMiddle_1x(lowpass, highpass, column, midpoints, multipliers, result);

            if (result[LL_BAND][column] < 0)
                result[LL_BAND][column] = 0;
        }

	return CODEC_ERROR_OKAY;
}
