/*! @file parameters.c
 *
 *  @brief Implementation of the data structure used to pass decoding
 *  parameters to the decoder.
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
#define PARAMETERS_VERSION		0

/*!
	@brief Initialize the parameters data structure

	The version number of the parameters data structure must be
	incremented whenever a change is made to the definition of
	the parameters data structure.
*/
CODEC_ERROR InitDecoderParameters(DECODER_PARAMETERS *parameters)
{
	memset(parameters, 0, sizeof(DECODER_PARAMETERS));
	parameters->version = 1;
	parameters->verbose_flag = false;
 
    parameters->enabled_parts = VC5_ENABLED_PARTS;
    
    parameters->output.format = PIXEL_FORMAT_RAW_DEFAULT;
    
    parameters->rgb_resolution = GPR_RGB_RESOLUTION_NONE;
    
    gpr_rgb_gain_set_defaults(&parameters->rgb_gain);
    
	return CODEC_ERROR_OKAY;
}
