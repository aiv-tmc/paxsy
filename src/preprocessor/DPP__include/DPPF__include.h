#ifndef DPPF__INCLUDE_H
#define DPPF__INCLUDE_H

#include "../preprocessor_state.h"

/**
 * Process #incfile directive - include file content
 * @param state: Preprocessor state
 * @param args: Directive arguments
 */
void DPPF__incfile(PreprocessorState* state, char* args);

/**
 * Process #inclib directive - include library file
 * @param state: Preprocessor state
 * @param args: Directive arguments
 */
void DPPF__inclib(PreprocessorState* state, char* args);

#endif
