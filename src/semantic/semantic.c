#include "semantic.h"
#include "../errhandler/errhandler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/** Size of the hash table (power of two for efficient modulo) */
#define HASH_TABLE_SIZE 64

/**
 * @brief Duplicate a string (C99-compatible replacement for strdup).
 * @param str String to duplicate.
 * @return Newly allocated copy, or NULL on failure.
 */
static char* duplicate_string(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* new_str = malloc(len + 1);
    if (!new_str) return NULL;
    memcpy(new_str, str, len);
    new_str[len] = '\0';
    return new_str;
}

/**
 * @brief FNV-1a hash function for symbol names.
 * @param str Input string.
 * @return 32-bit hash value.
 */
static uint32_t hash_string(const char* str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

/**
 * @brief Create a new symbol table.
 * @param parent Parent scope (may be NULL).
 * @param level Scope level.
 * @return Newly allocated table, or NULL on failure.
 */
static SymbolTable* create_symbol_table(SymbolTable* parent, ScopeLevel level) {
    SymbolTable* table = malloc(sizeof(SymbolTable));
    if (!table) return NULL;

    table->entries = calloc(HASH_TABLE_SIZE, sizeof(SymbolEntry*));
    if (!table->entries) {
        free(table);
        return NULL;
    }

    table->capacity = HASH_TABLE_SIZE;
    table->count = 0;
    table->level = level;
    table->parent = parent;
    table->children = NULL;
    table->next_child = NULL;

    /* Add to parent's child list */
    if (parent) {
        table->next_child = parent->children;
        parent->children = table;
    }

    return table;
}

/**
 * @brief Recursively destroy a symbol table and all its children.
 * @param table Table to destroy.
 */
static void destroy_symbol_table(SymbolTable* table) {
    if (!table) return;

    /* Destroy all child scopes first */
    SymbolTable* child = table->children;
    while (child) {
        SymbolTable* next = child->next_child;
        destroy_symbol_table(child);
        child = next;
    }

    /* Free all symbol entries in this table */
    for (size_t i = 0; i < table->capacity; i++) {
        SymbolEntry* entry = table->entries[i];
        while (entry) {
            SymbolEntry* next = entry->next;

            /* Free function-specific data */
            if (entry->type == TYPE_FUNCTION && entry->extra.func_sig) {
                FunctionParam* param = entry->extra.func_sig->params;
                while (param) {
                    FunctionParam* next_param = param->next;
                    free(param->name);
                    free(param);
                    param = next_param;
                }
                free(entry->extra.func_sig);
            }
            /* Free struct debug members and the compound scope */
            else if (entry->type == TYPE_COMPOUND) {
                if (entry->extra.compound_members) {
                    CompoundMember* member = entry->extra.compound_members;
                    while (member) {
                        CompoundMember* next_member = member->next;
                        free(member->name);
                        free(member->state_modifier);
                        free(member);
                        member = next_member;
                    }
                }
                if (entry->compound_scope) {
                    destroy_symbol_table(entry->compound_scope);
                }
            }

            free(entry->name);
            free(entry->state_modifier);
            free(entry);
            entry = next;
        }
    }

    free(table->entries);
    free(table);
}

/**
 * @brief Find a symbol in a specific table (no parent traversal).
 * @param table Symbol table to search.
 * @param name Symbol name.
 * @return SymbolEntry pointer, or NULL if not found.
 */
static SymbolEntry* find_symbol_in_table(SymbolTable* table, const char* name) {
    if (!table || !name) return NULL;

    uint32_t index = hash_string(name) % table->capacity;
    SymbolEntry* entry = table->entries[index];

    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
        entry = entry->next;
    }

    return NULL;
}

/**
 * @brief Insert a symbol entry into a specific table.
 * @param table Target symbol table.
 * @param entry Symbol entry to insert.
 * @return true on success, false on failure.
 */
static bool add_symbol_to_table(SymbolTable* table, SymbolEntry* entry) {
    if (!table || !entry || !entry->name) return false;

    uint32_t index = hash_string(entry->name) % table->capacity;
    entry->next = table->entries[index];
    table->entries[index] = entry;
    table->count++;
    return true;
}

/**
 * @brief Determine DataType from a Type structure.
 * @param type_info Type structure (may be NULL).
 * @param context Semantic context (needed for user-defined type lookup).
 * @return Corresponding DataType.
 */
static DataType semantic_type_from_type_info(Type* type_info, SemanticContext* context) {
    if (!type_info) return TYPE_UNKNOWN;

    /* Pointer, reference, array, compound have precedence */
    if (type_info->pointer_level > 0) {
        return TYPE_POINTER;
    }
    if (type_info->is_reference) {
        return TYPE_REFERENCE;
    }
    if (type_info->is_array) {
        return TYPE_ARRAY;
    }
    if (type_info->compound_types && type_info->compound_count > 0) {
        return TYPE_COMPOUND;
    }

    /* Basic types or user-defined structs */
    if (type_info->name) {
        /* Check if this is a user-defined struct type */
        SymbolEntry* entry = semantic_find_symbol(context, type_info->name);
        if (entry && entry->type == TYPE_COMPOUND) {
            return TYPE_COMPOUND;
        }

        /* Built-in types */
        if (strcmp(type_info->name, "Int") == 0) return TYPE_INT;
        if (strcmp(type_info->name, "Real") == 0) return TYPE_REAL;
        if (strcmp(type_info->name, "Char") == 0) return TYPE_CHAR;
        if (strcmp(type_info->name, "String") == 0) return TYPE_STRING;
        if (strcmp(type_info->name, "Bool") == 0) return TYPE_BOOL;
        if (strcmp(type_info->name, "Void") == 0) return TYPE_VOID;
        if (strcmp(type_info->name, "none") == 0) return TYPE_NONE;
    }

    return TYPE_UNKNOWN;
}

SemanticContext* semantic_create_context(void) {
    SemanticContext* context = malloc(sizeof(SemanticContext));
    if (!context) return NULL;

    context->global_scope = create_symbol_table(NULL, SCOPE_GLOBAL);
    if (!context->global_scope) {
        free(context);
        return NULL;
    }

    context->current_scope = context->global_scope;
    context->function_scope = NULL;
    context->has_errors = false;
    context->warnings_enabled = true;
    context->exit_on_error = true;
    context->in_loop = false;
    context->in_function = false;
    context->current_function = NULL;
    context->current_return_type = TYPE_VOID;

    return context;
}

void semantic_destroy_context(SemanticContext* context) {
    if (!context) return;
    destroy_symbol_table(context->global_scope);
    free(context);
}

void semantic_set_exit_on_error(SemanticContext* context, bool exit_on_error) {
    if (context) {
        context->exit_on_error = exit_on_error;
    }
}

