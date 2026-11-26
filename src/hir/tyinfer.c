#include "common.h"
#include "tyinfer.h"
#include "tyinfer_int.h"
#include <limits.h>
#include <stdint.h>

/*
 * tyinfer.c - the HIR typing pass.
 *
 * Every value-producing node receives its concrete type here.
 * Annotated positions (parameters, let annotations, casts) already
 * carry their type from lowering; this pass fills the inferred ones:
 * integer literals take the smallest platform-independent signed
 * width (8/16/32/64/128) that holds the value, unannotated lets
 * inherit their initializer, binary operations widen to their wider
 * operand while comparisons and logic yield Bool, shifts keep the
 * left operand's type. Global references bind to the module
 * declaration they name, so a reference to a function carries the
 * function's return type and a call takes it directly; unresolved
 * call targets stay untyped and are flagged HF_UNKNOWN_CALL, which
 * keeps them on the CPU in the offload analysis. Immediately after a
 * node is typed the constant folder (fold.c) gets one in-place fold
 * attempt, so the backends see the fully reduced tree.
 *
 * Slot typing: every HK_LET and HK_FOR claims its u.index slot in one
 * flat table per function; HK_LOCAL/HK_PARAM read it back. Slots are
 * unique per binding (shadowing allocates a new slot), so a flat
 * table needs no scope stack.
 *
 * Function layout: a function node's type is its return type, its
 * parameters live in the items with their own types, and the body
 * sits in child[3]. A let keeps its initializer in child[1]; an
 * annotated array let retypes its array literal so the element width
 * follows the annotation instead of the smallest-literal rule.
 */

typedef struct { const char *name; const HNode *decl; } TBind;

typedef struct {
    Diag *diag;
    HModule *cur;             /* module being typed (node arena)     */
    HNode *cur_fn;            /* function being typed (return cast)*/
    TBind *binds;          /* module-level functions/globals by name */
    int nbinds, capbinds;
    const HType **slot_ty;    /* slot index -> type, per function    */
    int nslots, capslots;
} TI;

static void terr(TI *t, const Token *tok, const char *what) {
    diag_report_full(t->diag, DK_ERROR,
                     tok->path ? tok->path : "<tyinfer>",
                     tok->line, tok->col, tok->len > 0 ? tok->len : 1,
                     "%s", what);
}

static void ttype_name(const HType *ty, char *out, size_t cap) {
    if (!ty) { snprintf(out, cap, "unknown"); return; }
    switch (ty->kind) {
    case HTY_VOID: snprintf(out, cap, "Void"); break;
    case HTY_BOOL: snprintf(out, cap, "Bool"); break;
    case HTY_CHAR: snprintf(out, cap, "Char"); break;
    case HTY_INT: snprintf(out, cap, "Int%d", ty->width); break;
    case HTY_UINT: snprintf(out, cap, "UInt%d", ty->width); break;
    case HTY_REAL: snprintf(out, cap, "Real%d", ty->width); break;
    case HTY_PTR: snprintf(out, cap, "pointer"); break;
    case HTY_ARRAY: snprintf(out, cap, "array"); break;
    case HTY_VIEW: snprintf(out, cap, "view"); break;
    case HTY_NAMED: snprintf(out, cap, "%s", ty->u.named.name); break;
    case HTY_TUPLE: snprintf(out, cap, "tuple"); break;
    case HTY_ERROR: snprintf(out, cap, "error value"); break;
    case HTY_FUNC: snprintf(out, cap, "function"); break;
    case HTY_CHAN: snprintf(out, cap, "channel"); break;
    default: snprintf(out, cap, "unknown"); break;
    }
}

static void terr_assign(TI *t, const Token *tok, const HType *expected,
                        const HType *actual, const char *context) {
    char want[80], got[80], msg[256];
    ttype_name(expected, want, sizeof(want));
    ttype_name(actual, got, sizeof(got));
    snprintf(msg, sizeof(msg), "%s: expected %s, found %s",
             context, want, got);
    const char *help = expected && actual &&
        expected->kind == HTY_CHAR &&
        (actual->kind == HTY_INT || actual->kind == HTY_UINT)
        ? "Use a character literal such as 'A', or explicitly convert with `65 as Char`."
        : "Change the value or declaration, or add an explicit cast "
          "when the conversion is intentional.";
    diag_report_detail(t->diag, DK_ERROR,
        tok->path ? tok->path : "<tyinfer>", tok->line, tok->col,
        tok->len > 0 ? tok->len : 1,
        "Paxsy checks this type boundary before code generation; "
        "the value cannot be used as the declared type.",
        help, "%s", msg);
}

static void tbind(TI *t, const char *name, const HNode *decl) {
    if (t->nbinds == t->capbinds) {
        t->capbinds = t->capbinds ? t->capbinds * 2 : 32;
        t->binds = realloc(t->binds, (size_t)t->capbinds * sizeof(*t->binds));
    }
    t->binds[t->nbinds].name = name;
    t->binds[t->nbinds].decl = decl;
    t->nbinds++;
}

static const char *ttail(const char *name) {
    const char *tail = strrchr(name ? name : "", ':');
    return tail && tail[1] ? tail + 1 : (name ? name : "");
}

static const HNode *tfind(TI *t, const char *name) {
    const HNode *match = NULL;
    for (int i = 0; i < t->nbinds; i++) {
        if (strcmp(t->binds[i].name, name) == 0) return t->binds[i].decl;
        if (strcmp(ttail(t->binds[i].name), ttail(name)) == 0) {
            if (match) return NULL; /* ambiguous short type name */
            match = t->binds[i].decl;
        }
    }
    return match;
}

/* The wider of two scalar types: reals dominate integers, otherwise
 * the larger width wins and a signed operand keeps the result
 * signed. A NULL side yields the other, so partially-typed trees
 * still make progress and collect every error in one pass. */
