/*! @file raw.c
 *
 *  @brief Definition of routines for packing a row of pixels into a RAW image.
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

/*!
	@brief Pack the component arrays into an output image
	
	The inverse component transform for Bayer images (VC-5 Part 3)
	is applied to the component arrays before combining the values
	into a packed image.
 */
CODEC_ERROR PackComponentsToRAW(const UNPACKED_IMAGE *image,
                                PIXEL *output_buffer, size_t output_pitch,
                                DIMENSION width, DIMENSION height,
                                ENABLED_PARTS enabled_parts, uint16_t output_bit_depth, PIXEL_FORMAT output_format )
{
    // Define pointers to the rows for each input component
    COMPONENT_VALUE *GS_input_buffer;
    COMPONENT_VALUE *RG_input_buffer;
    COMPONENT_VALUE *BG_input_buffer;
    COMPONENT_VALUE *GD_input_buffer;
    
    // Define pointers to the rows for each output component
    uint16_t *output_row1_ptr;
    uint16_t *output_row2_ptr;
    
    //size_t input_quarter_pitch;
    size_t output_half_pitch;
    
    int row;
    
    GS_input_buffer = image->component_array_list[0].data;
    RG_input_buffer = image->component_array_list[1].data;
    BG_input_buffer = image->component_array_list[2].data;
    GD_input_buffer = image->component_array_list[3].data;
    
    // Compute the distance between the half rows in the Bayer grid
    output_half_pitch = output_pitch / 2;
    
    for (row = 0; row < height; row++)
    {
        COMPONENT_VALUE *GS_input_row_ptr = (COMPONENT_VALUE *)((uintptr_t)GS_input_buffer + row * image->component_array_list[0].pitch);
        COMPONENT_VALUE *RG_input_row_ptr = (COMPONENT_VALUE *)((uintptr_t)RG_input_buffer + row * image->component_array_list[1].pitch);
        COMPONENT_VALUE *BG_input_row_ptr = (COMPONENT_VALUE *)((uintptr_t)BG_input_buffer + row * image->component_array_list[2].pitch);
        COMPONENT_VALUE *GD_input_row_ptr = (COMPONENT_VALUE *)((uintptr_t)GD_input_buffer + row * image->component_array_list[3].pitch);
        
        uint8_t *output_row_ptr = (uint8_t *)output_buffer + row * output_pitch;
        
        const int32_t midpoint = 2048;
        
        int column;
        
        output_row1_ptr = (uint16_t *)output_row_ptr;
        output_row2_ptr = (uint16_t *)(output_row_ptr + output_half_pitch);
        
        // Pack the rows of Bayer components into the BYR4 pattern
        for (column = 0; column < width; column++)
        {
            int32_t GS, RG, BG, GD;
            int32_t R, G1, G2, B;
            
            GS = GS_input_row_ptr[column];
            RG = RG_input_row_ptr[column];
            BG = BG_input_row_ptr[column];
            GD = GD_input_row_ptr[column];
            
            // Convert unsigned values to signed values
            GD -= midpoint;
            RG -= midpoint;
            BG -= midpoint;
            
            R = (RG << 1) + GS;
            B = (BG << 1) + GS;
            G1 = GS + GD;
            G2 = GS - GD;
            
            R  = clamp_uint(R,  12);
            G1 = clamp_uint(G1, 12);
            G2 = clamp_uint(G2, 12);
            B  = clamp_uint(B,  12);
            
            // Apply inverse protune log curve
            R  = DecoderLogCurve[R];
            B  = DecoderLogCurve[B];
            G1 = DecoderLogCurve[G1];
            G2 = DecoderLogCurve[G2];

            R   >>= (16 - output_bit_depth);
            B   >>= (16 - output_bit_depth);
            G1  >>= (16 - output_bit_depth);
            G2  >>= (16 - output_bit_depth);
            
            switch (output_format)
            {
                case PIXEL_FORMAT_RAW_RGGB_12:
                case PIXEL_FORMAT_RAW_RGGB_14:
                    output_row1_ptr[2 * column + 0] = (uint16_t)R;
                    output_row1_ptr[2 * column + 1] = (uint16_t)G1;
                    output_row2_ptr[2 * column + 0] = (uint16_t)G2;
                    output_row2_ptr[2 * column + 1] = (uint16_t)B;
                    break;
                    
                case PIXEL_FORMAT_RAW_GBRG_12:
                case PIXEL_FORMAT_RAW_GBRG_14:
                    output_row1_ptr[2 * column + 0] = (uint16_t)G1;
                    output_row1_ptr[2 * column + 1] = (uint16_t)B;
                    output_row2_ptr[2 * column + 0] = (uint16_t)R;
                    output_row2_ptr[2 * column + 1] = (uint16_t)G2;
                    break;
                    
                default:
                    assert(0);
                    break;
            }
        }
    }
    
    return CODEC_ERROR_OKAY;
}
