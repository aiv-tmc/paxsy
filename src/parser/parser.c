#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "parser.h"

#define MAX_MODIFIERS           8    /* maximum number of type modifiers       */
#define AST_INITIAL_CAPACITY    4    /* initial capacity of an AST node list   */
#define POOL_CHUNK_SIZE         64   /* nodes per pool chunk                   */

#define ARRAY_SIZE(arr)         (sizeof(arr) / sizeof((arr)[0]))

/* Token checks – use single‑token pushback */
#define TOKEN_IS(state, t)      (get_current_token_type(state) == (t))
#define SKIP_IF(state, t)       (TOKEN_IS(state, t) ? (advance_token(state), 1) : 0)
#define REQUIRE(state, t)       do { if (!expect_token(state, t)) return NULL; } while(0)

/* Memory wrappers – report fatal parser error on failure */
#define ALLOC(state, sz)        safe_malloc(state, sz)
#define REALLOC(state, p, sz)   safe_realloc(state, p, sz)
#define STRDUP(state, s)        safe_strdup(state, s)

/* AST node construction shortcuts */
#define AST_NEW_BINARY(state, op, left, right) \
    create_ast_node(state, AST_BINARY_OPERATION, op, NULL, left, right, NULL)

#define AST_NEW_UNARY(state, op, operand) \
    create_ast_node(state, AST_UNARY_OPERATION, op, NULL, NULL, operand, NULL)

#define AST_NEW_ASSIGNMENT(state, op, left, right) \
    create_ast_node(state, ((op) == TOKEN_EQUAL) ? AST_ASSIGNMENT : AST_COMPOUND_ASSIGNMENT, \
                    op, NULL, left, right, NULL)

#define AST_NEW_TERNARY(state, cond, true_expr, false_expr) \
    create_ast_node(state, AST_TERNARY_OPERATION, 0, NULL, cond, true_expr, false_expr)

/* Destruction helpers – free a subtree or list and then return NULL */
#define FREE_NODE_RETURN_NULL(state, node) do {          \
    free_node_recursive(node, (state)->pool);            \
    return NULL;                                         \
} while(0)

#define FREE_LIST_RETURN_NULL(state, list) do {          \
    parser__free_ast(list);                              \
    return NULL;                                         \
} while(0)

/* Append a node to a dynamic AST list; on failure free both and return NULL */
#define APPEND_OR_FAIL(state, list, node)                                 \
    do {                                                                  \
        if (!add_to_list_or_fail(state, list, node)) {                    \
            free_node_recursive(node, (state)->pool);                     \
            parser__free_ast(list);                                       \
            return NULL;                                                  \
        }                                                                 \
    } while(0)

/* Error reporting macros */
#define REPORT_ERR(state, level, code, ctx, ...) do {                     \
    Token *cur = get_current_token(state);                                \
    if (cur)                                                              \
        errhandler__report_error_ex(level, code, cur->line, cur->column,  \
                                    (int)strlen(cur->value), ctx, __VA_ARGS__); \
    else                                                                  \
        errhandler__report_error_ex(level, code, 0, 0, 0, ctx, __VA_ARGS__); \
} while(0)

#define PARSE_ERROR(state, code, ...) \
    REPORT_ERR(state, ERROR_LEVEL_ERROR, code, "syntax", __VA_ARGS__)

#define FATAL_ERROR(state, code, ...) do {                                \
    state->fatal_error = true;                                            \
    REPORT_ERR(state, ERROR_LEVEL_FATAL, code, "syntax", __VA_ARGS__);   \
} while(0)

#define UNEXPECTED_TOKEN(state, exp, act) do {                             \
    Token *cur = get_current_token(state);                                \
    if (cur)                                                              \
        errhandler__report_error_ex(ERROR_LEVEL_ERROR,                    \
                                    ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN,   \
                                    cur->line, cur->column,               \
                                    (int)strlen(cur->value),              \
                                    "syntax",                             \
                                    "Expected %s but got %s (value: '%s')", \
                                    exp, act, cur->value);                \
    else                                                                  \
        errhandler__report_error_ex(ERROR_LEVEL_ERROR,                    \
                                    ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN,   \
                                    0, 0, 0, "syntax",                  \
                                    "Expected %s but got EOF", exp);      \
    state->panic_mode = true;                                             \
} while(0)

#define UNEXPECTED_EOF(state) \
    PARSE_ERROR(state, ERROR_CODE_SYNTAX_UNEXPECTED_EOF, "Unexpected end of file")

static void free_node_recursive(ASTNode *node, ASTNodePool *pool);

/* Token stream access with single‑token pushback */
static TokenType get_current_token_type(ParserState *state);
static void      advance_token(ParserState *state);
static Token    *get_current_token(ParserState *state);
static bool      expect_token(ParserState *state, TokenType expected);

/* Safe memory allocation */
static void *safe_malloc(ParserState *state, size_t size);
static void *safe_realloc(ParserState *state, void *ptr, size_t size);
static char *safe_strdup(ParserState *state, const char *str);

/* Pool helper */
static void expand_pool(ParserState *state);

/* AST list helpers */
static ASTNode *create_ast_node(ParserState *state, ASTNodeType type,
                                TokenType op, char *value,
                                ASTNode *left, ASTNode *right,
                                ASTNode *extra);
static bool add_ast_node_to_list(AST *ast, ASTNode *node);
static AST *create_empty_ast_list(ParserState *state);
static bool add_to_list_or_fail(ParserState *state, AST *list, ASTNode *node);
static void ast_list_shrink_to_fit(AST *ast);

/* Error recovery */
static void skip_to_sync_token(ParserState *state);
static bool token_can_start_expression(TokenType t);

/* Type helpers */
static bool is_keyword_void(ParserState *state);
static bool is_arrow_token(ParserState *state);
static bool is_type_modifier(const char *val);
static void parse_type_prefixes(ParserState *state, uint8_t *ptr_lvl, uint8_t *ref);
static void apply_prefixes_to_type(Type *type, uint8_t ptr_lvl, uint8_t ref);
static Type *create_temp_type_with_prefixes(ParserState *state,
                                            uint8_t ptr_lvl, uint8_t ref);
static Type *parse_type_specifier_silent(ParserState *state,
                                         bool silent, bool parse_prefixes);
static Type *parse_type_specifier(ParserState *state, bool parse_prefixes);

/* Special forms */
static ASTNode *parse_fixed_args_func(ParserState *state, ASTNodeType type,
                                      uint8_t arg_cnt);
static ASTNode *parse_alloc_expression(ParserState *state);
static ASTNode *parse_realloc_expression(ParserState *state);

/* Expression parsing */
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
static ASTNode *parse_postfix_expression(ParserState *state, ASTNode *node);
static ASTNode *parse_binary_op(ParserState *state,
                                ASTNode *(*parse_op)(ParserState *),
                                const TokenType *ops, uint8_t cnt);

/* Statement & block parsing */
static ASTNode *parse_block(ParserState *state);
static ASTNode *parse_multi_initializer(ParserState *state);
static ASTNode *parse_conditional(ParserState *state, ASTNodeType ntype);
static ASTNode *parse_break_statement(ParserState *state);
static ASTNode *parse_continue_statement(ParserState *state);
static ASTNode *parse_nop_statement(ParserState *state);
static ASTNode *parse_signal_statement(ParserState *state);
static ASTNode *parse_halt_statement(ParserState *state);
static ASTNode *parse_kill_statement(ParserState *state);
static ASTNode *parse_asm_statement(ParserState *state);
static ASTNode *parse_jump_statement(ParserState *state);
static ASTNode *parse_return_statement(ParserState *state);
static ASTNode *parse_free_statement(ParserState *state);
static ASTNode *parse_else_statement(ParserState *state);
static ASTNode *parse_state_statement(ParserState *state);
static ASTNode *parse_parameter(ParserState *state);
static AST *parse_parameter_list(ParserState *state);
static ASTNode *parse_block_or_statement(ParserState *state);
static ASTNode *parse_statement(ParserState *state);
static bool statement_requires_semicolon(ASTNode *node);
static void parse_decl_special_modifiers(ParserState *state,
                                         char **acc, bool *cnst);
