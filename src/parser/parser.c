#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "parser.h"

/*
 * Configuration constants for internal limits and initial sizes.
 */
#define MAX_MODIFIERS 8                 /* Maximum number of type/decl modifiers.                */
#define AST_INITIAL_CAPACITY 8          /* Initial capacity for top‑level AST node array.        */
#define AST_POOL_INITIAL_CAPACITY 256   /* Initial capacity for node pool.                       */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0])) /* Helper to get array length.          */

/*
 * Forward declarations of static parsing functions.
 */
static TokenType get_current_token_type(ParserState *state);
static void advance_token(ParserState *state);
static Token *get_current_token(ParserState *state);
static bool expect_token(ParserState *state, TokenType expected_type);
static void *safe_malloc(ParserState *state, size_t size);
static void *safe_realloc(ParserState *state, void *ptr, size_t size);
static char *safe_strdup(ParserState *state, const char *str);
static ASTNode *create_ast_node(ParserState *state, ASTNodeType node_type, TokenType operation_type,
                                char *node_value, ASTNode *left_child, ASTNode *right_child, ASTNode *extra_node);
static bool add_ast_node_to_list(AST *ast, ASTNode *node);
static AST *parse_universal_list(ParserState *state, ASTNode *(*parse_element)(ParserState *),
                                 bool (*is_element_start)(ParserState *), TokenType separator, TokenType end_token);
static ASTNode *parse_binary_operation_universal(ParserState *state, ASTNode *(*parse_operand)(ParserState *),
                                                 TokenType *operators, uint8_t num_operators);
static bool parse_type_prefixes(ParserState *state, uint8_t *pointer_level, uint8_t *is_reference);
static void apply_prefixes_to_type(Type *type, uint8_t pointer_level, uint8_t is_reference);
static Type *create_temp_type_with_prefixes(ParserState *state, uint8_t pointer_level, uint8_t is_reference);
static Type *parse_type_specifier_silent(ParserState *state, bool silent, bool parse_prefixes);
static Type *parse_type_specifier(ParserState *state, bool parse_prefixes);
static ASTNode *parse_fixed_argument_function(ParserState *state, ASTNodeType node_type, uint8_t arg_count,
                                              const char *func_name);
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
static ASTNode *parse_interflag_statement(ParserState *state);
static ASTNode *parse_label_declaration(ParserState *state);
static ASTNode *parse_jump_statement(ParserState *state);
static ASTNode *parse_return_statement(ParserState *state);
static ASTNode *parse_free_statement(ParserState *state);
static ASTNode *parse_nop_statement(ParserState *state);
static ASTNode *parse_halt_statement(ParserState *state);
static ASTNode *parse_kill_statement(ParserState *state);
static ASTNode *parse_try_statement(ParserState *state);
static ASTNode *parse_else_statement(ParserState *state);   /* Standalone else statement handler */
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
static void parse_declaration_modifiers( ParserState *state
                                       , char ***modifiers_out
                                       , uint8_t *count_out);
static ASTNode *parse_function_declaration( ParserState *state
                                          , char *state_modifier
                                          , char *name
                                          , char **modifiers
                                          , uint8_t modifier_count);
static ASTNode *parse_struct_declaration( ParserState *state
                                        , char *state_modifier
                                        , char *name
                                        , char **modifiers
                                        , uint8_t modifier_count);
static ASTNode *parse_object_declaration(ParserState *state, bool allow_expression);
static ASTNode *parse_single_declarator( ParserState *state
                                       , char *state_modifier
                                       , char **modifiers
                                       , uint8_t modifier_count
                                       , Type *common_type
                                       , bool allow_type);
static ASTNode *parse_variable_declaration_list( ParserState *state
                                               , char *state_modifier
                                               , char **modifiers
                                               , uint8_t modifier_count
                                               , Type *common_type);
static void parse_semicolon(ParserState *state);
static ASTNode *parse_sequence(ParserState *state, bool expect_rcurly);

/*
 * Macros for error reporting with location information.
 * These macros capture the current token's location and forward to the error handler.
 */
#define REPORT_PARSE_ERROR_EX(state, level, error_code, context, ...) do { \
    Token *current = get_current_token(state); \
    if (current) { \
        errhandler__report_error_ex(level, error_code, current->line, current->column, \
                                    (int)strlen(current->value), context, __VA_ARGS__); \
    } else { \
        errhandler__report_error_ex(level, error_code, 0, 0, 0, context, __VA_ARGS__); \
    } \
    return NULL; \
} while(0)

#define REPORT_PARSE_ERROR(state, error_code, ...) \
    REPORT_PARSE_ERROR_EX(state, ERROR_LEVEL_ERROR, error_code, "syntax", __VA_ARGS__)

#define REPORT_UNEXPECTED_TOKEN(state, expected, actual) do { \
    Token *current = get_current_token(state); \
    if (current) { \
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN, \
                                    current->line, current->column, (int)strlen(current->value), \
                                    "syntax", "Expected %s but got %s (value: '%s')", \
                                    expected, actual, current->value); \
    } else { \
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN, \
                                    0, 0, 0, "syntax", "Expected %s but got EOF", expected); \
    } \
    return NULL; \
} while(0)

#define REPORT_UNEXPECTED_EOF(state) \
    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_UNEXPECTED_EOF, "Unexpected end of file")

#define CURRENT_TOKEN_TYPE_MATCHES(state, token) (get_current_token_type(state) == (token))
#define CONSUME_TOKEN(state, token) do { if (!expect_token(state, token)) return NULL; } while(0)
#define ATTEMPT_CONSUME_TOKEN(state, token) (CURRENT_TOKEN_TYPE_MATCHES(state, token) ? (advance_token(state), 1) : 0)

/*
 * Token stream helpers.
 */

/*
 * Returns the type of the current token, or TOKEN_EOF if at end.
 */
static TokenType get_current_token_type(ParserState *state) {
    return (state->current_token_position < state->total_tokens)
           ? state->token_stream[state->current_token_position].type
           : TOKEN_EOF;
}

/*
 * Advances the token pointer by one, if not already at the last token.
 */
static void advance_token(ParserState *state) {
    if (state->current_token_position < state->total_tokens - 1) {
        state->current_token_position++;
    }
}

/*
 * Returns a pointer to the current token, or NULL if at end.
 */
static Token *get_current_token(ParserState *state) {
    return (state->current_token_position < state->total_tokens)
           ? &state->token_stream[state->current_token_position]
           : NULL;
}

/*
 * Checks that the current token is of expected_type, advances if true, otherwise reports error.
 */
static bool expect_token(ParserState *state, TokenType expected_type) {
    if (CURRENT_TOKEN_TYPE_MATCHES(state, expected_type)) {
        advance_token(state);
        return true;
    }
    TokenType actual = get_current_token_type(state);
    const char *expected_name = token_names[expected_type];
    const char *actual_name = (actual == TOKEN_EOF) ? "EOF" : token_names[actual];
    REPORT_UNEXPECTED_TOKEN(state, expected_name, actual_name);
}

/*
 * Memory allocation helpers.
 */

/*
 * Allocates memory of given size, reports error on failure.
 */
static void *safe_malloc(ParserState *state, size_t size) {
    void *ptr = malloc(size);
    if (!ptr && size > 0) {
        REPORT_PARSE_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION, "Memory allocation failed");
    }
    return ptr;
}

/*
 * Reallocates memory, reports error on failure.
 */
static void *safe_realloc(ParserState *state, void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0) {
        REPORT_PARSE_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION, "Memory reallocation failed");
    }
    return new_ptr;
}

/*
 * Duplicates a string, reports error on failure.
 */
static char *safe_strdup(ParserState *state, const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char *dup = malloc(len);
    if (!dup) {
        REPORT_PARSE_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION, "String duplication failed");
    }
    memcpy(dup, str, len);
    return dup;
}

/*
 * Statement semicolon requirement.
 */

/*
 * Determines whether a statement node requires a trailing semicolon.
 */
static bool statement_requires_semicolon(ASTNode *node) {
    if (!node) return false;
    switch (node->type) {
        case AST_VARIABLE_LIST:
        case AST_BLOCK:
        case AST_IF_STATEMENT:
        case AST_DO_LOOP:
        case AST_LABEL_DECLARATION:
        case AST_TRY:
        case AST_ELSE_STATEMENT:   /* Standalone else does not need an extra semicolon (body may have its own). */
            return false;
        case AST_FUNCTION_DECLARATION:
            /* A function prototype (no body) requires a semicolon. */
            return (node->right == NULL);
        case AST_COMPOUND_TYPE:
            /* Type declarations (structs) require a terminating semicolon. */
            return true;
        default:
            return true;
    }
}

/*
 * Consumes a semicolon if present, otherwise reports an error.
 */
static void parse_semicolon(ParserState *state) {
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
        Token *cur = get_current_token(state);
        if (cur) {
            errhandler__report_error(ERROR_CODE_SYNTAX_MISSING_SEMICOLON,
                                     cur->line, cur->column, "syntax", "Expected ';'");
        } else {
            errhandler__report_error(ERROR_CODE_SYNTAX_MISSING_SEMICOLON,
                                     0, 0, "syntax", "Expected ';' at end of file");
        }
    } else {
        advance_token(state);
    }
}

/*
 * AST node pool management.
 */

/*
 * Creates a new node pool with given initial capacity.
 */
ASTNodePool *parser__ast_node_pool_create(uint16_t initial_capacity) {
    ASTNodePool *pool = malloc(sizeof(ASTNodePool));
    if (!pool) return NULL;
    pool->nodes = malloc(initial_capacity * sizeof(ASTNode));
    pool->free_list = malloc(initial_capacity * sizeof(uint16_t));
    if (!pool->nodes || !pool->free_list) {
        free(pool->nodes);
        free(pool->free_list);
        free(pool);
        return NULL;
    }
    pool->capacity = initial_capacity;
    pool->size = 0;
    pool->free_top = 0;
    for (uint16_t i = 0; i < initial_capacity; i++) {
        pool->free_list[pool->free_top++] = i;
    }
    return pool;
}

/*
 * Destroys a node pool and frees all associated memory.
 */
void parser__ast_node_pool_destroy(ASTNodePool *pool) {
    if (!pool) return;
    free(pool->nodes);
    free(pool->free_list);
    free(pool);
}

/*
 * Allocates a new AST node from the pool. Returns zeroed node.
 */
