/*! @file main_c.c
 *
 *  @brief Implement C conversion routines used by gpr_tools
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

#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>

#include "gpr.h"

#if defined __GNUC__
#define stricmp strcasecmp
#else
#endif // if defined __GNUC__

#include "main_c.h"
#include "gpr_parse_utils.h"
#include "gpr_print_utils.h"

#if GPR_JPEG_AVAILABLE
#include "jpeg.h"
#endif

#define MAX_FILE_PATH 256

typedef enum
{
    FILE_TYPE_UNKNOWN = -1,
    
    FILE_TYPE_RAW,
    FILE_TYPE_GPR,
    FILE_TYPE_DNG,
    FILE_TYPE_PPM,
    FILE_TYPE_JPG,
    
    FILE_TYPE_COUNT,
    
} FILE_TYPE;

static FILE_TYPE GetFileType( const char* file_path )
{
    const char *extension = NULL;
    
    if (file_path == NULL) {
        return FILE_TYPE_UNKNOWN;
    }
    
    // Get the pathname extension
    extension = strrchr(file_path, '.');
    if (extension == NULL)
    {
        return FILE_TYPE_UNKNOWN;
    }
    
    if (stricmp(extension, ".raw") == 0 || stricmp(extension, ".RAW") == 0)
    {
        return FILE_TYPE_RAW;
    }
    
    if (stricmp(extension, ".gpr") == 0 || stricmp(extension, ".GPR") == 0)
    {
        return FILE_TYPE_GPR;
    }
    
    if (stricmp(extension, ".dng") == 0 || stricmp(extension, ".DNG") == 0 )
    {
        return FILE_TYPE_DNG;
    }
    
    if (stricmp(extension, ".ppm") == 0 || stricmp(extension, ".PPM") == 0)
    {
        return FILE_TYPE_PPM;
    }

    if (stricmp(extension, ".jpg") == 0 || stricmp(extension, ".JPG") == 0)
    {
        return FILE_TYPE_JPG;
    }

    return FILE_TYPE_UNKNOWN;
}

int dng_convert_main(const char*  input_file_path, unsigned int input_width, unsigned int input_height, size_t input_pitch, size_t input_skip_rows, const char* input_pixel_format,
                     const char*  output_file_path, const char*  metadata_file_path, const char* gpmf_file_path, const char* rgb_file_resolution, int rgb_file_bits,
                     const char*  jpg_preview_file_path, int jpg_preview_file_width, int jpg_preview_file_height, int quality, bool enable_preview )
{
    bool success;
    bool write_buffer_to_file = true;
    
    FILE_TYPE input_file_type  = GetFileType( input_file_path );
    FILE_TYPE output_file_type = GetFileType( output_file_path );
    if (quality < -1 || quality > 5 || (quality >= 0 &&
        (output_file_type != FILE_TYPE_GPR || (input_file_type != FILE_TYPE_RAW && input_file_type != FILE_TYPE_DNG)))) {
        fprintf(stderr,"Quality must be 0..5 and applies to RAW/DNG to GPR encoding\n");
        return -1;
    }
    
    if( input_file_type == FILE_TYPE_UNKNOWN )
    {
        printf( "Unsupported input file type" );
        return -1;
    }

    if( output_file_type == FILE_TYPE_UNKNOWN )
    {
        printf( "Unsupported output file type" );
        return -1;
    }

    gpr_allocator allocator;
    allocator.Alloc = malloc;
    allocator.Free = free;
    
    gpr_parameters params;
    gpr_parameters_set_defaults(&params);
    
    gpr_buffer input_buffer  = { NULL, 0 };
    gpr_buffer output_buffer = { NULL, 0 };
    gpr_buffer preview = { NULL, 0 };
    int result = -1;
    
    if( read_from_file( &input_buffer, input_file_path, allocator.Alloc, allocator.Free ) != 0 )
    {
        return -1;
    }
    void *input_base = input_buffer.buffer;
  
    if( metadata_file_path && strcmp(metadata_file_path, "") )
    {
        if( gpr_parameters_parse( &params, metadata_file_path ) != 0 )
            goto cleanup;
    }
    else if( input_file_type == FILE_TYPE_GPR || input_file_type == FILE_TYPE_DNG )
    {
        if( output_file_type != FILE_TYPE_RAW && !gpr_parse_metadata( &allocator, &input_buffer, &params ) )
            goto cleanup;
    }
    else
    {
        struct input_format { const char *name; GPR_PIXEL_FORMAT format; unsigned bits; bool packed; };
        static const struct input_format formats[] = {
            {"rggb12",PIXEL_FORMAT_RGGB_12,12,false},
            {"rggb12p",PIXEL_FORMAT_RGGB_12P,12,true},
            {"rggb14",PIXEL_FORMAT_RGGB_14,14,false},
            {"gbrg12",PIXEL_FORMAT_GBRG_12,12,false},
            {"gbrg12p",PIXEL_FORMAT_GBRG_12P,12,true},
            {"rggb16",PIXEL_FORMAT_RGGB_16,16,false},
            {"gbrg16",PIXEL_FORMAT_GBRG_16,16,false},
            {"gbrg14",PIXEL_FORMAT_GBRG_14,14,false},
            {"grbg12",PIXEL_FORMAT_GRBG_12,12,false},
            {"grbg14",PIXEL_FORMAT_GRBG_14,14,false},
            {"grbg16",PIXEL_FORMAT_GRBG_16,16,false},
            {"bggr12",PIXEL_FORMAT_BGGR_12,12,false},
            {"bggr14",PIXEL_FORMAT_BGGR_14,14,false},
            {"bggr16",PIXEL_FORMAT_BGGR_16,16,false},
        };
        const struct input_format *selected = NULL;
        for (size_t i=0;i<sizeof(formats)/sizeof(formats[0]);++i)
            if (!strcmp(input_pixel_format,formats[i].name)) { selected=&formats[i]; break; }
        if (!selected) { fprintf(stderr,"Unsupported Bayer input format: %s\n",input_pixel_format); goto cleanup; }
        size_t row_bytes = selected->packed ? (size_t)input_width*3/2 : (size_t)input_width*2;
        if (input_pitch == SIZE_MAX) input_pitch = row_bytes;
        if (!input_width || !input_height || input_width%2 || input_height%2 ||
            input_pitch<row_bytes || input_pitch>UINT_MAX ||
            input_height-1 > (SIZE_MAX-row_bytes)/input_pitch ||
            input_buffer.size < (input_height-1)*input_pitch+row_bytes) goto cleanup;
        params.input_width = input_width;
        params.input_height = input_height;
        params.input_pitch = (unsigned int)input_pitch;
        params.tuning_info.pixel_format = selected->format;
        int32_t saturation_level = (1u << selected->bits)-1;
        params.tuning_info.dgain_saturation_level.level_red         = saturation_level;
        params.tuning_info.dgain_saturation_level.level_green_even  = saturation_level;
        params.tuning_info.dgain_saturation_level.level_green_odd   = saturation_level;
        params.tuning_info.dgain_saturation_level.level_blue        = saturation_level;
    }
    
    if (!enable_preview) params.enable_preview = false;
    if( gpmf_file_path != NULL && strcmp(gpmf_file_path, "") )
    {
        if( params.gpmf_payload.buffer ) allocator.Free(params.gpmf_payload.buffer);
        params.gpmf_payload.buffer = NULL;
        params.gpmf_payload.size = 0;
        if( read_from_file( &params.gpmf_payload, gpmf_file_path, allocator.Alloc, allocator.Free ) != 0 )
            goto cleanup;
    }

    if( input_skip_rows > 0 )
    {
        if( input_file_type != FILE_TYPE_RAW || input_pitch == 0 || input_skip_rows > SIZE_MAX / input_pitch ||
            input_skip_rows * input_pitch >= input_buffer.size ) goto cleanup;
        input_buffer.buffer = (unsigned char*)(input_buffer.buffer) + (input_skip_rows * input_pitch);
        input_buffer.size -= input_skip_rows * input_pitch;
    }

    if( strcmp(jpg_preview_file_path, "") != 0 )
    {
        if( read_from_file( &preview, jpg_preview_file_path, allocator.Alloc, allocator.Free) == 0 )
        {
            params.preview_image.jpg_preview    = preview;
            params.preview_image.preview_width  = jpg_preview_file_width;
            params.preview_image.preview_height = jpg_preview_file_height;
        }
        else goto cleanup;
    }
    
    if( input_file_type == FILE_TYPE_RAW && output_file_type == FILE_TYPE_DNG )
    {
        success = gpr_convert_raw_to_dng( &allocator, &params, &input_buffer, &output_buffer );
    }
    else if( input_file_type == FILE_TYPE_DNG && output_file_type == FILE_TYPE_RAW )
    {
        success = gpr_convert_dng_to_raw( &allocator, &input_buffer, &output_buffer );
    }
    else if( input_file_type == FILE_TYPE_DNG && output_file_type == FILE_TYPE_DNG )
    {
        success = gpr_convert_dng_to_dng( &allocator, &params, &input_buffer, &output_buffer );
    }
#if GPR_WRITING
    else if( input_file_type == FILE_TYPE_DNG && output_file_type == FILE_TYPE_GPR )
    {
        success = quality < 0 ? gpr_convert_dng_to_gpr( &allocator, &params, &input_buffer, &output_buffer ) :
            gpr_convert_dng_to_gpr_with_quality(&allocator,&params,(unsigned)quality,&input_buffer,&output_buffer);
    }
    else if( input_file_type == FILE_TYPE_RAW && output_file_type == FILE_TYPE_GPR )
    {
        success = quality < 0 ? gpr_convert_raw_to_gpr( &allocator, &params, &input_buffer, &output_buffer ) :
            gpr_convert_raw_to_gpr_with_quality(&allocator,&params,(unsigned)quality,&input_buffer,&output_buffer);
    }
#endif
#if GPR_READING
    else if( input_file_type == FILE_TYPE_GPR && ( output_file_type == FILE_TYPE_PPM || output_file_type == FILE_TYPE_JPG ) )
    {
        gpr_rgb_buffer rgb_buffer = { NULL, 0, 0, 0 };

        GPR_RGB_RESOLUTION rgb_resolution = GPR_RGB_RESOLUTION_DEFAULT;
        
        if( strcmp(rgb_file_resolution, "1:1") == 0 )
            rgb_resolution = GPR_RGB_RESOLUTION_FULL;
        else if( strcmp(rgb_file_resolution, "2:1") == 0 )
            rgb_resolution = GPR_RGB_RESOLUTION_HALF;
        else if( strcmp(rgb_file_resolution, "4:1") == 0 )
            rgb_resolution = GPR_RGB_RESOLUTION_QUARTER;
        else if( strcmp(rgb_file_resolution, "8:1") == 0 )
            rgb_resolution = GPR_RGB_RESOLUTION_EIGHTH;
        else if( strcmp(rgb_file_resolution, "16:1") == 0 )
            rgb_resolution = GPR_RGB_RESOLUTION_SIXTEENTH;

        if( output_file_type == FILE_TYPE_JPG && rgb_file_bits == 16 )
        {
            printf( "Asked to output 16-bits RGB, but that is only possible in PPM format.\n");
            rgb_file_bits = 8;
        }
            
        success = gpr_convert_gpr_to_rgb( &allocator, rgb_resolution, rgb_file_bits,  &input_buffer, &rgb_buffer );
        if( !success ) { allocator.Free(rgb_buffer.buffer); goto cleanup; }
        
        if( output_file_type == FILE_TYPE_PPM )
        {
#define PPM_HEADER_SIZE 100
            char header_text[PPM_HEADER_SIZE];

            if( rgb_file_bits == 8 )
            {
                // 8 bits
                sprintf( header_text, "P6\n%ld %ld\n255\n", rgb_buffer.width, rgb_buffer.height );
            }
            else
            {
                // 16 bits
                sprintf( header_text, "P6\n%ld %ld\n65535\n", rgb_buffer.width, rgb_buffer.height );
            }
            
            output_buffer.size   = rgb_buffer.size + strlen( header_text );
            output_buffer.buffer = allocator.Alloc( output_buffer.size );
            if( !output_buffer.buffer ) { allocator.Free(rgb_buffer.buffer); goto cleanup; }
            char* buffer_c = (char*)output_buffer.buffer;
            
            memcpy( buffer_c, header_text, strlen( header_text ) );
            memcpy( buffer_c + strlen( header_text ), rgb_buffer.buffer, rgb_buffer.size );
#undef PPM_HEADER_SIZE
        }
        else if( output_file_type == FILE_TYPE_JPG )
        {
            write_buffer_to_file = false;
#if GPR_JPEG_AVAILABLE
            tje_encode_to_file( output_file_path, rgb_buffer.width, rgb_buffer.height, 3, rgb_buffer.buffer );
#else
            printf("JPG writing capability is disabled. You could still write to a PPM file");
#endif
        }
        
        allocator.Free( rgb_buffer.buffer );
    }
    else if( input_file_type == FILE_TYPE_GPR && output_file_type == FILE_TYPE_DNG )
    {
        success = gpr_convert_gpr_to_dng( &allocator, &params, &input_buffer, &output_buffer );
    }
    else if( input_file_type == FILE_TYPE_GPR && output_file_type == FILE_TYPE_RAW )
    {
        success = gpr_convert_gpr_to_raw( &allocator, &input_buffer, &output_buffer );
    }
#endif
    else
    {
        printf( "Unsupported conversion from %s to %s \n", input_file_path, output_file_path );
        goto cleanup;
    }

    if( success == 0 )
    {
        printf("Conversion failed \n");
        goto cleanup;
    }
    else if( write_buffer_to_file )
    {
        if( write_to_file( &output_buffer, output_file_path ) != 0 ) goto cleanup;
    }
    result = 0;

cleanup:
    allocator.Free(input_base);
    allocator.Free(output_buffer.buffer);
    
    if( preview.buffer )
    {
        allocator.Free( preview.buffer );
    }
    
    gpr_parameters_destroy(&params, allocator.Free);
    
    return result;
}

