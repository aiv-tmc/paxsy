#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

#include <stdint.h>

struct PreprocessorState;

/* Preprocess source code by removing comments and preprocessing directives.
 * 
 * @param input    Source code to preprocess
 * @param filename Name of the file being processed (for error reporting)
 * @param error    Pointer to store error code (0 = success, non-zero = error)
 * @return         Preprocessed code (must be freed by caller) or NULL on error
 */
char* preprocess(const char* input, const char* filename, int* error);

/* Preprocess a buffer of source code using an existing preprocessor state.
 * The output is appended to the state's output buffer.
 * 
 * @param state    Preprocessor state (must be initialized)
 * @param input    Source code to preprocess
 * @param filename Name of the file being processed (for error reporting)
 * @return         0 on success, non-zero on error
 */
int preprocess_content(struct PreprocessorState* state, const char* input, const char* filename);

#endif
