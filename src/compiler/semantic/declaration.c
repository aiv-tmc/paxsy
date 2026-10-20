#define _POSIX_C_SOURCE 200809L
#include "declaration.h"
#include "semantic.h"
#include "statement.h"
#include "expression.h"
#include "comptime.h"
#include "builtins.h"
#include "type_utils.h"
#include "../../interfaces/errhandler/errhandler.h"
#include "../../interfaces/utils/common.h"
#include <string.h>
#include <stdlib.h>

/*
 * Forward declaration for control flow analysis.
 */
static bool check_all_paths_return(SemanticContext* ctx, ASTNode* node, bool is_void, bool can_error);

/*
 * Infer concrete type from a family type and initializer.
 */
static Type* infer_concrete_type(SemanticContext* ctx, Type* family, ASTNode* init) {
    if (!family || family->kind != TYPE_KIND_FAMILY) return type_copy(family);

    switch (family->u.family) {
        case TYPE_FAMILY_INT: {
            int64_t val;
            if (eval_const_expr(ctx, init, &val)) {
                if (val >= -128 && val <= 127) return type_basic(TYPE_BASIC_INT8);
                if (val >= -32768 && val <= 32767) return type_basic(TYPE_BASIC_INT16);
                if (val >= -2147483648LL && val <= 2147483647LL) return type_basic(TYPE_BASIC_INT32);
                return type_basic(TYPE_BASIC_INT64);
            }
            return type_basic(TYPE_BASIC_INT32);
        }
        case TYPE_FAMILY_UINT: {
            int64_t val;
            if (eval_const_expr(ctx, init, &val)) {
                if (val >= 0 && val <= 255) return type_basic(TYPE_BASIC_UINT8);
                if (val <= 65535) return type_basic(TYPE_BASIC_UINT16);
                if (val <= 4294967295LL) return type_basic(TYPE_BASIC_UINT32);
                return type_basic(TYPE_BASIC_UINT64);
            }
            return type_basic(TYPE_BASIC_UINT32);
        }
        case TYPE_FAMILY_REAL: {
            if (init->type == NODE_LITERAL) {
                LiteralExpr* lit = (LiteralExpr*)init;
                if (lit->kind == LIT_NUMBER) {
                    char* end;
                    strtod(lit->raw_value, &end);
                    if (end && *end == 'f')
                        return type_basic(TYPE_BASIC_REAL32);
                }
            }
            return type_basic(TYPE_BASIC_REAL64);
        }
        case TYPE_FAMILY_CHAR:
            return type_basic(TYPE_BASIC_UTF8);
        default:
            return type_copy(family);
    }
}

/*
 * Data for importing all symbols from a module.
 */
typedef struct ImportAllData {
    SemanticContext* ctx;
    const char* module_name;
    bool ok;
} ImportAllData;

/*
 * Callback for importing a symbol.
 */
static void import_all_callback(Symbol* sym, void* userdata) {
    ImportAllData* d = (ImportAllData*)userdata;
    if (!d->ok) return;
    if (!sym->is_public) return;
    if (scope_lookup_local(d->ctx->current_scope, sym->name)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, sym->line, sym->column, 1,
                                    "semantic", "Symbol '%s' already exists in current scope (imported from %s)",
                                    sym->name, d->module_name);
        d->ok = false;
        return;
    }
    Symbol* copy = symbol_copy_deep(sym);
    if (!copy) {
        d->ok = false;
        return;
    }
    copy->is_public = false;
    if (!scope_insert(d->ctx->current_scope, copy)) {
        symbol_free(copy);
        d->ok = false;
    }
}

/*
 * Import one public symbol from a module scope into the current
 * scope. Returns false after reporting an error.
 */
static bool import_one_symbol(SemanticContext* ctx, Scope* mod_scope,
                              const char* sym_name, const char* full_path,
                              uint32_t line, uint32_t column) {
    Symbol* orig = scope_lookup_local(mod_scope, sym_name);
    if (!orig) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, line, column, 1,
                                    "semantic", "Symbol '%s' not found in module '%s'",
                                    sym_name, full_path);
        return false;
    }
    if (!orig->is_public) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, line, column, 1,
                                    "semantic", "Symbol '%s' is not public in module '%s'",
                                    sym_name, full_path);
        return false;
    }
    if (scope_lookup_local(ctx->current_scope, sym_name)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, line, column, 1,
                                    "semantic", "Symbol '%s' already exists in current scope (imported from %s)",
                                    sym_name, full_path);
        return false;
    }
    Symbol* copy = symbol_copy_deep(orig);
    if (!copy) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, line, column, 1,
                                    "semantic", "Memory allocation failed");
        return false;
    }
    copy->is_public = false;
    if (!scope_insert(ctx->current_scope, copy)) {
        symbol_free(copy);
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, line, column, 1,
                                    "semantic", "Failed to insert imported symbol '%s'", sym_name);
        return false;
    }
    return true;
}

/*
 * Resolve a module path to its scope: the std::type builtins module,
 * or the project resolver for everything else.
 */
static Scope* resolve_module_scope(SemanticContext* ctx, const char* full_path) {
    if (strcmp(full_path, "std::type") == 0) {
        return builtins_get_scope();
    }
    if (!ctx->resolver) return NULL;
    return ctx->resolver(full_path);
}

/*
 * Analyze a use declaration.
 * 'use module;' imports every public symbol of the module.
 * 'use module::symbol;' imports a single symbol: when the full path
 * does not resolve to a module, the last path component is treated
 * as a symbol of the parent module.
 */
