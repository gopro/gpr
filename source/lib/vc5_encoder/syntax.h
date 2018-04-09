/*! @file syntax.h
 *
 *  @brief Declare functions for writing bitstream syntax of encoded samples.
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

#ifndef ENCODER_SYNTAX_H
#define ENCODER_SYNTAX_H

#ifdef __cplusplus
extern "C" {
#endif
    
    CODEC_ERROR PutSampleOffsetSegment(BITSTREAM *bitstream, uint32_t offset, TAGVALUE segment);

    CODEC_ERROR PutVideoLowpassTrailer(BITSTREAM *stream);
    
    CODEC_ERROR PutVideoBandTrailer(BITSTREAM *stream);

    CODEC_ERROR PutTagValue(BITSTREAM *stream, TAGVALUE segment);
    
    CODEC_ERROR PutBitstreamStartMarker(BITSTREAM *stream);
    
    CODEC_ERROR PushSampleSize(BITSTREAM *bitstream, TAGWORD tag);

    CODEC_ERROR PopSampleSize(BITSTREAM *bitstream);
      
#ifdef __cplusplus
}
#endif

#endif // ENCODER_SYNTAX_H
