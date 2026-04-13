#include "semantic.h"
#include "../errhandler/errhandler.h"
#include "../utils/str_utils.h"
#include "../utils/memory_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define HASH_TABLE_SIZE 64

static uint32_t hash_string(const char* str);
static SymbolTable* create_symbol_table(SymbolTable* parent, ScopeLevel level);
static void destroy_symbol_table(SymbolTable* table);
static SymbolEntry* find_symbol_in_table(SymbolTable* table, const char* name);
static bool add_symbol_to_table(SymbolTable* table, SymbolEntry* entry);
static DataType type_from_type_info(Type* type_info, SemanticContext* ctx);
static bool check_name_collision_in_current_scope(SemanticContext* ctx,
                                                  const char* name,
                                                  uint16_t line, uint16_t column);
static void warn_if_shadowing(SemanticContext* ctx, const char* name,
                              uint16_t line, uint16_t column);
static bool signatures_equal(FunctionSignature* a, FunctionSignature* b);
static SymbolEntry* find_member_in_compound(SymbolEntry* compound,
                                            const char* member_name);
static DataType determine_numeric_type(const char* value,
                                       bool* is_int_suffix,
                                       bool* is_real_suffix);
static bool contains_return(ASTNode* node);
static bool check_prototype_match(SemanticContext* ctx,
                                  SymbolEntry* existing,
                                  FunctionSignature* new_sig,
                                  uint16_t line, uint16_t column);
static bool final_verification(SemanticContext* ctx);

/*
 * Safe accessors for ASTNode fields that may be NULL.
 */
static bool ast_has_left(const ASTNode* node) {
    return node != NULL && node->left != NULL;
}

static bool ast_has_right(const ASTNode* node) {
    return node != NULL && node->right != NULL;
}

static bool ast_has_extra(const ASTNode* node) {
    return node != NULL && node->extra != NULL;
}

static bool ast_is_type(const ASTNode* node, ASTNodeType expected) {
    return node != NULL && node->type == expected;
}

static uint32_t hash_string(const char* str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

static SymbolTable* create_symbol_table(SymbolTable* parent, ScopeLevel level) {
    SymbolTable* table = (SymbolTable*)calloc(1, sizeof(SymbolTable));
    if (!table) return NULL;

    table->entries = (SymbolEntry**)calloc(HASH_TABLE_SIZE, sizeof(SymbolEntry*));
    if (!table->entries) {
        free(table);
        return NULL;
    }

    table->capacity = HASH_TABLE_SIZE;
    table->level = level;
    table->parent = parent;

    if (parent) {
        table->next_child = parent->children;
        parent->children = table;
    }

    return table;
}

static void destroy_symbol_table(SymbolTable* table) {
    if (!table) return;

    SymbolTable* child = table->children;
    while (child) {
        SymbolTable* next = child->next_child;
        destroy_symbol_table(child);
        child = next;
    }

    for (size_t i = 0; i < table->capacity; i++) {
        SymbolEntry* entry = table->entries[i];
        while (entry) {
            SymbolEntry* next = entry->next;

            free(entry->name);
            free(entry->state_modifier);
            free(entry->access_modifier);

            if (entry->type == TYPE_FUNCTION && entry->extra.func_sig) {
                FunctionParam* param = entry->extra.func_sig->params;
                while (param) {
                    FunctionParam* next_param = param->next;
                    free(param->name);
                    if (param->default_value) {
                        parser__free_ast_node(param->default_value);
                    }
                    free(param);
                    param = next_param;
                }
                free(entry->extra.func_sig);
            } else if (entry->type == TYPE_COMPOUND) {
                CompoundMember* member = entry->extra.compound_members;
                while (member) {
                    CompoundMember* next_member = member->next;
                    free(member->name);
                    free(member->state_modifier);
                    free(member->access_modifier);
                    free(member);
                    member = next_member;
                }
            }

            free(entry);
            entry = next;
        }
    }

    free(table->entries);
    free(table);
}

static SymbolEntry* find_symbol_in_table(SymbolTable* table, const char* name) {
    if (!table || !name) return NULL;

    uint32_t idx = hash_string(name) % table->capacity;
    SymbolEntry* entry = table->entries[idx];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

static bool add_symbol_to_table(SymbolTable* table, SymbolEntry* entry) {
    if (!table || !entry) return false;

    uint32_t idx = hash_string(entry->name) % table->capacity;
    entry->next = table->entries[idx];
    table->entries[idx] = entry;
    table->count++;
    return true;
}

SemanticContext* semantic__create_context(void) {
    SemanticContext* ctx = (SemanticContext*)calloc(1, sizeof(SemanticContext));
    if (!ctx) return NULL;

    ctx->global_scope = create_symbol_table(NULL, SCOPE_GLOBAL);
    if (!ctx->global_scope) {
        free(ctx);
        return NULL;
    }

    ctx->current_scope = ctx->global_scope;
    ctx->warnings_enabled = true;
    ctx->exit_on_error = true;
    return ctx;
}

void semantic__destroy_context(SemanticContext* ctx) {
    if (!ctx) return;
    destroy_symbol_table(ctx->global_scope);
    free(ctx);
}

void semantic__set_exit_on_error(SemanticContext* ctx, bool exit_on_error) {
    if (ctx) ctx->exit_on_error = exit_on_error;
}

void semantic__enter_scope_ex(SemanticContext* ctx, ScopeLevel level) {
    if (!ctx) return;

    SymbolTable* new_scope = create_symbol_table(ctx->current_scope, level);
    if (new_scope) {
        ctx->current_scope = new_scope;
        if (level == SCOPE_LOOP) ctx->in_loop = true;
        if (level == SCOPE_FUNCTION) ctx->in_function = true;
    }
}

void semantic__enter_scope(SemanticContext* ctx) {
    semantic__enter_scope_ex(ctx, SCOPE_BLOCK);
}

void semantic__exit_scope(SemanticContext* ctx) {
    if (!ctx || !ctx->current_scope || ctx->current_scope == ctx->global_scope) {
        return;
    }

    SymbolTable* old = ctx->current_scope;
    if (old->level == SCOPE_LOOP) ctx->in_loop = false;
    if (old->level == SCOPE_FUNCTION) {
        ctx->in_function = false;
        ctx->current_function = NULL;
    }

    ctx->current_scope = old->parent;
}

void semantic__enter_function_scope(SemanticContext* ctx,
                                   const char* function_name,
                                   DataType return_type) {
    semantic__enter_scope_ex(ctx, SCOPE_FUNCTION);
    ctx->current_function = function_name;
    ctx->current_return_type = return_type;
}

void semantic__exit_function_scope(SemanticContext* ctx) {
    semantic__exit_scope(ctx);
}

void semantic__enter_loop_scope(SemanticContext* ctx) {
    semantic__enter_scope_ex(ctx, SCOPE_LOOP);
}

void semantic__exit_loop_scope(SemanticContext* ctx) {
    semantic__exit_scope(ctx);
}

static DataType type_from_type_info(Type* type_info, SemanticContext* ctx) {
    if (!type_info) return TYPE_UNKNOWN;

    if (type_info->pointer_level > 0) return TYPE_POINTER;
    if (type_info->is_reference) return TYPE_REFERENCE;
    if (type_info->is_array) return TYPE_ARRAY;
    if (type_info->compound_types && type_info->compound_count > 0) return TYPE_COMPOUND;

    if (type_info->name) {
        if (strcmp(type_info->name, "Int") == 0) return TYPE_INT;
        if (strcmp(type_info->name, "Real") == 0) return TYPE_REAL;
        if (strcmp(type_info->name, "Char") == 0) return TYPE_CHAR;
        if (strcmp(type_info->name, "Void") == 0) return TYPE_VOID;
        SymbolEntry* entry = semantic__find_symbol(ctx, type_info->name);
        if (entry && entry->type == TYPE_COMPOUND) return TYPE_COMPOUND;
    }

    return TYPE_UNKNOWN;
}

static bool check_name_collision_in_current_scope(SemanticContext* ctx,
                                                  const char* name,
                                                  uint16_t line, uint16_t column) {
    SymbolEntry* existing = find_symbol_in_table(ctx->current_scope, name);
    if (existing) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_REDECLARATION,
                                    line, column, (uint16_t)strlen(name), "semantic",
                                    "Redeclaration of '%s' (previously declared as %s)",
                                    name, semantic__type_to_string(existing->type));
        ctx->has_errors = true;
        return true;
    }
    return false;
}