bool analyze_use_declaration(SemanticContext* ctx, UseDecl* use) {
    if (!use) return false;
    char full_path[256] = {0};
    for (size_t i = 0; i < use->path_len; ++i) {
        if (i > 0) strcat(full_path, "::");
        strcat(full_path, use->path[i]);
    }

    Scope* mod_scope = resolve_module_scope(ctx, full_path);
    if (mod_scope) {
        ImportAllData data = { ctx, full_path, true };
        scope_foreach(mod_scope, import_all_callback, &data);
        return data.ok;
    }

    if (use->path_len >= 2) {
        char parent_path[256] = {0};
        for (size_t i = 0; i + 1 < use->path_len; ++i) {
            if (i > 0) strcat(parent_path, "::");
            strcat(parent_path, use->path[i]);
        }
        const char* sym_name = use->path[use->path_len - 1];
        Scope* parent_scope = resolve_module_scope(ctx, parent_path);
        if (parent_scope) {
            return import_one_symbol(ctx, parent_scope, sym_name, parent_path,
                                     use->base.line, use->base.column);
        }
    }

    errhandler__report_error_ex(ERROR_LEVEL_ERROR, use->base.line, use->base.column, 1,
                                "semantic", "Module '%s' not found", full_path);
    return false;
}

/*
 * Analyze a module declaration.
 */
bool analyze_module_declaration(SemanticContext* ctx, ModuleDecl* mod) {
    if (!mod) return false;
    Scope* module_scope = scope_create(ctx->current_scope, SCOPE_MODULE);
    if (!module_scope) return false;
    Symbol* sym = symbol_create(mod->name, SYMBOL_MODULE, NULL, mod->base.line, mod->base.column);
    if (!sym) { scope_destroy(module_scope); return false; }
    sym->extra.module_scope = module_scope;
    sym->is_public = true;
    if (!scope_insert(ctx->current_scope, sym)) {
        free(sym->name); free(sym);
        scope_destroy(module_scope);
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, mod->base.line, mod->base.column, 1,
                                    "semantic", "Module '%s' already defined", mod->name);
        return false;
    }
    Scope* old_scope = ctx->current_scope;
    ctx->current_scope = module_scope;
    bool ok = true;
    for (size_t i = 0; i < mod->body_count; ++i) {
        ASTNode* item = mod->body[i];
        switch (item->type) {
            case NODE_USE:
                if (!analyze_use_declaration(ctx, (UseDecl*)item)) ok = false;
                break;
            case NODE_FUNC_DECL:
                if (!analyze_func_declaration(ctx, (FuncDecl*)item)) ok = false;
                break;
            case NODE_LET_DECL:
                if (!analyze_let_declaration(ctx, (LetDecl*)item)) ok = false;
                break;
            case NODE_COMPTIME_DECL:
                if (!analyze_comptime_declaration(ctx, (ComptimeDecl*)item)) ok = false;
                break;
            case NODE_EXTERN_DECL:
                if (!analyze_extern_declaration(ctx, (ExternDecl*)item)) ok = false;
                break;
            default:
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, item->line, item->column, 1,
                                            "semantic", "Unexpected item inside module");
                ok = false;
                break;
        }
    }
    ctx->current_scope = old_scope;
    return ok;
}

/*
 * Analyze a function declaration.
 */
/*
 * Build the TYPE_KIND_FUNCTION type for a function symbol from its parameter
 * list. The function type takes ownership of ret_type and of deep copies of
 * the parameter types.
 */
static Type* build_function_type(ParamInfo* params, size_t param_count, Type* ret_type) {
    Type** param_types = NULL;
    if (param_count > 0) {
        param_types = (Type**)calloc(param_count, sizeof(Type*));
        if (!param_types) return NULL;
        size_t j = 0;
        for (ParamInfo* p = params; p; p = p->next) {
            param_types[j] = type_copy(p->type);
            if (!param_types[j]) {
                for (size_t k = 0; k < j; ++k) type_free(param_types[k]);
                free(param_types);
                return NULL;
            }
            j++;
        }
    }
    return type_function(param_types, param_count, ret_type,
                         ret_type && ret_type->kind == TYPE_KIND_ERROR);
}

/*
 * Register a method declaration in the method table of each struct
 * named in func->method_of. Methods are reachable only through their
 * struct; no bare-name symbol is created. The full name Struct.method
 * is the method's identity, so duplicate registration on the same
 * struct is an error. Returns false after reporting an error.
 */
static bool register_methods(SemanticContext* ctx, FuncDecl* func) {
    for (size_t i = 0; i < func->method_of_count; ++i) {
        Symbol* struct_sym = scope_lookup(ctx->current_scope, func->method_of[i]);
        if (!struct_sym || struct_sym->kind != SYMBOL_TYPE) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, func->base.line, func->base.column, 1,
                                        "semantic", "Method '%s' declared on unknown type '%s'",
                                        func->name, func->method_of[i]);
            return false;
        }
        for (MethodInfo* mi = struct_sym->extra.methods; mi; mi = mi->next) {
            if (strcmp(mi->name, func->name) == 0) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, func->base.line, func->base.column, 1,
                                            "semantic", "Method '%s' already defined on type '%s'",
                                            func->name, func->method_of[i]);
                return false;
            }
        }
        MethodInfo* mi = (MethodInfo*)calloc(1, sizeof(MethodInfo));
        if (!mi) return false;
        mi->name = u__strduplic(func->name);
        mi->decl = func;
        mi->next = struct_sym->extra.methods;
        struct_sym->extra.methods = mi;
    }
    return true;
}

