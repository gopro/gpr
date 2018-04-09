/*! @file dequantize.h
 *
 *  @brief Declaration of inverse quantization functions
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

#ifndef QUANTIZE_H
#define QUANTIZE_H

#ifdef __cplusplus
extern "C" {
#endif
    
    CODEC_ERROR DequantizeBandRow16s(PIXEL *input, int width, int quantization, PIXEL *output);

    PIXEL DequantizedValue(int32_t value, int quantization);

#ifdef __cplusplus
}
#endif

#endif // QUANTIZE_H
