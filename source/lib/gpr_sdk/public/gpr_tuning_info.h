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

        PIXEL_FORMAT_RGGB_12P = 1,                          // RGGB 12bit pixels packed into 12bits
        
        PIXEL_FORMAT_RGGB_14 = 2,                           // RGGB 14bit pixels packed into 16bits
        
        PIXEL_FORMAT_GBRG_12 = 3,                           // GBRG 12bit pixels packed into 16bits
        
        PIXEL_FORMAT_GBRG_12P = 4,                          // GBRG 12bit pixels packed into 12bits

        PIXEL_FORMAT_BGGR_12 = 5,                           // BGGR 12bit pixels packed into 16bits

        PIXEL_FORMAT_BGGR_14 = 6,                           // BGGR 14bit pixels packed into 16bits

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

#define GPR_WARP_MAX_PLANES 3

    // Full-fidelity representation of a DNG OpcodeList3 WarpRectilinear opcode:
    // r_src = r * (k0 + k1*r^2 + k2*r^4 + k3*r^6) + tangential terms, with r
    // normalized by the distance from center to the farthest corner (DNG spec).
    // Camera-original GoPro warps carry only per-plane k0 (chromatic aberration
    // scaling); synthesized lens-distortion profiles populate the higher terms.
    typedef struct
    {
        uint32_t    planes;                             // 0 = no warp present; 1 or 3 otherwise

        uint32_t    flags;                              // DNG opcode flags (1 = optional, 2 = skip for preview)

        double      center_x;                           // optical center, normalized [0..1]
        double      center_y;

        double      radial[GPR_WARP_MAX_PLANES][4];     // k0..k3 per plane

        double      tangential[GPR_WARP_MAX_PLANES][2];

    } gpr_warp_rectilinear;

    // Describes how the (possibly larger) raw buffer relates to the final visible image, as
    // carried by the DNG ActiveArea / DefaultCropOrigin / DefaultCropSize tags. The raw buffer
    // returned by e.g. gpr_convert_dng_to_raw is always the full, uncropped sensor readout --
    // (active_area_right - active_area_left) x (active_area_bottom - active_area_top); the
    // final image is the default_crop_size_h x default_crop_size_v region starting at
    // (active_area_left + default_crop_origin_h, active_area_top + default_crop_origin_v).
    // All fields are 0 when the source had no crop tags (e.g. RAW input with no metadata).
    typedef struct
    {
        int32_t     active_area_top;
        int32_t     active_area_left;
        int32_t     active_area_bottom;
        int32_t     active_area_right;

        uint32_t    default_crop_origin_h;
        uint32_t    default_crop_origin_v;

        uint32_t    default_crop_size_h;
        uint32_t    default_crop_size_v;

    } gpr_crop_info;

    typedef struct
    {
        GPR_ORIENTATION         orientation;
        
        gpr_static_black_level  static_black_level;

        gpr_saturation_level    dgain_saturation_level;
        
        gpr_white_balance_gains wb_gains;
        
        gpr_auto_exposure_info  ae_info;
        
        double                  noise_scale;
        double                  noise_offset;
        
        gpr_warp_rectilinear    warp;

        gpr_gain_map            gain_map;

        GPR_PIXEL_FORMAT        pixel_format;

        double                  baseline_exposure;      // DNG BaselineExposure (EV); brightening applied at render time

        bool                    has_opcode_gain_maps;   // Source carries GainMap opcodes (per-area gains, e.g. lens shading).
                                                        // BaselineExposure is calibrated assuming they are applied, so a
                                                        // renderer that skips them (this SDK does) should skip it too.

        double                  baseline_sharpness;     // DNG BaselineSharpness (default 1.0)

        double                  baseline_noise;         // DNG BaselineNoise (default 1.0)

        gpr_crop_info           crop_info;              // Active area / crop relating the raw buffer to the final image

    } gpr_tuning_info;

    int32_t gpr_tuning_info_get_dgain_saturation_level(const gpr_tuning_info* x, GPR_RAW_CHANNEL channel);

    void gpr_tuning_info_set_defaults( gpr_tuning_info* x );

    int gpr_warp_rectilinear_is_valid( const gpr_warp_rectilinear* x );

    // True when the warp only scales each plane linearly (all higher-order radial and
    // tangential terms zero) -- i.e. chromatic aberration correction, no geometric distortion.
    int gpr_warp_rectilinear_is_ca_only( const gpr_warp_rectilinear* x );

#ifdef __cplusplus
    }
#endif

#endif // GPR_TUNING_INFO_H