ASTNode *parser__ast_node_pool_alloc(ASTNodePool *pool) {
    if (!pool || pool->free_top == 0) return NULL;
    uint16_t index = pool->free_list[--pool->free_top];
    ASTNode *node = &pool->nodes[index];
    memset(node, 0, sizeof(ASTNode));
    return node;
}

/*
 * Returns a node to the pool for reuse.
 */
void parser__ast_node_pool_free(ASTNodePool *pool, ASTNode *node) {
    if (!pool || !node) return;
    uint16_t index = (uint16_t)(node - pool->nodes);
    if (index < pool->capacity && pool->free_top < pool->capacity) {
        pool->free_list[pool->free_top++] = index;
    }
}

/*
 * Core AST node creation.
 */

/*
 * Creates a new AST node with given fields and records source location from current token.
 */
static ASTNode *create_ast_node(ParserState *state, ASTNodeType node_type, TokenType operation_type,
                                char *node_value, ASTNode *left_child, ASTNode *right_child, ASTNode *extra_node) {
    ASTNode *node = parser__ast_node_pool_alloc(state->pool);
    if (!node) return NULL;
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
    node->parent_struct = NULL;
    node->modifiers = NULL;
    node->modifier_count = 0;
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
 * Adds an AST node to the top‑level list of an AST, resizing if necessary.
 */
bool add_ast_node_to_list(AST *ast, ASTNode *node) {
    if (ast->count >= ast->capacity) {
        uint16_t new_capacity = ast->capacity ? ast->capacity * 2 : AST_INITIAL_CAPACITY;
        ASTNode **new_nodes = realloc(ast->nodes, new_capacity * sizeof(ASTNode*));
        if (!new_nodes) return false;
        ast->nodes = new_nodes;
        ast->capacity = new_capacity;
    }
    ast->nodes[ast->count++] = node;
    return true;
}

/*
 * Generic list and binary operation parsers.
 */

/*
 * Parses a generic list: elements separated by 'separator', terminated by 'end_token'.
 */
static AST *parse_universal_list(ParserState *state, ASTNode *(*parse_element)(ParserState *),
                                 bool (*is_element_start)(ParserState *), TokenType separator, TokenType end_token) {
    AST *list = safe_malloc(state, sizeof(AST));
    list->nodes = NULL;
    list->count = 0;
    list->capacity = 0;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, end_token)) {
        advance_token(state);
        return list;
    }
    while (!CURRENT_TOKEN_TYPE_MATCHES(state, end_token) && !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
        if (is_element_start && !is_element_start(state)) {
            if (CURRENT_TOKEN_TYPE_MATCHES(state, separator)) {
                advance_token(state);
                if (CURRENT_TOKEN_TYPE_MATCHES(state, end_token)) break;
                parser__free_ast(list);
                REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Unexpected comma in list");
            }
            break;
        }
        ASTNode *element = parse_element(state);
        if (!element) {
            parser__free_ast(list);
            return NULL;
        }
        if (!add_ast_node_to_list(list, element)) {
            parser__ast_node_pool_free(state->pool, element);
            parser__free_ast(list);
            return NULL;
        }
        if (CURRENT_TOKEN_TYPE_MATCHES(state, separator)) {
            advance_token(state);
            if (CURRENT_TOKEN_TYPE_MATCHES(state, end_token)) break;
            if (is_element_start && !is_element_start(state)) {
                parser__free_ast(list);
                REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected element after comma in list");
            }
        } else if (!CURRENT_TOKEN_TYPE_MATCHES(state, end_token)) {
            parser__free_ast(list);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                              "Expected '%s' or '%s'", token_names[separator], token_names[end_token]);
        }
    }
    CONSUME_TOKEN(state, end_token);
    return list;
}

/*
 * Parses left‑associative binary operations using the given operator list.
 */
static ASTNode *parse_binary_operation_universal(ParserState *state, ASTNode *(*parse_operand)(ParserState *),
                                                 TokenType *operators, uint8_t num_operators) {
    ASTNode *node = parse_operand(state);
    if (!node) return NULL;
    while (1) {
        TokenType current_type = get_current_token_type(state);
        bool is_operator = false;
        for (uint8_t i = 0; i < num_operators; i++) {
            if (current_type == operators[i]) {
                is_operator = true;
                break;
            }
        }
        if (!is_operator) break;
        TokenType operation = current_type;
        advance_token(state);
        ASTNode *right = parse_operand(state);
        if (!right) {
            parser__ast_node_pool_free(state->pool, node);
            return NULL;
        }
        ASTNode *new_node = AST_NEW_BINARY(state, operation, node, right);
        if (!new_node) {
            parser__ast_node_pool_free(state->pool, right);
            parser__ast_node_pool_free(state->pool, node);
            return NULL;
        }
        node = new_node;
    }
    return node;
}

/*
 * Type prefix parsing.
 * Recognises pointer ('@') and reference ('&') prefixes.
 * These prefixes can appear both as type modifiers and as unary operators.
 */

/*
 * Parses pointer (@) and reference (&) prefixes, updating the given counters.
 * Returns true if any prefixes were found.
 */
static bool parse_type_prefixes(ParserState *state, uint8_t *pointer_level, uint8_t *is_reference) {
    bool found = false;
    *pointer_level = 0;
    *is_reference = 0;
    while (1) {
        TokenType current = get_current_token_type(state);
        if (current == TOKEN_AT) {
            advance_token(state);
            (*pointer_level)++;
            found = true;
            *is_reference = 0;
        } else if (current == TOKEN_AMPERSAND) {
            advance_token(state);
            (*is_reference)++;
            found = true;
            *pointer_level = 0;
        } else {
            break;
        }
    }
    return found;
}

/*
 * Applies pointer and reference levels to a type structure.
 */
static void apply_prefixes_to_type(Type *type, uint8_t pointer_level, uint8_t is_reference) {
    if (!type) return;
    type->pointer_level = pointer_level;
    type->is_reference = is_reference;
}

/*
 * Creates a temporary type with given pointer/reference levels (default name "int").
 */
static Type *create_temp_type_with_prefixes(ParserState *state, uint8_t pointer_level, uint8_t is_reference) {
    Type *type = safe_malloc(state, sizeof(Type));
    memset(type, 0, sizeof(Type));
    type->name = safe_strdup(state, "int");
    type->pointer_level = pointer_level;
    type->is_reference = is_reference;
    return type;
}

/*
 * Declaration modifier parsing (any order).
 */

/*
 * Parses declaration modifiers (TOKEN_ACCMOD, TOKEN_CONSTMOD, TOKEN_MEMMOD, TOKEN_SIGNEDMOD).
 */
static void parse_declaration_modifiers(ParserState *state, char ***modifiers_out, uint8_t *count_out) {
    *modifiers_out = NULL;
    *count_out = 0;
    uint8_t cap = 0;

    while (1) {
        TokenType current = get_current_token_type(state);
        if (current == TOKEN_ACCMOD || current == TOKEN_CONSTMOD ||
            current == TOKEN_MEMMOD || current == TOKEN_SIGNEDMOD) {
            Token *mod_token = get_current_token(state);
            if (*count_out >= cap) {
                cap = cap ? cap * 2 : 2;
                *modifiers_out = realloc(*modifiers_out, cap * sizeof(char*));
            }
            (*modifiers_out)[*count_out] = safe_strdup(state, mod_token->value);
            (*count_out)++;
            advance_token(state);
        } else {
            break;
        }
    }
}

/*
 * Type specifier parsing.
 */

/*
 * Parses a type specifier, optionally without error messages (silent) and optionally parsing prefixes.
 * Type specifiers can include base types, pointer/reference prefixes, 'typeof', and angle‑bracket expressions.
 */
static Type *parse_type_specifier_silent(ParserState *state, bool silent, bool parse_prefixes) {
    Type *type = safe_malloc(state, sizeof(Type));
    memset(type, 0, sizeof(Type));
    type->typeof_expression = NULL;

    /* Parse modifiers that can appear before the type name. */
    while (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_CONSTMOD) ||
           CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_MEMMOD) ||
           CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SIGNEDMOD)) {
        if (type->modifier_count >= MAX_MODIFIERS) {
            parser__free_type(type);
            if (!silent) REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Too many type modifiers");
            return NULL;
        }
        Token *mod_token = get_current_token(state);
        char **new_mods = safe_realloc(state, type->modifiers, (type->modifier_count + 1) * sizeof(char*));
        type->modifiers = new_mods;
        type->modifiers[type->modifier_count] = safe_strdup(state, mod_token->value);
        type->modifier_count++;
        advance_token(state);
    }

    uint8_t pointer_level = 0, is_reference = 0;
    if (parse_prefixes) {
        parse_type_prefixes(state, &pointer_level, &is_reference);
        apply_prefixes_to_type(type, pointer_level, is_reference);
    }

    /* Handle typeof(expression) */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_TYPEOF)) {
        advance_token(state);
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
            if (!silent) REPORT_UNEXPECTED_TOKEN(state, "'('", token_names[get_current_token_type(state)]);
            parser__free_type(type);
            return NULL;
        }
        advance_token(state);
        ASTNode *expr = parse_expression(state);
        if (!expr) {
            if (!silent) REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                            "Failed to parse expression inside typeof");
            parser__free_type(type);
            return NULL;
        }
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
            if (!silent) REPORT_UNEXPECTED_TOKEN(state, "')'", token_names[get_current_token_type(state)]);
            parser__ast_node_pool_free(state->pool, expr);
            parser__free_type(type);
            return NULL;
        }
        advance_token(state);
        type->name = safe_strdup(state, "typeof");
        type->typeof_expression = expr;
        apply_prefixes_to_type(type, pointer_level, is_reference);
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LT) || CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LBRACE)) {
            if (!silent) REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                            "Type cannot have angle brackets or array dimensions after typeof");
            parser__free_type(type);
            return NULL;
        }
        return type;
    }

    /* Base type name: either a keyword TOKEN_TYPE or an identifier */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_TYPE) || CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
        Token *type_token = get_current_token(state);
        type->name = safe_strdup(state, type_token->value);
        advance_token(state);
    } else {
        if (!silent) {
            REPORT_UNEXPECTED_TOKEN(state, "type specifier, identifier, or typeof",
                                    token_names[get_current_token_type(state)]);
        }
        parser__free_type(type);
        return NULL;
    }

    /* Optional angle brackets for generic arguments or explicit size. */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LT)) {
        advance_token(state);
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_GT)) {
            parser__free_type(type);
            if (!silent) REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Empty angle brackets in type");
            return NULL;
        }
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_NUMBER)) {
            Token *number_token = get_current_token(state);
            long size_value = atol(number_token->value);
            if (size_value <= 0 || size_value > UINT8_MAX) {
                parser__free_type(type);
                if (!silent) {
                    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                       "Invalid type size: must be between 1 and %d", UINT8_MAX);
                }
                return NULL;
            }
            type->size_in_bytes = (uint8_t)size_value;
            advance_token(state);
            if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_GT)) {
                parser__free_type(type);
                if (!silent) REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected '>' after type size");
                return NULL;
            }
            advance_token(state);
        } else {
            ASTNode *angle_expr = parse_expression(state);
            if (!angle_expr) {
                parser__free_type(type);
                if (!silent) {
                    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                       "Failed to parse expression in angle brackets");
                }
                return NULL;
            }
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
                AST *angle_list = safe_malloc(state, sizeof(AST));
                angle_list->nodes = NULL;
                angle_list->count = 0;
                angle_list->capacity = 0;
                if (!add_ast_node_to_list(angle_list, angle_expr)) {
                    parser__ast_node_pool_free(state->pool, angle_expr);
                    parser__free_ast(angle_list);
                    parser__free_type(type);
                    return NULL;
                }
                while (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
                    advance_token(state);
                    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_GT)) break;
                    angle_expr = parse_expression(state);
                    if (!angle_expr) {
                        parser__free_ast(angle_list);
                        parser__free_type(type);
                        if (!silent) {
                            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                               "Failed to parse expression in angle brackets");
                        }
                        return NULL;
                    }
                    if (!add_ast_node_to_list(angle_list, angle_expr)) {
                        parser__ast_node_pool_free(state->pool, angle_expr);
                        parser__free_ast(angle_list);
                        parser__free_type(type);
                        return NULL;
                    }
                }
                if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_GT)) {
                    parser__free_ast(angle_list);
                    parser__free_type(type);
                    if (!silent) {
                        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                           "Expected '>' after expression in angle brackets");
                    }
                    return NULL;
                }
                advance_token(state);
                ASTNode *multi_init = create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL,
                                                      NULL, NULL, (ASTNode*)angle_list);
                if (!multi_init) {
                    parser__free_ast(angle_list);
                    parser__free_type(type);
                    return NULL;
                }
                type->angle_expression = multi_init;
            } else {
                if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_GT)) {
                    parser__ast_node_pool_free(state->pool, angle_expr);
                    parser__free_type(type);
                    if (!silent) {
                        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                           "Expected '>' after expression in angle brackets");
                    }
                    return NULL;
                }
                advance_token(state);
                type->angle_expression = angle_expr;
            }
        }
    }
    return type;
}