static void parse_semicolon(ParserState *state);
static ASTNode *parse_sequence(ParserState *state, bool expect_rcurly);

/* Comma‑separated expression list → single node or MULTI_INITIALIZER */
static ASTNode *parse_expression_or_multi(ParserState *state, bool allow_empty);

/* Body / initializer for `def` entities */
static ASTNode *parse_body_or_initializer(ParserState *state, bool is_function);

static TokenType get_current_token_type(ParserState *state) {
    if (state->has_pushback)
        return state->pushback_token.type;
    return (state->current_token_position < state->total_tokens)
           ? state->token_stream[state->current_token_position].type
           : TOKEN_EOF;
}

static void advance_token(ParserState *state) {
    if (state->has_pushback) {
        state->has_pushback = false;
    } else {
        if (state->current_token_position < state->total_tokens - 1)
            state->current_token_position++;
    }
}

static Token *get_current_token(ParserState *state) {
    if (state->has_pushback)
        return &state->pushback_token;
    return (state->current_token_position < state->total_tokens)
           ? &state->token_stream[state->current_token_position]
           : NULL;
}

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

static void *safe_malloc(ParserState *state, size_t size) {
    void *ptr = malloc(size);
    if (!ptr && size > 0)
        FATAL_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION,
                    "Memory allocation failed");
    return ptr;
}

static void *safe_realloc(ParserState *state, void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0)
        FATAL_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION,
                    "Memory reallocation failed");
    return new_ptr;
}

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

static void expand_pool(ParserState *state) {
    ASTNodePool *p = state->pool;
    ASTNode *chunk = malloc(POOL_CHUNK_SIZE * sizeof(ASTNode));
    if (!chunk)
        FATAL_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION,
                    "Pool chunk allocation failed");

    if (p->chunk_count >= p->chunk_capacity) {
        uint16_t new_cap = p->chunk_capacity ? p->chunk_capacity * 2 : 4;
        ASTNode **new_chunks = realloc(p->chunks, new_cap * sizeof(ASTNode *));
        if (!new_chunks) {
            free(chunk);
            FATAL_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION,
                        "Pool chunk list reallocation failed");
        }
        p->chunks = new_chunks;
        p->chunk_capacity = new_cap;
    }
    p->chunks[p->chunk_count++] = chunk;

    /* Insert all nodes into the free list (reverse order for LIFO) */
    for (int i = POOL_CHUNK_SIZE - 1; i >= 0; i--) {
        ASTNode *node = &chunk[i];
        node->left = (ASTNode *)p->free_head;
        p->free_head = node;
    }
}

ASTNodePool *parser__ast_node_pool_create(uint16_t initial_capacity) {
    (void)initial_capacity;
    ASTNodePool *p = malloc(sizeof(ASTNodePool));
    if (!p) return NULL;

    p->free_head      = NULL;
    p->chunks         = NULL;
    p->chunk_count    = 0;
    p->chunk_capacity = 0;

    /* Pre‑allocate the first chunk */
    ASTNode *first = malloc(POOL_CHUNK_SIZE * sizeof(ASTNode));
    if (!first) { free(p); return NULL; }

    p->chunks = malloc(sizeof(ASTNode *));
    if (!p->chunks) { free(first); free(p); return NULL; }
    p->chunks[0]     = first;
    p->chunk_count    = 1;
    p->chunk_capacity = 1;

    for (int i = POOL_CHUNK_SIZE - 1; i >= 0; i--) {
        ASTNode *n = &first[i];
        n->left = (ASTNode *)p->free_head;
        p->free_head = n;
    }
    return p;
}

void parser__ast_node_pool_destroy(ASTNodePool *p) {
    if (!p) return;
    for (uint16_t i = 0; i < p->chunk_count; i++)
        free(p->chunks[i]);
    free(p->chunks);
    free(p);
}

/* Non‑expanding allocator – used by optimizer passes that cannot signal fatal errors */
ASTNode *parser__ast_node_pool_alloc(ASTNodePool *pool) {
    if (!pool || !pool->free_head) return NULL;
    ASTNode *node = pool->free_head;
    pool->free_head = node->left;
    memset(node, 0, sizeof(ASTNode));
    return node;
}

/* Expanding allocator – used by the parser; signals fatal error if memory exhausted */
ASTNode *parser__ast_node_pool_alloc_or_expand(ParserState *state) {
    ASTNodePool *p = state->pool;
    if (!p->free_head)
        expand_pool(state);
    ASTNode *node = p->free_head;
    p->free_head = node->left;
    memset(node, 0, sizeof(ASTNode));
    return node;
}

void parser__ast_node_pool_free(ASTNodePool *p, ASTNode *node) {
    if (!p || !node) return;
    memset(node, 0, sizeof(ASTNode));
    node->left = p->free_head;
    p->free_head = node;
}

static ASTNode *create_ast_node(ParserState *state, ASTNodeType node_type,
                                TokenType operation_type, char *node_value,
                                ASTNode *left_child, ASTNode *right_child,
                                ASTNode *extra_node) {
    ASTNode *node = parser__ast_node_pool_alloc_or_expand(state);
    node->type           = node_type;
    node->operation_type = operation_type;
    node->value          = node_value;
    node->left           = left_child;
    node->right          = right_child;
    node->extra          = extra_node;

    Token *cur = get_current_token(state);
    if (cur) {
        node->line   = cur->line;
        node->column = cur->column;
    }
    return node;
}

static bool add_ast_node_to_list(AST *ast, ASTNode *node) {
    if (ast->count >= ast->capacity) {
        uint16_t new_cap = ast->capacity ? ast->capacity * 2 : AST_INITIAL_CAPACITY;
        ASTNode **new_nodes = realloc(ast->nodes, new_cap * sizeof(ASTNode *));
        if (!new_nodes) return false;
        ast->nodes = new_nodes;
        ast->capacity = new_cap;
    }
    ast->nodes[ast->count++] = node;
    return true;
}

static void ast_list_shrink_to_fit(AST *ast) {
    if (!ast || ast->count == ast->capacity) return;
    if (ast->count == 0) {
        free(ast->nodes);
        ast->nodes    = NULL;
        ast->capacity = 0;
    } else {
        ASTNode **new_nodes = realloc(ast->nodes, ast->count * sizeof(ASTNode *));
        if (new_nodes) {
            ast->nodes    = new_nodes;
            ast->capacity = ast->count;
        }
    }
}

static AST *create_empty_ast_list(ParserState *state) {
    AST *list = ALLOC(state, sizeof(AST));
    if (!list) return NULL;
    memset(list, 0, sizeof(AST));
    return list;
}

static bool add_to_list_or_fail(ParserState *state, AST *list, ASTNode *node) {
    (void)state;
    return add_ast_node_to_list(list, node);
}