static void warn_if_shadowing(SemanticContext* ctx, const char* name,
                              uint16_t line, uint16_t column) {
    if (!ctx->warnings_enabled) return;

    SymbolTable* parent = ctx->current_scope->parent;
    while (parent) {
        SymbolEntry* outer = find_symbol_in_table(parent, name);
        if (outer) {
            errhandler__report_error_ex(ERROR_LEVEL_WARNING, ERROR_CODE_SEM_REDECLARATION,
                                        line, column, (uint16_t)strlen(name), "semantic",
                                        "Declaration of '%s' shadows a previous declaration in outer scope",
                                        name);
            break;
        }
        parent = parent->parent;
    }
}

static bool signatures_equal(FunctionSignature* a, FunctionSignature* b) {
    if (!a || !b) return false;
    if (a->return_type != b->return_type) return false;
    if (a->param_count != b->param_count) return false;
    if (a->required_param_count != b->required_param_count) return false;
    if (a->is_variadic != b->is_variadic) return false;

    FunctionParam* pa = a->params;
    FunctionParam* pb = b->params;
    while (pa && pb) {
        if (pa->type != pb->type) return false;
        /* Default values are not part of the signature for compatibility,
           but their presence must be consistent. */
        if ((pa->default_value != NULL) != (pb->default_value != NULL))
            return false;
        pa = pa->next;
        pb = pb->next;
    }
    return (pa == NULL && pb == NULL);
}

static bool check_prototype_match(SemanticContext* ctx,
                                  SymbolEntry* existing,
                                  FunctionSignature* new_sig,
                                  uint16_t line, uint16_t column) {
    FunctionSignature* old_sig = existing->extra.func_sig;
    if (!signatures_equal(old_sig, new_sig)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                    line, column, (uint16_t)strlen(existing->name),
                                    "semantic",
                                    "Conflicting function signature for '%s'", existing->name);
        ctx->has_errors = true;
        return false;
    }
    return true;
}

bool semantic__add_variable_ex(SemanticContext* ctx, SymbolTable* target_scope,
                               const char* name, DataType type, Type* type_info,
                               bool is_constant, const char* state_modifier,
                               InitState init_state, uint16_t line, uint16_t column,
                               const char* access_modifier) {
    if (!ctx || !name) return false;

    SymbolTable* scope = target_scope ? target_scope : ctx->current_scope;

    if (check_name_collision_in_current_scope(ctx, name, line, column)) {
        return false;
    }

    warn_if_shadowing(ctx, name, line, column);

    SymbolEntry* entry = (SymbolEntry*)calloc(1, sizeof(SymbolEntry));
    if (!entry) return false;

    entry->name = u__strduplic(name);
    entry->state_modifier = state_modifier ? u__strduplic(state_modifier) : NULL;
    entry->access_modifier = access_modifier ? u__strduplic(access_modifier) : NULL;
    entry->type = type;
    entry->type_info = type_info;
    entry->is_constant = is_constant;
    entry->init_state = init_state;
    entry->declared_scope = scope->level;
    entry->line = line;
    entry->column = column;
    entry->is_mutable = !is_constant;

    if (!add_symbol_to_table(scope, entry)) {
        free(entry->name);
        free(entry->state_modifier);
        free(entry->access_modifier);
        free(entry);
        return false;
    }

    return true;
}

bool semantic__add_variable(SemanticContext* ctx, const char* name,
                           DataType type, Type* type_info, bool is_constant,
                           uint16_t line, uint16_t column) {
    InitState init = is_constant ? INIT_CONSTANT : INIT_UNINITIALIZED;
    return semantic__add_variable_ex(ctx, ctx->current_scope,
                                    name, type, type_info, is_constant,
                                    NULL, init, line, column, NULL);
}

bool semantic__add_function_ex(SemanticContext* ctx, SymbolTable* target_scope,
                               const char* name,
                               DataType return_type, Type* return_type_info,
                               FunctionParam* params, size_t param_count,
                               size_t required_count, bool is_variadic,
                               uint16_t line, uint16_t column,
                               const char* access_modifier,
                               bool has_body) {
    if (!ctx || !name) return false;

    SymbolTable* scope = target_scope ? target_scope : ctx->current_scope;

    SymbolEntry* existing = find_symbol_in_table(scope, name);
    if (existing) {
        if (existing->type != TYPE_FUNCTION) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_REDECLARATION,
                                        line, column, (uint16_t)strlen(name), "semantic",
                                        "Redeclaration of '%s' as a different kind of symbol", name);
            ctx->has_errors = true;
            goto cleanup_params;
        }

        FunctionSignature new_sig = {
            .return_type = return_type,
            .return_type_info = return_type_info,
            .params = params,
            .param_count = param_count,
            .required_param_count = required_count,
            .is_variadic = is_variadic,
            .has_body = has_body
        };

        if (!check_prototype_match(ctx, existing, &new_sig, line, column)) {
            goto cleanup_params;
        }

        FunctionSignature* old_sig = existing->extra.func_sig;
        if (has_body && !old_sig->has_body) {
            old_sig->has_body = true;
        } else if (has_body && old_sig->has_body) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_REDECLARATION,
                                        line, column, (uint16_t)strlen(name), "semantic",
                                        "Function '%s' already has a body", name);
            ctx->has_errors = true;
            goto cleanup_params;
        }

        /* Free the new parameter list as it is redundant. */
        FunctionParam* p = params;
        while (p) {
            FunctionParam* next = p->next;
            free(p->name);
            if (p->default_value) parser__free_ast_node(p->default_value);
            free(p);
            p = next;
        }
        return true;
    }

    /* No collision – normal insertion. */
    warn_if_shadowing(ctx, name, line, column);

    FunctionSignature* sig = (FunctionSignature*)calloc(1, sizeof(FunctionSignature));
    if (!sig) {
        goto cleanup_params;
    }

    sig->return_type = return_type;
    sig->return_type_info = return_type_info;
    sig->params = params;
    sig->param_count = param_count;
    sig->required_param_count = required_count;
    sig->is_variadic = is_variadic;
    sig->has_body = has_body;

    SymbolEntry* entry = (SymbolEntry*)calloc(1, sizeof(SymbolEntry));
    if (!entry) {
        free(sig);
        goto cleanup_params;
    }

    entry->name = u__strduplic(name);
    entry->state_modifier = u__strduplic("func");
    entry->access_modifier = access_modifier ? u__strduplic(access_modifier) : NULL;
    entry->type = TYPE_FUNCTION;
    entry->type_info = return_type_info;
    entry->is_constant = true;
    entry->init_state = INIT_CONSTANT;
    entry->declared_scope = scope->level;
    entry->line = line;
    entry->column = column;
    entry->extra.func_sig = sig;

    if (!add_symbol_to_table(scope, entry)) {
        free(entry->name);
        free(entry->state_modifier);
        free(entry->access_modifier);
        free(entry);
        FunctionParam* p = sig->params;
        while (p) {
            FunctionParam* next = p->next;
            free(p->name);
            if (p->default_value) parser__free_ast_node(p->default_value);
            free(p);
            p = next;
        }
        free(sig);
        return false;
    }

    return true;

cleanup_params:
    {
        FunctionParam* p = params;
        while (p) {
            FunctionParam* next = p->next;
            free(p->name);
            if (p->default_value) parser__free_ast_node(p->default_value);
            free(p);
            p = next;
        }
    }
    return false;
}

bool semantic__add_function(SemanticContext* ctx, const char* name,
                           DataType return_type, Type* return_type_info,
                           FunctionParam* params, size_t param_count,
                           size_t required_count, uint16_t line, uint16_t column) {
    return semantic__add_function_ex(ctx, ctx->current_scope,
                                    name, return_type, return_type_info,
                                    params, param_count, required_count, false,
                                    line, column, NULL, false);
}

static SymbolEntry* find_member_in_compound(SymbolEntry* compound, const char* member_name) {
    if (!compound || compound->type != TYPE_COMPOUND || !compound->compound_scope) {
        return NULL;
    }

    SymbolEntry* member = find_symbol_in_table(compound->compound_scope, member_name);
    if (member) return member;

    if (compound->base_class) {
        return find_member_in_compound(compound->base_class, member_name);
    }

    return NULL;
}

