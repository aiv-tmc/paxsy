#include "common.h"
#include "hir.h"

/*
 * hir.c - construction and ownership of the High-level Intermediate
 * Representation.
 *
 * Two arenas live here, both process-lifetime:
 *
 *   - the type arena interns every HType requested through the
 *     hty_* constructors, so pointer equality means type identity
 *     anywhere in the pipeline. Interning compares the full shape
 *     (kind, width, element, count, name, parameter list), which
 *     makes the typing pass cheap: two equal types are always the
 *     same pointer and a single comparison replaces a deep walk.
 *
 *   - the node arena belongs to one HModule. hir_node() records every
 *     produced node in a growable owner array; hir_module_free()
 *     releases every node together with its item array in one sweep.
 *     Node names and string slices are borrowed from the AST and the
 *     source text, which outlive the HIR inside one pipeline run, so
 *     nothing here copies or frees them.
 *
 * hir_dump() prints the module as an indented tree for the --hir
 * driver flag: kind name, result type, binding name, constant value
 * and set effect flags per node.
 */

/* ------------------------------------------------------------------ */
/* Type arena                                                          */
/* ------------------------------------------------------------------ */

const HType hty_void = { HTY_VOID, 0, { { NULL } } };
const HType hty_bool = { HTY_BOOL, 0, { { NULL } } };
const HType hty_char = { HTY_CHAR, 0, { { NULL } } };

static HType *ty_arena;   /* never freed: types live for the process */
static int ty_count, ty_cap;

/* Returns a heap copy of `n` parameter type pointers (empty list -> NULL). */
static const HType **ty_params_copy(const HType **params, int n) {
    if (n <= 0) return NULL;
    const HType **p = malloc((size_t)n * sizeof(*p));
    memcpy(p, params, (size_t)n * sizeof(*p));
    return p;
}

/* Linear interning lookup: the arena stays small (a few dozen shapes
 * per module), so a scan beats a hash table and keeps the code plain. */
static const HType *ty_intern(const HType *key) {
    for (int i = 0; i < ty_count; i++) {
        const HType *t = &ty_arena[i];
        if (t->kind != key->kind || t->width != key->width) continue;
        switch (key->kind) {
        case HTY_PTR: case HTY_CHAN:
            if (t->u.ptr.elem == key->u.ptr.elem) return t;
            break;
        case HTY_ARRAY:
            if (t->u.array.elem == key->u.array.elem &&
                t->u.array.count == key->u.array.count) return t;
            break;
        case HTY_NAMED: case HTY_COMPTIME:
            if (strcmp(t->u.named.name, key->u.named.name) == 0) return t;
            break;
        case HTY_ERROR:
            if (t->u.error.value == key->u.error.value) return t;
            break;
        case HTY_FUNC:
            if (t->u.func.nparams == key->u.func.nparams &&
                t->u.func.ret == key->u.func.ret) {
                int same = 1;
                for (int k = 0; k < key->u.func.nparams; k++)
                    if (t->u.func.params[k] != key->u.func.params[k]) {
                        same = 0;
                        break;
                    }
                if (same) return t;
            }
            break;
        default:
            return t;   /* scalars: kind+width fully describe the type */
        }
    }
    if (ty_count == ty_cap) {
        ty_cap = ty_cap ? ty_cap * 2 : 64;
        ty_arena = realloc(ty_arena, (size_t)ty_cap * sizeof(*ty_arena));
    }
    ty_arena[ty_count] = *key;
    if (key->kind == HTY_FUNC)
        ty_arena[ty_count].u.func.params =
            ty_params_copy(key->u.func.params, key->u.func.nparams);
    return &ty_arena[ty_count++];
}

const HType *hty_int(int width) {
    HType k = { HTY_INT, width, { { NULL } } };
    return ty_intern(&k);
}

const HType *hty_uint(int width) {
    HType k = { HTY_UINT, width, { { NULL } } };
    return ty_intern(&k);
}

