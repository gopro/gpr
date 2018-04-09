/*! @file wavelet.c
 *
 *  @brief Implementation of the module for wavelet data structures and transforms
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
	@brief Apply the inverse wavelet transform to reconstruct a lowpass band

	This routine reconstructs the lowpass band in the output wavelet from the
	decoded bands in the input wavelet.  The prescale argument is used to undo
	scaling that may have been performed during encoding to prevent overflow.
	
	@todo Replace the two different routines for different prescale shifts with
	a single routine that can handle any prescale shift.
*/
CODEC_ERROR TransformInverseSpatialQuantLowpass(gpr_allocator *allocator, WAVELET *input, WAVELET *output, uint16_t prescale)
{
	// Dimensions of each wavelet band
	DIMENSION input_width = input->width;
	DIMENSION input_height = input->height;

	// The output width may be less than twice the input width if the output width is odd
	DIMENSION output_width = output->width;

	// The output height may be less than twice the input height if the output height is odd
	DIMENSION output_height = output->height;

	// Check that a valid input image has been provided
	assert(input != NULL);
	assert(input->data[0] != NULL);
	assert(input->data[1] != NULL);
	assert(input->data[2] != NULL);
	assert(input->data[3] != NULL);

	// Check that the output image is a gray image or a lowpass wavelet band
	assert(output->data[0] != NULL);

	// Check for valid quantization values
	if (input->quant[0] == 0) {
		input->quant[0] = 1;
	}

	assert(input->quant[0] > 0);
	assert(input->quant[1] > 0);
	assert(input->quant[2] > 0);
	assert(input->quant[3] > 0);

	if (prescale > 1)
	{
		// This is a spatial transform for the lowpass temporal band
		assert(prescale == 2);

		// Apply the inverse spatial transform for a lowpass band that is not prescaled
		InvertSpatialQuantDescale16s(allocator,
									 (PIXEL *)input->data[0], input->pitch,
									 (PIXEL *)input->data[1], input->pitch,
									 (PIXEL *)input->data[2], input->pitch,
									 (PIXEL *)input->data[3], input->pitch,
									 output->data[0], output->pitch,
									 input_width, input_height,
									 output_width, output_height,
									 prescale, input->quant);
	}
	else
	{
		// This case does not handle any prescaling applied during encoding
		assert(prescale == 0);

		// Apply the inverse spatial transform for a lowpass band that is not prescaled
		InvertSpatialQuant16s(allocator,
							  (PIXEL *)input->data[0], input->pitch,
							  (PIXEL *)input->data[1], input->pitch,
							  (PIXEL *)input->data[2], input->pitch,
							  (PIXEL *)input->data[3], input->pitch,
							  output->data[0], output->pitch,
							  input_width, input_height,
							  output_width, output_height,
							  input->quant);
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Apply the inverse wavelet transform to reconstruct a component array

	This routine reconstructs the lowpass band in the output wavelet from the
	decoded bands in the input wavelet.  The prescale argument is used to undo
	scaling that may have been performed during encoding to prevent overflow.
*/
CODEC_ERROR TransformInverseSpatialQuantArray(gpr_allocator *allocator,
											  WAVELET *input,
											  COMPONENT_VALUE *output_buffer,
											  DIMENSION output_width,
											  DIMENSION output_height,
											  size_t output_pitch,
											  PRESCALE prescale)
{
	// Dimensions of each wavelet band
	DIMENSION input_width = input->width;
	DIMENSION input_height = input->height;

	// Check that a valid input image has been provided
	assert(input != NULL);
	assert(input->data[0] != NULL);
	assert(input->data[1] != NULL);
	assert(input->data[2] != NULL);
	assert(input->data[3] != NULL);

	// Check for valid quantization values
	if (input->quant[0] == 0) {
		input->quant[0] = 1;
	}

	assert(input->quant[0] > 0);
	assert(input->quant[1] > 0);
	assert(input->quant[2] > 0);
	assert(input->quant[3] > 0);

	assert(output_width > 0 && output_height > 0 && output_pitch > 0 && output_buffer != NULL);

	if (prescale > 1)
	{
		// This is a spatial transform for the lowpass temporal band
		assert(prescale == 2);

		// Apply the inverse spatial transform for a lowpass band that is not prescaled
		InvertSpatialQuantDescale16s(allocator,
									 (PIXEL *)input->data[0], input->pitch,
									 (PIXEL *)input->data[1], input->pitch,
									 (PIXEL *)input->data[2], input->pitch,
									 (PIXEL *)input->data[3], input->pitch,
									 (PIXEL *)output_buffer, (int)output_pitch,
									 input_width, input_height,
									 output_width, output_height,
									 prescale, input->quant);
	}
	else
	{
		// This case does not handle any prescaling applied during encoding
		assert(prescale == 0);

		// Apply the inverse spatial transform for a lowpass band that is not prescaled
		InvertSpatialQuant16s(allocator,
							  (PIXEL *)input->data[0], input->pitch,
							  (PIXEL *)input->data[1], input->pitch,
							  (PIXEL *)input->data[2], input->pitch,
							  (PIXEL *)input->data[3], input->pitch,
							  (PIXEL *)output_buffer, (int)output_pitch,
							  input_width, input_height,
							  output_width, output_height,
							  input->quant);
	}

	return CODEC_ERROR_OKAY;
}
