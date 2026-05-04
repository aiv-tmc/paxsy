#include "semantic.h"
#include "../errhandler/errhandler.h"
#include "../utils/str_utils.h"
#include "../utils/memory_utils.h"
#include "../utils/common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Size of the hash table for symbol scopes (must be a power of two). */
#define HASH_TABLE_SIZE 64

/*
 * Convenience macros for accessing optional AST children.
 * They avoid NULL‑pointer dereferences and make the code more readable.
 */
#define HAS_LEFT(n)       ((n) && (n)->left)
#define HAS_RIGHT(n)      ((n) && (n)->right)
#define IS_AST_TYPE(n,t)  ((n) && (n)->type == (t))

/* Shorthand to test whether a type is numeric (Int, Real, or Char). */
#define IS_NUMERIC(t)     ((t) == TYPE_INT || (t) == TYPE_REAL || (t) == TYPE_CHAR)

/* Types accepted as a condition (numeric or pointer). */
#define IS_CONDITION(t)   (IS_NUMERIC(t) || (t) == TYPE_POINTER)

/*
 * Internal error / warning reporting helpers.
 *
 * SEM_ERROR records an error and sets the abort_compilation flag so that
 * later phases are skipped.  SEM_WARNING emits a non‑fatal diagnostic.
 */
#define SEM_ERROR(ctx, code, line, col, len, ...) \
    do { \
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, (code), (line), (col), (len), \
                                    "semantic", __VA_ARGS__); \
        (ctx)->has_errors = true; \
        (ctx)->abort_compilation = true; \
    } while(0)

#define SEM_WARNING(ctx, code, line, col, len, ...) \
    do { \
        errhandler__report_error_ex(ERROR_LEVEL_WARNING, (code), (line), (col), (len), \
                                    "semantic", __VA_ARGS__); \
    } while(0)

/* Forward declarations of internal helper functions. */
static uint32_t          hash_string(const char *str);
static SymbolTable *     create_symbol_table(SymbolTable *parent, ScopeLevel level);
static void              destroy_symbol_table(SymbolTable *table);
static SymbolEntry *     find_symbol_in_table(SymbolTable *table, const char *name);
static bool              add_symbol_to_table(SymbolTable *table, SymbolEntry *entry);
static DataType          type_from_type_info(Type *type_info, SemanticContext *ctx);
static DataType          string_to_datatype(const char *name);
static bool              is_type_name(const char *name);
static const char *      get_default_modifiers(DataType type);
static bool              check_name_collision_in_current_scope(SemanticContext *ctx,
                                                               const char *name,
                                                               uint16_t line, uint16_t column);
static void              warn_if_shadowing(SemanticContext *ctx, const char *name,
                                          uint16_t line, uint16_t column);
static bool              signatures_equal(FunctionSignature *a, FunctionSignature *b);
static bool              check_prototype_match(SemanticContext *ctx,
                                               SymbolEntry *existing,
                                               FunctionSignature *new_sig,
                                               uint16_t line, uint16_t column);
static FunctionParam *   clone_function_params(FunctionParam *src);
static bool              can_implicitly_convert(SemanticContext *ctx, DataType from, DataType to);
static DataType          promote_numeric(DataType t1, DataType t2);
static bool              is_condition_true(const TypeCheckResult *cond);
static bool              check_function_call_arguments(SemanticContext *ctx,
                                                       SymbolEntry *func,
                                                       AST *arg_list,
                                                       uint16_t line, uint16_t column);
static bool              contains_return(ASTNode *node);
static bool              statement_ensures_return(SemanticContext *ctx, ASTNode *node);

/* Variable declaration handlers for each modifier. */
static bool              process_def_variable(SemanticContext *ctx, ASTNode *node);
static bool              process_pro_variable(SemanticContext *ctx, ASTNode *node);
static bool              process_del_variable(SemanticContext *ctx, ASTNode *node);
static bool              process_var_variable(SemanticContext *ctx, ASTNode *node);

/* Function and struct definition handlers. */
static bool              process_function_declaration(SemanticContext *ctx, ASTNode *node);
static bool              process_struct_definition(SemanticContext *ctx, ASTNode *node);

/* Label statement handler. */
static bool              process_label_statement(SemanticContext *ctx, ASTNode *node);

/* Type‑switch (re‑cast) handler. */
static bool              handle_type_switch(SemanticContext *ctx, ASTNode *cast_node);

/* Validates a struct initializer against the struct definition. */
static bool              check_struct_initializer(SemanticContext *ctx,
                                                  ASTNode *initializer_node,
                                                  SymbolEntry *struct_type_entry);
/* Recursively resolves a chain of field accesses (a.b.c). */
static bool              resolve_member_access(SemanticContext *ctx, ASTNode *node,
                                               DataType *out_type, Type **out_type_info,
                                               SymbolEntry **out_struct_entry,
                                               CompoundMember **out_member);

/* Looks up a struct member by name. */
static CompoundMember *  find_struct_member(SymbolEntry *struct_entry, const char *member_name);

/* Final verification pass. */
static bool              final_verification(SemanticContext *ctx);

/* Predicates for special literals and modifiers. */
static bool              is_none_literal(const ASTNode *node);
static bool              has_modifier(const char *mods, const char *name);

/*
 * FNV‑1a hash function for symbol table buckets.
 * Returns a 32‑bit hash of the null‑terminated input.
 */
static uint32_t hash_string(const char *str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

/*
 * Create a new symbol table and link it as a child of `parent`.
 * The table starts empty and uses the provided scope level.
 * Returns NULL on memory allocation failure.
 */
static SymbolTable *create_symbol_table(SymbolTable *parent, ScopeLevel level) {
    SymbolTable *table = calloc(1, sizeof(SymbolTable));
    if (!table) return NULL;
    table->entries = calloc(HASH_TABLE_SIZE, sizeof(SymbolEntry *));
    if (!table->entries) {
        free(table);
        return NULL;
    }
    table->capacity = HASH_TABLE_SIZE;
    table->level = level;
    table->parent = parent;
    if (parent) {
        /* Insert as the first child of the parent. */
        table->next_child = parent->children;
        parent->children = table;
    }
    return table;
}

/*
 * Recursively destroy a symbol table and all its children.
 * Every allocated string, parameter list, member list, and entry is freed.
 */
static void destroy_symbol_table(SymbolTable *table) {
    if (!table) return;
    /* Destroy children first. */
    SymbolTable *child = table->children;
    while (child) {
        SymbolTable *next = child->next_child;
        destroy_symbol_table(child);
        child = next;
    }
    /* Free entries in the hash table. */
    for (size_t i = 0; i < table->capacity; i++) {
        SymbolEntry *entry = table->entries[i];
        while (entry) {
            SymbolEntry *next = entry->next;
            free(entry->name);
            free(entry->access_modifier);
            if (entry->type == TYPE_FUNCTION && entry->extra.func_sig) {
                FunctionParam *p = entry->extra.func_sig->params;
                while (p) {
                    FunctionParam *n = p->next;
                    free(p->name);
                    free(p);
                    p = n;
                }
                free(entry->extra.func_sig);
            } else if (entry->type == TYPE_COMPOUND && entry->extra.compound_members) {
                CompoundMember *m = entry->extra.compound_members;
                while (m) {
                    CompoundMember *next_m = m->next;
                    free(m->name);
                    free(m->access_modifier);
                    free(m);
                    m = next_m;
                }
            }
            free(entry);
            entry = next;
        }
    }
    free(table->entries);
    free(table);
}

/*
 * Look up a name within a single scope (no parent traversal).
 * Returns the SymbolEntry* or NULL if not found.
 */
static SymbolEntry *find_symbol_in_table(SymbolTable *table, const char *name) {
    if (!table || !name) return NULL;
    uint32_t idx = hash_string(name) % table->capacity;
    SymbolEntry *entry = table->entries[idx];
    while (entry) {
        if (strcmp(entry->name, name) == 0) return entry;
        entry = entry->next;
    }
    return NULL;
}

/*
 * Insert a new symbol entry into the hash table.
 * Does NOT check for duplicates – the caller must guarantee uniqueness.
 * Returns true on success.
 */
static bool add_symbol_to_table(SymbolTable *table, SymbolEntry *entry) {
    if (!table || !entry) return false;
    uint32_t idx = hash_string(entry->name) % table->capacity;
    entry->next = table->entries[idx];
    table->entries[idx] = entry;
    table->count++;
    return true;
}

/*
 * Create a deep copy of a linked list of function parameters.
 * Used when duplicating function signatures during symbol insertion.
 * Returns the head of the new list, or NULL on allocation failure.
 */
static FunctionParam *clone_function_params(FunctionParam *src) {
    if (!src) return NULL;
    FunctionParam *head = NULL, *tail = NULL;
    while (src) {
        FunctionParam *copy = malloc(sizeof(FunctionParam));
        if (!copy) {
            /* On failure, clean up already allocated nodes. */
            while (head) {
                FunctionParam *tmp = head->next;
                free(head->name);
                free(head);
                head = tmp;
            }
            return NULL;
        }
        copy->name = src->name ? u__strduplic(src->name) : NULL;
        copy->type = src->type;
        copy->type_info = src->type_info;
        copy->default_value = src->default_value;
        copy->next = NULL;
        if (!head) { head = tail = copy; }
        else { tail->next = copy; tail = copy; }
        src = src->next;
    }
    return head;
}

/*
 * Return true if the AST node represents the `none` literal.
 */
static bool is_none_literal(const ASTNode *node) {
    if (!node) return false;
    return (node->type == AST_LITERAL_VALUE && node->operation_type == TOKEN_NONE);
}

/*
 * Check whether the modifier string equals the given name.
 * The comparison is exact; the string is typically `state_modifier` or
 * `access_modifier` from the AST.
 */
static bool has_modifier(const char *mods, const char *name) {
    if (!mods || !name) return false;
    return (strcmp(mods, name) == 0);
}

/*
 * Derive the coarse DataType from a full type descriptor.
 * Handles pointers, arrays, built‑in names, and user‑defined compounds.
 * Returns TYPE_UNKNOWN if the type cannot be resolved.
 */
static DataType type_from_type_info(Type *type_info, SemanticContext *ctx) {
    if (!type_info) return TYPE_UNKNOWN;
    if (type_info->pointer_level > 0) return TYPE_POINTER;
    if (type_info->is_array)          return TYPE_ARRAY;
    if (type_info->name) {
        DataType dt = string_to_datatype(type_info->name);
        if (dt != TYPE_UNKNOWN) return dt;
        /* It might be a user‑defined struct name. */
        SymbolEntry *entry = semantic__find_symbol(ctx, type_info->name);
        if (entry && entry->type == TYPE_COMPOUND) return TYPE_COMPOUND;
    }
    return TYPE_UNKNOWN;
}

/* Return true if the identifier names a built‑in type. */
static bool is_type_name(const char *name) {
    if (!name) return false;
    return (strcmp(name, "Int") == 0  || strcmp(name, "Real") == 0 ||
            strcmp(name, "Char") == 0 || strcmp(name, "Void") == 0 ||
            strcmp(name, "Auto") == 0 || strcmp(name, "Struct") == 0);
}

/* Map a built‑in type name to its DataType, or TYPE_UNKNOWN. */
static DataType string_to_datatype(const char *name) {
    if (!name) return TYPE_UNKNOWN;
    if (strcmp(name, "Int") == 0)    return TYPE_INT;
    if (strcmp(name, "Real") == 0)   return TYPE_REAL;
    if (strcmp(name, "Char") == 0)   return TYPE_CHAR;
    if (strcmp(name, "Void") == 0)   return TYPE_VOID;
    if (strcmp(name, "Auto") == 0)   return TYPE_AUTO;
    if (strcmp(name, "Struct") == 0) return TYPE_COMPOUND;
    return TYPE_UNKNOWN;
}

/*
 * Return the default modifier string for a given type.
 *
 *   - All types get the "open" modifier.
 *   - Int and Real additionally get "unsigned".
 *   - All other types (including Char, Void, structs, pointers) get "static".
 */
static const char *get_default_modifiers(DataType type) {
    switch (type) {
        case TYPE_INT:
        case TYPE_REAL:
            return "open unsigned";
        default:
            return "open static";
    }
}

/*
 * Emit an error if `name` already exists in the current scope.
 * Returns true if a collision was found (and the error was reported).
 */
static bool check_name_collision_in_current_scope(SemanticContext *ctx,
                                                  const char *name,
                                                  uint16_t line, uint16_t column) {
    SymbolEntry *existing = find_symbol_in_table(ctx->current_scope, name);
    if (existing) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_REDECLARATION, line, column,
                  (uint8_t)strlen(name),
                  "Redeclaration of '%s'", name);
        return true;
    }
    return false;
}

