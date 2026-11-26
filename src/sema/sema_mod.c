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
    case NK_FUNC: case NK_STATIC_FN:
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
    case NK_STATIC_CONST: {
        snprintf(qn, sizeof(qn), "%s%s", prefix, d->name);
        int is_type = d->child[1] && d->child[1]->kind == NK_NAME &&
                      sm_type_resolves(sm, d->child[1]->name);
        if (is_type) d->flags |= NF_TYPE_ALIAS;
        sm_scope_put(sm, qn, d, is_type);
        break;
    }
    case NK_STATIC_BLOCK: case NK_LET_MULTI:
        for (int i = 0; i < d->item_count; i++)
            sm_register_qualified(sm, prefix, d->items[i]);
        break;
    case NK_LET:
        snprintf(qn, sizeof(qn), "%s%s", prefix, d->name);
        sm_scope_put(sm, qn, d, 0);
        break;
    case NK_MODULE:
        /* A nested module is a namespace in its own right. Register
         * its full path so `use outer::inner;` can import its members,
         * while keeping the parent module's member list coherent. */
        if (d->name) {
            char full[256], p2[256];
            size_t plen = strlen(prefix);
            if (plen >= 2 && prefix[plen - 1] == ':' && prefix[plen - 2] == ':')
                plen -= 2;
            size_t separator_len = plen ? 2u : 0u;
            if (plen + separator_len + strlen(d->name) + 1 < sizeof(full)) {
                memcpy(full, prefix, plen);
                full[plen] = '\0';
                if (full[0]) strncat(full, "::", sizeof(full) - strlen(full) - 1);
                strncat(full, d->name, sizeof(full) - strlen(full) - 1);
                ModSym *nested = sm_module_add(sm, full, 1);
                (void)nested;
                if (plen) {
                    char parent[256];
                    if (plen < sizeof(parent)) {
                        memcpy(parent, prefix, plen);
                        parent[plen] = '\0';
                        ModSym *pm = sm_module_find(sm, parent);
                        if (pm) sm_member_record(pm, d->name);
                    }
                }
                snprintf(qn, sizeof(qn), "%s", full);
                sm_scope_put(sm, qn, d, 0);
                size_t full_len = strlen(full);
                if (full_len + 3 < sizeof(p2)) {
                    memcpy(p2, full, full_len);
                    memcpy(p2 + full_len, "::", 3);
                    for (int i = 0; i < d->item_count; i++) {
                        if (d->items[i]->name)
                            sm_member_record(nested, d->items[i]->name);
                        sm_register_qualified(sm, p2, d->items[i]);
                    }
                } else {
                    sm_error(sm, &d->tok, "nested module path is too long");
                }
            }
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
        if (!s) continue;
        Sym *existing = sm_scope_find(sm->cur, m->members[i]);
        if (existing) {
            if (existing->decl == s->decl && existing->is_type == s->is_type)
                continue;
            sm_error(sm, &use->tok,
                     "import of '%s' conflicts with an existing name",
                     m->members[i]);
            continue;
        }
        sm_scope_put(sm, m->members[i], s->decl, s->is_type);
    }
}

