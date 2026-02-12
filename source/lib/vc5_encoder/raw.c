/*! @file raw.c
 *
 *  @brief Implementation of routines for packing RAW image to a row of pixels.
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

#include "logcurve.h"

static INLINE uint16_t ApplyEncoderCurve(const uint16_t *table, uint16_t sample)
{
    if (vc5_logcurve_bypass())
    {
        return sample;
    }
    return table[sample];
}

#if ENABLED(NEON)
#include <arm_neon.h>

static INLINE void StoreVectorToPixels(int16x8_t vec, PIXEL *dst)
{
    const int32x4_t lo = vmovl_s16(vget_low_s16(vec));
    const int32x4_t hi = vmovl_s16(vget_high_s16(vec));
    vst1q_s32(dst, lo);
    vst1q_s32(dst + 4, hi);
}
#endif

/** ------------------- **/
/** 14 BIT INPUT FORMAT **/
/** ------------------- **/

static void UnpackPixel_14(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    uint16_t R1, G1, G2, B1;
    uint16_t GS, GD, RG, BG;
    
    PIXEL *GS_output_row_ptr = output_buffer[0];
    PIXEL *GD_output_row_ptr = output_buffer[3];
    PIXEL *RG_output_row_ptr = output_buffer[1];
    PIXEL *BG_output_row_ptr = output_buffer[2];
    
    const int internal_precision = 14;
    const int32_t midpoint = (1 << (internal_precision - 1));

    if( rggb )
    {
        R1 = input_row1_ptr[2 * column + 0];
        G1 = input_row1_ptr[2 * column + 1];
        G2 = input_row2_ptr[2 * column + 0];
        B1 = input_row2_ptr[2 * column + 1];
    }
    else
    {
        G1 = input_row1_ptr[2 * column + 0];
        B1 = input_row1_ptr[2 * column + 1];
        R1 = input_row2_ptr[2 * column + 0];
        G2 = input_row2_ptr[2 * column + 1];
    }
    
    // Apply protune log curve
    R1 = ApplyEncoderCurve(EncoderLogCurve14, R1);
    G1 = ApplyEncoderCurve(EncoderLogCurve14, G1);
    G2 = ApplyEncoderCurve(EncoderLogCurve14, G2);
    B1 = ApplyEncoderCurve(EncoderLogCurve14, B1);
    
    // Difference the green components and subtract green from the red and blue components
    GS = (G1 + G2) >> 1;
    GD = (G1 - G2 + 2 * midpoint) >> 1;
    RG = (R1 - GS + 2 * midpoint) >> 1;
    BG = (B1 - GS + 2 * midpoint) >> 1;
    
    GS_output_row_ptr[column] = (PIXEL)clamp_uint(GS, internal_precision);
    GD_output_row_ptr[column] = (PIXEL)clamp_uint(GD, internal_precision);
    RG_output_row_ptr[column] = (PIXEL)clamp_uint(RG, internal_precision);
    BG_output_row_ptr[column] = (PIXEL)clamp_uint(BG, internal_precision);
}

#if ENABLED(NEON)

