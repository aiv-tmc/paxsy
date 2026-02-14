#ifndef PARSER_H
#define PARSER_H

#include "../lexer/lexer.h"
#include "../errhandler/errhandler.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * AST node types for syntax tree representation.
 * Each constant corresponds to a specific language construct.
 */
typedef enum {
    AST_VARIABLE_DECLARATION,      // Variable declaration without initializer
    AST_VARIABLE_WITH_BODY,        // Variable declaration with a body (e.g., var x = { ... })
    AST_FUNCTION_DECLARATION,      // Function declaration (with parameters and body)
    AST_ARRAY_ACCESS,              // Array indexing expression (a[i])
    AST_BINARY_OPERATION,          // Binary operation (+, -, *, etc.)
    AST_UNARY_OPERATION,           // Unary operation (!, ~, *, /)
    AST_LITERAL_VALUE,             // Literal constant (number, string, char, null, none)
    AST_IDENTIFIER,                // Identifier (variable name, etc.)
    AST_REGISTER,                  // Register name (e.g., %eax)
    AST_ASSIGNMENT,                // Simple assignment (=)
    AST_COMPOUND_ASSIGNMENT,       // Compound assignment (+=, -=, etc.)
    AST_BLOCK,                     // Block of statements ( {...} or single statement after =>)
    AST_IF_STATEMENT,              // If statement (if (cond) then-block [else-block])
    AST_RETURN,                    // Return statement
    AST_FREE,                      // Free statement (free(expr))
    AST_SIZEOF,                    // Sizeof operator
    AST_PARSEOF,                   // Parseof operator (parse-related)
    AST_TYPEOF,                    // Typeof operator
    AST_STACK,                     // Stack operation placeholder
    AST_PUSH,                      // Push statement (push expr)
    AST_POP,                       // Pop statement (pop [expr])
    AST_CAST,                      // Type cast (type) expr
    AST_SIGNAL,                    // Signal statement
    AST_MULTI_INITIALIZER,         // Multi-value initializer { expr, expr, ... }
    AST_LABEL_DECLARATION,         // Label declaration (.label:)
    AST_JUMP,                      // Jump statement (jump target)
    AST_POSTFIX_CAST,              // Postfix cast (expr->(type))
    AST_FIELD_ACCESS,              // Field access via -> (expr->field)
    AST_NOP,                       // No operation (nop;)
    AST_ARRAY_DECLARATION,         // Array declaration (var a[10] : Int)
    AST_HALT,                      // Halt statement (halt;)
    AST_TYPE_CHANGE,               // Type change operation
    AST_MULTI_ASSIGNMENT,          // Multi-assignment (left, right = ...)
    AST_COMPOUND_TYPE,             // Compound type (e.g., (Int, String))
    AST_PREFIX_INCREMENT,          // Prefix increment (++expr)
    AST_PREFIX_DECREMENT,          // Prefix decrement (--expr)
    AST_POSTFIX_INCREMENT,         // Postfix increment (expr++)
    AST_POSTFIX_DECREMENT,         // Postfix decrement (expr--)
    AST_LABEL_VALUE,               // Label value (.label)
    AST_ALLOC,                     // Alloc expression (alloc(...))
    AST_REALLOC,                   // Realloc expression (realloc(...))
    AST_DO_LOOP,                   // Do loop (do ... while ...) – not yet used?
    AST_BREAK,                     // Break statement
    AST_CONTINUE,                  // Continue statement
    AST_TERNARY_OPERATION          // Ternary conditional (cond ? true_expr : false_expr)
} ASTNodeType;

/*
 * Type structure with modifiers and attributes.
 * Represents a type in the language, including pointers, references, arrays, compound types, etc.
 */
typedef struct Type {
    char *name;                    // Base type name (e.g., "Int", "Void", "none")
    char *access_modifier;         // Access modifier (public/private) – reserved for future use
    char **modifiers;              // Array of type modifiers (e.g., "const", "volatile")
    uint8_t modifier_count;        // Number of modifiers in the array
    uint8_t pointer_level;         // Pointer indirection level (0 = not a pointer)
    uint8_t is_reference;          // Reference flag (1 = reference, 2 = double reference?)
    uint8_t is_register;           // Register storage flag (1 = register)
    uint8_t prefix_number;         // Numeric prefix for special types (unused?)
    uint8_t is_array;              // Flag indicating this is an array type
    uint8_t size_in_bytes;         // Type size in bytes (if known, e.g., Int<4>)
    struct AST *array_dimensions;  // List of dimension expressions for arrays
    struct Type **compound_types;  // Components of a compound type (e.g., tuple elements)
    uint8_t compound_count;        // Number of components in compound type
    struct ASTNode *angle_expression; // Expression inside angle brackets (generic parameters or size)
} Type;

