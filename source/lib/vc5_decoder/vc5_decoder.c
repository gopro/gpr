/*! @file vc5_decoder.c
 *
 *  @brief Implementation of the top level vc5 decoder API.
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

void vc5_decoder_parameters_set_default(vc5_decoder_parameters* decoding_parameters)
{
    decoding_parameters->enabled_parts = VC5_ENABLED_PARTS;
    
    decoding_parameters->pixel_format = VC5_DECODER_PIXEL_FORMAT_DEFAULT;
    
    decoding_parameters->rgb_resolution = VC5_DECODER_RGB_RESOLUTION_DEFAULT;
    decoding_parameters->rgb_bits = 8;
    
    gpr_rgb_gain_set_defaults(&decoding_parameters->rgb_gain);
}

CODEC_ERROR vc5_decoder_process(const vc5_decoder_parameters*   decoding_parameters,    /* vc5 decoding parameters */
                                const gpr_buffer*               vc5_buffer,             /* vc5 input buffer. */
                                      gpr_buffer*               raw_buffer,             /* raw output buffer. */
                                      gpr_rgb_buffer*           rgb_buffer)             /* rgb output buffer */
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;

    IMAGE output_image;
    // Clear the members of the image data structure
    InitImage(&output_image);
    
    STREAM input;
    DECODER_PARAMETERS parameters;
    
    // Initialize the parameters passed to the decoder
    InitDecoderParameters(&parameters);
    
    parameters.enabled_parts   = decoding_parameters->enabled_parts;    
    
    parameters.rgb_resolution  = decoding_parameters->rgb_resolution;
    parameters.rgb_bits        = decoding_parameters->rgb_bits;
    parameters.rgb_gain        = decoding_parameters->rgb_gain;
    
    if( rgb_buffer == NULL )
    {
        parameters.rgb_resolution = GPR_RGB_RESOLUTION_NONE;
    }
        
    parameters.allocator.Alloc = decoding_parameters->mem_alloc;
    parameters.allocator.Free  = decoding_parameters->mem_free;
    
    switch( decoding_parameters->pixel_format )
    {
        case VC5_DECODER_PIXEL_FORMAT_RGGB_12:
            parameters.output.format = PIXEL_FORMAT_RAW_RGGB_12;
            break;

        case VC5_DECODER_PIXEL_FORMAT_RGGB_14:
            parameters.output.format = PIXEL_FORMAT_RAW_RGGB_14;
            break;
            
        case VC5_DECODER_PIXEL_FORMAT_GBRG_12:
            parameters.output.format = PIXEL_FORMAT_RAW_GBRG_12;
            break;

        case VC5_DECODER_PIXEL_FORMAT_GBRG_14:
            parameters.output.format = PIXEL_FORMAT_RAW_GBRG_14;
            break;
            
        default:
            assert(0);
    }
    
    // Check that the enabled parts are correct
    error =  CheckEnabledParts(&parameters.enabled_parts);
    if (error != CODEC_ERROR_OKAY) {
        return CODEC_ERROR_ENABLED_PARTS;
    }

    error = OpenStreamBuffer(&input, vc5_buffer->buffer, vc5_buffer->size );
    if (error != CODEC_ERROR_OKAY) {
        fprintf(stderr, "Could not open input vc5 stream\n" );
        return error;
    }

    RGB_IMAGE rgb_image;
    InitRGBImage(&rgb_image);
    
    error = DecodeImage(&input, &output_image, &rgb_image, &parameters);
    if (error != CODEC_ERROR_OKAY) {
        fprintf(stderr, "Could not decode input vc5 bitstream. Error number %d\n", error );
        return error;
    }
    
    if( parameters.rgb_resolution != GPR_RGB_RESOLUTION_NONE )
    {
        assert( rgb_buffer);
        
        rgb_buffer->buffer  = rgb_image.buffer;
        rgb_buffer->size    = rgb_image.size;
        rgb_buffer->width   = rgb_image.width;
        rgb_buffer->height  = rgb_image.height;
    }
    
    if( raw_buffer )
    {
        assert( output_image.buffer );
        assert( output_image.size == output_image.width * output_image.height * sizeof(unsigned short) );

        raw_buffer->buffer  = output_image.buffer;
        raw_buffer->size    = output_image.size;
    }
    else
    {
        // Nothing should be returned in output_image since we do not want output raw image
        assert( output_image.buffer == NULL );
    }
    
    return error;
}
