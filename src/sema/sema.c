#include "common.h"
#include "sema_int.h"
#include "pkgdir.h"
#include "stdpath.h"

/*
 * sema.c - the core of semantic analysis: diagnostics helpers, the
 * symbol table, type resolution, the expression walker, declaration
 * registration (including prototype tracking) and the entry point.
 * Structural walkers live in sema_walk.c; the module namespace table
 * and import machinery live in sema_mod.c.
 *
 * Name resolution distinguishes two shapes. A bare name resolves
 * through the lexical scope chain. A qualified name `m::f` must
 * either be present in the scope chain (a member of a module
 * declared in this program, registered as "m::f" during the
 * registration pass, or an alias introduced by a use directive) or
 * live under an external namespace (std, an installed package), in
 * which case any member path is accepted because the declarations
 * are only known at link time. A qualified name under a local module
 * that is not in scope is a hard error naming the missing member.
 */

char *sm_strdup(const char *s) {
    char *r = malloc(strlen(s) + 1);
    strcpy(r, s);
    return r;
}

void sm_error(Sema *sm, const Token *t, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag_report_full(sm->diag, DK_ERROR,
                     t->path ? t->path : sm->path,
                     t->line, t->col, t->len, "%s", buf);
}

void sm_warn(Sema *sm, const Token *t, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag_report_full(sm->diag, DK_WARNING,
                     t->path ? t->path : sm->path,
                     t->line, t->col, t->len, "%s", buf);
}

Sym *sm_scope_find(Scope *s, const char *name) {
    for (; s; s = s->parent)
        for (Sym *q = s->head; q; q = q->next)
            if (strcmp(q->name, name) == 0) return q;
    return NULL;
}

/* Inserts into the current scope. A body supersedes a pending
 * prototype of the same name in that scope; any other duplicate is
 * an error. `decl` may be NULL for opaque aliases (single-symbol
 * imports from external packages) that exist only to satisfy name
 * resolution. */
/* Remembers that the short spelling 'alias' denotes the fully
 * qualified external name 'target'. The first registration wins so
 * shadowing keeps source-order semantics. */
void sm_xalias_add(Sema *sm, const char *alias, const char *target) {
    for (int i = 0; i < sm->n_xalias; i++)
        if (strcmp(sm->xalias[i].alias, alias) == 0) return;
    if (sm->n_xalias == sm->xalias_cap) {
        sm->xalias_cap = sm->xalias_cap ? sm->xalias_cap * 2 : 16;
        sm->xalias = realloc(sm->xalias,
                             (size_t)sm->xalias_cap * sizeof(*sm->xalias));
    }
    sm->xalias[sm->n_xalias].alias = sm_strdup(alias);
    sm->xalias[sm->n_xalias].target = sm_strdup(target);
    sm->n_xalias++;
}

const char *sm_xalias_find(Sema *sm, const char *name) {
    for (int i = 0; i < sm->n_xalias; i++)
        if (strcmp(sm->xalias[i].alias, name) == 0)
            return sm->xalias[i].target;
    return NULL;
}

void sm_scope_clear(Scope *scope) {
    Sym *entry = scope ? scope->head : NULL;
    while (entry) {
        Sym *next = entry->next;
        free(entry->name);
        free(entry);
        entry = next;
    }
    if (scope) scope->head = NULL;
}

void sm_scope_destroy(Scope *scope) {
    if (!scope) return;
    sm_scope_clear(scope);
    free(scope);
}

void sm_scope_put(Sema *sm, const char *name, Node *decl, int is_type) {
    for (Sym *q = sm->cur->head; q; q = q->next)
        if (strcmp(q->name, name) == 0) {
            if (q->decl && q->decl->kind == NK_FUNC &&
                (q->decl->flags & NF_PROTOTYPE) &&
                decl && decl->kind == NK_FUNC &&
                !(decl->flags & NF_PROTOTYPE)) {
                q->decl = decl;      /* the prototype is fulfilled */
                return;
            }
            if (decl)
                sm_error(sm, &decl->tok, "duplicate definition of '%s'", name);
            else {
                Token at = {0};
                at.line = 1;
                at.col = 1;
                at.len = 1;
                sm_error(sm, &at, "duplicate import or definition of '%s'", name);
            }
            return;
        }
    Sym *s = malloc(sizeof(Sym));
    s->name = sm_strdup(name);
    s->decl = decl;
    s->is_type = is_type;
    s->next = sm->cur->head;
    sm->cur->head = s;
}

