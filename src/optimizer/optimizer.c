#include "optimizer.h"
#include "../utils/common.h"
#include "../parser/parser.h"
#include "../semantic/semantic.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

/* Forward declarations for all static helper and pass functions. */
static bool add_ast_node_to_list(AST *ast, ASTNode *node);
static void free_cloned_node(ASTNode *node);
static ASTNode *clone_ast_node(ASTNode *node, ASTNodePool *pool);
static bool ast_nodes_identical(ASTNode *a, ASTNode *b);

static void pass_convert_bases(ASTNode *node);
static void pass_algebraic_simplify(ASTNode *node, ASTNodePool *pool);
static void pass_fold_constants(ASTNode *node, SymbolTable *global_scope,
                                ASTNodePool *pool);
static void pass_propagate_fixed(ASTNode *node, SymbolTable *global_scope,
                                 ASTNodePool *pool);
static void pass_copy_propagation(ASTNode *node, SymbolTable *global_scope,
                                  ASTNodePool *pool);
static void pass_dead_store_elimination(ASTNode *node);
static void pass_merge_identical_statements(ASTNode *node);
static void pass_cse(ASTNode *node, ASTNodePool *pool);
static void pass_simplify_conditionals(ASTNode *node,
                                       SymbolTable *global_scope,
                                       ASTNodePool *pool);
static void pass_inline_functions(ASTNode *node, SymbolTable *global_scope,
                                  ASTNodePool *pool);
static void pass_loop_optimizations(ASTNode *node, SymbolTable *global_scope,
                                    ASTNodePool *pool);
static void pass_eliminate_dead_code(ASTNode *node);

static ASTNode *eval_constant(ASTNode *node, SymbolTable *global_scope,
                              ASTNodePool *pool);
static bool is_terminator(ASTNode *node);
static bool is_lvalue_parent(ASTNode *parent, ASTNode *child);
static SymbolEntry *lookup_global_symbol(SymbolTable *global_scope,
                                         const char *name);

static ASTNode *make_literal_zero(ASTNodePool *pool, uint16_t line,
                                  uint16_t col);
static int count_loop_iterations(ASTNode *loop, SymbolTable *global_scope);
static bool are_loops_adjacent_and_compatible(ASTNode *loop1, ASTNode *loop2);
static void fuse_loops(ASTNode *loop1, ASTNode *loop2, ASTNodePool *pool);

/*
 * Append a node to a dynamic AST list (local copy of parser helper).
 */
static bool add_ast_node_to_list(AST *ast, ASTNode *node) {
    if (!ast || !node) return false;
    const uint16_t initial_cap = 4;
    if (ast->count >= ast->capacity) {
        uint16_t new_cap = ast->capacity ? ast->capacity * 2 : initial_cap;
        ASTNode **new_nodes = realloc(ast->nodes, new_cap * sizeof(ASTNode*));
        if (!new_nodes) return false;
        ast->nodes = new_nodes;
        ast->capacity = new_cap;
    }
    ast->nodes[ast->count++] = node;
    return true;
}

/*
 * Look up a symbol in the global scope only.
 */
static SymbolEntry *lookup_global_symbol(SymbolTable *global_scope,
                                         const char *name) {
    if (!global_scope || !name) return NULL;
    /* FNV‑1a hash (same as in semantic.c) */
    uint32_t hash = 5381;
    const char *s = name;
    int c;
    while ((c = *s++)) hash = ((hash << 5) + hash) + c;
    size_t idx = hash % global_scope->capacity;
    SymbolEntry *e = global_scope->entries[idx];
    while (e) {
        if (strcmp(e->name, name) == 0) return e;
        e = e->next;
    }
    return NULL;
}

/*
 * Convert a single numeric literal to its decimal representation.
 * Handles hexadecimal, octal, and binary prefixes.
 */
static void convert_literal_to_decimal(char **value_ptr) {
    if (!value_ptr || !*value_ptr) return;
    char *str = *value_ptr;
    size_t len = strlen(str);

    /* Quick check: if it already looks purely decimal, skip. */
    bool is_float = false;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '.' || str[i] == 'e' || str[i] == 'E') {
            is_float = true;
            break;
        }
    }

    char new_str[128];
    if (is_float) {
        char *end = NULL;
        double val = strtod(str, &end);
        if (end != str && *end == '\0') {
            snprintf(new_str, sizeof(new_str), "%.10g", val);
            free(*value_ptr);
            *value_ptr = u__strduplic(new_str);
        }
    } else {
        char *end = NULL;
        intmax_t val = strtoimax(str, &end, 0); /* base 0 auto‑detects */
        if (end != str && *end == '\0') {
            snprintf(new_str, sizeof(new_str), "%" PRIdMAX, val);
            free(*value_ptr);
            *value_ptr = u__strduplic(new_str);
        }
    }
}

/*
 * Deep‑clone an AST subtree, allocating nodes from the given pool.
 * The node itself comes from the pool; its string fields are duplicated
 * with normal malloc and will be freed by `free_cloned_node`.
 */