#define UnpackPixel_14_8x UnpackPixel_14_8x_NEON_
static void UnpackPixel_14_8x_NEON_(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    int i;    
    uint16x8x2_t row_1, row_2;

    const int internal_precision = 14;
    const int32_t midpoint = (1 << (internal_precision - 1));
    
    // Apply protune log curve
    {
        uint16_t input_row1_12b[16];
        uint16_t input_row2_12b[16];

        for (i = 0; i < 16; ++i)
        {
            input_row1_12b[i] = ApplyEncoderCurve(EncoderLogCurve14, input_row1_ptr[2 * column + i]);
            input_row2_12b[i] = ApplyEncoderCurve(EncoderLogCurve14, input_row2_ptr[2 * column + i]);
        }

        row_1 = vld2q_u16( input_row1_12b  );
        row_2 = vld2q_u16( input_row2_12b  );
    }

    int16x8_t R1, G1, G2, B1;

    if( rggb )
    {
        R1 = vreinterpretq_s16_u16( row_1.val[0] );
        G1 = vreinterpretq_s16_u16( row_1.val[1] );
        G2 = vreinterpretq_s16_u16( row_2.val[0] );
        B1 = vreinterpretq_s16_u16( row_2.val[1] );
    }
    else
    {
        G1 = vreinterpretq_s16_u16( row_1.val[0] );
        B1 = vreinterpretq_s16_u16( row_1.val[1] );
        R1 = vreinterpretq_s16_u16( row_2.val[0] );
        G2 = vreinterpretq_s16_u16( row_2.val[1] );
    }
    
    int16x8_t GS, GD, RG, BG;
    
    GS = vhaddq_s16(G1, G2);
    StoreVectorToPixels(GS, output_buffer[0] + column);
    
    {
        const int16x8_t __midpoint_x2   = vdupq_n_s16(midpoint * 2);

        GD = vsubq_s16(G1, G2);
        GD = vhaddq_s16(GD, __midpoint_x2);
        StoreVectorToPixels(GD, output_buffer[3] + column);
        
        GS = vsubq_s16( __midpoint_x2, GS );
    }

    RG = vhaddq_s16(R1, GS);
    StoreVectorToPixels(RG, output_buffer[1] + column);
    
    BG = vhaddq_s16(B1, GS);
    StoreVectorToPixels(BG, output_buffer[2] + column);
}

#else

#define UnpackPixel_14_8x UnpackPixel_14_8x_C_
static void UnpackPixel_14_8x_C_(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    int i;
    for ( i = 0; i < 8; i++)
    {
        UnpackPixel_14(input_row1_ptr, input_row2_ptr, column + i, output_buffer, rggb );
    }
}

#endif

void UnpackImage_14(const PACKED_IMAGE *input, UNPACKED_IMAGE *output, ENABLED_PARTS enabled_parts, bool rggb )
{
    uint8_t *input_buffer = (uint8_t *)input->buffer + input->offset;
    
    const DIMENSION input_width     = input->width / 2;
    const DIMENSION input_width_m8  = (input_width / 8) * 8;
    
    const DIMENSION input_height = input->height / 2;
    
    size_t input_pitch = input->pitch;
    
    PIXEL *output_row_ptr_array[MAX_CHANNEL_COUNT];
    uint32_t output_row_ptr_array_pitch[MAX_CHANNEL_COUNT];
    
    uint16_t *input_row_ptr = (uint16_t*)input_buffer;

    int channel_number;

    int row;

    for (channel_number = 0; channel_number < MAX_CHANNEL_COUNT; channel_number++)
    {
        output_row_ptr_array[channel_number] = (PIXEL *)(output->component_array_list[channel_number].data);
        
        // output->component_array_list[channel_number].pitch is pitch in bytes, so we need to convert it to pitch in PIXELS
        output_row_ptr_array_pitch[channel_number] = (output->component_array_list[channel_number].pitch / sizeof(PIXEL));
    }

    for (row = 0; row < input_height; row++)
    {
        uint16_t* input_row2_ptr = input_row_ptr + (input_pitch / sizeof(uint16_t));

        int column = 0;

        // Unpack the row of Bayer components from the BYR4 pattern elements
        for (; column < input_width_m8; column+= 8)
        {
            UnpackPixel_14_8x(input_row_ptr, input_row2_ptr, column, output_row_ptr_array, rggb );
        }

        // Unpack the row of Bayer components from the BYR4 pattern elements
        for (; column < input_width; column++)
        {
            UnpackPixel_14(input_row_ptr, input_row2_ptr, column, output_row_ptr_array, rggb );
        }

        input_row_ptr += input_pitch;

        for (channel_number = 0; channel_number < MAX_CHANNEL_COUNT; channel_number++)
        {
            output_row_ptr_array[channel_number] += output_row_ptr_array_pitch[channel_number];
        }
    }
}

/** ------------------- **/
/** 16 BIT INPUT FORMAT **/
/** ------------------- **/

