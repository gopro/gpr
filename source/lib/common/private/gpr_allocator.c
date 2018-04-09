/*! @file gpr_allocator.c
 *
 *  @brief Implementation of the global memory allocation functions.
 *  In order to customize memory allocation and deallocation, applications
 *  can implement these functions in additional files and include in
 *  build process
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

#include "gpr_allocator.h"

/*!
 @brief Allocate a block with the specified size
 */
void* gpr_global_malloc(size_t size)
{
    return malloc(size);
}

/*!
 @brief Free a block that was allocated by the specified allocator
 
 It is an error to free a block allocated by one allocator using a
 different allocator.
 */
void gpr_global_free(void *block)
{
    free(block);
}

