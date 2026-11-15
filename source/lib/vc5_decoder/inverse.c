/*! @file inverse.c
 *
 *  @brief Implementation of the inverse wavelet transforms.
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

//! Rounding adjustment used by the inverse wavelet transforms
static const int32_t rounding = 4;

/*!
 @brief Apply the inverse horizontal wavelet transform
 This routine applies the inverse wavelet transform to a row of
 lowpass and highpass coefficients, producing an output row that
 is write as wide.
 */
STATIC CODEC_ERROR InvertHorizontal16s(PIXEL *lowpass,            //!< Horizontal lowpass coefficients
                                       PIXEL *highpass,        //!< Horizontal highpass coefficients
                                       PIXEL *output,            //!< Row of reconstructed results
                                       DIMENSION input_width,    //!< Number of values in the input row
                                       DIMENSION output_width  //!< Number of values in the output row
)
{
    const int last_column = input_width - 1;
    
    int32_t even;
    int32_t odd;
    
    // Start processing at the beginning of the row
    int column = 0;
    
    // Process the first two output points with special filters for the left border
    even = 0;
    odd = 0;
    
    // Apply the even reconstruction filter to the lowpass band
    even += 11 * lowpass[column + 0];
    even -=  4 * lowpass[column + 1];
    even +=  1 * lowpass[column + 2];
    even += rounding;
    even = DivideByShift(even, 3);
    
    // Add the highpass correction
    even += highpass[column];
    even >>= 1;
    
    // The lowpass result should be a positive number
    //assert(0 <= even && even <= INT16_MAX);
    
    // Apply the odd reconstruction filter to the lowpass band
    odd += 5 * lowpass[column + 0];
    odd += 4 * lowpass[column + 1];
    odd -= 1 * lowpass[column + 2];
    odd += rounding;
    odd = DivideByShift(odd, 3);
    
    // Subtract the highpass correction
    odd -= highpass[column];
    odd >>= 1;
    
    // The lowpass result should be a positive number
    //assert(0 <= odd && odd <= INT16_MAX);
    
    // Store the last two output points produced by the loop
    output[2 * column + 0] = clamp_uint14(even);
    output[2 * column + 1] = clamp_uint14(odd);
    
    // Advance to the next input column (second pair of output values)
    column++;
    
    // Process the rest of the columns up to the last column in the row
    for (; column < last_column; column++)
    {
        int32_t even = 0;        // Result of convolution with even filter
        int32_t odd = 0;        // Result of convolution with odd filter
        
        // Apply the even reconstruction filter to the lowpass band
        
        even += lowpass[column - 1];
        even -= lowpass[column + 1];
        even += 4;
        even >>= 3;
        even += lowpass[column + 0];
        
        // Add the highpass correction
        even += highpass[column];
        even >>= 1;
        
        // The lowpass result should be a positive number
        //assert(0 <= even && even <= INT16_MAX);
        
        // Place the even result in the even column
        //output[2 * column + 0] = clamp_uint12(even);
        output[2 * column + 0] = clamp_uint14(even);
        
        // Apply the odd reconstruction filter to the lowpass band
        odd -= lowpass[column - 1];
        odd += lowpass[column + 1];
        odd += 4;
        odd >>= 3;
        odd += lowpass[column + 0];
        
        // Subtract the highpass correction
        odd -= highpass[column];
        odd >>= 1;
        
        // The lowpass result should be a positive number
        //assert(0 <= odd && odd <= INT16_MAX);
        
        // Place the odd result in the odd column
        //output[2 * column + 1] = clamp_uint14(odd);
        output[2 * column + 1] = clamp_uint14(odd);
    }
    
    // Should have exited the loop at the column for right border processing
    assert(column == last_column);
    
    // Process the last two output points with special filters for the right border
    even = 0;
    odd = 0;
    
    // Apply the even reconstruction filter to the lowpass band
    even += 5 * lowpass[column + 0];
    even += 4 * lowpass[column - 1];
    even -= 1 * lowpass[column - 2];
    even += rounding;
    even = DivideByShift(even, 3);
    
    // Add the highpass correction
    even += highpass[column];
    even >>= 1;
    
    // The lowpass result should be a positive number
    //assert(0 <= even && even <= INT16_MAX);
    
    // Place the even result in the even column
    output[2 * column + 0] = clamp_uint14(even);
    
    if (2 * column + 1 < output_width)
    {
        // Apply the odd reconstruction filter to the lowpass band
        odd += 11 * lowpass[column + 0];
        odd -=  4 * lowpass[column - 1];
        odd +=  1 * lowpass[column - 2];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        
        // Subtract the highpass correction
        odd -= highpass[column];
        odd >>= 1;
        
        // The lowpass result should be a positive number
        //assert(0 <= odd && odd <= INT16_MAX);
        
        // Place the odd result in the odd column
        output[2 * column + 1] = clamp_uint14(odd);
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
 @brief Apply the inverse horizontal wavelet transform
 This routine is similar to @ref InvertHorizontal16s, but a scale factor
 that was applied during encoding is removed from the output values.
 */
STATIC CODEC_ERROR InvertHorizontalDescale16s(PIXEL *lowpass, PIXEL *highpass, PIXEL *output,
                                              DIMENSION input_width, DIMENSION output_width,
                                              int descale)
{
    const int last_column = input_width - 1;
    
    // Start processing at the beginning of the row
    int column = 0;
    
    int descale_shift = 0;
    
    int32_t even;
    int32_t odd;
    
    /*
     The implementation of the inverse filter includes descaling by a factor of two
     because the last division by two in the computation of the even and odd results
     that is performed using a right arithmetic shift has been omitted from the code.
     */
    if (descale == 2) {
        descale_shift = 1;
    }
    
    // Check that the descaling value is reasonable
    assert(descale_shift >= 0);
    
    // Process the first two output points with special filters for the left border
    even = 0;
    odd = 0;
    
    // Apply the even reconstruction filter to the lowpass band
    even += 11 * lowpass[column + 0];
    even -=  4 * lowpass[column + 1];
    even +=  1 * lowpass[column + 2];
    even += rounding;
    even = DivideByShift(even, 3);
    
    // Add the highpass correction
    even += highpass[column];
    
    // Remove any scaling used during encoding
    even *= (1 << descale_shift);
    
    // The lowpass result should be a positive number
    //assert(0 <= even && even <= INT16_MAX);
    
    // Apply the odd reconstruction filter to the lowpass band
    odd += 5 * lowpass[column + 0];
    odd += 4 * lowpass[column + 1];
    odd -= 1 * lowpass[column + 2];
    odd += rounding;
    odd = DivideByShift(odd, 3);
    
    // Subtract the highpass correction
    odd -= highpass[column];
    
    // Remove any scaling used during encoding
    odd *= (1 << descale_shift);
    
    // The lowpass result should be a positive number
    //assert(0 <= odd && odd <= INT16_MAX);
    
    output[2 * column + 0] = ClampPixel(even);
    output[2 * column + 1] = ClampPixel(odd);
    
    // Advance to the next input column (second pair of output values)
    column++;
    
    // Process the rest of the columns up to the last column in the row
    for (; column < last_column; column++)
    {
        int32_t even = 0;        // Result of convolution with even filter
        int32_t odd = 0;        // Result of convolution with odd filter
        
        // Apply the even reconstruction filter to the lowpass band
        even += lowpass[column - 1];
        even -= lowpass[column + 1];
        even += 4;
        even >>= 3;
        even += lowpass[column + 0];
        
        // Add the highpass correction
        even += highpass[column];
        
        // Remove any scaling used during encoding
        even *= (1 << descale_shift);
        
        // The lowpass result should be a positive number
        //assert(0 <= even && even <= INT16_MAX);
        
        // Place the even result in the even column
        output[2 * column + 0] = ClampPixel(even);
        
        // Apply the odd reconstruction filter to the lowpass band
        odd -= lowpass[column - 1];
        odd += lowpass[column + 1];
        odd += 4;
        odd >>= 3;
        odd += lowpass[column + 0];
        
        // Subtract the highpass correction
        odd -= highpass[column];
        
        // Remove any scaling used during encoding
        odd *= (1 << descale_shift);
        
        // The lowpass result should be a positive number
        //assert(0 <= odd && odd <= INT16_MAX);
        
        // Place the odd result in the odd column
        output[2 * column + 1] = ClampPixel(odd);
    }
    
    // Should have exited the loop at the column for right border processing
    assert(column == last_column);
    
    // Process the last two output points with special filters for the right border
    even = 0;
    odd = 0;
    
    // Apply the even reconstruction filter to the lowpass band
    even += 5 * lowpass[column + 0];
    even += 4 * lowpass[column - 1];
    even -= 1 * lowpass[column - 2];
    even += rounding;
    even = DivideByShift(even, 3);
    
    // Add the highpass correction
    even += highpass[column];
    
    // Remove any scaling used during encoding
    even *= (1 << descale_shift);
    
    // The lowpass result should be a positive number
    //assert(0 <= even && even <= INT16_MAX);
    
    // Place the even result in the even column
    output[2 * column + 0] = ClampPixel(even);
    
    if (2 * column + 1 < output_width)
    {
        // Apply the odd reconstruction filter to the lowpass band
        odd += 11 * lowpass[column + 0];
        odd -=  4 * lowpass[column - 1];
        odd +=  1 * lowpass[column - 2];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        
        // Subtract the highpass correction
        odd -= highpass[column];
        
        // Remove any scaling used during encoding
        odd *= (1 << descale_shift);
        
        // The lowpass result should be a positive number
        //assert(0 <= odd && odd <= INT16_MAX);
        
        // Place the odd result in the odd column
        output[2 * column + 1] = ClampPixel(odd);
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Apply the inverse spatial wavelet filter
	Dequantize the coefficients in the highpass bands and apply the
	inverse spatial wavelet filter to compute a lowpass band that
	has twice the width and height of the input bands.
	The inverse vertical filter is applied to the upper and lower bands
	on the left and the upper and lower bands on the right.  The inverse
	horizontal filter is applied to the left and right (lowpass and highpass)
	results from the vertical inverse.  Each application of the inverse
	vertical filter produces two output rows and each application of the
	inverse horizontal filter produces an output row that is twice as wide.
	The inverse wavelet filter is a three tap filter.
	
	For the even output values, add and subtract the off-center values,
	add the rounding correction, and divide by eight, then add the center
	value, add the highpass coefficient, and divide by two.
	
	For the odd output values, the add and subtract operations for the
	off-center values are reversed the the highpass coefficient is subtracted.
	Divisions are implemented by right arithmetic shifts.
	Special formulas for the inverse vertical filter are applied to the top
	and bottom rows.
 */
CODEC_ERROR InvertSpatialQuant16s(gpr_allocator *allocator,
                                  PIXEL *lowlow_band, int lowlow_pitch,
                                  PIXEL *lowhigh_band, int lowhigh_pitch,
                                  PIXEL *highlow_band, int highlow_pitch,
                                  PIXEL *highhigh_band, int highhigh_pitch,
                                  PIXEL *output_image, int output_pitch,
                                  DIMENSION input_width, DIMENSION input_height,
                                  DIMENSION output_width, DIMENSION output_height,
                                  QUANT quantization[])
{
    PIXEL *lowlow = (PIXEL *)lowlow_band;
    PIXEL *lowhigh = lowhigh_band;
    PIXEL *highlow = highlow_band;
    PIXEL *highhigh = highhigh_band;
    PIXEL *output = output_image;
    PIXEL *even_lowpass;
    PIXEL *even_highpass;
    PIXEL *odd_lowpass;
    PIXEL *odd_highpass;
    PIXEL *even_output;
    PIXEL *odd_output;
    size_t buffer_row_size;
    int last_row = input_height - 1;
    int row, column;
    
    PIXEL *lowhigh_row[3];
    
    PIXEL *lowhigh_line[3];
    PIXEL *highlow_line;
    PIXEL *highhigh_line;
    
    QUANT highlow_quantization = quantization[HL_BAND];
    QUANT lowhigh_quantization = quantization[LH_BAND];
    QUANT highhigh_quantization = quantization[HH_BAND];
    
    // Compute positions within the temporary buffer for each row of horizontal lowpass
    // and highpass intermediate coefficients computed by the vertical inverse transform
    buffer_row_size = input_width * sizeof(PIXEL);
    
    // Compute the positions of the even and odd rows of coefficients
    even_lowpass = (PIXEL *)allocator->Alloc(buffer_row_size);
    even_highpass = (PIXEL *)allocator->Alloc(buffer_row_size);
    odd_lowpass = (PIXEL *)allocator->Alloc(buffer_row_size);
    odd_highpass = (PIXEL *)allocator->Alloc(buffer_row_size);
    
    // Compute the positions of the dequantized highpass rows
    lowhigh_line[0] = (PIXEL *)allocator->Alloc(buffer_row_size);
    lowhigh_line[1] = (PIXEL *)allocator->Alloc(buffer_row_size);
    lowhigh_line[2] = (PIXEL *)allocator->Alloc(buffer_row_size);
    highlow_line = (PIXEL *)allocator->Alloc(buffer_row_size);
    highhigh_line = (PIXEL *)allocator->Alloc(buffer_row_size);
    if (!even_lowpass || !even_highpass || !odd_lowpass || !odd_highpass ||
        !lowhigh_line[0] || !lowhigh_line[1] || !lowhigh_line[2] || !highlow_line || !highhigh_line) {
        if (even_lowpass) allocator->Free(even_lowpass);
        if (even_highpass) allocator->Free(even_highpass);
        if (odd_lowpass) allocator->Free(odd_lowpass);
        if (odd_highpass) allocator->Free(odd_highpass);
        if (lowhigh_line[0]) allocator->Free(lowhigh_line[0]);
        if (lowhigh_line[1]) allocator->Free(lowhigh_line[1]);
        if (lowhigh_line[2]) allocator->Free(lowhigh_line[2]);
        if (highlow_line) allocator->Free(highlow_line);
        if (highhigh_line) allocator->Free(highhigh_line);
        return CODEC_ERROR_OUTOFMEMORY;
    }
    
    // Convert pitch from bytes to pixels
    lowlow_pitch /= sizeof(PIXEL);
    lowhigh_pitch /= sizeof(PIXEL);
    highlow_pitch /= sizeof(PIXEL);
    highhigh_pitch /= sizeof(PIXEL);
    output_pitch /= sizeof(PIXEL);
    
    // Initialize the pointers to the even and odd output rows
    even_output = output;
    odd_output = output + output_pitch;
    
    // Apply the vertical border filter to the first row
    row = 0;
    
    // Set pointers to the first three rows in the first highpass band
    lowhigh_row[0] = lowhigh + 0 * lowhigh_pitch;
    lowhigh_row[1] = lowhigh + 1 * lowhigh_pitch;
    lowhigh_row[2] = lowhigh + 2 * lowhigh_pitch;
    
    // Dequantize three rows of highpass coefficients in the first highpass band
    DequantizeBandRow16s(lowhigh_row[0], input_width, lowhigh_quantization, lowhigh_line[0]);
    DequantizeBandRow16s(lowhigh_row[1], input_width, lowhigh_quantization, lowhigh_line[1]);
    DequantizeBandRow16s(lowhigh_row[2], input_width, lowhigh_quantization, lowhigh_line[2]);
    
    // Dequantize one row of coefficients each in the second and third highpass bands
    DequantizeBandRow16s(highlow, input_width, highlow_quantization, highlow_line);
    DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);
    
    for (column = 0; column < input_width; column++)
    {
        int32_t even = 0;		// Result of convolution with even filter
        int32_t odd = 0;		// Result of convolution with odd filter
        
        
        /***** Compute the vertical inverse for the left two bands *****/
        
        // Apply the even reconstruction filter to the lowpass band
        even += 11 * lowlow[column + 0 * lowlow_pitch];
        even -=  4 * lowlow[column + 1 * lowlow_pitch];
        even +=  1 * lowlow[column + 2 * lowlow_pitch];
        even += rounding;
        even = DivideByShift(even, 3);
        
        // Add the highpass correction
        even += highlow_line[column];
        even >>= 1;
        
        // The inverse of the left two bands should be a positive number
        //assert(0 <= even && even <= INT16_MAX);
        
        // Place the even result in the even row
        even_lowpass[column] = ClampPixel(even);
        
        // Apply the odd reconstruction filter to the lowpass band
        odd += 5 * lowlow[column + 0 * lowlow_pitch];
        odd += 4 * lowlow[column + 1 * lowlow_pitch];
        odd -= 1 * lowlow[column + 2 * lowlow_pitch];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        
        // Subtract the highpass correction
        odd -= highlow_line[column];
        odd >>= 1;
        
        // The inverse of the left two bands should be a positive number
        //assert(0 <= odd && odd <= INT16_MAX);
        
        // Place the odd result in the odd row
        odd_lowpass[column] = ClampPixel(odd);
        
        
        /***** Compute the vertical inverse for the right two bands *****/
        
        even = 0;
        odd = 0;
        
        // Apply the even reconstruction filter to the lowpass band
        even += 11 * lowhigh_line[0][column];
        even -=  4 * lowhigh_line[1][column];
        even +=  1 * lowhigh_line[2][column];
        even += rounding;
        even = DivideByShift(even, 3);
        
        // Add the highpass correction
        even += highhigh_line[column];
        even >>= 1;
        
        // Place the even result in the even row
        even_highpass[column] = ClampPixel(even);
        
        // Apply the odd reconstruction filter to the lowpass band
        odd += 5 * lowhigh_line[0][column];
        odd += 4 * lowhigh_line[1][column];
        odd -= 1 * lowhigh_line[2][column];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        
        // Subtract the highpass correction
        odd -= highhigh_line[column];
        odd >>= 1;
        
        // Place the odd result in the odd row
        odd_highpass[column] = ClampPixel(odd);
    }
    
    // Apply the inverse horizontal transform to the even and odd rows
    InvertHorizontal16s(even_lowpass, even_highpass, even_output, input_width, output_width);
    InvertHorizontal16s(odd_lowpass, odd_highpass, odd_output, input_width, output_width);
    
    // Advance to the next pair of even and odd output rows
    even_output += 2 * output_pitch;
    odd_output += 2 * output_pitch;
    
    // Always advance the highpass row pointers
    highlow += highlow_pitch;
    highhigh += highhigh_pitch;
    
    // Advance the row index
    row++;
    
    // Process the middle rows using the interior reconstruction filters
    for (; row < last_row; row++)
    {
        // Dequantize one row from each of the two highpass bands
        DequantizeBandRow16s(highlow, input_width, highlow_quantization, highlow_line);
        DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);
        
        // Process the entire row
        for (column = 0; column < input_width; column++)
        {
            int32_t even = 0;		// Result of convolution with even filter
            int32_t odd = 0;		// Result of convolution with odd filter
            
            
            /***** Compute the vertical inverse for the left two bands *****/
            
            // Apply the even reconstruction filter to the lowpass band
            even += lowlow[column + 0 * lowlow_pitch];
            even -= lowlow[column + 2 * lowlow_pitch];
            even += 4;
            even >>= 3;
            even += lowlow[column + 1 * lowlow_pitch];
            
            // Add the highpass correction
            even += highlow_line[column];
            even >>= 1;
            
            // The inverse of the left two bands should be a positive number
            //assert(0 <= even && even <= INT16_MAX);
            
            // Place the even result in the even row
            even_lowpass[column] = ClampPixel(even);
            
            // Apply the odd reconstruction filter to the lowpass band
            odd -= lowlow[column + 0 * lowlow_pitch];
            odd += lowlow[column + 2 * lowlow_pitch];
            odd += 4;
            odd >>= 3;
            odd += lowlow[column + 1 * lowlow_pitch];
            
            // Subtract the highpass correction
            odd -= highlow_line[column];
            odd >>= 1;
            
            // The inverse of the left two bands should be a positive number
            //assert(0 <= odd && odd <= INT16_MAX);
            
            // Place the odd result in the odd row
            odd_lowpass[column] = ClampPixel(odd);
            
            
            /***** Compute the vertical inverse for the right two bands *****/
            
            even = 0;
            odd = 0;
            
            // Apply the even reconstruction filter to the lowpass band
            even += lowhigh_line[0][column];
            even -= lowhigh_line[2][column];
            even += 4;
            even >>= 3;
            even += lowhigh_line[1][column];
            
            // Add the highpass correction
            even += highhigh_line[column];
            even >>= 1;
            
            // Place the even result in the even row
            even_highpass[column] = ClampPixel(even);
            
            // Apply the odd reconstruction filter to the lowpass band
            odd -= lowhigh_line[0][column];
            odd += lowhigh_line[2][column];
            odd += 4;
            odd >>= 3;
            odd += lowhigh_line[1][column];
            
            // Subtract the highpass correction
            odd -= highhigh_line[column];
            odd >>= 1;
            
            // Place the odd result in the odd row
            odd_highpass[column] = ClampPixel(odd);
        }
        
        // Apply the inverse horizontal transform to the even and odd rows and descale the results
        InvertHorizontal16s(even_lowpass, even_highpass, even_output, input_width, output_width);
        InvertHorizontal16s(odd_lowpass, odd_highpass, odd_output, input_width, output_width);
        
        // Advance to the next input row in each band
        lowlow += lowlow_pitch;
        lowhigh += lowhigh_pitch;
        highlow += highlow_pitch;
        highhigh += highhigh_pitch;
        
        // Advance to the next pair of even and odd output rows
        even_output += 2 * output_pitch;
        odd_output += 2 * output_pitch;
        
        if (row < last_row - 1)
        {
            // Compute the address of the next row in the lowhigh band
            PIXEL *lowhigh_row_ptr = (lowhigh + 2 * lowhigh_pitch);
            //PIXEL *lowhigh_row_ptr = (lowhigh + lowhigh_pitch);
            
            // Shift the rows in the buffer of dequantized lowhigh bands
            PIXEL *temp = lowhigh_line[0];
            lowhigh_line[0] = lowhigh_line[1];
            lowhigh_line[1] = lowhigh_line[2];
            lowhigh_line[2] = temp;
            
            // Undo quantization for the next row in the lowhigh band
            DequantizeBandRow16s(lowhigh_row_ptr, input_width, lowhigh_quantization, lowhigh_line[2]);
        }
    }
    
    // Should have exited the loop at the last row
    assert(row == last_row);
    
    // Advance the lowlow pointer to the last row in the band
    lowlow += lowlow_pitch;
    
    // Check that the band pointers are on the last row in each wavelet band
    assert(lowlow == (lowlow_band + last_row * lowlow_pitch));
    
    assert(highlow == (highlow_band + last_row * highlow_pitch));
    assert(highhigh == (highhigh_band + last_row * highhigh_pitch));
    
    // Undo quantization for the highlow and highhigh bands
    DequantizeBandRow16s(highlow, input_width, highlow_quantization, highlow_line);
    DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);
    
    // Apply the vertical border filter to the last row
    for (column = 0; column < input_width; column++)
    {
        int32_t even = 0;		// Result of convolution with even filter
        int32_t odd = 0;		// Result of convolution with odd filter
        
        
        /***** Compute the vertical inverse for the left two bands *****/
        
        // Apply the even reconstruction filter to the lowpass band
        even += 5 * lowlow[column + 0 * lowlow_pitch];
        even += 4 * lowlow[column - 1 * lowlow_pitch];
        even -= 1 * lowlow[column - 2 * lowlow_pitch];
        even += 4;
        even = DivideByShift(even, 3);
        
        // Add the highpass correction
        even += highlow_line[column];
        even >>= 1;
        
        // The inverse of the left two bands should be a positive number
        //assert(0 <= even && even <= INT16_MAX);
        
        // Place the even result in the even row
        even_lowpass[column] = ClampPixel(even);
        
        // Apply the odd reconstruction filter to the lowpass band
        odd += 11 * lowlow[column + 0 * lowlow_pitch];
        odd -=  4 * lowlow[column - 1 * lowlow_pitch];
        odd +=  1 * lowlow[column - 2 * lowlow_pitch];
        odd += 4;
        odd = DivideByShift(odd, 3);
        
        // Subtract the highpass correction
        odd -= highlow_line[column];
        odd >>= 1;
        
        // The inverse of the left two bands should be a positive number
        //assert(0 <= odd && odd <= INT16_MAX);
        
        // Place the odd result in the odd row
        odd_lowpass[column] = ClampPixel(odd);
        
        
        // Compute the vertical inverse for the right two bands //
        
        even = 0;
        odd = 0;
        
        // Apply the even reconstruction filter to the lowpass band
        even += 5 * lowhigh_line[2][column];
        even += 4 * lowhigh_line[1][column];
        even -= 1 * lowhigh_line[0][column];
        even += 4;
        even = DivideByShift(even, 3);
        
        // Add the highpass correction
        even += highhigh_line[column];
        even >>= 1;
        
        // Place the even result in the even row
        even_highpass[column] = ClampPixel(even);
        
        // Apply the odd reconstruction filter to the lowpass band
        odd += 11 * lowhigh_line[2][column];
        odd -=  4 * lowhigh_line[1][column];
        odd +=  1 * lowhigh_line[0][column];
        odd += 4;
        odd = DivideByShift(odd, 3);
        
        // Subtract the highpass correction
        odd -= highhigh_line[column];
        odd >>= 1;
        
        // Place the odd result in the odd row
        odd_highpass[column] = ClampPixel(odd);
    }
    
    // Apply the inverse horizontal transform to the even and odd rows and descale the results
    InvertHorizontal16s(even_lowpass, even_highpass, even_output, input_width, output_width);
    
    // Is the output wavelet shorter than twice the height of the input wavelet?
    if (2 * row + 1 < output_height) {
        InvertHorizontal16s(odd_lowpass, odd_highpass, odd_output, input_width, output_width);
    }
    
    // Free the scratch buffers
    allocator->Free(even_lowpass);
    allocator->Free(even_highpass);
    allocator->Free(odd_lowpass);
    allocator->Free(odd_highpass);
    
    allocator->Free(lowhigh_line[0]);
    allocator->Free(lowhigh_line[1]);
    allocator->Free(lowhigh_line[2]);
    allocator->Free(highlow_line);
    allocator->Free(highhigh_line);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Apply the inverse spatial transform with descaling
	This routine is similar to @ref InvertSpatialQuant16s, but a scale factor
	that was applied during encoding is removed from the output values.
 */
CODEC_ERROR InvertSpatialQuantDescale16s(gpr_allocator *allocator,
                                         PIXEL *lowlow_band, int lowlow_pitch,
                                         PIXEL *lowhigh_band, int lowhigh_pitch,
                                         PIXEL *highlow_band, int highlow_pitch,
                                         PIXEL *highhigh_band, int highhigh_pitch,
                                         PIXEL *output_image, int output_pitch,
                                         DIMENSION input_width, DIMENSION input_height,
                                         DIMENSION output_width, DIMENSION output_height,
                                         int descale, QUANT quantization[])
{
    PIXEL *lowlow = lowlow_band;
    PIXEL *lowhigh = lowhigh_band;
    PIXEL *highlow = highlow_band;
    PIXEL *highhigh = highhigh_band;
    PIXEL *output = output_image;
    PIXEL *even_lowpass;
    PIXEL *even_highpass;
    PIXEL *odd_lowpass;
    PIXEL *odd_highpass;
    PIXEL *even_output;
    PIXEL *odd_output;
    size_t buffer_row_size;
    int last_row = input_height - 1;
    int row, column;
    
    PIXEL *lowhigh_row[3];
    
    PIXEL *lowhigh_line[3];
    PIXEL *highlow_line;
    PIXEL *highhigh_line;
    
    QUANT highlow_quantization = quantization[HL_BAND];
    QUANT lowhigh_quantization = quantization[LH_BAND];
    QUANT highhigh_quantization = quantization[HH_BAND];
    
    // Compute positions within the temporary buffer for each row of horizontal lowpass
    // and highpass intermediate coefficients computed by the vertical inverse transform
    buffer_row_size = input_width * sizeof(PIXEL);
    
    // Allocate space for the even and odd rows of results from the inverse vertical filter
    even_lowpass = (PIXEL *)allocator->Alloc(buffer_row_size);
    even_highpass = (PIXEL *)allocator->Alloc(buffer_row_size);
    odd_lowpass = (PIXEL *)allocator->Alloc(buffer_row_size);
    odd_highpass = (PIXEL *)allocator->Alloc(buffer_row_size);
    
    // Allocate scratch space for the dequantized highpass coefficients
    lowhigh_line[0] = (PIXEL *)allocator->Alloc(buffer_row_size);
    lowhigh_line[1] = (PIXEL *)allocator->Alloc(buffer_row_size);
    lowhigh_line[2] = (PIXEL *)allocator->Alloc(buffer_row_size);
    highlow_line = (PIXEL *)allocator->Alloc(buffer_row_size);
    highhigh_line = (PIXEL *)allocator->Alloc(buffer_row_size);
    if (!even_lowpass || !even_highpass || !odd_lowpass || !odd_highpass ||
        !lowhigh_line[0] || !lowhigh_line[1] || !lowhigh_line[2] || !highlow_line || !highhigh_line) {
        if (even_lowpass) allocator->Free(even_lowpass);
        if (even_highpass) allocator->Free(even_highpass);
        if (odd_lowpass) allocator->Free(odd_lowpass);
        if (odd_highpass) allocator->Free(odd_highpass);
        if (lowhigh_line[0]) allocator->Free(lowhigh_line[0]);
        if (lowhigh_line[1]) allocator->Free(lowhigh_line[1]);
        if (lowhigh_line[2]) allocator->Free(lowhigh_line[2]);
        if (highlow_line) allocator->Free(highlow_line);
        if (highhigh_line) allocator->Free(highhigh_line);
        return CODEC_ERROR_OUTOFMEMORY;
    }
    
    // Convert pitch from bytes to pixels
    lowlow_pitch /= sizeof(PIXEL);
    lowhigh_pitch /= sizeof(PIXEL);
    highlow_pitch /= sizeof(PIXEL);
    highhigh_pitch /= sizeof(PIXEL);
    output_pitch /= sizeof(PIXEL);
    
    // Initialize the pointers to the even and odd output rows
    even_output = output;
    odd_output = output + output_pitch;
    
    // Apply the vertical border filter to the first row
    row = 0;
    
    // Set pointers to the first three rows in the first highpass band
    lowhigh_row[0] = lowhigh + 0 * lowhigh_pitch;
    lowhigh_row[1] = lowhigh + 1 * lowhigh_pitch;
    lowhigh_row[2] = lowhigh + 2 * lowhigh_pitch;
    
    // Dequantize three rows of highpass coefficients in the first highpass band
    DequantizeBandRow16s(lowhigh_row[0], input_width, lowhigh_quantization, lowhigh_line[0]);
    DequantizeBandRow16s(lowhigh_row[1], input_width, lowhigh_quantization, lowhigh_line[1]);
    DequantizeBandRow16s(lowhigh_row[2], input_width, lowhigh_quantization, lowhigh_line[2]);
    
    // Dequantize one row of coefficients each in the second and third highpass bands
    DequantizeBandRow16s(highlow, input_width, highlow_quantization, highlow_line);
    DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);
    
    for (column = 0; column < input_width; column++)
    {
        int32_t even = 0;		// Result of convolution with even filter
        int32_t odd = 0;		// Result of convolution with odd filter
        
        
        /***** Compute the vertical inverse for the left two bands *****/
        
        // Apply the even reconstruction filter to the lowpass band
        even += 11 * lowlow[column + 0 * lowlow_pitch];
        even -=  4 * lowlow[column + 1 * lowlow_pitch];
        even +=  1 * lowlow[column + 2 * lowlow_pitch];
        even += rounding;
        even = DivideByShift(even, 3);
        
        // Add the highpass correction
        even += highlow_line[column];
        even = DivideByShift(even, 1);
        
        // The inverse of the left two bands should be a positive number
        //assert(0 <= even && even <= INT16_MAX);
        
        // Place the even result in the even row
        even_lowpass[column] = ClampPixel(even);
        
        // Apply the odd reconstruction filter to the lowpass band
        odd += 5 * lowlow[column + 0 * lowlow_pitch];
        odd += 4 * lowlow[column + 1 * lowlow_pitch];
        odd -= 1 * lowlow[column + 2 * lowlow_pitch];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        
        // Subtract the highpass correction
        odd -= highlow_line[column];
        odd = DivideByShift(odd, 1);
        
        // The inverse of the left two bands should be a positive number
        //assert(0 <= odd && odd <= INT16_MAX);
        
        // Place the odd result in the odd row
        odd_lowpass[column] = ClampPixel(odd);
        
        
        /***** Compute the vertical inverse for the right two bands *****/
        
        even = 0;
        odd = 0;
        
        // Apply the even reconstruction filter to the lowpass band
        even += 11 * lowhigh_line[0][column];
        even -=  4 * lowhigh_line[1][column];
        even +=  1 * lowhigh_line[2][column];
        even += rounding;
        even = DivideByShift(even, 3);
        
        // Add the highpass correction
        even += highhigh_line[column];
        even = DivideByShift(even, 1);
        
        // Place the even result in the even row
        even_highpass[column] = ClampPixel(even);
        
        // Apply the odd reconstruction filter to the lowpass band
        odd += 5 * lowhigh_line[0][column];
        odd += 4 * lowhigh_line[1][column];
        odd -= 1 * lowhigh_line[2][column];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        
        // Subtract the highpass correction
        odd -= highhigh_line[column];
        odd = DivideByShift(odd, 1);
        
        // Place the odd result in the odd row
        odd_highpass[column] = ClampPixel(odd);
    }
    
    // Apply the inverse horizontal transform to the even and odd rows and descale the results
    InvertHorizontalDescale16s(even_lowpass, even_highpass, even_output,
                               input_width, output_width, descale);
    
    InvertHorizontalDescale16s(odd_lowpass, odd_highpass, odd_output,
                               input_width, output_width, descale);
    
    // Advance to the next pair of even and odd output rows
    even_output += 2 * output_pitch;
    odd_output += 2 * output_pitch;
    
    // Always advance the highpass row pointers
    highlow += highlow_pitch;
    highhigh += highhigh_pitch;
    
    // Advance the row index
    row++;
    
    // Process the middle rows using the interior reconstruction filters
    for (; row < last_row; row++)
    {
        // Dequantize one row from each of the two highpass bands
        DequantizeBandRow16s(highlow, input_width, highlow_quantization, highlow_line);
        DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);
        
        // Process the entire row
        for (column = 0; column < input_width; column++)
        {
            int32_t even = 0;		// Result of convolution with even filter
            int32_t odd = 0;		// Result of convolution with odd filter
            
            
            /***** Compute the vertical inverse for the left two bands *****/
            
            // Apply the even reconstruction filter to the lowpass band
            even += lowlow[column + 0 * lowlow_pitch];
            even -= lowlow[column + 2 * lowlow_pitch];
            even += 4;
            even >>= 3;
            even += lowlow[column + 1 * lowlow_pitch];
            
            // Add the highpass correction
            even += highlow_line[column];
            even = DivideByShift(even, 1);
            
            // The inverse of the left two bands should be a positive number
            //assert(0 <= even && even <= INT16_MAX);
            
            // Place the even result in the even row
            even_lowpass[column] = ClampPixel(even);
            
            // Apply the odd reconstruction filter to the lowpass band
            odd -= lowlow[column + 0 * lowlow_pitch];
            odd += lowlow[column + 2 * lowlow_pitch];
            odd += 4;
            odd >>= 3;
            odd += lowlow[column + 1 * lowlow_pitch];
            
            // Subtract the highpass correction
            odd -= highlow_line[column];
            odd = DivideByShift(odd, 1);
            
            // The inverse of the left two bands should be a positive number
            //assert(0 <= odd && odd <= INT16_MAX);
            
            // Place the odd result in the odd row
            odd_lowpass[column] = ClampPixel(odd);
            
            
            /***** Compute the vertical inverse for the right two bands *****/
            
            even = 0;
            odd = 0;
            
            // Apply the even reconstruction filter to the lowpass band
            even += lowhigh_line[0][column];
            even -= lowhigh_line[2][column];
            even += 4;
            even >>= 3;
            even += lowhigh_line[1][column];
            
            // Add the highpass correction
            even += highhigh_line[column];
            even = DivideByShift(even, 1);
            
            // Place the even result in the even row
            even_highpass[column] = ClampPixel(even);
            
            // Apply the odd reconstruction filter to the lowpass band
            odd -= lowhigh_line[0][column];
            odd += lowhigh_line[2][column];
            odd += 4;
            odd >>= 3;
            odd += lowhigh_line[1][column];
            
            // Subtract the highpass correction
            odd -= highhigh_line[column];
            odd = DivideByShift(odd, 1);
            
            // Place the odd result in the odd row
            odd_highpass[column] = ClampPixel(odd);
        }
        
        // Apply the inverse horizontal transform to the even and odd rows and descale the results
        InvertHorizontalDescale16s(even_lowpass, even_highpass, even_output,
                                   input_width, output_width, descale);
        
        InvertHorizontalDescale16s(odd_lowpass, odd_highpass, odd_output,
                                   input_width, output_width, descale);
        
        // Advance to the next input row in each band
        lowlow += lowlow_pitch;
        lowhigh += lowhigh_pitch;
        highlow += highlow_pitch;
        highhigh += highhigh_pitch;
        
        // Advance to the next pair of even and odd output rows
        even_output += 2 * output_pitch;
        odd_output += 2 * output_pitch;
        
        if (row < last_row - 1)
        {
            // Compute the address of the next row in the lowhigh band
            PIXEL *lowhigh_row_ptr = (lowhigh + 2 * lowhigh_pitch);
            
            // Shift the rows in the buffer of dequantized lowhigh bands
            PIXEL *temp = lowhigh_line[0];
            lowhigh_line[0] = lowhigh_line[1];
            lowhigh_line[1] = lowhigh_line[2];
            lowhigh_line[2] = temp;
            
            // Undo quantization for the next row in the lowhigh band
            DequantizeBandRow16s(lowhigh_row_ptr, input_width, lowhigh_quantization, lowhigh_line[2]);
        }
    }
    
    // Should have exited the loop at the last row
    assert(row == last_row);
    
    // Advance the lowlow pointer to the last row in the band
    lowlow += lowlow_pitch;
    
    // Check that the band pointers are on the last row in each wavelet band
    assert(lowlow == (lowlow_band + last_row * lowlow_pitch));
    
    assert(highlow == (highlow_band + last_row * highlow_pitch));
    assert(highhigh == (highhigh_band + last_row * highhigh_pitch));
    
    // Undo quantization for the highlow and highhigh bands
    DequantizeBandRow16s(highlow, input_width, highlow_quantization, highlow_line);
    DequantizeBandRow16s(highhigh, input_width, highhigh_quantization, highhigh_line);
    
    // Apply the vertical border filter to the last row
    for (column = 0; column < input_width; column++)
    {
        int32_t even = 0;		// Result of convolution with even filter
        int32_t odd = 0;		// Result of convolution with odd filter
        
        
        /***** Compute the vertical inverse for the left two bands *****/
        
        // Apply the even reconstruction filter to the lowpass band
        even += 5 * lowlow[column + 0 * lowlow_pitch];
        even += 4 * lowlow[column - 1 * lowlow_pitch];
        even -= 1 * lowlow[column - 2 * lowlow_pitch];
        even += rounding;
        even = DivideByShift(even, 3);
        
        // Add the highpass correction
        even += highlow_line[column];
        even = DivideByShift(even, 1);
        
        // The inverse of the left two bands should be a positive number
        //assert(0 <= even && even <= INT16_MAX);
        
        // Place the even result in the even row
        even_lowpass[column] = ClampPixel(even);
        
        // Apply the odd reconstruction filter to the lowpass band
        odd += 11 * lowlow[column + 0 * lowlow_pitch];
        odd -=  4 * lowlow[column - 1 * lowlow_pitch];
        odd +=  1 * lowlow[column - 2 * lowlow_pitch];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        
        // Subtract the highpass correction
        odd -= highlow_line[column];
        odd = DivideByShift(odd, 1);
        
        // The inverse of the left two bands should be a positive number
        //assert(0 <= odd && odd <= INT16_MAX);
        
        // Place the odd result in the odd row
        odd_lowpass[column] = ClampPixel(odd);
        
        
        /***** Compute the vertical inverse for the right two bands *****/
        
        even = 0;
        odd = 0;
        
        // Apply the even reconstruction filter to the lowpass band
        even += 5 * lowhigh_line[2][column];
        even += 4 * lowhigh_line[1][column];
        even -= 1 * lowhigh_line[0][column];
        even += rounding;
        even = DivideByShift(even, 3);
        
        // Add the highpass correction
        even += highhigh_line[column];
        even = DivideByShift(even, 1);
        
        // Place the even result in the even row
        even_highpass[column] = ClampPixel(even);
        
        // Apply the odd reconstruction filter to the lowpass band
        odd += 11 * lowhigh_line[2][column];
        odd -=  4 * lowhigh_line[1][column];
        odd +=  1 * lowhigh_line[0][column];
        odd += rounding;
        odd = DivideByShift(odd, 3);
        
        // Subtract the highpass correction
        odd -= highhigh_line[column];
        odd = DivideByShift(odd, 1);
        
        // Place the odd result in the odd row
        odd_highpass[column] = ClampPixel(odd);
    }
    
    // Apply the inverse horizontal transform to the even and odd rows and descale the results
    InvertHorizontalDescale16s(even_lowpass, even_highpass, even_output,
                               input_width, output_width, descale);
    
    // Is the output wavelet shorter than twice the height of the input wavelet?
    if (2 * row + 1 < output_height) {
        InvertHorizontalDescale16s(odd_lowpass, odd_highpass, odd_output,
                                   input_width, output_width, descale);
    }
    
    // Free the scratch buffers
    allocator->Free(even_lowpass);
    allocator->Free(even_highpass);
    allocator->Free(odd_lowpass);
    allocator->Free(odd_highpass);
    
    allocator->Free(lowhigh_line[0]);
    allocator->Free(lowhigh_line[1]);
    allocator->Free(lowhigh_line[2]);
    allocator->Free(highlow_line);
    allocator->Free(highhigh_line);
    
    return CODEC_ERROR_OKAY;
}


