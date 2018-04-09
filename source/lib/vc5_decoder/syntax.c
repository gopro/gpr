/*! @file syntax.c
 *
 *  @brief Implementation of functions for parsing the bitstream syntax of encoded samples.
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

#include "headers.h"

//! Size of a tag or value (in bits)
#define BITSTREAM_TAG_SIZE				16

/*!
	@brief Read the next tag-valie pair from the bitstream.

	The next tag is read from the bitstream and the next value that
	immediately follows the tag in the bitstreeam are read from the
	bitstream.

	The value may be the length of the payload in bytes or the value
	may be a single scalar.  This routine only reads the next tag and
	value and does not intepret the tag or value and does not read any
	data that may follow the segment in the bitstream.

	The tag and value are interpreted by @ref UpdateCodecState and that
	routine may read additional information from the bitstream.

	If the value is the length of the payload then it encodes the number
	of bytes in the segment payload, not counting the segment header.
*/
TAGVALUE GetSegment(BITSTREAM *stream)
{
	TAGVALUE segment;
	segment.tuple.tag = (TAGWORD)GetBits(stream, 16);
	segment.tuple.value = (TAGWORD)GetBits(stream, 16);
	return segment;
}

/*!
	@brief Read the specified tag from the bitstream and return the value
*/
TAGWORD GetValue(BITSTREAM *stream, int tag)
{
	TAGVALUE segment = GetTagValue(stream);

	assert(stream->error == BITSTREAM_ERROR_OKAY);
	if (stream->error == BITSTREAM_ERROR_OKAY) {
		assert(segment.tuple.tag == tag);
		if (segment.tuple.tag == tag) {
			return segment.tuple.value;
		}
		else {
			stream->error = BITSTREAM_ERROR_BADTAG;
		}
	}

	// An error has occurred so return zero (error code was set above)
	return 0;
}

/*!
	@brief Read the next tag value pair from the bitstream
*/
TAGVALUE GetTagValue(BITSTREAM *stream)
{
	TAGVALUE segment = GetSegment(stream);
	while (segment.tuple.tag < 0) {
		segment = GetSegment(stream);
	}

	return segment;
}

/*!
	@brief Return true if the tag is optional
*/
bool IsTagOptional(TAGWORD tag)
{
	return (tag < 0);
}

/*!
	@brief Return true if the tag is required
*/
bool IsTagRequired(TAGWORD tag)
{
	return (tag >= 0);
}

/*!
	@brief Return true if a valid tag read from the bitstream
*/
bool IsValidSegment(BITSTREAM *stream, TAGVALUE segment, TAGWORD tag)
{
	return (stream->error == BITSTREAM_ERROR_OKAY &&
			segment.tuple.tag == tag);
}

/*!
	@brief Return true if the tag value pair has the specified tag and value
*/
bool IsTagValue(TAGVALUE segment, int tag, TAGWORD value)
{
	return (segment.tuple.tag == tag && segment.tuple.value == value);
}
