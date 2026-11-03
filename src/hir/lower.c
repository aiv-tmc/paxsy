#include "common.h"
#include "lower_int.h"
#include "stdpath.h"

/*
 * lower.c - AST-to-HIR lowering driver and declaration lowering.
 * Every declaration becomes an HNode attached to the module root;
 * expressions are lowered by lower_expr (lower_expr.c). The lowering
 * state tracks lexical scopes for local variables and parameters so
 * that name references bind to the correct slot.
 *
 * Named modules prefix their members: a function `square` inside
 * `module math` lowers to `math::square`, and a bare reference to
 * `square` from inside the module is rewritten to the same qualified
 * name. Sources inside a package (announced through
 * stdpath_set_pkg_prefix before lowering, e.g. the standard library)
 * carry the package prefix: `module inout` inside the std package
 * lowers to `std::inout`, and the root file's `module std` keeps the
 * announced prefix instead of nesting into `std::std`. Unnamed
 * modules are transparent: their members keep their bare names.
 */

/* Scope stack ---------------------------------------------------------- */

void lscope_push(Lower *l) {
    if (l->depth == 8) { lerr(l, NULL, "too many nested scopes"); return; }
    l->scopes[l->depth].v = NULL;
    l->scopes[l->depth].n = 0;
    l->scopes[l->depth].cap = 0;
    l->depth++;
}

void lscope_pop(Lower *l) {
    if (l->depth == 0) return;
    l->depth--;
    free(l->scopes[l->depth].v);
    l->scopes[l->depth].v = NULL;
}

void lscope_put(Lower *l, const char *name, int index, int is_param) {
    LScope *s = &l->scopes[l->depth - 1];
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = realloc(s->v, (size_t)s->cap * sizeof(*s->v));
    }
    s->v[s->n].name = name;
    s->v[s->n].index = index;
    s->v[s->n].is_param = is_param;
    s->n++;
}

/* Innermost binding wins; the scan runs from the innermost scope
 * outward. */
int lscope_find(Lower *l, const char *name, int *is_param) {
    for (int d = l->depth - 1; d >= 0; d--) {
        LScope *s = &l->scopes[d];
        for (int i = s->n - 1; i >= 0; i--)
            if (strcmp(s->v[i].name, name) == 0) {
                *is_param = s->v[i].is_param;
                return s->v[i].index;
            }
    }
    return -1;
}

/* Module prefix helpers ------------------------------------------------ */

/* Returns the fully-qualified name when `name` is a bare member of the
 * current module; otherwise returns `name` unchanged. Qualified names
 * (already containing "::") are never altered. A small ring buffer
 * holds the constructed strings; four slots are enough because no
 * single expression needs more than a few resolved names at once. */
const char *lower_resolve_name(Lower *l, const char *name) {
    if (!l->prefix[0] || strchr(name, ':')) return name;
    for (int i = 0; i < l->nmembers; i++)
        if (strcmp(l->members[i], name) == 0) {
            static char ring[4][256];
            static int idx = 0;
            char *buf = ring[idx];
            idx = (idx + 1) & 3;
            snprintf(buf, 256, "%s%s", l->prefix, name);
            return buf;
        }
    return name;
}

/* Recursively collects the bare names of all value and type
 * declarations inside a module body (functions, structs, unions,
 * enums, lets, comptime constants, and nested named modules). The
 * list drives lower_resolve_name: a bare reference that matches a
 * member is prefixed, everything else is left as-is (imports,
 * globals, external symbols). */
static void collect_members(Lower *l, const Node *mod) {
    for (int i = 0; i < mod->item_count; i++) {
        const Node *item = mod->items[i];
        switch (item->kind) {
        case NK_FUNC: case NK_STRUCT: case NK_UNION: case NK_ENUM:
        case NK_LET: case NK_COMPTIME_CONST:
            if (item->name) {
                if (l->nmembers == l->members_cap) {
                    int cap = l->members_cap ? l->members_cap * 2 : 16;
                    l->members = realloc(l->members,
                                         (size_t)cap * sizeof(char *));
                    l->members_cap = cap;
                }
                l->members[l->nmembers++] = (char *)item->name;
            }
            break;
        case NK_COMPTIME_BLOCK: case NK_LET_MULTI:
            collect_members(l, item);
            break;
        case NK_MODULE:
            if (item->name) {
                if (l->nmembers == l->members_cap) {
                    int cap = l->members_cap ? l->members_cap * 2 : 16;
                    l->members = realloc(l->members,
                                         (size_t)cap * sizeof(char *));
                    l->members_cap = cap;
                }
                l->members[l->nmembers++] = (char *)item->name;
            }
            break;
        default:
            break;
        }
    }
}

/* Type mapping ------------------------------------------------------- */

/* Int/UInt suffix: empty means 32 bits, else the width must exist
 * in lwidths. */
