#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "parser.h"

/* Maximum number of type modifiers (keywords such as unsigned, const, etc.) */
#define MAX_MODIFIERS           8
/* Initial capacity (number of elements) for dynamic AST node lists */
#define AST_INITIAL_CAPACITY    4
/* Initial number of pre-allocated nodes in the AST node pool */
#define AST_POOL_INIT_CAP       64
/* Convenience macro to get the number of elements in a static array */
#define ARRAY_SIZE(arr)         (sizeof(arr) / sizeof((arr)[0]))

/*
 * Forward declarations of all static functions.
 * The declarations are grouped by their purpose:
 *   - lexer interface helpers
 *   - memory management
 *   - AST node creation
 *   - list manipulation
 *   - type parsing
 *   - expression parsing
 *   - statement parsing
 *   - utility / error recovery
 */
static TokenType get_current_token_type(ParserState *state);
static void advance_token(ParserState *state);
static Token *get_current_token(ParserState *state);
static bool expect_token(ParserState *state, TokenType expected);
static void *safe_malloc(ParserState *state, size_t size);
static void *safe_realloc(ParserState *state, void *ptr, size_t size);
static char *safe_strdup(ParserState *state, const char *str);
static ASTNode *create_ast_node(ParserState *state, ASTNodeType type,
                                TokenType op, char *value,
                                ASTNode *left, ASTNode *right,
                                ASTNode *extra);
static bool add_ast_node_to_list(AST *ast, ASTNode *node);
static AST *create_empty_ast_list(ParserState *state);
static bool add_to_list_or_fail(ParserState *state, AST *list, ASTNode *node);
static AST *parse_universal_list(ParserState *state,
                                 ASTNode *(*parse_elem)(ParserState *),
                                 bool (*is_start)(ParserState *),
                                 TokenType sep, TokenType end);
static ASTNode *parse_binary_op(ParserState *state,
                                ASTNode *(*parse_op)(ParserState *),
                                const TokenType *ops, uint8_t cnt);
static bool parse_type_prefixes(ParserState *state,
                                uint8_t *ptr_lvl, uint8_t *dummy, uint8_t *ref);
static void apply_prefixes_to_type(Type *type,
                                   uint8_t ptr_lvl, uint8_t dummy, uint8_t ref);
static Type *create_temp_type_with_prefixes(ParserState *state,
                                            uint8_t ptr_lvl, uint8_t dummy,
                                            uint8_t ref);
static Type *parse_type_specifier_silent(ParserState *state,
                                         bool silent, bool parse_prefixes);
static Type *parse_type_specifier(ParserState *state, bool parse_prefixes);
static ASTNode *parse_fixed_args_func(ParserState *state, ASTNodeType type,
                                      uint8_t arg_cnt, const char *name);
static ASTNode *parse_alloc_expression(ParserState *state);
static ASTNode *parse_realloc_expression(ParserState *state);
static ASTNode *parse_postfix_expression(ParserState *state, ASTNode *node);
static ASTNode *parse_block(ParserState *state);
static ASTNode *parse_multi_initializer(ParserState *state);
static ASTNode *parse_if_statement(ParserState *state);
static ASTNode *parse_do_loop(ParserState *state);
static ASTNode *parse_break_statement(ParserState *state);
static ASTNode *parse_continue_statement(ParserState *state);
static ASTNode *parse_signal_statement(ParserState *state);
static ASTNode *parse_jump_statement(ParserState *state);
static ASTNode *parse_return_statement(ParserState *state);
static ASTNode *parse_free_statement(ParserState *state);
static ASTNode *parse_nop_statement(ParserState *state);
static ASTNode *parse_asm_statement(ParserState *state);
static ASTNode *parse_else_statement(ParserState *state);
static ASTNode *parse_state_statement(ParserState *state);
static ASTNode *parse_expression(ParserState *state);
static ASTNode *parse_assignment_expression(ParserState *state);
static ASTNode *parse_ternary_expression(ParserState *state);
static ASTNode *parse_logical_expression(ParserState *state);
static ASTNode *parse_bitwise_or_expression(ParserState *state);
static ASTNode *parse_bitwise_xor_expression(ParserState *state);
static ASTNode *parse_bitwise_and_expression(ParserState *state);
static ASTNode *parse_equality_expression(ParserState *state);
static ASTNode *parse_relational_expression(ParserState *state);
static ASTNode *parse_shift_expression(ParserState *state);
static ASTNode *parse_additive_expression(ParserState *state);
static ASTNode *parse_multiplicative_expression(ParserState *state);
static ASTNode *parse_unary_expression(ParserState *state);
static ASTNode *parse_primary_expression(ParserState *state);
static ASTNode *parse_parameter(ParserState *state);
static AST *parse_parameter_list(ParserState *state);
static ASTNode *parse_statement(ParserState *state);
static ASTNode *parse_block_or_statement(ParserState *state);
static bool statement_requires_semicolon(ASTNode *node);
static void parse_decl_special_modifiers(ParserState *state,
                                         char **acc, bool *cnst);
static void parse_semicolon(ParserState *state);
static ASTNode *parse_sequence(ParserState *state, bool expect_rcurly);
static void skip_to_sync_token(ParserState *state);

/*
 * Report an error using the current token’s position.
 * If no token is available (EOF), the position defaults to (0,0).
 */
#define REPORT_ERR(state, level, code, ctx, ...) do { \
    Token *cur = get_current_token(state); \
    if (cur) \
        errhandler__report_error_ex(level, code, cur->line, cur->column, \
                                    (int)strlen(cur->value), ctx, __VA_ARGS__); \
    else \
        errhandler__report_error_ex(level, code, 0, 0, 0, ctx, __VA_ARGS__); \
} while(0)

/*
 * Convenience macros for common error severities.
 * PARSE_ERROR   – a non‑fatal syntax error, panic mode is entered.
 * FATAL_ERROR   – an unrecoverable error (e.g., out of memory),
 *                 sets the fatal_error flag.
 * UNEXPECTED_TOKEN – specific error for unexpected token types.
 */
#define PARSE_ERROR(state, code, ...) \
    REPORT_ERR(state, ERROR_LEVEL_ERROR, code, "syntax", __VA_ARGS__)

#define FATAL_ERROR(state, code, ...) do { \
    state->fatal_error = true; \
    REPORT_ERR(state, ERROR_LEVEL_FATAL, code, "syntax", __VA_ARGS__); \
} while(0)

#define UNEXPECTED_TOKEN(state, exp, act) do { \
    Token *cur = get_current_token(state); \
    if (cur) \
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, \
                                    ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN, \
                                    cur->line, cur->column, \
                                    (int)strlen(cur->value), \
                                    "syntax", \
                                    "Expected %s but got %s (value: '%s')", \
                                    exp, act, cur->value); \
    else \
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, \
                                    ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN, \
                                    0, 0, 0, "syntax", \
                                    "Expected %s but got EOF", exp); \
    state->panic_mode = true; \
} while(0)

/*
 * Errors concerning unexpected end of file.
 */
#define UNEXPECTED_EOF(state) \
    PARSE_ERROR(state, ERROR_CODE_SYNTAX_UNEXPECTED_EOF, "Unexpected end of file")

/*
 * Token inspection and consumption shortcuts.
 */
#define TOKEN_IS(state, t)      (get_current_token_type(state) == (t))
#define SKIP_IF(state, t)       (TOKEN_IS(state, t) ? (advance_token(state), 1) : 0)
#define REQUIRE(state, t)       do { if (!expect_token(state, t)) return NULL; } while(0)

/*
 * Memory allocation helpers that route through safe_malloc/safe_realloc.
 * They will report a Fatal error on failure.
 */
#define ALLOC(state, sz)        safe_malloc(state, sz)
#define REALLOC(state, p, sz)   safe_realloc(state, p, sz)
#define STRDUP(state, s)        safe_strdup(state, s)

/*
 * Return the TokenType of the current token.
 * If the position is beyond the stream, TOKEN_EOF is returned.
 */
static TokenType get_current_token_type(ParserState *state) {
    return (state->current_token_position < state->total_tokens)
           ? state->token_stream[state->current_token_position].type
           : TOKEN_EOF;
}

/*
 * Advance the current token position by one, unless we are already at EOF.
 */
static void advance_token(ParserState *state) {
    if (state->current_token_position < state->total_tokens - 1)
        state->current_token_position++;
}

/*
 * Return a pointer to the current token, or NULL if the stream is exhausted.
 */
static Token *get_current_token(ParserState *state) {
    return (state->current_token_position < state->total_tokens)
           ? &state->token_stream[state->current_token_position]
           : NULL;
}

/*
 * Consume the current token if it matches `expected`; otherwise emit
 * an "unexpected token" error and return false.
 */
static bool expect_token(ParserState *state, TokenType expected) {
    if (TOKEN_IS(state, expected)) {
        advance_token(state);
        return true;
    }
    TokenType actual = get_current_token_type(state);
    const char *en = token_names[expected];
    const char *an = (actual == TOKEN_EOF) ? "EOF" : token_names[actual];
    UNEXPECTED_TOKEN(state, en, an);
    return false;
}

/*
 * Allocate `size` bytes. On failure the fatal_error flag is set and NULL
 * is returned.
 */
static void *safe_malloc(ParserState *state, size_t size) {
    void *ptr = malloc(size);
    if (!ptr && size > 0)
        FATAL_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION,
                    "Memory allocation failed");
    return ptr;
}

/*
 * Reallocate `ptr` to `size` bytes. On failure the fatal_error flag is set
 * and NULL is returned. The semantics match realloc() – the original block
 * remains unchanged on failure.
 */
