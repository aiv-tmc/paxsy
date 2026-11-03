#include "common.h"
#include "sema_int.h"
#include "stdpath.h"

/*
 * sema_mod.c - module namespace handling for semantic analysis.
 * Three jobs live here. First, the namespace table: every module
 * declared in the program registers as a local namespace, installed
 * packages and `std` register as external ones. Second, qualified
 * registration: the members of a named module are entered into the
 * global scope under `module::name`, which is what makes qualified
 * references (`geometry::distance`) resolvable and what allows full
 * member validation. Third, imports: a use directive connects the
 * named module to the current scope. A use directive that names a
 * module absent from the table is a hard error: the build stops in
 * sema and no code is generated for a program that references a
 * module that does not exist. For an external package the member
 * named by a qualified use is checked against the package manifest
 * (<pkg>.toml), so `use std::inout;` accepts a published library
 * and `use std::typo;` is refused in sema.
 *
 * Enum variants are deliberately not qualified: they are registered
 * as file-scope values (by sm_register_decl) and every emitter names
 * their macros after the bare variant, so `m::Red` would name a
 * symbol no backend defines.
 */

ModSym *sm_module_add(Sema *sm, const char *name, int local) {
    for (int i = 0; i < sm->nmods; i++)
        if (strcmp(sm->mods[i].name, name) == 0) {
            sm->mods[i].local |= local;
            return &sm->mods[i];
        }
    if (sm->nmods == sm->mods_cap) {
        int cap = sm->mods_cap ? sm->mods_cap * 2 : 16;
        sm->mods = realloc(sm->mods, (size_t)cap * sizeof(ModSym));
        sm->mods_cap = cap;
    }
    ModSym *m = &sm->mods[sm->nmods++];
    m->name = sm_strdup(name);
    m->local = local;
    m->members = NULL;
    m->nmembers = m->members_cap = 0;
    return m;
}

ModSym *sm_module_find(Sema *sm, const char *name) {
    for (int i = 0; i < sm->nmods; i++)
        if (strcmp(sm->mods[i].name, name) == 0) return &sm->mods[i];
    return NULL;
}

int sm_module_known(Sema *sm, const char *name) {
    return sm_module_find(sm, name) != NULL;
}

/* Records one member name; the list is deduplicated because the
 * registration pass can visit a module body more than once. */
void sm_member_record(ModSym *m, const char *name) {
    for (int i = 0; i < m->nmembers; i++)
        if (strcmp(m->members[i], name) == 0) return;
    if (m->nmembers == m->members_cap) {
        int cap = m->members_cap ? m->members_cap * 2 : 8;
        m->members = realloc(m->members, (size_t)cap * sizeof(char *));
        m->members_cap = cap;
    }
    m->members[m->nmembers++] = sm_strdup(name);
}

/* Registers one declaration under a "m::" prefix. This mirrors
 * sm_register_decl name for name, with two deliberate differences:
 * the scoped name carries the prefix, and enum variants are skipped
 * (they stay file-scope values; see the file header). */
void sm_register_qualified(Sema *sm, const char *prefix, Node *d) {
    char qn[256];
    switch (d->kind) {
    case NK_FUNC: case NK_COMPTIME_FN:
        snprintf(qn, sizeof(qn), "%s%s", prefix, d->name);
        if (d->kind == NK_FUNC && (d->flags & NF_PROTOTYPE)) {
            sm_proto_add(sm, sm_strdup(qn), d->tok);
        } else {
            for (int i = 0; i < sm->nprotos; i++)
                if (!sm->protos[i].defined &&
                    strcmp(sm->protos[i].name, qn) == 0)
                    sm->protos[i].defined = 1;
        }
        sm_scope_put(sm, qn, d, 0);
        break;
    case NK_STRUCT: case NK_UNION:
        snprintf(qn, sizeof(qn), "%s%s", prefix, d->name);
        sm_scope_put(sm, qn, d, 1);
        break;
    case NK_ENUM:
        snprintf(qn, sizeof(qn), "%s%s", prefix, d->name);
        sm_scope_put(sm, qn, d, 1);
        break;
    case NK_COMPTIME_CONST:
        snprintf(qn, sizeof(qn), "%s%s", prefix, d->name);
        sm_scope_put(sm, qn, d,
                     d->child[0] && d->child[0]->kind == NK_NAME ? 1 : 0);
        break;
    case NK_COMPTIME_BLOCK: case NK_LET_MULTI:
        for (int i = 0; i < d->item_count; i++)
            sm_register_qualified(sm, prefix, d->items[i]);
        break;
    case NK_LET:
        snprintf(qn, sizeof(qn), "%s%s", prefix, d->name);
        sm_scope_put(sm, qn, d, 0);
        break;
    case NK_MODULE:
        /* A nested module extends the prefix; an unnamed nested
         * module is transparent and keeps the current one. */
        if (d->name) {
            char p2[256];
            snprintf(p2, sizeof(p2), "%s%s::", prefix, d->name);
            for (int i = 0; i < d->item_count; i++)
                sm_register_qualified(sm, p2, d->items[i]);
        } else {
            for (int i = 0; i < d->item_count; i++)
                sm_register_qualified(sm, prefix, d->items[i]);
        }
        break;
    default:
        break;
    }
}

