#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "../parser/parser.h"
#include "../semantic/semantic.h"

/*
 * Run all optimization passes on the AST.
 *
 * global_scope is the symbol table populated by the semantic analysis phase.
 * The function returns true on success.
 */
bool optimizer__optimize(AST *ast, SymbolTable *global_scope);

#endif
