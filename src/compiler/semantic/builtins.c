#include "builtins.h"
#include "symbol.h"
#include "type.h"
#include "semantic.h"
#include "../../interfaces/utils/common.h"
#include "../../interfaces/errhandler/errhandler.h"
#include <string.h>
#include <stdlib.h>

/*
 * Comptime evaluator for `std::type::sizeof`.
 * Expects one argument: a type AST node.
 * Returns the size of that type in bytes as an Int64.
 */
static int64_t builtin_sizeof_eval(SemanticContext* ctx, ASTNode** args, size_t arg_count) {
    if (arg_count != 1) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, 0, 0, 1,
                                    "builtins", "sizeof expects exactly 1 argument");
        return 0;
    }
    ASTNode* type_node = args[0];
    Type* t = resolve_type_from_ast(ctx, type_node);
    if (!t) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, type_node->line, type_node->column, 1,
                                    "builtins", "sizeof argument must be a type");
        return 0;
    }
    int64_t size = type_sizeof(t);
    type_free(t);
    if (size < 0) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, type_node->line, type_node->column, 1,
                                    "builtins", "Cannot determine size of incomplete type");
        return 0;
    }
    return size;
}

/* Built-in scope */
static Scope* builtin_scope = NULL;

/*
 * Initialize built-ins module std::type.
 */
static void init_builtins(void) {
    if (builtin_scope) return;

    builtin_scope = scope_create(NULL, SCOPE_MODULE);
    if (!builtin_scope) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, 0, 0, 1,
                                    "builtins", "Failed to create builtins scope");
        return;
    }

    Type* func_type = type_function(NULL, 0, type_basic(TYPE_BASIC_INT64), false);
    Symbol* sizeof_sym = symbol_create("sizeof", SYMBOL_FUNCTION, func_type, 0, 0);
    if (!sizeof_sym) {
        type_free(func_type);
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, 0, 0, 1,
                                    "builtins", "Failed to create sizeof symbol");
        return;
    }
    sizeof_sym->is_public = true;
    sizeof_sym->is_comptime_builtin = true;
    sizeof_sym->comptime_eval = builtin_sizeof_eval;
    sizeof_sym->extra.func.return_type = type_basic(TYPE_BASIC_INT64);
    sizeof_sym->extra.func.return_is_error = false;
    sizeof_sym->extra.func.params = NULL;
    sizeof_sym->extra.func.param_count = 0;

    if (!scope_insert(builtin_scope, sizeof_sym)) {
        symbol_free(sizeof_sym);
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, 0, 0, 1,
                                    "builtins", "Failed to insert sizeof into builtins scope");
    }
}

/*
 * Get the built-in scope.
 */
struct Scope* builtins_get_scope(void) {
    init_builtins();
    return builtin_scope;
}

/*
 * Get a built-in symbol by name.
 */
Symbol* builtins_get_symbol(const char* name) {
    init_builtins();
    if (!builtin_scope) return NULL;
    return scope_lookup_local(builtin_scope, name);
}
