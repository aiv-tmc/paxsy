#include "statement.h"
#include "semantic.h"
#include "expression.h"
#include "declaration.h"
#include "comptime.h"
#include "type_utils.h"
#include "../../interfaces/errhandler/errhandler.h"
#include "../../interfaces/utils/common.h"
#include <string.h>
#include <stdlib.h>

/*
 * Push a label onto the label stack.
 */
static void push_label(SemanticContext* ctx, const char* name, bool is_loop) {
    LabelEntry* e = (LabelEntry*)memory_allocate_zero(sizeof(LabelEntry));
    if (!e) return;
    e->name = u__strduplic(name);
    e->is_loop = is_loop;
    e->next = (LabelEntry*)ctx->label_stack;
    ctx->label_stack = (void*)e;
}

/*
 * Pop a label from the label stack.
 */
static void pop_label(SemanticContext* ctx) {
    if (!ctx->label_stack) return;
    LabelEntry* next = (LabelEntry*)ctx->label_stack->next;
    free(((LabelEntry*)ctx->label_stack)->name);
    free(ctx->label_stack);
    ctx->label_stack = (void*)next;
}

/*
 * Find a label by name in the stack.
 */
static bool find_label(SemanticContext* ctx, const char* name, bool* is_loop) {
    LabelEntry* e = (LabelEntry*)ctx->label_stack;
    while (e) {
        if (strcmp(e->name, name) == 0) {
            *is_loop = e->is_loop;
            return true;
        }
        e = e->next;
    }
    return false;
}

/*
 * Dispatch analysis of a statement node.
 */
bool analyze_statement(SemanticContext* ctx, ASTNode* node) {
    if (!node) return true;
    switch (node->type) {
        case NODE_BLOCK:          return analyze_block(ctx, (Block*)node);
        case NODE_LET_DECL:       return analyze_let_declaration(ctx, (LetDecl*)node);
        case NODE_USE:            return analyze_use_declaration(ctx, (UseDecl*)node);
        case NODE_IF_STMT:        return analyze_if(ctx, (IfStmt*)node);
        case NODE_IF_EXISTS:      return analyze_if_exists(ctx, (IfExistsStmt*)node);
        case NODE_MATCH_STMT:     return analyze_match(ctx, (MatchStmt*)node);
        case NODE_WHILE_STMT:     return analyze_while(ctx, (WhileStmt*)node);
        case NODE_DO_WHILE_STMT:  return analyze_do_while(ctx, (DoWhileStmt*)node);
        case NODE_FOR_STMT:       return analyze_for(ctx, (ForStmt*)node);
        case NODE_BREAK_STMT:     return analyze_break(ctx, (BreakStmt*)node);
        case NODE_CONTINUE_STMT:  return analyze_continue(ctx, (ContinueStmt*)node);
        case NODE_RETURN_STMT:    return analyze_return(ctx, (ReturnStmt*)node);
        case NODE_DEFER_STMT:     return analyze_defer(ctx, (DeferStmt*)node);
        case NODE_ASM_STMT:       return analyze_asm(ctx, (AsmStmt*)node);
        case NODE_VOLATILE_STMT:  return analyze_volatile(ctx, (VolatileStmt*)node);
        case NODE_COMPTIME_STMT:  return analyze_comptime_statement(ctx, (ComptimeStmt*)node);
        case NODE_PASS_STMT:      return analyze_pass(ctx, (PassStmt*)node);
        case NODE_EMPTY_STMT:     return true;
        case NODE_EXPR_STMT:      return analyze_expr_statement(ctx, (ExprStmt*)node);
        case NODE_FREE_STMT:      return analyze_free_stmt(ctx, (FreeStmt*)node);
        default:
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, node->line, node->column, 1,
                                        "semantic", "Invalid statement node");
            return false;
    }
}

/*
 * Analyze a block. Every block opens its own scope: variables declared
 * inside do not leak into the enclosing scope.
 */
bool analyze_block(SemanticContext* ctx, Block* block) {
    if (!block) return true;
    Scope* block_scope = scope_create(ctx->current_scope, SCOPE_BLOCK);
    if (!block_scope) return false;
    ctx->current_scope = block_scope;
    bool ok = true;
    for (size_t i = 0; i < block->stmt_count; ++i) {
        if (!analyze_statement(ctx, block->statements[i])) {
            ok = false;
            break;
        }
    }
    ctx->current_scope = block_scope->parent;
    return ok;
}

