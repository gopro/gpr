/*! @file stream.h
 *
 *  @brief The stream abstracts the methods used by bitstreams to output bytes
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

#ifndef STREAM_H
#define STREAM_H

//! Type of stream (binary file or memory buffer)
typedef enum _stream_type
{
	STREAM_TYPE_UNKNOWN = 0,		//!< Unknown type of stream
	STREAM_TYPE_FILE,				//!< Simple binary file
	STREAM_TYPE_MEMORY,				//!< Buffer in memory

} STREAM_TYPE;

/*!
	@brief Stream access (read or write)

	The stream provided with the reference decoder only supports
	read access.
*/
typedef enum _stream_access
{
	STREAM_ACCESS_UNKNOWN = 0,
	STREAM_ACCESS_READ,
	STREAM_ACCESS_WRITE,

} STREAM_ACCESS;


/*!
	@brief Declaration of the data structure for a byte stream

	The byte stream encapsulates the location of encoded images and the
	means for reading (writing) encoded images samples.  The byte stream
	could be a binary file that has been opened for reading (writing) or
	a buffer in memory.  The reference codec uses a binary file as the byte
	stream so the functionality for streams attached to memory buffers has
	not been tested.

	It is intended that the byte stream can be enhanced to read (write)
	encoded images from (into) a track in a media container.
*/
#define STREAM_CACHE_SIZE 16
typedef struct _stream
{
	STREAM_TYPE type;		//!< Type of stream (file or memory buffer)
	STREAM_ACCESS access;	//!< Type of access (read or write)

	//! Union of parameters for different types of streams
	union _location
	{
		//! Parameters for a binary file stream
		struct _file
		{
			FILE *iobuf;	//!< Binary file that contains the stream
			BITWORD cache[STREAM_CACHE_SIZE];
			int		cache_index;

		} file;			//!< Parameters for a stream in a binary file

        //! Parameters for a stream bound to a memory buffer
		struct _memory
		{
			void *buffer;	//!< Memory buffer that contains the stream
			size_t size;	//!< Length of the stream (in bytes)

		} memory;		//!< Parameters for a stream in a memory buffer

		//TODO: Add other stream types for media containers

	} location;		//!< Location of the byte stream (file or memory buffer)

	size_t byte_count;		//!< Number of bytes read or written to the stream

} STREAM;

#ifdef __cplusplus
extern "C" {
#endif
    
    CODEC_ERROR OpenStream(STREAM *stream, const char *pathname);

    CODEC_ERROR CreateStream(STREAM *stream, const char *pathname);

    CODEC_ERROR RewindStream(STREAM *stream);

    BITWORD GetWord(STREAM *stream);

    uint8_t GetByte(STREAM *stream);

    CODEC_ERROR SkipBytes(STREAM *stream, size_t size);

    CODEC_ERROR PutWord(STREAM *stream, BITWORD word);

    CODEC_ERROR PutByte(STREAM *stream, uint8_t byte);

    CODEC_ERROR PadBytes(STREAM *stream, size_t size);

    CODEC_ERROR FlushStream(STREAM *stream);

    CODEC_ERROR OpenStreamBuffer(STREAM *stream, void *buffer, size_t size);

    CODEC_ERROR CreateStreamBuffer(STREAM *stream, void *buffer, size_t size);

    CODEC_ERROR GetStreamBuffer(STREAM *stream, void **buffer_out, size_t *size_out);

    CODEC_ERROR GetBlock(STREAM *stream, void *buffer, size_t size, size_t offset);

    CODEC_ERROR PutBlock(STREAM *stream, void *buffer, size_t size, size_t offset);

#ifdef __cplusplus
}
#endif

#endif
