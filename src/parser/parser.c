#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "parser.h"

/*
 * Constants & macro definitions
 */

/* Maximum supported modifiers in type declarations */
#define MAX_MODIFIERS 8
#define AST_INITIAL_CAPACITY 8
#define AST_POOL_INITIAL_CAPACITY 256
#define MAX_COMPOUND_TYPES 8

/*
 * Forward declarations for parser internal functions
 */
static TokenType get_current_token_type(ParserState *state);
static void advance_token(ParserState *state);
static Token *get_current_token(ParserState *state);
static bool expect_token(ParserState *state, TokenType expected_type);
static void *safe_malloc(ParserState *state, size_t size);
static void *safe_realloc(ParserState *state, void *ptr, size_t size);
static char *safe_strdup(ParserState *state, const char *str);
static ASTNode *create_ast_node(ParserState *state, ASTNodeType node_type,
                               TokenType operation_type, char *node_value,
                               ASTNode *left_child, ASTNode *right_child,
                               ASTNode *extra_node);
static bool add_ast_node_to_list(AST *ast, ASTNode *node);
static AST *parse_universal_list(ParserState *state,
                                ASTNode *(*parse_element)(ParserState *),
                                bool (*is_element_start)(ParserState *),
                                TokenType separator,
                                TokenType end_token);
static ASTNode *parse_binary_operation_universal(ParserState *state,
                                                ASTNode *(*parse_operand)(ParserState *),
                                                TokenType *operators,
                                                uint8_t num_operators);
static bool parse_type_prefixes(ParserState *state,
                               uint8_t *pointer_level,
                               uint8_t *is_reference,
                               uint8_t *is_register);
static void apply_prefixes_to_type(Type *type,
                                  uint8_t pointer_level,
                                  uint8_t is_reference,
                                  uint8_t is_register);
static Type *parse_compound_type(ParserState *state, bool parse_prefixes);
static Type *parse_type_specifier_silent(ParserState *state,
                                        bool silent,
                                        bool parse_prefixes);
static Type *parse_type_specifier(ParserState *state, bool parse_prefixes);
static bool is_argument_start(ParserState *state);
static ASTNode *parse_push_statement(ParserState *state);
static ASTNode *parse_pop_statement(ParserState *state);
static ASTNode *parse_fixed_argument_function(ParserState *state,
                                             ASTNodeType node_type,
                                             uint8_t arg_count,
                                             const char *func_name);
static ASTNode *parse_alloc_expression(ParserState *state);
static ASTNode *parse_realloc_expression(ParserState *state);
static ASTNode *parse_postfix_expression(ParserState *state, ASTNode *node);
static ASTNode *parse_block_statement(ParserState *state);
static ASTNode *parse_multi_initializer(ParserState *state);
static ASTNode *parse_if_statement(ParserState *state);
static ASTNode *parse_signal(ParserState *state);
static ASTNode *parse_inter(ParserState *state);
static ASTNode *parse_label_declaration(ParserState *state);
static ASTNode *parse_jump_statement(ParserState *state);
static ASTNode *parse_return_statement(ParserState *state);
static ASTNode *parse_free_statement(ParserState *state);
static ASTNode *parse_nop_statement(ParserState *state);
static ASTNode *parse_halt_statement(ParserState *state);
static ASTNode *parse_parseof_statement(ParserState *state);
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
static bool is_valid_statement_expression(ASTNode *node);
static ASTNode *parse_object_declaration(ParserState *state, bool allow_expression);

/*
 * Error reporting macros with proper error codes
 */

#define REPORT_PARSE_ERROR_EX(state, level, error_code, context, ...) do { \
    Token *current = get_current_token(state); \
    if (current) { \
        int length = (int)strlen(current->value); \
        errhandler__report_error_ex( \
            level, \
            error_code, \
            current->line, \
            current->column, \
            length, \
            context, \
            __VA_ARGS__ \
        ); \
    } else { \
        errhandler__report_error_ex( \
            level, \
            error_code, \
            0, \
            0, \
            0, \
            context, \
            __VA_ARGS__ \
        ); \
    } \
    return NULL; \
} while(0)

#define REPORT_PARSE_ERROR(state, error_code, ...) \
    REPORT_PARSE_ERROR_EX(state, ERROR_LEVEL_ERROR, error_code, "syntax", __VA_ARGS__)

#define REPORT_PARSE_FATAL(state, error_code, ...) \
    REPORT_PARSE_ERROR_EX(state, ERROR_LEVEL_FATAL, error_code, "syntax", __VA_ARGS__)

#define REPORT_UNEXPECTED_TOKEN(state, expected, actual) do { \
    Token *current = get_current_token(state); \
    if (current) { \
        int length = (int)strlen(current->value); \
        errhandler__report_error_ex( \
            ERROR_LEVEL_ERROR, \
            ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN, \
            current->line, \
            current->column, \
            length, \
            "syntax", \
            "Expected %s but got %s (value: '%s')", \
            expected, \
            actual, \
            current->value \
        ); \
    } else { \
        errhandler__report_error_ex( \
            ERROR_LEVEL_ERROR, \
            ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN, \
            0, \
            0, \
            0, \
            "syntax", \
            "Expected %s but got EOF", \
            expected \
        ); \
    } \
    return NULL; \
} while(0)

#define REPORT_UNEXPECTED_EOF(state) \
    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_UNEXPECTED_EOF, "Unexpected end of file")

#define REPORT_INVALID_CHARACTER(state, ch) \
    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_INVALID_CHAR, "Invalid character: '%c'", ch)

#define REPORT_MEMORY_ALLOCATION_ERROR(state) \
    REPORT_PARSE_FATAL(state, ERROR_CODE_MEMORY_ALLOCATION, "Memory allocation failed")

#define REPORT_INVALID_STATEMENT(state, expr_node) do { \
    if (expr_node) { \
        parser__ast_node_pool_free(state->pool, expr_node); \
    } \
    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_INVALID_STATEMENT, \
                      "Invalid statement: expression with no effect"); \
} while(0)

/* Universal semicolon consumption with error checking */
#define EXPECT_SEMICOLON(state) do { \
    if (get_current_token_type(state) == TOKEN_SEMICOLON) { \
        advance_token(state); \
    } else { \
        Token *current = get_current_token(state); \
        if (current) { \
            errhandler__report_error( \
                ERROR_CODE_SYNTAX_MISSING_SEMICOLON, \
                current->line, \
                current->column, \
                "syntax", \
                "Expected ';'" \
            ); \
        } else { \
            errhandler__report_error( \
                ERROR_CODE_SYNTAX_MISSING_SEMICOLON, \
                0, \
                0, \
                "syntax", \
                "Expected ';' at end of file" \
            ); \
        } \
    } \
} while(0)

/* Parser utility macros for token matching */
#define CURRENT_TOKEN_TYPE_MATCHES(state, token) \
    (get_current_token_type(state) == (token))

#define CONSUME_TOKEN(state, token) do { \
    if (!expect_token(state, token)) return NULL; \
} while(0)

#define ATTEMPT_CONSUME_TOKEN(state, token) \
    (CURRENT_TOKEN_TYPE_MATCHES(state, token) ? (advance_token(state), 1) : 0)

#define IS_PREFIX_TOKEN(type) \
    ((type) == TOKEN_AT || (type) == TOKEN_DOUBLE_AT || \
     (type) == TOKEN_AMPERSAND || (type) == TOKEN_DOUBLE_AMPERSAND || \
     (type) == TOKEN_PERCENT)

#define IS_DOUBLE_PREFIX_TOKEN(type) \
    ((type) == TOKEN_DOUBLE_AT || (type) == TOKEN_DOUBLE_AMPERSAND)

/*
 * Token stream management functions
 */

static TokenType get_current_token_type(ParserState *state)
{
    return (state->current_token_position < state->total_tokens)
           ? state->token_stream[state->current_token_position].type
           : TOKEN_EOF;
}

static void advance_token(ParserState *state) {
    if (state->current_token_position < state->total_tokens - 1)
        state->current_token_position++;
}

static Token *get_current_token(ParserState *state) {
    return (state->current_token_position < state->total_tokens)
           ? &state->token_stream[state->current_token_position]
           : NULL;
}

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

static void *safe_malloc(ParserState *state, size_t size) {
    void *ptr = malloc(size);
    if (!ptr && size > 0) REPORT_MEMORY_ALLOCATION_ERROR(state);
    return ptr;
}

static void *safe_realloc(ParserState *state, void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0) REPORT_MEMORY_ALLOCATION_ERROR(state);
    return new_ptr;
}

static char *safe_strdup(ParserState *state, const char *str) {
    if (!str) return NULL;
    
    size_t len = strlen(str) + 1;
    char *dup = malloc(len);
    
    if (!dup) REPORT_MEMORY_ALLOCATION_ERROR(state);
    
    memcpy(dup, str, len);
    return dup;
}