bool semantic__add_compound_type(SemanticContext* ctx, const char* name,
                                 ASTNode* members_ast, ASTNode* base_class_ast,
                                 uint16_t line, uint16_t column, uint8_t kind) {
    if (!ctx || !name) return false;

    if (check_name_collision_in_current_scope(ctx, name, line, column)) {
        return false;
    }

    warn_if_shadowing(ctx, name, line, column);

    /* Resolve base class if provided (only for classes) */
    SymbolEntry* base = NULL;
    if (kind == 1 && base_class_ast) {
        if (!ast_is_type(base_class_ast, AST_IDENTIFIER)) {
            errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                     base_class_ast->line, base_class_ast->column,
                                     "semantic",
                                     "Base class specifier must be an identifier");
            ctx->has_errors = true;
            return false;
        }
        const char* base_name = base_class_ast->value;
        if (!base_name) {
            errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                     base_class_ast->line, base_class_ast->column,
                                     "semantic",
                                     "Base class name missing");
            ctx->has_errors = true;
            return false;
        }
        base = semantic__find_symbol(ctx, base_name);
        if (!base || base->type != TYPE_COMPOUND || base->compound_kind != 1) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_UNDECLARED_SYMBOL,
                                        base_class_ast->line, base_class_ast->column,
                                        (uint16_t)strlen(base_name), "semantic",
                                        "Base class '%s' is not a declared class", base_name);
            ctx->has_errors = true;
            return false;
        }
    }

    /* Create symbol entry */
    SymbolEntry* entry = (SymbolEntry*)calloc(1, sizeof(SymbolEntry));
    if (!entry) return false;

    entry->name = u__strduplic(name);
    entry->type = TYPE_COMPOUND;
    entry->compound_kind = kind;
    entry->declared_scope = ctx->current_scope->level;
    entry->line = line;
    entry->column = column;
    entry->base_class = base;

    /* Create the scope that will hold members */
    SymbolTable* compound_scope = create_symbol_table(ctx->current_scope, SCOPE_COMPOUND);
    if (!compound_scope) {
        free(entry->name);
        free(entry);
        return false;
    }
    entry->compound_scope = compound_scope;

    /* Add the compound type symbol to the current scope */
    if (!add_symbol_to_table(ctx->current_scope, entry)) {
        destroy_symbol_table(compound_scope);
        free(entry->name);
        free(entry);
        return false;
    }

    /* Process member declarations */
    if (!members_ast) {
        /* A forward declaration without members is allowed */
        return true;
    }

    if (!ast_is_type(members_ast, AST_BLOCK) || !members_ast->extra) {
        errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR, line, column, "semantic",
                                 "Invalid member list for %s '%s'",
                                 kind ? "class" : "struct", name);
        ctx->has_errors = true;
        return true; /* The type is declared but has no members; not a fatal error */
    }

    AST* member_list = (AST*)members_ast->extra;
    SymbolTable* saved_scope = ctx->current_scope;
    ctx->current_scope = compound_scope;

    CompoundMember* debug_list = NULL;
    CompoundMember* debug_last = NULL;

    for (uint16_t i = 0; i < member_list->count; i++) {
        ASTNode* member = member_list->nodes[i];
        if (!member) continue;

        const char* member_access = member->access_modifier ? member->access_modifier : "public";

        if (member->type == AST_VARIABLE_DECLARATION) {
            const char* mod = member->state_modifier;
            if (!mod || (strcmp(mod, "var") != 0 && strcmp(mod, "obj") != 0)) {
                errhandler__report_error(ERROR_CODE_SEM_INVALID_OPERATION,
                                         member->line, member->column, "semantic",
                                         "Member variable must have 'var' or 'obj' modifier");
                ctx->has_errors = true;
                continue;
            }

            DataType mem_type = type_from_type_info(member->variable_type, ctx);
            if (mem_type == TYPE_UNKNOWN && member->default_value) {
                TypeCheckResult init_res = semantic__check_type(ctx, member->default_value);
                if (init_res.valid) mem_type = init_res.type;
            }

            bool is_const = false;
            InitState init = member->default_value ? INIT_FULL : INIT_UNINITIALIZED;

            if (!semantic__add_variable_ex(ctx, compound_scope,
                                           member->value, mem_type, member->variable_type,
                                           is_const, mod, init,
                                           member->line, member->column,
                                           kind == 0 ? NULL : member_access)) {
                ctx->has_errors = true;
            }

            /* Build debug list for output */
            CompoundMember* dbg = (CompoundMember*)malloc(sizeof(CompoundMember));
            if (dbg) {
                dbg->name = u__strduplic(member->value);
                dbg->state_modifier = u__strduplic(mod);
                dbg->access_modifier = kind == 0 ? NULL : u__strduplic(member_access);
                dbg->type = mem_type;
                dbg->type_info = member->variable_type;
                dbg->init_state = init;
                dbg->next = NULL;
                if (!debug_list) debug_list = dbg;
                else debug_last->next = dbg;
                debug_last = dbg;
            }
        }
        else if (member->type == AST_FUNCTION_DECLARATION) {
            const char* func_name = member->value;
            if (!func_name) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         member->line, member->column, "semantic",
                                         "Function member has no name");
                ctx->has_errors = true;
                continue;
            }
            ASTNode* params_node = member->left;
            AST* param_list = (params_node && params_node->type == TYPE_UNKNOWN && params_node->extra)
                              ? (AST*)params_node->extra : NULL;

            FunctionParam* params = NULL;
            FunctionParam* last = NULL;
            size_t param_count = 0;
            size_t required_count = 0;

            if (param_list) {
                for (uint16_t j = 0; j < param_list->count; j++) {
                    ASTNode* pnode = param_list->nodes[j];
                    if (!pnode || pnode->type != AST_VARIABLE_DECLARATION) {
                        errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                                 pnode ? pnode->line : 0,
                                                 pnode ? pnode->column : 0,
                                                 "semantic",
                                                 "Invalid parameter declaration");
                        ctx->has_errors = true;
                        continue;
                    }

                    const char* pname = pnode->value; /* may be NULL for unnamed parameter */
                    Type* ptype_info = pnode->variable_type;
                    DataType ptype = type_from_type_info(ptype_info, ctx);
                    if (ptype == TYPE_UNKNOWN && pnode->default_value) {
                        TypeCheckResult init_res = semantic__check_type(ctx, pnode->default_value);
                        if (init_res.valid) ptype = init_res.type;
                    }

                    FunctionParam* fp = (FunctionParam*)malloc(sizeof(FunctionParam));
                    fp->name = pname ? u__strduplic(pname) : NULL; /* allow NULL name */
                    fp->type = ptype;
                    fp->type_info = ptype_info;
                    fp->default_value = pnode->default_value;
                    fp->next = NULL;

                    if (!params) params = fp;
                    else last->next = fp;
                    last = fp;

                    param_count++;
                    if (!pnode->default_value) required_count++;
                }
            }

            DataType ret_type = TYPE_VOID;
            Type* ret_type_info = member->variable_type;
            if (ret_type_info) {
                ret_type = type_from_type_info(ret_type_info, ctx);
                if (ret_type == TYPE_UNKNOWN) {
                    errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                             member->line, member->column, "semantic",
                                             "Unknown return type for function '%s'", func_name);
                    ctx->has_errors = true;
                }
            }

            if (!semantic__add_function_ex(ctx, compound_scope,
                                           func_name, ret_type, ret_type_info,
                                           params, param_count, required_count, false,
                                           member->line, member->column,
                                           kind == 0 ? NULL : member_access,
                                           member->right != NULL)) {
                ctx->has_errors = true;
            }

            CompoundMember* dbg = (CompoundMember*)malloc(sizeof(CompoundMember));
            if (dbg) {
                dbg->name = u__strduplic(func_name);
                dbg->state_modifier = u__strduplic("func");
                dbg->access_modifier = kind == 0 ? NULL : u__strduplic(member_access);
                dbg->type = TYPE_FUNCTION;
                dbg->type_info = ret_type_info;
                dbg->init_state = INIT_CONSTANT;
                dbg->next = NULL;
                if (!debug_list) debug_list = dbg;
                else debug_last->next = dbg;
                debug_last = dbg;
            }
        }
        else if (member->type == AST_COMPOUND_TYPE) {
            uint8_t inner_kind = (member->state_modifier &&
                                  strcmp(member->state_modifier, "struct") == 0) ? 0 : 1;
            if (!semantic__add_compound_type(ctx, member->value,
                                             member->extra, member->left,
                                             member->line, member->column, inner_kind)) {
                ctx->has_errors = true;
            }
        }
        else {
            errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                     member->line, member->column, "semantic",
                                     "Invalid member in %s",
                                     kind ? "class" : "struct");
            ctx->has_errors = true;
        }
    }

    entry->extra.compound_members = debug_list;
    ctx->current_scope = saved_scope;
    return true;
}

