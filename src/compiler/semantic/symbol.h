#ifndef SYMBOL_H
#define SYMBOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct Type;
struct Scope;
struct SemanticContext;
struct ASTNode;
struct FuncDecl;

/*
 * Symbol kinds.
 */
typedef enum {
    SYMBOL_VARIABLE,
    SYMBOL_CONSTANT,
    SYMBOL_FUNCTION,
    SYMBOL_PARAMETER,
    SYMBOL_TYPE,
    SYMBOL_MODULE,
    SYMBOL_ENUM_CONSTANT
} SymbolKind;

/*
 * Scope levels.
 */
typedef enum {
    SCOPE_GLOBAL,
    SCOPE_FUNCTION,
    SCOPE_BLOCK,
    SCOPE_LOOP,
    SCOPE_MODULE
} ScopeLevel;

/*
 * Parameter information for functions.
 */
typedef struct ParamInfo {
    char* name;
    struct Type* type;
    struct ParamInfo* next;
} ParamInfo;

/*
 * Field information for structs, unions, enums.
 */
typedef struct FieldInfo {
    char* name;
    struct Type* type;
    struct FieldInfo* next;
} FieldInfo;

/*
 * Method information for struct types.
 */
typedef struct MethodInfo {
    char* name;
    struct FuncDecl* decl;
    struct MethodInfo* next;
} MethodInfo;

/*
 * Function pointer for comptime evaluation.
 */
typedef int64_t (*ComptimeEvalFn)(struct SemanticContext* ctx,
                                  struct ASTNode** args,
                                  size_t arg_count);

/*
 * Extra data for a symbol, depending on its kind.
 */
typedef struct SymbolExtra {
    union {
        struct {
            ParamInfo* params;
            size_t param_count;
            struct Type* return_type;
            bool return_is_error;
            struct ASTNode* comptime_body;  /* comptime function body, or the FuncDecl of a template */
            bool defined;   /* false for a prototype awaiting its definition */
            bool is_template;   /* comptime-parameter template function */
        } func;
        FieldInfo* fields;
        struct Scope* module_scope;
        int64_t const_value;
        struct Symbol* enum_type;
    };
    MethodInfo* methods;    /* method table of a struct type */
} SymbolExtra;

/*
 * Symbol structure.
 */
typedef struct Symbol {
    char* name;
    struct Type* type;
    SymbolKind kind;
    uint32_t line;
    uint32_t column;
    bool is_public;
    bool is_extern;
    bool is_used;
    bool is_initialized;
    bool is_comptime_builtin;
    bool is_comptime;
    ComptimeEvalFn comptime_eval;
    SymbolExtra extra;
    struct Symbol* next;
} Symbol;

/*
 * Scope structure.
 */
typedef struct Scope {
    struct Scope* parent;
    ScopeLevel level;
    Symbol** table;
    size_t capacity;
    size_t count;
} Scope;

/* Scope management */
Scope* scope_create(Scope* parent, ScopeLevel level);
void scope_destroy(Scope* scope);
bool scope_insert(Scope* scope, Symbol* sym);
Symbol* scope_lookup(Scope* scope, const char* name);
Symbol* scope_lookup_local(Scope* scope, const char* name);
void scope_foreach(Scope* scope, void (*callback)(Symbol*, void*), void* userdata);

/* Symbol creation and destruction */
Symbol* symbol_create(const char* name, SymbolKind kind, struct Type* type,
                      uint32_t line, uint32_t column);
Symbol* symbol_copy_deep(Symbol* orig);
void symbol_free(Symbol* sym);

#endif
