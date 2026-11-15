/*! @file gpr_allocator.h
 *
 *  @brief The API for memory allocation and deallocation functions.
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

#ifndef GPR_ALLOCATOR_H
#define GPR_ALLOCATOR_H

#include "gpr_platform.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
    
    extern void* gpr_global_malloc(size_t size);
    
    extern void  gpr_global_free(void *block);
    
    typedef void* (*gpr_malloc)(size_t size); /* Malloc callback function typedef */
    
    typedef void  (*gpr_free)(void* p);       /* Free callback function typedef */
    
    typedef struct
    {
        gpr_malloc      Alloc;              // Callback function to allocate memory
        
        gpr_free        Free;               // Callback function to free memory
        
    } gpr_allocator;

    /* Preserve caller allocator ownership while aligning hot codec buffers.
     * Pair only with gpr_aligned_free; never pass the adjusted pointer to Free. */
    static inline void *gpr_aligned_alloc(const gpr_allocator *allocator, size_t bytes)
    {
        const size_t overhead = 63 + sizeof(void *);
        if (bytes > SIZE_MAX - overhead) return NULL;
        void *base = allocator->Alloc(bytes + overhead);
        if (base == NULL) return NULL;
        uintptr_t address = ((uintptr_t)base + overhead) & ~(uintptr_t)63;
        ((void **)address)[-1] = base;
        return (void *)address;
    }

    static inline void gpr_aligned_free(const gpr_allocator *allocator, void *pointer)
    {
        if (pointer != NULL) allocator->Free(((void **)pointer)[-1]);
    }

#ifdef __cplusplus
}
#endif

#endif // GPR_ALLOCATOR_H
