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
#ifndef _WIN32
#include <pthread.h>
#endif

/*
	Prefix lookup table for fast VLC decoding.

	Instead of linear-searching through 264 codebook entries per codeword,
	peek the top VLC_PEEK_BITS bits from the bitstream buffer and use them
	as an index into this table.  Entries with bits > 0 are direct hits;
	entries with bits == 0 require fallback to the original linear search.
*/
#define VLC_PEEK_BITS 12

typedef struct {
	uint8_t  bits;    /* Codeword length in bits (0 = not in table) */
	uint8_t  value;   /* Unsigned coefficient magnitude */
	uint16_t count;   /* Run length */
} VLC_LOOKUP;

static VLC_LOOKUP vlc_table[1 << VLC_PEEK_BITS];  /* 4096 entries, 16KB */
#ifndef _WIN32
static pthread_once_t vlc_init_once = PTHREAD_ONCE_INIT;
#endif
static CODEBOOK *vlc_init_codebook = NULL;
static int vlc_fast_disabled = 0;

/*!
	@brief Build the prefix lookup table from a codebook

	For each codebook entry with codeword size <= VLC_PEEK_BITS, fill all
	table slots whose top bits match the codeword.  A codeword of N bits
	maps to 2^(PEEK-N) table entries (the "don't care" suffix bits).
*/
static void InitVLCTable(CODEBOOK *codebook)
{
	int codebook_length = codebook->length;
	RLV *entries = (RLV *)((uint8_t *)codebook + sizeof(CODEBOOK));
	int table_size = 1 << VLC_PEEK_BITS;

	/* Clear the table (bits=0 means "not found") */
	memset(vlc_table, 0, sizeof(vlc_table));

	for (int i = 0; i < codebook_length; i++)
	{
		int size = entries[i].size;
		if (size < 1 || size > VLC_PEEK_BITS)
			continue;

		uint32_t codeword = entries[i].bits;
		int fill_count = 1 << (VLC_PEEK_BITS - size);
		int base_index = (int)(codeword << (VLC_PEEK_BITS - size));

		VLC_LOOKUP entry;
		entry.bits  = (uint8_t)size;
		entry.value = (uint8_t)entries[i].value;
		entry.count = (uint16_t)entries[i].count;

		for (int j = 0; j < fill_count; j++)
		{
			vlc_table[base_index + j] = entry;
		}
	}
}

/*!
	@brief pthread_once callback for thread-safe VLC table initialization
*/
static void InitVLCTableOnce(void)
{
	InitVLCTable(vlc_init_codebook);
	if (getenv("VLC_NO_FAST") != NULL)
		vlc_fast_disabled = 1;
}

/*!
	@brief Fast combined RLV + sign bit decode using prefix lookup table

	Peeks VLC_PEEK_BITS bits from the bitstream buffer and does a single
	table lookup.  If the codeword is found and fits in the available bits,
	consumes the bits and reads the sign bit inline.

	When the buffer is empty, refills before peeking.  When the buffer has
	fewer than VLC_PEEK_BITS bits, peeks anyway (zero-padded) and validates
	that the found codeword fits within the available bits.  Falls back to
	GetRun() only when the codeword is genuinely longer than available bits
	or not in the table.
*/
CODEC_ERROR GetRunFast(BITSTREAM *stream, CODEBOOK *codebook, RUN *run)
{
	/* Thread-safe one-time initialization of the lookup table.
	   Note: assumes a single codebook is used throughout the decode session. */
	if (vlc_init_codebook == NULL)
		vlc_init_codebook = codebook;
	assert(vlc_init_codebook == codebook);
#ifndef _WIN32
	pthread_once(&vlc_init_once, InitVLCTableOnce);
#else
	/* Windows: no pthread_once, use simple flag */
	if (vlc_table[0].bits == 0 && !vlc_fast_disabled)
		InitVLCTableOnce();
#endif

	/* Bypass fast path if disabled via VLC_NO_FAST env var */
	if (vlc_fast_disabled)
		return GetRun(stream, codebook, run);

	/* Refill if buffer is completely empty */
	if (stream->count == 0)
		GetBuffer(stream);

	/* Peek top VLC_PEEK_BITS bits from buffer (zero-padded if count < PEEK).
	   The buffer is left-aligned in 32 bits; unused low bits are already 0. */
	uint32_t peek = stream->buffer >> (32 - VLC_PEEK_BITS);
	VLC_LOOKUP *entry = &vlc_table[peek];

	/* Validate: codeword must be in table AND fit within available bits.
	   When count < PEEK_BITS, the zero-padded peek may yield a wrong entry
	   for codewords longer than count, but we catch that with the size check. */
	if (entry->bits == 0 || entry->bits > stream->count)
		return GetRun(stream, codebook, run);

	/* Consume codeword bits from the buffer */
	stream->buffer <<= entry->bits;
	stream->count -= entry->bits;
	run->count = entry->count;

	/* Handle sign bit inline for non-zero magnitude values */
	int32_t value = (int32_t)entry->value;
	if (value != 0)
	{
		/* Refill buffer if empty */
		if (stream->count == 0)
			GetBuffer(stream);

		/* Read 1-bit sign: top bit of buffer */
		if (stream->buffer >> 31)
			value = -value;
		stream->buffer <<= 1;
		stream->count--;
	}
	run->value = value;

	return CODEC_ERROR_OKAY;
}

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
