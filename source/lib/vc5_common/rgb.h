/*! @file rgb.h
 *
 *  @brief Handles conversion of wavelet to rgb format
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

#ifndef RGB_H
#define RGB_H

#include "types.h"
#include "pixel.h"
#include "image.h"
#include "gpr_rgb_buffer.h"

/*!
	@brief Per-channel lens shading (vignetting) gains

	Decoded from the source's DNG OpcodeList2 GainMap opcodes - one opcode per
	CFA cell - and reduced to one grid per output channel (the two green cells
	are averaged). Sampled bilinearly over the image and multiplied into the
	linear signal after black-level subtraction and before white balance, which
	is where the DNG spec places OpcodeList2.

	Grid coordinates follow the DNG opcode: index = (p - origin) / spacing,
	where p is the pixel's position normalized over the image (0..1). Gains are
	>= 1 and reach ~3.9 at the corners of a GoPro frame.

	samples[0] == NULL disables the correction, which is the default and what
	sources without gain map opcodes get.
*/
typedef struct _rgb_shading_map
{
    const float*        samples[3];         //!< R, G, B gain grids, row major, points_v * points_h each; NULL = no correction

    int                 points_v;           //!< Grid rows
    int                 points_h;           //!< Grid columns

    float               origin_v;           //!< Normalized position of grid row 0
    float               origin_h;
    float               spacing_v;          //!< Normalized distance between grid rows
    float               spacing_h;

} RGB_SHADING_MAP;

/*!
	@brief Parameters of the wavelet -> RGB conversion

	Everything WaveletToRGB needs to render RGB output, shared by the encoder
	(the RGB preview/thumbnail generated during encoding) and the decoder (the
	RGB output path), so both render with identical colors.
*/
typedef struct _rgb_parameters
{
    GPR_RGB_RESOLUTION  resolution;         //!< Resolution of the RGB output (or preview) relative to the full sensor.

    int                 bits;               //!< Bits per output RGB component: 8 (default) or 16

    gpr_rgb_gain        white_balance_gain; //!< White-balance gains applied after black-level subtraction

    int                 black_level;        //!< Sensor black level (16-bit linear RGB domain) subtracted before applying white_balance_gain

    float               color_matrix[3][3];  //!< Camera->linear-sRGB color matrix (row major), applied after rgb_gain; identity when no color profile is available

    float               baseline_exposure;  //!< DNG BaselineExposure (EV), applied as linear gain ahead of the tone curve; 0 = no adjustment

    RGB_SHADING_MAP     shading_map;        //!< Lens shading correction from OpcodeList2 GainMap; samples[0] == NULL = none

} RGB_PARAMETERS;

#ifdef __cplusplus
extern "C" {
#endif

    void rgb_parameters_set_default( RGB_PARAMETERS* rgb_params );

    void WaveletToRGB( gpr_allocator allocator, PIXEL* GS_src, PIXEL* RG_src, PIXEL* BG_src, DIMENSION src_width, DIMENSION src_height, DIMENSION src_pitch,
                       RGB_IMAGE *dst_image, int input_precision_bits, const RGB_PARAMETERS* rgb_params );

#ifdef __cplusplus
}
#endif

#endif // RGB_H
