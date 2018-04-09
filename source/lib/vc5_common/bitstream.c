/*! @file bitstream.c
 *
 *  @brief Implementation of a bitstream for reading bits from a byte stream.
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
	@brief Return a mask with the specified number of bits set to one
 */
BITWORD BitMask(int n)
{
    if (n < bit_word_count)
    {
        BITWORD mask = 0;
        if (n > 0) {
            mask = ((1 << n) - 1);
        }
        return mask;
    }
    return BIT_WORD_MAX;
}

/*!
	@brief Initialize a bitstream data structure
 
	This routine is the constructor for the bitstream data type.
 
	The sample offset stack is used to mark the offset to a position
	in the bitstream for computing the size field of sample chunks.
 */
CODEC_ERROR InitBitstream(BITSTREAM *bitstream)
{
    if (bitstream != NULL)
    {
        bitstream->error = BITSTREAM_ERROR_OKAY;
        bitstream->stream = NULL;
        bitstream->buffer = 0;
        bitstream->count = 0;
        
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
        // Initialize the stack of sample offsets
        memset(bitstream->sample_offset_stack, 0, sizeof(bitstream->sample_offset_stack));
        bitstream->sample_offset_count = 0;
#endif

        return CODEC_ERROR_OKAY;
    }
    
    return CODEC_ERROR_NULLPTR;
}

/*!
	@brief Attach a bitstream to a byte stream.
 
	It is permitted for the byte stream to be NULL, in which case the
	bitstream will not be able to replenish its internal buffer, but
	the consequences are undefined.
 */
