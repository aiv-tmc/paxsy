#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "../parser/parser.h"
#include "../semantic/semantic.h"
#include <stdio.h>
#include <stdbool.h>

/*
 * Run all optimisation passes on the AST.  global_scope is the symbol table
 * populated by the semantic analysis phase.
 *
 * On success returns true.  On failure false is returned and diagnostics
 * are emitted through the error handler.
 *
 * The optimiser is architecture‑agnostic and works correctly on ARM
 * (Aarch64) targets.
 */
bool optimizer__optimize(AST *ast, SymbolTable *global_scope);

/*
 * Enable or disable debug output from the optimiser.
 * When enabled, the optimiser prints information about AST nodes
 * before and after each pass, using the configured debug file.
 */
void optimizer__enable_debug(bool enable);

/*
 * Set the FILE stream to which debug output is written.
 * If out is NULL, debug output is disabled. Default is stdout.
 */
void optimizer__set_debug_file(FILE *out);

#endif
