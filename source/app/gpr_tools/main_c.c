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
#ifndef _WIN32
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "gpr.h"

#ifdef GPR_PI_PROFILE
#include <time.h>
static inline double pi_prof_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#define PI_PROF_TICK(var) double var = pi_prof_now_ms()
#define PI_PROF_LOG(label, var) fprintf(stderr, "[PI_PROF] %-28s %8.2f ms\n", label, pi_prof_now_ms() - var)
#else
#define PI_PROF_TICK(var) ((void)0)
#define PI_PROF_LOG(label, var) ((void)0)
#endif

#if defined __GNUC__
#define stricmp strcasecmp
#elif defined _WIN32
#define stricmp _stricmp
#endif

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
                     const char*  jpg_preview_file_path, int jpg_preview_file_width, int jpg_preview_file_height, int quality,
                     bool denoise_enabled, bool denoise_auto, double denoise_strength, bool variance_stabilize, bool denoise_output,
                     bool noise_replace, const char* fpn_calibration_path, bool ans_enabled, bool embedded_mode )
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

    PI_PROF_TICK(t_total);
    PI_PROF_TICK(t_setup);

    gpr_allocator allocator;
    allocator.Alloc = malloc;
    allocator.Free = free;

    gpr_parameters params;
    gpr_parameters_set_defaults(&params);
    params.quality = quality;
    params.tuning_info.denoise_enabled = denoise_enabled;
    params.tuning_info.denoise_auto = denoise_auto;
    params.tuning_info.denoise_strength = denoise_strength;
    params.tuning_info.variance_stabilize = variance_stabilize;
    params.tuning_info.denoise_output = denoise_output;
    params.tuning_info.noise_replace = noise_replace;
    params.tuning_info.ans_enabled = ans_enabled;
    params.tuning_info.embedded_mode = embedded_mode;
    if (embedded_mode)
        fprintf(stderr, "  Embedded mode: single-thread, no parallel pre-encode\n");

    if (fpn_calibration_path && fpn_calibration_path[0] != '\0')
    {
        if (fpn_model_load(&params.fpn, fpn_calibration_path) == 0)
            fprintf(stderr, "  Loaded FPN calibration: %s\n", fpn_calibration_path);
        else
            fprintf(stderr, "  Warning: Failed to load FPN calibration: %s\n", fpn_calibration_path);
    }

    gpr_buffer input_buffer  = { NULL, 0 };
    int input_mmap_fd = -1;

#ifndef _WIN32
    /* Fast input: mmap the file for zero-copy access */
    {
        struct stat st;
        input_mmap_fd = open(input_file_path, O_RDONLY);
        if (input_mmap_fd >= 0 && fstat(input_mmap_fd, &st) == 0 && st.st_size > 0) {
            void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, input_mmap_fd, 0);
            if (map != MAP_FAILED) {
                input_buffer.buffer = map;
                input_buffer.size = st.st_size;
            } else {
                close(input_mmap_fd);
                input_mmap_fd = -1;
            }
        } else {
            if (input_mmap_fd >= 0) close(input_mmap_fd);
            input_mmap_fd = -1;
        }
    }
    if (input_mmap_fd < 0)