static void *safe_realloc(ParserState *state, void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0)
        FATAL_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION,
                    "Memory reallocation failed");
    return new_ptr;
}

/*
 * Duplicate a string. Returns NULL (and sets fatal_error) if allocation fails.
 */
static char *safe_strdup(ParserState *state, const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char *dup = malloc(len);
    if (!dup) {
        FATAL_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION,
                    "String duplication failed");
        return NULL;
    }
    memcpy(dup, str, len);
    return dup;
}

/*
 * Allocate a node from the state’s pool. If the pool is exhausted, it is
 * automatically expanded by doubling its capacity. On catastrophic failure
 * NULL is returned and a fatal error has already been reported.
 *
 * The returned node is zero‑initialised.
 */
ASTNode *parser__ast_node_pool_alloc_or_expand(ParserState *state) {
    ASTNodePool *p = state->pool;
    if (!p || p->free_top == 0) {
        /* Expand pool by doubling its capacity */
        uint16_t new_cap = p ? (p->capacity * 2) : AST_POOL_INIT_CAP;
        if (new_cap < 1) new_cap = 1;   /* paranoia: avoid overflow */

        ASTNode *new_nodes = REALLOC(state, p ? p->nodes : NULL,
                                     new_cap * sizeof(ASTNode));
        uint16_t *new_free = REALLOC(state, p ? p->free_list : NULL,
                                     new_cap * sizeof(uint16_t));

        if (!new_nodes || !new_free)
            return NULL;   /* FATAL_ERROR already set inside REALLOC */

        if (p) {
            /* If the nodes array moved, rebuild the free list for the new region */
            if (new_nodes != p->nodes) {
                p->nodes = new_nodes;
                p->capacity = new_cap;
                for (uint16_t i = p->free_top; i < new_cap; i++)
                    new_free[i] = i;
                p->free_list = new_free;
            } else {
                p->free_list = new_free;
                for (uint16_t i = p->capacity; i < new_cap; i++)
                    new_free[p->free_top++] = i;
                p->capacity = new_cap;
            }
        } else {
            /* First-time creation (normally the pool is pre‑created, but we
               handle the edge case gracefully) */
            p = ALLOC(state, sizeof(ASTNodePool));
            p->nodes = new_nodes;
            p->capacity = new_cap;
            p->free_list = new_free;
            p->free_top = 0;
            for (uint16_t i = 0; i < new_cap; i++)
                p->free_list[p->free_top++] = i;
            state->pool = p;
        }
    }

    uint16_t idx = p->free_list[--p->free_top];
    ASTNode *n = &p->nodes[idx];
    memset(n, 0, sizeof(ASTNode));
    return n;
}

/*
 * Create a new AST node, initialise its fields, and set its source position
 * from the current token (or to (0,0) if no token is available).
 */
static ASTNode *create_ast_node(ParserState *state, ASTNodeType node_type,
                                TokenType operation_type, char *node_value,
                                ASTNode *left_child, ASTNode *right_child,
                                ASTNode *extra_node) {
    ASTNode *node = parser__ast_node_pool_alloc_or_expand(state);
    if (!node) return NULL;   /* Fatal error already reported */
    node->type = node_type;
    node->operation_type = operation_type;
    node->value = node_value;
    node->left = left_child;
    node->right = right_child;
    node->extra = extra_node;
    node->variable_type = NULL;
    node->default_value = NULL;
    node->state_modifier = NULL;
    node->access_modifier = NULL;
    node->is_const = false;
    Token *cur = get_current_token(state);
    if (cur) {
        node->line = cur->line;
        node->column = cur->column;
    } else {
        node->line = 0;
        node->column = 0;
    }
    return node;
}

/*
 * Append a node to a dynamic AST list (AST. nodes array).
 * If the internal array is full, its capacity is doubled.
 * Returns false on memory allocation failure, true otherwise.
 */
bool add_ast_node_to_list(AST *ast, ASTNode *node) {
    if (ast->count >= ast->capacity) {
        uint16_t new_cap = ast->capacity ? ast->capacity * 2 : AST_INITIAL_CAPACITY;
        ASTNode **new_nodes = realloc(ast->nodes, new_cap * sizeof(ASTNode*));
        if (!new_nodes) return false;
        ast->nodes = new_nodes;
        ast->capacity = new_cap;
    }
    ast->nodes[ast->count++] = node;
    return true;
}

/*
 * Allocate a new, empty AST list structure (used for lists of statements,
 * parameters, initializers, etc.). Returns NULL on allocation failure.
 */
static AST *create_empty_ast_list(ParserState *state) {
    AST *list = ALLOC(state, sizeof(AST));
    if (!list) return NULL;
    memset(list, 0, sizeof(AST));
    return list;
}

/*
 * Add a node to a list. If the addition fails the node and the list are
 * freed and false is returned. This function is used to propagate memory
 * errors upward while cleaning up.
 */
static bool add_to_list_or_fail(ParserState *state, AST *list, ASTNode *node) {
    if (!add_ast_node_to_list(list, node)) {
        parser__ast_node_pool_free(state->pool, node);
        free(list->nodes);
        free(list);
        return false;
    }
    return true;
}

/*
 * Skip tokens until we reach a synchronisation point:
 *   - semicolon, closing curly brace, or EOF
 *   - a keyword that typically starts a new statement
 * Used during error recovery to abandon a malformed construct and continue
 * parsing the next statement.
 */
static void skip_to_sync_token(ParserState *state) {
    state->panic_mode = true;
    while (state->current_token_position < state->total_tokens) {
        TokenType t = get_current_token_type(state);
        if (t == TOKEN_SEMICOLON || t == TOKEN_RCURLY || t == TOKEN_EOF)
            break;
        if (t == TOKEN_IF || t == TOKEN_DO || t == TOKEN_RETURN ||
            t == TOKEN_ELSE || t == TOKEN_BREAK || t == TOKEN_CONTINUE ||
            t == TOKEN_FREE || t == TOKEN_JUMP || t == TOKEN_SIGNAL ||
            t == TOKEN_NOP || t == TOKEN_ASM ||
            t == TOKEN_LCURLY || t == TOKEN_ID || t == TOKEN_STATE ||
            t == TOKEN_TYPEMOD || t == TOKEN_STATEMOD)
            break;
        advance_token(state);
    }
    state->panic_mode = false;
}

/*
 * Parse a list of elements delimited by `sep` and terminated by `end`.
 * `is_start` (if non‑NULL) is called for each element after the first to
 * verify that the element can start; this allows e.g. parameter lists to
 * reject two consecutive commas.
 *
 * Returns an AST list (the nodes are stored in the list’s extra field) or
 * NULL on error.
 */
static AST *parse_universal_list(ParserState *state,
                                 ASTNode *(*parse_elem)(ParserState *),
                                 bool (*is_start)(ParserState *),
                                 TokenType sep, TokenType end) {
    AST *list = create_empty_ast_list(state);
    if (!list) return NULL;

    /* Empty list? */
    if (TOKEN_IS(state, end)) {
        advance_token(state);
        return list;
    }

    while (!TOKEN_IS(state, end) && !TOKEN_IS(state, TOKEN_EOF)) {
        if (state->panic_mode) {
            skip_to_sync_token(state);
            if (TOKEN_IS(state, end)) break;
        }

        /* Check start guard for elements after the first */
        if (is_start && !is_start(state)) {
            if (TOKEN_IS(state, sep)) {
                advance_token(state);
                if (TOKEN_IS(state, end)) break;
                parser__free_ast(list);
                PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                            "Unexpected comma in list");
                skip_to_sync_token(state);
                return NULL;
            }
            break;
        }

        /* Parse one element */
        ASTNode *elem = parse_elem(state);
        if (!elem) {
            parser__free_ast(list);
            skip_to_sync_token(state);
            return NULL;
        }
        if (!add_to_list_or_fail(state, list, elem))
            return NULL;

        /* After an element we either have a separator or the terminator */
        if (TOKEN_IS(state, sep)) {
            advance_token(state);
            if (TOKEN_IS(state, end)) break;
            if (is_start && !is_start(state)) {
                parser__free_ast(list);
                PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                            "Expected element after comma");
                skip_to_sync_token(state);
                return NULL;
            }
        } else if (!TOKEN_IS(state, end)) {
            parser__free_ast(list);
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Expected '%s' or '%s'",
                        token_names[sep], token_names[end]);
            skip_to_sync_token(state);
            return NULL;
        }
    }

    if (!expect_token(state, end)) {
        parser__free_ast(list);
        skip_to_sync_token(state);
        return NULL;
    }
    return list;
}

/*
 * Parse a left‑associative chain of binary operators.
 * `ops` is an array of valid token types, `cnt` its length.
 * `parse_op` parses the next higher precedence level.
 *
 * Returns the root of the binary AST subtree, or NULL on error.
 */
static ASTNode *parse_binary_op(ParserState *state,
                                ASTNode *(*parse_op)(ParserState *),
                                const TokenType *ops, uint8_t cnt) {
    ASTNode *node = parse_op(state);
    if (!node) { skip_to_sync_token(state); return NULL; }

    while (1) {
        TokenType cur = get_current_token_type(state);
        bool is_op = false;
        for (uint8_t i = 0; i < cnt; i++) {
            if (cur == ops[i]) { is_op = true; break; }
        }
        if (!is_op) break;

        TokenType op = cur;
        advance_token(state);
        ASTNode *rhs = parse_op(state);
        if (!rhs) {
            parser__ast_node_pool_free(state->pool, node);
            skip_to_sync_token(state);
            return NULL;
        }
        ASTNode *new_node = AST_NEW_BINARY(state, op, node, rhs);
        if (!new_node) {
            parser__ast_node_pool_free(state->pool, rhs);
            parser__ast_node_pool_free(state->pool, node);
            return NULL;
        }
        node = new_node;
    }
    return node;
}

