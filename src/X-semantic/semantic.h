#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../parser/parser.h"
#include <stdbool.h>

typedef enum {
    OBJ_VARIABLE,
    OBJ_ARRAY,
    OBJ_FUNCTION,
    OBJ_STRUCT,
    OBJ_CLASS,
    OBJ_OBJECT
} ObjectKind;

typedef struct Symbol {
    char* name;
    ObjectKind kind;
    Type* type;
    bool is_mutable;
    ASTNode* ast_node;
    int scope_level;
    struct Symbol* next_in_scope;
    struct Symbol* next_in_bucket;
} Symbol;

typedef struct Scope {
    struct Scope* parent;
    Symbol** symbols;
    int bucket_count;
    int level;
    int symbol_count;
} Scope;

typedef struct {
    Scope* current_scope;
    Scope* global_scope;
    bool has_errors;
    int error_count;
} SemanticContext;

SemanticContext* semantic_create_context(void);
void semantic_destroy_context(SemanticContext* context);
void semantic_enter_scope(SemanticContext* context);
void semantic_exit_scope(SemanticContext* context);
Symbol* semantic_lookup_symbol(SemanticContext* context, const char* name);
Symbol* semantic_lookup_symbol_current_scope(SemanticContext* context, const char* name);
bool semantic_insert_symbol(SemanticContext* context, Symbol* symbol);
void semantic_analyze(AST* ast);
void semantic_report_error(SemanticContext* context, ASTNode* node, const char* format, ...);

bool validate_variable_declaration(SemanticContext* context, ASTNode* node);
bool validate_array_declaration(SemanticContext* context, ASTNode* node);
bool validate_function_declaration(SemanticContext* context, ASTNode* node);
bool validate_struct_declaration(SemanticContext* context, ASTNode* node);
bool validate_class_declaration(SemanticContext* context, ASTNode* node);
bool validate_object_declaration(SemanticContext* context, ASTNode* node);

bool are_types_compatible(Type* t1, Type* t2);
bool is_string_assignment_valid(Type* target_type, ASTNode* string_node);

ObjectKind get_object_kind_from_state(const char* state_modifier);
const char* get_type_name(Type* type);
bool is_builtin_type(const char* type_name);
Symbol* find_symbol_by_type_name(SemanticContext* context, const char* type_name);

#endif
