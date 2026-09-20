/*! @file gpr_flat_write_stream.cpp
 *
 *  @brief Implementation of gpr_flat_write_stream.
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

#include "gpr_flat_write_stream.h"

#include "dng_exceptions.h"

#include <string.h>

gpr_flat_write_stream::gpr_flat_write_stream( gpr_malloc mem_alloc, gpr_free mem_free, uint64 initial_capacity )
    : dng_stream( (dng_abort_sniffer*)NULL, kBigBufferSize )
    , fMemAlloc( mem_alloc )
    , fMemFree( mem_free )
    , fData( NULL )
    , fCapacity( 0 )
    , fLogicalLength( 0 )
{
    GrowTo( initial_capacity ? initial_capacity : 1 );
}

gpr_flat_write_stream::~gpr_flat_write_stream()
{
    if( fData )
    {
        fMemFree( fData );
    }
}

void gpr_flat_write_stream::detach( gpr_buffer* out )
{
    Flush();

    out->buffer = fData;
    out->size   = (size_t)Length();

    fData          = NULL;
    fCapacity      = 0;
    fLogicalLength = 0;
}

uint64 gpr_flat_write_stream::DoGetLength()
{
    return fLogicalLength;
}

void gpr_flat_write_stream::DoRead( void *data, uint32 count, uint64 offset )
{
    if( offset + count > fLogicalLength )
    {
        ThrowEndOfFile();
    }

    memcpy( data, (const char*)fData + offset, count );
}

void gpr_flat_write_stream::DoSetLength( uint64 length )
{
    GrowTo( length );

    fLogicalLength = length;
}

void gpr_flat_write_stream::DoWrite( const void *data, uint32 count, uint64 offset )
{
    GrowTo( offset + count );

    memcpy( (char*)fData + offset, data, count );

    if( offset + count > fLogicalLength )
    {
        fLogicalLength = offset + count;
    }
}

void gpr_flat_write_stream::GrowTo( uint64 size )
{
    if( size <= fCapacity )
    {
        return;
    }

    uint64 new_capacity = fCapacity ? fCapacity : 1;

    while( new_capacity < size )
    {
        new_capacity *= 2;
    }

    void* new_data = fMemAlloc( (size_t)new_capacity );

    if( new_data == NULL )
    {
        ThrowMemoryFull();
    }

    if( fData )
    {
        memcpy( new_data, fData, (size_t)fLogicalLength );
        fMemFree( fData );
    }

    fData     = new_data;
    fCapacity = new_capacity;
}