void semantic_enter_scope_ex(SemanticContext* context, ScopeLevel level) {
    SymbolTable* new_scope = create_symbol_table(context->current_scope, level);
    if (new_scope) {
        context->current_scope = new_scope;
        if (level == SCOPE_LOOP) {
            context->in_loop = true;
        }
    }
}

void semantic_enter_scope(SemanticContext* context) {
    semantic_enter_scope_ex(context, SCOPE_BLOCK);
}

void semantic_exit_scope(SemanticContext* context) {
    if (context->current_scope && context->current_scope != context->global_scope) {
        SymbolTable* old_scope = context->current_scope;

        /* Update context flags based on the scope we are leaving */
        if (old_scope->level == SCOPE_LOOP) {
            context->in_loop = false;
        }
        else if (old_scope->level == SCOPE_FUNCTION) {
            context->in_function = false;
            context->current_function = NULL;
            context->current_return_type = TYPE_VOID;
            context->function_scope = NULL;
        }

        context->current_scope = old_scope->parent;
        /* Do NOT issue warnings about uninitialized variables here */
    }
}

void semantic_enter_function_scope(SemanticContext* context,
                                   const char* function_name,
                                   DataType return_type) {
    semantic_enter_scope_ex(context, SCOPE_FUNCTION);
    context->in_function = true;
    context->current_function = function_name;
    context->current_return_type = return_type;
    context->function_scope = context->current_scope;
}

void semantic_exit_function_scope(SemanticContext* context) {
    semantic_exit_scope(context);
}

void semantic_enter_loop_scope(SemanticContext* context) {
    semantic_enter_scope_ex(context, SCOPE_LOOP);
}

void semantic_exit_loop_scope(SemanticContext* context) {
    semantic_exit_scope(context);
}

bool semantic_add_variable_ex(SemanticContext* context, SymbolTable* target_scope,
                              const char* name, DataType type, Type* type_info,
                              bool is_constant, const char* state_modifier,
                              InitState init_state, uint16_t line, uint16_t column) {
    if (!context || !name) return false;

    SymbolTable* scope = target_scope ? target_scope : context->current_scope;

    /* Check for redeclaration in the same scope */
    SymbolEntry* existing = find_symbol_in_table(scope, name);
    if (existing) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_REDECLARATION,
            line, column, (uint8_t)strlen(name),
            "semantic",
            "Redeclaration of symbol '%s' (previous declaration at line %d)",
            name, existing->line
        );
        context->has_errors = true;
        return false;
    }

    /* Emit shadowing warning if applicable */
    if (context->warnings_enabled && scope == context->current_scope) {
        semantic_check_shadowing(context, name, line, column);
    }

    /* Allocate and initialize new symbol entry */
    SymbolEntry* entry = malloc(sizeof(SymbolEntry));
    if (!entry) return false;

    entry->name = duplicate_string(name);
    entry->state_modifier = state_modifier ? duplicate_string(state_modifier) : NULL;
    entry->type = type;
    entry->type_info = type_info;
    entry->is_constant = is_constant;
    entry->init_state = init_state;
    entry->is_used = false;            /* Unused warnings are disabled */
    entry->is_mutable = !is_constant;
    entry->declared_scope = scope->level;
    entry->line = line;
    entry->column = column;
    entry->extra.func_sig = NULL;
    entry->compound_scope = NULL;
    entry->next = NULL;

    if (!add_symbol_to_table(scope, entry)) {
        free(entry->name);
        free(entry->state_modifier);
        free(entry);
        return false;
    }

    return true;
}

bool semantic_add_variable(SemanticContext* context, const char* name,
                           DataType type, Type* type_info, bool is_constant,
                           uint16_t line, uint16_t column) {
    InitState init_state = is_constant ? INIT_CONSTANT : INIT_UNINITIALIZED;
    return semantic_add_variable_ex(context, context->current_scope,
                                    name, type, type_info,
                                    is_constant, NULL, init_state, line, column);
}

bool semantic_add_function_ex(SemanticContext* context, const char* name,
                              DataType return_type, Type* return_type_info,
                              FunctionParam* params, size_t param_count,
                              bool is_variadic, uint16_t line, uint16_t column) {
    if (!context || !name) return false;

    /* Check redeclaration in current scope */
    SymbolEntry* existing = find_symbol_in_table(context->current_scope, name);
    if (existing) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_REDECLARATION,
            line, column, (uint8_t)strlen(name),
            "semantic",
            "Redeclaration of function '%s' (previous declaration at line %d)",
            name, existing->line
        );
        context->has_errors = true;
        return false;
    }

    /* Create function signature */
    FunctionSignature* sig = malloc(sizeof(FunctionSignature));
    if (!sig) return false;

    sig->return_type = return_type;
    sig->return_type_info = return_type_info;
    sig->params = params;
    sig->param_count = param_count;
    sig->is_variadic = is_variadic;

    /* Allocate symbol entry */
    SymbolEntry* entry = malloc(sizeof(SymbolEntry));
    if (!entry) {
        free(sig);
        return false;
    }

    entry->name = duplicate_string(name);
    entry->state_modifier = NULL;
    entry->type = TYPE_FUNCTION;
    entry->type_info = return_type_info;
    entry->is_constant = true;
    entry->init_state = INIT_CONSTANT;
    entry->is_used = false;
    entry->is_mutable = false;
    entry->declared_scope = context->current_scope->level;
    entry->line = line;
    entry->column = column;
    entry->extra.func_sig = sig;
    entry->compound_scope = NULL;

    if (!add_symbol_to_table(context->current_scope, entry)) {
        free(entry->name);
        free(entry);
        free(sig);
        return false;
    }

    return true;
}

bool semantic_add_function(SemanticContext* context, const char* name,
                           DataType return_type, Type* return_type_info,
                           FunctionParam* params, size_t param_count,
                           uint16_t line, uint16_t column) {
    return semantic_add_function_ex(context, name, return_type, return_type_info,
                                    params, param_count, false, line, column);
}

/**
 * @brief Validate a struct member declaration.
 * @param context Semantic context.
 * @param member_node AST node of the member (AST_VARIABLE_DECLARATION).
 * @param struct_name Name of the containing struct (for error reporting).
 * @param line Line number of the struct definition.
 * @param column Column number of the struct definition.
 * @return true if member is valid, false otherwise.
 */
