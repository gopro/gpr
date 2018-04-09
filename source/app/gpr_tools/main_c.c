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
#include <string.h>
#include <stdbool.h>

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
                     const char*  jpg_preview_file_path, int jpg_preview_file_width, int jpg_preview_file_height )
{
    bool success;
    bool write_buffer_to_file = true;
    
    FILE_TYPE input_file_type  = GetFileType( input_file_path );
    FILE_TYPE output_file_type = GetFileType( output_file_path );
    
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
    
    if( read_from_file( &input_buffer, input_file_path, allocator.Alloc, allocator.Free ) != 0 )
    {
        return -1;
    }
  
    if( metadata_file_path && strcmp(metadata_file_path, "") )
    {
        if( gpr_parameters_parse( &params, metadata_file_path ) != 0 )
            return -1;
    }
    else if( input_file_type == FILE_TYPE_GPR || input_file_type == FILE_TYPE_DNG )
    {
        gpr_parse_metadata( &allocator, &input_buffer, &params );
    }
    else
    {
        params.input_width  = input_width;
        params.input_height = input_height;
        params.input_pitch  = input_pitch;
        
        int32_t saturation_level = params.tuning_info.dgain_saturation_level.level_red;
        
        if( output_file_type == FILE_TYPE_GPR )
            saturation_level = (1 << 14) - 1;
        else if( output_file_type == FILE_TYPE_DNG )
            saturation_level = (1 << 12) - 1;
        
        if( strcmp(input_pixel_format, "rggb12") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_RGGB_12;
            
            if( input_pitch == -1 )
                input_pitch = input_width * 2;
        }
        if( strcmp(input_pixel_format, "rggb12p") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_RGGB_12P;
            
            if( input_pitch == -1 )
                input_pitch = (input_width * 3 / 4) * 2;
        }
        else if( strcmp(input_pixel_format, "rggb14") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_RGGB_14;
            
            saturation_level = (1 << 14) - 1;

            if( input_pitch == -1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "gbrg12") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_GBRG_12;
            
            if( input_pitch == -1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "gbrg12p") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_GBRG_12P;
            
            if( input_pitch == -1 )
                input_pitch = (input_width * 3 / 4) * 2;
        }
        
        params.tuning_info.dgain_saturation_level.level_red         = saturation_level;
        params.tuning_info.dgain_saturation_level.level_green_even  = saturation_level;
        params.tuning_info.dgain_saturation_level.level_green_odd   = saturation_level;
        params.tuning_info.dgain_saturation_level.level_blue        = saturation_level;
    }
    
    if( gpmf_file_path != NULL && strcmp(gpmf_file_path, "") )
    {
        read_from_file( &params.gpmf_payload, gpmf_file_path, allocator.Alloc, allocator.Free );
    }
    
    gpr_buffer output_buffer = { NULL, 0 };

    if( input_skip_rows > 0 )
    {
        input_buffer.buffer = (unsigned char*)(input_buffer.buffer) + (input_skip_rows * input_pitch);
    }
    
    gpr_buffer preview = { NULL, 0 };

    if( strcmp(jpg_preview_file_path, "") != 0 )
    {
        if( read_from_file( &preview, jpg_preview_file_path, allocator.Alloc, allocator.Free) == 0 )
        {
            params.preview_image.jpg_preview    = preview;
            params.preview_image.preview_width  = jpg_preview_file_width;
            params.preview_image.preview_height = jpg_preview_file_height;
        }
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
        success = gpr_convert_dng_to_gpr( &allocator, &params, &input_buffer, &output_buffer );
    }
    else if( input_file_type == FILE_TYPE_RAW && output_file_type == FILE_TYPE_GPR )
    {
        success = gpr_convert_raw_to_gpr( &allocator, &params, &input_buffer, &output_buffer );
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
        return -1;
    }

    if( success == 0 )
    {
        printf("Conversion failed \n");
        return -1;
    }
    else if( write_buffer_to_file )
    {
        write_to_file( &output_buffer, output_file_path );
    }
    
    if( input_skip_rows > 0 )
    {
		input_buffer.buffer = (unsigned char*)(input_buffer.buffer) - (input_skip_rows * input_pitch);
    }
    
    if( preview.buffer )
    {
        allocator.Free( preview.buffer );
    }
    
    gpr_parameters_destroy(&params, allocator.Free);
    
    return 0;
}

