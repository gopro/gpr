/*! @file gpr.h
 *
 *  @brief Declaration of the GPR-SDK objects and functions
 *
 *  GPR API can be invoked by simply including this header file.
 *  This file includes all other header files that are needed.
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

#ifndef GPR_H
#define GPR_H

#include "gpr_platform.h"
#include "gpr_exif_info.h"
#include "gpr_profile_info.h"
#include "gpr_tuning_info.h"
#include "gpr_allocator.h"
#include "gpr_buffer.h"
#include "gpr_rgb_buffer.h"

#ifdef __cplusplus
    extern "C" {
#endif

        typedef struct
        {
          gpr_buffer            jpg_preview;                     /* Address to the memory location that this buffer points to */
          
          unsigned int          preview_width;                   /* Width of input source in pixels (only applies to raw input) */
          
          unsigned int          preview_height;                  /* Height of input source in pixels (only applies to raw input) */

        } gpr_preview_image;
      
        typedef struct
        {
            unsigned int        input_width;                   /* Width of input source in pixels (only applies to raw input) */

            unsigned int        input_height;                  /* Height of input source in pixels (only applies to raw input) */
            
            unsigned int        input_pitch;                   /* Pitch of input source in pixels (only applies to raw input) */

            bool                fast_encoding;
            
            bool                compute_md5sum;
            
            gpr_buffer          gpmf_payload;   /* GPMF payload of image file */
          
            gpr_preview_image   preview_image;  /* Preview JPG image */
            
            bool                enable_preview;
          
            gpr_exif_info       exif_info;      /* Exif info object */
            
            gpr_profile_info    profile_info;   /* Camera color profile info object */
            
            gpr_tuning_info     tuning_info;    /* Camera tuning info object */
            
        } gpr_parameters;
        
        void gpr_parameters_set_defaults(gpr_parameters* x);
        
        void gpr_parameters_construct_copy(const gpr_parameters* y, gpr_parameters* x);
        
        void gpr_parameters_destroy(gpr_parameters* x, gpr_free mem_free);
        
        //!< Parse Metadata of DNG File and return in gpr_parameters struct
        bool gpr_parse_metadata(const gpr_allocator*            allocator,
                                      gpr_buffer*               inp_dng_buffer,
                                      gpr_parameters*   parameters);
        
        //!< CHECK IF DNG IS VC5 COMPRESSED
        bool gpr_check_vc5( gpr_buffer* inp_dng_buffer, gpr_malloc mem_alloc, gpr_free mem_free );
        
        //!< CONVERSION FUNCTIONS
        
        //!< raw to dng conversion
        bool gpr_convert_raw_to_dng(const gpr_allocator*    allocator,
                                    const gpr_parameters*   parameters,
                                          gpr_buffer*       inp_raw_buffer,
                                          gpr_buffer*       out_dng_buffer);

        //!< dng to raw conversion
        bool gpr_convert_dng_to_raw(const gpr_allocator*    allocator,
                                          gpr_buffer*       inp_dng_buffer,
                                          gpr_buffer*       out_raw_buffer);

        //!< dng to raw conversion
        bool gpr_convert_dng_to_dng(const gpr_allocator*    allocator,
                                    const gpr_parameters*   parameters,
                                          gpr_buffer*       inp_dng_buffer,
                                          gpr_buffer*       out_dng_buffer);
        
        //!< vc5 to gpr conversion
        bool gpr_convert_vc5_to_gpr(const gpr_allocator*    allocator,
                                    const gpr_parameters*   parameters,
                                          gpr_buffer*       inp_vc5_buffer,
                                          gpr_buffer*       out_gpr_buffer);

        //!< gpr to vc5 conversion
        bool gpr_convert_gpr_to_vc5(const gpr_allocator*    allocator,
                                          gpr_buffer*       inp_gpr_buffer,
                                          gpr_buffer*       out_vc5_buffer);
        
#if GPR_WRITING

        //!< raw to gpr conversion
        bool gpr_convert_raw_to_gpr(const gpr_allocator*    allocator,
                                    const gpr_parameters*   parameters,
                                          gpr_buffer*       inp_raw_buffer,
                                          gpr_buffer*       out_gpr_buffer);

        //!< dng to gpr conversion
        bool gpr_convert_dng_to_gpr(const gpr_allocator*    allocator,
                                    const gpr_parameters*   parameters,
                                          gpr_buffer*       inp_dng_buffer,
                                          gpr_buffer*       out_gpr_buffer);

        //!< dng to vc5 conversion
        bool gpr_convert_dng_to_vc5(const gpr_allocator*    allocator,
                                          gpr_buffer*       inp_dng_buffer,
                                          gpr_buffer*       out_vc5_buffer);
#endif // GPR_WRITING
        

#if GPR_READING

        //!< gpr to rgb conversion
        bool gpr_convert_gpr_to_rgb(const gpr_allocator*        allocator,
                                          GPR_RGB_RESOLUTION    rgb_resolution,
                                          int                   rgb_bits,                                    
                                          gpr_buffer*           inp_gpr_buffer,
                                          gpr_rgb_buffer*       out_rgb_buffer);
        
        //!< gpr to dng conversion
        bool gpr_convert_gpr_to_dng(const gpr_allocator*    allocator,
                                    const gpr_parameters*   parameters,
                                          gpr_buffer*       inp_gpr_buffer,
                                          gpr_buffer*       out_dng_buffer);

        //!< vc5 to dng conversion
        bool gpr_convert_vc5_to_dng(const gpr_allocator*    allocator,
                                    const gpr_parameters*   parameters,
                                          gpr_buffer*       inp_vc5_buffer,
                                          gpr_buffer*       out_dng_buffer);
        
        //!< gpr to raw conversion
        bool gpr_convert_gpr_to_raw(const gpr_allocator*    allocator,
                                          gpr_buffer*       inp_gpr_buffer,
                                          gpr_buffer*       out_raw_buffer);
#endif
        
#ifdef __cplusplus
    }
#endif

#endif // GPR_H
