#ifndef U_ARENA_H
#define U_ARENA_H

#include <stddef.h>

/*
 * A simple bump allocator for fast temporary allocations.
 * All memory is freed when the arena is destroyed or reset.
 */
typedef struct Arena {
    unsigned char *buffer;
    size_t capacity;
    size_t used;
} Arena;

/*
 * Create a new arena with the given initial capacity.
 * Returns NULL on allocation failure.
 */
Arena *arena_create(size_t initial_capacity);

/*
 * Allocate a block of memory from the arena.
 * Returns NULL if out of memory (or arena is NULL).
 */
void *arena_alloc(Arena *arena, size_t size);

/*
 * Reset the arena to empty, discarding all allocations.
 */
void arena_reset(Arena *arena);

/*
 * Destroy the arena and free all associated memory.
 */
void arena_destroy(Arena *arena);

#endif
