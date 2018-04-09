/*! @file gpr_exif_info.h
 *
 *  @brief Declaration of gpr_exif_info object and associated functions
 *
 *  GPR API can be invoked by simply including this header file.
 *  This file includes all other header files that are needed.
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

#ifndef GPR_EXIF_INFO
#define GPR_EXIF_INFO

#define CAMERA_MAKE_SIZE        32
#define CAMERA_MODEL_SIZE       32
#define CAMERA_SERIAL_SIZE      32
#define SOFTWARE_VERSION_SIZE   32
#define USER_COMMENT_SIZE       64
#define SATELLITES_USED_SIZE    32
#define SURVEY_DATA_SIZE        32
#define PROCESSING_METHOD_SIZE  32
#define AREA_INFORMATION_SIZE   32
#define IMAGE_DESCRIPTION_SIZE  32

#include "gpr_platform.h"

#ifdef __cplusplus
    extern "C" {
#endif
      
    typedef enum
    {
        gpr_sensing_method_chip_color_area = 2,

    } gpr_sensing_method;

    typedef enum
    {
        gpr_file_source_digital_still = 3,

    } gpr_file_source;

    typedef enum
    {
        gpr_scene_type_directly_photographed = 1,

    } gpr_scene_type;

    typedef enum
    {
        gpr_white_balance_auto = 0,

        gpr_white_balance_manual = 1,

    } gpr_white_balance;

    typedef enum
    {
        gpr_exposure_mode_auto = 0,

        gpr_exposure_mode_manual = 1,

        gpr_exposure_mode_auto_bracket = 2,

    } gpr_exposure_mode;

    typedef enum
    {
        gpr_scene_capture_type_standard = 0,

        gpr_scene_capture_type_landscape = 1,

        gpr_scene_capture_type_portrait = 2,

        gpr_scene_capture_type_night = 3,

    } gpr_scene_capture_type;

    typedef enum
    {
        gpr_contrast_normal = 0,

    } gpr_contrast;

    typedef enum
    {
        gpr_gain_control_normal = 0,

    } gpr_gain_control;

    typedef enum
    {
        gpr_saturation_normal = 0,

    } gpr_saturation;

    typedef enum
    {
        gpr_sharpness_normal = 0,

        gpr_sharpness_soft = 1,

        gpr_sharpness_hard = 2,

    } gpr_sharpness;

    typedef enum
    {
        gpr_flash_not_used = 0,

        gpr_flash_used = 1,

        gpr_flash_not_supported = 32,

    } gpr_flash;

    typedef enum
    {
        gpr_exposure_program_manual_control = 1,

        gpr_exposure_program_normal = 2,

        gpr_exposure_program_aperture_priority = 3,

        gpr_exposure_program_shutter_priority = 4,

        gpr_exposure_program_creative = 5,

        gpr_exposure_program_action = 6,

        gpr_exposure_program_portrait_mode = 7,

        gpr_exposure_program_landscape_mode = 8,

    } gpr_exposure_program;

    typedef enum
    {
        gpr_metering_mode_average = 1,

        gpr_metering_mode_center_weighted_average = 2,

        gpr_metering_mode_spot = 3,

        gpr_metering_mode_multi_spot = 4,

        gpr_metering_mode_multi_segment = 5,

    } gpr_metering_mode;

    typedef enum
    {
        gpr_light_source_auto = 0,

        gpr_light_source_daylight = 1,

        gpr_light_source_fuorescent = 2,

        gpr_light_source_tungsten = 3,

    } gpr_light_source; // See this link for more info: http://www.awaresystems.be/imaging/tiff/tifftags/privateifd/exif/lightsource.html

    /*****************************************************************************/

    typedef struct
    {
        int32_t numerator;		// Numerator

        int32_t denominator;	// Denominator

    } gpr_signed_rational;

    typedef struct
    {
        uint32_t numerator;		// Numerator

        uint32_t denominator;	// Denominator

    } gpr_unsigned_rational;

    typedef struct
    {
        uint32_t year;

        uint32_t month;

        uint32_t day;

        uint32_t hour;

        uint32_t minute;

        uint32_t second;

    } gpr_date_and_time;

    typedef struct
    {
        bool                    gps_info_valid;

        uint32_t                version_id;

        char                    latitude_ref[2];

        gpr_unsigned_rational   latitude[3];

        char                    longitude_ref[2];

        gpr_unsigned_rational   longitude[3];

        uint8_t                 altitude_ref;

        gpr_unsigned_rational   altitude;

        gpr_unsigned_rational   time_stamp[3];

        char                    satellites[SATELLITES_USED_SIZE];

        char                    status[2];

        char                    measure_mode[2];

        gpr_unsigned_rational   dop;

        char                    speed_ref[2];

        gpr_unsigned_rational   speed;

        char                    track_ref[2];

        gpr_unsigned_rational   track;

        char                    img_direction_ref[2];

        gpr_unsigned_rational   img_direction;

        char                    map_datum[SURVEY_DATA_SIZE];

        char                    dest_latitude_ref[2];

        gpr_unsigned_rational   dest_latitude[3];

        char                    dest_longitude_ref[2];

        gpr_unsigned_rational   dest_longitude[3];

        char                    dest_bearing_ref[2];

        gpr_unsigned_rational   dest_bearing;

        char                    dest_distance_ref[2];

        gpr_unsigned_rational   dest_distance;

        char                    processing_method[PROCESSING_METHOD_SIZE];

        char                    area_information[AREA_INFORMATION_SIZE];

        char                    date_stamp[11];

        unsigned short          differential;

    } gpr_gps_info;

    typedef struct
    {
        char                        camera_make[CAMERA_MAKE_SIZE];

        char                        camera_model[CAMERA_MODEL_SIZE];

        char                        camera_serial[CAMERA_SERIAL_SIZE];

        char                        software_version[SOFTWARE_VERSION_SIZE];

        char                        user_comment[USER_COMMENT_SIZE];

        char                        image_description[IMAGE_DESCRIPTION_SIZE];

        gpr_unsigned_rational       exposure_time;                      /* exposure time (as fraction) */

        gpr_unsigned_rational       f_stop_number;                      /* f-stop number (as fraction) */

        gpr_unsigned_rational       aperture;                           /* aperture */

        gpr_exposure_program        exposure_program;

        uint16_t                    iso_speed_rating;

        gpr_date_and_time           date_time_original;

        gpr_date_and_time           date_time_digitized;

        gpr_signed_rational         exposure_bias;

        gpr_metering_mode           metering_mode;

        gpr_light_source            light_source;

        gpr_flash                   flash;

        gpr_unsigned_rational       focal_length;

        gpr_sharpness               sharpness;

        uint16_t                    saturation;

        gpr_gain_control            gain_control;

        gpr_contrast                contrast;

        gpr_scene_capture_type      scene_capture_type;

        gpr_exposure_mode           exposure_mode;

        uint16_t                    focal_length_in_35mm_film;

        gpr_unsigned_rational       digital_zoom;

        gpr_white_balance           white_balance;

        gpr_scene_type              scene_type;

        gpr_file_source             file_source;

        gpr_sensing_method          sensing_method;

        gpr_gps_info                gps_info;

    } gpr_exif_info;

    void gpr_exif_info_set_defaults( gpr_exif_info* x );

    void gpr_exif_info_get_camera_make_and_model( const gpr_exif_info* x, char camera_make_and_model[256] );

    gpr_date_and_time construct_gpr_date_and_time (uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute, uint32_t second);

#ifdef __cplusplus
    }
#endif

#endif // GPR_EXIF_INFO