SymbolEntry* semantic__find_symbol(SemanticContext* ctx, const char* name) {
    if (!ctx || !name) return NULL;

    SymbolTable* table = ctx->current_scope;
    while (table) {
        SymbolEntry* entry = find_symbol_in_table(table, name);
        if (entry) return entry;
        table = table->parent;
    }
    return NULL;
}

SymbolEntry* semantic__find_struct_member(SemanticContext* ctx,
                                         const char* struct_name,
                                         const char* field_name) {
    SymbolEntry* struct_entry = semantic__find_symbol(ctx, struct_name);
    if (!struct_entry || struct_entry->type != TYPE_COMPOUND) return NULL;
    return find_member_in_compound(struct_entry, field_name);
}

DataType semantic__type_from_token(TokenType token_type) {
    switch (token_type) {
        case TOKEN_NUMBER: return TYPE_REAL;
        case TOKEN_CHAR:   return TYPE_CHAR;
        default:           return TYPE_UNKNOWN;
    }
}

DataType semantic__type_from_string(const char* type_name) {
    if (!type_name) return TYPE_UNKNOWN;
    if (strcmp(type_name, "Int") == 0) return TYPE_INT;
    if (strcmp(type_name, "Real") == 0) return TYPE_REAL;
    if (strcmp(type_name, "Char") == 0) return TYPE_CHAR;
    if (strcmp(type_name, "Void") == 0) return TYPE_VOID;
    return TYPE_UNKNOWN;
}

bool semantic__types_compatible(DataType t1, DataType t2) {
    if (t1 == t2) return true;
    if ((t1 == TYPE_INT || t1 == TYPE_REAL || t1 == TYPE_CHAR) &&
        (t2 == TYPE_INT || t2 == TYPE_REAL || t2 == TYPE_CHAR))
        return true;
    if (t1 == TYPE_POINTER && t2 == TYPE_POINTER) return true;
    if (t1 == TYPE_REFERENCE && t2 == TYPE_REFERENCE) return true;
    if (t1 == TYPE_ARRAY && t2 == TYPE_ARRAY) return true;
    if (t1 == TYPE_COMPOUND && t2 == TYPE_COMPOUND) return true;
    return false;
}

bool semantic__types_assignable(DataType target, DataType source) {
    return semantic__types_compatible(target, source);
}

bool semantic__types_assignable_ex(DataType target, DataType source,
                                  InitState target_init, InitState source_init) {
    if (!semantic__types_compatible(target, source)) return false;
    if (source_init == INIT_UNINITIALIZED) return false;
    if (target_init == INIT_CONSTANT) return false;
    return true;
}

const char* semantic__type_to_string(DataType type) {
    switch (type) {
        case TYPE_INT:       return "Int";
        case TYPE_REAL:      return "Real";
        case TYPE_CHAR:      return "Char";
        case TYPE_VOID:      return "Void";
        case TYPE_POINTER:   return "pointer";
        case TYPE_REFERENCE: return "reference";
        case TYPE_ARRAY:     return "array";
        case TYPE_FUNCTION:  return "function";
        case TYPE_COMPOUND:  return "compound";
        case TYPE_LABEL:     return "label";
        default:             return "unknown";
    }
}

const char* semantic__init_state_to_string(InitState state) {
    switch (state) {
        case INIT_UNINITIALIZED: return "uninitialized";
        case INIT_PARTIAL:       return "partially initialized";
        case INIT_FULL:          return "fully initialized";
        case INIT_CONSTANT:      return "constant";
        case INIT_DEFAULT:       return "default initialized";
        default:                 return "unknown";
    }
}

static DataType determine_numeric_type(const char* value,
                                       bool* is_int_suffix,
                                       bool* is_real_suffix) {
    *is_int_suffix = false;
    *is_real_suffix = false;
    if (!value) return TYPE_INT;

    size_t len = strlen(value);
    if (len == 0) return TYPE_INT;

    char last = value[len - 1];
    bool has_suffix = false;
    if (last == 'i') {
        *is_int_suffix = true;
        has_suffix = true;
    } else if (last == 'f') {
        *is_real_suffix = true;
        has_suffix = true;
    }

    size_t check_len = len - (has_suffix ? 1 : 0);
    bool has_dot = false;
    bool has_exp = false;
    for (size_t i = 0; i < check_len; i++) {
        if (value[i] == '.') has_dot = true;
        if (value[i] == 'e' || value[i] == 'E') has_exp = true;
    }

    if (has_suffix) {
        if (*is_int_suffix) return TYPE_INT;
        if (*is_real_suffix) return TYPE_REAL;
    }

    return (has_dot || has_exp) ? TYPE_REAL : TYPE_INT;
}

