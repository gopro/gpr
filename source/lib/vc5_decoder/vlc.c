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

// Table-driven codeword decode. The original implementation searched the codebook linearly for
// every codeword, growing the candidate bit field one bitstream read at a time -- the top
// hotspot of a GPR decode. Instead, a lookup table indexed by the next VLC_LUT_INDEX_BITS bits
// of the stream resolves every codeword of that length or shorter (each codeword's
// run/value/size is replicated into all table slots sharing its prefix) in one step. Longer
// codewords -- statistically rare tail entries of the codebook -- are matched by scanning just
// the long entries against the bit cache, so they consume no stream state during the search
// either. See VLC_CURSOR in vlc.h for how the bitstream state is managed across codewords.

#define VLC_LUT_INDEX_BITS  12
#define VLC_LUT_SIZE        (1 << VLC_LUT_INDEX_BITS)

typedef struct _vlc_lut_entry
{
	uint8_t  size;			//!< Codeword length in bits; 0 means longer than VLC_LUT_INDEX_BITS
	uint16_t count;			//!< Run length
	int16_t  value;			//!< Run value (unsigned magnitude)
} VLC_LUT_ENTRY;

static VLC_LUT_ENTRY vlc_lut[VLC_LUT_SIZE];

//! Codebook the lookup table was built from (the decoder uses a single static codebook, so the
//! table is built once; decoding is single-threaded, and a concurrent rebuild would be harmless
//! anyway since every fill writes the same values)
static const CODEBOOK *vlc_lut_codebook = NULL;

//! Index of the first codebook entry longer than VLC_LUT_INDEX_BITS (the codebook is sorted by
//! codeword length), where the long-codeword fallback scan starts
static int vlc_lut_long_index = 0;

static const RLV *CodebookEntries(const CODEBOOK *codebook)
{
	return (const RLV *)((const uint8_t *)codebook + sizeof(CODEBOOK));
}

static void FillVlcLut(const CODEBOOK *codebook)
{
	const RLV *entries = CodebookEntries(codebook);
	int length = codebook->length;
	int i;

	memset(vlc_lut, 0, sizeof(vlc_lut));
	vlc_lut_long_index = length;

	for (i = 0; i < length; i++)
	{
		// The long-codeword fallback requires the codebook sorted by codeword length
		assert(i == 0 || entries[i].size >= entries[i - 1].size);

		if (entries[i].size > VLC_LUT_INDEX_BITS)
		{
			vlc_lut_long_index = i;
			break;
		}

		// Replicate the entry into every table slot that starts with this codeword
		{
			int slot_count = 1 << (VLC_LUT_INDEX_BITS - entries[i].size);
			uint32_t first_slot = entries[i].bits << (VLC_LUT_INDEX_BITS - entries[i].size);
			int slot;

			for (slot = 0; slot < slot_count; slot++)
			{
				vlc_lut[first_slot + slot].size  = (uint8_t)entries[i].size;
				vlc_lut[first_slot + slot].count = (uint16_t)entries[i].count;
				vlc_lut[first_slot + slot].value = (int16_t)entries[i].value;
			}
		}
	}

	vlc_lut_codebook = codebook;
}

/*!
	@brief Top up the cursor bit cache from the byte stream

	Loads whole 32-bit words in the same big-endian byte order as GetBuffer/Swap32, so the read
	position stays word aligned and can be handed back to the BITSTREAM by VlcCursorFlush. Near
	the end of the stream the cache is left zero-padded instead; a valid sample always contains
	the band-end codeword before that padding could be consumed.
*/
static void VlcCursorRefill(VLC_CURSOR *cursor)
{
	while (cursor->bits <= 32 && cursor->byte_count + 4 <= cursor->byte_size)
	{
		const uint8_t *p = cursor->base + cursor->byte_count;
		uint32_t word = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		                ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];

		cursor->cache |= (uint64_t)word << (32 - cursor->bits);
		cursor->bits += 32;
		cursor->byte_count += 4;
	}
}

void VlcCursorInit(VLC_CURSOR *cursor, BITSTREAM *stream, CODEBOOK *codebook)
{
	const STREAM *byte_stream = stream->stream;

	cursor->stream = stream;
	cursor->usable = (byte_stream != NULL && byte_stream->type == STREAM_TYPE_MEMORY);

	if (!cursor->usable)
		return;

	if (vlc_lut_codebook != codebook) {
		FillVlcLut(codebook);
	}

	cursor->base       = (const uint8_t *)byte_stream->location.memory.buffer;
	cursor->byte_size  = byte_stream->location.memory.size;
	cursor->byte_count = byte_stream->byte_count;

	// The bitstream buffer holds its unread bits left justified
	cursor->cache = ((uint64_t)stream->buffer) << 32;
	cursor->bits  = (int)stream->count;
}

/*!
	@brief Write the cursor state back into the bitstream it was opened on

	Whole unread words are pushed back to the byte stream (the read position only ever moves in
	word units, mirroring GetBuffer), and the remaining bits land in the bitstream buffer with
	the BITSTREAM invariants intact, so decoding can continue through the regular accessors.
*/
void VlcCursorFlush(VLC_CURSOR *cursor)
{
	BITSTREAM *stream;

	if (!cursor->usable)
		return;

	stream = cursor->stream;

	while (cursor->bits > 32)
	{
		cursor->byte_count -= 4;
		cursor->bits -= 32;
	}

	// Corrupt streams can leave the cursor having consumed zero padding past the end of the
	// stream; clamp so the BITSTREAM stays in a valid (empty) state.
	if (cursor->bits < 0)
		cursor->bits = 0;

	stream->buffer = (BITWORD)(cursor->cache >> 32) & ~BitMask(32 - cursor->bits);
	stream->count  = (BITCOUNT)cursor->bits;
	stream->stream->byte_count = cursor->byte_count;

	cursor->usable = 0;
}

