/*! @file parameters.c
 *
 *  @brief Implementation of the data structure used to pass parameters
 *  to the encoder.
 *
 *  The parameters data structure is currently a simple struct, but
 *  fields may be added, removed, or replaced.  A version number is
 *  included in the parameters data structure to allow decoders to
 *  adapt to changes.
 
 *  It is contemplated that future inplementations may use a dictionary
 *  of key-value pairs which would allow the decoder to determine whether
 *  a parameter is present.
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

#include "headers.h"

//! Current version number of the parameters data structure
#define PARAMETERS_VERSION		1

/*!
	@brief Initialize the parameters data structure

	The version number of the parameters data structure must be
	incremented whenever a change is made to the definition of
	the parameters data structure.

	@todo Special initialization required by the metadata?
*/
CODEC_ERROR InitEncoderParameters(ENCODER_PARAMETERS *parameters)
{
	memset(parameters, 0, sizeof(ENCODER_PARAMETERS));
	parameters->version = PARAMETERS_VERSION;

	// Set the default value for the number of bits per lowpass coefficient
	parameters->encoded.lowpass_precision = 16;

    parameters->input.width  = 4000;
    parameters->input.height = 3000;
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	// The maximum number of bits per component is the internal precision
	//parameters->max_bits_per_component  = internal_precision;
#endif

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    parameters->enabled_sections = VC5_ENABLED_SECTIONS;
#endif
    
	// The elementary bitstream is always enabled
	parameters->enabled_parts = VC5_ENABLED_PARTS;

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
	parameters->layer_count = 1;
	parameters->progressive = 1;
#endif

	// Initialize the quantization table using the default values
    {
        // Initialize to CineForm Filmscan-2
        QUANT quant_table[] = {1, 24, 24, 12, 24, 24, 12, 32, 32, 48};
        memcpy(parameters->quant_table, quant_table, sizeof(parameters->quant_table));
    }

	parameters->verbose_flag = false;
    
    gpr_rgb_gain_set_defaults(&parameters->rgb_gain);
    
    parameters->rgb_resolution = VC5_ENCODER_RGB_RESOLUTION_DEFAULT;
    
	return CODEC_ERROR_OKAY;
}

