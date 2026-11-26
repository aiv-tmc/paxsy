#include "common.h"
#include "lower_int.h"

/*
 * lower_use.c - import (use directive) support for lowering.
 * Semantic analysis validates use directives against its own scope
 * chain; lowering keeps a parallel alias stack so that a bare
 * reference can be rewritten to the qualified name the imported
 * definition carries ("square" -> "math::square"). Whole-module
 * imports ("use math;") are expanded through a member registry that
 * a pre-pass in lower_run fills from every module declaration, so a
 * use resolves no matter whether its module is declared before or
 * after it. Single-symbol imports alias the bare tail of the path to
 * the full path for both local modules and external packages ("use
 * std::inout;" aliases inout to std::inout). Aliases are scoped:
 * entering a named module pushes a mark and leaving it pops every
 * alias added inside, which mirrors the scope the directive lands
 * in during semantic analysis.
 */

static char *ldup(const char *s) {
    char *r = malloc(strlen(s) + 1);
    strcpy(r, s);
    return r;
}

void luse_init(Lower *l) {
    l->aliases = NULL;
    l->n_aliases = l->a_cap = 0;
    l->a_depth = 0;
    l->registry = NULL;
    l->nreg = l->reg_cap = 0;
}

void luse_free(Lower *l) {
    for (int i = 0; i < l->nreg; i++) {
        free(l->registry[i].mod);
        free(l->registry[i].member);
    }
    free(l->registry);
    free(l->aliases);
    l->registry = NULL;
    l->aliases = NULL;
    l->nreg = l->n_aliases = 0;
}

/* Alias stack -------------------------------------------------------- */

void luse_push(Lower *l) {
    if (l->a_depth == 32) return;
    l->a_marks[l->a_depth++] = l->n_aliases;
}

void luse_pop(Lower *l) {
    /* Alias strings are interned in the module arena (see luse_add),
     * so leaving a scope only rewinds the table; the names stay
     * valid for the whole backend pipeline. */
    if (l->a_depth == 0) return;
    l->n_aliases = l->a_marks[--l->a_depth];
}

static void luse_add(Lower *l, const char *alias, const char *target) {
    if (l->n_aliases == l->a_cap) {
        int cap = l->a_cap ? l->a_cap * 2 : 16;
        l->aliases = realloc(l->aliases, (size_t)cap * sizeof(*l->aliases));
        l->a_cap = cap;
    }
    /* Aliases are stored into HIR reference nodes, so they live in
     * the module arena rather than in per-scope heap copies. */
    l->aliases[l->n_aliases].alias = hir_name(l->mod, alias);
    l->aliases[l->n_aliases].target = hir_name(l->mod, target);
    l->n_aliases++;
}

static const char *luse_find(Lower *l, const char *name) {
    for (int i = l->n_aliases - 1; i >= 0; i--)
        if (strcmp(l->aliases[i].alias, name) == 0)
            return l->aliases[i].target;
    return NULL;
}

/* Member registry ---------------------------------------------------- */

static void lreg_add(Lower *l, const char *mod, const char *member) {
    int i;
    for (i = 0; i < l->nreg; i++)
        if (strcmp(l->registry[i].mod, mod) == 0 &&
            strcmp(l->registry[i].member, member) == 0)
            return;
    if (l->nreg == l->reg_cap) {
        int cap = l->reg_cap ? l->reg_cap * 2 : 32;
        l->registry = realloc(l->registry, (size_t)cap * sizeof(*l->registry));
        l->reg_cap = cap;
    }
    l->registry[l->nreg].mod = ldup(mod);
    l->registry[l->nreg].member = ldup(member);
    l->nreg++;
}

static int lreg_has_module(Lower *l, const char *mod) {
    for (int i = 0; i < l->nreg; i++)
        if (strcmp(l->registry[i].mod, mod) == 0) return 1;
    return 0;
}

