#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../parser/parser.h"
#include "../lexer/lexer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Forward declaration for the symbol table structure.
 * The full definition is provided in the implementation file.
 */
typedef struct SymbolTable SymbolTable;

/*
 * DataType enumerates the fundamental data categories used during semantic
 * analysis. It provides a coarse classification of the type of a value or
 * expression. More detailed information (e.g., pointer depth, compound type
 * identity) is stored in the associated `Type*` structure.
 */
typedef enum {
    TYPE_UNKNOWN,      /* Unknown or invalid type (error state) */
    TYPE_INT,          /* Signed integer type (Int) */
    TYPE_REAL,         /* Floating‑point type (Real) */
    TYPE_CHAR,         /* Character type (Char) */
    TYPE_VOID,         /* Void type (no value) */
    TYPE_POINTER,      /* Pointer to another type */
    TYPE_REFERENCE,    /* Reference (alias) to another type */
    TYPE_ARRAY,        /* Contiguous array of elements */
    TYPE_FUNCTION,     /* Function type (callable) */
    TYPE_COMPOUND,     /* User‑defined compound type (struct or class) */
    TYPE_LABEL         /* Label type (jump target) */
} DataType;

/*
 * ScopeLevel defines the nesting kind of a scope. It is used to enforce
 * visibility rules and to validate statements such as break/continue that
 * are only allowed inside loops.
 */
typedef enum {
    SCOPE_GLOBAL,      /* Global (file) scope */
    SCOPE_FUNCTION,    /* Function body scope */
    SCOPE_BLOCK,       /* Generic block scope (if, while, do, etc.) */
    SCOPE_LOOP,        /* Loop scope (allows break/continue) */
    SCOPE_COMPOUND     /* Compound type member scope (struct/class) */
} ScopeLevel;

/*
 * InitState tracks the initialization status of a variable. It is used to
 * detect uses of uninitialised values and to enforce constant propagation
 * where required.
 */
typedef enum {
    INIT_UNINITIALIZED,    /* Declared but never assigned a value */
    INIT_PARTIAL,          /* Partially initialized (e.g., some array elements) */
    INIT_FULL,             /* Fully initialized (all members/elements set) */
    INIT_CONSTANT,         /* Constant value (always initialized) */
    INIT_DEFAULT           /* Default‑initialized (zero value) */
} InitState;

/*
 * FunctionParam represents a single parameter of a function declaration.
 * It stores the parameter name, its type, and an optional default value
 * expression (AST node).
 */
typedef struct FunctionParam {
    char* name;                    /* Parameter name (may be empty for prototypes) */
    DataType type;                 /* Primary type category */
    Type* type_info;               /* Detailed type information from the AST */
    ASTNode* default_value;        /* Default argument expression, may be NULL */
    struct FunctionParam* next;    /* Next parameter in the linked list */
} FunctionParam;

/*
 * FunctionSignature stores the complete signature of a function. It is
 * used to compare declarations and to verify call compatibility.
 */
typedef struct FunctionSignature {
    DataType return_type;          /* Return type category */
    Type* return_type_info;        /* Detailed return type information */
    FunctionParam* params;         /* Linked list of parameters */
    size_t param_count;            /* Total number of parameters */
    size_t required_param_count;   /* Number of parameters without default values */
    bool is_variadic;              /* Whether the function accepts variable arguments */
    bool has_body;                 /* Whether a function body has been defined */
} FunctionSignature;

/*
 * CompoundMember is a debug/output representation of a member of a struct or
 * class. The actual member symbols reside in the compound_scope of the
 * compound type entry. This list is maintained only for printing purposes.
 */
typedef struct CompoundMember {
    char* name;                    /* Member name */
    char* state_modifier;          /* Modifier (var, obj, func, struct, class) */
    char* access_modifier;         /* Access modifier (public/private) */
    DataType type;                 /* Member type category */
    Type* type_info;               /* Detailed type information */
    InitState init_state;          /* Initialization state */
    struct CompoundMember* next;   /* Next member in the list */
} CompoundMember;