/* Prototype tracking is shared with the qualified registration in
 * sema_mod.c, so the table grows here but is appended from both
 * units. */
void sm_proto_add(Sema *sm, char *name, Token tok) {
    if (sm->nprotos == sm->protos_cap) {
        int cap = sm->protos_cap ? sm->protos_cap * 2 : 32;
        sm->protos = realloc(sm->protos, (size_t)cap * sizeof(*sm->protos));
        sm->protos_cap = cap;
    }
    sm->protos[sm->nprotos].name = name;
    sm->protos[sm->nprotos].tok = tok;
    sm->protos[sm->nprotos].defined = 0;
    sm->nprotos++;
}

static int is_builtin_type(const char *n) {
    static const char *const k[] = {
        "Int8","Int16","Int32","Int64","Int128","Int256",
        "UInt8","UInt16","UInt32","UInt64","UInt128","UInt256",
        "Real32","Real64","UTF8","UTF16","UTF32","Bool","Void","Auto",
        "Int","UInt","Real","Char", NULL };
    for (int i = 0; k[i]; i++) if (strcmp(k[i], n) == 0) return 1;
    return 0;
}

/* Qualified type names resolve exactly like qualified value names:
 * the full path must be in scope for local modules; external
 * namespaces accept any path. Bare names check the builtins first
 * and the type symbols second. */
int sm_type_resolves(Sema *sm, const char *name) {
    if (strchr(name, ':')) {
        Sym *s = sm_scope_find(sm->cur, name);
        ModSym *m;
        char first[64];
        const char *end = strchr(name, ':');
        size_t n = (size_t)(end - name);
        if (n >= sizeof(first)) n = sizeof(first) - 1;
        memcpy(first, name, n);
        first[n] = '\0';
        m = sm_module_find(sm, first);
        if (s) return s->is_type;
        if (!m) return 0;
        return !m->local;
    }
    if (is_builtin_type(name)) return 1;
    Sym *s = sm_scope_find(sm->cur, name);
    return s && s->is_type == 1;
}

int sm_is_type_node(const Node *n) {
    return n->kind >= NK_TYPE_NAME && n->kind <= NK_TYPE_ERROR;
}

void sm_walk_type(Sema *sm, Node *t) {
    if (!t) return;
    if (t->kind == NK_TYPE_NAME) {
        if (!sm_type_resolves(sm, t->name))
            sm_error(sm, &t->tok, "unknown type '%s'", t->name);
        return;
    }
    if (t->kind == NK_TYPE_TUPLE) {
        for (int i = 0; i < t->item_count; i++) sm_walk_type(sm, t->items[i]);
        return;
    }
    sm_walk_type(sm, t->child[0]);        /* ptr / chan / error / elem */
    if (t->kind == NK_TYPE_ARRAY && t->child[1]) sm_walk_expr(sm, t->child[1]);
}

/* Routes a child of an expression: types to sm_walk_type, blocks to a
 * scoped walk, everything else to sm_walk_expr. */
static void walk_child(Sema *sm, Node *c) {
    if (!c) return;
    if (sm_is_type_node(c)) sm_walk_type(sm, c);
    else if (c->kind == NK_BLOCK || c->kind == NK_ROUTINE)
        sm_walk_block(sm, c->kind == NK_BLOCK ? c : c->child[0]);
    else sm_walk_expr(sm, c);
}