static const HType *lower_width(Lower *l, const char *nm, Token tok,
                                int is_uint) {
    if (*nm == '\0') return is_uint ? hty_uint(32) : hty_int(32);
    int w = atoi(nm);
    for (int i = 0; lwidths[i]; i++)
        if (lwidths[i] == w)
            return is_uint ? hty_uint(w) : hty_int(w);
    lerr(l, &tok, is_uint ? "unknown UInt width" : "unknown Int width");
    return &hty_void;
}

/* Fixed names map directly, Real takes 32/64, String is a Char
 * pointer, any other name stays a named aggregate reference. */
static const HType *lower_named(Lower *l, const char *nm, Token tok) {
    const char *name = lglobal_name(l, nm);
    if (strcmp(name, "Void") == 0)   return &hty_void;
    if (strcmp(name, "Bool") == 0)   return &hty_bool;
    if (strcmp(name, "Char") == 0)   return &hty_char;
    if (strcmp(name, "String") == 0) return hty_ptr(&hty_char);
    if (strcmp(name, "Real") == 0)   return hty_real(64);
    if (strncmp(name, "Real", 4) == 0) {
        int w = atoi(name + 4);
        if (w == 32 || w == 64) return hty_real(w);
        lerr(l, &tok, "unknown Real width (use Real32 or Real64)");
        return &hty_void;
    }
    if (strncmp(name, "UInt", 4) == 0) return lower_width(l, name + 4, tok, 1);
    if (strncmp(name, "Int", 3) == 0)  return lower_width(l, name + 3, tok, 0);
    return hty_named(name);
}

/* Lowers one AST type node to an HType; unmapped syntax is
 * diagnosed. */
const HType *lower_type(Lower *l, const Node *t) {
    if (!t) return &hty_void;
    switch (t->kind) {
    case NK_TYPE_NAME:
        return lower_named(l, t->name, t->tok);
    case NK_TYPE_PTR:
        return hty_ptr(lower_type(l, t->child[0]));
    case NK_TYPE_ARRAY: {
        const HType *elem = lower_type(l, t->child[0]);
        long count = 0;
        if (t->child[1] && t->child[1]->kind == NK_LITERAL)
            count = lint_value(t->child[1]->tok.start, t->child[1]->tok.len);
        else if (t->child[1])
            lerr(l, &t->tok, "array size must be an integer literal");
        return hty_array(elem, count);
    }
    case NK_TYPE_ERROR:
        return hty_error(lower_type(l, t->child[0]));
    case NK_TYPE_CHAN:
        return hty_chan(lower_type(l, t->child[0]));
    default:
        lerr(l, &t->tok, "unsupported type syntax");
        return &hty_void;
    }
}

/* Declarations ------------------------------------------------------- */

/* Binds parameter slots 0..n-1 and registers the names in the fresh
 * function scope; the local counter resets for body bindings.
 * The function node's type is its return type (HTY_ERROR for T!);
 * parameter types live on the parameter nodes. */
static void lower_params(Lower *l, HNode *f, const Node *d) {
    int np = d->item_count;
    for (int i = 0; i < np; i++) {
        const Node *p = d->items[i];
        if (p->flags & NF_COMPTIME) {
            lerr(l, &p->tok, "comptime parameters need monomorphization");
            continue;
        }
        HNode *hp = lnode(l, HK_PARAM, p->tok);
        hp->name = p->name;
        hp->u.index = i;
        hp->type = p->child[0] ? lower_type(l, p->child[0]) : &hty_void;
        hir_add(f, hp);
    }
    f->type = d->child[0] ? lower_type(l, d->child[0]) : &hty_void;
    lscope_push(l);
    l->next_local = np;
    for (int i = 0; i < np; i++)
        lscope_put(l, d->items[i]->name, i, 1);
}

static void lower_func(Lower *l, const Node *d) {
    HNode *f = lnode(l, d->flags & NF_PROTOTYPE ? HK_EXTERN_FUNC : HK_FUNC,
                     d->tok);
    f->name = lower_resolve_name(l, d->name);
    hir_add(hir_root(l->mod), f);
    lower_params(l, f, d);
    if (d->flags & NF_PROTOTYPE) {
        lscope_pop(l);
        return;
    }
    HNode *body;
    if (d->child[2]) {              /* '= expr' shorthand */
        body = lnode(l, HK_BLOCK, d->child[2]->tok);
        HNode *r = lnode(l, HK_RETURN, d->child[2]->tok);
        r->child[0] = lower_expr(l, d->child[2]);
        hir_add(body, r);
    } else {
        body = lower_block(l, d->child[3]);
    }
    /* Body convention shared with every consumer: child[3]. */
    f->child[3] = body;
    lscope_pop(l);
    lflags(f);
}

