#include "common.h"
#include "lower_int.h"

#include <ctype.h>

/* Compile-time type parameter specialization. */

static void lower_generic_add(Lower *l, const Node *decl,
                              const Node *module, const char *qualified) {
    for (int i = 0; i < l->n_generic_functions; i++)
        if (strcmp(l->generic_functions[i].name, qualified) == 0) return;
    if (l->n_generic_functions == l->generic_functions_cap) {
        int cap = l->generic_functions_cap ? l->generic_functions_cap * 2 : 8;
        void *mem = realloc(l->generic_functions,
            (size_t)cap * sizeof(*l->generic_functions));
        if (!mem) {
            diag_report_full(l->diag, DK_ERROR,
                decl->tok.path ? decl->tok.path : "<lower>",
                decl->tok.line, decl->tok.col,
                decl->tok.len > 0 ? decl->tok.len : 1,
                "out of memory collecting generic functions");
            return;
        }
        l->generic_functions = mem;
        l->generic_functions_cap = cap;
    }
    char *name = malloc(strlen(qualified) + 1);
    if (!name) return;
    strcpy(name, qualified);
    l->generic_functions[l->n_generic_functions].decl = decl;
    l->generic_functions[l->n_generic_functions].module = module;
    l->generic_functions[l->n_generic_functions].name = name;
    l->n_generic_functions++;
}

static void lower_collect_generics_ctx(Lower *l, const Node *d,
                                       const char *prefix,
                                       const Node *module) {
    if (d->kind == NK_FUNC) {
        for (int i = 0; i < d->item_count; i++) {
            if (!(d->items[i]->flags & NF_STATIC)) continue;
            char name[512];
            snprintf(name, sizeof(name), "%s%s", prefix, d->name);
            lower_generic_add(l, d, module, name);
            return;
        }
    }
    if (d->kind == NK_MODULE && d->name) {
        char nested[512];
        snprintf(nested, sizeof(nested), "%s%s::", prefix, d->name);
        for (int i = 0; i < d->item_count; i++)
            lower_collect_generics_ctx(l, d->items[i], nested, d);
        return;
    }
    if (d->kind == NK_MODULE || d->kind == NK_STATIC_BLOCK ||
        d->kind == NK_LET_MULTI) {
        for (int i = 0; i < d->item_count; i++)
            lower_collect_generics_ctx(l, d->items[i], prefix, module);
    }
}

void lower_collect_generics(Lower *l, const Node *d, const char *prefix) {
    lower_collect_generics_ctx(l, d, prefix, NULL);
}

static void lower_generic_collect_members(Lower *l, const Node *module) {
    if (!module) return;
    for (int i = 0; i < module->item_count; i++) {
        const Node *item = module->items[i];
        if ((item->kind == NK_FUNC || item->kind == NK_STRUCT ||
             item->kind == NK_UNION || item->kind == NK_ENUM ||
             item->kind == NK_LET || item->kind == NK_STATIC_CONST ||
             item->kind == NK_MODULE) && item->name) {
            if (l->nmembers == l->members_cap) {
                int cap = l->members_cap ? l->members_cap * 2 : 16;
                l->members = realloc(l->members, (size_t)cap * sizeof(*l->members));
                l->members_cap = cap;
            }
            l->members[l->nmembers++] = item->name;
        }
    }
}

static const LGenericFn *lower_generic_find(Lower *l, const char *name) {
    for (int i = 0; i < l->n_generic_functions; i++)
        if (strcmp(l->generic_functions[i].name, name) == 0)
            return &l->generic_functions[i];
    return NULL;
}

static const char *lower_literal_type(const Node *arg) {
    if (!arg || arg->kind != NK_LITERAL) return NULL;
    switch (arg->tok.kind) {
    case TK_FLOAT_LIT: return "Real64";
    case TK_STRING_LIT: return "String";
    case TK_CHAR_LIT: return "Char";
    default: return "Int64";
    }
}