bool analyze_function_body(SemanticContext* ctx, FuncDecl* func,
                           ParamInfo* param_list, Type* ret_type) {
    Scope* func_scope = scope_create(ctx->current_scope, SCOPE_FUNCTION);
    if (!func_scope) return false;
    ctx->current_scope = func_scope;

    ParamInfo* p = param_list;
    while (p) {
        Symbol* ps = symbol_create(p->name, SYMBOL_PARAMETER, type_copy(p->type),
                                   func->base.line, func->base.column);
        if (ps) {
            ps->is_initialized = true;
            scope_insert(func_scope, ps);
        }
        p = p->next;
    }

    bool prev_in_func = ctx->in_function;
    ctx->in_function = true;
    ctx->return_type = ret_type;
    ctx->return_has_error = (ret_type && ret_type->kind == TYPE_KIND_ERROR);
    ctx->current_func_name = func->name;
    ctx->has_return = false;

    if (func->body) {
        analyze_block(ctx, (Block*)func->body);
    } else if (func->expr_body) {
        /* A brace literal in a tuple-returning shorthand body is
           a tuple, analyzed element-wise. */
        Type* expr_type = (ret_type && ret_type->kind == TYPE_KIND_TUPLE &&
                           func->expr_body->type == NODE_ARRAY_LITERAL)
            ? analyze_brace_as_tuple(ctx, (ArrayLiteral*)func->expr_body)
            : analyze_expression(ctx, func->expr_body);
        if (expr_type) {
            if (!type_compatible(expr_type, ret_type)) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, func->expr_body->line,
                                            func->expr_body->column, 1,
                                            "semantic", "Return type mismatch in expression body for '%s'", func->name);
            }
            type_free(expr_type);
        }
        ctx->has_return = true;
    }

    bool ok = true;
    if (!type_equal(ret_type, type_Void) && !ctx->return_has_error) {
        /* A shorthand body is a single return expression and
           therefore returns on every path by construction. */
        bool all_return = func->expr_body ? true : check_all_paths_return(ctx, (ASTNode*)func->body,
                                                 type_equal(ret_type, type_Void), ctx->return_has_error);
        if (!all_return) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, func->base.line, func->base.column, 1,
                                        "semantic", "Function '%s' does not return a value on all paths", func->name);
            ok = false;
        }
    }

    ctx->in_function = prev_in_func;
    ctx->return_type = NULL;
    ctx->current_func_name = NULL;
    ctx->current_scope = ctx->current_scope->parent;
    return ok;
}