/*
 * Emit a warning if the new declaration shadows one from an outer scope.
 * The warning is issued only when warnings are enabled.
 */
static void warn_if_shadowing(SemanticContext *ctx, const char *name,
                              uint16_t line, uint16_t column) {
    if (!ctx->warnings_enabled) return;
    SymbolTable *parent = ctx->current_scope->parent;
    while (parent) {
        if (find_symbol_in_table(parent, name)) {
            SEM_WARNING(ctx, ERROR_CODE_SEM_REDECLARATION, line, column,
                        (uint8_t)strlen(name),
                        "Declaration of '%s' shadows a previous declaration", name);
            break;
        }
        parent = parent->parent;
    }
}

/*
 * Compare two function signatures for exact equality.
 * Returns true if they match in return type, parameter count, types, and
 * default value presence.
 */
static bool signatures_equal(FunctionSignature *a, FunctionSignature *b) {
    if (!a || !b) return false;
    if (a->return_type != b->return_type) return false;
    if (a->param_count != b->param_count) return false;
    if (a->required_param_count != b->required_param_count) return false;
    if (a->is_variadic != b->is_variadic) return false;
    FunctionParam *pa = a->params, *pb = b->params;
    while (pa && pb) {
        if (pa->type != pb->type) return false;
        if ((pa->default_value != NULL) != (pb->default_value != NULL)) return false;
        pa = pa->next;
        pb = pb->next;
    }
    return (pa == NULL && pb == NULL);
}

/*
 * Verify that a new function signature matches an existing forward
 * declaration.  Reports an error and returns false if they differ.
 */
static bool check_prototype_match(SemanticContext *ctx,
                                  SymbolEntry *existing,
                                  FunctionSignature *new_sig,
                                  uint16_t line, uint16_t column) {
    if (!signatures_equal(existing->extra.func_sig, new_sig)) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, line, column,
                  (uint8_t)strlen(existing->name),
                  "Conflicting function signature for '%s'", existing->name);
        return false;
    }
    return true;
}

/*
 * Decide whether an implicit conversion from `from` to `to` is permitted.
 *
 * The special value `none` (TYPE_VOID) can be assigned to any type,
 * representing an uninitialised or zero value.
 * When strict_type_check is enabled (‑Wextra), only identity conversions
 * and the special void case are allowed.  Otherwise, numeric promotions
 * and pointer identity are also accepted.
 */
static bool can_implicitly_convert(SemanticContext *ctx, DataType from, DataType to) {
    /* The `none` literal is represented as TYPE_VOID and is assignable to anything. */
    if (from == TYPE_VOID) return true;
    if (from == to) return true;
    if (ctx->strict_type_check) return false;
    if (IS_NUMERIC(from) && IS_NUMERIC(to)) return true;
    if (from == TYPE_POINTER && to == TYPE_POINTER) return true;
    return false;
}

/* Return the common arithmetic type after numeric promotion. */
static DataType promote_numeric(DataType t1, DataType t2) {
    if (t1 == TYPE_REAL || t2 == TYPE_REAL) return TYPE_REAL;
    if (t1 == TYPE_INT  || t2 == TYPE_INT)  return TYPE_INT;
    return TYPE_CHAR;
}

/* Return true if the type result qualifies as a condition. */
static bool is_condition_true(const TypeCheckResult *cond) {
    return cond->valid && IS_CONDITION(cond->type);
}

/*
 * Validate the arguments of a function call against its signature.
 * Checks argument count, variadic limits, and type compatibility.
 * Returns true if all arguments are valid.
 */
static bool check_function_call_arguments(SemanticContext *ctx,
                                          SymbolEntry *func,
                                          AST *arg_list,
                                          uint16_t line, uint16_t column) {
    FunctionSignature *sig = func->extra.func_sig;
    if (!sig) return false;

    /* Reject calls to undefined functions (declared but missing a body). */
    if (!sig->has_body && !sig->is_none_body) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_UNDEFINED_VAR, line, column,
                  (uint8_t)strlen(func->name),
                  "Call to undefined function '%s'", func->name);
        return false;
    }

    size_t arg_count = arg_list ? arg_list->count : 0;
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

    /* Check each supplied argument against its corresponding parameter. */
    for (size_t i = 0; i < arg_count && param; i++, param = param->next) {
        ASTNode *arg_node = arg_list->nodes[i];
        TypeCheckResult arg_res = semantic__check_type(ctx, arg_node);
        if (!arg_res.valid) {
            ok = false;
            continue;
        }
        if (!can_implicitly_convert(ctx, arg_res.type, param->type)) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, arg_node->line,
                      arg_node->column, 0,
                      "Argument %zu: expected %s, got %s",
                      i + 1,
                      semantic__type_to_string(param->type),
                      semantic__type_to_string(arg_res.type));
            ok = false;
        }
    }

    /* Ensure that the remaining parameters have default values. */
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

/* Recursively detect a return statement anywhere in the subtree. */
static bool contains_return(ASTNode *node) {
    if (!node) return false;
    if (node->type == AST_RETURN) return true;
    if (node->type == AST_BLOCK && node->extra) {
        AST *block = (AST *)node->extra;
        for (uint16_t i = 0; i < block->count; i++)
            if (contains_return(block->nodes[i])) return true;
    }
    return false;
}

/*
 * Simple heuristic that checks whether a statement ensures a return on
 * every control path.  It examines the last statement in a block and both
 * branches of an if statement.
 */