static bool validate_struct_member(SemanticContext* context, ASTNode* member_node,
                                   const char* struct_name,
                                   uint16_t line, uint16_t column) {
    /* Must be a variable declaration */
    if (member_node->type != AST_VARIABLE_DECLARATION) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_INVALID_OPERATION,
            line, column, (uint8_t)strlen(struct_name),
            "semantic",
            "Struct '%s' contains non-variable member (only var/obj allowed)",
            struct_name
        );
        context->has_errors = true;
        return false;
    }

    /* State modifier must be 'var' or 'obj' */
    const char* mod = member_node->state_modifier;
    if (!mod || (strcmp(mod, "var") != 0 && strcmp(mod, "obj") != 0)) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_INVALID_OPERATION,
            line, column, (uint8_t)strlen(member_node->value),
            "semantic",
            "Struct member '%s' must have 'var' or 'obj' modifier (found: %s)",
            member_node->value,
            mod ? mod : "none"
        );
        context->has_errors = true;
        return false;
    }

    /* Determine member type */
    DataType mem_type = TYPE_UNKNOWN;
    Type* mem_type_info = member_node->variable_type;
    if (mem_type_info) {
        mem_type = semantic_type_from_type_info(mem_type_info, context);
    }

    if (mem_type == TYPE_UNKNOWN) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            line, column, (uint8_t)strlen(member_node->value),
            "semantic",
            "Struct member '%s' has unknown type",
            member_node->value
        );
        context->has_errors = true;
        return false;
    }

    /* If member is a struct, ensure that struct type is already declared */
    if (mem_type == TYPE_COMPOUND && mem_type_info && mem_type_info->name) {
        SymbolEntry* type_entry = semantic_find_symbol(context, mem_type_info->name);
        if (!type_entry || type_entry->type != TYPE_COMPOUND) {
            errhandler__report_error_ex(
                ERROR_LEVEL_ERROR,
                ERROR_CODE_SEM_UNDECLARED_SYMBOL,
                line, column, (uint8_t)strlen(mem_type_info->name),
                "semantic",
                "Struct member '%s' uses undeclared struct type '%s'",
                member_node->value, mem_type_info->name
            );
            context->has_errors = true;
            return false;
        }
    }

    return true;
}

bool semantic_add_compound_type(SemanticContext* context, const char* name,
                                ASTNode* members_ast,
                                uint16_t line, uint16_t column) {
    if (!context || !name) return false;

    /* Check for redeclaration in current scope */
    SymbolEntry* existing = find_symbol_in_table(context->current_scope, name);
    if (existing) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_REDECLARATION,
            line, column, (uint8_t)strlen(name),
            "semantic",
            "Redeclaration of struct '%s' (previous declaration at line %d)",
            name, existing->line
        );
        context->has_errors = true;
        return false;
    }

    /* The members_ast must be an AST_BLOCK containing the list of members */
    if (!members_ast || members_ast->type != AST_BLOCK || !members_ast->extra) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            line, column, (uint8_t)strlen(name),
            "semantic",
            "Invalid member list for struct '%s'",
            name
        );
        context->has_errors = true;
        return false;
    }

    AST* member_list = (AST*)members_ast->extra;

    /* Create the struct symbol */
    SymbolEntry* entry = malloc(sizeof(SymbolEntry));
    if (!entry) return false;

    entry->name = duplicate_string(name);
    entry->state_modifier = NULL;
    entry->type = TYPE_COMPOUND;
    entry->type_info = NULL;
    entry->is_constant = true;
    entry->init_state = INIT_CONSTANT;
    entry->is_used = false;
    entry->is_mutable = false;
    entry->declared_scope = context->current_scope->level;
    entry->line = line;
    entry->column = column;
    entry->extra.compound_members = NULL; /* Will be filled later for debug */
    entry->compound_scope = NULL;
    entry->next = NULL;

    /* Create a dedicated scope for members */
    SymbolTable* struct_scope = create_symbol_table(context->current_scope, SCOPE_COMPOUND);
    if (!struct_scope) {
        free(entry->name);
        free(entry);
        return false;
    }
    entry->compound_scope = struct_scope;

    /* Process each member */
    CompoundMember* member_debug_list = NULL;
    CompoundMember* last_debug = NULL;

    for (uint16_t i = 0; i < member_list->count; i++) {
        ASTNode* member_node = member_list->nodes[i];
        if (!validate_struct_member(context, member_node, name, line, column)) {
            /* Error already reported */
            destroy_symbol_table(struct_scope);
            free(entry->name);
            free(entry);
            return false;
        }

        /* Extract member information */
        const char* mod = member_node->state_modifier;
        DataType mem_type = semantic_type_from_type_info(member_node->variable_type, context);
        Type* mem_type_info = member_node->variable_type;
        InitState mem_init = member_node->default_value ? INIT_FULL : INIT_UNINITIALIZED;
        bool is_const = (mod && strcmp(mod, "const") == 0); /* Not typical for structs, but allowed */

        /* Add member to the struct's scope */
        if (!semantic_add_variable_ex(context, struct_scope,
                                      member_node->value,
                                      mem_type, mem_type_info,
                                      is_const, mod,
                                      mem_init,
                                      line, column)) {
            /* Error already reported */
            destroy_symbol_table(struct_scope);
            free(entry->name);
            free(entry);
            return false;
        }

        /* Also record in debug list for output */
        CompoundMember* dbg = malloc(sizeof(CompoundMember));
        if (!dbg) {
            destroy_symbol_table(struct_scope);
            free(entry->name);
            free(entry);
            return false;
        }
        dbg->name = duplicate_string(member_node->value);
        dbg->state_modifier = duplicate_string(mod);
        dbg->type = mem_type;
        dbg->type_info = mem_type_info;
        dbg->init_state = mem_init;
        dbg->next = NULL;

        if (!member_debug_list) {
            member_debug_list = dbg;
            last_debug = dbg;
        } else {
            last_debug->next = dbg;
            last_debug = dbg;
        }
    }

    entry->extra.compound_members = member_debug_list;

    /* Finally add the struct symbol itself */
    if (!add_symbol_to_table(context->current_scope, entry)) {
        destroy_symbol_table(struct_scope);
        free(entry->name);
        free(entry);
        return false;
    }

    return true;
}

SymbolEntry* semantic_find_symbol(SemanticContext* context, const char* name) {
    if (!context || !name) return NULL;

    SymbolTable* table = context->current_scope;
    while (table) {
        SymbolEntry* entry = find_symbol_in_table(table, name);
        if (entry) {
            return entry;
        }
        table = table->parent;
    }
    return NULL;
}

SymbolEntry* semantic_find_struct_member(SemanticContext* context,
                                         const char* struct_name,
                                         const char* field_name) {
    SymbolEntry* struct_entry = semantic_find_symbol(context, struct_name);
    if (!struct_entry || struct_entry->type != TYPE_COMPOUND || !struct_entry->compound_scope)
        return NULL;
    return find_symbol_in_table(struct_entry->compound_scope, field_name);
}

