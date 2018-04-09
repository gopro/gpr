/*! @file sections.c
 *
 *  @brief Implementation of code for encoding sections
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

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)

/*
 @brief Write codec state parameters used for decoding the section into the bitstream
 
 A section element may be decoded independently from other sections of the same type.
 Concurrent decoding implies that all codec state parameters needed to decode a section
 element be present in that section element.
 
 In principle, it is only necessary to write the codec state parameters that may be changed
 as other section elements are decoded independently.  This sample encoder takes the simple
 approach and writes all non-header codec state parameters into the bitstream.
 */
static CODEC_ERROR PutCodecState(ENCODER *encoder, BITSTREAM *stream, SECTION_NUMBER section_number)
{
    CODEC_STATE *codec = &encoder->codec;
    TAGWORD prescale_shift = 0;
    
    switch (section_number)
    {
        case SECTION_NUMBER_IMAGE:
            assert(0);
            break;
            
        case SECTION_NUMBER_HEADER:
            // No codec state parameters to be written into the bitstream
            break;
            
        case SECTION_NUMBER_CHANNEL:
            // Encode the transform prescale for the first channel (assume all channels are the same)
            prescale_shift = PackTransformPrescale(&encoder->transform[0]);
            
            PutTagPair(stream, CODEC_TAG_ChannelNumber, codec->channel_number);
            PutTagPair(stream, CODEC_TAG_SubbandNumber, codec->subband_number);
            PutTagPair(stream, CODEC_TAG_LowpassPrecision, codec->lowpass_precision);
            PutTagPair(stream, CODEC_TAG_Quantization, codec->band.quantization);
            PutTagPair(stream, CODEC_TAG_PrescaleShift, prescale_shift);
            
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
            if (!IsPartEnabled(encoder->enabled_parts, VC5_PART_IMAGE_FORMATS))
            {
                PutTagPair(stream, CODEC_TAG_ChannelWidth, codec->channel_width);
                PutTagPair(stream, CODEC_TAG_ChannelHeight, codec->channel_height);
            }
#endif
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
            if (IsPartEnabled(encoder->enabled_parts, VC5_PART_LAYERS))
            {
                PutTagPair(stream, CODEC_TAG_LayerNumber, codec->layer_number);
            }
#endif
            break;
            
        case SECTION_NUMBER_WAVELET:
            PutTagPair(stream, CODEC_TAG_ChannelNumber, codec->channel_number);
            PutTagPair(stream, CODEC_TAG_SubbandNumber, codec->subband_number);
            PutTagPair(stream, CODEC_TAG_LowpassPrecision, codec->lowpass_precision);
            //PutTagPair(stream, CODEC_TAG_Quantization, codec->band.quantization);
            //PutTagPair(stream, CODEC_TAG_PrescaleShift, prescale_shift);
            break;
            
        case SECTION_NUMBER_SUBBAND:
            PutTagPair(stream, CODEC_TAG_ChannelNumber, codec->channel_number);
            PutTagPair(stream, CODEC_TAG_SubbandNumber, codec->subband_number);
            PutTagPair(stream, CODEC_TAG_LowpassPrecision, codec->lowpass_precision);
            PutTagPair(stream, CODEC_TAG_Quantization, codec->band.quantization);
            //PutTagPair(stream, CODEC_TAG_PrescaleShift, prescale_shift);
            break;
            
        default:
            assert(0);
            return CODEC_ERROR_UNEXPECTED;
    }
    
    return CODEC_ERROR_OKAY;
}

/*
    @brief Return true if specified type of section is enabled
 */
bool IsSectionEnabled(ENCODER *encoder, SECTION_NUMBER section_number)
{
    if (IsPartEnabled(encoder->enabled_parts, VC5_PART_SECTIONS))
    {
        if (SECTION_NUMBER_MINIMUM <= section_number && section_number <= SECTION_NUMBER_MAXIMUM)
        {
            uint32_t section_mask = SECTION_NUMBER_MASK(section_number);
            
            if (encoder->enabled_sections & section_mask) {
                return true;
            }
        }
    }

    // None of the predefined VC-5 sections are enabled
    return false;
}

/*
    @brief Start a new section with the specified tag
 
    The location of the the tag-value pair that marks the beginning of the new
    section is pushed onto a stack so that the tag-value pair can be updated with
    the actual size of the section when the section is ended by a call to the
    @ref EndSection function.
 */
CODEC_ERROR BeginSection(BITSTREAM *bitstream, TAGWORD tag)
{
    return PushSampleSize(bitstream, tag);
}

/*
    @brief End a section
 
    Update the tag-value pair that marks the section with the actual size of the section.
 */