TypeCheckResult semantic__check_type(SemanticContext* ctx, ASTNode* node) {
    TypeCheckResult res = {false, TYPE_UNKNOWN, NULL, INIT_UNINITIALIZED, NULL};
    if (!node) {
        res.error_msg = u__strduplic("Null node");
        return res;
    }

    switch (node->type) {
        case AST_LITERAL_VALUE: {
            TokenType tt = node->operation_type;
            if (tt == TOKEN_NUMBER) {
                bool int_suffix, real_suffix;
                DataType num_type = determine_numeric_type(node->value,
                                                           &int_suffix, &real_suffix);
                res.type = num_type;
                res.init_state = INIT_CONSTANT;
                res.valid = true;
            } else if (tt == TOKEN_CHAR) {
                res.type = TYPE_CHAR;
                res.init_state = INIT_CONSTANT;
                res.valid = true;
            } else {
                res.valid = false;
                res.error_msg = u__strduplic("Invalid literal");
            }
            break;
        }

        case AST_IDENTIFIER: {
            if (!node->value) {
                res.valid = false;
                res.error_msg = u__strduplic("Identifier with null name");
                break;
            }
            SymbolEntry* sym = semantic__find_symbol(ctx, node->value);
            if (!sym) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_UNDECLARED_SYMBOL,
                                            node->line, node->column,
                                            (uint16_t)strlen(node->value), "semantic",
                                            "Undeclared identifier '%s'", node->value);
                ctx->has_errors = true;
                res.error_msg = u__strduplic("Undeclared identifier");
                res.valid = false;
            } else {
                if (sym->type == TYPE_FUNCTION) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_INVALID_OPERATION,
                                                node->line, node->column,
                                                (uint16_t)strlen(node->value), "semantic",
                                                "Function '%s' cannot be used as a value without parentheses",
                                                node->value);
                    ctx->has_errors = true;
                    res.valid = false;
                    res.error_msg = u__strduplic("Function used as value");
                    break;
                }

                res.type = sym->type;
                res.type_info = sym->type_info;
                res.init_state = sym->init_state;
                res.valid = true;

                if (sym->init_state == INIT_UNINITIALIZED &&
                    sym->type != TYPE_FUNCTION && sym->type != TYPE_COMPOUND) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_UNINITIALIZED,
                                                node->line, node->column,
                                                (uint16_t)strlen(node->value), "semantic",
                                                "Use of uninitialized variable '%s'", node->value);
                    ctx->has_errors = true;
                    res.valid = false;
                }
            }
            break;
        }

        case AST_BINARY_OPERATION: {
            if (!ast_has_left(node) || !ast_has_right(node)) {
                res.valid = false;
                res.error_msg = u__strduplic("Binary operation missing operands");
                break;
            }
            TypeCheckResult left = semantic__check_type(ctx, node->left);
            TypeCheckResult right = semantic__check_type(ctx, node->right);

            if (!left.valid || !right.valid) {
                res.valid = false;
                res.error_msg = u__strduplic("Invalid operand type");
                break;
            }

            if (!semantic__types_compatible(left.type, right.type)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Type mismatch: %s and %s",
                         semantic__type_to_string(left.type),
                         semantic__type_to_string(right.type));
                res.error_msg = u__strduplic(msg);
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                            node->line, node->column, 0, "semantic", "%s", msg);
                ctx->has_errors = true;
                res.valid = false;
                break;
            }

            switch (node->operation_type) {
                case TOKEN_PLUS:
                case TOKEN_MINUS:
                case TOKEN_STAR:
                case TOKEN_SLASH:
                case TOKEN_PERCENT:
                    res.type = left.type;
                    break;
                case TOKEN_PIPE:
                case TOKEN_AMPERSAND:
                case TOKEN_CARET:
                case TOKEN_SHL:
                case TOKEN_SHR:
                    if (left.type == TYPE_INT && right.type == TYPE_INT)
                        res.type = TYPE_INT;
                    else {
                        res.valid = false;
                        res.error_msg = u__strduplic("Bitwise ops require integers");
                    }
                    break;
                default:
                    res.type = left.type;
                    break;
            }

            if (res.valid) {
                res.init_state = (left.init_state < right.init_state)
                                 ? left.init_state : right.init_state;
                res.type_info = left.type_info;
            }
            break;
        }

        case AST_UNARY_OPERATION: {
            if (!ast_has_right(node)) {
                res.valid = false;
                res.error_msg = u__strduplic("Unary operation missing operand");
                break;
            }
            TypeCheckResult operand = semantic__check_type(ctx, node->right);
            if (!operand.valid) {
                res.valid = false;
                res.error_msg = u__strduplic("Invalid operand type");
                break;
            }

            switch (node->operation_type) {
                case TOKEN_PLUS:
                case TOKEN_MINUS:
                    if (operand.type == TYPE_INT || operand.type == TYPE_REAL) {
                        res.type = operand.type;
                        res.valid = true;
                    } else {
                        res.valid = false;
                        res.error_msg = u__strduplic("Unary +/- requires numeric");
                    }
                    break;
                case TOKEN_TILDE:
                    if (operand.type == TYPE_INT) {
                        res.type = TYPE_INT;
                        res.valid = true;
                    } else {
                        res.valid = false;
                        res.error_msg = u__strduplic("Bitwise NOT requires int");
                    }
                    break;
                default:
                    res.valid = false;
                    res.error_msg = u__strduplic("Unknown unary operation");
                    break;
            }

            if (res.valid) res.init_state = operand.init_state;
            break;
        }

        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT: {
            if (!ast_has_left(node) || !ast_has_right(node)) {
                res.valid = false;
                res.error_msg = u__strduplic("Assignment missing left or right side");
                break;
            }
            if (!ast_is_type(node->left, AST_IDENTIFIER)) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Left side of assignment must be an identifier");
                ctx->has_errors = true;
                res.valid = false;
                break;
            }

            SymbolEntry* target = semantic__find_symbol(ctx, node->left->value);
            if (!target) {
                res.valid = false;
                break;
            }

            if (target->is_constant) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_ASSIGN_TO_CONST,
                                            node->line, node->column,
                                            (uint16_t)strlen(node->left->value), "semantic",
                                            "Cannot assign to constant '%s'", node->left->value);
                ctx->has_errors = true;
                res.valid = false;
                break;
            }

            TypeCheckResult right = semantic__check_type(ctx, node->right);
            if (!right.valid) {
                res.valid = false;
                break;
            }

            if (!semantic__types_assignable_ex(target->type, right.type,
                                              target->init_state, right.init_state)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Cannot assign %s (%s) to %s (%s)",
                         semantic__type_to_string(right.type),
                         semantic__init_state_to_string(right.init_state),
                         semantic__type_to_string(target->type),
                         semantic__init_state_to_string(target->init_state));
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                            node->line, node->column, 0, "semantic", "%s", msg);
                ctx->has_errors = true;
                res.valid = false;
                break;
            }

            target->init_state = (right.init_state == INIT_FULL || right.init_state == INIT_CONSTANT)
                                 ? INIT_FULL : INIT_PARTIAL;
            res.type = target->type;
            res.type_info = target->type_info;
            res.init_state = target->init_state;
            res.valid = true;
            break;
        }

        case AST_FUNCTION_DECLARATION: {
            /* Function call node: left is identifier, extra is argument list */
            if (!ast_has_left(node) || !ast_is_type(node->left, AST_IDENTIFIER)) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Invalid function call");
                ctx->has_errors = true;
                res.valid = false;
                break;
            }

            const char* func_name = node->left->value;
            if (!func_name) {
                res.valid = false;
                break;
            }
            SymbolEntry* func = semantic__find_symbol(ctx, func_name);
            if (!func || func->type != TYPE_FUNCTION) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_UNDECLARED_SYMBOL,
                                            node->line, node->column,
                                            (uint16_t)strlen(func_name), "semantic",
                                            "Call to undeclared function '%s'", func_name);
                ctx->has_errors = true;
                res.valid = false;
                break;
            }

            FunctionSignature* sig = func->extra.func_sig;
            if (!sig) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Function signature missing");
                ctx->has_errors = true;
                res.valid = false;
                break;
            }

            AST* arg_list = (node->extra && node->extra->type == TYPE_UNKNOWN) ? (AST*)node->extra->extra : NULL;
            size_t arg_count = arg_list ? arg_list->count : 0;

            if (arg_count < sig->required_param_count) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                            node->line, node->column,
                                            (uint16_t)strlen(func_name), "semantic",
                                            "Function '%s' requires at least %zu arguments, got %zu",
                                            func_name, sig->required_param_count, arg_count);
                ctx->has_errors = true;
                res.valid = false;
                break;
            }

            if (arg_count > sig->param_count && !sig->is_variadic) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                            node->line, node->column,
                                            (uint16_t)strlen(func_name), "semantic",
                                            "Function '%s' expects at most %zu arguments, got %zu",
                                            func_name, sig->param_count, arg_count);
                ctx->has_errors = true;
                res.valid = false;
                break;
            }

            FunctionParam* param = sig->params;
            bool ok = true;
            size_t i = 0;
            for (; i < arg_count && param; i++, param = param->next) {
                ASTNode* arg = arg_list->nodes[i];
                TypeCheckResult arg_type = semantic__check_type(ctx, arg);
                if (!arg_type.valid) {
                    ok = false;
                    continue;
                }
                if (!semantic__types_assignable_ex(param->type, arg_type.type,
                                                  INIT_FULL, arg_type.init_state)) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                                node->line, node->column, 0, "semantic",
                                                "Argument %zu of function '%s' type mismatch: expected %s, got %s",
                                                i+1, func_name,
                                                semantic__type_to_string(param->type),
                                                semantic__type_to_string(arg_type.type));
                    ctx->has_errors = true;
                    ok = false;
                }
            }

            for (; param; param = param->next) {
                if (!param->default_value) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                                node->line, node->column, 0, "semantic",
                                                "Missing argument for parameter '%s' of function '%s'",
                                                param->name ? param->name : "(unnamed)", func_name);
                    ctx->has_errors = true;
                    ok = false;
                }
            }

            if (!ok) {
                res.valid = false;
                break;
            }

            res.type = sig->return_type;
            res.type_info = sig->return_type_info;
            res.init_state = (sig->return_type == TYPE_VOID) ? INIT_DEFAULT : INIT_FULL;
            res.valid = true;
            break;
        }

        case AST_CAST: {
            if (!node->variable_type) {
                res.valid = false;
                res.error_msg = u__strduplic("Cast missing type information");
                break;
            }

            Type* target_type = node->variable_type;
            DataType target_dt = type_from_type_info(target_type, ctx);
            if (target_dt == TYPE_UNKNOWN) {
                res.valid = false;
                res.error_msg = u__strduplic("Invalid cast type");
                break;
            }

            if (!ast_has_left(node)) {
                res.valid = false;
                res.error_msg = u__strduplic("Cast missing expression");
                break;
            }

            TypeCheckResult expr_res = semantic__check_type(ctx, node->left);
            if (!expr_res.valid) {
                res.valid = false;
                res.error_msg = expr_res.error_msg;
                expr_res.error_msg = NULL;
                break;
            }

            bool expr_numeric = (expr_res.type == TYPE_INT ||
                                 expr_res.type == TYPE_REAL ||
                                 expr_res.type == TYPE_CHAR);
            bool target_numeric = (target_dt == TYPE_INT ||
                                   target_dt == TYPE_REAL ||
                                   target_dt == TYPE_CHAR);

            if (expr_numeric && target_numeric) {
                res.type = target_dt;
                res.type_info = target_type;
                res.init_state = expr_res.init_state;
                res.valid = true;
            } else {
                res.valid = false;
                res.error_msg = u__strduplic("Invalid cast between types");
            }
            break;
        }

        case AST_LABEL_VALUE: {
            const char* label_name = node->value;
            if (!label_name) {
                res.valid = false;
                res.error_msg = u__strduplic("Label value with null name");
                break;
            }
            SymbolEntry* entry = semantic__find_symbol(ctx, label_name);
            if (!entry || (entry->type != TYPE_LABEL && entry->type != TYPE_FUNCTION)) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_UNDECLARED_SYMBOL,
                                            node->line, node->column,
                                            (uint16_t)strlen(label_name), "semantic",
                                            "Undeclared label or function '%s'", label_name);
                ctx->has_errors = true;
                res.valid = false;
                res.error_msg = u__strduplic("Undeclared label/function");
            } else {
                res.type = (entry->type == TYPE_LABEL) ? TYPE_LABEL : TYPE_FUNCTION;
                res.init_state = INIT_CONSTANT;
                res.valid = true;
            }
            break;
        }

        case AST_PREFIX_INCREMENT:
        case AST_PREFIX_DECREMENT:
        case AST_POSTFIX_INCREMENT:
        case AST_POSTFIX_DECREMENT: {
            ASTNode* operand = (node->type == AST_POSTFIX_INCREMENT ||
                                node->type == AST_POSTFIX_DECREMENT)
                               ? node->left : node->right;

            if (!operand || !ast_is_type(operand, AST_IDENTIFIER)) {
                res.valid = false;
                res.error_msg = u__strduplic("Invalid operand for increment/decrement");
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column,
                                         "semantic", "%s", res.error_msg);
                ctx->has_errors = true;
                break;
            }

            TypeCheckResult op_res = semantic__check_type(ctx, operand);
            if (!op_res.valid) {
                res = op_res;
                break;
            }

            SymbolEntry* sym = semantic__find_symbol(ctx, operand->value);
            if (!sym) {
                res.valid = false;
                break;
            }

            if (sym->is_constant) {
                res.valid = false;
                res.error_msg = u__strduplic("Cannot modify constant");
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_ASSIGN_TO_CONST,
                                            node->line, node->column,
                                            (uint16_t)strlen(operand->value), "semantic",
                                            "Cannot modify constant '%s'", operand->value);
                ctx->has_errors = true;
                break;
            }

            if (sym->init_state == INIT_UNINITIALIZED) {
                res.valid = false;
                res.error_msg = u__strduplic("Uninitialized variable");
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_UNINITIALIZED,
                                            node->line, node->column,
                                            (uint16_t)strlen(operand->value), "semantic",
                                            "Use of uninitialized variable '%s'", operand->value);
                ctx->has_errors = true;
                break;
            }

            res.type = op_res.type;
            res.type_info = op_res.type_info;
            res.init_state = op_res.init_state;
            res.valid = true;
            break;
        }

        default:
            res.valid = true;
            break;
    }

    return res;
}