CODEC_ERROR AttachBitstream(struct _bitstream *bitstream, struct _stream *stream)
{
    assert(bitstream != NULL);
    bitstream->stream = stream;
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Detach a bitstream from a byte stream.
 
	Any resources allocated by the bitstream are released without deallocating
	the bitstream data structure itself.  The byte stream associated with the
	bitstream is not closed by this routine.  The byte stream must be closed,
	if and when appropriate, by the caller.
 */
CODEC_ERROR ReleaseBitstream(BITSTREAM *bitstream)
{
    //TODO: What cleanup needs to be performed?
    (void) bitstream;
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Return the specified number of bits from the bitstream
 */
BITWORD GetBits(BITSTREAM *stream, BITCOUNT count)
{
    // Return zero if the request cannot be satisfied
    BITWORD bits = 0;
    
    // Check that the number of requested bits is valid
    //assert(0 <= count && count <= bit_word_count);
    assert(count <= bit_word_count);
    
    // Check that the unused portion of the bit buffer is empty
    assert((stream->buffer & BitMask(bit_word_count - stream->count)) == 0);
    
    if (count == 0) goto finish;
    
    // Are there enough bits in the buffer to satisfy the request?
    if (count <= stream->count)
    {
        // Right align the requested number of bits in the bit buffer
        bits = (stream->buffer >> (bit_word_count - count));
        
        // Reduce the number of bits in the bit buffer
        stream->buffer <<= count;
        stream->count = (stream->count - count);
    }
    else
    {
        BITCOUNT low_bit_count;
        
        // Use the remaining bits in the bit buffer
        assert(stream->count > 0 || stream->buffer == 0);
        bits = (stream->buffer >> (bit_word_count - count));
        
        // Compute the number of bits to be used from the next word
        low_bit_count = count - stream->count;
        stream->count = 0;
        assert(low_bit_count > 0);
        
        // Fill the bit buffer
        assert(stream->count == 0);
        GetBuffer(stream);
        assert(stream->count >= low_bit_count);
        
        // Use the new bits in the bit buffer
        bits |= (stream->buffer >> (bit_word_count - low_bit_count));
        
        // Reduce the number of bits in the bit buffer
        if (low_bit_count < bit_word_count) {
            stream->buffer <<= low_bit_count;
        }
        else {
            stream->buffer = 0;
        }
        assert(low_bit_count <= stream->count);
        stream->count = (stream->count - low_bit_count);
    }
    
finish:
    // The bit count should never be negative or larger than the size of the bit buffer
    //assert(0 <= stream->count && stream->count <= bit_word_count);
    assert(stream->count <= bit_word_count);
    
    // The unused bits in the bit buffer should all be zero
    assert((stream->buffer & BitMask(bit_word_count - stream->count)) == 0);
    
    // The unused bits in the result should all be zero
    assert((bits & ~BitMask(count)) == 0);
    
    return bits;
}

/*!
	@brief Read more bits and append to an existing word of bits
 
	Read the specified number of bits from the bitstream and append
	the new bits to the right end of the word supplied as an argument.
	This is a convenience routine that is used to accumulate bits that
	may match a codeword.
 */
BITWORD AddBits(BITSTREAM *bitstream, BITWORD bits, BITCOUNT count)
{
    BITWORD new_bits = GetBits(bitstream, count);
    assert((new_bits & ~BitMask(count)) == 0);
    
    bits = (bits << count) | new_bits;
    
    return bits;
}

/*!
	@brief Write a longword (32 bits) to the stream
 */
CODEC_ERROR PutLong(BITSTREAM *stream, BITWORD longword)
{
    return PutBits(stream, longword, bit_word_count);
}

/*!
	@brief Write the specified number of bits to the bitstream
 */
CODEC_ERROR PutBits(BITSTREAM *stream, BITWORD bits, BITCOUNT count)
{
    BITCOUNT unused_bit_count;
    
    if (count == 0) {
        return CODEC_ERROR_OKAY;
    }
    
    // Check that the unused portion of the input bits is empty
    assert((bits & (BitMask(bit_word_count - count) << count)) == 0);
    
    // Check that the number of input bits is valid
    //assert(0 <= count && count <= bit_word_count);
    assert(count <= bit_word_count);
    
    // Check that the unused portion of the bit buffer is empty
    unused_bit_count = bit_word_count - stream->count;
    assert((stream->buffer & BitMask(unused_bit_count)) == 0);
    
    // Is there room in the bit buffer for the new bits?
    if (count <= unused_bit_count)
    {
        // Fill the remaining space in the bit buffer
        stream->buffer |= (bits << (unused_bit_count - count));
        
        // Reduce the number of unused bits in the bit buffer
        stream->count += count;
    }
    else
    {
        // Any room in the bit buffer?
        if (unused_bit_count > 0)
        {
            // Use the number of input bits that will fit in the bit buffer
            stream->buffer |= (bits >> (count - unused_bit_count));
            
            // Reduce the number of input bits by the amount used
            count -= unused_bit_count;
            //assert(count >= 0);
        }
        
        // Write the bit buffer to the byte stream
        PutWord(stream->stream, stream->buffer );
        
        // Insert the remaining input bits into the bit buffer
        stream->buffer = (bits << (bit_word_count - count));
        
        // Increment the number of bits in the bit buffer
        stream->count = count;
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Fill the internal bitstream buffer by reading a byte stream
 
	@todo Need to modify this routine to return an error code if it cannot
	read from the byte stream associated with the bitstream.
 */
CODEC_ERROR GetBuffer(BITSTREAM *bitstream)
{
    // Need to signal an underflow error?
    if (! (bitstream != NULL && bitstream->stream != NULL)) {
        assert(0);
        
        if( bitstream->error == BITSTREAM_ERROR_UNDERFLOW )
            return CODEC_ERROR_OUTOFMEMORY;
    }
    
    // The bit buffer should be empty
    assert(bitstream->count == 0);
    
    // Fill the bit buffer with a word from the byte stream
    bitstream->buffer = Swap32(GetWord(bitstream->stream));
    bitstream->count = bit_word_count;
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Write the internal bitstream buffer to a byte stream
 
	@todo Need to modify this routine to return an error code if it cannot
	write to the byte stream associated with the bitstream.
 */
CODEC_ERROR PutBuffer(BITSTREAM *bitstream)
{
    //TODO: Need to signal an overflow error
    assert(bitstream != NULL && bitstream->stream != NULL);
    
    // The bit buffer should be full
    assert(bitstream->count == bit_word_count);
    
    // Write the bit buffer to the byte stream
    PutWord(bitstream->stream, bitstream->buffer );
    
    // Empty the bit buffer
    bitstream->buffer = 0;
    bitstream->count = 0;
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Convert a bitstream error code to a codec error codec
 
	The bitstream and byte stream modules use a separate enumeration
	for error codes since these modules are used in other applications.
	The bistream error code is embedded in a range of codec error codes
	that are reserved for bitstream errors.
 */
CODEC_ERROR CodecErrorBitstream(BITSTREAM_ERROR error)
{
    uint32_t codec_error = CODEC_ERROR_BITSTREAM;
    codec_error |= (uint32_t)error;
    return (CODEC_ERROR)codec_error;
}

/*!
	@brief Convert a bitstream error code into a codec error code
 
	The bitstream and byte stream modules might be used in other
	applications and have their own errors codes.  This routine
	embeds a bitstream error code into a codec error code.  The
	bitstream error code is carried in the low bits of the codec
	error code.
 
	@todo Eliminate separate error codes for bitstreams?
 */
CODEC_ERROR BitstreamCodecError(BITSTREAM *bitstream)
{
    assert(bitstream != NULL);
    if (! (bitstream != NULL)) {
        return CODEC_ERROR_NULLPTR;
    }
    
    return CodecErrorBitstream(bitstream->error);
}

/*!
 @brief Read a block of bytes from the bitstream
 */
CODEC_ERROR GetByteArray(BITSTREAM *bitstream, uint8_t *array, size_t size)
{
    size_t i;
    
    for (i = 0; i < size; i++)
    {
        array[i] = GetBits(bitstream, 8);
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
 @brief Write a block of bytes into the bitstream
 */
CODEC_ERROR PutByteArray(BITSTREAM *bitstream, const uint8_t *array, size_t size)
{
    size_t i;
    
    for (i = 0; i < size; i++)
    {
        PutBits(bitstream, array[i], 8);
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Write any bits in the buffer to the byte stream
 */
CODEC_ERROR FlushBitstream(BITSTREAM *bitstream)
{
    // Any bits remaining in the bit buffer?
    if (bitstream->count > 0)
    {
        // Write the bit buffer to the output stream
        PutBuffer(bitstream);
    }
    
    // Indicate that the bitstream buffer is empty
    bitstream->count = 0;
    bitstream->buffer = 0;
    
    // Flush the output stream
    FlushStream(bitstream->stream);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Get the current position in the byte stream
 
	The current position in the byte stream associated with the
	bitstream is returned.  The intent is to allow the bitstream
	(and the associated byte stream) to be restored to the saved
	position.
 
	@todo Need to record the state of the bitstream buffer and
	bit count so that the entire bitstream state can be restored.
 */
size_t GetBitstreamPosition(BITSTREAM *bitstream)
{
    if (bitstream->count == bit_word_count) {
        PutBuffer(bitstream);
    }
    
    // The bit buffer must be empty
    assert(bitstream->count == 0);
    
    // The bit buffer must be empty
    return (bitstream->stream->byte_count);
}

/*!
	@brief Rewind the bitstream
 
	This routine rewinds the bitstream to the beginning of the byte stream
	that has been attached to the bitstream.  The byte stream is also reset.
 
	If the byte stream could not be reset, then the internal bitstream state
	is not reset.
 */
CODEC_ERROR RewindBitstream(BITSTREAM *bitstream)
{
    assert(bitstream != NULL);
    if (! (bitstream != NULL)) {
        return CODEC_ERROR_NULLPTR;
    }
    
    if (bitstream->stream != NULL) {
        CODEC_ERROR error = RewindStream(bitstream->stream);
        if (error != CODEC_ERROR_OKAY) {
            return error;
        }
    }
    
    // Reset the bitstream internal state
    bitstream->buffer = 0;
    bitstream->count = 0;
    bitstream->error = BITSTREAM_ERROR_OKAY;
    
    return CODEC_ERROR_OKAY;
}