static void free_node_recursive(ASTNode *node, ASTNodePool *pool) {
    if (!node) return;

    if (node->left)  free_node_recursive(node->left, pool);
    if (node->right) free_node_recursive(node->right, pool);

    /* Handle extra data that contains AST lists */
    if (node->extra) {
        switch (node->type) {
            case AST_BLOCK:
            case AST_MULTI_INITIALIZER:
            case AST_FUNCTION_CALL:
            case AST_ALLOC:
            case AST_REALLOC: {
                AST *list = (AST *)node->extra;
                for (uint16_t i = 0; i < list->count; i++)
                    free_node_recursive(list->nodes[i], pool);
                free(list->nodes);
                free(list);
                break;
            }
            default:
                free_node_recursive((ASTNode *)node->extra, pool);
                break;
        }
    }

    free(node->value);
    free(node->state_modifier);
    free(node->access_modifier);
    if (node->variable_type) parser__free_type(node->variable_type);
    if (node->default_value) free_node_recursive(node->default_value, pool);

    if (pool) {
        parser__ast_node_pool_free(pool, node);
    } else {
        memset(node, 0, sizeof(ASTNode));
    }
}

/* Public convenience wrapper for external code */
void parser__free_ast_node(ASTNode *node, ASTNodePool *pool) {
    free_node_recursive(node, pool);
}

void parser__free_type(Type *type) {
    if (!type) return;
    free(type->name);
    if (type->modifiers) {
        for (uint8_t i = 0; i < type->modifier_count; i++)
            free(type->modifiers[i]);
        free(type->modifiers);
    }
    if (type->array_dimensions) free_node_recursive(type->array_dimensions, NULL);
    if (type->angle_expression)  free_node_recursive(type->angle_expression, NULL);
    if (type->typeof_expression) free_node_recursive(type->typeof_expression, NULL);
    if (type->struct_definition) free_node_recursive(type->struct_definition, NULL);
    free(type);
}

void parser__free_ast(AST *ast) {
    if (!ast) return;
    for (uint16_t i = 0; i < ast->count; i++)
        if (ast->nodes[i])
            free_node_recursive(ast->nodes[i], ast->pool);
    free(ast->nodes);
    if (ast->pool) parser__ast_node_pool_destroy(ast->pool);
    free(ast);
}

static void skip_to_sync_token(ParserState *state) {
    state->panic_mode = true;
    while (state->current_token_position < state->total_tokens) {
        TokenType t = get_current_token_type(state);
        if (t == TOKEN_SEMICOLON || t == TOKEN_RCURLY || t == TOKEN_EOF)
            break;
        /* Statement‑starting tokens also serve as sync points */
        if (t == TOKEN_IF || t == TOKEN_DO || t == TOKEN_RETURN ||
            t == TOKEN_ELSE || t == TOKEN_BREAK || t == TOKEN_CONTINUE ||
            t == TOKEN_FREE || t == TOKEN_JUMP || t == TOKEN_SIGNAL ||
            t == TOKEN_NOP || t == TOKEN_ASM ||
            t == TOKEN_LCURLY || t == TOKEN_STATE ||
            t == TOKEN_TYPEMOD || t == TOKEN_STATEMOD)
            break;
        advance_token(state);
    }
    state->panic_mode = false;
}

static bool token_can_start_expression(TokenType t) {
    switch (t) {
        case TOKEN_NUMBER: case TOKEN_STRING: case TOKEN_CHAR:
        case TOKEN_NONE: case TOKEN_TYPE: case TOKEN_DOLLAR:
        case TOKEN_ID: case TOKEN_LPAREN: case TOKEN_LCURLY:
        case TOKEN_SIZEOF: case TOKEN_TYPEOF: case TOKEN_ALLOC:
        case TOKEN_REALLOC: case TOKEN_DOUBLE_PLUS: case TOKEN_DOUBLE_MINUS:
        case TOKEN_BANG: case TOKEN_TILDE: case TOKEN_STAR:
        case TOKEN_SLASH: case TOKEN_AT: case TOKEN_AMPERSAND:
            return true;
        default:
            return false;
    }
}

static bool is_keyword_void(ParserState *state) {
    Token *tok = get_current_token(state);
    if (!tok) return false;
    return (tok->type == TOKEN_TYPE || tok->type == TOKEN_NONE) &&
           (strcmp(tok->value, "Void") == 0 || strcmp(tok->value, "none") == 0);
}

static bool is_arrow_token(ParserState *state) {
    if (state->current_token_position + 1 < state->total_tokens &&
        !state->has_pushback) {
        Token *t1 = &state->token_stream[state->current_token_position];
        Token *t2 = &state->token_stream[state->current_token_position + 1];
        if (t1->type == TOKEN_MINUS && t2->type == TOKEN_GT) {
            state->current_token_position += 2;
            return true;
        }
    }
    Token *cur = get_current_token(state);
    if (cur && strcmp(cur->value, "->") == 0) {
        advance_token(state);
        return true;
    }
    return false;
}

static bool is_type_modifier(const char *val) {
    static const char *mods[] = {
        "unsigned", "signed", "long", "short", "const", "volatile"
    };
    for (int i = 0; i < 6; i++)
        if (strcmp(val, mods[i]) == 0) return true;
    return false;
}

static void parse_type_prefixes(ParserState *state, uint8_t *ptr_lvl, uint8_t *ref) {
    *ptr_lvl = 0;
    *ref = 0;
    while (1) {
        if (TOKEN_IS(state, TOKEN_AT)) {
            advance_token(state);
            (*ptr_lvl)++;
        } else if (TOKEN_IS(state, TOKEN_AMPERSAND)) {
            advance_token(state);
            *ref = 1;
        } else break;
    }
}

static void apply_prefixes_to_type(Type *type, uint8_t ptr_lvl, uint8_t ref) {
    if (!type) return;
    type->pointer_level = ptr_lvl;
    type->is_reference  = ref;
}

static Type *create_temp_type_with_prefixes(ParserState *state,
                                            uint8_t ptr_lvl, uint8_t ref) {
    Type *t = ALLOC(state, sizeof(Type));
    if (!t) return NULL;
    memset(t, 0, sizeof(Type));
    t->name = STRDUP(state, "int");
    t->pointer_level = ptr_lvl;
    t->is_reference  = ref;
    return t;
}