/*
 * SymbolEntry represents a declared identifier in the symbol table.
 * Each entry corresponds to a variable, function, label, or compound type.
 * The structure is designed to support nested scopes, inheritance, and
 * detailed type information.
 */
typedef struct SymbolEntry {
    char* name;                    /* Identifier name */
    char* state_modifier;          /* Modifier (var, obj, const, func, struct, class) */
    char* access_modifier;         /* Access modifier (public/private) */
    DataType type;                 /* Primary data type */
    Type* type_info;               /* Detailed type information from the AST */
    bool is_constant;              /* Whether the symbol is immutable */
    InitState init_state;          /* Current initialization state */
    bool is_used;                  /* Whether the symbol has been referenced */
    bool is_mutable;               /* Whether the symbol can be modified */
    ScopeLevel declared_scope;     /* Scope level where the symbol was declared */
    uint16_t line;                 /* Line number of declaration */
    uint16_t column;               /* Column number of declaration */
    uint8_t compound_kind;         /* 0 for struct, 1 for class (only for TYPE_COMPOUND) */
    union {
        FunctionSignature* func_sig;            /* Function signature (if type == TYPE_FUNCTION) */
        CompoundMember* compound_members;       /* Debug list of members (if type == TYPE_COMPOUND) */
    } extra;                        /* Additional symbol‑specific data */
    SymbolTable* compound_scope;    /* Scope containing members (if type == TYPE_COMPOUND) */
    struct SymbolEntry* base_class; /* Pointer to base class entry (for classes) */
    struct SymbolEntry* next;       /* Next entry in hash collision chain */
} SymbolEntry;

/*
 * SymbolTable is a hash table that manages symbols within a single scope.
 * Scopes are arranged in a tree structure (parent‑child relationships)
 * to model lexical nesting.
 */
struct SymbolTable {
    SymbolEntry** entries;         /* Hash table buckets */
    size_t capacity;               /* Number of buckets */
    size_t count;                  /* Number of symbols in this table */
    ScopeLevel level;              /* Scope level of this table */
    struct SymbolTable* parent;    /* Enclosing scope (NULL for global) */
    struct SymbolTable* children;  /* Linked list of child scopes */
    struct SymbolTable* next_child;/* Next sibling in parent's children list */
};

/*
 * SemanticContext holds the global state of the semantic analysis phase.
 * It tracks the current scope, function context, and error reporting flags.
 */
typedef struct SemanticContext {
    SymbolTable* current_scope;    /* Currently active scope */
    SymbolTable* global_scope;     /* Root global scope */
    bool has_errors;               /* Set to true if any error occurred */
    bool warnings_enabled;         /* Whether warning diagnostics are enabled */
    bool exit_on_error;            /* Whether to stop compilation on first error */
    bool in_loop;                  /* True if inside a loop construct */
    bool in_function;              /* True if inside a function body */
    const char* current_function;  /* Name of the current function (if any) */
    DataType current_return_type;  /* Return type of the current function */
} SemanticContext;

/*
 * TypeCheckResult is returned by type checking functions. It contains the
 * inferred type, validity flag, and an optional error message.
 */
typedef struct TypeCheckResult {
    bool valid;                    /* Whether the type check succeeded */
    DataType type;                 /* Inferred or checked type */
    Type* type_info;               /* Detailed type information */
    InitState init_state;          /* Initialization state of the expression */
    char* error_msg;               /* Dynamically allocated error message */
} TypeCheckResult;

/*
 * VisibilityResult is returned by symbol visibility checks.
 */
typedef struct VisibilityResult {
    bool visible;                  /* Whether the symbol is visible */
    SymbolEntry* entry;            /* Pointer to the found symbol entry */
    ScopeLevel found_in_scope;     /* Scope level where the symbol was found */
    char* error_msg;               /* Dynamically allocated error message */
} VisibilityResult;

/*
 * Create a new semantic analysis context.
 * Returns a pointer to the context, or NULL on allocation failure.
 */
