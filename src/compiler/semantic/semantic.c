#include "semantic.h"
#include "expression.h"
#include "builtins.h"
#include "comptime.h"
#include "statement.h"
#include "declaration.h"
#include "../../interfaces/errhandler/errhandler.h"
#include "../../interfaces/utils/common.h"
#include <string.h>
#include <stdlib.h>

/* Create a new semantic context */
SemanticContext* semantic__create_context(void) {
    SemanticContext* ctx = (SemanticContext*)memory_allocate_zero(sizeof(SemanticContext));
    if (!ctx) return NULL;

    ctx->global_scope = scope_create(NULL, SCOPE_GLOBAL);
    if (!ctx->global_scope) {
        free(ctx);
        return NULL;
    }
    ctx->file_scope = scope_create(ctx->global_scope, SCOPE_MODULE);
    if (!ctx->file_scope) {
        scope_destroy(ctx->global_scope);
        free(ctx);
        return NULL;
    }
    ctx->current_scope = ctx->file_scope;
    ctx->in_function = false;
    ctx->in_loop = false;
    ctx->in_match = false;
    ctx->return_type = NULL;
    ctx->return_has_error = false;
    ctx->current_func_name = NULL;
    ctx->resolver = NULL;
    ctx->extra_warnings = false;
    ctx->has_return = false;
    ctx->label_stack = NULL;
    return ctx;
}

/* Destroy a semantic context */
void semantic__destroy_context(SemanticContext* ctx) {
    if (!ctx) return;
    while (ctx->label_stack) {
        LabelEntry* next = ctx->label_stack->next;
        free(ctx->label_stack->name);
        free(ctx->label_stack);
        ctx->label_stack = next;
    }
    free(ctx);
}

/* Enable or disable extra warnings */
void semantic__set_extra_warnings(SemanticContext* ctx, bool enable) {
    if (ctx) ctx->extra_warnings = enable;
}

/* Main semantic analysis entry point */
int semantic__analyze(Program* ast, ModuleResolver resolver) {
    if (!ast) return 1;
    SemanticContext* ctx = semantic__create_context();
    if (!ctx) return 1;
    ctx->resolver = resolver;

    bool ok = true;
    for (size_t i = 0; i < ast->item_count; ++i) {
        ASTNode* item = ast->items[i];
        switch (item->type) {
            case NODE_MODULE:
                if (!analyze_module_declaration(ctx, (ModuleDecl*)item)) ok = false;
                break;
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
            case NODE_EXTERN_BLOCK:
                if (!analyze_extern_block(ctx, (ExternBlock*)item)) ok = false;
                break;
            default:
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, item->line, item->column, 1,
                                            "semantic", "Unexpected top‑level item");
                ok = false;
                break;
        }
    }

    semantic__destroy_context(ctx);
    return ok ? 0 : 1;
}
