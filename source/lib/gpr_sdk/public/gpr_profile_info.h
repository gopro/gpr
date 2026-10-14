/*! @file gpr_profile_info.h
 *
 *  @brief Declaration of gpr_profile_info object and associated functions
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

#ifndef GPR_PROFILE_INFO_H
#define GPR_PROFILE_INFO_H

#include "gpr_platform.h"

#ifdef __cplusplus
    extern "C" {
#endif

    typedef double Matrix[3][3];

    typedef struct
    {
        
        bool        compute_color_matrix;
        
        double      matrix_weighting;
        
        double      wb1[3];
        double      wb2[3];
        
        Matrix      cam_to_srgb_1;
        Matrix      cam_to_srgb_2;
        
        Matrix      color_matrix_1;
        Matrix      color_matrix_2;

        Matrix      forward_matrix_1;
        Matrix      forward_matrix_2;
        bool        has_forward_matrix;

        uint16_t    illuminant1;
        uint16_t    illuminant2;

        double      baseline_exposure;
        double      analog_balance[3];

        /* ProfileHueSatMapData — per-hue color correction LUT */
        uint32_t    hue_sat_map_dims[3];    /* [hue, sat, val] divisions */
        float       *hue_sat_map_data1;     /* illuminant 1: dims[0]*dims[1]*dims[2]*3 floats */
        float       *hue_sat_map_data2;     /* illuminant 2 (may be NULL) */
        uint32_t    hue_sat_map_encoding;

        /* ProfileLookTableData — 3D tone-and-color look LUT (the "camera look").
           Adobe-converted Z8/Nikon DNGs ship a ~24×24×40 HSV-delta map here
           that defines the rendered tone curve. Without it, raw decoders
           fall back to a neutral rendering — saturation/hue land in the
           wrong place (Y-PSNR ~17 dB on the gate's smooth-gradient test).
           Plumbed in/out alongside HueSatMap so gpr_tools roundtrips preserve
           the source DNG's color look. */
        uint32_t    look_table_dims[3];     /* [hue, sat, val] divisions */
        float       *look_table_data;       /* dims[0]*dims[1]*dims[2]*3 floats; NULL if absent */
        uint32_t    look_table_encoding;

        /* Tone-rendering metadata. Without these the decoded DNG renders
           ~2× brighter than the source: sips falls back to a generic curve
           and the gate's Y-PSNR collapses to ~17 dB on smooth gradients
           even though the bayer round-trip is 61 dB. */
        uint32_t    tone_curve_count;       /* number of (x,y) pairs */
        float       *tone_curve_data;       /* count * 2 floats; NULL if absent */
        double      baseline_exposure_offset;
        uint32_t    default_black_render;   /* dng_default_black_render_None=0 or Auto=1 */
        bool        has_tone_curve;
        bool        has_baseline_exposure_offset;
        bool        has_default_black_render;

        /* Negative-level render hints. The original gpr_sdk hardcoded
           BaselineNoise=1 and BaselineSharpness=1 on output, and never
           read BayerGreenSplit at all. Plumbing them from source so
           sips' rendering doesn't drift on portrait-content DNGs. */
        double      baseline_noise;
        double      baseline_sharpness;
        uint32_t    bayer_green_split;
        bool        has_baseline_noise;
        bool        has_baseline_sharpness;
        bool        has_bayer_green_split;

    } gpr_profile_info;

    void gpr_profile_info_set_defaults(gpr_profile_info* x);

#ifdef __cplusplus
    }
#endif

#endif // GPR_PROFILE_INFO_H