static void UnpackPixel_16(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    uint16_t R1, G1, G2, B1;
    uint16_t GS, GD, RG, BG;

    PIXEL *GS_output_row_ptr = output_buffer[0];
    PIXEL *GD_output_row_ptr = output_buffer[3];
    PIXEL *RG_output_row_ptr = output_buffer[1];
    PIXEL *BG_output_row_ptr = output_buffer[2];

    const int internal_precision = 16;
    const int32_t midpoint = (1 << (internal_precision - 1));

    if( rggb )
    {
        R1 = input_row1_ptr[2 * column + 0];
        G1 = input_row1_ptr[2 * column + 1];
        G2 = input_row2_ptr[2 * column + 0];
        B1 = input_row2_ptr[2 * column + 1];
    }
    else
    {
        G1 = input_row1_ptr[2 * column + 0];
        B1 = input_row1_ptr[2 * column + 1];
        R1 = input_row2_ptr[2 * column + 0];
        G2 = input_row2_ptr[2 * column + 1];
    }

    // Apply protune log curve using full 16-bit table
    R1 = ApplyEncoderCurve(EncoderLogCurve16, R1);
    G1 = ApplyEncoderCurve(EncoderLogCurve16, G1);
    G2 = ApplyEncoderCurve(EncoderLogCurve16, G2);
    B1 = ApplyEncoderCurve(EncoderLogCurve16, B1);

    GS = (G1 + G2) >> 1;
    GD = (G1 - G2 + 2 * midpoint) >> 1;
    RG = (R1 - GS + 2 * midpoint) >> 1;
    BG = (B1 - GS + 2 * midpoint) >> 1;

    GS_output_row_ptr[column] = (PIXEL)clamp_uint(GS, internal_precision);
    GD_output_row_ptr[column] = (PIXEL)clamp_uint(GD, internal_precision);
    RG_output_row_ptr[column] = (PIXEL)clamp_uint(RG, internal_precision);
    BG_output_row_ptr[column] = (PIXEL)clamp_uint(BG, internal_precision);
}

static void UnpackPixel_16_8x_C_(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    int i;
    for ( i = 0; i < 8; i++)
    {
        UnpackPixel_16(input_row1_ptr, input_row2_ptr, column + i, output_buffer, rggb );
    }
}

#if ENABLED(NEON)

#define UnpackPixel_16_8x UnpackPixel_16_8x_NEON_

static INLINE void UnpackPixel_16_4_helper_NEON_(
    uint16x4_t r1_half, uint16x4_t g1_half, uint16x4_t g2_half, uint16x4_t b1_half,
    int32x4_t v_midpoint_x2,
    PIXEL *gs_out, PIXEL *rg_out, PIXEL *bg_out, PIXEL *gd_out)
{
    int32x4_t R1 = vreinterpretq_s32_u32(vmovl_u16(r1_half));
    int32x4_t G1 = vreinterpretq_s32_u32(vmovl_u16(g1_half));
    int32x4_t G2 = vreinterpretq_s32_u32(vmovl_u16(g2_half));
    int32x4_t B1 = vreinterpretq_s32_u32(vmovl_u16(b1_half));

    int32x4_t GS = vshrq_n_s32(vaddq_s32(G1, G2), 1);
    vst1q_s32(gs_out, GS);

    int32x4_t GD = vshrq_n_s32(vaddq_s32(vsubq_s32(G1, G2), v_midpoint_x2), 1);
    vst1q_s32(gd_out, GD);

    int32x4_t RG = vshrq_n_s32(vaddq_s32(vsubq_s32(R1, GS), v_midpoint_x2), 1);
    vst1q_s32(rg_out, RG);

    int32x4_t BG = vshrq_n_s32(vaddq_s32(vsubq_s32(B1, GS), v_midpoint_x2), 1);
    vst1q_s32(bg_out, BG);
}