/*!
	@brief Match a codeword longer than the lookup table covers

	The refilled cache holds more bits than the longest codeword, so the candidate entries are
	compared against it directly and bits are only consumed once a match is found.
*/
static CODEC_ERROR VlcCursorLong(VLC_CURSOR *cursor, const CODEBOOK *codebook, RUN *run)
{
	const RLV *entries = CodebookEntries(codebook);
	int length = codebook->length;
	int i;

	for (i = vlc_lut_long_index; i < length; i++)
	{
		if ((BITWORD)(cursor->cache >> (64 - entries[i].size)) == entries[i].bits)
		{
			cursor->cache <<= entries[i].size;
			cursor->bits   -= entries[i].size;
			run->count = entries[i].count;
			run->value = entries[i].value;
			return CODEC_ERROR_OKAY;
		}
	}

	return CODEC_ERROR_NOTFOUND;
}

/*!
	@brief Parse a run length coded magnitude at the cursor (without the sign)
*/
CODEC_ERROR VlcCursorGetRlv(VLC_CURSOR *cursor, CODEBOOK *codebook, RUN *run)
{
	const VLC_LUT_ENTRY *entry;

	VlcCursorRefill(cursor);

	// Out of stream bits without finding a codeword? (only corrupt streams get here)
	if (cursor->bits <= 0)
		return CODEC_ERROR_NOTFOUND;

	entry = &vlc_lut[cursor->cache >> (64 - VLC_LUT_INDEX_BITS)];

	if (entry->size > 0)
	{
		cursor->cache <<= entry->size;
		cursor->bits   -= entry->size;
		run->count = entry->count;
		run->value = entry->value;
		return CODEC_ERROR_OKAY;
	}

	return VlcCursorLong(cursor, codebook, run);
}

/*!
	@brief Parse a run length coded signed value at the cursor

	The sign bit that follows a non-zero magnitude is consumed together with the codeword.
*/
CODEC_ERROR VlcCursorGetRun(VLC_CURSOR *cursor, CODEBOOK *codebook, RUN *run)
{
	const VLC_LUT_ENTRY *entry;
	CODEC_ERROR error;

	VlcCursorRefill(cursor);

	// Out of stream bits without finding a codeword? (only corrupt streams get here)
	if (cursor->bits <= 0)
		return CODEC_ERROR_NOTFOUND;

	entry = &vlc_lut[cursor->cache >> (64 - VLC_LUT_INDEX_BITS)];

	if (entry->size > 0)
	{
		run->count = entry->count;

		if (entry->value != 0)
		{
			// The sign bit is the next bit after the codeword
			uint32_t sign = (uint32_t)(cursor->cache >> (63 - entry->size)) & 1;

			run->value = (sign == VLC_NEGATIVE_CODE) ? neg(entry->value) : entry->value;
			cursor->cache <<= entry->size + VLC_SIGNCODE_SIZE;
			cursor->bits   -= entry->size + VLC_SIGNCODE_SIZE;
		}
		else
		{
			run->value = 0;
			cursor->cache <<= entry->size;
			cursor->bits   -= entry->size;
		}

		return CODEC_ERROR_OKAY;
	}

	error = VlcCursorLong(cursor, codebook, run);
	if (error != CODEC_ERROR_OKAY) {
		return error;
	}

	// Restore the sign to the magnitude of the run value (same semantics as GetRun: any
	// non-zero value, including special markers, is followed by a sign bit)
	if (run->value != 0)
	{
		uint32_t sign = (uint32_t)(cursor->cache >> 63) & 1;

		run->value = (sign == VLC_NEGATIVE_CODE) ? neg(run->value) : run->value;
		cursor->cache <<= VLC_SIGNCODE_SIZE;
		cursor->bits   -= VLC_SIGNCODE_SIZE;
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Parse a run length coded magnitude from a stream that cannot be peeked

	This is the original linear codebook search, kept for byte streams that are not memory
	buffers (the search grows the candidate bit field entry by entry, consuming stream bits).
*/
static CODEC_ERROR GetRlvSlow(BITSTREAM *stream, CODEBOOK *codebook, RUN *run)
{
	BITWORD bitstream_bits = 0;			// Buffer of bits read from the stream
	BITCOUNT bitstream_count = 0;		// Number of bits read from the stream

	// Get the length of the codebook and initialize a pointer to its entries
	int codebook_length = codebook->length;
	const RLV *codebook_entry = CodebookEntries(codebook);

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
	@brief Parse a run length coded magnitude in the bitstream
*/
CODEC_ERROR GetRlv(BITSTREAM *stream, CODEBOOK *codebook, RUN *run)
{
	VLC_CURSOR cursor;
	CODEC_ERROR error;

	VlcCursorInit(&cursor, stream, codebook);

	if (!cursor.usable) {
		return GetRlvSlow(stream, codebook, run);
	}

	error = VlcCursorGetRlv(&cursor, codebook, run);
	VlcCursorFlush(&cursor);

	return error;
}

/*!
	Parse a run length coded signed value in the bitstream
*/
CODEC_ERROR GetRun(BITSTREAM *stream, CODEBOOK *codebook, RUN *run)
{
	VLC_CURSOR cursor;
	CODEC_ERROR error;
	int32_t value;

	VlcCursorInit(&cursor, stream, codebook);

	if (cursor.usable)
	{
		error = VlcCursorGetRun(&cursor, codebook, run);
		VlcCursorFlush(&cursor);
		return error;
	}

	// Get the magnitude of the number from the bitstream
	error = GetRlvSlow(stream, codebook, run);

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