static const HType *twider(const HType *a, const HType *b) {
    if (!a) return b;
    if (!b) return a;
    if (a->kind == HTY_REAL || b->kind == HTY_REAL) return hty_real(64);
    if (a->kind != HTY_INT && a->kind != HTY_UINT) return a;
    if (b->kind != HTY_INT && b->kind != HTY_UINT) return b;
    int w = a->width > b->width ? a->width : b->width;
    return a->kind == HTY_INT || b->kind == HTY_INT
        ? hty_int(w) : hty_uint(w);
}

/* Smallest signed width holding `v` (8/16/32/64), else 128. */
static const HType *tint_type(long long v) {
    for (int i = 0; i < 4; i++) {
        int w = 8 << i;
        if (v >= -(1LL << (w - 1)) && v <= (1LL << (w - 1)) - 1)
            return hty_int(w);
    }
    return hty_int(128);
}

static const HType *tslot(TI *t, int slot) {
    return slot >= 0 && slot < t->nslots ? t->slot_ty[slot] : NULL;
}

static void tset(TI *t, int slot, const HType *ty) {
    if (slot < 0 || !ty) return;
    if (slot >= t->capslots) {
        int c = t->capslots ? t->capslots : 64;
        while (c <= slot) c *= 2;
        t->slot_ty = realloc(t->slot_ty, (size_t)c * sizeof(*t->slot_ty));
        for (int i = t->capslots; i < c; i++) t->slot_ty[i] = NULL;
        t->capslots = c;
    }
    if (slot >= t->nslots) t->nslots = slot + 1;
    t->slot_ty[slot] = ty;
}

static void twalk(TI *t, HNode *n);

/* Struct field lookup: the object type (or the pointee of a pointer
 * to one) names the aggregate; the declaration's field descriptors
 * give slot and type. */
/* Conservative assignability: the source type may initialize or be
 * assigned to the destination type. Numeric widths and signs convert
 * freely (the language computes in 64-bit); a fixed array decays to
 * a pointer to its element (string literals are Char[]); every other
 * type must match exactly. This rejects the silent Char[]-to-Int
 * hole while keeping all specification-sanctioned coercions. */
static int tnumeric(int k) {
    return k == HTY_INT || k == HTY_UINT || k == HTY_REAL ||
           k == HTY_CHAR;
}

/* Type equality with the Char/Int8/UInt8 representation merge. */
static int teq(const HType *a, const HType *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    int ca = a->kind == HTY_CHAR, cb = b->kind == HTY_CHAR;
    if (ca != cb) {
        const HType *i = ca ? b : a;
        if ((i->kind == HTY_INT || i->kind == HTY_UINT) && i->width == 8)
            return 1;
    }
    if (a->kind != b->kind) return 0;
    if (a->kind == HTY_TUPLE) {
        if (a->u.tuple.count != b->u.tuple.count) return 0;
        for (int i = 0; i < a->u.tuple.count; ++i)
            if (!teq(a->u.tuple.elems[i], b->u.tuple.elems[i])) return 0;
        return 1;
    }
    return 0;
}

static int tassignable(const HType *dst, const HType *src) {
    if (!dst || !src) return 1;            /* unknown: other passes report */
    if (dst == src) return 1;
    /* A T! annotation against a value the call already unwrapped
     * (call sites carry the payload type; the error flag flows via
     * the error path, not the value) is the natural binding. */
    if (dst->kind == HTY_ERROR && src->kind == HTY_ERROR)
        return tassignable(dst->u.error.value, src->u.error.value);
    if (dst->kind == HTY_ERROR)
        return tassignable(dst->u.error.value, src);
    if (src->kind == HTY_ERROR)
        return tassignable(dst, src->u.error.value);
    if (dst->kind == HTY_VOID || src->kind == HTY_VOID) return 0;
    /* Char is semantically distinct from numeric values. Converting
     * an integer to a character requires an explicit `as Char` cast. */
    if (dst->kind == HTY_CHAR || src->kind == HTY_CHAR)
        return dst->kind == HTY_CHAR && src->kind == HTY_CHAR;
    if ((dst->kind == HTY_INT || dst->kind == HTY_UINT ||
         dst->kind == HTY_REAL) &&
        (src->kind == HTY_INT || src->kind == HTY_UINT ||
         src->kind == HTY_REAL))
        return 1;
    if (dst->kind == HTY_BOOL && src->kind == HTY_BOOL) return 1;
    /* Char and the 8-bit integer types share the representation. */
    if (dst->kind == HTY_CHAR &&
        ((src->kind == HTY_INT || src->kind == HTY_UINT) &&
         src->width == 8)) return 1;
    if (src->kind == HTY_CHAR &&
        ((dst->kind == HTY_INT || dst->kind == HTY_UINT) &&
         dst->width == 8)) return 1;
    if (dst->kind == HTY_PTR && src->kind == HTY_PTR) {
        const HType *de = dst->u.ptr.elem;
        const HType *se = src->u.ptr.elem;
        /* `&Void` is the language's erased/opaque pointer type.  APIs
         * such as std::heap::free accept it so any typed allocation can
         * be released without an unsafe, noisy cast at every call site.
         * Keep typed pointers invariant otherwise (apart from the
         * existing numeric representation compatibility). */
        if (!de || !se || de->kind == HTY_VOID || se->kind == HTY_VOID)
            return 1;
        return teq(de, se) ||
               (tnumeric(de->kind) && tnumeric(se->kind));
    }
    /* Address-of carries the pointee's type in this implementation
     * (addresses are untyped words); assigning any numeric address
     * word to a pointer is the sanctioned decay. */
    if (dst->kind == HTY_PTR && tnumeric(src->kind))
        return 1;
    if (dst->kind == HTY_PTR && src->kind == HTY_ARRAY)
        return teq(dst->u.ptr.elem, src->u.array.elem);
    if (dst->kind == HTY_VIEW && src->kind == HTY_VIEW)
        return teq(dst->u.ptr.elem, src->u.ptr.elem);
    if (dst->kind == HTY_ARRAY && src->kind == HTY_ARRAY)
        return teq(dst->u.array.elem, src->u.array.elem) ||
               (tnumeric(dst->u.array.elem->kind) &&
                tnumeric(src->u.array.elem->kind));
    if (dst->kind == HTY_ARRAY && src->kind == HTY_PTR &&
        teq(dst->u.array.elem, src->u.ptr.elem))
        return 1;
    if (dst->kind == HTY_NAMED || src->kind == HTY_NAMED)
        return dst == src;
    if (dst->kind == HTY_TUPLE && src->kind == HTY_TUPLE) {
        if (dst->u.tuple.count != src->u.tuple.count) return 0;
        for (int i = 0; i < dst->u.tuple.count; ++i)
            if (!tassignable(dst->u.tuple.elems[i],
                             src->u.tuple.elems[i])) return 0;
        return 1;
    }
    return 0;
}