/*
 * Abstract syntax tree node structure.
 * Each node represents a language construct.
 */
typedef struct ASTNode {
    ASTNodeType type;              // Node type from ASTNodeType enum
    TokenType operation_type;      // Token type for operations (e.g., TOKEN_PLUS, TOKEN_EQUAL)
    char *value;                   // String value for literals/identifiers/labels
    struct ASTNode *left;          // Left child node (primary operand)
    struct ASTNode *right;         // Right child node (secondary operand)
    struct ASTNode *extra;         // Additional node for special cases (e.g., argument list, else block)
    Type *variable_type;           // Type associated with the node (variable/function return type)
    struct ASTNode *default_value; // Default value for parameters/variables (after =)
    char *state_modifier;          // State modifier (var, let, const, etc.) from TOKEN_STATE
    char *access_modifier;         // Access modifier (public/private) – reserved
} ASTNode;

/*
 * Pool-based memory management for AST nodes.
 * Allows fast allocation/deallocation of nodes without fragmenting the heap.
 */
typedef struct ASTNodePool {
    ASTNode *nodes;                // Array of AST nodes (contiguous memory)
    uint16_t size;                 // Current number of allocated nodes (used slots)
    uint16_t capacity;             // Maximum capacity of the pool (total slots)
    uint16_t *free_list;           // List of free node indices (stack)
    uint16_t free_top;             // Index of top element in free list
} ASTNodePool;

/*
 * Complete AST containing all parsed statements.
 * This is the top‑level structure returned by the parser.
 */
typedef struct AST {
    ASTNode **nodes;               // Array of statement nodes
    uint16_t count;                // Number of statements in the array
    uint16_t capacity;             // Current capacity of the nodes array
    ASTNodePool *pool;             // Memory pool used for all nodes in this AST
} AST;

/*
 * Parser state tracking position and tokens during parsing.
 * Holds the current token stream and parsing context.
 */
typedef struct ParserState {
    uint16_t current_token_position; // Current index in token_stream
    Token *token_stream;            // Array of tokens from lexer
    uint16_t total_tokens;          // Total number of tokens
    ASTNodePool *pool;              // AST node memory pool (shared with AST)
    bool in_declaration_context;    // Flag indicating we are inside a declaration (affects parsing rules)
} ParserState;

/*
 * Memory management functions.
 */

/*
 * Create AST node memory pool.
 * @param initial_capacity: initial pool size (number of nodes)
 * @return: allocated pool or NULL on failure
 */
ASTNodePool *parser__ast_node_pool_create(uint16_t initial_capacity);

/*
 * Destroy AST node pool and release memory.
 * @param pool: pool to destroy
 */
void parser__ast_node_pool_destroy(ASTNodePool *pool);

/*
 * Allocate a single node from the pool.
 * @param pool: source memory pool
 * @return: allocated node (zeroed) or NULL if pool is full
 */
ASTNode *parser__ast_node_pool_alloc(ASTNodePool *pool);

/*
 * Return a node to the pool for reuse.
 * @param pool: target memory pool
 * @param node: node to free (must belong to this pool)
 */
void parser__ast_node_pool_free(ASTNodePool *pool, ASTNode *node);

/*
 * Recursively free a type structure and all its components.
 * @param type: type structure to free
 */
void parser__free_type(Type *type);

/*
 * Free an AST node and all its children recursively.
 * @param node: root node to free
 */
void parser__free_ast_node(ASTNode *node);

/*
 * Free an entire AST structure, including its nodes and pool.
 * @param ast: AST to free
 */
void parser__free_ast(AST *ast);

/*
 * Main parsing functions.
 */

/*
 * Main parsing entry point.
 * @param tokens: token array from lexer
 * @param token_count: number of tokens
 * @return: complete AST or NULL on failure (errors reported via errhandler)
 */
AST *parse(Token *tokens, uint16_t token_count);

#endif
