/*! @file vlc.c
 *
 *  @brief Implementation of routines to insert variable length codes into the bitstream
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

#include "headers.h"

/*!
	@brief Write the codewords for a run of zeros into the bitstream

	The codebook contains codewords for a few runs of zeros.  This routine writes
	multiple codewords into the bitstream until the specified number of zeros has
	been written into the bitstream.
*/
CODEC_ERROR PutZeros(BITSTREAM *stream, const RUNS_TABLE *runs_table, uint32_t count)
{
	// Get the length of the codebook and a pointer to the entries
	uint32_t length = runs_table->length;
	RLC *rlc = (RLC *)((uint8_t *)runs_table + sizeof(RUNS_TABLE));
	int index;

	// Output one or more run lengths until the run is finished
	while (count > 0)
	{
		// Index into the codebook to get a run length code that covers most of the run
		index = (count < length) ? count : length - 1;

		// Output the run length code
		PutBits(stream, rlc[index].bits, rlc[index].size);

		// Reduce the length of the run by the amount output
		count -= rlc[index].count;
	}

	// Should have output enough runs to cover the run of zeros
	assert(count == 0);

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Insert a special codeword into the bitstream

	The codebook contains special codewords in addition to the codebook
	entries for coefficient magnitudes and runs of zeros.  Special codewords
	are used to mark important locations in the bitstream.  Currently,
	the only special codeword is the one that marks the end of an encoded
	band.
*/
CODEC_ERROR PutSpecial(BITSTREAM *stream, const CODEBOOK *codebook, SPECIAL_MARKER marker)
{
	int codebook_length = codebook->length;
	RLV *codebook_entry = (RLV *)((uint8_t *)codebook + sizeof(CODEBOOK));

	int index;

	// Find the special code that corresponds to the marker
	for (index = 0; index < codebook_length; index++)
	{
		// Is this entry a special code?
		if (codebook_entry[index].count != 0) {
			continue;
		}
		
		// Is this entry the special code for the marker?
		if (codebook_entry[index].value == marker) {
			break;
		}
	}
	assert(index < codebook_length);
	if (! (index < codebook_length)) {
		// Did not find the special code for the marker in the codebook
		return CODEC_ERROR_INVALID_MARKER;
	}

	PutBits(stream, codebook_entry[index].bits, codebook_entry[index].size);

	return CODEC_ERROR_OKAY;
}