static const HType *tfield(TI *t, HNode *f) {
    const HType *ot = f->child[0] ? f->child[0]->type : NULL;
    if (ot && ot->kind == HTY_PTR) ot = ot->u.ptr.elem;
    /* A view's length lives in the fat pair; the field keeps its
     * runtime form and the backend loads it. */
    if (ot && ot->kind == HTY_VIEW) {
        if (strcmp(f->name, "len") == 0) return hty_int(64);
        if (strcmp(f->name, "ptr") == 0) return hty_ptr(ot->u.ptr.elem);
        return NULL;
    }
    /* The length of a fixed-size array is a compile-time constant. */
    if (ot && ot->kind == HTY_ARRAY && ot->u.array.count >= 0 &&
        strcmp(f->name, "len") == 0) {
        f->kind = HK_CONST_INT;
        f->u.ival = ot->u.array.count;
        return hty_int(64);
    }
    if (!ot || ot->kind != HTY_NAMED) return NULL;
    const HNode *s = tfind(t, ot->u.named.name);
    /* static bindings wrap the aggregate in a global node */
    if (s && s->kind == HK_GLOBAL && s->child[0]) s = s->child[0];
    if (s && s->kind == HK_ENUM) {
        /* an enum variant is its discriminant constant */
        for (int i = 0; i < s->nitems; i++)
            if (strcmp(s->items[i]->name, f->name) == 0) {
                f->kind = HK_CONST_INT;
                f->u.ival = s->items[i]->u.ival;
                return ot;
            }
        return NULL;
    }
    if (!s || (s->kind != HK_STRUCT && s->kind != HK_UNION)) return NULL;
    for (int i = 0; i < s->nitems; i++)
        if (strcmp(s->items[i]->name, f->name) == 0) {
            f->u.index = i;
            return s->items[i]->type;
        }
    return NULL;
}

