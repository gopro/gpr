/*! @file component.h
 *
 *  @brief Declaration of routines for the inverse component transform and permutation.
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

#ifndef COMPONENT_H
#define COMPONENT_H

#ifdef __cplusplus
extern "C" {
#endif

    CODEC_ERROR ParseInverseComponentTransform(DECODER *decoder, BITSTREAM *stream, size_t chunk_size);

    CODEC_ERROR ParseInverseComponentPermutation(DECODER *decoder, BITSTREAM *stream, size_t chunk_size);

#ifdef __cplusplus
}
#endif

#endif // COMPONENT_H
