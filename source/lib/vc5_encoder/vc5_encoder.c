/*! @file vc5_encoder.c
 *
 *  @brief Implementation of the top level vc5 encoder data structures and functions.
 *
 *  @version 1.0.0
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

void vc5_encoder_parameters_set_default(vc5_encoder_parameters* encoding_parameters)
{
    encoding_parameters->enabled_parts      = VC5_ENABLED_PARTS;
    encoding_parameters->input_width        = 4000;
    encoding_parameters->input_height       = 3000;
    encoding_parameters->input_pitch        = 4000;
    
    encoding_parameters->pixel_format       = VC5_ENCODER_PIXEL_FORMAT_DEFAULT;
    encoding_parameters->quality_setting    = VC5_ENCODER_QUALITY_SETTING_DEFAULT;

    encoding_parameters->mem_alloc = malloc;
    encoding_parameters->mem_free  = free;

    encoding_parameters->denoise_enabled  = false;
    encoding_parameters->denoise_strength = 1.0;
    encoding_parameters->noise_scale      = 0.0;
    encoding_parameters->noise_offset     = 0.0;
    encoding_parameters->variance_stabilize = false;
    encoding_parameters->ans_enabled      = false;

    /* Output fields — must be zeroed so callers that test noise_seed != 0
       (see write_dng in gpr_sdk) don't read uninitialized memory when
       denoise_enabled is false. Without this, Linux glibc malloc (which
       does not zero pages by default) can hand back stack-residue
       garbage where noise_seed is nonzero and noise_sigma_out holds huge
       doubles like 1.73e+185. The downstream dng_xmp::Set_real64 then
       formats that into a 64-byte stack buffer with sprintf("%0.6f", x)
       and blows the stack canary on Linux (macOS happens to land in
       zero-initialized pages and skips the if-noise_seed-nonzero block,
       which is why this only reproduces on Linux). */
    encoding_parameters->noise_seed = 0;
    encoding_parameters->noise_sigma_out[0] = 0.0;
    encoding_parameters->noise_sigma_out[1] = 0.0;
    encoding_parameters->noise_sigma_out[2] = 0.0;
    encoding_parameters->noise_sigma_out[3] = 0.0;
}

CODEC_ERROR vc5_encoder_process(vc5_encoder_parameters*         encoding_parameters,    /* vc5 encoding parameters */
                                const gpr_buffer*               raw_buffer,             /* raw input buffer. */
                                      gpr_buffer*               vc5_buffer,
                                      gpr_rgb_buffer*           rgb_buffer)             /* rgb output buffer. */
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    IMAGE image;
    memset(&image, 0, sizeof(IMAGE));
    ENCODER_PARAMETERS parameters;
    
    STREAM bitstream_file;
    
    // Allocate a conservative buffer for VC5 bitstream: 1.5x raw size + 1MB
    size_t base_size = image.size;
    if (base_size == 0)
    {
        base_size = (size_t)encoding_parameters->input_width * encoding_parameters->input_height * sizeof(uint16_t);
    }
const size_t max_vc5_buffer_size = base_size + (base_size >> 1) + (1 << 20);

    // Initialize the data structure for passing parameters to the encoder
    InitEncoderParameters(&parameters);
    
    {
        QUANT quant_table[VC5_ENCODER_QUALITY_SETTING_COUNT][sizeof(parameters.quant_table) / sizeof(parameters.quant_table[0])] = {
            {1, 24, 24, 12, 64, 64, 48, 512, 512, 768}, // 0  CineForm Low
            {1, 24, 24, 12, 48, 48, 32, 256, 256, 384}, // 1  CineForm Medium
            {1, 24, 24, 12, 32, 32, 24, 128, 128, 192}, // 2  CineForm High
            {1, 24, 24, 12, 24, 24, 12,  96,  96, 144}, // 3  CineForm Filmscan-1
            {1, 24, 24, 12, 24, 24, 12,  64,  64,  96}, // 4  CineForm Filmscan-X
            {1, 24, 24, 12, 24, 24, 12,  32,  32,  48}, // 5  CineForm Filmscan-2
            {1, 12, 12,  6, 12, 12,  6,  16,  16,  24}, // 6  CineForm Filmscan-3 (Edit-Safe)
            {1,  6,  6,  4, 12, 12,  6,  16,  16,  24}, // 7  CineForm Filmscan-4 (Near-Lossless)
            {1,  4,  4,  2, 10, 10,  6,  16,  16,  24}, // 8  CineForm Filmscan-5 (Virtually Lossless)
            {1,  4,  4,  2, 10, 10,  6,  16,  16,  24}, // 9  Reserved (mirrors FS5)
            {1,  4,  4,  2, 10, 10,  6,  16,  16,  24}, // 10 Reserved (mirrors FS5)
            /* 11 CNN-aware ("turn it up to 11"). Crank slots 7/8/9 (multi-level
               L1 highpass: LH1/HL1/HH1) so the encoder produces ~10-17% smaller
               files. Designed to pair with a CNN trained on the cranked
               distribution (e.g. BayInBayOut_1x_AAon_w16_ANE_HH1x4.pt) — see
               docs/quant_calibration_findings.md. Slots 0-6 unchanged from
               q=3 default so L2/L3 wavelet quality is preserved; single-ll
               mode reads slots 1/2/3 here (unchanged) and sees no benefit at
               q=11 — single-ll users wanting CNN-aware should crank slot 3
               directly via GPR_QUANT_OVERRIDE during development. */
            {1, 24, 24, 12, 24, 24, 12, 192, 192, 576}, // 11 CNN-aware
        };
        
        int quality = encoding_parameters->quality_setting;
        if( quality >= 0 && quality < VC5_ENCODER_QUALITY_SETTING_COUNT )
        {
            memcpy(parameters.quant_table, quant_table[quality], sizeof(parameters.quant_table));
        }
    }
    
    parameters.enabled_parts  = encoding_parameters->enabled_parts;
    parameters.encoded.format = IMAGE_FORMAT_RAW;
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
    // Test interlaced encoding using one layer per field
    parameters.layer_count = 2;
    parameters.progressive = 0;
    parameters.decompositor = DecomposeFields;