/*
 * Wrapper for parse_type_specifier_silent with error reporting enabled.
 */
static Type *parse_type_specifier(ParserState *state, bool parse_prefixes) {
    return parse_type_specifier_silent(state, false, parse_prefixes);
}

/*
 * Expression parsing.
 */

/*
 * Parses an expression (entry point for expression parsing).
 */
static ASTNode *parse_expression(ParserState *state) {
    /* State modifier no longer exists as a token; expressions cannot start with state keyword. */
    return parse_assignment_expression(state);
}

/*
 * Parses assignment expressions ( = , += , -= , etc.)
 */
static ASTNode *parse_assignment_expression(ParserState *state) {
    ASTNode *left = parse_ternary_expression(state);
    if (!left) return NULL;
    if (left->type == AST_MULTI_INITIALIZER) {
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EQUAL)) return left;
        advance_token(state);
        ASTNode *right = parse_expression(state);
        if (!right) {
            parser__ast_node_pool_free(state->pool, left);
            return NULL;
        }
        return create_ast_node(state, AST_MULTI_ASSIGNMENT, 0, NULL, left, right, NULL);
    }
    static const TokenType assignment_ops[] = {
        TOKEN_EQUAL, TOKEN_PLUS_EQ, TOKEN_MINUS_EQ, TOKEN_STAR_EQ, TOKEN_SLASH_EQ,
        TOKEN_PERCENT_EQ, TOKEN_PIPE_EQ, TOKEN_AMPERSAND_EQ, TOKEN_CARET_EQ,
        TOKEN_SHL_EQ, TOKEN_SHR_EQ, TOKEN_SAL_EQ, TOKEN_SAR_EQ, TOKEN_ROL_EQ, TOKEN_ROR_EQ
    };
    TokenType current = get_current_token_type(state);
    for (uint8_t i = 0; i < ARRAY_SIZE(assignment_ops); i++) {
        if (current == assignment_ops[i]) {
            advance_token(state);
            ASTNode *right = parse_assignment_expression(state);
            if (!right) {
                parser__ast_node_pool_free(state->pool, left);
                return NULL;
            }
            return AST_NEW_ASSIGNMENT(state, current, left, right);
        }
    }
    return left;
}

/*
 * Parses ternary conditional expressions (cond ? {true} : {false}).
 */
static ASTNode *parse_ternary_expression(ParserState *state) {
    ASTNode *condition = parse_logical_expression(state);
    if (!condition) return NULL;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_QUESTION)) {
        advance_token(state);
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LCURLY)) {
            parser__ast_node_pool_free(state->pool, condition);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Expected '{' after '?' in ternary expression");
        }
        ASTNode *true_expr = parse_block(state);
        if (!true_expr) {
            parser__ast_node_pool_free(state->pool, condition);
            return NULL;
        }
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
            parser__ast_node_pool_free(state->pool, condition);
            parser__ast_node_pool_free(state->pool, true_expr);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Expected ':' in ternary operator");
        }
        advance_token(state);
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LCURLY)) {
            parser__ast_node_pool_free(state->pool, condition);
            parser__ast_node_pool_free(state->pool, true_expr);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Expected '{' after ':' in ternary expression");
        }
        ASTNode *false_expr = parse_block(state);
        if (!false_expr) {
            parser__ast_node_pool_free(state->pool, condition);
            parser__ast_node_pool_free(state->pool, true_expr);
            return NULL;
        }
        return AST_NEW_TERNARY(state, condition, true_expr, false_expr);
    }
    return condition;
}

/*
 * Parses logical OR expressions (||).
 */
static ASTNode *parse_logical_expression(ParserState *state) {
    static const TokenType operators[] = { TOKEN_LOGICAL };
    return parse_binary_operation_universal(state, parse_bitwise_or_expression,
                                            (TokenType*)operators, ARRAY_SIZE(operators));
}

/*
 * Parses bitwise OR expressions (|).
 */
static ASTNode *parse_bitwise_or_expression(ParserState *state) {
    static const TokenType operators[] = { TOKEN_PIPE };
    return parse_binary_operation_universal(state, parse_bitwise_xor_expression,
                                            (TokenType*)operators, ARRAY_SIZE(operators));
}

/*
 * Parses bitwise XOR expressions (^) and NE_TILDE (special).
 */
static ASTNode *parse_bitwise_xor_expression(ParserState *state) {
    static const TokenType operators[] = { TOKEN_CARET, TOKEN_NE_TILDE };
    return parse_binary_operation_universal(state, parse_bitwise_and_expression,
                                            (TokenType*)operators, ARRAY_SIZE(operators));
}

/*
 * Parses bitwise AND expressions (&).
 */
static ASTNode *parse_bitwise_and_expression(ParserState *state) {
    static const TokenType operators[] = { TOKEN_AMPERSAND };
    return parse_binary_operation_universal(state, parse_equality_expression,
                                            (TokenType*)operators, ARRAY_SIZE(operators));
}

/*
 * Parses equality expressions (==, !=).
 */
static ASTNode *parse_equality_expression(ParserState *state) {
    static const TokenType operators[] = { TOKEN_DOUBLE_EQ, TOKEN_NE };
    return parse_binary_operation_universal(state, parse_relational_expression,
                                            (TokenType*)operators, ARRAY_SIZE(operators));
}

/*
 * Parses relational expressions (<, >, <=, >=).
 */
static ASTNode *parse_relational_expression(ParserState *state) {
    static const TokenType operators[] = { TOKEN_LT, TOKEN_GT, TOKEN_LE, TOKEN_GE };
    return parse_binary_operation_universal(state, parse_shift_expression,
                                            (TokenType*)operators, ARRAY_SIZE(operators));
}

/*
 * Parses shift expressions (<<, >>, sal, sar, rol, ror).
 */
static ASTNode *parse_shift_expression(ParserState *state) {
    static const TokenType operators[] = {
        TOKEN_SHL, TOKEN_SHR, TOKEN_SAL, TOKEN_SAR, TOKEN_ROL, TOKEN_ROR
    };
    return parse_binary_operation_universal(state, parse_additive_expression,
                                            (TokenType*)operators, ARRAY_SIZE(operators));
}

/*
 * Parses additive expressions (+, -).
 */
static ASTNode *parse_additive_expression(ParserState *state) {
    static const TokenType operators[] = { TOKEN_PLUS, TOKEN_MINUS };
    return parse_binary_operation_universal(state, parse_multiplicative_expression,
                                            (TokenType*)operators, ARRAY_SIZE(operators));
}

/*
 * Parses multiplicative expressions (*, /, %).
 */
static ASTNode *parse_multiplicative_expression(ParserState *state) {
    static const TokenType operators[] = { TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT };
    return parse_binary_operation_universal(state, parse_unary_expression,
                                            (TokenType*)operators, ARRAY_SIZE(operators));
}

/*
 * Parses unary expressions (prefix ops, casts, postfix).
 * Unary operators: @ (address-of), & (reference), ++, --, !, ~, *, /.
 * Cast expressions of the form (Type)expr are also handled here.
 */
static ASTNode *parse_unary_expression(ParserState *state) {
    /* Try to parse a cast: (Type) expr */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
        int saved_pos = state->current_token_position;
        advance_token(state);
        Type *cast_type = parse_type_specifier_silent(state, true, true);
        if (cast_type && CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
            advance_token(state);
            ASTNode *expression = parse_unary_expression(state);
            if (!expression) {
                parser__free_type(cast_type);
                return NULL;
            }
            ASTNode *node = create_ast_node(state, AST_CAST, 0, NULL, expression, NULL, NULL);
            node->variable_type = cast_type;
            return node;
        }
        /* Not a valid cast – backtrack */
        if (cast_type) parser__free_type(cast_type);
        state->current_token_position = saved_pos;
    }

    /* Prefix increment / decrement */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOUBLE_PLUS) ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOUBLE_MINUS)) {
        TokenType operation = get_current_token_type(state);
        advance_token(state);
        ASTNode *operand = parse_unary_expression(state);
        if (!operand) return NULL;
        ASTNodeType node_type = (operation == TOKEN_DOUBLE_PLUS) ? AST_PREFIX_INCREMENT : AST_PREFIX_DECREMENT;
        return create_ast_node(state, node_type, operation, NULL, NULL, operand, NULL);
    }

    /* Unary operators: !  ~  *  /  @  & */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_BANG) ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_TILDE) ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_STAR)   ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SLASH)  ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_AT)     ||   /* address‑of */
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_AMPERSAND))  /* reference */
    {
        TokenType operation = get_current_token_type(state);
        advance_token(state);
        ASTNode *operand = parse_unary_expression(state);
        if (!operand) return NULL;
        return AST_NEW_UNARY(state, operation, operand);
    }

    /* No unary operator – parse a primary expression and possibly postfix operators */
    ASTNode *primary = parse_primary_expression(state);
    if (!primary) return NULL;
    return parse_postfix_expression(state, primary);
}