static bool is_valid_statement_expression(ASTNode *node) {
    if (!node) return false;
    
    switch (node->type) {
        case AST_LITERAL_VALUE:
            if (node->operation_type == TOKEN_NUMBER ||
                node->operation_type == TOKEN_STRING ||
                node->operation_type == TOKEN_CHAR ||
                node->operation_type == TOKEN_NULL ||
                node->operation_type == TOKEN_NONE) {
                return false;
            }
            break;
            
        case AST_IDENTIFIER:
        case AST_REGISTER:
        case AST_FUNCTION_DECLARATION:
            return true;
            
        case AST_BINARY_OPERATION:
            if (node->operation_type == TOKEN_PLUS ||
                node->operation_type == TOKEN_MINUS ||
                node->operation_type == TOKEN_STAR ||
                node->operation_type == TOKEN_SLASH ||
                node->operation_type == TOKEN_PERCENT ||
                node->operation_type == TOKEN_PIPE ||
                node->operation_type == TOKEN_AMPERSAND ||
                node->operation_type == TOKEN_CARET) {
                return false;
            }
            break;
            
        case AST_UNARY_OPERATION:
            if (node->operation_type == TOKEN_PLUS ||
                node->operation_type == TOKEN_MINUS ||
                node->operation_type == TOKEN_TILDE ||
                node->operation_type == TOKEN_BANG) {
                return false;
            }
            break;
            
        case AST_MULTI_INITIALIZER:
            return false;
            
        default: break;
    }
    
    return true;
}

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
    
    for (uint16_t i = 0; i < initial_capacity; i++)
        pool->free_list[pool->free_top++] = i;
    
    return pool;
}

void parser__ast_node_pool_destroy(ASTNodePool *pool) {
    if (!pool) return;
    
    free(pool->nodes);
    free(pool->free_list);
    free(pool);
}

ASTNode *parser__ast_node_pool_alloc(ASTNodePool *pool) {
    if (!pool || pool->free_top == 0) return NULL;
    
    uint16_t index = pool->free_list[--pool->free_top];
    ASTNode *node = &pool->nodes[index];
    memset(node, 0, sizeof(ASTNode));
    
    return node;
}

void parser__ast_node_pool_free(ASTNodePool *pool, ASTNode *node) {
    if (!pool || !node) return;
    
    uint16_t index = (uint16_t)(node - pool->nodes);
    if (index < pool->capacity && pool->free_top < pool->capacity)
        pool->free_list[pool->free_top++] = index;
}

static ASTNode *create_ast_node
    ( ParserState *state, ASTNodeType node_type
    , TokenType operation_type, char *node_value
    , ASTNode *left_child, ASTNode *right_child
    , ASTNode *extra_node
) {
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
    
    return node;
}

bool add_ast_node_to_list(AST *ast, ASTNode *node) {
    if (ast->count >= ast->capacity) {
        uint16_t new_capacity = ast->capacity ? ast->capacity * 2 
                                              : AST_INITIAL_CAPACITY;
        ASTNode **new_nodes = realloc
        ( ast->nodes
        , new_capacity * sizeof(ASTNode*)
        );
        if (!new_nodes) return false;
        
        ast->nodes = new_nodes;
        ast->capacity = new_capacity;
    }
    
    ast->nodes[ast->count++] = node;
    return true;
}

static AST *parse_universal_list
    ( ParserState *state
    , ASTNode *(*parse_element)(ParserState *)
    , bool (*is_element_start)(ParserState *)
    , TokenType separator
    , TokenType end_token
) {
    AST *list = safe_malloc(state, sizeof(AST));
    list->nodes = NULL;
    list->count = 0;
    list->capacity = 0;
    
    if (CURRENT_TOKEN_TYPE_MATCHES(state, end_token)) {
        advance_token(state);
        return list;
    }
    
    while (!CURRENT_TOKEN_TYPE_MATCHES(state, end_token) &&
           !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
        if (is_element_start && !is_element_start(state)) {
            if (CURRENT_TOKEN_TYPE_MATCHES(state, separator)) {
                advance_token(state);
                if (CURRENT_TOKEN_TYPE_MATCHES(state, end_token))
                    break;
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
                "Expected '%s' or '%s'",
                token_names[separator],
                token_names[end_token]
            );
        }
    }
    
    CONSUME_TOKEN(state, end_token);
    return list;
}

static ASTNode *parse_binary_operation_universal
    ( ParserState *state
    , ASTNode *(*parse_operand)(ParserState *)
    , TokenType *operators
    , uint8_t num_operators
) {
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
        
        ASTNode *new_node = create_ast_node
            ( state
            , AST_BINARY_OPERATION
            , operation
            , NULL
            , node
            , right
            , NULL
        );
        if (!new_node) {
            parser__ast_node_pool_free(state->pool, right);
            parser__ast_node_pool_free(state->pool, node);
            return NULL;
        }
        
        node = new_node;
    }
    
    return node;
}

static bool parse_type_prefixes
    ( ParserState *state
    , uint8_t *pointer_level
    , uint8_t *is_reference
    , uint8_t *is_register
) {
    TokenType current = get_current_token_type(state);
    if (!IS_PREFIX_TOKEN(current)) return false;
    
    if (current == TOKEN_AT) {
        *pointer_level = 1;
        advance_token(state);
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_AT)) {
            *pointer_level = 2;
            advance_token(state);
        }
    } else if (current == TOKEN_AMPERSAND) {
        *is_reference = 1;
        advance_token(state);
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_AMPERSAND)) {
            *is_reference = 2;
            advance_token(state);
        }
    } else if (current == TOKEN_PERCENT) {
        *is_register = 1;
        advance_token(state);
    } else if (current == TOKEN_DOUBLE_AT) {
        *pointer_level = 2;
        advance_token(state);
    } else if (current == TOKEN_DOUBLE_AMPERSAND) {
        *is_reference = 2;
        advance_token(state);
    }
    
    return true;
}

static void apply_prefixes_to_type
    ( Type *type
    , uint8_t pointer_level
    , uint8_t is_reference
    , uint8_t is_register
) {
    if (!type) return;
    
    if (pointer_level > 0) {
        type->pointer_level = pointer_level;
        type->is_reference = 0;
        type->is_register = 0;
    } else if (is_reference > 0) {
        type->is_reference = is_reference;
        type->pointer_level = 0;
        type->is_register = 0;
    } else if (is_register > 0) {
        type->is_register = is_register;
        type->pointer_level = 0;
        type->is_reference = 0;
    }
}

static Type *parse_compound_type(ParserState *state, bool parse_prefixes) {
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN))
        return NULL;
    
    advance_token(state);
    
    Type *compound_type = safe_malloc(state, sizeof(Type));
    if (!compound_type) return NULL;
    
    memset(compound_type, 0, sizeof(Type));
    
    while (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN) &&
           !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
        if (compound_type->compound_count >= MAX_COMPOUND_TYPES) {
            parser__free_type(compound_type);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                "Too many types in compound type, maximum is %d",
                MAX_COMPOUND_TYPES
            );
        }
        
        Type *sub_type = parse_type_specifier_silent(state, false, parse_prefixes);
        if (!sub_type) {
            parser__free_type(compound_type);
            return NULL;
        }
        
        Type **new_compound_types = safe_realloc(
            state,
            compound_type->compound_types,
            (compound_type->compound_count + 1) * sizeof(Type*)
        );
        compound_type->compound_types = new_compound_types;
        compound_type->compound_types[compound_type->compound_count] = sub_type;
        compound_type->compound_count++;
        
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA))
            advance_token(state);
        else break;
    }
    
    CONSUME_TOKEN(state, TOKEN_RPAREN);
    
    if (compound_type->compound_count == 0) {
        parser__free_type(compound_type);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Empty compound type");
    }
    
    return compound_type;
}