bool analyze_func_declaration(SemanticContext* ctx, FuncDecl* func) {
    if (!func) return false;

    /* Template function with comptime parameters: stored and
       specialized at each call site, not analyzed here. */
    bool is_template = false;
    for (size_t i = 0; i < func->param_count; ++i) {
        Param* prm = (Param*)func->params[i];
        if (prm && prm->is_comptime) {
            is_template = true;
            break;
        }
    }
    if (is_template) {
        bool has_body = (func->body != NULL || func->expr_body != NULL);
        if ((func->modifiers & MOD_EXTERN) != 0 || !has_body) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, func->base.line, func->base.column, 1,
                                        "semantic", "Function '%s' with comptime parameters must have a body",
                                        func->name);
            return false;
        }
        Symbol* sym = symbol_create(func->name, SYMBOL_FUNCTION, NULL,
                                    func->base.line, func->base.column);
        if (!sym) return false;
        sym->extra.func.is_template = true;
        sym->extra.func.comptime_body = (ASTNode*)func;
        sym->is_initialized = true;
        if (!scope_insert(ctx->current_scope, sym)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, func->base.line, func->base.column, 1,
                                        "semantic", "Function '%s' already defined", func->name);
            symbol_free(sym);
            return false;
        }
        return true;
    }

    Type* ret_type = NULL;
    if (func->return_type) {
        ret_type = resolve_type_from_ast(ctx, func->return_type);
        if (!ret_type) return false;
    } else {
        ret_type = type_Void;
    }

    ParamInfo* param_list = NULL;
    ParamInfo* last = NULL;
    size_t param_count = 0;
    for (size_t i = 0; i < func->param_count; ++i) {
        Param* p = (Param*)func->params[i];
        if (!p) continue;
        Type* ptype = resolve_type_from_ast(ctx, p->type);
        if (!ptype) {
            type_free(ret_type);
            return false;
        }
        ParamInfo* pi = (ParamInfo*)calloc(1, sizeof(ParamInfo));
        if (!pi) { type_free(ptype); type_free(ret_type); return false; }
        pi->name = u__strduplic(p->name);
        pi->type = ptype;
        pi->next = NULL;
        if (!param_list) param_list = pi;
        else last->next = pi;
        last = pi;
        param_count++;
    }

    bool has_body = (func->body != NULL || func->expr_body != NULL);
    bool is_extern = (func->modifiers & MOD_EXTERN) != 0;
    if (is_extern && has_body) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, func->base.line, func->base.column, 1,
                                    "semantic", "extern function '%s' cannot have a body", func->name);
        type_free(ret_type);
        while (param_list) { ParamInfo* next = param_list->next; free(param_list->name); type_free(param_list->type); free(param_list); param_list = next; }
        return false;
    }
    /* A bodiless non-extern declaration is a prototype; a definition
       with an exactly matching signature must appear later in the
       same translation unit. comptime functions need a body to be
       evaluated at compile time. */
    if (!is_extern && !has_body && func->is_comptime) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, func->base.line, func->base.column, 1,
                                    "semantic", "comptime function '%s' must have a body", func->name);
        type_free(ret_type);
        while (param_list) { ParamInfo* next = param_list->next; free(param_list->name); type_free(param_list->type); free(param_list); param_list = next; }
        return false;
    }

    bool is_method = (func->method_of_count > 0);
    if (is_method && !register_methods(ctx, func)) {
        type_free(ret_type);
        while (param_list) { ParamInfo* next = param_list->next; free(param_list->name); type_free(param_list->type); free(param_list); param_list = next; }
        return false;
    }

    Type* func_type = is_method ? NULL : build_function_type(param_list, param_count, ret_type);
    if (!is_method && !func_type) {
        type_free(ret_type);
        while (param_list) { ParamInfo* next = param_list->next; free(param_list->name); type_free(param_list->type); free(param_list); param_list = next; }
        return false;
    }
    /* sym->type is the full function type; extra.func.return_type borrows from it */
    Symbol* sym = NULL;
    Symbol* prev = is_method ? NULL : scope_lookup_local(ctx->current_scope, func->name);
    if (prev && prev->kind == SYMBOL_FUNCTION && !prev->extra.func.defined &&
        !prev->is_extern && has_body && !is_extern) {
        /* Definition of a previously declared prototype: the
           signature must match the prototype exactly. */
        if (!type_equal(prev->type, func_type)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, func->base.line, func->base.column, 1,
                                        "semantic", "Definition of '%s' does not match its prototype", func->name);
            type_free(func_type);
            while (param_list) { ParamInfo* next = param_list->next; free(param_list->name); type_free(param_list->type); free(param_list); param_list = next; }
            return false;
        }
        type_free(prev->type);
        while (prev->extra.func.params) {
            ParamInfo* next = prev->extra.func.params->next;
            free(prev->extra.func.params->name);
            type_free(prev->extra.func.params->type);
            free(prev->extra.func.params);
            prev->extra.func.params = next;
        }
        prev->type = func_type;
        prev->extra.func.return_type = ret_type;
        prev->extra.func.params = param_list;
        prev->extra.func.param_count = param_count;
        prev->extra.func.defined = true;
        prev->line = func->base.line;
        prev->column = func->base.column;
        sym = prev;
    } else if (!is_method) {
        sym = symbol_create(func->name, SYMBOL_FUNCTION, func_type, func->base.line, func->base.column);
        if (!sym) {
            type_free(func_type);
            while (param_list) { ParamInfo* next = param_list->next; free(param_list->name); type_free(param_list->type); free(param_list); param_list = next; }
            return false;
        }
        sym->extra.func.return_type = ret_type;
        sym->extra.func.return_is_error = (ret_type && ret_type->kind == TYPE_KIND_ERROR);
        sym->extra.func.params = param_list;
        sym->extra.func.param_count = param_count;
        sym->extra.func.defined = has_body;
        sym->is_extern = is_extern;
        sym->is_initialized = true;
        sym->is_comptime = func->is_comptime;
        if (func->is_comptime) {
            sym->extra.func.comptime_body = func->body ? (ASTNode*)func->body : func->expr_body;
        }

        if (!scope_insert(ctx->current_scope, sym)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, func->base.line, func->base.column, 1,
                                        "semantic", "Function '%s' already defined", func->name);
            symbol_free(sym);
            return false;
        }
    }

    if (has_body && !is_extern) {
        if (!analyze_function_body(ctx, func, param_list, ret_type)) {
            return false;
        }
    }
    if (is_method) {
        /* The method's type artifacts were needed only for analysis;
           the method table keeps the AST declaration. */
        type_free(ret_type);
        while (param_list) { ParamInfo* next = param_list->next; free(param_list->name); type_free(param_list->type); free(param_list); param_list = next; }
    }
    return true;
}

/*
 * Analyze a let declaration.
 * Supports two forms:
 *   1) let ( name : Type, name : Type, ... ) = expr ;  (destructuring)
 *   2) let name [ size ] : Type = expr ;  (single variable with optional array)
 * The array size (if present) applies only to the single-variable form.
 */