static void UnpackPixel_16_8x_NEON_(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb)
{
    int i;
    uint16x8x2_t row_1, row_2;

    const int internal_precision = 16;
    const int32_t midpoint = (1 << (internal_precision - 1));
    const int32x4_t v_midpoint_x2 = vdupq_n_s32(midpoint * 2);

    // Apply log curve (scalar -- 65536-entry table too large for vector LUT)
    {
        uint16_t input_row1_curved[16];
        uint16_t input_row2_curved[16];

        for (i = 0; i < 16; ++i)
        {
            input_row1_curved[i] = ApplyEncoderCurve(EncoderLogCurve16, input_row1_ptr[2 * column + i]);
            input_row2_curved[i] = ApplyEncoderCurve(EncoderLogCurve16, input_row2_ptr[2 * column + i]);
        }

        row_1 = vld2q_u16(input_row1_curved);
        row_2 = vld2q_u16(input_row2_curved);
    }

    uint16x8_t r1_u16, g1_u16, g2_u16, b1_u16;

    if (rggb)
    {
        r1_u16 = row_1.val[0];
        g1_u16 = row_1.val[1];
        g2_u16 = row_2.val[0];
        b1_u16 = row_2.val[1];
    }
    else
    {
        g1_u16 = row_1.val[0];
        b1_u16 = row_1.val[1];
        r1_u16 = row_2.val[0];
        g2_u16 = row_2.val[1];
    }

    // Process low 4 pixels (16-bit values overflow int16, must use 32-bit arithmetic)
    UnpackPixel_16_4_helper_NEON_(
        vget_low_u16(r1_u16), vget_low_u16(g1_u16),
        vget_low_u16(g2_u16), vget_low_u16(b1_u16),
        v_midpoint_x2,
        output_buffer[0] + column, output_buffer[1] + column,
        output_buffer[2] + column, output_buffer[3] + column);

    // Process high 4 pixels
    UnpackPixel_16_4_helper_NEON_(
        vget_high_u16(r1_u16), vget_high_u16(g1_u16),
        vget_high_u16(g2_u16), vget_high_u16(b1_u16),
        v_midpoint_x2,
        output_buffer[0] + column + 4, output_buffer[1] + column + 4,
        output_buffer[2] + column + 4, output_buffer[3] + column + 4);
}

#else

#define UnpackPixel_16_8x UnpackPixel_16_8x_C_

#endif

void UnpackImage_16(const PACKED_IMAGE *input, UNPACKED_IMAGE *output, ENABLED_PARTS enabled_parts, bool rggb )
{
    uint8_t *input_buffer = (uint8_t *)input->buffer + input->offset;

    const DIMENSION input_width     = input->width / 2;
    const DIMENSION input_width_m8  = (input_width / 8) * 8;

    const DIMENSION input_height = input->height / 2;

    size_t input_pitch = input->pitch;

    PIXEL *output_row_ptr_array[MAX_CHANNEL_COUNT];
    uint32_t output_row_ptr_array_pitch[MAX_CHANNEL_COUNT];

    uint16_t *input_row_ptr = (uint16_t*)input_buffer;

    int channel_number;

    int row;

    for (channel_number = 0; channel_number < MAX_CHANNEL_COUNT; channel_number++)
    {
        output_row_ptr_array[channel_number] = (PIXEL *)(output->component_array_list[channel_number].data);

        output_row_ptr_array_pitch[channel_number] = (output->component_array_list[channel_number].pitch / sizeof(PIXEL));
    }

    for (row = 0; row < input_height; row++)
    {
        uint16_t* input_row2_ptr = input_row_ptr + (input_pitch / sizeof(uint16_t));

        int column = 0;

        for (; column < input_width_m8; column+= 8)
        {
            UnpackPixel_16_8x(input_row_ptr, input_row2_ptr, column, output_row_ptr_array, rggb );
        }

        for (; column < input_width; column++)
        {
            UnpackPixel_16(input_row_ptr, input_row2_ptr, column, output_row_ptr_array, rggb );
        }

        input_row_ptr += input_pitch;

        for (channel_number = 0; channel_number < MAX_CHANNEL_COUNT; channel_number++)
        {
            output_row_ptr_array[channel_number] += output_row_ptr_array_pitch[channel_number];
        }
    }
}

/** ------------------- **/
/** 12 BIT INPUT FORMAT **/
/** ------------------- **/

