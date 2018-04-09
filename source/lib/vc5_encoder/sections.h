/*! @file sections.h
 *
 *  @brief Declaration of routines for handling sections in the encoder.
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

#ifndef SECTIONS_H
#define SECTIONS_H

/*
    @brief Enumeration of the predefined section numbers
 
    The predefined section numbers are defined in ST 2073-2.
 */
typedef enum _section_number
{
    SECTION_NUMBER_IMAGE = 1,       //!< Image section
    SECTION_NUMBER_HEADER = 2,      //!< Bitstream header section
    SECTION_NUMBER_LAYER = 3,       //!< Layer section
    SECTION_NUMBER_CHANNEL = 4,     //!< Channel section
    SECTION_NUMBER_WAVELET = 5,     //!< Wavelet section
    SECTION_NUMBER_SUBBAND = 6,     //!< Subband section
    
    //TODO: Add more section number definitions as required
    
    //! Modify the smallest and largest section numbers as more sections are added
    SECTION_NUMBER_MINIMUM = SECTION_NUMBER_IMAGE,
    SECTION_NUMBER_MAXIMUM = SECTION_NUMBER_SUBBAND,
    
} SECTION_NUMBER;

/*
    @Macro for creating a section number bit mask from a section number
 
    The macro does not check that the section number argument is valid.
 */
#define SECTION_NUMBER_MASK(section_number)  (1 << (section_number - 1))

/*
    @brief Data type for the bit mask that represents enabled sections

    The bit mask indicates which section numbers defined in ST 2073-2 are enabled
    at runtime.
 */
typedef uint32_t ENABLED_SECTIONS;

#define VC5_ENABLED_SECTIONS       (SECTION_NUMBER_MASK(SECTION_NUMBER_CHANNEL)     | \
                                    SECTION_NUMBER_MASK(SECTION_NUMBER_WAVELET)     | \
                                    SECTION_NUMBER_MASK(SECTION_NUMBER_SUBBAND))

#ifdef __cplusplus
extern "C" {
#endif

    bool IsSectionEnabled(struct _encoder *encoder, SECTION_NUMBER section_number);

    CODEC_ERROR BeginSection(BITSTREAM *bitstream, TAGWORD tag);

    CODEC_ERROR EndSection(BITSTREAM *bitstream);

    CODEC_ERROR BeginImageSection(struct _encoder *encoder, BITSTREAM *stream);

    CODEC_ERROR BeginHeaderSection(struct _encoder *encoder, BITSTREAM *stream);

    CODEC_ERROR BeginLayerSection(struct _encoder *encoder, BITSTREAM *stream);

    CODEC_ERROR BeginChannelSection(struct _encoder *encoder, BITSTREAM *stream);

    CODEC_ERROR BeginWaveletSection(struct _encoder *encoder, BITSTREAM *stream);

    CODEC_ERROR BeginSubbandSection(struct _encoder *encoder, BITSTREAM *stream);

    bool GetEnabledSections(const char *string, uint32_t *enabled_sections_out);

#ifdef __cplusplus
}
#endif

#endif // SECTIONS_H