static Type *parse_type_specifier_silent
    ( ParserState *state
    , bool silent
    , bool parse_prefixes
) {
    Type *type = safe_malloc(state, sizeof(Type));
    if (!type) return NULL;
    
    memset(type, 0, sizeof(Type));
    
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_NONE) ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_TYPE)) {
        Token *type_token = get_current_token(state);
        type->name = safe_strdup(state, type_token->value);
        advance_token(state);
        goto check_angle_brackets;
    }
    
    while (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_MODIFIER)
          && type->modifier_count < MAX_MODIFIERS) {
        Token *modifier_token = get_current_token(state);
        char **new_modifiers = safe_realloc(
            state,
            type->modifiers,
            (type->modifier_count + 1) * sizeof(char*)
        );
        type->modifiers = new_modifiers;
        type->modifiers[type->modifier_count] = safe_strdup(state, 
                                                            modifier_token->value
                                                            );
        type->modifier_count++;
        advance_token(state);
    }
    
    uint8_t pointer_level = 0;
    uint8_t is_reference = 0;
    uint8_t is_register = 0;
    
    if (parse_prefixes) {
        parse_type_prefixes(state, &pointer_level, &is_reference, &is_register);
        apply_prefixes_to_type(type, pointer_level, is_reference, is_register);
    }
    
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_NUMBER)) {
        Token *number_token = get_current_token(state);
        type->prefix_number = (uint8_t)atoi(number_token->value);
        advance_token(state);
    }

    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
        Type *compound_type = parse_compound_type(state, parse_prefixes);
        if (compound_type) {
            for (uint8_t i = 0; i < compound_type->compound_count; i++) {
                if (compound_type->compound_types[i]) {
                    if (type->modifiers && type->modifier_count > 0) {
                        compound_type->compound_types[i]->modifiers = type->modifiers;
                        compound_type->compound_types[i]->modifier_count = type->modifier_count;
                        type->modifiers = NULL;
                        type->modifier_count = 0;
                    }
                    if (type->prefix_number > 0)
                        compound_type->compound_types[i]->prefix_number = type->prefix_number;
                    if (pointer_level > 0 || is_reference > 0 || is_register > 0) {
                        apply_prefixes_to_type( compound_type->compound_types[i]
                                              , pointer_level
                                              , is_reference
                                              , is_register
                        );
                    }
                }
            }
            free(type);
            type = compound_type;
            goto check_angle_brackets;
        }
    }

    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_TYPE) ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
        Token *type_token = get_current_token(state);
        type->name = safe_strdup(state, type_token->value);
        advance_token(state);
    } else {
        if (!silent) {
            REPORT_UNEXPECTED_TOKEN(
                state,
                "type specifier or identifier",
                token_names[get_current_token_type(state)]
            );
        }
        parser__free_type(type);
        return NULL;
    }
    
check_angle_brackets:
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LT)) {
        advance_token(state);
        
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_GT)) {
            parser__free_type(type);
            if (!silent) {
                REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Empty angle brackets in type");
            }
            return NULL;
        }
        
        /* For type size (e.g., Int<1>), parse a numeric literal */
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_NUMBER)) {
            Token *number_token = get_current_token(state);
            long size_value = atol(number_token->value);
            
            if (size_value <= 0 || size_value > UINT8_MAX) {
                parser__free_type(type);
                if (!silent) {
                    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                        "Invalid type size: must be between 1 and %d", 
                        UINT8_MAX);
                }
                return NULL;
            }
            
            type->size_in_bytes = (uint8_t)size_value;
            advance_token(state);
            
            if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_GT)) {
                parser__free_type(type);
                if (!silent) {
                    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected '>' after type size");
                }
                return NULL;
            }
            advance_token(state);
        } else {
            /* For generic types, parse as expression */
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
                    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_GT)) {
                        break;
                    }
                    
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
                
                ASTNode *multi_init = create_ast_node(state, 
                    AST_MULTI_INITIALIZER, 0, NULL, NULL, NULL, 
                    (ASTNode*)angle_list);
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

static Type *parse_type_specifier(ParserState *state, bool parse_prefixes) {
    return parse_type_specifier_silent(state, false, parse_prefixes);
}

static ASTNode *parse_expression(ParserState *state) {
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_STATE)) {
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_INVALID_STATEMENT,
            "State modifier cannot be used in expression context");
    }
    return parse_assignment_expression(state);
}

static ASTNode *parse_assignment_expression(ParserState *state) {
    ASTNode *left = parse_ternary_expression(state);
    if (!left) return NULL;
    
    if (left->type == AST_MULTI_INITIALIZER) {
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EQUAL))
            return left;
        advance_token(state);
        
        ASTNode *right = parse_expression(state);
        if (!right) {
            parser__ast_node_pool_free(state->pool, left);
            return NULL;
        }
        
        return create_ast_node
            ( state
            , AST_MULTI_ASSIGNMENT
            , 0
            , NULL
            , left
            , right
            , NULL
        );
    }
    
    static const TokenType assignment_ops[] = {
        TOKEN_EQUAL, TOKEN_PLUS_EQ, TOKEN_MINUS_EQ, TOKEN_STAR_EQ,
        TOKEN_SLASH_EQ, TOKEN_PERCENT_EQ, TOKEN_PIPE_EQ, TOKEN_AMPERSAND_EQ,
        TOKEN_CARET_EQ, TOKEN_SHL_EQ, TOKEN_SHR_EQ, TOKEN_SAL_EQ,
        TOKEN_SAR_EQ, TOKEN_ROL_EQ, TOKEN_ROR_EQ
    };
    
    TokenType current = get_current_token_type(state);
    for (uint8_t i = 0; i < sizeof(assignment_ops)/sizeof(assignment_ops[0]); i++) {
        if (current == assignment_ops[i]) {
            advance_token(state);
            ASTNode *right = parse_assignment_expression(state);
            if (!right) {
                parser__ast_node_pool_free(state->pool, left);
                return NULL;
            }
            
            ASTNodeType node_type = (current == TOKEN_EQUAL) 
                                   ? AST_ASSIGNMENT 
                                   : AST_COMPOUND_ASSIGNMENT;
            return create_ast_node(state, node_type, current, NULL, 
                                  left, right, NULL);
        }
    }
    
    return left;
}

static ASTNode *parse_ternary_expression(ParserState *state) {
    ASTNode *condition = parse_logical_expression(state);
    if (!condition) return NULL;
    
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_QUESTION)) {
        advance_token(state);
        ASTNode *true_expr = parse_expression(state);
        if (!true_expr) {
            parser__ast_node_pool_free(state->pool, condition);
            return NULL;
        }
        
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
            parser__ast_node_pool_free(state->pool, condition);
            parser__ast_node_pool_free(state->pool, true_expr);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected ':' in ternary operator");
        }
        advance_token(state);
        
        ASTNode *false_expr = parse_ternary_expression(state);
        if (!false_expr) {
            parser__ast_node_pool_free(state->pool, condition);
            parser__ast_node_pool_free(state->pool, true_expr);
            return NULL;
        }
        
        return create_ast_node
            ( state, AST_TERNARY_OPERATION
            , 0
            , NULL
            , condition
            , true_expr
            , false_expr
        );
    }
    
    return condition;
}

static ASTNode *parse_logical_expression(ParserState *state) {
    TokenType operators[] = {TOKEN_LOGICAL, TOKEN_ERROR};
    return parse_binary_operation_universal
        ( state
        , parse_bitwise_or_expression
        , operators
        , 2
    );
}

static ASTNode *parse_bitwise_or_expression(ParserState *state) {
    TokenType operators[] = {TOKEN_PIPE, TOKEN_ERROR};
    return parse_binary_operation_universal
        ( state
        , parse_bitwise_xor_expression
        , operators
        , 2
    );
}

static ASTNode *parse_bitwise_xor_expression(ParserState *state) {
    TokenType operators[] = {TOKEN_CARET, TOKEN_NE_TILDE};
    return parse_binary_operation_universal
        ( state
        , parse_bitwise_and_expression
        , operators
        , 2
    );
}

static ASTNode *parse_bitwise_and_expression(ParserState *state) {
    TokenType operators[] = {TOKEN_AMPERSAND, TOKEN_ERROR};
    return parse_binary_operation_universal
        ( state
        , parse_equality_expression
        , operators
        , 2
    );
}

static ASTNode *parse_equality_expression(ParserState *state) {
    TokenType operators[] = {TOKEN_DOUBLE_EQ, TOKEN_NE};
    return parse_binary_operation_universal
        ( state
        , parse_relational_expression
        , operators
        , 2
    );
}

static ASTNode *parse_relational_expression(ParserState *state)
{
    TokenType operators[] = {TOKEN_LT, TOKEN_GT, TOKEN_LE, TOKEN_GE};
    return parse_binary_operation_universal
        ( state
        , parse_shift_expression
        , operators
        , 4
    );
}

static ASTNode *parse_shift_expression(ParserState *state) {
    TokenType operators[] = {TOKEN_SHL, TOKEN_SHR, TOKEN_SAL, TOKEN_SAR, 
                            TOKEN_ROL, TOKEN_ROR};
    return parse_binary_operation_universal
        ( state
        , parse_additive_expression
        , operators
        , 6
    );
}

static ASTNode *parse_additive_expression(ParserState *state) {
    TokenType operators[] = {TOKEN_PLUS, TOKEN_MINUS};
    return parse_binary_operation_universal
        ( state
        , parse_multiplicative_expression
        , operators
        , 2
    );
}

static ASTNode *parse_multiplicative_expression(ParserState *state) {
    TokenType operators[] = {TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT};
    return parse_binary_operation_universal
        ( state
        , parse_unary_expression
        , operators
        , 3
    );
}

