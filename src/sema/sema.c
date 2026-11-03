#include "common.h"
#include "sema_int.h"
#include "pkgdir.h"

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
    diag_report_full(sm->diag, DK_ERROR, sm->path, t->line, t->col,
                     t->len, "%s", buf);
}

void sm_warn(Sema *sm, const Token *t, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag_report_full(sm->diag, DK_WARNING, sm->path, t->line, t->col,
                     t->len, "%s", buf);
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
            sm_error(sm, &decl->tok, "duplicate definition of '%s'", name);
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
        const char *colon = strstr(n->name, "::");
        if (colon) {
            /* Qualified access: the full path must either resolve in
             * the scope chain (a declared member "m::f" or a use
             * alias) or belong to an external namespace. For a local
             * module the second case is precise: the module exists
             * but the member does not. */
            char first[64];
            size_t len = (size_t)(colon - n->name);
            ModSym *m;
            if (sm_scope_find(sm->cur, n->name)) return;
            if (len >= sizeof(first)) len = sizeof(first) - 1;
            memcpy(first, n->name, len);
            first[len] = '\0';
            m = sm_module_find(sm, first);
            if (!m)
                sm_error(sm, &n->tok, "unknown module '%s'", first);
            else if (m->local)
                sm_error(sm, &n->tok, "module '%s' has no member '%s'",
                         first, colon + 2);
            return;
        }
        if (!sm_scope_find(sm->cur, n->name)) {
            /* A bare name may also be a type used as a compile-time
             * argument; accept it when it resolves as a type. */
            if (!sm_type_resolves(sm, n->name))
                sm_error(sm, &n->tok,
                         "use of undeclared identifier '%s'", n->name);
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
            if (s && s->decl && s->decl->kind == NK_COMPTIME_CONST)
                sm_error(sm, &n->tok,
                         "cannot assign to comptime constant '%s'",
                         n->child[0]->name);
        }
        break;
    case NK_MATCH:
        check_match(sm, n);
        break;
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
    case NK_FUNC: case NK_COMPTIME_FN:
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
    case NK_COMPTIME_CONST:
        sm_scope_put(sm, d->name, d,
                     d->child[0] && d->child[0]->kind == NK_NAME ? 1 : 0);
        break;
    case NK_COMPTIME_BLOCK: case NK_LET_MULTI:
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
    char pkgs[32][PKG_NAME_MAX];
    int npkgs = pkg_list_installed(pkgs, 32);
    for (int i = 0; i < npkgs; i++)
        sm_module_add(&sm, sm_strdup(pkgs[i]), 0);

    for (int i = 0; i < program->item_count; i++)
        sm_register_decl(&sm, program->items[i]);
    for (int i = 0; i < program->item_count; i++)
        sm_walk_decl(&sm, program->items[i]);

    /* Prototypes without a definition are external symbols; the
     * linker resolves them. Surface that to the reader. */
    for (int i = 0; i < sm.nprotos; i++)
        if (!sm.protos[i].defined)
            sm_warn(&sm, &sm.protos[i].tok,
                    "prototype '%s' has no definition; assumed external",
                    sm.protos[i].name);

    return diag_error_count(diag);
}
