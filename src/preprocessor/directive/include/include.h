#ifndef DIR__INCLUDE_H
#define DIR__INCLUDE_H

/* Forward declaration of the preprocessor state structure. */
#include "../../preprocessor_state.h"

/*
 * Process the #import directive.
 * Imports a header file (.hp) using a path relative to the current file.
 * Prevents multiple inclusions via an internal registry.
 */
void DPPF__import(PreprocessorState* state, char* args);

/*
 * Process the #using directive.
 * Imports a library header file (.hp) from standard system locations.
 * Searches in: same directory as current file, current working directory,
 * ./lib/, /usr/lib/paxsy/incl/, /usr/local/lib/paxsy/incl/.
 */
void DPPF__using(PreprocessorState* state, char* args);

#endif