static ASTNode *parse_unary_expression(ParserState *state) {
    uint8_t pointer_level = 0;
    uint8_t is_reference = 0;
    uint8_t is_register = 0;
    
    parse_type_prefixes(state, &pointer_level, &is_reference, &is_register);
    
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
        int saved_pos = state->current_token_position;
        advance_token(state);
        
        Type *cast_type = parse_type_specifier_silent(state, true, true);
        
        if (cast_type != NULL && CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
            advance_token(state);
            
            ASTNode *expression = parse_unary_expression(state);
            if (!expression) {
                parser__free_type(cast_type);
                return NULL;
            }
            
            ASTNode *node = create_ast_node
                ( state
                , AST_CAST
                , 0
                , NULL 
                , expression
                , NULL
                , NULL
            );
            node->variable_type = cast_type;
            return node;
        }
        
        if (cast_type)
            parser__free_type(cast_type);
        state->current_token_position = saved_pos;
    }
    
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOUBLE_PLUS) ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOUBLE_MINUS)) {
        TokenType operation = get_current_token_type(state);
        advance_token(state);
        ASTNode *operand = parse_unary_expression(state);
        if (!operand) return NULL;
        
        ASTNodeType node_type = (operation == TOKEN_DOUBLE_PLUS)
                               ? AST_PREFIX_INCREMENT
                               : AST_PREFIX_DECREMENT;
        ASTNode *node = create_ast_node
            ( state
            , node_type
            , operation
            , NULL
            , NULL
            , operand
            , NULL
        );
        
        if (pointer_level > 0 || is_reference || is_register) {
            Type *temp_type = safe_malloc(state, sizeof(Type));
            temp_type->name = safe_strdup(state, "int");
            temp_type->pointer_level = pointer_level;
            temp_type->is_reference = is_reference;
            temp_type->is_register = is_register;
            node->variable_type = temp_type;
        }
        return node;
    }

    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_BANG) ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_TILDE) ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_STAR) ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SLASH)) {
        TokenType operation = get_current_token_type(state);
        advance_token(state);
        ASTNode *operand = parse_unary_expression(state);
        if (!operand) return NULL;
        
        ASTNode *node = create_ast_node
            ( state
            , AST_UNARY_OPERATION   
            , operation
            , NULL
            , NULL
            , operand
            , NULL
        );
        
        if (pointer_level > 0 || is_reference || is_register) {
            Type *temp_type = safe_malloc(state, sizeof(Type));
            temp_type->name = safe_strdup(state, "int");
            temp_type->pointer_level = pointer_level;
            temp_type->is_reference = is_reference;
            temp_type->is_register = is_register;
            node->variable_type = temp_type;
        }
        return node;
    }
    
    ASTNode *primary = parse_primary_expression(state);
    if (!primary) {
        return NULL;
    }
    
    if (pointer_level > 0 || is_reference || is_register) {
        Type *temp_type = safe_malloc(state, sizeof(Type));
        temp_type->name = safe_strdup(state, "int");
        temp_type->pointer_level = pointer_level;
        temp_type->is_reference = is_reference;
        temp_type->is_register = is_register;
        primary->variable_type = temp_type;
    }
    
    return parse_postfix_expression(state, primary);
}

static ASTNode *parse_primary_expression(ParserState *state) {
    Token *token = get_current_token(state);
    if (!token) REPORT_UNEXPECTED_EOF(state);
    
    if (IS_PREFIX_TOKEN(token->type) || IS_DOUBLE_PREFIX_TOKEN(token->type)) {
        int saved_pos = state->current_token_position;
        uint8_t pointer_level = 0;
        uint8_t is_reference = 0;
        uint8_t is_register = 0;
        
        while (IS_PREFIX_TOKEN(get_current_token_type(state)) ||
               IS_DOUBLE_PREFIX_TOKEN(get_current_token_type(state))) {
            TokenType prefix = get_current_token_type(state);
            
            if (prefix == TOKEN_AT)
                pointer_level = 1;
            else if (prefix == TOKEN_DOUBLE_AT)
                pointer_level = 2;
            else if (prefix == TOKEN_AMPERSAND)
                is_reference = 1;
            else if (prefix == TOKEN_DOUBLE_AMPERSAND)
                is_reference = 2;
            else if (prefix == TOKEN_PERCENT)
                is_register = 1;
            
            advance_token(state);
        }
        
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
            token = get_current_token(state);
            char *value = safe_strdup(state, token->value);
            advance_token(state);
            
            ASTNode *node = create_ast_node(state, AST_IDENTIFIER, 0, value, 
                                           NULL, NULL, NULL);
            
            if (pointer_level > 0 || is_reference > 0 || is_register > 0) {
                Type *type = safe_malloc(state, sizeof(Type));
                memset(type, 0, sizeof(Type));
                type->name = safe_strdup(state, "auto");
                type->pointer_level = pointer_level;
                type->is_reference = is_reference;
                type->is_register = is_register;
                node->variable_type = type;
            }
            
            return parse_postfix_expression(state, node);
        } else
            state->current_token_position = saved_pos;
    }
    
    switch (token->type) {
        case TOKEN_DOT:
            advance_token(state);
            if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID))
                REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected label name after '.'");
            token = get_current_token(state);
            char *value = safe_strdup(state, token->value);
            advance_token(state);
            return create_ast_node(state, AST_LABEL_VALUE, 0, value, 
                                  NULL, NULL, NULL);
        
        case TOKEN_RETURN:
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_INVALID_STATEMENT,
                "return can only be used as a statement, not an expression");
            break;
        
        case TOKEN_PERCENT:
            advance_token(state);
            if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID))
                REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected register name after '%'");
            token = get_current_token(state);
            value = safe_strdup(state, token->value);
            advance_token(state);
            return create_ast_node(state, AST_REGISTER, 0, value, 
                                  NULL, NULL, NULL);
        
        case TOKEN_LPAREN:
            advance_token(state);
            int saved_pos = state->current_token_position;
            
            Type *cast_type = parse_type_specifier_silent(state, true, true);
            if (cast_type != NULL && CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
                advance_token(state);
                ASTNode *expression = parse_unary_expression(state);
                if (!expression) {
                    parser__free_type(cast_type);
                    return NULL;
                }
                ASTNode *node = create_ast_node(state, AST_CAST, 0, NULL, 
                                               expression, NULL, NULL);
                node->variable_type = cast_type;
                return node;
            } else {
                if (cast_type)
                    parser__free_type(cast_type);
                state->current_token_position = saved_pos;
                ASTNode *expression = parse_expression(state);
                if (!expression) return NULL;
                CONSUME_TOKEN(state, TOKEN_RPAREN);
                return expression;
            }
        
        case TOKEN_LCURLY:
            return parse_multi_initializer(state);
        
        case TOKEN_SIZEOF:
            ASTNodeType node_type = (token->type == TOKEN_SIZEOF) 
                                   ? AST_SIZEOF 
                                   : AST_TYPEOF;
            advance_token(state);
            CONSUME_TOKEN(state, TOKEN_LPAREN);
            ASTNode *arguments = parse_expression(state);
            if (!arguments) return NULL;
            CONSUME_TOKEN(state, TOKEN_RPAREN);
            return create_ast_node(state, node_type, 0, NULL, arguments, NULL, NULL);
        
        case TOKEN_POP:
            return parse_pop_statement(state);
        
        case TOKEN_ALLOC:
            return parse_alloc_expression(state);
        
        case TOKEN_REALLOC:
            return parse_realloc_expression(state);
        
        case TOKEN_NUMBER:
        case TOKEN_STRING:
        case TOKEN_CHAR:
        case TOKEN_NULL:
        case TOKEN_NONE:
        case TOKEN_TYPE: {
            char *value = safe_strdup(state, token->value);
            advance_token(state);
            return create_ast_node
                ( state
                , AST_LITERAL_VALUE
                , token->type
                , value
                , NULL
                , NULL
                , NULL
            );
            break;
        }
        
        case TOKEN_ID: {
            char *value = safe_strdup(state, token->value);
            advance_token(state);
            
            ASTNode *node = create_ast_node
                ( state
                , AST_IDENTIFIER
                , 0
                , value
                , NULL
                , NULL
                , NULL
            );
            return parse_postfix_expression(state, node);
            break;
        }
        
        case TOKEN_ERROR:
            REPORT_INVALID_CHARACTER(state, token->value[0]);
            break;
        
        default: 
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Invalid syntax in expression");
    }
    
    return NULL;
}