TypeCheckResult semantic__check_binary_op(SemanticContext* ctx, ASTNode* node) {
    return semantic__check_type(ctx, node);
}

TypeCheckResult semantic__check_unary_op(SemanticContext* ctx, ASTNode* node) {
    return semantic__check_type(ctx, node);
}

TypeCheckResult semantic__check_assignment(SemanticContext* ctx, ASTNode* node) {
    return semantic__check_type(ctx, node);
}

TypeCheckResult semantic__check_function_call(SemanticContext* ctx, ASTNode* node) {
    return semantic__check_type(ctx, node);
}

bool semantic__check_expression(SemanticContext* ctx, ASTNode* node) {
    if (!node) return true;
    TypeCheckResult res = semantic__check_type(ctx, node);
    if (!res.valid && res.error_msg) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                    node->line, node->column, 0, "semantic",
                                    "Type error: %s", res.error_msg);
        ctx->has_errors = true;
        free(res.error_msg);
        return false;
    }
    free(res.error_msg);
    return true;
}

static bool contains_return(ASTNode* node) {
    if (!node) return false;
    if (node->type == AST_RETURN) return true;

    if (node->type == AST_BLOCK) {
        AST* block = (AST*)node->extra;
        if (block) {
            for (uint16_t i = 0; i < block->count; i++) {
                if (contains_return(block->nodes[i])) return true;
            }
        }
        return false;
    }

    if (node->type == AST_IF_STATEMENT) {
        if (node->right && contains_return(node->right)) return true;
        if (node->extra && contains_return(node->extra)) return true;
        return false;
    }

    return false;
}

bool semantic__statement_ensures_return(SemanticContext* ctx, ASTNode* node) {
    (void)ctx;
    if (!node) return false;
    if (node->type == AST_RETURN) return true;

    if (node->type == AST_BLOCK) {
        AST* block = (AST*)node->extra;
        if (!block || block->count == 0) return false;
        ASTNode* last = block->nodes[block->count - 1];
        if (last->type == AST_RETURN) return true;
        if (last->type == AST_IF_STATEMENT) {
            bool then_ret = last->right ? semantic__statement_ensures_return(ctx, last->right) : false;
            bool else_ret = last->extra ? semantic__statement_ensures_return(ctx, last->extra) : false;
            return then_ret && else_ret;
        }
        if (last->type == AST_BLOCK) {
            return semantic__statement_ensures_return(ctx, last);
        }
        return false;
    }

    if (node->type == AST_IF_STATEMENT) {
        bool then_ret = node->right ? semantic__statement_ensures_return(ctx, node->right) : false;
        bool else_ret = node->extra ? semantic__statement_ensures_return(ctx, node->extra) : false;
        return then_ret && else_ret;
    }

    return false;
}

bool semantic__check_block_ends_with_return(SemanticContext* ctx, AST* block_ast) {
    if (!block_ast || block_ast->count == 0) return false;
    ASTNode* last = block_ast->nodes[block_ast->count - 1];
    return semantic__statement_ensures_return(ctx, last);
}

