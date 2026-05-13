#include "optimizer.h"
#include "../utils/common.h"
#include "../parser/parser.h"
#include "../semantic/semantic.h"
#include "../errhandler/errhandler.h"
#include "../output/output.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

static FILE *optimizer_debug_out = NULL;
static bool optimizer_debug_enabled = false;

void optimizer__enable_debug(bool enable) {
    optimizer_debug_enabled = enable;
}

void optimizer__set_debug_file(FILE *out) {
    optimizer_debug_out = out ? out : stdout;
}

static void optimizer_debug_print_ast(const char *title, AST *ast) {
    if (!optimizer_debug_enabled || !optimizer_debug_out) return;
    fprintf(optimizer_debug_out, "\n========== %s ==========\n", title);
    if (!ast) { fprintf(optimizer_debug_out, "(null AST)\n"); return; }
    print_ast_detailed(ast, optimizer_debug_out);
}

#define POOL_ALLOC(pool)                                                       \
    ({                                                                         \
        ASTNode *n = parser__ast_node_pool_alloc(pool);                        \
        if (n) {                                                               \
            memset(n, 0, sizeof(ASTNode));                                     \
        } else {                                                               \
            errhandler__report_error(ERROR_CODE_OPTIM_MEMORY_ALLOCATION,       \
                                     0, 0, "optimizer",                        \
                                     "failed to allocate AST node from pool"); \
        }                                                                      \
        n;                                                                     \
    })

#define STR_DUP(s)  u__strduplic(s)

static bool add_ast_node_to_list(AST *ast, ASTNode *node);
static void free_cloned_node(ASTNode *node);
static ASTNode *clone_ast_node(ASTNode *node, ASTNodePool *pool);
static bool ast_nodes_identical(ASTNode *a, ASTNode *b);
static bool ast_nodes_identical_commutative(ASTNode *a, ASTNode *b);

static bool add_ast_node_to_list(AST *ast, ASTNode *node) {
    if (!ast || !node) return false;
    const uint16_t initial_cap = 4;
    if (ast->count >= ast->capacity) {
        uint16_t new_cap = ast->capacity ? ast->capacity * 2 : initial_cap;
        ASTNode **new_nodes = realloc(ast->nodes, new_cap * sizeof(ASTNode *));
        if (!new_nodes) {
            errhandler__report_error(ERROR_CODE_OPTIM_MEMORY_ALLOCATION,
                                     0, 0, "optimizer",
                                     "realloc failed in add_ast_node_to_list");
            return false;
        }
        ast->nodes = new_nodes;
        ast->capacity = new_cap;
    }
    ast->nodes[ast->count++] = node;
    return true;
}

static ASTNode *clone_ast_node(ASTNode *node, ASTNodePool *pool) {
    if (!node) return NULL;
    ASTNode *clone = POOL_ALLOC(pool);
    if (!clone) return NULL;
    clone->type = node->type;
    clone->operation_type = node->operation_type;
    clone->line = node->line;
    clone->column = node->column;
    clone->value = node->value ? STR_DUP(node->value) : NULL;
    clone->state_modifier  = node->state_modifier ? STR_DUP(node->state_modifier) : NULL;
    clone->access_modifier = node->access_modifier ? STR_DUP(node->access_modifier) : NULL;
    clone->is_const = node->is_const;
    clone->left  = clone_ast_node(node->left, pool);
    clone->right = clone_ast_node(node->right, pool);
    clone->extra = clone_ast_node(node->extra, pool);
    clone->default_value = clone_ast_node(node->default_value, pool);
    clone->variable_type = NULL;
    return clone;
}

static void free_cloned_node(ASTNode *node) {
    if (!node) return;
    free(node->value);
    free(node->state_modifier);
    free(node->access_modifier);
    free_cloned_node(node->left);
    free_cloned_node(node->right);
    free_cloned_node(node->extra);
    free_cloned_node(node->default_value);
    memset(node, 0, sizeof(ASTNode));
}

static bool is_terminator(ASTNode *node) {
    if (!node) return false;
    switch (node->type) {
        case AST_RETURN: case AST_BREAK: case AST_CONTINUE: case AST_JUMP:
            return true;
        default: return false;
    }
}

static bool is_lvalue_parent(ASTNode *parent, ASTNode *child) {
    if (!parent) return false;
    if ((parent->type == AST_ASSIGNMENT || parent->type == AST_COMPOUND_ASSIGNMENT) &&
        parent->left == child) return true;
    if ((parent->type == AST_POSTFIX_INCREMENT || parent->type == AST_POSTFIX_DECREMENT ||
         parent->type == AST_PREFIX_INCREMENT  || parent->type == AST_PREFIX_DECREMENT) &&
        parent->right == child) return true;
    if (parent->type == AST_FIELD_ACCESS && parent->left == child) return true;
    return false;
}

static bool is_pure_expression(ASTNode *node) {
    if (!node) return true;
    switch (node->type) {
        case AST_LITERAL_VALUE: case AST_IDENTIFIER: return true;
        case AST_UNARY_OPERATION: return is_pure_expression(node->right);
        case AST_BINARY_OPERATION: return is_pure_expression(node->left) && is_pure_expression(node->right);
        default: return false;
    }
}

static bool ast_contains_read_of(ASTNode *node, const char *varname) {
    if (!node || !varname) return false;
    if (node->type == AST_IDENTIFIER && node->value && strcmp(node->value, varname) == 0) return true;
    return ast_contains_read_of(node->left, varname) ||
           ast_contains_read_of(node->right, varname) ||
           ast_contains_read_of(node->extra, varname) ||
           ast_contains_read_of(node->default_value, varname);
}

static bool ast_nodes_identical(ASTNode *a, ASTNode *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return false;
    if (a->operation_type != b->operation_type) return false;
    if (a->value && b->value) { if (strcmp(a->value, b->value) != 0) return false; }
    else if (a->value || b->value) return false;
    if (!ast_nodes_identical(a->left, b->left)) return false;
    if (!ast_nodes_identical(a->right, b->right)) return false;
    if (!ast_nodes_identical(a->extra, b->extra)) return false;
    return true;
}

