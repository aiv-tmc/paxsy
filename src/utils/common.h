#ifndef U__COMMON_H
#define U__COMMON_H

#include <stddef.h>      /* size_t */
#include <stdint.h>      /* fixed-width integers */
#include <stdbool.h>     /* bool, true, false */
#include <stdlib.h>      /* malloc, free, atexit, abort */
#include <string.h>      /* memcpy, memset, memmove, strlen, strcmp */
#include <stdio.h>       /* fprintf (used by assert macros) */

/* Project utility headers */
#include "str_utils.h"
#include "char_utils.h"
#include "memory_utils.h"

/* Common macros */
#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(x,lo,hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifdef __STDC_VERSION__
    #if __STDC_VERSION__ >= 201112L
        #define STATIC_ASSERT(expr) _Static_assert(expr, #expr)
    #else
        #define STATIC_ASSERT(expr) \
            typedef char U_STATIC_ASSERT_##__LINE__[(expr) ? 1 : -1]
    #endif
#else
    #define STATIC_ASSERT(expr)
#endif

/* Common result codes */
typedef enum {
    RESULT_OK = 0,
    RESULT_ERROR = 1,
    RESULT_OUT_OF_MEMORY = 2,
    RESULT_IO_ERROR = 3,
    RESULT_INVALID_ARGUMENT = 4,
    RESULT_NOT_FOUND = 5,
    RESULT_ALREADY_EXISTS = 6
} ResultCode;

/* Platform detection */
#ifdef _WIN32
    #define PLATFORM_WINDOWS 1
    #define PLATFORM_POSIX   0
#elif defined(__APPLE__)
    #define PLATFORM_WINDOWS 0
    #define PLATFORM_POSIX   1
    #define PLATFORM_MACOS   1
#elif defined(__linux__)
    #define PLATFORM_WINDOWS 0
    #define PLATFORM_POSIX   1
    #define PLATFORM_LINUX   1
#else
    #define PLATFORM_WINDOWS 0
    #define PLATFORM_POSIX   0
#endif

/* Compiler hints (GCC / Clang) */
#ifdef __GNUC__
    #define LIKELY(x)       __builtin_expect(!!(x), 1)
    #define UNLIKELY(x)     __builtin_expect(!!(x), 0)
    #define NO_RETURN       __attribute__((noreturn))
    #define NO_INLINE       __attribute__((noinline))
    #define ALWAYS_INLINE   __attribute__((always_inline))
    #define ALIGN_AS(type)  __attribute__((aligned(sizeof(type))))
    #define CACHE_ALIGNED   __attribute__((aligned(64)))
#else
    #define LIKELY(x)       (x)
    #define UNLIKELY(x)     (x)
    #define NO_RETURN
    #define NO_INLINE
    #define ALWAYS_INLINE
    #define ALIGN_AS(type)
    #define CACHE_ALIGNED
#endif

/* Debug assertions */
#ifdef NDEBUG
    #define ASSERT(expr)        ((void)0)
    #define ASSERT_MSG(expr, msg) ((void)0)
#else
    #define ASSERT(expr)        assert(expr)
    #define ASSERT_MSG(expr, msg) \
        do { \
            if (!(expr)) { \
                fprintf(stderr, "Assertion failed: %s (%s:%d)\n", \
                        (msg), __FILE__, __LINE__); \
                abort(); \
            } \
        } while (0)
#endif

/* Inline helpers */
static inline int is_null(const void* ptr) {
    return ptr == NULL;
}

static inline int is_empty(const char* str) {
    return !str || str[0] == '\0';
}

#endif
