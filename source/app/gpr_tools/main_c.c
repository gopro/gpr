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

#include "gpr.h"

#if defined __GNUC__
#define stricmp strcasecmp
#else
#endif // if defined __GNUC__

#include "main_c.h"
#include "gpr_parse_utils.h"
#include "gpr_print_utils.h"

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

// Map a -x pixel format string to its enum. Returns 1 on match, 0 otherwise.
static int parse_input_pixel_format( const char* s, GPR_PIXEL_FORMAT* out )
{
    if( strcmp(s, "rggb12")  == 0 ) { *out = PIXEL_FORMAT_RGGB_12;  return 1; }
    if( strcmp(s, "rggb12p") == 0 ) { *out = PIXEL_FORMAT_RGGB_12P; return 1; }
    if( strcmp(s, "rggb14")  == 0 ) { *out = PIXEL_FORMAT_RGGB_14;  return 1; }
    if( strcmp(s, "gbrg12")  == 0 ) { *out = PIXEL_FORMAT_GBRG_12;  return 1; }
    if( strcmp(s, "gbrg12p") == 0 ) { *out = PIXEL_FORMAT_GBRG_12P; return 1; }
    if( strcmp(s, "bggr12")  == 0 ) { *out = PIXEL_FORMAT_BGGR_12;  return 1; }
    if( strcmp(s, "bggr14")  == 0 ) { *out = PIXEL_FORMAT_BGGR_14;  return 1; }
    return 0;
}

static bool pixel_format_is_packed(GPR_PIXEL_FORMAT p)
{
    switch( p ) {
        case PIXEL_FORMAT_RGGB_12P:
        case PIXEL_FORMAT_GBRG_12P:
            return true;
        default:
            return false;
    }
}

static unsigned int pixel_format_get_bits(GPR_PIXEL_FORMAT p)
{
    switch( p ) {
        case PIXEL_FORMAT_RGGB_14:
        case PIXEL_FORMAT_BGGR_14:
            return 14;
        default:
            return 12;
    }
}

static GPR_RGB_RESOLUTION parse_resolution(const char* resolution)
{
    if( strcmp(resolution, "") != 0 )
    {
        if( strcmp(resolution, "2:1") == 0 )
            return GPR_RGB_RESOLUTION_HALF;
        else if( strcmp(resolution, "4:1") == 0 )
            return GPR_RGB_RESOLUTION_QUARTER;
        else if( strcmp(resolution, "8:1") == 0 )
            return GPR_RGB_RESOLUTION_EIGHTH;
        else if( strcmp(resolution, "16:1") == 0 )
            return GPR_RGB_RESOLUTION_SIXTEENTH;
        else
            fprintf( stderr, "Unsupported resolution `%s'; using default. Valid values: 2:1, 4:1, 8:1, 16:1\n", resolution );
    } else {
        return GPR_RGB_RESOLUTION_DEFAULT;
    }
}