bool analyze_let_declaration(SemanticContext* ctx, LetDecl* let) {
    if (!let) return false;
    bool ok = true;

    /* Determine if this is a tuple destructuring declaration */
    bool is_tuple = (let->count > 1);

    for (size_t i = 0; i < let->count; ++i) {
        char* name = let->names[i];
        ASTNode* type_node = let->types[i];
        Type* var_type = NULL;

        if (type_node) {
            var_type = resolve_type_from_ast(ctx, type_node);
            if (!var_type) { ok = false; continue; }
        } else {
            var_type = type_Auto;
        }

        /* Array size is only valid for single-variable declarations */
        bool is_array = false;
        int64_t size_val = -1;
        if (!is_tuple) {
            if (let->array_size) {
                /* Explicit array size: name [ size ] : Type */
                if (!eval_const_expr(ctx, let->array_size, &size_val)) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->array_size->line,
                                                let->array_size->column, 1,
                                                "semantic", "Array size must be a constant integer");
                    type_free(var_type);
                    ok = false;
                    continue;
                }
                if (size_val < 0) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->array_size->line,
                                                let->array_size->column, 1,
                                                "semantic", "Array size must be non-negative");
                    type_free(var_type);
                    ok = false;
                    continue;
                }
                is_array = true;
            } else if (let->initializer && let->initializer->type == NODE_ARRAY_LITERAL &&
                       var_type->kind != TYPE_KIND_TUPLE) {
                /* Infer array size from initializer if no explicit size.
                   A brace literal on a tuple-typed variable is a tuple. */
                ArrayLiteral* arr = (ArrayLiteral*)let->initializer;
                size_val = (int64_t)arr->count;
                is_array = true;
            }
        }

        Type* element_type = var_type;
        Type* array_type = NULL;

        if (is_array) {
            /* For array variables, var_type is actually the element type */
            /* But we stored it as the full type; we need to treat it as element type */
            /* In our current AST, if an array is declared, the type node is the element type.
               However, we stored it as var_type. We'll use var_type as element type. */
            /* If the element type is a family or Auto, infer it from the initializer */
            bool infer_elem = element_type->kind == TYPE_KIND_FAMILY ||
                              (element_type->kind == TYPE_KIND_BASIC &&
                               element_type->u.basic == TYPE_BASIC_AUTO);
            if (infer_elem) {
                Type* inferred_elem = NULL;
                if (let->initializer && let->initializer->type == NODE_ARRAY_LITERAL) {
                    ArrayLiteral* arr = (ArrayLiteral*)let->initializer;
                    Type* arr_type = analyze_array_literal(ctx, arr);
                    if (arr_type) {
                        if (arr_type->kind == TYPE_KIND_ARRAY) {
                            inferred_elem = type_copy(arr_type->u.array.base);
                        }
                        type_free(arr_type);
                    }
                }
                if (inferred_elem) {
                    type_free(element_type);
                    element_type = inferred_elem;
                } else if (element_type->kind == TYPE_KIND_FAMILY) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->base.line, let->base.column, 1,
                                                "semantic", "Cannot infer element type for array '%s'", name);
                    type_free(var_type);
                    ok = false;
                    continue;
                }
            }
            array_type = type_array(element_type, size_val);
            var_type = array_type;
        } else {
            /* Non-array variable: if type is family and initializer exists, infer concrete */
            if (element_type->kind == TYPE_KIND_FAMILY && let->initializer) {
                Type* concrete = infer_concrete_type(ctx, element_type, let->initializer);
                type_free(element_type);
                var_type = concrete;
            }
        }

        Symbol* sym = symbol_create(name, SYMBOL_VARIABLE, var_type, let->base.line, let->base.column);
        if (!sym) { type_free(var_type); ok = false; continue; }
        sym->is_public = false;
        sym->is_initialized = false;

        if (let->initializer) {
            /* A brace literal where a tuple is expected (a tuple-typed
               variable or a destructuring declaration) is analyzed
               element-wise as a tuple, not as an array. */
            bool brace_tuple = let->initializer->type == NODE_ARRAY_LITERAL &&
                               (is_tuple || var_type->kind == TYPE_KIND_TUPLE);
            Type* init_type = brace_tuple
                ? analyze_brace_as_tuple(ctx, (ArrayLiteral*)let->initializer)
                : analyze_expression(ctx, let->initializer);
            if (!init_type) {
                symbol_free(sym);
                ok = false;
                continue;
            }

            if (!is_array) {
                if (!is_tuple) {
                    /* Single variable initialisation */
                    if (var_type->kind == TYPE_KIND_BASIC && var_type->u.basic == TYPE_BASIC_AUTO) {
                        /* Auto: the first assignment fixes the concrete type */
                        var_type = type_copy(init_type);
                        sym->type = var_type;
                    } else if (!type_compatible(init_type, var_type)) {
                        errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->initializer->line,
                                                    let->initializer->column, 1,
                                                    "semantic", "Initializer type %s incompatible with declared type %s",
                                                    type_to_string(init_type), type_to_string(var_type));
                        type_free(init_type);
                        symbol_free(sym);
                        ok = false;
                        continue;
                    }
                    sym->is_initialized = true;
                } else {
                    /* Tuple destructuring: init_type must be a tuple with matching arity */
                    if (init_type->kind != TYPE_KIND_TUPLE) {
                        errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->initializer->line,
                                                    let->initializer->column, 1,
                                                    "semantic", "Initializer must be a tuple for destructuring");
                        type_free(init_type);
                        symbol_free(sym);
                        ok = false;
                        continue;
                    }
                    if (init_type->u.tuple.count != let->count) {
                        errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->initializer->line,
                                                    let->initializer->column, 1,
                                                    "semantic", "Tuple arity mismatch: expected %zu, got %zu",
                                                    let->count, init_type->u.tuple.count);
                        type_free(init_type);
                        symbol_free(sym);
                        ok = false;
                        continue;
                    }
                    Type* elem_type = init_type->u.tuple.elements[i];
                    if (var_type->kind == TYPE_KIND_BASIC && var_type->u.basic == TYPE_BASIC_AUTO) {
                        /* Auto element: take the tuple element's type */
                        var_type = type_copy(elem_type);
                        sym->type = var_type;
                    } else if (!type_compatible(elem_type, var_type)) {
                        errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->initializer->line,
                                                    let->initializer->column, 1,
                                                    "semantic", "Tuple element type mismatch for variable '%s'", name);
                        type_free(init_type);
                        symbol_free(sym);
                        ok = false;
                        continue;
                    }
                    sym->is_initialized = true;
                }
            } else {
                /* Array initialisation */
                if (init_type->kind == TYPE_KIND_ARRAY) {
                    if (!type_compatible(init_type->u.array.base, element_type)) {
                        errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->initializer->line,
                                                    let->initializer->column, 1,
                                                    "semantic", "Array initializer element type mismatch");
                        type_free(init_type);
                        symbol_free(sym);
                        ok = false;
                        continue;
                    }
                } else {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->initializer->line,
                                                let->initializer->column, 1,
                                                "semantic", "Array variable requires array initializer");
                    type_free(init_type);
                    symbol_free(sym);
                    ok = false;
                    continue;
                }
                sym->is_initialized = true;
                type_free(init_type);
                if (!scope_insert(ctx->current_scope, sym)) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->base.line, let->base.column, 1,
                                                "semantic", "Variable '%s' already defined", name);
                    symbol_free(sym);
                    ok = false;
                }
                continue;
            }
            type_free(init_type);
        } else {
            /* No initializer: variable remains uninitialized */
            if (is_array) {
                sym->is_initialized = false; /* array may be partially initialized, but we treat as uninit */
            }
        }

        if (!scope_insert(ctx->current_scope, sym)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, let->base.line, let->base.column, 1,
                                        "semantic", "Variable '%s' already defined", name);
            symbol_free(sym);
            ok = false;
        }
    }
    return ok;
}