#endif
    
    parameters.allocator.Alloc = encoding_parameters->mem_alloc;
    parameters.allocator.Free = encoding_parameters->mem_free;

    parameters.denoise_enabled    = encoding_parameters->denoise_enabled;
    parameters.denoise_strength   = encoding_parameters->denoise_strength;
    parameters.noise_scale        = encoding_parameters->noise_scale;
    parameters.noise_offset       = encoding_parameters->noise_offset;
    parameters.variance_stabilize = encoding_parameters->variance_stabilize;
    parameters.ans_enabled        = encoding_parameters->ans_enabled;
    parameters.embedded_mode      = encoding_parameters->embedded_mode;

    // Check that the enabled parts are correct
    error = CheckEnabledParts(&parameters.enabled_parts);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    image.buffer = raw_buffer->buffer;
    
    image.width  = encoding_parameters->input_width;
    image.height = encoding_parameters->input_height;
    image.pitch  = encoding_parameters->input_pitch;
    image.size   = image.width * image.height * 2;
    image.offset = 0;

    switch( encoding_parameters->pixel_format )
    {
        case VC5_ENCODER_PIXEL_FORMAT_RGGB_12:
            image.format = PIXEL_FORMAT_RAW_RGGB_12;
            break;

        case VC5_ENCODER_PIXEL_FORMAT_RGGB_12P:
            image.format = PIXEL_FORMAT_RAW_RGGB_12P;
            break;
            
        case VC5_ENCODER_PIXEL_FORMAT_RGGB_14:
            image.format = PIXEL_FORMAT_RAW_RGGB_14;
            break;
            
        case VC5_ENCODER_PIXEL_FORMAT_GBRG_12:
            image.format = PIXEL_FORMAT_RAW_GBRG_12;
            break;

        case VC5_ENCODER_PIXEL_FORMAT_GBRG_14:
            image.format = PIXEL_FORMAT_RAW_GBRG_14;
            break;
            
        case VC5_ENCODER_PIXEL_FORMAT_GBRG_12P:
            image.format = PIXEL_FORMAT_RAW_GBRG_12P;
            break;

        case VC5_ENCODER_PIXEL_FORMAT_RGGB_16:
            image.format = PIXEL_FORMAT_RAW_RGGB_16;
            break;

        case VC5_ENCODER_PIXEL_FORMAT_GBRG_16:
            image.format = PIXEL_FORMAT_RAW_GBRG_16;
            break;

        case VC5_ENCODER_PIXEL_FORMAT_GRBG_12:
            image.format = PIXEL_FORMAT_RAW_GRBG_12;
            break;

        case VC5_ENCODER_PIXEL_FORMAT_GRBG_14:
            image.format = PIXEL_FORMAT_RAW_GRBG_14;
            break;

        case VC5_ENCODER_PIXEL_FORMAT_GRBG_16:
            image.format = PIXEL_FORMAT_RAW_GRBG_16;
            break;

        case VC5_ENCODER_PIXEL_FORMAT_BGGR_12:
            image.format = PIXEL_FORMAT_RAW_BGGR_12;
            break;

        case VC5_ENCODER_PIXEL_FORMAT_BGGR_14:
            image.format = PIXEL_FORMAT_RAW_BGGR_14;
            break;

        case VC5_ENCODER_PIXEL_FORMAT_BGGR_16:
            image.format = PIXEL_FORMAT_RAW_BGGR_16;
            break;
            
        default:
            assert(0);
    }
    
    // Set the dimensions and pixel format of the packed input image
    {
        parameters.input.width = image.width;
        parameters.input.height = image.height;
        parameters.input.format = image.format;
    }
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
    // Test interlaced encoding using one layer per field
    parameters.layer_count = 2;
    parameters.progressive = 0;
    parameters.decompositor = DecomposeFields;
#endif
    
    vc5_buffer->buffer = encoding_parameters->mem_alloc( max_vc5_buffer_size );
    if (vc5_buffer->buffer == NULL)
    {
        return CODEC_ERROR_OUTOFMEMORY;
    }
    
    // Open a stream to the output file
    error = CreateStreamBuffer(&bitstream_file, vc5_buffer->buffer, max_vc5_buffer_size );
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    RGB_IMAGE rgb_image;
    InitRGBImage(&rgb_image);

    // Encode the image into the byte stream
    error = EncodeImage(&image, &bitstream_file, &rgb_image, &parameters);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }

    // Copy noise model output back to caller (for DNG XMP serialization)
    if (encoding_parameters->denoise_enabled)
    {
        encoding_parameters->noise_seed = parameters.noise_seed;
        memcpy(encoding_parameters->noise_sigma_out, parameters.noise_sigma,
               sizeof(encoding_parameters->noise_sigma_out));
    }
    
    if( rgb_buffer )
    {
        rgb_buffer->buffer  = rgb_image.buffer;
        rgb_buffer->size    = rgb_image.size;
        rgb_buffer->width   = rgb_image.width;
        rgb_buffer->height  = rgb_image.height;
    }
    
    vc5_buffer->size = bitstream_file.byte_count;
    
    return CODEC_ERROR_OKAY;
}
