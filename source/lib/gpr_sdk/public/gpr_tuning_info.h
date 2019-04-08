/*! @file gpr_tuning_info.h
 *
 *  @brief Declaration of gpr_tuning_info object and associated functions
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

#ifndef GPR_TUNING_INFO_H
#define GPR_TUNING_INFO_H

#include "gpr_platform.h"

#ifdef __cplusplus
    extern "C" {
#endif
        
    typedef enum
    {
        RAW_CHANNEL_RED = 0,
        
        RAW_CHANNEL_GREEN_EVEN = 1,
        
        RAW_CHANNEL_GREEN_ODD = 2,
        
        RAW_CHANNEL_BLUE = 3,
        
    } GPR_RAW_CHANNEL;

    typedef enum
    {
        PIXEL_FORMAT_RGGB_12 = 0,                           // RGGB 12bit pixels packed into 16bits

        PIXEL_FORMAT_RGGB_12P,                          // RGGB 12bit pixels packed into 12bits

		PIXEL_FORMAT_RGGB_14,                           // RGGB 14bit pixels packed into 16bits

		PIXEL_FORMAT_RGGB_16,                           // RGGB 16bit pixels packed into 16bits

        PIXEL_FORMAT_GBRG_12,                           // GBRG 12bit pixels packed into 16bits

		PIXEL_FORMAT_GBRG_12P,                          // GBRG 12bit pixels packed into 12bits

		PIXEL_FORMAT_GBRG_14,                           // GBRG 14bit pixels packed into 16bits

		PIXEL_FORMAT_GBRG_16,                           // GBRG 16bit pixels packed into 16bits
        
    } GPR_PIXEL_FORMAT;

    typedef enum
    {
        ORIENTATION_NORMAL      = 0,
        
        ORIENTATION_MIRROR      = 4,
        
        ORIENTATION_DEFAULT     = ORIENTATION_MIRROR,
        
    } GPR_ORIENTATION;

    typedef struct
    {
        int32_t r_black;
        
        int32_t g_r_black;
        
        int32_t g_b_black;
        
        int32_t b_black;
        
    } gpr_static_black_level;

    typedef struct
    {
        uint16_t    iso_value;
        
        uint32_t    shutter_time;
        
    } gpr_auto_exposure_info;

    typedef struct
    {
        int32_t level_red;
        
        int32_t level_green_even;
        
        int32_t level_green_odd;
        
        int32_t level_blue;
        
    } gpr_saturation_level;

    typedef struct
    {
        float_t r_gain;
        
        float_t g_gain;
        
        float_t b_gain;
        
    } gpr_white_balance_gains;

    typedef struct
    {
        char        *buffers[4];
        
        uint32_t    size;
        
    } gpr_gain_map;

    typedef struct
    {
        GPR_ORIENTATION         orientation;
        
        gpr_static_black_level  static_black_level;

        gpr_saturation_level    dgain_saturation_level;
        
        gpr_white_balance_gains wb_gains;
        
        gpr_auto_exposure_info  ae_info;
        
        double                  noise_scale;
        double                  noise_offset;
        
        double                  warp_red_coefficient;
        double                  warp_blue_coefficient;
        
        gpr_gain_map            gain_map;
        
        GPR_PIXEL_FORMAT        pixel_format;

    } gpr_tuning_info;

    int32_t gpr_tuning_info_get_dgain_saturation_level(const gpr_tuning_info* x, GPR_RAW_CHANNEL channel);

    void gpr_tuning_info_set_defaults( gpr_tuning_info* x );

#ifdef __cplusplus
    }
#endif

#endif // GPR_TUNING_INFO_H
