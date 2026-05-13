#ifndef DIR__DEFINE_H
#define DIR__DEFINE_H

#include "../../preprocessor_state.h"

/*
 * Maximum allowed number of parameters for a function‑like macro.
 * 256 is chosen to be large enough for any realistic use case while
 * still allowing fixed‑size arrays on the stack during parsing.
 */
#define MAX_MACRO_PARAMS 256

/*
 * Maximum length (excluding null terminator) of a macro name.
 * Must be large enough to hold any valid C identifier.
 */
#define MAX_MACRO_NAME_LEN 512

/*
 * Process a #define directive.
 *
 * state  – current preprocessor state (holds the macro table).
 * args   – pointer into the directive line buffer immediately after
 *          the "define" keyword (whitespace already trimmed).
 */
void DPPF__define(PreprocessorState* state, char* args);

/*
 * Process an #undef directive.
 *
 * state  – current preprocessor state.
 * args   – pointer into the directive line buffer immediately after
 *          the "undef" keyword (whitespace already trimmed).
 */
void DPPF__undef(PreprocessorState* state, char* args);

#endif