static void check_match(Sema *sm, Node *m) {
    if (!(m->flags & NF_EXPR) && !m->child[2])
        sm_warn(sm, &m->tok, "match statement without an 'else' branch");
    if ((m->flags & NF_EXPR) && !m->child[2])
        sm_error(sm, &m->tok, "match expression requires an 'else' branch");
}

void sm_walk_expr(Sema *sm, Node *n) {
    if (!n) return;
    switch (n->kind) {
    case NK_NAME: {
        /* One rewrite pass: a use alias may rename either the whole
         * bare spelling ('println' after 'use std::inout::println;')
         * or the head segment ('sys::write' after 'use std::sys;'). */
        for (int attempt = 0; attempt < 2; attempt++) {
            const char *colon = strstr(n->name, "::");
            if (colon) {
                /* Qualified access: the full path must either resolve
                 * in the scope chain (a declared member "m::f") or
                 * belong to an external namespace. */
                char first[64];
                size_t len = (size_t)(colon - n->name);
                ModSym *m;
                if (sm_scope_find(sm->cur, n->name)) return;
                if (len >= sizeof(first)) len = sizeof(first) - 1;
                memcpy(first, n->name, len);
                first[len] = '\0';
                m = sm_module_find(sm, first);
                if (!m) {
                    Sym *head = sm_scope_find(sm->cur, first);
                    if (head) {
                        sm_error(sm, &n->tok,
                                 "'%s' is a value or type, not a module; cannot resolve '%s'",
                                 first, n->name);
                        return;
                    }
                    const char *t = sm_xalias_find(sm, first);
                    if (t && attempt == 0) {
                        size_t rest = strlen(colon);
                        char *nn = malloc(strlen(t) + rest + 1);
                        strcpy(nn, t);
                        strcat(nn, colon);
                        n->name = nn;
                        continue;
                    }
                    sm_error(sm, &n->tok, "unknown module '%s'", first);
                } else if (m->local) {
                    sm_error(sm, &n->tok,
                             "unknown function or value '%s' in module '%s'",
                             n->name, first);
                } else if (strcmp(first, "std") == 0) {
                    const char *module_name = colon + 2;
                    const char *member_sep = strrchr(module_name, ':');
                    if (member_sep && member_sep > module_name &&
                        member_sep[-1] == ':') {
                        char module[256];
                        size_t module_len = (size_t)(member_sep - 1 - module_name);
                        if (module_len < sizeof(module)) {
                            memcpy(module, module_name, module_len);
                            module[module_len] = '\0';
                            const char *symbol = member_sep + 1;
                            int exists = sm_external_member_exists(
                                "std", module, symbol);
                            if (exists == 0)
                                sm_error(sm, &n->tok,
                                    "unknown function or value '%s' in module 'std::%s'",
                                    symbol, module);
                        }
                    }
                }
                return;
            }
            if (!sm_scope_find(sm->cur, n->name)) {
                /* Compile-time layout operators are intrinsic names,
                 * not ordinary declarations or library functions. */
                if (strcmp(n->name, "sizeof") == 0 ||
                    strcmp(n->name, "alignof") == 0)
                    return;
                const char *t = sm_xalias_find(sm, n->name);
                if (t && attempt == 0) {
                    n->name = sm_strdup(t);
                    continue;
                }
                /* A bare name may also be a type used as a
                 * compile-time argument; accept it when it resolves
                 * as a type. */
                if (!sm_type_resolves(sm, n->name))
                    sm_error(sm, &n->tok,
                             "unknown variable or function '%s'; check its declaration or import",
                             n->name);
            }
            return;
        }
        return;
    }
    case NK_STRUCT_LIT:
        if (!sm_type_resolves(sm, n->name))
            sm_error(sm, &n->tok, "unknown type '%s'", n->name);
        break;
    case NK_UNARY:
        if (n->op == TK_KW_TRY && !sm->fn_err)
            sm_error(sm, &n->tok, "'try' used in a function that does not "
                                  "return an error type");
        break;
    case NK_BINARY:
        if (n->op == TK_ASSIGN && n->child[0] &&
            n->child[0]->kind == NK_NAME) {
            Sym *s = sm_scope_find(sm->cur, n->child[0]->name);
            if (s && s->decl && s->decl->kind == NK_STATIC_CONST)
                sm_error(sm, &n->tok,
                         "cannot assign to static constant '%s'",
                         n->child[0]->name);
        }
        break;
    case NK_MATCH:
        check_match(sm, n);
        break;
    case NK_ALLOC: {
        /* Type-name operand: allocation of that type (tagged so
         * lowering passes the element size); any other operand is a
         * plain size expression. */
        if (n->child[0] && n->child[0]->kind == NK_NAME) {
            Sym *e = sm_scope_find(sm->cur, n->child[0]->name);
            if (e && e->is_type) {
                n->flags |= NF_TYPE_OPERAND;
                sm_walk_type(sm, n->child[0]);
                break;
            }
        }
        sm_walk_expr(sm, n->child[0]);
        break;
    }
    case NK_CATCH: {
        /* One handler for both statement and nested positions: the
         * guarded expression walks in the enclosing scope, the
         * optional binding name enters a scope of its own that
         * encloses only the handler block. */
        Scope *saved;
        sm_walk_expr(sm, n->child[0]);
        saved = sm->cur;
        sm->cur = malloc(sizeof(Scope));
        sm->cur->head = NULL;
        sm->cur->parent = saved;
        if (n->name) sm_scope_put(sm, n->name, n, 0);
        sm_walk_block(sm, n->child[1]);
        sm_scope_destroy(sm->cur);
        sm->cur = saved;
        return;
    }
    case NK_IF:
        /* The bare-condition form (no blocks) yields the condition
         * itself, so it needs no else branch. */
        if ((n->flags & NF_EXPR) && n->child[1] && !n->child[2])
            sm_error(sm, &n->tok, "if expression requires an 'else' branch");
        break;
    default:
        break;
    }
    for (int i = 0; i < 4; i++) walk_child(sm, n->child[i]);
    for (int i = 0; i < n->item_count; i++) walk_child(sm, n->items[i]);
}

