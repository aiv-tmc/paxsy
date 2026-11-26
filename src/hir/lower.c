#include "common.h"
#include "lower_int.h"
#include "stdpath.h"
#include <limits.h>

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

/* A per-scope open-addressed index avoids repeatedly scanning every local
 * binding while lowering large functions.  The vector remains canonical;
 * the index stores vector positions (plus one, reserving zero for empty).
 * Allocation failure simply disables the index for that scope and retains
 * the original, correct reverse linear lookup. */
static unsigned lscope_hash_name(const char *name) {
    unsigned h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static int lscope_hash_rebuild(LScope *s, int requested) {
    int cap = 16;
    while (cap < requested) {
        if (cap > INT_MAX / 2) return 0;
        cap *= 2;
    }
    unsigned *table = calloc((size_t)cap, sizeof(*table));
    if (!table) return 0;
    for (int i = 0; i < s->n; ++i) {
        unsigned mask = (unsigned)cap - 1u;
        unsigned at = lscope_hash_name(s->v[i].name) & mask;
        for (;;) {
            unsigned encoded = table[at];
            if (!encoded) {
                table[at] = (unsigned)i + 1u;
                break;
            }
            unsigned old = encoded - 1u;
            if (strcmp(s->v[old].name, s->v[i].name) == 0) {
                table[at] = (unsigned)i + 1u;
                break;
            }
            at = (at + 1u) & mask;
        }
    }
    free(s->hash);
    s->hash = table;
    s->hash_cap = cap;
    return 1;
}

void lscope_push(Lower *l) {
    if (l->depth == 32) { lerr(l, NULL, "too many nested scopes"); return; }
    LScope *s = &l->scopes[l->depth];
    memset(s, 0, sizeof(*s));
    l->depth++;
}

void lscope_pop(Lower *l) {
    if (l->depth == 0) return;
    LScope *s = &l->scopes[--l->depth];
    free(s->v);
    free(s->hash);
    memset(s, 0, sizeof(*s));
}

void lscope_put(Lower *l, const char *name, int index, int is_param) {
    /* Bindings without a name (an unnamed catch) simply do not exist
     * in the scope table; one guard here keeps every call site free
     * of name-conditional logic. */
    if (!name || l->depth <= 0) return;
    LScope *s = &l->scopes[l->depth - 1];
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 16;
        LSlot *v = realloc(s->v, (size_t)cap * sizeof(*v));
        if (!v) {
            lerr(l, NULL, "out of memory growing local scope");
            return;
        }
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n].name = name;
    s->v[s->n].index = index;
    s->v[s->n].is_param = is_param;
    s->n++;

    /* Keep at most 70% occupancy.  If growing the side index fails,
     * discard it and use the canonical vector until this scope ends. */
    if (!s->hash || (size_t)s->n * 10u >= (size_t)s->hash_cap * 7u) {
        int wanted = 16;
        if (s->hash_cap) {
            if (s->hash_cap > INT_MAX / 2) {
                free(s->hash);
                s->hash = NULL;
                s->hash_cap = 0;
                return;
            }
            wanted = s->hash_cap * 2;
        }
        if (!lscope_hash_rebuild(s, wanted)) {
            free(s->hash);
            s->hash = NULL;
            s->hash_cap = 0;
        }
    } else {
        unsigned mask = (unsigned)s->hash_cap - 1u;
        unsigned at = lscope_hash_name(name) & mask;
        for (;;) {
            unsigned encoded = s->hash[at];
            if (!encoded) {
                s->hash[at] = (unsigned)s->n;
                break;
            }
            unsigned old = encoded - 1u;
            if (strcmp(s->v[old].name, name) == 0) {
                s->hash[at] = (unsigned)s->n;
                break;
            }
            at = (at + 1u) & mask;
        }
    }
}

