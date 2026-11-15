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

#include <stdint.h>
#if defined(_WIN32)
#include <windows.h>
static SRWLOCK encoder_process_mutex = SRWLOCK_INIT;
#else
#include <pthread.h>
static pthread_mutex_t encoder_process_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

void vc5_encoder_parameters_set_default(vc5_encoder_parameters* encoding_parameters)
{
    encoding_parameters->enabled_parts      = VC5_ENABLED_PARTS;
    encoding_parameters->input_width        = 4000;
    encoding_parameters->input_height       = 3000;
    encoding_parameters->input_pitch        = 8000;
    
    encoding_parameters->pixel_format       = VC5_ENCODER_PIXEL_FORMAT_DEFAULT;
    encoding_parameters->quality_setting    = VC5_ENCODER_QUALITY_SETTING_DEFAULT;

    encoding_parameters->mem_alloc = malloc;
    encoding_parameters->mem_free  = free;
}

typedef struct {
    vc5_encoder_preview_fn callback;
    void *user;
    unsigned width,height;
} PREVIEW_CALLBACK;
static void encoder_preview_adapter(void *user,const TRANSFORM *transforms)
{
    PREVIEW_CALLBACK *callback=user;
    vc5_preview_coefficients view={0};
    view.sensor_width=callback->width;view.sensor_height=callback->height;
    for (unsigned channel=0;channel<3;++channel) for (unsigned level=0;level<2;++level) {
        const WAVELET *wavelet=transforms[channel].wavelet[level+1];
        vc5_preview_level *output=&view.channels[channel][level];
        output->width=wavelet->width;output->height=wavelet->height;
        output->pitch_samples=wavelet->pitch/sizeof(PIXEL);
        output->prescale=transforms[channel].prescale[level+1];
        for (unsigned band=0;band<4;++band) {
            output->bands[band]=wavelet->data[band];output->quant[band]=wavelet->quant[band];
            output->band_samples[band]=(size_t)(wavelet->height-1)*output->pitch_samples+wavelet->width;
        }
    }
    callback->callback(callback->user,&view);
}
static CODEC_ERROR vc5_encoder_encode(const vc5_encoder_parameters*   encoding_parameters,    /* vc5 encoding parameters */
                                const gpr_buffer*               raw_buffer,             /* raw input buffer. */
                                      gpr_buffer*               vc5_buffer,
                                      gpr_rgb_buffer*           rgb_buffer,
                                      vc5_encoder_preview_fn preview,void *preview_user,
                                      vc5_encoder_preview_row_fn rows,void *row_user)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    IMAGE image;
    ENCODER_PARAMETERS parameters;
    
    STREAM bitstream_file;
    
    const size_t buffer_margin = 1024 * 1024;
    size_t max_vc5_buffer_size;

    if (encoding_parameters == NULL || encoding_parameters->mem_alloc == NULL ||
        raw_buffer == NULL || raw_buffer->buffer == NULL || vc5_buffer == NULL ||
        raw_buffer->size > SIZE_MAX - (raw_buffer->size / 2) - buffer_margin)
    {
        return CODEC_ERROR_BAD_ARGUMENT;
    }

    /*
     * A fixed 10 MB buffer is too small for full-resolution camera frames.
     * Keep enough headroom for incompressible input and container overhead.
     */
    max_vc5_buffer_size = raw_buffer->size + (raw_buffer->size / 2) + buffer_margin;

    // Initialize the data structure for passing parameters to the encoder
    InitEncoderParameters(&parameters);
    PREVIEW_CALLBACK callback={preview,preview_user,encoding_parameters->input_width,encoding_parameters->input_height};
    if (preview) {parameters.preview_observer=encoder_preview_adapter;parameters.preview_user=&callback;}
    parameters.preview_row_observer=rows;parameters.preview_row_user=row_user;
    
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
    image.size   = raw_buffer->size;
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
            
        case VC5_ENCODER_PIXEL_FORMAT_GBRG_12P:
            image.format = PIXEL_FORMAT_RAW_GBRG_12P;
            break;
            
        case VC5_ENCODER_PIXEL_FORMAT_RGGB_16:
            image.format = PIXEL_FORMAT_RAW_RGGB_16;
            break;
        case VC5_ENCODER_PIXEL_FORMAT_GBRG_16:
            image.format = PIXEL_FORMAT_RAW_GBRG_16;
            break;
        case VC5_ENCODER_PIXEL_FORMAT_GBRG_14:
            image.format = PIXEL_FORMAT_RAW_GBRG_14;
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
            return CODEC_ERROR_PIXEL_FORMAT;
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
    // A thumbnail is optional. Generating one for a NULL recipient both
    // wastes frame time and leaves its allocation without an owner.
    error = EncodeImage(&image, &bitstream_file, rgb_buffer ? &rgb_image : NULL, &parameters);
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

/* Validate the advertised input span before assembly, SIMD or scalar unpacking.
 * The third six-tap transform requires at least 48 sensor samples per axis.
 * Dimensions are stored in 16-bit codec fields and image counts use signed int. */