const HType *hty_real(int width) {
    HType k = { HTY_REAL, width, { { NULL } } };
    return ty_intern(&k);
}

const HType *hty_ptr(const HType *elem) {
    HType k = { HTY_PTR, 0, { { NULL } } };
    k.u.ptr.elem = elem;
    return ty_intern(&k);
}

const HType *hty_array(const HType *elem, long count) {
    HType k = { HTY_ARRAY, 0, { { NULL } } };
    k.u.array.elem = elem;
    k.u.array.count = count;
    return ty_intern(&k);
}

const HType *hty_error(const HType *value) {
    HType k = { HTY_ERROR, 0, { { NULL } } };
    k.u.error.value = value;
    return ty_intern(&k);
}

const HType *hty_named(const char *name) {
    HType k = { HTY_NAMED, 0, { { NULL } } };
    k.u.named.name = name;
    return ty_intern(&k);
}

const HType *hty_func(const HType **params, int nparams, const HType *ret) {
    HType k = { HTY_FUNC, 0, { { NULL } } };
    k.u.func.params = params;
    k.u.func.nparams = nparams;
    k.u.func.ret = ret;
    return ty_intern(&k);
}

const HType *hty_chan(const HType *elem) {
    HType k = { HTY_CHAN, 0, { { NULL } } };
    k.u.chan.elem = elem;
    return ty_intern(&k);
}

/* ------------------------------------------------------------------ */
/* Node arena                                                          */
/* ------------------------------------------------------------------ */

struct HModule {
    HNode *root;
    HNode **nodes;    /* owner list of every node in this module */
    int count, cap;
};

HModule *hir_module_new(void) {
    HModule *m = calloc(1, sizeof(*m));
    m->root = hir_node(m, HK_MODULE, (Token){ 0 });
    return m;
}

HNode *hir_root(HModule *m) { return m->root; }

/* Allocating the root before the module exists (hir_module_new passes
 * the half-built record) is safe: only the node array fields are read
 * and they start zeroed from calloc. */
HNode *hir_node(HModule *m, HNodeKind kind, Token tok) {
    HNode *n = calloc(1, sizeof(*n));
    n->kind = kind;
    n->tok = tok;
    n->aux = -1;          /* string collector has not seen this node  */
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 64;
        m->nodes = realloc(m->nodes, (size_t)m->cap * sizeof(*m->nodes));
    }
    m->nodes[m->count++] = n;
    return n;
}

/* Appends one statement/declaration to a block, module or match node. */
void hir_add(HNode *parent, HNode *item) {
    parent->items = realloc(parent->items,
                            (size_t)(parent->nitems + 1) * sizeof(*parent->items));
    parent->items[parent->nitems++] = item;
}

void hir_set_child(HNode *n, int slot, HNode *child) { n->child[slot] = child; }

void hir_module_free(HModule *m) {
    if (!m) return;
    for (int i = 0; i < m->count; i++) {
        free(m->nodes[i]->items);
        free(m->nodes[i]);
    }
    free(m->nodes);
    free(m);
}

/* ------------------------------------------------------------------ */
/* Text dump (--hir)                                                   */
/* ------------------------------------------------------------------ */

