#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

#include <stdint.h>

/**
 * Preprocess source code by removing comments and preprocessing directives
 * while preserving configuration macros and code structure.
 * 
 * @param input: Source code to preprocess
 * @param filename: Name of the file being processed (for error reporting)
 * @param error: Pointer to store error code (0 = success, non-zero = error)
 * @return: Preprocessed code (must be freed by caller) or NULL on error
 */
char* preprocess(const char* input, const char* filename, int* error);

#endif
