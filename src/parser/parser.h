#ifndef PARSER_H
#define PARSER_H

#include "../lexer/lexer.h"
#include "../errhandler/errhandler.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * AST node type enumeration.
 * Each value identifies the kind of syntactic construct represented by an AST node.
 */
typedef enum {
    AST_VARIABLE_DECLARATION,   /* Single variable declaration (e.g., variable : Void;).           */
    AST_VARIABLE_WITH_BODY,     /* Variable declaration with an associated body (unused).          */
    AST_VARIABLE_LIST,          /* List of variable declarations (comma-separated).                */
    AST_FUNCTION_DECLARATION,   /* Function declaration or definition (func name(params) : ret).   */
    AST_ARRAY_ACCESS,           /* Array subscript access, e.g., arr[index].                       */
    AST_BINARY_OPERATION,       /* Binary operation (+, -, *, /, etc.).                            */
    AST_UNARY_OPERATION,        /* Unary operation (+, -, !, ~, etc.).                             */
    AST_LITERAL_VALUE,          /* Literal constant (number, string, character).                   */
    AST_IDENTIFIER,             /* Identifier (variable or function name).                         */
    AST_REGISTER,               /* Register access (%%register).                                   */
    AST_ASSIGNMENT,             /* Simple assignment (=).                                          */
    AST_COMPOUND_ASSIGNMENT,    /* Compound assignment (+=, -=, etc.).                             */
    AST_BLOCK,                  /* Block of statements enclosed in {}.                             */
    AST_IF_STATEMENT,           /* If statement with optional else branch.                         */
    AST_RETURN,                 /* Return statement (return expr, ...).                            */
    AST_FREE,                   /* Free memory statement (free expression).                        */
    AST_SIZEOF,                 /* sizeof operator.                                                */
    AST_STACK,                  /* Stack operation (unused placeholder).                           */
    AST_PUSH,                   /* Push operation (unused placeholder).                            */
    AST_POP,                    /* Pop operation (unused placeholder).                             */
    AST_CAST,                   /* Type cast (e.g., (int)expr).                                    */
    AST_SIGNAL,                 /* Signal statement (signal(params) : type).                       */
    AST_INTERFLAG,              /* Interflag operation (statement or expression).                  */
    AST_MULTI_INITIALIZER,      /* Multi-value initializer list {a, b, c}.                         */
    AST_LABEL_DECLARATION,      /* Label declaration (@label:).                                    */
    AST_JUMP,                   /* Jump statement (jump target).                                   */
    AST_POSTFIX_CAST,           /* Postfix cast (expr->type).                                      */
    AST_FIELD_ACCESS,           /* Field access (struct.field or ptr->field).                      */
    AST_NOP,                    /* No operation (nop).                                             */
    AST_ARRAY_DECLARATION,      /* Array declaration (unused placeholder).                         */
    AST_HALT,                   /* Halt execution (halt).                                          */
    AST_TYPE_CHANGE,            /* Type change operation (unused).                                 */
    AST_MULTI_ASSIGNMENT,       /* Multi-assignment (multiple values).                             */
    AST_COMPOUND_TYPE,          /* Compound type (struct declaration).                             */
    AST_PREFIX_INCREMENT,       /* Prefix increment (++x).                                         */
    AST_PREFIX_DECREMENT,       /* Prefix decrement (--x).                                         */
    AST_POSTFIX_INCREMENT,      /* Postfix increment (x++).                                        */
    AST_POSTFIX_DECREMENT,      /* Postfix decrement (x--).                                        */
    AST_LABEL_VALUE,            /* Label as value (unused).                                        */
    AST_ALLOC,                  /* alloc() built-in function call.                                 */
    AST_REALLOC,                /* realloc() built-in function call.                               */
    AST_DO_LOOP,                /* Do loop: do (condition) { ... } [else { ... }].                */
    AST_BREAK,                  /* Break statement with optional label (.label).                   */
    AST_CONTINUE,               /* Continue statement with optional label (.label).                */
    AST_TERNARY_OPERATION,      /* Ternary conditional (cond ? {true} : {false}).                  */
    AST_TYPEOF,                 /* Typeof operator.                                                */
    AST_KILL,                   /* Kill statement (kill).                                          */
    AST_ELSE_STATEMENT          /* Standalone else statement (e.g., else => nop;).                */
} ASTNodeType;

/*
 * Type structure: holds all information about a type in the language.
 * Includes base name, modifiers, pointer/reference levels, and generic arguments.
 */
typedef struct Type {
    char *name;                     /* Base type name (e.g., "Int", "MyStruct").                  */
    char *access_modifier;          /* Access modifier (reserved for future use).                 */
    char **modifiers;               /* Array of type modifier strings (const, volatile, register, signed, unsigned). */
    uint8_t modifier_count;         /* Number of modifiers.                                       */
    uint8_t pointer_level;          /* Number of pointer indirections (@).                        */
    uint8_t is_reference;           /* Whether the type is a reference (&).                       */
    uint8_t prefix_number;          /* Numeric prefix (unused).                                   */
    uint8_t is_array;               /* Flag indicating that this type is an array.                */
    uint8_t size_in_bytes;          /* Explicit size in bytes (from type<size> syntax).           */
    struct AST *array_dimensions;   /* AST for array dimensions (if array).                       */
    struct Type **compound_types;   /* Nested compound types (for generics).                      */
    uint8_t compound_count;         /* Number of compound types.                                  */
    struct ASTNode *angle_expression; /* Expression inside angle brackets (e.g., type<expr>).     */
    struct ASTNode *typeof_expression; /* Expression for typeof (typeof(expr)).                   */
} Type;