/*
 * Consume consecutive `@` (pointer) and `&` (reference) prefixes.
 * Sets `ptr_lvl` and `ref` accordingly; `dummy` is always 0 (reserved).
 * Returns true if at least one prefix was consumed.
 */
static bool parse_type_prefixes(ParserState *state,
                                uint8_t *ptr_lvl, uint8_t *dummy, uint8_t *ref) {
    *ptr_lvl = 0;
    *dummy = 0;
    *ref = 0;
    bool found = false;
    while (1) {
        if (TOKEN_IS(state, TOKEN_AT)) {
            advance_token(state);
            (*ptr_lvl)++;
            found = true;
        } else if (TOKEN_IS(state, TOKEN_AMPERSAND)) {
            advance_token(state);
            *ref = 1;
            found = true;
        } else break;
    }
    return found;
}

/*
 * Apply pointer/reference levels to an existing Type descriptor.
 */
static void apply_prefixes_to_type(Type *type,
                                   uint8_t ptr_lvl, uint8_t dummy, uint8_t ref) {
    if (!type) return;
    type->pointer_level = ptr_lvl;
    type->is_reference = ref;
}

/*
 * Create a temporary Type object with a default name "int" and the given
 * pointer/reference levels. Used when a type must be inferred from context.
 */
static Type *create_temp_type_with_prefixes(ParserState *state,
                                            uint8_t ptr_lvl, uint8_t dummy,
                                            uint8_t ref) {
    Type *t = ALLOC(state, sizeof(Type));
    if (!t) return NULL;
    memset(t, 0, sizeof(Type));
    t->name = STRDUP(state, "int");
    t->pointer_level = ptr_lvl;
    t->is_reference = ref;
    return t;
}

/*
 * Parse a full type specifier (modifiers, base type, optional angle brackets
 * with size or expression, optional pointer/reference prefixes).
 *
 * When `silent` is true, no errors are reported on failure – this is used
 * for speculative parsing (trying to determine whether a construct is a
 * type specifier or something else).
 *
 * When `parse_prefixes` is true, leading @/& are consumed after modifiers
 * and applied to the resulting type.
 */
static Type *parse_type_specifier_silent(ParserState *state,
                                         bool silent, bool parse_prefixes) {
    Type *type = ALLOC(state, sizeof(Type));
    if (!type) return NULL;
    memset(type, 0, sizeof(Type));

    /* Consume modifier keywords (e.g., unsigned, volatile) */
    bool seen_const = false;
    while (TOKEN_IS(state, TOKEN_TYPEMOD)) {
        if (type->modifier_count >= MAX_MODIFIERS) {
            if (!silent) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                     "Too many modifiers");
            parser__free_type(type);
            return NULL;
        }
        Token *mod = get_current_token(state);
        char **new_mods = REALLOC(state, type->modifiers,
                                  (type->modifier_count + 1) * sizeof(char*));
        if (!new_mods) { parser__free_type(type); return NULL; }
        type->modifiers = new_mods;
        type->modifiers[type->modifier_count] = STRDUP(state, mod->value);
        if (!type->modifiers[type->modifier_count]) {
            parser__free_type(type);
            return NULL;
        }
        type->modifier_count++;
        advance_token(state);
    }

    /* Pointer/reference prefixes */
    uint8_t ptr_lvl = 0, dummy = 0, ref = 0;
    if (parse_prefixes) {
        parse_type_prefixes(state, &ptr_lvl, &dummy, &ref);
        apply_prefixes_to_type(type, ptr_lvl, dummy, ref);
    }

    /* typeof(expr) special form */
    if (TOKEN_IS(state, TOKEN_TYPEOF)) {
        advance_token(state);
        if (!TOKEN_IS(state, TOKEN_LPAREN)) {
            if (!silent) UNEXPECTED_TOKEN(state, "'('",
                                          token_names[get_current_token_type(state)]);
            parser__free_type(type);
            return NULL;
        }
        advance_token(state);
        ASTNode *expr = parse_expression(state);
        if (!expr) {
            if (!silent) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                     "Invalid typeof expression");
            parser__free_type(type);
            return NULL;
        }
        if (!expect_token(state, TOKEN_RPAREN)) {
            parser__ast_node_pool_free(state->pool, expr);
            parser__free_type(type);
            return NULL;
        }
        type->name = STRDUP(state, "typeof");
        type->typeof_expression = expr;
        apply_prefixes_to_type(type, ptr_lvl, dummy, ref);
        if (TOKEN_IS(state, TOKEN_LT) || TOKEN_IS(state, TOKEN_LBRACE)) {
            if (!silent) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                     "typeof cannot have angle brackets or "
                                     "array dimensions");
            parser__free_type(type);
            return NULL;
        }
        return type;
    }

    /* Base type name: either TOKEN_TYPE (built‑in) or TOKEN_ID (user‑defined) */
    if (TOKEN_IS(state, TOKEN_TYPE) || TOKEN_IS(state, TOKEN_ID)) {
        Token *tok = get_current_token(state);
        type->name = STRDUP(state, tok->value);
        if (!type->name) { parser__free_type(type); return NULL; }
        advance_token(state);
    } else {
        if (!silent) UNEXPECTED_TOKEN(state, "type name",
                                      token_names[get_current_token_type(state)]);
        parser__free_type(type);
        return NULL;
    }

    /* Generic or explicit size inside < > */
    if (TOKEN_IS(state, TOKEN_LT)) {
        advance_token(state);
        if (TOKEN_IS(state, TOKEN_GT)) {
            parser__free_type(type);
            if (!silent) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                     "Empty angle brackets");
            return NULL;
        }
        if (TOKEN_IS(state, TOKEN_NUMBER)) {
            /* Explicit byte size, e.g. <4> */
            long sz = atol(get_current_token(state)->value);
            if (sz <= 0 || sz > UINT8_MAX) {
                parser__free_type(type);
                if (!silent) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                         "Invalid type size: must be 1..%d",
                                         UINT8_MAX);
                return NULL;
            }
            type->size_in_bytes = (uint8_t)sz;
            advance_token(state);
            if (!expect_token(state, TOKEN_GT)) {
                parser__free_type(type);
                return NULL;
            }
        } else {
            /* Generic expression inside angle brackets */
            ASTNode *expr = parse_expression(state);
            if (!expr) { parser__free_type(type); return NULL; }
            if (TOKEN_IS(state, TOKEN_COMMA)) {
                AST *list = create_empty_ast_list(state);
                if (!list) {
                    parser__ast_node_pool_free(state->pool, expr);
                    parser__free_type(type);
                    return NULL;
                }
                if (!add_to_list_or_fail(state, list, expr)) {
                    parser__free_type(type);
                    return NULL;
                }
                while (TOKEN_IS(state, TOKEN_COMMA)) {
                    advance_token(state);
                    if (TOKEN_IS(state, TOKEN_GT)) break;
                    expr = parse_expression(state);
                    if (!expr) {
                        parser__free_ast(list);
                        parser__free_type(type);
                        return NULL;
                    }
                    if (!add_to_list_or_fail(state, list, expr)) {
                        parser__free_type(type);
                        return NULL;
                    }
                }
                if (!expect_token(state, TOKEN_GT)) {
                    parser__free_ast(list);
                    parser__free_type(type);
                    return NULL;
                }
                type->angle_expression = create_ast_node(state,
                                                        AST_MULTI_INITIALIZER,
                                                        0, NULL, NULL, NULL,
                                                        (ASTNode*)list);
            } else {
                if (!expect_token(state, TOKEN_GT)) {
                    parser__ast_node_pool_free(state->pool, expr);
                    parser__free_type(type);
                    return NULL;
                }
                type->angle_expression = expr;
            }
        }
    }
    return type;
}

/*
 * Public wrapper: parse a type specifier with error reporting.
 */
static Type *parse_type_specifier(ParserState *state, bool parse_prefixes) {
    return parse_type_specifier_silent(state, false, parse_prefixes);
}

/*
 * Top-level expression entry point; currently starts at assignment level.
 */
static ASTNode *parse_expression(ParserState *state) {
    return parse_assignment_expression(state);
}

/*
 * Parse assignment expressions, including compound assignments
 * (+=, -=, etc.) and multi‑initializer assignments (e.g., {a,b} = expr).
 */
static ASTNode *parse_assignment_expression(ParserState *state) {
    ASTNode *left = parse_ternary_expression(state);
    if (!left) return NULL;

    /* Multi‑initializer on the left side: {a, b} = ... */
    if (left->type == AST_MULTI_INITIALIZER) {
        if (!TOKEN_IS(state, TOKEN_EQUAL)) return left;
        advance_token(state);
        ASTNode *right = parse_expression(state);
        if (!right) {
            parser__ast_node_pool_free(state->pool, left);
            return NULL;
        }
        return create_ast_node(state, AST_MULTI_ASSIGNMENT, 0, NULL,
                               left, right, NULL);
    }

    /* Check for all assignment operators */
    static const TokenType assign_ops[] = {
        TOKEN_EQUAL, TOKEN_PLUS_EQ, TOKEN_MINUS_EQ, TOKEN_STAR_EQ,
        TOKEN_SLASH_EQ, TOKEN_PERCENT_EQ, TOKEN_PIPE_EQ, TOKEN_AMPERSAND_EQ,
        TOKEN_CARET_EQ, TOKEN_SHL_EQ, TOKEN_SHR_EQ, TOKEN_SAL_EQ,
        TOKEN_SAR_EQ, TOKEN_ROL_EQ, TOKEN_ROR_EQ
    };
    TokenType cur = get_current_token_type(state);
    for (uint8_t i = 0; i < ARRAY_SIZE(assign_ops); i++) {
        if (cur == assign_ops[i]) {
            advance_token(state);
            ASTNode *right = parse_assignment_expression(state);
            if (!right) {
                parser__ast_node_pool_free(state->pool, left);
                return NULL;
            }
            return AST_NEW_ASSIGNMENT(state, cur, left, right);
        }
    }
    return left;
}