/* A static name whose initializer is a type expression is a type alias. */
static int sm_static_is_type(Sema *sm, Node *d) {
    if (!d->child[1] || d->child[1]->kind != NK_NAME) return 0;
    if (!sm_type_resolves(sm, d->child[1]->name)) return 0;
    d->flags |= NF_TYPE_ALIAS;
    return 1;
}

/* Registers a declaration name into the current scope without walking
 * it. Enum variants are registered as values alongside the type;
 * prototypes are recorded so that a later body can resolve them.
 *
 * A named module registers twice: its namespace enters the module
 * table (so `m::...` paths validate against it), and each member is
 * registered under its qualified name "m::f" in the current scope, so
 * qualified references resolve from anywhere. An unnamed module is
 * transparent: its members register into the current scope under
 * their bare names, which makes them visible throughout the project
 * as the language reference specifies. */
void sm_register_decl(Sema *sm, Node *d) {
    switch (d->kind) {
    case NK_FUNC: case NK_STATIC_FN:
        if (d->kind == NK_FUNC && (d->flags & NF_PROTOTYPE)) {
            sm_proto_add(sm, sm_strdup(d->name), d->tok);
        } else {
            for (int i = 0; i < sm->nprotos; i++)
                if (!sm->protos[i].defined &&
                    strcmp(sm->protos[i].name, d->name) == 0)
                    sm->protos[i].defined = 1;
        }
        sm_scope_put(sm, d->name, d, 0);
        break;
    case NK_STRUCT: case NK_UNION:
        sm_scope_put(sm, d->name, d, 1);
        break;
    case NK_ENUM:
        sm_scope_put(sm, d->name, d, 1);
        for (int i = 0; i < d->item_count; i++)
            sm_scope_put(sm, d->items[i]->name, d->items[i], 0);
        break;
    case NK_STATIC_CONST:
        sm_scope_put(sm, d->name, d, sm_static_is_type(sm, d));
        break;
    case NK_STATIC_BLOCK: case NK_LET_MULTI:
        for (int i = 0; i < d->item_count; i++)
            sm_register_decl(sm, d->items[i]);
        break;
    case NK_LET:
        sm_scope_put(sm, d->name, d, 0);
        break;
    case NK_MODULE:
        if (d->name) {
            ModSym *m = sm_module_add(sm, d->name, 1);
            char prefix[256];
            snprintf(prefix, sizeof(prefix), "%s::", d->name);
            for (int i = 0; i < d->item_count; i++) {
                if (d->items[i]->name) sm_member_record(m, d->items[i]->name);
                sm_register_qualified(sm, prefix, d->items[i]);
            }
        } else {
            for (int i = 0; i < d->item_count; i++)
                sm_register_decl(sm, d->items[i]);
        }
        break;
    default:
        break;
    }
}