static void UnpackPixel_12(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    uint16_t R1, G1, G2, B1;
    uint16_t GS, GD, RG, BG;
    
    PIXEL *GS_output_row_ptr = output_buffer[0];
    PIXEL *GD_output_row_ptr = output_buffer[3];
    PIXEL *RG_output_row_ptr = output_buffer[1];
    PIXEL *BG_output_row_ptr = output_buffer[2];
    
    const int internal_precision = 12;
    const int32_t midpoint = (1 << (internal_precision - 1));
    
    if( rggb )
    {
        R1 = input_row1_ptr[2 * column + 0];
        G1 = input_row1_ptr[2 * column + 1];
        G2 = input_row2_ptr[2 * column + 0];
        B1 = input_row2_ptr[2 * column + 1];
    }
    else
    {
        G1 = input_row1_ptr[2 * column + 0];
        B1 = input_row1_ptr[2 * column + 1];
        R1 = input_row2_ptr[2 * column + 0];
        G2 = input_row2_ptr[2 * column + 1];
    }
    
    // Apply protune log curve
    R1 = ApplyEncoderCurve(EncoderLogCurve12, R1);
    G1 = ApplyEncoderCurve(EncoderLogCurve12, G1);
    G2 = ApplyEncoderCurve(EncoderLogCurve12, G2);
    B1 = ApplyEncoderCurve(EncoderLogCurve12, B1);
    
    // Difference the green components and subtract green from the red and blue components
    GS = (G1 + G2) >> 1;
    GD = (G1 - G2 + 2 * midpoint) >> 1;
    RG = (R1 - GS + 2 * midpoint) >> 1;
    BG = (B1 - GS + 2 * midpoint) >> 1;
    
    GS_output_row_ptr[column] = (PIXEL)clamp_uint(GS, internal_precision);
    GD_output_row_ptr[column] = (PIXEL)clamp_uint(GD, internal_precision);
    RG_output_row_ptr[column] = (PIXEL)clamp_uint(RG, internal_precision);
    BG_output_row_ptr[column] = (PIXEL)clamp_uint(BG, internal_precision);
}

#if ENABLED(NEON)

#define UnpackPixel_12_8x UnpackPixel_12_8x_NEON_
static void UnpackPixel_12_8x_NEON_(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    int i;
    uint16x8x2_t row_1, row_2;
    
    const int internal_precision = 12;
    const int32_t midpoint = (1 << (internal_precision - 1));
    
    // Apply protune log curve
    {
        uint16_t input_row1_12b[16];
        uint16_t input_row2_12b[16];
        
        for (i = 0; i < 16; ++i)
        {
            input_row1_12b[i] = ApplyEncoderCurve(EncoderLogCurve12, input_row1_ptr[2 * column + i]);
            input_row2_12b[i] = ApplyEncoderCurve(EncoderLogCurve12, input_row2_ptr[2 * column + i]);
        }
        
        row_1 = vld2q_u16( input_row1_12b  );
        row_2 = vld2q_u16( input_row2_12b  );
    }
    
    int16x8_t R1, G1, G2, B1;
    
    if( rggb )
    {
        R1 = vreinterpretq_s16_u16( row_1.val[0] );
        G1 = vreinterpretq_s16_u16( row_1.val[1] );
        G2 = vreinterpretq_s16_u16( row_2.val[0] );
        B1 = vreinterpretq_s16_u16( row_2.val[1] );
    }
    else
    {
        G1 = vreinterpretq_s16_u16( row_1.val[0] );
        B1 = vreinterpretq_s16_u16( row_1.val[1] );
        R1 = vreinterpretq_s16_u16( row_2.val[0] );
        G2 = vreinterpretq_s16_u16( row_2.val[1] );
    }
    
    int16x8_t GS, GD, RG, BG;
    
    GS = vhaddq_s16(G1, G2);
    StoreVectorToPixels(GS, output_buffer[0] + column);
    
    {
        const int16x8_t __midpoint_x2   = vdupq_n_s16(midpoint * 2);
        
        GD = vsubq_s16(G1, G2);
        GD = vhaddq_s16(GD, __midpoint_x2);
        StoreVectorToPixels(GD, output_buffer[3] + column);
        
        GS = vsubq_s16( __midpoint_x2, GS );
    }
    
    RG = vhaddq_s16(R1, GS);
    StoreVectorToPixels(RG, output_buffer[1] + column);
    
    BG = vhaddq_s16(B1, GS);
    StoreVectorToPixels(BG, output_buffer[2] + column);
}

