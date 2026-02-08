#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../parser/parser.h"
#include "../lexer/lexer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Data types for semantic analysis
 */
typedef enum {
    TYPE_UNKNOWN,      /**< Unknown or invalid type */
    TYPE_INT,          /**< Integer type */
    TYPE_REAL,         /**< Floating-point type */
    TYPE_CHAR,         /**< Character type */
    TYPE_STRING,       /**< String type */
    TYPE_BOOL,         /**< Boolean type */
    TYPE_VOID,         /**< Void type (no value) */
    TYPE_NONE,         /**< None/Null type */
    TYPE_POINTER,      /**< Pointer type */
    TYPE_REFERENCE,    /**< Reference type */
    TYPE_ARRAY,        /**< Array type */
    TYPE_FUNCTION,     /**< Function type */
    TYPE_COMPOUND      /**< Compound type (tuple, struct, etc.) */
} DataType;

/**
 * Function parameter information
 */
typedef struct FunctionParam {
    char* name;                    /**< Parameter name */
    DataType type;                 /**< Parameter type */
    Type* type_info;               /**< Detailed type information */
    struct FunctionParam* next;    /**< Next parameter */
} FunctionParam;

/**
 * Function signature information
 */
typedef struct FunctionSignature {
    DataType return_type;          /**< Return type */
    Type* return_type_info;        /**< Detailed return type info */
    FunctionParam* params;         /**< Parameter list */
    size_t param_count;            /**< Number of parameters */
} FunctionSignature;

/**
 * Symbol table entry representing a declared symbol
 */
typedef struct SymbolEntry {
    char* name;                    /**< Symbol name */
    DataType type;                 /**< Primary data type */
    Type* type_info;               /**< Detailed type information from AST */
    bool is_constant;              /**< Whether symbol is constant */
    bool is_initialized;           /**< Whether symbol has been initialized */
    bool is_used;                  /**< Whether symbol has been used */
    uint16_t line;                 /**< Line number of declaration */
    uint16_t column;               /**< Column number of declaration */
    union {
        FunctionSignature* func_sig; /**< Function signature (if type == TYPE_FUNCTION) */
    } extra;                       /**< Additional symbol info */
    struct SymbolEntry* next;      /**< Next entry in collision chain */
} SymbolEntry;

/**
 * Symbol table for scope management
 */
typedef struct SymbolTable {
    SymbolEntry** entries;         /**< Array of symbol entries */
    size_t capacity;               /**< Hash table capacity */
    size_t count;                  /**< Number of symbols in table */
    struct SymbolTable* parent;    /**< Parent scope table */
} SymbolTable;

/**
 * Semantic analysis context
 */
typedef struct SemanticContext {
    SymbolTable* current_scope;    /**< Current scope symbol table */
    SymbolTable* global_scope;     /**< Global scope symbol table */
    bool has_errors;               /**< Whether analysis encountered errors */
    bool warnings_enabled;         /**< Whether to emit warnings */
    bool exit_on_error;            /**< Whether to exit compilation on error */
} SemanticContext;

/**
 * Type checking result
 */
typedef struct TypeCheckResult {
    bool valid;                    /**< Type check passed */
    DataType type;                 /**< Inferred or expected type */
    Type* type_info;               /**< Detailed type information */
    char* error_msg;               /**< Error message if check failed */
} TypeCheckResult;

/**
 * Create a new semantic analysis context
 * @return New context or NULL on allocation failure
 */
SemanticContext* semantic_create_context(void);

/**
 * Destroy semantic analysis context and free all memory
 * @param context Context to destroy
 */
void semantic_destroy_context(SemanticContext* context);

/**
 * Set whether to exit compilation on semantic error
 * @param context Semantic context
 * @param exit_on_error true to exit on error
 */
void semantic_set_exit_on_error(SemanticContext* context, bool exit_on_error);

/**
 * Enter a new scope (e.g., function body, block)
 * @param context Current semantic context
 */
void semantic_enter_scope(SemanticContext* context);

/**
 * Exit current scope and return to parent scope
 * @param context Current semantic context
 */
void semantic_exit_scope(SemanticContext* context);

/**
 * Add a variable symbol to the current scope
 * @param context Semantic context
 * @param name Symbol name
 * @param type Data type
 * @param type_info Detailed type information
 * @param is_constant Whether symbol is constant
 * @param line Declaration line number
 * @param column Declaration column number
 * @return true if added successfully, false on error or redeclaration
 */
bool semantic_add_variable(SemanticContext* context, const char* name,
                          DataType type, Type* type_info, bool is_constant,
                          uint16_t line, uint16_t column);

