#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

/* Preprocess source code by removing comments (both single-line and multi-line).
 * Line continuations (backslash followed by newline) are handled and removed.
 * String and character literals are respected, so comments inside them are not removed.
 *
 * @param input    Source code to preprocess (null-terminated)
 * @param filename Name of the file being processed (for error reporting, may be NULL)
 * @param error    Pointer to store error code (0 = success, non-zero = error), may be NULL
 * @return         Newly allocated string with comments removed, or NULL on error
 */
char* preprocess(const char* input, const char* filename, int* error);

#endif
