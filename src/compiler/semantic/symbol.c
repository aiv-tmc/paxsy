#include "../semantic/symbol.h"
#include "../../interfaces/utils/common.h"
#include "type.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Hash function for strings */
static uint32_t hash_string(const char* str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

/* Grow the scope hash table */
static bool grow_scope(Scope* scope) {
    if (!scope) return false;
    size_t old_cap = scope->capacity;
    Symbol** old_table = scope->table;
    size_t new_cap = old_cap ? old_cap * 2 : 8;
    Symbol** new_table = calloc(new_cap, sizeof(Symbol*));
    if (!new_table) return false;
    scope->table = new_table;
    scope->capacity = new_cap;
    /* Rehash existing symbols into the new, zeroed table */
    for (size_t i = 0; i < old_cap; ++i) {
        Symbol* sym = old_table[i];
        if (!sym) continue;
        size_t new_idx = hash_string(sym->name) % new_cap;
        while (new_table[new_idx]) {
            new_idx = (new_idx + 1) % new_cap;
        }
        new_table[new_idx] = sym;
    }
    free(old_table);
    return true;
}

/* Create a new scope */
Scope* scope_create(Scope* parent, ScopeLevel level) {
    Scope* scope = calloc(1, sizeof(Scope));
    if (!scope) return NULL;
    scope->parent = parent;
    scope->level = level;
    scope->capacity = 0;
    scope->count = 0;
    scope->table = NULL;
    return scope;
}

/* Destroy a scope */
void scope_destroy(Scope* scope) {
    if (!scope) return;
    free(scope->table);
    free(scope);
}

/* Insert a symbol into a scope */
bool scope_insert(Scope* scope, Symbol* sym) {
    if (!scope || !sym || !sym->name) return false;
    if (scope->count * 2 >= scope->capacity) {
        if (!grow_scope(scope)) return false;
    }
    size_t idx = hash_string(sym->name) % scope->capacity;
    while (scope->table[idx]) {
        if (strcmp(scope->table[idx]->name, sym->name) == 0) {
            return false;
        }
        idx = (idx + 1) % scope->capacity;
    }
    scope->table[idx] = sym;
    scope->count++;
    return true;
}

/* Look up a symbol in the scope chain */
Symbol* scope_lookup(Scope* scope, const char* name) {
    while (scope) {
        Symbol* found = scope_lookup_local(scope, name);
        if (found) return found;
        scope = scope->parent;
    }
    return NULL;
}

/* Look up a symbol in a single scope */
Symbol* scope_lookup_local(Scope* scope, const char* name) {
    if (!scope || !name || scope->capacity == 0) return NULL;
    size_t idx = hash_string(name) % scope->capacity;
    for (size_t i = 0; i < scope->capacity; ++i) {
        Symbol* sym = scope->table[(idx + i) % scope->capacity];
        if (!sym) continue;
        if (strcmp(sym->name, name) == 0) {
            return sym;
        }
    }
    return NULL;
}

/* Iterate over all symbols in a scope */
void scope_foreach(Scope* scope, void (*callback)(Symbol*, void*), void* userdata) {
    if (!scope) return;
    for (size_t i = 0; i < scope->capacity; ++i) {
        if (scope->table[i]) {
            callback(scope->table[i], userdata);
        }
    }
}

/* Create a new symbol */
Symbol* symbol_create(const char* name, SymbolKind kind, struct Type* type,
                      uint32_t line, uint32_t column) {
    Symbol* sym = (Symbol*)memory_allocate_zero(sizeof(Symbol));
    if (!sym) return NULL;
    sym->name = u__strduplic(name);
    if (!sym->name) {
        free(sym);
        return NULL;
    }
    sym->kind = kind;
    sym->type = type;
    sym->line = line;
    sym->column = column;
    sym->is_public = false;
    sym->is_extern = false;
    sym->is_used = false;
    sym->is_initialized = false;
    sym->is_comptime_builtin = false;
    sym->is_comptime = false;
    sym->comptime_eval = NULL;
    memset(&sym->extra, 0, sizeof(sym->extra));
    return sym;
}

/* Copy parameter list */
static ParamInfo* copy_param_list(ParamInfo* orig) {
    if (!orig) return NULL;
    ParamInfo head, *tail = &head;
    for (ParamInfo* p = orig; p; p = p->next) {
        ParamInfo* np = (ParamInfo*)calloc(1, sizeof(ParamInfo));
        if (!np) return NULL;
        np->name = u__strduplic(p->name);
        if (!np->name) { free(np); return NULL; }
        np->type = type_copy(p->type);
        np->next = NULL;
        tail->next = np;
        tail = np;
    }
    return head.next;
}

/* Copy field list */
static FieldInfo* copy_field_list(FieldInfo* orig) {
    if (!orig) return NULL;
    FieldInfo head, *tail = &head;
    for (FieldInfo* f = orig; f; f = f->next) {
        FieldInfo* nf = (FieldInfo*)calloc(1, sizeof(FieldInfo));
        if (!nf) return NULL;
        nf->name = u__strduplic(f->name);
        if (!nf->name) { free(nf); return NULL; }
        nf->type = type_copy(f->type);
        nf->next = NULL;
        tail->next = nf;
        tail = nf;
    }
    return head.next;
}

/* Copy method list */
static MethodInfo* copy_method_list(MethodInfo* orig) {
    if (!orig) return NULL;
    MethodInfo head, *tail = &head;
    for (MethodInfo* m = orig; m; m = m->next) {
        MethodInfo* nm = (MethodInfo*)calloc(1, sizeof(MethodInfo));
        if (!nm) return NULL;
        nm->name = u__strduplic(m->name);
        if (!nm->name) { free(nm); return NULL; }
        nm->decl = m->decl;
        nm->next = NULL;
        tail->next = nm;
        tail = nm;
    }
    return head.next;
}

/* Deep copy a symbol */
Symbol* symbol_copy_deep(Symbol* orig) {
    if (!orig) return NULL;
    Symbol* copy = symbol_create(orig->name, orig->kind, type_copy(orig->type),
                                 orig->line, orig->column);
    if (!copy) return NULL;
    copy->is_public = orig->is_public;
    copy->is_extern = orig->is_extern;
    copy->is_used = orig->is_used;
    copy->is_initialized = orig->is_initialized;
    copy->is_comptime_builtin = orig->is_comptime_builtin;
    copy->is_comptime = orig->is_comptime;
    copy->comptime_eval = orig->comptime_eval;

    switch (orig->kind) {
        case SYMBOL_FUNCTION:
            copy->extra.func.params = copy_param_list(orig->extra.func.params);
            copy->extra.func.param_count = orig->extra.func.param_count;
            /* return_type is owned by the copied function type */
            copy->extra.func.return_type =
                (copy->type && copy->type->kind == TYPE_KIND_FUNCTION)
                    ? copy->type->u.function.return_type : NULL;
            copy->extra.func.return_is_error = orig->extra.func.return_is_error;
            copy->extra.func.comptime_body = orig->extra.func.comptime_body;
            break;
        case SYMBOL_TYPE:
            copy->extra.fields = copy_field_list(orig->extra.fields);
            copy->extra.methods = copy_method_list(orig->extra.methods);
            break;
        case SYMBOL_MODULE:
            copy->extra.module_scope = orig->extra.module_scope;
            break;
        case SYMBOL_CONSTANT:
            copy->extra.const_value = orig->extra.const_value;
            break;
        case SYMBOL_ENUM_CONSTANT:
            copy->extra.enum_type = orig->extra.enum_type;
            break;
        default:
            break;
    }
    return copy;
}

/* Free a symbol */
void symbol_free(Symbol* sym) {
    if (!sym) return;
    free(sym->name);
    type_free(sym->type);

    if (sym->kind == SYMBOL_FUNCTION) {
        ParamInfo* p = sym->extra.func.params;
        while (p) {
            ParamInfo* next = p->next;
            free(p->name);
            type_free(p->type);
            free(p);
            p = next;
        }
        /* extra.func.return_type is owned by sym->type (the function type) */
    } else if (sym->kind == SYMBOL_TYPE) {
        FieldInfo* f = sym->extra.fields;
        while (f) {
            FieldInfo* next = f->next;
            free(f->name);
            type_free(f->type);
            free(f);
            f = next;
        }
        MethodInfo* m = sym->extra.methods;
        while (m) {
            MethodInfo* next = m->next;
            free(m->name);
            free(m);
            m = next;
        }
    }
    free(sym);
}
