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

#if ENABLED(NEON)
#include <arm_neon.h>
#endif

/** ------------------- **/
/** 14 BIT INPUT FORMAT **/
/** ------------------- **/

static void UnpackPixel_14(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    uint16_t R1, G1, G2, B1;
    uint16_t GS, GD, RG, BG;
    
    uint16_t *GS_output_row_ptr = (uint16_t *)output_buffer[0];
    uint16_t *GD_output_row_ptr = (uint16_t *)output_buffer[3];
    uint16_t *RG_output_row_ptr = (uint16_t *)output_buffer[1];
    uint16_t *BG_output_row_ptr = (uint16_t *)output_buffer[2];
    
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
    R1 = EncoderLogCurve[ R1 >> 2 ];
    G1 = EncoderLogCurve[ G1 >> 2 ];
    G2 = EncoderLogCurve[ G2 >> 2 ];
    B1 = EncoderLogCurve[ B1 >> 2 ];
    
    // Difference the green components and subtract green from the red and blue components
    GS = (G1 + G2) >> 1;
    GD = (G1 - G2 + 2 * midpoint) >> 1;
    RG = (R1 - GS + 2 * midpoint) >> 1;
    BG = (B1 - GS + 2 * midpoint) >> 1;
    
    GS_output_row_ptr[column] = clamp_uint(GS, internal_precision);
    GD_output_row_ptr[column] = clamp_uint(GD, internal_precision);
    RG_output_row_ptr[column] = clamp_uint(RG, internal_precision);
    BG_output_row_ptr[column] = clamp_uint(BG, internal_precision);
}

#if ENABLED(NEON)

#define UnpackPixel_14_8x UnpackPixel_14_8x_NEON_
static void UnpackPixel_14_8x_NEON_(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
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
            input_row1_12b[i] = EncoderLogCurve[ input_row1_ptr[2 * column + i] >> 2 ];
            input_row2_12b[i] = EncoderLogCurve[ input_row2_ptr[2 * column + i] >> 2 ];
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
    vst1q_s16( output_buffer[0] + column, GS );
    
    {
        const int16x8_t __midpoint_x2   = vdupq_n_s16(midpoint * 2);

        GD = vsubq_s16(G1, G2);
        GD = vhaddq_s16(GD, __midpoint_x2);
        vst1q_s16( output_buffer[3] + column, GD );
        
        GS = vsubq_s16( __midpoint_x2, GS );
    }

    RG = vhaddq_s16(R1, GS);
    vst1q_s16( output_buffer[1] + column, RG );
    
    BG = vhaddq_s16(B1, GS);
    vst1q_s16( output_buffer[2] + column, BG );
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
/** 12 BIT INPUT FORMAT **/
/** ------------------- **/

static void UnpackPixel_12(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr, int column, PIXEL *output_buffer[], bool rggb )
{
    uint16_t R1, G1, G2, B1;
    uint16_t GS, GD, RG, BG;
    
    uint16_t *GS_output_row_ptr = (uint16_t *)output_buffer[0];
    uint16_t *GD_output_row_ptr = (uint16_t *)output_buffer[3];
    uint16_t *RG_output_row_ptr = (uint16_t *)output_buffer[1];
    uint16_t *BG_output_row_ptr = (uint16_t *)output_buffer[2];
    
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
    R1 = EncoderLogCurve[ R1 ];
    G1 = EncoderLogCurve[ G1 ];
    G2 = EncoderLogCurve[ G2 ];
    B1 = EncoderLogCurve[ B1 ];
    
    // Difference the green components and subtract green from the red and blue components
    GS = (G1 + G2) >> 1;
    GD = (G1 - G2 + 2 * midpoint) >> 1;
    RG = (R1 - GS + 2 * midpoint) >> 1;
    BG = (B1 - GS + 2 * midpoint) >> 1;
    
    GS_output_row_ptr[column] = clamp_uint(GS, internal_precision);
    GD_output_row_ptr[column] = clamp_uint(GD, internal_precision);
    RG_output_row_ptr[column] = clamp_uint(RG, internal_precision);
    BG_output_row_ptr[column] = clamp_uint(BG, internal_precision);
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
            input_row1_12b[i] = EncoderLogCurve[ input_row1_ptr[2 * column + i] ];
            input_row2_12b[i] = EncoderLogCurve[ input_row2_ptr[2 * column + i] ];
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
    vst1q_s16( output_buffer[0] + column, GS );
    
    {
        const int16x8_t __midpoint_x2   = vdupq_n_s16(midpoint * 2);
        
        GD = vsubq_s16(G1, G2);
        GD = vhaddq_s16(GD, __midpoint_x2);
        vst1q_s16( output_buffer[3] + column, GD );
        
        GS = vsubq_s16( __midpoint_x2, GS );
    }
    
    RG = vhaddq_s16(R1, GS);
    vst1q_s16( output_buffer[1] + column, RG );
    
    BG = vhaddq_s16(B1, GS);
    vst1q_s16( output_buffer[2] + column, BG );
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
    G1 = EncoderLogCurve[ G1 ];
    B1 = EncoderLogCurve[ B1 ];
    R1 = EncoderLogCurve[ R1 ];
    G2 = EncoderLogCurve[ G2 ];
    
    // difference the green components and subtract green from the red and blue components
    GS = (G1 + G2) >> 1;
    GD = (G1 - G2 + 2 * midpoint) >> 1;
    RG = (R1 - GS + 2 * midpoint) >> 1;
    BG = (B1 - GS + 2 * midpoint) >> 1;

    { // write output
        uint16_t *GS_output_row_ptr = (uint16_t *)output_buffer[0];
        uint16_t *GD_output_row_ptr = (uint16_t *)output_buffer[3];
        uint16_t *RG_output_row_ptr = (uint16_t *)output_buffer[1];
        uint16_t *BG_output_row_ptr = (uint16_t *)output_buffer[2];
        
        GS_output_row_ptr[column] = clamp_uint(GS, internal_precision);
        GD_output_row_ptr[column] = clamp_uint(GD, internal_precision);
        RG_output_row_ptr[column] = clamp_uint(RG, internal_precision);
        BG_output_row_ptr[column] = clamp_uint(BG, internal_precision);
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
        g1[i] = EncoderLogCurve[g1[i]];
        b1[i] = EncoderLogCurve[b1[i]];
        r1[i] = EncoderLogCurve[r1[i]];
        g2[i] = EncoderLogCurve[g2[i]];
    }
    
    int16x8_t R1, G1, G2, B1;
    
    G1 = vreinterpretq_s16_u16( g1 );
    B1 = vreinterpretq_s16_u16( b1 );
    R1 = vreinterpretq_s16_u16( r1 );
    G2 = vreinterpretq_s16_u16( g2 );
    
    int16x8_t GS, GD, RG, BG;
    
    GS = vhaddq_s16(G1, G2);
    vst1q_s16( output_buffer[0] + column, GS );
    
    {
        const int16x8_t __midpoint_x2   = vdupq_n_s16(midpoint * 2);
        
        GD = vsubq_s16(G1, G2);
        GD = vhaddq_s16(GD, __midpoint_x2);
        vst1q_s16( output_buffer[3] + column, GD );
        GS = vsubq_s16( __midpoint_x2, GS );
    }
    
    RG = vhaddq_s16(R1, GS);
    vst1q_s16( output_buffer[1] + column, RG );
    
    BG = vhaddq_s16(B1, GS);
    vst1q_s16( output_buffer[2] + column, BG );
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