static ASTNode *parse_postfix_expression(ParserState *state, ASTNode *node) {
    while (1) {
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOUBLE_PLUS)) {
            advance_token(state);
            node = create_ast_node
                ( state
                , AST_POSTFIX_INCREMENT
                , TOKEN_DOUBLE_PLUS
                , NULL
                , node
                , NULL
                , NULL
            );
        } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_DOUBLE_MINUS)) {
            advance_token(state);
            node = create_ast_node
                ( state
                , AST_POSTFIX_DECREMENT
                , TOKEN_DOUBLE_MINUS
                , NULL
                , node
                , NULL
                , NULL
            );
        } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
            advance_token(state);
            
            AST *arguments = safe_malloc(state, sizeof(AST));
            arguments->nodes = NULL;
            arguments->count = 0;
            arguments->capacity = 0;
            
            if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
                ASTNode *arg_expr = parse_expression(state);
                if (!arg_expr) {
                    parser__ast_node_pool_free(state->pool, node);
                    parser__free_ast(arguments);
                    return NULL;
                }
                
                if (!add_ast_node_to_list(arguments, arg_expr)) {
                    parser__ast_node_pool_free(state->pool, arg_expr);
                    parser__ast_node_pool_free(state->pool, node);
                    parser__free_ast(arguments);
                    return NULL;
                }
                
                while (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
                    advance_token(state);
                    arg_expr = parse_expression(state);
                    if (!arg_expr) {
                        parser__ast_node_pool_free(state->pool, node);
                        parser__free_ast(arguments);
                        return NULL;
                    }
                    
                    if (!add_ast_node_to_list(arguments, arg_expr)) {
                        parser__ast_node_pool_free(state->pool, arg_expr);
                        parser__ast_node_pool_free(state->pool, node);
                        parser__free_ast(arguments);
                        return NULL;
                    }
                }
            }
            
            CONSUME_TOKEN(state, TOKEN_RPAREN);
            
            ASTNode *func_call = create_ast_node
                ( state
                , AST_FUNCTION_DECLARATION
                , 0
                , NULL
                , node
                , NULL
                , NULL
            );
            if (!func_call) {
                parser__free_ast(arguments);
                parser__ast_node_pool_free(state->pool, node);
                return NULL;
            }
            func_call->extra = (ASTNode*)arguments;
            node = func_call;
        } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LBRACE)) {
            advance_token(state);
            
            ASTNode *index_expr = parse_expression(state);
            if (!index_expr) {
                parser__ast_node_pool_free(state->pool, node);
                return NULL;
            }
            
            CONSUME_TOKEN(state, TOKEN_RBRACE);
            
            ASTNode *array_access = create_ast_node
                ( state
                , AST_ARRAY_ACCESS
                , 0
                , NULL
                , node
                , index_expr
                , NULL
            );
            node = array_access;
        } else if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_INDICATOR)) {
            advance_token(state);
            
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
                advance_token(state);
                Type *target_type = parse_type_specifier(state, true);
                if (!target_type) {
                    parser__ast_node_pool_free(state->pool, node);
                    return NULL;
                }
                CONSUME_TOKEN(state, TOKEN_RPAREN);
                node = create_ast_node(state, AST_POSTFIX_CAST, 0, NULL, 
                                      node, NULL, NULL);
                node->variable_type = target_type;
            } else {
                if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
                    parser__ast_node_pool_free(state->pool, node);
                    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected field name after '->'");
                }
                
                Token *field_token = get_current_token(state);
                char *field_name = safe_strdup(state, field_token->value);
                advance_token(state);
                
                ASTNode *field_node = create_ast_node
                    ( state
                    , AST_IDENTIFIER
                    , 0
                    , field_name
                    , NULL
                    , NULL
                    , NULL
                );
                node = create_ast_node
                    ( state
                    , AST_FIELD_ACCESS
                    , 0
                    , NULL
                    , node
                    , field_node
                    , NULL
                );
            }
        } else break;
    }
    return node;
}

static bool is_argument_start(ParserState *state) {
    TokenType type = get_current_token_type(state);
    
    if (IS_PREFIX_TOKEN(type) || IS_DOUBLE_PREFIX_TOKEN(type))
        return true;
    
    switch (type) {
        case TOKEN_LPAREN:
        case TOKEN_LCURLY:
        case TOKEN_SIZEOF:
        case TOKEN_POP:
        case TOKEN_ALLOC:
        case TOKEN_REALLOC:
        case TOKEN_NUMBER:
        case TOKEN_STRING:
        case TOKEN_CHAR:
        case TOKEN_NULL:
        case TOKEN_NONE:
        case TOKEN_TYPE:
        case TOKEN_ERROR:
        case TOKEN_DOT:
        case TOKEN_PERCENT:
        case TOKEN_BANG:
        case TOKEN_TILDE:
        case TOKEN_STAR:
        case TOKEN_SLASH:
        case TOKEN_AT:
        case TOKEN_DOUBLE_AT:
        case TOKEN_AMPERSAND:
        case TOKEN_DOUBLE_AMPERSAND:
        case TOKEN_DOUBLE_PLUS:
        case TOKEN_DOUBLE_MINUS:
        case TOKEN_ID:
            return true;
        default:
            return false;
    }
}

static ASTNode *parse_push_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_PUSH);
    
    ASTNode *expr = NULL;
    if (is_argument_start(state)) {
        expr = parse_expression(state);
        if (!expr) return NULL;
    }
    
    EXPECT_SEMICOLON(state);
    return create_ast_node(state, AST_PUSH, 0, NULL, expr, NULL, NULL);
}

static ASTNode *parse_pop_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_POP);
    
    ASTNode *expr = NULL;
    if (is_argument_start(state)) {
        expr = parse_expression(state);
        if (!expr) return NULL;
    }
    
    return create_ast_node(state, AST_POP, 0, NULL, expr, NULL, NULL);
}

static ASTNode *parse_fixed_argument_function
    ( ParserState *state
    , ASTNodeType node_type
    , uint8_t arg_count
    , const char *func_name
) {
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
                    "Expected comma after argument %d in %s()",
                    i + 1,
                    func_name
                );
            }
            advance_token(state);
        }
    }
    
    CONSUME_TOKEN(state, TOKEN_RPAREN);
    
    if (arguments->count != arg_count) {
        parser__free_ast(arguments);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
            "%s() requires exactly %d arguments",
            func_name,
            arg_count
        );
    }
    
    ASTNode *arguments_block = create_ast_node
        ( state
        , AST_BLOCK
        , 0
        , NULL
        , NULL
        , NULL
        , (ASTNode*)arguments
    );
    if (!arguments_block) {
        parser__free_ast(arguments);
        return NULL;
    }
    
    ASTNode *func_node = create_ast_node
        ( state
        , node_type
        , 0
        , NULL
        , arguments_block
        , NULL
        , NULL
    );
    if (!func_node) {
        parser__free_ast_node(arguments_block);
        return NULL;
    }
    
    return func_node;
}

static ASTNode *parse_alloc_expression(ParserState *state) {
    return parse_fixed_argument_function(state, AST_ALLOC, 3, "alloc");
}

static ASTNode *parse_realloc_expression(ParserState *state) {
    return parse_fixed_argument_function(state, AST_REALLOC, 2, "realloc");
}

static ASTNode *parse_block_statement(ParserState *state) {
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_THEN)) {
        advance_token(state);
        ASTNode *statement = parse_statement(state);
        if (!statement) return NULL;
        return create_ast_node(state, AST_BLOCK, 0, NULL, statement, NULL, NULL);
    }
    
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LCURLY)) {
        CONSUME_TOKEN(state, TOKEN_LCURLY);
        ASTNode *block_node = create_ast_node
            ( state
            , AST_BLOCK
            , 0
            , NULL
            , NULL
            , NULL
            , NULL
        );
        if (!block_node) return NULL;
        
        AST *block_ast = safe_malloc(state, sizeof(AST));
        block_ast->nodes = NULL;
        block_ast->count = 0;
        block_ast->capacity = 0;
        
        while (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY) &&
               !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
            ASTNode *statement = parse_statement(state);
            if (statement && !add_ast_node_to_list(block_ast, statement)) {
                parser__ast_node_pool_free(state->pool, statement);
                parser__free_ast(block_ast);
                parser__ast_node_pool_free(state->pool, block_node);
                return NULL;
            }
        }
        CONSUME_TOKEN(state, TOKEN_RCURLY);
        
        block_node->extra = (ASTNode*)block_ast;
        return block_node;
    }
    
    REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected '=>' or '{' for block statement");
}

static ASTNode *parse_multi_initializer(ParserState *state) {
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LCURLY))
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected '{' for multi-value initializer");
    advance_token(state);
    
    AST *list = safe_malloc(state, sizeof(AST));
    list->nodes = NULL;
    list->count = 0;
    list->capacity = 0;

    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY)) {
        advance_token(state);
        return create_ast_node
            ( state
            , AST_MULTI_INITIALIZER
            , 0
            , NULL
            , NULL
            , NULL
            , (ASTNode*)list
        );
    }

    while (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY) &&
           !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
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
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY))
                break;
        } else {
            if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY)) {
                parser__free_ast(list);
                REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected ',' or '}' in multi-initializer");
            }
            break;
        }
    }

    CONSUME_TOKEN(state, TOKEN_RCURLY);
    return create_ast_node
        ( state
        , AST_MULTI_INITIALIZER
        , 0
        , NULL
        , NULL
        , NULL
        , (ASTNode*)list
    );
}

