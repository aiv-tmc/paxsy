#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../parser/parser.h"
#include "../lexer/lexer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Forward declaration for symbol table structure */
typedef struct SymbolTable SymbolTable;

/**
 * @enum DataType
 * @brief Represents the fundamental data types used in semantic analysis.
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
    TYPE_COMPOUND      /**< Compound type (struct, etc.) */
} DataType;

/**
 * @enum ScopeLevel
 * @brief Defines the nesting level of a scope for visibility rules.
 */
typedef enum {
    SCOPE_GLOBAL,      /**< Global scope (file level) */
    SCOPE_FUNCTION,    /**< Function scope (parameters and function body) */
    SCOPE_BLOCK,       /**< Block scope (if, while, for, etc.) */
    SCOPE_LOOP,        /**< Loop scope (special for break/continue) */
    SCOPE_COMPOUND     /**< Compound type scope (struct member scope) */
} ScopeLevel;

/**
 * @enum InitState
 * @brief Tracks the initialization status of a variable.
 */
typedef enum {
    INIT_UNINITIALIZED,    /**< Declared but never initialized */
    INIT_PARTIAL,          /**< Partially initialized (arrays/structs) */
    INIT_FULL,             /**< Fully initialized */
    INIT_CONSTANT,         /**< Constant value (always initialized) */
    INIT_DEFAULT           /**< Default initialized (zero values) */
} InitState;

/**
 * @struct FunctionParam
 * @brief Represents a single parameter of a function.
 */
typedef struct FunctionParam {
    char* name;                    /**< Parameter name */
    DataType type;                 /**< Parameter type */
    Type* type_info;              /**< Detailed type information from AST */
    struct FunctionParam* next;   /**< Next parameter in linked list */
} FunctionParam;

/**
 * @struct FunctionSignature
 * @brief Stores complete signature information for a function.
 */
typedef struct FunctionSignature {
    DataType return_type;          /**< Return type of the function */
    Type* return_type_info;        /**< Detailed return type info */
    FunctionParam* params;         /**< Linked list of parameters */
    size_t param_count;           /**< Number of parameters */
    bool is_variadic;            /**< Whether function accepts variable arguments */
} FunctionSignature;

/**
 * @struct CompoundMember
 * @brief Debug/Output representation of a struct member.
 * @note Actual member symbols are stored in a dedicated symbol table (compound_scope).
 */
typedef struct CompoundMember {
    char* name;                    /**< Member name */
    char* state_modifier;          /**< State modifier (var, obj, etc.) */
    DataType type;                /**< Member type */
    Type* type_info;             /**< Detailed type information */
    InitState init_state;        /**< Member initialization state */
    struct CompoundMember* next; /**< Next member in linked list */
} CompoundMember;

/**
 * @struct SymbolEntry
 * @brief Entry in a symbol table representing a declared identifier.
 */
typedef struct SymbolEntry {
    char* name;                    /**< Symbol name */
    char* state_modifier;          /**< State modifier (var, obj, const, etc.) */
    DataType type;                /**< Primary data type */
    Type* type_info;             /**< Detailed type information from AST */
    bool is_constant;            /**< Whether symbol is constant */
    InitState init_state;        /**< Current initialization state */
    bool is_used;                /**< Whether symbol has been used (warnings disabled) */
    bool is_mutable;            /**< Whether symbol can be modified */
    ScopeLevel declared_scope;   /**< Scope level where symbol was declared */
    uint16_t line;              /**< Line number of declaration */
    uint16_t column;            /**< Column number of declaration */
    union {
        FunctionSignature* func_sig;   /**< Function signature (if type == TYPE_FUNCTION) */
        CompoundMember* compound_members; /**< Debug list of struct members (if type == TYPE_COMPOUND) */
    } extra;                    /**< Additional symbol-specific data */
    SymbolTable* compound_scope; /**< Scope containing struct members (if type == TYPE_COMPOUND) */
    struct SymbolEntry* next;   /**< Next entry in hash collision chain */
} SymbolEntry;

/**
 * @struct SymbolTable
 * @brief Hash table that manages symbols within a single scope.
 */