static ASTNode *clone_ast_node(ASTNode *node, ASTNodePool *pool) {
    if (!node) return NULL;
    ASTNode *clone = parser__ast_node_pool_alloc(pool);
    if (!clone) return NULL;
    memset(clone, 0, sizeof(ASTNode));
    clone->type = node->type;
    clone->operation_type = node->operation_type;
    clone->line = node->line;
    clone->column = node->column;
    clone->value = node->value ? u__strduplic(node->value) : NULL;
    clone->state_modifier  = node->state_modifier
                             ? u__strduplic(node->state_modifier) : NULL;
    clone->access_modifier = node->access_modifier
                             ? u__strduplic(node->access_modifier) : NULL;
    clone->is_const = node->is_const;
    clone->left  = clone_ast_node(node->left,  pool);
    clone->right = clone_ast_node(node->right, pool);
    clone->extra = clone_ast_node(node->extra, pool);
    clone->default_value = clone_ast_node(node->default_value, pool);
    clone->variable_type = NULL; /* type descriptors are not deep‑copied */
    return clone;
}

/*
 * Free all dynamically‑allocated parts of a cloned node.
 * The node itself stays in the AST pool.
 */
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

/*
 * Return true if a statement is an unconditional terminator (return,
 * break, continue, halt, kill, jump).
 */
static bool is_terminator(ASTNode *node) {
    if (!node) return false;
    switch (node->type) {
        case AST_RETURN:
        case AST_BREAK:
        case AST_CONTINUE:
        case AST_JUMP:
            return true;
        default:
            return false;
    }
}

/*
 * Determine whether an identifier appears as an lvalue (is written to) in
 * its parent expression.
 */
static bool is_lvalue_parent(ASTNode *parent, ASTNode *child) {
    if (!parent) return false;
    if ((parent->type == AST_ASSIGNMENT ||
         parent->type == AST_COMPOUND_ASSIGNMENT) &&
        parent->left == child) return true;
    if ((parent->type == AST_POSTFIX_INCREMENT ||
         parent->type == AST_POSTFIX_DECREMENT ||
         parent->type == AST_PREFIX_INCREMENT  ||
         parent->type == AST_PREFIX_DECREMENT) &&
        parent->right == child) return true;
    if (parent->type == AST_FIELD_ACCESS && parent->left == child) return true;
    return false;
}

/*
 * Lightweight structural equality comparison of two AST subtrees.
 * Returns true if the two nodes are syntactically identical (type,
 * operation, value strings, and recursively children).
 */
static bool ast_nodes_identical(ASTNode *a, ASTNode *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return false;
    if (a->operation_type != b->operation_type) return false;
    if (a->value && b->value) {
        if (strcmp(a->value, b->value) != 0) return false;
    } else if (a->value || b->value) return false;
    if (!ast_nodes_identical(a->left, b->left)) return false;
    if (!ast_nodes_identical(a->right, b->right)) return false;
    if (!ast_nodes_identical(a->extra, b->extra)) return false;
    return true;
}

/*
 * Return a newly allocated literal node representing zero.
 */
static ASTNode *make_literal_zero(ASTNodePool *pool, uint16_t line,
                                  uint16_t col) {
    ASTNode *n = parser__ast_node_pool_alloc(pool);
    if (!n) return NULL;
    memset(n, 0, sizeof(ASTNode));
    n->type = AST_LITERAL_VALUE;
    n->operation_type = TOKEN_NUMBER;
    n->value = u__strduplic("0");
    n->line = line;
    n->column = col;
    return n;
}

/*
 * Evaluate a binary operation on two constant literal operands.
 * Returns a new literal node from the pool, or NULL if the operation
 * cannot be evaluated.
 */