static Type *parse_type_specifier_silent(ParserState *state,
                                         bool silent, bool parse_prefixes) {
    Type *type = ALLOC(state, sizeof(Type));
    if (!type) return NULL;
    memset(type, 0, sizeof(Type));

    /* Collect modifier keywords */
    while (TOKEN_IS(state, TOKEN_TYPEMOD)) {
        Token *cur = get_current_token(state);
        if (!is_type_modifier(cur->value)) break;
        if (type->modifier_count >= MAX_MODIFIERS) {
            if (!silent) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                     "Too many type modifiers");
            parser__free_type(type);
            return NULL;
        }
        char **new_mods = REALLOC(state, type->modifiers,
                                  (type->modifier_count + 1) * sizeof(char *));
        if (!new_mods) { parser__free_type(type); return NULL; }
        type->modifiers = new_mods;
        type->modifiers[type->modifier_count] = STRDUP(state, cur->value);
        if (!type->modifiers[type->modifier_count]) {
            parser__free_type(type);
            return NULL;
        }
        type->modifier_count++;
        advance_token(state);
    }

    uint8_t ptr_lvl = 0, ref = 0;
    if (parse_prefixes) {
        parse_type_prefixes(state, &ptr_lvl, &ref);
        apply_prefixes_to_type(type, ptr_lvl, ref);
    }

    /* typeof(expr) */
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
            free_node_recursive(expr, NULL);
            parser__free_type(type);
            return NULL;
        }
        type->name = STRDUP(state, "typeof");
        type->typeof_expression = expr;
        apply_prefixes_to_type(type, ptr_lvl, ref);
        if (TOKEN_IS(state, TOKEN_LT) || TOKEN_IS(state, TOKEN_LBRACE)) {
            if (!silent) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                     "typeof cannot have angle brackets or array dimensions");
            parser__free_type(type);
            return NULL;
        }
        return type;
    }

    /* Base type name */
    if (TOKEN_IS(state, TOKEN_TYPE) || TOKEN_IS(state, TOKEN_ID) ||
        TOKEN_IS(state, TOKEN_TYPEMOD)) {
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

    /* Angle brackets: <...> */
    if (TOKEN_IS(state, TOKEN_LT)) {
        advance_token(state);
        if (TOKEN_IS(state, TOKEN_GT) || TOKEN_IS(state, TOKEN_GE)) {
            parser__free_type(type);
            if (!silent) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                     "Empty angle brackets not allowed");
            return NULL;
        }
        if (TOKEN_IS(state, TOKEN_NUMBER)) {
            long sz = atol(get_current_token(state)->value);
            if (sz <= 0 || sz > UINT8_MAX) {
                parser__free_type(type);
                if (!silent) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                         "Invalid type size: must be 1..%d", UINT8_MAX);
                return NULL;
            }
            type->size_in_bytes = (uint8_t)sz;
            advance_token(state);

            /* Handle >= operator disguised as > */
            if (TOKEN_IS(state, TOKEN_GE)) {
                state->pushback_token.type  = TOKEN_EQUAL;
                state->pushback_token.value = "=";
                state->pushback_token.line  = get_current_token(state)->line;
                state->pushback_token.column = get_current_token(state)->column;
                state->has_pushback = true;
                state->current_token_position++;
            } else if (TOKEN_IS(state, TOKEN_GT)) {
                advance_token(state);
            } else {
                if (!silent) UNEXPECTED_TOKEN(state, "'>'",
                                              token_names[get_current_token_type(state)]);
                parser__free_type(type);
                return NULL;
            }
        } else {
            /* Generic angle expression */
            ASTNode *expr = parse_expression(state);
            if (!expr) { parser__free_type(type); return NULL; }
            if (TOKEN_IS(state, TOKEN_COMMA)) {
                AST *list = create_empty_ast_list(state);
                if (!list) {
                    free_node_recursive(expr, NULL);
                    parser__free_type(type);
                    return NULL;
                }
                APPEND_OR_FAIL(state, list, expr);
                while (TOKEN_IS(state, TOKEN_COMMA)) {
                    advance_token(state);
                    if (TOKEN_IS(state, TOKEN_GT) || TOKEN_IS(state, TOKEN_GE))
                        break;
                    expr = parse_expression(state);
                    if (!expr) {
                        parser__free_ast(list);
                        parser__free_type(type);
                        return NULL;
                    }
                    APPEND_OR_FAIL(state, list, expr);
                }
                if (TOKEN_IS(state, TOKEN_GE)) {
                    state->pushback_token.type  = TOKEN_EQUAL;
                    state->pushback_token.value = "=";
                    state->pushback_token.line  = get_current_token(state)->line;
                    state->pushback_token.column = get_current_token(state)->column;
                    state->has_pushback = true;
                    state->current_token_position++;
                } else if (TOKEN_IS(state, TOKEN_GT)) {
                    advance_token(state);
                } else {
                    if (!silent) UNEXPECTED_TOKEN(state, "'>'",
                                                  token_names[get_current_token_type(state)]);
                    parser__free_ast(list);
                    parser__free_type(type);
                    return NULL;
                }
                ast_list_shrink_to_fit(list);
                type->angle_expression = create_ast_node(state, AST_MULTI_INITIALIZER,
                                                         0, NULL, NULL, NULL,
                                                         (ASTNode *)list);
            } else {
                if (TOKEN_IS(state, TOKEN_GE)) {
                    state->pushback_token.type  = TOKEN_EQUAL;
                    state->pushback_token.value = "=";
                    state->pushback_token.line  = get_current_token(state)->line;
                    state->pushback_token.column = get_current_token(state)->column;
                    state->has_pushback = true;
                    state->current_token_position++;
                } else if (TOKEN_IS(state, TOKEN_GT)) {
                    advance_token(state);
                } else {
                    if (!silent) UNEXPECTED_TOKEN(state, "'>'",
                                                  token_names[get_current_token_type(state)]);
                    free_node_recursive(expr, NULL);
                    parser__free_type(type);
                    return NULL;
                }
                type->angle_expression = expr;
            }
        }
    }
    return type;
}

static Type *parse_type_specifier(ParserState *state, bool parse_prefixes) {
    return parse_type_specifier_silent(state, false, parse_prefixes);
}

static ASTNode *parse_fixed_args_func(ParserState *state, ASTNodeType nt,
                                      uint8_t arg_cnt) {
    advance_token(state);
    REQUIRE(state, TOKEN_LPAREN);

    AST *args = create_empty_ast_list(state);
    if (!args) return NULL;

    for (uint8_t i = 0; i < arg_cnt; i++) {
        ASTNode *arg = parse_expression(state);
        if (!arg) { parser__free_ast(args); return NULL; }
        APPEND_OR_FAIL(state, args, arg);
        if (i < arg_cnt - 1) {
            if (!TOKEN_IS(state, TOKEN_COMMA)) {
                parser__free_ast(args);
                PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                            "Expected comma after argument %d", i + 1);
                return NULL;
            }
            advance_token(state);
        }
    }
    REQUIRE(state, TOKEN_RPAREN);

    ast_list_shrink_to_fit(args);
    ASTNode *args_block = create_ast_node(state, AST_BLOCK, 0, NULL,
                                          NULL, NULL, (ASTNode *)args);
    if (!args_block) { parser__free_ast(args); return NULL; }
    return create_ast_node(state, nt, 0, NULL, args_block, NULL, NULL);
}

static ASTNode *parse_alloc_expression(ParserState *state) {
    return parse_fixed_args_func(state, AST_ALLOC, 3);
}
static ASTNode *parse_realloc_expression(ParserState *state) {
    return parse_fixed_args_func(state, AST_REALLOC, 2);
}

static ASTNode *parse_expression_or_multi(ParserState *state, bool allow_empty) {
    AST *list = create_empty_ast_list(state);
    if (!list) return NULL;

    if (TOKEN_IS(state, TOKEN_RPAREN)) {
        if (!allow_empty) {
            Token *rp = get_current_token(state);
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_EMPTY_PARENS,
                        "Empty parentheses not allowed, use Void instead");
            advance_token(state);
        }
        ast_list_shrink_to_fit(list);
        list->had_errors = true;
        return create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL,
                               NULL, NULL, (ASTNode *)list);
    }

    ASTNode *first = parse_expression(state);
    if (!first) { FREE_LIST_RETURN_NULL(state, list); }
    APPEND_OR_FAIL(state, list, first);

    while (TOKEN_IS(state, TOKEN_COMMA)) {
        advance_token(state);
        ASTNode *next = parse_expression(state);
        if (!next) { FREE_LIST_RETURN_NULL(state, list); }
        APPEND_OR_FAIL(state, list, next);
    }

    ast_list_shrink_to_fit(list);
    if (list->count == 1) {
        ASTNode *single = list->nodes[0];
        free(list->nodes);
        free(list);
        return single;
    }
    return create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL,
                           NULL, NULL, (ASTNode *)list);
}

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
            FREE_NODE_RETURN_NULL(state, node);
            skip_to_sync_token(state);
            return NULL;
        }
        ASTNode *new_node = AST_NEW_BINARY(state, op, node, rhs);
        if (!new_node) {
            free_node_recursive(rhs, state->pool);
            FREE_NODE_RETURN_NULL(state, node);
            return NULL;
        }
        node = new_node;
    }
    return node;
}

static ASTNode *parse_expression(ParserState *state) {
    return parse_assignment_expression(state);
}

