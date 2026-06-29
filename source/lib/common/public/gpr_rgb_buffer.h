/*! @file gpr_buffer.h
 *
 *  @brief Declaration of gpr_rgb_buffer object and enums/functions that work on it
 *
 *  @version 1.0.0
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

#ifndef GPR_RGB_BUFFER_H
#define GPR_RGB_BUFFER_H

#include "gpr_allocator.h"
#include "gpr_platform.h"

#ifdef __cplusplus
    extern "C" {
#endif
        
        typedef enum
        {
            GPR_RGB_RESOLUTION_SIXTEENTH = 1,
            GPR_RGB_RESOLUTION_EIGHTH    = 2,
            GPR_RGB_RESOLUTION_QUARTER   = 3,
            GPR_RGB_RESOLUTION_HALF      = 4,
            GPR_RGB_RESOLUTION_FULL      = 5,
            
            GPR_RGB_RESOLUTION_NONE      = 6,

            GPR_RGB_RESOLUTION_DEFAULT   = GPR_RGB_RESOLUTION_QUARTER,
            
        } GPR_RGB_RESOLUTION;


        typedef struct
        {
            int     r_gain_num;
            int     r_gain_pow2_den;

            int     g_gain_num;
            int     g_gain_pow2_den;

            int     b_gain_num;
            int     b_gain_pow2_den;

        } gpr_rgb_gain;
        
        void gpr_rgb_gain_set_defaults(gpr_rgb_gain* x);

        typedef struct
        {
            void*               buffer;                     /* Address to the memory location that this buffer points to */
            
            size_t              size;                       /* Size of the memory block */
            
            size_t              width;
            
            size_t              height;
            
        } gpr_rgb_buffer;

#ifdef __cplusplus
    }
#endif

#endif // GPR_RGB_BUFFER_H