SemanticContext* semantic__create_context(void);

/*
 * Destroy a semantic context and all associated symbol tables.
 */
void semantic__destroy_context(SemanticContext* context);

/*
 * Set whether semantic errors should cause immediate termination.
 */
void semantic__set_exit_on_error(SemanticContext* context, bool exit_on_error);

/*
 * Enter a new scope with the specified level.
 */
void semantic__enter_scope_ex(SemanticContext* context, ScopeLevel level);

/*
 * Enter a generic block scope (SCOPE_BLOCK).
 */
void semantic__enter_scope(SemanticContext* context);

/*
 * Exit the current scope, restoring the parent scope.
 */
void semantic__exit_scope(SemanticContext* context);

/*
 * Enter a function scope. Sets the function name and return type.
 */
void semantic__enter_function_scope(SemanticContext* context,
                                   const char* function_name,
                                   DataType return_type);

/*
 * Exit a function scope.
 */
void semantic__exit_function_scope(SemanticContext* context);

/*
 * Enter a loop scope (SCOPE_LOOP). Used for break/continue validation.
 */
void semantic__enter_loop_scope(SemanticContext* context);

/*
 * Exit a loop scope.
 */
void semantic__exit_loop_scope(SemanticContext* context);

/*
 * Add a variable symbol to the specified scope.
 * Returns true on success, false on error (e.g., redeclaration).
 */
bool semantic__add_variable_ex(SemanticContext* context, SymbolTable* target_scope,
                              const char* name, DataType type, Type* type_info,
                              bool is_constant, const char* state_modifier,
                              InitState init_state, uint16_t line, uint16_t column,
                              const char* access_modifier);

/*
 * Simplified variable addition using current scope and default values.
 */
bool semantic__add_variable(SemanticContext* context, const char* name,
                           DataType type, Type* type_info, bool is_constant,
                           uint16_t line, uint16_t column);

/*
 * Add a function symbol to the specified scope.
 * Handles prototype/definition merging if signatures match.
 * Returns true on success, false on error.
 */
bool semantic__add_function_ex(SemanticContext* context, SymbolTable* target_scope,
                              const char* name,
                              DataType return_type, Type* return_type_info,
                              FunctionParam* params, size_t param_count,
                              size_t required_count, bool is_variadic,
                              uint16_t line, uint16_t column,
                              const char* access_modifier,
                              bool has_body);

/*
 * Simplified function addition (prototype only, no body, not variadic).
 */
bool semantic__add_function(SemanticContext* context, const char* name,
                           DataType return_type, Type* return_type_info,
                           FunctionParam* params, size_t param_count,
                           size_t required_count, uint16_t line, uint16_t column);

/*
 * Add a compound type (struct or class) to the current scope.
 * The members are processed and added to a dedicated compound scope.
 * Returns true on success, false on error.
 */
bool semantic__add_compound_type(SemanticContext* context, const char* name,
                                ASTNode* members_ast, ASTNode* base_class_ast,
                                uint16_t line, uint16_t column,
                                uint8_t kind);

/*
 * Find a symbol by name, searching from the current scope outward.
 * Returns the first matching SymbolEntry, or NULL if not found.
 */
SymbolEntry* semantic__find_symbol(SemanticContext* context, const char* name);

/*
 * Find a member of a struct/class by name.
 */
SymbolEntry* semantic__find_struct_member(SemanticContext* context,
                                         const char* struct_name,
                                         const char* field_name);

/*
 * Check the visibility of a symbol from the current scope.
 * Optionally requires that the symbol be initialized.
 */
VisibilityResult semantic__check_visibility(SemanticContext* context,
                                           const char* name,
                                           bool require_initialized,
                                           bool allow_shadowing);

/*
 * Mark a symbol as used (referenced).
 */
bool semantic__mark_symbol_used(SemanticContext* context, const char* name);

/*
 * Update the initialization state of a variable.
 */