/* Row-wise inverse building block for a bounded preview pipeline. The three
 * LL/LH rows are ordered top-to-bottom; HL/HH belong to the center output pair.
 * The caller supplies 9*input_width scratch samples and may omit the odd row
 * when the physical output height is odd. No allocation or band ownership. */
CODEC_ERROR gpr_preview_inverse_row_dequantized(const PIXEL *const ll[3],
    const PIXEL *const lh[3],const PIXEL *hl,const PIXEL *hh,
    DIMENSION width,DIMENSION output_width,int mode,uint16_t prescale,
    PIXEL *scratch,PIXEL *even_output,PIXEL *odd_output)
{
    if (!ll || !lh || !hl || !hh || !scratch || !even_output ||
        width<3 || (output_width!=2*width && output_width!=2*width-1) ||
        mode<0 || mode>2 || (prescale!=0 && prescale!=2))
        return CODEC_ERROR_BAD_ARGUMENT;
    for (int r=0;r<3;++r) if (!ll[r] || !lh[r]) return CODEC_ERROR_BAD_ARGUMENT;
    const PIXEL *const *dlh=lh;
    const PIXEL *dhl=hl,*dhh=hh;
    PIXEL *el=scratch,*eh=scratch+width;
    PIXEL *ol=scratch+2*width,*oh=scratch+3*width;
    for (int x=0;x<width;++x) {
        int32_t le,lo,he,ho;
        if (mode==0) {
            le=(11*ll[0][x]-4*ll[1][x]+ll[2][x]+4)>>3;
            lo=(5*ll[0][x]+4*ll[1][x]-ll[2][x]+4)>>3;
            he=(11*dlh[0][x]-4*dlh[1][x]+dlh[2][x]+4)>>3;
            ho=(5*dlh[0][x]+4*dlh[1][x]-dlh[2][x]+4)>>3;
        } else if (mode==2) {
            le=(5*ll[2][x]+4*ll[1][x]-ll[0][x]+4)>>3;
            lo=(11*ll[2][x]-4*ll[1][x]+ll[0][x]+4)>>3;
            he=(5*dlh[2][x]+4*dlh[1][x]-dlh[0][x]+4)>>3;
            ho=(11*dlh[2][x]-4*dlh[1][x]+dlh[0][x]+4)>>3;
        } else {
            le=((ll[0][x]-ll[2][x]+4)>>3)+ll[1][x];
            lo=((-ll[0][x]+ll[2][x]+4)>>3)+ll[1][x];
            he=((dlh[0][x]-dlh[2][x]+4)>>3)+dlh[1][x];
            ho=((-dlh[0][x]+dlh[2][x]+4)>>3)+dlh[1][x];
        }
        el[x]=ClampPixel((le+dhl[x])>>1);ol[x]=ClampPixel((lo-dhl[x])>>1);
        eh[x]=ClampPixel((he+dhh[x])>>1);oh[x]=ClampPixel((ho-dhh[x])>>1);
    }
    if (prescale==2) {
        InvertHorizontalDescale16s(el,eh,even_output,width,output_width,prescale);
        if (odd_output) InvertHorizontalDescale16s(ol,oh,odd_output,width,output_width,prescale);
    } else {
        InvertHorizontal16s(el,eh,even_output,width,output_width);
        if (odd_output) InvertHorizontal16s(ol,oh,odd_output,width,output_width);
    }
    return CODEC_ERROR_OKAY;
}

