/*! @file gpr_buffer.h
 *
 *  @brief Declaration of gpr_buffer object and functions that work on buffer
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

#ifndef GPR_BUFFER_H
#define GPR_BUFFER_H

#include "gpr_allocator.h"
#include "gpr_platform.h"

#ifdef __cplusplus
    extern "C" {
#endif
        
        typedef struct
        {
            void*               buffer;                     /* Address to the memory location that this buffer points to */
            
            size_t              size;                       /* Size of the memory block */
            
        } gpr_buffer;

        int read_from_file(gpr_buffer* buffer, const char* file_path, gpr_malloc malloc_function, gpr_free free_function);

        int write_to_file(const gpr_buffer* buffer, const char* file_path);

#ifdef __cplusplus
    }
#endif

#endif // GPR_BUFFER_H