#else

#define UnpackPixel_12_8x UnpackPixel_12_8x_C_
static void UnpackPixel_12_8x_C_(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    int i;
    for ( i = 0; i < 8; i++)
    {
        UnpackPixel_12(input_row1_ptr, input_row2_ptr, column + i, output_buffer, rggb );
    }
}

#endif

void UnpackImage_12(const PACKED_IMAGE *input, UNPACKED_IMAGE *output, ENABLED_PARTS enabled_parts, bool rggb )
{
    uint8_t *input_buffer = (uint8_t *)input->buffer + input->offset;
    
    const DIMENSION input_width     = input->width / 2;
    const DIMENSION input_width_m8  = (input_width / 8) * 8;
    const DIMENSION input_height    = input->height / 2;
    
    size_t input_pitch = input->pitch;
    
    PIXEL *output_row_ptr_array[MAX_CHANNEL_COUNT];
    uint32_t output_row_ptr_array_pitch[MAX_CHANNEL_COUNT];
    
    uint16_t *input_row_ptr = (uint16_t*)input_buffer;

    int channel_number;

    int row;
    
    for (channel_number = 0; channel_number < MAX_CHANNEL_COUNT; channel_number++)
    {
        output_row_ptr_array[channel_number] = (PIXEL *)(output->component_array_list[channel_number].data);
        
        // output->component_array_list[channel_number].pitch is pitch in bytes, so we need to convert it to pitch in PIXELS
        output_row_ptr_array_pitch[channel_number] = (output->component_array_list[channel_number].pitch / sizeof(PIXEL));
    }
    
    for (row = 0; row < input_height; row++)
    {
        uint16_t* input_row2_ptr = input_row_ptr + (input_pitch / sizeof(uint16_t));
        
        int column = 0;
        
        // Unpack the row of Bayer components from the BYR4 pattern elements
        for (; column < input_width_m8; column+= 8)
        {
            UnpackPixel_12_8x(input_row_ptr, input_row2_ptr, column, output_row_ptr_array, rggb );
        }
        
        // Unpack the row of Bayer components from the BYR4 pattern elements
        for (; column < input_width; column++)
        {
            UnpackPixel_12(input_row_ptr, input_row2_ptr, column, output_row_ptr_array, rggb );
        }
        
        input_row_ptr += input_pitch;
        
        for (channel_number = 0; channel_number < MAX_CHANNEL_COUNT; channel_number++)
        {
            output_row_ptr_array[channel_number] += output_row_ptr_array_pitch[channel_number];
        }
    }
}

/** -------------------------- **/
/** 12 bit PACKED INPUT FORMAT **/
/** -------------------------- **/