/*
 * AST node structure.
 * Contains all information for a single syntactic node, including location data
 * for accurate error reporting during semantic analysis.
 */
typedef struct ASTNode {
    ASTNodeType type;               /* Type of this AST node.                                     */
    TokenType operation_type;       /* Token type for operations (e.g., TOKEN_PLUS).              */
    char *value;                    /* String value (for literals, identifiers, labels).          */
    uint16_t line;                  /* Source line number where the node starts.                  */
    uint16_t column;                /* Source column number where the node starts.                */
    struct ASTNode *left;           /* Left child (for binary/ternary operations).                */
    struct ASTNode *right;          /* Right child (for binary/ternary operations).               */
    struct ASTNode *extra;          /* Extra child or list (e.g., block statements, catch list).  */
    Type *variable_type;            /* Type information associated with this node.                */
    struct ASTNode *default_value;  /* Default value (for variable declarations).                 */
    char *state_modifier;           /* State modifier (e.g., "var", "func", "struct").            */
    char *access_modifier;          /* Access modifier (reserved).                                */
    char *parent_struct;            /* Parent struct name (from 'extends' statement inside struct). */
    char **modifiers;               /* Declaration modifiers (extern, static, const, volatile, register, signed, unsigned). */
    uint8_t modifier_count;         /* Number of declaration modifiers.                           */
} ASTNode;

/*
 * AST node pool.
 * Provides fast allocation and reuse of ASTNode structures using a free list.
 */
typedef struct ASTNodePool {
    ASTNode *nodes;                 /* Contiguous array of nodes.                                 */
    uint16_t size;                  /* Number of currently allocated nodes (unused).              */
    uint16_t capacity;              /* Maximum number of nodes that can be stored.                */
    uint16_t *free_list;            /* Stack of free indices for reuse.                           */
    uint16_t free_top;              /* Top index of the free list stack.                          */
} ASTNodePool;

/*
 * AST root structure.
 * Holds the top-level statements of a program.
 */
typedef struct AST {
    ASTNode **nodes;                /* Array of top-level AST nodes.                              */
    uint16_t count;                 /* Number of nodes currently in the array.                    */
    uint16_t capacity;              /* Capacity of the nodes array.                               */
    ASTNodePool *pool;              /* Pool from which all nodes were allocated.                  */
} AST;

/*
 * Parser state.
 * Maintains the token stream, current position, and the node pool.
 */
typedef struct ParserState {
    uint16_t current_token_position; /* Index of the current token in the token stream.           */
    Token *token_stream;            /* Array of tokens produced by the lexer.                     */
    uint16_t total_tokens;          /* Total number of tokens in the stream.                      */
    ASTNodePool *pool;              /* Node pool for AST allocations.                             */
    bool in_declaration_context;    /* Flag indicating whether the parser is in a declaration context. */
} ParserState;

/*
 * Helper macros for creating common AST node patterns.
 * These macros simplify the construction of binary, unary, assignment, and ternary nodes.
 */
#define AST_NEW_BINARY(state, op, left, right) \
    create_ast_node(state, AST_BINARY_OPERATION, op, NULL, left, right, NULL)

#define AST_NEW_UNARY(state, op, operand) \
    create_ast_node(state, AST_UNARY_OPERATION, op, NULL, NULL, operand, NULL)

#define AST_NEW_ASSIGNMENT(state, op, left, right) \
    create_ast_node(state, ((op) == TOKEN_EQUAL) ? AST_ASSIGNMENT : AST_COMPOUND_ASSIGNMENT, op, NULL, left, right, NULL)

#define AST_NEW_TERNARY(state, cond, true_expr, false_expr) \
    create_ast_node(state, AST_TERNARY_OPERATION, 0, NULL, cond, true_expr, false_expr)

/*
 * AST node pool management functions.
 */
ASTNodePool *parser__ast_node_pool_create(uint16_t initial_capacity);
void parser__ast_node_pool_destroy(ASTNodePool *pool);
ASTNode *parser__ast_node_pool_alloc(ASTNodePool *pool);
void parser__ast_node_pool_free(ASTNodePool *pool, ASTNode *node);

/*
 * Memory cleanup functions for types and AST structures.
 */
void parser__free_type(Type *type);
void parser__free_ast_node(ASTNode *node);
void parser__free_ast(AST *ast);

/*
 * Main parsing entry point.
 * Consumes a token array and returns the root AST of the program.
 */
AST *parse(Token *tokens, uint16_t token_count);

#endif /* PARSER_H */