static const char *lower_find_decl_type(const Node *n, const char *name,
                                       int depth) {
    if (!n || depth > 64) return NULL;
    if ((n->kind == NK_PARAM || n->kind == NK_LET) && n->name &&
        strcmp(n->name, name) == 0) {
        if (n->child[0] && n->child[0]->kind == NK_TYPE_NAME)
            return n->child[0]->name;
        return lower_literal_type(n->child[1]);
    }
    for (int i = n->item_count - 1; i >= 0; i--) {
        const char *found = lower_find_decl_type(n->items[i], name, depth + 1);
        if (found) return found;
    }
    for (int i = 3; i >= 0; i--) {
        const char *found = lower_find_decl_type(n->child[i], name, depth + 1);
        if (found) return found;
    }
    return NULL;
}

static const char *lower_infer_type_name(Lower *l, const Node *arg) {
    if (!arg) return "Int64";
    if (arg->kind == NK_CAST && arg->child[1] &&
        arg->child[1]->kind == NK_TYPE_NAME)
        return arg->child[1]->name;
    const char *literal = lower_literal_type(arg);
    if (literal) return literal;
    if (arg->kind == NK_NAME) {
        const char *found = lower_find_decl_type(l->current_fn,
                                                 arg->name, 0);
        if (!found && l->program)
            for (int i = 0; i < l->program->item_count && !found; i++)
                found = lower_find_decl_type(l->program->items[i],
                                             arg->name, 0);
        if (found) return found;
    }
    return "Int64";
}

static int lower_type_alias_push(Lower *l, const char *name,
                                 const char *target) {
    if (l->n_type_aliases == l->type_aliases_cap) {
        int cap = l->type_aliases_cap ? l->type_aliases_cap * 2 : 8;
        void *mem = realloc(l->type_aliases,
            (size_t)cap * sizeof(*l->type_aliases));
        if (!mem) return -1;
        l->type_aliases = mem;
        l->type_aliases_cap = cap;
    }
    l->type_aliases[l->n_type_aliases].name = name;
    l->type_aliases[l->n_type_aliases].target = target;
    l->n_type_aliases++;
    return 0;
}

static int lower_hir_has_function(Lower *l, const char *name) {
    HNode *root = hir_root(l->mod);
    for (int i = 0; i < root->nitems; i++)
        if (root->items[i]->name &&
            strcmp(root->items[i]->name, name) == 0) return 1;
    return 0;
}

