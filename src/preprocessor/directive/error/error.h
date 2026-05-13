#ifndef DIR__ERROR_H
#define DIR__ERROR_H

#include "../../preprocessor_state.h"

/* Process the #error directive.
 * args: The argument string following #error (may be quoted or unquoted).
 * state: The preprocessor state, used for error reporting location.
 * This function reports a preprocessing error with the given message. */
void DPPF__error(PreprocessorState *state, char *args);

#endif