/*
 * Ternary operator:  cond ? { true } : { false }
 * Both branches are blocks (enclosed in braces).
 */
static ASTNode *parse_ternary_expression(ParserState *state) {
    ASTNode *cond = parse_logical_expression(state);
    if (!cond) return NULL;

    if (TOKEN_IS(state, TOKEN_QUESTION)) {
        advance_token(state);
        REQUIRE(state, TOKEN_LCURLY);
        ASTNode *true_expr = parse_block(state);
        if (!true_expr) {
            parser__ast_node_pool_free(state->pool, cond);
            return NULL;
        }
        REQUIRE(state, TOKEN_COLON);
        REQUIRE(state, TOKEN_LCURLY);
        ASTNode *false_expr = parse_block(state);
        if (!false_expr) {
            parser__ast_node_pool_free(state->pool, cond);
            parser__free_ast_node(true_expr);
            return NULL;
        }
        return AST_NEW_TERNARY(state, cond, true_expr, false_expr);
    }
    return cond;
}

/*
 * The following functions implement the standard precedence levels:
 *   logical (||)
 *   bitwise or (|)
 *   bitwise xor (^)
 *   bitwise and (&)
 *   equality (==, !=)
 *   relational (<, >, <=, >=)
 *   shift (<<, >>, <<<, >>>, <|, |>)
 *   additive (+, -)
 *   multiplicative (*, /, %)
 */
static ASTNode *parse_logical_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_LOGICAL };
    return parse_binary_op(state, parse_bitwise_or_expression,
                           ops, ARRAY_SIZE(ops));
}
static ASTNode *parse_bitwise_or_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_PIPE };
    return parse_binary_op(state, parse_bitwise_xor_expression,
                           ops, ARRAY_SIZE(ops));
}
static ASTNode *parse_bitwise_xor_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_CARET };
    return parse_binary_op(state, parse_bitwise_and_expression,
                           ops, ARRAY_SIZE(ops));
}
static ASTNode *parse_bitwise_and_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_AMPERSAND };
    return parse_binary_op(state, parse_equality_expression,
                           ops, ARRAY_SIZE(ops));
}
static ASTNode *parse_equality_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_DOUBLE_EQ, TOKEN_NE };
    return parse_binary_op(state, parse_relational_expression,
                           ops, ARRAY_SIZE(ops));
}
static ASTNode *parse_relational_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_LT, TOKEN_GT, TOKEN_LE, TOKEN_GE };
    return parse_binary_op(state, parse_shift_expression,
                           ops, ARRAY_SIZE(ops));
}
static ASTNode *parse_shift_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_SHL, TOKEN_SHR, TOKEN_SAL,
                                     TOKEN_SAR, TOKEN_ROL, TOKEN_ROR };
    return parse_binary_op(state, parse_additive_expression,
                           ops, ARRAY_SIZE(ops));
}
static ASTNode *parse_additive_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_PLUS, TOKEN_MINUS };
    return parse_binary_op(state, parse_multiplicative_expression,
                           ops, ARRAY_SIZE(ops));
}
static ASTNode *parse_multiplicative_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT };
    return parse_binary_op(state, parse_unary_expression,
                           ops, ARRAY_SIZE(ops));
}

/*
 * Parse unary operators (prefix ++, --, !, ~, *, /, @, &) and then
 * postfix operators.
 */
static ASTNode *parse_unary_expression(ParserState *state) {
    if (TOKEN_IS(state, TOKEN_DOUBLE_PLUS) ||
        TOKEN_IS(state, TOKEN_DOUBLE_MINUS)) {
        TokenType op = get_current_token_type(state);
        advance_token(state);
        ASTNode *operand = parse_unary_expression(state);
        if (!operand) return NULL;
        ASTNodeType nt = (op == TOKEN_DOUBLE_PLUS) ? AST_PREFIX_INCREMENT
                                                   : AST_PREFIX_DECREMENT;
        return create_ast_node(state, nt, op, NULL, NULL, operand, NULL);
    }
    if (TOKEN_IS(state, TOKEN_BANG)   || TOKEN_IS(state, TOKEN_TILDE) ||
        TOKEN_IS(state, TOKEN_STAR)   || TOKEN_IS(state, TOKEN_SLASH) ||
        TOKEN_IS(state, TOKEN_AT)     || TOKEN_IS(state, TOKEN_AMPERSAND)) {
        TokenType op = get_current_token_type(state);
        advance_token(state);
        ASTNode *operand = parse_unary_expression(state);
        if (!operand) return NULL;
        return AST_NEW_UNARY(state, op, operand);
    }

    /* Fall through to primary, then apply postfix operators */
    ASTNode *prim = parse_primary_expression(state);
    if (!prim) return NULL;
    return parse_postfix_expression(state, prim);
}

/*
 * Parse primary expressions: literals, identifiers, parenthesised
 * expressions, built‑in functions (sizeof, typeof, alloc, realloc),
 * and multi‑initializer blocks.
 */
static ASTNode *parse_primary_expression(ParserState *state) {
    Token *tok = get_current_token(state);
    if (!tok) { UNEXPECTED_EOF(state); skip_to_sync_token(state); return NULL; }

    if (tok->type == TOKEN_LPAREN) {
        advance_token(state);
        ASTNode *expr = parse_expression(state);
        if (!expr) { skip_to_sync_token(state); return NULL; }
        REQUIRE(state, TOKEN_RPAREN);
        return expr;
    }
    if (tok->type == TOKEN_LCURLY) {
        return parse_multi_initializer(state);
    }
    if (tok->type == TOKEN_SIZEOF) {
        advance_token(state);
        REQUIRE(state, TOKEN_LPAREN);
        ASTNode *arg = parse_expression(state);
        if (!arg) return NULL;
        REQUIRE(state, TOKEN_RPAREN);
        return create_ast_node(state, AST_SIZEOF, 0, NULL, arg, NULL, NULL);
    }
    if (tok->type == TOKEN_TYPEOF) {
        advance_token(state);
        REQUIRE(state, TOKEN_LPAREN);
        ASTNode *expr = parse_expression(state);
        if (!expr) return NULL;
        REQUIRE(state, TOKEN_RPAREN);
        return create_ast_node(state, AST_TYPEOF, 0, NULL, expr, NULL, NULL);
    }
    if (tok->type == TOKEN_ALLOC)  return parse_alloc_expression(state);
    if (tok->type == TOKEN_REALLOC) return parse_realloc_expression(state);

    if (tok->type == TOKEN_NUMBER || tok->type == TOKEN_STRING ||
        tok->type == TOKEN_CHAR   || tok->type == TOKEN_NONE ||
        tok->type == TOKEN_TYPE) {
        char *val = STRDUP(state, tok->value);
        if (!val) return NULL;
        advance_token(state);
        return create_ast_node(state, AST_LITERAL_VALUE, tok->type,
                               val, NULL, NULL, NULL);
    }

    if (tok->type == TOKEN_ID) {
        char *name = STRDUP(state, tok->value);
        if (!name) return NULL;
        advance_token(state);
        ASTNode *id = create_ast_node(state, AST_IDENTIFIER, 0,
                                      name, NULL, NULL, NULL);
        return parse_postfix_expression(state, id);
    }

    PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Invalid expression");
    skip_to_sync_token(state);
    return NULL;
}

/*
 * Parse postfix operators: ++, --, function call, array access,
 * field access, and postfix cast (expr : type).
 */
static ASTNode *parse_postfix_expression(ParserState *state, ASTNode *node) {
    while (1) {
        if (TOKEN_IS(state, TOKEN_DOUBLE_PLUS)) {
            advance_token(state);
            node = create_ast_node(state, AST_POSTFIX_INCREMENT,
                                   TOKEN_DOUBLE_PLUS, NULL, node, NULL, NULL);
        } else if (TOKEN_IS(state, TOKEN_DOUBLE_MINUS)) {
            advance_token(state);
            node = create_ast_node(state, AST_POSTFIX_DECREMENT,
                                   TOKEN_DOUBLE_MINUS, NULL, node, NULL, NULL);
        } else if (TOKEN_IS(state, TOKEN_LPAREN)) {
            advance_token(state);
            AST *args = parse_universal_list(state, parse_expression,
                                             NULL, TOKEN_COMMA, TOKEN_RPAREN);
            if (!args) {
                parser__ast_node_pool_free(state->pool, node);
                return NULL;
            }
            ASTNode *call = create_ast_node(state, AST_FUNCTION_CALL, 0, NULL,
                                            node, NULL, (ASTNode*)args);
            if (!call) {
                parser__free_ast(args);
                parser__ast_node_pool_free(state->pool, node);
                return NULL;
            }
            node = call;
        } else if (TOKEN_IS(state, TOKEN_LBRACE)) {
            advance_token(state);
            ASTNode *idx = NULL;
            if (!TOKEN_IS(state, TOKEN_RBRACE)) {
                idx = parse_expression(state);
                if (!idx) {
                    parser__ast_node_pool_free(state->pool, node);
                    return NULL;
                }
            }
            REQUIRE(state, TOKEN_RBRACE);
            node = create_ast_node(state, AST_ARRAY_ACCESS, 0, NULL,
                                   node, idx, NULL);
        } else if (TOKEN_IS(state, TOKEN_DOT)) {
            advance_token(state);
            if (!TOKEN_IS(state, TOKEN_ID)) {
                parser__ast_node_pool_free(state->pool, node);
                PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                            "Field name expected after '.'");
                skip_to_sync_token(state);
                return NULL;
            }
            char *field = STRDUP(state, get_current_token(state)->value);
            advance_token(state);
            ASTNode *fld = create_ast_node(state, AST_IDENTIFIER, 0,
                                           field, NULL, NULL, NULL);
            node = create_ast_node(state, AST_FIELD_ACCESS, TOKEN_DOT, NULL,
                                   node, fld, NULL);
        } else if (TOKEN_IS(state, TOKEN_COLON)) {
            /* Postfix cast: expr : type */
            advance_token(state);
            Type *tp = parse_type_specifier(state, true);
            if (!tp) {
                parser__ast_node_pool_free(state->pool, node);
                return NULL;
            }
            ASTNode *cast = create_ast_node(state, AST_CAST, 0, NULL,
                                            node, NULL, NULL);
            if (!cast) {
                parser__free_type(tp);
                parser__ast_node_pool_free(state->pool, node);
                return NULL;
            }
            cast->variable_type = tp;
            node = cast;
        } else break;
    }
    return node;
}

