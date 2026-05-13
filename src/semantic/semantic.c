#include "semantic.h"
#include "../errhandler/errhandler.h"
#include "../utils/str_utils.h"
#include "../utils/memory_utils.h"
#include "../utils/common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* FNV‑1a hash for identifier strings. */
#define HASH_TABLE_SIZE 64
static uint32_t hash_string(const char *str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

/* Macros for AST navigation and common checks. */
#define HAS_LEFT(n)       ((n) && (n)->left)
#define HAS_RIGHT(n)      ((n) && (n)->right)
#define IS_NUMERIC(t)     ((t) == TYPE_INT || (t) == TYPE_REAL)
#define IS_CONDITION(t)   (IS_NUMERIC(t) || (t) == TYPE_POINTER)
#define STR_EQUAL(a,b)    ((a) && (b) && strcmp((a),(b)) == 0)
#define BREAK_IF_INVALID(res) if (!(res).valid) break
#define RETURN_IF_INVALID(res) if (!(res).valid) return false

/* Error/warning reporting. */
#define SEM_ERROR(ctx, code, line, col, len, ...)                              \
    do {                                                                       \
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, (code), (line), (col),  \
                                    (len), "semantic", __VA_ARGS__);          \
        (ctx)->has_errors = true;                                              \
        (ctx)->abort_compilation = true;                                       \
    } while(0)

#define SEM_WARNING(ctx, code, line, col, len, ...)                            \
    do {                                                                       \
        errhandler__report_error_ex(ERROR_LEVEL_WARNING, (code), (line),       \
                                    (col), (len), "semantic", __VA_ARGS__);   \
    } while(0)

/* Forward declarations of internal helpers. */
static SymbolTable *    create_symbol_table(SymbolTable *parent, ScopeLevel level);
static void             destroy_symbol_table(SymbolTable *table);
static SymbolEntry *    find_symbol_in_table(SymbolTable *table, const char *name);
static bool             add_symbol_to_table(SymbolTable *table, SymbolEntry *entry);

static DataType         string_to_datatype(const char *name);
static DataType         type_from_type_info(Type *type_info, SemanticContext *ctx);
static const char *     get_default_modifiers(DataType type);
static bool             check_name_and_warn(SemanticContext *ctx, const char *name,
                                            uint16_t line, uint16_t column);
static bool             validate_modifiers_for_type(SemanticContext *ctx,
                                                    const char *mods,
                                                    DataType type,
                                                    uint16_t line, uint16_t column,
                                                    const char *ident_name);
static bool             modifier_check_type_allowed(const char *mod_token, DataType type);

static void             free_function_param_list(FunctionParam *list);
static void             destroy_compound_member_list(CompoundMember *list);

static bool             can_implicitly_convert(SemanticContext *ctx, DataType from, DataType to);
static DataType         promote_numeric(DataType t1, DataType t2);
static bool             is_condition_true(const TypeCheckResult *cond);

static bool             signatures_equal(const FunctionSignature *a, const FunctionSignature *b);
static bool             check_prototype_match(SemanticContext *ctx,
                                              SymbolEntry *existing,
                                              const FunctionSignature *new_sig,
                                              uint16_t line, uint16_t column);
static bool             check_function_call_arguments(SemanticContext *ctx,
                                                      SymbolEntry *func,
                                                      AST *arg_list,
                                                      uint16_t line, uint16_t column);
static bool             check_function_call(SemanticContext *ctx, ASTNode *node,
                                            TypeCheckResult *out);

static bool             is_struct_type_definition(const SymbolEntry *entry);
static bool             is_incomplete_type(const SymbolEntry *entry);
static CompoundMember * get_compound_members(const SymbolEntry *entry, SemanticContext *ctx);
static CompoundMember * find_compound_member_from_list(CompoundMember *list,
                                                       const char *member_name);
static bool             resolve_member_access(SemanticContext *ctx, ASTNode *node,
                                              DataType *out_type, Type **out_type_info,
                                              SymbolEntry **out_struct_entry,
                                              CompoundMember **out_member);
static bool             check_struct_initializer(SemanticContext *ctx,
                                                 ASTNode *initializer_node,
                                                 SymbolEntry *compound_type_entry);
static CompoundMember * build_member_list_from_block(SemanticContext *ctx,
                                                     ASTNode *block_node,
                                                     DataType compound_kind);

static bool process_variable_declaration(SemanticContext *ctx, ASTNode *node,
                                         int kind); /* 0=auto, 1=def, 2=pro */
static bool process_function_declaration(SemanticContext *ctx, ASTNode *node);
static bool process_compound_definition(SemanticContext *ctx, ASTNode *node,
                                        DataType compound_kind, bool is_prototype);
static bool process_label_statement(SemanticContext *ctx, ASTNode *node);
static bool process_del_variable(SemanticContext *ctx, ASTNode *node);
static bool process_return_statement(SemanticContext *ctx, ASTNode *node);

static bool deduce_and_check_variable(SemanticContext *ctx, ASTNode *node,
                                      DataType *out_type,
                                      Type **out_type_info,
                                      InitState *out_init_state,
                                      const char **out_default_mods);

static bool final_verification(SemanticContext *ctx);

/* Helper: detect the literal "none". */
static bool is_none_literal(const ASTNode *node) {
    return node && node->type == AST_LITERAL_VALUE && node->operation_type == TOKEN_NONE;
}

/* Helper: check if a modifier string contains a specific modifier name. */
static bool has_modifier(const char *mods, const char *name) {
    return mods && name && strcmp(mods, name) == 0;
}

/* Helper: check if an expression is the literal "Void". */
static bool is_void_literal(const ASTNode *node) {
    return node && node->type == AST_LITERAL_VALUE &&
           node->operation_type == TOKEN_ID &&
           node->value && STR_EQUAL(node->value, "Void");
}

/* Creates a new symbol table (scope) with a given parent and nesting level. */
static SymbolTable *create_symbol_table(SymbolTable *parent, ScopeLevel level) {
    SymbolTable *table = calloc(1, sizeof(SymbolTable));
    if (!table) return NULL;
    table->entries = calloc(HASH_TABLE_SIZE, sizeof(SymbolEntry *));
    if (!table->entries) { free(table); return NULL; }
    table->capacity = HASH_TABLE_SIZE;
    table->level = level;
    table->parent = parent;
    if (parent) {
        table->next_child = parent->children;
        parent->children = table;
    }
    return table;
}

/* Recursively destroys a symbol table and all its nested scopes and entries. */
static void destroy_symbol_table(SymbolTable *table) {
    if (!table) return;
    SymbolTable *child = table->children;
    while (child) {
        SymbolTable *next = child->next_child;
        destroy_symbol_table(child);
        child = next;
    }
    for (size_t i = 0; i < table->capacity; i++) {
        SymbolEntry *entry = table->entries[i];
        while (entry) {
            SymbolEntry *next = entry->next;
            free(entry->name);
            free(entry->access_modifier);
            if (entry->type == TYPE_FUNCTION && entry->extra.func_sig) {
                free_function_param_list(entry->extra.func_sig->params);
                free(entry->extra.func_sig);
            } else if ((entry->type == TYPE_COMPOUND || entry->type == TYPE_UNION) &&
                       entry->extra.compound_members) {
                destroy_compound_member_list(entry->extra.compound_members);
            }
            free(entry);
            entry = next;
        }
    }
    free(table->entries);
    free(table);
}

/* Find a symbol by name inside a single table (no parent chain). */
static SymbolEntry *find_symbol_in_table(SymbolTable *table, const char *name) {
    if (!table || !name) return NULL;
    uint32_t idx = hash_string(name) % table->capacity;
    SymbolEntry *entry = table->entries[idx];
    while (entry) {
        if (strcmp(entry->name, name) == 0)
            return entry;
        entry = entry->next;
    }
    return NULL;
}

/* Insert a symbol entry into a specific table (no duplicate check). */
static bool add_symbol_to_table(SymbolTable *table, SymbolEntry *entry) {
    if (!table || !entry) return false;
    uint32_t idx = hash_string(entry->name) % table->capacity;
    entry->next = table->entries[idx];
    table->entries[idx] = entry;
    table->count++;
    return true;
}

/* Convert a type name string to the corresponding DataType enum. */
static DataType string_to_datatype(const char *name) {
    if (!name) return TYPE_UNKNOWN;
    if (STR_EQUAL(name, "Int"))    return TYPE_INT;
    if (STR_EQUAL(name, "Real"))   return TYPE_REAL;
    if (STR_EQUAL(name, "Char"))   return TYPE_CHAR;
    if (STR_EQUAL(name, "Void"))   return TYPE_VOID;
    if (STR_EQUAL(name, "Auto"))   return TYPE_AUTO;
    if (STR_EQUAL(name, "Struct")) return TYPE_COMPOUND;
    if (STR_EQUAL(name, "Union"))  return TYPE_UNION;
    return TYPE_UNKNOWN;
}

/* Determines whether a symbol entry represents a full compound type definition. */
static bool is_struct_type_definition(const SymbolEntry *entry) {
    if (!entry) return false;
    if (entry->type != TYPE_COMPOUND && entry->type != TYPE_UNION)
        return false;
    return entry->is_constant && entry->init_state == INIT_CONSTANT;
}

/* Checks if a compound type entry is incomplete (only a prototype, no members). */
static bool is_incomplete_type(const SymbolEntry *entry) {
    if (!entry) return false;
    if (!is_struct_type_definition(entry)) return false;
    return entry->extra.compound_members == NULL;
}