static const HType *twalk_expr(TI *t, HNode *n) {
    switch (n->kind) {
    /* A preset type (boolean literals lower as constant 1/0 with
     * the Bool type already attached) wins over the smallest-width
     * integer rule. */
    case HK_CONST_INT:    if (!n->type) n->type = tint_type(n->u.ival); break;
    case HK_CONST_FLOAT:  n->type = hty_real(64); break;
    case HK_CONST_STRING: n->type = hty_ptr(&hty_char); break;
    case HK_FMT: {
        /* type every interpolated operand like any expression; the
         * whole node is a freshly built &Char buffer */
        for (int i = 0; i < n->nitems; i++) twalk(t, n->items[i]);
        n->type = hty_ptr(&hty_char);
        break;
    }
    case HK_CONST_CHAR:   n->type = &hty_char; break;
    case HK_PARAM: case HK_LOCAL:
        n->type = tslot(t, n->u.index); break;
    case HK_GLOBAL_REF: {
        const HNode *d = tfind(t, n->name);
        /* a reference to a type name (enum/struct/union declared via
         * static) carries the named type itself */
        if (d && (d->kind == HK_ENUM || d->kind == HK_STRUCT ||
                  d->kind == HK_UNION))
            n->type = hty_named(n->name);
        else
            n->type = d ? d->type : NULL;
        break;
    }
    case HK_BINOP: {
        twalk(t, n->child[0]);
        twalk(t, n->child[1]);
        const HType *lt = n->child[0] ? n->child[0]->type : NULL;
        const HType *rt = n->child[1] ? n->child[1]->type : NULL;
        switch (n->u.binop) {
        case HOP_EQ: case HOP_NE: case HOP_LT: case HOP_LE:
        case HOP_GT: case HOP_GE: case HOP_LAND: case HOP_LOR:
            n->type = &hty_bool; break;
        case HOP_SHL: case HOP_SHR: case HOP_USHR:
            n->type = lt; break;
        default: {
            /* Arithmetic on arrays (string literals are Char[]) and
             * pointer-on-pointer arithmetic have no meaning; reject
             * them here so the backend never sees a bogus scaling. */
            int bad = 0;
            if ((lt && lt->kind == HTY_ARRAY) ||
                (rt && rt->kind == HTY_ARRAY))
                bad = 1;
            if (n->u.binop != HOP_SUB &&
                (lt && lt->kind == HTY_PTR) &&
                (rt && rt->kind == HTY_PTR))
                bad = 1;
            if (bad) {
                terr(t, &n->tok,
                     "this operator is not defined for array or "
                     "pointer-pair operands");
                n->type = &hty_void;
                break;
            }
            /* The distance of two pointers is an element count, an
             * integer - never a pointer. */
            if (n->u.binop == HOP_SUB &&
                lt && lt->kind == HTY_PTR &&
                rt && rt->kind == HTY_PTR)
                n->type = hty_int(64);
            else
                n->type = twider(lt, rt);
            break;
        }
        }
        tfold_expr(n);
        break;
    }
    case HK_UNOP: {
        twalk(t, n->child[0]);
        const HType *ot = n->child[0] ? n->child[0]->type : NULL;
        n->type = n->u.unop == HUN_LNOT ? &hty_bool :
                  n->u.unop == HOP_ADDR_OF && ot
                      ? hty_ptr(ot->kind == HTY_ARRAY ? ot->u.array.elem : ot) :
                  n->u.unop == HUN_DEREF && ot && ot->kind == HTY_PTR
                      ? ot->u.ptr.elem : ot;
        tfold_expr(n);
        break;
    }
    case HK_CALL: {
        twalk(t, n->child[0]);
        for (int i = 0; i < n->nitems; i++) twalk(t, n->items[i]);
        /* a function reference carries the return type; T!
         * unwraps to the payload, anything else stays external.
         * A preset type (e.g. alloc's concrete &T) always wins. */
        const HType *ct = n->child[0]->type;
        if (!n->type) {
            if (ct && ct->kind == HTY_ERROR) n->type = ct->u.error.value;
            else if (ct) n->type = ct;
        }
        if (!n->type && !ct) n->flags |= HF_UNKNOWN_CALL;
        if (n->child[0] && n->child[0]->kind == HK_GLOBAL_REF) {
            const HNode *decl = tfind(t, n->child[0]->name);
            if (decl && (decl->kind == HK_FUNC ||
                         decl->kind == HK_EXTERN_FUNC)) {
                if (decl->nitems != n->nitems) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "function expects %d argument(s), but %d were supplied",
                             decl->nitems, n->nitems);
                    diag_report_detail(t->diag, DK_ERROR,
                        n->tok.path ? n->tok.path : "<tyinfer>",
                        n->tok.line, n->tok.col,
                        n->tok.len > 0 ? n->tok.len : 1,
                        "The call does not match the function's declared parameter list.",
                        "Pass every required argument exactly once, in declaration order.",
                        "%s", msg);
                } else {
                    for (int i = 0; i < n->nitems; i++) {
                        const HType *pt = decl->items[i]->type;
                        const HType *at = n->items[i]->type;
                        int string_format = pt && at &&
                            ((pt->kind == HTY_ARRAY &&
                              pt->u.array.elem->kind == HTY_CHAR) ||
                             (pt->kind == HTY_PTR &&
                              pt->u.ptr.elem->kind == HTY_CHAR)) &&
                            (at->kind == HTY_INT || at->kind == HTY_UINT ||
                             at->kind == HTY_BOOL || at->kind == HTY_CHAR);
                        int error_constructor = pt &&
                            pt->kind == HTY_ERROR &&
                            n->items[i]->kind == HK_ERR_VAL;
                        if (error_constructor) n->items[i]->type = pt;
                        if (pt && at && !string_format && !error_constructor &&
                            !tassignable(pt, at))
                            terr_assign(t, &n->items[i]->tok, pt, at,
                                        "function argument type mismatch");
                    }
                }
            }
        }
        /* A scalar argument bound to a Char[] parameter is
         * auto-formatted as a decimal string by the backend; flag it
         * so the emitter converts the value in place. */
        if (n->child[0]->kind == HK_GLOBAL_REF) {
            const HNode *decl = tfind(t, n->child[0]->name);
            if (decl && decl->kind == HK_FUNC && decl->nitems >= n->nitems) {
                for (int i = 0; i < n->nitems; i++) {
                    const HType *pt = decl->items[i]->type;
                    const HType *at = n->items[i]->type;
                    int want_str =
                        pt &&
                        ((pt->kind == HTY_ARRAY &&
                          pt->u.array.elem->kind == HTY_CHAR) ||
                         (pt->kind == HTY_PTR &&
                          pt->u.ptr.elem->kind == HTY_CHAR));
                    int is_scalar =
                        at && (at->kind == HTY_INT ||
                               at->kind == HTY_UINT ||
                               at->kind == HTY_BOOL ||
                               at->kind == HTY_CHAR);
                    if (want_str && is_scalar)
                        n->items[i]->flags |= HF_FSTR;
                }
            }
        }
        break;
    }
    case HK_STORE: {
        const HType *lt = n->child[0] ? twalk_expr(t, n->child[0]) : NULL;
        const HType *rt = n->child[1] ? twalk_expr(t, n->child[1]) : NULL;
        int error_constructor = lt && lt->kind == HTY_ERROR &&
            n->child[1] && n->child[1]->kind == HK_ERR_VAL;
        if (error_constructor) n->child[1]->type = lt;
        if (lt && rt && !error_constructor && !tassignable(lt, rt))
            terr_assign(t, &n->tok, lt, rt, "assignment type mismatch");
        n->type = &hty_void;
        break;
    }
    case HK_CAST:    twalk(t, n->child[0]); break;  /* type preset  */
    case HK_SLICE: {
        twalk(t, n->child[0]);
        if (n->child[1]) twalk(t, n->child[1]);
        if (n->child[2]) twalk(t, n->child[2]);
        const HType *bt = n->child[0] ? n->child[0]->type : NULL;
        const HType *elem = NULL;
        if (bt && bt->kind == HTY_ARRAY) elem = bt->u.array.elem;
        else if (bt && bt->kind == HTY_VIEW) elem = bt->u.ptr.elem;
        else if (bt && bt->kind == HTY_PTR) elem = bt->u.ptr.elem;
        if (!elem) {
            terr(t, &n->tok, "slicing is only defined for arrays and views");
            break;
        }
        n->type = hty_view(elem);
        break;
    }
    case HK_INDEX: {
        twalk(t, n->child[0]);
        twalk(t, n->child[1]);
        const HType *bt = n->child[0] ? n->child[0]->type : NULL;
        if (bt && bt->kind == HTY_TUPLE) {
            /* constant element position selects the slot type */
            n->type = &hty_void;
            if (n->child[1] && n->child[1]->kind == HK_CONST_INT &&
                n->child[1]->u.ival >= 0 &&
                n->child[1]->u.ival < bt->u.tuple.count)
                n->type = bt->u.tuple.elems[n->child[1]->u.ival];
        } else
            n->type = bt && bt->kind == HTY_ARRAY ? bt->u.array.elem :
                      bt && bt->kind == HTY_VIEW  ? bt->u.ptr.elem :
                      bt && bt->kind == HTY_PTR   ? bt->u.ptr.elem : NULL;
        break;
    }
    case HK_FIELD:
        twalk(t, n->child[0]);
        n->type = tfield(t, n);
        break;
    case HK_STRUCT_LIT: {
        /* NULL items are unset fields (named-init gaps) */
        for (int i = 0; i < n->nitems; i++)
            if (n->items[i]) twalk(t, n->items[i]);
        n->type = hty_named(n->name);
        /* Named initializers were bound to field positions during
         * lowering; the backend stores item k at field k and leaves
         * NULL gaps zeroed. */
        break;
    }
    case HK_ARRAY_LIT: {
        /* Homogeneous items form an array; a preset type (annotated
         * array let) fixes the element type; heterogeneous items
         * form a tuple with one slot per element. */
        const HType *et = NULL;
        int mixed = 0;
        const HType *preset = n->type;
        for (int i = 0; i < n->nitems; i++) {
            twalk(t, n->items[i]);
            const HType *it = n->items[i]->type;
            if (et && it && it != et) mixed = 1;
            if (!et) et = it;
            et = twider(et, it);
        }
        if (preset && preset->kind == HTY_ARRAY) {
            /* annotated array let: keep the preset type */
        } else if (preset && preset->kind == HTY_TUPLE) {
            /* A tuple annotation supplies the missing context for a
             * homogeneous brace literal: `{1, 2}` is otherwise inferred
             * as an array because its elements share one type. */
            if (preset->u.tuple.count != n->nitems) {
                terr(t, &n->tok, "tuple literal element count does not match its annotation");
            } else {
                for (int i = 0; i < n->nitems; ++i)
                    if (!tassignable(preset->u.tuple.elems[i],
                                     n->items[i]->type))
                        terr(t, &n->items[i]->tok,
                             "tuple element is not assignable to its annotated type");
                n->type = preset;
            }
        }
        else if (mixed) {
            const HType **elems =
                calloc((size_t)n->nitems, sizeof(HType *));
            for (int i = 0; i < n->nitems; i++)
                elems[i] = n->items[i]->type;
            n->type = hty_tuple(elems, n->nitems);
        } else
            n->type = hty_array(et ? et : &hty_void, n->nitems);
        break;
    }
    case HK_ADDR:
        twalk(t, n->child[0]);
        n->type = hty_ptr(n->child[0] ? n->child[0]->type : NULL);
        break;
    case HK_DEREF: {
        twalk(t, n->child[0]);
        const HType *pt = n->child[0] ? n->child[0]->type : NULL;
        n->type = pt && pt->kind == HTY_PTR ? pt->u.ptr.elem : NULL;
        break;
    }
    case HK_RECV: {
        twalk(t, n->child[0]);
        const HType *ct = n->child[0] ? n->child[0]->type : NULL;
        n->type = ct && ct->kind == HTY_CHAN ? ct->u.chan.elem
                                             : hty_int(64);
        break;
    }
    case HK_NEW_CHAN: case HK_ALLOC:
        if (n->child[0]) twalk(t, n->child[0]);
        break;
    case HK_TRY:
        twalk(t, n->child[0]);
        n->type = n->child[0] && n->child[0]->type &&
                  n->child[0]->type->kind == HTY_ERROR
                      ? n->child[0]->type->u.error.value
                      : (n->child[0] ? n->child[0]->type : NULL);
        break;
    case HK_CATCH: {
        /* The expression yields the operand's value type; the
         * handler must produce that same type or diverge (a block
         * that only returns is void-typed and stays legal). */
        const HType *vt, *ht;
        twalk(t, n->child[0]);
        /* Error codes travel in the native ABI's 64-bit RDX return
         * register. Give the catch binding that concrete integer type
         * before walking its body so every use (including formatting)
         * is typed consistently. */
        if (n->name && n->u.index >= 0)
            tset(t, n->u.index, hty_int(64));
        twalk(t, n->child[1]);
        vt = n->child[0] && n->child[0]->type &&
             n->child[0]->type->kind == HTY_ERROR
                 ? n->child[0]->type->u.error.value
                 : (n->child[0] ? n->child[0]->type : NULL);
        ht = n->child[1] ? n->child[1]->type : NULL;
        n->type = vt;
        if (vt && ht && ht->kind != HTY_VOID && ht->kind != HTY_ERROR &&
            !tassignable(vt, ht)) {
            terr(t, &n->tok,
                 "the catch handler must yield the operand's type "
                 "or diverge");
        }
        break;
    }
    case HK_AWAIT:
        twalk(t, n->child[0]);
        n->type = hty_int(64);               /* async slot result */
        break;
    case HK_ERR_VAL:
        twalk(t, n->child[0]);
        n->type = hty_error(n->child[0] ? n->child[0]->type : &hty_void);
        break;
    case HK_IF: {
        twalk(t, n->child[0]);
        if (n->child[1]) twalk(t, n->child[1]);
        if (n->child[2]) twalk(t, n->child[2]);
        n->type = (n->flags & HF_EXPR)
            ? (n->child[1] ? n->child[1]->type
                           : n->child[0]->type)   /* bare condition */
            : &hty_void;
        break;
    }
    case HK_MATCH: {
        twalk(t, n->child[0]);
        for (int i = 0; i < n->nitems; i++) {
            HNode *c = n->items[i];
            for (int k = 0; k < c->nitems; k++) twalk(t, c->items[k]);
            twalk(t, c->child[1]);
        }
        if (n->child[2]) twalk(t, n->child[2]);
        n->type = (n->flags & HF_EXPR) && n->nitems > 0
            ? n->items[0]->child[1]->type : &hty_void;
        break;
    }
    default:
        /* Everything else (store, send, close, free, select, blocks
         * reached in expression position, ...) walks its children
         * for typing and yields Void. */
        for (int i = 0; i < 4; i++) twalk(t, n->child[i]);
        for (int i = 0; i < n->nitems; i++) twalk(t, n->items[i]);
        n->type = &hty_void;
        break;
    }
    return n->type;
}

