/*! @file gpr_print_utils.cpp
 *
 *  @brief Printing utilities for gpr_tools
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

#include "gpr_print_utils.h"

#include <iostream>
#include <fstream>
#include <algorithm>

using namespace std;

#include "dng_stream.h"
#include "dng_misc_opcodes.h"
#include "dng_gain_map.h"

uint32 spaces = 0;

ostream& operator<<(ostream& output, const gpr_signed_rational& x)
{
    output << "[" << x.numerator << "," << x.denominator << "]";
    
    return output;
}

ostream& operator<<(ostream& output, const gpr_unsigned_rational& x)
{
    output << "[" << x.numerator << "," << x.denominator << "]";
    
    return output;
}

ostream& operator<<(ostream& output, const gpr_date_and_time& x)
{
    output << "\"" << x.year << "-" << x.month << "-" << x.day << " " << x.hour << ":" << x.minute << ":" << x.second << "\"";
    
    return output;
}

ostream& operator<<(ostream& output, const dng_area_spec& x)
{
    dng_rect area = x.Area();
    
    output << "{ \"top\" : " << area.t << ", \"left\" : " << area.l << ", \"bottom\" : " << area.b << ", \"right\" : " << area.r << ", \"row_pitch\" : " << x.RowPitch() << ", \"col_pitch\" : " << x.ColPitch() << " }";
    
    return output;
}

ostream& operator<<(ostream& output, const dng_point& x)
{
    output << "{ \"h\" : " << x.h << ", \"v\" : " << x.v << " }";
    
    return output;
}

ostream& operator<<(ostream& output, const dng_point_real64& x)
{
    output << "{ \"h\" : " << x.h << ", \"v\" : " << x.v << " }";
    
    return output;
}

void start_tag( const string& tag_id, ostream& output )
{
    output << "{" << endl;
    spaces += 2;
}

void end_tag( const string& tag_id, ostream& output )
{
    spaces -= 2;
    output << string( spaces, ' ' ).c_str() << "}";
}

template<class T>
void print_val(ostream& output, string tag, T x, int N = 0, bool last = false)
{
    if( last )
        output << string( spaces, ' ' ).c_str() << "\"" << tag.c_str() << "\": " << x << "" << endl;
    else
        output << string( spaces, ' ' ).c_str() << "\"" << tag.c_str() << "\": " << x << "," << endl;
}

template<>
void print_val<const char*>(ostream& output, string tag, const char* x, int N, bool last)
{
    if( last )
        output << string( spaces, ' ' ).c_str() << "\"" << tag.c_str() << "\": " << "\"" << x << "\"" << "" << endl;
    else
        output << string( spaces, ' ' ).c_str() << "\"" << tag.c_str() << "\": " << "\"" << x << "\"" << "," << endl;
}

template<>
void print_val<const double*>(ostream& output, string tag, const double* x, int N, bool last)
{
    output << string( spaces, ' ' ).c_str() << "\"" << tag.c_str() << "\": " << "[";
    
    for (int i = 0; i < N; i++)
    {
        if( i < N - 1 )
            output << x[i] << ",";
        else
            output << x[i];
    }
    
    if( last )
        output << "]" << "" << endl;
    else
        output << "]" << "," << endl;
}

template<>
void print_val<const gpr_unsigned_rational*>(ostream& output, string tag, const gpr_unsigned_rational* x, int N, bool last)
{
    output << string( spaces, ' ' ).c_str() << "\"" << tag.c_str() << "\": " << "[";
    
    for (int i = 0; i < N; i++)
    {
        if( i < N - 1 )
            output << x[i] << ",";
        else
            output << x[i];
    }
    
    if( last )
        output << "]" << "" << endl;
    else
        output << "]" << "," << endl;
}

template<class T, int N>
void print_val(ostream& output, string tag, T x[N][N])
{
    output << string( spaces, ' ' ) << "\"" << tag << "\": " << "[";
    output << x[0][0] << ",";
    output << x[0][1] << ",";
    output << x[0][2] << ",";
    output << x[1][0] << ",";
    output << x[1][1] << ",";
    output << x[1][2] << ",";
    output << x[2][0] << ",";
    output << x[2][1] << ",";
    output << x[2][2];
    output << "]";
    
    output << "," << endl;
}

ostream& operator<<(ostream& output, const gpr_gain_map& x)
{
    start_tag( "gain_map", output );
    
    if( x.size > 0 )
    {
        print_val( output, "size", x.size );
        
        for (int i = 0; i < 4; i++)
        {
            dng_stream gain_map_stream (x.buffers[i], x.size);
            
            output << string( spaces, ' ' ).c_str() << "\"" << "channel_" << i << "\": ";
            start_tag( "channel", output );
            
            print_val( output, "version", gain_map_stream.Get_uint32() );
            
            print_val( output, "flags", gain_map_stream.Get_uint32() );
            
            print_val( output, "bytes", gain_map_stream.Get_uint32() );
            
            dng_area_spec area_spec;
            area_spec.GetData (gain_map_stream);
            
            print_val( output, "area", area_spec );
            
            AutoPtr<dng_gain_map> gain_map;
            
            gain_map.Reset (dng_gain_map::GetStream (gain_map_stream, gDefaultDNGMemoryAllocator));
            
            dng_point points = gain_map.Get()->Points();
            dng_point_real64 spacing = gain_map.Get()->Spacing();
            dng_point_real64 origin = gain_map.Get()->Origin();
            
            print_val( output, "points", points );
            print_val( output, "spacing", spacing );
            print_val( output, "origin", origin );
            
            output << string( spaces, ' ' ).c_str() << "\"" << "values" << "\": [";
            
            for (int row = 0; row < points.v; row++)
            {
                for (int col = 0; col < points.h; col++)
                {
                    output << gain_map->Entry (row, col, 0);
                    
                    if( row == points.v - 1 && col == points.h - 1 )
                        output << " ";
                    else
                        output << ", ";
                }
            }
            output << "] " << endl;
            
            end_tag( "channel", output );
            
            if( i < 3 )
                output << ", " << endl;            
        }
    }
    
    end_tag( "gain_map", output );
    
    return output;
}

ostream& operator<<(ostream& output, const gpr_gps_info& x)
{
    start_tag( "gps_info", output );
    
    if( x.gps_info_valid )
    {
        print_val( output, "gps_info_valid", x.gps_info_valid );
        
        print_val( output, "version_id", x.version_id );
        
        print_val( output, "latitude_ref", x.latitude_ref );

        print_val( output, "latitude", x.latitude, 3 );

        print_val( output, "longitude_ref", x.longitude_ref );

        print_val( output, "longitude", x.longitude, 3 );

        print_val( output, "altitude_ref", (uint32)x.altitude_ref );

        print_val( output, "altitude", x.altitude );

        print_val( output, "time_stamp", x.time_stamp );
        
        print_val( output, "satellites", x.satellites );
        
        print_val( output, "status", x.status );
        
        print_val( output, "dop", x.dop );
        
        print_val( output, "speed_ref", x.speed_ref );
        
        print_val( output, "speed", x.speed );
        
        print_val( output, "track_ref", x.track_ref );
        
        print_val( output, "track", x.track );
        
        print_val( output, "img_direction_ref", x.img_direction_ref );
        
        print_val( output, "img_direction", x.img_direction );
        
        print_val( output, "map_datum", x.map_datum );
        
        print_val( output, "dest_latitude_ref", x.dest_latitude_ref );

        print_val( output, "dest_latitude", x.dest_latitude );

        print_val( output, "dest_longitude_ref", x.dest_longitude_ref );

        print_val( output, "dest_longitude", x.dest_longitude );

        print_val( output, "dest_bearing_ref", x.dest_bearing_ref );

        print_val( output, "dest_bearing", x.dest_bearing );

        print_val( output, "dest_distance_ref", x.dest_distance_ref );

        print_val( output, "dest_distance", x.dest_distance );

        print_val( output, "processing_method", x.processing_method );
        
        print_val( output, "area_information", x.area_information );
        
        print_val( output, "date_stamp", x.date_stamp );
        
        print_val( output, "differential", x.differential, 0, true );
    }
    else
    {
        print_val( output, "gps_info_valid", x.gps_info_valid, 0, true );
    }
    
    end_tag( "gps_info", output );
    
    return output;
}

ostream& operator<<(ostream& output, const gpr_exif_info& x)
{
    start_tag( "exif_info", output );
    
    print_val( output, "camera_make", x.camera_make );

    print_val( output, "camera_model", x.camera_model );

    print_val( output, "camera_serial", x.camera_serial );

    print_val( output, "software_version", x.software_version );

    print_val( output, "user_comment", x.user_comment );

    {
        string str_image_description = x.image_description;
        std::replace( str_image_description.begin(), str_image_description.end(), '\\', '/');
        print_val( output, "image_description", str_image_description.c_str() );
    }

    print_val( output, "exposure_time", x.exposure_time );

    print_val( output, "f_stop_number", x.f_stop_number );
    
    print_val( output, "aperture", x.aperture );
    
    print_val( output, "exposure_program", x.exposure_program );
    
    print_val( output, "iso_speed_rating", x.iso_speed_rating );
    
    print_val( output, "date_time_original", x.date_time_original );
    
    print_val( output, "date_time_digitized", x.date_time_digitized );
    
    print_val( output, "exposure_bias", x.exposure_bias );
    
    print_val( output, "light_source", x.light_source );
    
    print_val( output, "flash", x.flash );
    
    print_val( output, "focal_length", x.focal_length );
    
    print_val( output, "sharpness", x.sharpness );
    
    print_val( output, "saturation", x.saturation );
    
    print_val( output, "gain_control", x.gain_control );
    
    print_val( output, "contrast", x.contrast );
    
    print_val( output, "scene_capture_type", x.scene_capture_type );
    
    print_val( output, "exposure_mode", x.exposure_mode );
    
    print_val( output, "focal_length_in_35mm_film", x.focal_length_in_35mm_film );
    
    print_val( output, "digital_zoom", x.digital_zoom );
    
    print_val( output, "white_balance", x.white_balance );
    
    print_val( output, "scene_type", x.scene_type );
    
    print_val( output, "file_source", x.file_source );
    
    print_val( output, "sensing_method", x.sensing_method );
    
    print_val( output, "gps_info", x.gps_info, 0, true );
    
    end_tag( "exif_info", output );
    
    return output;
}

ostream& operator<<(ostream& output, const gpr_profile_info& x)
{
    start_tag( "profile_info", output );
    
    print_val( output, "compute_color_matrix", x.compute_color_matrix );
    
    print_val( output, "matrix_weighting", x.matrix_weighting );
    
    print_val( output, "wb1", x.wb1, 3, false );
    
    print_val( output, "wb2", x.wb2, 3, false );
    
    print_val( output, "cam_to_srgb_1", (const double*)x.cam_to_srgb_1, 9, false );
    
    print_val( output, "cam_to_srgb_2", (const double*)x.cam_to_srgb_2, 9, false );

    print_val( output, "color_matrix_1", (const double*)x.color_matrix_1, 9, false );

    print_val( output, "color_matrix_2", (const double*)x.color_matrix_2, 9, false );
    
    print_val( output, "illuminant1", x.illuminant1, 0, false );
    
    print_val( output, "illuminant2", x.illuminant2, 0, true );
    
    end_tag( "profile_info", output );
    
    return output;
}

ostream& operator<<(ostream& output, const gpr_static_black_level& x)
{
    start_tag( "static_black_level", output );
    
    print_val( output, "r_black", x.r_black );
    
    print_val( output, "g_r_black", x.g_r_black );
    
    print_val( output, "g_b_black", x.g_b_black );
    
    print_val( output, "b_black", x.b_black, 0, true );
    
    end_tag( "static_black_level", output );
    
    return output;
}

ostream& operator<<(ostream& output, const gpr_saturation_level& x)
{
    start_tag( "dgain_saturation_level", output );

    print_val( output, "level_red", x.level_red );
    
    print_val( output, "level_green_even", x.level_green_even );
    
    print_val( output, "level_green_odd", x.level_green_odd );
    
    print_val( output, "level_blue", x.level_blue, 0, true );
    
    end_tag( "dgain_saturation_level", output );
    
    return output;
}

ostream& operator<<(ostream& output, const gpr_white_balance_gains& x)
{
    start_tag( "wb_gains", output );
    
    print_val( output, "r_gain", x.r_gain );
    
    print_val( output, "g_gain", x.g_gain );
    
    print_val( output, "b_gain", x.b_gain, 0, true );
    
    end_tag( "wb_gains", output );
    
    return output;
}

ostream& operator<<(ostream& output, const gpr_auto_exposure_info& x)
{
    start_tag( "ae_info", output );
    
    print_val( output, "iso_value", x.iso_value );

    print_val( output, "shutter_time", x.shutter_time, 0, true );
    
    end_tag( "ae_info", output );
    
    return output;
}

ostream& operator<<(ostream& output, const gpr_tuning_info& x)
{
    start_tag( "tuning_info", output );

    print_val( output, "orientation", x.orientation );

    print_val( output, "static_black_level", x.static_black_level );

    print_val( output, "dgain_saturation_level", x.dgain_saturation_level );

    print_val( output, "wb_gains", x.wb_gains );

    print_val( output, "ae_info", x.ae_info );

    print_val( output, "noise_scale", x.noise_scale );

    print_val( output, "noise_offset", x.noise_offset );

    print_val( output, "warp_red_coefficient", x.warp_red_coefficient );
    
    print_val( output, "warp_blue_coefficient", x.warp_blue_coefficient );
    
    print_val( output, "gain_map", x.gain_map );
    
    print_val( output, "pixel_format", x.pixel_format, 0, true );
    
    end_tag( "tuning_info", output );

    return output;
}

ostream& operator<<(ostream& output, const gpr_parameters& x)
{
    print_val( output, "input_width", x.input_width );

    print_val( output, "input_height", x.input_height );

    print_val( output, "input_pitch", x.input_pitch );

    print_val( output, "fast_encoding", x.fast_encoding );

    // print_val( output, "gpmf_payload_buffer", x.gpmf_payload.buffer );
    
    print_val( output, "gpmf_payload_size", x.gpmf_payload.size );

    print_val( output, "exif_info", x.exif_info );

    print_val( output, "profile_info", x.profile_info );
    
    print_val( output, "tuning_info", x.tuning_info, 0, true );
    
    return output;
}

int gpr_parameters_print( const gpr_parameters* parameters, const char* output_file_path )
{
    ofstream output;
    ostream* output_ref = &cout;

    if( output_file_path )
    {
        output.open (output_file_path);
        output_ref = &output;
    }

    start_tag( "", *output_ref );
    *output_ref << *parameters;
    end_tag( "", *output_ref );
    
    return 0;
}
