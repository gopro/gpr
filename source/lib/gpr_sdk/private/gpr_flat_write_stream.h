/*! @file gpr_flat_write_stream.h
 *
 *  @brief Declaration of gpr_flat_write_stream, a growable contiguous
 *  in-memory dng_stream for writing. Unlike dng_memory_stream (a list of
 *  64 KB pages, which forces a full copy to extract the finished file), the
 *  bytes live in one allocator-owned block, so the finished file is handed
 *  to the caller via detach() with no final copy.
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

#ifndef GPR_FLAT_WRITE_STREAM_H
#define GPR_FLAT_WRITE_STREAM_H

#include "gpr_buffer.h"

#include "dng_stream.h"

// Growable contiguous in-memory write stream. The base class delivers writes
// as offset-addressed DoWrite calls (including the TIFF offset seek-back
// patching done by dng_image_writer::WriteDNG), which a flat buffer satisfies
// naturally.
class gpr_flat_write_stream : public dng_stream
{
public:
    gpr_flat_write_stream( gpr_malloc mem_alloc, gpr_free mem_free, uint64 initial_capacity );

    virtual ~gpr_flat_write_stream();

    // Hand the buffer to the caller, sized to the stream length. The underlying allocation
    // may be larger; that is fine since consumers free by pointer with no size introspection.
    // The stream must not be used after this call.
    void detach( gpr_buffer* out );

protected:

    virtual uint64 DoGetLength();

    virtual void DoRead( void *data, uint32 count, uint64 offset );

    virtual void DoSetLength( uint64 length );

    virtual void DoWrite( const void *data, uint32 count, uint64 offset );

private:

    void GrowTo( uint64 size );

    // Hidden copy constructor and assignment operator.
    gpr_flat_write_stream( const gpr_flat_write_stream& );
    gpr_flat_write_stream& operator= ( const gpr_flat_write_stream& );

    gpr_malloc fMemAlloc;
    gpr_free   fMemFree;

    void*  fData;
    uint64 fCapacity;
    uint64 fLogicalLength;
};

#endif // GPR_FLAT_WRITE_STREAM_H
