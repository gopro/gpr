/*! @file codeset.h
 *
 *  @brief The codeset data structure includes the codebook and flags that
 *  indicate how to use the codebook.  The codeset may also include tables
 *  that are derived from the codebook to facilite encoding and decoding.
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

#ifndef CODESET_H
#define CODESET_H

/*!
	@brief Codeset flags that determine how the codebook is used for encoding

	The codeset flags determine how the codebook in the codeset is used to
	compute the tables for encoding coefficient magnitudes and runs of zeros.

	The companding curve is applied to the quantized coefficients before the
	values are entropy coded to fit the coefficient magnitudes into the range
	of magnitudes provided by the codebook.  The companding curve is applied
	when the encoding table for coefficient magnitudes is computed.
*/
typedef enum _codeset_flags
{
	CODESET_FLAGS_COMPANDING_NONE = 0x0002,		//!< Do not apply a companding curve
	CODESET_FLAGS_COMPANDING_CUBIC = 0x0004,	//!< Apply a cubic companding curve

} CODESET_FLAGS;

#endif // CODESET_H