struct SymbolTable {
    SymbolEntry** entries;         /**< Hash table array of entry lists */
    size_t capacity;              /**< Size of the hash table */
    size_t count;                /**< Number of symbols in this table */
    ScopeLevel level;            /**< Scope level of this table */
    struct SymbolTable* parent;  /**< Parent scope (enclosing scope) */
    struct SymbolTable* children; /**< Linked list of child scopes */
    struct SymbolTable* next_child; /**< Next child in parent's child list */
};

/**
 * @struct SemanticContext
 * @brief Global context holding the state of the semantic analysis.
 */
typedef struct SemanticContext {
    SymbolTable* current_scope;    /**< Currently active scope */
    SymbolTable* global_scope;     /**< Root global scope */
    SymbolTable* function_scope;   /**< Current function scope (if inside function) */
    bool has_errors;              /**< Whether any error has been reported */
    bool warnings_enabled;        /**< Whether warning messages are enabled */
    bool exit_on_error;          /**< Whether to abort compilation on first error */
    bool in_loop;               /**< Whether currently inside a loop construct */
    bool in_function;           /**< Whether currently inside a function body */
    const char* current_function;/**< Name of the current function (if any) */
    DataType current_return_type;/**< Return type of the current function */
} SemanticContext;

/**
 * @struct TypeCheckResult
 * @brief Result of a type checking operation on an AST node.
 */
typedef struct TypeCheckResult {
    bool valid;                    /**< Whether type check passed */
    DataType type;                /**< Inferred or checked type */
    Type* type_info;             /**< Detailed type information */
    InitState init_state;        /**< Initialization state of the expression */
    char* error_msg;            /**< Dynamically allocated error message */
} TypeCheckResult;

/**
 * @struct VisibilityResult
 * @brief Result of a symbol visibility check.
 */
typedef struct VisibilityResult {
    bool visible;                  /**< Whether symbol is visible */
    SymbolEntry* entry;           /**< Pointer to the found symbol entry */
    ScopeLevel found_in_scope;    /**< Scope level where symbol was found */
    char* error_msg;             /**< Dynamically allocated error message */
} VisibilityResult;

/* -------------------------------------------------------------------------
   Context management
   ------------------------------------------------------------------------- */

/**
 * @brief Create a new semantic analysis context.
 * @return Pointer to new context, or NULL on allocation failure.
 */
SemanticContext* semantic_create_context(void);

/**
 * @brief Destroy a semantic context and free all associated memory.
 * @param context Context to destroy.
 */
void semantic_destroy_context(SemanticContext* context);

/**
 * @brief Set whether compilation should terminate on semantic errors.
 * @param context Semantic context.
 * @param exit_on_error true to exit on error, false to continue.
 */
void semantic_set_exit_on_error(SemanticContext* context, bool exit_on_error);

/* -------------------------------------------------------------------------
   Scope management
   ------------------------------------------------------------------------- */

/**
 * @brief Enter a new scope with a specific level.
 * @param context Semantic context.
 * @param level Scope level (SCOPE_GLOBAL, SCOPE_FUNCTION, etc.)
 */
void semantic_enter_scope_ex(SemanticContext* context, ScopeLevel level);

/**
 * @brief Enter a new scope (defaults to SCOPE_BLOCK).
 * @param context Semantic context.
 */
void semantic_enter_scope(SemanticContext* context);

/**
 * @brief Exit the current scope and return to its parent.
 * @param context Semantic context.
 */
void semantic_exit_scope(SemanticContext* context);

/**
 * @brief Enter a function scope (special handling for return types).
 * @param context Semantic context.
 * @param function_name Name of the function.
 * @param return_type Return type of the function.
 */
void semantic_enter_function_scope(SemanticContext* context,
                                   const char* function_name,
                                   DataType return_type);

/**
 * @brief Exit the current function scope.
 * @param context Semantic context.
 */
void semantic_exit_function_scope(SemanticContext* context);

/**
 * @brief Enter a loop scope (enables break/continue statements).
 * @param context Semantic context.
 */
void semantic_enter_loop_scope(SemanticContext* context);

/**
 * @brief Exit the current loop scope.
 * @param context Semantic context.
 */
void semantic_exit_loop_scope(SemanticContext* context);

/* -------------------------------------------------------------------------
   Symbol table insertion
   ------------------------------------------------------------------------- */