static void UnpackPixel_12P(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    uint16_t R1, G1, G2, B1;
    uint16_t GS, GD, RG, BG;
    
    const int internal_precision = 12;
    const int32_t midpoint = (1 << (internal_precision - 1));
    
    const unsigned int byte_offset = (column * 3);
    
    { // read first row data
        uint8_t* row1_ptr = (uint8_t*)input_row1_ptr;
        
        unsigned char byte_0 = row1_ptr[byte_offset + 0];
        unsigned char byte_1 = row1_ptr[byte_offset + 1];
        unsigned char byte_2 = row1_ptr[byte_offset + 2];
        
        if( rggb )
        {
            R1 = (byte_0)      + ((byte_1 & 0x0f) << 8);
            G1 = (byte_2 << 4) + ((byte_1 & 0xf0) >> 4);
        }
        else
        {
            G1 = (byte_0)      + ((byte_1 & 0x0f) << 8);
            B1 = (byte_2 << 4) + ((byte_1 & 0xf0) >> 4);
        }
    }
    
    { // read second row data
        uint8_t* row2_ptr = (uint8_t*)input_row2_ptr;
        
        unsigned char byte_0 = row2_ptr[byte_offset + 0];
        unsigned char byte_1 = row2_ptr[byte_offset + 1];
        unsigned char byte_2 = row2_ptr[byte_offset + 2];
        
        if( rggb )
        {
            G2 = (byte_0)      + ((byte_1 & 0x0f) << 8);
            B1 = (byte_2 << 4) + ((byte_1 & 0xf0) >> 4);
        }
        else
        {
            R1 = (byte_0)      + ((byte_1 & 0x0f) << 8);
            G2 = (byte_2 << 4) + ((byte_1 & 0xf0) >> 4);
        }
    }
    
    // Apply protune log curve
    G1 = ApplyEncoderCurve(EncoderLogCurve12, G1);
    B1 = ApplyEncoderCurve(EncoderLogCurve12, B1);
    R1 = ApplyEncoderCurve(EncoderLogCurve12, R1);
    G2 = ApplyEncoderCurve(EncoderLogCurve12, G2);
    
    // difference the green components and subtract green from the red and blue components
    GS = (G1 + G2) >> 1;
    GD = (G1 - G2 + 2 * midpoint) >> 1;
    RG = (R1 - GS + 2 * midpoint) >> 1;
    BG = (B1 - GS + 2 * midpoint) >> 1;

    { // write output
        PIXEL *GS_output_row_ptr = output_buffer[0];
        PIXEL *GD_output_row_ptr = output_buffer[3];
        PIXEL *RG_output_row_ptr = output_buffer[1];
        PIXEL *BG_output_row_ptr = output_buffer[2];
        
        GS_output_row_ptr[column] = (PIXEL)clamp_uint(GS, internal_precision);
        GD_output_row_ptr[column] = (PIXEL)clamp_uint(GD, internal_precision);
        RG_output_row_ptr[column] = (PIXEL)clamp_uint(RG, internal_precision);
        BG_output_row_ptr[column] = (PIXEL)clamp_uint(BG, internal_precision);
    }
}

#if ENABLED(NEON)

#define UnpackPixel_12P_8x UnpackPixel_12P_8x_NEON_
static void UnpackPixel_12P_8x_NEON_(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    int i;
    uint16x8_t g1, b1, r1, g2;
    
    const int internal_precision = 12;
    const int32_t midpoint = (1 << (internal_precision - 1));
    const unsigned int byte_offset = (column * 3)/2;
    
    // Apply protune log curve
    {
        uint8_t* row1_ptr = (uint8_t*) &input_row1_ptr[byte_offset];    //Taken care: input_row1_ptr is 16-bit pointer. So halving it
        uint8x8x3_t __byte012         = vld3_u8(row1_ptr);              //Make sure you move only 24 bytes at a time
        uint8x8_t   __byte0           = __byte012.val[0];
        uint8x8_t   __byte1           = __byte012.val[1];
        uint8x8_t   __byte2           = __byte012.val[2];

        if( rggb )
        {
            r1 = vaddw_u8(vshll_n_u8(vshl_n_u8(__byte1, 4), 4), __byte0);
            g1 = vaddw_u8(vshll_n_u8(__byte2, 4), vshr_n_u8(__byte1, 4));
        }
        else
        {
            g1 = vaddw_u8(vshll_n_u8(vshl_n_u8(__byte1, 4), 4), __byte0);
            b1 = vaddw_u8(vshll_n_u8(__byte2, 4), vshr_n_u8(__byte1, 4));
        }
    }
    {
        uint8_t* row2_ptr = (uint8_t*) &input_row2_ptr[byte_offset];
        uint8x8x3_t __byte012         = vld3_u8(row2_ptr);
        uint8x8_t   __byte0           = __byte012.val[0];
        uint8x8_t   __byte1           = __byte012.val[1];
        uint8x8_t   __byte2           = __byte012.val[2];

        if( rggb )
        {
            g2 = vaddw_u8(vshll_n_u8(vshl_n_u8(__byte1, 4), 4), __byte0);
            b1 = vaddw_u8(vshll_n_u8(__byte2, 4), vshr_n_u8(__byte1, 4));
        }
        else
        {
            r1 = vaddw_u8(vshll_n_u8(vshl_n_u8(__byte1, 4), 4), __byte0);
            g2 = vaddw_u8(vshll_n_u8(__byte2, 4), vshr_n_u8(__byte1, 4));
        }
    }
    
    for(i = 0; i < 8; i++)
    {
        g1[i] = ApplyEncoderCurve(EncoderLogCurve12, g1[i]);
        b1[i] = ApplyEncoderCurve(EncoderLogCurve12, b1[i]);
        r1[i] = ApplyEncoderCurve(EncoderLogCurve12, r1[i]);
        g2[i] = ApplyEncoderCurve(EncoderLogCurve12, g2[i]);
    }
    
    int16x8_t R1, G1, G2, B1;
    
    G1 = vreinterpretq_s16_u16( g1 );
    B1 = vreinterpretq_s16_u16( b1 );
    R1 = vreinterpretq_s16_u16( r1 );
    G2 = vreinterpretq_s16_u16( g2 );
    
    int16x8_t GS, GD, RG, BG;
    
    GS = vhaddq_s16(G1, G2);
    StoreVectorToPixels(GS, output_buffer[0] + column);
    
    {
        const int16x8_t __midpoint_x2   = vdupq_n_s16(midpoint * 2);
        
        GD = vsubq_s16(G1, G2);
        GD = vhaddq_s16(GD, __midpoint_x2);
        StoreVectorToPixels(GD, output_buffer[3] + column);
        GS = vsubq_s16( __midpoint_x2, GS );
    }
    
    RG = vhaddq_s16(R1, GS);
    StoreVectorToPixels(RG, output_buffer[1] + column);
    
    BG = vhaddq_s16(B1, GS);
    StoreVectorToPixels(BG, output_buffer[2] + column);
}