static bool ast_nodes_identical_commutative(ASTNode *a, ASTNode *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return false;
    if (a->operation_type != b->operation_type) return false;
    if (a->value && b->value) { if (strcmp(a->value, b->value) != 0) return false; }
    else if (a->value || b->value) return false;
    if (a->type == AST_BINARY_OPERATION) {
        TokenType op = a->operation_type;
        if ((op == TOKEN_PLUS || op == TOKEN_STAR) &&
            is_pure_expression(a->left) && is_pure_expression(a->right) &&
            is_pure_expression(b->left) && is_pure_expression(b->right)) {
            if (ast_nodes_identical_commutative(a->left, b->left) &&
                ast_nodes_identical_commutative(a->right, b->right)) return true;
            if (ast_nodes_identical_commutative(a->left, b->right) &&
                ast_nodes_identical_commutative(a->right, b->left)) return true;
            return false;
        }
    }
    if (!ast_nodes_identical_commutative(a->left, b->left)) return false;
    if (!ast_nodes_identical_commutative(a->right, b->right)) return false;
    if (!ast_nodes_identical_commutative(a->extra, b->extra)) return false;
    return true;
}

static uint32_t expr_hash_commutative(ASTNode *node) {
    if (!node) return 0;
    uint32_t h = (uint32_t)node->type * 31 + (uint32_t)node->operation_type;
    if (node->value) {
        const char *s = node->value;
        while (*s) h = h * 31 + (unsigned char)*s++;
    }
    if (node->type == AST_BINARY_OPERATION) {
        TokenType op = node->operation_type;
        if ((op == TOKEN_PLUS || op == TOKEN_STAR) &&
            is_pure_expression(node->left) && is_pure_expression(node->right)) {
            uint32_t hl = expr_hash_commutative(node->left);
            uint32_t hr = expr_hash_commutative(node->right);
            if (hl > hr) { uint32_t tmp = hl; hl = hr; hr = tmp; }
            h = h * 31 + hl;
            h = h * 31 + hr;
        } else {
            h ^= expr_hash_commutative(node->left);
            h ^= expr_hash_commutative(node->right);
        }
    } else {
        h ^= expr_hash_commutative(node->left);
        h ^= expr_hash_commutative(node->right);
    }
    h ^= expr_hash_commutative(node->extra);
    return h;
}

static ASTNode *make_literal_zero(ASTNodePool *pool, uint16_t line, uint16_t col) {
    ASTNode *n = POOL_ALLOC(pool);
    if (!n) return NULL;
    n->type = AST_LITERAL_VALUE;
    n->operation_type = TOKEN_NUMBER;
    n->value = STR_DUP("0");
    n->line = line;
    n->column = col;
    return n;
}

static ASTNode *eval_binary_const(ASTNode *node, ASTNodePool *pool) {
    ASTNode *lhs = node->left;
    ASTNode *rhs = node->right;
    if (!lhs || !rhs) return NULL;
    bool lhs_int = (lhs->operation_type == TOKEN_NUMBER && strcspn(lhs->value, ".eE") == strlen(lhs->value));
    bool rhs_int = (rhs->operation_type == TOKEN_NUMBER && strcspn(rhs->value, ".eE") == strlen(rhs->value));
    bool use_int = lhs_int && rhs_int;
    TokenType op = node->operation_type;
    char result_str[128] = {0};
    bool valid = false;
    if (op == TOKEN_PLUS || op == TOKEN_MINUS || op == TOKEN_STAR ||
        op == TOKEN_SLASH || op == TOKEN_PERCENT) {
        if (use_int) {
            intmax_t lhv = strtoimax(lhs->value, NULL, 0);
            intmax_t rhv = strtoimax(rhs->value, NULL, 0);
            intmax_t res = 0;
            if (op == TOKEN_PLUS)      { res = lhv + rhv; valid = true; }
            else if (op == TOKEN_MINUS) { res = lhv - rhv; valid = true; }
            else if (op == TOKEN_STAR)  { res = lhv * rhv; valid = true; }
            else if (op == TOKEN_SLASH) { if (rhv != 0) { res = lhv / rhv; valid = true; } }
            else if (op == TOKEN_PERCENT) { if (rhv != 0) { res = lhv % rhv; valid = true; } }
            if (valid) snprintf(result_str, sizeof(result_str), "%" PRIdMAX, res);
        } else {
            double lhv = strtod(lhs->value, NULL);
            double rhv = strtod(rhs->value, NULL);
            double res = 0;
            if (op == TOKEN_PLUS)      { res = lhv + rhv; valid = true; }
            else if (op == TOKEN_MINUS) { res = lhv - rhv; valid = true; }
            else if (op == TOKEN_STAR)  { res = lhv * rhv; valid = true; }
            else if (op == TOKEN_SLASH) { if (rhv != 0.0) { res = lhv / rhv; valid = true; } }
            if (valid) snprintf(result_str, sizeof(result_str), "%.10g", res);
        }
    } else if (op == TOKEN_LT || op == TOKEN_GT || op == TOKEN_LE || op == TOKEN_GE ||
               op == TOKEN_DOUBLE_EQ || op == TOKEN_NE) {
        if (use_int) {
            intmax_t lhv = strtoimax(lhs->value, NULL, 0);
            intmax_t rhv = strtoimax(rhs->value, NULL, 0);
            int res = 0;
            if (op == TOKEN_LT) res = lhv < rhv;
            else if (op == TOKEN_GT) res = lhv > rhv;
            else if (op == TOKEN_LE) res = lhv <= rhv;
            else if (op == TOKEN_GE) res = lhv >= rhv;
            else if (op == TOKEN_DOUBLE_EQ) res = lhv == rhv;
            else if (op == TOKEN_NE) res = lhv != rhv;
            snprintf(result_str, sizeof(result_str), "%d", res);
            valid = true;
        } else {
            double lhv = strtod(lhs->value, NULL);
            double rhv = strtod(rhs->value, NULL);
            int res = 0;
            if (op == TOKEN_LT) res = lhv < rhv;
            else if (op == TOKEN_GT) res = lhv > rhv;
            else if (op == TOKEN_LE) res = lhv <= rhv;
            else if (op == TOKEN_GE) res = lhv >= rhv;
            else if (op == TOKEN_DOUBLE_EQ) res = lhv == rhv;
            else if (op == TOKEN_NE) res = lhv != rhv;
            snprintf(result_str, sizeof(result_str), "%d", res);
            valid = true;
        }
    } else if (op == TOKEN_LOGICAL && lhs->value && rhs->value) {
        bool lhv_b = (strcmp(lhs->value, "0") != 0);
        bool rhv_b = (strcmp(rhs->value, "0") != 0);
        bool res = (node->value && strcmp(node->value, "or") == 0) ? (lhv_b || rhv_b) : (lhv_b && rhv_b);
        snprintf(result_str, sizeof(result_str), "%d", res);
        valid = true;
    }
    if (!valid) return NULL;
    ASTNode *new_lit = POOL_ALLOC(pool);
    if (!new_lit) return NULL;
    new_lit->type = AST_LITERAL_VALUE;
    new_lit->operation_type = TOKEN_NUMBER;
    new_lit->value = STR_DUP(result_str);
    new_lit->line = node->line;
    new_lit->column = node->column;
    return new_lit;
}