/*
 * Parses primary expressions: literals, identifiers, parenthesized expressions, etc.
 */
static ASTNode *parse_primary_expression(ParserState *state) {
    Token *token = get_current_token(state);
    if (!token) REPORT_UNEXPECTED_EOF(state);
    if (token->type == TOKEN_PERCENT) {
        advance_token(state);
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected register name after '%%'");
        }
        token = get_current_token(state);
        char *value = safe_strdup(state, token->value);
        advance_token(state);
        return create_ast_node(state, AST_REGISTER, 0, value, NULL, NULL, NULL);
    }
    if (token->type == TOKEN_LPAREN) {
        advance_token(state);
        int saved_pos = state->current_token_position;
        Type *cast_type = parse_type_specifier_silent(state, true, true);
        if (cast_type && CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
            advance_token(state);
            ASTNode *expression = parse_unary_expression(state);
            if (!expression) {
                parser__free_type(cast_type);
                return NULL;
            }
            ASTNode *node = create_ast_node(state, AST_CAST, 0, NULL, expression, NULL, NULL);
            node->variable_type = cast_type;
            return node;
        } else {
            if (cast_type) parser__free_type(cast_type);
            state->current_token_position = saved_pos;
            ASTNode *expression = parse_expression(state);
            if (!expression) return NULL;
            CONSUME_TOKEN(state, TOKEN_RPAREN);
            return expression;
        }
    }
    if (token->type == TOKEN_LCURLY) return parse_block(state);
    if (token->type == TOKEN_SIZEOF) {
        advance_token(state);
        CONSUME_TOKEN(state, TOKEN_LPAREN);
        ASTNode *arguments = parse_expression(state);
        if (!arguments) return NULL;
        CONSUME_TOKEN(state, TOKEN_RPAREN);
        return create_ast_node(state, TOKEN_SIZEOF, 0, NULL, arguments, NULL, NULL);
    }
    if (token->type == TOKEN_TYPEOF) {
        advance_token(state);
        CONSUME_TOKEN(state, TOKEN_LPAREN);
        ASTNode *expr = parse_expression(state);
        if (!expr) return NULL;
        CONSUME_TOKEN(state, TOKEN_RPAREN);
        return create_ast_node(state, AST_TYPEOF, 0, NULL, expr, NULL, NULL);
    }
    if (token->type == TOKEN_ALLOC) return parse_alloc_expression(state);
    if (token->type == TOKEN_REALLOC) return parse_realloc_expression(state);
    if (token->type == TOKEN_INTERFLAG) {
        advance_token(state);
        return create_ast_node(state, AST_INTERFLAG, 0, NULL, NULL, NULL, NULL);
    }
    if (token->type == TOKEN_NUMBER || token->type == TOKEN_STRING ||
        token->type == TOKEN_CHAR || token->type == TOKEN_NONE ||
        token->type == TOKEN_TYPE) {
        char *value = safe_strdup(state, token->value);
        advance_token(state);
        return create_ast_node(state, AST_LITERAL_VALUE, token->type, value, NULL, NULL, NULL);
    }
    if (token->type == TOKEN_ID) {
        char *value = safe_strdup(state, token->value);
        advance_token(state);
        ASTNode *node = create_ast_node(state, AST_IDENTIFIER, 0, value, NULL, NULL, NULL);
        return parse_postfix_expression(state, node);
    }
    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Invalid syntax in expression");
}

/*
 * Parses postfix expressions: ++, --, function calls, array access, field access, postfix cast.
 */
static ASTNode *parse_postfix_expression(ParserState *state, ASTNode *node) {
    while (1) {
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOUBLE_PLUS)) {
            advance_token(state);
            node = create_ast_node(state, AST_POSTFIX_INCREMENT, TOKEN_DOUBLE_PLUS, NULL, node, NULL, NULL);
        } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOUBLE_MINUS)) {
            advance_token(state);
            node = create_ast_node(state, AST_POSTFIX_DECREMENT, TOKEN_DOUBLE_MINUS, NULL, node, NULL, NULL);
        } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
            advance_token(state);
            AST *arguments = parse_universal_list(state, parse_expression, NULL, TOKEN_COMMA, TOKEN_RPAREN);
            if (!arguments) {
                parser__ast_node_pool_free(state->pool, node);
                return NULL;
            }
            ASTNode *func_call = create_ast_node(state, AST_FUNCTION_DECLARATION, 0, NULL,
                                                 node, NULL, (ASTNode*)arguments);
            if (!func_call) {
                parser__free_ast(arguments);
                parser__ast_node_pool_free(state->pool, node);
                return NULL;
            }
            node = func_call;
        } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LBRACE)) {
            advance_token(state);
            ASTNode *index_expr = parse_expression(state);
            if (!index_expr) {
                parser__ast_node_pool_free(state->pool, node);
                return NULL;
            }
            CONSUME_TOKEN(state, TOKEN_RBRACE);
            node = create_ast_node(state, AST_ARRAY_ACCESS, 0, NULL, node, index_expr, NULL);
        } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOT)) {
            advance_token(state);
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
                advance_token(state);
                Type *target_type = parse_type_specifier(state, true);
                if (!target_type) {
                    parser__ast_node_pool_free(state->pool, node);
                    return NULL;
                }
                CONSUME_TOKEN(state, TOKEN_RPAREN);
                node = create_ast_node(state, AST_POSTFIX_CAST, 0, NULL, node, NULL, NULL);
                node->variable_type = target_type;
            } else {
                if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
                    parser__ast_node_pool_free(state->pool, node);
                    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected field name after '->'");
                }
                Token *field_token = get_current_token(state);
                char *field_name = safe_strdup(state, field_token->value);
                advance_token(state);
                ASTNode *field_node = create_ast_node(state, AST_IDENTIFIER, 0, field_name, NULL, NULL, NULL);
                node = create_ast_node(state, AST_FIELD_ACCESS, 0, NULL, node, field_node, NULL);
            }
        } else {
            break;
        }
    }
    return node;
}

/*
 * Parses built-in functions with a fixed number of arguments (alloc, realloc).
 */
static ASTNode *parse_fixed_argument_function(ParserState *state, ASTNodeType node_type,
                                              uint8_t arg_count, const char *func_name) {
    advance_token(state);
    AST *arguments = safe_malloc(state, sizeof(AST));
    arguments->nodes = NULL;
    arguments->count = 0;
    arguments->capacity = 0;
    CONSUME_TOKEN(state, TOKEN_LPAREN);
    for (uint8_t i = 0; i < arg_count; i++) {
        ASTNode *arg = parse_expression(state);
        if (!arg) {
            parser__free_ast(arguments);
            return NULL;
        }
        if (!add_ast_node_to_list(arguments, arg)) {
            parser__ast_node_pool_free(state->pool, arg);
            parser__free_ast(arguments);
            return NULL;
        }
        if (i < arg_count - 1) {
            if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
                parser__free_ast(arguments);
                REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                   "Expected comma after argument %d in %s()", i + 1, func_name);
            }
            advance_token(state);
        }
    }
    CONSUME_TOKEN(state, TOKEN_RPAREN);
    if (arguments->count != arg_count) {
        parser__free_ast(arguments);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                           "%s() requires exactly %d arguments", func_name, arg_count);
    }
    ASTNode *arguments_block = create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL, (ASTNode*)arguments);
    if (!arguments_block) {
        parser__free_ast(arguments);
        return NULL;
    }
    ASTNode *func_node = create_ast_node(state, node_type, 0, NULL, arguments_block, NULL, NULL);
    if (!func_node) {
        parser__free_ast_node(arguments_block);
        return NULL;
    }
    return func_node;
}

/*
 * Parses alloc(pointer, size, flags).
 */
static ASTNode *parse_alloc_expression(ParserState *state) {
    return parse_fixed_argument_function(state, AST_ALLOC, 3, "alloc");
}

/*
 * Parses realloc(pointer, new_size).
 */
static ASTNode *parse_realloc_expression(ParserState *state) {
    return parse_fixed_argument_function(state, AST_REALLOC, 2, "realloc");
}

/*
 * Parses a block: { sequence_of_statements }.
 */
static ASTNode *parse_block(ParserState *state) {
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LCURLY)) {
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected '{' to start block");
    }
    advance_token(state);
    ASTNode *block = parse_sequence(state, true);
    if (!block) return NULL;
    CONSUME_TOKEN(state, TOKEN_RCURLY);
    return block;
}

/*
 * Parses a multi-initializer: { expr, expr, ... }
 */
static ASTNode *parse_multi_initializer(ParserState *state) {
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LCURLY)) {
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected '{' for multi-value initializer");
    }
    advance_token(state);
    AST *list = safe_malloc(state, sizeof(AST));
    list->nodes = NULL;
    list->count = 0;
    list->capacity = 0;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY)) {
        advance_token(state);
        return create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL, NULL, NULL, (ASTNode*)list);
    }
    while (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY) && !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
        ASTNode *expression = parse_expression(state);
        if (!expression) {
            parser__free_ast(list);
            return NULL;
        }
        if (!add_ast_node_to_list(list, expression)) {
            parser__ast_node_pool_free(state->pool, expression);
            parser__free_ast(list);
            return NULL;
        }
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
            advance_token(state);
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY)) break;
        } else if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY)) {
            parser__free_ast(list);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Expected ',' or '}' in multi-initializer");
        }
    }
    CONSUME_TOKEN(state, TOKEN_RCURLY);
    return create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL, NULL, NULL, (ASTNode*)list);
}

/*
 * Parses an if statement.
 * If the body is a single statement introduced by '=>', then an optional 'else' is NOT consumed;
 * the 'else' will be parsed as a separate statement later.
 */
