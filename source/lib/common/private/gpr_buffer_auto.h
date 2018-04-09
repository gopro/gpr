/*! @file gpr_internal_buffer
 *
 *  @brief Declaration of gpr_buffer_auto object. This object
 *  implements a buffer that carries size information. gpr_buffer_auto can
 *  deallocate itself in destructor (depending on free_in_destructor member)
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

#ifndef GPR_INTERNAL_BUFFER_H
#define GPR_INTERNAL_BUFFER_H

#include "gpr_allocator.h"
#include "gpr_buffer.h"
#include "gpr_platform.h"
#include "stdc_includes.h"

class gpr_buffer_auto
{
private:
    gpr_buffer              buffer;
    
    bool                    free_in_destructor;
    
    gpr_malloc              mem_alloc;
    
    gpr_free                mem_free;
    
public:
    void reset()
    {
        buffer.buffer = NULL;
        buffer.size = 0;
        free_in_destructor = false;
    }
    
    gpr_buffer_auto(gpr_malloc malloc_function, gpr_free free_function)
    {
        reset();
        
        mem_alloc = malloc_function;
        mem_free  = free_function;
    }

    ~gpr_buffer_auto()
    {
        deallocate();
    }
    
    void allocate(size_t size_of_memory)
    {
        assert(buffer.buffer == NULL);
        assert(buffer.size == 0);
        
        buffer.size = size_of_memory;
        
        if(buffer.size > 0)
        {
            buffer.buffer = mem_alloc(buffer.size);
            assert(buffer.buffer);
            
            free_in_destructor = true;
        }
    }
    
    void deallocate()
    {
        if(buffer.buffer && free_in_destructor)
            mem_free( (char*)buffer.buffer );
        
        reset();
    }

    void resize( size_t new_size )
    {
        buffer.size    = new_size;
        
        assert(buffer.buffer);
        assert(buffer.size);
    }
    
    void set(void* _buffer, size_t _size, bool _free_in_destructor = false )
    {
        assert(buffer.buffer == NULL);
        assert(buffer.size == 0);
        
        buffer.buffer  = _buffer;
        buffer.size    = _size;
        free_in_destructor = _free_in_destructor;
        
        assert(buffer.buffer);
        assert(buffer.size);
    }

    void zero()
    {
        // Cant call zero for the case when buffer has been preallocated
        // Call deallocate()
        assert( free_in_destructor == false );
        
        buffer.buffer  = NULL;
        buffer.size    = 0;
    }

    bool is_valid() const
    {
        return ( buffer.buffer != NULL ) && buffer.size > 0;
    }
    
// Accessors
    void* get_buffer() const { return buffer.buffer; }

    char* to_char() const { return (char*)buffer.buffer; }
    
    unsigned char* to_uchar() const { return (unsigned char*)buffer.buffer; }

    unsigned short* to_ushort() const { return (unsigned short*)buffer.buffer; }
    
    uint16_t* to_uint16_t() const { return (uint16_t*)buffer.buffer; }

    size_t get_size() const { return buffer.size; }
    
    gpr_malloc get_malloc() const { return mem_alloc; }

    gpr_free get_free() const { return mem_free; }
    
// Operations
    int read_from_file(const char* file_path);
    
    int write_to_file(const char* file_path);
    
    const gpr_buffer& get_gpr_buffer() { return buffer; }
};

#endif // GPR_INTERNAL_BUFFER_H