static ASTNode *eval_unary_const(ASTNode *node, ASTNodePool *pool) {
    ASTNode *operand = node->right;
    if (!operand) return NULL;
    char result_str[128] = {0};
    bool valid = false;
    if (node->operation_type == TOKEN_MINUS) {
        if (operand->operation_type == TOKEN_NUMBER) {
            bool is_int = (strcspn(operand->value, ".eE") == strlen(operand->value));
            if (is_int) {
                intmax_t val = strtoimax(operand->value, NULL, 0);
                snprintf(result_str, sizeof(result_str), "%" PRIdMAX, -val);
                valid = true;
            } else {
                double val = strtod(operand->value, NULL);
                snprintf(result_str, sizeof(result_str), "%.10g", -val);
                valid = true;
            }
        }
    } else if (node->operation_type == TOKEN_BANG) {
        bool is_zero = (strcmp(operand->value, "0") == 0);
        snprintf(result_str, sizeof(result_str), "%d", is_zero ? 1 : 0);
        valid = true;
    }
    if (!valid) return NULL;
    ASTNode *new_lit = POOL_ALLOC(pool);
    if (!new_lit) return NULL;
    new_lit->type = AST_LITERAL_VALUE;
    new_lit->operation_type = TOKEN_NUMBER;
    new_lit->value = STR_DUP(result_str);
    new_lit->line = node->line;
    new_lit->column = node->column;
    return new_lit;
}

static ASTNode *eval_constant(ASTNode *node, SymbolTable *global_scope, ASTNodePool *pool) {
    (void)global_scope;
    if (!node) return NULL;
    switch (node->type) {
        case AST_LITERAL_VALUE: return clone_ast_node(node, pool);
        case AST_BINARY_OPERATION: {
            ASTNode *lhs = eval_constant(node->left, global_scope, pool);
            if (!lhs) return NULL;
            ASTNode *rhs = eval_constant(node->right, global_scope, pool);
            if (!rhs) { free_cloned_node(lhs); return NULL; }
            node->left = lhs; node->right = rhs;
            ASTNode *result = eval_binary_const(node, pool);
            node->left = node->right = NULL;
            if (!result) { free_cloned_node(lhs); free_cloned_node(rhs); return NULL; }
            free_cloned_node(lhs); free_cloned_node(rhs);
            return result;
        }
        case AST_UNARY_OPERATION: {
            ASTNode *operand = eval_constant(node->right, global_scope, pool);
            if (!operand) return NULL;
            node->right = operand;
            ASTNode *result = eval_unary_const(node, pool);
            node->right = NULL;
            if (!result) { free_cloned_node(operand); return NULL; }
            free_cloned_node(operand);
            return result;
        }
        case AST_TERNARY_OPERATION: {
            ASTNode *cond = eval_constant(node->left, global_scope, pool);
            if (!cond) return NULL;
            bool cond_true = strcmp(cond->value, "0") != 0;
            free_cloned_node(cond);
            ASTNode *branch = cond_true ? eval_constant(node->right, global_scope, pool)
                                        : eval_constant(node->extra, global_scope, pool);
            return branch;
        }
        default: return NULL;
    }
}

static void convert_literal_to_decimal(char **value_ptr) {
    if (!value_ptr || !*value_ptr) return;
    char *str = *value_ptr;
    size_t len = strlen(str);
    bool is_float = false;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '.' || str[i] == 'e' || str[i] == 'E') { is_float = true; break; }
    }
    char new_str[128];
    if (is_float) {
        char *end = NULL;
        double val = strtod(str, &end);
        if (end != str && *end == '\0') {
            snprintf(new_str, sizeof(new_str), "%.10g", val);
            free(*value_ptr);
            *value_ptr = STR_DUP(new_str);
        }
    } else {
        char *end = NULL;
        intmax_t val = strtoimax(str, &end, 0);
        if (end != str && *end == '\0') {
            snprintf(new_str, sizeof(new_str), "%" PRIdMAX, val);
            free(*value_ptr);
            *value_ptr = STR_DUP(new_str);
        }
    }
}

static void pass_convert_bases(ASTNode *node) {
    if (!node) return;
    if (node->type == AST_LITERAL_VALUE && node->operation_type == TOKEN_NUMBER)
        convert_literal_to_decimal(&node->value);
    pass_convert_bases(node->left);
    pass_convert_bases(node->right);
    pass_convert_bases(node->extra);
    if (node->default_value) pass_convert_bases(node->default_value);
}

static void pass_algebraic_simplify(ASTNode *node, ASTNodePool *pool) {
    if (!node) return;
    pass_algebraic_simplify(node->left, pool);
    pass_algebraic_simplify(node->right, pool);
    pass_algebraic_simplify(node->extra, pool);
    if (node->default_value) pass_algebraic_simplify(node->default_value, pool);
    if (node->type != AST_BINARY_OPERATION) return;
    ASTNode *lhs = node->left;
    ASTNode *rhs = node->right;
    if (!lhs || !rhs) return;
#define IS_LIT(n, v) ((n)->type == AST_LITERAL_VALUE && (n)->operation_type == TOKEN_NUMBER && strcmp((n)->value, (v)) == 0)
    TokenType op = node->operation_type;
    if (op == TOKEN_PLUS) {
        if (IS_LIT(lhs, "0")) { parser__free_ast_node(lhs, pool); *node = *rhs; return; }
        if (IS_LIT(rhs, "0")) { parser__free_ast_node(rhs, pool); *node = *lhs; return; }
    }
    if (op == TOKEN_MINUS) {
        if (IS_LIT(rhs, "0")) { parser__free_ast_node(rhs, pool); *node = *lhs; return; }
    }
    if (op == TOKEN_STAR) {
        if (IS_LIT(lhs, "1")) { parser__free_ast_node(lhs, pool); *node = *rhs; return; }
        if (IS_LIT(rhs, "1")) { parser__free_ast_node(rhs, pool); *node = *lhs; return; }
        if (IS_LIT(lhs, "0") || IS_LIT(rhs, "0")) {
            parser__free_ast_node(lhs, pool); parser__free_ast_node(rhs, pool);
            ASTNode *zero = make_literal_zero(pool, node->line, node->column);
            memcpy(node, zero, sizeof(ASTNode));
            return;
        }
    }
    if (op == TOKEN_SLASH) {
        if (IS_LIT(rhs, "1")) { parser__free_ast_node(rhs, pool); *node = *lhs; return; }
    }
#undef IS_LIT
}