static bool statement_ensures_return(SemanticContext *ctx, ASTNode *node) {
    (void)ctx;
    if (!node) return false;
    if (node->type == AST_RETURN) return true;
    if (node->type == AST_BLOCK && node->extra) {
        AST *block = (AST *)node->extra;
        if (block->count == 0) return false;
        return statement_ensures_return(ctx, block->nodes[block->count - 1]);
    }
    if (node->type == AST_IF_STATEMENT) {
        bool then_ret = node->right ? statement_ensures_return(ctx, node->right) : false;
        bool else_ret = node->extra ? statement_ensures_return(ctx, node->extra) : false;
        return then_ret && else_ret;
    }
    return false;
}

/*
 * Find a struct member by name inside the struct definition's member list.
 * Returns NULL if not found.
 */
static CompoundMember *find_struct_member(SymbolEntry *struct_entry,
                                          const char *member_name) {
    if (!struct_entry || struct_entry->type != TYPE_COMPOUND || !member_name)
        return NULL;
    CompoundMember *m = struct_entry->extra.compound_members;
    while (m) {
        if (strcmp(m->name, member_name) == 0)
            return m;
        m = m->next;
    }
    return NULL;
}

/*
 * Recursively resolve a chain of field accesses (a.b.c).
 *
 * Sets out_type, out_type_info, out_struct_entry, and out_member.
 * Returns false and emits an error on failure.
 */
static bool resolve_member_access(SemanticContext *ctx, ASTNode *node,
                                  DataType *out_type, Type **out_type_info,
                                  SymbolEntry **out_struct_entry,
                                  CompoundMember **out_member) {
    if (!node || node->type != AST_FIELD_ACCESS) return false;

    ASTNode *left = node->left;
    DataType lhs_type = TYPE_UNKNOWN;
    Type *lhs_type_info = NULL;
    SymbolEntry *struct_entry = NULL;

    /* Handle nested field accesses recursively. */
    if (left->type == AST_FIELD_ACCESS) {
        CompoundMember *dummy = NULL;
        if (!resolve_member_access(ctx, left, &lhs_type, &lhs_type_info,
                                   &struct_entry, &dummy))
            return false;
    } else if (left->type == AST_IDENTIFIER) {
        /* Base variable: look it up and verify that it is a struct. */
        TypeCheckResult id_res = semantic__check_type(ctx, left);
        if (!id_res.valid) return false;
        lhs_type = id_res.type;
        lhs_type_info = id_res.type_info;
        if (lhs_type != TYPE_COMPOUND) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, left->line, left->column, 0,
                      "Member access on non‑struct value '%s'", left->value);
            return false;
        }
        /* Locate the struct definition symbol. */
        if (lhs_type_info && lhs_type_info->name) {
            struct_entry = semantic__find_symbol(ctx, lhs_type_info->name);
        }
        if (!struct_entry || struct_entry->type != TYPE_COMPOUND) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, left->line, left->column, 0,
                      "Struct type information missing for '%s'", left->value);
            return false;
        }
    } else {
        return false; /* Invalid left side of field access. */
    }

    const char *member_name = node->right->value;
    if (!member_name) return false;

    CompoundMember *member = find_struct_member(struct_entry, member_name);
    if (!member) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, node->line, node->column,
                  (uint8_t)strlen(member_name),
                  "Struct '%s' has no member '%s'",
                  struct_entry->name, member_name);
        return false;
    }

    if (out_type) *out_type = member->type;
    if (out_type_info) *out_type_info = member->type_info;
    if (out_struct_entry) *out_struct_entry = struct_entry;
    if (out_member) *out_member = member;
    return true;
}

/*
 * Validate a compound initializer (multi‑initializer) against a struct type.
 *
 * Supports both positional and designated initializers.  For designated
 * initializers the parser emits an AST_FIELD_ACCESS node where the left
 * child is an AST_IDENTIFIER carrying the member name and the right child
 * is the value expression.
 *
 * Returns true on success, false on error (with diagnostics emitted).
 */
static bool check_struct_initializer(SemanticContext *ctx,
                                     ASTNode *initializer_node,
                                     SymbolEntry *struct_type_entry) {
    if (!initializer_node || !struct_type_entry)
        return false;

    if (initializer_node->type != AST_MULTI_INITIALIZER) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                  initializer_node->line, initializer_node->column, 0,
                  "Expected a struct literal");
        return false;
    }

    AST *elements = (AST *)initializer_node->extra;
    if (!elements || elements->count == 0) return true; /* empty – default zero init */

    bool is_designated = (elements->count > 0 &&
                          elements->nodes[0]->type == AST_FIELD_ACCESS);

    if (is_designated) {
        /* Each element is AST_FIELD_ACCESS: left = IDENTIFIER, right = value. */
        for (uint16_t i = 0; i < elements->count; i++) {
            ASTNode *designator = elements->nodes[i];
            if (designator->type != AST_FIELD_ACCESS ||
                !designator->left || !designator->right ||
                designator->left->type != AST_IDENTIFIER) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                          designator->line, designator->column, 0,
                          "Mixed designated and positional initializers not allowed");
                continue;
            }

            const char *member_name = designator->left->value;
            ASTNode *value_expr     = designator->right;

            if (!member_name) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                          designator->line, designator->column, 0,
                          "Invalid designated initializer: member name missing");
                continue;
            }

            CompoundMember *m = find_struct_member(struct_type_entry, member_name);
            if (!m) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL,
                          designator->line, designator->column,
                          (uint8_t)strlen(member_name),
                          "Struct '%s' has no member named '%s'",
                          struct_type_entry->name, member_name);
                continue;
            }

            TypeCheckResult val_res = semantic__check_type(ctx, value_expr);
            if (!val_res.valid) continue;
            if (!can_implicitly_convert(ctx, val_res.type, m->type)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                          value_expr->line, value_expr->column, 0,
                          "Cannot initialise member '%s' (%s) with %s",
                          member_name,
                          semantic__type_to_string(m->type),
                          semantic__type_to_string(val_res.type));
            }
        }
    } else {
        /* Positional initializer: match members in declaration order. */
        CompoundMember *member = struct_type_entry->extra.compound_members;
        for (uint16_t i = 0; i < elements->count; i++) {
            ASTNode *val = elements->nodes[i];
            if (!member) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                          val->line, val->column, 0,
                          "Too many initialisers for struct '%s'",
                          struct_type_entry->name);
                return false;
            }
            TypeCheckResult val_res = semantic__check_type(ctx, val);
            if (!val_res.valid) {
                member = member->next;
                continue;
            }
            if (!can_implicitly_convert(ctx, val_res.type, member->type)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                          val->line, val->column, 0,
                          "Initialiser for member '%s' expects %s, got %s",
                          member->name,
                          semantic__type_to_string(member->type),
                          semantic__type_to_string(val_res.type));
            }
            member = member->next;
        }
    }
    return true;
}

/*
 * Process a `def` variable declaration.
 *
 * Enforced rules:
 *   1. `inline` is forbidden on variables (only functions).
 *   2. `fixed` requires a concrete numeric type (Int, Real, Char) AND a
 *      non‑`none` initialiser.
 *
 * Added rule: `none` is a valid initialiser for any type, including structs.
 * It leaves the variable uninitialised (INIT_UNINITIALIZED).
 */
static bool process_def_variable(SemanticContext *ctx, ASTNode *node) {
    const char *name = node->value;
    const char *state_mod = node->state_modifier;
    const char *acc_mod   = node->access_modifier;

    if (!name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "`def` variable requires a name");
        return false;
    }
    if (!node->variable_type) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_DEF_VAR_MISSING_TYPE, node->line, node->column,
                  (uint8_t)strlen(name),
                  "`def` variable '%s' must have a type annotation", name);
        return false;
    }
    if (!node->default_value) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_DEF_VAR_MISSING_INIT, node->line, node->column,
                  (uint8_t)strlen(name),
                  "`def` variable '%s' must be initialised", name);
        return false;
    }

    /* Rule 1 – `inline` on a variable is illegal. */
    if (has_modifier(state_mod, "inline") || has_modifier(acc_mod, "inline")) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Modifier 'inline' can only be applied to functions, not to variable '%s'", name);
        return false;
    }

    if (check_name_collision_in_current_scope(ctx, name, node->line, node->column))
        return false;
    warn_if_shadowing(ctx, name, node->line, node->column);

    Type *tinfo = node->variable_type;
    DataType dt = type_from_type_info(tinfo, ctx);

    if (dt == TYPE_AUTO) {
        /* Deduce concrete type from the initialiser. */
        TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
        if (!init_res.valid) return false;
        dt = init_res.type;
        if (dt == TYPE_AUTO || dt == TYPE_UNKNOWN || dt == TYPE_VOID) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                      "Cannot deduce concrete type for '%s'", name);
            return false;
        }
    } else if (dt == TYPE_UNKNOWN) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Unknown type for variable '%s'", name);
        return false;
    }

    /* Rule 2 – `fixed` modifier checks. */
    if (has_modifier(state_mod, "fixed") || has_modifier(acc_mod, "fixed")) {
        if (dt != TYPE_INT && dt != TYPE_REAL && dt != TYPE_CHAR) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)strlen(name),
                      "Modifier 'fixed' can only be applied to variables of type Int, Real, or Char");
            return false;
        }
        if (is_none_literal(node->default_value)) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)strlen(name),
                      "Modifier 'fixed' cannot be used with initialiser 'none'");
            return false;
        }
    }

    /* Special case: struct initialisation with `none`. */
    if (dt == TYPE_COMPOUND && is_none_literal(node->default_value)) {
        /* none leaves the struct uninitialised; no further checking needed. */
        const char *mods = get_default_modifiers(dt);
        if (!semantic__add_variable_ex(ctx, ctx->current_scope,
                                       name, dt, tinfo,
                                       false, INIT_UNINITIALIZED,
                                       node->line, node->column, mods))
            return false;
        /* Mark fixed if applicable. */
        if (has_modifier(state_mod, "fixed") || has_modifier(acc_mod, "fixed")) {
            SymbolEntry *entry = find_symbol_in_table(ctx->current_scope, name);
            if (entry) entry->is_fixed = true;
        }
        return true;
    }

    /* General initialiser type check. */
    TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
    if (!init_res.valid) return false;
    if (!can_implicitly_convert(ctx, init_res.type, dt)) {
        /* Special case: struct initializer with multi‑initializer. */
        if (dt == TYPE_COMPOUND && node->default_value->type == AST_MULTI_INITIALIZER) {
            SymbolEntry *struct_entry = semantic__find_symbol(ctx, tinfo->name);
            if (struct_entry && struct_entry->type == TYPE_COMPOUND) {
                return check_struct_initializer(ctx, node->default_value, struct_entry);
            }
        }
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Cannot initialise '%s' (%s) with %s",
                  name, semantic__type_to_string(dt),
                  semantic__type_to_string(init_res.type));
        return false;
    }

    const char *mods = get_default_modifiers(dt);
    if (!semantic__add_variable_ex(ctx, ctx->current_scope,
                                   name, dt, tinfo,
                                   false, INIT_FULL,
                                   node->line, node->column, mods))
        return false;

    /* Mark the variable as fixed if the modifier is present. */
    if (has_modifier(state_mod, "fixed") || has_modifier(acc_mod, "fixed")) {
        SymbolEntry *entry = find_symbol_in_table(ctx->current_scope, name);
        if (entry) entry->is_fixed = true;
    }
    return true;
}