static ASTNode *parse_if_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_IF);
    CONSUME_TOKEN(state, TOKEN_LPAREN);
    ASTNode *condition = parse_expression(state);
    if (!condition) return NULL;
    CONSUME_TOKEN(state, TOKEN_RPAREN);

    /* Determine whether the body is a block '{' or a single statement via '=>' */
    bool is_single_stmt = CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_THEN);
    ASTNode *if_block = parse_block_or_statement(state);
    if (!if_block) {
        parser__ast_node_pool_free(state->pool, condition);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                           "Expected '=>' or '{' after if condition");
    }

    ASTNode *else_block = NULL;
    /* Only attempt to parse an else clause if the body was a block (not a single statement) */
    if (!is_single_stmt && ATTEMPT_CONSUME_TOKEN(state, TOKEN_ELSE)) {
        else_block = parse_block_or_statement(state);
        if (!else_block) {
            parser__ast_node_pool_free(state->pool, condition);
            parser__ast_node_pool_free(state->pool, if_block);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Expected '=>' or '{' after else");
        }
    }
    return create_ast_node(state, AST_IF_STATEMENT, 0, NULL, condition, if_block, else_block);
}

/*
 * Parses a do loop: do (condition) { body } [ else { ... } ]
 * If the body is a single statement via '=>', an optional 'else' is not consumed.
 */
static ASTNode *parse_do_loop(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_DO);
    CONSUME_TOKEN(state, TOKEN_LPAREN);
    ASTNode *condition = parse_expression(state);
    if (!condition) return NULL;
    CONSUME_TOKEN(state, TOKEN_RPAREN);

    bool is_single_stmt = CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_THEN);
    ASTNode *body = parse_block_or_statement(state);
    if (!body) {
        parser__ast_node_pool_free(state->pool, condition);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                           "Expected '=>' or '{' after do condition");
    }

    ASTNode *else_block = NULL;
    if (!is_single_stmt && ATTEMPT_CONSUME_TOKEN(state, TOKEN_ELSE)) {
        else_block = parse_block_or_statement(state);
        if (!else_block) {
            parser__ast_node_pool_free(state->pool, condition);
            parser__ast_node_pool_free(state->pool, body);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Expected '=>' or '{' after else in do-else");
        }
    }
    return create_ast_node(state, AST_DO_LOOP, 0, NULL, condition, body, else_block);
}

/*
 * Parses a break statement (optionally with .label).
 */
static ASTNode *parse_break_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_BREAK);
    char *label = NULL;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOT)) {
        advance_token(state);
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected label name after '.'");
        }
        Token *label_token = get_current_token(state);
        label = safe_strdup(state, label_token->value);
        advance_token(state);
    }
    return create_ast_node(state, AST_BREAK, 0, label, NULL, NULL, NULL);
}

/*
 * Parses a continue statement (optionally with .label).
 */
static ASTNode *parse_continue_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_CONTINUE);
    char *label = NULL;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOT)) {
        advance_token(state);
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected label name after '.'");
        }
        Token *label_token = get_current_token(state);
        label = safe_strdup(state, label_token->value);
        advance_token(state);
    }
    return create_ast_node(state, AST_CONTINUE, 0, label, NULL, NULL, NULL);
}

/*
 * Parses a signal statement.
 * Syntax: signal [ '(' expression_list? ')' ] ':' expression ';'
 * Example: signal() : 1;
 * The optional parentheses may contain a comma-separated list of expressions (parameters).
 * The expression after colon is the signal type/value.
 * Returns an AST node of type AST_SIGNAL with left child set to an AST_BLOCK node containing
 * the parameter expressions (or NULL if none), and right child set to the type expression.
 */
static ASTNode *parse_signal_statement(ParserState *state) {
    /* Consume 'signal' keyword */
    CONSUME_TOKEN(state, TOKEN_SIGNAL);

    AST *param_list = NULL;

    /* Optional parentheses with parameters */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
        advance_token(state); /* consume '(' */

        /* If next token is not ')', parse parameter expressions */
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
            param_list = safe_malloc(state, sizeof(AST));
            param_list->nodes = NULL;
            param_list->count = 0;
            param_list->capacity = 0;

            /* Parse first parameter expression */
            ASTNode *first_param = parse_expression(state);
            if (!first_param) {
                parser__free_ast(param_list);
                return NULL;
            }
            if (!add_ast_node_to_list(param_list, first_param)) {
                parser__ast_node_pool_free(state->pool, first_param);
                parser__free_ast(param_list);
                return NULL;
            }

            /* Parse remaining comma-separated parameters */
            while (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
                advance_token(state); /* consume ',' */
                if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
                    break; /* trailing comma allowed */
                }
                ASTNode *param = parse_expression(state);
                if (!param) {
                    parser__free_ast(param_list);
                    return NULL;
                }
                if (!add_ast_node_to_list(param_list, param)) {
                    parser__ast_node_pool_free(state->pool, param);
                    parser__free_ast(param_list);
                    return NULL;
                }
            }
        }

        /* Expect closing ')' */
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
            if (param_list) parser__free_ast(param_list);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected ')' after signal parameters");
        }
        advance_token(state); /* consume ')' */
    }

    /* Expect colon */
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
        if (param_list) parser__free_ast(param_list);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected ':' in signal statement");
    }
    advance_token(state); /* consume ':' */

    /* Parse type expression */
    ASTNode *type_expr = parse_expression(state);
    if (!type_expr) {
        if (param_list) parser__free_ast(param_list);
        return NULL;
    }

    /* Create an AST_BLOCK node to hold the parameter list (if any) */
    ASTNode *params_block = NULL;
    if (param_list) {
        params_block = create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL, (ASTNode*)param_list);
        if (!params_block) {
            parser__free_ast(param_list);
            parser__ast_node_pool_free(state->pool, type_expr);
            return NULL;
        }
    }

    /* Create the signal node: left = params block, right = type expression */
    ASTNode *signal_node = create_ast_node(state, AST_SIGNAL, TOKEN_SIGNAL, NULL,
                                           params_block, type_expr, NULL);
    if (!signal_node) {
        if (params_block) parser__free_ast_node(params_block);
        parser__ast_node_pool_free(state->pool, type_expr);
        return NULL;
    }

    return signal_node;
}

/*
 * Parses an interflag statement (optional expression).
 */
static ASTNode *parse_interflag_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_INTERFLAG);
    ASTNode *value = NULL;
    TokenType next = get_current_token_type(state);
    if (next != TOKEN_SEMICOLON && next != TOKEN_RCURLY && next != TOKEN_EOF) {
        value = parse_expression(state);
        if (!value) return NULL;
    }
    return create_ast_node(state, AST_INTERFLAG, 0, NULL, value, NULL, NULL);
}

/*
 * Parses a label declaration: @label:
 */
static ASTNode *parse_label_declaration(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_AT);
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected label name after '@'");
    }
    Token *label_token = get_current_token(state);
    char *label_name = safe_strdup(state, label_token->value);
    advance_token(state);
    CONSUME_TOKEN(state, TOKEN_COLON);
    return create_ast_node(state, AST_LABEL_DECLARATION, 0, label_name, NULL, NULL, NULL);
}

/*
 * Parses a jump statement: jump target_expression
 */
static ASTNode *parse_jump_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_JUMP);
    ASTNode *target = parse_expression(state);
    if (!target) return NULL;
    return create_ast_node(state, AST_JUMP, 0, NULL, target, NULL, NULL);
}

/*
 * Parses a return statement: return [ expr (, expr)* ]
 */
static ASTNode *parse_return_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_RETURN);
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
        return create_ast_node(state, AST_RETURN, 0, NULL, NULL, NULL, NULL);
    }
    AST *return_list = safe_malloc(state, sizeof(AST));
    return_list->nodes = NULL;
    return_list->count = 0;
    return_list->capacity = 0;
    ASTNode *first_expr = parse_expression(state);
    if (!first_expr) {
        parser__free_ast(return_list);
        return NULL;
    }
    if (!add_ast_node_to_list(return_list, first_expr)) {
        parser__ast_node_pool_free(state->pool, first_expr);
        parser__free_ast(return_list);
        return NULL;
    }
    while (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
        advance_token(state);
        ASTNode *next_expr = parse_expression(state);
        if (!next_expr) {
            parser__free_ast(return_list);
            return NULL;
        }
        if (!add_ast_node_to_list(return_list, next_expr)) {
            parser__ast_node_pool_free(state->pool, next_expr);
            parser__free_ast(return_list);
            return NULL;
        }
    }
    if (return_list->count == 0) {
        parser__free_ast(return_list);
        return create_ast_node(state, AST_RETURN, 0, NULL, NULL, NULL, NULL);
    } else if (return_list->count == 1) {
        ASTNode *expr = return_list->nodes[0];
        free(return_list->nodes);
        free(return_list);
        return create_ast_node(state, AST_RETURN, 0, NULL, expr, NULL, NULL);
    } else {
        ASTNode *multi_return = create_ast_node(state, AST_MULTI_INITIALIZER, 0, NULL,
                                                NULL, NULL, (ASTNode*)return_list);
        if (!multi_return) {
            parser__free_ast(return_list);
            return NULL;
        }
        return create_ast_node(state, AST_RETURN, 0, NULL, multi_return, NULL, NULL);
    }
}

/*
 * Parses a free statement: free ( expression ) or free expression
 */
static ASTNode *parse_free_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_FREE);
    ASTNode *expression = NULL;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
        advance_token(state);
        expression = parse_expression(state);
        if (!expression) return NULL;
        CONSUME_TOKEN(state, TOKEN_RPAREN);
    } else {
        expression = parse_expression(state);
        if (!expression) return NULL;
    }
    return create_ast_node(state, AST_FREE, 0, NULL, expression, NULL, NULL);
}

/*
 * Parses a nop statement.
 */
static ASTNode *parse_nop_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_NOP);
    return create_ast_node(state, AST_NOP, 0, NULL, NULL, NULL, NULL);
}

/*
 * Parses a halt statement.
 */
static ASTNode *parse_halt_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_HALT);
    return create_ast_node(state, AST_HALT, 0, NULL, NULL, NULL, NULL);
}

/*
 * Parses a kill statement.
 */
static ASTNode *parse_kill_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_KILL);
    return create_ast_node(state, AST_KILL, 0, NULL, NULL, NULL, NULL);
}

/*
 * Parses a try-catch-else statement.
 * Syntax: try { ... } catch error_code { ... } [ else { ... } ]
 */