bool semantic__check_statement(SemanticContext* ctx, ASTNode* node) {
    if (!node) return true;

    switch (node->type) {
        case AST_VARIABLE_DECLARATION: {
            const char* name = node->value;
            if (!name) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Variable declaration missing name");
                ctx->has_errors = true;
                return false;
            }
            Type* type_info = node->variable_type;
            DataType type = TYPE_VOID;

            if (type_info) {
                type = type_from_type_info(type_info, ctx);
                if (type == TYPE_UNKNOWN) {
                    errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                             node->line, node->column, "semantic",
                                             "Unknown type for variable '%s'", name);
                    ctx->has_errors = true;
                    return false;
                }
                if (type == TYPE_COMPOUND) {
                    SymbolEntry* comp = semantic__find_symbol(ctx, type_info->name);
                    if (!comp || !comp->compound_scope) {
                        errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                                 node->line, node->column, "semantic",
                                                 "Incomplete type '%s' for variable '%s'",
                                                 type_info->name, name);
                        ctx->has_errors = true;
                        return false;
                    }
                }
            } else if (node->default_value) {
                TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
                if (init_res.valid) {
                    type = init_res.type;
                } else {
                    errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                             node->line, node->column, "semantic",
                                             "Cannot infer type for variable '%s'", name);
                    ctx->has_errors = true;
                    return false;
                }
            } else {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Variable '%s' must have a type or initializer", name);
                ctx->has_errors = true;
                return false;
            }

            bool is_const = (node->state_modifier &&
                             strcmp(node->state_modifier, "const") == 0);
            if (is_const && !node->default_value) {
                errhandler__report_error(ERROR_CODE_SEM_UNINITIALIZED,
                                         node->line, node->column, "semantic",
                                         "Constant '%s' must be initialized", name);
                ctx->has_errors = true;
                return false;
            }

            InitState init = node->default_value ? INIT_FULL : INIT_UNINITIALIZED;
            if (is_const) init = INIT_CONSTANT;

            if (type == TYPE_COMPOUND && node->state_modifier &&
                strcmp(node->state_modifier, "obj") != 0) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Variable of compound type must be declared with 'obj' modifier");
                ctx->has_errors = true;
                return false;
            }

            const char* access_mod = node->access_modifier;
            if (!semantic__add_variable_ex(ctx, ctx->current_scope,
                                           name, type, type_info, is_const,
                                           node->state_modifier, init,
                                           node->line, node->column, access_mod)) {
                return false;
            }

            if (node->default_value) {
                TypeCheckResult init_res = semantic__check_type(ctx, node->default_value);
                if (!init_res.valid) return false;
                if (!semantic__types_assignable_ex(type, init_res.type,
                                                   INIT_UNINITIALIZED, init_res.init_state)) {
                    errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                             node->line, node->column, "semantic",
                                             "Initializer type mismatch for variable '%s'", name);
                    ctx->has_errors = true;
                    return false;
                }
                SymbolEntry* var = semantic__find_symbol(ctx, name);
                if (var) var->init_state = INIT_FULL;
            }

            return true;
        }

        case AST_FUNCTION_DECLARATION: {
            const char* name = node->value;
            if (!name) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Function declaration missing name");
                ctx->has_errors = true;
                return false;
            }
            ASTNode* params_node = node->left;
            ASTNode* body = node->right;
            Type* ret_type_info = node->variable_type;
            DataType ret_type = TYPE_VOID;

            if (ret_type_info) {
                ret_type = type_from_type_info(ret_type_info, ctx);
                if (ret_type == TYPE_UNKNOWN) {
                    errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                             node->line, node->column, "semantic",
                                             "Unknown return type for function '%s'", name);
                    ctx->has_errors = true;
                    return false;
                }
            }

            FunctionParam* params = NULL;
            FunctionParam* last = NULL;
            size_t param_count = 0;
            size_t required_count = 0;
            bool param_list_is_void = false;

            if (params_node && params_node->extra) {
                AST* param_list = (AST*)params_node->extra;
                if (param_list->count == 1) {
                    ASTNode* single = param_list->nodes[0];
                    if (single && single->variable_type && single->variable_type->name &&
                        strcmp(single->variable_type->name, "Void") == 0) {
                        param_list_is_void = true;
                    }
                }

                if (!param_list_is_void) {
                    for (uint16_t i = 0; i < param_list->count; i++) {
                        ASTNode* p = param_list->nodes[i];
                        if (!p || p->type != AST_VARIABLE_DECLARATION) {
                            errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                                     p ? p->line : 0,
                                                     p ? p->column : 0,
                                                     "semantic",
                                                     "Invalid parameter declaration");
                            ctx->has_errors = true;
                            continue;
                        }

                        const char* pname = p->value; /* may be NULL */
                        Type* ptype_info = p->variable_type;
                        DataType ptype = TYPE_VOID;

                        if (ptype_info) {
                            ptype = type_from_type_info(ptype_info, ctx);
                            if (ptype == TYPE_UNKNOWN) {
                                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                                         p->line, p->column, "semantic",
                                                         "Unknown type for parameter '%s'",
                                                         pname ? pname : "(unnamed)");
                                ctx->has_errors = true;
                            }
                        } else if (p->default_value) {
                            TypeCheckResult init_res = semantic__check_type(ctx, p->default_value);
                            if (init_res.valid) ptype = init_res.type;
                        }

                        FunctionParam* param = (FunctionParam*)malloc(sizeof(FunctionParam));
                        param->name = pname ? u__strduplic(pname) : NULL;
                        param->type = ptype;
                        param->type_info = ptype_info;
                        param->default_value = p->default_value;
                        param->next = NULL;

                        if (!params) params = param;
                        else last->next = param;
                        last = param;

                        param_count++;
                        if (!p->default_value) required_count++;
                    }
                }
            }

            const char* access_mod = node->access_modifier;
            bool has_body = (body != NULL);

            if (!semantic__add_function_ex(ctx, ctx->current_scope,
                                           name, ret_type, ret_type_info,
                                           params, param_count, required_count, false,
                                           node->line, node->column, access_mod, has_body)) {
                FunctionParam* p = params;
                while (p) {
                    FunctionParam* next = p->next;
                    free(p->name);
                    free(p);
                    p = next;
                }
                return false;
            }

            if (!has_body) {
                return true; /* prototype only */
            }

            semantic__enter_function_scope(ctx, name, ret_type);

            /* Add parameters to function scope (skip unnamed ones) */
            for (FunctionParam* p = params; p; p = p->next) {
                if (p->name) { /* only add named parameters to local scope */
                    semantic__add_variable_ex(ctx, ctx->current_scope,
                                             p->name, p->type, p->type_info,
                                             false, "var", INIT_FULL,
                                             node->line, node->column, NULL);
                }
            }

            for (FunctionParam* p = params; p; p = p->next) {
                if (p->default_value) {
                    TypeCheckResult def_res = semantic__check_type(ctx, p->default_value);
                    if (!def_res.valid) {
                        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                                    node->line, node->column,
                                                    p->name ? (uint16_t)strlen(p->name) : 0, "semantic",
                                                    "Invalid default value for parameter '%s' of function '%s'",
                                                    p->name ? p->name : "(unnamed)", name);
                        ctx->has_errors = true;
                    } else if (!semantic__types_assignable_ex(p->type, def_res.type,
                                                              INIT_UNINITIALIZED, def_res.init_state)) {
                        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                                    node->line, node->column,
                                                    p->name ? (uint16_t)strlen(p->name) : 0, "semantic",
                                                    "Default value type mismatch for parameter '%s' of function '%s'",
                                                    p->name ? p->name : "(unnamed)", name);
                        ctx->has_errors = true;
                    }
                }
            }

            bool body_ok = true;
            if (body) {
                if (body->type == AST_BLOCK) {
                    semantic__enter_scope(ctx);
                    AST* block = (AST*)body->extra;
                    if (block) {
                        for (uint16_t i = 0; i < block->count; i++) {
                            if (!semantic__check_statement(ctx, block->nodes[i])) {
                                body_ok = false;
                            }
                        }
                    }
                    semantic__exit_scope(ctx);
                } else {
                    if (!semantic__check_statement(ctx, body)) {
                        body_ok = false;
                    }
                }
            }

            /* All functions (including Void) must guarantee a return. */
            if (!semantic__statement_ensures_return(ctx, body)) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_MISSING_RETURN,
                                            node->line, node->column,
                                            (uint16_t)strlen(name), "semantic",
                                            "Function '%s' must contain a return statement", name);
                ctx->has_errors = true;
                body_ok = false;
            }

            semantic__exit_function_scope(ctx);
            return body_ok;
        }

        case AST_COMPOUND_TYPE:
            return semantic__check_compound_type(ctx, node);

        case AST_BLOCK: {
            semantic__enter_scope(ctx);
            AST* block = (AST*)node->extra;
            bool ok = true;
            if (block) {
                for (uint16_t i = 0; i < block->count; i++) {
                    if (!semantic__check_statement(ctx, block->nodes[i])) {
                        ok = false;
                    }
                }
            }
            semantic__exit_scope(ctx);
            return ok;
        }

        case AST_IF_STATEMENT: {
            if (!semantic__check_expression(ctx, node->left)) return false;

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

        case AST_RETURN: {
            if (!ctx->in_function) {
                errhandler__report_error(ERROR_CODE_SEM_INVALID_OPERATION,
                                         node->line, node->column, "semantic",
                                         "Return statement outside function");
                ctx->has_errors = true;
                return false;
            }

            if (node->left) {
                TypeCheckResult ret_val = semantic__check_type(ctx, node->left);
                if (!ret_val.valid) return false;

                if (!semantic__types_assignable_ex(ctx->current_return_type, ret_val.type,
                                                  INIT_UNINITIALIZED, ret_val.init_state)) {
                    errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                             node->line, node->column, "semantic",
                                             "Return type mismatch: expected %s, got %s",
                                             semantic__type_to_string(ctx->current_return_type),
                                             semantic__type_to_string(ret_val.type));
                    ctx->has_errors = true;
                    return false;
                }
            } else {
                if (ctx->current_return_type != TYPE_VOID) {
                    errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                             node->line, node->column, "semantic",
                                             "Non-void function must return a value");
                    ctx->has_errors = true;
                    return false;
                }
            }
            return true;
        }

        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
            return semantic__check_expression(ctx, node);

        case AST_IDENTIFIER: {
            if (!node->value) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Identifier with null name");
                ctx->has_errors = true;
                return false;
            }
            SymbolEntry* sym = semantic__find_symbol(ctx, node->value);
            if (sym && sym->type == TYPE_FUNCTION) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_INVALID_OPERATION,
                                            node->line, node->column,
                                            (uint16_t)strlen(node->value), "semantic",
                                            "Function '%s' cannot be used as a statement without parentheses",
                                            node->value);
                ctx->has_errors = true;
                return false;
            }
            return semantic__check_expression(ctx, node);
        }

        case AST_LABEL_DECLARATION: {
            const char* label_name = node->value;
            if (!label_name) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Invalid label declaration");
                ctx->has_errors = true;
                return false;
            }

            if (check_name_collision_in_current_scope(ctx, label_name,
                                                      node->line, node->column)) {
                return false;
            }
            warn_if_shadowing(ctx, label_name, node->line, node->column);

            SymbolEntry* entry = (SymbolEntry*)calloc(1, sizeof(SymbolEntry));
            if (!entry) return false;

            entry->name = u__strduplic(label_name);
            entry->type = TYPE_LABEL;
            entry->init_state = INIT_CONSTANT;
            entry->is_constant = true;
            entry->declared_scope = ctx->current_scope->level;
            entry->line = node->line;
            entry->column = node->column;

            if (!add_symbol_to_table(ctx->current_scope, entry)) {
                free(entry->name);
                free(entry);
                return false;
            }
            return true;
        }

        case AST_JUMP: {
            if (!node->left) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Jump target missing");
                ctx->has_errors = true;
                return false;
            }

            TypeCheckResult target_res = semantic__check_type(ctx, node->left);
            if (!target_res.valid) {
                return false;
            }

            if (target_res.type != TYPE_LABEL && target_res.type != TYPE_FUNCTION) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                         node->line, node->column, "semantic",
                                         "Jump target must be a label or a function");
                ctx->has_errors = true;
                return false;
            }
            return true;
        }

        case AST_BREAK:
        case AST_CONTINUE: {
            if (!ctx->in_loop) {
                errhandler__report_error(ERROR_CODE_SEM_INVALID_OPERATION,
                                         node->line, node->column, "semantic",
                                         "%s statement outside loop",
                                         (node->type == AST_BREAK) ? "Break" : "Continue");
                ctx->has_errors = true;
                return false;
            }
            return true;
        }

        default:
            return semantic__check_expression(ctx, node);
    }
}