/* Retrieves the member list of a compound type, following type_info aliases if needed. */
static CompoundMember *get_compound_members(const SymbolEntry *entry, SemanticContext *ctx) {
    if (!entry) return NULL;
    if (entry->extra.compound_members)
        return entry->extra.compound_members;
    if (entry->type_info && entry->type_info->name) {
        SymbolEntry *type_def = semantic__find_symbol(ctx, entry->type_info->name);
        if (type_def && is_struct_type_definition(type_def)) {
            type_def->is_used = true;
            return type_def->extra.compound_members;
        }
    }
    return NULL;
}

/* Resolves the DataType from a full type descriptor (from AST) using the symbol table. */
static DataType type_from_type_info(Type *type_info, SemanticContext *ctx) {
    if (!type_info) return TYPE_UNKNOWN;
    if (type_info->pointer_level > 0) return TYPE_POINTER;
    if (type_info->is_array)          return TYPE_ARRAY;

    DataType builtin = string_to_datatype(type_info->name);
    if (builtin != TYPE_UNKNOWN)
        return builtin;

    SymbolEntry *entry = semantic__find_symbol(ctx, type_info->name);
    if (entry) {
        if ((entry->type == TYPE_COMPOUND || entry->type == TYPE_UNION) &&
            is_struct_type_definition(entry)) {
            entry->is_used = true;
            return entry->type;
        }
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, 0, 0, 0,
                  "'%s' is not a type; it is a %s",
                  type_info->name,
                  semantic__type_to_string(entry->type));
        return TYPE_UNKNOWN;
    }
    SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, 0, 0,
              (uint8_t)(type_info->name ? strlen(type_info->name) : 0),
              "Unknown type name '%s'", type_info->name);
    return TYPE_UNKNOWN;
}

/* Returns a default access modifier string for a given coarse type. */
static const char *get_default_modifiers(DataType type) {
    if (type == TYPE_INT || type == TYPE_REAL)
        return "open unsigned";
    return "open static";
}

/* Checks for redeclaration in the current scope and emits warnings for shadowing. */
static bool check_name_and_warn(SemanticContext *ctx, const char *name,
                                uint16_t line, uint16_t column) {
    if (find_symbol_in_table(ctx->current_scope, name)) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_REDECLARATION, line, column,
                  (uint8_t)strlen(name), "Redeclaration of '%s'", name);
        return true;
    }
    if (ctx->warnings_enabled) {
        SymbolTable *p = ctx->current_scope->parent;
        while (p) {
            if (find_symbol_in_table(p, name)) {
                SEM_WARNING(ctx, ERROR_CODE_SEM_REDECLARATION, line, column,
                            (uint8_t)strlen(name),
                            "'%s' shadows a previous declaration", name);
                break;
            }
            p = p->parent;
        }
    }
    return false;
}

/* Validates if a modifier token is allowed for a given type. */
static bool modifier_check_type_allowed(const char *mod_token, DataType type) {
    if (!mod_token) return true;
    if (STR_EQUAL(mod_token, "unsigned") || STR_EQUAL(mod_token, "signed"))
        return (type == TYPE_INT || type == TYPE_REAL || type == TYPE_CHAR ||
                type == TYPE_VOID || type == TYPE_AUTO);
    if (STR_EQUAL(mod_token, "const"))
        return (type == TYPE_INT || type == TYPE_REAL || type == TYPE_CHAR ||
                type == TYPE_VOID || type == TYPE_AUTO ||
                type == TYPE_COMPOUND || type == TYPE_UNION);
    if (STR_EQUAL(mod_token, "close"))
        return (type == TYPE_COMPOUND || type == TYPE_UNION);
    if (STR_EQUAL(mod_token, "inline"))
        return true;
    return true;
}

/* Checks a complete modifier list against a data type, reporting errors. */
static bool validate_modifiers_for_type(SemanticContext *ctx,
                                        const char *mods,
                                        DataType type,
                                        uint16_t line, uint16_t column,
                                        const char *ident_name) {
    if (!mods || !mods[0]) return true;
    bool ok = true;
    char *copy = u__strduplic(mods);
    if (!copy) return false;
    char *token = strtok(copy, " \t");
    while (token) {
        if (!modifier_check_type_allowed(token, type)) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, line, column,
                      (uint8_t)(ident_name ? strlen(ident_name) : 0),
                      "Modifier '%s' is not allowed for type '%s' on '%s'",
                      token, semantic__type_to_string(type),
                      ident_name ? ident_name : "(unnamed)");
            ok = false;
        }
        token = strtok(NULL, " \t");
    }
    free(copy);
    return ok;
}

/* Frees a linked list of function parameters. */
static void free_function_param_list(FunctionParam *list) {
    while (list) {
        FunctionParam *next = list->next;
        free(list->name);
        free(list);
        list = next;
    }
}

/* Recursively destroys a list of compound members and any nested members. */
static void destroy_compound_member_list(CompoundMember *list) {
    while (list) {
        CompoundMember *next = list->next;
        free(list->name);
        free(list->access_modifier);
        if (list->compound_members)
            destroy_compound_member_list(list->compound_members);
        free(list);
        list = next;
    }
}

/* Decides whether an implicit conversion from 'from' to 'to' is permitted. */
static bool can_implicitly_convert(SemanticContext *ctx, DataType from, DataType to) {
    if (from == TYPE_VOID) return true;
    if (from == to) return true;
    if (ctx->strict_type_check) return false;
    if ((from == TYPE_INT && to == TYPE_REAL) ||
        (from == TYPE_REAL && to == TYPE_INT)) return true;
    if (from == TYPE_POINTER && to == TYPE_POINTER) return true;
    return false;
}

/* Returns the promoted numeric type for binary operations (int or real). */
static DataType promote_numeric(DataType t1, DataType t2) {
    if (t1 == TYPE_REAL || t2 == TYPE_REAL) return TYPE_REAL;
    return TYPE_INT;
}

/* Checks if a type check result represents a valid condition (numeric or pointer). */
static bool is_condition_true(const TypeCheckResult *cond) {
    return cond->valid && IS_CONDITION(cond->type);
}

/* Compares two function signatures for equality (return type, parameters, defaults). */
static bool signatures_equal(const FunctionSignature *a, const FunctionSignature *b) {
    if (!a || !b) return false;
    if (a->return_type != b->return_type) return false;
    if (a->param_count != b->param_count) return false;
    if (a->required_param_count != b->required_param_count) return false;
    if (a->is_variadic != b->is_variadic) return false;
    const FunctionParam *pa = a->params, *pb = b->params;
    while (pa && pb) {
        if (pa->type != pb->type) return false;
        if ((pa->default_value != NULL) != (pb->default_value != NULL)) return false;
        pa = pa->next; pb = pb->next;
    }
    return !pa && !pb;
}

/* Verifies that a new function signature matches an existing declaration (prototype). */
static bool check_prototype_match(SemanticContext *ctx,
                                  SymbolEntry *existing,
                                  const FunctionSignature *new_sig,
                                  uint16_t line, uint16_t column) {
    if (!signatures_equal(existing->extra.func_sig, new_sig)) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, line, column,
                  (uint8_t)strlen(existing->name),
                  "Conflicting function signature for '%s'", existing->name);
        return false;
    }
    return true;
}

/* Checks argument types and counts for a function call against the function's signature. */
static bool check_function_call_arguments(SemanticContext *ctx,
                                          SymbolEntry *func,
                                          AST *arg_list,
                                          uint16_t line, uint16_t column) {
    FunctionSignature *sig = func->extra.func_sig;
    if (!sig) return false;

    size_t arg_count = arg_list ? arg_list->count : 0;

    if (ctx->extra_warnings && arg_count == 0) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, line, column, 0,
                  "Function call requires at least one argument; use 'Void' if none are needed");
        return false;
    }

    if (arg_count == 1 && arg_list->nodes[0] && is_void_literal(arg_list->nodes[0])) {
        arg_count = 0;
        arg_list = NULL;
    }

    /* Removed the check for has_body here. A prototype (has_body=false) is sufficient
       to allow calls; missing definitions are diagnosed in final_verification. */

    if (arg_count < sig->required_param_count) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, line, column,
                  (uint8_t)strlen(func->name),
                  "Function '%s' requires at least %zu argument(s), got %zu",
                  func->name, sig->required_param_count, arg_count);
        return false;
    }
    if (arg_count > sig->param_count && !sig->is_variadic) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, line, column,
                  (uint8_t)strlen(func->name),
                  "Function '%s' expects at most %zu argument(s), got %zu",
                  func->name, sig->param_count, arg_count);
        return false;
    }

    FunctionParam *param = sig->params;
    bool ok = true;
    for (size_t i = 0; i < arg_count && param; i++, param = param->next) {
        ASTNode *arg_node = arg_list->nodes[i];
        TypeCheckResult arg_res = semantic__check_type(ctx, arg_node);
        if (!arg_res.valid) { ok = false; continue; }
        if (!can_implicitly_convert(ctx, arg_res.type, param->type)) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, arg_node->line,
                      arg_node->column, 0,
                      "Argument %zu: expected %s, got %s",
                      i + 1, semantic__type_to_string(param->type),
                      semantic__type_to_string(arg_res.type));
            ok = false;
        }
    }
    for (; param; param = param->next) {
        if (!param->default_value) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, line, column, 0,
                      "Missing argument for parameter '%s'",
                      param->name ? param->name : "(unnamed)");
            ok = false;
        }
    }
    return ok;
}