/*
 * Helper that parses a built‑in function call taking a fixed number of
 * arguments (e.g., alloc, realloc). The arguments are parsed as
 * expressions and stored in an AST_BLOCK node attached to the extra field.
 */
static ASTNode *parse_fixed_args_func(ParserState *state, ASTNodeType nt,
                                      uint8_t arg_cnt, const char *name) {
    advance_token(state);
    REQUIRE(state, TOKEN_LPAREN);

    AST *args = create_empty_ast_list(state);
    if (!args) return NULL;

    for (uint8_t i = 0; i < arg_cnt; i++) {
        ASTNode *arg = parse_expression(state);
        if (!arg) { parser__free_ast(args); return NULL; }
        if (!add_to_list_or_fail(state, args, arg)) return NULL;
        if (i < arg_cnt - 1) {
            if (!TOKEN_IS(state, TOKEN_COMMA)) {
                parser__free_ast(args);
                PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                            "Expected comma after argument %d", i+1);
                return NULL;
            }
            advance_token(state);
        }
    }
    REQUIRE(state, TOKEN_RPAREN);

    ASTNode *args_block = create_ast_node(state, AST_BLOCK, 0, NULL,
                                          NULL, NULL, (ASTNode*)args);
    if (!args_block) { parser__free_ast(args); return NULL; }
    return create_ast_node(state, nt, 0, NULL, args_block, NULL, NULL);
}

static ASTNode *parse_alloc_expression(ParserState *state) {
    return parse_fixed_args_func(state, AST_ALLOC, 3, "alloc");
}
static ASTNode *parse_realloc_expression(ParserState *state) {
    return parse_fixed_args_func(state, AST_REALLOC, 2, "realloc");
}

/*
 * Parse a block enclosed in { } and return an AST_BLOCK node.
 * The body is parsed as a sequence of statements.
 */
static ASTNode *parse_block(ParserState *state) {
    REQUIRE(state, TOKEN_LCURLY);
    ASTNode *seq = parse_sequence(state, true);
    if (!seq) return NULL;
    REQUIRE(state, TOKEN_RCURLY);
    return seq;
}

/*
 * Parse a multi‑initializer: { expr, .field = expr, ... }
 * Returns an AST_MULTI_INITIALIZER node where the extra field points to
 * an AST list containing the individual elements.
 */
static ASTNode *parse_multi_initializer(ParserState *state) {
    REQUIRE(state, TOKEN_LCURLY);
    AST *list = create_empty_ast_list(state);
    if (!list) return NULL;

    if (TOKEN_IS(state, TOKEN_RCURLY)) {
        advance_token(state);
        return create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL,
                               NULL, NULL, (ASTNode*)list);
    }

    while (!TOKEN_IS(state, TOKEN_RCURLY) && !TOKEN_IS(state, TOKEN_EOF)) {
        if (state->panic_mode) {
            skip_to_sync_token(state);
            if (TOKEN_IS(state, TOKEN_RCURLY)) break;
        }

        ASTNode *elem = NULL;
        if (TOKEN_IS(state, TOKEN_DOT)) {
            /* Designated initializer: .field = value */
            advance_token(state);
            if (!TOKEN_IS(state, TOKEN_ID)) {
                parser__free_ast(list);
                PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                            "Field name expected after '.'");
                return NULL;
            }
            char *field = STRDUP(state, get_current_token(state)->value);
            advance_token(state);
            REQUIRE(state, TOKEN_EQUAL);
            ASTNode *val = parse_expression(state);
            if (!val) { free(field); parser__free_ast(list); return NULL; }
            ASTNode *fname = create_ast_node(state, AST_IDENTIFIER, 0,
                                             field, NULL, NULL, NULL);
            elem = create_ast_node(state, AST_FIELD_ACCESS, TOKEN_DOT, NULL,
                                   fname, val, NULL);
        } else {
            elem = parse_expression(state);
        }
        if (!elem) {
            parser__free_ast(list);
            skip_to_sync_token(state);
            return NULL;
        }
        if (!add_to_list_or_fail(state, list, elem)) return NULL;

        if (TOKEN_IS(state, TOKEN_COMMA)) {
            advance_token(state);
            if (TOKEN_IS(state, TOKEN_RCURLY)) break;
        } else if (!TOKEN_IS(state, TOKEN_RCURLY)) {
            parser__free_ast(list);
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Expected ',' or '}' in initializer");
            return NULL;
        }
    }
    REQUIRE(state, TOKEN_RCURLY);
    return create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL,
                           NULL, NULL, (ASTNode*)list);
}

/*
 * Parse an `if` statement:
 *   if (cond) body   or   if (cond) then body
 * Optionally followed by `else` body.
 */
static ASTNode *parse_if_statement(ParserState *state) {
    REQUIRE(state, TOKEN_IF);
    REQUIRE(state, TOKEN_LPAREN);
    ASTNode *cond = parse_expression(state);
    if (!cond) return NULL;
    REQUIRE(state, TOKEN_RPAREN);

    bool single = TOKEN_IS(state, TOKEN_THEN);
    ASTNode *if_body = parse_block_or_statement(state);
    if (!if_body) {
        parser__ast_node_pool_free(state->pool, cond);
        PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                    "Expected block after if");
        return NULL;
    }

    ASTNode *else_body = NULL;
    if (!single && SKIP_IF(state, TOKEN_ELSE)) {
        else_body = parse_block_or_statement(state);
        if (!else_body) {
            parser__ast_node_pool_free(state->pool, cond);
            parser__free_ast_node(if_body);
            return NULL;
        }
    }
    return create_ast_node(state, AST_IF_STATEMENT, 0, NULL,
                           cond, if_body, else_body);
}

/*
 * Parse a `do` loop:
 *   do (cond) body   or   do (cond) then body
 * Optionally followed by `else` body (runs if loop never executed).
 */
static ASTNode *parse_do_loop(ParserState *state) {
    REQUIRE(state, TOKEN_DO);
    REQUIRE(state, TOKEN_LPAREN);
    ASTNode *cond = parse_expression(state);
    if (!cond) return NULL;
    REQUIRE(state, TOKEN_RPAREN);

    bool single = TOKEN_IS(state, TOKEN_THEN);
    ASTNode *body = parse_block_or_statement(state);
    if (!body) {
        parser__ast_node_pool_free(state->pool, cond);
        return NULL;
    }

    ASTNode *else_body = NULL;
    if (!single && SKIP_IF(state, TOKEN_ELSE)) {
        else_body = parse_block_or_statement(state);
        if (!else_body) {
            parser__ast_node_pool_free(state->pool, cond);
            parser__free_ast_node(body);
            return NULL;
        }
    }
    return create_ast_node(state, AST_DO_LOOP, 0, NULL,
                           cond, body, else_body);
}

/* Simple statements that consume a keyword and (optionally) an expression. */
static ASTNode *parse_break_statement(ParserState *state) {
    REQUIRE(state, TOKEN_BREAK);
    return create_ast_node(state, AST_BREAK, 0, NULL, NULL, NULL, NULL);
}
static ASTNode *parse_continue_statement(ParserState *state) {
    REQUIRE(state, TOKEN_CONTINUE);
    return create_ast_node(state, AST_CONTINUE, 0, NULL, NULL, NULL, NULL);
}

/*
 * Parse `signal` statement: signal expr, expr, ...
 * If a single expression is given, it is wrapped directly; multiple
 * values are stored inside a MULTI_INITIALIZER.
 */
static ASTNode *parse_signal_statement(ParserState *state) {
    REQUIRE(state, TOKEN_SIGNAL);
    if (TOKEN_IS(state, TOKEN_SEMICOLON))
        return create_ast_node(state, AST_SIGNAL, 0, NULL, NULL, NULL, NULL);

    AST *list = create_empty_ast_list(state);
    if (!list) return NULL;
    ASTNode *first = parse_expression(state);
    if (!first) { parser__free_ast(list); return NULL; }
    if (!add_to_list_or_fail(state, list, first)) return NULL;

    while (TOKEN_IS(state, TOKEN_COMMA)) {
        advance_token(state);
        ASTNode *nxt = parse_expression(state);
        if (!nxt) { parser__free_ast(list); return NULL; }
        if (!add_to_list_or_fail(state, list, nxt)) return NULL;
    }

    if (list->count == 1) {
        ASTNode *expr = list->nodes[0];
        free(list->nodes);
        free(list);
        return create_ast_node(state, AST_SIGNAL, 0, NULL, expr, NULL, NULL);
    }
    ASTNode *multi = create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL,
                                     NULL, NULL, (ASTNode*)list);
    if (!multi) { parser__free_ast(list); return NULL; }
    return create_ast_node(state, AST_SIGNAL, 0, NULL, multi, NULL, NULL);
}

