/*! @file gpr_parse_utils.cpp
 *
 *  @brief Parsing utilities for gpr_tools
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

#include "gpr_parse_utils.h"
#include "stdcpp_utils.h"

#include "cJSON.h"

#include "dng_stream.h"
#include "dng_misc_opcodes.h"
#include "dng_gain_map.h"

#define MAX_BUF_SIZE 16000

void parse_gps_info( cJSON* pGpsInfo, gpr_gps_info& exif_info )
{
    exif_info.gps_info_valid = false;
}

void parse_exif_info( cJSON* pExifInfo, gpr_exif_info& exif_info )
{
    cJSON* pJSON = pExifInfo->child;
    
    strcpy( exif_info.camera_make, pJSON->valuestring );
    pJSON = pJSON->next;
    
    strcpy( exif_info.camera_model, pJSON->valuestring );
    pJSON = pJSON->next;
    
    strcpy( exif_info.camera_serial, pJSON->valuestring );
    pJSON = pJSON->next;
    
    strcpy( exif_info.software_version, pJSON->valuestring );
    pJSON = pJSON->next;
    
    strcpy( exif_info.user_comment, pJSON->valuestring );
    pJSON = pJSON->next;
    
    strcpy( exif_info.image_description, pJSON->valuestring );
    pJSON = pJSON->next;
    
    exif_info.exposure_time.numerator   = pJSON->child->valueint;
    exif_info.exposure_time.denominator = pJSON->child->next->valueint;
    pJSON = pJSON->next;
    
    exif_info.f_stop_number.numerator   = pJSON->child->valueint;
    exif_info.f_stop_number.denominator = pJSON->child->next->valueint;
    pJSON = pJSON->next;
    
    exif_info.aperture.numerator        = pJSON->child->valueint;
    exif_info.aperture.denominator      = pJSON->child->next->valueint;
    pJSON = pJSON->next;
    
    exif_info.exposure_program = (gpr_exposure_program)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.iso_speed_rating = pJSON->valueint;
    pJSON = pJSON->next;
    
//    strcpy( exif_info.date_time_original, pJSON->valuestring );
    pJSON = pJSON->next;
    
//    strcpy( exif_info.date_time_digitized, pJSON->valuestring );
    pJSON = pJSON->next;
    
    exif_info.exposure_bias.numerator   = pJSON->child->valueint;
    exif_info.exposure_bias.denominator = pJSON->child->next->valueint;
    pJSON = pJSON->next;
    
    exif_info.light_source              = (gpr_light_source)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.flash                     = (gpr_flash)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.focal_length.numerator    = pJSON->child->valueint;
    exif_info.focal_length.denominator  = pJSON->child->next->valueint;
    pJSON = pJSON->next;
    
    exif_info.sharpness                 = (gpr_sharpness)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.saturation                = pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.gain_control              = (gpr_gain_control)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.contrast                  = (gpr_contrast)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.scene_capture_type        = (gpr_scene_capture_type)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.exposure_mode             = (gpr_exposure_mode)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.focal_length_in_35mm_film = pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.digital_zoom.numerator    = pJSON->child->valueint;
    exif_info.digital_zoom.denominator  = pJSON->child->next->valueint;
    pJSON = pJSON->next;
    
    exif_info.white_balance             = (gpr_white_balance)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.scene_type                = (gpr_scene_type)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.file_source               = (gpr_file_source)pJSON->valueint;
    pJSON = pJSON->next;
    
    exif_info.sensing_method            = (gpr_sensing_method)pJSON->valueint;
    pJSON = pJSON->next;
    
    parse_gps_info( pJSON, exif_info.gps_info );
}

void parse_profile_info( cJSON* pProfileInfo, gpr_profile_info& profile_info )
{
    cJSON* pJSON = pProfileInfo->child;

    profile_info.compute_color_matrix = pJSON->valueint > 0 ? true : false;
    pJSON = pJSON->next;
    
    profile_info.matrix_weighting = pJSON->valuedouble;
    pJSON = pJSON->next;
    
    
    {
        cJSON* child = pJSON->child;

        profile_info.wb1[0] = child->valuedouble;
        child = child->next;
        
        profile_info.wb1[1] = child->valuedouble;
        child = child->next;
        
        profile_info.wb1[2] = child->valuedouble;
        
        pJSON = pJSON->next;        
    }

    {
        cJSON* child = pJSON->child;
        profile_info.wb2[0] = child->valuedouble;
        child = child->next;
        
        profile_info.wb2[1] = child->valuedouble;
        child = child->next;
        
        profile_info.wb2[2] = child->valuedouble;
        
        pJSON = pJSON->next;
    }
    
    {
        cJSON* child = pJSON->child;
        profile_info.cam_to_srgb_1[0][0] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_1[0][1] = child->valuedouble;
        child = child->next;

        profile_info.cam_to_srgb_1[0][2] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_1[1][0] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_1[1][1] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_1[1][2] = child->valuedouble;
        child = child->next;

        profile_info.cam_to_srgb_1[2][0] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_1[2][1] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_1[2][2] = child->valuedouble;
        
        pJSON = pJSON->next;
    }

    {
        cJSON* child = pJSON->child;
        profile_info.cam_to_srgb_2[0][0] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_2[0][1] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_2[0][2] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_2[1][0] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_2[1][1] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_2[1][2] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_2[2][0] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_2[2][1] = child->valuedouble;
        child = child->next;
        
        profile_info.cam_to_srgb_2[2][2] = child->valuedouble;
        
        pJSON = pJSON->next;
    }

    {
        cJSON* child = pJSON->child;
        profile_info.color_matrix_1[0][0] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_1[0][1] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_1[0][2] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_1[1][0] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_1[1][1] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_1[1][2] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_1[2][0] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_1[2][1] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_1[2][2] = child->valuedouble;
        
        pJSON = pJSON->next;
    }
    
    {
        cJSON* child = pJSON->child;
        profile_info.color_matrix_2[0][0] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_2[0][1] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_2[0][2] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_2[1][0] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_2[1][1] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_2[1][2] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_2[2][0] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_2[2][1] = child->valuedouble;
        child = child->next;
        
        profile_info.color_matrix_2[2][2] = child->valuedouble;
        
        pJSON = pJSON->next;
    }
    
    profile_info.illuminant1 = pJSON->valueint;
    pJSON = pJSON->next;

    profile_info.illuminant2 = pJSON->valueint;
}

void parse_tuning_info( cJSON* pTuningInfo, gpr_tuning_info& tuning_info )
{
    cJSON* pJSON = pTuningInfo->child;

    tuning_info.orientation = (GPR_ORIENTATION)pJSON->valueint;
    pJSON = pJSON->next;
    
    {
        cJSON* child = pJSON->child;
        tuning_info.static_black_level.r_black = child->valueint;
        child = child->next;
        
        tuning_info.static_black_level.g_r_black = child->valueint;
        child = child->next;

        tuning_info.static_black_level.g_b_black = child->valueint;
        child = child->next;

        tuning_info.static_black_level.b_black = child->valueint;
        
        pJSON = pJSON->next;
    }
    
    {
        cJSON* child = pJSON->child;
        tuning_info.dgain_saturation_level.level_red = child->valueint;
        child = child->next;
        
        tuning_info.dgain_saturation_level.level_green_even = child->valueint;
        child = child->next;
        
        tuning_info.dgain_saturation_level.level_green_odd = child->valueint;
        child = child->next;
        
        tuning_info.dgain_saturation_level.level_blue = child->valueint;
        
        pJSON = pJSON->next;
    }
    
    {
        cJSON* child = pJSON->child;
        tuning_info.wb_gains.r_gain = (float_t)child->valuedouble;
        child = child->next;
        
        tuning_info.wb_gains.g_gain = (float_t)child->valuedouble;
        child = child->next;

        tuning_info.wb_gains.b_gain = (float_t)child->valuedouble;
        
        pJSON = pJSON->next;
    }
    
    {
        cJSON* child = pJSON->child;
        tuning_info.ae_info.iso_value = child->valueint;
        child = child->next;

        tuning_info.ae_info.shutter_time = child->valueint;
        
        pJSON = pJSON->next;
    }
    
    tuning_info.noise_scale = pJSON->valuedouble;
    pJSON = pJSON->next;

    tuning_info.noise_offset = pJSON->valuedouble;
    pJSON = pJSON->next;

    tuning_info.warp_red_coefficient = pJSON->valuedouble;
    pJSON = pJSON->next;

    tuning_info.warp_blue_coefficient = pJSON->valuedouble;
    pJSON = pJSON->next;

    if( pJSON->child )
    {
        cJSON* size = pJSON->child;
        int buffer_size = size->valueint;
        
        tuning_info.gain_map.size = buffer_size;
        
        cJSON* channel = size->next;
        
        int channel_index = 0;
        while( channel && channel_index < 4 && buffer_size > 0 )
        {
            cJSON* child = channel->child;
            
            int version = child->valueint;
            child = child->next;

            int flags = child->valueint;
            child = child->next;

            int bytes = child->valueint;
            child = child->next;
            
            char gain_map_buffer[MAX_BUF_SIZE];
            
            tuning_info.gain_map.buffers[channel_index] = (char*)malloc( buffer_size );
            
            dng_stream gain_map_stream ( gain_map_buffer, buffer_size );
            
            gain_map_stream.Put_uint32( version );
            gain_map_stream.Put_uint32( flags );
            gain_map_stream.Put_uint32( bytes );
            
            {
                cJSON* _child = child->child;
                dng_rect rect;
                rect.t = _child->valueint;
                _child = _child->next;

                rect.l = _child->valueint;
                _child = _child->next;

                rect.b = _child->valueint;
                _child = _child->next;

                rect.r = _child->valueint;

                dng_area_spec area_spec(rect, 0, 1, 2, 2);
                area_spec.PutData (gain_map_stream);
                
                child = child->next;
            }
            
            
            dng_point points;
            
            {
                cJSON* _child = child->child;
                
                points.h = _child->valueint;
                _child = _child->next;

                points.v = _child->valueint;
                
                child = child->next;
            }

            dng_point_real64 spacing;
            
            {
                cJSON* _child = child->child;
                
                spacing.h = _child->valuedouble;
                _child = _child->next;
                
                spacing.v = _child->valuedouble;
                
                child = child->next;
            }

            dng_point_real64 origin;
            
            {
                cJSON* _child = child->child;
                
                origin.h = _child->valuedouble;
                _child = _child->next;
                
                origin.v = _child->valuedouble;
                
                child = child->next;
            }
            
            dng_gain_map gain_map( gDefaultDNGMemoryAllocator, points, spacing, origin, 1 );

            cJSON* _child = child->child;
            for (int row = 0; row < points.v; row++)
            {
                for (int col = 0; col < points.h; col++)
                {
                    gain_map.Entry (row, col, 0) = (float_t)_child->valuedouble;
                    _child = _child->next;
                }
            }

            gain_map.PutStream( gain_map_stream );
            
            memcpy( tuning_info.gain_map.buffers[channel_index], gain_map_buffer, buffer_size );
            
            channel = channel->next;
            
            channel_index++;
        }
    }
    
    pJSON = pJSON->next;

    tuning_info.pixel_format = (GPR_PIXEL_FORMAT)pJSON->valueint;
}

int gpr_parameters_parse( gpr_parameters* parameters, const char* input_file_path )
{
    gpr_buffer buffer;
    
    if( read_from_file( &buffer, input_file_path, malloc, free) )
    {
        return -2;
    }
    
    const char* return_parse_end;
    
    cJSON* pRoot = cJSON_ParseWithOpts( (const char*)buffer.buffer, &return_parse_end, 0 );
    
    if( pRoot == NULL )
    {
        printf( "Error parsing %s \n", input_file_path );
        printf( "Error: %s", return_parse_end );
        return -1;
    }
    
    cJSON* pJSON = pRoot->child;
    
    parameters->input_width = pJSON->valueint;
    pJSON = pJSON->next;
    
    parameters->input_height = pJSON->valueint;
    pJSON = pJSON->next;
    
    parameters->input_pitch = pJSON->valueint;
    pJSON = pJSON->next;

    parameters->fast_encoding = pJSON->valueint > 0 ? true : false;
    pJSON = pJSON->next;

    parameters->gpmf_payload.size = pJSON->valueint;
    pJSON = pJSON->next;
    
    parse_exif_info( pJSON, parameters->exif_info );
    pJSON = pJSON->next;
    
    parse_profile_info( pJSON, parameters->profile_info );
    pJSON = pJSON->next;
    
    parse_tuning_info( pJSON, parameters->tuning_info );
    
    free( buffer.buffer );
    
    return 0;
}
