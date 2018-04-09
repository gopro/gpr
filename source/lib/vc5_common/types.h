/*! @file types.h
 *
 *  @brief Definition of some common data types used by the codec.
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

#ifndef TYPES_H
#define TYPES_H

//! Data type for Boolean variables
typedef uint_fast8_t BOOLEAN;

//! Data type for image and frame dimensions
typedef uint_least16_t DIMENSION;

//! Data type for an unsigned number (count) that can be the value in a tag-value pair
typedef uint_least16_t COUNT;

/*!
	@brief Define a data type large enough to hold a quantization value

	Define a data type that is large enough to hold any quantization value.
	Older code assumed that the quantization data type was an int, but newer
	could should use the QUANT data type.  After all quantization values are
	stored in a QUANT data type, the data type can be redefined as a byte.
*/
typedef int QUANT;

/*!
	@brief Integer value for the amount of prescale shift

	@todo Redefine this data type to be uint_fast8_t?
*/
typedef uint16_t PRESCALE;


/*!
	@brief Number of bits in a component value
*/
typedef uint_least8_t PRECISION;

enum
{
	PRECISION_MIN = 8,
	PRECISION_MAX = 32,
};

/*!
	@brief Data type for the channel number
*/
typedef uint_least8_t CHANNEL;

/*!
	@brief Data type for the bit mask that represents enabled parts

	The bit mask indicates which parts of the VC-5 standard are enabled
	at runtime.
*/
typedef uint32_t ENABLED_PARTS;

/*!
	@brief Codec version number (major, minor, revision, build)

	The major and minor product numbers identify versions that are released.
	Revision numbers are used to identify interim releases for bug fixes.
	The build number is incremented for every build, independent of product
	numbering, to uniquely identify every build that is a release candidate.
*/
typedef struct _version
{
	uint8_t major;			//!< Major product number
	uint8_t minor;			//!< Minor product number
	uint8_t revision;		//!< Product revision
	uint8_t padding;		//!< Padding to a multiple of 4 bytes
	uint32_t build;			//!< Unique number for each codec build
} VERSION;

#define VERSION_INITIALIZER(major, minor, revision, build) {major, minor, revision, 0, build}

#endif // TYPES_H
