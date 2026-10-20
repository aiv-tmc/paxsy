#include "comptime.h"
#include "expression.h"
#include "semantic.h"
#include "builtins.h"
#include "statement.h"
#include "../parser/parser.h"
#include "../../interfaces/errhandler/errhandler.h"
#include <string.h>
#include <stdlib.h>

/*
 * Forward declarations for internal evaluators.
 */
static bool eval_block(SemanticContext* ctx, ASTNode* block, int64_t* result);
static bool eval_statement(SemanticContext* ctx, ASTNode* stmt, int64_t* out_val, bool* has_return);

/*
 * Evaluate a comptime function call.
 * Defined before eval_const_expr because it is called from there.
 */
bool comptime_evaluate_call(SemanticContext* ctx, Symbol* func,
                            ASTNode** args, size_t arg_count, int64_t* result) {
    if (!func || !func->is_comptime) return false;

    /* Evaluate all arguments as constants */
    int64_t* arg_vals = NULL;
    if (arg_count > 0) {
        arg_vals = (int64_t*)calloc(arg_count, sizeof(int64_t));
        if (!arg_vals) return false;
        for (size_t i = 0; i < arg_count; ++i) {
            if (!eval_const_expr(ctx, args[i], &arg_vals[i])) {
                free(arg_vals);
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, args[i]->line, args[i]->column, 1,
                                            "semantic", "Argument %zu is not a constant expression", i+1);
                return false;
            }
        }
    }

    /* Create temporary scope for parameters */
    Scope* param_scope = scope_create(ctx->current_scope, SCOPE_FUNCTION);
    if (!param_scope) { free(arg_vals); return false; }
    Scope* old_scope = ctx->current_scope;
    ctx->current_scope = param_scope;

    /* Bind parameters to argument values */
    ParamInfo* p = func->extra.func.params;
    size_t idx = 0;
    while (p && idx < arg_count) {
        Symbol* param_sym = symbol_create(p->name, SYMBOL_PARAMETER, type_copy(p->type), 0, 0);
        if (param_sym) {
            param_sym->kind = SYMBOL_CONSTANT;
            param_sym->extra.const_value = arg_vals[idx];
            param_sym->is_initialized = true;
            scope_insert(param_scope, param_sym);
        }
        p = p->next;
        idx++;
    }
    free(arg_vals);

    /* Execute function body */
    ASTNode* body = func->extra.func.comptime_body;
    bool ok = false;
    if (body) {
        if (body->type == NODE_BLOCK) {
            ok = eval_block(ctx, body, result);
        } else {
            ok = eval_const_expr(ctx, body, result);
        }
    }

    ctx->current_scope = old_scope;
    scope_destroy(param_scope);
    return ok;
}

/*
 * Evaluate a constant expression.
 */
bool eval_const_expr(SemanticContext* ctx, ASTNode* node, int64_t* out_val) {
    if (!node) return false;
    switch (node->type) {
        case NODE_LITERAL: {
            LiteralExpr* lit = (LiteralExpr*)node;
            if (lit->kind == LIT_NUMBER) {
                char* end;
                long long val = strtoll(lit->raw_value, &end, 10);
                if (*end != '\0' && *end != 'i' && *end != 'u' && *end != 'f') return false;
                *out_val = val;
                return true;
            }
            return false;
        }
        case NODE_IDENT: {
            IdentExpr* id = (IdentExpr*)node;
            Symbol* sym = scope_lookup(ctx->current_scope, id->name);
            if (!sym) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, id->base.line, id->base.column, 1,
                                            "semantic", "Constant '%s' not found", id->name);
                return false;
            }
            if (sym->kind == SYMBOL_CONSTANT) {
                *out_val = sym->extra.const_value;
                return true;
            }
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, id->base.line, id->base.column, 1,
                                        "semantic", "'%s' is not a constant", id->name);
            return false;
        }
        case NODE_BINARY: {
            BinaryExpr* bin = (BinaryExpr*)node;
            int64_t left, right;
            if (!eval_const_expr(ctx, bin->left, &left)) return false;
            if (!eval_const_expr(ctx, bin->right, &right)) return false;
            switch (bin->op) {
                case BIN_ADD: *out_val = left + right; return true;
                case BIN_SUB: *out_val = left - right; return true;
                case BIN_MUL: *out_val = left * right; return true;
                case BIN_DIV:
                    if (right == 0) { errhandler__report_error_ex(ERROR_LEVEL_ERROR, bin->base.line, bin->base.column, 1, "semantic", "Division by zero"); return false; }
                    *out_val = left / right; return true;
                case BIN_MOD:
                    if (right == 0) { errhandler__report_error_ex(ERROR_LEVEL_ERROR, bin->base.line, bin->base.column, 1, "semantic", "Modulo by zero"); return false; }
                    *out_val = left % right; return true;
                case BIN_EQ:   *out_val = (left == right); return true;
                case BIN_NE:   *out_val = (left != right); return true;
                case BIN_LT:   *out_val = (left < right); return true;
                case BIN_GT:   *out_val = (left > right); return true;
                case BIN_LE:   *out_val = (left <= right); return true;
                case BIN_GE:   *out_val = (left >= right); return true;
                case BIN_AND:  *out_val = (left && right); return true;
                case BIN_OR:   *out_val = (left || right); return true;
                case BIN_XOR:  *out_val = (left ^ right); return true;
                default: return false;
            }
        }
        case NODE_UNARY: {
            UnaryExpr* un = (UnaryExpr*)node;
            int64_t operand;
            if (!eval_const_expr(ctx, un->operand, &operand)) return false;
            switch (un->op) {
                case UNARY_MINUS: *out_val = -operand; return true;
                case UNARY_PLUS:  *out_val = operand; return true;
                case UNARY_LOGICAL_NOT: *out_val = !operand; return true;
                case UNARY_BIT_NOT: *out_val = ~operand; return true;
                default: return false;
            }
        }
        case NODE_CALL: {
            CallExpr* call = (CallExpr*)node;
            Symbol* func_sym = NULL;
            if (call->callee->type == NODE_MODULE_ACCESS) {
                ModuleAccess* ma = (ModuleAccess*)call->callee;
                if (ma->path_len == 2 && strcmp(ma->path[0], "std") == 0 && strcmp(ma->path[1], "type") == 0) {
                    func_sym = builtins_get_symbol(ma->symbol);
                }
            } else if (call->callee->type == NODE_IDENT) {
                IdentExpr* id = (IdentExpr*)call->callee;
                Symbol* sym = scope_lookup(ctx->current_scope, id->name);
                if (sym && (sym->is_comptime_builtin || sym->is_comptime)) {
                    func_sym = sym;
                }
            }
            if (!func_sym) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, call->base.line, call->base.column, 1,
                                            "semantic", "Cannot evaluate non-builtin non-comptime function at compile time");
                return false;
            }
            if (func_sym->is_comptime_builtin) {
                int64_t result = func_sym->comptime_eval(ctx, call->args, call->arg_count);
                *out_val = result;
                return true;
            } else if (func_sym->is_comptime) {
                return comptime_evaluate_call(ctx, func_sym, call->args, call->arg_count, out_val);
            }
            return false;
        }
        default:
            return false;
    }
}