CODEC_ERROR EndSection(BITSTREAM *bitstream)
{
    return PopSampleSize(bitstream);
}

/*!
    @brief Write an image section header into the bitstream
 */
CODEC_ERROR BeginImageSection(struct _encoder *encoder, BITSTREAM *stream)
{
    assert(0);
    return CODEC_ERROR_OKAY;
}

/*
    @brief Write a section header for the bitstream header into the bitstream
 */
CODEC_ERROR BeginHeaderSection(struct _encoder *encoder, BITSTREAM *stream)
{
    // Write the section header for the bitstream header into the bitstream
    return BeginSection(stream, CODEC_TAG_HeaderSectionTag);
}

/*
    @brief Write a layer section header into the bitstream
     
    Any codec state parameters that are required to decode the layer must be explicitly
    written into the bitstream so that the layer sections and be decoded concurrently.
 */
CODEC_ERROR BeginLayerSection(struct _encoder *encoder, BITSTREAM *stream)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    // Duplicate all codec state parameters required for decoding the layer
    PutCodecState(encoder, stream, SECTION_NUMBER_LAYER);
    
    // Write the section header for the layer into the bitstream
    error = BeginSection(stream, CODEC_TAG_LayerSectionTag);
    
    return error;
}

/*
    @brief Write a channel section header into the bitstream
 
    Any codec state parameters that are required to decode the channel must be explicitly
    written into the bitstream so that the channel sections and be decoded concurrently.
 */
CODEC_ERROR BeginChannelSection(ENCODER *encoder, BITSTREAM *stream)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    // Duplicate all codec state parameters required for decoding the channel
    PutCodecState(encoder, stream, SECTION_NUMBER_CHANNEL);
    
    // Write the section header for the channel into the bitstream
    error = BeginSection(stream, CODEC_TAG_ChannelSectionTag);
    
    return error;
}

/*
    @brief Write a wavelet section header into the bitstream
     
    Any codec state parameters that are required to decode the wavelet must be explicitly
    written into the bitstream so that the wavelet sections and be decoded concurrently.
 */
CODEC_ERROR BeginWaveletSection(struct _encoder *encoder, BITSTREAM *stream)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    // Duplicate all codec state parameters required for decoding the wavelet
    PutCodecState(encoder, stream, SECTION_NUMBER_WAVELET);
    
    // Write the section header for the wavelet into the bitstream
    error = BeginSection(stream, CODEC_TAG_WaveletSectionTag);
    
    return error;
}

/*
    @brief Write a subband section header into the bitstream
     
    Any codec state parameters that are required to decode the subband must be explicitly
    written into the bitstream so that the subband sections and be decoded concurrently.
 */
CODEC_ERROR BeginSubbandSection(struct _encoder *encoder, BITSTREAM *stream)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    // Duplicate all codec state parameters required for decoding the subband
    PutCodecState(encoder, stream, SECTION_NUMBER_SUBBAND);
    
    // Write the section header for the subband into the bitstream
    error = BeginSection(stream, CODEC_TAG_SubbandSectionTag);
    
    return error;
}

/*!
	@brief Set the flags that indicate which sections in VC-5 Part 6 are enabled
 
	The argument is a list of comma-separated integers for the section numbers
 in the VC-5 Part 2 conformance specification that are enabled for this invocation
 of the encoder.
 
 Note: Enabling sections at runtime has no effect unless support for sections
 is compiled into the program by enabling the corresponding compile-time switch
 for VC-5 part 6 (sections).
 */
bool GetEnabledSections(const char *string, uint32_t *enabled_sections_out)
{
    if (string != NULL && enabled_sections_out != NULL)
    {
        // No sections are enabled by default
        ENABLED_SECTIONS enabled_sections = 0;
        
        const char *p = string;
        assert(p != NULL);
        while (*p != '\0')
        {
            char *q = NULL;
            long section_number;
            if (!isdigit((int)*p)) break;
            section_number = strtol(p, &q, 10);
            
            // Is the section number in bounds?
            if (SECTION_NUMBER_MINIMUM <= section_number && section_number <= SECTION_NUMBER_MAXIMUM)
            {
                // Set the bit that corresponds to this section number
                enabled_sections |= SECTION_NUMBER_MASK(section_number);
                
                // Advance to the next section number in the command-line argument
                p = (*q != '\0') ? q + 1 : q;
            }
            else
            {
                // Invalid section number
                assert(0);
                return false;
            }
        }
        
        // Return the bit mask for the enabled sections
        *enabled_sections_out = enabled_sections;
        
        // Should have parsed all section numbers in the argument string
        assert(*p == '\0');
        return true;
    }
    
    // Invalid input arguments
    return false;
}


#endif