/*
 * Process a `pro` variable declaration (prototype).
 *
 * Same modifier rules apply: `inline` forbidden; `fixed` only for concrete
 * numeric types with a non‑`none` initialiser.  Since `pro` has no
 * initialiser, `fixed` always yields an error.
 */
static bool process_pro_variable(SemanticContext *ctx, ASTNode *node) {
    const char *name = node->value;
    const char *state_mod = node->state_modifier;
    const char *acc_mod   = node->access_modifier;

    if (!name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "`pro` variable requires a name");
        return false;
    }
    if (node->default_value) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_PRO_VAR_HAS_INIT, node->line, node->column,
                  (uint8_t)strlen(name),
                  "`pro` variable '%s' must not have an initialiser", name);
        return false;
    }
    if (!node->variable_type) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_PRO_VAR_MISSING_TYPE, node->line, node->column,
                  (uint8_t)strlen(name),
                  "`pro` variable '%s' requires a type annotation", name);
        return false;
    }

    if (has_modifier(state_mod, "inline") || has_modifier(acc_mod, "inline")) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Modifier 'inline' can only be applied to functions, not to variable '%s'", name);
        return false;
    }

    if (check_name_collision_in_current_scope(ctx, name, node->line, node->column))
        return false;
    warn_if_shadowing(ctx, name, node->line, node->column);

    Type *tinfo = node->variable_type;
    DataType dt = type_from_type_info(tinfo, ctx);
    if (dt == TYPE_AUTO) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_PRO_VAR_MISSING_TYPE, node->line, node->column,
                  (uint8_t)strlen(name),
                  "`pro` variable '%s' cannot have type Auto without an initialiser", name);
        return false;
    }
    if (dt == TYPE_UNKNOWN) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Unknown type for variable '%s'", name);
        return false;
    }

    if (has_modifier(state_mod, "fixed") || has_modifier(acc_mod, "fixed")) {
        if (dt != TYPE_INT && dt != TYPE_REAL && dt != TYPE_CHAR) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)strlen(name),
                      "Modifier 'fixed' can only be applied to variables of type Int, Real, or Char");
            return false;
        }
        /* Since there is no initialiser, `fixed` is always an error. */
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Modifier 'fixed' requires a non‑none initialiser, which is missing for a 'pro' variable");
        return false;
    }

    const char *mods = get_default_modifiers(dt);
    return semantic__add_variable_ex(ctx, ctx->current_scope,
                                     name, dt, tinfo,
                                     false, INIT_UNINITIALIZED,
                                     node->line, node->column, mods);
}

/*
 * Process a `del` statement – removes a variable from the current scope.
 */
static bool process_del_variable(SemanticContext *ctx, ASTNode *node) {
    const char *name = node->value;
    if (!name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "`del` requires a variable name");
        return false;
    }

    SymbolEntry *existing = find_symbol_in_table(ctx->current_scope, name);
    if (!existing) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, node->line, node->column,
                  (uint8_t)strlen(name),
                  "`del` on undeclared variable '%s'", name);
        return false;
    }
    if (existing->type == TYPE_FUNCTION ||
        existing->type == TYPE_COMPOUND ||
        existing->type == TYPE_LABEL) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_DEL_INVALID_TARGET, node->line, node->column,
                  (uint8_t)strlen(name),
                  "`del` cannot be applied to '%s'", name);
        return false;
    }

    /* Unlink the entry from its hash bucket. */
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

/*
 * Process a plain `var` variable declaration (id : Type [= init]).
 *
 * Enforces the same modifier rules as above.  `none` is a valid initialiser
 * that leaves the variable uninitialised.
 */
static bool process_var_variable(SemanticContext *ctx, ASTNode *node) {
    const char *name = node->value;
    const char *state_mod = node->state_modifier;
    const char *acc_mod   = node->access_modifier;

    if (!name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Variable declaration requires a name");
        return false;
    }

    if (has_modifier(state_mod, "inline") || has_modifier(acc_mod, "inline")) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Modifier 'inline' can only be applied to functions, not to variable '%s'", name);
        return false;
    }

    if (check_name_collision_in_current_scope(ctx, name, node->line, node->column))
        return false;
    warn_if_shadowing(ctx, name, node->line, node->column);

    Type *tinfo = node->variable_type;
    DataType dt = TYPE_UNKNOWN;
    InitState init_state = INIT_UNINITIALIZED;

    if (tinfo) {
        dt = type_from_type_info(tinfo, ctx);
        if (dt == TYPE_AUTO) {
            if (node->default_value) {
                TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
                if (!init_res.valid) return false;
                dt = init_res.type;
                if (dt == TYPE_AUTO || dt == TYPE_UNKNOWN || dt == TYPE_VOID) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                              "Cannot deduce concrete type for '%s'", name);
                    return false;
                }
            } else {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                          (uint8_t)strlen(name),
                          "`var` variable '%s' with type Auto must be initialised", name);
                return false;
            }
        }
    } else {
        if (!node->default_value) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)strlen(name),
                      "Variable '%s' requires a type annotation or initialiser", name);
            return false;
        }
        TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
        if (!init_res.valid) return false;
        dt = init_res.type;
        if (dt == TYPE_AUTO || dt == TYPE_UNKNOWN || dt == TYPE_VOID) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                      "Cannot deduce usable type for '%s'", name);
            return false;
        }
    }

    if (dt == TYPE_UNKNOWN) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Unknown type for variable '%s'", name);
        return false;
    }

    if (has_modifier(state_mod, "fixed") || has_modifier(acc_mod, "fixed")) {
        if (dt != TYPE_INT && dt != TYPE_REAL && dt != TYPE_CHAR) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)strlen(name),
                      "Modifier 'fixed' can only be applied to variables of type Int, Real, or Char");
            return false;
        }
        if (is_none_literal(node->default_value)) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)strlen(name),
                      "Modifier 'fixed' cannot be used with initialiser 'none'");
            return false;
        }
        if (!node->default_value) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                      (uint8_t)strlen(name),
                      "Modifier 'fixed' requires an initialiser");
            return false;
        }
    }

    /* Treat `none` as a special initialiser that leaves the variable uninitialised. */
    if (is_none_literal(node->default_value)) {
        const char *mods = get_default_modifiers(dt);
        if (!semantic__add_variable_ex(ctx, ctx->current_scope,
                                       name, dt, tinfo,
                                       false, INIT_UNINITIALIZED,
                                       node->line, node->column, mods))
            return false;
        if (has_modifier(state_mod, "fixed") || has_modifier(acc_mod, "fixed")) {
            SymbolEntry *entry = find_symbol_in_table(ctx->current_scope, name);
            if (entry) entry->is_fixed = true;
        }
        return true;
    }

    if (node->default_value) {
        TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
        if (!init_res.valid) return false;
        if (!can_implicitly_convert(ctx, init_res.type, dt)) {
            if (dt == TYPE_COMPOUND && node->default_value->type == AST_MULTI_INITIALIZER) {
                SymbolEntry *struct_entry = semantic__find_symbol(ctx, tinfo->name);
                if (struct_entry && struct_entry->type == TYPE_COMPOUND) {
                    return check_struct_initializer(ctx, node->default_value, struct_entry);
                }
            }
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                      "Cannot initialise '%s' (%s) with %s",
                      name, semantic__type_to_string(dt),
                      semantic__type_to_string(init_res.type));
            return false;
        }
        init_state = INIT_FULL;
    } else {
        init_state = INIT_DEFAULT;
    }

    const char *mods = get_default_modifiers(dt);
    if (!semantic__add_variable_ex(ctx, ctx->current_scope,
                                   name, dt, tinfo,
                                   false, init_state,
                                   node->line, node->column, mods))
        return false;

    /* Mark the variable as fixed if the modifier is present. */
    if (has_modifier(state_mod, "fixed") || has_modifier(acc_mod, "fixed")) {
        SymbolEntry *entry = find_symbol_in_table(ctx->current_scope, name);
        if (entry) entry->is_fixed = true;
    }
    return true;
}