/**
 * @brief Add a variable symbol to a specific symbol table.
 * @param context Semantic context.
 * @param target_scope Scope to add to (if NULL, uses current_scope).
 * @param name Symbol name.
 * @param type Data type.
 * @param type_info Detailed type information (may be NULL).
 * @param is_constant Whether symbol is constant.
 * @param state_modifier State modifier string (var, obj, const, etc.) – may be NULL.
 * @param init_state Initialization state.
 * @param line Declaration line number.
 * @param column Declaration column number.
 * @return true on success, false on redeclaration or allocation failure.
 */
bool semantic_add_variable_ex(SemanticContext* context, SymbolTable* target_scope,
                              const char* name, DataType type, Type* type_info,
                              bool is_constant, const char* state_modifier,
                              InitState init_state, uint16_t line, uint16_t column);

/**
 * @brief Add a variable symbol to the current scope (simplified version).
 * @param context Semantic context.
 * @param name Symbol name.
 * @param type Data type.
 * @param type_info Detailed type information.
 * @param is_constant Whether symbol is constant.
 * @param line Declaration line number.
 * @param column Declaration column number.
 * @return true on success, false on redeclaration or allocation failure.
 */
bool semantic_add_variable(SemanticContext* context, const char* name,
                           DataType type, Type* type_info, bool is_constant,
                           uint16_t line, uint16_t column);

/**
 * @brief Add a function symbol to the current scope (full version).
 * @param context Semantic context.
 * @param name Function name.
 * @param return_type Return type.
 * @param return_type_info Detailed return type information.
 * @param params Linked list of parameters.
 * @param param_count Number of parameters.
 * @param is_variadic Whether function accepts variable arguments.
 * @param line Declaration line number.
 * @param column Declaration column number.
 * @return true on success, false on redeclaration or allocation failure.
 */
bool semantic_add_function_ex(SemanticContext* context, const char* name,
                              DataType return_type, Type* return_type_info,
                              FunctionParam* params, size_t param_count,
                              bool is_variadic, uint16_t line, uint16_t column);

/**
 * @brief Add a function symbol to the current scope (simplified version).
 * @param context Semantic context.
 * @param name Function name.
 * @param return_type Return type.
 * @param return_type_info Detailed return type information.
 * @param params Linked list of parameters.
 * @param param_count Number of parameters.
 * @param line Declaration line number.
 * @param column Declaration column number.
 * @return true on success, false on redeclaration or allocation failure.
 */
bool semantic_add_function(SemanticContext* context, const char* name,
                           DataType return_type, Type* return_type_info,
                           FunctionParam* params, size_t param_count,
                           uint16_t line, uint16_t column);

/**
 * @brief Add a compound type (struct) symbol to the current scope.
 *        Creates a dedicated scope for members and validates each member.
 * @param context Semantic context.
 * @param name Struct name.
 * @param members_ast AST node of type AST_BLOCK containing member declarations.
 * @param line Declaration line number.
 * @param column Declaration column number.
 * @return true on success, false on error.
 */
bool semantic_add_compound_type(SemanticContext* context, const char* name,
                                ASTNode* members_ast,
                                uint16_t line, uint16_t column);

/* -------------------------------------------------------------------------
   Symbol lookup
   ------------------------------------------------------------------------- */

/**
 * @brief Find a symbol by name, searching from current scope up to global.
 * @param context Semantic context.
 * @param name Symbol name.
 * @return SymbolEntry pointer, or NULL if not found.
 */
SymbolEntry* semantic_find_symbol(SemanticContext* context, const char* name);

/**
 * @brief Find a member of a struct.
 * @param context Semantic context.
 * @param struct_name Name of the struct type.
 * @param field_name Name of the member.
 * @return SymbolEntry of the member, or NULL if not found.
 */
SymbolEntry* semantic_find_struct_member(SemanticContext* context,
                                         const char* struct_name,
                                         const char* field_name);

/**
 * @brief Check visibility of a symbol with detailed error reporting.
 * @param context Semantic context.
 * @param name Symbol name.
 * @param require_initialized Whether symbol must be initialized.
 * @param allow_shadowing Whether to allow shadowed symbols.
 * @return VisibilityResult structure.
 */
VisibilityResult semantic_check_visibility(SemanticContext* context,
                                           const char* name,
                                           bool require_initialized,
                                           bool allow_shadowing);

