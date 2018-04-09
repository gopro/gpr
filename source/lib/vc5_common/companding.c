/*! @file companding.c
 *
 *  @brief Implementation of the routines for computing the companding curves
 *  that is applied to the quantized coefficient magnitudes.
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

//TODO: Change the companding parameter into a global constant

#ifndef COMPANDING
#define COMPANDING		1
#define COMPANDING_MORE	(54)	// Zero means no companding (54 is a good value)
#endif


/*!
	@brief Maximum coefficient magnitude in the codebook
	
	@todo Need to calculate the maximum value from the codebook
*/
const int maximum_codebook_value = 255;


/*!
	@brief Apply the default companding curve to the specified value

	Note that this companding curve has been superceeded by the cubic curve.
*/
int32_t CompandedValue(int32_t value)
{
	const int midpoint_rounding = 2;

	int32_t magnitude = absolute(value);

#if COMPANDING
	if (magnitude >= 40)
	{
		magnitude -= 40;
		magnitude += midpoint_rounding;
		magnitude >>= 2;
		magnitude += 40;

 #if COMPANDING_MORE
		if (magnitude >= COMPANDING_MORE)
		{
			magnitude -= COMPANDING_MORE;
			magnitude += midpoint_rounding;
			magnitude >>= 2;
			magnitude += COMPANDING_MORE;
		}
#endif
	}
#endif

	// Restore the sign to the companded value
	return ((value >= 0) ? magnitude : neg(magnitude));
}

/*!
	@brief Return the parameter that controls the companding curve

	This parameter does not apply if the cubic companding curve is used.
*/
uint32_t CompandingParameter()
{
	return COMPANDING_MORE;
}

/*!
	@brief Compute a table of values for the cubic companding curve

	The companding curve is f(x) = x + (x ^ 3 / (255 ^ 3)) * 768
	so the range of coefficient magnitudes from 0 to 255 becomess 0 to 1023.
*/
CODEC_ERROR ComputeCubicTable(int16_t cubic_table[], int cubic_table_length, int16_t maximum_value)
{
	size_t cubic_table_size = cubic_table_length * sizeof(cubic_table[0]);
	int last_cubic_table_index = cubic_table_length - 2;
	int16_t last_magnitude;
	int16_t index;

	// Clear the cubic table
	memset(cubic_table, 0, cubic_table_size);

	for (index = 1; index <= maximum_value; index++)
	{
		double cubic = index;

		int magnitude = index;

		cubic *= index;
		cubic *= index;
		cubic *= 768;
		//cubic /= 256*256*256;
		cubic /= 255*255*255;

		magnitude += (int)cubic;

		//if (mag > 1023) mag = 1023;
		if (magnitude > last_cubic_table_index) {
			magnitude = last_cubic_table_index;
		}

		cubic_table[magnitude] = index;
	}

	// Fill unused entries in the cubic table
	last_magnitude = 0;
	for (index = 0; index < cubic_table_length; index++)
	{
		if (cubic_table[index])
		{
			last_magnitude = cubic_table[index];
		}
		else
		{
			cubic_table[index] = last_magnitude;
		}
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Invert the companding curve applied during encoding
*/
int32_t UncompandedValue(int32_t value)
{
#if 1	//CUBIC_COMPANDING
	double cubic;
	int32_t magnitude = absolute(value);

	cubic = magnitude;
	cubic *= magnitude;
	cubic *= magnitude;
	cubic *= 768;
	//cubic /= 256*256*256;
	cubic /= 255*255*255;

	magnitude += (int32_t)cubic;

	// Restore the sign
	value = ((value < 0) ? neg(magnitude) : magnitude);
#else
	if (40 <= value && value < 264)
	{
#if COMPANDING_MORE
		if (value >= COMPANDING_MORE)
		{
			value -= COMPANDING_MORE;
			value <<= 2;
			value += COMPANDING_MORE;
		}
#endif
		value -= 40;
		value <<= 2;
		value += 40;
	}
	else if (value <= -40)
	{
		// Apply the inverse companding formula to the absolute value
		value = -value;

#if COMPANDING_MORE
		if (value >= COMPANDING_MORE)
		{
			value -= COMPANDING_MORE;
			value <<= 2;
			value += COMPANDING_MORE;
		}
#endif
		value -= 40;
		value <<= 2;
		value += 40;

		// Restore the sign
		value = -value;
	}
#endif

	return value;
}

/*!
	@brief Invert the companding curve applied to a pixel
*/
PIXEL UncompandedPixel(PIXEL value)
{
#if 1	//CUBIC_COMPANDING
	double cubic;
	int32_t magnitude = absolute(value);

	cubic = magnitude;
	cubic *= magnitude;
	cubic *= magnitude;
	cubic *= 768;
	//cubic /= 256*256*256;
	cubic /= 255*255*255;

	magnitude += (int32_t)cubic;

	// Restore the sign
	value = ClampPixel((value < 0) ? neg(magnitude) : magnitude);
#else
	if (40 <= value && value < 264)
	{
#if COMPANDING_MORE
		if (value >= COMPANDING_MORE)
		{
			value -= COMPANDING_MORE;
			value <<= 2;
			value += COMPANDING_MORE;
		}
#endif
		value -= 40;
		value <<= 2;
		value += 40;
	}
	else if (value <= -40)
	{
		// Apply the inverse companding formula to the absolute value
		value = neg(value);

#if COMPANDING_MORE
		if (value >= COMPANDING_MORE)
		{
			value -= COMPANDING_MORE;
			value <<= 2;
			value += COMPANDING_MORE;
		}
#endif
		value -= 40;
		value <<= 2;
		value += 40;

		// Restore the sign
		value = neg(value);
	}
#endif

	return value;
}

