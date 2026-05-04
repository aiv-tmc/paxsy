#ifndef DIR__INCLUDE_H
#define DIR__INCLUDE_H

#include "../../preprocessor_state.h"

/* Process the #import directive.
 * Imports a header file (.hp) using a path relative to the current file.
 * Prevents multiple inclusions via an internal registry. */
void DPPF__import(PreprocessorState* state, char* args);

/* Process the #using directive.
 * Imports a library header file from standard system locations. */
void DPPF__using(PreprocessorState* state, char* args);

/* Free the internal inclusion registry. Should be called after preprocessing completes. */
void free_included_registry(void);

#endif