static void pass_fold_constants(ASTNode *node, SymbolTable *global_scope, ASTNodePool *pool) {
    if (!node) return;
    ASTNode *const_val = eval_constant(node, global_scope, pool);
    if (const_val) {
        const_val->line = node->line;
        const_val->column = node->column;
        if (node->left)  parser__free_ast_node(node->left, pool);
        if (node->right) parser__free_ast_node(node->right, pool);
        if (node->extra) parser__free_ast_node(node->extra, pool);
        memcpy(node, const_val, sizeof(ASTNode));
        memset(const_val, 0, sizeof(ASTNode));
        parser__ast_node_pool_free(pool, const_val);
        return;
    }
    pass_fold_constants(node->left, global_scope, pool);
    pass_fold_constants(node->right, global_scope, pool);
    pass_fold_constants(node->extra, global_scope, pool);
    if (node->default_value) pass_fold_constants(node->default_value, global_scope, pool);
}

typedef struct { const char *name; ASTNode *init_expr; } InlineVar;

static bool variable_is_written(ASTNode *node, const char *varname) {
    if (!node || !varname) return false;
    if (node->type == AST_ASSIGNMENT || node->type == AST_COMPOUND_ASSIGNMENT) {
        ASTNode *lhs = node->left;
        if (lhs && lhs->type == AST_IDENTIFIER && lhs->value && strcmp(lhs->value, varname) == 0) return true;
    }
    if (node->type == AST_POSTFIX_INCREMENT || node->type == AST_POSTFIX_DECREMENT ||
        node->type == AST_PREFIX_INCREMENT  || node->type == AST_PREFIX_DECREMENT) {
        ASTNode *operand = node->right;
        if (operand && operand->type == AST_IDENTIFIER && operand->value && strcmp(operand->value, varname) == 0) return true;
    }
    return variable_is_written(node->left, varname) || variable_is_written(node->right, varname) ||
           variable_is_written(node->extra, varname) || variable_is_written(node->default_value, varname);
}

static void collect_inline_vars(ASTNode *node, InlineVar **vars, size_t *count, size_t *cap) {
    if (!node) return;
    if (node->type == AST_VARIABLE_DECLARATION) {
        const char *state = node->state_modifier;
        const char *access = node->access_modifier;
        bool is_inline = (access && strstr(access, "inline")) || (state && strstr(state, "inline"));
        if (state && strcmp(state, "def") == 0 && is_inline && node->default_value) {
            const char *varname = node->value;
            if (varname) {
                if (*count >= *cap) {
                    *cap = *cap ? *cap * 2 : 8;
                    *vars = realloc(*vars, *cap * sizeof(InlineVar));
                    if (!*vars) {
                        errhandler__report_error(ERROR_CODE_OPTIM_MEMORY_ALLOCATION, 0, 0, "optimizer",
                                                 "realloc failed in collect_inline_vars");
                        return;
                    }
                }
                ASTNode *init_clone = clone_ast_node(node->default_value, NULL);
                if (init_clone) {
                    (*vars)[*count].name = varname;
                    (*vars)[*count].init_expr = init_clone;
                    (*count)++;
                }
            }
        }
    }
    collect_inline_vars(node->left, vars, count, cap);
    collect_inline_vars(node->right, vars, count, cap);
    collect_inline_vars(node->extra, vars, count, cap);
    if (node->default_value) collect_inline_vars(node->default_value, vars, count, cap);
}

static void replace_inline_reads(ASTNode *parent, ASTNode **child_ptr, InlineVar *vars, size_t var_count, ASTNodePool *pool) {
    if (!child_ptr || !*child_ptr) return;
    ASTNode *ch = *child_ptr;
    if (ch->type == AST_IDENTIFIER && !is_lvalue_parent(parent, ch)) {
        for (size_t i = 0; i < var_count; i++) {
            if (strcmp(ch->value, vars[i].name) == 0) {
                ASTNode *repl = clone_ast_node(vars[i].init_expr, pool);
                if (repl) {
                    repl->line = ch->line; repl->column = ch->column;
                    parser__ast_node_pool_free(pool, ch);
                    *child_ptr = repl;
                }
                break;
            }
        }
    }
    replace_inline_reads(ch, &ch->left, vars, var_count, pool);
    replace_inline_reads(ch, &ch->right, vars, var_count, pool);
    replace_inline_reads(ch, &ch->extra, vars, var_count, pool);
    if (ch->default_value) replace_inline_reads(ch, &ch->default_value, vars, var_count, pool);
}

static void remove_inline_declarations(ASTNode **node_ptr, InlineVar *vars, size_t var_count, ASTNodePool *pool) {
    if (!node_ptr || !*node_ptr) return;
    ASTNode *node = *node_ptr;
    if (node->type == AST_VARIABLE_DECLARATION) {
        const char *state = node->state_modifier;
        const char *access = node->access_modifier;
        bool is_inline = (access && strstr(access, "inline")) || (state && strstr(state, "inline"));
        if (state && strcmp(state, "def") == 0 && is_inline && node->default_value) {
            const char *varname = node->value;
            if (varname) {
                for (size_t i = 0; i < var_count; i++) {
                    if (strcmp(varname, vars[i].name) == 0) {
                        parser__free_ast_node(node, pool);
                        *node_ptr = NULL;
                        return;
                    }
                }
            }
        }
    }
    remove_inline_declarations(&node->left, vars, var_count, pool);
    remove_inline_declarations(&node->right, vars, var_count, pool);
    remove_inline_declarations(&node->extra, vars, var_count, pool);
}