static ASTNode *eval_binary_const(ASTNode *node, ASTNodePool *pool) {
    ASTNode *lhs = node->left;
    ASTNode *rhs = node->right;
    if (!lhs || !rhs) return NULL;

    bool lhs_int = (lhs->operation_type == TOKEN_NUMBER &&
                    strcspn(lhs->value, ".eE") == strlen(lhs->value));
    bool rhs_int = (rhs->operation_type == TOKEN_NUMBER &&
                    strcspn(rhs->value, ".eE") == strlen(rhs->value));
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
            else if (op == TOKEN_SLASH) {
                if (rhv != 0) { res = lhv / rhv; valid = true; }
            } else if (op == TOKEN_PERCENT) {
                if (rhv != 0) { res = lhv % rhv; valid = true; }
            }
            if (valid) snprintf(result_str, sizeof(result_str),
                                "%" PRIdMAX, res);
        } else {
            double lhv = strtod(lhs->value, NULL);
            double rhv = strtod(rhs->value, NULL);
            double res = 0;
            if (op == TOKEN_PLUS)      { res = lhv + rhv; valid = true; }
            else if (op == TOKEN_MINUS) { res = lhv - rhv; valid = true; }
            else if (op == TOKEN_STAR)  { res = lhv * rhv; valid = true; }
            else if (op == TOKEN_SLASH) {
                if (rhv != 0.0) { res = lhv / rhv; valid = true; }
            }
            if (valid) snprintf(result_str, sizeof(result_str),
                                "%.10g", res);
        }
    } else if (op == TOKEN_LT || op == TOKEN_GT ||
               op == TOKEN_LE || op == TOKEN_GE ||
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
        bool lhv_b = strcmp(lhs->value, "0") != 0;
        bool rhv_b = strcmp(rhs->value, "0") != 0;
        bool res;
        if (node->value && strcmp(node->value, "or") == 0)
            res = lhv_b || rhv_b;
        else
            res = lhv_b && rhv_b; /* 'and' */
        snprintf(result_str, sizeof(result_str), "%d", res);
        valid = true;
    }

    if (!valid) return NULL;

    ASTNode *new_lit = parser__ast_node_pool_alloc(pool);
    if (!new_lit) return NULL;
    memset(new_lit, 0, sizeof(ASTNode));
    new_lit->type = AST_LITERAL_VALUE;
    new_lit->operation_type = TOKEN_NUMBER;
    new_lit->value = u__strduplic(result_str);
    new_lit->line = node->line;
    new_lit->column = node->column;
    return new_lit;
}

/*
 * Evaluate a unary constant expression (minus, logical not).
 */