/* Statements: bindings claim slots; expressions route through the
 * expression walker; structured control flow recurses in order. */
static void twalk(TI *t, HNode *n) {
    if (!n) return;
    switch (n->kind) {
    case HK_BLOCK: {
        for (int i = 0; i < n->nitems; i++) twalk(t, n->items[i]);
        if (n->nitems > 0) {
            HNode *last = n->items[n->nitems - 1];
            /* A block used as an if/match expression arm can end in a
             * nested value-producing control-flow expression. The
             * lowering pass marks that exact trailing node with
             * HF_ARMVAL; preserve its inferred type just as we preserve
             * the type of a trailing expression statement. */
            if (last->kind == HK_EXPR_STMT && last->child[0])
                n->type = last->child[0]->type;
            else if (last->flags & HF_ARMVAL)
                n->type = last->type;
            else
                n->type = &hty_void;
        } else {
            n->type = &hty_void;
        }
        break;
    }
    case HK_LET: {
        /* the initializer lives in child[1] */
        if (n->child[1]) {
            /* Propagate tuple context into a homogeneous brace literal;
             * without it the literal defaults to a fixed array. */
            if (n->type && n->type->kind == HTY_TUPLE &&
                n->child[1]->kind == HK_ARRAY_LIT)
                n->child[1]->type = n->type;
            twalk(t, n->child[1]);
            /* an annotated array let retypes its literal so the
             * element width follows the annotation, not the
             * smallest-literal-width rule */
            if (getenv("PAXSY_TDBG"))
                fprintf(stderr, "[let '%s'] dst kind=%d src kind=%d\n",
                        n->name ? n->name : "?", n->type ? (int)n->type->kind : -9,
                        n->child[1]->type ? (int)n->child[1]->type->kind : -9);
            int error_constructor = n->child[1]->kind == HK_ERR_VAL &&
                n->type && n->type->kind == HTY_ERROR;
            if (error_constructor) n->child[1]->type = n->type;
            if (n->type && n->child[1]->type && !error_constructor &&
                !tassignable(n->type, n->child[1]->type)) {
                if (getenv("PAXSY_TDBG"))
                    fprintf(stderr, "[let '%s'] dst kind=%d "
                            "src kind=%d\n", n->name ? n->name : "?",
                            (int)n->type->kind,
                            (int)n->child[1]->type->kind);
                diag_report_detail(t->diag, DK_ERROR,
                    n->child[1]->tok.path ? n->child[1]->tok.path :
                        (n->tok.path ? n->tok.path : "<tyinfer>"),
                    n->child[1]->tok.line, n->child[1]->tok.col,
                    n->child[1]->tok.len > 0 ? n->child[1]->tok.len : 1,
                    "The declared type and initializer type are different; "
                    "Paxsy does not reinterpret an integer as a character.",
                    n->type->kind == HTY_CHAR
                        ? "Use a character literal, for example: let c: Char = 'A'; "
                          "or make the conversion explicit with `65 as Char`."
                        : "Change the annotation or explicitly convert the initializer.",
                    "type of the initializer is not assignable "
                    "to the annotated type of the binding");
                break;
            }
            /* An implicit numeric conversion must be materialized:
             * without the cast the raw bits of the initializer would
             * land in a slot of a different interpretation. */
            if (n->type && n->child[1]->type &&
                n->type != n->child[1]->type &&
                tnumeric(n->type->kind) &&
                tnumeric(n->child[1]->type->kind)) {
                /* The node registry in the module owns the cast; it
                 * must not be appended to a block (hir_add parents it
                 * under a block root, which is not what a wrapped
                 * initializer wants). */
                HNode *c = hir_node(t->cur, HK_CAST, n->tok);
                c->type = n->type;
                c->child[0] = n->child[1];
                n->child[1] = c;
            }
            if (n->type && n->type->kind == HTY_ARRAY &&
                n->child[1]->kind == HK_ARRAY_LIT) {
                /* An unsized annotation (T[]) takes its size from
                 * the literal: the array lives in place, so the
                 * count must be known before slots are laid out. */
                if (n->type->u.array.count == 0 &&
                    n->child[1]->nitems > 0)
                    n->type = hty_array(n->type->u.array.elem,
                                        n->child[1]->nitems);
                n->child[1]->type = n->type;
            }
            if (!n->type) n->type = n->child[1]->type;
        }
        if (!n->type) {
            terr(t, &n->tok, "cannot infer the type of an uninitialised let");
            n->type = &hty_void;
        }
        tset(t, n->u.index, n->type);
        break;
    }
    case HK_FOR: {
        const HType *lt = n->child[0] ? twalk_expr(t, n->child[0]) : NULL;
        const HType *rt = n->child[1] ? twalk_expr(t, n->child[1]) : NULL;
        const HType *vt = twider(lt, rt);
        tset(t, n->u.index, vt ? vt : hty_int(64));
        n->type = &hty_void;
        twalk(t, n->child[2]);
        break;
    }
    case HK_WHILE:
        twalk_expr(t, n->child[0]);
        twalk(t, n->child[1]);
        n->type = &hty_void;
        break;
    case HK_DOWHILE:
        twalk(t, n->child[0]);
        twalk_expr(t, n->child[1]);
        n->type = &hty_void;
        break;
    case HK_RETURN: {
        const HType *expected = t->cur_fn ? t->cur_fn->type : NULL;
        const HType *actual = NULL;
        if (n->child[0]) actual = twalk_expr(t, n->child[0]);
        if (expected && expected->kind == HTY_VOID && n->child[0]) {
            terr_assign(t, &n->child[0]->tok, expected, actual,
                        "return value in a Void function");
        } else if (expected && expected->kind != HTY_VOID && !n->child[0] &&
                   !(expected->kind == HTY_ERROR &&
                     expected->u.error.value &&
                     expected->u.error.value->kind == HTY_VOID)) {
            terr(t, &n->tok, "non-Void function must return a value");
        } else if (expected && actual &&
                   !(n->child[0]->kind == HK_ERR_VAL &&
                     expected->kind == HTY_ERROR) &&
                   !tassignable(expected, actual)) {
            terr_assign(t, &n->child[0]->tok, expected, actual,
                        "return type mismatch");
        } else if (expected && actual && expected != actual &&
                   tnumeric(expected->kind) && tnumeric(actual->kind)) {
            HNode *c = hir_node(t->cur, HK_CAST, n->tok);
            c->type = expected;
            c->child[0] = n->child[0];
            n->child[0] = c;
        }
        n->type = &hty_void;
        break;
    }
    case HK_DEFER:
        twalk(t, n->child[0]);
        n->type = &hty_void;
        break;
    case HK_LABEL:
        twalk(t, n->child[0]);
        n->type = &hty_void;
        break;
    default:
        twalk_expr(t, n);
        break;
    }
}