/* Type‑checks a function call node, producing the result type and init state. */
static bool check_function_call(SemanticContext *ctx, ASTNode *node, TypeCheckResult *out) {
    if (!node || !out) return false;
    const char *fname = node->value;
    if (!fname) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Function call missing name");
        return false;
    }

    SymbolEntry *func = semantic__find_symbol(ctx, fname);
    if (!func) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, node->line, node->column,
                  (uint8_t)strlen(fname), "Undeclared function '%s'", fname);
        return false;
    }
    if (func->type != TYPE_FUNCTION) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(fname), "'%s' is not a function", fname);
        return false;
    }

    func->is_used = true;
    AST *arg_list = node->extra ? (AST *)node->extra : NULL;
    if (!check_function_call_arguments(ctx, func, arg_list, node->line, node->column))
        return false;

    out->type = func->extra.func_sig->return_type;
    out->type_info = func->extra.func_sig->return_type_info;
    out->init_state = INIT_UNINITIALIZED;
    out->valid = true;
    out->error_msg = NULL;
    return true;
}

/* Searches for a member by name in a flat list of compound members. */
static CompoundMember *find_compound_member_from_list(CompoundMember *list,
                                                       const char *member_name) {
    while (list) {
        if (list->name && strcmp(list->name, member_name) == 0)
            return list;
        list = list->next;
    }
    return NULL;
}

/* Resolves a field access (e.g. obj.member) and returns the member's type and metadata. */
static bool resolve_member_access(SemanticContext *ctx, ASTNode *node,
                                  DataType *out_type, Type **out_type_info,
                                  SymbolEntry **out_struct_entry,
                                  CompoundMember **out_member) {
    if (!node || node->type != AST_FIELD_ACCESS) return false;
    ASTNode *left = node->left;
    DataType lhs_type = TYPE_UNKNOWN;
    Type *lhs_type_info = NULL;
    SymbolEntry *struct_entry = NULL;

    if (left->type == AST_FIELD_ACCESS) {
        CompoundMember *dummy = NULL;
        if (!resolve_member_access(ctx, left, &lhs_type, &lhs_type_info,
                                   &struct_entry, &dummy))
            return false;
    } else if (left->type == AST_IDENTIFIER) {
        SymbolEntry *sym = semantic__find_symbol(ctx, left->value);
        if (!sym) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, left->line, left->column,
                      (uint8_t)strlen(left->value),
                      "Undeclared identifier '%s'", left->value);
            return false;
        }
        if (sym->type != TYPE_COMPOUND && sym->type != TYPE_UNION) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, left->line, left->column, 0,
                      "Member access on non‑compound value '%s'", left->value);
            return false;
        }
        if (is_struct_type_definition(sym)) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, left->line, left->column, 0,
                      "Cannot access members of type '%s' (use an instance)", left->value);
            return false;
        }
        lhs_type = sym->type;
        lhs_type_info = sym->type_info;
        sym->is_used = true;
        struct_entry = sym;
    } else {
        return false;
    }

    const char *member_name = node->right->value;
    if (!member_name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Missing member name in field access");
        return false;
    }

    CompoundMember *member = NULL;
    CompoundMember *members = get_compound_members(struct_entry, ctx);
    if (!members && lhs_type_info && lhs_type_info->name) {
        SymbolEntry *type_def = semantic__find_symbol(ctx, lhs_type_info->name);
        if (type_def && is_struct_type_definition(type_def)) {
            if (is_incomplete_type(type_def)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                          (uint8_t)strlen(type_def->name),
                          "Cannot access members of incomplete type '%s'", type_def->name);
                return false;
            }
            type_def->is_used = true;
            members = type_def->extra.compound_members;
            struct_entry = type_def;
        }
    }
    if (!members) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, left->line, left->column, 0,
                  "Compound variable '%s' has no associated type information", left->value);
        return false;
    }

    member = find_compound_member_from_list(members, member_name);
    if (!member) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, node->line, node->column,
                  (uint8_t)strlen(member_name),
                  "Compound type '%s' has no member '%s'",
                  struct_entry->name, member_name);
        return false;
    }
    if (out_type) *out_type = member->type;
    if (out_type_info) *out_type_info = member->type_info;
    if (out_struct_entry) *out_struct_entry = struct_entry;
    if (out_member) *out_member = member;
    return true;
}

/* Validates an initialiser for a compound type (struct/union). */
static bool check_struct_initializer(SemanticContext *ctx,
                                     ASTNode *initializer_node,
                                     SymbolEntry *compound_type_entry) {
    if (!initializer_node || !compound_type_entry) return false;
    if (initializer_node->type != AST_MULTI_INITIALIZER) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                  initializer_node->line, initializer_node->column, 0,
                  "Expected a compound literal");
        return false;
    }

    CompoundMember *members = compound_type_entry->extra.compound_members;
    if (!members) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                  initializer_node->line, initializer_node->column, 0,
                  "Cannot initialise an incomplete compound type");
        return false;
    }

    AST *elements = (AST *)initializer_node->extra;
    if (!elements || elements->count == 0) return true;

    size_t member_count = 0;
    for (CompoundMember *m = members; m; m = m->next) member_count++;

    bool is_designated = (elements->count > 0 &&
                          elements->nodes[0]->type == AST_FIELD_ACCESS);

    if (is_designated) {
        for (uint16_t i = 0; i < elements->count; i++) {
            ASTNode *des = elements->nodes[i];
            if (des->type != AST_FIELD_ACCESS || !des->left || !des->right ||
                des->left->type != AST_IDENTIFIER) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, des->line, des->column, 0,
                          "Invalid designated initializer");
                continue;
            }
            const char *mname = des->left->value;
            if (!find_compound_member_from_list(members, mname)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, des->line, des->column,
                          (uint8_t)strlen(mname),
                          "Compound type has no member '%s'", mname);
            }
        }
    } else {
        if (elements->count > member_count) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                      initializer_node->line, initializer_node->column, 0,
                      "Too many initialisers for compound type (expected %zu, got %u)",
                      member_count, elements->count);
            return false;
        }
    }
    return true;
}

/* Builds a linked list of CompoundMember from an AST_BLOCK that contains variable declarations. */
static CompoundMember *build_member_list_from_block(SemanticContext *ctx,
                                                    ASTNode *block_node,
                                                    DataType compound_kind) {
    if (!block_node || block_node->type != AST_BLOCK) return NULL;
    AST *mlist = (AST *)block_node->extra;
    if (!mlist) return NULL;

    CompoundMember *head = NULL, *tail = NULL;
    for (uint16_t i = 0; i < mlist->count; i++) {
        ASTNode *mnode = mlist->nodes[i];
        if (!mnode || mnode->type != AST_VARIABLE_DECLARATION) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, mnode ? mnode->line : 0,
                      mnode ? mnode->column : 0, 0,
                      "Only variable declarations are allowed inside a compound type");
            continue;
        }

        const char *mstate = mnode->state_modifier;
        if (!mstate || (strcmp(mstate, "def") != 0 && strcmp(mstate, "member") != 0)) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, mnode->line, mnode->column, 0,
                      "Compound member must be declared with 'def' or 'member'");
            continue;
        }

        const char *mname = mnode->value;
        Type *mtinfo = mnode->variable_type;
        ASTNode *mdef = mnode->default_value;
        const char *macc = mnode->access_modifier;
        bool is_inline = has_modifier(macc, "inline");

        if (!mname && mtinfo && mtinfo->name &&
            (STR_EQUAL(mtinfo->name, "Struct") || STR_EQUAL(mtinfo->name, "Union")) &&
            mnode->right && mnode->right->type == AST_BLOCK) {
            DataType nkind = STR_EQUAL(mtinfo->name, "Struct") ? TYPE_COMPOUND : TYPE_UNION;
            if (!validate_modifiers_for_type(ctx, macc, nkind, mnode->line, mnode->column, NULL))
                continue;

            CompoundMember *anon = calloc(1, sizeof(CompoundMember));
            if (!anon) continue;
            anon->name = NULL;
            anon->access_modifier = macc ? u__strduplic(macc)
                                          : u__strduplic(get_default_modifiers(nkind));
            anon->type = nkind;
            anon->type_info = NULL;
            anon->init_state = INIT_CONSTANT;
            anon->is_inline = is_inline;
            anon->compound_members = build_member_list_from_block(ctx, mnode->right, nkind);
            anon->next = NULL;
            if (!head) head = tail = anon;
            else { tail->next = anon; tail = anon; }
            continue;
        }

        if (!mname || !mtinfo) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, mnode->line, mnode->column, 0,
                      "Compound member must have a name and type");
            continue;
        }
        DataType mtype = type_from_type_info(mtinfo, ctx);
        if (mtype == TYPE_AUTO || mtype == TYPE_UNKNOWN || mtype == TYPE_VOID) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, mnode->line, mnode->column, 0,
                      "Invalid type for compound member '%s'", mname);
            continue;
        }
        if (!validate_modifiers_for_type(ctx, macc, mtype, mnode->line, mnode->column, mname))
            continue;

        if (strcmp(mstate, "def") == 0 && !mdef) {
            if (ctx->extra_warnings) {
                SEM_WARNING(ctx, ERROR_CODE_SEM_DEF_VAR_MISSING_INIT,
                            mnode->line, mnode->column,
                            (uint8_t)strlen(mname),
                            "'def' member '%s' declared without initialiser", mname);
            }
        }

        InitState init = INIT_UNINITIALIZED;
        if (mdef) {
            if (mdef->type == AST_MULTI_INITIALIZER) {
                init = INIT_FULL;
            } else {
                TypeCheckResult ir = semantic__check_type(ctx, mdef);
                if (!ir.valid) continue;
                if (!can_implicitly_convert(ctx, ir.type, mtype)) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, mdef->line, mdef->column, 0,
                              "Cannot initialise member '%s' (%s) with %s",
                              mname, semantic__type_to_string(mtype),
                              semantic__type_to_string(ir.type));
                    continue;
                }
                init = INIT_FULL;
            }
        }

        const char *mmods = (macc && macc[0]) ? macc : get_default_modifiers(mtype);
        CompoundMember *member = calloc(1, sizeof(CompoundMember));
        if (!member) continue;
        member->name = u__strduplic(mname);
        member->access_modifier = u__strduplic(mmods);
        member->type = mtype;
        member->type_info = mtinfo;
        member->init_state = init;
        member->is_inline = is_inline;
        member->compound_members = NULL;
        member->next = NULL;

        if (!head) head = tail = member;
        else { tail->next = member; tail = member; }
    }
    return head;
}