static ASTNode *parse_assignment_expression(ParserState *state) {
    ASTNode *left = parse_ternary_expression(state);
    if (!left) return NULL;

    if (left->type == AST_MULTI_INITIALIZER) {
        if (!TOKEN_IS(state, TOKEN_EQUAL)) return left;
        advance_token(state);
        ASTNode *right = parse_expression(state);
        if (!right) FREE_NODE_RETURN_NULL(state, left);
        return create_ast_node(state, AST_MULTI_ASSIGNMENT, 0, NULL,
                               left, right, NULL);
    }

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
            if (!right) FREE_NODE_RETURN_NULL(state, left);
            return AST_NEW_ASSIGNMENT(state, cur, left, right);
        }
    }
    return left;
}

static ASTNode *parse_ternary_expression(ParserState *state) {
    ASTNode *cond = parse_logical_expression(state);
    if (!cond) return NULL;
    if (TOKEN_IS(state, TOKEN_QUESTION)) {
        advance_token(state);
        ASTNode *true_expr = parse_expression(state);
        if (!true_expr) FREE_NODE_RETURN_NULL(state, cond);
        REQUIRE(state, TOKEN_COLON);
        ASTNode *false_expr = parse_ternary_expression(state);
        if (!false_expr) {
            free_node_recursive(true_expr, state->pool);
            FREE_NODE_RETURN_NULL(state, cond);
        }
        return AST_NEW_TERNARY(state, cond, true_expr, false_expr);
    }
    return cond;
}

static ASTNode *parse_logical_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_LOGICAL };
    return parse_binary_op(state, parse_bitwise_or_expression, ops, 1);
}
static ASTNode *parse_bitwise_or_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_PIPE };
    return parse_binary_op(state, parse_bitwise_xor_expression, ops, 1);
}
static ASTNode *parse_bitwise_xor_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_CARET };
    return parse_binary_op(state, parse_bitwise_and_expression, ops, 1);
}
static ASTNode *parse_bitwise_and_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_AMPERSAND };
    return parse_binary_op(state, parse_equality_expression, ops, 1);
}
static ASTNode *parse_equality_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_DOUBLE_EQ, TOKEN_NE };
    return parse_binary_op(state, parse_relational_expression, ops, 2);
}
static ASTNode *parse_relational_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_LT, TOKEN_GT, TOKEN_LE, TOKEN_GE };
    return parse_binary_op(state, parse_shift_expression, ops, 4);
}
static ASTNode *parse_shift_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_SHL, TOKEN_SHR, TOKEN_SAL,
                                     TOKEN_SAR, TOKEN_ROL, TOKEN_ROR };
    return parse_binary_op(state, parse_additive_expression, ops, 6);
}
static ASTNode *parse_additive_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_PLUS, TOKEN_MINUS };
    return parse_binary_op(state, parse_multiplicative_expression, ops, 2);
}
static ASTNode *parse_multiplicative_expression(ParserState *state) {
    static const TokenType ops[] = { TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT };
    return parse_binary_op(state, parse_unary_expression, ops, 3);
}

static ASTNode *parse_unary_expression(ParserState *state) {
    if (TOKEN_IS(state, TOKEN_DOUBLE_PLUS) || TOKEN_IS(state, TOKEN_DOUBLE_MINUS)) {
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

    ASTNode *prim = parse_primary_expression(state);
    if (!prim) return NULL;
    return parse_postfix_expression(state, prim);
}

static ASTNode *parse_primary_expression(ParserState *state) {
    Token *tok = get_current_token(state);
    if (!tok) { UNEXPECTED_EOF(state); skip_to_sync_token(state); return NULL; }

    switch (tok->type) {
        case TOKEN_LPAREN: {
            advance_token(state);
            ASTNode *expr = parse_expression(state);
            if (!expr) { skip_to_sync_token(state); return NULL; }
            REQUIRE(state, TOKEN_RPAREN);
            return expr;
        }
        case TOKEN_LCURLY:
            return parse_multi_initializer(state);
        case TOKEN_SIZEOF: {
            advance_token(state);
            REQUIRE(state, TOKEN_LPAREN);
            ASTNode *arg = parse_expression(state);
            if (!arg) return NULL;
            REQUIRE(state, TOKEN_RPAREN);
            return create_ast_node(state, AST_SIZEOF, 0, NULL, arg, NULL, NULL);
        }
        case TOKEN_TYPEOF: {
            advance_token(state);
            REQUIRE(state, TOKEN_LPAREN);
            ASTNode *expr = parse_expression(state);
            if (!expr) return NULL;
            REQUIRE(state, TOKEN_RPAREN);
            return create_ast_node(state, AST_TYPEOF, 0, NULL, expr, NULL, NULL);
        }
        case TOKEN_ALLOC:  return parse_alloc_expression(state);
        case TOKEN_REALLOC: return parse_realloc_expression(state);

        case TOKEN_NUMBER: case TOKEN_STRING: case TOKEN_CHAR:
        case TOKEN_NONE:   case TOKEN_TYPE:   case TOKEN_DOLLAR: {
            char *val = STRDUP(state, tok->value);
            if (!val) return NULL;
            advance_token(state);
            return create_ast_node(state, AST_LITERAL_VALUE, tok->type,
                                   val, NULL, NULL, NULL);
        }
        case TOKEN_ID: {
            char *name = STRDUP(state, tok->value);
            if (!name) return NULL;
            advance_token(state);
            ASTNode *id = create_ast_node(state, AST_IDENTIFIER, 0,
                                          name, NULL, NULL, NULL);
            return parse_postfix_expression(state, id);
        }
        default:
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Invalid expression");
            skip_to_sync_token(state);
            return NULL;
    }
}

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
            AST *args = create_empty_ast_list(state);
            if (!args) { FREE_NODE_RETURN_NULL(state, node); }
            if (TOKEN_IS(state, TOKEN_RPAREN)) {
                ast_list_shrink_to_fit(args);
            } else {
                ASTNode *first = parse_expression(state);
                if (!first) { parser__free_ast(args); FREE_NODE_RETURN_NULL(state, node); }
                APPEND_OR_FAIL(state, args, first);
                while (TOKEN_IS(state, TOKEN_COMMA)) {
                    advance_token(state);
                    ASTNode *next = parse_expression(state);
                    if (!next) { parser__free_ast(args); FREE_NODE_RETURN_NULL(state, node); }
                    APPEND_OR_FAIL(state, args, next);
                }
                ast_list_shrink_to_fit(args);
            }
            REQUIRE(state, TOKEN_RPAREN);
            ASTNode *call = create_ast_node(state, AST_FUNCTION_CALL, 0, NULL,
                                            node, NULL, (ASTNode *)args);
            if (!call) { parser__free_ast(args); FREE_NODE_RETURN_NULL(state, node); }
            node = call;
        } else if (TOKEN_IS(state, TOKEN_LBRACE)) {
            advance_token(state);
            ASTNode *idx = NULL;
            if (!TOKEN_IS(state, TOKEN_RBRACE)) {
                idx = parse_expression(state);
                if (!idx) FREE_NODE_RETURN_NULL(state, node);
            }
            REQUIRE(state, TOKEN_RBRACE);
            node = create_ast_node(state, AST_ARRAY_ACCESS, 0, NULL,
                                   node, idx, NULL);
        } else if (TOKEN_IS(state, TOKEN_DOT)) {
            advance_token(state);
            if (!TOKEN_IS(state, TOKEN_ID)) {
                FREE_NODE_RETURN_NULL(state, node);
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
        } else if (TOKEN_IS(state, TOKEN_LCURLY)) {
            ASTNode *init = parse_multi_initializer(state);
            if (!init) FREE_NODE_RETURN_NULL(state, node);
            node = create_ast_node(state, AST_STRUCT_INITIALIZER, 0, NULL,
                                   node, init, NULL);
        } else if (TOKEN_IS(state, TOKEN_COLON)) {
            advance_token(state);
            Type *tp = parse_type_specifier(state, true);
            if (!tp) FREE_NODE_RETURN_NULL(state, node);
            ASTNode *cast = create_ast_node(state, AST_CAST, 0, NULL,
                                            node, NULL, NULL);
            if (!cast) { parser__free_type(tp); FREE_NODE_RETURN_NULL(state, node); }
            cast->variable_type = tp;
            node = cast;
        } else break;
    }
    return node;
}

