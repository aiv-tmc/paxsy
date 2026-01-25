#ifndef PARSER_H
#define PARSER_H

#include "../lexer/lexer.h"
#include "../errman/errman.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * AST Node typrs for syntax tree representation
 */

typedef enum {
    AST_VARIABLE_DECLARATION,
    AST_VARIABLE_WITH_BODY,
    AST_FUNCTION_DECLARATION,
    AST_ARRAY_ACCESS,
    AST_BINARY_OPERATION,
    AST_UNARY_OPERATION,
    AST_LITERAL_VALUE,
    AST_IDENTIFIER,
    AST_REGISTER,
    AST_ASSIGNMENT,
    AST_COMPOUND_ASSIGNMENT,
    AST_BLOCK,
    AST_IF_STATEMENT,
    AST_RETURN,
    AST_FREE,
    AST_SIZEOF,
    AST_PARSEOF,
    AST_TYPEOF,
    AST_STACK,
    AST_PUSH,
    AST_POP,
    AST_CAST,
    AST_SIGNAL,
    AST_INTER,
    AST_MULTI_INITIALIZER,
    AST_LABEL_DECLARATION,
    AST_JUMP,
    AST_POSTFIX_CAST,
    AST_FIELD_ACCESS,
    AST_NOP,
    AST_ARRAY_DECLARATION,
    AST_HALT,
    AST_TYPE_CHANGE,
    AST_MULTI_ASSIGNMENT,
    AST_COMPOUND_TYPE,
    AST_PREFIX_INCREMENT,
    AST_PREFIX_DECREMENT,
    AST_POSTFIX_INCREMENT,
    AST_POSTFIX_DECREMENT,
    AST_LABEL_VALUE,
    AST_ALLOC,
    AST_REALLOC,
    AST_DO_LOOP,
    AST_BREAK,
    AST_CONTINUE,
    AST_TERNARY_OPERATION
} ASTNodeType;

/*
 * Type structure with modifiers and attributes
 */

typedef struct Type {
    char *name;                    /* Type name (e.g., "Int", "String")       */
    char *access_modifier;         /* Access modifier (public/private)        */
    char **modifiers;              /* Type modifiers array                    */
    uint8_t modifier_count;        /* Number of modifiers                     */
    uint8_t pointer_level;         /* Pointer indirection level               */
    uint8_t is_reference;          /* Reference flag                          */
    uint8_t is_register;           /* Register storage flag                   */
    uint8_t prefix_number;         /* Numeric prefix for special types        */
    uint8_t is_array;              /* Array type flag                         */
    uint8_t size_in_bytes;         /* Type size in bytes                      */
    struct AST *array_dimensions;  /* Array dimensions expression list        */
    struct Type **compound_types;  /* Compound type components                */
    uint8_t compound_count;        /* Number of compound type components      */
    struct ASTNode *angle_expression;  /* Angle bracket expression            */
} Type;

/*
 * Abstract syntax tree node structure
 */

typedef struct ASTNode {
    ASTNodeType type;              /* Node type from ASTNodeType enum         */
    TokenType operation_type;      /* Token type for operations               */
    char *value;                   /* String value for literals/identifiers   */
    struct ASTNode *left;          /* Left child node                         */
    struct ASTNode *right;         /* Right child node                        */
    struct ASTNode *extra;         /* Additional node for special cases       */
    Type *variable_type;           /* Variable/function return type           */
    struct ASTNode *default_value; /* Default value for parameters/variables  */
    char *state_modifier;          /* State modifier (var, let, const, etc.)  */
    char *access_modifier;         /* Access modifier for declarations        */
} ASTNode;

/*
 * Pool-based memory management for AST nodes 
 */

typedef struct ASTNodePool {
    ASTNode *nodes;                /* Array of AST nodes                      */
    uint16_t size;                 /* Current number of allocated nodes       */
    uint16_t capacity;             /* Maximum capacity of the pool            */
    uint16_t *free_list;           /* List of free node indices              */
    uint16_t free_top;             /* Index of top element in free list      */
} ASTNodePool;

/*
 * Complete AST containing all parsed statements
 */

typedef struct AST {
    ASTNode **nodes;               /* Array of statement nodes                */
    uint16_t count;                /* Number of statements                    */
    uint16_t capacity;             /* Current capacity of nodes array         */
    ASTNodePool *pool;             /* Memory pool for AST nodes               */
} AST;

/*
 * Parser state trecking position and tokens
 */

typedef struct ParserState {
    uint16_t current_token_position; /* Current position in token stream      */
    Token *token_stream;            /* Array of tokens from lexer             */
    uint16_t total_tokens;          /* Total number of tokens                 */
    ASTNodePool *pool;              /* AST node memory pool                   */
    bool in_declaration_context;    /* Flag for declaration context           */
} ParserState;

/*
 * Memory management functions
 */

/*
 * Create AST node memory pool
 * @param initial_capacity: initial pool size
 * @return: allocated pool or NULL on failure
 */
ASTNodePool *parser__ast_node_pool_create(uint16_t initial_capacity);

/*
 * Destroy AST node pool and release memory
 * @param pool: pool to destroy
 */
void parser__ast_node_pool_destroy(ASTNodePool *pool);

/*
 * Allocate single node from pool
 * @param pool: source memory pool
 * @return: allocated node or NULL
 */
ASTNode *parser__ast_node_pool_alloc(ASTNodePool *pool);

/*
 * Return node to pool for reuse
 * @param pool: target memory pool
 * @param node: node to free
 */
void parser__ast_node_pool_free(ASTNodePool *pool, ASTNode *node);

/*
 * Recursively free type structure
 * @param type: type structure to free
 */
void parser__free_type(Type *type);

/*
 * Free AST node and all children
 * @param node: root node to free
 */
void parser__free_ast_node(ASTNode *node);

/*
 * Free entire AST structure
 * @param ast: AST to free
 */
void parser__free_ast(AST *ast);

/*
 * Main parsing functions
 */

/*
 * Main parsing entry point
 * @param tokens: token array from lexer
 * @param token_count: number of tokens
 * @return: complete AST or NULL on failure
 */
AST *parse(Token *tokens, uint16_t token_count);

#endif
