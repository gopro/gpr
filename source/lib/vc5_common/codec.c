/*! @file codec.c
 *
 *  @brief Implementation of functions that are common to the
 *  reference decoder and encoder
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

#include "common.h"

/*!
	@brief Initialize the codec state to before encoding or decoding the bitstream

	Most of the codec state can be deduced from the decoding parameters.
	For example, the dimensions of the first wavelet band in the bitstream
	can be deduced from the encoded frame dimensions and the structure of
	the wavelet tree.

	The encoder will not insert parameters into the bitstream if the values
	of the parameters are the same as in the codec state.  This routine
	should initialize the codec state with correct values if those values
	can be inferred by the decoder, otherwise the use incorrect or default
	values.

	Note that the default encoded format is YUV 4:2:2, but this format is not
	supported by the baseline profile encoder so the encoded format must be
	explicitly written into the bitstream.

	@todo Add more default values required to properly initialize the codec state.
*/
CODEC_ERROR PrepareCodecState(CODEC_STATE *codec)
{
	// Initialize the channel to channel number zero
	codec->channel_number = 0;

	// Initialize the subband number to subband zero (the lowpass band)
	codec->subband_number = 0;

	// The number of subbands per channel is a constant
	codec->subband_count = 10;

	// The default precision of pixels in the input frame is ten bits
	codec->bits_per_component = 12;

	// Force the encoder to insert the encoded dimensions into the bitstream
	//codec->encoded.width = 0;
	//codec->encoded.height = 0;

	/*
		TODO: Do not have to encode the display dimensions into the bitstream if the decoder
		can infer the display dimensions from the encoded dimensions and format, but must
		encode the display dimensions if the encoded dimensions include padding.

		TODO: Check that this matches the current decoder implementation.
	*/

	// Set the default precision for encoding lowpass band coefficients
	codec->lowpass_precision = 16;

	//TODO: Set more default values in the codec state

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Reformat a segment value into the encoder version

	The version of the encoder that created the clip may be encoded into every sample.

	@todo Document the encoder version number format
*/
uint32_t EncoderVersion(uint32_t value)
{
	return (((value >> 12) & 0x0F) << 16) |
			(((value >> 8) & 0x0F) <<  8) |
			((value) & 0xFF);
}

/*!
	@brief Unpack the version tag value into its components
*/
void SetCodecVersion(uint8_t version[3], uint16_t value)
{
	version[0] = (uint8_t)((value >> 12) & 0x0F);		// Major version
	version[1] = (uint8_t)((value >>  8) & 0x0F);		// Minor version
	version[2] = (uint8_t)(value & 0xFF);				// Revision
}

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
/*!
	@brief Return true if the image format is valid
*/
bool ValidImageFormat(IMAGE_FORMAT image_format)
{
	if (0 < image_format && image_format < IMAGE_FORMAT_COUNT) {
		return true;
	}
	return false;
}
#endif

/*!
	@brief Unpack the tag value into the prescale table

	The prescale table contains the prescale value for each wavelet in the
	transform.  The prescale value is a right shift that is applied to the
	input data before the wavelet is computed.

	The prescale table is used for all transforms and does not depend on the
	channel number.
*/
CODEC_ERROR UpdatePrescaleTable(CODEC_STATE *codec, TAGWORD value)
{
	int wavelet_index;

	for (wavelet_index = 0; wavelet_index < MAX_WAVELET_COUNT; wavelet_index++)
	{
		// Unpack the prescale value
		int prescale_value = (value >> (14 - wavelet_index * 2)) & 0x03;
		assert(0 <= prescale_value && prescale_value < UINT8_MAX);

		// Store the prescale value in the codec state
		codec->prescale_table[wavelet_index] = (uint_fast8_t)prescale_value;
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Update the flags that describe the frame structure

	The frame structure includes characteristics such as interlaced versus
	progressive and top or bottom field first.
*/
CODEC_ERROR UpdateFrameStructureFlags(CODEC_STATE *codec, TAGWORD value)
{
	codec->progressive = !(value & IMAGE_STRUCTURE_INTERLACED);
	codec->top_field_first = !(value & IMAGE_STRUCTURE_BOTTOM_FIELD_FIRST);
	codec->frame_inverted = (value & IMAGE_STRUCTURE_BOTTOM_ROW_FIRST);

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Initialize the codec state using the default constructor

	This routine is like a default constructor in C++ as it guarantees that
	the codec state is initialized to a know starting state with all pointers
	set to NULL and all counters set to zero.

	The routine @ref PrepareCodecState is used to set default values for the
	codec state prior to decoding a sample.
*/
CODEC_ERROR InitCodecState(CODEC_STATE *state)
{
	// Clear the codec state
	memset(state, 0, sizeof(CODEC_STATE));
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Set the flags that determine the band coding

	There can be up to 15 different codebooks as specified by the lower
	four bigs in the band coding flags.  Use the default codebook if the
	active codebook is zero.

	The baseline profile does not allow difference coding or alternative
	codebooks.
*/
CODEC_ERROR SetBandCoding(CODEC_STATE *codec, TAGWORD value)
{
	(void) codec;
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Return true if the specified part is enabled at runtime

	This predicate is used to test whether a specific part in the VC-5 standard is
	enabled at runtime by this codec implementation.

*/
bool IsPartEnabled(ENABLED_PARTS enabled_parts, int part_number)
{
	return ((enabled_parts & VC5_PART_MASK(part_number)) != 0);
}

