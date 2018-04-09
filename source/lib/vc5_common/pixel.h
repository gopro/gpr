/*! @file pixel.h
 *
 *  @brief The pixel format enumerations define the pixel packing formats that are
 *  supported by the codec for input to the image unpacking process and for
 *  output from the image repacking process.
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

#ifndef PIXEL_H
#define PIXEL_H

//! Data type for pixels
typedef int16_t PIXEL;

//! Minimum and maximum pixel values
enum {
	PIXEL_MIN = INT16_MIN,
	PIXEL_MAX = INT16_MAX,
};

//! Alternative definition for wavelet coefficients
typedef int16_t COEFFICIENT;

//! Minimum and maximum coefficient values
enum
{
	COEFFICIENT_MIN = INT16_MIN,
	COEFFICIENT_MAX = INT16_MAX,
};

/*!
	@brief Pixels formats supported by the codec

	The pixel format is only the packing arrangement for color components and
	does not specify whether the image is interlaced or the bottom row is first.

	@todo Need to add support for more pixel formats to the reference decoder
*/
typedef enum
{
	PIXEL_FORMAT_UNKNOWN = 0,

    PIXEL_FORMAT_RAW_RGGB_16  = 104,

    PIXEL_FORMAT_RAW_RGGB_12  = 106,
    PIXEL_FORMAT_RAW_RGGB_12P = 107,
    PIXEL_FORMAT_RAW_RGGB_14  = 108,

    PIXEL_FORMAT_RAW_GBRG_12  = 109,
    PIXEL_FORMAT_RAW_GBRG_12P = 110,
    PIXEL_FORMAT_RAW_GBRG_14  = 111,
    
    PIXEL_FORMAT_RAW_DEFAULT = PIXEL_FORMAT_RAW_RGGB_14,
    
	//! Input pixel formats above this value must be encoded into the sample
	PIXEL_FORMAT_TAG_REQUIRED = 100,

} PIXEL_FORMAT;


#ifdef __cplusplus
extern "C" {
#endif

/*!
@brief Force a pixel value to be in range
*/
STATIC_INLINE PIXEL ClampPixel(int32_t value)
{
	// Check for values that are outside the range (for debugging)
	assert(PIXEL_MIN <= value && value <= PIXEL_MAX);

	if (value < PIXEL_MIN)
		value = PIXEL_MIN;
	else
		if (value > PIXEL_MAX)
			value = PIXEL_MAX;

	return (PIXEL)value;
}

#ifdef __cplusplus
}
#endif

#endif // PIXEL_H