bool semantic__check_compound_type(SemanticContext* ctx, ASTNode* node) {
    if (!node || !node->value) {
        errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR, 0, 0, "semantic",
                                 "Invalid compound type definition");
        ctx->has_errors = true;
        return false;
    }

    uint8_t kind;
    if (node->state_modifier && strcmp(node->state_modifier, "struct") == 0) {
        kind = 0;
    } else if (node->state_modifier && strcmp(node->state_modifier, "class") == 0) {
        kind = 1;
    } else {
        errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR,
                                 node->line, node->column, "semantic",
                                 "Expected 'struct' or 'class' modifier");
        ctx->has_errors = true;
        return false;
    }

    ASTNode* members = node->extra;
    ASTNode* base = node->left;
    return semantic__add_compound_type(ctx, node->value, members, base,
                                       node->line, node->column, kind);
}

static bool final_verification(SemanticContext* ctx) {
    bool ok = true;

    SymbolTable* global = ctx->global_scope;
    for (size_t i = 0; i < global->capacity; i++) {
        SymbolEntry* entry = global->entries[i];
        while (entry) {
            if (entry->type == TYPE_FUNCTION) {
                FunctionSignature* sig = entry->extra.func_sig;
                if (entry->is_used && !sig->has_body) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_UNDEFINED_VAR,
                                                entry->line, entry->column,
                                                (uint16_t)strlen(entry->name), "semantic",
                                                "Function '%s' is declared but never defined",
                                                entry->name);
                    ctx->has_errors = true;
                    ok = false;
                }
                if (!entry->is_used && ctx->warnings_enabled) {
                    errhandler__report_error_ex(ERROR_LEVEL_WARNING, ERROR_CODE_SEM_UNUSED_VARIABLE,
                                                entry->line, entry->column,
                                                (uint16_t)strlen(entry->name), "semantic",
                                                "Function '%s' is declared but never used",
                                                entry->name);
                }
            } else if (entry->type == TYPE_COMPOUND) {
                if (!entry->compound_scope) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_TYPE_ERROR,
                                                entry->line, entry->column,
                                                (uint16_t)strlen(entry->name), "semantic",
                                                "Compound type '%s' is incomplete", entry->name);
                    ctx->has_errors = true;
                    ok = false;
                }
            } else {
                if (!entry->is_used && ctx->warnings_enabled) {
                    errhandler__report_error_ex(ERROR_LEVEL_WARNING, ERROR_CODE_SEM_UNUSED_VARIABLE,
                                                entry->line, entry->column,
                                                (uint16_t)strlen(entry->name), "semantic",
                                                "Variable '%s' is declared but never used",
                                                entry->name);
                }
            }
            entry = entry->next;
        }
    }

    return ok;
}

bool semantic__analyze(SemanticContext* ctx, AST* ast) {
    if (!ctx || !ast) return false;

    ctx->has_errors = false;

    for (uint16_t i = 0; i < ast->count; i++) {
        if (!semantic__check_statement(ctx, ast->nodes[i])) {
            ctx->has_errors = true;
            if (ctx->exit_on_error) {
                errhandler__report_error(ERROR_CODE_SEM_TYPE_ERROR, 0, 0,
                                         "semantic", "Semantic analysis failed");
                exit(1);
            }
        }
    }

    if (!final_verification(ctx)) {
        ctx->has_errors = true;
    }

    return !ctx->has_errors;
}

bool semantic__check_scope_initialization(SemanticContext* ctx, SymbolTable* scope) {
    (void)ctx;
    (void)scope;
    return true;
}

void semantic__check_shadowing(SemanticContext* ctx, const char* name,
                               uint16_t line, uint16_t column) {
    warn_if_shadowing(ctx, name, line, column);
}

VisibilityResult semantic__check_visibility(SemanticContext* ctx, const char* name,
                                           bool require_initialized,
                                           bool allow_shadowing) {
    VisibilityResult res = {false, NULL, SCOPE_GLOBAL, NULL};
    (void)allow_shadowing;

    SymbolEntry* entry = semantic__find_symbol(ctx, name);
    if (!entry) {
        res.error_msg = u__strduplic("Undeclared symbol");
        return res;
    }

    if (require_initialized && entry->init_state == INIT_UNINITIALIZED) {
        res.error_msg = u__strduplic("Uninitialized variable");
        return res;
    }

    res.visible = true;
    res.entry = entry;
    res.found_in_scope = entry->declared_scope;
    return res;
}

bool semantic__mark_symbol_used(SemanticContext* ctx, const char* name) {
    SymbolEntry* entry = semantic__find_symbol(ctx, name);
    if (entry) entry->is_used = true;
    return entry != NULL;
}

bool semantic__update_init_state(SemanticContext* ctx, const char* name,
                                 InitState new_state) {
    SymbolEntry* entry = semantic__find_symbol(ctx, name);
    if (!entry) return false;
    if (entry->is_constant && entry->init_state == INIT_CONSTANT) return false;
    if (entry->init_state == INIT_UNINITIALIZED &&
        (new_state == INIT_FULL || new_state == INIT_PARTIAL)) {
        entry->init_state = new_state;
        return true;
    }
    if (entry->init_state == INIT_PARTIAL && new_state == INIT_FULL) {
        entry->init_state = new_state;
        return true;
    }
    return false;
}

InitState semantic__get_init_state(SemanticContext* ctx, const char* name) {
    SymbolEntry* entry = semantic__find_symbol(ctx, name);
    return entry ? entry->init_state : INIT_UNINITIALIZED;
}

bool semantic__can_modify_symbol(SemanticContext* ctx, const char* name) {
    SymbolEntry* entry = semantic__find_symbol(ctx, name);
    return entry && !entry->is_constant && entry->is_mutable;
}

bool semantic__validate_mutation(SemanticContext* ctx, const char* name,
                                 uint16_t line, uint16_t column) {
    SymbolEntry* entry = semantic__find_symbol(ctx, name);
    if (!entry) return false;
    if (entry->is_constant) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ERROR_CODE_SEM_ASSIGN_TO_CONST,
                                    line, column, (uint16_t)strlen(name), "semantic",
                                    "Cannot assign to constant '%s'", name);
        ctx->has_errors = true;
        return false;
    }
    return true;
}

bool semantic__check_field_access(SemanticContext* ctx, ASTNode* node) {
    TypeCheckResult res = semantic__check_type(ctx, node);
    return res.valid;
}

bool semantic__is_valid_struct_member_modifier(const char* mod) {
    return mod && (strcmp(mod, "var") == 0 || strcmp(mod, "obj") == 0);
}

size_t semantic__get_symbol_count(SemanticContext* ctx) {
    if (!ctx || !ctx->global_scope) return 0;
    return ctx->global_scope->count;
}

SymbolTable* semantic__get_global_table(SemanticContext* ctx) {
    return ctx ? ctx->global_scope : NULL;
}

bool semantic__has_errors(SemanticContext* ctx) {
    return ctx ? ctx->has_errors : false;
}

bool semantic__warnings_enabled(SemanticContext* ctx) {
    return ctx ? ctx->warnings_enabled : false;
}
