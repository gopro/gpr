/*! @file forward.h
 *
 *  @brief Declare of the forward wavelet transform functions.
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

#ifndef FORWARD_H
#define FORWARD_H

#ifdef __cplusplus
extern "C" {
#endif
    
    CODEC_ERROR FilterHorizontalRow(PIXEL *input, PIXEL *lowpass, PIXEL *highpass, int width, int prescale);
    
    CODEC_ERROR FilterVerticalTopRow(PIXEL **lowpass, PIXEL **highpass, PIXEL **output, int wavelet_width, int wavelet_pitch, int32_t midpoints[], int32_t multipliers[], int input_row );
    
    CODEC_ERROR FilterVerticalMiddleRow(PIXEL **lowpass, PIXEL **highpass, PIXEL **output, int wavelet_width, int wavelet_pitch, int32_t midpoints[], int32_t multipliers[], int input_row );
    
    CODEC_ERROR FilterVerticalBottomRow(PIXEL **lowpass, PIXEL **highpass, PIXEL **output, int wavelet_width, int wavelet_pitch, int32_t midpoints[], int32_t multipliers[], int input_row );

#ifdef __cplusplus
}
#endif
        
#endif // FORWARD_H
