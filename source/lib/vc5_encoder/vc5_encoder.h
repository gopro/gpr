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
#include "vc5_preview.h"

#ifdef __cplusplus
    extern "C" {
#endif

    /*!
     @brief Bayer pattern ordering for vc5 encoder processing
     */
    typedef enum
    {
        VC5_ENCODER_PIXEL_FORMAT_RGGB_12 = 0,           // RGGB 14bit pixels packed into 16bits

        VC5_ENCODER_PIXEL_FORMAT_RGGB_12P = 1,           // RGGB 14bit pixels packed into 16bits

        VC5_ENCODER_PIXEL_FORMAT_RGGB_14 = 2,           // RGGB 14bit pixels packed into 16bits
        
        VC5_ENCODER_PIXEL_FORMAT_GBRG_12 = 3,           // GBRG 12bit pixels packed into 16bits
        
        VC5_ENCODER_PIXEL_FORMAT_GBRG_12P = 4,          // GBRG 12bit pixels packed into 12bits
        
        /* Appended values preserve the historical ABI and the earlier feature PR. */
        VC5_ENCODER_PIXEL_FORMAT_RGGB_16 = 5,
        VC5_ENCODER_PIXEL_FORMAT_GBRG_16 = 6,
        VC5_ENCODER_PIXEL_FORMAT_GBRG_14 = 7,
        VC5_ENCODER_PIXEL_FORMAT_GRBG_12 = 8,
        VC5_ENCODER_PIXEL_FORMAT_GRBG_14 = 9,
        VC5_ENCODER_PIXEL_FORMAT_GRBG_16 = 10,
        VC5_ENCODER_PIXEL_FORMAT_BGGR_12 = 11,
        VC5_ENCODER_PIXEL_FORMAT_BGGR_14 = 12,
        VC5_ENCODER_PIXEL_FORMAT_BGGR_16 = 13,

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
        int                         input_pitch;            // Row stride in bytes (Default: 8000)
        
        VC5_ENCODER_PIXEL_FORMAT    pixel_format;         // Bayer Ordering Pattern (Default: VC5_ENCODER_BAYER_ORDERING_RGGB)
        
        VC5_ENCODER_QUALITY_SETTING quality_setting;        // Quality setting of the encoder (Default: VC5_ENCODER_QUALITY_SETTING_FS2)
        
        gpr_malloc                  mem_alloc;              // Callback function to allocate memory
        
        gpr_free                    mem_free;               // Callback function to free memory
        
    } vc5_encoder_parameters;

    void vc5_encoder_parameters_set_default(vc5_encoder_parameters* encoding_parameters);

    /* Even sensor dimensions >=48, <=UINT16_MAX, with width*height*2 <=INT_MAX.
     * Input capacity must cover every active row through the last pixel;
     * trailing row padding is optional. Stride is positive bytes. Unaligned
     * historical word/packed-12 layouts use an aligned scratch copy; ordinary
     * aligned inputs retain the existing fast path. Invalid geometry/span,
     * formats, quality or callbacks are rejected before codec allocation. */
    CODEC_ERROR vc5_encoder_process(const vc5_encoder_parameters*   encoding_parameters,    /* vc5 encoding parameters */
                                    const gpr_buffer*               raw_buffer,             /* raw input buffer. */
                                          gpr_buffer*               vc5_buffer,
                                          gpr_rgb_buffer*           rgb_buffer);            /* vc5 output buffer. */

    /* Same encoding/validation and payload as process. Optional preview callback
     * borrows retained upper bands after encoding and before codec cleanup.
     * Callback runs synchronously under the codec's global encoder lock: do not
     * reenter any encoder/SDK encoding call or let exceptions cross C frames.
     * Copy/reconstruct coefficients before return. NULL keeps ordinary memory
     * behavior. Extra band storage is allocated through the supplied allocator.
     * Callback return does not establish payload durability or recording. */
    CODEC_ERROR vc5_encoder_process_with_preview(const vc5_encoder_parameters *parameters,
        const gpr_buffer *raw,gpr_buffer *vc5,gpr_rgb_buffer *rgb,
        vc5_encoder_preview_fn preview,void *user);

    /* Same validation/normalization and payload as process. Rows are borrowed
     * until callback return. Different channels may call concurrently; each
     * channel's levels are delivered serially in forward-pipeline order.
     * Runs under the encoder lock: no reentrant encoding or exceptions across C.
     * No callback occurs after return. On encoding failure discard any partial
     * preview; observation does not imply payload recording or durability.
     * NULL preserves ordinary encoding behavior and memory requirements. */
    CODEC_ERROR vc5_encoder_process_with_preview_rows(const vc5_encoder_parameters *parameters,
        const gpr_buffer *raw,gpr_buffer *vc5,gpr_rgb_buffer *rgb,
        vc5_encoder_preview_row_fn rows,void *user);

#ifdef __cplusplus
    }
#endif

#endif // VC5_ENCODER_H