HNode *lower_generic_call(Lower *l, const Node *call) {
    const Node *callee = call->child[0];
    if (!callee || callee->kind != NK_NAME) return NULL;
    const char *resolved_name = lglobal_name(l, callee->name);
    const LGenericFn *entry = lower_generic_find(l, resolved_name);
    if (!entry) return NULL;
    const Node *generic = entry->decl;
    const char *generic_name = entry->name;

    int type_index = -1;
    for (int i = 0; i < generic->item_count; i++) {
        if (generic->items[i]->flags & NF_STATIC) {
            if (type_index >= 0) {
                lerr(l, &call->tok,
                     "only one static type parameter is supported per generic function");
                return lnode(l, HK_CONST_INT, call->tok);
            }
            type_index = i;
        }
    }
    if (type_index < 0) return NULL;

    const Node *type_arg = NULL;
    int has_default = generic->items[type_index]->child[0] != NULL;
    int explicit_type = call->item_count == generic->item_count;
    if (explicit_type) type_arg = call->items[type_index];
    else if (!has_default || call->item_count != generic->item_count - 1) {
        lerr(l, &call->tok, "missing or unexpected static type argument");
        return lnode(l, HK_CONST_INT, call->tok);
    }

    const char *type_name = type_arg && type_arg->kind == NK_NAME
        ? type_arg->name : lower_infer_type_name(l,
            call->item_count ? call->items[0] : NULL);
    if (type_arg && type_arg->kind != NK_NAME) {
        lerr(l, &type_arg->tok, "static type argument must be a type name");
        return lnode(l, HK_CONST_INT, call->tok);
    }
    if (strcmp(type_name, "Auto") == 0) {
        int first_runtime = explicit_type && type_index == 0 ? 1 : 0;
        type_name = lower_infer_type_name(l,
            first_runtime < call->item_count ? call->items[first_runtime] : NULL);
    }

    char *specialized = malloc(512);
    if (!specialized) {
        lerr(l, &call->tok, "out of memory specializing generic function");
        return lnode(l, HK_CONST_INT, call->tok);
    }
    size_t off = (size_t)snprintf(specialized, 512, "%s__", generic_name);
    for (const char *p = type_name; *p && off + 1 < 512; p++)
        specialized[off++] = (char)(isalnum((unsigned char)*p) || *p == '_'
                                    ? *p : '_');
    specialized[off] = '\0';

    const char *specialized_name = hir_name(l->mod, specialized);
    if (!lower_hir_has_function(l, specialized_name)) {
        Node clone = *generic;
        Node **params = calloc((size_t)generic->item_count, sizeof(Node *));
        if (!params) {
            free(specialized);
            lerr(l, &call->tok, "out of memory specializing generic parameters");
            return lnode(l, HK_CONST_INT, call->tok);
        }
        int nparams = 0;
        for (int i = 0; i < generic->item_count; i++)
            if (!(generic->items[i]->flags & NF_STATIC))
                params[nparams++] = generic->items[i];
        clone.name = (char *)specialized_name;
        clone.items = params;
        clone.item_count = nparams;
        if (lower_type_alias_push(l, generic->items[type_index]->name,
                                  type_name) != 0) {
            free(params);
            free(specialized);
            lerr(l, &call->tok, "out of memory binding generic type");
            return lnode(l, HK_CONST_INT, call->tok);
        }
        /* A generic declaration is specialized at its call site, but
         * its unqualified sibling calls and imports belong to the
         * module where the generic was declared, not the caller. */
        char saved_prefix[sizeof(l->prefix)];
        snprintf(saved_prefix, sizeof(saved_prefix), "%s", l->prefix);
        char *saved_members = (char *)l->members;
        int saved_nmembers = l->nmembers, saved_members_cap = l->members_cap;
        snprintf(l->prefix, sizeof(l->prefix), "%s", generic_name);
        char *last_sep = strrchr(l->prefix, ':');
        if (last_sep && last_sep > l->prefix && last_sep[-1] == ':')
            last_sep[1] = '\0';
        else
            l->prefix[0] = '\0';
        l->members = NULL;
        l->nmembers = l->members_cap = 0;
        lower_generic_collect_members(l, entry->module);
        luse_push(l);
        if (entry->module)
            for (int i = 0; i < entry->module->item_count; i++)
                if (entry->module->items[i]->kind == NK_USE)
                    luse_apply(l, entry->module->items[i]);
        lower_func(l, &clone);
        luse_pop(l);
        free(l->members);
        l->members = (char **)saved_members;
        l->nmembers = saved_nmembers;
        l->members_cap = saved_members_cap;
        snprintf(l->prefix, sizeof(l->prefix), "%s", saved_prefix);
        l->n_type_aliases--;
        free(params);
    }

    HNode *result = lnode(l, HK_CALL, call->tok);
    HNode *target = lnode(l, HK_GLOBAL_REF, callee->tok);
    target->name = (char *)specialized_name;
    result->child[0] = target;
    free(specialized);
    for (int i = 0; i < call->item_count; i++) {
        if (explicit_type && i == type_index) continue;
        hir_add(result, lower_expr(l, call->items[i]));
    }
    return result;
}

void lower_context_free(Lower *l) {
    free(l->members);
    free(l->file_members);
    free(l->type_aliases);
    for (int i = 0; i < l->n_generic_functions; i++)
        free(l->generic_functions[i].name);
    free(l->generic_functions);
    luse_free(l);
}