/* Seeds parameter slots from the parameter nodes, then types the
 * body found in child[3]. */
static void tfunc(TI *t, HNode *f) {
    t->nslots = 0;
    t->cur_fn = f;
    for (int i = 0; i < f->nitems; i++)
        tset(t, f->items[i]->u.index, f->items[i]->type);
    if (f->child[3]) twalk(t, f->child[3]);
}

/* Compile-time constant expansion -------------------------------------
 *
 * A `static` value is represented in the AST as an immutable global so
 * declarations remain visible to name/type resolution. After every
 * module has been typed, this pass substitutes eligible static scalar
 * expressions at use sites. The expansion is deliberately limited to
 * side-effect-free constant trees; unsupported values remain ordinary
 * global references rather than being guessed or partially evaluated.
 *
 * A per-expansion declaration stack rejects cycles, and address-taking
 * keeps the original global reference so `&CONSTANT` retains storage.
 */
typedef struct {
    const HNode *decls[128];
    int ndecls;
    int fuel;
    const HNode *cycle;
} TiCtContext;

static int ti_ct_node_is_safe(const HNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case HK_CONST_INT: case HK_CONST_FLOAT:
    case HK_CONST_STRING: case HK_CONST_CHAR:
        return 1;
    case HK_BINOP:
        if (!n->child[0] || !n->child[1]) return 0;
        if ((n->u.binop == HOP_DIV || n->u.binop == HOP_MOD) &&
            n->child[1]->kind == HK_CONST_INT &&
            n->child[1]->u.ival == 0) return 0;
        if (n->u.binop == HOP_DIV &&
            n->child[1]->kind == HK_CONST_FLOAT &&
            n->child[1]->u.fval == 0.0) return 0;
        if (n->u.binop == HOP_DIV || n->u.binop == HOP_MOD) {
            if (n->child[0]->kind == HK_CONST_INT &&
                n->child[1]->kind == HK_CONST_INT &&
                n->child[0]->u.ival == LLONG_MIN &&
                n->child[1]->u.ival == -1 &&
                (!n->child[0]->type ||
                 n->child[0]->type->kind != HTY_UINT)) return 0;
        }
        return 1;
    case HK_UNOP:
        return n->child[0] && n->u.unop != HUN_DEREF &&
               n->u.unop != HOP_ADDR_OF;
    case HK_CAST:
        return n->child[0] != NULL;
    default:
        return 0;
    }
}

