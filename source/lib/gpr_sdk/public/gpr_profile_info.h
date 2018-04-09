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
     
        uint16_t    illuminant1;
        uint16_t    illuminant2;
        
    } gpr_profile_info;

    void gpr_profile_info_set_defaults(gpr_profile_info* x);

#ifdef __cplusplus
    }
#endif

#endif // GPR_PROFILE_INFO_H