static ASTNode *eval_unary_const(ASTNode *node, ASTNodePool *pool) {
    ASTNode *operand = node->right;
    if (!operand) return NULL;

    char result_str[128] = {0};
    bool valid = false;

    if (node->operation_type == TOKEN_MINUS) {
        if (operand->operation_type == TOKEN_NUMBER) {
            bool is_int = (strcspn(operand->value, ".eE") ==
                           strlen(operand->value));
            if (is_int) {
                intmax_t val = strtoimax(operand->value, NULL, 0);
                snprintf(result_str, sizeof(result_str),
                         "%" PRIdMAX, -val);
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

    ASTNode *new_lit = parser__ast_node_pool_alloc(pool);
    if (!new_lit) return NULL;
    memset(new_lit, 0, sizeof(ASTNode));
    new_lit->type = AST_LITERAL_VALUE;
    new_lit->operation_type = TOKEN_NUMBER;
    new_lit->value = u__strduplic(result_str);
    new_lit->line = node->line;
    new_lit->column = node->column;
    return new_lit;
}

/*
 * Recursively evaluate a constant expression.
 * Returns NULL if any subexpression is not compile‑time constant.
 */
static ASTNode *eval_constant(ASTNode *node, SymbolTable *global_scope,
                              ASTNodePool *pool) {
    if (!node) return NULL;
    switch (node->type) {
        case AST_LITERAL_VALUE:
            return clone_ast_node(node, pool);

        case AST_BINARY_OPERATION: {
            ASTNode *lhs = eval_constant(node->left, global_scope, pool);
            if (!lhs) return NULL;
            ASTNode *rhs = eval_constant(node->right, global_scope, pool);
            if (!rhs) { free_cloned_node(lhs); return NULL; }
            node->left = lhs;
            node->right = rhs;
            ASTNode *result = eval_binary_const(node, pool);
            node->left = node->right = NULL;
            if (!result) { free_cloned_node(lhs); free_cloned_node(rhs); return NULL; }
            free_cloned_node(lhs);
            free_cloned_node(rhs);
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
            ASTNode *branch = cond_true
                              ? eval_constant(node->right, global_scope, pool)
                              : eval_constant(node->extra, global_scope, pool);
            return branch;
        }

        default:
            return NULL;
    }
}

static void pass_convert_bases(ASTNode *node) {
    if (!node) return;
    if (node->type == AST_LITERAL_VALUE &&
        node->operation_type == TOKEN_NUMBER) {
        convert_literal_to_decimal(&node->value);
    }
    pass_convert_bases(node->left);
    pass_convert_bases(node->right);
    pass_convert_bases(node->extra);
    if (node->default_value)
        pass_convert_bases(node->default_value);
}

static void pass_algebraic_simplify(ASTNode *node, ASTNodePool *pool) {
    if (!node) return;

    pass_algebraic_simplify(node->left, pool);
    pass_algebraic_simplify(node->right, pool);
    pass_algebraic_simplify(node->extra, pool);
    if (node->default_value)
        pass_algebraic_simplify(node->default_value, pool);

    if (node->type != AST_BINARY_OPERATION) return;

    ASTNode *lhs = node->left;
    ASTNode *rhs = node->right;
    if (!lhs || !rhs) return;

    #define IS_LIT(n, v) ((n)->type == AST_LITERAL_VALUE && \
                          (n)->operation_type == TOKEN_NUMBER && \
                          strcmp((n)->value, (v)) == 0)

    TokenType op = node->operation_type;

    if (op == TOKEN_PLUS) {
        if (IS_LIT(lhs, "0")) {
            parser__free_ast_node(lhs);
            ASTNode tmp = *rhs; tmp.line = node->line; tmp.column = node->column;
            *node = tmp;
            return;
        }
        if (IS_LIT(rhs, "0")) {
            parser__free_ast_node(rhs);
            ASTNode tmp = *lhs; tmp.line = node->line; tmp.column = node->column;
            *node = tmp;
            return;
        }
    }
    if (op == TOKEN_MINUS) {
        if (IS_LIT(rhs, "0")) {
            parser__free_ast_node(rhs);
            ASTNode tmp = *lhs; tmp.line = node->line; tmp.column = node->column;
            *node = tmp;
            return;
        }
        /* x - x => 0 is safe only when no side effects. We skip it. */
    }
    if (op == TOKEN_STAR) {
        if (IS_LIT(lhs, "1")) {
            parser__free_ast_node(lhs);
            ASTNode tmp = *rhs; *node = tmp;
            return;
        }
        if (IS_LIT(rhs, "1")) {
            parser__free_ast_node(rhs);
            ASTNode tmp = *lhs; *node = tmp;
            return;
        }
        if (IS_LIT(lhs, "0") || IS_LIT(rhs, "0")) {
            parser__free_ast_node(lhs);
            parser__free_ast_node(rhs);
            ASTNode *zero = make_literal_zero(pool, node->line, node->column);
            memcpy(node, zero, sizeof(ASTNode));
            return;
        }
    }
    if (op == TOKEN_SLASH) {
        if (IS_LIT(rhs, "1")) {
            parser__free_ast_node(rhs);
            ASTNode tmp = *lhs; *node = tmp;
            return;
        }
    }
    #undef IS_LIT
}

static void pass_fold_constants(ASTNode *node, SymbolTable *global_scope,
                                ASTNodePool *pool) {
    if (!node) return;
    ASTNode *const_val = eval_constant(node, global_scope, pool);
    if (const_val) {
        const_val->line = node->line;
        const_val->column = node->column;
        if (node->left)  parser__free_ast_node(node->left);
        if (node->right) parser__free_ast_node(node->right);
        if (node->extra) parser__free_ast_node(node->extra);
        memcpy(node, const_val, sizeof(ASTNode));
        memset(const_val, 0, sizeof(ASTNode));
        parser__ast_node_pool_free(pool, const_val);
        return;
    }
    pass_fold_constants(node->left, global_scope, pool);
    pass_fold_constants(node->right, global_scope, pool);
    pass_fold_constants(node->extra, global_scope, pool);
    if (node->default_value)
        pass_fold_constants(node->default_value, global_scope, pool);
}

static void pass_propagate_fixed(ASTNode *node, SymbolTable *global_scope,
                                 ASTNodePool *pool) {
    if (!node) return;

    typedef struct { char *name; ASTNode *value; } FixedVar;
    FixedVar *fvs = NULL;
    size_t cnt = 0, cap = 0;

    /* Collect fixed variables and their evaluated constant values. */
    void collect(ASTNode *n) {
        if (!n) return;
        if (n->type == AST_VARIABLE_DECLARATION) {
            const char *mod = n->state_modifier;
            if ((mod && strstr(mod, "fixed")) ||
                (n->access_modifier && strstr(n->access_modifier, "fixed"))) {
                SymbolEntry *sym = lookup_global_symbol(global_scope, n->value);
                if (sym && sym->is_fixed && n->default_value) {
                    ASTNode *val = eval_constant(n->default_value,
                                                 global_scope, pool);
                    if (val) {
                        if (cnt >= cap) {
                            cap = cap ? cap * 2 : 4;
                            fvs = realloc(fvs, cap * sizeof(FixedVar));
                        }
                        fvs[cnt].name  = u__strduplic(n->value);
                        fvs[cnt].value = val;
                        cnt++;
                    }
                }
            }
        }
        collect(n->left); collect(n->right); collect(n->extra);
        if (n->default_value) collect(n->default_value);
    }
    collect(node);

    /* Replace reads of those variables with the constant. */
    void replace_uses(ASTNode *parent, ASTNode **child_ptr) {
        if (!child_ptr || !*child_ptr) return;
        ASTNode *ch = *child_ptr;
        if (ch->type == AST_IDENTIFIER && !is_lvalue_parent(parent, ch)) {
            for (size_t i = 0; i < cnt; i++) {
                if (strcmp(ch->value, fvs[i].name) == 0) {
                    ASTNode *new_node = clone_ast_node(fvs[i].value, pool);
                    new_node->line = ch->line;
                    new_node->column = ch->column;
                    parser__ast_node_pool_free(pool, ch);
                    *child_ptr = new_node;
                    break;
                }
            }
        }
        replace_uses(ch, &ch->left);
        replace_uses(ch, &ch->right);
        replace_uses(ch, &ch->extra);
        if (ch->default_value) replace_uses(ch, &ch->default_value);
    }
    replace_uses(NULL, &node);

    /* Remove the fixed variable declarations. */
    void remove_decls(ASTNode **parent_ptr) {
        if (!parent_ptr || !*parent_ptr) return;
        ASTNode *n = *parent_ptr;
        if (n->type == AST_VARIABLE_DECLARATION) {
            const char *mod = n->state_modifier;
            if ((mod && strstr(mod, "fixed")) ||
                (n->access_modifier && strstr(n->access_modifier, "fixed"))) {
                parser__free_ast_node(n);
                *parent_ptr = NULL;
                return;
            }
        }
        if (n->type == AST_BLOCK && n->extra) {
            AST *list = (AST *)n->extra;
            for (uint16_t i = 0; i < list->count; i++)
                if (list->nodes[i]) remove_decls(&list->nodes[i]);
        }
        remove_decls(&n->left);
        remove_decls(&n->right);
        remove_decls(&n->extra);
    }
    remove_decls(&node);

    for (size_t i = 0; i < cnt; i++) free(fvs[i].name);
    free(fvs);
}

static void pass_copy_propagation(ASTNode *node, SymbolTable *global_scope,
                                  ASTNodePool *pool) {
    (void)global_scope;
    (void)pool;
    if (!node) return;
    /* Full implementation requires basic‑block detection.  Placeholder:
       we just recurse into children. */
    pass_copy_propagation(node->left, global_scope, pool);
    pass_copy_propagation(node->right, global_scope, pool);
    pass_copy_propagation(node->extra, global_scope, pool);
}

static void pass_dead_store_elimination(ASTNode *node) {
    if (!node) return;
    pass_dead_store_elimination(node->left);
    pass_dead_store_elimination(node->right);
    pass_dead_store_elimination(node->extra);
}

static void merge_identical_in_block(ASTNode *block) {
    if (!block || block->type != AST_BLOCK || !block->extra) return;
    AST *list = (AST *)block->extra;
    for (uint16_t i = 0; i + 1 < list->count; i++) {
        ASTNode *s1 = list->nodes[i];
        ASTNode *s2 = list->nodes[i + 1];
        if (s1 && s2 && ast_nodes_identical(s1, s2)) {
            parser__free_ast_node(s2);
            for (uint16_t j = i + 1; j < list->count - 1; j++)
                list->nodes[j] = list->nodes[j + 1];
            list->count--;
            i--; /* re‑examine current position */
        }
    }
}

static void pass_merge_identical_statements(ASTNode *node) {
    if (!node) return;
    if (node->type == AST_BLOCK)
        merge_identical_in_block(node);
    pass_merge_identical_statements(node->left);
    pass_merge_identical_statements(node->right);
    pass_merge_identical_statements(node->extra);
}

/* Simple expression hash for CSE. */
typedef struct CSEEntry {
    uint32_t hash;
    ASTNode  *expr;
    struct CSEEntry *next;
} CSEEntry;

#define CSE_HASH_SIZE 64

static uint32_t expr_hash(ASTNode *node) {
    if (!node) return 0;
    uint32_t h = (uint32_t)node->type * 31 + (uint32_t)node->operation_type;
    if (node->value) {
        const char *s = node->value;
        while (*s) h = h * 31 + (unsigned char)*s++;
    }
    h ^= expr_hash(node->left);
    h ^= expr_hash(node->right);
    h ^= expr_hash(node->extra);
    return h;
}

static bool expr_equal(ASTNode *a, ASTNode *b) {
    return ast_nodes_identical(a, b);
}

static void cse_process_block(ASTNode *block, ASTNodePool *pool) {
    if (!block || block->type != AST_BLOCK || !block->extra) return;
    AST *list = (AST *)block->extra;
    CSEEntry *table[CSE_HASH_SIZE] = {0};

    for (uint16_t i = 0; i < list->count; i++) {
        ASTNode *stmt = list->nodes[i];
        if (!stmt) continue;

        /* Recursively handle nested blocks. */
        if (stmt->type == AST_BLOCK)
            cse_process_block(stmt, pool);

        /* Look for the right‑hand side of an assignment. Other statement
           types (e.g. standalone function calls) are not simplified. */
        ASTNode *expr = NULL;
        if (stmt->type == AST_ASSIGNMENT || stmt->type == AST_COMPOUND_ASSIGNMENT)
            expr = stmt->right;
        /* If we later add a dedicated expression‑statement node type,
           handle it here. */
        if (!expr || expr->type == AST_LITERAL_VALUE ||
            expr->type == AST_IDENTIFIER)
            continue;

        uint32_t h = expr_hash(expr);
        uint32_t idx = h % CSE_HASH_SIZE;
        CSEEntry *entry = table[idx];
        while (entry) {
            if (entry->hash == h && expr_equal(entry->expr, expr)) {
                /* Replace the duplicate with a clone of the original. */
                parser__free_ast_node(expr);
                ASTNode *copy = clone_ast_node(entry->expr, pool);
                if (stmt->type == AST_ASSIGNMENT || stmt->type == AST_COMPOUND_ASSIGNMENT)
                    stmt->right = copy;
                break;
            }
            entry = entry->next;
        }
        if (!entry) {
            /* First occurrence – insert into table. */
            CSEEntry *new_entry = malloc(sizeof(CSEEntry));
            if (!new_entry) continue;
            new_entry->hash = h;
            new_entry->expr = expr; /* keep the original expression */
            new_entry->next = table[idx];
            table[idx] = new_entry;
        }
    }

    /* Free the hash table entries (the expressions themselves remain in AST). */
    for (int i = 0; i < CSE_HASH_SIZE; i++) {
        CSEEntry *e = table[i];
        while (e) {
            CSEEntry *next = e->next;
            free(e);
            e = next;
        }
    }
}

static void pass_cse(ASTNode *node, ASTNodePool *pool) {
    if (!node) return;
    if (node->type == AST_BLOCK)
        cse_process_block(node, pool);
    pass_cse(node->left, pool);
    pass_cse(node->right, pool);
    pass_cse(node->extra, pool);
}

static void simplify_conditional(ASTNode **node_ptr,
                                 SymbolTable *global_scope,
                                 ASTNodePool *pool) {
    if (!node_ptr || !*node_ptr) return;
    ASTNode *node = *node_ptr;
    if (node->type != AST_IF_STATEMENT) return;

    ASTNode *cond_const = eval_constant(node->left, global_scope, pool);
    if (cond_const) {
        bool condition_true = (strcmp(cond_const->value, "0") != 0);
        free_cloned_node(cond_const);
        ASTNode *chosen = condition_true ? node->right : node->extra;
        parser__free_ast_node(node->left);
        if (node->right) parser__free_ast_node(node->right);
        if (node->extra) parser__free_ast_node(node->extra);
        if (chosen) {
            *node_ptr = chosen;
        } else {
            ASTNode *empty = parser__ast_node_pool_alloc(pool);
            memset(empty, 0, sizeof(ASTNode));
            empty->type = AST_BLOCK;
            *node_ptr = empty;
        }
        return;
    }

    /* Merge identical branches. */
    if (node->right && node->extra &&
        ast_nodes_identical(node->right, node->extra)) {
        ASTNode *branch = node->right;
        parser__free_ast_node(node->left);
        if (node->extra) parser__free_ast_node(node->extra);
        *node_ptr = branch;
        return;
    }

    /* Recurse into branches. */
    simplify_conditional(&node->right, global_scope, pool);
    if (node->extra)
        simplify_conditional(&node->extra, global_scope, pool);
}

static void pass_simplify_conditionals(ASTNode *node,
                                       SymbolTable *global_scope,
                                       ASTNodePool *pool) {
    if (!node) return;
    if (node->type == AST_BLOCK && node->extra) {
        AST *list = (AST *)node->extra;
        for (uint16_t i = 0; i < list->count; i++) {
            if (list->nodes[i])
                simplify_conditional(&list->nodes[i], global_scope, pool);
        }
    }
    pass_simplify_conditionals(node->left, global_scope, pool);
    pass_simplify_conditionals(node->right, global_scope, pool);
    pass_simplify_conditionals(node->extra, global_scope, pool);
}

static bool function_is_recursive(const char *func_name, ASTNode *body);
static ASTNode *clone_body_with_args(ASTNode *body, ASTNodePool *pool,
                                     char **param_names, size_t param_count,
                                     ASTNode **args, size_t arg_count);

static void pass_inline_functions(ASTNode *node, SymbolTable *global_scope,
                                  ASTNodePool *pool) {
    if (!node) return;

    typedef struct {
        char *name;
        ASTNode *body;
        char **param_names;
        size_t param_count;
        bool is_inline;
    } InlineFunc;

    InlineFunc *ilist = NULL;
    size_t icnt = 0, icap = 0;

    void collect(ASTNode *n) {
        if (!n) return;
        if (n->type == AST_FUNCTION_DECLARATION) {
            SymbolEntry *fsym = lookup_global_symbol(global_scope, n->value);
            if (fsym && fsym->is_inline && n->right) {
                ASTNode *body = n->right;
                if (body->type != AST_BLOCK) return;
                char **names = NULL;
                size_t pcount = 0;
                if (n->left && n->left->type == AST_BLOCK && n->left->extra) {
                    AST *plist = (AST *)n->left->extra;
                    pcount = plist->count;
                    names = calloc(pcount, sizeof(char*));
                    if (!names) return;
                    for (uint16_t i = 0; i < plist->count; i++) {
                        if (plist->nodes[i]->type == AST_VARIABLE_DECLARATION)
                            names[i] = u__strduplic(plist->nodes[i]->value);
                    }
                }
                if (icnt >= icap) {
                    icap = icap ? icap * 2 : 4;
                    ilist = realloc(ilist, icap * sizeof(InlineFunc));
                }
                ilist[icnt].name        = u__strduplic(n->value);
                ilist[icnt].body        = body;
                ilist[icnt].param_names = names;
                ilist[icnt].param_count = pcount;
                ilist[icnt].is_inline   = !function_is_recursive(n->value, body);
                icnt++;
            }
        }
        collect(n->left);
        collect(n->right);
        collect(n->extra);
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
                        ASTNode *new_body = clone_body_with_args(
                            ilist[i].body, pool,
                            ilist[i].param_names, ilist[i].param_count,
                            args, argc);
                        if (new_body) {
                            parser__free_ast_node(*node_ptr);
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
        for (size_t j = 0; j < ilist[i].param_count; j++)
            free(ilist[i].param_names[j]);
        free(ilist[i].param_names);
    }
    free(ilist);
}

static bool function_is_recursive(const char *func_name, ASTNode *body) {
    if (!func_name || !body) return false;
    if (body->type == AST_FUNCTION_CALL &&
        body->left && body->left->type == AST_IDENTIFIER &&
        strcmp(body->left->value, func_name) == 0)
        return true;
    return function_is_recursive(func_name, body->left) ||
           function_is_recursive(func_name, body->right) ||
           function_is_recursive(func_name, body->extra);
}

static ASTNode *clone_body_with_args(ASTNode *body, ASTNodePool *pool,
                                     char **param_names, size_t param_count,
                                     ASTNode **args, size_t arg_count) {
    if (!body) return NULL;
    if (body->type == AST_BLOCK && body->extra) {
        AST *list = (AST *)body->extra;
        AST *new_list = calloc(1, sizeof(AST));
        if (!new_list) return NULL;
        for (uint16_t i = 0; i < list->count; i++) {
            ASTNode *stmt = clone_body_with_args(list->nodes[i], pool,
                                                 param_names, param_count,
                                                 args, arg_count);
            if (stmt && !add_ast_node_to_list(new_list, stmt)) {
                parser__free_ast_node(stmt);
                free(new_list->nodes);
                free(new_list);
                return NULL;
            }
        }
        ASTNode *block = parser__ast_node_pool_alloc(pool);
        memset(block, 0, sizeof(ASTNode));
        block->type = AST_BLOCK;
        block->extra = (ASTNode *)new_list;
        return block;
    }
    if (body->type == AST_IDENTIFIER) {
        for (size_t i = 0; i < param_count && i < arg_count; i++) {
            if (strcmp(body->value, param_names[i]) == 0)
                return clone_ast_node(args[i], pool);
        }
    }
    ASTNode *clone = parser__ast_node_pool_alloc(pool);
    memcpy(clone, body, sizeof(ASTNode));
    clone->left  = clone_body_with_args(body->left, pool,
                                        param_names, param_count,
                                        args, arg_count);
    clone->right = clone_body_with_args(body->right, pool,
                                        param_names, param_count,
                                        args, arg_count);
    clone->extra = clone_body_with_args(body->extra, pool,
                                        param_names, param_count,
                                        args, arg_count);
    return clone;
}

/*
 * Count the number of iterations of a do‑loop if it is constant.
 * Returns the count, or 0 if not determinable.
 */
static int count_loop_iterations(ASTNode *loop, SymbolTable *global_scope) {
    if (!loop || loop->type != AST_DO_LOOP) return 0;
    /* Very simplistic: a do-loop without condition or with a constant
       condition.  In this language a do-loop might be `do { ... } while(cond);`
       but currently our AST uses left child as condition and right as body.
       We attempt to evaluate the condition constant. */
    ASTNode *cond = loop->left;
    if (!cond) return 0;
    ASTNode *const_cond = eval_constant(cond, global_scope, NULL);
    if (!const_cond) return 0;
    bool cond_true = (strcmp(const_cond->value, "0") != 0);
    free_cloned_node(const_cond);
    /* If condition is always true, the loop runs indefinitely. */
    if (cond_true) return 0;
    /* If condition is false initially, the loop never executes?  The semantics
       of `do...while` is at least once.  But our AST node may represent a
       different kind of loop.  For simplicity we return 0. */
    return 0;
}

/*
 * Check if two do‑loop statements are adjacent and have compatible
 * structure for fusion.
 */
static bool are_loops_adjacent_and_compatible(ASTNode *loop1,
                                              ASTNode *loop2) {
    if (!loop1 || !loop2) return false;
    if (loop1->type != AST_DO_LOOP || loop2->type != AST_DO_LOOP)
        return false;
    /* Both loops must have the same condition and identical bodies
       (to be safe) or at least no side‑effects that would change
       iteration space.  We perform a conservative check. */
    if (!ast_nodes_identical(loop1->left, loop2->left)) return false;
    return true;
}

static void fuse_loops(ASTNode *loop1, ASTNode *loop2, ASTNodePool *pool) {
    /* Merge bodies: append the second body to the first, then remove
       the second loop. */
    ASTNode *body1 = loop1->right;
    ASTNode *body2 = loop2->right;
    if (!body1 || !body2) return;
    if (body1->type != AST_BLOCK || body2->type != AST_BLOCK) return;
    AST *list1 = (AST *)body1->extra;
    AST *list2 = (AST *)body2->extra;
    if (!list1 || !list2) return;
    for (uint16_t i = 0; i < list2->count; i++) {
        ASTNode *stmt = clone_ast_node(list2->nodes[i], pool);
        if (stmt)
            add_ast_node_to_list(list1, stmt);
    }
    /* The second loop will be removed by blanking it; DCE will clean. */
    parser__free_ast_node(loop2);
}

static void pass_loop_optimizations(ASTNode *node,
                                    SymbolTable *global_scope,
                                    ASTNodePool *pool) {
    if (!node) return;
    if (node->type == AST_BLOCK && node->extra) {
        AST *list = (AST *)node->extra;
        for (uint16_t i = 0; i < list->count; i++) {
            ASTNode *stmt = list->nodes[i];
            if (!stmt) continue;
            if (stmt->type == AST_DO_LOOP) {
                /* Attempt unrolling. */
                int iters = count_loop_iterations(stmt, global_scope);
                if (iters > 0 && iters <= 8) {
                    /* Unroll: duplicate the body iters‑1 times. */
                    ASTNode *body = stmt->right;
                    if (body && body->type == AST_BLOCK) {
                        for (int k = 1; k < iters; k++) {
                            ASTNode *copy = clone_ast_node(body, pool);
                            if (copy)
                                add_ast_node_to_list((AST *)body->extra, copy);
                        }
                    }
                    /* Remove the loop wrapper: replace loop with its body. */
                    parser__free_ast_node(stmt->left); /* condition */
                    ASTNode *unrolled = body;
                    /* We must replace the loop node with the block.
                       Since we are inside a block list, we can directly
                       replace the array element. */
                    parser__free_ast_node(stmt); /* the loop node itself */
                    list->nodes[i] = unrolled;
                }

                /* Loop‑invariant code motion: scan the loop body for
                   expressions that do not depend on the loop index.
                   Hoist them before the loop.  Not implemented. */

                /* Loop fusion: check if the next statement is a compatible
                   loop. */
                if (i + 1 < list->count) {
                    ASTNode *next = list->nodes[i + 1];
                    if (next && are_loops_adjacent_and_compatible(stmt, next)) {
                        fuse_loops(stmt, next, pool);
                        /* Since we removed next, shift the list. */
                        for (uint16_t j = i + 1; j < list->count - 1; j++)
                            list->nodes[j] = list->nodes[j + 1];
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

static void eliminate_dead_in_block(ASTNode *block) {
    if (!block || block->type != AST_BLOCK || !block->extra) return;
    AST *list = (AST *)block->extra;
    uint16_t i = 0;
    while (i < list->count) {
        ASTNode *stmt = list->nodes[i];
        if (!stmt) { i++; continue; }
        if (stmt->type == AST_BLOCK)
            eliminate_dead_in_block(stmt);
        if (stmt->type == AST_IF_STATEMENT) {
            if (stmt->right) eliminate_dead_in_block(stmt->right);
            if (stmt->extra) eliminate_dead_in_block(stmt->extra);
        }
        if (stmt->type == AST_DO_LOOP) {
            if (stmt->right) eliminate_dead_in_block(stmt->right);
            if (stmt->extra) eliminate_dead_in_block(stmt->extra);
        }
        if (is_terminator(stmt)) {
            for (uint16_t j = i + 1; j < list->count; j++) {
                if (list->nodes[j])
                    parser__free_ast_node(list->nodes[j]);
                list->nodes[j] = NULL;
            }
            list->count = i + 1;
            break;
        }
        i++;
    }
}

static void pass_eliminate_dead_code(ASTNode *node) {
    if (!node) return;
    if (node->type == AST_BLOCK)
        eliminate_dead_in_block(node);
    pass_eliminate_dead_code(node->left);
    pass_eliminate_dead_code(node->right);
    pass_eliminate_dead_code(node->extra);
}

bool optimizer__optimize(AST *ast, SymbolTable *global_scope) {
    if (!ast || !global_scope) return false;
    ASTNodePool *pool = ast->pool;
    if (!pool) return false;

    for (uint16_t i = 0; i < ast->count; i++) {
        ASTNode *stmt = ast->nodes[i];
        if (!stmt) continue;

        pass_convert_bases(stmt);
        pass_algebraic_simplify(stmt, pool);
        pass_fold_constants(stmt, global_scope, pool);
        pass_propagate_fixed(stmt, global_scope, pool);
        pass_copy_propagation(stmt, global_scope, pool);
        pass_dead_store_elimination(stmt);
        pass_merge_identical_statements(stmt);
        pass_cse(stmt, pool);
        pass_simplify_conditionals(stmt, global_scope, pool);
        pass_inline_functions(stmt, global_scope, pool);
        pass_loop_optimizations(stmt, global_scope, pool);
        pass_eliminate_dead_code(stmt);
    }

    return true;
}