static ASTNode *parse_if_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_IF);
    CONSUME_TOKEN(state, TOKEN_LPAREN);
    ASTNode *condition = parse_expression(state);
    if (!condition) return NULL;
    CONSUME_TOKEN(state, TOKEN_RPAREN);
    
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_THEN)) {
        advance_token(state);
        ASTNode *if_block = parse_statement(state);
        if (!if_block) {
            parser__ast_node_pool_free(state->pool, condition);
            return NULL;
        }
        
        ASTNode *else_block = NULL;
        if (ATTEMPT_CONSUME_TOKEN(state, TOKEN_ELSE)) {
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_THEN)) {
                advance_token(state);
                else_block = parse_statement(state);
            } else else_block = parse_block_statement(state);
        }
        
        return create_ast_node
            ( state
            , AST_IF_STATEMENT  
            , 0
            , NULL
            , condition
            , if_block
            , else_block
        );
    }
    
    ASTNode *if_block = parse_block_statement(state);
    if (!if_block) {
        parser__ast_node_pool_free(state->pool, condition);
        return NULL;
    }
    
    ASTNode *else_block = NULL;
    if (ATTEMPT_CONSUME_TOKEN(state, TOKEN_ELSE)) {
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_THEN)) {
            advance_token(state);
            else_block = parse_statement(state);
        } else else_block = parse_block_statement(state);
    }
    
    return create_ast_node
        ( state
        , AST_IF_STATEMENT
        , 0
        , NULL
        , condition
        , if_block
        , else_block
    );
}

static ASTNode *parse_signal(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_SIGNAL);
    CONSUME_TOKEN(state, TOKEN_LPAREN);
    
    AST *arguments = parse_universal_list
        ( state
        , parse_expression
        , NULL
        , TOKEN_COMMA
        , TOKEN_RPAREN
    );
    if (!arguments) return NULL;
    
    if (arguments->count == 0) {
        parser__free_ast(arguments);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "signal requires at least one argument");
    }
    
    EXPECT_SEMICOLON(state);
    return create_ast_node
        ( state
        , AST_SIGNAL
        , 0
        , NULL
        , (ASTNode*)arguments
        , NULL
        , NULL
    );
}

static ASTNode *parse_inter(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_INTER);
    
    AST *arguments = NULL;
    
    if (ATTEMPT_CONSUME_TOKEN(state, TOKEN_LPAREN)) {
        arguments = parse_universal_list
            ( state
            , parse_expression
            , NULL
            , TOKEN_COMMA
            , TOKEN_RPAREN
        );
        if (!arguments) return NULL;
    } else {
        arguments = safe_malloc(state, sizeof(AST));
        if (!arguments) return NULL;
        arguments->nodes = NULL;
        arguments->count = 0;
        arguments->capacity = 0;
    }
    
    CONSUME_TOKEN(state, TOKEN_COLON);
    ASTNode *inter_expression = parse_expression(state);
    if (!inter_expression) {
        parser__free_ast(arguments);
        return NULL;
    }
    
    EXPECT_SEMICOLON(state);
    return create_ast_node
        ( state
        , AST_INTER
        , 0
        , NULL
        , (ASTNode*)arguments
        , inter_expression
        , NULL
    );
}

static ASTNode *parse_label_declaration(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_DOT);
    
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID))
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected label name after '.'");
    
    Token *token = get_current_token(state);
    char *label_name = safe_strdup(state, token->value);
    advance_token(state);
    
    CONSUME_TOKEN(state, TOKEN_COLON);
    return create_ast_node
        ( state
        , AST_LABEL_DECLARATION
        , 0
        , label_name
        , NULL
        , NULL
        , NULL
    );
}

static ASTNode *parse_jump_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_JUMP);
    ASTNode *target = parse_expression(state);
    if (!target) return NULL;
    
    EXPECT_SEMICOLON(state);
    return create_ast_node(state, AST_JUMP, 0, NULL, target, NULL, NULL);
}

static ASTNode *parse_return_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_RETURN);
    
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
        EXPECT_SEMICOLON(state);
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
    
    EXPECT_SEMICOLON(state);
    
    if (return_list->count == 0) {
        parser__free_ast(return_list);
        return create_ast_node(state, AST_RETURN, 0, NULL, NULL, NULL, NULL);
    } else if (return_list->count == 1) {
        ASTNode *expr = return_list->nodes[0];
        free(return_list->nodes);
        free(return_list);
        return create_ast_node(state, AST_RETURN, 0, NULL, expr, NULL, NULL);
    } else {
        ASTNode *multi_return = create_ast_node
            ( state
            , AST_MULTI_INITIALIZER
            , 0
            , NULL
            , NULL
            , NULL
            , (ASTNode*)return_list
        );
        if (!multi_return) {
            parser__free_ast(return_list);
            return NULL;
        }
        return create_ast_node(state, AST_RETURN, 0, NULL, multi_return, NULL, NULL);
    }
}

static ASTNode *parse_free_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_FREE);
    
    ASTNode *expression = NULL;
    
    /* Check if there are parentheses */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
        advance_token(state);
        expression = parse_expression(state);
        if (!expression) return NULL;
        CONSUME_TOKEN(state, TOKEN_RPAREN);
    } else {
        /* No parentheses - parse expression directly */
        expression = parse_expression(state);
        if (!expression) return NULL;
    }
    
    EXPECT_SEMICOLON(state);
    return create_ast_node(state, AST_FREE, 0, NULL, expression, NULL, NULL);
}

static ASTNode *parse_nop_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_NOP);
    EXPECT_SEMICOLON(state);
    return create_ast_node(state, AST_NOP, 0, NULL, NULL, NULL, NULL);
}

static ASTNode *parse_halt_statement(ParserState *state) {
    CONSUME_TOKEN(state, TOKEN_HALT);
    EXPECT_SEMICOLON(state);
    return create_ast_node(state, AST_HALT, 0, NULL, NULL, NULL, NULL);
}

static ASTNode *parse_parseof_statement(ParserState *state) {
    advance_token(state);
    ASTNode *expr = parse_expression(state);
    if (!expr) return NULL;
    EXPECT_SEMICOLON(state);
    return create_ast_node(state, AST_PARSEOF, 0, NULL, expr, NULL, NULL);
}

