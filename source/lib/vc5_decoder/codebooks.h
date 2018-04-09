/*! @file codebooks.h
 *
 *  @brief Declaration of routines for the inverse component transform and permutation.
 *  The collection of codebooks that are used by the decoder are called a codeset.
 *  The codebook in seach codeset is derived from the master codebook that is
 *  included in the codec by including the table for the codebook.  The encoder
 *  uses specialized codebooks for coefficient magnitudes and runs of zeros that
 *  are derived from the master codebook.
 
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

#ifndef CODEBOOKS_H
#define CODEBOOKS_H

#ifdef __cplusplus
extern "C" {
#endif
    
    typedef struct decoder_codeset {
        
        const char *title;					//!< Identifying string for the codeset
        
        const CODEBOOK *codebook;			//!< Codebook for runs and magnitudes
        
        uint32_t flags;						//!< Encoding flags (see the codeset flags)
        
    } DECODER_CODESET;
    
    //TODO: Need to support other codesets in the reference decoder?
    extern DECODER_CODESET decoder_codeset_17;

#ifdef __cplusplus
}
#endif

#endif // CODEBOOKS_H