VisibilityResult semantic_check_visibility(SemanticContext* context,
                                           const char* name,
                                           bool require_initialized,
                                           bool allow_shadowing) {
    VisibilityResult result = {false, NULL, SCOPE_GLOBAL, NULL};

    if (!context || !name) {
        result.error_msg = duplicate_string("Invalid parameters");
        return result;
    }

    SymbolTable* table = context->current_scope;
    SymbolEntry* found = NULL;
    SymbolTable* found_table = NULL;

    /* Search from current scope up to global */
    while (table) {
        SymbolEntry* entry = find_symbol_in_table(table, name);
        if (entry) {
            found = entry;
            found_table = table;
            break;
        }
        table = table->parent;
    }

    if (!found) {
        result.error_msg = duplicate_string("Undeclared symbol");
        return result;
    }

    /* Check if symbol is shadowed by an inner declaration */
    if (!allow_shadowing) {
        SymbolTable* inner_table = context->current_scope;
        while (inner_table && inner_table != found_table) {
            SymbolEntry* inner_entry = find_symbol_in_table(inner_table, name);
            if (inner_entry) {
                result.error_msg = duplicate_string("Symbol shadowed by inner scope declaration");
                return result;
            }
            if (inner_table->parent == found_table) break;
            inner_table = inner_table->parent;
        }
    }

    /* Require initialization if requested */
    if (require_initialized && found->init_state == INIT_UNINITIALIZED) {
        result.error_msg = duplicate_string("Use of uninitialized variable");
        return result;
    }

    /* Scope accessibility: cannot access a symbol declared in an inner scope */
    if (found->declared_scope > context->current_scope->level) {
        result.error_msg = duplicate_string("Symbol not accessible from current scope");
        return result;
    }

    result.visible = true;
    result.entry = found;
    result.found_in_scope = found_table->level;
    return result;
}

bool semantic_mark_symbol_used(SemanticContext* context, const char* name) {
    SymbolEntry* entry = semantic_find_symbol(context, name);
    if (entry) {
        entry->is_used = true;
        return true;
    }
    return false;
}

bool semantic_update_init_state(SemanticContext* context, const char* name,
                                InitState new_state) {
    SymbolEntry* entry = semantic_find_symbol(context, name);
    if (!entry) return false;

    /* Cannot change initialization state of constants */
    if (entry->is_constant && entry->init_state == INIT_CONSTANT) {
        return false;
    }

    /* Allowed transitions */
    if (entry->init_state == INIT_UNINITIALIZED) {
        if (new_state == INIT_PARTIAL || new_state == INIT_FULL ||
            new_state == INIT_DEFAULT) {
            entry->init_state = new_state;
            return true;
        }
    } else if (entry->init_state == INIT_PARTIAL) {
        if (new_state == INIT_FULL) {
            entry->init_state = new_state;
            return true;
        }
    }
    /* Always allow setting to same or more restrictive state */
    if (new_state >= entry->init_state) {
        entry->init_state = new_state;
        return true;
    }
    return false;
}

InitState semantic_get_init_state(SemanticContext* context, const char* name) {
    SymbolEntry* entry = semantic_find_symbol(context, name);
    return entry ? entry->init_state : INIT_UNINITIALIZED;
}

bool semantic_can_modify_symbol(SemanticContext* context, const char* name) {
    SymbolEntry* entry = semantic_find_symbol(context, name);
    if (!entry) return false;
    if (entry->is_constant) return false;
    if (!entry->is_mutable) return false;
    return true;
}

bool semantic_is_valid_struct_member_modifier(const char* state_modifier) {
    if (!state_modifier) return false;
    return (strcmp(state_modifier, "var") == 0 ||
            strcmp(state_modifier, "obj") == 0);
}

DataType semantic_type_from_token(TokenType token_type) {
    switch (token_type) {
        case TOKEN_NUMBER:  return TYPE_REAL;
        case TOKEN_CHAR:    return TYPE_CHAR;
        case TOKEN_STRING:  return TYPE_STRING;
        case TOKEN_NULL:
        case TOKEN_NONE:    return TYPE_NONE;
        default:            return TYPE_UNKNOWN;
    }
}

const char* semantic_type_to_string(DataType type) {
    switch (type) {
        case TYPE_INT:      return "Int";
        case TYPE_REAL:     return "Real";
        case TYPE_CHAR:     return "Char";
        case TYPE_STRING:   return "String";
        case TYPE_BOOL:     return "Bool";
        case TYPE_VOID:     return "Void";
        case TYPE_NONE:     return "none";
        case TYPE_POINTER:  return "pointer";
        case TYPE_REFERENCE:return "reference";
        case TYPE_ARRAY:    return "array";
        case TYPE_FUNCTION: return "function";
        case TYPE_COMPOUND: return "struct";
        default:            return "unknown";
    }
}

const char* semantic_init_state_to_string(InitState state) {
    switch (state) {
        case INIT_UNINITIALIZED: return "uninitialized";
        case INIT_PARTIAL:       return "partially initialized";
        case INIT_FULL:          return "fully initialized";
        case INIT_CONSTANT:      return "constant";
        case INIT_DEFAULT:       return "default initialized";
        default:                 return "unknown";
    }
}

bool semantic_types_compatible(DataType type1, DataType type2) {
    /* Same type is always compatible */
    if (type1 == type2) return true;

    /* Numeric promotion */
    if ((type1 == TYPE_INT && type2 == TYPE_REAL) ||
        (type1 == TYPE_REAL && type2 == TYPE_INT)) {
        return true;
    }

    /* none is compatible with pointers and references */
    if (type1 == TYPE_NONE) {
        return (type2 == TYPE_POINTER || type2 == TYPE_REFERENCE);
    }
    if (type2 == TYPE_NONE) {
        return (type1 == TYPE_POINTER || type1 == TYPE_REFERENCE);
    }

    /* Pointers and references of same base type are compatible (base check omitted) */
    if ((type1 == TYPE_POINTER && type2 == TYPE_POINTER) ||
        (type1 == TYPE_REFERENCE && type2 == TYPE_REFERENCE)) {
        return true;
    }

    /* Struct types: consider compatible if both are structs */
    if (type1 == TYPE_COMPOUND && type2 == TYPE_COMPOUND) {
        return true;
    }

    return false;
}

bool semantic_types_assignable_ex(DataType target_type, DataType source_type,
                                  InitState target_init, InitState source_init) {
    /* Type compatibility first */
    if (!semantic_types_compatible(target_type, source_type)) {
        return false;
    }

    /* Cannot assign uninitialized value */
    if (source_init == INIT_UNINITIALIZED) {
        return false;
    }

    /* Cannot assign to constant */
    if (target_init == INIT_CONSTANT) {
        return false;
    }

    /* Special: assigning default value to uninitialized variable */
    if (target_init == INIT_UNINITIALIZED && source_init == INIT_DEFAULT) {
        return true;
    }

    return true;
}

