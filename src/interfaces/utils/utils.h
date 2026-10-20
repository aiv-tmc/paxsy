#ifndef UPARSER_H
#define UPARSER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/*
 * General‑purpose macros.
 */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define is_null(ptr) ((ptr) == NULL)

/*
 * Character classification functions (locale‑independent).
 * All functions treat char as unsigned to avoid sign‑extension issues.
 */
static inline int u__char_is_whitespace(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static inline int u__char_is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static inline int u__char_is_identifier_start(char c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_');
}

static inline int u__char_is_identifier_char(char c) {
    return (u__char_is_identifier_start(c) || u__char_is_digit(c));
}

/*
 * Memory allocation wrappers.
 */
void* memory_allocate_zero(size_t size);
void* memory_reallocate_zero(void* ptr, size_t old_size, size_t new_size);
void memory_free_safe(void** ptr);
/* memory_set_safe is declared in memory.h; do not duplicate it here */

/*
 * Duplicate a string of given length (allocates new memory).
 */
char* u__strdup(const char* str, size_t len);

#endif
