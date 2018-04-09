/*! @file syntax.c
 *
 *  @brief Implementation of functions for parsing the bitstream syntax of encoded samples.
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

//TODO: Simplify the definition of the marker bits
#define CODEC_LOWPASS_START_CODE		0x1A4A
#define CODEC_LOWPASS_START_SIZE		16
#define CODEC_LOWPASS_END_CODE			0x1B4B
#define CODEC_LOWPASS_END_SIZE			16

#define CODEC_HIGHPASS_START_CODE		0x0D0D
#define CODEC_HIGHPASS_START_SIZE		16
#define CODEC_HIGHPASS_END_CODE			0x0C0C
#define CODEC_HIGHPASS_END_SIZE			16

#define CODEC_BAND_START_CODE			0x0E0E
#define CODEC_BAND_START_SIZE			16
//#define CODEC_BAND_END_CODE 			0x038F0B3E	//Codeset dependent cs9
//#define CODEC_BAND_END_SIZE 			26			//Codeset dependent cs9
#define CODEC_BAND_END_CODE				0x0000E33F	//Codeset dependent cs15
#define CODEC_BAND_END_SIZE				16			//Codeset dependent cs15

#define CODEC_SAMPLE_STOP_CODE			0x1E1E
#define CODEC_SAMPLE_STOP_SIZE			16

#define CODEC_COEFFICIENT_START_CODE	0x0F0F
#define CODEC_COEFFICIENT_START_SIZE	16

//! Size of a tag or value (in bits)
#define BITSTREAM_TAG_SIZE				16


// Bits in the interlace structure flags

#define CODEC_FLAGS_INTERLACED			0x01	//!< Interlaced flags
#define CODEC_FLAGS_FIELD1_FIRST		0x02	//!< NTSC has this bit cleared
#define CODEC_FLAGS_FIELD1_ONLY			0x04	//!< Indicates missing fields
#define CODEC_FLAGS_FIELD2_ONLY			0x08
#define CODEC_FLAGS_DOMINANCE			0x10

#define CODEC_FLAGS_INTERLACED_MASK		0x1F	//!< Unused bits must be zero

// Useful macros for testing the interlaced flags

#define INTERLACED(flags)			(((flags) & CODEC_FLAGS_INTERLACED) != 0)
#define PROGRESSIVE(flags)			(((flags) & CODEC_FLAGS_INTERLACED) == 0)
#define FIELD_ORDER_NTSC(flags)		(((flags) & CODEC_FLAGS_FIELD1_FIRST) == 0)
#define FIELD_ORDER_PAL(flags)		(((flags) & CODEC_FLAGS_FIELD1_FIRST) != 0)
#define FIELD_ONE_ONLY(flags)		(((flags) & CODEC_FLAGS_FIELD1_ONLY) != 0)
#define FIELD_TWO_ONLY(flags)		(((flags) & CODEC_FLAGS_FIELD2_ONLY) != 0)
#define FIELD_ONE_PRESENT(flags)	(((flags) & CODEC_FLAGS_FIELD2_ONLY) == 0)
#define FIELD_TWO_PRESENT(flags)	(((flags) & CODEC_FLAGS_FIELD1_ONLY) == 0)
#define FIELD_BOTH_PRESENT(flags)	(((flags) & (CODEC_FLAGS_FIELD1_ONLY | CODEC_FLAGS_FIELD1_ONLY)) == 0)

// Bits in the copy protection flags

#define CODEC_FLAGS_PROTECTED			0x01	//!< Copy protection flags
#define CODEC_FLAGS_PROTECTION_MASK		0x01	//!< Unused bits must be zero

/*!
	@brief Check that the bitstream is aligned to a segment boundary
 
 This function definition duplicates the same function in common/src/syntax.c,
 but that file includes definitions intended only for the encoder.
 
 @todo Remove duplicate function definitions.
 */
bool IsAlignedSegment(BITSTREAM *stream)
{
    return (stream->count == 0 || stream->count == bit_word_count);
}

/*!
	@brief Convert the tag to an optional tag

	An optional tag has a negative value.
*/
TAGWORD OptionalTag(TAGWORD tag)
{
	return ((tag < 0) ? tag : neg(tag));
}

/*!
	@brief Convert the tag to a required tag

	An optional tag has a negative value.
*/
TAGWORD RequiredTag(TAGWORD tag)
{
	return ((tag >= 0) ? tag : neg(tag));
}


/*!
	@brief Check that the bitstream is aligned to a tag word boundary

	@todo Check the places in the code where this function is used to
	determine whether the bitstream should actually be aligned to a
	segment boundary.
*/
bool IsAlignedTag(BITSTREAM *stream)
{
	return ((stream->count % BITSTREAM_TAG_SIZE) == 0);
}

/*!
	@brief Pack the vector of prescale values into a single word

	The wavelet transform uses a vector of prescale values indexed by the
	wavelet level with the input image at level zero to specify the amount
	of prescaling that should be performed on the input the wavelet transform.

	This routine packs the prescale values into a segment value that can be
	written into the bitstream.
*/
TAGWORD PackTransformPrescale(TRANSFORM *transform)
{
	TAGWORD packed_prescale = 0;
	int i;

	// Encode the prescale values that are actually used
	for (i = 0; i < MAX_WAVELET_COUNT; i++)
	{
		assert((transform->prescale[i] & ~0x03) == 0);
		packed_prescale += transform->prescale[i] << (14 - i * 2);
	}

	// The remaining prescale values with filled with zeros

	return packed_prescale;
}