bool semantic_types_assignable(DataType target_type, DataType source_type) {
    return semantic_types_compatible(target_type, source_type);
}

TypeCheckResult semantic_check_type(SemanticContext* context, ASTNode* node) {
    TypeCheckResult result = {false, TYPE_UNKNOWN, NULL, INIT_UNINITIALIZED, NULL};

    if (!node) {
        result.error_msg = duplicate_string("Null node");
        return result;
    }

    switch (node->type) {
        case AST_LITERAL_VALUE:
            result.type = semantic_type_from_token(node->operation_type);
            result.init_state = INIT_CONSTANT;
            result.valid = (result.type != TYPE_UNKNOWN);
            if (!result.valid) {
                result.error_msg = duplicate_string("Invalid literal type");
            }
            break;

        case AST_IDENTIFIER: {
            if (node->value) {
                VisibilityResult vis = semantic_check_visibility(
                    context, node->value, true, false);
                if (vis.visible) {
                    semantic_mark_symbol_used(context, node->value);
                    result.type = vis.entry->type;
                    result.type_info = vis.entry->type_info;
                    result.init_state = vis.entry->init_state;
                    result.valid = true;

                    /* Warn if variable is used uninitialized */
                    if (vis.entry->init_state == INIT_UNINITIALIZED &&
                        context->warnings_enabled &&
                        vis.entry->type != TYPE_FUNCTION &&
                        vis.entry->type != TYPE_COMPOUND) {
                        errhandler__report_error_ex(
                            ERROR_LEVEL_WARNING,
                            ERROR_CODE_SEM_UNINITIALIZED,
                            0, 0, (uint8_t)strlen(node->value),
                            "semantic",
                            "Use of possibly uninitialized variable '%s'",
                            node->value
                        );
                    }
                } else {
                    errhandler__report_error_ex(
                        ERROR_LEVEL_ERROR,
                        ERROR_CODE_SEM_UNDECLARED_SYMBOL,
                        0, 0, (uint8_t)strlen(node->value),
                        "semantic",
                        "%s: '%s'",
                        vis.error_msg ? vis.error_msg : "Undeclared identifier",
                        node->value
                    );
                    result.error_msg = duplicate_string("Undeclared identifier");
                    context->has_errors = true;
                    free(vis.error_msg);
                }
            }
            break;
        }

        case AST_BINARY_OPERATION:
            result = semantic_check_binary_op(context, node);
            break;

        case AST_UNARY_OPERATION:
            result = semantic_check_unary_op(context, node);
            break;

        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
            result = semantic_check_assignment(context, node);
            break;

        case AST_FUNCTION_DECLARATION:
            result.type = TYPE_FUNCTION;
            result.init_state = INIT_CONSTANT;
            result.valid = true;
            break;

        case AST_COMPOUND_TYPE:
            result.type = TYPE_COMPOUND;
            result.init_state = INIT_CONSTANT;
            result.valid = true;
            break;

        case AST_CAST: {
            if (node->variable_type) {
                result.type = semantic_type_from_type_info(node->variable_type, context);
                result.type_info = node->variable_type;
                TypeCheckResult expr = semantic_check_type(context, node->left);
                result.valid = expr.valid &&
                               semantic_types_compatible(result.type, expr.type);
                if (result.valid) {
                    result.init_state = expr.init_state;
                } else {
                    result.error_msg = duplicate_string("Invalid cast");
                }
            } else {
                result.error_msg = duplicate_string("Cast without target type");
            }
            break;
        }

        case AST_FIELD_ACCESS: {
            if (!semantic_check_field_access(context, node)) {
                result.valid = false;
                result.error_msg = duplicate_string("Invalid field access");
                break;
            }
            /* Retrieve the field's type */
            if (node->left && node->left->type == AST_IDENTIFIER &&
                node->right && node->right->type == AST_IDENTIFIER) {
                const char* struct_name = node->left->value;
                const char* field_name = node->right->value;
                SymbolEntry* field = semantic_find_struct_member(context,
                                                                 struct_name,
                                                                 field_name);
                if (field) {
                    result.type = field->type;
                    result.type_info = field->type_info;
                    result.init_state = field->init_state;
                    result.valid = true;
                }
            }
            break;
        }

        default:
            result.valid = true;
            break;
    }

    return result;
}

TypeCheckResult semantic_check_binary_op(SemanticContext* context, ASTNode* node) {
    TypeCheckResult result = {false, TYPE_UNKNOWN, NULL, INIT_UNINITIALIZED, NULL};

    if (!node->left || !node->right) {
        result.error_msg = duplicate_string("Binary operation missing operands");
        return result;
    }

    TypeCheckResult left = semantic_check_type(context, node->left);
    TypeCheckResult right = semantic_check_type(context, node->right);

    if (!left.valid || !right.valid) {
        result.error_msg = duplicate_string("Invalid operand type");
        return result;
    }

    if (!semantic_types_compatible(left.type, right.type)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Type mismatch in binary operation: %s and %s",
                 semantic_type_to_string(left.type),
                 semantic_type_to_string(right.type));
        result.error_msg = duplicate_string(msg);
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, 0,
            "semantic",
            "%s", msg
        );
        context->has_errors = true;
        return result;
    }

    switch (node->operation_type) {
        case TOKEN_PLUS:
        case TOKEN_MINUS:
        case TOKEN_STAR:
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
            /* Arithmetic */
            if (left.type == TYPE_STRING || right.type == TYPE_STRING) {
                if (node->operation_type == TOKEN_PLUS) {
                    result.type = TYPE_STRING;
                    result.valid = true;
                } else {
                    result.error_msg = duplicate_string("Invalid operation for string type");
                }
            } else if (left.type == TYPE_INT && right.type == TYPE_INT) {
                result.type = TYPE_INT;
                result.valid = true;
            } else {
                result.type = TYPE_REAL;
                result.valid = true;
            }
            break;

        case TOKEN_DOUBLE_EQ:
        case TOKEN_NE:
        case TOKEN_LT:
        case TOKEN_GT:
        case TOKEN_LE:
        case TOKEN_GE:
            /* Comparisons -> boolean */
            result.type = TYPE_BOOL;
            result.valid = true;
            break;

        case TOKEN_LOGICAL:
            if (left.type == TYPE_BOOL && right.type == TYPE_BOOL) {
                result.type = TYPE_BOOL;
                result.valid = true;
            } else {
                result.error_msg = duplicate_string("Logical operations require boolean operands");
            }
            break;

        case TOKEN_PIPE:
        case TOKEN_AMPERSAND:
        case TOKEN_CARET:
        case TOKEN_SHL:
        case TOKEN_SHR:
            /* Bitwise -> integer */
            if (left.type == TYPE_INT && right.type == TYPE_INT) {
                result.type = TYPE_INT;
                result.valid = true;
            } else {
                result.error_msg = duplicate_string("Bitwise operations require integer operands");
            }
            break;

        default:
            result.type = left.type;
            result.valid = true;
            break;
    }

    if (result.valid) {
        /* Result initialization state is the worse (lower) of the two */
        result.init_state = (left.init_state < right.init_state) ?
                             left.init_state : right.init_state;
    }

    return result;
}

