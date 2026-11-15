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
static CODEC_ERROR vc5_decoder_process_internal(
                                const vc5_decoder_parameters*   decoding_parameters,
                                const gpr_buffer*               vc5_buffer,
                                      gpr_buffer*               raw_buffer,
                                      gpr_rgb_buffer*           rgb_buffer,
                                      int                       wavelet_channel)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;

    if (!decoding_parameters || !vc5_buffer || !vc5_buffer->buffer ||
        (!raw_buffer && !rgb_buffer))
        return CODEC_ERROR_NULLPTR;
    if (!decoding_parameters->mem_alloc || !decoding_parameters->mem_free ||
        (raw_buffer && rgb_buffer) ||
        (const void *)raw_buffer == (const void *)vc5_buffer ||
        (const void *)raw_buffer == (const void *)rgb_buffer ||
        (const void *)rgb_buffer == (const void *)vc5_buffer ||
        (const void *)raw_buffer == (const void *)decoding_parameters ||
        (const void *)rgb_buffer == (const void *)decoding_parameters ||
        vc5_buffer->size < 64 || vc5_buffer->size % 4 ||
        memcmp(vc5_buffer->buffer, "VC-5", 4))
        return CODEC_ERROR_BAD_ARGUMENT;
    if (rgb_buffer) {
        const gpr_rgb_gain *gain = &decoding_parameters->rgb_gain;
        if (decoding_parameters->rgb_resolution < GPR_RGB_RESOLUTION_SIXTEENTH ||
            decoding_parameters->rgb_resolution > GPR_RGB_RESOLUTION_HALF ||
            (decoding_parameters->rgb_bits != 8 && decoding_parameters->rgb_bits != 16) ||
            (decoding_parameters->rgb_bits == 8 &&
             (gain->r_gain_num < 1 || gain->r_gain_num > 1024 ||
              gain->g_gain_num < 1 || gain->g_gain_num > 1024 ||
              gain->b_gain_num < 1 || gain->b_gain_num > 1024 ||
              gain->r_gain_pow2_den < 0 || gain->r_gain_pow2_den > 16 ||
              gain->g_gain_pow2_den < 0 || gain->g_gain_pow2_den > 16 ||
              gain->b_gain_pow2_den < 0 || gain->b_gain_pow2_den > 16)))
            return CODEC_ERROR_BAD_ARGUMENT;
    }

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
    parameters.wavelet_channel = wavelet_channel;
    
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
            
        case VC5_DECODER_PIXEL_FORMAT_RGGB_16:
            parameters.output.format = PIXEL_FORMAT_RAW_RGGB_16;
            break;
        case VC5_DECODER_PIXEL_FORMAT_GBRG_16:
            parameters.output.format = PIXEL_FORMAT_RAW_GBRG_16;
            break;
        case VC5_DECODER_PIXEL_FORMAT_GRBG_12:
            parameters.output.format = PIXEL_FORMAT_RAW_GRBG_12;
            break;
        case VC5_DECODER_PIXEL_FORMAT_GRBG_14:
            parameters.output.format = PIXEL_FORMAT_RAW_GRBG_14;
            break;
        case VC5_DECODER_PIXEL_FORMAT_GRBG_16:
            parameters.output.format = PIXEL_FORMAT_RAW_GRBG_16;
            break;
        case VC5_DECODER_PIXEL_FORMAT_BGGR_12:
            parameters.output.format = PIXEL_FORMAT_RAW_BGGR_12;
            break;
        case VC5_DECODER_PIXEL_FORMAT_BGGR_14:
            parameters.output.format = PIXEL_FORMAT_RAW_BGGR_14;
            break;
        case VC5_DECODER_PIXEL_FORMAT_BGGR_16:
            parameters.output.format = PIXEL_FORMAT_RAW_BGGR_16;
            break;
        default:
            return CODEC_ERROR_PIXEL_FORMAT;
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

CODEC_ERROR vc5_decoder_process(const vc5_decoder_parameters* decoding_parameters,
                                const gpr_buffer* vc5_buffer,
                                gpr_buffer* raw_buffer,
                                gpr_rgb_buffer* rgb_buffer)
{
    return vc5_decoder_process_internal(decoding_parameters, vc5_buffer,
                                        raw_buffer, rgb_buffer, -1);
}

CODEC_ERROR vc5_decoder_process_wavelet_channel(
                                const vc5_decoder_parameters* decoding_parameters,
                                const gpr_buffer* vc5_buffer,
                                unsigned channel,
                                gpr_rgb_buffer* wavelet_buffer)
{
    if (!decoding_parameters || !wavelet_buffer || channel >= 3 ||
        (decoding_parameters->rgb_resolution != GPR_RGB_RESOLUTION_QUARTER &&
         decoding_parameters->rgb_resolution != GPR_RGB_RESOLUTION_EIGHTH &&
         decoding_parameters->rgb_resolution != GPR_RGB_RESOLUTION_SIXTEENTH))
        return CODEC_ERROR_BAD_ARGUMENT;
    return vc5_decoder_process_internal(decoding_parameters, vc5_buffer, NULL,
                                        wavelet_buffer, (int)channel);
}