/*
 * Analyze a compile-time existence check: resolve whether the named
 * entity is visible in the current scope and analyze only the taken
 * branch. The discarded branch does not need to type-check.
 */
bool analyze_if_exists(SemanticContext* ctx, IfExistsStmt* stmt) {
    Symbol* sym = scope_lookup(ctx->current_scope, stmt->name);
    bool exists = false;
    if (sym) {
        switch (stmt->kind) {
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
        return analyze_statement(ctx, stmt->then_branch);
    }
    if (stmt->else_branch) {
        return analyze_statement(ctx, stmt->else_branch);
    }
    return true;
}

/*
 * Analyze an if statement.
 */
bool analyze_if(SemanticContext* ctx, IfStmt* ifstmt) {
    Type* cond = analyze_expression(ctx, ifstmt->condition);
    if (!cond) return false;
    if (!type_compatible(cond, type_Bool)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ifstmt->condition->line,
                                    ifstmt->condition->column, 1,
                                    "semantic", "If condition must be boolean");
        type_free(cond);
        return false;
    }
    type_free(cond);

    bool ok = true;
    if (!analyze_statement(ctx, ifstmt->then_branch)) ok = false;
    for (size_t i = 0; i < ifstmt->else_if_count; ++i) {
        Type* econd = analyze_expression(ctx, ifstmt->else_if_conditions[i]);
        if (!econd) { ok = false; continue; }
        if (!type_compatible(econd, type_Bool)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR,
                                        ifstmt->else_if_conditions[i]->line,
                                        ifstmt->else_if_conditions[i]->column, 1,
                                        "semantic", "Else‑if condition must be boolean");
            type_free(econd);
            ok = false;
            continue;
        }
        type_free(econd);
        if (!analyze_statement(ctx, ifstmt->else_if_branches[i])) ok = false;
    }
    if (ifstmt->else_branch) {
        if (!analyze_statement(ctx, ifstmt->else_branch)) ok = false;
    }
    return ok;
}

/*
 * Analyze a match statement.
 */
bool analyze_match(SemanticContext* ctx, MatchStmt* match) {
    Type* expr_type = analyze_expression(ctx, match->expr);
    if (!expr_type) return false;

    bool ok = true;
    bool prev_match = ctx->in_match;
    ctx->in_match = true;

    for (size_t i = 0; i < match->case_count; ++i) {
        MatchCase* mc = (MatchCase*)match->cases[i];
        for (size_t j = 0; j < mc->pattern_count; ++j) {
            Type* pat_type = analyze_expression(ctx, mc->patterns[j]);
            if (!pat_type) { ok = false; continue; }
            if (!type_compatible(pat_type, expr_type)) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, mc->patterns[j]->line,
                                            mc->patterns[j]->column, 1,
                                            "semantic", "Pattern type incompatible with matched expression");
                type_free(pat_type);
                ok = false;
                continue;
            }
            type_free(pat_type);
        }
        if (!analyze_statement(ctx, mc->body)) ok = false;
    }
    if (match->else_case) {
        if (!analyze_statement(ctx, match->else_case)) ok = false;
    }

    ctx->in_match = prev_match;
    type_free(expr_type);
    return ok;
}

/*
 * Analyze a while loop.
 */
bool analyze_while(SemanticContext* ctx, WhileStmt* while_stmt) {
    Type* cond = analyze_expression(ctx, while_stmt->condition);
    if (!cond) return false;
    if (!type_compatible(cond, type_Bool)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, while_stmt->condition->line,
                                    while_stmt->condition->column, 1,
                                    "semantic", "While condition must be boolean");
        type_free(cond);
        return false;
    }
    type_free(cond);

    if (while_stmt->label) push_label(ctx, while_stmt->label, true);
    bool prev_loop = ctx->in_loop;
    ctx->in_loop = true;
    bool ok = analyze_statement(ctx, while_stmt->body);
    ctx->in_loop = prev_loop;
    if (while_stmt->label) pop_label(ctx);
    return ok;
}

/*
 * Analyze a do-while loop.
 */
