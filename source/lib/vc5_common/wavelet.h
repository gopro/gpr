/*! @file vlc.h
 *
 *  @brief This file defines the data structures for the wavelet tree
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

#ifndef WAVELET_H
#define WAVELET_H

#include "common.h"

/*!
	@brief Data structure used for wavelets

	This data structure is used for wavelets and can be used for images since
	an image with multiple planar channels and a wavelet with multiple bands
	are similar data structures.

	The pitch is the distance between rows in bytes and must always be
	an integer multiple of the pixel size in bytes.

	The wavelet data structure contains an array of the scale factor for
	each band that is the cummulative result of the application of the
	wavelet transforms that created the wavelet.
*/
typedef struct _wavelet
{
	DIMENSION width;					//!< Width of the image in pixels
	DIMENSION height;					//!< Height of the image in lines
	DIMENSION pitch;					//!< Distance between rows (in bytes)
	uint16_t band_count;				//!< Number of bands in a wavelet
	uint32_t valid_band_mask;			//!< Mask indicating which bands have been decoded
	uint16_t scale[MAX_BAND_COUNT];		//!< Cumulative scaling by the wavelet transforms
	QUANT quant[MAX_BAND_COUNT];		//!< Quantization value for each band
	PIXEL *data[MAX_BAND_COUNT];		//!< Data buffer for each band
    
} WAVELET;

//! Indices for the wavelet bands in the image data structure
typedef enum
{
	LL_BAND = 0,	//!< Lowpass transform of lowpass intermediate result
	LH_BAND,		//!< Lowpass transform of highpass intermediate result
	HL_BAND,		//!< Highpass transform of lowpass intermediate result
	HH_BAND			//!< Highpass transform of highpass intermediate result
} WAVELET_BAND;

//! Types of wavelet tranforms
enum
{
	WAVELET_TYPE_HORIZONTAL = 1,
	WAVELET_TYPE_VERTICAL = 2,
	WAVELET_TYPE_TEMPORAL = 4,

	//! The baseline profile only supports spatial wavelets
	WAVELET_TYPE_SPATIAL = (WAVELET_TYPE_HORIZONTAL | WAVELET_TYPE_VERTICAL),

};

//! Data structure for the wavelet tree (one channel)
typedef struct _transform
{
	//! Prescale the input by the specified shift before the transform
	PRESCALE prescale[MAX_WAVELET_COUNT];

	//! List of the wavelets in the transform for one channel
	WAVELET *wavelet[MAX_WAVELET_COUNT];

} TRANSFORM;


#ifdef __cplusplus
extern "C" {
#endif

    CODEC_ERROR AllocWavelet(gpr_allocator *allocator, WAVELET *wavelet, DIMENSION width, DIMENSION height);
    CODEC_ERROR ReleaseWavelet(gpr_allocator *allocator, WAVELET *wavelet);

    WAVELET *CreateWavelet(gpr_allocator *allocator, DIMENSION width, DIMENSION height);
    CODEC_ERROR DeleteWavelet(gpr_allocator *allocator, WAVELET *wavelet);

    CODEC_ERROR SetTransformScale(TRANSFORM *transform);

    CODEC_ERROR SetTransformPrescale(TRANSFORM *transform, int precision);

    bool BandValidMask(int band);

    bool BandsAllValid(WAVELET *wavelet);
    #define AllBandsValid BandsAllValid

    CODEC_ERROR UpdateWaveletValidBandMask(WAVELET *wavelet, int band);

    int SubbandWaveletIndex(int subband);

    int SubbandBandIndex(int subband);

    CODEC_ERROR ResetTransformFlags(TRANSFORM transform[], int transform_count);

    CODEC_ERROR ReleaseTransform(gpr_allocator *allocator, TRANSFORM *transform);

    bool IsTransformPrescaleDefault(TRANSFORM *transform, int precision);

    PIXEL *WaveletRowAddress(WAVELET *wavelet, int band, int row);

    void WaveletToRGB( gpr_allocator allocator, PIXEL* GS_src, PIXEL* RG_src, PIXEL* BG_src, DIMENSION src_width, DIMENSION src_height, DIMENSION src_pitch, RGB_IMAGE *dst_image,
                       int input_precision_bits, int output_precision_bits, gpr_rgb_gain* rgb_gain );
    
#ifdef __cplusplus
}
#endif

#endif // WAVELET_H
