/*! @file companding.h
 *
 *  @brief Declaration of the routines for computing the companding curves
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

#ifndef COMPANDING_H
#define COMPANDING_H

#ifdef __cplusplus
extern "C" {
#endif

    int32_t CompandedValue(int32_t value);
   
    uint32_t CompandingParameter();

    CODEC_ERROR ComputeCubicTable(int16_t cubic_table[], int cubic_table_length, int16_t maximum_value);

    // Invert the companding curve applied to a quantized coefficient magnitude (for debugging)
    int32_t UncompandedValue(int32_t value);
    
    PIXEL UncompandedPixel(PIXEL value);
    
    CODEC_ERROR InvertCompanding(PIXEL *image, DIMENSION width, DIMENSION height, DIMENSION pitch);

#ifdef __cplusplus
}
#endif
        
#endif // COMPANDING_H
