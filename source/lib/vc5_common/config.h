/*! @file config.h
 *
 *  @brief Parameters that control the configuration of the codec.
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

#ifndef CONFIG_H
#define CONFIG_H

// Maximum number of layers
#define MAX_LAYER_COUNT		10

// Maximum number of color channels
#define MAX_CHANNEL_COUNT	4
//const int MAX_CHANNEL_COUNT = 4;

// Maximum number of wavelets per channel
#define MAX_WAVELET_COUNT	3
//const int MAX_WAVELET_COUNT = 3;

// Maximum number of bands per wavelet
#define MAX_BAND_COUNT		4
//const int MAX_BAND_COUNT = 4;

// Maximum number of subbands in all wavelets (including the lowpass band in the first wavelet)
#define MAX_SUBBAND_COUNT	10

// The number of prescale values that are encoded into the bitstream
#define MAX_PRESCALE_COUNT	8

//TODO: The maximum number of channels and wavelets depends on the profile

//! Internal precision of the intermediate results after unpacking
static const int default_internal_precision = 12;

//TODO: Change the global variable from internal_precision to encoded_precision?

// Definitions of VC-5 parts
#define VC5_PART_ELEMENTARY		1
#define VC5_PART_CONFORMANCE	2		// Conformance does not affect what capabilities are included
#define VC5_PART_IMAGE_FORMATS	3
#define VC5_PART_COLOR_SAMPLING	4
#define VC5_PART_LAYERS			5
#define VC5_PART_SECTIONS		6
#define VC5_PART_METADATA		7

// Convert a part number into a part mask
#define VC5_PART_MASK(n)		(1 << ((n)-1))

// Define the parts supported by this codec implementation
#define VC5_ENABLED_PARTS       (VC5_PART_MASK(VC5_PART_ELEMENTARY)         | \
                                 VC5_PART_MASK(VC5_PART_IMAGE_FORMATS)      | \
                                 VC5_PART_MASK(VC5_PART_COLOR_SAMPLING)     | \
                                 VC5_PART_MASK(VC5_PART_SECTIONS))

// Compile code if any of the parts in the specified mask are supported
#define VC5_ENABLED_MASK(m)		((VC5_ENABLED_PARTS & (m)) != 0)

// Macro for testing support for a specific part of the VC-5 standard
#define VC5_ENABLED_PART(n)		(VC5_ENABLED_MASK(VC5_PART_MASK(n)))


#if VC5_ENABLED_PART(VC5_PART_LAYERS)

// Maximum number of layers supported by the reference codec
#define MAX_LAYER_COUNT		10

#endif

//! Number of rows of intermediate horizontal transform results
#define ROW_BUFFER_COUNT	6

#endif // CONFIG_H
