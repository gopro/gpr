/*! @file codebooks.c
 *
 *  @brief Implementation of routines for the inverse component transform and permutation.
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

#include "headers.h"
#include "table17.inc"

/*!
	@brief Define the codeset used by the reference codec
 
	The baseline codec only supports codebook #17.
 
	Codebook #17 is intended to be used with cubic companding
	(see @ref FillMagnitudeEncodingTable and @ref ComputeCubicTable).
 */
DECODER_CODESET decoder_codeset_17 = {
    "Codebook set 17 from data by David Newman with tables automatically generated for the FSM decoder",
    (const CODEBOOK *)&table17,
    CODESET_FLAGS_COMPANDING_CUBIC,
};
