/*! @file stream.h
 *
 *  @brief This module implements a byte stream abstraction that hides the details
 *  of how a stream of bytes is read or written on demand by the bitstream.
 *  The    byte stream can be bound to a binary file opened for reading (writing),
 *  to a buffer in memory, or to a module that reads (writes) a video track
 *  in a media container.
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

// Local functions
CODEC_ERROR GetBlockFile(STREAM *stream, void *buffer, size_t size, size_t offset);
CODEC_ERROR PutBlockFile(STREAM *stream, void *buffer, size_t size, size_t offset);
CODEC_ERROR GetBlockMemory(STREAM *stream, void *buffer, size_t size, size_t offset);
CODEC_ERROR PutBlockMemory(STREAM *stream, void *buffer, size_t size, size_t offset);

/*!
	@brief Open a stream for reading bytes from the specified file

*/
CODEC_ERROR OpenStream(STREAM *stream, const char *pathname)
{
	assert(stream != NULL);
	if (! (stream != NULL)) {
		return CODEC_ERROR_NULLPTR;
	}

	// Clear all members of the stream data structure
	memset(stream, 0, sizeof(STREAM));

	// Open the file and bind it to the stream
	stream->location.file.iobuf = fopen(pathname, "rb");
	assert(stream->location.file.iobuf != NULL);
	if (! (stream->location.file.iobuf != NULL)) {
		return CODEC_ERROR_OPEN_FILE_FAILED;
	}

	// Set the stream type and access
	stream->type = STREAM_TYPE_FILE;
	stream->access = STREAM_ACCESS_READ;

	// Clear the number of bytes read from the stream
	stream->byte_count = 0;

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Create a stream for writing bytes to a specified file

*/
CODEC_ERROR CreateStream(STREAM *stream, const char *pathname)
{
	assert(stream != NULL);
	if (! (stream != NULL)) {
		return CODEC_ERROR_NULLPTR;
	}

	// Clear all members of the stream data structure
	memset(stream, 0, sizeof(STREAM));

	// Open the file and bind it to the stream
	stream->location.file.iobuf = fopen(pathname, "wb+");
	assert(stream->location.file.iobuf != NULL);
	if (! (stream->location.file.iobuf != NULL)) {
		return CODEC_ERROR_CREATE_FILE_FAILED;
	}

	// Set the stream type and access
	stream->type = STREAM_TYPE_FILE;
	stream->access = STREAM_ACCESS_WRITE;

	// Clear the number of bytes written to the stream
	stream->byte_count = 0;

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Read a word from a byte stream

	This routine is used by the bitstream to read a word from a byte stream.
	A word is the number of bytes that can be stored in the internal buffer
	used by the bitstream.

	@todo Need to modify the routine to return an indication of end of file
	or an error reading from the byte stream.
*/
BITWORD GetWord(STREAM *stream)
{
    BITWORD buffer = 0;
    size_t bytes_read = sizeof(buffer);
    
    assert(stream != NULL);
    
    switch (stream->type)
    {
        case STREAM_TYPE_FILE:
            bytes_read = fread(&buffer, 1, sizeof(buffer), stream->location.file.iobuf);
            assert(bytes_read == sizeof(buffer));
            break;
            
        case STREAM_TYPE_MEMORY:
            memcpy(&buffer, (uint8_t *)stream->location.memory.buffer + stream->byte_count, sizeof(buffer));
            break;
            
        default:
            assert(0);
            break;
    }
    
    if (bytes_read > 0)
        stream->byte_count += sizeof(buffer);
    
	return buffer;
}

/*!
	@brief Read a byte from a byte stream
*/
uint8_t GetByte(STREAM *stream)
{
    assert(stream != NULL);
    int byte = 0;
    
    switch (stream->type)
    {
        case STREAM_TYPE_FILE:
            byte = fgetc(stream->location.file.iobuf);
            break;
            
        case STREAM_TYPE_MEMORY:
            byte = ((uint8_t *)stream->location.memory.buffer)[stream->byte_count];            
            break;
            
        default:
            assert(0);
            break;
    }
    
    stream->byte_count++;
    assert(byte >= 0 && (byte & ~0xFF) == 0);
    
	return (uint8_t)byte;
}

/*!
	@brief Write a word to a byte stream

	This routine is used by the bitstream to write a word to a byte stream.
	A word is the number of bytes that can be stored in the internal buffer
	used by the bitstream.

	@todo Need to modify the routine to return an indication of an error
	writing to the byte stream.
*/
CODEC_ERROR PutWord(STREAM *stream, BITWORD word)
{
	size_t written;

    word = Swap32(word);
    
	assert(stream != NULL);
    
	switch (stream->type)
	{
	case STREAM_TYPE_FILE:
		written = fwrite(&word, sizeof(word), 1, stream->location.file.iobuf);
		if (written == 0)
			return CODEC_ERROR_FILE_WRITE_FAILED;
		break;

	case STREAM_TYPE_MEMORY:
        {
            uint8_t* buffer = (uint8_t *)stream->location.memory.buffer + stream->byte_count;
            
            memcpy(buffer, &word, sizeof(word));            
        }
		break;

	default:
		assert(0);
		break;
	}

	stream->byte_count += sizeof(word);
    
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Write a byte to a byte stream
*/
CODEC_ERROR PutByte(STREAM *stream, uint8_t byte)
{
	assert(stream != NULL);

	//assert(byte >= 0 && (byte & ~0xFF) == 0);

	switch (stream->type)
	{
	case STREAM_TYPE_FILE:
		if (fputc(byte, stream->location.file.iobuf) == EOF)
			return CODEC_ERROR_FILE_WRITE_FAILED;
		break;

	case STREAM_TYPE_MEMORY:
		((uint8_t *)stream->location.memory.buffer)[stream->byte_count] = byte;
		break;

	default:
		assert(0);
		break;
	}

	stream->byte_count++;

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Rewind the stream to the beginning of the buffer or file
*/
CODEC_ERROR RewindStream(STREAM *stream)
{
	assert(stream != NULL);
	if (! (stream != NULL)) {
		return CODEC_ERROR_NULLPTR;
	}
	
    if( stream->type == STREAM_TYPE_FILE )
    {
        if (stream->location.file.iobuf != NULL) {
            assert(fseek(stream->location.file.iobuf, 0, SEEK_SET) == 0);
            return CODEC_ERROR_BITSTREAM;
        }
    }

	stream->byte_count = 0;

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Skip the specified number of bytes in the stream
*/
CODEC_ERROR SkipBytes(STREAM *stream, size_t size)
{
	for (; size > 0; size--)
	{
		(void)GetByte(stream);
	}
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Pad the specified number of bytes in the stream
*/
CODEC_ERROR PadBytes(STREAM *stream, size_t size)
{
	const uint8_t byte = 0;
	for (; size > 0; size--)
	{
		PutByte(stream, byte);
	}
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Write the stream buffer to the file
*/
CODEC_ERROR FlushStream(STREAM *stream)
{
	assert(stream != NULL);
	if (! (stream != NULL)) {
		return CODEC_ERROR_NULLPTR;
	}

	if (stream->type == STREAM_TYPE_FILE)
	{
		int result = fflush(stream->location.file.iobuf);
		if (result != 0) {
			return CODEC_ERROR_FILE_FLUSH_FAILED;
		}
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Create a byte stream for reading from a memory location
 */
CODEC_ERROR OpenStreamBuffer(STREAM *stream, void *buffer, size_t size)
{
    assert(stream != NULL);
    if (! (stream != NULL)) {
        return CODEC_ERROR_NULLPTR;
    }
    
    // Clear all members of the stream data structure
    memset(stream, 0, sizeof(STREAM));
    
    // Bind the stream to the buffer
    stream->location.memory.buffer = buffer;
    stream->location.memory.size = size;
    
    // Set the stream type and access
    stream->type = STREAM_TYPE_MEMORY;
    stream->access = STREAM_ACCESS_READ;
    
    // Clear the number of bytes written to the stream
    stream->byte_count = 0;
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Create a byte stream for writing to a memory location
*/
CODEC_ERROR CreateStreamBuffer(STREAM *stream, void *buffer, size_t size)
{
	assert(stream != NULL);
	if (! (stream != NULL)) {
		return CODEC_ERROR_NULLPTR;
	}

	// Clear all members of the stream data structure
	memset(stream, 0, sizeof(STREAM));

	// Bind the stream to the buffer
	stream->location.memory.buffer = buffer;
	stream->location.memory.size = size;

	// Set the stream type and access
	stream->type = STREAM_TYPE_MEMORY;
	stream->access = STREAM_ACCESS_WRITE;

	// Clear the number of bytes written to the stream
	stream->byte_count = 0;
    
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Return the starting address and number of bytes in a buffer

	This routine is used to get the address and count of the bytes written
	to a memory stream (buffer).
*/
CODEC_ERROR GetStreamBuffer(STREAM *stream, void **buffer_out, size_t *size_out)
{
	assert(stream != NULL);
	if (! (stream != NULL)) {
		return CODEC_ERROR_NULLPTR;
	}

	assert(stream->type == STREAM_TYPE_MEMORY);

	if (buffer_out != NULL) {
		*buffer_out = stream->location.memory.buffer;
	}

	if (size_out != NULL) {
		*size_out = stream->byte_count;
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Read a block of data at the specified offset in the byte stream
*/
CODEC_ERROR GetBlock(STREAM *stream, void *buffer, size_t size, size_t offset)
{
	switch (stream->type)
	{
	case STREAM_TYPE_FILE:
		return GetBlockFile(stream, buffer, size, offset);
		break;

	case STREAM_TYPE_MEMORY:
		return GetBlockMemory(stream, buffer, size, offset);
		break;

	case STREAM_TYPE_UNKNOWN:
		assert(0);
		break;
	}

	return CODEC_ERROR_UNEXPECTED;
}

/*!
	@brief Read a block of data from a file stream
*/
CODEC_ERROR GetBlockFile(STREAM *stream, void *buffer, size_t size, size_t offset)
{
	FILE *file = stream->location.file.iobuf;
	fpos_t position;

    (void)file;
    (void)position;
    
	// Save the current position in the file
	if (fgetpos(file, &position) != 0) {
		return CODEC_ERROR_FILE_GET_POSITION;
	}

	// Seek to the specified offset
	assert(offset <= LONG_MAX);
	if (fseek(file, (long)offset, SEEK_SET) != 0) {
		return CODEC_ERROR_FILE_SEEK;
	}

	// Read data from the file
	if (fread(buffer, size, 1, file) != 1) {
		return CODEC_ERROR_FILE_READ;
	}

	// Return to the previous position in the file
	// if (fseek(file, (long)position, SEEK_SET) != 0) {
	if (fsetpos(file, &position) != 0) {
		return CODEC_ERROR_FILE_SEEK;
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Read a block of data from a memory stream (buffer)
*/
CODEC_ERROR GetBlockMemory(STREAM *stream, void *buffer, size_t size, size_t offset)
{
	uint8_t *block = (uint8_t *)stream->location.memory.buffer + offset;
	memcpy(buffer, block, size);
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Write a block of data at the specified offset in the byte stream
*/
CODEC_ERROR PutBlock(STREAM *stream, void *buffer, size_t size, size_t offset)
{
	switch (stream->type)
	{
	case STREAM_TYPE_FILE:
		return PutBlockFile(stream, buffer, size, offset);
		break;

	case STREAM_TYPE_MEMORY:
		return PutBlockMemory(stream, buffer, size, offset);
		break;

	case STREAM_TYPE_UNKNOWN:
		assert(0);
		break;
	}

	return CODEC_ERROR_UNEXPECTED;
}

/*!
	@brief Write a block of data at the specified offset in a file stream
*/
CODEC_ERROR PutBlockFile(STREAM *stream, void *buffer, size_t size, size_t offset)
{
	FILE *file = stream->location.file.iobuf;
	fpos_t position;

    (void)file;
    (void)position;
    
	// Save the current position in the file
	if (fgetpos(file, &position) != 0) {
		return CODEC_ERROR_FILE_GET_POSITION;
	}

	// Seek to the specified offset and write to the file
	assert(offset <= LONG_MAX);
	if (fseek(file, (long)offset, SEEK_SET) != 0) {
		return CODEC_ERROR_FILE_SEEK;
	}

	// Write data to the file
	if (fwrite(buffer, size, 1, file) != 1) {
		return CODEC_ERROR_FILE_WRITE;
	}

	// Return to the previous position in the file
	// if (fseek(file, (long)position, SEEK_SET) != 0) {
	if (fsetpos(file, &position) != 0) {
		return CODEC_ERROR_FILE_SEEK;
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Write a block of data at the specified offset in a memory stream
*/
CODEC_ERROR PutBlockMemory(STREAM *stream, void *buffer, size_t size, size_t offset)
{
	uint8_t *block = (uint8_t *)stream->location.memory.buffer + offset;
	memcpy(block, buffer, size);
	return CODEC_ERROR_OKAY;
}