bool semantic__update_init_state(SemanticContext* context, const char* name,
                                InitState new_state);

/*
 * Retrieve the initialization state of a symbol.
 */
InitState semantic__get_init_state(SemanticContext* context, const char* name);

/*
 * Determine whether a symbol can be modified.
 */
bool semantic__can_modify_symbol(SemanticContext* context, const char* name);

/*
 * Validate that a mutation (assignment) to a symbol is allowed.
 * Reports an error if the symbol is constant.
 */
bool semantic__validate_mutation(SemanticContext* context, const char* name,
                                uint16_t line, uint16_t column);

/*
 * Check field access (e.g., obj.field) for type correctness.
 */
bool semantic__check_field_access(SemanticContext* context, ASTNode* node);

/*
 * Main type checking routine. Determines the type, validity, and
 * initialization state of an AST node.
 */
TypeCheckResult semantic__check_type(SemanticContext* context, ASTNode* node);

/*
 * Specialised type checks for specific node categories.
 */
TypeCheckResult semantic__check_binary_op(SemanticContext* context, ASTNode* node);
TypeCheckResult semantic__check_unary_op(SemanticContext* context, ASTNode* node);
TypeCheckResult semantic__check_assignment(SemanticContext* context, ASTNode* node);
TypeCheckResult semantic__check_function_call(SemanticContext* context, ASTNode* node);

/*
 * Check an expression and report errors.
 */
bool semantic__check_expression(SemanticContext* context, ASTNode* node);

/*
 * Process a statement node, performing all necessary semantic checks.
 */
bool semantic__check_statement(SemanticContext* context, ASTNode* node);

/*
 * Check a compound type definition (struct or class).
 */
bool semantic__check_compound_type(SemanticContext* context, ASTNode* node);

/*
 * Convert a token type to a DataType.
 */
DataType semantic__type_from_token(TokenType token_type);

/*
 * Convert a type name string to a DataType.
 */
DataType semantic__type_from_string(const char* type_name);

/*
 * Convert a DataType to its string representation.
 */
const char* semantic__type_to_string(DataType type);

/*
 * Convert an InitState to a readable string.
 */
const char* semantic__init_state_to_string(InitState state);

/*
 * Check if two data types are compatible (e.g., for binary operations).
 */
bool semantic__types_compatible(DataType type1, DataType type2);

/*
 * Check if a source type can be assigned to a target type.
 */
bool semantic__types_assignable_ex(DataType target_type, DataType source_type,
                                  InitState target_init, InitState source_init);
bool semantic__types_assignable(DataType target_type, DataType source_type);

/*
 * Validate a struct member modifier.
 */
bool semantic__is_valid_struct_member_modifier(const char* state_modifier);

/*
 * Main entry point for semantic analysis.
 * Walks the AST and performs all checks.
 * Returns true if no errors were detected, false otherwise.
 */
bool semantic__analyze(SemanticContext* context, AST* ast);

/*
 * Check that all variables in a scope are initialized (used for final pass).
 */
bool semantic__check_scope_initialization(SemanticContext* context, SymbolTable* scope);

/*
 * Check if a block ends with a return statement.
 */
bool semantic__check_block_ends_with_return(SemanticContext* context, AST* block_ast);

/*
 * Determine whether a statement guarantees a return.
 */
bool semantic__statement_ensures_return(SemanticContext* context, ASTNode* node);

/*
 * Issue a warning if a declaration shadows an outer symbol.
 */
void semantic__check_shadowing(SemanticContext* context, const char* name,
                              uint16_t line, uint16_t column);

/*
 * Get the total number of symbols in the global scope.
 */
size_t semantic__get_symbol_count(SemanticContext* context);

/*
 * Get the global symbol table.
 */
SymbolTable* semantic__get_global_table(SemanticContext* context);

/*
 * Check if any errors were recorded.
 */
bool semantic__has_errors(SemanticContext* context);

/*
 * Check if warnings are enabled.
 */
bool semantic__warnings_enabled(SemanticContext* context);

#endif
