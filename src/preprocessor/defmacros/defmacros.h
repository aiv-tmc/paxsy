#ifndef DEFMACROS_H
#define DEFMACROS_H

#include "../directive/define/macro.h"

/* Target platform globals - must be set by the driver before calling
 * builtin_macros_init() to reflect the compilation target. */
extern const char* builtin_target_os;
extern const char* builtin_target_arch;
extern const char* builtin_target_bits;

/* Initialize the macro table with all built-in macros.
 * Parameters:
 *   table    - pointer to the macro table to populate
 *   filename - current source file name (for __file__ macro) */
void builtin_macros_init(MacroTable* table, const char* filename);

#endif