/*
 * Process a struct type definition.
 *
 * Supports `def Name : Struct { ... }` and `Name : Struct { ... }`.
 * Members can appear as AST_VARIABLE_DECLARATION or AST_CAST (identifier
 * : type).  Both forms are handled transparently.
 */
static bool process_struct_definition(SemanticContext *ctx, ASTNode *node) {
    const char *name = node->value;
    if (!name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Struct definition missing name");
        return false;
    }

    if (check_name_collision_in_current_scope(ctx, name, node->line, node->column))
        return false;
    warn_if_shadowing(ctx, name, node->line, node->column);

    ASTNode *block_node = node->right;   /* struct body block */
    if (!block_node || block_node->type != AST_BLOCK) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Struct '%s' requires a member block", name);
        return false;
    }

    AST *member_list = (AST *)block_node->extra;
    size_t member_count = member_list ? member_list->count : 0;

    CompoundMember *head = NULL, *tail = NULL;

    for (uint16_t i = 0; i < member_count; i++) {
        ASTNode *mnode = member_list->nodes[i];
        if (!mnode) continue;

        const char *mname = NULL;
        Type *mtinfo = NULL;

        /* Accept both VARIABLE_DECLARATION and CAST representations. */
        if (mnode->type == AST_VARIABLE_DECLARATION) {
            mname = mnode->value;
            mtinfo = mnode->variable_type;
        } else if (mnode->type == AST_CAST) {
            ASTNode *id_node = mnode->left;
            if (id_node && id_node->type == AST_IDENTIFIER) {
                mname = id_node->value;
            }
            mtinfo = mnode->variable_type;
        } else {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                      mnode->line, mnode->column, 0,
                      "Invalid struct member declaration");
            continue;
        }

        if (!mname || !mtinfo) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                      mnode->line, mnode->column, 0,
                      "Struct member must have a name and type");
            continue;
        }

        DataType mtype = type_from_type_info(mtinfo, ctx);
        if (mtype == TYPE_AUTO || mtype == TYPE_UNKNOWN || mtype == TYPE_VOID) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                      mnode->line, mnode->column, 0,
                      "Invalid type for struct member '%s'", mname);
            continue;
        }

        const char *member_mods = get_default_modifiers(mtype);

        CompoundMember *member = calloc(1, sizeof(CompoundMember));
        if (!member) continue;
        member->name = u__strduplic(mname);
        member->access_modifier = u__strduplic(member_mods);
        member->type = mtype;
        member->type_info = mtinfo;
        member->init_state = INIT_UNINITIALIZED;
        member->next = NULL;
        if (!head) head = tail = member;
        else { tail->next = member; tail = member; }
    }

    SymbolEntry *entry = calloc(1, sizeof(SymbolEntry));
    if (!entry) {
        while (head) {
            CompoundMember *n = head->next;
            free(head->name); free(head->access_modifier); free(head);
            head = n;
        }
        return false;
    }
    entry->name = u__strduplic(name);
    entry->access_modifier = NULL;
    entry->type = TYPE_COMPOUND;
    entry->type_info = NULL;
    entry->is_constant = true;
    entry->init_state = INIT_CONSTANT;
    entry->declared_scope = ctx->current_scope->level;
    entry->line = node->line;
    entry->column = node->column;
    entry->extra.compound_members = head;
    entry->compound_scope = NULL;
    entry->is_mutable = false;

    if (!add_symbol_to_table(ctx->current_scope, entry)) {
        free(entry->name);
        while (head) {
            CompoundMember *n = head->next;
            free(head->name); free(head->access_modifier); free(head);
            head = n;
        }
        free(entry);
        return false;
    }
    return true;
}

/*
 * Handle a label statement: `labelName:`.
 *
 * A label is a jump target (for future goto support).  It is added to the
 * current scope as a symbol of type TYPE_LABEL.  Duplicate labels in the
 * same function are not allowed (we use the current scope for simplicity
 * on function level; a more precise check would be per function, but here
 * we only check the innermost scope to avoid immediate collisions).
 */
static bool process_label_statement(SemanticContext *ctx, ASTNode *node) {
    /* The label name is stored in node->value. */
    const char *name = node->value;
    if (!name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Label requires a name");
        return false;
    }

    if (check_name_collision_in_current_scope(ctx, name, node->line, node->column))
        return false;
    warn_if_shadowing(ctx, name, node->line, node->column);

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
    entry->is_used = false;   /* will be set to true when referenced by a goto */

    if (!add_symbol_to_table(ctx->current_scope, entry)) {
        free(entry->name);
        free(entry);
        return false;
    }
    return true;
}

/*
 * Handle a type‑switch expression: `id : NewType;`.
 * Reassigns the type of an existing mutable variable.
 */
static bool handle_type_switch(SemanticContext *ctx, ASTNode *cast_node) {
    if (!cast_node || cast_node->type != AST_CAST) return false;
    ASTNode *id_node = cast_node->left;
    if (!IS_AST_TYPE(id_node, AST_IDENTIFIER)) return false;

    const char *name = id_node->value;
    SymbolEntry *sym = semantic__find_symbol(ctx, name);
    if (!sym) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, cast_node->line, cast_node->column,
                  (uint8_t)strlen(name),
                  "Cannot change type of undeclared variable '%s'", name);
        return false;
    }
    if (sym->type == TYPE_FUNCTION || sym->type == TYPE_COMPOUND || sym->type == TYPE_LABEL) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_SWITCH_NON_VAR, cast_node->line, cast_node->column,
                  (uint8_t)strlen(name),
                  "Type switch can only be applied to variables, not '%s'", name);
        return false;
    }
    if (sym->is_constant) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_SWITCH_CONST, cast_node->line, cast_node->column,
                  (uint8_t)strlen(name),
                  "Cannot change type of constant '%s'", name);
        return false;
    }

    Type *new_tinfo = cast_node->variable_type;
    if (!new_tinfo) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, cast_node->line, cast_node->column, 0,
                  "Missing type annotation in type switch");
        return false;
    }

    DataType new_dt = type_from_type_info(new_tinfo, ctx);
    if (new_dt == TYPE_UNKNOWN) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, cast_node->line, cast_node->column, 0,
                  "Unknown type in type switch");
        return false;
    }

    sym->type = new_dt;
    sym->type_info = new_tinfo;
    sym->init_state = INIT_UNINITIALIZED;
    return true;
}

/*
 * Process a function declaration or definition.
 *
 * Modifier rules:
 *   - `inline` is allowed only on `def` functions (must have a body).
 *   - `fixed` is forbidden on functions (it is a variable‑only modifier).
 *
 * The parameter list may contain both VARIABLE_DECLARATION and LITERAL_VALUE
 * nodes (for unnamed parameters).  Both are handled uniformly.
 */
