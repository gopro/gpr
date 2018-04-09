/*! @file syntax.c
 *
 *  @brief Implementation of functions for writing bitstream syntax of encoded samples.
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

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)

/*!
	@brief Pop the top value from the sample offset stack
 */
static uint32_t PopSampleOffsetStack(BITSTREAM *bitstream)
{
    assert(bitstream->sample_offset_count > 0);
    return bitstream->sample_offset_stack[--bitstream->sample_offset_count];
}

#endif

/*!
	@brief Write a segment at the specified offset in the bitstream
 
	The segment at the specified offset in the bitstream is overwritten by the new
	segment provided as an argument.  Typically this is done to update a segment that
	is intended to provide the size or offset to a syntax element in the encoded sample.
 */
CODEC_ERROR PutSampleOffsetSegment(BITSTREAM *bitstream, uint32_t offset, TAGVALUE segment)
{
    // Translate the segment to network byte order
    uint32_t buffer = Swap32(segment.longword);
    
    // Must write the segment on a segment boundary
    assert((offset % sizeof(TAGVALUE)) == 0);
    
    // Write the segment to the byte stream at the specified offset
    return PutBlock(bitstream->stream, &buffer, sizeof(buffer), offset);
}

static bool IsTagOptional(TAGWORD tag)
{
    return (tag < 0);
}

/*!
	@brief Write the trailer for the lowpass band into the bitstream
 
	This routine writes a marker into the bitstream that can aid in debugging,
	but the most important function is to update the segment that contains the
	size of this subband with the actual size of the lowpass band.
 */
CODEC_ERROR PutVideoLowpassTrailer(BITSTREAM *stream)
{
    // Check that the bitstream is tag aligned before writing the pixels
    assert(IsAlignedSegment(stream));
    
    // Set the size of the large chunk for the lowpass band codeblock
    PopSampleSize(stream);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Write the next tag value pair to the bitstream
 
	@todo Change the code to use the @ref PutLong function?
 */
CODEC_ERROR PutTagValue(BITSTREAM *stream, TAGVALUE segment)
{
    CODEC_ERROR error;
    
    // Write the tag to the bitstream
    error = PutBits(stream, segment.tuple.tag, tagword_count);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    // Write the value to the bitstream
    error = PutBits(stream, segment.tuple.value, tagword_count);
    
    return error;
}

/*!
	@brief Write a required tag value pair
 
	@todo Should the tag value pair be output on a segment boundary?
 */
CODEC_ERROR PutTagPair(BITSTREAM *stream, int tag, int value)
{
    // The bitstream should be aligned on a tag word boundary
    assert(IsAlignedTag(stream));
    
    // The value must fit within a tag word
    assert(((uint32_t)value & ~(uint32_t)CODEC_TAG_MASK) == 0);
    
    PutLong(stream, ((uint32_t )tag << 16) | (value & CODEC_TAG_MASK));
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Write an optional tag value pair
 
	@todo Should the tag value pair be output on a segment boundary?
 */
CODEC_ERROR PutTagPairOptional(BITSTREAM *stream, int tag, int value)
{
    // The bitstream should be aligned on a tag word boundary
    assert(IsAlignedTag(stream));
    
    // The value must fit within a tag word
    assert(((uint32_t)value & ~(uint32_t)CODEC_TAG_MASK) == 0);
    
    // Set the optional tag bit
    //tag |= CODEC_TAG_OPTIONAL;
    //tag = NEG(tag);
    tag = neg(tag);
    
    PutLong(stream, ((uint32_t )tag << 16) | (value & CODEC_TAG_MASK));
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Write a tag value pair that specifies the size of a syntax element
 
	The routine pushes the current position in the bitstream onto the sample offset
	stack and writes a tag value pair for the size of the current syntax element.
	The routine @ref PopSampleSize overwrites the segment with a tag value pair
	that contains the actual size of the syntax element.
 
	This routine corresponds to the routine SizeTagPush in the current codec implementation.
 */
CODEC_ERROR PushSampleSize(BITSTREAM *bitstream, TAGWORD tag)
{
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    
    size_t position = GetBitstreamPosition(bitstream);
    
    // Check for stack overflow
    assert(bitstream->sample_offset_count < MAX_SAMPLE_OFFSET_COUNT);
    
    // Check that the bitstream position can be pushed onto the stack
    assert(position <= UINT32_MAX);
    
    // Push the current sample offset onto the stack
    bitstream->sample_offset_stack[bitstream->sample_offset_count++] = (uint32_t)position;

#endif

    // Write a tag value pair for the size of this chunk
    PutTagPairOptional(bitstream, tag, 0);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Update a sample size segment with the actual size of the syntax element
 
	This routine pops the offset in the bitstream to the most recent tag value pair
	that was written into the bitstream from the sample offset stack and overwrites
	the segment with the tag value pair that contains the actual size of the syntax
	element.
 
	This routine corresponds to the routine SizeTagPop in the current codec implementation.
 */
CODEC_ERROR PopSampleSize(BITSTREAM *bitstream)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    
    if (bitstream->sample_offset_count > 0)
    {
        TAGVALUE segment;
        TAGWORD tag;
        
        uint32_t current_offset;
        uint32_t previous_offset;
        
        uint32_t chunk_size;
        
        size_t position = GetBitstreamPosition(bitstream);
        
        // Get the offset to the current position in the bitstream
        assert(position <= UINT32_MAX);
        current_offset = (uint32_t)position;
        
        // Pop the offset for this chunk from the sample offset stack
        previous_offset = PopSampleOffsetStack(bitstream);
        
        assert(previous_offset < current_offset);
        
        // Get the segment for the chunk written at the most recent offset
        error = GetSampleOffsetSegment(bitstream, previous_offset, &segment);
        if (error != CODEC_ERROR_OKAY) {
            return error;
        }
        
        // Get the tag for the chunk segment
        tag = segment.tuple.tag;
        
        // Should be an optional tag-value pair
        assert(IsTagOptional(tag));
        if (! (IsTagOptional(tag))) {
            return CODEC_ERROR_UNEXPECTED;
        }
        
        // Convert the tag to required
        tag = RequiredTag(tag);
        
        // Compute the size of the current chunk
        chunk_size = current_offset - previous_offset;
        
        if (chunk_size >= 4)
        {
            // The chunk payload should contain an integer number of segments
            assert((chunk_size % sizeof(TAGVALUE)) == 0);
            
            // Compute the number of segments in the chunk payload
            chunk_size = (chunk_size / sizeof(TAGVALUE)) - 1;
        }
        else
        {
            chunk_size = 0;
        }
        
        // Does this chunk have a 24-bit size field?
        if (tag & CODEC_TAG_LARGE_CHUNK)
        {
            // Add the most significant eight bits of the size to the tag
            tag |= ((chunk_size >> 16) & 0xFF);
        }
        
        // The segment value is the least significant 16 bits of the payload size
        chunk_size &= 0xFFFF;
        
        // Update the segment with the optional tag and chunk size
        segment.tuple.tag = OptionalTag(tag);
        segment.tuple.value = chunk_size;
        
        return PutSampleOffsetSegment(bitstream, previous_offset, segment);
    }
#endif

    return CODEC_ERROR_UNEXPECTED;
}

/*!
	@brief Write the bitstream start marker
 */
CODEC_ERROR PutBitstreamStartMarker(BITSTREAM *stream)
{
    assert(stream != NULL);
    if (! (stream != NULL)) {
        return CODEC_ERROR_UNEXPECTED;
    }
    
    return PutLong(stream, StartMarkerSegment);
}