/* Checks a return statement against the current function's return type. */
static bool process_return_statement(SemanticContext *ctx, ASTNode *node) {
    ASTNode *expr = node->left;
    DataType func_ret = ctx->current_return_type;

    if (ctx->extra_warnings && !expr) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Return statement requires an expression; use 'return Void;' if no value is returned");
        return false;
    }

    if (!expr) {
        if (func_ret != TYPE_VOID) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                      "Non‑void function must return a value");
            return false;
        }
        return true;
    }

    TypeCheckResult ret_res = semantic__check_type(ctx, expr);
    if (!ret_res.valid) return false;

    if (!can_implicitly_convert(ctx, ret_res.type, func_ret)) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Cannot return %s from function returning %s",
                  semantic__type_to_string(ret_res.type),
                  semantic__type_to_string(func_ret));
        return false;
    }
    return true;
}

/* Processes a 'del' statement: removes a variable from the current scope. */
static bool process_del_variable(SemanticContext *ctx, ASTNode *node) {
    const char *name = node->value;
    if (!name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "'del' requires a variable name");
        return false;
    }

    SymbolEntry *existing = find_symbol_in_table(ctx->current_scope, name);
    if (!existing) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, node->line, node->column,
                  (uint8_t)strlen(name), "'del' on undeclared variable '%s'", name);
        return false;
    }

    if (existing->type == TYPE_FUNCTION || existing->type == TYPE_COMPOUND ||
        existing->type == TYPE_UNION    || existing->type == TYPE_LABEL) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_DEL_INVALID_TARGET, node->line, node->column,
                  (uint8_t)strlen(name), "'del' cannot be applied to '%s'", name);
        return false;
    }

    uint32_t idx = hash_string(name) % ctx->current_scope->capacity;
    SymbolEntry **prev = &ctx->current_scope->entries[idx];
    while (*prev) {
        if (*prev == existing) {
            *prev = existing->next;
            free(existing->name);
            free(existing->access_modifier);
            free(existing);
            ctx->current_scope->count--;
            return true;
        }
        prev = &(*prev)->next;
    }
    return false;
}

/* Processes a struct or union definition or prototype. */
static bool process_compound_definition(SemanticContext *ctx, ASTNode *node,
                                        DataType compound_kind, bool is_prototype) {
    const char *name = node->value;
    if (!name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Compound definition requires a name");
        return false;
    }

    SymbolEntry *existing = find_symbol_in_table(ctx->current_scope, name);
    if (existing) {
        if (existing->type != compound_kind) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_REDECLARATION, node->line, node->column,
                      (uint8_t)strlen(name), "Redeclaration of '%s' as a different kind", name);
            return false;
        }
        if (is_prototype) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_REDECLARATION, node->line, node->column,
                      (uint8_t)strlen(name), "Duplicate prototype for compound type '%s'", name);
            return false;
        }
        if (!is_incomplete_type(existing)) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_REDECLARATION, node->line, node->column,
                      (uint8_t)strlen(name), "Compound type '%s' already defined", name);
            return false;
        }

        ASTNode *block = node->right;
        if (!block || block->type != AST_BLOCK) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)strlen(name),
                      "Definition of compound type '%s' requires a member block", name);
            return false;
        }
        existing->extra.compound_members = build_member_list_from_block(ctx, block, compound_kind);
        if (node->access_modifier) {
            free(existing->access_modifier);
            existing->access_modifier = u__strduplic(node->access_modifier);
        }
        existing->is_inline = has_modifier(node->access_modifier, "inline");
        return true;
    }

    CompoundMember *mlist = NULL;
    if (!is_prototype) {
        ASTNode *block = node->right;
        if (!block || block->type != AST_BLOCK) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)strlen(name),
                      "Compound type '%s' requires a member block", name);
            return false;
        }
        mlist = build_member_list_from_block(ctx, block, compound_kind);
    }

    SymbolEntry *entry = calloc(1, sizeof(SymbolEntry));
    if (!entry) {
        destroy_compound_member_list(mlist);
        return false;
    }
    entry->name = u__strduplic(name);
    entry->access_modifier = node->access_modifier ? u__strduplic(node->access_modifier) : NULL;
    entry->type = compound_kind;
    entry->type_info = NULL;
    entry->is_constant = true;
    entry->init_state = INIT_CONSTANT;
    entry->declared_scope = ctx->current_scope->level;
    entry->line = node->line;
    entry->column = node->column;
    entry->extra.compound_members = mlist;
    entry->is_mutable = false;
    entry->is_inline = has_modifier(node->access_modifier, "inline");

    if (!add_symbol_to_table(ctx->current_scope, entry)) {
        free(entry->name);
        free(entry->access_modifier);
        destroy_compound_member_list(mlist);
        free(entry);
        return false;
    }
    return true;
}

/* Processes a label declaration (jump target). */
static bool process_label_statement(SemanticContext *ctx, ASTNode *node) {
    const char *name = node->value;
    if (!name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Label requires a name");
        return false;
    }
    if (check_name_and_warn(ctx, name, node->line, node->column))
        return false;

    SymbolEntry *entry = calloc(1, sizeof(SymbolEntry));
    if (!entry) return false;
    entry->name = u__strduplic(name);
    entry->access_modifier = NULL;
    entry->type = TYPE_LABEL;
    entry->type_info = NULL;
    entry->is_constant = true;
    entry->init_state = INIT_CONSTANT;
    entry->declared_scope = ctx->current_scope->level;
    entry->line = node->line;
    entry->column = node->column;
    entry->is_mutable = false;
    entry->is_used = false;
    entry->is_inline = false;

    if (!add_symbol_to_table(ctx->current_scope, entry)) {
        free(entry->name);
        free(entry);
        return false;
    }
    return true;
}