/**
 * Add a function symbol to the current scope
 * @param context Semantic context
 * @param name Function name
 * @param return_type Return type
 * @param return_type_info Detailed return type information
 * @param params Function parameters
 * @param param_count Number of parameters
 * @param line Declaration line number
 * @param column Declaration column number
 * @return true if added successfully, false on error or redeclaration
 */
bool semantic_add_function(SemanticContext* context, const char* name,
                          DataType return_type, Type* return_type_info,
                          FunctionParam* params, size_t param_count,
                          uint16_t line, uint16_t column);

/**
 * Find a symbol in current and parent scopes
 * @param context Semantic context
 * @param name Symbol name to find
 * @return Symbol entry or NULL if not found
 */
SymbolEntry* semantic_find_symbol(SemanticContext* context, const char* name);

/**
 * Mark a symbol as used (for unused variable detection)
 * @param context Semantic context
 * @param name Symbol name
 * @return true if symbol was found and marked, false otherwise
 */
bool semantic_mark_symbol_used(SemanticContext* context, const char* name);

/**
 * Mark a symbol as initialized
 * @param context Semantic context
 * @param name Symbol name
 * @return true if symbol was found and marked, false otherwise
 */
bool semantic_mark_symbol_initialized(SemanticContext* context, const char* name);

/**
 * Check type of AST expression node
 * @param context Semantic context
 * @param node AST node to check
 * @return Type check result
 */
TypeCheckResult semantic_check_type(SemanticContext* context, ASTNode* node);

/**
 * Check type of binary operation
 * @param context Semantic context
 * @param node Binary operation AST node
 * @return Type check result
 */
TypeCheckResult semantic_check_binary_op(SemanticContext* context,
                                         ASTNode* node);

/**
 * Check type of unary operation
 * @param context Semantic context
 * @param node Unary operation AST node
 * @return Type check result
 */
TypeCheckResult semantic_check_unary_op(SemanticContext* context,
                                        ASTNode* node);

/**
 * Check type of assignment operation
 * @param context Semantic context
 * @param node Assignment AST node
 * @return Type check result
 */
TypeCheckResult semantic_check_assignment(SemanticContext* context,
                                          ASTNode* node);

/**
 * Check type of function call
 * @param context Semantic context
 * @param node Function call AST node
 * @return Type check result
 */
TypeCheckResult semantic_check_function_call(SemanticContext* context,
                                             ASTNode* node);

/**
 * Perform semantic analysis on entire AST
 * @param context Semantic context
 * @param ast Abstract syntax tree
 * @return true if analysis passed without errors, false otherwise
 */
bool semantic_analyze(SemanticContext* context, AST* ast);

/**
 * Check semantic validity of a statement
 * @param context Semantic context
 * @param node Statement AST node
 * @return true if statement is valid, false otherwise
 */
bool semantic_check_statement(SemanticContext* context, ASTNode* node);

/**
 * Check semantic validity of an expression
 * @param context Semantic context
 * @param node Expression AST node
 * @return true if expression is valid, false otherwise
 */
bool semantic_check_expression(SemanticContext* context, ASTNode* node);

/**
 * Convert token type to data type
 * @param token_type Lexer token type
 * @return Corresponding data type
 */
DataType semantic_type_from_token(TokenType token_type);

/**
 * Convert type name string to data type
 * @param type_name Type name string
 * @return Corresponding data type
 */
DataType semantic_type_from_string(const char* type_name);

/**
 * Convert data type to string representation
 * @param type Data type
 * @return String representation
 */
const char* semantic_type_to_string(DataType type);

/**
 * Check if two types are compatible for operations
 * @param type1 First type
 * @param type2 Second type
 * @return true if types are compatible, false otherwise
 */
bool semantic_types_compatible(DataType type1, DataType type2);

/**
 * Check if type can be assigned to another type
 * @param target_type Target variable type
 * @param source_type Source expression type
 * @return true if assignment is valid, false otherwise
 */
bool semantic_types_assignable(DataType target_type, DataType source_type);

/**
 * Get total number of symbols in global scope
 * @param context Semantic context
 * @return Number of symbols
 */
size_t semantic_get_symbol_count(SemanticContext* context);

/**
 * Get global symbol table for inspection
 * @param context Semantic context
 * @return Global symbol table
 */
SymbolTable* semantic_get_global_table(SemanticContext* context);

/**
 * Check if semantic analysis encountered errors
 * @param context Semantic context
 * @return true if errors were found, false otherwise
 */
bool semantic_has_errors(SemanticContext* context);

/**
 * Check if warnings are enabled
 * @param context Semantic context
 * @return true if warnings are enabled, false otherwise
 */
bool semantic_warnings_enabled(SemanticContext* context);

#endif
