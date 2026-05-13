#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../parser/parser.h"
#include "../lexer/lexer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Opaque handle – the complete struct is defined inside semantic.c. */
typedef struct SymbolTable SymbolTable;

/* Coarse type categories used for quick classification during semantic analysis. */
typedef enum {
    TYPE_UNKNOWN,       /* Type could not be determined                      */
    TYPE_INT,           /* Signed integer                                    */
    TYPE_REAL,          /* Floating point number                             */
    TYPE_CHAR,          /* Character literal                                 */
    TYPE_VOID,          /* Void – no value                                   */
    TYPE_AUTO,          /* Must be inferred from an initialiser              */
    TYPE_POINTER,       /* Pointer to another type                           */
    TYPE_ARRAY,         /* Homogeneous array                                 */
    TYPE_FUNCTION,      /* Function                                          */
    TYPE_COMPOUND,      /* Struct – user‑defined compound type               */
    TYPE_UNION,         /* Union – user‑defined union type                   */
    TYPE_LABEL          /* Label (jump target), not a regular value type     */
} DataType;

/* Nesting level of a scope, used for visibility and control‑flow checks. */
typedef enum {
    SCOPE_GLOBAL,       /* Outermost scope                                   */
    SCOPE_FUNCTION,     /* Function body                                     */
    SCOPE_BLOCK,        /* General block { ... }                             */
    SCOPE_LOOP,         /* Loop body (enables break / continue checks)       */
    SCOPE_COMPOUND      /* Scope introduced by a struct / union definition   */
} ScopeLevel;

/* Initialisation state of a symbol. */
typedef enum {
    INIT_UNINITIALIZED, /* No initialiser, value undefined                   */
    INIT_PARTIAL,       /* Some members initialised (struct / union only)    */
    INIT_FULL,          /* Fully initialised                                 */
    INIT_CONSTANT,      /* Compile‑time constant expression                  */
    INIT_DEFAULT        /* Default initialisation (zeroing)                  */
} InitState;

/* Describes one member of a compound type (struct or union). */
typedef struct CompoundMember {
    char *name;                        /* Member name (NULL for anonymous)   */
    char *access_modifier;             /* Visibility / storage modifiers     */
    DataType type;                     /* Coarse type                        */
    Type *type_info;                   /* Full type descriptor (from AST)    */
    InitState init_state;              /* Initialisation status              */
    bool is_inline;                    /* True if 'inline' modifier present  */
    struct CompoundMember *next;       /* Next sibling in linked list        */
    struct CompoundMember *compound_members; /* Nested members for anonymous sub‑compound */
} CompoundMember;

/* Descriptor for one function parameter. */
typedef struct FunctionParam {
    char *name;                     /* Parameter name (may be NULL)          */
    DataType type;                  /* Resolved coarse type                  */
    ASTNode *default_value;         /* Default argument AST (NULL if none)   */
    struct FunctionParam *next;     /* Next parameter in the list            */
} FunctionParam;

/* Complete function signature stored inside the symbol table. */
typedef struct FunctionSignature {
    DataType return_type;            /* Coarse return type                   */
    Type *return_type_info;          /* Full return‑type annotation          */
    FunctionParam *params;           /* Linked list of parameters            */
    size_t param_count;              /* Total number of parameters           */
    size_t required_param_count;     /* Parameters without a default         */
    bool is_variadic;                /* Accepts extra arguments via '...'    */
    bool has_body;                   /* True after a body is supplied        */
    bool is_none_body;               /* True when the body is 'none'         */
} FunctionSignature;

/* A single entry in a symbol table – one name binding. */
typedef struct SymbolEntry {
    char *name;                     /* The identifier string                 */
    char *access_modifier;          /* Modifier list (owned copy)            */
    DataType type;                  /* Coarse data type                      */
    Type *type_info;                /* Full type descriptor (shallow, AST)   */
    bool is_constant;               /* True if declared with 'const'         */
    InitState init_state;           /* Current initialisation status         */
    bool is_used;                   /* Toggled once the symbol is read       */
    bool is_mutable;                /* False for constants                   */
    ScopeLevel declared_scope;      /* Scope where the symbol was introduced */
    uint16_t line;                  /* Declaration source line               */
    uint16_t column;                /* Declaration source column             */
    bool is_inline;                 /* True if 'inline' modifier present     */
    union {
        FunctionSignature *func_sig;           /* Valid when TYPE_FUNCTION   */
        CompoundMember *compound_members;      /* Members if struct / union  */
    } extra;
    struct SymbolEntry *next;       /* Next entry in the hash bucket chain   */
} SymbolEntry;

