/*! @file syntax.h
 *
 *  @brief Declare functions for parsing the bitstream syntax of encoded samples.
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

#ifndef DECODER_SYNTAX_H
#define DECODER_SYNTAX_H

#ifdef __cplusplus
extern "C" {
#endif

    TAGVALUE GetSegment(BITSTREAM *stream);

    TAGWORD GetValue(BITSTREAM *stream, int tag);

    TAGVALUE GetTagValue(BITSTREAM *stream);

    bool IsTagOptional(TAGWORD tag);

    bool IsTagRequired(TAGWORD tag);

    bool IsValidSegment(BITSTREAM *stream, TAGVALUE segment, TAGWORD tag);

    bool IsTagValue(TAGVALUE segment, int tag, TAGWORD value);

#ifdef __cplusplus
}
#endif

#endif // DECODER_SYNTAX_H