/*
 * Parse `jump` statement: jump target_expression;
 */
static ASTNode *parse_jump_statement(ParserState *state) {
    REQUIRE(state, TOKEN_JUMP);
    ASTNode *tgt = parse_expression(state);
    if (!tgt) return NULL;
    return create_ast_node(state, AST_JUMP, 0, NULL, tgt, NULL, NULL);
}

/*
 * Parse `return` statement: return expr, expr, ...
 * Same logic as signal – single value stored directly, multiple wrapped
 * in MULTI_INITIALIZER.
 */
static ASTNode *parse_return_statement(ParserState *state) {
    REQUIRE(state, TOKEN_RETURN);
    if (TOKEN_IS(state, TOKEN_SEMICOLON))
        return create_ast_node(state, AST_RETURN, 0, NULL, NULL, NULL, NULL);

    AST *list = create_empty_ast_list(state);
    if (!list) return NULL;
    ASTNode *first = parse_expression(state);
    if (!first) { parser__free_ast(list); return NULL; }
    if (!add_to_list_or_fail(state, list, first)) return NULL;

    while (TOKEN_IS(state, TOKEN_COMMA)) {
        advance_token(state);
        ASTNode *nxt = parse_expression(state);
        if (!nxt) { parser__free_ast(list); return NULL; }
        if (!add_to_list_or_fail(state, list, nxt)) return NULL;
    }

    if (list->count == 1) {
        ASTNode *expr = list->nodes[0];
        free(list->nodes);
        free(list);
        return create_ast_node(state, AST_RETURN, 0, NULL, expr, NULL, NULL);
    }
    ASTNode *multi = create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL,
                                     NULL, NULL, (ASTNode*)list);
    if (!multi) { parser__free_ast(list); return NULL; }
    return create_ast_node(state, AST_RETURN, 0, NULL, multi, NULL, NULL);
}

/*
 * Parse `free` statement: free expression   or   free(expression)
 */
static ASTNode *parse_free_statement(ParserState *state) {
    REQUIRE(state, TOKEN_FREE);
    ASTNode *expr = NULL;
    if (TOKEN_IS(state, TOKEN_LPAREN)) {
        advance_token(state);
        expr = parse_expression(state);
        if (!expr) return NULL;
        REQUIRE(state, TOKEN_RPAREN);
    } else {
        expr = parse_expression(state);
        if (!expr) return NULL;
    }
    return create_ast_node(state, AST_FREE, 0, NULL, expr, NULL, NULL);
}

/*
 * Parse `nop` statement (no operation).
 */
static ASTNode *parse_nop_statement(ParserState *state) {
    REQUIRE(state, TOKEN_NOP);
    return create_ast_node(state, AST_NOP, 0, NULL, NULL, NULL, NULL);
}

/*
 * Parse inline assembly statement: asm("assembly string");
 * The string is stored in the node's value field.
 */
static ASTNode *parse_asm_statement(ParserState *state) {
    REQUIRE(state, TOKEN_ASM);
    REQUIRE(state, TOKEN_LPAREN);

    if (!TOKEN_IS(state, TOKEN_STRING)) {
        UNEXPECTED_TOKEN(state, "string literal",
                         token_names[get_current_token_type(state)]);
        skip_to_sync_token(state);
        return NULL;
    }
    char *asm_str = STRDUP(state, get_current_token(state)->value);
    if (!asm_str) return NULL;
    advance_token(state);

    REQUIRE(state, TOKEN_RPAREN);
    return create_ast_node(state, AST_ASM, 0, asm_str, NULL, NULL, NULL);
}

/*
 * Parse a standalone `else` statement (used when else appears without a
 * preceding if; it becomes a catch‑all else that executes its body).
 */
static ASTNode *parse_else_statement(ParserState *state) {
    REQUIRE(state, TOKEN_ELSE);
    ASTNode *body = parse_block_or_statement(state);
    if (!body) {
        PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                    "Expected block after standalone else");
        return NULL;
    }
    return create_ast_node(state, AST_ELSE_STATEMENT, 0, NULL, NULL, body, NULL);
}

/*
 * Parse a declaration or definition statement starting with a state modifier
 * (TOKEN_STATE). This covers variable, array, and function declarations.
 * Detailed logic:
 *   - consume the state modifier
 *   - optional access modifier and const qualifier
 *   - optional pointer/reference prefixes
 *   - optional identifier name
 *   - optional array dimensions [size]...
 *   - if '(' follows, parse parameter list → function declaration
 *   - otherwise, optionally parse `: type` and initializer/struct body.
 *
 * Returns the AST node (AST_VARIABLE_DECLARATION or AST_FUNCTION_DECLARATION)
 * or NULL on error.
 */
static ASTNode *parse_state_statement(ParserState *state) {
    Token *state_tok = get_current_token(state);
    char *state_modifier = STRDUP(state, state_tok->value);
    if (!state_modifier) return NULL;
    advance_token(state);

    char *access_mod = NULL;
    bool is_const = false;
    parse_decl_special_modifiers(state, &access_mod, &is_const);

    uint8_t ptr_lvl = 0, dummy = 0, ref = 0;
    parse_type_prefixes(state, &ptr_lvl, &dummy, &ref);

    char *name = NULL;
    if (TOKEN_IS(state, TOKEN_ID)) {
        name = STRDUP(state, get_current_token(state)->value);
        if (!name) { free(state_modifier); free(access_mod); return NULL; }
        advance_token(state);
    }

    /* 'del' is a special deletion declaration – no type or body, just name */
    if (strcmp(state_modifier, "del") == 0) {
        if (!name && ptr_lvl == 0 && ref == 0) {
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Expected variable name or prefix after 'del'");
            free(state_modifier); free(access_mod); free(name);
            return NULL;
        }
        ASTNode *node = create_ast_node(state, AST_VARIABLE_DECLARATION,
                                        0, name, NULL, NULL, NULL);
        if (!node) { free(state_modifier); free(access_mod); free(name); return NULL; }
        node->state_modifier = state_modifier;
        node->access_modifier = access_mod;
        if (ptr_lvl || ref) {
            Type *t = create_temp_type_with_prefixes(state, ptr_lvl, dummy, ref);
            if (t) {
                t->name = STRDUP(state, name ? name : "?");
                node->variable_type = t;
            }
        }
        return node;
    }

    /* Parse optional array dimensions */
    AST *dimlist = NULL;
    uint8_t dimcnt = 0;
    while (TOKEN_IS(state, TOKEN_LBRACE)) {
        advance_token(state);
        ASTNode *sz = NULL;
        if (!TOKEN_IS(state, TOKEN_RBRACE)) {
            sz = parse_expression(state);
            if (!sz) {
                free(state_modifier); free(access_mod); free(name);
                if (dimlist) parser__free_ast(dimlist);
                return NULL;
            }
        }
        if (!expect_token(state, TOKEN_RBRACE)) {
            free(state_modifier); free(access_mod); free(name);
            if (dimlist) parser__free_ast(dimlist);
            return NULL;
        }
        if (!dimlist) {
            dimlist = create_empty_ast_list(state);
            if (!dimlist) {
                free(state_modifier); free(access_mod); free(name);
                return NULL;
            }
        }
        if (!add_to_list_or_fail(state, dimlist, sz)) {
            free(state_modifier); free(access_mod); free(name);
            return NULL;
        }
        dimcnt++;
    }

    /* Function declaration? */
    if (TOKEN_IS(state, TOKEN_LPAREN)) {
        if (!name) {
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Function name required");
            free(state_modifier); free(access_mod);
            if (dimlist) parser__free_ast(dimlist);
            return NULL;
        }
        if (dimcnt > 0) {
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Array dimensions before '(' not allowed");
            free(state_modifier); free(access_mod); free(name);
            if (dimlist) parser__free_ast(dimlist);
            return NULL;
        }

        AST *params = parse_parameter_list(state);
        if (!params) {
            free(state_modifier); free(access_mod); free(name);
            return NULL;
        }

        Type *ret_type = NULL;
        if (TOKEN_IS(state, TOKEN_COLON)) {
            advance_token(state);
            ret_type = parse_type_specifier(state, true);
            if (!ret_type) {
                free(state_modifier); free(access_mod); free(name);
                parser__free_ast(params);
                return NULL;
            }
        }

        ASTNode *body = NULL;
        if (strcmp(state_modifier, "def") == 0) {
            if (TOKEN_IS(state, TOKEN_LCURLY)) {
                body = parse_block(state);
                if (!body) {
                    free(state_modifier); free(access_mod); free(name);
                    parser__free_type(ret_type);
                    parser__free_ast(params);
                    return NULL;
                }
            }
        }

        ASTNode *params_node = create_ast_node(state, AST_BLOCK, 0, NULL,
                                               NULL, NULL, (ASTNode*)params);
        if (!params_node) {
            free(state_modifier); free(access_mod); free(name);
            parser__free_type(ret_type);
            parser__free_ast(params);
            if (body) parser__free_ast_node(body);
            return NULL;
        }
        ASTNode *func = create_ast_node(state, AST_FUNCTION_DECLARATION,
                                        0, name, params_node, body, NULL);
        if (!func) {
            free(state_modifier); free(access_mod);
            parser__free_type(ret_type);
            parser__free_ast(params);
            parser__free_ast_node(params_node);
            if (body) parser__free_ast_node(body);
            return NULL;
        }
        func->variable_type = ret_type;
        func->state_modifier = state_modifier;
        func->access_modifier = access_mod;
        func->is_const = is_const;
        return func;
    }

    /* Variable declaration */
    Type *var_type = NULL;
    if (TOKEN_IS(state, TOKEN_COLON)) {
        advance_token(state);
        var_type = parse_type_specifier(state, true);
        if (!var_type) {
            free(state_modifier); free(access_mod); free(name);
            if (dimlist) parser__free_ast(dimlist);
            return NULL;
        }
        var_type->pointer_level += ptr_lvl;
        var_type->is_reference = ref;
    } else if (ptr_lvl || ref || dimcnt > 0) {
        var_type = create_temp_type_with_prefixes(state, ptr_lvl, dummy, ref);
        if (!var_type) {
            free(state_modifier); free(access_mod); free(name);
            if (dimlist) parser__free_ast(dimlist);
            return NULL;
        }
    }

    if (dimcnt > 0) {
        if (!var_type) {
            var_type = create_temp_type_with_prefixes(state, 0, 0, 0);
            if (!var_type) {
                free(state_modifier); free(access_mod); free(name);
                return NULL;
            }
        }
        ASTNode *dims_node = create_ast_node(state, AST_MULTI_INITIALIZER,
                                             0, NULL, NULL, NULL,
                                             (ASTNode*)dimlist);
        if (!dims_node) {
            parser__free_ast(dimlist);
            free(state_modifier); free(access_mod); free(name);
            parser__free_type(var_type);
            return NULL;
        }
        var_type->array_dimensions = dims_node;
        var_type->is_array = 1;
    }

    ASTNode *init = NULL;
    ASTNode *var_body = NULL;
    if (strcmp(state_modifier, "def") == 0) {
        if (TOKEN_IS(state, TOKEN_EQUAL)) {
            advance_token(state);
            init = parse_expression(state);
            if (!init) {
                free(state_modifier); free(access_mod); free(name);
                parser__free_type(var_type);
                return NULL;
            }
        } else if (TOKEN_IS(state, TOKEN_LCURLY)) {
            /* Struct definition body stored as the variable body */
            var_body = parse_block(state);
            if (!var_body) {
                free(state_modifier); free(access_mod); free(name);
                parser__free_type(var_type);
                return NULL;
            }
        }
    }

    ASTNode *decl = create_ast_node(state, AST_VARIABLE_DECLARATION,
                                    0, name, NULL, var_body, NULL);
    if (!decl) {
        free(state_modifier); free(access_mod); free(name);
        parser__free_type(var_type);
        if (init) parser__free_ast_node(init);
        if (var_body) parser__free_ast_node(var_body);
        return NULL;
    }
    decl->state_modifier = state_modifier;
    decl->access_modifier = access_mod;
    decl->is_const = is_const;
    decl->variable_type = var_type;
    decl->default_value = init;
    return decl;
}

