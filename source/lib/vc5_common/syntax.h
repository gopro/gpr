/*! @file syntax.h
 *
 *  @brief Declaration of bitstream elements and functions that define the syntax
 *  of an encoded sample.
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

#ifndef SYNTAX_H
#define SYNTAX_H

#define CODEC_TAG_SIZE		16			//!< Size of a codec tag (in bits)
#define CODEC_TAG_MASK		0xFFFF		//!< Mask for usable part of tag or value

typedef uint32_t SEGMENT;		//!< The bitstream is a sequence of segments

typedef int16_t TAGWORD;		//!< Bitstream tag or value

//! Number of bits in a tag or value
static const BITCOUNT tagword_count = 16;

//! Number of bits in a segment (tag value pair)
static const BITCOUNT segment_count = 32;

typedef union tagvalue			//!< Bitstream tag and value pair
{
	struct {					// Fields are in the order for byte swapping
		TAGWORD value;
		TAGWORD tag;
	} tuple;					//!< Tag value pair as separate members

	uint32_t  longword;			//!< Tag value pair as a int32_t word

} TAGVALUE;

/*!
	@brief Values corresponding to the special codewords

	Special codewords are inserted into an entropy coded band to
	mark certain locations in the bitstream.  For example, the end
	of an encoded band is marked by the band end codeword.  Special
	codewords are recorded in the codebook as entries that have a
	run length of zero.  The value indicates the syntax element that
	is represented by the codeword.
*/
typedef enum _special_marker
{
	SPECIAL_MARKER_BAND_END = 1,

} SPECIAL_MARKER;


// The encoded quality is inserted into the bitstream using two tag value pairs
#define ENCODED_QUALITY_LOW_SHIFT	0			//!< Shift for the low part of the quality
#define ENCODED_QUALITY_LOW_MASK	0xFFFF		//!< Mask for the low part of the quality
#define ENCODED_QUALITY_HIGH_SHIFT	16			//!< Shift for the high part of the quality
#define ENCODED_QUALITY_HIGH_MASK	0xFFFF		//!< Mask for the high part of the quality

#ifdef __cplusplus
extern "C" {
#endif

TAGWORD RequiredTag(TAGWORD tag);

//TAGVALUE GetSegment(BITSTREAM *stream);

//TAGWORD GetValue(BITSTREAM *stream, int tag);

// Output a tagged value with double word alignment
CODEC_ERROR PutTagPair(BITSTREAM *stream, int tag, int value);

// Output an optional tagged value
CODEC_ERROR PutTagPairOptional(BITSTREAM *stream, int tag, int value);

// Output a tag that marks a place in the bitstream for debugging
CODEC_ERROR PutTagMarker(BITSTREAM *stream, uint32_t  marker, int size);

TAGWORD OptionalTag(TAGWORD tag);

//bool IsTagOptional(TAGWORD tag);

//bool IsTagRequired(TAGWORD tag);

//bool IsValidSegment(BITSTREAM *stream, TAGVALUE segment, TAGWORD tag);

//CODEC_ERROR AlignBitsTag(BITSTREAM *stream);

bool IsLowPassHeaderMarker(int marker);
bool IsLowPassBandMarker(int marker);
bool IsHighPassBandMarker(int marker);

bool IsAlignedTag(BITSTREAM *stream);

bool IsAlignedSegment(BITSTREAM *stream);

// Write an index block for the sample bands
CODEC_ERROR PutGroupIndex(BITSTREAM *stream,
						  void *index_table[],
						  int index_table_length,
						  size_t *channel_size_table_offset);

TAGWORD PackTransformPrescale(TRANSFORM *transform);

//TODO: Move other declarations for routines that write syntax elements here
struct _encoder ;

CODEC_ERROR PutFrameStructureFlags(struct _encoder *encoder, BITSTREAM *stream);

// Output a tag and marker before the lowpass coefficients for debugging
CODEC_ERROR PutVideoLowpassMarker(BITSTREAM *stream);

#ifdef __cplusplus
}
#endif

#endif // SYNTAX_H
