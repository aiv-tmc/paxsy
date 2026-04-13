#ifndef DIR__DEFINE_H
#define DIR__DEFINE_H

#include "../../preprocessor_state.h"

/*
 * Constants
 */
#define MAX_MACRO_PARAMS 256
#define MAX_MACRO_NAME_LEN 512

/*
 * Function declarations for #define and #undef directives.
 */
void DPPF__undef(PreprocessorState* state, char* args);
void DPPF__define(PreprocessorState* state, char* args);

#endif