CODEC_ERROR gpr_preview_inverse_row(const PIXEL *const ll[3],
    const PIXEL *const lh[3],const PIXEL *hl,const PIXEL *hh,
    DIMENSION width,DIMENSION output_width,int mode,uint16_t prescale,
    const QUANT quant[4],PIXEL *scratch,PIXEL *even_output,PIXEL *odd_output)
{
    if (!ll || !lh || !hl || !hh || !scratch || !quant || !even_output || width<3)
        return CODEC_ERROR_BAD_ARGUMENT;
    for (int b=0;b<4;++b) if (!quant[b]) return CODEC_ERROR_BAD_ARGUMENT;
    for (int r=0;r<3;++r) if (!ll[r] || !lh[r]) return CODEC_ERROR_BAD_ARGUMENT;
    PIXEL *dlh[3]={scratch,scratch+width,scratch+2*width};
    PIXEL *dhl=scratch+3*width,*dhh=scratch+4*width;
    for (int r=0;r<3;++r) DequantizeBandRow16s((PIXEL *)lh[r],width,quant[LH_BAND],dlh[r]);
    DequantizeBandRow16s((PIXEL *)hl,width,quant[HL_BAND],dhl);
    DequantizeBandRow16s((PIXEL *)hh,width,quant[HH_BAND],dhh);
    const PIXEL *rows[3]={dlh[0],dlh[1],dlh[2]};
    return gpr_preview_inverse_row_dequantized(ll,rows,dhl,dhh,width,output_width,
        mode,prescale,scratch+5*width,even_output,odd_output);
}
