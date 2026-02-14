#include "memory_utils.h"
#include <stdlib.h>
#include <string.h>

void* memory_allocate_zero(size_t size) {
    if (size == 0) return NULL;
    
    void* ptr = malloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

void* memory_reallocate_zero
    ( void* ptr
    , size_t old_size
    , size_t new_size
) {
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    
    void* new_ptr = realloc(ptr, new_size);
    if (new_ptr && new_size > old_size)
        memset((char*)new_ptr + old_size, 0, new_size - old_size);
    return new_ptr;
}

void* memory_duplicate(const void* src, size_t size) {
    if (!src || size == 0) return NULL;
    
    void* dest = malloc(size);
    if (dest) memcpy(dest, src, size);
    return dest;
}

void memory_free_safe(void** ptr) {
    if (ptr && *ptr) {
        free(*ptr);
        *ptr = NULL;
    }
}

int memory_is_aligned(const void* ptr, size_t alignment) {
    return ((uintptr_t)ptr & (alignment - 1)) == 0;
}

void* memory_align_up(void* ptr, size_t alignment) {
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t mask = alignment - 1;
    
    if ((p & mask) == 0) return ptr;
    
    return (void*)((p + mask) & ~mask);
}

size_t memory_alignment_padding
    ( const void* ptr
    , size_t alignment
) {
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t mask = alignment - 1;
    
    if ((p & mask) == 0) return 0;
    
    return alignment - (p & mask);
}

void* memory_allocate_aligned(size_t size, size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        /* alignment must be power of 2 */
        return NULL;
    }
    
    /* Allocate extra space for alignment and original pointer */
    size_t total_size = size + alignment + sizeof(void*);
    void* original_ptr = malloc(total_size);
    
    if (!original_ptr) return NULL;
    
    /* Align the pointer after storing the original pointer */
    void* aligned_ptr =
        (void*)
        (
            (
                (uintptr_t)original_ptr
                + sizeof(void*)
                + alignment - 1
            )
            & ~(alignment - 1)
        );
    
    /* Store original pointer just before aligned memory */
    ((void**)aligned_ptr)[-1] = original_ptr;
    
    return aligned_ptr;
}

void memory_free_aligned(void* ptr) {
    if (ptr) {
        void* original_ptr = ((void**)ptr)[-1];
        free(original_ptr);
    }
}

void* memory_copy_safe
    ( void* dest
    , const void* src
    , size_t size
) {
    if (!dest || !src || size == 0) return dest;
    
    /* Check for overlap */
    if ((src < dest && (char*)src + size > (char*)dest)
    || (dest < src && (char*)dest + size > (char*)src)
    ) return memmove(dest, src, size);
    
    return memcpy(dest, src, size);
}

void* memory_set_safe(void* dest, int value, size_t count) {
    if (!dest || count == 0) return dest;
    
    return memset(dest, value, count);
}