static int lreg_has_member(Lower *l, const char *mod, const char *member) {
    for (int i = 0; i < l->nreg; i++)
        if (strcmp(l->registry[i].mod, mod) == 0 &&
            strcmp(l->registry[i].member, member) == 0)
            return 1;
    return 0;
}

/* Registers every named declaration inside a module body under the
 * module's qualified name; nested named modules register their own
 * name with the parent and recurse with the extended path. */
static void lreg_scan(Lower *l, const Node *mod, const char *parent) {
    char full[256];
    int i;
    if (mod->name)
        snprintf(full, sizeof(full), "%s%s%s", parent,
                 parent[0] ? "::" : "", mod->name);
    else
        snprintf(full, sizeof(full), "%s", parent);
    for (i = 0; i < mod->item_count; i++) {
        const Node *it = mod->items[i];
        switch (it->kind) {
        case NK_FUNC: case NK_STRUCT: case NK_UNION: case NK_ENUM:
        case NK_LET: case NK_STATIC_CONST:
            if (it->name) lreg_add(l, full, it->name);
            break;
        case NK_MODULE:
            if (it->name) {
                lreg_add(l, full, it->name);
                lreg_scan(l, it, full);
            } else {
                lreg_scan(l, it, full);
            }
            break;
        default:
            break;
        }
    }
}

void luse_registry_build(Lower *l, const Node *program) {
    int i;
    for (i = 0; i < program->item_count; i++)
        if (program->items[i]->kind == NK_MODULE)
            lreg_scan(l, program->items[i], "");
}

/* use directives ------------------------------------------------------ */

/* "use math;" - alias every member of the module to its qualified
 * name. Modules absent from the registry are external (std, installed
 * packages): nothing is aliasable and references stay qualified. */
static void luse_apply_all(Lower *l, const char *mod) {
    int i;
    if (!lreg_has_module(l, mod)) return;
    for (i = 0; i < l->nreg; i++)
        if (strcmp(l->registry[i].mod, mod) == 0) {
            char target[512];
            snprintf(target, sizeof(target), "%s::%s", mod,
                     l->registry[i].member);
            luse_add(l, l->registry[i].member, target);
        }
}

/* "use math::square;" / "use std::inout;" - alias the bare tail of
 * the path to the full path. Local modules are validated against the
 * registry; a missing member was already reported by semantic
 * analysis, so lowering stays silent. External packages alias the
 * path as written. */
static void luse_apply_one(Lower *l, const Node *use, const char *colon) {
    char module[256];
    const char *scan = use->name, *best_sep = NULL;
    size_t best_len = 0;
    (void)colon;
    while ((scan = strstr(scan, "::")) != NULL) {
        size_t n = (size_t)(scan - use->name);
        if (n < sizeof(module)) {
            memcpy(module, use->name, n);
            module[n] = '\0';
            if (lreg_has_module(l, module)) {
                best_len = n;
                best_sep = scan;
            }
        }
        scan += 2;
    }
    if (best_sep) {
        const char *member = best_sep + 2;
        if (strstr(member, "::")) {
            /* A deeper registered module is handled as a whole import
             * by luse_apply; otherwise this is not a known local item. */
            return;
        }
        if (!lreg_has_member(l, module, member)) return;
    } else {
        /* External package path: preserve the existing full spelling. */
        const char *first_sep = strstr(use->name, "::");
        if (!first_sep) return;
        best_sep = first_sep;
        best_len = (size_t)(first_sep - use->name);
    }
    (void)best_len;
    luse_add(l, strrchr(use->name, ':') + 1, use->name);
}

void luse_apply(Lower *l, const Node *use) {
    const char *colon = strstr(use->name, "::");
    if (!colon || lreg_has_module(l, use->name)) {
        luse_apply_all(l, use->name);
        return;
    }
    luse_apply_one(l, use, colon);
}

/* Global name resolution: an import alias wins over the member
 * prefix; both leave ordinary names untouched. */
const char *lglobal_name(Lower *l, const char *name) {
    const char *alias = luse_find(l, name);
    if (alias) return alias;
    return lower_resolve_name(l, name);
}