/*
 * Evaluate a block in comptime context.
 */
static bool eval_block(SemanticContext* ctx, ASTNode* block, int64_t* result) {
    if (!block || block->type != NODE_BLOCK) return false;
    Block* b = (Block*)block;
    for (size_t i = 0; i < b->stmt_count; ++i) {
        bool has_return = false;
        if (!eval_statement(ctx, b->statements[i], result, &has_return)) {
            return false;
        }
        if (has_return) return true;
    }
    return false;
}

/*
 * Evaluate a statement in comptime context.
 */
static bool eval_statement(SemanticContext* ctx, ASTNode* stmt, int64_t* out_val, bool* has_return) {
    *has_return = false;
    if (!stmt) return true;
    switch (stmt->type) {
        case NODE_BLOCK:
            return eval_block(ctx, stmt, out_val);
        case NODE_RETURN_STMT: {
            ReturnStmt* ret = (ReturnStmt*)stmt;
            if (ret->value) {
                if (!eval_const_expr(ctx, ret->value, out_val)) return false;
            } else {
                *out_val = 0;
            }
            *has_return = true;
            return true;
        }
        case NODE_IF_STMT: {
            IfStmt* ifs = (IfStmt*)stmt;
            int64_t cond_val;
            if (!eval_const_expr(ctx, ifs->condition, &cond_val)) return false;
            if (cond_val) {
                return eval_statement(ctx, ifs->then_branch, out_val, has_return);
            } else {
                for (size_t i = 0; i < ifs->else_if_count; ++i) {
                    int64_t econd;
                    if (!eval_const_expr(ctx, ifs->else_if_conditions[i], &econd)) return false;
                    if (econd) {
                        return eval_statement(ctx, ifs->else_if_branches[i], out_val, has_return);
                    }
                }
                if (ifs->else_branch) {
                    return eval_statement(ctx, ifs->else_branch, out_val, has_return);
                } else {
                    return true;
                }
            }
        }
        case NODE_MATCH_STMT: {
            MatchStmt* ms = (MatchStmt*)stmt;
            int64_t expr_val;
            if (!eval_const_expr(ctx, ms->expr, &expr_val)) return false;
            for (size_t i = 0; i < ms->case_count; ++i) {
                MatchCase* mc = (MatchCase*)ms->cases[i];
                for (size_t j = 0; j < mc->pattern_count; ++j) {
                    int64_t pat_val;
                    if (!eval_const_expr(ctx, mc->patterns[j], &pat_val)) return false;
                    if (expr_val == pat_val) {
                        return eval_statement(ctx, mc->body, out_val, has_return);
                    }
                }
            }
            if (ms->else_case) {
                return eval_statement(ctx, ms->else_case, out_val, has_return);
            }
            return true;
        }
        case NODE_EXPR_STMT: {
            ExprStmt* es = (ExprStmt*)stmt;
            int64_t dummy;
            if (!eval_const_expr(ctx, es->expr, &dummy)) return false;
            return true;
        }
        case NODE_PASS_STMT:
            return true;
        default:
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, stmt->line, stmt->column, 1,
                                        "semantic", "Unsupported statement in comptime function");
            return false;
    }
}