static bool process_function_declaration(SemanticContext *ctx, ASTNode *node) {
    const char *name = node->value;
    const char *state_mod = node->state_modifier;
    const char *acc_mod   = node->access_modifier;

    if (!name) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                  "Function declaration missing name");
        return false;
    }

    /* `fixed` is never allowed on functions. */
    if (has_modifier(state_mod, "fixed") || has_modifier(acc_mod, "fixed")) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Modifier 'fixed' cannot be applied to function '%s'", name);
        return false;
    }

    /* Determine the real declaration kind (def / pro / del). */
    const char *decl_kind = NULL;
    if (state_mod) {
        if (strcmp(state_mod, "def") == 0 || strcmp(state_mod, "pro") == 0 ||
            strcmp(state_mod, "del") == 0) {
            decl_kind = state_mod;
        } else if (strcmp(state_mod, "inline") == 0) {
            /* inline alone implies `pro`. */
        }
    }
    if (decl_kind == NULL) decl_kind = "pro";

    /* `inline` modifier requires a body (def). */
    if ((has_modifier(state_mod, "inline") || has_modifier(acc_mod, "inline")) &&
        strcmp(decl_kind, "def") != 0) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Modifier 'inline' can only be used on 'def' functions with a body");
        return false;
    }

    ASTNode *params_node = node->left;          /* AST_BLOCK of parameters */
    ASTNode *body_node   = node->right;         /* Function body (NULL for `pro`) */
    Type    *ret_tinfo   = node->variable_type;

    DataType ret_type = TYPE_VOID;
    if (ret_tinfo) {
        ret_type = type_from_type_info(ret_tinfo, ctx);
        if (ret_type == TYPE_AUTO) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                      "Function '%s' cannot have return type Auto", name);
            return false;
        }
        if (ret_type == TYPE_UNKNOWN) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                      "Unknown return type for function '%s'", name);
            return false;
        }
    }

    FunctionParam *params = NULL, *last = NULL;
    size_t param_count = 0, required_count = 0;

    /* Parse the parameter list. */
    if (params_node && params_node->type == AST_BLOCK && params_node->extra) {
        AST *param_list = (AST *)params_node->extra;

        /* A single `Void` parameter means zero parameters. */
        if (param_list->count == 1) {
            ASTNode *single = param_list->nodes[0];
            if (single && single->type == AST_LITERAL_VALUE && single->value &&
                strcmp(single->value, "Void") == 0) {
                param_list = NULL;
            }
        }

        if (param_list) {
            for (uint16_t i = 0; i < param_list->count; i++) {
                ASTNode *p = param_list->nodes[i];
                if (!p) continue;

                const char *pname = NULL;
                Type *ptinfo = NULL;
                ASTNode *pdefault = NULL;
                DataType ptype = TYPE_VOID;

                /* Accept both named (VARIABLE_DECLARATION) and unnamed (LITERAL_VALUE) forms. */
                if (p->type == AST_VARIABLE_DECLARATION) {
                    pname = p->value;
                    ptinfo = p->variable_type;
                    pdefault = p->default_value;
                } else if (p->type == AST_LITERAL_VALUE) {
                    pname = NULL;
                    DataType dt = string_to_datatype(p->value);
                    if (dt == TYPE_UNKNOWN) {
                        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, p->line, p->column, 0,
                                  "Unknown type '%s' for unnamed parameter", p->value);
                        continue;
                    }
                    ptinfo = calloc(1, sizeof(Type));
                    if (ptinfo) {
                        ptinfo->name = u__strduplic(p->value);
                    }
                    ptype = dt;
                } else {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR,
                              p->line, p->column, 0,
                              "Invalid parameter syntax");
                    continue;
                }

                if (ptinfo && ptype == TYPE_VOID) {
                    ptype = type_from_type_info(ptinfo, ctx);
                    if (ptype == TYPE_AUTO && !pdefault) {
                        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, p->line, p->column, 0,
                                  "Parameter '%s' has type Auto but no default value",
                                  pname ? pname : "(unnamed)");
                        ptype = TYPE_UNKNOWN;
                    }
                }

                if (ptype == TYPE_UNKNOWN) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, p->line, p->column, 0,
                              "Unknown type for parameter '%s'",
                              pname ? pname : "(unnamed)");
                }

                FunctionParam *param = malloc(sizeof(FunctionParam));
                param->name = pname ? u__strduplic(pname) : NULL;
                param->type = ptype;
                param->type_info = ptinfo;
                param->default_value = pdefault;
                param->next = NULL;
                if (!params) params = param;
                else last->next = param;
                last = param;
                param_count++;
                if (!pdefault) required_count++;
            }
        }
    }

    /* `del` on functions is not allowed. */
    if (strcmp(decl_kind, "del") == 0) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_DEL_INVALID_TARGET, node->line, node->column,
                  (uint8_t)strlen(name),
                  "`del` cannot be applied to functions");
        while (params) { FunctionParam *n = params->next; free(params->name); free(params); params = n; }
        return false;
    }

    if (strcmp(decl_kind, "pro") == 0) {
        if (body_node) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_PRO_FUNC_HAS_BODY, node->line, node->column,
                      (uint8_t)strlen(name),
                      "Prototype '%s' must not have a body", name);
            while (params) { FunctionParam *n = params->next; free(params->name); free(params); params = n; }
            return false;
        }
        for (FunctionParam *p = params; p; p = p->next) {
            if (p->default_value) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_PROTO_PARAM_DEFAULT, node->line, node->column,
                          (uint8_t)strlen(name),
                          "Prototype parameter '%s' of '%s' must not have a default value",
                          p->name ? p->name : "(unnamed)", name);
                while (params) { FunctionParam *n = params->next; free(params->name); free(params); params = n; }
                return false;
            }
        }
        return semantic__add_function_ex(ctx, ctx->current_scope, name,
                                         ret_type, ret_tinfo,
                                         params, param_count, required_count,
                                         false,
                                         node->line, node->column, NULL,
                                         false, false);
    }

    /* `def` function: body is mandatory and must not be `none`. */
    if (!body_node) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_DEF_FUNC_MISSING_BODY, node->line, node->column,
                  (uint8_t)strlen(name),
                  "`def` function '%s' must have a body", name);
        while (params) { FunctionParam *n = params->next; free(params->name); free(params); params = n; }
        return false;
    }
    if (body_node->type == AST_LITERAL_VALUE && body_node->operation_type == TOKEN_NONE) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_DEF_FUNC_NONE_BODY, node->line, node->column,
                  (uint8_t)strlen(name),
                  "`def` function '%s' cannot have a 'none' body", name);
        while (params) { FunctionParam *n = params->next; free(params->name); free(params); params = n; }
        return false;
    }

    bool added = semantic__add_function_ex(ctx, ctx->current_scope, name,
                                           ret_type, ret_tinfo,
                                           params, param_count, required_count,
                                           false,
                                           node->line, node->column, NULL,
                                           true, false);
    if (!added) return false;

    SymbolEntry *func_entry = semantic__find_symbol(ctx, name);
    if (!func_entry || func_entry->type != TYPE_FUNCTION) return false;

    /* Mark function as inline. */
    if (has_modifier(state_mod, "inline") || has_modifier(acc_mod, "inline"))
        func_entry->is_inline = true;

    FunctionSignature *sig = func_entry->extra.func_sig;
    semantic__enter_function_scope(ctx, name, ret_type);

    /* Add parameters to the function scope. */
    for (FunctionParam *p = sig->params; p; p = p->next) {
        if (p->name) {
            const char *param_mods = get_default_modifiers(p->type);
            semantic__add_variable_ex(ctx, ctx->current_scope,
                                      p->name, p->type, p->type_info,
                                      false, INIT_FULL,
                                      node->line, node->column, param_mods);
        }
    }

    /* Check default values of parameters. */
    for (FunctionParam *p = sig->params; p; p = p->next) {
        if (p->default_value) {
            TypeCheckResult def_res = semantic__check_type(ctx, p->default_value);
            if (!def_res.valid || !can_implicitly_convert(ctx, def_res.type, p->type)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Invalid default value for parameter '%s'",
                          p->name ? p->name : "(unnamed)");
            }
        }
    }

    bool body_ok = semantic__check_statement(ctx, body_node);

    if (!contains_return(body_node)) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_MISSING_RETURN, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Function '%s' does not contain a return statement", name);
        body_ok = false;
    }

    if (ret_type != TYPE_VOID && !statement_ensures_return(ctx, body_node)) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_MISSING_RETURN, node->line, node->column,
                  (uint8_t)strlen(name),
                  "Non‑void function '%s' must return a value on every path", name);
        body_ok = false;
    }

    semantic__exit_function_scope(ctx);
    return body_ok;
}

/*
 * Type‑check a single AST node and return its type information.
 *
 * Handles literals (including `none`), identifiers, field access, and
 * multi‑initializers.  For identifiers, the symbol is marked as used and
 * any use‑before‑init error is emitted.
 */