/* -------------------------------------------------------------------------
   Symbol state manipulation
   ------------------------------------------------------------------------- */

/**
 * @brief Mark a symbol as used (does not trigger warnings – feature disabled).
 * @param context Semantic context.
 * @param name Symbol name.
 * @return true if symbol was found and marked, false otherwise.
 */
bool semantic_mark_symbol_used(SemanticContext* context, const char* name);

/**
 * @brief Update the initialization state of a variable.
 * @param context Semantic context.
 * @param name Symbol name.
 * @param new_state New initialization state.
 * @return true if update succeeded, false if symbol not found or invalid transition.
 */
bool semantic_update_init_state(SemanticContext* context, const char* name,
                                InitState new_state);

/**
 * @brief Get the current initialization state of a variable.
 * @param context Semantic context.
 * @param name Symbol name.
 * @return InitState value, or INIT_UNINITIALIZED if not found.
 */
InitState semantic_get_init_state(SemanticContext* context, const char* name);

/**
 * @brief Check whether a variable can be modified.
 * @param context Semantic context.
 * @param name Symbol name.
 * @return true if variable is mutable and not constant, false otherwise.
 */
bool semantic_can_modify_symbol(SemanticContext* context, const char* name);

/* -------------------------------------------------------------------------
   Type checking
   ------------------------------------------------------------------------- */

/**
 * @brief Perform type checking on an AST node.
 * @param context Semantic context.
 * @param node AST node to check.
 * @return TypeCheckResult containing type, init state, and possible error.
 */
TypeCheckResult semantic_check_type(SemanticContext* context, ASTNode* node);

/**
 * @brief Specialized type check for binary operations.
 * @param context Semantic context.
 * @param node AST_BINARY_OPERATION node.
 * @return TypeCheckResult.
 */
TypeCheckResult semantic_check_binary_op(SemanticContext* context,
                                         ASTNode* node);

/**
 * @brief Specialized type check for unary operations.
 * @param context Semantic context.
 * @param node AST_UNARY_OPERATION node.
 * @return TypeCheckResult.
 */
TypeCheckResult semantic_check_unary_op(SemanticContext* context,
                                        ASTNode* node);

/**
 * @brief Specialized type check for assignment operations.
 * @param context Semantic context.
 * @param node AST_ASSIGNMENT or AST_COMPOUND_ASSIGNMENT node.
 * @return TypeCheckResult.
 */
TypeCheckResult semantic_check_assignment(SemanticContext* context,
                                          ASTNode* node);

/**
 * @brief Specialized type check for function calls.
 * @param context Semantic context.
 * @param node AST_FUNCTION_DECLARATION node used as call.
 * @return TypeCheckResult.
 */
TypeCheckResult semantic_check_function_call(SemanticContext* context,
                                             ASTNode* node);

/* -------------------------------------------------------------------------
   AST traversal and analysis
   ------------------------------------------------------------------------- */

/**
 * @brief Perform full semantic analysis on the entire AST.
 * @param context Semantic context.
 * @param ast Abstract syntax tree.
 * @return true if no errors, false otherwise.
 */
bool semantic_analyze(SemanticContext* context, AST* ast);

/**
 * @brief Check semantic correctness of a single statement node.
 * @param context Semantic context.
 * @param node Statement AST node.
 * @return true if valid, false otherwise (error reported).
 */
bool semantic_check_statement(SemanticContext* context, ASTNode* node);

/**
 * @brief Check semantic correctness of a single expression node.
 * @param context Semantic context.
 * @param node Expression AST node.
 * @return true if valid, false otherwise (error reported).
 */
bool semantic_check_expression(SemanticContext* context, ASTNode* node);

/**
 * @brief Validate a struct definition and add it to symbol table.
 * @param context Semantic context.
 * @param node AST_COMPOUND_TYPE node.
 * @return true if valid, false otherwise.
 */
bool semantic_check_compound_type(SemanticContext* context, ASTNode* node);

/**
 * @brief Check that all variables in a scope are properly initialized.
 * @param context Semantic context.
 * @param scope Scope to check (NULL = current scope).
 * @return true if all initialized, false otherwise (warnings emitted).
 */
