#ifndef BUILTINS_H
#define BUILTINS_H

#include "symbol.h"
#include "semantic.h"

/*
 * Get the scope for the built-in module std::type.
 * Creates it lazily.
 */
struct Scope* builtins_get_scope(void);

/*
 * Get a builtin symbol by name (e.g., "sizeof").
 * Returns NULL if not found.
 */
Symbol* builtins_get_symbol(const char* name);

#endif