/* Imports `use <first>::<member>;`: validates the member against the
 * package manifest when one exists, and refuses the member outright
 * when the package resolves to a directory without a manifest (a
 * resolvable package must declare its exports; only a package that
 * cannot be located at all stays opaque). Aliasing is global because
 * module use directives already behave that way here. */
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
        /* Only the module part of the path is validated against the
         * package manifest; anything deeper names a member of that
         * module (a function, a constant) and is accepted. */
        char modname[64];
        const char *tail = colon + 2;
        const char *tailcol = strstr(tail, "::");
        size_t mlen = tailcol ? (size_t)(tailcol - tail) : strlen(tail);
        char names[PAXSY_MAX_MODULES][64];
        int n = pkg_modules(first, names, PAXSY_MAX_MODULES);
        if (n != 0) {
            int found = 0;
            if (mlen >= sizeof(modname)) mlen = sizeof(modname) - 1;
            memcpy(modname, tail, mlen);
            modname[mlen] = '\0';
            for (int i = 0; i < n; i++)
                if (strcmp(names[i], modname) == 0) found = 1;
            if (!found) {
                sm_error(sm, &use->tok,
                         "module '%s' has no member '%s'",
                         first, modname);
                return;
            }
        }
        /* The bare alias carries no declaration node; references are
         * rewritten to the qualified path during lowering. */
        Sym *existing = sm_scope_find(sm->cur, colon + 2);
        if (existing) {
            if (!existing->decl) return;
            sm_error(sm, &use->tok,
                     "import of '%s' conflicts with an existing name",
                     colon + 2);
            return;
        }
        sm_scope_put(sm, colon + 2, NULL, 0);
        return;
    }
    s = sm_scope_find(sm->cur, use->name);
    if (!s) {
        sm_error(sm, &use->tok, "module '%s' has no member '%s'",
                 first, colon + 2);
        return;
    }
    const char *alias = strrchr(use->name, ':');
    alias = alias ? alias + 1 : use->name;
    Sym *existing = sm_scope_find(sm->cur, alias);
    if (existing) {
        if (existing->decl == s->decl && existing->is_type == s->is_type)
            return;
        sm_error(sm, &use->tok,
                 "import of '%s' conflicts with an existing name", alias);
        return;
    }
    sm_scope_put(sm, alias, s->decl, s->is_type);
}

/* Resolves one use directive. A bare `use name;` connects every
 * member of a local module, or the whole external package; a
 * qualified `use name::member;` connects one member. Both forms
 * reject an unknown module with a hard error, so a program cannot
 * reference modules that do not exist. */
void sm_import(Sema *sm, Node *use) {
    const char *colon = strstr(use->name, "::");
    char first[256];
    ModSym *m;
    /* Prefer the longest registered namespace prefix. This is what
     * distinguishes `use outer::inner;` (whole nested module) from
     * `use outer::inner::item;` (one member of that module). */
    if (colon) {
        const char *scan = use->name;
        const char *best_sep = NULL;
        ModSym *best = NULL;
        while ((scan = strstr(scan, "::")) != NULL) {
            size_t n = (size_t)(scan - use->name);
            if (n < 256) {
                char candidate[256];
                memcpy(candidate, use->name, n);
                candidate[n] = '\0';
                ModSym *found = sm_module_find(sm, candidate);
                if (found) { best = found; best_sep = scan; }
            }
            scan += 2;
        }
        if (best && best_sep && best_sep[2]) {
            const char *tail = best_sep + 2;
            if (!strstr(tail, "::")) {
                /* The registered prefix is the parent namespace;
                 * continue through the established member importer. */
            } else {
                /* A deeper path may itself be a registered module. */
                size_t tail_len = strlen(tail);
                if (tail_len < 256) {
                    char nested[256];
                    snprintf(nested, sizeof(nested), "%s", use->name);
                    if (sm_module_find(sm, nested)) {
                        sm_import_all(sm, use, sm_module_find(sm, nested));
                        return;
                    }
                }
            }
        }
        if (sm_module_find(sm, use->name)) {
            sm_import_all(sm, use, sm_module_find(sm, use->name));
            return;
        }
    }
    if (!colon) {
        m = sm_module_find(sm, use->name);
        if (!m) {
            sm_error(sm, &use->tok, "unknown module '%s' in use directive",
                     use->name);
            return;
        }
        sm_import_all(sm, use, m);
        if (!m->local) {
            /* The module's short name becomes an alias for its
             * fully qualified spelling. */
            const char *seg = strrchr(use->name, ':');
            sm_xalias_add(sm, seg ? seg + 1 : use->name, use->name);
        }
        return;
    }
    {
        size_t len = (size_t)(colon - use->name);
        if (len >= sizeof(first)) len = sizeof(first) - 1;
        memcpy(first, use->name, len);
        first[len] = '\0';
    }
    sm_import_one(sm, use, colon, first);
    m = sm_module_find(sm, first);
    if (m && !m->local) {
        /* Member use: the member's short name (the last path
         * segment) denotes the fully qualified spelling. */
        const char *seg = strrchr(use->name, ':');
        sm_xalias_add(sm, seg ? seg + 1 : use->name, use->name);
    }
}