/* Symbol table – chained hash table that supports nested scopes. */
struct SymbolTable {
    SymbolEntry **entries;          /* Hash bucket array (capacity long)     */
    size_t capacity;                /* Number of buckets (power of two)      */
    size_t count;                   /* Current number of symbols             */
    ScopeLevel level;               /* Nesting level of this frame           */
    struct SymbolTable *parent;     /* Enclosing scope (NULL for global)     */
    struct SymbolTable *children;   /* First child scope (linked list)       */
    struct SymbolTable *next_child; /* Next sibling among children           */
};

/* Semantic analysis context – carries all mutable state during a pass. */
typedef struct SemanticContext {
    SymbolTable *current_scope;     /* Innermost active scope                */
    SymbolTable *global_scope;      /* Root (global) scope, never popped     */
    bool has_errors;                /* True if any semantic error occurred   */
    bool warnings_enabled;          /* Controls emission of warnings         */
    bool exit_on_error;             /* Reserved – currently unused           */
    bool strict_type_check;         /* Reject implicit numeric conversions   */
    bool extra_warnings;            /* Enable extra warnings (-Wextra)       */
    bool abort_compilation;         /* Set when compilation cannot continue  */
    bool in_loop;                   /* True while inside a loop body         */
    bool in_function;               /* True while inside a function body     */
    const char *current_function;   /* Name of the function being analysed   */
    DataType current_return_type;   /* Return type of the current function   */
} SemanticContext;

/* Result of type‑checking an expression node. */
typedef struct TypeCheckResult {
    bool valid;                     /* True if no errors were found          */
    DataType type;                  /* Deduced coarse type                   */
    Type *type_info;                /* Full type descriptor, if available    */
    InitState init_state;           /* Initialisation state of the expression*/
    char *error_msg;                /* Owned diagnostic; caller must free    */
} TypeCheckResult;

/* Context lifecycle. */
SemanticContext *semantic__create_context(void);
void             semantic__destroy_context(SemanticContext *ctx);

/* Scope management – enter / exit with explicit or automatic levels. */
void semantic__enter_scope_ex(SemanticContext *ctx, ScopeLevel level);
void semantic__enter_scope(SemanticContext *ctx);
void semantic__exit_scope(SemanticContext *ctx);
void semantic__enter_function_scope(SemanticContext *ctx, const char *name, DataType ret);
void semantic__exit_function_scope(SemanticContext *ctx);
void semantic__enter_loop_scope(SemanticContext *ctx);
void semantic__exit_loop_scope(SemanticContext *ctx);

/* Low‑level symbol insertion. */
bool semantic__add_variable_ex(SemanticContext *ctx, SymbolTable *target_scope,
                               const char *name, DataType type, Type *type_info,
                               bool is_constant, InitState init_state,
                               uint16_t line, uint16_t column,
                               const char *access_modifier);
bool semantic__add_function_ex(SemanticContext *ctx, SymbolTable *target_scope,
                               const char *name,
                               DataType return_type, Type *return_type_info,
                               FunctionParam *params,         /* ownership transferred */
                               size_t param_count,
                               size_t required_count, bool is_variadic,
                               uint16_t line, uint16_t column,
                               const char *access_modifier,
                               bool has_body, bool is_none_body);

/* Lookup a symbol from the current scope chain. */
SymbolEntry *semantic__find_symbol(SemanticContext *ctx, const char *name);

/* Entry points for checking individual nodes. */
bool semantic__check_expression(SemanticContext *ctx, ASTNode *node);
bool semantic__check_statement(SemanticContext *ctx, ASTNode *node);
TypeCheckResult semantic__check_type(SemanticContext *ctx, ASTNode *node);

/* Top‑level analysis – walks an entire AST and returns pass / fail. */
bool semantic__analyze(SemanticContext *ctx, AST *ast);

/* Utilities. */
void        semantic__set_extra_warnings(SemanticContext *ctx, bool enable);
bool        semantic__should_abort(const SemanticContext *ctx);
const char *semantic__type_to_string(DataType type);
size_t      semantic__get_symbol_count(SemanticContext *ctx);
bool        semantic__has_errors(const SemanticContext *ctx);

#endif
