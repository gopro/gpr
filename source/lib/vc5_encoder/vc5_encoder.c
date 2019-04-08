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
}

CODEC_ERROR vc5_encoder_process(const vc5_encoder_parameters*   encoding_parameters,    /* vc5 encoding parameters */
                                const gpr_buffer*               raw_buffer,             /* raw input buffer. */
                                      gpr_buffer*               vc5_buffer,
                                      gpr_rgb_buffer*           rgb_buffer)             /* rgb output buffer. */
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    IMAGE image;
    ENCODER_PARAMETERS parameters;
    
    STREAM bitstream_file;
    
    const int max_vc5_buffer_size = 10000000;

    // Initialize the data structure for passing parameters to the encoder
    InitEncoderParameters(&parameters);
    
    {
        QUANT quant_table[VC5_ENCODER_QUALITY_SETTING_COUNT][sizeof(parameters.quant_table) / sizeof(parameters.quant_table[0])] = {
            {1, 24, 24, 12, 64, 64, 48, 512, 512, 768}, // CineForm Low
            {1, 24, 24, 12, 48, 48, 32, 256, 256, 384}, // CineForm Medium
            {1, 24, 24, 12, 32, 32, 24, 128, 128, 192}, // CineForm High
            {1, 24, 24, 12, 24, 24, 12, 96, 96, 144},   // CineForm Filmscan-1
            {1, 24, 24, 12, 24, 24, 12, 64, 64, 96},    // CineForm Filmscan-X
            {1, 24, 24, 12, 24, 24, 12, 32, 32, 48}     // CineForm Filmscan-2
        };
        
        if( encoding_parameters->quality_setting < VC5_ENCODER_QUALITY_SETTING_COUNT )
        {
            memcpy(parameters.quant_table, quant_table[encoding_parameters->quality_setting], sizeof(parameters.quant_table));
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

		case VC5_ENCODER_PIXEL_FORMAT_RGGB_16:
			image.format = PIXEL_FORMAT_RAW_RGGB_16;
			break;
            
        case VC5_ENCODER_PIXEL_FORMAT_GBRG_12:
            image.format = PIXEL_FORMAT_RAW_GBRG_12;
            break;
            
        case VC5_ENCODER_PIXEL_FORMAT_GBRG_12P:
            image.format = PIXEL_FORMAT_RAW_GBRG_12P;
            break;

		case VC5_ENCODER_PIXEL_FORMAT_GBRG_14:
			image.format = PIXEL_FORMAT_RAW_GBRG_14;
			break;

		case VC5_ENCODER_PIXEL_FORMAT_GBRG_16:
			image.format = PIXEL_FORMAT_RAW_GBRG_16;
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