bool analyze_do_while(SemanticContext* ctx, DoWhileStmt* do_while) {
    Type* cond = analyze_expression(ctx, do_while->condition);
    if (!cond) return false;
    if (!type_compatible(cond, type_Bool)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, do_while->condition->line,
                                    do_while->condition->column, 1,
                                    "semantic", "Do‑while condition must be boolean");
        type_free(cond);
        return false;
    }
    type_free(cond);

    if (do_while->label) push_label(ctx, do_while->label, true);
    bool prev_loop = ctx->in_loop;
    ctx->in_loop = true;
    bool ok = analyze_statement(ctx, do_while->body);
    ctx->in_loop = prev_loop;
    if (do_while->label) pop_label(ctx);
    return ok;
}

/*
 * Analyze a for loop.
 */
bool analyze_for(SemanticContext* ctx, ForStmt* for_stmt) {
    if (for_stmt->range->type == NODE_RANGE) {
        RangeExpr* range = (RangeExpr*)for_stmt->range;
        Type* start_type = analyze_expression(ctx, range->start);
        Type* end_type = analyze_expression(ctx, range->end);
        if (!start_type || !end_type) { type_free(start_type); type_free(end_type); return false; }
        if (!type_is_integer(start_type) || !type_is_integer(end_type)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, range->base.line, range->base.column, 1,
                                        "semantic", "Range bounds must be integers");
            type_free(start_type); type_free(end_type);
            return false;
        }
        type_free(start_type); type_free(end_type);
        Type* elem_type = type_basic(TYPE_BASIC_INT32);

        Scope* loop_scope = scope_create(ctx->current_scope, SCOPE_BLOCK);
        if (!loop_scope) return false;
        ctx->current_scope = loop_scope;

        Symbol* var_sym = symbol_create(for_stmt->var_name, SYMBOL_VARIABLE,
                                        type_copy(elem_type),
                                        for_stmt->base.line, for_stmt->base.column);
        if (var_sym) {
            var_sym->is_initialized = true;
            scope_insert(loop_scope, var_sym);
        }

        if (for_stmt->label) push_label(ctx, for_stmt->label, true);
        bool prev_loop = ctx->in_loop;
        ctx->in_loop = true;
        bool ok = analyze_statement(ctx, for_stmt->body);
        ctx->in_loop = prev_loop;
        if (for_stmt->label) pop_label(ctx);

        ctx->current_scope = ctx->current_scope->parent;
        return ok;
    } else {
        Type* range_type = analyze_expression(ctx, for_stmt->range);
        if (!range_type) return false;
        if (range_type->kind != TYPE_KIND_ARRAY) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, for_stmt->range->line,
                                        for_stmt->range->column, 1,
                                        "semantic", "For loop range must be iterable (array or range)");
            type_free(range_type);
            return false;
        }
        Type* elem_type = range_type->u.array.base;
        type_free(range_type);

        Scope* loop_scope = scope_create(ctx->current_scope, SCOPE_BLOCK);
        if (!loop_scope) return false;
        ctx->current_scope = loop_scope;

        Symbol* var_sym = symbol_create(for_stmt->var_name, SYMBOL_VARIABLE,
                                        type_copy(elem_type),
                                        for_stmt->base.line, for_stmt->base.column);
        if (var_sym) {
            var_sym->is_initialized = true;
            scope_insert(loop_scope, var_sym);
        }

        if (for_stmt->label) push_label(ctx, for_stmt->label, true);
        bool prev_loop = ctx->in_loop;
        ctx->in_loop = true;
        bool ok = analyze_statement(ctx, for_stmt->body);
        ctx->in_loop = prev_loop;
        if (for_stmt->label) pop_label(ctx);

        ctx->current_scope = ctx->current_scope->parent;
        return ok;
    }
}

/*
 * Analyze a break statement.
 */
bool analyze_break(SemanticContext* ctx, BreakStmt* brk) {
    if (brk->label) {
        bool is_loop = false;
        if (!find_label(ctx, brk->label, &is_loop)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, brk->base.line,
                                        brk->base.column, 1,
                                        "semantic", "Break label '%s' not found", brk->label);
            return false;
        }
        if (!is_loop) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, brk->base.line,
                                        brk->base.column, 1,
                                        "semantic", "Break target must be a loop");
            return false;
        }
    } else if (!ctx->in_loop) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, brk->base.line,
                                    brk->base.column, 1,
                                    "semantic", "Break outside loop");
        return false;
    }
    return true;
}

/*
 * Analyze a continue statement.
 */
