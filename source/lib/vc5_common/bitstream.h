/*! @file bitstream.h
 *
 *  @brief Declaration of the bitstream data structure.
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

#ifndef BITSTREAM_H
#define BITSTREAM_H

/*!
	@brief Bitstream error codes
 
	The bitstream contains its own enumeration of error codes since this
	module may be used in other applications.
 */
typedef enum _bitstream_error
{
    BITSTREAM_ERROR_OKAY = 0,		//!< No error
    BITSTREAM_ERROR_UNDERFLOW,		//!< No unread bits remaining in the bitstream
    BITSTREAM_ERROR_OVERFLOW,		//!< No more bits can be written to the bitstream
    BITSTREAM_ERROR_BADTAG,			//!< Unexpected tag found in the bitstream
    
    //TODO: Add more bitstream errors
    
} BITSTREAM_ERROR;

typedef uint32_t BITWORD;			//!< Data type of the internal bitstream buffer

typedef uint_fast8_t BITCOUNT;		//!< Number of bits in the bitsteam buffer

//! Maximum number of bits in a bit word
static const BITCOUNT bit_word_count = 32;

//! Maximum value of a bit word
#define BIT_WORD_MAX 0xFFFFFFFF

//! Sample offset stack depth
#define MAX_SAMPLE_OFFSET_COUNT		8

/*!
	@brief Declaration of the bitstream data structure
 
	The bitstream uses a byte stream to read bytes from a file or a buffer
	in memory.  This isolates that bitstream module from the type of byte
	stream.
 */
typedef struct _bitstream
{
    BITSTREAM_ERROR error;		//!< Error while processing the bitstream
    struct _stream *stream;		//!< Stream for reading bytes into the buffer
    BITWORD buffer;				//!< Internal buffer holds remaining bits
    BITCOUNT count;				//!< Number of bits remaining in the buffer
    
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    /*!
     The sample offset stack is used to record offsets to the start of nested
     syntax structures.  For example, a sample size segment is written into the
     bitstream with a size of zero, since the size of the syntax element is not
     known in advance.  The offset to the sample size segment is pushed onto the
     sample offset stack so that the location of the sample size segment can be
     updated with the actual size of a syntax element after the complete element
     is written into the bitstream.
     
     This data structure is called the ChunkSizeOffset in the current codec
     implementation.
     */
    uint32_t sample_offset_stack[MAX_SAMPLE_OFFSET_COUNT];
    
    //! Number of entries in the sample offset stack
    uint_fast8_t sample_offset_count;
#endif
    
} BITSTREAM;

#ifdef __cplusplus
extern "C" {
#endif
    
    BITWORD BitMask(int n);

    CODEC_ERROR CodecErrorBitstream(BITSTREAM_ERROR error);

    // Initialize a bitstream data structure
    CODEC_ERROR InitBitstream(BITSTREAM *bitstream);

    // Bind the bitstream to a byte stream
    CODEC_ERROR AttachBitstream(struct _bitstream *bitstream, struct _stream *stream);

    CODEC_ERROR ReleaseBitstream(BITSTREAM *stream);

    BITWORD GetBits(BITSTREAM *stream, BITCOUNT count);

    CODEC_ERROR PutBits(BITSTREAM *stream, BITWORD bits, BITCOUNT count);

    BITWORD AddBits(BITSTREAM *stream, BITWORD bits, BITCOUNT count);

    CODEC_ERROR GetBuffer(BITSTREAM *stream);

    CODEC_ERROR PutBuffer(BITSTREAM *stream);

    CODEC_ERROR PutLong(BITSTREAM *stream, BITWORD longword);

    // Rewind the bitstream and the associated byte stream
    CODEC_ERROR RewindBitstream(BITSTREAM *bitstream);

    CODEC_ERROR BitstreamCodecError(BITSTREAM *bitstream);

    // Return the current position of the bitstream pointer in the sample
    size_t GetBitstreamPosition(BITSTREAM *stream);

    CODEC_ERROR FlushBitstream(BITSTREAM *bitstream);

    CODEC_ERROR GetByteArray(BITSTREAM *bitstream, uint8_t *array, size_t size);

    CODEC_ERROR PutByteArray(BITSTREAM *bitstream, const uint8_t *block, size_t size);

#ifdef __cplusplus
}
#endif

#endif // BITSTREAM_H
