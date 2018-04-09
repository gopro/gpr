/*! @file parameters.h
 *
 *  @brief Declare a data structure for holding a table of parameters that is passed
 *  to the encoder during initialization.
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

#ifndef PARAMETERS_H
#define PARAMETERS_H

#include "vc5_encoder.h"

/*!
	@brief Function prototype for a decompositor

	A decompositor is the opposite of an image composition operator:
	It decomposes a frame into one or more frames.
	
	For example, an interlaced frame can be decomposed into fields or two frames
	arranged side-by-side within a single frame can be decomposed into individual
	frames.

	Each layer in an encoded sample may correspond to a separate input frame.
	For convenience, the reference codec stores the input to the encoder in a
	separate file with one frame per file if the encoded sample has a single layer.
	To allow the reference encoder to store all of the input frames that are
	encoded as separate layers in an encoded sample in a single file, multiple
	frames are stored in the file (often using over-under frame packing).  The
	decomposer unpacks multiple frames in a single frame into individual frames
	for encoding with one frame per layer.
*/

typedef CODEC_ERROR (* DECOMPOSITOR)(IMAGE *packed_image, IMAGE *image_array[], int frame_count);

/*!
	@brief Declaration of a data structure for passing parameters to the encoder

	The encoded dimensions are the width and height of the planes of pixels as
	represented internally in the encoded sample.  In the case where the planes
	have different dimensions (for example YUV with 4:2:2 sampling), the first
	encoded plane (corresponding to the luma plane, for example) is reported.
*/
typedef struct _encoder_parameters
{
	uint32_t version;				//!< Version number for this definition of the parameters
    
    // BAYER_ORDERING bayer_ordering;
    
	ENABLED_PARTS enabled_parts;	//!< Parts of the VC-5 standard that are enabled

	//! Data structure for the input frame dimensions and format
	struct _input_parameters
	{
		DIMENSION width;			//!< Width of the frames input to the encoder
		DIMENSION height;			//!< Height of the frames input to the encoder
		PIXEL_FORMAT format;		//!< Pixel format of the input frames
		PRECISION precision;		//!< Bits per component in the input image

	} input;		//!< Dimensions and format of the input frame

	//! Data structure for the encoded representation of the image
	struct _encoded_parameters
	{
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
		DIMENSION width;			//!< Width of the encoded frame
		DIMENSION height;			//!< Height of the encoded frame
		IMAGE_FORMAT format;		//!< Internal format of the encoded image
		PRECISION precision;		//!< Encoded precision of the image after scaling
#endif
		//! Number of bits used to encode lowpass coefficients
		PRECISION lowpass_precision;

	} encoded;		//!< Encoded frame dimensions and the encoded format

	//! Array of quantization values indexed by the subband number
    QUANT quant_table[MAX_SUBBAND_COUNT];

#if VC5_ENABLED_PART(VC5_PART_METADATA)
	//! Metadata that controls decoding (currently not used)
	METADATA metadata;
#endif
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
	int layer_count;
	int progressive;
	DECOMPOSITOR decompositor;
#endif

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    ENABLED_SECTIONS enabled_sections;
#endif
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	// Definition of the pattern elements
	DIMENSION pattern_width;
	DIMENSION pattern_height;
	DIMENSION components_per_sample;
	//PRECISION max_bits_per_component;
#endif

	//! Table for the order in which channels are encoded into the bitstream
	CHANNEL channel_order_table[MAX_CHANNEL_COUNT];

	//! Number of entries in the channel order table (may be less than the channel count)
	int channel_order_count;
    
	//! Flag that controls verbose output
	bool verbose_flag;

    gpr_allocator       allocator;
    
    GPR_RGB_RESOLUTION  rgb_resolution;

    gpr_rgb_gain        rgb_gain;
    
} ENCODER_PARAMETERS;

#ifdef __cplusplus
extern "C" {
#endif
    
    CODEC_ERROR InitEncoderParameters(ENCODER_PARAMETERS *parameters);

#ifdef __cplusplus
}
#endif

#endif // PARAMETERS_H