bool analyze_continue(SemanticContext* ctx, ContinueStmt* cont) {
    if (cont->label) {
        bool is_loop = false;
        if (!find_label(ctx, cont->label, &is_loop)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, cont->base.line,
                                        cont->base.column, 1,
                                        "semantic", "Continue label '%s' not found", cont->label);
            return false;
        }
        if (!is_loop) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, cont->base.line,
                                        cont->base.column, 1,
                                        "semantic", "Continue target must be a loop");
            return false;
        }
    } else if (!ctx->in_loop) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, cont->base.line,
                                    cont->base.column, 1,
                                    "semantic", "Continue outside loop");
        return false;
    }
    return true;
}

/*
 * Analyze a return statement.
 */
bool analyze_return(SemanticContext* ctx, ReturnStmt* ret) {
    if (!ctx->in_function) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ret->base.line,
                                    ret->base.column, 1,
                                    "semantic", "Return outside function");
        return false;
    }
    if (ret->value) {
        /* A brace literal returned from a tuple-returning function
           is a tuple, analyzed element-wise. */
        Type* ret_type = (ctx->return_type && ctx->return_type->kind == TYPE_KIND_TUPLE &&
                          ret->value->type == NODE_ARRAY_LITERAL)
            ? analyze_brace_as_tuple(ctx, (ArrayLiteral*)ret->value)
            : analyze_expression(ctx, ret->value);
        if (!ret_type) return false;
        if (type_equal(ctx->return_type, type_Void)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ret->value->line,
                                        ret->value->column, 1,
                                        "semantic", "Void function cannot return a value");
            type_free(ret_type);
            return false;
        }
        if (!type_compatible(ret_type, ctx->return_type)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ret->value->line,
                                        ret->value->column, 1,
                                        "semantic", "Return type mismatch: expected %s, got %s",
                                        type_to_string(ctx->return_type),
                                        type_to_string(ret_type));
            type_free(ret_type);
            return false;
        }
        type_free(ret_type);
    } else {
        if (!type_equal(ctx->return_type, type_Void)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ret->base.line,
                                        ret->base.column, 1,
                                        "semantic", "Non‑void function must return a value");
            return false;
        }
    }
    ctx->has_return = true;
    return true;
}

/*
 * Analyze a defer statement.
 */
bool analyze_defer(SemanticContext* ctx, DeferStmt* defer) {
    if (!defer->stmt) return true;
    return analyze_statement(ctx, defer->stmt);
}

/*
 * Analyze an asm statement.
 */
bool analyze_asm(SemanticContext* ctx, AsmStmt* asm_stmt) {
    (void)ctx;
    if (asm_stmt->code) {
        if (asm_stmt->code->type != NODE_LITERAL) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, asm_stmt->code->line,
                                        asm_stmt->code->column, 1,
                                        "semantic", "Asm argument must be a string literal");
            return false;
        }
        LiteralExpr* lit = (LiteralExpr*)asm_stmt->code;
        if (lit->kind != LIT_STRING) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, asm_stmt->code->line,
                                        asm_stmt->code->column, 1,
                                        "semantic", "Asm argument must be a string literal");
            return false;
        }
    }
    return true;
}

/*
 * Analyze a volatile statement.
 */
bool analyze_volatile(SemanticContext* ctx, VolatileStmt* volatile_stmt) {
    return analyze_statement(ctx, volatile_stmt->stmt);
}

/*
 * Analyze a comptime statement.
 */
bool analyze_comptime_statement(SemanticContext* ctx, ComptimeStmt* comp) {
    if (comp->stmt) {
        return analyze_statement(ctx, comp->stmt);
    }
    return true;
}

/*
 * Analyze a pass statement.
 */
bool analyze_pass(SemanticContext* ctx, PassStmt* pass) {
    (void)ctx; (void)pass;
    return true;
}

/*
 * Analyze an expression statement.
 */
bool analyze_expr_statement(SemanticContext* ctx, ExprStmt* expr_stmt) {
    if (!expr_stmt->expr) return true;
    Type* t = analyze_expression(ctx, expr_stmt->expr);
    if (!t) return false;
    type_free(t);
    return true;
}

/*
 * Analyze a free statement.
 */
bool analyze_free_stmt(SemanticContext* ctx, FreeStmt* free_stmt) {
    if (!type_utils_check_expression_is_pointer(ctx, free_stmt->expr, "free argument")) {
        return false;
    }
    return true;
}