/*
 * Consume optional access modifier and const qualifier tokens.
 * `acc` receives a duplicated string for the access modifier (or NULL),
 * `cnst` becomes true if `const` was seen.
 */
static void parse_decl_special_modifiers(ParserState *state,
                                         char **acc, bool *cnst) {
    *acc = NULL;
    *cnst = false;
    bool saw_acc = false, saw_c = false;
    while (1) {
        TokenType t = get_current_token_type(state);
        if (t == TOKEN_STATEMOD) {
            const char *mod = get_current_token(state)->value;
            if (strcmp(mod, "const") == 0) {
                if (!saw_c) { *cnst = true; saw_c = true; }
                advance_token(state);
            } else {
                if (!saw_acc) {
                    *acc = STRDUP(state, mod);
                    saw_acc = true;
                }
                advance_token(state);
            }
        } else break;
    }
}

/*
 * Parse a single function parameter. May optionally be prefixed by a state
 * modifier (`def`, `del`, `pro`). If no identifier is present but a type
 * specifier can be parsed, it is treated as an unnamed parameter (a type
 * literal). Otherwise it is parsed as an expression.
 */
static ASTNode *parse_parameter(ParserState *state) {
    char *state_mod = NULL;
    if (TOKEN_IS(state, TOKEN_STATE)) {
        state_mod = STRDUP(state, get_current_token(state)->value);
        advance_token(state);
    }

    if (TOKEN_IS(state, TOKEN_ID)) {
        char *nm = STRDUP(state, get_current_token(state)->value);
        advance_token(state);
        Type *tp = NULL;
        if (TOKEN_IS(state, TOKEN_COLON)) {
            advance_token(state);
            tp = parse_type_specifier(state, true);
            if (!tp) { free(nm); free(state_mod); return NULL; }
        }
        ASTNode *node = create_ast_node(state, AST_VARIABLE_DECLARATION,
                                        0, nm, NULL, NULL, NULL);
        if (!node) { free(nm); free(state_mod); parser__free_type(tp); return NULL; }
        node->variable_type = tp;
        node->state_modifier = state_mod ? state_mod : STRDUP(state, "var");
        return node;
    }

    if (state_mod) {
        PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                    "Expected parameter name after '%s'", state_mod);
        free(state_mod);
        return NULL;
    }

    /* Fallback: unnamed parameter – try parsing a type literal */
    int saved = state->current_token_position;
    Type *lit = parse_type_specifier_silent(state, true, true);
    if (lit) {
        if (TOKEN_IS(state, TOKEN_COMMA) || TOKEN_IS(state, TOKEN_RPAREN)) {
            ASTNode *node = create_ast_node(state, AST_LITERAL_VALUE,
                                            TOKEN_TYPE,
                                            STRDUP(state, lit->name),
                                            NULL, NULL, NULL);
            parser__free_type(lit);
            return node;
        } else {
            parser__free_type(lit);
            state->current_token_position = saved;
        }
    }
    return parse_expression(state);
}

/*
 * Parse a parameter list enclosed in parentheses.
 * Returns an AST list containing AST_VARIABLE_DECLARATION nodes (or type
 * literal nodes for unnamed parameters).
 */
static AST *parse_parameter_list(ParserState *state) {
    REQUIRE(state, TOKEN_LPAREN);
    AST *list = create_empty_ast_list(state);
    if (!list) return NULL;

    /* `none` keyword for an empty parameter list */
    if (TOKEN_IS(state, TOKEN_NONE)) {
        advance_token(state);
        REQUIRE(state, TOKEN_RPAREN);
        return list;
    }
    if (TOKEN_IS(state, TOKEN_RPAREN)) {
        advance_token(state);
        return list;
    }

    while (!TOKEN_IS(state, TOKEN_RPAREN) && !TOKEN_IS(state, TOKEN_EOF)) {
        if (state->panic_mode) {
            skip_to_sync_token(state);
            if (TOKEN_IS(state, TOKEN_RPAREN)) break;
        }
        ASTNode *p = parse_parameter(state);
        if (!p) { parser__free_ast(list); return NULL; }
        if (!add_to_list_or_fail(state, list, p)) return NULL;
        if (TOKEN_IS(state, TOKEN_COMMA)) {
            advance_token(state);
            if (TOKEN_IS(state, TOKEN_RPAREN)) break;
        } else if (!TOKEN_IS(state, TOKEN_RPAREN)) {
            parser__free_ast(list);
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Expected ',' or ')'");
            return NULL;
        }
    }
    REQUIRE(state, TOKEN_RPAREN);
    return list;
}

/*
 * Parse either a full block (inside braces) or a single statement after
 * the `then` keyword. Returns an AST_BLOCK node in both cases (the latter
 * wraps the single statement in a block).
 */
static ASTNode *parse_block_or_statement(ParserState *state) {
    if (TOKEN_IS(state, TOKEN_LCURLY)) return parse_block(state);
    if (TOKEN_IS(state, TOKEN_THEN)) {
        advance_token(state);
        ASTNode *stmt = parse_statement(state);
        if (!stmt) return NULL;
        AST *list = create_empty_ast_list(state);
        if (!list) { parser__ast_node_pool_free(state->pool, stmt); return NULL; }
        if (!add_to_list_or_fail(state, list, stmt)) return NULL;
        return create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL,
                               (ASTNode*)list);
    }
    return NULL;
}

/*
 * Dispatch to the appropriate statement parser based on the current token.
 * Falls back to expression parsing (expression statement) if no keyword
 * matches.
 */