int sema_run(Node *program, Diag *diag, const char *path) {
    Sema sm;
    memset(&sm, 0, sizeof(sm));
    sm.diag = diag;
    sm.path = path;
    Scope global = { NULL, NULL };
    sm.cur = &global;

    /* The standard library namespace is external; installed packages
     * provide further external namespaces. Program modules are added
     * as local namespaces during registration below. */
    sm_module_add(&sm, "std", 0);
    /* A source that lives inside the std package (one of its
     * library files) sees every sibling library named by the
     * package manifest as a module of its own, so library code
     * reaches neighbours as 'sys::write' without the 'std::'
     * qualifier; user code outside the package never gets these. */
    {
        char pfx[80];
        if (std_pkg_prefix(path, pfx, sizeof(pfx)) == 0) {
            char names[PAXSY_MAX_MODULES][64];
            int n = pkg_modules("std", names, PAXSY_MAX_MODULES);
            for (int i = 0; i < n; i++)
                sm_module_add(&sm, names[i], 0);
        }
    }
    char pkgs[32][PKG_NAME_MAX];
    int npkgs = pkg_list_installed(pkgs, 32);
    for (int i = 0; i < npkgs; i++)
        sm_module_add(&sm, sm_strdup(pkgs[i]), 0);

    for (int i = 0; i < program->item_count; i++)
        sm_register_decl(&sm, program->items[i]);
    for (int pass = 0; pass < program->item_count; pass++) {
        int changed = 0;
        for (int i = 0; i < program->item_count; i++) {
            Node *d = program->items[i];
            if (d->kind != NK_STATIC_CONST || !d->child[1] ||
                d->child[1]->kind != NK_NAME) continue;
            Sym *symbol = sm_scope_find(sm.cur, d->name);
            if (symbol && !symbol->is_type &&
                sm_type_resolves(&sm, d->child[1]->name)) {
                symbol->is_type = 1;
                d->flags |= NF_TYPE_ALIAS;
                changed = 1;
            }
        }
        if (!changed) break;
    }
    for (int i = 0; i < program->item_count; i++)
        sm_walk_decl(&sm, program->items[i]);

    /* Prototypes without a definition are external symbols; the
     * linker resolves them. Surface that to the reader. */
    for (int i = 0; i < sm.nprotos; i++)
        if (!sm.protos[i].defined)
            sm_warn(&sm, &sm.protos[i].tok,
                    "prototype '%s' has no definition; assumed external",
                    sm.protos[i].name);

    int errors = diag_error_count(diag);
    sm_scope_clear(&global);
    for (int i = 0; i < sm.nmods; i++) {
        free(sm.mods[i].name);
        for (int j = 0; j < sm.mods[i].nmembers; j++)
            free(sm.mods[i].members[j]);
        free(sm.mods[i].members);
    }
    free(sm.mods);
    for (int i = 0; i < sm.nprotos; i++) free(sm.protos[i].name);
    free(sm.protos);
    for (int i = 0; i < sm.n_xalias; i++) {
        free(sm.xalias[i].alias);
        free(sm.xalias[i].target);
    }
    free(sm.xalias);
    return errors;
}