static int ti_ct_is_const_expr(const HNode *n) {
    if (!ti_ct_node_is_safe(n)) return 0;
    switch (n->kind) {
    case HK_CONST_INT: case HK_CONST_FLOAT:
    case HK_CONST_STRING: case HK_CONST_CHAR:
        return 1;
    case HK_BINOP:
        return ti_ct_is_const_expr(n->child[0]) &&
               ti_ct_is_const_expr(n->child[1]);
    case HK_UNOP: case HK_CAST:
        return ti_ct_is_const_expr(n->child[0]);
    default:
        return 0;
    }
}

static HNode *ti_ct_clone(TI *t, HModule *owner, const HNode *src,
                          TiCtContext *ctx) {
    if (!src || ctx->fuel-- <= 0 || ctx->ndecls >= 128) return NULL;
    if (src->kind == HK_GLOBAL_REF) {
        const HNode *decl = tfind(t, src->name);
        if (!decl || decl->kind != HK_GLOBAL ||
            !(decl->flags & HF_STATIC_CONST) || !decl->child[0])
            return NULL;
        for (int i = 0; i < ctx->ndecls; ++i) {
            if (ctx->decls[i] == decl) {
                ctx->cycle = decl;
                return NULL;
            }
        }
        ctx->decls[ctx->ndecls++] = decl;
        HNode *expanded = ti_ct_clone(t, owner, decl->child[0], ctx);
        ctx->ndecls--;
        return expanded;
    }

    switch (src->kind) {
    case HK_CONST_INT: case HK_CONST_FLOAT:
    case HK_CONST_STRING: case HK_CONST_CHAR:
        break;
    case HK_BINOP: case HK_CAST:
        if (!src->child[0] ||
            (src->kind == HK_BINOP && !src->child[1])) return NULL;
        break;
    case HK_UNOP:
        if (!src->child[0] || src->u.unop == HUN_DEREF ||
            src->u.unop == HOP_ADDR_OF) return NULL;
        break;
    default:
        return NULL;
    }

    HNode *copy = hir_node(owner, src->kind, src->tok);
    if (!copy) return NULL;
    copy->type = src->type;
    copy->flags = src->flags;
    copy->name = src->name;
    copy->aux = src->aux;
    copy->u = src->u;
    if (src->child[0]) {
        copy->child[0] = ti_ct_clone(t, owner, src->child[0], ctx);
        if (!copy->child[0]) return NULL;
    }
    if (src->child[1]) {
        copy->child[1] = ti_ct_clone(t, owner, src->child[1], ctx);
        if (!copy->child[1]) return NULL;
    }
    tfold_expr(copy);
    return ti_ct_node_is_safe(copy) ? copy : NULL;
}

typedef struct {
    HNode *decl;
} TiStaticDecl;