#endif
    {
        if( read_from_file( &input_buffer, input_file_path, allocator.Alloc, allocator.Free ) != 0 )
        {
            return -1;
        }
    }
    PI_PROF_LOG("read input", t_setup);
    PI_PROF_TICK(t_meta);

    if( metadata_file_path && strcmp(metadata_file_path, "") )
    {
        if( gpr_parameters_parse( &params, metadata_file_path ) != 0 )
            return -1;
    }
    else if( input_file_type == FILE_TYPE_GPR || input_file_type == FILE_TYPE_DNG )
    {
        /* Skip expensive DNG metadata parsing in two cases:
         * (1) GPR/DNG → RAW: no metadata needed for the raw output.
         * (2) DNG → GPR with input_skip_rows == 0: gpr_convert_dng_to_gpr
         *     internally calls read_dng which extracts the same metadata
         *     into its own params_with_meta copy. The outer params is
         *     only used between this call and the convert when
         *     input_skip_rows > 0 (which reads params.input_pitch) or
         *     when an external preview/gpmf is supplied. On Pi 5
         *     Cortex-A76 the redundant read_dng is ~600 ms for a 50 MP
         *     image (38% of total wall-time on a q=3 encode). */
        const int is_dng_to_gpr = (input_file_type == FILE_TYPE_DNG) &&
                                  (output_file_type == FILE_TYPE_GPR);
        const int skip_md = (output_file_type == FILE_TYPE_RAW && input_file_type != FILE_TYPE_GPR) ||
                            (is_dng_to_gpr && input_skip_rows == 0);
        if (!skip_md)
            gpr_parse_metadata( &allocator, &input_buffer, &params );
    }
    else
    {
        params.input_width  = input_width;
        params.input_height = input_height;

        int32_t saturation_level = params.tuning_info.dgain_saturation_level.level_red;

        if( output_file_type == FILE_TYPE_GPR )
            saturation_level = (1 << 14) - 1;
        else if( output_file_type == FILE_TYPE_DNG )
            saturation_level = (1 << 12) - 1;

        if( strcmp(input_pixel_format, "rggb12") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_RGGB_12;

            saturation_level = (1 << 12) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        if( strcmp(input_pixel_format, "rggb12p") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_RGGB_12P;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = (input_width * 3 / 4) * 2;
        }
        else if( strcmp(input_pixel_format, "rggb14") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_RGGB_14;

            saturation_level = (1 << 14) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "rggb16") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_RGGB_16;

            saturation_level = (1 << 16) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "gbrg12") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_GBRG_12;

            saturation_level = (1 << 12) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "gbrg12p") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_GBRG_12P;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = (input_width * 3 / 4) * 2;
        }
        else if( strcmp(input_pixel_format, "gbrg14") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_GBRG_14;

            saturation_level = (1 << 14) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "gbrg16") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_GBRG_16;

            saturation_level = (1 << 16) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "grbg12") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_GRBG_12;

            saturation_level = (1 << 12) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "grbg14") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_GRBG_14;

            saturation_level = (1 << 14) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "grbg16") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_GRBG_16;

            saturation_level = (1 << 16) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "bggr12") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_BGGR_12;

            saturation_level = (1 << 12) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "bggr14") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_BGGR_14;

            saturation_level = (1 << 14) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }
        else if( strcmp(input_pixel_format, "bggr16") == 0 )
        {
            params.tuning_info.pixel_format = PIXEL_FORMAT_BGGR_16;

            saturation_level = (1 << 16) - 1;

            if( input_pitch == 0 || input_pitch == (size_t)-1 )
                input_pitch = input_width * 2;
        }

        params.input_pitch  = input_pitch;

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
        PI_PROF_LOG("setup (metadata+params)", t_meta);
        PI_PROF_TICK(t_conv);
        success = gpr_convert_dng_to_gpr( &allocator, &params, &input_buffer, &output_buffer );
        PI_PROF_LOG("gpr_convert_dng_to_gpr", t_conv);
    }
    else if( input_file_type == FILE_TYPE_RAW && output_file_type == FILE_TYPE_GPR )
    {
        const char *fast_raw_to_gpr = getenv("GPR_FAST_RAW_TO_GPR");
        if( fast_raw_to_gpr != NULL && strcmp(fast_raw_to_gpr, "1") == 0 )
        {
            fprintf(stderr, "GPR_FAST_RAW_TO_GPR is retired; using the metadata-preserving SDK writer.\n");
        }
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
        /* Try fast GPR decode first (bypasses DNG SDK, ~10x faster) */
        {
            extern int gpr_fast_decode(const uint8_t *gpr_data, size_t gpr_size,
                                        void **raw_output, size_t *raw_size,
                                        int pixel_format);

            void *raw_out = NULL;
            size_t raw_sz = 0;
            int pf = 1; /* Fast decoder enum default: RGGB_14 */
            switch (params.tuning_info.pixel_format)
            {
                case PIXEL_FORMAT_RGGB_12:
                case PIXEL_FORMAT_RGGB_12P:
                    pf = 0;
                    break;
                case PIXEL_FORMAT_RGGB_14:
                    pf = 1;
                    break;
                case PIXEL_FORMAT_GBRG_12:
                case PIXEL_FORMAT_GBRG_12P:
                    pf = 2;
                    break;
                case PIXEL_FORMAT_GBRG_14:
                    pf = 3;
                    break;
                case PIXEL_FORMAT_GBRG_16:
                    pf = 5;
                    break;
                case PIXEL_FORMAT_GRBG_12:
                case PIXEL_FORMAT_GRBG_14:
                case PIXEL_FORMAT_GRBG_16:
                case PIXEL_FORMAT_BGGR_12:
                case PIXEL_FORMAT_BGGR_14:
                case PIXEL_FORMAT_BGGR_16:
                    pf = -1;
                    break;
                case PIXEL_FORMAT_RGGB_16:
                    pf = 4;
                    break;
                default:
                    pf = 1;
                    break;
            }
            int rc = pf >= 0
                ? gpr_fast_decode((const uint8_t *)input_buffer.buffer, input_buffer.size,
                                  &raw_out, &raw_sz, pf)
                : -1;
            if (rc == 0 && raw_out) {
                output_buffer.buffer = raw_out;
                output_buffer.size = raw_sz;
                success = 1;
            } else {
                /* Fallback to DNG SDK path */
                if (params.fpn.valid || noise_replace)
                    success = gpr_convert_gpr_to_raw_ex( &allocator, &params, &input_buffer, &output_buffer );
                else
                    success = gpr_convert_gpr_to_raw( &allocator, &input_buffer, &output_buffer );
            }
        }
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
        PI_PROF_TICK(t_write);
        write_to_file( &output_buffer, output_file_path );
        PI_PROF_LOG("write output", t_write);
    }
    PI_PROF_LOG("TOTAL", t_total);
    
    if( input_skip_rows > 0 )
    {
		input_buffer.buffer = (unsigned char*)(input_buffer.buffer) - (input_skip_rows * input_pitch);
    }
    
    if( preview.buffer )
    {
        allocator.Free( preview.buffer );
    }
    
    gpr_parameters_destroy(&params, allocator.Free);

#ifndef _WIN32
    if (input_mmap_fd >= 0) {
        munmap(input_buffer.buffer, input_buffer.size);
        close(input_mmap_fd);
    }
#endif

    return 0;
}