/* Innermost binding wins; hash lookup is constant-time on average. */
int lscope_find(Lower *l, const char *name, int *is_param) {
    if (!name) return -1;
    for (int d = l->depth - 1; d >= 0; d--) {
        LScope *s = &l->scopes[d];
        if (s->hash && s->hash_cap > 0) {
            unsigned mask = (unsigned)s->hash_cap - 1u;
            unsigned at = lscope_hash_name(name) & mask;
            for (int probes = 0; probes < s->hash_cap; ++probes) {
                unsigned encoded = s->hash[at];
                if (!encoded) break;
                unsigned i = encoded - 1u;
                if (strcmp(s->v[i].name, name) == 0) {
                    *is_param = s->v[i].is_param;
                    return s->v[i].index;
                }
                at = (at + 1u) & mask;
            }
        } else {
            for (int i = s->n - 1; i >= 0; i--)
                if (strcmp(s->v[i].name, name) == 0) {
                    *is_param = s->v[i].is_param;
                    return s->v[i].index;
                }
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
    char buf[256];
    if (!l->prefix[0]) return name;
    if (!strchr(name, ':')) {
        /* Bare reference: a module member takes the module prefix,
         * a file-level member takes the package-root prefix. */
        for (int i = 0; i < l->nmembers; i++)
            if (strcmp(l->members[i], name) == 0) {
                snprintf(buf, sizeof(buf), "%s%s", l->prefix, name);
                return hir_name(l->mod, buf);
            }
        for (int i = 0; i < l->n_file_members; i++)
            if (strcmp(l->file_members[i], name) == 0) {
                snprintf(buf, sizeof(buf), "%s%s", l->file_prefix, name);
                return hir_name(l->mod, buf);
            }
        return name;
    }
    /* Qualified reference inside a package source. Sibling libraries
     * ('heap::alloc') and the current library spelled out
     * ('inout::ERR_WRITE') both mean the package root, so they take
     * the package prefix; a name that already carries it
     * ('std::inout::print') is left alone. */
    {
        size_t plen = (size_t)(strchr(l->prefix, ':') - l->prefix);
        size_t hlen = (size_t)(strstr(name, "::") - name);
        /* A name that already carries the package name, and the
         * universal 'std' root, are complete as written. */
        if ((hlen == plen && strncmp(name, l->prefix, plen) == 0) ||
            (hlen == 3 && strncmp(name, "std", 3) == 0))
            return name;
        snprintf(buf, sizeof(buf), "%.*s::%s", (int)plen, l->prefix, name);
        return hir_name(l->mod, buf);
    }
}

/* Recursively collects the bare names of all value and type
 * declarations inside a module body (functions, structs, unions,
 * enums, lets, static constants, and nested named modules). The
 * list drives lower_resolve_name: a bare reference that matches a
 * member is prefixed, everything else is left as-is (imports,
 * globals, external symbols). */
/* Registers a file-level (package-root) member; top-level
 * declarations outside any module resolve under the announced
 * package prefix. */
static void collect_file_member(Lower *l, const char *name) {
    if (l->n_file_members == l->file_members_cap) {
        l->file_members_cap = l->file_members_cap ? l->file_members_cap * 2 : 32;
        l->file_members = realloc(l->file_members, (size_t)l->file_members_cap * sizeof(*l->file_members));
    }
    l->file_members[l->n_file_members++] = (char *)name;
}

static void collect_members(Lower *l, const Node *mod) {
    for (int i = 0; i < mod->item_count; i++) {
        const Node *item = mod->items[i];
        switch (item->kind) {
        case NK_FUNC: case NK_STRUCT: case NK_UNION: case NK_ENUM:
        case NK_LET: case NK_STATIC_CONST:
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
        case NK_STATIC_BLOCK: case NK_LET_MULTI:
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
    for (int i = l->n_type_aliases - 1; i >= 0; i--)
        if (strcmp(l->type_aliases[i].name, name) == 0 &&
            strcmp(l->type_aliases[i].target, name) != 0)
            return lower_named(l, l->type_aliases[i].target, tok);
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
    /* User-defined types live under the declaring module; qualified
     * references (imports, std::) are stored verbatim. */
    if (strchr(name, ':')) return hty_named(name);
    return hty_named(hir_name(l->mod, name));
}

/* Lowers one AST type node to an HType; unmapped syntax is
 * diagnosed. */
const HType *lower_type(Lower *l, const Node *t) {
    if (!t) return &hty_void;
    switch (t->kind) {
    case NK_TYPE_NAME:
        return lower_named(l, t->name, t->tok);
    case NK_TYPE_TUPLE: {
        int count = t->item_count;
        const HType **elems = count > 0
            ? malloc((size_t)count * sizeof(*elems)) : NULL;
        if (count > 0 && !elems) {
            lerr(l, &t->tok, "out of memory lowering tuple type");
            return &hty_void;
        }
        for (int i = 0; i < count; ++i)
            elems[i] = lower_type(l, t->items[i]);
        const HType *tuple = hty_tuple(elems, count);
        free(elems);
        return tuple;
    }
    case NK_TYPE_PTR:
        return hty_ptr(lower_type(l, t->child[0]));
    case NK_TYPE_ARRAY: {
        if (t->flags & NF_OPEN_LO) {       /* T[...]: slice view   */
            const HType *e = lower_type(l, t->child[0]);
            return hty_view(e);
        }
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
        if (p->flags & NF_STATIC) {
            lerr(l, &p->tok, "static parameters need monomorphization");
            continue;
        }
        HNode *hp = lnode(l, HK_PARAM, p->tok);
        hp->name = p->name;
        hp->u.index = i;
        /* An omitted parameter annotation defaults to Int64. */
        hp->type = p->child[0] ? lower_type(l, p->child[0]) : hty_int(64);
        hir_add(f, hp);
    }
    f->type = d->child[0] ? lower_type(l, d->child[0]) : &hty_void;
    lscope_push(l);
    l->next_local = np;
    for (int i = 0; i < np; i++)
        lscope_put(l, d->items[i]->name, i, 1);
}

void lower_func(Lower *l, const Node *d) {
    const Node *saved_fn = l->current_fn;
    l->current_fn = d;
    HNode *f = lnode(l, d->flags & NF_PROTOTYPE ? HK_EXTERN_FUNC : HK_FUNC,
                     d->tok);
    f->name = lower_resolve_name(l, d->name);
    hir_add(hir_root(l->mod), f);
    lower_params(l, f, d);
    if (d->flags & NF_PROTOTYPE) {
        lscope_pop(l);
        l->current_fn = saved_fn;
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
    l->current_fn = saved_fn;
}

static void lower_aggregate(Lower *l, const Node *d) {
    HNode *s = lnode(l, d->kind == NK_STRUCT ? HK_STRUCT :
                     d->kind == NK_UNION ? HK_UNION : HK_ENUM, d->tok);
    s->name = lower_resolve_name(l, d->name);
    if (d->child[0]) s->type = lower_type(l, d->child[0]);
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
    if (d->kind == NK_STATIC_CONST) g->flags |= HF_STATIC_CONST;
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
    case NK_FUNC: {
        int generic = 0;
        for (int i = 0; i < d->item_count; i++)
            if (d->items[i]->flags & NF_STATIC) generic = 1;
        if (!generic) lower_func(l, d);
        break;
    }
    case NK_STRUCT: case NK_UNION: case NK_ENUM:
                       lower_aggregate(l, d); break;
    /* Type aliases are compile-time metadata and have no runtime storage. */
    case NK_STATIC_CONST:
        if (!(d->flags & NF_TYPE_ALIAS)) lower_global(l, d);
        break;
    case NK_LET: lower_global(l, d); break;
    case NK_USE:
        /* Imports alias qualified names for bare references; the
         * aliases are scoped by the module marks in the NK_MODULE
         * case below, so a use inside a module stops at its boundary. */
        luse_apply(l, d);
        break;
    case NK_MODULE: {
        char saved_prefix[256];

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
        break;
    }
    default:
        lerr(l, &d->tok, "unsupported declaration in this phase");
    }
}

/* Driver ------------------------------------------------------------- */

static const Node *const *project_sources;
static int project_source_count;

void lower_set_generic_sources(const Node *const *programs, int count) {
    project_sources = programs;
    project_source_count = count > 0 ? count : 0;
}

HModule *lower_run(const Node *program, Diag *diag) {
    Lower l;
    memset(&l, 0, sizeof(l));
    l.mod = hir_module_new();
    l.diag = diag;
    l.program = program;
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
        strncpy(l.file_prefix, stdpath_pkg_prefix(), sizeof(l.file_prefix) - 1);
        l.file_prefix[sizeof(l.file_prefix) - 1] = '\0';
        for (int i = 0; i < program->item_count; i++)
            if (program->items[i]->name)
                collect_file_member(&l, program->items[i]->name);
    }
    luse_init(&l);
    char current_package[80] = "";
    int current_is_package = program->tok.path &&
        std_pkg_prefix(program->tok.path, current_package,
                       sizeof(current_package)) == 0;
    if (!current_is_package) luse_registry_build(&l, program);
    for (int i = 0; i < program->item_count; i++)
        lower_collect_generics(&l, program->items[i], l.prefix);
    for (int p = 0; p < project_source_count; p++) {
        const Node *other = project_sources[p];
        if (!other || other == program) continue;
        char prefix[80] = "";
        int is_package = other->tok.path &&
            std_pkg_prefix(other->tok.path, prefix, sizeof(prefix)) == 0;
        if (!is_package) luse_registry_build(&l, other);
        for (int i = 0; i < other->item_count; i++)
            lower_collect_generics(&l, other->items[i], prefix);
    }
    for (int i = 0; i < program->item_count; i++) {
        const Node *d = program->items[i];
        if (d->kind == NK_STATIC_CONST && (d->flags & NF_TYPE_ALIAS) &&
            d->child[1] && d->child[1]->kind == NK_NAME) {
            if (l.n_type_aliases == l.type_aliases_cap) {
                int cap = l.type_aliases_cap ? l.type_aliases_cap * 2 : 8;
                void *mem = realloc(l.type_aliases,
                    (size_t)cap * sizeof(*l.type_aliases));
                if (!mem) {
                    diag_report(diag, DK_ERROR, "<lower>", d->tok.line,
                                d->tok.col, "out of memory collecting type aliases");
                    lower_context_free(&l);
                    hir_module_free(l.mod);
                    return NULL;
                }
                l.type_aliases = mem;
                l.type_aliases_cap = cap;
            }
            l.type_aliases[l.n_type_aliases].name = d->name;
            l.type_aliases[l.n_type_aliases].target = d->child[1]->name;
            l.n_type_aliases++;
        }
    }
    lscope_push(&l);            /* module scope for future top-level lets */
    for (int i = 0; i < program->item_count; i++)
        lower_decl(&l, program->items[i]);
    lscope_pop(&l);
    lower_context_free(&l);
    if (diag_error_count(diag) > errs) {
        hir_module_free(l.mod);
        return NULL;
    }
    return l.mod;
}
