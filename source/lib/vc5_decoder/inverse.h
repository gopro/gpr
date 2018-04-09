/*! @file inverse.h
 *
 *  @brief Declaration of the inverse wavelet transform functions.
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

#ifndef INVERSE_H
#define INVERSE_H

#ifdef __cplusplus
extern "C" {
#endif
    
    CODEC_ERROR InvertSpatialQuant16s(gpr_allocator *allocator,
                                      PIXEL *lowlow_band, int lowlow_pitch,
                                      PIXEL *lowhigh_band, int lowhigh_pitch,
                                      PIXEL *highlow_band, int highlow_pitch,
                                      PIXEL *highhigh_band, int highhigh_pitch,
                                      PIXEL *output_image, int output_pitch,
                                      DIMENSION input_width, DIMENSION input_height,
                                      DIMENSION output_width, DIMENSION output_height,
                                      QUANT quantization[]);

    CODEC_ERROR InvertSpatialQuantDescale16s(gpr_allocator *allocator,
                                             PIXEL *lowlow_band, int lowlow_pitch,
                                             PIXEL *lowhigh_band, int lowhigh_pitch,
                                             PIXEL *highlow_band, int highlow_pitch,
                                             PIXEL *highhigh_band, int highhigh_pitch,
                                             PIXEL *output_image, int output_pitch,
                                             DIMENSION input_width, DIMENSION input_height,
                                             DIMENSION output_width, DIMENSION output_height,
                                             //ROI roi, PIXEL *buffer, size_t buffer_size,
                                             int descale, QUANT quantization[]);

#ifdef __cplusplus
}
#endif

#endif // INVERSE_H
