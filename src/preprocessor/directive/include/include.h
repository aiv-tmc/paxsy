#ifndef DIR__INCLUDE_H
#define DIR__INCLUDE_H

#include "../../preprocessor_state.h"

/**
 * @brief Process a #import directive – include a .hp file from a relative path.
 * @param state Preprocessor state (contains current file, output buffer, etc.)
 * @param args  Directive arguments, e.g. "subfolder/module" (quotes are part of the argument).
 *
 * The file name is automatically given a .hp extension if not already present.
 * Inclusion is guarded against duplicates and recursive cycles.
 */
void DPPF__import(PreprocessorState* state, char* args);

/**
 * @brief Process a #using directive – include a library .hp file from standard locations.
 * @param state Preprocessor state.
 * @param args  Directive arguments, e.g. "mylib" (quotes are part of the argument).
 *
 * The library name is automatically extended with .hp. The file is searched in:
 * - the same directory as the current file,
 * - the current working directory,
 * - the ./lib/ subdirectory,
 * - a platform‑specific installation path.
 * Inclusion is guarded against duplicates and recursive cycles.
 */
void DPPF__using(PreprocessorState* state, char* args);

#endif