bool semantic_check_scope_initialization(SemanticContext* context,
                                         SymbolTable* scope);

/* -------------------------------------------------------------------------
   Utility functions
   ------------------------------------------------------------------------- */

/**
 * @brief Validate that a struct member has a legal state modifier.
 * @param state_modifier Modifier string.
 * @return true if modifier is "var" or "obj".
 */
bool semantic_is_valid_struct_member_modifier(const char* state_modifier);

/**
 * @brief Convert a token type to the corresponding DataType.
 * @param token_type Token type from lexer.
 * @return DataType value.
 */
DataType semantic_type_from_token(TokenType token_type);

/**
 * @brief Convert a type name string to DataType.
 * @param type_name Type name.
 * @return DataType value.
 */
DataType semantic_type_from_string(const char* type_name);

/**
 * @brief Convert DataType to a human-readable string.
 * @param type DataType.
 * @return Constant string representation.
 */
const char* semantic_type_to_string(DataType type);

/**
 * @brief Convert InitState to a human-readable string.
 * @param state Initialization state.
 * @return Constant string representation.
 */
const char* semantic_init_state_to_string(InitState state);

/**
 * @brief Check if two types are compatible for arithmetic/logical operations.
 * @param type1 First type.
 * @param type2 Second type.
 * @return true if compatible.
 */
bool semantic_types_compatible(DataType type1, DataType type2);

/**
 * @brief Check if a source type can be assigned to a target type,
 *        considering initialization states.
 * @param target_type Type of the left-hand side.
 * @param source_type Type of the right-hand side.
 * @param target_init Initialization state of target.
 * @param source_init Initialization state of source.
 * @return true if assignment is allowed.
 */
bool semantic_types_assignable_ex(DataType target_type, DataType source_type,
                                  InitState target_init, InitState source_init);

/**
 * @brief Simplified version of assignability check (ignores init states).
 * @param target_type Target type.
 * @param source_type Source type.
 * @return true if types are compatible.
 */
bool semantic_types_assignable(DataType target_type, DataType source_type);

/**
 * @brief Get the total number of symbols in the global scope.
 * @param context Semantic context.
 * @return Symbol count.
 */
size_t semantic_get_symbol_count(SemanticContext* context);

/**
 * @brief Get the global symbol table for inspection (e.g., for debug output).
 * @param context Semantic context.
 * @return Global symbol table.
 */
SymbolTable* semantic_get_global_table(SemanticContext* context);

/**
 * @brief Check whether any semantic error has been reported.
 * @param context Semantic context.
 * @return true if errors exist.
 */
bool semantic_has_errors(SemanticContext* context);

/**
 * @brief Check whether warnings are enabled.
 * @param context Semantic context.
 * @return true if warnings are enabled.
 */
bool semantic_warnings_enabled(SemanticContext* context);

/**
 * @brief Determine if a block of statements ends with a return statement.
 * @param context Semantic context.
 * @param block_ast AST of the block.
 * @return true if the last statement is a return or all branches return.
 */
bool semantic_check_block_ends_with_return(SemanticContext* context, AST* block_ast);

/**
 * @brief Determine if a statement guarantees a return (for non-void functions).
 * @param context Semantic context.
 * @param node Statement node.
 * @return true if statement ensures return.
 */
bool semantic_statement_ensures_return(SemanticContext* context, ASTNode* node);

/**
 * @brief Emit a warning if a newly declared variable shadows an outer one.
 * @param context Semantic context.
 * @param name Variable name.
 * @param line Declaration line.
 * @param column Declaration column.
 */
void semantic_check_shadowing(SemanticContext* context, const char* name,
                              uint16_t line, uint16_t column);

/**
 * @brief Validate that a variable can be mutated (assigned to).
 * @param context Semantic context.
 * @param name Variable name.
 * @param line Mutation line (for error reporting).
 * @param column Mutation column.
 * @return true if mutation is allowed.
 */
bool semantic_validate_mutation(SemanticContext* context, const char* name,
                                uint16_t line, uint16_t column);

/**
 * @brief Validate field access via the '->' operator.
 * @param context Semantic context.
 * @param node AST_FIELD_ACCESS node.
 * @return true if the access is semantically valid.
 */
bool semantic_check_field_access(SemanticContext* context, ASTNode* node);

#endif