static void pass_inline_variables(ASTNode *node, ASTNodePool *pool) {
    InlineVar *vars = NULL;
    size_t var_count = 0, var_cap = 0;
    collect_inline_vars(node, &vars, &var_count, &var_cap);
    if (var_count == 0) { free(vars); return; }
    bool safe = true;
    for (size_t i = 0; i < var_count; i++) {
        if (variable_is_written(node, vars[i].name)) {
            errhandler__report_error(ERROR_CODE_OPTIM_INLINE_WRITE, 0, 0, "optimizer",
                                     "Cannot inline variable '%s' because it is modified", vars[i].name);
            safe = false;
        }
    }
    if (!safe) {
        for (size_t i = 0; i < var_count; i++) free_cloned_node(vars[i].init_expr);
        free(vars);
        return;
    }
    replace_inline_reads(NULL, &node, vars, var_count, pool);
    remove_inline_declarations(&node, vars, var_count, pool);
    for (size_t i = 0; i < var_count; i++) free_cloned_node(vars[i].init_expr);
    free(vars);
}

typedef struct { char *dest; ASTNode *source; uint16_t def_idx; } CopyInfo;

static void copy_prop_in_block(ASTNode *block, ASTNodePool *pool) {
    if (!block || block->type != AST_BLOCK || !block->extra) return;
    AST *list = (AST *)block->extra;
    if (!list->nodes || list->count == 0) return;
    CopyInfo *copies = NULL;
    size_t copy_cnt = 0, copy_cap = 0;
    void kill_var(const char *varname) {
        for (size_t i = 0; i < copy_cnt; ) {
            if ((copies[i].dest && strcmp(copies[i].dest, varname) == 0) ||
                (copies[i].source && copies[i].source->type == AST_IDENTIFIER &&
                 strcmp(copies[i].source->value, varname) == 0)) {
                free(copies[i].dest);
                memmove(&copies[i], &copies[i+1], (copy_cnt - i - 1) * sizeof(CopyInfo));
                copy_cnt--;
            } else i++;
        }
    }
    for (uint16_t i = 0; i < list->count; i++) {
        ASTNode *stmt = list->nodes[i];
        if (!stmt) continue;
        if (stmt->type == AST_BLOCK) copy_prop_in_block(stmt, pool);
        if (stmt->type == AST_ASSIGNMENT && stmt->left && stmt->left->type == AST_IDENTIFIER) {
            const char *lhs_name = stmt->left->value;
            kill_var(lhs_name);
            if (stmt->right && stmt->right->type == AST_IDENTIFIER) {
                if (copy_cnt >= copy_cap) {
                    copy_cap = copy_cap ? copy_cap * 2 : 4;
                    copies = realloc(copies, copy_cap * sizeof(CopyInfo));
                    if (!copies) {
                        errhandler__report_error(ERROR_CODE_OPTIM_MEMORY_ALLOCATION, 0, 0, "optimizer",
                                                 "realloc failed in copy_prop_in_block");
                        return;
                    }
                }
                copies[copy_cnt].dest = STR_DUP(lhs_name);
                copies[copy_cnt].source = stmt->right;
                copies[copy_cnt].def_idx = i;
                copy_cnt++;
            }
        } else if (stmt->type == AST_ASSIGNMENT || stmt->type == AST_COMPOUND_ASSIGNMENT) {
            ASTNode *lhs = stmt->left;
            if (lhs && lhs->type == AST_IDENTIFIER) kill_var(lhs->value);
        }
        void replace_in_stmt(ASTNode *parent, ASTNode **child_ptr) {
            if (!child_ptr || !*child_ptr) return;
            ASTNode *ch = *child_ptr;
            if (ch->type == AST_IDENTIFIER && !is_lvalue_parent(parent, ch)) {
                for (size_t c = 0; c < copy_cnt; c++) {
                    if (copies[c].dest && strcmp(ch->value, copies[c].dest) == 0) {
                        ASTNode *repl = clone_ast_node(copies[c].source, pool);
                        if (repl) {
                            repl->line = ch->line; repl->column = ch->column;
                            parser__ast_node_pool_free(pool, ch);
                            *child_ptr = repl;
                        }
                        break;
                    }
                }
            }
            replace_in_stmt(ch, &ch->left);
            replace_in_stmt(ch, &ch->right);
            replace_in_stmt(ch, &ch->extra);
            if (ch->default_value) replace_in_stmt(ch, &ch->default_value);
        }
        replace_in_stmt(NULL, &stmt);
    }
    for (size_t i = 0; i < copy_cnt; i++) free(copies[i].dest);
    free(copies);
}

static void pass_copy_propagation(ASTNode *node, SymbolTable *global_scope, ASTNodePool *pool) {
    (void)global_scope;
    if (!node) return;
    if (node->type == AST_BLOCK) copy_prop_in_block(node, pool);
    pass_copy_propagation(node->left, global_scope, pool);
    pass_copy_propagation(node->right, global_scope, pool);
    pass_copy_propagation(node->extra, global_scope, pool);
}

typedef struct { char *name; uint16_t def_idx; bool may_have_read; } DefInfo;

static void dead_store_elim_in_block(ASTNode *block, ASTNodePool *pool) {
    if (!block || block->type != AST_BLOCK || !block->extra) return;
    AST *list = (AST *)block->extra;
    if (!list->nodes || list->count == 0) return;
    DefInfo *defs = NULL;
    size_t def_cnt = 0, def_cap = 0;
    for (uint16_t i = 0; i < list->count; i++) {
        ASTNode *stmt = list->nodes[i];
        if (!stmt) continue;
        if (stmt->type == AST_BLOCK) dead_store_elim_in_block(stmt, pool);
        if ((stmt->type == AST_ASSIGNMENT || stmt->type == AST_COMPOUND_ASSIGNMENT) &&
            stmt->left && stmt->left->type == AST_IDENTIFIER) {
            const char *varname = stmt->left->value;
            bool eliminated = false;
            for (size_t d = 0; d < def_cnt; d++) {
                if (strcmp(defs[d].name, varname) == 0) {
                    if (!defs[d].may_have_read) {
                        ASTNode *prev = list->nodes[defs[d].def_idx];
                        parser__free_ast_node(prev, pool);
                        list->nodes[defs[d].def_idx] = NULL;
                        eliminated = true;
                    }
                    free(defs[d].name);
                    memmove(&defs[d], &defs[d+1], (def_cnt - d - 1) * sizeof(DefInfo));
                    def_cnt--;
                    break;
                }
            }
            if (def_cnt >= def_cap) {
                def_cap = def_cap ? def_cap * 2 : 4;
                defs = realloc(defs, def_cap * sizeof(DefInfo));
                if (!defs) {
                    errhandler__report_error(ERROR_CODE_OPTIM_MEMORY_ALLOCATION, 0, 0, "optimizer",
                                             "realloc failed in dead_store_elim_in_block");
                    return;
                }
            }
            defs[def_cnt].name = STR_DUP(varname);
            defs[def_cnt].def_idx = i;
            defs[def_cnt].may_have_read = false;
            def_cnt++;
        } else {
            for (size_t d = 0; d < def_cnt; d++) {
                if (ast_contains_read_of(stmt, defs[d].name)) defs[d].may_have_read = true;
            }
        }
    }
    uint16_t write = 0;
    for (uint16_t i = 0; i < list->count; i++) if (list->nodes[i] != NULL) list->nodes[write++] = list->nodes[i];
    list->count = write;
    for (size_t i = 0; i < def_cnt; i++) free(defs[i].name);
    free(defs);
}

