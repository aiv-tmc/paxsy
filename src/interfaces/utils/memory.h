#ifndef U__MEMORY_UTILS_H
#define U__MEMORY_UTILS_H

#include <stddef.h>
#include <stdint.h>

/**
 * Allocate zero-initialized memory
 * @param size: Number of bytes to allocate
 * @return: Pointer to allocated memory, NULL on failure
 * @note: Memory is initialized to zero
 */
void* memory_allocate_zero(size_t size);

/**
 * Reallocate memory with zero initialization of new bytes
 * @param ptr: Pointer to existing memory block
 * @param old_size: Current size of memory block
 * @param new_size: New size of memory block
 * @return: Pointer to reallocated memory, NULL on failure
 */
void* memory_reallocate_zero(void* ptr, size_t old_size, size_t new_size);

/**
 * Duplicate memory block
 * @param src: Source memory block
 * @param size: Number of bytes to duplicate
 * @return: Newly allocated copy of memory, NULL on failure
 */
void* memory_duplicate(const void* src, size_t size);

/**
 * Safely free memory and set pointer to NULL
 * @param ptr: Pointer to memory to free
 */
void memory_free_safe(void** ptr);

/**
 * Check if pointer is aligned to specified boundary
 * @param ptr: Pointer to check
 * @param alignment: Alignment boundary (must be power of 2)
 * @return: 1 if pointer is aligned, 0 otherwise
 */
int memory_is_aligned(const void* ptr, size_t alignment);

/**
 * Align pointer up to specified boundary
 * @param ptr: Pointer to align
 * @param alignment: Alignment boundary (must be power of 2)
 * @return: Aligned pointer
 */
void* memory_align_up(void* ptr, size_t alignment);

/**
 * Calculate padding needed to align pointer
 * @param ptr: Pointer to check
 * @param alignment: Alignment boundary (must be power of 2)
 * @return: Number of bytes needed for alignment
 */
size_t memory_alignment_padding(const void* ptr, size_t alignment);

/**
 * Allocate aligned memory
 * @param size: Number of bytes to allocate
 * @param alignment: Alignment boundary (must be power of 2)
 * @return: Aligned memory pointer, NULL on failure
 */
void* memory_allocate_aligned(size_t size, size_t alignment);

/**
 * Free aligned memory allocated with memory_allocate_aligned
 * @param ptr: Pointer to aligned memory
 */
void memory_free_aligned(void* ptr);

/**
 * Copy memory with overlap protection
 * @param dest: Destination buffer
 * @param src: Source buffer
 * @param size: Number of bytes to copy
 * @return: Destination buffer
 */
void* memory_copy_safe(void* dest, const void* src, size_t size);

/**
 * Set memory to specific value
 * @param dest: Destination buffer
 * @param value: Value to set
 * @param count: Number of bytes to set
 * @return: Destination buffer
 */
void* memory_set_safe(void* dest, int value, size_t count);

#endif
