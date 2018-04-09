/*! @file parameters.h
 *
 *  @brief Declare a data structure for holding a table of parameters used
 *  during decoding to override the default decoding behavior.
 
 *  The decoder can be initialized using the dimensions of the encoded frame
 *  obtained from an external source such as a media container and the pixel
 *  format of the decoded frame.  The encoded sample will be decoded to the
 *  dimensions of the encoded frame without at the full encoded resolution
 *  without scaling.  The decoded frames will have the specified pixel format,
 *  but this assumes that the encoded dimensions used during initialization
 *  are the same as the actual encoded dimensions and that the pixel format of
 *  the decoded frames is a valid pixel format.
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

#ifndef PARAMETERS_H
#define PARAMETERS_H

#include "vc5_decoder.h"

/*!
	@brief Declaration of a data structure for passing decoding parameters to the decoder
*/
typedef struct _decoder_parameters
{
	uint32_t version;			//!< Version number for this definition of the parameters
	
	uint32_t enabled_parts;		//!< Parts of the VC-5 standard that are enabled

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
	int layer_count;		//!< Number of layers in the sample
	bool progressive;		//!< True if the frame is progressive
	bool top_field_first;	//!< True if interlaced with top field first
#endif

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    bool section_flag;          //!< True if decoding sections element in the bitstream
#endif
    
	//! Dimensions and format of the output of the image unpacking process
	struct _input_parameters
	{
		DIMENSION width;
		DIMENSION height;
		// PIXEL_FORMAT format;
	} input;		//! Dimensions and format of the unpacked image

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	//! Dimensions and format of the output of the decoding process
	struct _decoded_parameters
	{
		DIMENSION width;
		DIMENSION height;
		PIXEL_FORMAT format;
	} decoded;		//! Decoded image dimensions and pixel format
#endif

#if VC5_ENABLED_PART(VC5_PART_ELEMENTARY)
	//! Dimensions and format of the output of the image repacking process
	struct _output_parameters
	{
		DIMENSION width;
		DIMENSION height;
		PIXEL_FORMAT format;
	} output;
#endif

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	//! Dimensions and format of the displayable image
	struct _display_parameters
	{
		DIMENSION width;
		DIMENSION height;
		PIXEL_FORMAT format;
	} display;
#endif

#if VC5_ENABLED_PART(VC5_PART_METADATA)
	//! Metadata that controls decoding
	METADATA metadata;
#endif

	//! Flag that controls verbose output
	bool verbose_flag;
    
    GPR_RGB_RESOLUTION  rgb_resolution;
    
    int                         rgb_bits;
    
    gpr_rgb_gain                rgb_gain;
    
    gpr_allocator allocator;
    
} DECODER_PARAMETERS;

#ifdef __cplusplus
extern "C" {
#endif

    CODEC_ERROR InitDecoderParameters(DECODER_PARAMETERS *parameters);

#ifdef __cplusplus
}
#endif
    
#endif // PARAMETERS_H
