/*! @file dequantize.c
 *
 *  @brief Implementation of inverse quantization functions
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

#if ENABLED(NEON)
#include <arm_neon.h>
#endif

// Not using midpoint correction in dequantization
static const int midpoint = 0;

/*!
	@brief Dequantize a band with the specified dimensions

	The companding curve is inverted and the value is multiplied by the
	quantization value that was used by the encoder to compress the band.
*/

#if ENABLED(NEON)

CODEC_ERROR DequantizeBandRow16s(PIXEL *input, int width, int quantization, PIXEL *output)
{
	int column = 0;
	const int width_m4 = (width / 4) * 4;
	const int32x4_t zero = vdupq_n_s32(0);
	const int32x4_t quant_vec = vdupq_n_s32(quantization);

	// Process 4 pixels at a time
	for (; column < width_m4; column += 4)
	{
		int32x4_t values = vld1q_s32(&input[column]);

		// Scalar LUT lookups for uncompanding (NEON can't do 32-bit table lookups)
		int32_t v0 = vgetq_lane_s32(values, 0);
		int32_t v1 = vgetq_lane_s32(values, 1);
		int32_t v2 = vgetq_lane_s32(values, 2);
		int32_t v3 = vgetq_lane_s32(values, 3);

		int32_t u0 = UncompandedValueFast(v0);
		int32_t u1 = UncompandedValueFast(v1);
		int32_t u2 = UncompandedValueFast(v2);
		int32_t u3 = UncompandedValueFast(v3);

		// Load uncompanded values into vector
		int32_t uncomp[4] = { u0, u1, u2, u3 };
		int32x4_t uncompanded = vld1q_s32(uncomp);

		// Get absolute values and compute signs
		int32x4_t abs_vals = vabsq_s32(uncompanded);
		uint32x4_t neg_mask = vcltq_s32(uncompanded, zero);

		// Multiply by quantization
		int32x4_t dequant = vmulq_s32(abs_vals, quant_vec);

		// Restore signs
		int32x4_t negated = vnegq_s32(dequant);
		int32x4_t result = vbslq_s32(neg_mask, negated, dequant);

		vst1q_s32(&output[column], result);
	}

	// Scalar cleanup for remaining pixels
	for (; column < width; column++)
	{
		int32_t value = input[column];
		value = UncompandedValueFast(value);

		if (value > 0)
			value = (quantization * value) + midpoint;
		else if (value < 0)
		{
			value = neg(value);
			value = (quantization * value) + midpoint;
			value = neg(value);
		}

		output[column] = ClampPixel(value);
	}

	return CODEC_ERROR_OKAY;
}

#else

CODEC_ERROR DequantizeBandRow16s(PIXEL *input, int width, int quantization, PIXEL *output)
{
	int column;

	// Undo quantization in the entire row
	for (column = 0; column < width; column++)
	{
		int32_t value = input[column];

		// Invert the companding curve using fast LUT
		value = UncompandedValueFast(value);

		// Dequantize the absolute value
		if (value > 0)
		{
			value = (quantization * value) + midpoint;
		}
		else if (value < 0)
		{
			value = neg(value);
			value = (quantization * value) + midpoint;
			value = neg(value);
		}

		// Store the dequantized coefficient
		output[column] = ClampPixel(value);
	}

	return CODEC_ERROR_OKAY;
}

#endif

/*!
	@brief This function dequantizes the pixel value

	The inverse companding curve is applied to convert the pixel value
	to its quantized value and then the pixel value is multiplied by
	the quantization parameter.
*/
PIXEL DequantizedValue(int32_t value, int quantization)
{
	// Invert the companding curve using fast LUT
	value = UncompandedValueFast(value);

	// Dequantize the absolute value
	if (value > 0)
	{
		value = (quantization * value) + midpoint;
	}
	else if (value < 0)
	{
		value = neg(value);
		value = (quantization * value) + midpoint;
		value = neg(value);
	}

	return ClampPixel(value);
}