static void pass_dead_store_elimination(ASTNode *node, ASTNodePool *pool) {
    if (!node) return;
    if (node->type == AST_BLOCK) dead_store_elim_in_block(node, pool);
    pass_dead_store_elimination(node->left, pool);
    pass_dead_store_elimination(node->right, pool);
    pass_dead_store_elimination(node->extra, pool);
}

static void merge_identical_in_block(ASTNode *block) {
    if (!block || block->type != AST_BLOCK || !block->extra) return;
    AST *list = (AST *)block->extra;
    for (uint16_t i = 0; i + 1 < list->count; i++) {
        ASTNode *s1 = list->nodes[i];
        ASTNode *s2 = list->nodes[i + 1];
        if (s1 && s2 && ast_nodes_identical(s1, s2)) {
            for (uint16_t j = i + 1; j < list->count - 1; j++) list->nodes[j] = list->nodes[j + 1];
            list->count--;
            i--;
        }
    }
}

static void pass_merge_identical_statements(ASTNode *node) {
    if (!node) return;
    if (node->type == AST_BLOCK) merge_identical_in_block(node);
    pass_merge_identical_statements(node->left);
    pass_merge_identical_statements(node->right);
    pass_merge_identical_statements(node->extra);
}

typedef struct CSEEntry { uint32_t hash; ASTNode *expr; struct CSEEntry *next; } CSEEntry;
#define CSE_HASH_SIZE 64

static void cse_process_block(ASTNode *block, ASTNodePool *pool) {
    if (!block || block->type != AST_BLOCK || !block->extra) return;
    AST *list = (AST *)block->extra;
    CSEEntry *table[CSE_HASH_SIZE] = {0};
    for (uint16_t i = 0; i < list->count; i++) {
        ASTNode *stmt = list->nodes[i];
        if (!stmt) continue;
        if (stmt->type == AST_BLOCK) cse_process_block(stmt, pool);
        ASTNode *expr = NULL;
        if (stmt->type == AST_ASSIGNMENT || stmt->type == AST_COMPOUND_ASSIGNMENT) expr = stmt->right;
        if (!expr || expr->type == AST_LITERAL_VALUE || expr->type == AST_IDENTIFIER) continue;
        uint32_t h = expr_hash_commutative(expr);
        uint32_t idx = h % CSE_HASH_SIZE;
        CSEEntry *entry = table[idx];
        bool found = false;
        while (entry) {
            if (entry->hash == h && ast_nodes_identical_commutative(entry->expr, expr)) {
                parser__free_ast_node(expr, pool);
                ASTNode *copy = clone_ast_node(entry->expr, pool);
                if (stmt->type == AST_ASSIGNMENT || stmt->type == AST_COMPOUND_ASSIGNMENT) stmt->right = copy;
                found = true;
                break;
            }
            entry = entry->next;
        }
        if (!found) {
            CSEEntry *new_entry = malloc(sizeof(CSEEntry));
            if (!new_entry) {
                errhandler__report_error(ERROR_CODE_OPTIM_MEMORY_ALLOCATION, 0, 0, "optimizer",
                                         "malloc failed for CSE entry");
                continue;
            }
            new_entry->hash = h;
            new_entry->expr = expr;
            new_entry->next = table[idx];
            table[idx] = new_entry;
        }
    }
    for (int i = 0; i < CSE_HASH_SIZE; i++) {
        CSEEntry *e = table[i];
        while (e) { CSEEntry *next = e->next; free(e); e = next; }
    }
}

static void pass_cse(ASTNode *node, ASTNodePool *pool) {
    if (!node) return;
    if (node->type == AST_BLOCK) cse_process_block(node, pool);
    pass_cse(node->left, pool);
    pass_cse(node->right, pool);
    pass_cse(node->extra, pool);
}

static void simplify_conditional(ASTNode **node_ptr, SymbolTable *global_scope, ASTNodePool *pool) {
    if (!node_ptr || !*node_ptr) return;
    ASTNode *node = *node_ptr;
    if (node->type != AST_IF_STATEMENT) return;
    ASTNode *cond_const = eval_constant(node->left, global_scope, pool);
    if (cond_const) {
        bool cond_true = (strcmp(cond_const->value, "0") != 0);
        free_cloned_node(cond_const);
        ASTNode *chosen = cond_true ? node->right : node->extra;
        parser__free_ast_node(node->left, pool);
        if (node->right) parser__free_ast_node(node->right, pool);
        if (node->extra) parser__free_ast_node(node->extra, pool);
        if (chosen) { *node_ptr = chosen; } else {
            ASTNode *empty = POOL_ALLOC(pool);
            empty->type = AST_BLOCK;
            *node_ptr = empty;
        }
        return;
    }
    if (node->right && node->extra && ast_nodes_identical(node->right, node->extra)) {
        ASTNode *branch = node->right;
        parser__free_ast_node(node->left, pool);
        if (node->extra) parser__free_ast_node(node->extra, pool);
        *node_ptr = branch;
        return;
    }
    simplify_conditional(&node->right, global_scope, pool);
    if (node->extra) simplify_conditional(&node->extra, global_scope, pool);
}

static void pass_simplify_conditionals(ASTNode *node, SymbolTable *global_scope, ASTNodePool *pool) {
    if (!node) return;
    if (node->type == AST_BLOCK && node->extra) {
        AST *list = (AST *)node->extra;
        for (uint16_t i = 0; i < list->count; i++) {
            if (list->nodes[i]) simplify_conditional(&list->nodes[i], global_scope, pool);
        }
    }
    pass_simplify_conditionals(node->left, global_scope, pool);
    pass_simplify_conditionals(node->right, global_scope, pool);
    pass_simplify_conditionals(node->extra, global_scope, pool);
}

