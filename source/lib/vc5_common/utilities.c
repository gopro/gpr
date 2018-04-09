/*! @file utilities.c
 *
 *  @brief The utilities in this file are included to allow the codec to be tested.
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

#include "common.h"

/*!
	@brief Check that the enabled parts are correct
*/
CODEC_ERROR CheckEnabledParts(ENABLED_PARTS *enabled_parts_ref)
{
	ENABLED_PARTS enabled_parts = (*enabled_parts_ref);

	// The elementary bitstream is always enabled
	if ((enabled_parts & VC5_PART_MASK(VC5_PART_ELEMENTARY)) == 0) {
		enabled_parts |= VC5_PART_MASK(VC5_PART_ELEMENTARY);
	}

	// The conformance specification is never enabled
	enabled_parts &= ~((uint32_t)VC5_PART_MASK(VC5_PART_CONFORMANCE));

	// Image formats must be enabled if subsampled color differences are enabled
	if ((enabled_parts & VC5_PART_MASK(VC5_PART_COLOR_SAMPLING)) != 0) {
		enabled_parts |= VC5_PART_MASK(VC5_PART_IMAGE_FORMATS);
	}

	// Check that the enabled parts were built at compile-time
	//assert((enabled_parts & VC5_ENABLED_PARTS) == enabled_parts);
	if (! ((enabled_parts & VC5_ENABLED_PARTS) == enabled_parts)) {
		return CODEC_ERROR_ENABLED_PARTS;
	}

	// Return the correct enabled parts mask
	*enabled_parts_ref = enabled_parts;
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Verify that the enabled parts are correct
*/
CODEC_ERROR VerifyEnabledParts(ENABLED_PARTS enabled_parts)
{
	// The elementary bitstream must always be enabled
	if ((enabled_parts & VC5_PART_MASK(VC5_PART_ELEMENTARY)) == 0) {
		return CODEC_ERROR_ENABLED_PARTS;
	}

	// The conformance specification must not be enabled
	if ((enabled_parts & VC5_PART_MASK(VC5_PART_CONFORMANCE)) != 0) {
		return CODEC_ERROR_ENABLED_PARTS;
	}

	// Image formats must be enabled if subsampled color differences are enabled
	if ((enabled_parts & VC5_PART_MASK(VC5_PART_COLOR_SAMPLING)) != 0 &&
		(enabled_parts & VC5_PART_MASK(VC5_PART_IMAGE_FORMATS)) == 0) {
		return CODEC_ERROR_ENABLED_PARTS;
	}

	// All enabled parts must be compiled into this codec implementation
	if ((enabled_parts & VC5_ENABLED_PARTS) != enabled_parts) {
		return CODEC_ERROR_ENABLED_PARTS;
	}

	// This codec implementation supports the enabled parts of the VC-5 standard
	return CODEC_ERROR_OKAY;
}
