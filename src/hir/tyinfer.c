#include "common.h"
#include "tyinfer.h"
#include "tyinfer_int.h"

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
    TBind *binds;          /* module-level functions/globals by name */
    int nbinds, capbinds;
    const HType **slot_ty;    /* slot index -> type, per function    */
    int nslots, capslots;
} TI;

static void terr(TI *t, const Token *tok, const char *what) {
    diag_report(t->diag, DK_ERROR, "<tyinfer>", tok->line, tok->col,
                "%s", what);
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

static const HNode *tfind(TI *t, const char *name) {
    for (int i = 0; i < t->nbinds; i++)
        if (strcmp(t->binds[i].name, name) == 0) return t->binds[i].decl;
    return NULL;
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
static const HType *tfield(TI *t, HNode *f) {
    const HType *ot = f->child[0] ? f->child[0]->type : NULL;
    if (ot && ot->kind == HTY_PTR) ot = ot->u.ptr.elem;
    if (!ot || ot->kind != HTY_NAMED) return NULL;
    const HNode *s = tfind(t, ot->u.named.name);
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
    case HK_CONST_INT:    n->type = tint_type(n->u.ival); break;
    case HK_CONST_FLOAT:  n->type = hty_real(64); break;
    case HK_CONST_STRING: case HK_FMT:
        n->type = hty_ptr(&hty_char); break;
    case HK_CONST_CHAR:   n->type = &hty_char; break;
    case HK_PARAM: case HK_LOCAL:
        n->type = tslot(t, n->u.index); break;
    case HK_GLOBAL_REF: {
        const HNode *d = tfind(t, n->name);
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
        default:
            n->type = twider(lt, rt); break;
        }
        tfold_expr(n);
        break;
    }
    case HK_UNOP: {
        twalk(t, n->child[0]);
        const HType *ot = n->child[0] ? n->child[0]->type : NULL;
        n->type = n->u.unop == HUN_LNOT ? &hty_bool :
                  n->u.unop == HUN_DEREF && ot && ot->kind == HTY_PTR
                      ? ot->u.ptr.elem : ot;
        tfold_expr(n);
        break;
    }
    case HK_CALL: {
        twalk(t, n->child[0]);
        for (int i = 0; i < n->nitems; i++) twalk(t, n->items[i]);
        /* a function reference carries the return type; T!
         * unwraps to the payload, anything else stays external */
        const HType *ct = n->child[0]->type;
        if (ct && ct->kind == HTY_ERROR) n->type = ct->u.error.value;
        else if (ct) n->type = ct;
        else n->flags |= HF_UNKNOWN_CALL;
        break;
    }
    case HK_CAST:    twalk(t, n->child[0]); break;  /* type preset  */
    case HK_INDEX: {
        twalk(t, n->child[0]);
        twalk(t, n->child[1]);
        const HType *bt = n->child[0] ? n->child[0]->type : NULL;
        n->type = bt && bt->kind == HTY_ARRAY ? bt->u.array.elem :
                  bt && bt->kind == HTY_PTR  ? bt->u.ptr.elem : NULL;
        break;
    }
    case HK_FIELD:
        twalk(t, n->child[0]);
        n->type = tfield(t, n);
        break;
    case HK_STRUCT_LIT:
        for (int i = 0; i < n->nitems; i++) twalk(t, n->items[i]);
        n->type = hty_named(n->name);
        break;
    case HK_ARRAY_LIT: {
        const HType *et = NULL;
        for (int i = 0; i < n->nitems; i++) {
            twalk(t, n->items[i]);
            et = twider(et, n->items[i]->type);
        }
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
    case HK_CATCH:
        twalk(t, n->child[0]);
        twalk(t, n->child[1]);
        n->type = n->child[1] ? n->child[1]->type : NULL;
        break;
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
        n->type = n->nitems > 0 &&
                  n->items[n->nitems - 1]->kind == HK_EXPR_STMT
            ? n->items[n->nitems - 1]->child[0]->type : &hty_void;
        break;
    }
    case HK_LET: {
        /* the initializer lives in child[1] */
        if (n->child[1]) {
            twalk(t, n->child[1]);
            /* an annotated array let retypes its literal so the
             * element width follows the annotation, not the
             * smallest-literal-width rule */
            if (n->type && n->type->kind == HTY_ARRAY &&
                n->child[1]->kind == HK_ARRAY_LIT)
                n->child[1]->type = n->type;
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
    case HK_RETURN:
        if (n->child[0]) twalk_expr(t, n->child[0]);
        n->type = &hty_void;
        break;
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
    for (int i = 0; i < f->nitems; i++)
        tset(t, f->items[i]->u.index, f->items[i]->type);
    if (f->child[3]) twalk(t, f->child[3]);
}

int hir_tyinfer(HModule *m, Diag *diag) {
    TI t;
    memset(&t, 0, sizeof(t));
    t.diag = diag;
    HNode *root = hir_root(m);
    /* Bind every module-level name first: functions may call each
     * other in any order, so all bindings must exist before any body
     * is typed. */
    for (int i = 0; i < root->nitems; i++)
        if (root->items[i]->name)
            tbind(&t, root->items[i]->name, root->items[i]);
    int errs = diag_error_count(diag);
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
    free(t.binds);
    free(t.slot_ty);
    return diag_error_count(diag) - errs;
}