static bool function_is_recursive(const char *func_name, ASTNode *body) {
    if (!func_name || !body) return false;
    if (body->type == AST_FUNCTION_CALL && body->left && body->left->type == AST_IDENTIFIER &&
        strcmp(body->left->value, func_name) == 0) return true;
    return function_is_recursive(func_name, body->left) ||
           function_is_recursive(func_name, body->right) ||
           function_is_recursive(func_name, body->extra);
}

static ASTNode *clone_body_with_args(ASTNode *body, ASTNodePool *pool, char **param_names, size_t param_count,
                                     ASTNode **args, size_t arg_count) {
    if (!body) return NULL;
    if (body->type == AST_BLOCK && body->extra) {
        AST *list = (AST *)body->extra;
        AST *new_list = calloc(1, sizeof(AST));
        if (!new_list) {
            errhandler__report_error(ERROR_CODE_OPTIM_MEMORY_ALLOCATION, 0, 0, "optimizer",
                                     "calloc failed for new AST list in function inlining");
            return NULL;
        }
        for (uint16_t i = 0; i < list->count; i++) {
            ASTNode *stmt = clone_body_with_args(list->nodes[i], pool, param_names, param_count, args, arg_count);
            if (stmt && !add_ast_node_to_list(new_list, stmt)) {
                parser__free_ast_node(stmt, pool);
                free(new_list->nodes); free(new_list);
                return NULL;
            }
        }
        ASTNode *block = POOL_ALLOC(pool);
        block->type = AST_BLOCK;
        block->extra = (ASTNode *)new_list;
        return block;
    }
    if (body->type == AST_IDENTIFIER) {
        for (size_t i = 0; i < param_count && i < arg_count; i++) {
            if (strcmp(body->value, param_names[i]) == 0) return clone_ast_node(args[i], pool);
        }
    }
    ASTNode *clone = POOL_ALLOC(pool);
    memcpy(clone, body, sizeof(ASTNode));
    clone->left  = clone_body_with_args(body->left, pool, param_names, param_count, args, arg_count);
    clone->right = clone_body_with_args(body->right, pool, param_names, param_count, args, arg_count);
    clone->extra = clone_body_with_args(body->extra, pool, param_names, param_count, args, arg_count);
    return clone;
}

static void pass_inline_functions(ASTNode *node, SymbolTable *global_scope, ASTNodePool *pool) {
    if (!node) return;
    typedef struct { char *name; ASTNode *body; char **param_names; size_t param_count; bool is_inline; } InlineFunc;
    InlineFunc *ilist = NULL;
    size_t icnt = 0, icap = 0;
    void collect(ASTNode *n) {
        if (!n) return;
        if (n->type == AST_FUNCTION_DECLARATION) {
            const char *amod = n->access_modifier;
            bool is_inline = (amod && strstr(amod, "inline"));
            if (is_inline && n->right) {
                ASTNode *body = n->right;
                if (body->type != AST_BLOCK) return;
                char **names = NULL;
                size_t pcount = 0;
                if (n->left && n->left->type == AST_BLOCK && n->left->extra) {
                    AST *plist = (AST *)n->left->extra;
                    pcount = plist->count;
                    names = calloc(pcount, sizeof(char *));
                    if (!names) return;
                    for (uint16_t i = 0; i < plist->count; i++) {
                        if (plist->nodes[i]->type == AST_VARIABLE_DECLARATION) names[i] = STR_DUP(plist->nodes[i]->value);
                    }
                }
                if (icnt >= icap) {
                    icap = icap ? icap * 2 : 4;
                    ilist = realloc(ilist, icap * sizeof(InlineFunc));
                    if (!ilist) return;
                }
                ilist[icnt].name = STR_DUP(n->value);
                ilist[icnt].body = body;
                ilist[icnt].param_names = names;
                ilist[icnt].param_count = pcount;
                ilist[icnt].is_inline = !function_is_recursive(n->value, body);
                icnt++;
            }
        }
        collect(n->left); collect(n->right); collect(n->extra);
    }
    collect(node);
    void replace_calls(ASTNode **node_ptr) {
        if (!node_ptr || !*node_ptr) return;
        ASTNode *n = *node_ptr;
        if (n->type == AST_FUNCTION_CALL) {
            ASTNode *fname = n->left;
            if (fname && fname->type == AST_IDENTIFIER) {
                for (size_t i = 0; i < icnt; i++) {
                    if (!ilist[i].is_inline) continue;
                    if (strcmp(fname->value, ilist[i].name) == 0) {
                        AST *arg_list = n->right ? (AST *)n->right : NULL;
                        ASTNode **args = NULL;
                        size_t argc = 0;
                        if (arg_list) { args = arg_list->nodes; argc = arg_list->count; }
                        ASTNode *new_body = clone_body_with_args(ilist[i].body, pool, ilist[i].param_names,
                                                                 ilist[i].param_count, args, argc);
                        if (new_body) {
                            parser__free_ast_node(*node_ptr, pool);
                            *node_ptr = new_body;
                        }
                        break;
                    }
                }
            }
        }
        if (*node_ptr) {
            replace_calls(&(*node_ptr)->left);
            replace_calls(&(*node_ptr)->right);
            replace_calls(&(*node_ptr)->extra);
        }
    }
    replace_calls(&node);
    for (size_t i = 0; i < icnt; i++) {
        free(ilist[i].name);
        for (size_t j = 0; j < ilist[i].param_count; j++) free(ilist[i].param_names[j]);
        free(ilist[i].param_names);
    }
    free(ilist);
}

static int count_loop_iterations(ASTNode *loop, SymbolTable *global_scope) {
    if (!loop || loop->type != AST_DO_LOOP) return 0;
    ASTNode *cond = loop->left;
    if (!cond) return 0;
    ASTNode *const_cond = eval_constant(cond, global_scope, NULL);
    if (!const_cond) return 0;
    bool cond_true = (strcmp(const_cond->value, "0") != 0);
    free_cloned_node(const_cond);
    return cond_true ? 0 : 1;
}

static bool are_loops_adjacent_and_compatible(ASTNode *loop1, ASTNode *loop2) {
    if (!loop1 || !loop2) return false;
    if (loop1->type != AST_DO_LOOP || loop2->type != AST_DO_LOOP) return false;
    return ast_nodes_identical(loop1->left, loop2->left) &&
           is_pure_expression(loop1->left) && is_pure_expression(loop2->left);
}