static int ti_static_decl_cmp(const void *a, const void *b) {
    const TiStaticDecl *x = a, *y = b;
    return strcmp(x->decl->name, y->decl->name);
}

static HNode *ti_static_find(TiStaticDecl *decls, size_t count,
                             const char *name) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(decls[mid].decl->name, name);
        if (cmp < 0) lo = mid + 1;
        else if (cmp > 0) hi = mid;
        else return decls[mid].decl;
    }
    return NULL;
}

static void ti_ct_mark_static_uses(HNode *n, TiStaticDecl *decls,
                                   size_t count) {
    if (!n) return;
    if (n->kind == HK_GLOBAL_REF && n->name) {
        HNode *decl = ti_static_find(decls, count, n->name);
        if (decl) decl->flags |= HF_STATIC_USED;
    }
    for (int i = 0; i < 4; ++i)
        ti_ct_mark_static_uses(n->child[i], decls, count);
    for (int i = 0; i < n->nitems; ++i)
        ti_ct_mark_static_uses(n->items[i], decls, count);
}

static void ti_prune_static_storage(HModule **mods, int nmods) {
    size_t count = 0;
    for (int m = 0; m < nmods; ++m) {
        HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; ++i) {
            HNode *n = root->items[i];
            if (n->kind == HK_GLOBAL && (n->flags & HF_STATIC_CONST) &&
                n->name)
                count++;
        }
    }
    if (!count || count > SIZE_MAX / sizeof(TiStaticDecl)) return;
    TiStaticDecl *decls = malloc(count * sizeof(*decls));
    if (!decls) return; /* Keep storage if the use index cannot be built. */

    size_t at = 0;
    for (int m = 0; m < nmods; ++m) {
        HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; ++i) {
            HNode *n = root->items[i];
            if (n->kind == HK_GLOBAL && (n->flags & HF_STATIC_CONST) &&
                n->name)
                decls[at++].decl = n;
        }
    }
    qsort(decls, count, sizeof(*decls), ti_static_decl_cmp);

    /* One project-wide walk marks every remaining global use. The sorted
     * index makes each reference lookup logarithmic instead of rescanning
     * every module for every static declaration. */
    for (int m = 0; m < nmods; ++m) {
        HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; ++i)
            ti_ct_mark_static_uses(root->items[i], decls, count);
    }

    for (int m = 0; m < nmods; ++m) {
        HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; ++i) {
            HNode *decl = root->items[i];
            if (decl->kind != HK_GLOBAL ||
                !(decl->flags & HF_STATIC_CONST)) continue;
            int removable = !(decl->flags & HF_STATIC_USED) &&
                            ti_ct_is_const_expr(decl->child[0]);
            decl->flags &= ~HF_STATIC_USED;
            if (!removable) continue;
            if (i + 1 < root->nitems)
                memmove(&root->items[i], &root->items[i + 1],
                    (size_t)(root->nitems - i - 1) * sizeof(root->items[0]));
            root->nitems--;
            i--;
        }
    }
    free(decls);
}

static void ti_ct_walk(TI *t, HModule *owner, HNode **slot,
                       int preserve_address) {
    if (!slot || !*slot) return;
    HNode *n = *slot;
    if (n->kind == HK_GLOBAL_REF && !preserve_address) {
        TiCtContext context;
        memset(&context, 0, sizeof(context));
        context.fuel = 8192;
        HNode *expanded = ti_ct_clone(t, owner, n, &context);
        if (expanded) {
            *slot = expanded;
            n = expanded;
        } else if (context.cycle) {
            terr(t, &n->tok, "cyclic static constant reference");
        }
    }
    for (int i = 0; i < 4; ++i) {
        int preserve = preserve_address;
        if ((n->kind == HK_ADDR ||
             (n->kind == HK_UNOP && n->u.unop == HOP_ADDR_OF)) && i == 0)
            preserve = 1;
        ti_ct_walk(t, owner, &n->child[i], preserve);
    }
    for (int i = 0; i < n->nitems; ++i)
        ti_ct_walk(t, owner, &n->items[i], preserve_address);
}

static void ti_expand_static_constants(TI *t, HModule **mods, int nmods) {
    for (int m = 0; m < nmods; ++m) {
        HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; ++i)
            ti_ct_walk(t, mods[m], &root->items[i], 0);
    }
    /* Once every use has been expanded, remove scalar static declarations
     * that have no remaining references. Address-taking and unsupported
     * expressions naturally keep their backing global alive. */
    ti_prune_static_storage(mods, nmods);
}

int hir_tyinfer(HModule **mods, int nmods, Diag *diag) {
    TI t;
    memset(&t, 0, sizeof(t));
    t.diag = diag;
    /* Bind every module-level name of every module first: functions
     * may call each other in any order and across modules (qualified
     * names like std::heap::alloc), so all bindings must exist before
     * any body is typed. */
    for (int k = 0; k < nmods; k++) {
        HNode *root = hir_root(mods[k]);
        for (int i = 0; i < root->nitems; i++)
            if (root->items[i]->name)
                tbind(&t, root->items[i]->name, root->items[i]);
    }
    int errs = diag_error_count(diag);
    for (int k = 0; k < nmods; k++) {
        t.cur = mods[k];
        HNode *root = hir_root(mods[k]);
        for (int i = 0; i < root->nitems; i++) {
            HNode *d = root->items[i];
            if (d->kind == HK_FUNC) tfunc(&t, d);
            else if (d->kind == HK_GLOBAL) {
                if (d->child[0]) {
                    twalk_expr(&t, d->child[0]);
                    if (!d->type) d->type = d->child[0]->type;
                }
                if (!d->type) {
                    terr(&t, &d->tok, "global needs a type or an initializer");
                    d->type = &hty_void;
                }
            }
        }
    }
    if (diag_error_count(diag) == errs)
        ti_expand_static_constants(&t, mods, nmods);
    free(t.binds);
    free(t.slot_ty);
    return diag_error_count(diag) - errs;
}