#else

#define UnpackPixel_12P_8x UnpackPixel_12P_8x_C_
static void UnpackPixel_12P_8x_C_(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    int i;
    for ( i = 0; i < 8; i++)
    {
        UnpackPixel_12P(input_row1_ptr, input_row2_ptr, column + i, output_buffer, rggb );
    }
}

#endif

void UnpackImage_12P(const PACKED_IMAGE *input, UNPACKED_IMAGE *output, ENABLED_PARTS enabled_parts, bool rggb )
{
    uint8_t *input_buffer = (uint8_t *)input->buffer + input->offset;
    
    const DIMENSION input_width     = input->width / 2;
    const DIMENSION input_width_m8  = (input_width / 8) * 8;
    const DIMENSION input_height    = input->height / 2;
    
    size_t input_pitch = input->pitch;
    
    PIXEL *output_row_ptr_array[MAX_CHANNEL_COUNT];
    uint32_t output_row_ptr_array_pitch[MAX_CHANNEL_COUNT];
    
    uint16_t *input_row_ptr = (uint16_t*)input_buffer;
    
    int channel_number;
 
    int row;

    for (channel_number = 0; channel_number < MAX_CHANNEL_COUNT; channel_number++)
    {
        output_row_ptr_array[channel_number] = (PIXEL *)(output->component_array_list[channel_number].data);
        
        output_row_ptr_array_pitch[channel_number] = (output->component_array_list[channel_number].pitch / sizeof(PIXEL));
    }
    
    for (row = 0; row < input_height; row++)
    {
        uint16_t* input_row2_ptr = input_row_ptr + (input_pitch / sizeof(uint16_t));
        
        int column = 0;
        
        // Unpack the row of Bayer components from the BYR4 pattern elements
        for (; column < input_width_m8; column+= 8)
        {
            UnpackPixel_12P_8x(input_row_ptr, input_row2_ptr, column, output_row_ptr_array, rggb );
        }
        
        // Unpack the row of Bayer components from the BYR4 pattern elements
        for (; column < input_width; column++)
        {
            UnpackPixel_12P(input_row_ptr, input_row2_ptr, column, output_row_ptr_array, rggb );
        }
        
        input_row_ptr += input_pitch;
        
        for (channel_number = 0; channel_number < MAX_CHANNEL_COUNT; channel_number++)
        {
            output_row_ptr_array[channel_number] += output_row_ptr_array_pitch[channel_number];
        }
    }
}
