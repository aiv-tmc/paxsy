#ifndef PAXSY_COMMON_H
#define PAXSY_COMMON_H

/*
 * common.h - shared system includes and project-wide constants.
 * Every translation unit includes this header first so that the
 * feature set (C99) and the standard library surface stay uniform.
 */

#include <stdio.h>   /* fprintf, snprintf, vfprintf, FILE, stderr */
#include <stdlib.h>  /* malloc, realloc, free, exit */
#include <string.h>  /* memcpy, memcmp, strlen, strcmp, strcat */
#include <stdarg.h>  /* va_list, va_start, va_end */
#include <ctype.h>   /* isalpha, isdigit, isalnum, isxdigit */

/* Identity strings embedded into the startup banner and --version. */
#define PAXSY_VERSION "1.0.0"
#define PAXSY_STATE   "stable"
#define PAXSY_DATE    "30.10.2026"

/* Upper bound for file-system paths assembled by the project commands. */
#define PAXSY_PATH_MAX 512

#endif /* PAXSY_COMMON_H */