static ASTNode *parse_statement(ParserState *state) {
    if (TOKEN_IS(state, TOKEN_SEMICOLON)) { advance_token(state); return NULL; }
    TokenType tt = get_current_token_type(state);

    /* State declarations (`def`, `del`, `pro` ...) */
    if (tt == TOKEN_STATE)
        return parse_state_statement(state);

    /* Label declaration: identifier ':' (only if not a type specifier) */
    if (tt == TOKEN_ID) {
        int saved = state->current_token_position;
        Token *id_tok = get_current_token(state);

        if (state->current_token_position + 1 < state->total_tokens) {
            Token *next = &state->token_stream[state->current_token_position + 1];
            if (next->type == TOKEN_COLON) {
                int after_col = state->current_token_position + 2;
                bool is_type = false;
                if (after_col < state->total_tokens) {
                    Token *t3 = &state->token_stream[after_col];
                    if (t3->type == TOKEN_TYPE || t3->type == TOKEN_ID ||
                        t3->type == TOKEN_TYPEMOD || t3->type == TOKEN_STATEMOD ||
                        t3->type == TOKEN_AT || t3->type == TOKEN_AMPERSAND ||
                        t3->type == TOKEN_TYPEOF) {
                        is_type = true;
                    }
                }
                if (!is_type) {
                    char *lbl = STRDUP(state, id_tok->value);
                    if (!lbl) return NULL;
                    advance_token(state);
                    advance_token(state);
                    return create_ast_node(state, AST_LABEL_DECLARATION,
                                           0, lbl, NULL, NULL, NULL);
                }
            }
        }
        state->current_token_position = saved;
    }

    /* Keyword statements */
    switch (tt) {
        case TOKEN_LCURLY:    return parse_block(state);
        case TOKEN_IF:        return parse_if_statement(state);
        case TOKEN_DO:        return parse_do_loop(state);
        case TOKEN_BREAK:     return parse_break_statement(state);
        case TOKEN_CONTINUE:  return parse_continue_statement(state);
        case TOKEN_RETURN:    return parse_return_statement(state);
        case TOKEN_FREE:      return parse_free_statement(state);
        case TOKEN_JUMP:      return parse_jump_statement(state);
        case TOKEN_SIGNAL:    return parse_signal_statement(state);
        case TOKEN_NOP:       return parse_nop_statement(state);
        case TOKEN_ASM:       return parse_asm_statement(state);
        case TOKEN_ELSE:      return parse_else_statement(state);
        default: break;
    }

    /* Fallback – treat as an expression statement */
    int sp = state->current_token_position;
    ASTNode *expr = parse_expression(state);
    if (!expr) {
        state->current_token_position = sp;
        Token *et = get_current_token(state);
        if (et) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                            "Invalid statement starting with '%s'", et->value);
        else UNEXPECTED_EOF(state);
        skip_to_sync_token(state);
    }
    return expr;
}

/*
 * Parse a sequence of statements until an optional closing curly brace
 * (`expect_rc` == true) or EOF is reached. Returns an AST_BLOCK node
 * containing the list of statements.
 */
static ASTNode *parse_sequence(ParserState *state, bool expect_rc) {
    AST *list = create_empty_ast_list(state);
    if (!list) return NULL;

    while (!(expect_rc && TOKEN_IS(state, TOKEN_RCURLY)) &&
           !TOKEN_IS(state, TOKEN_EOF)) {
        if (state->panic_mode) {
            skip_to_sync_token(state);
            if (expect_rc && TOKEN_IS(state, TOKEN_RCURLY)) break;
        }
        ASTNode *stmt = parse_statement(state);
        if (stmt) {
            if (!add_to_list_or_fail(state, list, stmt)) return NULL;
            if (statement_requires_semicolon(stmt)) parse_semicolon(state);
            else if (TOKEN_IS(state, TOKEN_SEMICOLON)) advance_token(state);
        } else {
            if (TOKEN_IS(state, TOKEN_SEMICOLON)) advance_token(state);
        }
    }

    ASTNode *block = create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL,
                                     (ASTNode*)list);
    if (!block) { parser__free_ast(list); return NULL; }
    return block;
}

/*
 * Determine whether a semicolon is required after the given statement.
 */
static bool statement_requires_semicolon(ASTNode *node) {
    if (!node) return false;
    switch (node->type) {
        case AST_VARIABLE_LIST:
        case AST_BLOCK:
        case AST_IF_STATEMENT:
        case AST_DO_LOOP:
        case AST_LABEL_DECLARATION:
        case AST_ELSE_STATEMENT:
            return false;
        case AST_FUNCTION_DECLARATION:
            /* Only require a semicolon if the function has no body (declaration) */
            return (node->right == NULL);
        default:
            return true;
    }
}

/*
 * Consume a semicolon. If the next token is not a semicolon, report a
 * missing semicolon error and enter panic mode.
 */
static void parse_semicolon(ParserState *state) {
    if (!TOKEN_IS(state, TOKEN_SEMICOLON)) {
        Token *cur = get_current_token(state);
        if (cur)
            errhandler__report_error(ERROR_CODE_SYNTAX_MISSING_SEMICOLON,
                                     cur->line, cur->column, "syntax",
                                     "Expected ';'");
        else
            errhandler__report_error(ERROR_CODE_SYNTAX_MISSING_SEMICOLON,
                                     0, 0, "syntax",
                                     "Expected ';' at end of file");
        state->panic_mode = true;
    } else advance_token(state);
}

/*
 * Parse the entire token stream and return the resulting AST.
 * Performs error recovery and continues parsing as long as possible.
 * If a fatal error occurs (e.g., out of memory) NULL is returned.
 */
AST *parse(Token *tokens, uint16_t token_count) {
    ParserState state;
    state.current_token_position = 0;
    state.token_stream = tokens;
    state.total_tokens = token_count;
    state.pool = parser__ast_node_pool_create(AST_POOL_INIT_CAP);
    state.panic_mode = false;
    state.fatal_error = false;
    if (!state.pool) return NULL;

    AST *ast = ALLOC(&state, sizeof(AST));
    if (!ast) { parser__ast_node_pool_destroy(state.pool); return NULL; }
    memset(ast, 0, sizeof(AST));
    ast->pool = state.pool;

    while (get_current_token_type(&state) != TOKEN_EOF) {
        if (state.fatal_error) {
            /* An unrecoverable error occurred – discard everything and exit. */
            parser__free_ast(ast);
            return NULL;
        }
        if (state.panic_mode) skip_to_sync_token(&state);

        ASTNode *stmt = parse_statement(&state);
        if (stmt) {
            if (!add_ast_node_to_list(ast, stmt)) {
                parser__ast_node_pool_free(state.pool, stmt);
                parser__free_ast(ast);
                return NULL;
            }
            if (statement_requires_semicolon(stmt)) parse_semicolon(&state);
            else if (TOKEN_IS(&state, TOKEN_SEMICOLON)) advance_token(&state);
        } else {
            ast->had_errors = true;
            if (TOKEN_IS(&state, TOKEN_SEMICOLON)) advance_token(&state);
            else if (state.current_token_position < state.total_tokens)
                state.current_token_position++;
        }
    }

    if (state.fatal_error) { parser__free_ast(ast); return NULL; }
    return ast;
}

/*
 * Recursively free a Type structure and all its owned memory.
 */
void parser__free_type(Type *type) {
    if (!type) return;
    free(type->name);
    free(type->access_modifier);
    if (type->modifiers) {
        for (uint8_t i = 0; i < type->modifier_count; i++)
            free(type->modifiers[i]);
        free(type->modifiers);
    }
    if (type->array_dimensions) parser__free_ast_node(type->array_dimensions);
    if (type->angle_expression) parser__free_ast_node(type->angle_expression);
    if (type->typeof_expression) parser__free_ast_node(type->typeof_expression);
    free(type);
}

/*
 * Recursively free an AST node and all its children.
 * For node types that store an AST list in `extra` (BLOCK,
 * MULTI_INITIALIZER, FUNCTION_CALL, etc.), the list is freed as well.
 */
void parser__free_ast_node(ASTNode *node) {
    if (!node) return;
    if (node->left)  parser__free_ast_node(node->left);
    if (node->right) parser__free_ast_node(node->right);
    if (node->extra) {
        switch (node->type) {
            case AST_BLOCK:
            case AST_MULTI_INITIALIZER:
            case AST_VARIABLE_LIST:
            case AST_FUNCTION_CALL:
            case AST_ALLOC:
            case AST_REALLOC: {
                AST *list = (AST*)node->extra;
                if (list->nodes) {
                    for (uint16_t i = 0; i < list->count; i++)
                        parser__free_ast_node(list->nodes[i]);
                    free(list->nodes);
                }
                free(list);
                break;
            }
            default: parser__free_ast_node((ASTNode*)node->extra); break;
        }
    }
    free(node->value);
    free(node->state_modifier);
    free(node->access_modifier);
    if (node->variable_type) parser__free_type(node->variable_type);
}

/*
 * Free the entire AST tree and its memory pool.
 */
void parser__free_ast(AST *ast) {
    if (!ast) return;
    for (uint16_t i = 0; i < ast->count; i++)
        if (ast->nodes[i]) parser__free_ast_node(ast->nodes[i]);
    free(ast->nodes);
    if (ast->pool) parser__ast_node_pool_destroy(ast->pool);
    free(ast);
}

/*
 * Create a new node pool with the given initial capacity.
 */
ASTNodePool *parser__ast_node_pool_create(uint16_t cap) {
    ASTNodePool *p = malloc(sizeof(ASTNodePool));
    if (!p) return NULL;
    p->nodes = malloc(cap * sizeof(ASTNode));
    p->free_list = malloc(cap * sizeof(uint16_t));
    if (!p->nodes || !p->free_list) {
        free(p->nodes); free(p->free_list); free(p);
        return NULL;
    }
    p->capacity = cap;
    p->free_top = 0;
    for (uint16_t i = 0; i < cap; i++) p->free_list[p->free_top++] = i;
    return p;
}

/*
 * Destroy a node pool and release all memory associated with it.
 */
void parser__ast_node_pool_destroy(ASTNodePool *p) {
    if (!p) return;
    free(p->nodes);
    free(p->free_list);
    free(p);
}

/*
 * Allocate a single node from the pool (no expansion).
 * Returns NULL if the pool is full.
 */
ASTNode *parser__ast_node_pool_alloc(ASTNodePool *p) {
    if (!p || p->free_top == 0) return NULL;
    uint16_t idx = p->free_list[--p->free_top];
    ASTNode *n = &p->nodes[idx];
    memset(n, 0, sizeof(ASTNode));
    return n;
}

/*
 * Return a node to the pool after zeroing its contents.
 */
void parser__ast_node_pool_free(ASTNodePool *p, ASTNode *node) {
    if (!p || !node) return;
    uint16_t idx = (uint16_t)(node - p->nodes);
    if (idx < p->capacity && p->free_top < p->capacity) {
        memset(node, 0, sizeof(ASTNode));
        p->free_list[p->free_top++] = idx;
    }
}