static ASTNode *parse_block(ParserState *state) {
    REQUIRE(state, TOKEN_LCURLY);
    ASTNode *seq = parse_sequence(state, true);
    if (!seq) return NULL;
    REQUIRE(state, TOKEN_RCURLY);
    return seq;
}

static ASTNode *parse_multi_initializer(ParserState *state) {
    REQUIRE(state, TOKEN_LCURLY);
    AST *list = create_empty_ast_list(state);
    if (!list) return NULL;

    if (TOKEN_IS(state, TOKEN_RCURLY)) {
        advance_token(state);
        ast_list_shrink_to_fit(list);
        return create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL,
                               NULL, NULL, (ASTNode *)list);
    }

    while (!TOKEN_IS(state, TOKEN_RCURLY) && !TOKEN_IS(state, TOKEN_EOF)) {
        if (state->panic_mode) {
            skip_to_sync_token(state);
            if (TOKEN_IS(state, TOKEN_RCURLY)) break;
        }
        ASTNode *elem = NULL;
        if (TOKEN_IS(state, TOKEN_DOT)) {
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
            if (!val) { free(field); FREE_LIST_RETURN_NULL(state, list); }
            ASTNode *fname = create_ast_node(state, AST_IDENTIFIER, 0,
                                             field, NULL, NULL, NULL);
            elem = create_ast_node(state, AST_FIELD_ACCESS, TOKEN_DOT, NULL,
                                   fname, val, NULL);
        } else {
            elem = parse_expression(state);
        }
        if (!elem) { FREE_LIST_RETURN_NULL(state, list); skip_to_sync_token(state); return NULL; }
        APPEND_OR_FAIL(state, list, elem);
        if (TOKEN_IS(state, TOKEN_COMMA)) {
            advance_token(state);
            if (TOKEN_IS(state, TOKEN_RCURLY)) break;
        } else if (!TOKEN_IS(state, TOKEN_RCURLY)) {
            FREE_LIST_RETURN_NULL(state, list);
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Expected ',' or '}' in initializer");
            return NULL;
        }
    }
    REQUIRE(state, TOKEN_RCURLY);
    ast_list_shrink_to_fit(list);
    return create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL,
                           NULL, NULL, (ASTNode *)list);
}

static ASTNode *parse_conditional(ParserState *state, ASTNodeType ntype) {
    advance_token(state);  /* consume IF or DO keyword */
    REQUIRE(state, TOKEN_LPAREN);

    ASTNode *cond;
    if (TOKEN_IS(state, TOKEN_RPAREN)) {
        Token *rp = get_current_token(state);
        PARSE_ERROR(state, ERROR_CODE_SYNTAX_EMPTY_PARENS,
                    "Empty condition not allowed, use Void");
        advance_token(state);
        cond = create_ast_node(state, AST_LITERAL_VALUE, TOKEN_NONE,
                               STRDUP(state, "Void"), NULL, NULL, NULL);
        if (!cond) return NULL;
        cond->line = rp->line; cond->column = rp->column;
    } else {
        cond = parse_expression(state);
        if (!cond) return NULL;
        REQUIRE(state, TOKEN_RPAREN);
    }

    ASTNode *if_body = parse_block_or_statement(state);
    if (!if_body) { FREE_NODE_RETURN_NULL(state, cond); }

    ASTNode *else_body = NULL;
    if (SKIP_IF(state, TOKEN_ELSE)) {
        else_body = parse_block_or_statement(state);
        if (!else_body) {
            free_node_recursive(if_body, state->pool);
            FREE_NODE_RETURN_NULL(state, cond);
        }
    }
    return create_ast_node(state, ntype, 0, NULL, cond, if_body, else_body);
}

static ASTNode *parse_break_statement(ParserState *state) {
    REQUIRE(state, TOKEN_BREAK);
    return create_ast_node(state, AST_BREAK, 0, NULL, NULL, NULL, NULL);
}
static ASTNode *parse_continue_statement(ParserState *state) {
    REQUIRE(state, TOKEN_CONTINUE);
    return create_ast_node(state, AST_CONTINUE, 0, NULL, NULL, NULL, NULL);
}

static ASTNode *parse_signal_statement(ParserState *state) {
    REQUIRE(state, TOKEN_SIGNAL);
    if (TOKEN_IS(state, TOKEN_SEMICOLON)) {
        PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                    "signal requires at least one argument");
        return NULL;
    }
    ASTNode *expr = parse_expression_or_multi(state, false);
    if (!expr) return NULL;
    return create_ast_node(state, AST_SIGNAL, 0, NULL, expr, NULL, NULL);
}

static ASTNode *parse_halt_statement(ParserState *state) {
    REQUIRE(state, TOKEN_HALT);
    return create_ast_node(state, AST_HALT, 0, NULL, NULL, NULL, NULL);

}
static ASTNode *parse_jump_statement(ParserState *state) {
    REQUIRE(state, TOKEN_JUMP);
    ASTNode *tgt = parse_expression(state);
    if (!tgt) return NULL;
    return create_ast_node(state, AST_JUMP, 0, NULL, tgt, NULL, NULL);
}

static ASTNode *parse_return_statement(ParserState *state) {
    REQUIRE(state, TOKEN_RETURN);
    if (TOKEN_IS(state, TOKEN_SEMICOLON))
        return create_ast_node(state, AST_RETURN, 0, NULL, NULL, NULL, NULL);
    ASTNode *expr = parse_expression_or_multi(state, true);
    if (!expr) return NULL;
    return create_ast_node(state, AST_RETURN, 0, NULL, expr, NULL, NULL);
}

static ASTNode *parse_free_statement(ParserState *state) {
    REQUIRE(state, TOKEN_FREE);
    if (TOKEN_IS(state, TOKEN_LPAREN)) {
        advance_token(state);
        ASTNode *expr = parse_expression(state);
        if (!expr) return NULL;
        REQUIRE(state, TOKEN_RPAREN);
        return create_ast_node(state, AST_FREE, 0, NULL, expr, NULL, NULL);
    }
    ASTNode *expr = parse_expression(state);
    if (!expr) return NULL;
    return create_ast_node(state, AST_FREE, 0, NULL, expr, NULL, NULL);
}

static ASTNode *parse_nop_statement(ParserState *state) {
    REQUIRE(state, TOKEN_NOP);
    return create_ast_node(state, AST_NOP, 0, NULL, NULL, NULL, NULL);
}

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

