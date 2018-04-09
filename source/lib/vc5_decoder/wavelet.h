/*! @file wavelet.h
 *
 *  @brief Declare of wavelet decoding functions
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

#ifndef _DECODER_WAVELET_H
#define _DECODER_WAVELET_H

#ifdef __cplusplus
extern "C" {
#endif

CODEC_ERROR TransformInverseSpatialQuantLowpass(gpr_allocator *allocator, WAVELET *input, WAVELET *output, uint16_t prescale);

CODEC_ERROR TransformInverseSpatialQuantArray(	gpr_allocator *allocator,
												WAVELET *input,
												COMPONENT_VALUE *output_buffer,
												DIMENSION output_width,
												DIMENSION output_height,
												size_t output_pitch,
												PRESCALE prescale);

#ifdef __cplusplus
}
#endif

#endif