static ASTNode *parse_try_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_TRY);
    ASTNode *try_block = parse_block(state);
    if (!try_block) return NULL;

    AST *catch_list = safe_malloc(state, sizeof(AST));
    catch_list->nodes = NULL;
    catch_list->count = 0;
    catch_list->capacity = 0;

    while (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_CATCH)) {
        advance_token(state);
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_NUMBER)) {
            parser__free_ast(catch_list);
            parser__free_ast_node(try_block);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Expected error code after 'catch'");
        }
        Token *num_tok = get_current_token(state);
        if (strncmp(num_tok->value, "0x", 2) != 0 &&
            strncmp(num_tok->value, "0X", 2) != 0) {
            parser__free_ast(catch_list);
            parser__free_ast_node(try_block);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Error code must be a hexadecimal number starting with 0x");
        }
        char *error_code_str = safe_strdup(state, num_tok->value);
        advance_token(state);
        ASTNode *catch_block = parse_block(state);
        if (!catch_block) {
            free(error_code_str);
            parser__free_ast(catch_list);
            parser__free_ast_node(try_block);
            return NULL;
        }
        ASTNode *catch_node = create_ast_node(state, AST_CATCH, 0,
                                              error_code_str, catch_block, NULL, NULL);
        if (!catch_node) {
            free(error_code_str);
            parser__free_ast_node(catch_block);
            parser__free_ast(catch_list);
            parser__free_ast_node(try_block);
            return NULL;
        }
        if (!add_ast_node_to_list(catch_list, catch_node)) {
            parser__ast_node_pool_free(state->pool, catch_node);
            parser__free_ast(catch_list);
            parser__free_ast_node(try_block);
            return NULL;
        }
    }

    ASTNode *else_block = NULL;
    if (ATTEMPT_CONSUME_TOKEN(state, TOKEN_ELSE)) {
        else_block = parse_block(state);
        if (!else_block) {
            parser__free_ast(catch_list);
            parser__free_ast_node(try_block);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Expected '{' after else in try-catch");
        }
        ASTNode *default_catch = create_ast_node(state, AST_CATCH, 0, NULL, else_block, NULL, NULL);
        if (!default_catch) {
            parser__free_ast_node(else_block);
            parser__free_ast(catch_list);
            parser__free_ast_node(try_block);
            return NULL;
        }
        if (!add_ast_node_to_list(catch_list, default_catch)) {
            parser__ast_node_pool_free(state->pool, default_catch);
            parser__free_ast(catch_list);
            parser__free_ast_node(try_block);
            return NULL;
        }
    }

    if (catch_list->count == 0) {
        parser__free_ast(catch_list);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                           "try statement must have at least one catch or else block");
    }

    ASTNode *try_node = create_ast_node(state, AST_TRY, 0, NULL,
                                        try_block, NULL, (ASTNode*)catch_list);
    if (!try_node) {
        parser__free_ast(catch_list);
        parser__free_ast_node(try_block);
    }
    return try_node;
}

/*
 * Parses a standalone else statement.
 * Syntax: else ( { block } | => statement )
 * This allows 'else' to appear independently when an 'if' or 'do' body used '=>'.
 * The body is parsed using parse_block_or_statement.
 */
static ASTNode *parse_else_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_ELSE);
    ASTNode *body = parse_block_or_statement(state);
    if (!body) {
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                           "Expected '=>' or '{' after standalone else");
    }
    return create_ast_node(state, AST_ELSE_STATEMENT, 0, NULL, NULL, body, NULL);
}

/*
 * Parse a single variable declarator.
 * Syntax: [@*] [&] name [ : Type ] [ = initializer ]
 * The state_modifier and declaration modifiers are attached to the resulting AST node.
 * Returns an AST node of type AST_VARIABLE_DECLARATION.
 */
static ASTNode *parse_single_declarator(ParserState *state, char *state_modifier,
                                        char **modifiers, uint8_t modifier_count,
                                        Type *common_type, bool allow_type) {
    uint8_t name_pointer_level = 0, name_is_reference = 0;
    parse_type_prefixes(state, &name_pointer_level, &name_is_reference);

    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected variable name");
    }
    Token *name_token = get_current_token(state);
    char *name = safe_strdup(state, name_token->value);
    advance_token(state);

    Type *var_type = NULL;
    if (common_type) {
        var_type = safe_malloc(state, sizeof(Type));
        memcpy(var_type, common_type, sizeof(Type));
        /* Deep copy strings and arrays to avoid double‑free issues. */
        var_type->name = safe_strdup(state, common_type->name);
        var_type->access_modifier = common_type->access_modifier ? safe_strdup(state, common_type->access_modifier) : NULL;
        var_type->modifiers = NULL;
        if (common_type->modifier_count > 0) {
            var_type->modifiers = safe_malloc(state, common_type->modifier_count * sizeof(char*));
            for (uint8_t i = 0; i < common_type->modifier_count; i++) {
                var_type->modifiers[i] = safe_strdup(state, common_type->modifiers[i]);
            }
            var_type->modifier_count = common_type->modifier_count;
        }
        var_type->compound_types = NULL;
        var_type->compound_count = 0;
        var_type->array_dimensions = NULL;
        var_type->angle_expression = NULL;
        var_type->typeof_expression = NULL;

        /* Apply pointer/reference prefixes to the copied type. */
        var_type->pointer_level += name_pointer_level;
        if (name_is_reference) var_type->is_reference = 1;
    } else if (allow_type && CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
        advance_token(state);
        var_type = parse_type_specifier(state, true);
        if (!var_type) {
            free(name);
            return NULL;
        }
        var_type->pointer_level += name_pointer_level;
        if (name_is_reference) var_type->is_reference = 1;
    } else {
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
            /* A colon appears but we are not allowed to have a type (common_type provided).
             * This is a syntax error. */
            free(name);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Type specifier not allowed here; a common type was already given for the list");
        }
        if (name_pointer_level || name_is_reference) {
            var_type = create_temp_type_with_prefixes(state, name_pointer_level, name_is_reference);
        }
    }

    ASTNode *initializer = NULL;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EQUAL)) {
        advance_token(state);
        initializer = parse_expression(state);
        if (!initializer) {
            free(name);
            if (var_type) parser__free_type(var_type);
            return NULL;
        }
    }

    ASTNode *decl_node = create_ast_node(state, AST_VARIABLE_DECLARATION, 0, name, NULL, NULL, NULL);
    if (!decl_node) {
        free(name);
        if (var_type) parser__free_type(var_type);
        if (initializer) parser__ast_node_pool_free(state->pool, initializer);
        return NULL;
    }
    decl_node->state_modifier = safe_strdup(state, state_modifier);
    decl_node->variable_type = var_type;
    decl_node->default_value = initializer;

    if (modifier_count > 0) {
        decl_node->modifiers = malloc(modifier_count * sizeof(char*));
        for (uint8_t i = 0; i < modifier_count; i++) {
            decl_node->modifiers[i] = safe_strdup(state, modifiers[i]);
        }
        decl_node->modifier_count = modifier_count;
    }
    return decl_node;
}

/*
 * Parse a comma‑separated list of variable declarators.
 * Examples: var a : Int = 1, b = 2, c : Int;
 * The state_modifier (e.g., "var") and modifiers apply to all declarators in the list.
 * Returns an AST node of type AST_VARIABLE_LIST whose extra field holds an AST* list
 * of individual AST_VARIABLE_DECLARATION nodes.
 */
static ASTNode *parse_variable_declaration_list(ParserState *state, char *state_modifier,
                                                char **modifiers, uint8_t modifier_count,
                                                Type *common_type) {
    AST *decl_list = safe_malloc(state, sizeof(AST));
    decl_list->nodes = NULL;
    decl_list->count = 0;
    decl_list->capacity = 0;

    /* When a common type is provided, individual declarators cannot have their own ': Type'. */
    bool allow_type = (common_type == NULL);

    ASTNode *first = parse_single_declarator(state, state_modifier, modifiers, modifier_count,
                                             common_type, allow_type);
    if (!first) {
        parser__free_ast(decl_list);
        return NULL;
    }
    if (!add_ast_node_to_list(decl_list, first)) {
        parser__ast_node_pool_free(state->pool, first);
        parser__free_ast(decl_list);
        return NULL;
    }

    while (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
        advance_token(state);
        ASTNode *next = parse_single_declarator(state, state_modifier, modifiers, modifier_count,
                                                common_type, allow_type);
        if (!next) {
            parser__free_ast(decl_list);
            return NULL;
        }
        if (!add_ast_node_to_list(decl_list, next)) {
            parser__ast_node_pool_free(state->pool, next);
            parser__free_ast(decl_list);
            return NULL;
        }
    }

    ASTNode *list_node = create_ast_node(state, AST_VARIABLE_LIST, 0, NULL, NULL, NULL, (ASTNode*)decl_list);
    if (!list_node) {
        parser__free_ast(decl_list);
        return NULL;
    }
    list_node->state_modifier = safe_strdup(state, state_modifier);
    return list_node;
}

/*
 * Parse a function declaration or definition.
 * Syntax: name ( parameters ) [ : return_type ] [ = default_value ] ( { body } | => statement | ; )
 */
static ASTNode *parse_function_declaration(ParserState *state, char *state_modifier, char *name,
                                           char **modifiers, uint8_t modifier_count) {
    AST *param_list = parse_parameter_list(state);
    if (!param_list) {
        free(name);
        return NULL;
    }

    Type *return_type = NULL;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
        advance_token(state);
        return_type = parse_type_specifier(state, true);
        if (!return_type) {
            free(name);
            parser__free_ast(param_list);
            return NULL;
        }
    }

    /* Optional default value via '=' (for function aliases / default implementations) */
    ASTNode *default_value = NULL;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EQUAL)) {
        advance_token(state);
        default_value = parse_expression(state);
        if (!default_value) {
            free(name);
            parser__free_type(return_type);
            parser__free_ast(param_list);
            return NULL;
        }
    }

    ASTNode *body = NULL;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_THEN) || CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LCURLY)) {
        body = parse_block_or_statement(state);
        if (!body) {
            free(name);
            parser__free_type(return_type);
            parser__free_ast(param_list);
            if (default_value) parser__free_ast_node(default_value);
            return NULL;
        }
    }

    ASTNode *params_node = create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL, (ASTNode*)param_list);
    if (!params_node) {
        free(name);
        parser__free_type(return_type);
        parser__free_ast(param_list);
        if (default_value) parser__free_ast_node(default_value);
        if (body) parser__free_ast_node(body);
        return NULL;
    }

    ASTNode *func_node = create_ast_node(state, AST_FUNCTION_DECLARATION, 0, name,
                                         params_node, body, NULL);
    if (!func_node) {
        parser__free_type(return_type);
        parser__free_ast(param_list);
        parser__free_ast_node(params_node);
        if (default_value) parser__free_ast_node(default_value);
        if (body) parser__free_ast_node(body);
        return NULL;
    }
    func_node->variable_type = return_type;
    func_node->state_modifier = safe_strdup(state, state_modifier);
    func_node->default_value = default_value;

    if (modifier_count > 0) {
        func_node->modifiers = malloc(modifier_count * sizeof(char*));
        for (uint8_t i = 0; i < modifier_count; i++) {
            func_node->modifiers[i] = safe_strdup(state, modifiers[i]);
        }
        func_node->modifier_count = modifier_count;
    }
    return func_node;
}