static ASTNode *parse_body_or_initializer(ParserState *state, bool is_function) {
    if (TOKEN_IS(state, TOKEN_EQUAL)) {
        advance_token(state);
        ASTNode *expr = parse_expression(state);
        if (!expr) return NULL;
        if (is_function) {
            ASTNode *ret_stmt = create_ast_node(state, AST_RETURN, 0, NULL, expr, NULL, NULL);
            if (!ret_stmt) { free_node_recursive(expr, state->pool); return NULL; }
            AST *list = create_empty_ast_list(state);
            if (!list) { free_node_recursive(ret_stmt, state->pool); free_node_recursive(expr, state->pool); return NULL; }
            if (!add_ast_node_to_list(list, ret_stmt)) {
                free_node_recursive(ret_stmt, state->pool);
                free_node_recursive(expr, state->pool);
                free(list);
                return NULL;
            }
            ast_list_shrink_to_fit(list);
            return create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL, (ASTNode *)list);
        }
        return expr;
    }

    if (is_function && is_arrow_token(state)) {
        ASTNode *stmt = parse_statement(state);
        if (!stmt) return NULL;
        AST *list = create_empty_ast_list(state);
        if (!list) { free_node_recursive(stmt, state->pool); return NULL; }
        if (!add_ast_node_to_list(list, stmt)) {
            free_node_recursive(stmt, state->pool);
            free(list);
            return NULL;
        }
        ast_list_shrink_to_fit(list);
        return create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL, (ASTNode *)list);
    }

    if (TOKEN_IS(state, TOKEN_LCURLY))
        return parse_block(state);

    return NULL;
}

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
                if (!saw_acc) { *acc = STRDUP(state, mod); saw_acc = true; }
                advance_token(state);
            }
        } else break;
    }
}

static ASTNode *parse_state_statement(ParserState *state) {
    Token *state_tok = get_current_token(state);
    char *state_modifier = STRDUP(state, state_tok->value);
    if (!state_modifier) return NULL;
    advance_token(state);

    char *access_mod = NULL;
    bool is_const = false;
    parse_decl_special_modifiers(state, &access_mod, &is_const);

    uint8_t ptr_lvl = 0, ref = 0;
    parse_type_prefixes(state, &ptr_lvl, &ref);

    char *name = NULL;
    if (TOKEN_IS(state, TOKEN_ID)) {
        name = STRDUP(state, get_current_token(state)->value);
        if (!name) { free(state_modifier); free(access_mod); return NULL; }
        advance_token(state);
    }

    /* del statement */
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
            Type *t = create_temp_type_with_prefixes(state, ptr_lvl, ref);
            if (t) {
                t->name = STRDUP(state, name ? name : "?");
                node->variable_type = t;
            }
        }
        return node;
    }

    /* Optional array dimensions */
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
            if (!dimlist) { free(state_modifier); free(access_mod); free(name); return NULL; }
        }
        APPEND_OR_FAIL(state, dimlist, sz);
        dimcnt++;
    }

    /* Function declaration / definition */
    if (TOKEN_IS(state, TOKEN_LPAREN)) {
        if (!name) {
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Function name required");
            free(state_modifier); free(access_mod);
            if (dimlist) parser__free_ast(dimlist);
            return NULL;
        }
        if (dimcnt > 0) {
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Array dimensions before '(' not allowed");
            free(state_modifier); free(access_mod); free(name);
            parser__free_ast(dimlist);
            return NULL;
        }

        AST *params = parse_parameter_list(state);
        if (!params) { free(state_modifier); free(access_mod); free(name); return NULL; }

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
            body = parse_body_or_initializer(state, true);
            if (TOKEN_IS(state, TOKEN_SEMICOLON)) advance_token(state);
        }

        ASTNode *params_node = create_ast_node(state, AST_BLOCK, 0, NULL,
                                               NULL, NULL, (ASTNode *)params);
        if (!params_node) {
            free(state_modifier); free(access_mod); free(name);
            parser__free_type(ret_type);
            parser__free_ast(params);
            if (body) free_node_recursive(body, state->pool);
            return NULL;
        }
        ASTNode *func = create_ast_node(state, AST_FUNCTION_DECLARATION,
                                        0, name, params_node, body, NULL);
        if (!func) {
            free(state_modifier); free(access_mod);
            parser__free_type(ret_type);
            parser__free_ast(params);
            free_node_recursive(params_node, state->pool);
            if (body) free_node_recursive(body, state->pool);
            return NULL;
        }
        func->variable_type   = ret_type;
        func->state_modifier  = state_modifier;
        func->access_modifier = access_mod;
        func->is_const        = is_const;
        return func;
    }

    /* Variable declaration */
    Type *var_type = NULL;
    ASTNode *var_init = NULL;
    ASTNode *var_body = NULL;

    /* Inline struct definition: name : { ... } [ : Type ] [ = init ] */
    if (name && TOKEN_IS(state, TOKEN_COLON) &&
        state->current_token_position + 1 < state->total_tokens &&
        state->token_stream[state->current_token_position + 1].type == TOKEN_LCURLY) {
        advance_token(state);
        var_body = parse_block(state);
        if (!var_body) {
            free(state_modifier); free(access_mod); free(name);
            return NULL;
        }
        if (TOKEN_IS(state, TOKEN_COLON)) {
            advance_token(state);
            var_type = parse_type_specifier(state, true);
            if (!var_type) {
                free(state_modifier); free(access_mod); free(name);
                free_node_recursive(var_body, state->pool);
                return NULL;
            }
            var_type->struct_definition = var_body;
        } else {
            var_type = create_temp_type_with_prefixes(state, 0, 0);
            var_type->name = STRDUP(state, "Struct");
            var_type->struct_definition = var_body;
        }
        if (TOKEN_IS(state, TOKEN_EQUAL)) {
            advance_token(state);
            var_init = parse_expression(state);
            if (!var_init) {
                free(state_modifier); free(access_mod); free(name);
                parser__free_type(var_type);
                return NULL;
            }
        }
    } else {
        /* Standard type colon parsing */
        if (TOKEN_IS(state, TOKEN_COLON)) {
            advance_token(state);
            var_type = parse_type_specifier(state, true);
            if (!var_type) {
                free(state_modifier); free(access_mod); free(name);
                if (dimlist) parser__free_ast(dimlist);
                return NULL;
            }
            var_type->pointer_level += ptr_lvl;
            var_type->is_reference   = ref;
        } else if (ptr_lvl || ref || dimcnt > 0) {
            var_type = create_temp_type_with_prefixes(state, ptr_lvl, ref);
            if (!var_type) {
                free(state_modifier); free(access_mod); free(name);
                if (dimlist) parser__free_ast(dimlist);
                return NULL;
            }
        }

        /* Array dimensions */
        if (dimcnt > 0) {
            if (!var_type) {
                var_type = create_temp_type_with_prefixes(state, 0, 0);
                if (!var_type) { free(state_modifier); free(access_mod); free(name); return NULL; }
            }
            ast_list_shrink_to_fit(dimlist);
            ASTNode *dims_node = create_ast_node(state, AST_MULTI_INITIALIZER,
                                                 0, NULL, NULL, NULL,
                                                 (ASTNode *)dimlist);
            if (!dims_node) {
                parser__free_ast(dimlist);
                free(state_modifier); free(access_mod); free(name);
                parser__free_type(var_type);
                return NULL;
            }
            var_type->array_dimensions = dims_node;
            var_type->is_array = 1;
        }

        /* Body / initializer for `def` variable */
        if (strcmp(state_modifier, "def") == 0) {
            ASTNode *init_or_body = parse_body_or_initializer(state, false);
            if (init_or_body) {
                if (init_or_body->type == AST_BLOCK)
                    var_body = init_or_body;
                else
                    var_init = init_or_body;
            }
        }
    }

    ASTNode *decl = create_ast_node(state, AST_VARIABLE_DECLARATION,
                                    0, name, NULL, var_body, NULL);
    if (!decl) {
        free(state_modifier); free(access_mod); free(name);
        parser__free_type(var_type);
        if (var_init) free_node_recursive(var_init, state->pool);
        if (var_body) free_node_recursive(var_body, state->pool);
        return NULL;
    }
    decl->state_modifier  = state_modifier;
    decl->access_modifier = access_mod;
    decl->is_const        = is_const;
    decl->variable_type   = var_type;
    decl->default_value    = var_init;
    return decl;
}

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
        node->variable_type  = tp;
        node->state_modifier = state_mod ? state_mod : STRDUP(state, "var");
        if (TOKEN_IS(state, TOKEN_EQUAL)) {
            advance_token(state);
            ASTNode *def_val = parse_expression(state);
            if (!def_val) { FREE_NODE_RETURN_NULL(state, node); return NULL; }
            node->default_value = def_val;
        }
        return node;
    }

    if (state_mod) {
        PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                    "Expected parameter name after '%s'", state_mod);
        free(state_mod);
        return NULL;
    }

    /* Tentative type literal */
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

