#include "gpr_allocator.h"
#include <assert.h>
#include <string.h>

static void *allocation, *owner;
static unsigned skew, allocations, releases;
static void *Allocate(size_t bytes)
{
    allocation = malloc(bytes + 64);
    if (!allocation) return NULL;
    owner = (unsigned char *)allocation + skew;
    allocations++;
    return owner;
}
static void Release(void *pointer)
{
    assert(pointer == owner);
    releases++;
    free(allocation);
}
int main(void)
{
    gpr_allocator allocator = {Allocate, Release};
    for (skew = 0; skew < 64; skew++) {
        for (size_t bytes = 1; bytes < 256; bytes++) {
            void *pointer = gpr_aligned_alloc(&allocator, bytes);
            assert(pointer && ((uintptr_t)pointer & 63) == 0);
            memset(pointer, 0x5a, bytes);
            gpr_aligned_free(&allocator, pointer);
        }
    }
    assert(allocations == releases);
    unsigned before = allocations;
    assert(gpr_aligned_alloc(&allocator, SIZE_MAX) == NULL);
    assert(allocations == before);
    gpr_aligned_free(&allocator, NULL);
    return 0;
}