TypeCheckResult semantic_check_unary_op(SemanticContext* context, ASTNode* node) {
    TypeCheckResult result = {false, TYPE_UNKNOWN, NULL, INIT_UNINITIALIZED, NULL};

    if (!node->right) {
        result.error_msg = duplicate_string("Unary operation missing operand");
        return result;
    }

    TypeCheckResult operand = semantic_check_type(context, node->right);
    if (!operand.valid) {
        result.error_msg = duplicate_string("Invalid operand type");
        return result;
    }

    switch (node->operation_type) {
        case TOKEN_PLUS:
        case TOKEN_MINUS:
            if (operand.type == TYPE_INT || operand.type == TYPE_REAL) {
                result.type = operand.type;
                result.valid = true;
            } else {
                result.error_msg = duplicate_string("Unary +/- requires numeric operand");
            }
            break;

        case TOKEN_BANG:
            if (operand.type == TYPE_BOOL) {
                result.type = TYPE_BOOL;
                result.valid = true;
            } else {
                result.error_msg = duplicate_string("Logical NOT requires boolean operand");
            }
            break;

        case TOKEN_TILDE:
            if (operand.type == TYPE_INT) {
                result.type = TYPE_INT;
                result.valid = true;
            } else {
                result.error_msg = duplicate_string("Bitwise NOT requires integer operand");
            }
            break;

        default:
            result.error_msg = duplicate_string("Unknown unary operation");
            break;
    }

    if (result.valid) {
        result.init_state = operand.init_state;
    }

    return result;
}

TypeCheckResult semantic_check_assignment(SemanticContext* context, ASTNode* node) {
    TypeCheckResult result = {false, TYPE_UNKNOWN, NULL, INIT_UNINITIALIZED, NULL};

    if (!node->left || !node->right) {
        result.error_msg = duplicate_string("Assignment missing operands");
        return result;
    }

    /* Left side must be an l-value (identifier) */
    if (node->left->type != AST_IDENTIFIER) {
        result.error_msg = duplicate_string("Left side of assignment must be an identifier");
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, 0,
            "semantic",
            "Left side of assignment must be an identifier"
        );
        context->has_errors = true;
        return result;
    }

    const char* var_name = node->left->value;

    /* Check mutability */
    if (!semantic_validate_mutation(context, var_name, 0, 0)) {
        result.error_msg = duplicate_string("Cannot modify variable");
        return result;
    }

    SymbolEntry* target = semantic_find_symbol(context, var_name);
    if (!target) {
        result.error_msg = duplicate_string("Assignment to undeclared variable");
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_UNDECLARED_SYMBOL,
            0, 0, (uint8_t)strlen(var_name),
            "semantic",
            "Assignment to undeclared variable '%s'",
            var_name
        );
        context->has_errors = true;
        return result;
    }

    /* Check right-hand side */
    TypeCheckResult rhs = semantic_check_type(context, node->right);
    if (!rhs.valid) {
        result.error_msg = duplicate_string("Invalid right-hand side type");
        return result;
    }

    /* Type compatibility with initialization states */
    if (!semantic_types_assignable_ex(target->type, rhs.type,
                                      target->init_state, rhs.init_state)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Type mismatch in assignment: cannot assign %s (%s) to %s (%s)",
                 semantic_type_to_string(rhs.type),
                 semantic_init_state_to_string(rhs.init_state),
                 semantic_type_to_string(target->type),
                 semantic_init_state_to_string(target->init_state));
        result.error_msg = duplicate_string(msg);
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, (uint8_t)strlen(var_name),
            "semantic",
            "%s", msg
        );
        context->has_errors = true;
        return result;
    }

    /* Update variable initialization state */
    InitState new_state = (rhs.init_state == INIT_FULL ||
                           rhs.init_state == INIT_CONSTANT) ?
                           INIT_FULL : INIT_PARTIAL;
    semantic_update_init_state(context, var_name, new_state);

    result.type = target->type;
    result.type_info = target->type_info;
    result.init_state = new_state;
    result.valid = true;

    return result;
}

TypeCheckResult semantic_check_function_call(SemanticContext* context,
                                             ASTNode* node) {
    /* TODO: Implement full function call type checking */
    TypeCheckResult result = {false, TYPE_UNKNOWN, NULL, INIT_UNINITIALIZED, NULL};
    result.valid = true;
    result.type = TYPE_UNKNOWN;
    return result;
}

bool semantic_check_compound_type(SemanticContext* context, ASTNode* node) {
    if (!node || !node->value) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, 0,
            "semantic",
            "Invalid struct definition"
        );
        context->has_errors = true;
        return false;
    }

    if (!node->extra) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, (uint8_t)strlen(node->value),
            "semantic",
            "Struct '%s' has no members",
            node->value
        );
        context->has_errors = true;
        return false;
    }

    ASTNode* extra = node->extra;
    if (extra->type != AST_BLOCK || !extra->extra) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, (uint8_t)strlen(node->value),
            "semantic",
            "Invalid member list for struct '%s'",
            node->value
        );
        context->has_errors = true;
        return false;
    }

    return semantic_add_compound_type(context, node->value, extra,
                                      0, 0);
}

bool semantic_check_block_ends_with_return(SemanticContext* context, AST* block_ast) {
    if (!block_ast || block_ast->count == 0) return false;

    ASTNode* last = block_ast->nodes[block_ast->count - 1];
    if (last->type == AST_RETURN) return true;

    if (last->type == AST_IF_STATEMENT) {
        if (last->right && last->extra) {
            AST* then_block = (AST*)last->right;
            AST* else_block = (AST*)last->extra;
            return semantic_check_block_ends_with_return(context, then_block) &&
                   semantic_check_block_ends_with_return(context, else_block);
        }
    }
    return false;
}