static const char *const kKindNames[] = {
    [HK_MODULE] = "module",       [HK_FUNC] = "func",
    [HK_EXTERN_FUNC] = "extern",  [HK_STRUCT] = "struct",
    [HK_UNION] = "union",         [HK_ENUM] = "enum",
    [HK_GLOBAL] = "global",       [HK_BLOCK] = "block",
    [HK_LET] = "let",             [HK_STORE] = "store",
    [HK_IF] = "if",               [HK_WHILE] = "while",
    [HK_DOWHILE] = "dowhile",     [HK_FOR] = "for",
    [HK_MATCH] = "match",         [HK_RETURN] = "return",
    [HK_BREAK] = "break",         [HK_CONTINUE] = "continue",
    [HK_DEFER] = "defer",         [HK_EXPR_STMT] = "expr_stmt",
    [HK_LABEL] = "label",         [HK_SEND] = "send",
    [HK_RECV] = "recv",           [HK_SELECT] = "select",
    [HK_CLOSE] = "close",         [HK_GO_ROUTINE] = "routine",
    [HK_ASYNC] = "async",         [HK_AWAIT] = "await",
    [HK_INLINE_ASM] = "asm",      [HK_CONST_INT] = "const_int",
    [HK_CONST_FLOAT] = "const_float", [HK_CONST_STRING] = "const_string",
    [HK_CONST_CHAR] = "const_char",   [HK_PARAM] = "param",
    [HK_LOCAL] = "local",         [HK_GLOBAL_REF] = "global_ref",
    [HK_BINOP] = "binop",         [HK_UNOP] = "unop",
    [HK_CALL] = "call",           [HK_CAST] = "cast",
    [HK_INDEX] = "index",         [HK_SLICE] = "slice",
    [HK_FIELD] = "field",         [HK_STRUCT_LIT] = "struct_lit",
    [HK_ARRAY_LIT] = "array_lit", [HK_ADDR] = "addr",
    [HK_DEREF] = "deref",         [HK_ALLOC] = "alloc",
    [HK_FREE] = "free",           [HK_NEW_CHAN] = "new_chan",
    [HK_TRY] = "try",             [HK_CATCH] = "catch",
    [HK_FMT] = "fmt",             [HK_PANIC] = "panic",
    [HK_ERR_VAL] = "err_val",
};

/* Short scalar name for the type slot of a dump line. */
static void ty_brief(const HType *t, char *buf, size_t n) {
    if (!t) { snprintf(buf, n, "?"); return; }
    switch (t->kind) {
    case HTY_VOID:  snprintf(buf, n, "Void");  break;
    case HTY_BOOL:  snprintf(buf, n, "Bool");  break;
    case HTY_CHAR:  snprintf(buf, n, "Char");  break;
    case HTY_INT:   snprintf(buf, n, "I%d", t->width);  break;
    case HTY_UINT:  snprintf(buf, n, "U%d", t->width);  break;
    case HTY_REAL:  snprintf(buf, n, "F%d", t->width);  break;
    case HTY_PTR:   snprintf(buf, n, "*");     break;
    case HTY_ARRAY: snprintf(buf, n, "[%ld]", t->u.array.count); break;
    case HTY_NAMED: snprintf(buf, n, "%s", t->u.named.name); break;
    case HTY_ERROR: snprintf(buf, n, "!");     break;
    case HTY_FUNC:  snprintf(buf, n, "fn");    break;
    case HTY_CHAN:  snprintf(buf, n, "chan");  break;
    default:        snprintf(buf, n, "?");     break;
    }
}

static void dump_node(const HNode *n, int depth, FILE *out) {
    char ty[24];
    for (int i = 0; i < depth; i++) fputs("  ", out);
    fprintf(out, "%s", kKindNames[n->kind]);
    ty_brief(n->type, ty, sizeof(ty));
    fprintf(out, " <%s>", ty);
    if (n->name) fprintf(out, " '%s'", n->name);
    if (n->kind == HK_CONST_INT) fprintf(out, " = %lld", n->u.ival);
    if (n->kind == HK_CONST_FLOAT) fprintf(out, " = %g", n->u.fval);
    if (n->kind == HK_CONST_STRING || n->kind == HK_CONST_CHAR)
        fprintf(out, " = \"%.*s\"", n->u.slice.len, n->u.slice.start);
    if (n->flags) fprintf(out, "  flags=0x%x", n->flags);
    fputc('\n', out);
    for (int i = 0; i < 4; i++)
        if (n->child[i]) dump_node(n->child[i], depth + 1, out);
    for (int i = 0; i < n->nitems; i++)
        dump_node(n->items[i], depth + 1, out);
}

void hir_dump(const HModule *m, FILE *out) { dump_node(m->root, 0, out); }
