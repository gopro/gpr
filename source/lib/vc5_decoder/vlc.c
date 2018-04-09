/*! @file vlc.c
 *
 *  @brief Implementation of routines to parse a variable-length encoded bitstream.
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
	@brief Parse a run length coded magnitude in the bitstream
*/
CODEC_ERROR GetRlv(BITSTREAM *stream, CODEBOOK *codebook, RUN *run)
{
	BITWORD bitstream_bits = 0;			// Buffer of bits read from the stream
	BITCOUNT bitstream_count = 0;		// Number of bits read from the stream

	// Get the length of the codebook and initialize a pointer to its entries
	int codebook_length = codebook->length;
	RLV *codebook_entry = (RLV *)((uint8_t *)codebook + sizeof(CODEBOOK));

	// Index into the codebook
	int codeword_index = 0;

	// Search the codebook for the run length and value
	while (codeword_index < codebook_length)
	{
		// Get the size of the current word in the codebook
		BITCOUNT codeword_count = codebook_entry[codeword_index].size;

		// Need to read more bits from the stream?
		if (bitstream_count < codeword_count)
		{
			// Calculate the number of additional bits to read from the stream
			BITCOUNT read_count = codeword_count - bitstream_count;
			bitstream_bits = AddBits(stream, bitstream_bits, read_count);
			bitstream_count = codeword_count;
		}

		// Examine the run length table entries that have the same bit field length
		for (; (codeword_index < codebook_length) && (bitstream_count == codebook_entry[codeword_index].size);
				codeword_index++) {
			if (bitstream_bits == codebook_entry[codeword_index].bits) {
				run->count = codebook_entry[codeword_index].count;
				run->value = codebook_entry[codeword_index].value;
				goto found;
			}
		}
	}

	// Did not find a matching code in the codebook
	return CODEC_ERROR_NOTFOUND;

found:

	// Found a valid codeword in the bitstream
	return CODEC_ERROR_OKAY;
}

/*!
	Parse a run length coded signed value in the bitstream
*/
CODEC_ERROR GetRun(BITSTREAM *stream, CODEBOOK *codebook, RUN *run)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;
	int32_t value;

	// Get the magnitude of the number from the bitstream
	error = GetRlv(stream, codebook, run);

	// Error while parsing the bitstream?
	if (error != CODEC_ERROR_OKAY) {
		return error;
	}

	// Restore the sign to the magnitude of the run value
	value = run->value;

	// Signed quantity?
	if (value != 0)
	{
		BITWORD sign;

		// Something is wrong if the value is already negative
		assert(value > 0);

		// Get the codeword for the sign of the value
		sign = GetBits(stream, VLC_SIGNCODE_SIZE);

		// Change the sign if the codeword signalled a negative value
		value = ((sign == VLC_NEGATIVE_CODE) ? neg(value) : value);
	}

	// Return the signed value of the coefficient
	run->value = value;

	return CODEC_ERROR_OKAY;
}