/* Whole-module import. Local modules alias every member into the
 * current scope under its bare name, which makes bare references
 * work. External packages keep references qualified: the alias pass
 * of lowering expands whole-module imports through the module
 * registry compiled from the package sources, and semantic analysis
 * accepts any qualified path into an external namespace, so
 * `use std;` followed by `std::inout::p(...)` resolves without
 * member aliasing here. */
static void sm_import_all(Sema *sm, Node *use, ModSym *m) {
    (void)use;
    if (!m->local) return;
    for (int i = 0; i < m->nmembers; i++) {
        char qn[256];
        Sym *s;
        snprintf(qn, sizeof(qn), "%s::%s", m->name, m->members[i]);
        s = sm_scope_find(sm->cur, qn);
        if (s) sm_scope_put(sm, m->members[i], s->decl, s->is_type);
    }
}

/* Single-symbol import: `use geometry::distance;` aliases distance,
 * `use std::inout;` connects the inout library of the std package.
 * An unknown module is a hard error. For an external package the
 * member is checked against the package manifest when one exists,
 * so a mistyped library name is refused here instead of surfacing
 * later as an unresolved call; packages without a manifest stay
 * opaque and the member is accepted as written. */
static void sm_import_one(Sema *sm, Node *use, const char *colon,
                          const char *first) {
    ModSym *m = sm_module_find(sm, first);
    Sym *s;
    if (!m) {
        sm_error(sm, &use->tok, "unknown module '%s' in use directive",
                 first);
        return;
    }
    if (!m->local) {
        char names[32][64];
        int n = pkg_exports(first, names, 32);
        if (n > 0) {
            int found = 0;
            for (int i = 0; i < n; i++)
                if (strcmp(names[i], colon + 2) == 0) found = 1;
            if (!found) {
                sm_error(sm, &use->tok,
                         "module '%s' has no member '%s'",
                         first, colon + 2);
                return;
            }
        }
        /* The bare alias carries no declaration node; references are
         * rewritten to the qualified path during lowering. */
        sm_scope_put(sm, colon + 2, NULL, 0);
        return;
    }
    s = sm_scope_find(sm->cur, use->name);
    if (!s) {
        sm_error(sm, &use->tok, "module '%s' has no member '%s'",
                 first, colon + 2);
        return;
    }
    sm_scope_put(sm, colon + 2, s->decl, s->is_type);
}

/* Resolves one use directive. A bare `use name;` connects every
 * member of a local module, or the whole external package; a
 * qualified `use name::member;` connects one member. Both forms
 * reject an unknown module with a hard error, so a program cannot
 * reference modules that do not exist. */
void sm_import(Sema *sm, Node *use) {
    const char *colon = strstr(use->name, "::");
    char first[64];
    ModSym *m;
    if (!colon) {
        m = sm_module_find(sm, use->name);
        if (!m) {
            sm_error(sm, &use->tok, "unknown module '%s' in use directive",
                     use->name);
            return;
        }
        sm_import_all(sm, use, m);
        return;
    }
    {
        size_t len = (size_t)(colon - use->name);
        if (len >= sizeof(first)) len = sizeof(first) - 1;
        memcpy(first, use->name, len);
        first[len] = '\0';
    }
    sm_import_one(sm, use, colon, first);
}
