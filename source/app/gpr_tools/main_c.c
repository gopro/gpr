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

// Read the pixel dimensions of a JPEG image from its header, without decoding it.
// Walks the marker segments until the Start-Of-Frame (SOF) marker, which carries the
// height and width. Returns 1 and fills *width/*height on success, 0 otherwise.
static int jpeg_get_dimensions( const unsigned char* d, size_t n, int* width, int* height )
{
    size_t i;

    if( d == NULL || n < 4 || d[0] != 0xFF || d[1] != 0xD8 ) // SOI
        return 0;

    i = 2;
    while( i + 1 < n )
    {
        unsigned char marker;
        int seg_len;
        bool is_sof;

        if( d[i] != 0xFF ) { i++; continue; }    // skip fill bytes until a marker
        marker = d[i + 1];
        i += 2;

        // Standalone markers carry no length: SOI, EOI, TEM and the restart markers.
        if( marker == 0xD8 || marker == 0xD9 || marker == 0x01 || ( marker >= 0xD0 && marker <= 0xD7 ) )
            continue;

        if( i + 2 > n )
            break;

        seg_len = ( d[i] << 8 ) | d[i + 1];      // length includes these 2 bytes
        if( seg_len < 2 )
            break;

        // SOF markers (0xC0-0xCF) carry the frame dimensions, except DHT(C4), JPG(C8), DAC(CC).
        is_sof = ( marker >= 0xC0 && marker <= 0xCF ) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if( is_sof )
        {
            if( i + 7 > n )                      // length(2) precision(1) height(2) width(2)
                break;

            *height = ( d[i + 3] << 8 ) | d[i + 4];
            *width  = ( d[i + 5] << 8 ) | d[i + 6];
            return ( *width > 0 && *height > 0 ) ? 1 : 0;
        }

        i += seg_len;                            // skip this segment
    }

    return 0;
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

#if GPR_JPEG_AVAILABLE

// Convert a DNG/Adobe orientation (0-7) to its EXIF/TIFF Orientation value (1-8).
static int adobe_orientation_to_exif( int adobe_orientation )
{
    // Indexed by the Adobe orientation enum (kNormal=0 .. kMirror90CCW=7).
    static const int exif[8] = { 1, 6, 3, 8, 2, 7, 4, 5 };
    return ( adobe_orientation >= 0 && adobe_orientation < 8 ) ? exif[adobe_orientation] : 1;
}

// Collects the JPEG bytes produced by tje_encode_with_func into a growable buffer.
typedef struct
{
    unsigned char*  data;
    size_t          size;
    size_t          capacity;

} jpg_mem_buffer;

static void jpg_mem_sink( void* context, void* data, int size )
{
    jpg_mem_buffer* buf = (jpg_mem_buffer*)context;

    if( buf->size + (size_t)size > buf->capacity )
    {
        size_t new_capacity = buf->capacity ? buf->capacity : ( 1 << 20 );
        while( new_capacity < buf->size + (size_t)size )
            new_capacity *= 2;

        buf->data = (unsigned char*)realloc( buf->data, new_capacity );
        buf->capacity = new_capacity;
    }

    memcpy( buf->data + buf->size, data, (size_t)size );
    buf->size += (size_t)size;
}

// Encode interleaved RGB to a JPEG file, embedding an EXIF Orientation tag so viewers
// display it the right way up without the pixels being physically rotated. Returns 1 on
// success, 0 on failure.
static int write_jpg_with_exif_orientation( const char* output_file_path, int quality,
                                            int width, int height, const unsigned char* rgb,
                                            int exif_orientation )
{
    // Minimal EXIF APP1 segment (little-endian TIFF) with two IFD0 tags in ascending order:
    // Orientation (0x0112) and YCbCrPositioning (0x0213, required for a conformant JPEG EXIF).
    unsigned char app1[] =
    {
        0xFF, 0xE1, 0x00, 0x2E,                          // APP1 marker, segment length = 46
        'E',  'x',  'i',  'f',  0x00, 0x00,              // "Exif\0\0"
        'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,  // TIFF header (LE), IFD0 at offset 8
        0x02, 0x00,                                      // IFD0: 2 entries
        0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,  // tag 0x0112 Orientation, SHORT, count 1
        0x00, 0x00, 0x00, 0x00,                          // Orientation value (set below) + padding
        0x13, 0x02, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,  // tag 0x0213 YCbCrPositioning, SHORT, count 1
        0x01, 0x00, 0x00, 0x00,                          // YCbCrPositioning value = 1 (centered) + padding
        0x00, 0x00, 0x00, 0x00                           // next IFD offset = 0
    };

    jpg_mem_buffer jpg = { NULL, 0, 0 };
    FILE* f;
    int ok = 1;

    app1[28] = (unsigned char)( exif_orientation & 0xFF ); // Orientation SHORT (low byte)
    app1[29] = 0x00;

    if( tje_encode_with_func( jpg_mem_sink, &jpg, quality, width, height, 3, rgb ) == 0 )
    {
        free( jpg.data );
        return 0;
    }

    f = fopen( output_file_path, "wb" );
    if( f == NULL )
    {
        free( jpg.data );
        return 0;
    }

    // Insert the EXIF segment right after the SOI marker (FFD8) so it is the first segment.
    if( jpg.size >= 2 && jpg.data[0] == 0xFF && jpg.data[1] == 0xD8 )
    {
        ok = ok && ( fwrite( jpg.data, 1, 2, f ) == 2 );
        ok = ok && ( fwrite( app1, 1, sizeof(app1), f ) == sizeof(app1) );
        ok = ok && ( fwrite( jpg.data + 2, 1, jpg.size - 2, f ) == jpg.size - 2 );
    }
    else
    {
        ok = ok && ( fwrite( jpg.data, 1, jpg.size, f ) == jpg.size );
    }

    fclose( f );
    free( jpg.data );

    return ok;
}

#endif // GPR_JPEG_AVAILABLE

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
    unsigned int input_width            = convert_params->input_width;
    unsigned int input_height           = convert_params->input_height;
    size_t       input_pitch            = convert_params->input_pitch;
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
    
    gpr_buffer input_buffer  = { NULL, 0 };
    
    if( read_from_file( &input_buffer, input_file_path, allocator.Alloc, allocator.Free ) != 0 )
    {
        return -1;
    }
  
    if( metadata_file_path && strcmp(metadata_file_path, "") )
    {
        if( gpr_parameters_parse( &params, metadata_file_path ) != 0 )
            return -1;

        // An explicit -x overrides the Bayer pattern carried in the metadata file.
        // This matters for RAW input, where the metadata may have been dumped from a
        // file whose detected pixel_format does not match the raw's actual layout.
        if( input_file_type == FILE_TYPE_RAW && strcmp(input_pixel_format, "") != 0 )
        {
            GPR_PIXEL_FORMAT pf;
            if( parse_input_pixel_format(input_pixel_format, &pf) )
                params.tuning_info.pixel_format = pf;
        }

        // Explicit dimensions on the command line override those from the metadata file.
        // Relevant for RAW input, where -w/-h/-p define how the raw bytes are interpreted.
        if( input_file_type == FILE_TYPE_RAW )
        {
            if( input_width  != 0 ) params.input_width  = input_width;
            if( input_height != 0 ) params.input_height = input_height;
            if( input_pitch  != 0 ) params.input_pitch  = input_pitch;
        }
    }
    else if( input_file_type == FILE_TYPE_GPR || input_file_type == FILE_TYPE_DNG )
    {
        if( gpr_parse_metadata( &allocator, &input_buffer, &params ) == false )
            return -1;
    }
    else
    {
        // 0 means "not specified on the command line"; fall back to legacy defaults.
        params.input_width  = ( input_width  != 0 ) ? input_width  : 4000;
        params.input_height = ( input_height != 0 ) ? input_height : 3000;
        params.input_pitch  = ( input_pitch  != 0 ) ? input_pitch  : params.input_width * 2;

        // Default RAW pixel format when -x is not specified.
        if( strcmp(input_pixel_format, "") == 0 )
            input_pixel_format = "rggb14";

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
        else if( strcmp(input_pixel_format, "bggr12") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_BGGR_12;

            if( input_pitch == -1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "bggr14") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_BGGR_14;

            saturation_level = (1 << 14) - 1;

            if( input_pitch == -1 )
                input_pitch = input_width * 2;
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

    // Resolution of the preview image embedded when writing a GPR/DNG file.
    if( preview_resolution != NULL )
    {
        params.preview_resolution = parse_resolution(preview_resolution);
    }

    gpr_buffer output_buffer = { NULL, 0 };

    // This only makes sense for FILE_TYPE_RAW input, where input_buffer is a headerless pixel dump; for DNG/GPR input,
    //  input_buffer is the whole file (TIFF header + IFDs + pixel data), and shifting it corrupts the container before it can even be parsed.
    if( input_file_type == FILE_TYPE_RAW )
    {
        // input_skip_rows/cols shift the start of the raw pixel buffer to adjust the Bayer phase
        // (e.g. BGGR -> GBRG).
        if( input_skip_rows > 0 )
        {
            input_buffer.buffer = (unsigned char*)(input_buffer.buffer) + (input_skip_rows * input_pitch);
        }

        // Skipping columns shifts the horizontal Bayer phase (e.g. BGGR -> GBRG).
        // Each pixel sample is 16 bits, so advance by 2 bytes per column.
        if( input_skip_cols > 0 )
        {
            input_buffer.buffer = (unsigned char*)(input_buffer.buffer) + (input_skip_cols * sizeof(uint16_t));
        }
    }

    gpr_buffer preview = { NULL, 0 };

    if( strcmp(jpg_preview_file_path, "") != 0 )
    {
        if( read_from_file( &preview, jpg_preview_file_path, allocator.Alloc, allocator.Free) == 0 )
        {
            // Read the preview dimensions straight from the JPEG header (no decode needed).
            int jpg_width = 0, jpg_height = 0;

            if( jpeg_get_dimensions( (const unsigned char*)preview.buffer, preview.size, &jpg_width, &jpg_height ) == 0 )
            {
                fprintf( stderr, "Warning: could not read preview dimensions from `%s'; preview may be malformed.\n", jpg_preview_file_path );
            }

            params.preview_image.jpg_preview    = preview;
            params.preview_image.preview_width  = jpg_width;
            params.preview_image.preview_height = jpg_height;
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

        GPR_RGB_RESOLUTION rgb_resolution = parse_resolution(rgb_file_resolution);
        
        if( output_file_type == FILE_TYPE_JPG && rgb_file_bits == 16 )
        {
            printf( "Asked to output 16-bits RGB, but that is only possible in PPM format.\n");
            rgb_file_bits = 8;
        }
            
        success = gpr_convert_gpr_to_rgb( &allocator, rgb_resolution, rgb_file_bits,  &input_buffer, &rgb_buffer );
        
        if( output_file_type == FILE_TYPE_PPM )
        {
            // PPM has no metadata channel, so orientation cannot be recorded; the pixels are
            // written in sensor orientation. Warn when that differs from the display orientation.
            if( (int)params.tuning_info.orientation != ORIENTATION_NORMAL )
                fprintf( stderr, "Note: PPM cannot store orientation; output pixels are in sensor "
                                 "orientation (not rotated). Use JPG output to preserve orientation.\n" );

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
            // tinyjpeg only supports quality levels 1 (lowest), 2, or 3 (highest)
            if( jpg_quality < 1 || jpg_quality > 3 )
            {
                fprintf( stderr, "JPG quality %d out of range, clamping to [1,3]\n", jpg_quality );
                jpg_quality = jpg_quality < 1 ? 1 : 3;
            }

            // The GPR/DNG store the image as a sensor buffer plus an orientation flag; carry the
            // same hint into the JPG via an EXIF Orientation tag rather than rotating the pixels.
            int exif_orientation = adobe_orientation_to_exif( (int)params.tuning_info.orientation );

            if( write_jpg_with_exif_orientation( output_file_path, jpg_quality, rgb_buffer.width, rgb_buffer.height, (const unsigned char*)rgb_buffer.buffer, exif_orientation ) == 0 )
            {
                fprintf( stderr, "Failed to write JPG file %s\n", output_file_path );
                success = false;
            }
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
    
    if( input_file_type == FILE_TYPE_RAW )
    {
        if( input_skip_rows > 0 )
        {
            input_buffer.buffer = (unsigned char*)(input_buffer.buffer) - (input_skip_rows * input_pitch);
        }

        if( input_skip_cols > 0 )
        {
            input_buffer.buffer = (unsigned char*)(input_buffer.buffer) - (input_skip_cols * sizeof(uint16_t));
        }
    }
    
    
    if( preview.buffer )
    {
        allocator.Free( preview.buffer );
    }
    
    gpr_parameters_destroy(&params, allocator.Free);
    
    return 0;
}

