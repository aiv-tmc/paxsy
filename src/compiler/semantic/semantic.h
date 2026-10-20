#ifndef SEMANTIC_H
#define SEMANTIC_H

#include <stdbool.h>
#include <stdint.h>

#include "../parser/parser.h"
#include "symbol.h"
#include "type.h"

/*
 * Function pointer type for resolving module names to scopes.
 * Used by 'use' declarations to import symbols from other modules.
 */
typedef struct Scope* (*ModuleResolver)(const char* module_name);

/*
 * Label stack entry.
 */
typedef struct LabelEntry {
    char* name;
    bool is_loop;
    struct LabelEntry* next;
} LabelEntry;

/*
 * Semantic analysis context.
 * Holds all state during the analysis of a translation unit.
 */
typedef struct SemanticContext {
    Scope* current_scope;
    Scope* global_scope;
    Scope* file_scope;
    bool in_function;
    bool in_loop;
    bool in_match;
    Type* return_type;
    bool return_has_error;
    const char* current_func_name;
    ModuleResolver resolver;
    bool extra_warnings;

    bool has_return;
    LabelEntry* label_stack;
} SemanticContext;

/*
 * Create a new semantic analysis context.
 * Returns a pointer to the allocated context, or NULL on failure.
 */
SemanticContext* semantic__create_context(void);

/*
 * Destroy a semantic analysis context and free all associated resources.
 */
void semantic__destroy_context(SemanticContext* ctx);

/*
 * Enable or disable extra warnings for the context.
 */
void semantic__set_extra_warnings(SemanticContext* ctx, bool enable);

/*
 * Perform semantic analysis on the given AST.
 * Returns 0 on success, non‑zero on error.
 */
int semantic__analyze(Program* ast, ModuleResolver resolver);

#endif