bool semantic_statement_ensures_return(SemanticContext* context, ASTNode* node) {
    if (!node) return false;
    if (node->type == AST_RETURN) return true;
    if (node->type == AST_BLOCK) {
        if (node->extra) {
            AST* block = (AST*)node->extra;
            return semantic_check_block_ends_with_return(context, block);
        }
    }
    if (node->type == AST_IF_STATEMENT) {
        if (node->right && node->extra) {
            return semantic_statement_ensures_return(context, node->right) &&
                   semantic_statement_ensures_return(context, node->extra);
        }
    }
    return false;
}

bool semantic_check_scope_initialization(SemanticContext* context, SymbolTable* scope) {
    if (!scope) scope = context->current_scope;
    bool all_init = true;

    for (size_t i = 0; i < scope->capacity; i++) {
        SymbolEntry* entry = scope->entries[i];
        while (entry) {
            if (entry->type != TYPE_FUNCTION && entry->type != TYPE_COMPOUND &&
                !entry->is_constant && entry->init_state == INIT_UNINITIALIZED) {
                if (context->warnings_enabled) {
                    errhandler__report_error_ex(
                        ERROR_LEVEL_WARNING,
                        ERROR_CODE_SEM_UNINITIALIZED,
                        entry->line, entry->column, (uint8_t)strlen(entry->name),
                        "semantic",
                        "Variable '%s' declared but never initialized",
                        entry->name
                    );
                }
                all_init = false;
            }
            entry = entry->next;
        }
    }
    return all_init;
}

void semantic_check_shadowing(SemanticContext* context, const char* name,
                              uint16_t line, uint16_t column) {
    SymbolEntry* outer = NULL;
    SymbolTable* table = context->current_scope->parent;
    while (table) {
        SymbolEntry* entry = find_symbol_in_table(table, name);
        if (entry) {
            outer = entry;
            break;
        }
        table = table->parent;
    }
    if (outer) {
        errhandler__report_error_ex(
            ERROR_LEVEL_WARNING,
            ERROR_CODE_SEM_REDECLARATION,
            line, column, (uint8_t)strlen(name),
            "semantic",
            "Variable '%s' shadows declaration from line %d",
            name, outer->line
        );
    }
}

bool semantic_validate_mutation(SemanticContext* context, const char* name,
                                uint16_t line, uint16_t column) {
    SymbolEntry* entry = semantic_find_symbol(context, name);
    if (!entry) return false;

    if (entry->is_constant) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_ASSIGN_TO_CONST,
            line, column, (uint8_t)strlen(name),
            "semantic",
            "Cannot assign to constant variable '%s'",
            name
        );
        context->has_errors = true;
        return false;
    }

    if (!entry->is_mutable) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_ASSIGN_TO_CONST,
            line, column, (uint8_t)strlen(name),
            "semantic",
            "Variable '%s' is not mutable",
            name
        );
        context->has_errors = true;
        return false;
    }
    return true;
}

bool semantic_check_field_access(SemanticContext* context, ASTNode* node) {
    if (!node || node->type != AST_FIELD_ACCESS) return false;

    ASTNode* object = node->left;
    ASTNode* field = node->right;

    if (!object || !field || field->type != AST_IDENTIFIER) {
        errhandler__report_error(
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0,
            "semantic",
            "Invalid field access syntax"
        );
        context->has_errors = true;
        return false;
    }

    if (object->type != AST_IDENTIFIER) {
        errhandler__report_error(
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0,
            "semantic",
            "Left side of '->' must be an object identifier"
        );
        context->has_errors = true;
        return false;
    }

    const char* obj_name = object->value;
    SymbolEntry* obj = semantic_find_symbol(context, obj_name);
    if (!obj) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_UNDECLARED_SYMBOL,
            0, 0, (uint8_t)strlen(obj_name),
            "semantic",
            "Undeclared object '%s'",
            obj_name
        );
        context->has_errors = true;
        return false;
    }

    /* Object must be of compound type */
    if (obj->type != TYPE_COMPOUND) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, (uint8_t)strlen(obj_name),
            "semantic",
            "Cannot access field of non-struct type '%s'",
            semantic_type_to_string(obj->type)
        );
        context->has_errors = true;
        return false;
    }

    /* Only variables declared with 'obj' can access struct members */
    if (!obj->state_modifier || strcmp(obj->state_modifier, "obj") != 0) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, (uint8_t)strlen(obj_name),
            "semantic",
            "Only 'obj' variables can access struct members (variable '%s' is '%s')",
            obj_name,
            obj->state_modifier ? obj->state_modifier : "none"
        );
        context->has_errors = true;
        return false;
    }

    /* Determine the struct type name */
    const char* struct_name = NULL;
    if (obj->type_info && obj->type_info->name) {
        struct_name = obj->type_info->name;
    } else {
        errhandler__report_error(
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0,
            "semantic",
            "Cannot determine struct type of object '%s'",
            obj_name
        );
        context->has_errors = true;
        return false;
    }

    /* Check that the field exists */
    SymbolEntry* field_entry = semantic_find_struct_member(context,
                                                           struct_name,
                                                           field->value);
    if (!field_entry) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_UNDECLARED_SYMBOL,
            0, 0, (uint8_t)strlen(field->value),
            "semantic",
            "Struct '%s' has no member named '%s'",
            struct_name, field->value
        );
        context->has_errors = true;
        return false;
    }

    return true;
}

/**
 * @brief Check a variable declaration statement.
 * @param context Semantic context.
 * @param node AST_VARIABLE_DECLARATION node.
 * @return true if valid, false otherwise.
 */
static bool check_variable_declaration(SemanticContext* context, ASTNode* node) {
    if (!node || !node->value) return false;

    DataType type = TYPE_UNKNOWN;
    Type* type_info = node->variable_type;
    InitState init_state = INIT_UNINITIALIZED;

    /* Determine type from explicit annotation or default value */
    if (type_info) {
        type = semantic_type_from_type_info(type_info, context);
    }

    if (node->default_value) {
        TypeCheckResult def = semantic_check_type(context, node->default_value);
        if (def.valid) {
            type = def.type;
            init_state = (def.init_state == INIT_CONSTANT) ?
                         INIT_CONSTANT : INIT_FULL;
        }
    }

    /* Default to Int if type cannot be inferred */
    if (type == TYPE_UNKNOWN) {
        type = TYPE_INT;
        if (node->default_value) {
            init_state = INIT_FULL;
        }
    }

    bool has_init = (node->default_value != NULL);
    bool is_const = (node->state_modifier &&
                     (strcmp(node->state_modifier, "const") == 0));

    /* Constants must be initialized */
    if (is_const && !has_init) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_UNINITIALIZED,
            0, 0, (uint8_t)strlen(node->value),
            "semantic",
            "Constant '%s' must be initialized",
            node->value
        );
        context->has_errors = true;
        return false;
    }

    if (is_const) {
        init_state = INIT_CONSTANT;
    } else if (has_init) {
        init_state = INIT_FULL;
    } else {
        init_state = INIT_UNINITIALIZED;
    }

    bool added = semantic_add_variable_ex(context, context->current_scope,
                                          node->value, type, type_info,
                                          is_const, node->state_modifier,
                                          init_state, 0, 0);
    if (added && node->default_value) {
        if (!semantic_check_expression(context, node->default_value)) {
            return false;
        }
    }
    return added;
}

