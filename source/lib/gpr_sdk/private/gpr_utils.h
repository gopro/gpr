/*! @file gpr_utils.h
 *
 *  @brief Declaration of general gpr functions that belong in gpr library
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

#ifndef GPR_UTILS_H
#define GPR_UTILS_H

#include "gpr_buffer_auto.h"
#include "dng_image.h"

#ifdef __cplusplus
extern "C" {
#endif

    void CopyRawImageToBuffer( const dng_image& raw_image, gpr_buffer_auto& buffer );

    void CopyBufferToRawImage( const gpr_buffer_auto& buffer, size_t buffer_stride, dng_image& raw_image );

#ifdef __cplusplus
}
#endif

#endif // GPR_UTILS_H