static CODEC_ERROR vc5_encoder_process_unlocked(const vc5_encoder_parameters *p,
                                               const gpr_buffer *raw, gpr_buffer *out,
                                               gpr_rgb_buffer *rgb,vc5_encoder_preview_fn preview,void *preview_user,
                                      vc5_encoder_preview_row_fn rows,void *row_user)
{
    size_t row,span,pitch;
    gpr_buffer input;
    vc5_encoder_parameters normalized;
    void *scratch=NULL;
    CODEC_ERROR error;
    bool packed;
    if (!p || !raw || !raw->buffer || !out || raw==out || !p->mem_alloc || !p->mem_free)
        return CODEC_ERROR_BAD_ARGUMENT;
    if ((unsigned)p->pixel_format>VC5_ENCODER_PIXEL_FORMAT_BGGR_16)
        return CODEC_ERROR_PIXEL_FORMAT;
    if ((unsigned)p->quality_setting>=VC5_ENCODER_QUALITY_SETTING_COUNT)
        return CODEC_ERROR_BAD_ARGUMENT;
    if (p->input_width<48 || p->input_height<48 || p->input_width>UINT16_MAX ||
        p->input_height>UINT16_MAX || (p->input_width&1) || (p->input_height&1) ||
        (uint64_t)p->input_width*p->input_height>INT_MAX/2)
        return CODEC_ERROR_IMAGE_DIMENSIONS;
    packed=p->pixel_format==VC5_ENCODER_PIXEL_FORMAT_RGGB_12P ||
        p->pixel_format==VC5_ENCODER_PIXEL_FORMAT_GBRG_12P;
    row=packed ? (size_t)p->input_width*3/2 : (size_t)p->input_width*2;
    if (p->input_pitch<=0 || (size_t)p->input_pitch<row) return CODEC_ERROR_BAD_ARGUMENT;
    pitch=(size_t)p->input_pitch;
    if (p->input_height-1>(SIZE_MAX-row)/pitch) return CODEC_ERROR_BAD_ARGUMENT;
    span=(p->input_height-1)*pitch+row;
    if (raw->size<span) return CODEC_ERROR_BAD_ARGUMENT;
    input=*raw;input.size=span;
    normalized=*p;
    /* Historical unpackers address uint16_t rows. Keep the aligned fast path
     * untouched; normalize only layouts that would misalign/round that stride.
     * New word-Bayer fallbacks already use memcpy and accept arbitrary bytes. */
    if (p->pixel_format<=VC5_ENCODER_PIXEL_FORMAT_GBRG_12P &&
        (((uintptr_t)raw->buffer&1) || (pitch&1))) {
        const gpr_allocator allocator={p->mem_alloc,p->mem_free};
        size_t aligned_pitch=(row+1)&~(size_t)1;
        if (p->input_height>SIZE_MAX/aligned_pitch) return CODEC_ERROR_BAD_ARGUMENT;
        scratch=gpr_aligned_alloc(&allocator,aligned_pitch*p->input_height);
        if (!scratch) return CODEC_ERROR_OUTOFMEMORY;
        for (unsigned y=0;y<p->input_height;++y) {
            uint8_t *destination=(uint8_t *)scratch+y*aligned_pitch;
            memcpy(destination,(const uint8_t *)raw->buffer+y*pitch,row);
            if (aligned_pitch>row) destination[row]=0;
        }
        normalized.input_pitch=(int)aligned_pitch;
        input.buffer=scratch;input.size=(p->input_height-1)*aligned_pitch+row;
    }
    error=vc5_encoder_encode(&normalized,&input,out,rgb,preview,preview_user,rows,row_user);
    if (scratch) {
        const gpr_allocator allocator={p->mem_alloc,p->mem_free};
        gpr_aligned_free(&allocator,scratch);
    }
    return error;
}

/* Codebook lookup buffers are process-global in the historical implementation.
 * Serialize top-level calls from capture AND the still SDK; independent Bayer
 * channel workers inside each call remain parallel. */
static CODEC_ERROR vc5_encoder_process_locked(const vc5_encoder_parameters *parameters,
    const gpr_buffer *raw,gpr_buffer *vc5,gpr_rgb_buffer *rgb,
    vc5_encoder_preview_fn preview,void *preview_user,
    vc5_encoder_preview_row_fn rows,void *row_user)
{
    CODEC_ERROR error;
#if defined(_WIN32)
    AcquireSRWLockExclusive(&encoder_process_mutex);
#else
    pthread_mutex_lock(&encoder_process_mutex);
#endif
    error=vc5_encoder_process_unlocked(parameters,raw,vc5,rgb,preview,preview_user,rows,row_user);
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&encoder_process_mutex);
#else
    pthread_mutex_unlock(&encoder_process_mutex);
#endif
    return error;
}
CODEC_ERROR vc5_encoder_process(const vc5_encoder_parameters *parameters,
    const gpr_buffer *raw,gpr_buffer *vc5,gpr_rgb_buffer *rgb)
{
    return vc5_encoder_process_locked(parameters,raw,vc5,rgb,NULL,NULL,NULL,NULL);
}
CODEC_ERROR vc5_encoder_process_with_preview(const vc5_encoder_parameters *parameters,
    const gpr_buffer *raw,gpr_buffer *vc5,gpr_rgb_buffer *rgb,
    vc5_encoder_preview_fn preview,void *user)
{
    return vc5_encoder_process_locked(parameters,raw,vc5,rgb,preview,user,NULL,NULL);
}
CODEC_ERROR vc5_encoder_process_with_preview_rows(const vc5_encoder_parameters *parameters,
    const gpr_buffer *raw,gpr_buffer *vc5,gpr_rgb_buffer *rgb,
    vc5_encoder_preview_row_fn rows,void *user)
{
    return vc5_encoder_process_locked(parameters,raw,vc5,rgb,NULL,NULL,rows,user);
}