static ASTNode *parse_object_declaration(ParserState *state, bool allow_expression) {
    int saved_pos = state->current_token_position;
    
    /* Check state modifier */
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_STATE)) {
        if (allow_expression) {
            ASTNode *expr = parse_expression(state);
            if (expr) return expr;
        }
        state->current_token_position = saved_pos;
        return NULL;
    }
    
    Token *state_token = get_current_token(state);
    char *state_modifier = safe_strdup(state, state_token->value);
    advance_token(state);
    
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
        free(state_modifier);
        if (!allow_expression)
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected identifier after state modifier");
        state->current_token_position = saved_pos;
        return NULL;
    }
    
    Token *name_token = get_current_token(state);
    char *name = safe_strdup(state, name_token->value);
    advance_token(state);
    
    bool is_function = false;
    AST *parameter_list = NULL;
    AST *dimension_list = NULL;
    
    /* Parse array dimensions if present */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LBRACE)) {
        advance_token(state);
        dimension_list = safe_malloc(state, sizeof(AST));
        dimension_list->nodes = NULL;
        dimension_list->count = 0;
        dimension_list->capacity = 0;
        
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RBRACE)) {
            while (1) {
                ASTNode *dim_expr = parse_expression(state);
                if (!dim_expr) {
                    free(name);
                    free(state_modifier);
                    parser__free_ast(dimension_list);
                    state->current_token_position = saved_pos;
                    return allow_expression ? parse_expression(state) : NULL;
                }
                
                if (!add_ast_node_to_list(dimension_list, dim_expr)) {
                    parser__ast_node_pool_free(state->pool, dim_expr);
                    free(name);
                    free(state_modifier);
                    parser__free_ast(dimension_list);
                    state->current_token_position = saved_pos;
                    return allow_expression ? parse_expression(state) : NULL;
                }
                
                if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA)) {
                    advance_token(state);
                    continue;
                }
                break;
            }
        }
        
        CONSUME_TOKEN(state, TOKEN_RBRACE);
    }
    
    /* Check if it's a function */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
        is_function = true;
        parameter_list = parse_parameter_list(state);
        if (!parameter_list) {
            free(name);
            free(state_modifier);
            if (dimension_list)
                parser__free_ast(dimension_list);
            state->current_token_position = saved_pos;
            return allow_expression ? parse_expression(state) : NULL;
        }
    }
    
    Type *type = NULL;
    bool has_explicit_type = false;
    
    /* Parse optional type specifier */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
        advance_token(state);
        type = parse_type_specifier(state, true);
        if (!type) {
            free(name);
            free(state_modifier);
            if (dimension_list)
                parser__free_ast(dimension_list);
            if (parameter_list)
                parser__free_ast(parameter_list);
            state->current_token_position = saved_pos;
            return allow_expression ? parse_expression(state) : NULL;
        }
        has_explicit_type = true;
    }
    
    /* Parse optional default value */
    ASTNode *default_value = NULL;
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EQUAL)) {
        advance_token(state);
        default_value = parse_expression(state);
        if (!default_value) {
            free(name);
            free(state_modifier);
            parser__free_type(type);
            if (dimension_list)
                parser__free_ast(dimension_list);
            if (parameter_list)
                parser__free_ast(parameter_list);
            state->current_token_position = saved_pos;
            return allow_expression ? parse_expression(state) : NULL;
        }
    }
    
    /* Handle function without explicit type but with all parameters having default values */
    if (is_function && !has_explicit_type) {
        bool all_params_have_default = true;
        if (parameter_list) {
            for (uint16_t i = 0; i < parameter_list->count; i++) {
                ASTNode *param = parameter_list->nodes[i];
                if (param && param->type == AST_VARIABLE_DECLARATION && 
                    param->default_value == NULL) {
                    all_params_have_default = false;
                    break;
                }
            }
        }
        
        if (!all_params_have_default && !default_value) {
            free(name);
            free(state_modifier);
            parser__free_type(type);
            if (dimension_list)
                parser__free_ast(dimension_list);
            if (parameter_list)
                parser__free_ast(parameter_list);
            if (default_value)
                parser__ast_node_pool_free(state->pool, default_value);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                "Function declaration requires explicit type unless all parameters have default values");
        }
    }
    
    /* Create node for parameters as AST_BLOCK */
    ASTNode *params_node = NULL;
    if (is_function && parameter_list) {
        params_node = create_ast_node
            ( state
            , AST_BLOCK
            , 0
            , NULL
            , NULL
            , NULL 
            , (ASTNode*)parameter_list
        );
        if (!params_node) {
            free(name);
            free(state_modifier);
            parser__free_type(type);
            if (dimension_list)
                parser__free_ast(dimension_list);
            if (default_value)
                parser__ast_node_pool_free(state->pool, default_value);
            return NULL;
        }
    }
    
    /* Check for function body */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_THEN) ||
        CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LCURLY)) {
        
        ASTNode *body_block = parse_block_statement(state);
        if (!body_block) {
            free(name);
            free(state_modifier);
            parser__free_type(type);
            if (dimension_list)
                parser__free_ast(dimension_list);
            if (params_node)
                parser__free_ast_node(params_node);
            if (default_value)
                parser__ast_node_pool_free(state->pool, default_value);
            return NULL;
        }
        
        ASTNodeType node_type;
        if (is_function)
            node_type = AST_FUNCTION_DECLARATION;
        else if (dimension_list != NULL && dimension_list->count > 0) {
            node_type = AST_ARRAY_DECLARATION;
            if (type) {
                type->is_array = 1;
                type->array_dimensions = dimension_list;
            }
        } else
            node_type = AST_VARIABLE_WITH_BODY;
        
        /* For function: left = params_node, right = body_block */
        ASTNode *node = create_ast_node(state, node_type, 0, name, 
                                       params_node, body_block, NULL);
        node->variable_type = type;
        node->state_modifier = state_modifier;
        node->default_value = default_value;
        
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON))
            advance_token(state);
        
        return node;
    }
    
    /* Create node without body */
    ASTNodeType node_type;
    if (is_function)
        node_type = AST_FUNCTION_DECLARATION;
    else if (dimension_list != NULL && dimension_list->count > 0) {
        node_type = AST_ARRAY_DECLARATION;
        if (type) {
            type->is_array = 1;
            type->array_dimensions = dimension_list;
        }
    } else node_type = AST_VARIABLE_DECLARATION;
    
    /* For function without body: left = params_node */
    ASTNode *node = create_ast_node
        ( state
        , node_type
        , 0
        , name
        , params_node
        , NULL
        , NULL
    );
    if (!node) {
        free(name);
        free(state_modifier);
        parser__free_type(type);
        if (dimension_list)
            parser__free_ast(dimension_list);
        if (params_node)
            parser__free_ast_node(params_node);
        if (default_value)
            parser__ast_node_pool_free(state->pool, default_value);
        return NULL;
    }
    
    node->state_modifier = state_modifier;
    node->variable_type = type;
    node->default_value = default_value;
    
    if (!allow_expression) EXPECT_SEMICOLON(state);
    
    return node;
}

static ASTNode *parse_parameter(ParserState *state) {
    int saved_pos = state->current_token_position;
    
    /* Try to parse as state modifier + identifier (var arg : Int) */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_STATE)) {
        Token *state_token = get_current_token(state);
        char *state_modifier = safe_strdup(state, state_token->value);
        advance_token(state);
        
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
            free(state_modifier);
            state->current_token_position = saved_pos;
            return parse_expression(state);
        }
        
        Token *name_token = get_current_token(state);
        char *name = safe_strdup(state, name_token->value);
        advance_token(state);
        
        Type *type = NULL;
        
        /* Parse optional type annotation */
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON)) {
            advance_token(state);
            type = parse_type_specifier(state, true);
            if (!type) {
                free(name);
                free(state_modifier);
                state->current_token_position = saved_pos;
                return parse_expression(state);
            }
        }
        
        /* Parse optional default value */
        ASTNode *default_value = NULL;
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EQUAL)) {
            advance_token(state);
            default_value = parse_expression(state);
            if (!default_value) {
                free(name);
                free(state_modifier);
                parser__free_type(type);
                state->current_token_position = saved_pos;
                return parse_expression(state);
            }
        }
        
        ASTNode *node = create_ast_node(state, AST_VARIABLE_DECLARATION, 0, 
                                       name, NULL, NULL, NULL);
        node->variable_type = type;
        node->state_modifier = state_modifier;
        node->default_value = default_value;
        
        return node;
    }
    
    /* Try to parse as type literal (none, Void) without state modifier */
    Type *type_literal = parse_type_specifier_silent(state, true, true);
    if (type_literal) {
        /* Check if this is the end of parameter list */
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COMMA) || 
            CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
            /* This is a type literal (e.g., none, Void) */
            ASTNode *node = create_ast_node(state, AST_LITERAL_VALUE, TOKEN_TYPE, 
                                          safe_strdup(state, type_literal->name), 
                                          NULL, NULL, NULL);
            parser__free_type(type_literal);
            return node;
        } else {
            /* Not a type literal, rollback */
            parser__free_type(type_literal);
            state->current_token_position = saved_pos;
        }
    }
    
    /* Try to parse as expression (numeric literal, identifier, etc.) */
    return parse_expression(state);
}

static AST *parse_parameter_list(ParserState *state) {
    if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN))
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected '(' for parameter list");
    advance_token(state);
    
    AST *param_list = safe_malloc(state, sizeof(AST));
    param_list->nodes = NULL;
    param_list->count = 0;
    param_list->capacity = 0;
    
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN)) {
        advance_token(state);
        return param_list;
    }
    
    while (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RPAREN) &&
           !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
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
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected ',' or ')' in parameter list");
        }
    }
    
    CONSUME_TOKEN(state, TOKEN_RPAREN);
    return param_list;
}