/* Processes function declarations (both prototypes 'pro' and definitions 'def'). */
static bool process_function_declaration(SemanticContext *ctx, ASTNode *node) {
    const char *name = node->value;
    const char *smod = node->state_modifier;
    const char *amod = node->access_modifier;
    bool is_inline = has_modifier(amod, "inline") || has_modifier(smod, "inline");

    const char *decl_kind = "pro";
    if (smod) {
        if (STR_EQUAL(smod, "def") || STR_EQUAL(smod, "pro") || STR_EQUAL(smod, "del"))
            decl_kind = smod;
        else if (STR_EQUAL(smod, "inline"))
            decl_kind = "pro";
    }
    if (is_inline && !STR_EQUAL(decl_kind, "def")) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)(name ? strlen(name) : 0),
                  "Modifier 'inline' can only be used on 'def' functions with a body");
        return false;
    }

    ASTNode *params_node = node->left;
    ASTNode *body_node   = node->right;
    Type    *ret_tinfo   = node->variable_type;

    DataType ret_type = TYPE_VOID;
    if (ret_tinfo) {
        ret_type = type_from_type_info(ret_tinfo, ctx);
        if (ret_type == TYPE_AUTO) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                      "Function '%s' cannot have return type Auto",
                      name ? name : "(anonymous)");
            return false;
        }
        if (ret_type == TYPE_UNKNOWN) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                      "Unknown return type for function '%s'",
                      name ? name : "(anonymous)");
            return false;
        }
    }

    /* Build the parameter list from the AST parameter block. */
    FunctionParam *params = NULL, *last = NULL;
    size_t pcount = 0, reqcount = 0;
    if (params_node && params_node->type == AST_BLOCK && params_node->extra) {
        AST *plist = (AST *)params_node->extra;
        if (plist->count == 1) {
            ASTNode *s = plist->nodes[0];
            if (s && s->type == AST_LITERAL_VALUE && STR_EQUAL(s->value, "Void"))
                plist = NULL;
        }
        if (plist) {
            for (uint16_t i = 0; i < plist->count; i++) {
                ASTNode *p = plist->nodes[i];
                if (!p) continue;

                const char *pname = NULL;
                DataType ptype = TYPE_VOID;
                ASTNode *pdef = NULL;
                const char *pamod = p->access_modifier;

                if (p->type == AST_VARIABLE_DECLARATION) {
                    pname = p->value;
                    pdef  = p->default_value;
                    if (p->variable_type)
                        ptype = type_from_type_info(p->variable_type, ctx);
                } else if (p->type == AST_LITERAL_VALUE) {
                    ptype = string_to_datatype(p->value);
                    if (ptype == TYPE_UNKNOWN) {
                        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, p->line, p->column, 0,
                                  "Unknown type '%s' for unnamed parameter", p->value);
                        continue;
                    }
                } else {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, p->line, p->column, 0,
                              "Invalid parameter syntax");
                    continue;
                }

                if (ptype == TYPE_AUTO && !pdef) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, p->line, p->column, 0,
                              "Parameter '%s' has type Auto but no default value",
                              pname ? pname : "(unnamed)");
                    ptype = TYPE_UNKNOWN;
                }
                if (ptype == TYPE_UNKNOWN) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, p->line, p->column, 0,
                              "Unknown type for parameter '%s'", pname ? pname : "(unnamed)");
                }
                if ((ptype == TYPE_COMPOUND || ptype == TYPE_UNION) &&
                    p->variable_type && p->variable_type->name) {
                    SymbolEntry *type_def = semantic__find_symbol(ctx, p->variable_type->name);
                    if (type_def && is_struct_type_definition(type_def) &&
                        is_incomplete_type(type_def)) {
                        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, p->line, p->column, 0,
                                  "Parameter '%s' has incomplete type '%s'",
                                  pname ? pname : "(unnamed)", p->variable_type->name);
                        ptype = TYPE_UNKNOWN;
                    }
                }
                if (!validate_modifiers_for_type(ctx, pamod, ptype, p->line, p->column,
                                                 pname ? pname : "(unnamed)"))
                    continue;

                FunctionParam *param = malloc(sizeof(FunctionParam));
                if (!param) continue;
                param->name = pname ? u__strduplic(pname) : NULL;
                param->type = ptype;
                param->default_value = pdef;
                param->next = NULL;
                if (!params) params = param;
                else last->next = param;
                last = param;
                pcount++;
                if (!pdef) reqcount++;
            }
        }
    }

    if (STR_EQUAL(decl_kind, "del")) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_DEL_INVALID_TARGET, node->line, node->column,
                  (uint8_t)(name ? strlen(name) : 0),
                  "'del' cannot be applied to functions");
        free_function_param_list(params);
        return false;
    }

    if (!name) {
        /* Anonymous function – allowed only as a lambda? Here we simply analyse its body if any. */
        if (body_node) {
            semantic__enter_function_scope(ctx, "(anonymous)", ret_type);
            semantic__check_statement(ctx, body_node);
            semantic__exit_function_scope(ctx);
        }
        free_function_param_list(params);
        return true;
    }

    if (check_name_and_warn(ctx, name, node->line, node->column)) {
        free_function_param_list(params);
        return false;
    }

    if (STR_EQUAL(decl_kind, "pro")) {
        /* Prototype: must not have a body, no default parameters allowed. */
        if (body_node) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_PRO_FUNC_HAS_BODY, node->line, node->column,
                      (uint8_t)strlen(name), "Prototype '%s' must not have a body", name);
            free_function_param_list(params);
            return false;
        }
        for (FunctionParam *p = params; p; p = p->next) {
            if (p->default_value) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_PROTO_PARAM_DEFAULT, node->line, node->column,
                          (uint8_t)strlen(name),
                          "Prototype parameter '%s' of '%s' cannot have a default value",
                          p->name ? p->name : "(unnamed)", name);
                free_function_param_list(params);
                return false;
            }
        }
        if (!semantic__add_function_ex(ctx, ctx->current_scope, name,
                                       ret_type, ret_tinfo,
                                       params, pcount, reqcount,
                                       false, node->line, node->column, NULL,
                                       false, false)) {
            return false;
        }
        SymbolEntry *fentry = semantic__find_symbol(ctx, name);
        if (fentry && is_inline) fentry->is_inline = true;
        return true;
    }

    /* Definition ('def') handling. */
    if (!body_node) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_DEF_FUNC_MISSING_BODY, node->line, node->column,
                  (uint8_t)strlen(name), "'def' function '%s' must have a body", name);
        free_function_param_list(params);
        return false;
    }
    if (body_node->type == AST_LITERAL_VALUE && body_node->operation_type == TOKEN_NONE) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_DEF_FUNC_NONE_BODY, node->line, node->column,
                  (uint8_t)strlen(name), "'def' function '%s' cannot have a 'none' body", name);
        free_function_param_list(params);
        return false;
    }

    bool added = semantic__add_function_ex(ctx, ctx->current_scope, name,
                                           ret_type, ret_tinfo,
                                           params, pcount, reqcount,
                                           false, node->line, node->column, NULL,
                                           true, false);
    if (!added) {
        return false;
    }

    SymbolEntry *fentry = semantic__find_symbol(ctx, name);
    if (!fentry || fentry->type != TYPE_FUNCTION) return false;
    fentry->is_inline = is_inline;

    FunctionSignature *sig = fentry->extra.func_sig;
    semantic__enter_function_scope(ctx, name, ret_type);
    for (FunctionParam *p = sig->params; p; p = p->next) {
        if (p->name)
            semantic__add_variable_ex(ctx, ctx->current_scope,
                                      p->name, p->type, NULL,
                                      false, INIT_FULL,
                                      node->line, node->column,
                                      get_default_modifiers(p->type));
    }
    for (FunctionParam *p = sig->params; p; p = p->next) {
        if (p->default_value) {
            TypeCheckResult dr = semantic__check_type(ctx, p->default_value);
            if (!dr.valid || !can_implicitly_convert(ctx, dr.type, p->type)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Invalid default value for parameter '%s'",
                          p->name ? p->name : "(unnamed)");
            }
        }
    }

    semantic__check_statement(ctx, body_node);
    semantic__exit_function_scope(ctx);
    return true;
}

/* Processes a variable declaration (var/def/pro). The 'kind' parameter distinguishes
   the declaration kind: 0 = var (inferred or typed), 1 = def (must have initialiser),
   2 = pro (prototype, only type, no initialiser). */
static bool process_variable_declaration(SemanticContext *ctx, ASTNode *node,
                                         int kind) {
    const char *name = node->value;
    const char *state_mod = node->state_modifier;
    const char *acc_mod   = node->access_modifier;
    bool is_inline = has_modifier(acc_mod, "inline") || has_modifier(state_mod, "inline");

    /* Handle anonymous struct/union definitions that are declared inline. */
    if (node->variable_type && node->variable_type->name &&
        (STR_EQUAL(node->variable_type->name, "Struct") ||
         STR_EQUAL(node->variable_type->name, "Union")) &&
        node->right && node->right->type == AST_BLOCK) {
        DataType comp_kind = STR_EQUAL(node->variable_type->name, "Struct")
                                 ? TYPE_COMPOUND : TYPE_UNION;

        if (node->default_value) {
            if (!name) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Anonymous struct variable must be named");
                return false;
            }
            if (check_name_and_warn(ctx, name, node->line, node->column))
                return false;

            CompoundMember *mlist = build_member_list_from_block(ctx, node->right, comp_kind);
            InitState init_state = INIT_UNINITIALIZED;
            if (is_none_literal(node->default_value)) {
                init_state = INIT_UNINITIALIZED;
            } else {
                if (node->default_value->type == AST_MULTI_INITIALIZER) {
                    SymbolEntry tmp = { .extra.compound_members = mlist };
                    if (!check_struct_initializer(ctx, node->default_value, &tmp)) {
                        destroy_compound_member_list(mlist);
                        return false;
                    }
                    init_state = INIT_FULL;
                } else {
                    TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
                    if (!init_res.valid) { destroy_compound_member_list(mlist); return false; }
                    if (!can_implicitly_convert(ctx, init_res.type, comp_kind)) {
                        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                                  "Initializer type does not match compound variable");
                        destroy_compound_member_list(mlist);
                        return false;
                    }
                    init_state = INIT_FULL;
                }
            }

            SymbolEntry *entry = calloc(1, sizeof(SymbolEntry));
            if (!entry) { destroy_compound_member_list(mlist); return false; }
            entry->name = u__strduplic(name);
            entry->access_modifier = acc_mod ? u__strduplic(acc_mod)
                                             : u__strduplic(get_default_modifiers(comp_kind));
            entry->type = comp_kind;
            entry->type_info = node->variable_type;
            entry->is_constant = false;
            entry->init_state = init_state;
            entry->declared_scope = ctx->current_scope->level;
            entry->line = node->line;
            entry->column = node->column;
            entry->extra.compound_members = mlist;
            entry->is_mutable = true;
            entry->is_inline = is_inline;

            if (!add_symbol_to_table(ctx->current_scope, entry)) {
                free(entry->name); free(entry->access_modifier);
                destroy_compound_member_list(mlist); free(entry);
                return false;
            }
            return true;
        } else {
            return process_compound_definition(ctx, node, comp_kind, false);
        }
    }

    if (name && check_name_and_warn(ctx, name, node->line, node->column))
        return false;

    /* Specific checks for 'def' and 'pro' kinds. */
    if (kind == 1) {
        if (!node->variable_type) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_DEF_VAR_MISSING_TYPE, node->line, node->column,
                      (uint8_t)(name ? strlen(name) : 0),
                      "'def' variable '%s' must have a type annotation",
                      name ? name : "(anonymous)");
            return false;
        }
    } else if (kind == 2) {
        if (node->default_value) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_PRO_VAR_HAS_INIT, node->line, node->column,
                      (uint8_t)(name ? strlen(name) : 0),
                      "'pro' variable '%s' must not have an initialiser",
                      name ? name : "(anonymous)");
            return false;
        }
        if (!node->variable_type) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_PRO_VAR_MISSING_TYPE, node->line, node->column,
                      (uint8_t)(name ? strlen(name) : 0),
                      "'pro' variable '%s' requires a type annotation",
                      name ? name : "(anonymous)");
            return false;
        }
        if (node->variable_type->name &&
            (STR_EQUAL(node->variable_type->name, "Struct") ||
             STR_EQUAL(node->variable_type->name, "Union"))) {
            return process_compound_definition(ctx, node,
                STR_EQUAL(node->variable_type->name, "Struct") ? TYPE_COMPOUND : TYPE_UNION,
                true);
        }
        if (type_from_type_info(node->variable_type, ctx) == TYPE_AUTO) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_PRO_VAR_MISSING_TYPE, node->line, node->column,
                      (uint8_t)(name ? strlen(name) : 0),
                      "'pro' variable '%s' cannot have type Auto",
                      name ? name : "(anonymous)");
            return false;
        }
    }

    DataType dt;
    Type *tinfo;
    InitState init_state;
    const char *default_mods;

    if (!deduce_and_check_variable(ctx, node, &dt, &tinfo, &init_state, &default_mods))
        return false;

    if (!validate_modifiers_for_type(ctx, acc_mod, dt, node->line, node->column, name))
        return false;

    if (!name) {
        if (node->default_value) semantic__check_type(ctx, node->default_value);
        return true;
    }

    return semantic__add_variable_ex(ctx, ctx->current_scope,
                                     name, dt, tinfo, false, init_state,
                                     node->line, node->column, default_mods);
}

