/*! @file gpr_rgb.h
 *
 *  @brief Implementation of top level functions that implement GPR-SDK API
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

#ifndef GPR_RGB_H
#define GPR_RGB_H

#include "gpr_platform.h"
#include "gpr_exif_info.h"
#include "gpr_profile_info.h"
#include "gpr_tuning_info.h"
#include "gpr_allocator.h"
#include "gpr_buffer.h"
#include "gpr_rgb_buffer.h"

#ifdef __cplusplus
    extern "C" {
#endif

    void compute_xyz_to_camera_color_matrix( double in_matrix[3][3], double wb[3], double weight, double out_matrix[3][3] );
    
    bool compute_camera_to_srgb_color_matrix( const gpr_tuning_info* tuning_info, const gpr_profile_info* profile_info, float out_matrix[3][3] );

#ifdef __cplusplus
    }
#endif

#endif // GPR_RGB_H
