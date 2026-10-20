#include "arena.h"
#include <stdlib.h>

Arena *arena_create(size_t initial_capacity) {
    Arena *a = (Arena*)malloc(sizeof(Arena));
    if (!a) return NULL;
    a->buffer = (unsigned char*)malloc(initial_capacity);
    if (!a->buffer) {
        free(a);
        return NULL;
    }
    a->capacity = initial_capacity;
    a->used = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t size) {
    if (!a) return NULL;
    if (a->used + size > a->capacity) {
        size_t new_cap = a->capacity * 2;
        if (new_cap < a->used + size) new_cap = a->used + size;
        unsigned char *new_buf = (unsigned char*)realloc(a->buffer, new_cap);
        if (!new_buf) return NULL;
        a->buffer = new_buf;
        a->capacity = new_cap;
    }
    void *ptr = a->buffer + a->used;
    a->used += size;
    return ptr;
}

void arena_reset(Arena *a) {
    if (a) a->used = 0;
}

void arena_destroy(Arena *a) {
    if (a) {
        free(a->buffer);
        free(a);
    }
}