/* Deduces the type of a variable from its type annotation or initialiser,
   and performs the necessary compatibility checks. */
static bool deduce_and_check_variable(SemanticContext *ctx, ASTNode *node,
                                      DataType *out_type,
                                      Type **out_type_info,
                                      InitState *out_init_state,
                                      const char **out_default_mods) {
    Type *tinfo = node->variable_type;
    DataType dt = TYPE_UNKNOWN;
    *out_init_state = INIT_UNINITIALIZED;

    if (tinfo) {
        dt = type_from_type_info(tinfo, ctx);
        if (dt == TYPE_AUTO) {
            if (!node->default_value) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                          (uint8_t)(node->value ? strlen(node->value) : 0),
                          "Auto type requires an initialiser");
                return false;
            }
            TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
            RETURN_IF_INVALID(init_res);
            dt = init_res.type;
            if (dt != TYPE_INT && dt != TYPE_REAL && dt != TYPE_CHAR && dt != TYPE_VOID) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Auto type deduced as '%s'; only Int, Real, Char, Void allowed",
                          semantic__type_to_string(dt));
                return false;
            }
        } else if (dt == TYPE_UNKNOWN) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)(node->value ? strlen(node->value) : 0),
                      "Unknown type for variable '%s'", node->value ? node->value : "(anonymous)");
            return false;
        }
    } else {
        if (!node->default_value) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)(node->value ? strlen(node->value) : 0),
                      "Variable '%s' requires a type annotation or initialiser",
                      node->value ? node->value : "(anonymous)");
            return false;
        }
        TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
        RETURN_IF_INVALID(init_res);
        dt = init_res.type;
        if (dt == TYPE_AUTO || dt == TYPE_UNKNOWN || dt == TYPE_VOID) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                      "Cannot deduce usable type for '%s'",
                      node->value ? node->value : "(anonymous)");
            return false;
        }
    }

    const char *default_mods = get_default_modifiers(dt);
    if (node->default_value) {
        if (is_none_literal(node->default_value)) {
            *out_init_state = INIT_UNINITIALIZED;
        } else {
            TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
            RETURN_IF_INVALID(init_res);
            if (!can_implicitly_convert(ctx, init_res.type, dt)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Cannot initialise '%s' (%s) with %s",
                          node->value ? node->value : "(anonymous)",
                          semantic__type_to_string(dt),
                          semantic__type_to_string(init_res.type));
                return false;
            }
            *out_init_state = INIT_FULL;
        }
    } else {
        *out_init_state = INIT_DEFAULT;
        if (ctx->extra_warnings && node->value) {
            SEM_WARNING(ctx, ERROR_CODE_SEM_UNINITIALIZED,
                        node->line, node->column,
                        (uint8_t)strlen(node->value),
                        "Variable '%s' declared without an initialiser", node->value);
        }
    }

    *out_type = dt;
    *out_type_info = tinfo;
    *out_default_mods = default_mods;
    return true;
}

/* Public function: type‑check an expression node and return detailed result. */
TypeCheckResult semantic__check_type(SemanticContext *ctx, ASTNode *node) {
    TypeCheckResult res = { false, TYPE_UNKNOWN, NULL, INIT_UNINITIALIZED, NULL };
    if (!node) return res;

    switch (node->type) {
        case AST_LITERAL_VALUE: {
            TokenType tt = node->operation_type;
            if (tt == TOKEN_NUMBER) {
                const char *val = node->value;
                size_t len = val ? strlen(val) : 0;
                if (len == 0) { res.valid = false; break; }
                char last = val[len - 1];
                if (last == 'i' || last == 'I')
                    res.type = TYPE_INT;
                else if (last == 'f' || last == 'F')
                    res.type = TYPE_REAL;
                else if (strchr(val, '.') || strchr(val, 'e') || strchr(val, 'E'))
                    res.type = TYPE_REAL;
                else
                    res.type = TYPE_INT;
                res.init_state = INIT_CONSTANT;
                res.valid = true;
            } else if (tt == TOKEN_CHAR) {
                res.type = TYPE_CHAR; res.init_state = INIT_CONSTANT; res.valid = true;
            } else if (tt == TOKEN_STRING) {
                if (node->value && strlen(node->value) == 3 && node->value[0] == '\'') {
                    res.type = TYPE_CHAR; res.init_state = INIT_CONSTANT; res.valid = true;
                } else {
                    res.valid = false;
                }
            } else if (tt == TOKEN_NONE) {
                res.type = TYPE_VOID; res.init_state = INIT_UNINITIALIZED; res.valid = true;
            } else {
                res.type = string_to_datatype(node->value);
                if (res.type != TYPE_UNKNOWN) {
                    res.init_state = INIT_CONSTANT;
                    res.valid = true;
                } else {
                    res.valid = false;
                }
            }
            break;
        }

        case AST_IDENTIFIER: {
            if (!node->value) { res.valid = false; break; }
            SymbolEntry *sym = semantic__find_symbol(ctx, node->value);
            if (!sym) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, node->line, node->column,
                          (uint8_t)strlen(node->value),
                          "Undeclared identifier '%s'", node->value);
                break;
            }
            if (sym->type == TYPE_FUNCTION || sym->type == TYPE_LABEL) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_INVALID_OPERATION, node->line, node->column,
                          (uint8_t)strlen(node->value),
                          "Cannot use '%s' as a value", node->value);
                break;
            }
            res.type = sym->type;
            res.type_info = sym->type_info;
            res.init_state = sym->init_state;
            res.valid = true;
            sym->is_used = true;
            break;
        }

        case AST_FIELD_ACCESS: {
            if (!HAS_LEFT(node) || !node->right) { res.valid = false; break; }
            DataType mtype;
            Type *mtinfo = NULL;
            if (!resolve_member_access(ctx, node, &mtype, &mtinfo, NULL, NULL))
                res.valid = false;
            else {
                res.type = mtype;
                res.type_info = mtinfo;
                res.init_state = INIT_UNINITIALIZED;
                res.valid = true;
            }
            break;
        }

        case AST_ARRAY_ACCESS: {
            if (!HAS_LEFT(node) || !node->right) { res.valid = false; break; }
            TypeCheckResult left_res = semantic__check_type(ctx, node->left);
            BREAK_IF_INVALID(left_res);
            if (left_res.type != TYPE_ARRAY) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Cannot index a non‑array value");
                break;
            }
            TypeCheckResult idx_res = semantic__check_type(ctx, node->right);
            BREAK_IF_INVALID(idx_res);
            if (!IS_NUMERIC(idx_res.type)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->right->line, node->right->column, 0,
                          "Array index must be numeric");
                break;
            }
            res.type = TYPE_INT;
            res.type_info = NULL;
            res.init_state = INIT_UNINITIALIZED;
            res.valid = true;
            break;
        }

        case AST_MULTI_INITIALIZER: {
            res.type = TYPE_COMPOUND;
            res.type_info = NULL;
            res.init_state = INIT_FULL;
            res.valid = true;
            break;
        }

        case AST_CAST: {
            ASTNode *expr = node->left;
            TypeCheckResult expr_res = semantic__check_type(ctx, expr);
            if (!expr_res.valid) { res = expr_res; break; }
            Type *target = node->variable_type;
            if (!target) { res.valid = false; break; }
            DataType target_dt = type_from_type_info(target, ctx);
            if (target_dt == TYPE_UNKNOWN) { res.valid = false; break; }
            res.type = target_dt;
            res.type_info = target;
            res.init_state = expr_res.init_state;
            res.valid = true;
            break;
        }

        case AST_FUNCTION_CALL:
            check_function_call(ctx, node, &res);
            break;

        case AST_TERNARY_OPERATION: {
            ASTNode *cond = node->left, *tru = node->right;
            ASTNode *fls = node->extra ? (ASTNode *)node->extra : NULL;
            if (!cond || !tru || !fls) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Malformed ternary");
                break;
            }
            TypeCheckResult cr = semantic__check_type(ctx, cond);
            BREAK_IF_INVALID(cr);
            if (!IS_CONDITION(cr.type)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, cond->line, cond->column, 0,
                          "Ternary condition must be numeric or pointer");
                break;
            }
            TypeCheckResult tr = semantic__check_type(ctx, tru);
            TypeCheckResult fr = semantic__check_type(ctx, fls);
            BREAK_IF_INVALID(tr);
            BREAK_IF_INVALID(fr);
            DataType common = promote_numeric(tr.type, fr.type);
            if (!can_implicitly_convert(ctx, tr.type, common) ||
                !can_implicitly_convert(ctx, fr.type, common)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Incompatible ternary branches: %s and %s",
                          semantic__type_to_string(tr.type),
                          semantic__type_to_string(fr.type));
                break;
            }
            res.type = common;
            res.init_state = INIT_UNINITIALIZED;
            res.valid = true;
            break;
        }

        default:
            res.valid = true;
            break;
    }
    return res;
}

