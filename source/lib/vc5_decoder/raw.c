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
#include "logcurve.h"

#if ENABLED(NEON)
#include <arm_neon.h>
#endif

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
    int component_bits = image->component_array_list[0].bits_per_component;
    if (component_bits <= 0)
        component_bits = 12;
    if (component_bits > 16)
        component_bits = 16;
    if (!vc5_logcurve_bypass())
    {
        // Scan ALL components to find the maximum value (not just component 0)
        // This is critical because RG/BG/GD are midpoint-centered at 2^(bits-1)
        COMPONENT_VALUE max_val = 0;
        for (int comp = 0; comp < image->component_count; ++comp)
        {
            COMPONENT_VALUE *component_data = image->component_array_list[comp].data;
            if (component_data != NULL)
            {
                size_t total = (size_t)image->component_array_list[comp].width * image->component_array_list[comp].height;
#if ENABLED(NEON)
                {
                    size_t total_m4 = (total / 4) * 4;
                    int32x4_t vmax4 = vdupq_n_s32(0);
                    size_t idx;
                    for (idx = 0; idx < total_m4; idx += 4)
                    {
                        int32x4_t vals = vld1q_s32(&component_data[idx]);
                        vmax4 = vmaxq_s32(vmax4, vals);
                    }
                    max_val = maximum(max_val, vmaxvq_s32(vmax4));
                    for (; idx < total; ++idx)
                    {
                        if (component_data[idx] > max_val)
                            max_val = component_data[idx];
                    }
                }
#else
                for (size_t idx = 0; idx < total; ++idx)
                {
                    COMPONENT_VALUE value = component_data[idx];
                    if (value > max_val)
                        max_val = value;
                }
#endif
            }
        }
        if (max_val > 0)
        {
            int bits_from_data = 0;
            uint32_t val = (uint32_t)max_val;
            while (val > 0)
            {
                bits_from_data++;
                val >>= 1;
            }
            if (bits_from_data > 0)
            {
                if (bits_from_data > 16) bits_from_data = 16;
                if (bits_from_data < 12) bits_from_data = 12;
                component_bits = minimum(component_bits, bits_from_data);
            }
        }
    }
    const int log_bits = component_bits;

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
    
    const int32_t midpoint = 1 << (log_bits - 1);

    for (row = 0; row < height; row++)
    {
        COMPONENT_VALUE *GS_input_row_ptr = (COMPONENT_VALUE *)((uintptr_t)GS_input_buffer + row * image->component_array_list[0].pitch);
        COMPONENT_VALUE *RG_input_row_ptr = (COMPONENT_VALUE *)((uintptr_t)RG_input_buffer + row * image->component_array_list[1].pitch);
        COMPONENT_VALUE *BG_input_row_ptr = (COMPONENT_VALUE *)((uintptr_t)BG_input_buffer + row * image->component_array_list[2].pitch);
        COMPONENT_VALUE *GD_input_row_ptr = (COMPONENT_VALUE *)((uintptr_t)GD_input_buffer + row * image->component_array_list[3].pitch);
        
        uint8_t *output_row_ptr = (uint8_t *)output_buffer + row * output_pitch;
        
        int column;
        
        output_row1_ptr = (uint16_t *)output_row_ptr;
        output_row2_ptr = (uint16_t *)(output_row_ptr + output_half_pitch);
        
        // Determine output ordering from format (once per row, outside column loop)
        int rggb_order;
        switch (output_format)
        {
            case PIXEL_FORMAT_RAW_RGGB_12:
            case PIXEL_FORMAT_RAW_RGGB_14:
            case PIXEL_FORMAT_RAW_RGGB_16:
                rggb_order = 1;
                break;
            case PIXEL_FORMAT_RAW_GBRG_12:
            case PIXEL_FORMAT_RAW_GBRG_14:
            case PIXEL_FORMAT_RAW_GBRG_16:
                rggb_order = 0;
                break;
            default:
                assert(0);
                rggb_order = 0;
                break;
        }

#if ENABLED(NEON)
        {
            const int width_m4 = (width / 4) * 4;
            const int32x4_t v_midpoint = vdupq_n_s32(midpoint);
            const int32x4_t v_zero = vdupq_n_s32(0);
            const int32x4_t v_max = vdupq_n_s32((1 << log_bits) - 1);
            const int bypass = vc5_logcurve_bypass();
            const int shift = 16 - output_bit_depth;

            // NEON: process 4 columns at a time
            for (column = 0; column < width_m4; column += 4)
            {
                int32x4_t gs = vld1q_s32(&GS_input_row_ptr[column]);
                int32x4_t rg = vld1q_s32(&RG_input_row_ptr[column]);
                int32x4_t bg = vld1q_s32(&BG_input_row_ptr[column]);
                int32x4_t gd = vld1q_s32(&GD_input_row_ptr[column]);

                /* Clamp components to valid range BEFORE color conversion
                   to prevent quantization error from compounding */
                gs = vmaxq_s32(vminq_s32(gs, v_max), v_zero);
                rg = vmaxq_s32(vminq_s32(rg, v_max), v_zero);
                bg = vmaxq_s32(vminq_s32(bg, v_max), v_zero);
                gd = vmaxq_s32(vminq_s32(gd, v_max), v_zero);

                gd = vsubq_s32(gd, v_midpoint);
                rg = vsubq_s32(rg, v_midpoint);
                bg = vsubq_s32(bg, v_midpoint);

                /* Clamp color differences to valid signed range [-midpoint, midpoint-1] */
                {
                    const int32x4_t v_neg_mid = vnegq_s32(v_midpoint);
                    const int32x4_t v_mid_m1 = vsubq_s32(v_midpoint, vdupq_n_s32(1));
                    rg = vmaxq_s32(vminq_s32(rg, v_mid_m1), v_neg_mid);
                    bg = vmaxq_s32(vminq_s32(bg, v_mid_m1), v_neg_mid);
                    gd = vmaxq_s32(vminq_s32(gd, v_mid_m1), v_neg_mid);
                }

                int32x4_t r  = vaddq_s32(vshlq_n_s32(rg, 1), gs);
                int32x4_t b  = vaddq_s32(vshlq_n_s32(bg, 1), gs);
                int32x4_t g1 = vaddq_s32(gs, gd);
                int32x4_t g2 = vsubq_s32(gs, gd);

                r  = vmaxq_s32(vminq_s32(r,  v_max), v_zero);
                g1 = vmaxq_s32(vminq_s32(g1, v_max), v_zero);
                g2 = vmaxq_s32(vminq_s32(g2, v_max), v_zero);
                b  = vmaxq_s32(vminq_s32(b,  v_max), v_zero);

                // Extract to scalar for log curve lookups + store
                int32_t r_a[4], g1_a[4], g2_a[4], b_a[4];
                vst1q_s32(r_a, r);
                vst1q_s32(g1_a, g1);
                vst1q_s32(g2_a, g2);
                vst1q_s32(b_a, b);

                int k;
                for (k = 0; k < 4; k++)
                {
                    int32_t R  = DecodeLogValue(r_a[k], log_bits);
                    int32_t G1 = DecodeLogValue(g1_a[k], log_bits);
                    int32_t G2 = DecodeLogValue(g2_a[k], log_bits);
                    int32_t B  = DecodeLogValue(b_a[k], log_bits);

                    if (!bypass)
                    {
                        R  >>= shift;
                        G1 >>= shift;
                        G2 >>= shift;
                        B  >>= shift;
                    }

                    if (rggb_order)
                    {
                        output_row1_ptr[2 * (column + k) + 0] = (uint16_t)R;
                        output_row1_ptr[2 * (column + k) + 1] = (uint16_t)G1;
                        output_row2_ptr[2 * (column + k) + 0] = (uint16_t)G2;
                        output_row2_ptr[2 * (column + k) + 1] = (uint16_t)B;
                    }
                    else
                    {
                        output_row1_ptr[2 * (column + k) + 0] = (uint16_t)G1;
                        output_row1_ptr[2 * (column + k) + 1] = (uint16_t)B;
                        output_row2_ptr[2 * (column + k) + 0] = (uint16_t)R;
                        output_row2_ptr[2 * (column + k) + 1] = (uint16_t)G2;
                    }
                }
            }
        }
#else
        column = 0;
#endif

        // Scalar cleanup for remaining columns
        for (; column < width; column++)
        {
            int32_t GS, RG, BG, GD;
            int32_t R, G1, G2, B;

            GS = GS_input_row_ptr[column];
            RG = RG_input_row_ptr[column];
            BG = BG_input_row_ptr[column];
            GD = GD_input_row_ptr[column];

            /* Clamp components to valid range BEFORE color conversion.
               The inverse wavelet + dequantization + uncompanding can produce
               values outside the valid [0, 2^bits-1] range. Without clamping,
               the R=(RG<<1)+GS reconstruction doubles the RG error, causing
               catastrophic overflow at high quality (Q6+) with 14-bit data. */
            {
                const int32_t comp_max = (1 << log_bits) - 1;
                if (GS < 0) GS = 0; else if (GS > comp_max) GS = comp_max;
                if (RG < 0) RG = 0; else if (RG > comp_max) RG = comp_max;
                if (BG < 0) BG = 0; else if (BG > comp_max) BG = comp_max;
                if (GD < 0) GD = 0; else if (GD > comp_max) GD = comp_max;
            }

            GD -= midpoint;
            RG -= midpoint;
            BG -= midpoint;

            /* Clamp color differences to their valid signed range.
               RG = (R-GS)/2, so valid range is [-midpoint, midpoint-1].
               Without this clamp, dequantization errors are doubled by <<1. */
            {
                const int32_t diff_max = midpoint - 1;
                if (RG < -midpoint) RG = -midpoint; else if (RG > diff_max) RG = diff_max;
                if (BG < -midpoint) BG = -midpoint; else if (BG > diff_max) BG = diff_max;
                if (GD < -midpoint) GD = -midpoint; else if (GD > diff_max) GD = diff_max;
            }

            R = (RG << 1) + GS;
            B = (BG << 1) + GS;
            G1 = GS + GD;
            G2 = GS - GD;

            R  = clamp_uint(R,  log_bits);
            G1 = clamp_uint(G1, log_bits);
            G2 = clamp_uint(G2, log_bits);
            B  = clamp_uint(B,  log_bits);

            R  = DecodeLogValue(R, log_bits);
            B  = DecodeLogValue(B, log_bits);
            G1 = DecodeLogValue(G1, log_bits);
            G2 = DecodeLogValue(G2, log_bits);

            if (!vc5_logcurve_bypass())
            {
                R   >>= (16 - output_bit_depth);
                B   >>= (16 - output_bit_depth);
                G1  >>= (16 - output_bit_depth);
                G2  >>= (16 - output_bit_depth);
            }

            if (rggb_order)
            {
                output_row1_ptr[2 * column + 0] = (uint16_t)R;
                output_row1_ptr[2 * column + 1] = (uint16_t)G1;
                output_row2_ptr[2 * column + 0] = (uint16_t)G2;
                output_row2_ptr[2 * column + 1] = (uint16_t)B;
            }
            else
            {
                output_row1_ptr[2 * column + 0] = (uint16_t)G1;
                output_row1_ptr[2 * column + 1] = (uint16_t)B;
                output_row2_ptr[2 * column + 0] = (uint16_t)R;
                output_row2_ptr[2 * column + 1] = (uint16_t)G2;
            }
        }
    }
    
    return CODEC_ERROR_OKAY;
}