TypeCheckResult semantic__check_type(SemanticContext *ctx, ASTNode *node) {
    TypeCheckResult res = { false, TYPE_UNKNOWN, NULL, INIT_UNINITIALIZED, NULL };
    if (!node) { res.error_msg = u__strduplic("Null node"); return res; }

    switch (node->type) {
        case AST_LITERAL_VALUE: {
            TokenType tt = node->operation_type;
            if (tt == TOKEN_NUMBER) {
                size_t len = strlen(node->value);
                char last = len ? node->value[len - 1] : 0;
                if (last == 'i' || last == 'I')          res.type = TYPE_INT;
                else if (last == 'f' || last == 'F')     res.type = TYPE_REAL;
                else if (strchr(node->value, '.') ||
                         strchr(node->value, 'e') ||
                         strchr(node->value, 'E'))       res.type = TYPE_REAL;
                else                                     res.type = TYPE_INT;
                res.init_state = INIT_CONSTANT;
                res.valid = true;
            } else if (tt == TOKEN_CHAR) {
                res.type = TYPE_CHAR; res.init_state = INIT_CONSTANT; res.valid = true;
            } else if (tt == TOKEN_STRING) {
                /* Single‑character string literals (length 3 with quotes) → Char. */
                if (node->value && strlen(node->value) == 3 && node->value[0] == '\'') {
                    res.type = TYPE_CHAR; res.init_state = INIT_CONSTANT; res.valid = true;
                } else {
                    res.valid = false; res.error_msg = u__strduplic("String literal not allowed here");
                }
            } else if (tt == TOKEN_NONE) {
                /* The `none` literal is of type Void and always valid. */
                res.type = TYPE_VOID;
                res.init_state = INIT_UNINITIALIZED;
                res.valid = true;
            } else {
                if (node->value && is_type_name(node->value)) {
                    res.type = string_to_datatype(node->value);
                    res.init_state = INIT_CONSTANT; res.valid = true;
                } else {
                    res.valid = false; res.error_msg = u__strduplic("Invalid literal");
                }
            }
            break;
        }

        case AST_IDENTIFIER: {
            if (!node->value) { res.valid = false; res.error_msg = u__strduplic("Null identifier"); break; }
            SymbolEntry *sym = semantic__find_symbol(ctx, node->value);
            if (!sym) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_UNDECLARED_SYMBOL, node->line, node->column,
                          (uint8_t)strlen(node->value),
                          "Undeclared identifier '%s'", node->value);
                res.valid = false; res.error_msg = u__strduplic("Undeclared identifier");
            } else {
                if (sym->type == TYPE_FUNCTION || sym->type == TYPE_COMPOUND ||
                    sym->type == TYPE_LABEL) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_INVALID_OPERATION, node->line, node->column,
                              (uint8_t)strlen(node->value),
                              "Cannot use '%s' as a value", node->value);
                    res.valid = false; break;
                }
                res.type = sym->type;
                res.type_info = sym->type_info;
                res.init_state = sym->init_state;
                res.valid = true;
                sym->is_used = true;
                /*
                 * Use‑before‑init check.
                 * Raw struct variables (not their members) are exempt because
                 * `none` initialisation leaves them uninitialised and we want
                 * to allow e.g. `obj.member = ...` without triggering an error
                 * on `obj`.
                 */
                if (sym->init_state == INIT_UNINITIALIZED &&
                    sym->type != TYPE_FUNCTION && sym->type != TYPE_COMPOUND &&
                    sym->type != TYPE_LABEL) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_UNINITIALIZED, node->line, node->column,
                              (uint8_t)strlen(node->value),
                              "Use of uninitialized variable '%s'", node->value);
                    res.valid = false;
                }
            }
            break;
        }

        case AST_FIELD_ACCESS: {
            if (!HAS_LEFT(node) || !node->right) {
                res.valid = false; res.error_msg = u__strduplic("Invalid member access"); break;
            }
            DataType member_type;
            Type *member_type_info = NULL;
            CompoundMember *member = NULL;
            if (!resolve_member_access(ctx, node, &member_type, &member_type_info,
                                       NULL, &member)) {
                res.valid = false; break;
            }
            res.type = member_type;
            res.type_info = member_type_info;
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

        default: res.valid = true; break;
    }
    return res;
}

/*
 * Main statement dispatcher.
 *
 * Routes variable declarations, function declarations, blocks, if/loop
 * statements, assignments, labels, and expression statements to their
 * respective handlers.  Also enforces that assignments to a constant
 * variable or an invalid struct field are rejected.
 */
bool semantic__check_statement(SemanticContext *ctx, ASTNode *node) {
    if (!node) return true;

    switch (node->type) {
        case AST_VARIABLE_DECLARATION: {
            const char *mod = node->state_modifier;

            /* Detect a struct definition: type name "Struct" + a block body. */
            if (node->variable_type && node->variable_type->name &&
                strcmp(node->variable_type->name, "Struct") == 0 &&
                node->right && node->right->type == AST_BLOCK) {
                return process_struct_definition(ctx, node);
            }

            if (!mod || strcmp(mod, "var") == 0)
                return process_var_variable(ctx, node);

            if (strcmp(mod, "def") == 0) return process_def_variable(ctx, node);
            if (strcmp(mod, "pro") == 0) return process_pro_variable(ctx, node);
            if (strcmp(mod, "del") == 0) return process_del_variable(ctx, node);
            if (strcmp(mod, "member") == 0) return true; /* handled by struct processor */

            return process_var_variable(ctx, node);
        }

        case AST_FUNCTION_DECLARATION:
            return process_function_declaration(ctx, node);

        case AST_BLOCK: {
            semantic__enter_scope(ctx);
            AST *list = (AST *)node->extra;
            bool ok = true;
            if (list) {
                for (uint16_t i = 0; i < list->count; i++)
                    if (!semantic__check_statement(ctx, list->nodes[i])) ok = false;
            }
            semantic__exit_scope(ctx);
            return ok;
        }

        case AST_IF_STATEMENT: {
            TypeCheckResult cond = semantic__check_type(ctx, node->left);
            if (!cond.valid) return false;
            if (!is_condition_true(&cond)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "If condition must be numeric or pointer");
                return false;
            }
            semantic__enter_scope(ctx);
            bool then_ok = semantic__check_statement(ctx, node->right);
            semantic__exit_scope(ctx);
            bool else_ok = true;
            if (node->extra) {
                semantic__enter_scope(ctx);
                else_ok = semantic__check_statement(ctx, node->extra);
                semantic__exit_scope(ctx);
            }
            return then_ok && else_ok;
        }

        case AST_DO_LOOP: {
            TypeCheckResult cond = semantic__check_type(ctx, node->left);
            if (!cond.valid) return false;
            if (!is_condition_true(&cond)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Do loop condition must be numeric or pointer");
                return false;
            }
            semantic__enter_loop_scope(ctx);
            bool body_ok = semantic__check_statement(ctx, node->right);
            semantic__exit_loop_scope(ctx);
            bool else_ok = true;
            if (node->extra) {
                semantic__enter_scope(ctx);
                else_ok = semantic__check_statement(ctx, node->extra);
                semantic__exit_scope(ctx);
            }
            return body_ok && else_ok;
        }

        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT: {
            ASTNode *lhs = node->left;
            ASTNode *rhs = node->right;
            if (!lhs || !rhs) return true;

            /* Resolve the left‑hand side type. */
            TypeCheckResult lhs_res = semantic__check_type(ctx, lhs);
            if (!lhs_res.valid) return false;

            /*
             * Check that the left side is assignable.
             * For a plain identifier, the symbol must be mutable.
             * For a field access (struct member), the struct variable itself
             * must be mutable.
             */
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
                while (base && base->type == AST_FIELD_ACCESS)
                    base = base->left;
                if (base && base->type == AST_IDENTIFIER) {
                    SymbolEntry *sym = semantic__find_symbol(ctx, base->value);
                    if (sym && !sym->is_mutable) {
                        SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, lhs->line, lhs->column,
                                  (uint8_t)strlen(base->value),
                                  "Cannot assign to field of constant struct '%s'", base->value);
                        return false;
                    }
                }
            }

            /* Struct literal assignment to a struct variable: validate initializer. */
            if (lhs->type == AST_IDENTIFIER && lhs_res.type == TYPE_COMPOUND &&
                rhs->type == AST_MULTI_INITIALIZER) {
                SymbolEntry *struct_def = NULL;
                if (lhs_res.type_info && lhs_res.type_info->name)
                    struct_def = semantic__find_symbol(ctx, lhs_res.type_info->name);
                if (struct_def && struct_def->type == TYPE_COMPOUND) {
                    return check_struct_initializer(ctx, rhs, struct_def);
                }
            }

            /* Type‑check the right‑hand side and verify compatibility. */
            TypeCheckResult rhs_res = semantic__check_type(ctx, rhs);
            if (!rhs_res.valid) return false;

            if (!can_implicitly_convert(ctx, rhs_res.type, lhs_res.type)) {
                SEM_ERROR(ctx, ERROR_CODE_SEM_TYPE_ERROR, node->line, node->column, 0,
                          "Cannot assign %s to %s",
                          semantic__type_to_string(rhs_res.type),
                          semantic__type_to_string(lhs_res.type));
                return false;
            }
            return true;
        }

        case AST_LABEL_DECLARATION:
            return process_label_statement(ctx, node);

        default: return semantic__check_expression(ctx, node);
    }
}

/*
 * Check an expression (typically a function call or computed value) for
 * type correctness.
 */
bool semantic__check_expression(SemanticContext *ctx, ASTNode *node) {
    if (!node) return true;
    TypeCheckResult res = semantic__check_type(ctx, node);
    if (!res.valid) { ctx->has_errors = true; free(res.error_msg); return false; }
    free(res.error_msg);
    return true;
}

/*
 * Final verification pass.
 *
 * Reports:
 *   - Functions declared but never defined (error).
 *   - Unused functions (warning, only with -Wextra, excluding main).
 *   - Unused variables (warning, only with -Wextra).
 *   - Exactly one `main` function with a body must exist (error).
 */
