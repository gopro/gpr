/*! @file codebooks.c
 *
 *  @brief Implementation of the routines for computing the encoding tables from a codebook.
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
#include "table17.inc"

//! Length of the codebook for runs of zeros
#define RUNS_TABLE_LENGTH 3072


/*!
	@brief Define the codeset used by the reference codec
 
	The baseline codec only supports codebook #17.
 
	Codebook #17 is intended to be used with cubic companding
	(see @ref FillMagnitudeEncodingTable and @ref ComputeCubicTable).
 */
ENCODER_CODESET encoder_codeset_17 = {
    "Codebook set 17 from data by David Newman with tables automatically generated for the FSM decoder",
    (const CODEBOOK *)&table17,
    CODESET_FLAGS_COMPANDING_CUBIC,
    NULL,
    NULL,
};

/*!
	@brief Initialize the codeset by creating more efficient tables for encoding

	This routine takes the original codebook in the codeset and creates a table
	of codewords for runs of zeros, indexed by the run length, and a table for
	coefficient magnitudes, indexed by the coefficient magnitude.  This allows
	runs of zeros and non-zero coefficients to be entropy coded using a simple
	table lookup.
*/
CODEC_ERROR PrepareCodebooks(const gpr_allocator *allocator, ENCODER_CODESET *cs)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;

	// Create a new table for runs of zeros with the default length
	const int runs_table_length = RUNS_TABLE_LENGTH;

	// Initialize the indexable table of run length codes
	RLV *old_codes = (RLV *)(((uint8_t *)cs->codebook) + sizeof(CODEBOOK));
	int old_length = cs->codebook->length;

	size_t runs_table_size = runs_table_length * sizeof(RLC) + sizeof(RUNS_TABLE);

	RUNS_TABLE *runs_table = allocator->Alloc(runs_table_size);
	RLC *new_codes = (RLC *)(((uint8_t *)runs_table) + sizeof(RUNS_TABLE));
	int new_length = runs_table_length;

	// Set the length of the table for encoding coefficient magnitudes
	int mags_table_shift = 8;
	int mags_table_length;
	size_t mags_table_size;
	MAGS_TABLE *mags_table;
	VLE *mags_table_entries;

	// Use a larger table if companding
	if (CompandingParameter() > 0) {
		//mags_table_shift = 11;
		mags_table_shift = 10;
	}

	// Allocate the table for encoding coefficient magnitudes
	mags_table_length = (1 << mags_table_shift);
	mags_table_size = mags_table_length * sizeof(VLE) + sizeof(MAGS_TABLE);
	mags_table = allocator->Alloc(mags_table_size);
	if (mags_table == NULL) {
		return CODEC_ERROR_OUTOFMEMORY;
	}

	mags_table_entries = (VLE *)(((uint8_t *)mags_table) + sizeof(MAGS_TABLE));

	// Create a more efficient codebook for encoding runs of zeros
	error = ComputeRunLengthCodeTable(allocator,
		old_codes, old_length, new_codes, new_length);
	if (error != CODEC_ERROR_OKAY) {
		return error;
	}

	// Store the new table for runs of zeros in the codeset
	runs_table->length = runs_table_length;
	cs->runs_table = runs_table;

	error = FillMagnitudeEncodingTable(cs->codebook, mags_table_entries, mags_table_length, cs->flags);
	if (error != CODEC_ERROR_OKAY) {
		return error;
	}

	mags_table->length = mags_table_length;
	cs->mags_table = mags_table;

	// The codebooks have been initialized successfully
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Free all data structures allocated for the codebooks
*/
CODEC_ERROR ReleaseCodebooks(gpr_allocator *allocator, ENCODER_CODESET *cs)
{
	allocator->Free( (void *)cs->runs_table );
	allocator->Free( (void *)cs->mags_table);
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Compute a table of codewords for runs of zeros

	The table is indexed by the length of the run of zeros.
*/
CODEC_ERROR ComputeRunLengthCodeTable(const gpr_allocator *allocator,
									  RLV *input_codes, int input_length,
									  RLC *output_codes, int output_length)
{
	// Need enough space for the codebook and the code for a single value
	int runs_codebook_length = input_length + 1;
	size_t runs_codebook_size = runs_codebook_length * sizeof(RLC);
	RLC *runs_codebook = (RLC *)allocator->Alloc(runs_codebook_size);
	bool single_zero_run_flag = false;
	int input_index;
	int runs_codebook_count = 0;
    CODEC_ERROR return_code = CODEC_ERROR_OKAY;

	// Copy the codes for runs of zeros into the temporary codebook for sorting
	for (input_index = 0; input_index < input_length; input_index++)
	{
		uint32_t count = input_codes[input_index].count;
		int32_t value = input_codes[input_index].value;

		// Is this code for a run of zeros?
		if (value != 0 || count == 0) {
			// Skip codebook entries for coefficient magnitudes and special codes
			continue;
		}

		// Is this code for a single zero
		if (count == 1 && value == 0) {
			single_zero_run_flag = true;
		}

		// Check that the temporary runs codebook is not full
		assert(runs_codebook_count < runs_codebook_length);

		// Copy the code into the temporary runs codebook
		runs_codebook[runs_codebook_count].size = input_codes[input_index].size;
		runs_codebook[runs_codebook_count].bits = input_codes[input_index].bits;
		runs_codebook[runs_codebook_count].count = count;

		// Check the codebook entry
		assert(runs_codebook[runs_codebook_count].size > 0);
		assert(runs_codebook[runs_codebook_count].count > 0);

		// Increment the count of codes in the temporary runs codebook
		runs_codebook_count++;
	}

	// Check that the runs codebook includes a run of a single zero
    if( single_zero_run_flag == false )
    {
        assert(false);
        return_code = CODEC_ERROR_UNEXPECTED;
    }

	// Sort the codewords into decreasing run length
	SortDecreasingRunLength(runs_codebook, runs_codebook_count);

	// The last code must be for a single run
	assert(runs_codebook[runs_codebook_count - 1].count == 1);

	// Fill the lookup table with codes for runs indexed by the run length
	FillRunLengthEncodingTable(runs_codebook, runs_codebook_count, output_codes, output_length);

	// Free the space used for the sorted codewords
	allocator->Free(runs_codebook);

	return return_code;
}

/*!
	@brief Sort the codebook into decreasing length of the run
*/
CODEC_ERROR SortDecreasingRunLength(RLC *codebook, int length)
{
	int i;
	int j;

	// Perform a simple bubble sort since the codebook may already be sorted
	for (i = 0; i < length; i++)
	{
		for (j = i+1; j < length; j++)
		{
			// Should not have more than one codebook entry with the same run length
			assert(codebook[i].count != codebook[j].count);

			// Exchange codebook entries if the current entry is smaller
			if (codebook[i].count < codebook[j].count)
			{
				int size = codebook[i].size;
				uint32_t  bits = codebook[i].bits;
				int count = codebook[i].count;

				codebook[i].size = codebook[j].size;
				codebook[i].bits = codebook[j].bits;
				codebook[i].count = codebook[j].count;

				codebook[j].size = size;
				codebook[j].bits = bits;
				codebook[j].count = count;
			}
		}

		// After two iterations that last two items should be in the proper order
		assert(i == 0 || codebook[i-1].count > codebook[i].count);
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Use a sparse run length code table to create an indexable table for faster encoding
*/
CODEC_ERROR FillRunLengthEncodingTable(RLC *codebook, int codebook_length, RLC *table, int table_length)
{
	int i;		// Index into the lookup table
	int j;		// Index into the codebook

	// Use all of the bits except the sign bit for the codewords
	int max_code_size = bit_word_count - 1;

	// Check that the input codes are sorted into decreasing run length
	for (j = 1; j < codebook_length; j++)
	{
		RLC *previous = &codebook[j-1];
		RLC *current = &codebook[j];

		assert(previous->count > current->count);
		if (! (previous->count > current->count)) {
			return CODEC_ERROR_UNEXPECTED;
		}
	}

	// The last input code should be the code for a single zero
	assert(codebook[codebook_length - 1].count == 1);

	// Create the shortest codeword for each table entry
	for (i = 0; i < table_length; i++)
	{
		int length = i;			// Length of the run for this table entry
		uint32_t codeword = 0;	// Composite codeword for this run length
		int codesize = 0;		// Number of bits in the composite codeword
		int remaining;			// Remaining run length not covered by the codeword

		remaining = length;

		for (j = 0; j < codebook_length; j++)
		{
			int repetition;		// Number of times the codeword is used
			int k;

			// Nothing to do if the remaining run length is zero
			if (remaining == 0) break;

			// The number of times that the codeword is used is the number
			// of times that it divides evenly into the remaining run length
			repetition = remaining / codebook[j].count;

			// Append the codes to the end of the composite codeword
			for (k = 0; k < repetition; k++)
			{
				// Terminate the loop if the codeword will not fit
				if (codebook[j].size > (max_code_size - codesize))
				{
					if (codesize)
					{
						remaining -= (k * codebook[j].count);
						goto next;
					}
					else
					{
						break;
					}
				}

				// Shift the codeword to make room for the appended codes
				codeword <<= codebook[j].size;

				// Insert the codeword from the codebook at the end of the composite codeword
				codeword |= codebook[j].bits;

				// Increment the number of bits in the composite codeword
				codesize += codebook[j].size;
			}

			// Reduce the run length by the amount that was consumed by the repeated codeword
			remaining -= (k * codebook[j].count);
		}

next:
		// Should have covered the entire run if the last codeword would fit
		//assert(remaining == 0 || (max_code_size - codesize) < codebook[codebook_length - 1].size);

		// Store the composite run length in the lookup table
		table[i].bits = codeword;
		table[i].size = codesize;
		table[i].count = length - remaining;
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Fill lookup table for encoding coefficient magnitudes

	The table for encoding coefficient magnitudes is indexed by the magnitude.
	Each entry is a codeword and the size in bits.

	@todo Implement cubic companding
*/
CODEC_ERROR FillMagnitudeEncodingTable(const CODEBOOK *codebook, VLE *mags_table_entry, int mags_table_length, uint32_t flags)
{
	// Get the length of the codebook and a pointer to the entries
	//int codebook_length = codebook->length;
	RLV *codebook_entry = (RLV *)((uint8_t *)codebook + sizeof(CODEBOOK));

	int32_t maximum_magnitude_value = 0;

	uint32_t codebook_index;

	int16_t cubic_table[1025];
	int cubic_table_length = sizeof(cubic_table) / sizeof(cubic_table[0]);

	int mags_table_index;

	// Find the maximum coefficient magnitude in the codebook
	for (codebook_index = 0; codebook_index < codebook->length; codebook_index++)
	{
		// Does this codebook entry correspond to a coefficient magnitude?
		if (codebook_entry[codebook_index].count == 1)
		{
			int32_t codebook_value = codebook_entry[codebook_index].value;
			if (maximum_magnitude_value < codebook_value) {
				maximum_magnitude_value = codebook_value;
			}
		}
	}
	assert(maximum_magnitude_value > 0);

	if (flags & CODESET_FLAGS_COMPANDING_CUBIC)
	{
		ComputeCubicTable(cubic_table, cubic_table_length, maximum_magnitude_value);
	}

	// Fill each table entry with the codeword for that (signed) value
	for (mags_table_index = 0; mags_table_index < mags_table_length; mags_table_index++)
	{
		// Compute the magnitude that corresponds to this index
		int32_t magnitude = mags_table_index;
		uint32_t codeword;
		int codesize;

		// Apply the companding curve
		if (flags & CODESET_FLAGS_COMPANDING_CUBIC)
		{
			// Apply a cubic companding curve
			assert(magnitude < cubic_table_length);
			magnitude = cubic_table[magnitude];
		}
		else if (flags & CODESET_FLAGS_COMPANDING_NONE)
		{
			// Do not apply a companding curve
		}
		else
		{
			// Apply an old-style companding curve
			magnitude = CompandedValue(magnitude);
		}

		// Is the magnitude larger than the number of entries in the codebook?
		if (magnitude > maximum_magnitude_value) {
			magnitude = maximum_magnitude_value;
		}

		// Find the codebook entry corresponding to this coefficient magnitude
        codeword = 0;
		codesize = 0;
		for (codebook_index = 0; codebook_index < codebook->length; codebook_index++)
		{
			if (codebook_entry[codebook_index].value == magnitude)
			{
				assert(codebook_entry[codebook_index].count == 1);
				codeword = codebook_entry[codebook_index].bits;
				codesize = codebook_entry[codebook_index].size;
				break;
			}
		}
		assert(0 < codesize && codesize <= 32);

		mags_table_entry[mags_table_index].bits = codeword;
		mags_table_entry[mags_table_index].size = codesize;

	}

	return CODEC_ERROR_OKAY;
}