/* Public function: type‑check a statement node. */
bool semantic__check_statement(SemanticContext *ctx, ASTNode *node) {
    if (!node) return true;

    switch (node->type) {
        case AST_VARIABLE_DECLARATION: {
            const char *mod = node->state_modifier;
            int kind;
            if (!mod || STR_EQUAL(mod, "var")) kind = 0;
            else if (STR_EQUAL(mod, "def"))    kind = 1;
            else if (STR_EQUAL(mod, "pro"))    kind = 2;
            else if (STR_EQUAL(mod, "del"))    return process_del_variable(ctx, node);
            else if (STR_EQUAL(mod, "member")) return true;
            else kind = 0;
            return process_variable_declaration(ctx, node, kind);
        }

        case AST_FUNCTION_DECLARATION:
            return process_function_declaration(ctx, node);

        case AST_BLOCK: {
            semantic__enter_scope(ctx);
            AST *list = (AST *)node->extra;
            bool ok = true;
            if (list)
                for (uint16_t i = 0; i < list->count; i++)
                    if (!semantic__check_statement(ctx, list->nodes[i])) ok = false;
            semantic__exit_scope(ctx);
            return ok;
        }

        case AST_IF_STATEMENT: {
            ASTNode *cond_node = node->left;
            if (ctx->extra_warnings && !cond_node) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "'if' statement requires a condition; use 'if(Void)' if no condition is needed");
                return false;
            }
            if (cond_node) {
                TypeCheckResult cond;
                if (is_void_literal(cond_node)) {
                    cond.valid = true;
                    cond.type = TYPE_VOID;
                } else {
                    cond = semantic__check_type(ctx, cond_node);
                }
                RETURN_IF_INVALID(cond);
                if (!is_condition_true(&cond)) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                              "If condition must be numeric or pointer");
                    return false;
                }
            }
            semantic__enter_scope(ctx);
            bool t_ok = semantic__check_statement(ctx, node->right);
            semantic__exit_scope(ctx);
            bool e_ok = true;
            if (node->extra) {
                semantic__enter_scope(ctx);
                e_ok = semantic__check_statement(ctx, node->extra);
                semantic__exit_scope(ctx);
            }
            return t_ok && e_ok;
        }

        case AST_DO_LOOP: {
            ASTNode *cond_node = node->left;
            if (ctx->extra_warnings && !cond_node) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "'do' loop requires a condition; use 'do(Void)' if no condition is needed");
                return false;
            }
            if (cond_node) {
                TypeCheckResult cond;
                if (is_void_literal(cond_node)) {
                    cond.valid = true;
                    cond.type = TYPE_VOID;
                } else {
                    cond = semantic__check_type(ctx, cond_node);
                }
                RETURN_IF_INVALID(cond);
                if (!is_condition_true(&cond)) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                              "Do loop condition must be numeric or pointer");
                    return false;
                }
            }
            semantic__enter_loop_scope(ctx);
            bool b_ok = semantic__check_statement(ctx, node->right);
            semantic__exit_loop_scope(ctx);
            bool e_ok = true;
            if (node->extra) {
                semantic__enter_scope(ctx);
                e_ok = semantic__check_statement(ctx, node->extra);
                semantic__exit_scope(ctx);
            }
            return b_ok && e_ok;
        }

        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT: {
            ASTNode *lhs = node->left, *rhs = node->right;
            if (!lhs || !rhs) return true;
            TypeCheckResult lr = semantic__check_type(ctx, lhs);
            RETURN_IF_INVALID(lr);

            /* Check mutability of the left‑hand side. */
            if (lhs->type == AST_IDENTIFIER) {
                SymbolEntry *sym = semantic__find_symbol(ctx, lhs->value);
                if (sym && !sym->is_mutable) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, lhs->line, lhs->column,
                              (uint8_t)strlen(lhs->value),
                              "Cannot assign to constant variable '%s'", lhs->value);
                    return false;
                }
            } else if (lhs->type == AST_FIELD_ACCESS) {
                ASTNode *base = lhs->left;
                while (base && base->type == AST_FIELD_ACCESS) base = base->left;
                if (base && base->type == AST_IDENTIFIER) {
                    SymbolEntry *sym = semantic__find_symbol(ctx, base->value);
                    if (sym && !sym->is_mutable) {
                        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, lhs->line, lhs->column,
                                  (uint8_t)strlen(base->value),
                                  "Cannot assign to field of constant compound '%s'",
                                  base->value);
                        return false;
                    }
                }
            } else if (lhs->type == AST_ARRAY_ACCESS) {
                ASTNode *arr = lhs->left;
                while (arr && arr->type == AST_ARRAY_ACCESS) arr = arr->left;
                if (arr && arr->type == AST_IDENTIFIER) {
                    SymbolEntry *sym = semantic__find_symbol(ctx, arr->value);
                    if (sym && !sym->is_mutable) {
                        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, lhs->line, lhs->column,
                                  (uint8_t)strlen(arr->value),
                                  "Cannot assign to element of constant array '%s'",
                                  arr->value);
                        return false;
                    }
                }
            }

            /* Special handling for compound initialisers. */
            if (lhs->type == AST_IDENTIFIER &&
                (lr.type == TYPE_COMPOUND || lr.type == TYPE_UNION) &&
                rhs->type == AST_MULTI_INITIALIZER) {
                SymbolEntry *var = semantic__find_symbol(ctx, lhs->value);
                if (!var) return false;
                SymbolEntry *target = var;
                if (!var->extra.compound_members && lr.type_info && lr.type_info->name) {
                    target = semantic__find_symbol(ctx, lr.type_info->name);
                    if (!target || !is_struct_type_definition(target)) {
                        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                                  "Could not determine compound type for initializer");
                        return false;
                    }
                }
                return check_struct_initializer(ctx, rhs, target);
            }

            TypeCheckResult rr = semantic__check_type(ctx, rhs);
            RETURN_IF_INVALID(rr);
            if (!can_implicitly_convert(ctx, rr.type, lr.type)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Cannot assign %s to %s",
                          semantic__type_to_string(rr.type),
                          semantic__type_to_string(lr.type));
                return false;
            }
            return true;
        }

        case AST_FUNCTION_CALL: {
            TypeCheckResult dummy;
            return check_function_call(ctx, node, &dummy);
        }

        case AST_LABEL_DECLARATION:
            return process_label_statement(ctx, node);

        case AST_RETURN:
            return process_return_statement(ctx, node);

        default:
            return semantic__check_expression(ctx, node);
    }
}

/* Public function: type‑check an expression node (simple wrapper). */
bool semantic__check_expression(SemanticContext *ctx, ASTNode *node) {
    if (!node) return true;
    TypeCheckResult res = semantic__check_type(ctx, node);
    if (!res.valid) { ctx->has_errors = true; return false; }
    return true;
}

/* Performs final verification after the whole AST has been processed:
   – checks that every used function has a body
   – ensures that a main function exists (unless -Wextra)
   – reports unused symbols under -Wextra. */
static bool final_verification(SemanticContext *ctx) {
    bool ok = true;
    SymbolTable *global = ctx->global_scope;
    int main_count = 0;

    for (size_t i = 0; i < global->capacity; i++) {
        for (SymbolEntry *entry = global->entries[i]; entry; entry = entry->next) {
            if (entry->type == TYPE_FUNCTION) {
                FunctionSignature *sig = entry->extra.func_sig;
                if (entry->is_used && !sig->has_body && !sig->is_none_body) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_UNDEFINED_VAR,
                              entry->line, entry->column,
                              (uint8_t)strlen(entry->name),
                              "Function '%s' declared but never defined", entry->name);
                    ok = false;
                }
                if (STR_EQUAL(entry->name, "main") && sig->has_body) main_count++;
                if (!entry->is_used && ctx->extra_warnings &&
                    !STR_EQUAL(entry->name, "main")) {
                    SEM_WARNING(ctx, ERROR_CODE_SEM_UNUSED_VARIABLE,
                                entry->line, entry->column,
                                (uint8_t)strlen(entry->name),
                                "Function '%s' declared but never used", entry->name);
                }
            } else if (!entry->is_used && ctx->extra_warnings &&
                       entry->type != TYPE_LABEL) {
                SEM_WARNING(ctx, ERROR_CODE_SEM_UNUSED_VARIABLE,
                            entry->line, entry->column,
                            (uint8_t)strlen(entry->name),
                            "'%s' declared but never used", entry->name);
            }
        }
    }

    if (main_count == 0) {
        if (ctx->extra_warnings) {
            SEM_WARNING(ctx, ERROR_CODE_SEM_UNDEFINED_VAR, 0, 0, 0,
                        "No 'main' function with a body defined (warning under -Wextra)");
        } else {
            SEM_ERROR(ctx, ERROR_CODE_SEM_UNDEFINED_VAR, 0, 0, 0,
                      "No 'main' function with a body defined");
            ok = false;
        }
    } else if (main_count > 1) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_REDECLARATION, 0, 0, 0,
                  "Multiple definitions of 'main' function");
        ok = false;
    }
    return ok;
}