/*
 * Analyze a type definition (struct/union/enum) inside a comptime declaration.
 */
static bool analyze_type_def(SemanticContext* ctx, ASTNode* node, Symbol** out_sym) {
    if (!node) return false;
    *out_sym = NULL;
    FieldInfo* fields = NULL;
    FieldInfo* last = NULL;
    SymbolKind kind = SYMBOL_TYPE;
    const char* type_name = "anonymous";

    switch (node->type) {
        case NODE_STRUCT_TYPE: {
            StructType* st = (StructType*)node;
            for (size_t i = 0; i < st->field_count; ++i) {
                Field* f = (Field*)st->fields[i];
                if (!f) continue;
                Type* ftype = resolve_type_from_ast(ctx, f->type);
                if (!ftype) {
                    while (fields) { FieldInfo* next = fields->next; free(fields->name); type_free(fields->type); free(fields); fields = next; }
                    return false;
                }
                FieldInfo* fi = (FieldInfo*)calloc(1, sizeof(FieldInfo));
                if (!fi) { type_free(ftype); while (fields) { FieldInfo* next = fields->next; free(fields->name); type_free(fields->type); free(fields); fields = next; } return false; }
                fi->name = u__strduplic(f->name);
                fi->type = ftype;
                fi->next = NULL;
                if (!fields) fields = fi;
                else last->next = fi;
                last = fi;
            }
            break;
        }
        case NODE_UNION_TYPE: {
            UnionType* ut = (UnionType*)node;
            for (size_t i = 0; i < ut->field_count; ++i) {
                Field* f = (Field*)ut->fields[i];
                if (!f) continue;
                Type* ftype = resolve_type_from_ast(ctx, f->type);
                if (!ftype) {
                    while (fields) { FieldInfo* next = fields->next; free(fields->name); type_free(fields->type); free(fields); fields = next; }
                    return false;
                }
                FieldInfo* fi = (FieldInfo*)calloc(1, sizeof(FieldInfo));
                if (!fi) { type_free(ftype); while (fields) { FieldInfo* next = fields->next; free(fields->name); type_free(fields->type); free(fields); fields = next; } return false; }
                fi->name = u__strduplic(f->name);
                fi->type = ftype;
                fi->next = NULL;
                if (!fields) fields = fi;
                else last->next = fi;
                last = fi;
            }
            break;
        }
        case NODE_ENUM_TYPE: {
            EnumType* et = (EnumType*)node;
            int64_t current_val = 0;
            for (size_t i = 0; i < et->const_count; ++i) {
                EnumConst* ec = (EnumConst*)et->constants[i];
                if (!ec) continue;
                int64_t val = current_val;
                if (ec->value) {
                    if (!eval_const_expr(ctx, ec->value, &val)) {
                        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ec->value->line, ec->value->column, 1,
                                                    "semantic", "Enum constant value must be constant");
                        while (fields) { FieldInfo* next = fields->next; free(fields->name); type_free(fields->type); free(fields); fields = next; }
                        return false;
                    }
                }
                Type* const_type = type_basic(TYPE_BASIC_INT64);
                FieldInfo* fi = (FieldInfo*)calloc(1, sizeof(FieldInfo));
                if (!fi) { type_free(const_type); while (fields) { FieldInfo* next = fields->next; free(fields->name); type_free(fields->type); free(fields); fields = next; } return false; }
                fi->name = u__strduplic(ec->name);
                fi->type = const_type;
                fi->next = NULL;
                if (!fields) fields = fi;
                else last->next = fi;
                last = fi;
                current_val = val + 1;
            }
            break;
        }
        default:
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, node->line, node->column, 1,
                                        "semantic", "Expected struct, union, or enum definition");
            return false;
    }

    Symbol* sym = symbol_create(type_name, kind, NULL, node->line, node->column);
    if (!sym) {
        while (fields) { FieldInfo* next = fields->next; free(fields->name); type_free(fields->type); free(fields); fields = next; }
        return false;
    }
    sym->extra.fields = fields;
    sym->is_public = false;
    sym->is_initialized = true;

    if (node->type == NODE_STRUCT_TYPE) {
        StructType* st = (StructType*)node;
        MethodInfo* methods = NULL;
        MethodInfo* mlast = NULL;
        for (size_t i = 0; i < st->method_count; ++i) {
            FuncDecl* mdecl = (FuncDecl*)st->methods[i];
            if (!mdecl) continue;
            MethodInfo* mi = (MethodInfo*)calloc(1, sizeof(MethodInfo));
            if (!mi) { symbol_free(sym); return false; }
            mi->name = u__strduplic(mdecl->name);
            mi->decl = mdecl;
            mi->next = NULL;
            if (!methods) methods = mi;
            else mlast->next = mi;
            mlast = mi;
        }
        sym->extra.methods = methods;
    }

    *out_sym = sym;
    return true;
}

