#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../parser/parser.h"
#include "../lexer/lexer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Opaque symbol table structure. */
typedef struct SymbolTable SymbolTable;

/*
 * Coarse data categories used by the semantic analyser.
 *
 * TYPE_AUTO is a placeholder that forces the analyser to deduce the
 * concrete type from an initialiser.
 *
 * TYPE_LABEL is reserved for label symbols (jump targets).  It is never
 * used as a value type.
 */
typedef enum {
    TYPE_UNKNOWN,    /* Type could not be determined                      */
    TYPE_INT,        /* Signed integer                                    */
    TYPE_REAL,       /* Floating point number                             */
    TYPE_CHAR,       /* Character literal                                 */
    TYPE_VOID,       /* Void – no value                                   */
    TYPE_AUTO,       /* Type must be inferred from initialiser            */
    TYPE_POINTER,    /* Pointer to another type                           */
    TYPE_ARRAY,      /* Array of homogeneous elements                     */
    TYPE_FUNCTION,   /* Function                                          */
    TYPE_COMPOUND,   /* Struct/record – user defined compound type        */
    TYPE_LABEL       /* Label (jump target), not a regular value type     */
} DataType;

/* Nesting levels of a scope. */
typedef enum {
    SCOPE_GLOBAL,    /* Global scope – outermost                          */
    SCOPE_FUNCTION,  /* Function body scope                               */
    SCOPE_BLOCK,     /* General block scope { ... }                       */
    SCOPE_LOOP,      /* Loop body scope (enables break/continue checks)   */
    SCOPE_COMPOUND   /* Scope introduced by a struct definition           */
} ScopeLevel;

/* Describes the initialisation state of a variable. */
typedef enum {
    INIT_UNINITIALIZED,  /* No initialiser, value undefined               */
    INIT_PARTIAL,        /* Some members initialised (struct only)        */
    INIT_FULL,           /* Fully initialised                             */
    INIT_CONSTANT,       /* Compile‑time constant expression              */
    INIT_DEFAULT         /* Default initialisation (zeroing)              */
} InitState;

/* Descriptor for a single function parameter. */
typedef struct FunctionParam {
    char *name;                 /* Parameter name (may be NULL for unnamed)*/
    DataType type;              /* Resolved coarse type                     */
    Type *type_info;            /* Full type annotation from the AST        */
    ASTNode *default_value;     /* Default argument expression, if any      */
    struct FunctionParam *next; /* Next parameter in the linked list        */
} FunctionParam;

/* Complete function signature stored in the symbol table. */
typedef struct FunctionSignature {
    DataType return_type;            /* Coarse return type                     */
    Type *return_type_info;          /* Original type annotation, or NULL     */
    FunctionParam *params;           /* Linked list of parameters             */
    size_t param_count;              /* Total number of parameters            */
    size_t required_param_count;     /* Parameters without a default value    */
    bool is_variadic;                /* Accepts extra arguments after the fixed ones */
    bool has_body;                   /* True if a function body has been supplied*/
    bool is_none_body;               /* True if the body is the 'none' literal*/
} FunctionSignature;

/* Descriptor for a member of a compound type (struct). */
typedef struct CompoundMember {
    char *name;                     /* Member name                              */
    char *access_modifier;          /* Visibility / storage modifiers (owned)  */
    DataType type;                  /* Coarse type                              */
    Type *type_info;                /* Full type descriptor from the AST       */
    InitState init_state;           /* Initialisation status (for constants)   */
    struct CompoundMember *next;    /* Next member in the linked list          */
} CompoundMember;

/* A single entry in a symbol table. */
typedef struct SymbolEntry {
    char *name;                     /* The identifier string                    */
    char *access_modifier;          /* Modifier list string (owned)            */
    DataType type;                  /* Coarse data type                         */
    Type *type_info;                /* Full type descriptor from the AST       */
    bool is_constant;               /* True if declared with 'const'           */
    InitState init_state;           /* Initialisation status                   */
    bool is_used;                   /* Toggled when the symbol is read         */
    bool is_mutable;                /* False for constants (symbol cannot be reassigned) */
    ScopeLevel declared_scope;      /* Scope where the symbol was introduced   */
    uint16_t line;                  /* Source line of the declaration          */
    uint16_t column;                /* Source column                           */
    uint8_t compound_kind;          /* Reserved for future use                 */
    union {
        FunctionSignature *func_sig;            /* Valid if TYPE_FUNCTION      */
        CompoundMember *compound_members;       /* Members if this entry is a struct type */
    } extra;                        /* Type‑specific metadata                  */
    SymbolTable *compound_scope;    /* Nested scope for compound type member lookup */
    struct SymbolEntry *base_class; /* Reserved for inheritance                */
    struct SymbolEntry *next;       /* Next entry in the hash bucket chain     */

    /*
     * Flags used by the optimizer.
     * is_inline is set for functions marked with the 'inline' modifier.
     * is_fixed is set for variables declared with the 'fixed' modifier.
     */
    bool is_inline;
    bool is_fixed;
} SymbolEntry;