static CODEC_ERROR reconstruct_preview(const vc5_preview_coefficients *input,
    gpr_malloc mem_alloc,gpr_free mem_free,gpr_rgb_buffer planes[3],int selected)
{
    WAVELET *levels[3]={NULL,NULL,NULL};
    gpr_allocator allocator={mem_alloc,mem_free};
    CODEC_ERROR error=CODEC_ERROR_OKAY;
    int16_t cubic[1025];
    if (!input || !mem_alloc || !mem_free || !planes ||
        input->sensor_width<48 || input->sensor_height<48 ||
        input->sensor_width>UINT16_MAX || input->sensor_height>UINT16_MAX ||
        (uint64_t)input->sensor_width*input->sensor_height>INT_MAX/2 ||
        (input->sensor_width&1) || (input->sensor_height&1)) return CODEC_ERROR_BAD_ARGUMENT;
    for (unsigned channel=0;channel<3;++channel) {
        if (planes[channel].buffer || planes[channel].size) return CODEC_ERROR_BAD_ARGUMENT;
        for (unsigned level=0;level<2;++level) {
            const vc5_preview_level *source=&input->channels[channel][level];
            unsigned scale=8u<<level;
            if (source->width!=(input->sensor_width+scale-1)/scale ||
                source->height!=(input->sensor_height+scale-1)/scale ||
                source->pitch_samples<source->width || source->pitch_samples>UINT16_MAX/2 ||
                (source->prescale!=0 && source->prescale!=2)) return CODEC_ERROR_BAD_ARGUMENT;
            for (unsigned band=level ? 0:1;band<4;++band) {
                size_t required=(size_t)(source->height-1)*source->pitch_samples+source->width;
                if (!source->bands[band] || ((uintptr_t)source->bands[band]&1) ||
                    source->band_samples[band]<required || !source->quant[band] || source->quant[band]>32767)
                    return CODEC_ERROR_BAD_ARGUMENT;
            }
        }
    }
    ComputeCubicTable(cubic,1025,255);
    for (unsigned channel=selected<0 ? 0:(unsigned)selected;channel<(selected<0 ? 3:(unsigned)selected+1);++channel) {
        // Only intermediate levels need four bands. The final inverse writes
        // directly to the caller-owned packed output, avoiding a full-plane
        // copy and three unused final highpass bands. Keep intermediate
        // wavelets' aligned, cache-skewed pitches unchanged.
        for (unsigned stage=1;stage<3;++stage) {
            unsigned scale=4u<<stage;
            levels[stage]=CreateWavelet(&allocator,(input->sensor_width+scale-1)/scale,
                (input->sensor_height+scale-1)/scale);
            if (!levels[stage]) {error=CODEC_ERROR_OUTOFMEMORY;goto done;}
            const vc5_preview_level *source=&input->channels[channel][stage-1];
            for (unsigned band=stage==2 ? 0:1;band<4;++band) {
                levels[stage]->quant[band]=(QUANT)source->quant[band];
                for (unsigned y=0;y<source->height;++y) {
                    const int16_t *row=source->bands[band]+(size_t)y*source->pitch_samples;
                    PIXEL *out=(PIXEL *)((uint8_t *)levels[stage]->data[band]+(size_t)y*levels[stage]->pitch);
                    for (unsigned x=0;x<source->width;++x) {
                        int value=row[x],magnitude=value<0 ? -value:value;
                        if (band) {if (magnitude>1023) magnitude=1023;value=cubic[magnitude]*(value<0 ? -1:1);}
                        out[x]=(PIXEL)value;
                    }
                }
            }
        }
        error=TransformInverseSpatialQuantLowpass(&allocator,levels[2],levels[1],input->channels[channel][1].prescale);
        if (error) goto done;
        gpr_rgb_buffer *output=&planes[channel];
        output->width=(input->sensor_width+3)/4;output->height=(input->sensor_height+3)/4;
        output->size=(size_t)output->width*output->height*sizeof(PIXEL);
        output->buffer=mem_alloc(output->size);
        if (!output->buffer) {error=CODEC_ERROR_OUTOFMEMORY;goto done;}
        error=TransformInverseSpatialQuantArray(&allocator,levels[1],output->buffer,
            output->width,output->height,output->width*sizeof(PIXEL),input->channels[channel][0].prescale);
        if (error) goto done;
        for (unsigned stage=1;stage<3;++stage) {DeleteWavelet(&allocator,levels[stage]);levels[stage]=NULL;}
    }
done:
    for (unsigned stage=0;stage<3;++stage) if (levels[stage]) DeleteWavelet(&allocator,levels[stage]);
    if (error) for (unsigned channel=0;channel<3;++channel) {
        if (planes[channel].buffer) mem_free(planes[channel].buffer);
        memset(&planes[channel],0,sizeof(planes[channel]));
    }
    return error;
}
CODEC_ERROR vc5_decoder_reconstruct_preview(const vc5_preview_coefficients *input,
    gpr_malloc mem_alloc,gpr_free mem_free,gpr_rgb_buffer planes[3])
{
    return reconstruct_preview(input,mem_alloc,mem_free,planes,-1);
}
CODEC_ERROR vc5_decoder_reconstruct_preview_channel(const vc5_preview_coefficients *input,
    unsigned channel,gpr_malloc mem_alloc,gpr_free mem_free,gpr_rgb_buffer *plane)
{
    gpr_rgb_buffer planes[3]={{0}};
    if (channel>=3 || !plane || plane->buffer || plane->size) return CODEC_ERROR_BAD_ARGUMENT;
    CODEC_ERROR error=reconstruct_preview(input,mem_alloc,mem_free,planes,(int)channel);
    if (!error) *plane=planes[channel];
    return error;
}
