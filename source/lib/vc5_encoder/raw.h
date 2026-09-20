/*! @file raw.h
 *
 *  @brief Declaration of routines for packing RAW image to a row of pixels.
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

#ifndef RAW_H
#define RAW_H

#ifdef __cplusplus
extern "C" {
#endif

    // Bayer quad ordering for the 2x2 CFA cell.
    typedef enum
    {
        BAYER_ORDERING_RGGB = 0,
        BAYER_ORDERING_GBRG = 1,
        BAYER_ORDERING_BGGR = 2,
    } BAYER_ORDERING;

    void UnpackImage_14(const PACKED_IMAGE *input, UNPACKED_IMAGE *output, ENABLED_PARTS enabled_parts, BAYER_ORDERING bayer_ordering );

    void UnpackImage_12(const PACKED_IMAGE *input, UNPACKED_IMAGE *output, ENABLED_PARTS enabled_parts, BAYER_ORDERING bayer_ordering );

    void UnpackImage_12P(const PACKED_IMAGE *input, UNPACKED_IMAGE *output, ENABLED_PARTS enabled_parts, BAYER_ORDERING bayer_ordering );

#ifdef __cplusplus
}
#endif

#endif // RAW_H