/* Top‑level entry point: analyses the whole AST and returns true if no semantic errors. */
bool semantic__analyze(SemanticContext *ctx, AST *ast) {
    if (!ctx || !ast) return false;
    ctx->has_errors = false;
    ctx->abort_compilation = false;

    for (uint16_t i = 0; i < ast->count; i++)
        if (!semantic__check_statement(ctx, ast->nodes[i]))
            ctx->has_errors = true;

    if (!final_verification(ctx))
        ctx->has_errors = true;

    return !ctx->has_errors;
}

/* Creates a fresh semantic analysis context with an empty global scope. */
SemanticContext *semantic__create_context(void) {
    SemanticContext *ctx = calloc(1, sizeof(SemanticContext));
    if (!ctx) return NULL;
    ctx->global_scope = create_symbol_table(NULL, SCOPE_GLOBAL);
    if (!ctx->global_scope) { free(ctx); return NULL; }
    ctx->current_scope = ctx->global_scope;
    ctx->warnings_enabled = true;
    ctx->exit_on_error = false;
    ctx->strict_type_check = false;
    ctx->extra_warnings = false;
    ctx->abort_compilation = false;
    ctx->in_loop = false;
    ctx->in_function = false;
    ctx->current_function = NULL;
    ctx->current_return_type = TYPE_VOID;
    return ctx;
}

/* Destroys the context and all associated symbol tables. */
void semantic__destroy_context(SemanticContext *ctx) {
    if (!ctx) return;
    destroy_symbol_table(ctx->global_scope);
    free(ctx);
}

/* Enables or disables extra warnings (‑Wextra). */
void semantic__set_extra_warnings(SemanticContext *ctx, bool enable) {
    if (ctx) ctx->extra_warnings = enable;
}

/* Returns whether the compilation should be aborted due to a fatal error. */
bool semantic__should_abort(const SemanticContext *ctx) {
    return ctx ? ctx->abort_compilation : false;
}

/* Enters a new scope with a specific nesting level. */
void semantic__enter_scope_ex(SemanticContext *ctx, ScopeLevel level) {
    if (!ctx) return;
    SymbolTable *s = create_symbol_table(ctx->current_scope, level);
    if (s) ctx->current_scope = s;
}

/* Enters a new generic block scope. */
void semantic__enter_scope(SemanticContext *ctx) {
    semantic__enter_scope_ex(ctx, SCOPE_BLOCK);
}

/* Exits the current scope (does nothing if already at global scope). */
void semantic__exit_scope(SemanticContext *ctx) {
    if (!ctx || !ctx->current_scope || ctx->current_scope == ctx->global_scope) return;
    ctx->current_scope = ctx->current_scope->parent;
}

/* Enters a function scope, setting the current function name and return type. */
void semantic__enter_function_scope(SemanticContext *ctx, const char *name, DataType ret) {
    semantic__enter_scope_ex(ctx, SCOPE_FUNCTION);
    ctx->in_function = true;
    ctx->current_function = name;
    ctx->current_return_type = ret;
}

/* Exits the current function scope. */
void semantic__exit_function_scope(SemanticContext *ctx) {
    semantic__exit_scope(ctx);
    ctx->in_function = false;
    ctx->current_function = NULL;
}

/* Enters a loop scope, enabling break/continue checks. */
void semantic__enter_loop_scope(SemanticContext *ctx) {
    semantic__enter_scope_ex(ctx, SCOPE_LOOP);
    ctx->in_loop = true;
}

/* Exits a loop scope. */
void semantic__exit_loop_scope(SemanticContext *ctx) {
    semantic__exit_scope(ctx);
    ctx->in_loop = false;
}

/* Low‑level variable addition to a symbol table. */
bool semantic__add_variable_ex(SemanticContext *ctx, SymbolTable *target_scope,
                               const char *name, DataType type, Type *type_info,
                               bool is_constant, InitState init_state,
                               uint16_t line, uint16_t column,
                               const char *access_modifier) {
    if (!ctx || !name) return false;
    SymbolTable *scope = target_scope ? target_scope : ctx->current_scope;

    SymbolEntry *entry = calloc(1, sizeof(SymbolEntry));
    if (!entry) return false;
    entry->name = u__strduplic(name);
    entry->access_modifier = access_modifier ? u__strduplic(access_modifier) : NULL;
    entry->type = type;
    entry->type_info = type_info;
    entry->is_constant = is_constant;
    entry->init_state = init_state;
    entry->declared_scope = scope->level;
    entry->line = line;
    entry->column = column;
    entry->is_mutable = !is_constant;
    entry->is_inline = has_modifier(access_modifier, "inline");

    if (!add_symbol_to_table(scope, entry)) {
        free(entry->name); free(entry->access_modifier); free(entry);
        return false;
    }
    return true;
}

/* Low‑level function addition to a symbol table. Handles prototypes and definitions. */
bool semantic__add_function_ex(SemanticContext *ctx, SymbolTable *target_scope,
                               const char *name,
                               DataType return_type, Type *return_type_info,
                               FunctionParam *params,
                               size_t param_count,
                               size_t required_count, bool is_variadic,
                               uint16_t line, uint16_t column,
                               const char *access_modifier,
                               bool has_body, bool is_none_body) {
    if (!ctx || !name) {
        free_function_param_list(params);
        return false;
    }
    SymbolTable *scope = target_scope ? target_scope : ctx->current_scope;

    SymbolEntry *existing = find_symbol_in_table(scope, name);
    if (existing) {
        if (existing->type != TYPE_FUNCTION) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_FUNC_DIFFERENT_KIND, line, column,
                      (uint8_t)strlen(name),
                      "Redeclaration of '%s' as a different symbol kind", name);
            free_function_param_list(params);
            return false;
        }

        FunctionSignature new_sig = {
            .return_type = return_type,
            .return_type_info = return_type_info,
            .params = params,
            .param_count = param_count,
            .required_param_count = required_count,
            .is_variadic = is_variadic,
            .has_body = has_body,
            .is_none_body = is_none_body
        };

        if (!check_prototype_match(ctx, existing, &new_sig, line, column)) {
            free_function_param_list(params);
            return false;
        }

        FunctionSignature *sig = existing->extra.func_sig;
        if (has_body && sig->has_body) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_FUNC_DUPLICATE_BODY, line, column,
                      (uint8_t)strlen(name),
                      "Function '%s' already has a body", name);
            free_function_param_list(params);
            return false;
        }

        free_function_param_list(params);

        if (has_body && !sig->has_body) {
            sig->has_body = true;
            sig->is_none_body = is_none_body;
        }
        existing->is_inline = existing->is_inline || has_modifier(access_modifier, "inline");
        return true;
    }

    FunctionSignature *sig = calloc(1, sizeof(FunctionSignature));
    if (!sig) {
        free_function_param_list(params);
        return false;
    }
    sig->return_type = return_type;
    sig->return_type_info = return_type_info;
    sig->params = params;
    sig->param_count = param_count;
    sig->required_param_count = required_count;
    sig->is_variadic = is_variadic;
    sig->has_body = has_body;
    sig->is_none_body = is_none_body;

    SymbolEntry *entry = calloc(1, sizeof(SymbolEntry));
    if (!entry) {
        free_function_param_list(params);
        free(sig);
        return false;
    }
    entry->name = u__strduplic(name);
    entry->access_modifier = access_modifier ? u__strduplic(access_modifier) : NULL;
    entry->type = TYPE_FUNCTION;
    entry->type_info = return_type_info;
    entry->is_constant = true;
    entry->init_state = INIT_CONSTANT;
    entry->declared_scope = scope->level;
    entry->line = line;
    entry->column = column;
    entry->extra.func_sig = sig;
    if (strcmp(name, "main") == 0) entry->is_used = true;
    entry->is_inline = has_modifier(access_modifier, "inline");

    if (!add_symbol_to_table(scope, entry)) {
        free(entry->name); free(entry->access_modifier); free(entry);
        free_function_param_list(params);
        free(sig);
        return false;
    }
    return true;
}

/* Looks up a symbol in the current scope chain. */
SymbolEntry *semantic__find_symbol(SemanticContext *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    SymbolTable *t = ctx->current_scope;
    while (t) {
        SymbolEntry *e = find_symbol_in_table(t, name);
        if (e) return e;
        t = t->parent;
    }
    return NULL;
}

/* Returns a human‑readable string representation of a DataType. */
const char *semantic__type_to_string(DataType type) {
    switch (type) {
        case TYPE_INT:      return "Int";
        case TYPE_REAL:     return "Real";
        case TYPE_CHAR:     return "Char";
        case TYPE_VOID:     return "Void";
        case TYPE_AUTO:     return "Auto";
        case TYPE_POINTER:  return "pointer";
        case TYPE_ARRAY:    return "array";
        case TYPE_FUNCTION: return "function";
        case TYPE_COMPOUND: return "Struct";
        case TYPE_UNION:    return "Union";
        case TYPE_LABEL:    return "label";
        default:            return "unknown";
    }
}

/* Returns the total number of symbols in the global scope. */
size_t semantic__get_symbol_count(SemanticContext *ctx) {
    return ctx && ctx->global_scope ? ctx->global_scope->count : 0;
}

/* Returns whether any semantic error has been recorded. */
bool semantic__has_errors(const SemanticContext *ctx) {
    return ctx ? ctx->has_errors : false;
}
