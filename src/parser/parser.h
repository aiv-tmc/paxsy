#ifndef PARSER_H
#define PARSER_H

#include "../lexer/lexer.h"
#include "../errhandler/errhandler.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    AST_VARIABLE_DECLARATION,       /* Variable / constant / field declaration  */
    AST_FUNCTION_DECLARATION,       /* Function declaration / definition        */
    AST_BLOCK,                      /* Brace‑delimited statement list           */
    AST_IF_STATEMENT,               /* if(cond) then … [else …]                 */
    AST_ELSE_STATEMENT,             /* standalone else block                    */
    AST_LABEL_DECLARATION,          /* identifier :                             */
    AST_DO_LOOP,                    /* do(cond) body [else body]                */
    AST_BREAK,                      /* break                                    */
    AST_CONTINUE,                   /* continue                                 */
    AST_NOP,                        /* nop                                      */
    AST_SIGNAL,                     /* signal(expr, …)                          */
    AST_HALT,                       /* halt                                     */
    AST_ASM,                        /* asm("string")                            */
    AST_JUMP,                       /* jump target                              */
    AST_RETURN,                     /* return [expr, …]                         */
    AST_SIZEOF,                     /* sizeof(expr)                             */
    AST_TYPEOF,                     /* typeof(expr)                             */
    AST_ALLOC,                      /* alloc(size, align, type)                 */
    AST_REALLOC,                    /* realloc(ptr, size)                       */
    AST_FREE,                       /* free(expr)                               */
    AST_DEL,                        /* del identifier                           */
    AST_IDENTIFIER,                 /* simple identifier                        */
    AST_LITERAL_VALUE,              /* numeric, string, char, none, type, $     */
    AST_BINARY_OPERATION,           /* e.g. a + b, a && b                       */
    AST_UNARY_OPERATION,            /* e.g. -a, !a, *a                          */
    AST_TERNARY_OPERATION,          /* cond ? true_expr : false_expr            */
    AST_ASSIGNMENT,                 /* plain =                                  */
    AST_COMPOUND_ASSIGNMENT,        /* +=, -=, etc.                             */
    AST_POSTFIX_INCREMENT,          /* a++                                      */
    AST_POSTFIX_DECREMENT,          /* a--                                      */
    AST_PREFIX_INCREMENT,           /* ++a                                      */
    AST_PREFIX_DECREMENT,           /* --a                                      */
    AST_FIELD_ACCESS,               /* a.b                                      */
    AST_ARRAY_ACCESS,               /* a[i]                                     */
    AST_FUNCTION_CALL,              /* f(args)                                  */
    AST_CAST,                       /* expr : Type                              */
    AST_MULTI_INITIALIZER,          /* { e1, e2, … }                            */
    AST_MULTI_ASSIGNMENT,           /* { a, b } = rhs                           */
    AST_STRUCT_INITIALIZER          /* expr { .field = value, … }               */
} ASTNodeType;

/* Type descriptor – describes a type annotation in the AST */
typedef struct Type {
    char *name;                     /* Base type name (e.g. "Int", "String")    */
    char **modifiers;               /* Modifier strings ("unsigned","const"…)   */
    uint8_t modifier_count;         /* Number of modifier strings allocated     */
    uint8_t pointer_level;          /* Depth of '@' pointers                    */
    uint8_t is_reference;           /* Whether '&' reference was specified      */
    uint8_t is_array;               /* Whether array brackets were used         */
    uint8_t size_in_bytes;          /* Explicit width from angle brackets       */
    struct ASTNode *array_dimensions; /* Expressions inside [ ]                 */
    struct ASTNode *angle_expression; /* Generic expression inside < >          */
    struct ASTNode *typeof_expression; /* Expression for typeof(type)           */
    struct ASTNode *struct_definition; /* Block describing an inline struct     */
} Type;

/* AST node – the fundamental tree element */
typedef struct ASTNode {
    ASTNodeType type;               /* Node kind                                */
    TokenType   operation_type;     /* Token type for operators / assignments   */
    char       *value;              /* String value (lexeme)                    */
    uint16_t    line;               /* Source line (1‑based)                    */
    uint16_t    column;             /* Source column (1‑based)                  */
    struct ASTNode *left;           /* Left child                               */
    struct ASTNode *right;          /* Right child                              */
    struct ASTNode *extra;          /* Extra data (AST* list or single node)    */
    Type        *variable_type;     /* Type annotation (declarations, casts)    */
    struct ASTNode *default_value;  /* Default initialiser / body               */
    char        *state_modifier;    /* "var", "def", "del", …                   */
    char        *access_modifier;   /* Access modifier ("public", "private")    */
    bool         is_const;          /* Constant flag                            */
} ASTNode;

/* Fast pool allocator for AST nodes – all nodes of one AST live in the pool */
typedef struct ASTNodePool {
    ASTNode  *free_head;            /* Singly‑linked free list                  */
    ASTNode **chunks;               /* Array of large allocated blocks          */
    uint16_t  chunk_count;          /* Number of chunks                         */
    uint16_t  chunk_capacity;       /* Capacity of the chunks array             */
} ASTNodePool;

/* Top‑level AST produced by parse() */
typedef struct AST {
    ASTNode    **nodes;             /* Array of top‑level statements            */
    uint16_t     count;             /* Number of statements                     */
    uint16_t     capacity;          /* Allocated size of the array              */
    ASTNodePool *pool;              /* Pool that owns all nodes                 */
    bool         had_errors;        /* True if any syntax error was reported    */
} AST;

/* Parser state – holds token stream, pool, error flags and pushback buffer */
typedef struct ParserState {
    uint16_t     current_token_position; /* Index into token_stream            */
    Token       *token_stream;       /* Array of tokens (lexer output)          */
    uint16_t     total_tokens;       /* Number of tokens in the stream          */
    ASTNodePool *pool;               /* Current node pool                       */
    bool         panic_mode;         /* Set when performing error recovery      */
    bool         fatal_error;        /* Set on memory allocation failure        */
    Token        pushback_token;     /* One‑token pushback storage              */
    bool         has_pushback;       /* True if pushback_token is valid         */
} ParserState;

/* Pool interface */
ASTNodePool *parser__ast_node_pool_create(uint16_t initial_capacity);
void         parser__ast_node_pool_destroy(ASTNodePool *pool);

/* Non‑expanding allocator – returns NULL when pool exhausted.
   Used by passes that must not trigger parser fatal errors. */
ASTNode     *parser__ast_node_pool_alloc(ASTNodePool *pool);

/* Expanding allocator – calls expand_pool() and may report fatal error.
   Used inside the recursive descent parser. */
ASTNode     *parser__ast_node_pool_alloc_or_expand(ParserState *state);

void         parser__ast_node_pool_free(ASTNodePool *pool, ASTNode *node);

/* Recursively free a subtree, returning nodes to the pool */
void         parser__free_ast_node(ASTNode *node, ASTNodePool *pool);

/* Free an entire AST including its pool */
void         parser__free_ast(AST *ast);

/* Release memory occupied by a Type descriptor */
void         parser__free_type(Type *type);

/* Main entry point – returns a fully parsed AST (or NULL on fatal error) */
AST *parse(Token *tokens, uint16_t token_count);

/* Special error code for empty parentheses in function calls etc. */
#define ERROR_CODE_SYNTAX_EMPTY_PARENS  0x7A08

#endif