/**
 * @brief Check a function declaration statement.
 * @param context Semantic context.
 * @param node AST_FUNCTION_DECLARATION node.
 * @return true if valid, false otherwise.
 */
static bool check_function_declaration(SemanticContext* context, ASTNode* node) {
    if (!node || !node->value) return false;

    DataType return_type = TYPE_VOID;
    Type* return_type_info = node->variable_type;
    if (return_type_info) {
        return_type = semantic_type_from_type_info(return_type_info, context);
    }

    /* TODO: Extract parameters from AST */
    FunctionParam* params = NULL;
    size_t param_count = 0;

    bool added = semantic_add_function(context, node->value, return_type,
                                       return_type_info, params, param_count,
                                       0, 0);
    if (added && node->right) {
        semantic_enter_function_scope(context, node->value, return_type);
        /* TODO: Add parameters to function scope */
        semantic_check_statement(context, node->right);

        if (return_type != TYPE_VOID) {
            if (!semantic_statement_ensures_return(context, node->right)) {
                errhandler__report_error_ex(
                    ERROR_LEVEL_ERROR,
                    ERROR_CODE_SEM_MISSING_RETURN,
                    0, 0, (uint8_t)strlen(node->value),
                    "semantic",
                    "Function '%s' with non-void return type must end with a return statement",
                    node->value
                );
                context->has_errors = true;
                semantic_exit_function_scope(context);
                return false;
            }
        } else {
            if (!semantic_statement_ensures_return(context, node->right)) {
                errhandler__report_error_ex(
                    ERROR_LEVEL_WARNING,
                    ERROR_CODE_SEM_MISSING_RETURN,
                    0, 0, (uint8_t)strlen(node->value),
                    "semantic",
                    "Function '%s' should end with a return statement",
                    node->value
                );
            }
        }

        semantic_check_scope_initialization(context, context->function_scope);
        semantic_exit_function_scope(context);
    }

    return added;
}

bool semantic_check_expression(SemanticContext* context, ASTNode* node) {
    if (!node) return true;
    TypeCheckResult res = semantic_check_type(context, node);
    if (!res.valid && res.error_msg) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, 0,
            "semantic",
            "Type error: %s",
            res.error_msg
        );
        context->has_errors = true;
        free(res.error_msg);
        return false;
    }
    free(res.error_msg);
    return true;
}

bool semantic_check_statement(SemanticContext* context, ASTNode* node) {
    if (!node) return true;

    switch (node->type) {
        case AST_VARIABLE_DECLARATION:
            return check_variable_declaration(context, node);

        case AST_FUNCTION_DECLARATION:
            return check_function_declaration(context, node);

        case AST_COMPOUND_TYPE:
            return semantic_check_compound_type(context, node);

        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
            return semantic_check_expression(context, node);

        case AST_IF_STATEMENT:
            if (!semantic_check_expression(context, node->left)) {
                return false;
            }
            semantic_enter_scope(context);
            if (node->right && !semantic_check_statement(context, node->right)) {
                semantic_exit_scope(context);
                return false;
            }
            semantic_exit_scope(context);
            if (node->extra) {
                semantic_enter_scope(context);
                if (!semantic_check_statement(context, node->extra)) {
                    semantic_exit_scope(context);
                    return false;
                }
                semantic_exit_scope(context);
            }
            return true;

        case AST_BLOCK: {
            semantic_enter_scope(context);
            if (node->extra) {
                AST* block = (AST*)node->extra;
                for (uint16_t i = 0; i < block->count; i++) {
                    if (!semantic_check_statement(context, block->nodes[i])) {
                        semantic_exit_scope(context);
                        return false;
                    }
                }
            }
            semantic_check_scope_initialization(context, NULL);
            semantic_exit_scope(context);
            return true;
        }

        case AST_RETURN:
            if (node->left) {
                return semantic_check_expression(context, node->left);
            }
            return true;

        case AST_DO_LOOP:
            semantic_enter_loop_scope(context);
            if (!semantic_check_expression(context, node->left)) {
                semantic_exit_loop_scope(context);
                return false;
            }
            if (node->right && !semantic_check_statement(context, node->right)) {
                semantic_exit_loop_scope(context);
                return false;
            }
            semantic_exit_loop_scope(context);
            return true;

        case AST_BREAK:
        case AST_CONTINUE:
            if (!context->in_loop) {
                errhandler__report_error_ex(
                    ERROR_LEVEL_ERROR,
                    ERROR_CODE_SEM_INVALID_OPERATION,
                    0, 0, 0,
                    "semantic",
                    "%s statement not in loop",
                    (node->type == AST_BREAK) ? "break" : "continue"
                );
                context->has_errors = true;
                return false;
            }
            return true;

        default:
            return semantic_check_expression(context, node);
    }
}

bool semantic_analyze(SemanticContext* context, AST* ast) {
    if (!context || !ast || !ast->nodes) {
        return false;
    }

    context->has_errors = false;

    for (uint16_t i = 0; i < ast->count; i++) {
        if (!semantic_check_statement(context, ast->nodes[i])) {
            context->has_errors = true;
            if (context->exit_on_error) {
                fprintf(stderr, "Semantic analysis failed with errors. Compilation terminated.\n");
                exit(1);
            }
        }
    }

    /* No warnings for unused variables – feature disabled */
    if (context->warnings_enabled) {
        semantic_check_scope_initialization(context, context->global_scope);
    }

    if (context->has_errors && context->exit_on_error) {
        fprintf(stderr, "Semantic analysis failed with errors. Compilation terminated.\n");
        exit(1);
    }

    return !context->has_errors;
}

size_t semantic_get_symbol_count(SemanticContext* context) {
    if (!context || !context->global_scope) return 0;
    return context->global_scope->count;
}

SymbolTable* semantic_get_global_table(SemanticContext* context) {
    if (!context) return NULL;
    return context->global_scope;
}

bool semantic_has_errors(SemanticContext* context) {
    return context ? context->has_errors : false;
}

bool semantic_warnings_enabled(SemanticContext* context) {
    return context ? context->warnings_enabled : false;
}
