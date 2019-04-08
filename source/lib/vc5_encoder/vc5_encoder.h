/*! @file vc5_encoder.h
 *
 *  @brief Declaration of the top level vc5 encoder API.
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

#ifndef VC5_ENCODER_H
#define VC5_ENCODER_H

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
        VC5_ENCODER_PIXEL_FORMAT_RGGB_12 = 0,       // RGGB 12bit LSB pixels stored into 16bits

        VC5_ENCODER_PIXEL_FORMAT_RGGB_12P,          // RGGB 12bit pixels packed into 12bits

		VC5_ENCODER_PIXEL_FORMAT_RGGB_14,           // RGGB 14bit LSB pixels stored into 16bits

		VC5_ENCODER_PIXEL_FORMAT_RGGB_16,           // RGGB 16bit stored into 16bits
        
        VC5_ENCODER_PIXEL_FORMAT_GBRG_12,           // GBRG 12bit pixels packed into 16bits
        
        VC5_ENCODER_PIXEL_FORMAT_GBRG_12P,          // GBRG 12bit pixels packed into 12bits

		VC5_ENCODER_PIXEL_FORMAT_GBRG_14,           // GBRG 14bit LSB pixels stored into 16bits

		VC5_ENCODER_PIXEL_FORMAT_GBRG_16,           // GBRG 16bit stored into 16bits
        
        VC5_ENCODER_PIXEL_FORMAT_DEFAULT = VC5_ENCODER_PIXEL_FORMAT_RGGB_14,
        
    } VC5_ENCODER_PIXEL_FORMAT;

    #define VC5_ENCODER_RGB_RESOLUTION_DEFAULT GPR_RGB_RESOLUTION_SIXTEENTH
        
    /*!
     @brief Quality setting of the VC5 encoder
     */
    typedef enum
    {
        VC5_ENCODER_QUALITY_SETTING_LOW         = 0,		// Low (Lowest Quality)
        VC5_ENCODER_QUALITY_SETTING_MEDIUM      = 1,		// Medium
        VC5_ENCODER_QUALITY_SETTING_HIGH        = 2,		// High
        VC5_ENCODER_QUALITY_SETTING_FS1         = 3,		// Film Scan 1
        VC5_ENCODER_QUALITY_SETTING_FSX         = 4,		// Film Scan X
        VC5_ENCODER_QUALITY_SETTING_FS2         = 5,		// Film Scan 2 (Highest Quality)
        
        VC5_ENCODER_QUALITY_SETTING_COUNT       = 6,
        
        VC5_ENCODER_QUALITY_SETTING_DEFAULT     = VC5_ENCODER_QUALITY_SETTING_FSX,
        
    } VC5_ENCODER_QUALITY_SETTING;

    /*!
     @brief vc5 encoder parameters
     */
    typedef struct
    {
        ENABLED_PARTS               enabled_parts;
        
        unsigned int                input_width;            // Image Width in Components (Default: 4000)
        unsigned int                input_height;           // Image Height in Components (Default: 3000)
        int                         input_pitch;            // Image Buffer Stride in Components (Default: 4000)
        
        VC5_ENCODER_PIXEL_FORMAT    pixel_format;         // Bayer Ordering Pattern (Default: VC5_ENCODER_BAYER_ORDERING_RGGB)
        
        VC5_ENCODER_QUALITY_SETTING quality_setting;        // Quality setting of the encoder (Default: VC5_ENCODER_QUALITY_SETTING_FS2)
        
        gpr_malloc                  mem_alloc;              // Callback function to allocate memory
        
        gpr_free                    mem_free;               // Callback function to free memory
        
    } vc5_encoder_parameters;

    void vc5_encoder_parameters_set_default(vc5_encoder_parameters* encoding_parameters);

    CODEC_ERROR vc5_encoder_process(const vc5_encoder_parameters*   encoding_parameters,    /* vc5 encoding parameters */
                                    const gpr_buffer*               raw_buffer,             /* raw input buffer. */
                                          gpr_buffer*               vc5_buffer,
                                          gpr_rgb_buffer*           rgb_buffer);            /* vc5 output buffer. */

#ifdef __cplusplus
    }
#endif

#endif // VC5_ENCODER_H