static bool final_verification(SemanticContext *ctx) {
    bool ok = true;
    SymbolTable *global = ctx->global_scope;
    int main_count = 0;

    for (size_t i = 0; i < global->capacity; i++) {
        for (SymbolEntry *entry = global->entries[i]; entry; entry = entry->next) {
            if (entry->type == TYPE_FUNCTION) {
                FunctionSignature *sig = entry->extra.func_sig;
                /* Error: function used but never defined. */
                if (entry->is_used && !sig->has_body && !sig->is_none_body) {
                    SEM_ERROR(ctx, ERROR_CODE_SEM_UNDEFINED_VAR,
                              entry->line, entry->column,
                              (uint8_t)strlen(entry->name),
                              "Function '%s' declared but never defined", entry->name);
                    ok = false;
                }
                /* Count main functions with a body. */
                if (strcmp(entry->name, "main") == 0 && sig->has_body)
                    main_count++;

                /* Warning: unused function (only with -Wextra). */
                if (!entry->is_used && ctx->extra_warnings &&
                    strcmp(entry->name, "main") != 0) {
                    SEM_WARNING(ctx, ERROR_CODE_SEM_UNUSED_VARIABLE,
                                entry->line, entry->column,
                                (uint8_t)strlen(entry->name),
                                "Function '%s' declared but never used", entry->name);
                }
            } else if (entry->type == TYPE_COMPOUND) {
                /* Structs are not required to be used. */
            } else if (!entry->is_used && ctx->extra_warnings &&
                       entry->type != TYPE_LABEL && entry->type != TYPE_FUNCTION) {
                SEM_WARNING(ctx, ERROR_CODE_SEM_UNUSED_VARIABLE,
                            entry->line, entry->column,
                            (uint8_t)strlen(entry->name),
                            "Variable '%s' declared but never used", entry->name);
            }
        }
    }

    /* Enforce that exactly one main function exists. */
    if (main_count == 0) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_UNDEFINED_VAR, 0, 0, 0,
                  "No 'main' function with a body defined");
        ok = false;
    } else if (main_count > 1) {
        SEM_ERROR(ctx, ERROR_CODE_SEM_REDECLARATION, 0, 0, 0,
                  "Multiple definitions of 'main' function");
        ok = false;
    }

    return ok;
}

/* Main analysis entry point. */
bool semantic__analyze(SemanticContext *ctx, AST *ast) {
    if (!ctx || !ast) return false;
    ctx->has_errors = false;
    ctx->abort_compilation = false;

    for (uint16_t i = 0; i < ast->count; i++) {
        if (!semantic__check_statement(ctx, ast->nodes[i])) {
            ctx->has_errors = true;
        }
    }

    if (!final_verification(ctx))
        ctx->has_errors = true;

    return !ctx->has_errors;
}

/* Lifecycle – create / destroy context. */
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
    return ctx;
}

void semantic__destroy_context(SemanticContext *ctx) {
    if (!ctx) return;
    destroy_symbol_table(ctx->global_scope);
    free(ctx);
}

/* Enable / disable extra warnings. */
void semantic__set_extra_warnings(SemanticContext *ctx, bool enable) {
    if (ctx) ctx->extra_warnings = enable;
}

/* Should the compilation be aborted? */
bool semantic__should_abort(const SemanticContext *ctx) {
    return ctx ? ctx->abort_compilation : false;
}

/* Scope management functions. */
void semantic__enter_scope_ex(SemanticContext *ctx, ScopeLevel level) {
    if (!ctx) return;
    SymbolTable *new_scope = create_symbol_table(ctx->current_scope, level);
    if (new_scope) {
        ctx->current_scope = new_scope;
        if (level == SCOPE_LOOP) ctx->in_loop = true;
        if (level == SCOPE_FUNCTION) ctx->in_function = true;
    }
}
void semantic__enter_scope(SemanticContext *ctx) { semantic__enter_scope_ex(ctx, SCOPE_BLOCK); }
void semantic__exit_scope(SemanticContext *ctx) {
    if (!ctx || !ctx->current_scope || ctx->current_scope == ctx->global_scope) return;
    SymbolTable *old = ctx->current_scope;
    if (old->level == SCOPE_LOOP) ctx->in_loop = false;
    if (old->level == SCOPE_FUNCTION) { ctx->in_function = false; ctx->current_function = NULL; }
    ctx->current_scope = old->parent;
}
void semantic__enter_function_scope(SemanticContext *ctx, const char *name, DataType ret) {
    semantic__enter_scope_ex(ctx, SCOPE_FUNCTION);
    ctx->current_function = name;
    ctx->current_return_type = ret;
}
void semantic__exit_function_scope(SemanticContext *ctx) { semantic__exit_scope(ctx); }
void semantic__enter_loop_scope(SemanticContext *ctx) { semantic__enter_scope_ex(ctx, SCOPE_LOOP); }
void semantic__exit_loop_scope(SemanticContext *ctx) { semantic__exit_scope(ctx); }

/*
 * Add a variable symbol to the given scope (or current scope).
 * Checks for redeclaration and shadowing automatically.
 */
bool semantic__add_variable_ex(SemanticContext *ctx, SymbolTable *target_scope,
                               const char *name, DataType type, Type *type_info,
                               bool is_constant, InitState init_state,
                               uint16_t line, uint16_t column,
                               const char *access_modifier) {
    if (!ctx || !name) return false;
    SymbolTable *scope = target_scope ? target_scope : ctx->current_scope;
    if (check_name_collision_in_current_scope(ctx, name, line, column)) return false;
    warn_if_shadowing(ctx, name, line, column);

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
    /* is_inline and is_fixed are left as false; they will be set by the
       specific variable handler if appropriate. */
    if (!add_symbol_to_table(scope, entry)) {
        free(entry->name); free(entry->access_modifier); free(entry);
        return false;
    }
    return true;
}

/*
 * Add a function symbol to the given scope.
 * If a forward declaration already exists, its signature is checked and
 * the body flag is updated.  Otherwise a new entry is created.
 */
bool semantic__add_function_ex(SemanticContext *ctx, SymbolTable *target_scope,
                               const char *name,
                               DataType return_type, Type *return_type_info,
                               FunctionParam *params, size_t param_count,
                               size_t required_count, bool is_variadic,
                               uint16_t line, uint16_t column,
                               const char *access_modifier,
                               bool has_body, bool is_none_body) {
    if (!ctx || !name) return false;
    SymbolTable *scope = target_scope ? target_scope : ctx->current_scope;

    SymbolEntry *existing = find_symbol_in_table(scope, name);
    if (existing) {
        if (existing->type != TYPE_FUNCTION) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_FUNC_DIFFERENT_KIND, line, column,
                      (uint8_t)strlen(name),
                      "Redeclaration of '%s' as a different symbol kind", name);
            return false;
        }
        FunctionSignature new_sig = {
            .return_type = return_type, .return_type_info = return_type_info,
            .params = params, .param_count = param_count,
            .required_param_count = required_count, .is_variadic = is_variadic,
            .has_body = has_body, .is_none_body = is_none_body
        };
        if (!check_prototype_match(ctx, existing, &new_sig, line, column)) return false;
        FunctionSignature *old_sig = existing->extra.func_sig;
        if (has_body && old_sig->has_body) {
            SEM_ERROR(ctx, ERROR_CODE_SEM_FUNC_DUPLICATE_BODY, line, column,
                      (uint8_t)strlen(name),
                      "Function '%s' already has a body", name);
            return false;
        }
        if (has_body && !old_sig->has_body) {
            old_sig->has_body = true;
            old_sig->is_none_body = is_none_body;
        }
        /* Retain is_inline flag if already set. */
        return true;
    }

    warn_if_shadowing(ctx, name, line, column);
    FunctionParam *params_copy = clone_function_params(params);
    if (!params_copy && params != NULL) return false;

    FunctionSignature *sig = calloc(1, sizeof(FunctionSignature));
    if (!sig) {
        while (params_copy) { FunctionParam *n = params_copy->next; free(params_copy->name); free(params_copy); params_copy = n; }
        return false;
    }
    sig->return_type = return_type; sig->return_type_info = return_type_info;
    sig->params = params_copy; sig->param_count = param_count;
    sig->required_param_count = required_count; sig->is_variadic = is_variadic;
    sig->has_body = has_body; sig->is_none_body = is_none_body;

    SymbolEntry *entry = calloc(1, sizeof(SymbolEntry));
    if (!entry) {
        while (params_copy) { FunctionParam *n = params_copy->next; free(params_copy->name); free(params_copy); params_copy = n; }
        free(sig); return false;
    }
    entry->name = u__strduplic(name);
    entry->access_modifier = access_modifier ? u__strduplic(access_modifier) : NULL;
    entry->type = TYPE_FUNCTION;
    entry->type_info = return_type_info;
    entry->is_constant = true;
    entry->init_state = INIT_CONSTANT;
    entry->declared_scope = scope->level;
    entry->line = line; entry->column = column;
    entry->extra.func_sig = sig;
    /* main is considered used by default to avoid unused warning. */
    if (strcmp(name, "main") == 0) entry->is_used = true;

    if (!add_symbol_to_table(scope, entry)) {
        free(entry->name); free(entry->access_modifier); free(entry);
        while (params_copy) { FunctionParam *n = params_copy->next; free(params_copy->name); free(params_copy); params_copy = n; }
        free(sig); return false;
    }
    return true;
}

/* Look up a symbol by name, traversing the scope chain. */
SymbolEntry *semantic__find_symbol(SemanticContext *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    SymbolTable *table = ctx->current_scope;
    while (table) {
        SymbolEntry *entry = find_symbol_in_table(table, name);
        if (entry) return entry;
        table = table->parent;
    }
    return NULL;
}

/* Convert a DataType to its human‑readable string. */
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
        case TYPE_LABEL:    return "label";
        default:            return "unknown";
    }
}

/* Number of symbols in the global scope. */
size_t semantic__get_symbol_count(SemanticContext *ctx) {
    return ctx && ctx->global_scope ? ctx->global_scope->count : 0;
}

/* True if any semantic error was recorded. */
bool semantic__has_errors(SemanticContext *ctx) {
    return ctx ? ctx->has_errors : false;
}