static AST *parse_parameter_list(ParserState *state) {
    REQUIRE(state, TOKEN_LPAREN);
    AST *list = create_empty_ast_list(state);
    if (!list) return NULL;

    if (is_keyword_void(state)) {
        advance_token(state);
        REQUIRE(state, TOKEN_RPAREN);
        ast_list_shrink_to_fit(list);
        return list;
    }

    if (TOKEN_IS(state, TOKEN_RPAREN)) {
        Token *rp = get_current_token(state);
        PARSE_ERROR(state, ERROR_CODE_SYNTAX_EMPTY_PARENS,
                    "Empty parentheses not allowed, use Void instead");
        advance_token(state);
        list->had_errors = true;
        ast_list_shrink_to_fit(list);
        return list;
    }

    while (!TOKEN_IS(state, TOKEN_RPAREN) && !TOKEN_IS(state, TOKEN_EOF)) {
        if (state->panic_mode) {
            skip_to_sync_token(state);
            if (TOKEN_IS(state, TOKEN_RPAREN)) break;
        }
        ASTNode *p = parse_parameter(state);
        if (!p) { FREE_LIST_RETURN_NULL(state, list); }
        APPEND_OR_FAIL(state, list, p);
        if (TOKEN_IS(state, TOKEN_COMMA)) {
            advance_token(state);
            if (TOKEN_IS(state, TOKEN_RPAREN)) break;
        } else if (!TOKEN_IS(state, TOKEN_RPAREN)) {
            FREE_LIST_RETURN_NULL(state, list);
            PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Expected ',' or ')'");
            return NULL;
        }
    }
    REQUIRE(state, TOKEN_RPAREN);
    ast_list_shrink_to_fit(list);
    return list;
}

static ASTNode *parse_block_or_statement(ParserState *state) {
    if (TOKEN_IS(state, TOKEN_LCURLY)) return parse_block(state);
    if (TOKEN_IS(state, TOKEN_THEN)) {
        advance_token(state);
        ASTNode *stmt = parse_statement(state);
        if (!stmt) return NULL;
        AST *list = create_empty_ast_list(state);
        if (!list) { FREE_NODE_RETURN_NULL(state, stmt); }
        APPEND_OR_FAIL(state, list, stmt);
        ast_list_shrink_to_fit(list);
        return create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL,
                               (ASTNode *)list);
    }
    return NULL;
}

static ASTNode *parse_statement(ParserState *state) {
    if (TOKEN_IS(state, TOKEN_SEMICOLON)) { advance_token(state); return NULL; }

    if (state->panic_mode) {
        skip_to_sync_token(state);
        if (TOKEN_IS(state, TOKEN_EOF)) return NULL;
    }

    TokenType tt = get_current_token_type(state);

    if (tt == TOKEN_STATE)
        return parse_state_statement(state);

    /* Label: identifier ':' */
    if (tt == TOKEN_ID) {
        int saved = state->current_token_position;
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
                        t3->type == TOKEN_TYPEOF)
                        is_type = true;
                }
                if (!is_type) {
                    char *lbl = STRDUP(state, get_current_token(state)->value);
                    if (!lbl) return NULL;
                    advance_token(state); advance_token(state);
                    return create_ast_node(state, AST_LABEL_DECLARATION,
                                           0, lbl, NULL, NULL, NULL);
                }
            }
        }
        state->current_token_position = saved;
    }

    switch (tt) {
        case TOKEN_LCURLY:      return parse_block(state);
        case TOKEN_IF:          return parse_conditional(state, AST_IF_STATEMENT);
        case TOKEN_DO:          return parse_conditional(state, AST_DO_LOOP);
        case TOKEN_BREAK:       return parse_break_statement(state);
        case TOKEN_CONTINUE:    return parse_continue_statement(state);
        case TOKEN_RETURN:      return parse_return_statement(state);
        case TOKEN_FREE:        return parse_free_statement(state);
        case TOKEN_JUMP:        return parse_jump_statement(state);
        case TOKEN_SIGNAL:      return parse_signal_statement(state);
        case TOKEN_NOP:         return parse_nop_statement(state);
        case TOKEN_ASM:         return parse_asm_statement(state);
        case TOKEN_ELSE:        return parse_else_statement(state);
        default: break;
    }

    /* Fallback: expression statement */
    if (token_can_start_expression(tt)) {
        int sp = state->current_token_position;
        ASTNode *expr = parse_expression(state);
        if (!expr) {
            state->current_token_position = sp;
            skip_to_sync_token(state);
        }
        return expr;
    }

    Token *et = get_current_token(state);
    if (et) PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Unexpected token '%s' in statement context", et->value);
    else UNEXPECTED_EOF(state);
    skip_to_sync_token(state);
    return NULL;
}

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
            APPEND_OR_FAIL(state, list, stmt);
            if (statement_requires_semicolon(stmt)) parse_semicolon(state);
            else if (TOKEN_IS(state, TOKEN_SEMICOLON)) advance_token(state);
        } else {
            if (TOKEN_IS(state, TOKEN_SEMICOLON)) {
                advance_token(state);
            } else if (state->current_token_position < state->total_tokens) {
                advance_token(state);
            }
        }
    }

    ast_list_shrink_to_fit(list);
    ASTNode *block = create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL,
                                     (ASTNode *)list);
    if (!block) { FREE_LIST_RETURN_NULL(state, list); }
    return block;
}

static bool statement_requires_semicolon(ASTNode *node) {
    if (!node) return false;
    switch (node->type) {
        case AST_BLOCK: case AST_IF_STATEMENT:
        case AST_DO_LOOP: case AST_LABEL_DECLARATION: case AST_ELSE_STATEMENT:
            return false;
        case AST_FUNCTION_DECLARATION:
            return (node->right == NULL);
        default:
            return true;
    }
}

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
    } else {
        advance_token(state);
    }
}

AST *parse(Token *tokens, uint16_t token_count) {
    ParserState state;
    state.current_token_position = 0;
    state.token_stream           = tokens;
    state.total_tokens            = token_count;
    state.pool                   = parser__ast_node_pool_create(1);
    state.panic_mode             = false;
    state.fatal_error            = false;
    state.has_pushback           = false;
    if (!state.pool) return NULL;

    AST *ast = ALLOC(&state, sizeof(AST));
    if (!ast) { parser__ast_node_pool_destroy(state.pool); return NULL; }
    memset(ast, 0, sizeof(AST));
    ast->pool = state.pool;

    while (get_current_token_type(&state) != TOKEN_EOF) {
        if (state.fatal_error) { parser__free_ast(ast); return NULL; }
        if (state.panic_mode) skip_to_sync_token(&state);

        ASTNode *stmt = parse_statement(&state);
        if (stmt) {
            if (!add_ast_node_to_list(ast, stmt)) {
                free_node_recursive(stmt, state.pool);
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
    ast_list_shrink_to_fit(ast);
    return ast;
}