/*
 * Parse a struct declaration.
 * Syntax: name : Type { [ extends OtherStruct; ] ... }
 * The optional 'extends' statement appears inside the struct body.
 * The closing brace must be followed by a semicolon.
 */
static ASTNode *parse_struct_declaration(ParserState *state, char *state_modifier, char *name,
                                         char **modifiers, uint8_t modifier_count) {
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
        free(name);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                           "Expected ':' before 'Type' in struct declaration");
    }
    advance_token(state); /* consume ':' */

    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_TYPE) ||
        strcmp(get_current_token(state)->value, "Type") != 0) {
        free(name);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                           "Expected 'Type' after ':' in struct declaration");
    }
    advance_token(state);

    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LCURLY)) {
        free(name);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                           "Expected '{' to start struct body");
    }
    advance_token(state); /* consume '{' */

    /* We'll build a block AST for the body statements, but we also need to extract
     * any 'extends' statement. */
    AST *stmt_list = safe_malloc(state, sizeof(AST));
    stmt_list->nodes = NULL;
    stmt_list->count = 0;
    stmt_list->capacity = 0;

    char *parent_struct = NULL;

    while (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY) &&
           !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {

        /* Check for 'extends' statement */
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EXTENDS)) {
            /* If we already have a parent, it's a duplicate; we can either ignore or error.
             * We'll allow only one and ignore subsequent ones. */
            advance_token(state); /* consume 'extends' */
            if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
                parser__free_ast(stmt_list);
                free(name);
                free(parent_struct);
                REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                   "Expected struct name after 'extends'");
            }
            Token *parent_tok = get_current_token(state);
            if (!parent_struct) {
                parent_struct = safe_strdup(state, parent_tok->value);
            }
            advance_token(state); /* consume parent name */
            /* Expect semicolon */
            if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
                parser__free_ast(stmt_list);
                free(name);
                free(parent_struct);
                REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                                   "Expected ';' after 'extends' statement");
            }
            advance_token(state); /* consume ';' */
            /* Do not add 'extends' to the statement list */
            continue;
        }

        /* Otherwise, parse a regular statement */
        ASTNode *stmt = parse_statement(state);
        if (stmt) {
            if (!add_ast_node_to_list(stmt_list, stmt)) {
                parser__ast_node_pool_free(state->pool, stmt);
                parser__free_ast(stmt_list);
                free(name);
                free(parent_struct);
                return NULL;
            }
            if (statement_requires_semicolon(stmt)) {
                parse_semicolon(state);
            } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
                advance_token(state);
            }
        } else {
            /* Empty statement (just a semicolon) */
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
                advance_token(state);
            }
        }
    }

    CONSUME_TOKEN(state, TOKEN_RCURLY);

    /* Type declaration requires a terminating semicolon */
    parse_semicolon(state);

    ASTNode *body_block = create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL, (ASTNode*)stmt_list);
    if (!body_block) {
        parser__free_ast(stmt_list);
        free(name);
        free(parent_struct);
        return NULL;
    }

    ASTNode *struct_node = create_ast_node(state, AST_COMPOUND_TYPE, 0, name, NULL, body_block, NULL);
    if (!struct_node) {
        parser__free_ast_node(body_block);
        free(parent_struct);
        return NULL;
    }

    struct_node->parent_struct = parent_struct;          /* may be NULL */
    struct_node->state_modifier = safe_strdup(state, state_modifier);   /* "struct" */

    /* Attach declaration modifiers (extern, static, etc.) if any */
    if (modifier_count > 0) {
        struct_node->modifiers = malloc(modifier_count * sizeof(char*));
        if (!struct_node->modifiers) {
            REPORT_PARSE_ERROR(state, ERROR_CODE_MEMORY_ALLOCATION,
                               "Failed to allocate modifiers for struct");
        }
        for (uint8_t i = 0; i < modifier_count; i++) {
            struct_node->modifiers[i] = safe_strdup(state, modifiers[i]);
        }
        struct_node->modifier_count = modifier_count;
    }

    return struct_node;
}

/*
 * Object declaration dispatcher.
 * Determines the kind of declaration (variable, object instance, function, struct)
 * based on the content rather than a leading state token.
 * Returns NULL if no declaration can be parsed and allow_expression is false.
 * If allow_expression is true, attempts to parse an expression as fallback.
 */
static ASTNode *parse_object_declaration(ParserState *state, bool allow_expression) {
    int saved_pos = state->current_token_position;

    /* First, parse any declaration modifiers (extern, const, etc.) */
    char **modifiers = NULL;
    uint8_t modifier_count = 0;
    parse_declaration_modifiers(state, &modifiers, &modifier_count);

    /* Look ahead to see if this could be a declaration. */
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
        if (allow_expression) {
            /* No identifier -> not a declaration. Clean up modifiers and try expression. */
            if (modifiers) {
                for (uint8_t i = 0; i < modifier_count; i++) free(modifiers[i]);
                free(modifiers);
            }
            state->current_token_position = saved_pos;
            return parse_expression(state);
        }
        /* Not an expression context: error. */
        state->current_token_position = saved_pos;
        return NULL;
    }

    /* We have an identifier. Save it for later. */
    Token *name_token = get_current_token(state);
    char *name = safe_strdup(state, name_token->value);
    advance_token(state); /* consume identifier */

    /* Determine the declaration type by looking at the next token. */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
        /* After ':' could be a type (variable or object instance) or 'Type' (struct). */
        int after_colon_pos = state->current_token_position;
        advance_token(state); /* consume ':' */
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_TYPE) &&
            strcmp(get_current_token(state)->value, "Type") == 0) {
            /* It's a struct declaration: name : Type { ... } */
            /* We need to rewind to before the colon because parse_struct_declaration
             * expects to parse the ': Type' part. */
            state->current_token_position = saved_pos;
            /* Skip modifiers again */
            parse_declaration_modifiers(state, &modifiers, &modifier_count);
            /* Consume name */
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
                name = safe_strdup(state, get_current_token(state)->value);
                advance_token(state);
            } else {
                /* Should not happen */
                free(name);
                return NULL;
            }
            ASTNode *struct_node = parse_struct_declaration(state, "struct", name, modifiers, modifier_count);
            free(modifiers);
            return struct_node;
        } else {
            /* It's a variable or object instance: name : Type */
            /* Rewind to before the colon to let parse_variable_declaration_list handle it. */
            state->current_token_position = after_colon_pos;
            /* Actually, we need to rewind all the way to before the name so that
             * parse_variable_declaration_list can parse the name and colon. */
            state->current_token_position = saved_pos;
            /* Skip modifiers again */
            parse_declaration_modifiers(state, &modifiers, &modifier_count);
            /* Now parse the variable list. The state_modifier will be "var" for variables,
             * but we can also have "obj" for object instances. Since we can't distinguish
             * syntactically, we'll use "var" as default. (Semantic analysis can refine.) */
            ASTNode *var_list = parse_variable_declaration_list(state, "var", modifiers, modifier_count, NULL);
            free(modifiers);
            return var_list;
        }
    } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
        /* It's a function: name ( ... ) */
        /* Rewind to before the name. */
        state->current_token_position = saved_pos;
        /* Skip modifiers again */
        parse_declaration_modifiers(state, &modifiers, &modifier_count);
        /* Consume name */
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
            name = safe_strdup(state, get_current_token(state)->value);
            advance_token(state);
        } else {
            free(name);
            return NULL;
        }
        ASTNode *func_node = parse_function_declaration(state, "func", name, modifiers, modifier_count);
        free(modifiers);
        return func_node;
    } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON) ||
               CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EQUAL) ||
               CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
        /* It's a variable without explicit type: name [ = initializer ] */
        state->current_token_position = saved_pos;
        parse_declaration_modifiers(state, &modifiers, &modifier_count);
        ASTNode *var_list = parse_variable_declaration_list(state, "var", modifiers, modifier_count, NULL);
        free(modifiers);
        return var_list;
    } else {
        /* Not a declaration. */
        if (allow_expression) {
            /* Clean up and try expression. */
            free(name);
            if (modifiers) {
                for (uint8_t i = 0; i < modifier_count; i++) free(modifiers[i]);
                free(modifiers);
            }
            state->current_token_position = saved_pos;
            return parse_expression(state);
        } else {
            free(name);
            if (modifiers) {
                for (uint8_t i = 0; i < modifier_count; i++) free(modifiers[i]);
                free(modifiers);
            }
            state->current_token_position = saved_pos;
            return NULL;
        }
    }
}

/*
 * Parameter parsing for functions.
 * A parameter can be a full variable declaration (with optional state modifier),
 * a type literal (used as a type argument), or a plain expression.
 */
static ASTNode *parse_parameter(ParserState *state) {
    int saved_pos = state->current_token_position;
    /* Since there is no TOKEN_STATE, we try to parse a variable declaration (name [: Type] [= default]) */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
        Token *name_token = get_current_token(state);
        char *name = safe_strdup(state, name_token->value);
        advance_token(state);
        Type *type = NULL;
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
            advance_token(state);
            type = parse_type_specifier(state, true);
            if (!type) {
                free(name);
                state->current_token_position = saved_pos;
                return parse_expression(state);
            }
        }
        ASTNode *default_value = NULL;
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EQUAL)) {
            advance_token(state);
            default_value = parse_expression(state);
            if (!default_value) {
                free(name);
                parser__free_type(type);
                state->current_token_position = saved_pos;
                return parse_expression(state);
            }
        }
        ASTNode *node = create_ast_node(state, AST_VARIABLE_DECLARATION, 0, name, NULL, NULL, NULL);
        node->variable_type = type;
        node->state_modifier = safe_strdup(state, "var"); /* default to "var" for parameters */
        node->default_value = default_value;
        return node;
    }
    /* Try to parse a type literal (e.g., Int) */
    Type *type_literal = parse_type_specifier_silent(state, true, true);
    if (type_literal) {
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA) || CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
            ASTNode *node = create_ast_node(state, AST_LITERAL_VALUE, TOKEN_TYPE,
                                            safe_strdup(state, type_literal->name), NULL, NULL, NULL);
            parser__free_type(type_literal);
            return node;
        } else {
            parser__free_type(type_literal);
            state->current_token_position = saved_pos;
        }
    }
    return parse_expression(state);
}

