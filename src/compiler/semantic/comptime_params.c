#include "comptime_params.h"
#include "declaration.h"
#include "expression.h"
#include "comptime.h"
#include "../../interfaces/errhandler/errhandler.h"
#include "../../interfaces/utils/str.h"
#include <stdlib.h>
#include <string.h>

/*
 * Returns true when the argument node is a type designator rather
 * than a value expression.
 */
static bool arg_is_type_node(ASTNode* arg) {
    switch (arg->type) {
        case NODE_BASIC_TYPE:
        case NODE_FAMILY_TYPE:
        case NODE_POINTER_TYPE:
        case NODE_ARRAY_TYPE:
        case NODE_TUPLE_TYPE:
        case NODE_NULLABLE_TYPE:
        case NODE_ERROR_TYPE:
        case NODE_CHAN_TYPE:
        case NODE_FUNC_TYPE:
        case NODE_IDENT_TYPE:
        case NODE_STRUCT_TYPE:
        case NODE_UNION_TYPE:
        case NODE_ENUM_TYPE:
            return true;
        default:
            return false;
    }
}

/*
 * Free a ParamInfo list built during specialization.
 */
static void free_param_list(ParamInfo* list) {
    while (list) {
        ParamInfo* next = list->next;
        free(list->name);
        type_free(list->type);
        free(list);
        list = next;
    }
}

Type* specialize_template_call(SemanticContext* ctx, Symbol* tmpl, CallExpr* call) {
    FuncDecl* decl = (FuncDecl*)tmpl->extra.func.comptime_body;
    if (call->arg_count != decl->param_count) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, call->base.line, call->base.column, 1,
                                    "semantic", "Argument count mismatch: expected %zu, got %zu",
                                    decl->param_count, call->arg_count);
        return NULL;
    }

    Scope* saved_scope = ctx->current_scope;
    Scope* spec_scope = scope_create(saved_scope, SCOPE_BLOCK);
    if (!spec_scope) return NULL;
    ctx->current_scope = spec_scope;

    ParamInfo* param_list = NULL;
    ParamInfo* last = NULL;
    bool ok = true;

    for (size_t i = 0; i < decl->param_count && ok; ++i) {
        Param* p = (Param*)decl->params[i];
        ASTNode* arg = call->args[i];
        if (p->is_comptime) {
            if (arg_is_type_node(arg)) {
                /* A type name binds the comptime parameter as an alias. */
                Type* t = resolve_type_from_ast(ctx, arg);
                if (!t) { ok = false; break; }
                Symbol* s = symbol_create(p->name, SYMBOL_TYPE, t, arg->line, arg->column);
                if (!s || !scope_insert(spec_scope, s)) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, arg->line, arg->column, 1,
                                                "semantic", "Cannot bind comptime parameter '%s'", p->name);
                    ok = false;
                }
            } else {
                /* Any other comptime argument must be a compile-time constant. */
                int64_t val = 0;
                if (!eval_const_expr(ctx, arg, &val)) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, arg->line, arg->column, 1,
                                                "semantic", "comptime argument must be a type or a compile-time constant");
                    ok = false;
                    break;
                }
                Symbol* s = symbol_create(p->name, SYMBOL_CONSTANT, type_Int32,
                                          arg->line, arg->column);
                if (!s) { ok = false; break; }
                s->extra.const_value = val;
                s->is_initialized = true;
                if (!scope_insert(spec_scope, s)) { ok = false; }
            }
        } else {
            /* Ordinary parameter: check the argument against the
               declared type, resolved with the comptime bindings. */
            Type* ptype = resolve_type_from_ast(ctx, p->type);
            if (!ptype) { ok = false; break; }
            Type* atype = analyze_expression(ctx, arg);
            if (!atype) {
                type_free(ptype);
                ok = false;
                break;
            }
            if (!type_compatible(atype, ptype)) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, arg->line, arg->column, 1,
                                            "semantic", "Argument %zu type mismatch: expected %s, got %s",
                                            i + 1, type_to_string(ptype), type_to_string(atype));
                type_free(atype);
                type_free(ptype);
                ok = false;
                break;
            }
            type_free(atype);
            ParamInfo* pi = (ParamInfo*)calloc(1, sizeof(ParamInfo));
            if (!pi) {
                type_free(ptype);
                ok = false;
                break;
            }
            pi->name = u__strduplic(p->name);
            pi->type = ptype;
            pi->next = NULL;
            if (!param_list) param_list = pi;
            else last->next = pi;
            last = pi;
        }
    }

    Type* ret_type = NULL;
    if (ok) {
        ret_type = decl->return_type ? resolve_type_from_ast(ctx, decl->return_type)
                                     : type_Void;
        ok = (ret_type != NULL);
    }
    if (ok) {
        ok = analyze_function_body(ctx, decl, param_list, ret_type);
    }

    ctx->current_scope = saved_scope;
    free_param_list(param_list);
    if (!ok) {
        type_free(ret_type);
        return NULL;
    }
    return ret_type;
}
