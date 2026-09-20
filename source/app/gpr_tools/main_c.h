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
        const char*     output_format;           /* Optional override of the format implied by the output file extension (GPR or DNG) */
        const char*     metadata_file_path;
        const char*     gpmf_file_path;

        const char*     lens_correction;         /* Geometric lens-distortion correction, DNG output only.
                                                    NULL or empty: carry over a camera-original warp
                                                    unchanged. "auto": look up the built-in profile for
                                                    the source camera model (fails when there is none).
                                                    "k0,k1,k2,k3[,cx,cy]": explicit WarpRectilinear radial
                                                    coefficients, center defaulting to 0.5,0.5. */

        const char*     lens_correction_strength; /* Strength of the geometric correction, "0".."1".
                                                    1 = fully rectilinear (heaviest crop), 0 = none.
                                                    NULL or empty: the camera profile's recommended
                                                    strength for "auto", 1.0 for explicit
                                                    coefficients. */

        const char*     rgb_file_resolution;
        int             rgb_file_bits;
        int             jpg_quality;

        const char*     quality;                 /* VC-5 encoder quality for GPR output: low, medium, high,
                                                    fs1, fsx, fs2 or ultra. NULL or empty: the encoder
                                                    default (fsx). A GPR input, otherwise repackaged as-is,
                                                    is re-encoded at this quality. Fails the conversion for
                                                    any other output type. */

        const char*     preview;                 /* Embedded preview image. NULL or empty: no preview is
                                                    written. The path of a jpg file on disk: that file is
                                                    embedded as the preview. 2:1, 4:1, 8:1 or 16:1: a
                                                    preview is auto-generated at that resolution.
                                                    Any other value fails the conversion. */

    } dng_convert_params;

    int dng_convert_main( const dng_convert_params* convert_params );

#ifdef __cplusplus
}
#endif

#endif // MAIN_C_H