/*
 * Parse a parameter list enclosed in parentheses.
 * Supports the special `none` keyword as a placeholder for an empty list.
 */
static AST *parse_parameter_list(ParserState *state) {
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected '(' for parameter list");
    }
    advance_token(state);
    AST *param_list = safe_malloc(state, sizeof(AST));
    param_list->nodes = NULL;
    param_list->count = 0;
    param_list->capacity = 0;

    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_NONE)) {
        advance_token(state);
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
            advance_token(state);
            return param_list;
        } else {
            parser__free_ast(param_list);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Expected ')' after 'none' in parameter list");
        }
    }

    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
        advance_token(state);
        return param_list;
    }

    while (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN) && !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
        ASTNode *param = parse_parameter(state);
        if (!param) {
            parser__free_ast(param_list);
            return NULL;
        }
        if (!add_ast_node_to_list(param_list, param)) {
            parser__ast_node_pool_free(state->pool, param);
            parser__free_ast(param_list);
            return NULL;
        }
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
            advance_token(state);
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) break;
        } else if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
            parser__free_ast(param_list);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Expected ',' or ')' in parameter list");
        }
    }
    CONSUME_TOKEN(state, TOKEN_RPAREN);
    return param_list;
}

/*
 * Block or single statement prefixed by '=>'.
 */
static ASTNode *parse_block_or_statement(ParserState *state) {
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LCURLY)) {
        return parse_block(state);
    } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_THEN)) {
        advance_token(state);
        ASTNode *stmt = parse_statement(state);
        if (!stmt) return NULL;
        AST *list = safe_malloc(state, sizeof(AST));
        list->nodes = NULL;
        list->count = 0;
        list->capacity = 0;
        if (!add_ast_node_to_list(list, stmt)) {
            parser__ast_node_pool_free(state->pool, stmt);
            parser__free_ast(list);
            return NULL;
        }
        return create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL, (ASTNode*)list);
    }
    return NULL;
}

/*
 * Statement dispatcher.
 */
static ASTNode *parse_statement(ParserState *state) {
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
        advance_token(state);
        return NULL;
    }
    TokenType token_type = get_current_token_type(state);

    /* Check for declaration starters: modifiers or identifier that could begin a declaration. */
    if (token_type == TOKEN_ACCMOD || token_type == TOKEN_CONSTMOD ||
        token_type == TOKEN_MEMMOD || token_type == TOKEN_SIGNEDMOD ||
        token_type == TOKEN_ID) {
        ASTNode *decl = parse_object_declaration(state, false);
        if (decl) return decl;
        /* If parse_object_declaration returned NULL and did not consume tokens,
         * fall through to expression parsing. */
    }

    if (token_type == TOKEN_LCURLY) return parse_block(state);
    switch (token_type) {
        case TOKEN_IF:        return parse_if_statement(state);
        case TOKEN_DO:        return parse_do_loop(state);
        case TOKEN_BREAK:     return parse_break_statement(state);
        case TOKEN_CONTINUE:  return parse_continue_statement(state);
        case TOKEN_RETURN:    return parse_return_statement(state);
        case TOKEN_FREE:      return parse_free_statement(state);
        case TOKEN_JUMP:      return parse_jump_statement(state);
        case TOKEN_SIGNAL:    return parse_signal_statement(state);
        case TOKEN_INTERFLAG: return parse_interflag_statement(state);
        case TOKEN_NOP:       return parse_nop_statement(state);
        case TOKEN_HALT:      return parse_halt_statement(state);
        case TOKEN_KILL:      return parse_kill_statement(state);
        case TOKEN_AT:        return parse_label_declaration(state);
        case TOKEN_TRY:       return parse_try_statement(state);
        case TOKEN_ELSE:      return parse_else_statement(state);   /* Standalone else */
        default: break;
    }

    int saved_pos = state->current_token_position;
    ASTNode *expr = parse_expression(state);
    if (!expr) {
        state->current_token_position = saved_pos;
        Token *err_tok = get_current_token(state);
        if (err_tok) {
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                               "Invalid statement starting with '%s'", err_tok->value);
        } else {
            REPORT_UNEXPECTED_EOF(state);
        }
    }
    return expr;
}

/*
 * Sequence of statements until closing '}' or EOF.
 */
static ASTNode *parse_sequence(ParserState *state, bool expect_rcurly) {
    AST *stmt_list = safe_malloc(state, sizeof(AST));
    stmt_list->nodes = NULL;
    stmt_list->count = 0;
    stmt_list->capacity = 0;
    while (!(expect_rcurly && CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY)) &&
           !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
        ASTNode *stmt = parse_statement(state);
        if (stmt) {
            if (!add_ast_node_to_list(stmt_list, stmt)) {
                parser__ast_node_pool_free(state->pool, stmt);
                parser__free_ast(stmt_list);
                return NULL;
            }
            if (statement_requires_semicolon(stmt)) {
                parse_semicolon(state);
            } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
                advance_token(state);
            }
        } else {
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
                advance_token(state);
            }
        }
    }
    ASTNode *block_node = create_ast_node(state, AST_BLOCK, 0, NULL, NULL, NULL, (ASTNode*)stmt_list);
    if (!block_node) {
        parser__free_ast(stmt_list);
        return NULL;
    }
    return block_node;
}

/*
 * Main parsing entry point.
 */
AST *parse(Token *tokens, uint16_t token_count) {
    ParserState state;
    state.current_token_position = 0;
    state.token_stream = tokens;
    state.total_tokens = token_count;
    state.pool = parser__ast_node_pool_create(AST_POOL_INITIAL_CAPACITY);
    state.in_declaration_context = true;
    if (!state.pool) return NULL;

    AST *ast = safe_malloc(&state, sizeof(AST));
    ast->nodes = NULL;
    ast->count = 0;
    ast->capacity = 0;
    ast->pool = state.pool;

    while (get_current_token_type(&state) != TOKEN_EOF) {
        ASTNode *stmt = parse_statement(&state);
        if (stmt) {
            if (!add_ast_node_to_list(ast, stmt)) {
                parser__ast_node_pool_free(state.pool, stmt);
                parser__free_ast(ast);
                return NULL;
            }
            if (statement_requires_semicolon(stmt)) {
                parse_semicolon(&state);
            } else if (CURRENT_TOKEN_TYPE_MATCHES(&state, TOKEN_SEMICOLON)) {
                advance_token(&state);
            }
        } else {
            if (CURRENT_TOKEN_TYPE_MATCHES(&state, TOKEN_SEMICOLON)) {
                advance_token(&state);
            } else if (state.current_token_position < state.total_tokens) {
                state.current_token_position++;
            }
        }
    }
    return ast;
}

/*
 * Memory cleanup functions.
 */
void parser__free_type(Type *type) {
    if (!type) return;
    free(type->name);
    free(type->access_modifier);
    if (type->modifiers) {
        for (uint8_t i = 0; i < type->modifier_count; i++) free(type->modifiers[i]);
        free(type->modifiers);
    }
    if (type->array_dimensions) parser__free_ast(type->array_dimensions);
    if (type->compound_types) {
        for (uint8_t i = 0; i < type->compound_count; i++) parser__free_type(type->compound_types[i]);
        free(type->compound_types);
    }
    if (type->angle_expression) parser__free_ast_node(type->angle_expression);
    if (type->typeof_expression) parser__free_ast_node(type->typeof_expression);
    free(type);
}

void parser__free_ast_node(ASTNode *node) {
    if (!node) return;
    /* Recursively free children */
    if (node->left) parser__free_ast_node(node->left);
    if (node->right) parser__free_ast_node(node->right);
    /* Handle extra field based on node type */
    if (node->extra) {
        if (node->type == AST_BLOCK) {
            AST *block_ast = (AST*)node->extra;
            if (block_ast->nodes) {
                for (uint16_t i = 0; i < block_ast->count; i++) {
                    parser__free_ast_node(block_ast->nodes[i]);
                }
                free(block_ast->nodes);
            }
            free(block_ast);
        } else if (node->type == AST_MULTI_INITIALIZER) {
            AST *init_list = (AST*)node->extra;
            if (init_list->nodes) {
                for (uint16_t i = 0; i < init_list->count; i++) {
                    parser__free_ast_node(init_list->nodes[i]);
                }
                free(init_list->nodes);
            }
            free(init_list);
        } else if (node->type == AST_VARIABLE_LIST) {
            AST *decl_list = (AST*)node->extra;
            if (decl_list->nodes) {
                for (uint16_t i = 0; i < decl_list->count; i++) {
                    parser__free_ast_node(decl_list->nodes[i]);
                }
                free(decl_list->nodes);
            }
            free(decl_list);
        } else if (node->type == AST_TRY) {
            AST *catch_ast = (AST*)node->extra;
            if (catch_ast->nodes) {
                for (uint16_t i = 0; i < catch_ast->count; i++) {
                    parser__free_ast_node(catch_ast->nodes[i]);
                }
                free(catch_ast->nodes);
            }
            free(catch_ast);
        } else if (node->type == AST_CATCH) {
            free(node->value);
        } else {
            parser__free_ast_node(node->extra);
        }
    }
    /* Free other dynamically allocated fields */
    if (node->default_value) parser__free_ast_node(node->default_value);
    free(node->value);
    free(node->state_modifier);
    free(node->access_modifier);
    free(node->parent_struct);
    if (node->modifiers) {
        for (uint8_t i = 0; i < node->modifier_count; i++) free(node->modifiers[i]);
        free(node->modifiers);
    }
    if (node->variable_type) parser__free_type(node->variable_type);
}

void parser__free_ast(AST *ast) {
    if (!ast) return;
    for (uint16_t i = 0; i < ast->count; i++) {
        if (ast->nodes[i]) parser__free_ast_node(ast->nodes[i]);
    }
    free(ast->nodes);
    if (ast->pool) parser__ast_node_pool_destroy(ast->pool);
    free(ast);
}