/*
 * Analyze a comptime declaration.
 */
bool analyze_comptime_declaration(SemanticContext* ctx, ComptimeDecl* comp) {
    if (!comp) return false;
    if (comp->type_def) {
        if (comp->type_def->type != NODE_STRUCT_TYPE &&
            comp->type_def->type != NODE_UNION_TYPE &&
            comp->type_def->type != NODE_ENUM_TYPE) {
            /* Type alias: the name denotes exactly the aliased type. */
            Type* aliased = resolve_type_from_ast(ctx, comp->type_def);
            if (!aliased) return false;
            Symbol* sym = symbol_create(comp->name, SYMBOL_TYPE, aliased,
                                        comp->base.line, comp->base.column);
            if (!sym) {
                type_free(aliased);
                return false;
            }
            sym->is_public = (comp->modifiers & MOD_PUBLIC) != 0;
            sym->is_initialized = true;
            if (!scope_insert(ctx->current_scope, sym)) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, comp->base.line, comp->base.column, 1,
                                            "semantic", "Type '%s' already defined", comp->name);
                symbol_free(sym);
                return false;
            }
            return true;
        }
        Symbol* type_sym = NULL;
        if (!analyze_type_def(ctx, comp->type_def, &type_sym)) return false;
        type_sym->name = comp->name;
        type_sym->is_public = (comp->modifiers & MOD_PUBLIC) != 0;

        if (!scope_insert(ctx->current_scope, type_sym)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, comp->base.line, comp->base.column, 1,
                                        "semantic", "Type '%s' already defined", comp->name);
            symbol_free(type_sym);
            return false;
        }
        return true;
    } else if (comp->value) {
        int64_t result;
        if (!eval_const_expr(ctx, comp->value, &result)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, comp->value->line, comp->value->column, 1,
                                        "semantic", "Constant expression not evaluable");
            return false;
        }
        Type* const_type = type_Int32;
        Symbol* sym = symbol_create(comp->name, SYMBOL_CONSTANT, const_type,
                                    comp->base.line, comp->base.column);
        if (!sym) return false;
        sym->extra.const_value = result;
        sym->is_initialized = true;
        sym->is_public = (comp->modifiers & MOD_PUBLIC) != 0;
        if (!scope_insert(ctx->current_scope, sym)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, comp->base.line, comp->base.column, 1,
                                        "semantic", "Constant '%s' already defined", comp->name);
            symbol_free(sym);
            return false;
        }
        return true;
    }
    errhandler__report_error_ex(ERROR_LEVEL_ERROR, comp->base.line, comp->base.column, 1,
                                "semantic", "Invalid comptime declaration");
    return false;
}

/*
 * Analyze an extern declaration.
 */
bool analyze_extern_declaration(SemanticContext* ctx, ExternDecl* ext) {
    if (!ext) return false;
    if (ext->is_func) {
        Type* ret_type = resolve_type_from_ast(ctx, ext->type);
        if (!ret_type) return false;
        ParamInfo* param_list = NULL;
        ParamInfo* last = NULL;
        for (size_t i = 0; i < ext->param_count; ++i) {
            Param* p = (Param*)ext->params[i];
            if (!p) continue;
            Type* ptype = resolve_type_from_ast(ctx, p->type);
            if (!ptype) { type_free(ret_type); return false; }
            ParamInfo* pi = (ParamInfo*)calloc(1, sizeof(ParamInfo));
            if (!pi) { type_free(ptype); type_free(ret_type); return false; }
            pi->name = u__strduplic(p->name);
            pi->type = ptype;
            pi->next = NULL;
            if (!param_list) param_list = pi;
            else last->next = pi;
            last = pi;
        }
        Type* func_type = build_function_type(param_list, ext->param_count, ret_type);
        if (!func_type) { type_free(ret_type); return false; }
        /* sym->type is the full function type; extra.func.return_type borrows from it */
        Symbol* sym = symbol_create(ext->name, SYMBOL_FUNCTION, func_type,
                                    ext->base.line, ext->base.column);
        if (!sym) { type_free(func_type); return false; }
        sym->extra.func.return_type = ret_type;
        sym->extra.func.return_is_error = (ret_type && ret_type->kind == TYPE_KIND_ERROR);
        sym->extra.func.params = param_list;
        sym->extra.func.param_count = ext->param_count;
        sym->is_extern = true;
        sym->is_initialized = true;
        if (!scope_insert(ctx->current_scope, sym)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ext->base.line, ext->base.column, 1,
                                        "semantic", "Extern function '%s' already defined", ext->name);
            symbol_free(sym);
            return false;
        }
        return true;
    } else {
        Type* var_type = resolve_type_from_ast(ctx, ext->type);
        if (!var_type) return false;
        Symbol* sym = symbol_create(ext->name, SYMBOL_VARIABLE, var_type,
                                    ext->base.line, ext->base.column);
        if (!sym) { type_free(var_type); return false; }
        sym->is_extern = true;
        sym->is_initialized = true;
        if (!scope_insert(ctx->current_scope, sym)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ext->base.line, ext->base.column, 1,
                                        "semantic", "Extern variable '%s' already defined", ext->name);
            symbol_free(sym);
            return false;
        }
        return true;
    }
}

