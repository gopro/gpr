/*! @file main_c.h
 *
 *  @brief Definition of C conversion routines used by gpr_tools
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

#ifndef MAIN_C_H
#define MAIN_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

    /*! @brief Inputs for dng_convert_main(), bundled so callers do not have to pass a long argument list. */
    typedef struct
    {
        const char*     input_file_path;
        unsigned int    input_width;
        unsigned int    input_height;
        size_t          input_pitch;
        size_t          input_skip_rows;
        size_t          input_skip_cols;
        const char*     input_pixel_format;

        const char*     output_file_path;
        const char*     metadata_file_path;
        const char*     gpmf_file_path;

        const char*     rgb_file_resolution;
        int             rgb_file_bits;
        int             jpg_quality;

        const char*     preview_resolution;     /* Resolution of the embedded preview image (2:1, 4:1, 8:1, 16:1) */

        const char*     jpg_preview_file_path;   /* External JPEG preview to embed; dimensions are read from its header */

    } dng_convert_params;

    int dng_convert_main( const dng_convert_params* convert_params );

#ifdef __cplusplus
}
#endif

#endif // MAIN_C_H