static void fuse_loops(ASTNode *loop1, ASTNode *loop2, ASTNodePool *pool) {
    ASTNode *body1 = loop1->right;
    ASTNode *body2 = loop2->right;
    if (!body1 || !body2) return;
    if (body1->type != AST_BLOCK || body2->type != AST_BLOCK) return;
    AST *list1 = (AST *)body1->extra;
    AST *list2 = (AST *)body2->extra;
    if (!list1 || !list2) return;
    for (uint16_t i = 0; i < list2->count; i++) {
        ASTNode *stmt = clone_ast_node(list2->nodes[i], pool);
        if (stmt) add_ast_node_to_list(list1, stmt);
    }
    parser__free_ast_node(loop2, pool);
}

static void pass_loop_optimizations(ASTNode *node, SymbolTable *global_scope, ASTNodePool *pool) {
    if (!node) return;
    if (node->type == AST_BLOCK && node->extra) {
        AST *list = (AST *)node->extra;
        for (uint16_t i = 0; i < list->count; i++) {
            ASTNode *stmt = list->nodes[i];
            if (!stmt) continue;
            if (stmt->type == AST_DO_LOOP) {
                int iters = count_loop_iterations(stmt, global_scope);
                if (iters > 0 && iters <= 8) {
                    ASTNode *body = stmt->right;
                    if (body && body->type == AST_BLOCK) {
                        for (int k = 1; k < iters; k++) {
                            ASTNode *copy = clone_ast_node(body, pool);
                            if (copy) add_ast_node_to_list((AST *)body->extra, copy);
                        }
                    }
                    parser__free_ast_node(stmt->left, pool);
                    ASTNode *unrolled = body;
                    parser__free_ast_node(stmt, pool);
                    list->nodes[i] = unrolled;
                }
                if (i + 1 < list->count) {
                    ASTNode *next = list->nodes[i + 1];
                    if (next && are_loops_adjacent_and_compatible(stmt, next)) {
                        fuse_loops(stmt, next, pool);
                        for (uint16_t j = i + 1; j < list->count - 1; j++) list->nodes[j] = list->nodes[j + 1];
                        list->count--;
                    }
                }
            }
        }
    }
    pass_loop_optimizations(node->left, global_scope, pool);
    pass_loop_optimizations(node->right, global_scope, pool);
    pass_loop_optimizations(node->extra, global_scope, pool);
}

static void eliminate_dead_in_block(ASTNode *block, ASTNodePool *pool) {
    if (!block || block->type != AST_BLOCK || !block->extra) return;
    AST *list = (AST *)block->extra;
    uint16_t i = 0;
    while (i < list->count) {
        ASTNode *stmt = list->nodes[i];
        if (!stmt) { i++; continue; }
        if (stmt->type == AST_BLOCK) eliminate_dead_in_block(stmt, pool);
        if (stmt->type == AST_IF_STATEMENT) {
            if (stmt->right) eliminate_dead_in_block(stmt->right, pool);
            if (stmt->extra) eliminate_dead_in_block(stmt->extra, pool);
        }
        if (stmt->type == AST_DO_LOOP) {
            if (stmt->right) eliminate_dead_in_block(stmt->right, pool);
            if (stmt->extra) eliminate_dead_in_block(stmt->extra, pool);
        }
        if (is_terminator(stmt)) {
            for (uint16_t j = i + 1; j < list->count; j++) {
                if (list->nodes[j]) parser__free_ast_node(list->nodes[j], pool);
                list->nodes[j] = NULL;
            }
            list->count = i + 1;
            break;
        }
        i++;
    }
}

static void pass_eliminate_dead_code(ASTNode *node, ASTNodePool *pool) {
    if (!node) return;
    if (node->type == AST_BLOCK) eliminate_dead_in_block(node, pool);
    pass_eliminate_dead_code(node->left, pool);
    pass_eliminate_dead_code(node->right, pool);
    pass_eliminate_dead_code(node->extra, pool);
}

bool optimizer__optimize(AST *ast, SymbolTable *global_scope) {
    if (!ast || !global_scope) {
        errhandler__report_error(ERROR_CODE_OPTIM_INTERNAL_ERROR, 0, 0, "optimizer",
                                 "NULL AST or symbol table passed to optimizer");
        return false;
    }
    ASTNodePool *pool = ast->pool;
    if (!pool) {
        errhandler__report_error(ERROR_CODE_OPTIM_INTERNAL_ERROR, 0, 0, "optimizer",
                                 "AST node pool is NULL");
        return false;
    }
    optimizer_debug_print_ast("BEFORE OPTIMIZATION", ast);
    for (uint16_t i = 0; i < ast->count; i++) {
        ASTNode *stmt = ast->nodes[i];
        if (!stmt) continue;
        pass_convert_bases(stmt);
        optimizer_debug_print_ast("After pass_convert_bases", ast);
        pass_algebraic_simplify(stmt, pool);
        optimizer_debug_print_ast("After pass_algebraic_simplify", ast);
        pass_fold_constants(stmt, global_scope, pool);
        optimizer_debug_print_ast("After pass_fold_constants", ast);
        pass_inline_variables(stmt, pool);
        optimizer_debug_print_ast("After pass_inline_variables", ast);
        pass_copy_propagation(stmt, global_scope, pool);
        optimizer_debug_print_ast("After pass_copy_propagation", ast);
        pass_dead_store_elimination(stmt, pool);
        optimizer_debug_print_ast("After pass_dead_store_elimination", ast);
        pass_merge_identical_statements(stmt);
        optimizer_debug_print_ast("After pass_merge_identical_statements", ast);
        pass_cse(stmt, pool);
        optimizer_debug_print_ast("After pass_cse", ast);
        pass_simplify_conditionals(stmt, global_scope, pool);
        optimizer_debug_print_ast("After pass_simplify_conditionals", ast);
        pass_inline_functions(stmt, global_scope, pool);
        optimizer_debug_print_ast("After pass_inline_functions", ast);
        pass_loop_optimizations(stmt, global_scope, pool);
        optimizer_debug_print_ast("After pass_loop_optimizations", ast);
        pass_eliminate_dead_code(stmt, pool);
        optimizer_debug_print_ast("After pass_eliminate_dead_code", ast);
        if (errhandler__has_errors()) return false;
    }
    optimizer_debug_print_ast("AFTER OPTIMIZATION", ast);
    return true;
}
