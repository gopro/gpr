/*! @file codebooks.h
 *
 *  @brief Declaration of the routines for computing the encoding tables from a codebook.
 
 *  A codebooks contains the variable-length codes for coefficient magnitudes, runs
 *  of zeros, and special codewords that mark entropy codec locations in bitstream.
 
 *  The codebook is used to create tables and simplify entropy coding of signed values
 *  and runs of zeros.
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

    /*!
     Codeset data structure that is used by the encoder
     */
    typedef struct encoder_codeset {
        
        const char *title;					//!< Identifying string for the codeset
        
        const CODEBOOK *codebook;			//!< Codebook for runs and magnitudes

        uint32_t flags;						//!< Encoding flags (see the codeset flags)
        
        const MAGS_TABLE *mags_table;		//!< Table for encoding coefficient magnitudes
        
        const RUNS_TABLE *runs_table;		//!< Table for encoding runs of zeros
        
    } ENCODER_CODESET;
    
    extern ENCODER_CODESET encoder_codeset_17;
        
    CODEC_ERROR PrepareCodebooks(const gpr_allocator *allocator, ENCODER_CODESET *cs);
    
    CODEC_ERROR ReleaseCodebooks(gpr_allocator *allocator, ENCODER_CODESET *cs);    
    
    CODEC_ERROR ComputeRunLengthCodeTable(const gpr_allocator *allocator,
                                          RLV *input_codes, int input_length,
                                          RLC *output_codes, int output_length);
    
    CODEC_ERROR SortDecreasingRunLength(RLC *codebook, int length);
    
    CODEC_ERROR FillRunLengthEncodingTable(RLC *codebook, int codebook_length,
                                           RLC *table, int table_length);
    
    CODEC_ERROR FillMagnitudeEncodingTable(const CODEBOOK *codebook, VLE *table, int size, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif // CODEBOOKS_H
