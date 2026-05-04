#ifndef PARSER_H
#define PARSER_H

#include "../lexer/lexer.h"
#include "../errhandler/errhandler.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Enumeration of all possible AST node types.
 * Every construct of the language is represented by exactly one value. */
typedef enum {
    /* Declarations */
    AST_VARIABLE_DECLARATION,
    AST_VARIABLE_LIST,
    AST_FUNCTION_DECLARATION,

    /* Statements */
    AST_BLOCK,
    AST_IF_STATEMENT,
    AST_ELSE_STATEMENT,
    AST_LABEL_DECLARATION,
    AST_DO_LOOP,
    AST_BREAK,
    AST_CONTINUE,
    AST_NOP,
    AST_SIGNAL,
    AST_ASM,
    AST_JUMP,
    AST_RETURN,
    AST_SIZEOF,
    AST_TYPEOF,
    AST_ALLOC,
    AST_REALLOC,
    AST_FREE,
    AST_DEF,
    AST_DEL,
    AST_PRO,

    /* Expressions */
    AST_IDENTIFIER,
    AST_LITERAL_VALUE,
    AST_BINARY_OPERATION,
    AST_UNARY_OPERATION,
    AST_TERNARY_OPERATION,
    AST_ASSIGNMENT,
    AST_COMPOUND_ASSIGNMENT,
    AST_POSTFIX_INCREMENT,
    AST_POSTFIX_DECREMENT,
    AST_PREFIX_INCREMENT,
    AST_PREFIX_DECREMENT,
    AST_FIELD_ACCESS,
    AST_ARRAY_ACCESS,
    AST_FUNCTION_CALL,
    AST_CAST,
    AST_MULTI_INITIALIZER,
    AST_MULTI_ASSIGNMENT,

    AST_ERROR
} ASTNodeType;

/* Comprehensive type descriptor – encodes all static type information.
 * The '$' (dollar) prefix has been removed from the language. */
typedef struct Type {
    char *name;                     /* base type name (e.g., "Int") */
    char *access_modifier;          /* reserved for future use */
    char **modifiers;               /* array of type modifiers */
    uint8_t modifier_count;
    uint8_t pointer_level;          /* number of '@' indirections */
    uint8_t is_reference;           /* 1 if '&' present */
    uint8_t is_array;               /* 1 if array dimensions are set */
    bool is_const;                  /* const qualifier */
    uint8_t size_in_bytes;          /* explicit size from <N> (0 = default) */
    struct ASTNode *array_dimensions;   /* MULTI_INITIALIZER of dimension sizes */
    struct ASTNode *angle_expression;   /* generic or size expression in < > */
    struct ASTNode *typeof_expression;  /* expression inside typeof() */
} Type;

/* A single node in the abstract syntax tree.
 * Union-like usage: different fields are active for different types.
 * - left / right: typical binary structure
 * - extra:         used for lists (BLOCK, MULTI_INITIALIZER, FUNCTION_CALL, …)
 * - default_value: initializer expression for variables
 * - variable_type: target type for declarations, casts, etc. */
typedef struct ASTNode {
    ASTNodeType type;
    TokenType operation_type;       /* token type for operators */
    char *value;                    /* identifier / literal / label */
    uint16_t line;
    uint16_t column;
    struct ASTNode *left;
    struct ASTNode *right;          /* also used as variable body (struct definition) */
    struct ASTNode *extra;          /* additional child or list */
    Type *variable_type;            /* type attached to this node */
    struct ASTNode *default_value;  /* initializer (kept for backward compat.) */
    char *state_modifier;           /* "def", "del", "pro", "func", … */
    char *access_modifier;          /* public / private (future) */
    bool is_const;
} ASTNode;

/* Arena‑like memory pool for ASTNode structures.
 * Automatically expands on demand to avoid out‑of‑memory situations. */
typedef struct ASTNodePool {
    ASTNode *nodes;                 /* contiguous array */
    uint16_t capacity;
    uint16_t *free_list;            /* stack of free indices */
    uint16_t free_top;
} ASTNodePool;

/* Top‑level AST – a dynamic array of statements. */
typedef struct AST {
    ASTNode **nodes;
    uint16_t count;
    uint16_t capacity;
    ASTNodePool *pool;              /* pool from which all nodes are taken */
    bool had_errors;
} AST;

/* Parser state – holds the stream, position, pool and error flags. */
typedef struct ParserState {
    uint16_t current_token_position;
    Token *token_stream;
    uint16_t total_tokens;
    ASTNodePool *pool;
    bool panic_mode;                /* set during error recovery */
    bool fatal_error;               /* set on unrecoverable memory / logic error */
} ParserState;

/* Helper macros for building AST nodes (type‑safe shorthands). */
#define AST_NEW_BINARY(state, op, left, right) \
    create_ast_node(state, AST_BINARY_OPERATION, op, NULL, left, right, NULL)
#define AST_NEW_UNARY(state, op, operand) \
    create_ast_node(state, AST_UNARY_OPERATION, op, NULL, NULL, operand, NULL)
#define AST_NEW_ASSIGNMENT(state, op, left, right) \
    create_ast_node(state, ((op) == TOKEN_EQUAL) ? AST_ASSIGNMENT : AST_COMPOUND_ASSIGNMENT, op, NULL, left, right, NULL)
#define AST_NEW_TERNARY(state, cond, t, f) \
    create_ast_node(state, AST_TERNARY_OPERATION, 0, NULL, cond, t, f)

/* Public API – pool management, cleanup and main entry point. */
ASTNodePool *parser__ast_node_pool_create(uint16_t initial_capacity);
void parser__ast_node_pool_destroy(ASTNodePool *pool);
ASTNode *parser__ast_node_pool_alloc(ASTNodePool *pool);
ASTNode *parser__ast_node_pool_alloc_or_expand(ParserState *state);
void parser__ast_node_pool_free(ASTNodePool *pool, ASTNode *node);   /* <-- added to resolve implicit declaration */

void parser__free_type(Type *type);
void parser__free_ast_node(ASTNode *node);
void parser__free_ast(AST *ast);

AST *parse(Token *tokens, uint16_t token_count);

#endif