int dng_convert_main( const dng_convert_params* convert_params )
{
    if( convert_params == NULL )
    {
        printf( "No conversion parameters provided" );
        return -1;
    }

    // Unpack into local working copies. Several of these are mutated below, so we
    // deliberately keep the caller's dng_convert_params untouched.
    const char*  input_file_path        = convert_params->input_file_path;
    size_t       input_skip_rows        = convert_params->input_skip_rows;
    size_t       input_skip_cols        = convert_params->input_skip_cols;
    const char*  input_pixel_format     = convert_params->input_pixel_format;
    const char*  output_file_path       = convert_params->output_file_path;
    const char*  metadata_file_path     = convert_params->metadata_file_path;
    const char*  gpmf_file_path         = convert_params->gpmf_file_path;
    const char*  rgb_file_resolution    = convert_params->rgb_file_resolution;
    int          rgb_file_bits          = convert_params->rgb_file_bits;
    int          jpg_quality            = convert_params->jpg_quality;
    const char*  preview_resolution     = convert_params->preview_resolution;
    const char*  jpg_preview_file_path  = convert_params->jpg_preview_file_path;

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
    params.input_width  = convert_params->input_width;
    params.input_height = convert_params->input_height;
    params.input_pitch  = convert_params->input_pitch;
    
    gpr_buffer input_buffer  = { NULL, 0 };
    
    if( read_from_file( &input_buffer, input_file_path, allocator.Alloc, allocator.Free ) != 0 )
    {
        return -1;
    }
  
    if( metadata_file_path && strcmp(metadata_file_path, "") )
    {
        if( gpr_parameters_parse_json( &params, metadata_file_path ) != 0 )
            return -1;

        // -x is optional here: when absent, keep the pixel format from the metadata file.
        if( strcmp(input_pixel_format, "") != 0 &&
            parse_input_pixel_format(input_pixel_format, &params.tuning_info.pixel_format) == 0 ) {
            fprintf( stderr, "Invalid pixel format %s\n", input_pixel_format);
            return -1;
        }
    }
    else if( input_file_type == FILE_TYPE_GPR || input_file_type == FILE_TYPE_DNG )
    {
        if( gpr_parameters_parse_dng( &allocator, &input_buffer, &params ) == false )
            return -1;

        // -x is optional here: when absent, keep the pixel format read from the DNG/GPR.
        if( strcmp(input_pixel_format, "") != 0 &&
            parse_input_pixel_format(input_pixel_format, &params.tuning_info.pixel_format) == 0 ) {
            fprintf( stderr, "Invalid pixel format %s\n", input_pixel_format);
            return -1;
        }
    }
    else
    {
        // Default RAW pixel format when -x is not specified.
        if( strcmp(input_pixel_format, "") == 0 )
            input_pixel_format = "rggb14";

        if( parse_input_pixel_format(input_pixel_format, &params.tuning_info.pixel_format) == 0 ) {
            fprintf( stderr, "Invalid pixel format %s\n", input_pixel_format);
            return -1;
        }

        if( params.input_pitch <= 0 )
        {
            if( pixel_format_is_packed(params.tuning_info.pixel_format) ) {
                params.input_pitch = (params.input_width * 3 / 4) * 2;
            } else {
                params.input_pitch = params.input_width * 2;
            }
        }
        
        int32_t saturation_level = params.tuning_info.dgain_saturation_level.level_red;
        if( pixel_format_get_bits(params.tuning_info.pixel_format) == 14 )
            saturation_level = (1 << 14) - 1;
        else if( output_file_type == FILE_TYPE_GPR )
            saturation_level = (1 << 14) - 1;
        else if( output_file_type == FILE_TYPE_DNG )
            saturation_level = (1 << 12) - 1;
        else
            saturation_level = 0;
        
        params.tuning_info.dgain_saturation_level.level_red         = saturation_level;
        params.tuning_info.dgain_saturation_level.level_green_even  = saturation_level;
        params.tuning_info.dgain_saturation_level.level_green_odd   = saturation_level;
        params.tuning_info.dgain_saturation_level.level_blue        = saturation_level;
    }
    

    if( gpmf_file_path != NULL && strcmp(gpmf_file_path, "") )
    {
        read_from_file( &params.gpmf_payload, gpmf_file_path, allocator.Alloc, allocator.Free );
    }

    // Resolution of the preview image embedded when writing a GPR/DNG file.
    if( preview_resolution != NULL )
    {
        params.preview_resolution = parse_resolution(preview_resolution);
    }

    gpr_buffer output_buffer = { NULL, 0 };

    // input_skip_rows/cols shift the start of the raw image to adjust its Bayer phase
    // (e.g. BGGR -> GBRG). The shift is applied inside the SDK, right before encoding:
    // for RAW input on the loaded pixel buffer, for DNG input on the decoded raw image
    // (it cannot happen here for DNG, where input_buffer is the whole TIFF container).
    params.input_skip_rows = input_skip_rows;
    params.input_skip_cols = input_skip_cols;

    gpr_buffer preview = { NULL, 0 };

    if( strcmp(jpg_preview_file_path, "") != 0 )
    {
        if( read_from_file( &preview, jpg_preview_file_path, allocator.Alloc, allocator.Free) == 0 )
        {
            // The SDK reads the preview dimensions from the JPEG header itself when embedding it,
            // so the app only needs to hand over the compressed JPEG bytes.
            params.preview_image.jpg_preview = preview;
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
    else if( input_file_type == FILE_TYPE_GPR && output_file_type == FILE_TYPE_PPM )
    {
        GPR_RGB_RESOLUTION rgb_resolution = parse_resolution(rgb_file_resolution);

        // PPM has no metadata channel, so orientation cannot be recorded; the pixels are
        // written in sensor orientation. Warn when that differs from the display orientation.
        if( (int)params.tuning_info.orientation != ORIENTATION_NORMAL )
            fprintf( stderr, "Note: PPM cannot store orientation; output pixels are in sensor "
                             "orientation (not rotated). Use JPG output to preserve orientation.\n" );

        success = gpr_convert_gpr_to_ppm( &allocator, rgb_resolution, rgb_file_bits, &input_buffer, &output_buffer );
    }
    else if( input_file_type == FILE_TYPE_GPR && output_file_type == FILE_TYPE_JPG )
    {
        GPR_RGB_RESOLUTION rgb_resolution = parse_resolution(rgb_file_resolution);

        if( rgb_file_bits == 16 )
            printf( "Asked to output 16-bits RGB, but that is only possible in PPM format.\n");

        // tinyjpeg only supports quality levels 1 (lowest), 2, or 3 (highest)
        if( jpg_quality < 1 || jpg_quality > 3 )
        {
            fprintf( stderr, "JPG quality %d out of range, clamping to [1,3]\n", jpg_quality );
            jpg_quality = jpg_quality < 1 ? 1 : 3;
        }

        success = gpr_convert_gpr_to_jpg( &allocator, rgb_resolution, jpg_quality, &input_buffer, &output_buffer );

        if( success == 0 )
            fprintf( stderr, "Failed to convert GPR to JPG (is JPEG support compiled in?)\n" );
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
#if GPR_WRITING && GPR_READING
    else if( input_file_type == FILE_TYPE_GPR && output_file_type == FILE_TYPE_GPR )
    {
        // Re-encodes from scratch (rather than copying the input bytes), so this can be used to
        // add or refresh the embedded preview/thumbnail on a GPR file, e.g. one that was written
        // without a preview -- see preview_resolution / preview_file_path options.
        success = gpr_convert_gpr_to_gpr( &allocator, &params, &input_buffer, &output_buffer );
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
    
    if( preview.buffer )
    {
        allocator.Free( preview.buffer );
    }
    
    gpr_parameters_destroy(&params, allocator.Free);
    
    return 0;
}