/*
 * Analyze an extern block: every collected prototype is analyzed as
 * an ordinary extern declaration.
 */
bool analyze_extern_block(SemanticContext* ctx, ExternBlock* blk) {
    if (!blk) return false;
    bool ok = true;
    for (size_t i = 0; i < blk->item_count; ++i) {
        if (!analyze_extern_declaration(ctx, (ExternDecl*)blk->items[i])) {
            ok = false;
        }
    }
    return ok;
}

/*
 * Control flow analysis: check that all paths in a function return a value.
 */
static bool check_all_paths_return(SemanticContext* ctx, ASTNode* node, bool is_void, bool can_error) {
    if (!node) return false;
    switch (node->type) {
        case NODE_BLOCK: {
            Block* b = (Block*)node;
            for (size_t i = 0; i < b->stmt_count; ++i) {
                if (check_all_paths_return(ctx, b->statements[i], is_void, can_error)) {
                    return true;
                }
            }
            return false;
        }
        case NODE_IF_STMT: {
            IfStmt* ifs = (IfStmt*)node;
            bool then_ret = check_all_paths_return(ctx, ifs->then_branch, is_void, can_error);
            bool else_ret = false;
            if (ifs->else_branch) {
                else_ret = check_all_paths_return(ctx, ifs->else_branch, is_void, can_error);
            } else {
                for (size_t i = 0; i < ifs->else_if_count; ++i) {
                    if (check_all_paths_return(ctx, ifs->else_if_branches[i], is_void, can_error)) {
                        else_ret = true;
                        break;
                    }
                }
            }
            return then_ret && else_ret;
        }
        case NODE_IF_EXISTS: {
            /* The return check follows the same branch selection as
               analyze_if_exists: only the taken branch is compiled. */
            IfExistsStmt* ie = (IfExistsStmt*)node;
            Symbol* sym = scope_lookup(ctx->current_scope, ie->name);
            bool exists = false;
            if (sym) {
                switch (ie->kind) {
                    case EXISTS_LET:
                        exists = (sym->kind == SYMBOL_VARIABLE || sym->kind == SYMBOL_PARAMETER);
                        break;
                    case EXISTS_FUNC:
                        exists = (sym->kind == SYMBOL_FUNCTION);
                        break;
                    case EXISTS_COMPTIME:
                        exists = (sym->kind == SYMBOL_CONSTANT || sym->kind == SYMBOL_TYPE ||
                                  (sym->kind == SYMBOL_FUNCTION && sym->is_comptime));
                        break;
                }
            }
            if (exists) {
                return check_all_paths_return(ctx, ie->then_branch, is_void, can_error);
            }
            if (ie->else_branch) {
                return check_all_paths_return(ctx, ie->else_branch, is_void, can_error);
            }
            return false;
        }
        case NODE_MATCH_STMT: {
            MatchStmt* ms = (MatchStmt*)node;
            bool all_ret = true;
            for (size_t i = 0; i < ms->case_count; ++i) {
                MatchCase* mc = (MatchCase*)ms->cases[i];
                if (!check_all_paths_return(ctx, mc->body, is_void, can_error)) {
                    all_ret = false;
                }
            }
            if (ms->else_case) {
                if (!check_all_paths_return(ctx, ms->else_case, is_void, can_error)) {
                    all_ret = false;
                }
            } else {
                all_ret = false;
            }
            return all_ret;
        }
        case NODE_RETURN_STMT: {
            ReturnStmt* ret = (ReturnStmt*)node;
            if (!is_void && !ret->value) return false;
            if (ret->value) {
                /* A brace literal in a tuple-returning function is a
                   tuple, analyzed element-wise (as in analyze_return). */
                Type* t = (ctx->return_type && ctx->return_type->kind == TYPE_KIND_TUPLE &&
                           ret->value->type == NODE_ARRAY_LITERAL)
                    ? analyze_brace_as_tuple(ctx, (ArrayLiteral*)ret->value)
                    : analyze_expression(ctx, ret->value);
                if (!t) return false;
                bool ok = type_compatible(t, ctx->return_type);
                type_free(t);
                if (!ok) return false;
            }
            return true;
        }
        case NODE_DO_WHILE_STMT: {
            DoWhileStmt* dw = (DoWhileStmt*)node;
            return check_all_paths_return(ctx, dw->body, is_void, can_error);
        }
        case NODE_WHILE_STMT:
        case NODE_FOR_STMT:
        case NODE_DEFER_STMT:
        case NODE_VOLATILE_STMT:
        case NODE_COMPTIME_STMT:
        case NODE_PASS_STMT:
        case NODE_EXPR_STMT:
        case NODE_BREAK_STMT:
        case NODE_CONTINUE_STMT:
            return false;
        default:
            return false;
    }
}