static void lower_aggregate(Lower *l, const Node *d) {
    HNode *s = lnode(l, d->kind == NK_STRUCT ? HK_STRUCT :
                     d->kind == NK_UNION ? HK_UNION : HK_ENUM, d->tok);
    s->name = lower_resolve_name(l, d->name);
    hir_add(hir_root(l->mod), s);
    if (d->kind == NK_ENUM) {
        long next = 0;
        for (int i = 0; i < d->item_count; i++) {
            const Node *v = d->items[i];
            HNode *hv = lnode(l, HK_CONST_INT, v->tok);
            hv->name = v->name;
            hv->u.ival = v->child[0] && v->child[0]->kind == NK_LITERAL
                ? lint_value(v->child[0]->tok.start, v->child[0]->tok.len)
                : next;
            next = hv->u.ival + 1;
            hir_add(s, hv);
        }
        return;
    }
    for (int i = 0; i < d->item_count; i++) {
        const Node *fd = d->items[i];
        HNode *hfd = lnode(l, HK_FIELD, fd->tok);
        hfd->name = fd->name;
        hfd->u.index = i;
        hfd->type = lower_type(l, fd->child[0]);
        hir_add(s, hfd);
    }
}

static void lower_global(Lower *l, const Node *d) {
    HNode *g = lnode(l, HK_GLOBAL, d->tok);
    g->name = lower_resolve_name(l, d->name);
    g->type = d->child[0] ? lower_type(l, d->child[0]) : NULL;
    if (d->child[1]) g->child[0] = lower_expr(l, d->child[1]);
    lflags(g);
    hir_add(hir_root(l->mod), g);
}

/* Functions, aggregates, enums, module-level lets. use declarations
 * carry no runtime content; a module wraps its declarations, so
 * lowering recurses into its members. Anything else is diagnosed. */
void lower_decl(Lower *l, const Node *d) {
    switch (d->kind) {
    case NK_FUNC:      lower_func(l, d);      break;
    case NK_STRUCT: case NK_UNION: case NK_ENUM:
                       lower_aggregate(l, d); break;
    case NK_LET:       lower_global(l, d);    break;
    case NK_USE:
        /* Imports alias qualified names for bare references; the
         * aliases are scoped by the module marks in the NK_MODULE
         * case below, so a use inside a module stops at its boundary. */
        luse_apply(l, d);
        break;
    case NK_MODULE: {
        char saved_prefix[256];
        char **saved_members = l->members;
        int saved_n = l->nmembers;
        int saved_cap = l->members_cap;

        strcpy(saved_prefix, l->prefix);
        if (d->name) {
            /* At the package root (the prefix equals the announced
             * package prefix) a module named like the package IS the
             * root module: nesting would double the name. */
            int pkg_root = l->prefix[0] &&
                strcmp(l->prefix, stdpath_pkg_prefix()) == 0 &&
                stdpath_pkg_name()[0] &&
                strcmp(d->name, stdpath_pkg_name()) == 0;
            if (l->prefix[0] && !pkg_root) {
                char tmp[768];
                snprintf(tmp, sizeof(tmp), "%.250s%.250s::",
                         l->prefix, d->name);
                strncpy(l->prefix, tmp, sizeof(l->prefix) - 1);
                l->prefix[sizeof(l->prefix) - 1] = '\0';
            } else if (!pkg_root) {
                /* A top-level module in a program source stands
                 * alone; the announced package prefix is empty
                 * there. */
                snprintf(l->prefix, sizeof(l->prefix), "%s%s::",
                         stdpath_pkg_prefix(), d->name);
            }
            /* pkg_root keeps the announced prefix unchanged. */
            l->members = NULL;
            l->nmembers = l->members_cap = 0;
            collect_members(l, d);
            /* Uses inside a named module see its imports only. */
            luse_push(l);
        }
        for (int i = 0; i < d->item_count; i++)
            lower_decl(l, d->items[i]);
        if (d->name) luse_pop(l);

        strcpy(l->prefix, saved_prefix);
        if (l->members) free(l->members);
        l->members = saved_members;
        l->nmembers = saved_n;
        l->members_cap = saved_cap;
        break;
    }
    default:
        lerr(l, &d->tok, "unsupported declaration in this phase");
    }
}

/* Driver ------------------------------------------------------------- */

HModule *lower_run(const Node *program, Diag *diag) {
    Lower l;
    memset(&l, 0, sizeof(l));
    l.mod = hir_module_new();
    l.diag = diag;
    int errs = diag_error_count(diag);
    l.prefix[0] = '\0';
    l.members = NULL;
    l.nmembers = l.members_cap = 0;
    /* A package source (the standard library) lowers under the
     * announced package prefix, so `module inout` in a library file
     * becomes std::inout and top-level declarations outside any
     * module become members of the package root. */
    if (stdpath_pkg_prefix()[0]) {
        strncpy(l.prefix, stdpath_pkg_prefix(), sizeof(l.prefix) - 1);
        l.prefix[sizeof(l.prefix) - 1] = '\0';
        collect_members(&l, program);
    }
    luse_init(&l);
    lscope_push(&l);            /* module scope for future top-level lets */
    for (int i = 0; i < program->item_count; i++)
        lower_decl(&l, program->items[i]);
    lscope_pop(&l);
    if (diag_error_count(diag) > errs)
        return NULL;            /* the caller frees nothing on NULL */
    return l.mod;
}
