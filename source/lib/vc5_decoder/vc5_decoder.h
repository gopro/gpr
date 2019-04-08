/*! @file vc5_decoder.h
 *
 *  @brief Declaration of the top level vc5 decoder API.
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

#ifndef VC5_DECODER_H
#define VC5_DECODER_H

#include "error.h"
#include "types.h"
#include "gpr_buffer.h"
#include "gpr_rgb_buffer.h"
#include "vc5_common.h"

#ifdef __cplusplus
    extern "C" {
#endif

    /*!
     @brief Bayer pattern ordering for vc5 encoder processing
     */
    typedef enum
    {
        VC5_DECODER_PIXEL_FORMAT_RGGB_12 = 0,           // RGGB 14bit pixels packed into 16bits

		VC5_DECODER_PIXEL_FORMAT_RGGB_14,               // RGGB 14bit pixels packed into 16bits

		VC5_DECODER_PIXEL_FORMAT_RGGB_16,               // RGGB 16bit pixels packed into 16bits
        
        VC5_DECODER_PIXEL_FORMAT_GBRG_12,               // GBRG 12bit pixels packed into 16bits
        
		VC5_DECODER_PIXEL_FORMAT_GBRG_14,               // GBRG 14bit pixels packed into 16bits
        
		VC5_DECODER_PIXEL_FORMAT_GBRG_16,               // GBRG 16bit pixels packed into 16bits
        
        VC5_DECODER_PIXEL_FORMAT_DEFAULT = VC5_DECODER_PIXEL_FORMAT_RGGB_14,
        
    } VC5_DECODER_PIXEL_FORMAT;

    #define VC5_DECODER_RGB_RESOLUTION_DEFAULT GPR_RGB_RESOLUTION_QUARTER
        
    /*!
     @brief vc5 decoder parameters
     */
    typedef struct
    {
        ENABLED_PARTS                   enabled_parts;
        
        VC5_DECODER_PIXEL_FORMAT        pixel_format;           // Bayer Ordering Pattern (Default: VC5_ENCODER_BAYER_ORDERING_RGGB)
        
        GPR_RGB_RESOLUTION              rgb_resolution;
        
        int                             rgb_bits;
        
        gpr_rgb_gain                    rgb_gain;
        
        gpr_malloc                      mem_alloc;              // Callback function to allocate memory
        
        gpr_free                        mem_free;               // Callback function to free memory
        
    } vc5_decoder_parameters;
        
    void vc5_decoder_parameters_set_default(vc5_decoder_parameters* decoding_parameters);

    CODEC_ERROR vc5_decoder_process(const vc5_decoder_parameters*   decoding_parameters,    /* vc5 decoding parameters */
                                    const gpr_buffer*               vc5_buffer,             /* vc5 input buffer. */
                                          gpr_buffer*               raw_buffer,             /* raw output buffer. */
                                          gpr_rgb_buffer*           rgb_buffer);            /* rgb output buffer */
        

#ifdef __cplusplus
    }
#endif

#endif // VC5_DECODER_H