/*
 * Symbol table implemented as a chained hash table.
 */
struct SymbolTable {
    SymbolEntry **entries;          /* Hash buckets                            */
    size_t capacity;                /* Number of buckets (power of two)        */
    size_t count;                   /* Number of symbols currently stored      */
    ScopeLevel level;               /* Nesting level of this table             */
    struct SymbolTable *parent;     /* Enclosing scope                         */
    struct SymbolTable *children;   /* First child scope (linked list)         */
    struct SymbolTable *next_child; /* Next child among siblings               */
};

/*
 * Semantic analysis context.
 *
 * The 'strict_type_check' flag, when true, causes the analyser to reject
 * any implicit type conversion. It is intended to be enabled by the -Wextra
 * command‑line flag.
 */
typedef struct SemanticContext {
    SymbolTable *current_scope;     /* Innermost active scope                  */
    SymbolTable *global_scope;      /* Root (global) scope, never popped       */
    bool has_errors;                /* Set to true if any semantic error occurs*/
    bool warnings_enabled;          /* Controls emission of semantic warnings  */
    bool exit_on_error;             /* If true, analysis stops on first error  */
    bool in_loop;                   /* True while inside a loop body           */
    bool in_function;               /* True while inside a function body       */
    const char *current_function;   /* Name of the function being analysed, or NULL */
    DataType current_return_type;   /* Return type of the current function     */
    bool strict_type_check;         /* Reject implicit numeric conversions if true */
    bool extra_warnings;            /* Enable extra warnings (e.g. unused variables) */
    bool abort_compilation;         /* Set when compilation cannot continue after analysis */
} SemanticContext;

/* Result of type‑checking an expression. */
typedef struct TypeCheckResult {
    bool valid;                     /* True if no errors were found            */
    DataType type;                  /* Deduced coarse type                     */
    Type *type_info;                /* Full type descriptor, if available      */
    InitState init_state;           /* Initialisation state of the expression  */
    char *error_msg;                /* Owned diagnostic message; caller must free */
} TypeCheckResult;

/* Public API – lifecycle */
SemanticContext *semantic__create_context(void);
void             semantic__destroy_context(SemanticContext *ctx);

/* Scope management */
void semantic__enter_scope_ex(SemanticContext *ctx, ScopeLevel level);
void semantic__enter_scope(SemanticContext *ctx);
void semantic__exit_scope(SemanticContext *ctx);
void semantic__enter_function_scope(SemanticContext *ctx, const char *name, DataType ret);
void semantic__exit_function_scope(SemanticContext *ctx);
void semantic__enter_loop_scope(SemanticContext *ctx);
void semantic__exit_loop_scope(SemanticContext *ctx);

/* Symbol addition */
bool semantic__add_variable_ex(SemanticContext *ctx, SymbolTable *target_scope,
                               const char *name, DataType type, Type *type_info,
                               bool is_constant, InitState init_state,
                               uint16_t line, uint16_t column,
                               const char *access_modifier);
bool semantic__add_function_ex(SemanticContext *ctx, SymbolTable *target_scope,
                               const char *name,
                               DataType return_type, Type *return_type_info,
                               FunctionParam *params, size_t param_count,
                               size_t required_count, bool is_variadic,
                               uint16_t line, uint16_t column,
                               const char *access_modifier,
                               bool has_body, bool is_none_body);

/* Symbol lookup – traverses scope chain */
SymbolEntry *semantic__find_symbol(SemanticContext *ctx, const char *name);

/* Type checking of expressions and statements */
bool semantic__check_expression(SemanticContext *ctx, ASTNode *node);
bool semantic__check_statement(SemanticContext *ctx, ASTNode *node);
TypeCheckResult semantic__check_type(SemanticContext *ctx, ASTNode *node);

/* Main entry point for the analysis pass */
bool semantic__analyze(SemanticContext *ctx, AST *ast);

/* Configuration */
void semantic__set_extra_warnings(SemanticContext *ctx, bool enable);

/* Query whether compilation must be aborted after semantic analysis */
bool semantic__should_abort(const SemanticContext *ctx);

/* Utility functions */
const char *semantic__type_to_string(DataType type);
size_t      semantic__get_symbol_count(SemanticContext *ctx);
bool        semantic__has_errors(SemanticContext *ctx);

#endif