static ASTNode *parse_statement(ParserState *state) {
    /* Skip empty statements */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
        advance_token(state);
        return NULL;
    }
    
    TokenType token_type = get_current_token_type(state);
    
    if (token_type == TOKEN_STATE) {
        /* Save position for error reporting */
        Token *state_token = get_current_token(state);
        int saved_pos = state->current_token_position;
        
        /* Parse object declaration - state token is mandatory here */
        ASTNode *declaration = parse_object_declaration(state, false);
        if (declaration) {
            return declaration;
        }
        
        /* If we get here, parsing failed. Report specific error */
        state->current_token_position = saved_pos;
        
        /* Check what went wrong */
        advance_token(state); /* Skip state token */
        
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
            state->current_token_position = saved_pos;
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC, "Expected identifier after state modifier");
        }
        
        if (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
            state->current_token_position = saved_pos;
            const char *actual = (get_current_token(state)) 
                               ? token_names[get_current_token_type(state)]
                               : "EOF";
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                "Expected identifier after '%s', got %s",
                state_token->value, actual);
        }
        
        /* We got identifier, but declaration still failed */
        state->current_token_position = saved_pos;
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
            "Invalid object declaration after '%s %s'",
            state_token->value, get_current_token(state)->value);
    }
    
    if (token_type == TOKEN_LCURLY)
        return parse_block_statement(state);
    
    switch (token_type) {
        case TOKEN_IF:
            return parse_if_statement(state);
        
        case TOKEN_RETURN:
            return parse_return_statement(state);
        
        case TOKEN_FREE: {
            ASTNode *free_stmt = parse_free_statement(state);
            if (free_stmt) return free_stmt;
            break;
        }
        
        case TOKEN_DOT: {
            ASTNode *label_stmt = parse_label_declaration(state);
            if (label_stmt) return label_stmt;
            break;
        }
        
        case TOKEN_JUMP: {
            ASTNode *jump_stmt = parse_jump_statement(state);
            if (jump_stmt) return jump_stmt;
            break;
        }
        
        case TOKEN_INTER: {
            ASTNode *inter_stmt = parse_inter(state);
            if (inter_stmt) return inter_stmt;
            break;
        }
        
        case TOKEN_SIGNAL: {
            ASTNode *signal_stmt = parse_signal(state);
            if (signal_stmt) return signal_stmt;
            break;
        }
        
        case TOKEN_PARSEOF: {
            ASTNode *parseof_stmt = parse_parseof_statement(state);
            if (parseof_stmt) return parseof_stmt;
            break;
        }
        
        case TOKEN_PUSH: {
            ASTNode *push_stmt = parse_push_statement(state);
            if (push_stmt) return push_stmt;
            break;
        }
        
        case TOKEN_NOP: {
            ASTNode *nop_stmt = parse_nop_statement(state);
            if (nop_stmt) return nop_stmt;
            break;
        }
        
        case TOKEN_HALT: {
            ASTNode *halt_stmt = parse_halt_statement(state);
            if (halt_stmt) return halt_stmt;
            break;
        }
        
        default: break;
    }    
    
    int saved_pos = state->current_token_position;
    
    /* Check for TOKEN_STATE in expression */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_STATE)) {
        Token *error_token = get_current_token(state);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_INVALID_STATEMENT,
            "State modifier '%s' cannot be used in expression context. "
            "Use it only for object declarations (e.g., 'var x = 5')",
            error_token->value);
    }
    
    /* Try to parse as expression */
    ASTNode *expression = parse_expression(state);
    if (!expression) {
        state->current_token_position = saved_pos;
        
        /* Check if this is an invalid object declaration without state */
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_ID)) {
            Token *id_token = get_current_token(state);
            advance_token(state);
            
            /* Check if it looks like a declaration (has colon or braces) */
            if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_COLON) ||
                CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LBRACE) ||
                CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_LPAREN)) {
                
                state->current_token_position = saved_pos;
                REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                    "Object declaration requires state modifier. "
                    "Use 'var %s' or 'func %s' instead of just '%s'",
                    id_token->value, id_token->value, id_token->value);
            }
            
            state->current_token_position = saved_pos;
        }
        
        /* Generic syntax error */
        Token *error_token = get_current_token(state);
        if (error_token) {
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_GENERIC,
                "Invalid statement starting with '%s'",
                error_token->value);
        } else REPORT_UNEXPECTED_EOF(state);
    }
    
    /* Validate that expression can be a statement */
    if (!is_valid_statement_expression(expression)) {
        /* Provide helpful error message based on expression type */
        if (expression->type == AST_LITERAL_VALUE) {
            const char *literal_type = "";
            switch (expression->operation_type) {
                case TOKEN_NUMBER: 
                    literal_type = "number"; 
                    break;
                case TOKEN_STRING: 
                    literal_type = "string"; 
                    break;
                case TOKEN_CHAR: 
                    literal_type = "character"; 
                    break;
                case TOKEN_NULL: 
                    literal_type = "null"; 
                    break;
                case TOKEN_NONE: 
                    literal_type = "none"; 
                    break;
                default: 
                    literal_type = "literal"; 
                    break;
            }
            
            parser__ast_node_pool_free(state->pool, expression);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_INVALID_STATEMENT,
                "Invalid statement: %s literal cannot be used as a standalone statement",
                literal_type);
        }
        
        if (expression->type == AST_MULTI_INITIALIZER) {
            parser__ast_node_pool_free(state->pool, expression);
            REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_INVALID_STATEMENT,
                "Invalid statement: multi-initializer must be used in assignment or declaration");
        }
        
        parser__ast_node_pool_free(state->pool, expression);
        REPORT_PARSE_ERROR(state, ERROR_CODE_SYNTAX_INVALID_STATEMENT,
            "Invalid statement: expression has no effect");
    }
    
    /* Expect semicolon after expression statement */
    if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)) {
        advance_token(state);
    } else {
        /* Try to recover by consuming until semicolon if possible */
        Token *current = get_current_token(state);
        if (current) {
            errhandler__report_error(
                ERROR_CODE_SYNTAX_MISSING_SEMICOLON,
                current->line,
                current->column,
                "syntax",
                "Expected ';' after expression"
            );
        }
        
        /* Try to consume tokens until semicolon or end of block */
        while (!CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON)
            && !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_RCURLY)
            && !CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_EOF)) {
            advance_token(state);
        }
        
        if (CURRENT_TOKEN_TYPE_MATCHES(state, TOKEN_SEMICOLON))
            advance_token(state);
        
        /* Return the expression even with missing semicolon */
        return expression;
    }
    
    return expression;
}

AST *parse(Token *tokens, uint16_t token_count) {
    ParserState state;
    AST *ast;
    
    state.current_token_position = 0;
    state.token_stream = tokens;
    state.total_tokens = token_count;
    state.pool = parser__ast_node_pool_create(AST_POOL_INITIAL_CAPACITY);
    state.in_declaration_context = true;
    
    if (!state.pool) return NULL;
    
    ast = safe_malloc(&state, sizeof(AST));
    ast->nodes = NULL;
    ast->count = 0;
    ast->capacity = 0;
    ast->pool = state.pool;
    
    while (get_current_token_type(&state) != TOKEN_EOF) {
        ASTNode *statement = parse_statement(&state);
        
        if (statement) {
            if (!add_ast_node_to_list(ast, statement)) {
                parser__ast_node_pool_free(state.pool, statement);
                parser__free_ast(ast);
                return NULL;
            }
        } else {
            Token *current = get_current_token(&state);
            if (current) {
                errhandler__report_error(
                    ERROR_CODE_SYNTAX_GENERIC,
                    current->line,
                    current->column,
                    "syntax",
                    "Syntax error, skipping token"
                );
            }
            
            if (state.current_token_position < state.total_tokens)
                state.current_token_position++;
        }
    }
    
    return ast;
}

void parser__free_type(Type *type) {
    if (!type) return;
    
    free(type->name);
    free(type->access_modifier);
    
    if (type->modifiers) {
        for (uint8_t i = 0; i < type->modifier_count; i++)
            free(type->modifiers[i]);
        free(type->modifiers);
    }
    
    if (type->array_dimensions)
        parser__free_ast(type->array_dimensions);
    
    if (type->compound_types) {
        for (uint8_t i = 0; i < type->compound_count; i++)
            parser__free_type(type->compound_types[i]);
        free(type->compound_types);
    }
    
    if (type->angle_expression)
        parser__free_ast_node(type->angle_expression);
    
    free(type);
}

void parser__free_ast_node(ASTNode *node) {
    if (!node) return;
    
    if (node->left) parser__free_ast_node(node->left);
    
    if (node->right) parser__free_ast_node(node->right);
    
    if (node->extra) {
        if (node->type == AST_FUNCTION_DECLARATION) {
            AST *args_ast = (AST*)node->extra;
            if (args_ast->nodes) {
                for (uint16_t i = 0; i < args_ast->count; i++)
                    parser__free_ast_node(args_ast->nodes[i]);
                free(args_ast->nodes);
            }
            free(args_ast);
        } else if (node->type == AST_BLOCK) {
            if (node->left == NULL && node->right == NULL) {
                AST *block_ast = (AST*)node->extra;
                if (block_ast->nodes) {
                    for (uint16_t i = 0; i < block_ast->count; i++) {
                        parser__free_ast_node(block_ast->nodes[i]);
                    }
                    free(block_ast->nodes);
                }
                free(block_ast);
            } else parser__free_ast_node(node->extra);
        } else if (node->type == AST_MULTI_INITIALIZER) {
            AST *init_list = (AST*)node->extra;
            if (init_list->nodes) {
                for (uint16_t i = 0; i < init_list->count; i++)
                    parser__free_ast_node(init_list->nodes[i]);
                free(init_list->nodes);
            }
            free(init_list);
        } else parser__free_ast_node(node->extra);
    }
    
    if (node->default_value)
        parser__free_ast_node(node->default_value);
    
    free(node->value);
    free(node->state_modifier);
    free(node->access_modifier);
    
    if (node->variable_type)
        parser__free_type(node->variable_type);
}

void parser__free_ast(AST *ast) {
    if (!ast) return;
    
    for (uint16_t i = 0; i < ast->count; i++)
        if (ast->nodes[i]) parser__free_ast_node(ast->nodes[i]);
    
    free(ast->nodes);
    
    if (ast->pool) parser__ast_node_pool_destroy(ast->pool);
    
    free(ast);
}
