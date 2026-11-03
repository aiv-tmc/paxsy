#include "common.h"
#include "hir.h"
#include "opt.h"
#include "opt.h"

/*
 * opt_strength.c - strength reduction over the HIR.
 *
 * Expensive operations with one constant operand are replaced by
 * cheaper equivalents, bottom-up per function:
 *   x * 2^k      -> x << k        (any integer width)
 *   x * 1 / x + 0 / x - 0        -> x
 *   x * 0        -> 0            (only when x is pure)
 *   x / 2^k      -> x >>> k       (unsigned only: negative signed
 *                                  values round differently)
 *   x % 2^k      -> x & (2^k-1)   (unsigned only, same reason)
 *   shifts by 0  -> x
 * Identity rules live here rather than in opt_fold so that folding
 * stays strictly about two-constant operands; the pass manager runs
 * folding first, so by the time strength reduction sees a node the
 * truly constant cases are already gone.
 */

typedef struct {
    HModule *mod;
    long changes;
} StrCtx;

/* Returns 1 when `v` is a positive power of two. */
static int is_pow2(long long v) {
    return v > 0 && (v & (v - 1)) == 0;
}

/* Position of the lowest set bit: the shift count replacing a
 * multiplication or division by a power of two. */
static int pow2_log(long long v) {
    int n = 0;
    while (v >>= 1) n++;
    return n;
}

/* Builds an integer constant node typed like `t`. */
static HNode *str_const(StrCtx *ctx, HNode *ref, long long v) {
    HNode *k = hir_node(ctx->mod, HK_CONST_INT, ref->tok);
    k->u.ival = v;
    k->type = ref->type;
    return k;
}

/* Points the slot of `parent` at `repl`; `n` stays in the arena. */
static void str_replace(StrCtx *ctx, HNode *parent, HNode *n, HNode *repl) {
    if (!parent || parent == n) return;
    opt_rewire(parent, n, repl);
    ctx->changes++;
}

/* Multiplication rules: the constant may sit on either side. */
static void str_mul(StrCtx *ctx, HNode *parent, HNode *n) {
    HNode *c = n->child[0], *x = n->child[1];
    int cleft = c && c->kind == HK_CONST_INT;
    if (!cleft) {                 /* try the other side            */
        c = n->child[1]; x = n->child[0];
        cleft = c && c->kind == HK_CONST_INT;
    }
    if (!cleft) return;
    long long v = c->u.ival;
    if (v == 1) {
        str_replace(ctx, parent, n, x);
    } else if (v == 0 && opt_is_pure(x)) {
        str_replace(ctx, parent, n, str_const(ctx, n, 0));
    } else if (is_pow2(v)) {
        HNode *sh = hir_node(ctx->mod, HK_BINOP, n->tok);
        sh->u.binop = HOP_SHL;
        sh->type = n->type;
        sh->child[0] = x;
        sh->child[1] = str_const(ctx, n, pow2_log(v));
        str_replace(ctx, parent, n, sh);
    }
}

/* Division and remainder rules: the constant must be the divisor
 * (child[1]); signed integers are skipped because arithmetic shift of
 * negative values does not match division rounding. */
static void str_divmod(StrCtx *ctx, HNode *parent, HNode *n) {
    HNode *c = n->child[1];
    if (!c || c->kind != HK_CONST_INT) return;
    if (!n->type || n->type->kind != HTY_UINT) return;
    long long v = c->u.ival;
    if (n->u.binop == HOP_DIV) {
        if (v == 1) {
            str_replace(ctx, parent, n, n->child[0]);
        } else if (is_pow2(v)) {
            HNode *sh = hir_node(ctx->mod, HK_BINOP, n->tok);
            sh->u.binop = HOP_USHR;
            sh->type = n->type;
            sh->child[0] = n->child[0];
            sh->child[1] = str_const(ctx, n, pow2_log(v));
            str_replace(ctx, parent, n, sh);
        }
    } else if (n->u.binop == HOP_MOD && is_pow2(v)) {
        HNode *and = hir_node(ctx->mod, HK_BINOP, n->tok);
        and->u.binop = HOP_AND;
        and->type = n->type;
        and->child[0] = n->child[0];
        and->child[1] = str_const(ctx, n, v - 1);
        str_replace(ctx, parent, n, and);
    }
}

/* Add/subtract/and/or/xor identities and zero-count shifts. */
static void str_identity(StrCtx *ctx, HNode *parent, HNode *n) {
    HNode *c = n->child[1], *x = n->child[0];
    if (!c || c->kind != HK_CONST_INT) {
        /* add/or/xor are commutative: retry on the left operand */
        if (n->u.binop != HOP_ADD && n->u.binop != HOP_OR &&
            n->u.binop != HOP_XOR) return;
        c = n->child[0]; x = n->child[1];
        if (!c || c->kind != HK_CONST_INT) return;
    }
    long long v = c->u.ival;
    int is_addsub = n->u.binop == HOP_ADD || n->u.binop == HOP_SUB;
    int is_orxor  = n->u.binop == HOP_OR  || n->u.binop == HOP_XOR;
    int is_shift  = n->u.binop == HOP_SHL || n->u.binop == HOP_SHR ||
                    n->u.binop == HOP_USHR;
    if (((is_addsub || is_orxor) && v == 0) || (is_shift && v == 0)) {
        str_replace(ctx, parent, n, x);
        return;
    }
    if (n->u.binop == HOP_AND && v == 0 && opt_is_pure(x)) {
        str_replace(ctx, parent, n, str_const(ctx, n, 0));
    }
}

/* Rewrites the subtree rooted at `n` in place. */
static void str_node(StrCtx *ctx, HNode *parent, HNode *n) {
    if (!n) return;
    for (int i = 0; i < 4; i++) str_node(ctx, n, n->child[i]);
    for (int i = 0; i < n->nitems; i++) str_node(ctx, n, n->items[i]);
    if (n->kind != HK_BINOP) return;
    switch (n->u.binop) {
    case HOP_MUL:  str_mul(ctx, parent, n); break;
    case HOP_DIV:
    case HOP_MOD:  str_divmod(ctx, parent, n); break;
    case HOP_ADD:
    case HOP_SUB:
    case HOP_AND:
    case HOP_OR:
    case HOP_XOR:
    case HOP_SHL:
    case HOP_SHR:
    case HOP_USHR: str_identity(ctx, parent, n); break;
    default: break;
    }
}

long opt_strength(HModule *m) {
    HNode *root = hir_root(m);
    if (!root) return 0;
    StrCtx ctx = { m, 0 };
    for (int i = 0; i < root->nitems; i++) {
        HNode *tl = root->items[i];
        if (!tl || tl->kind != HK_FUNC) continue;
        for (int c = 0; c < 4; c++) str_node(&ctx, tl, tl->child[c]);
        for (int c = 0; c < tl->nitems; c++) str_node(&ctx, tl, tl->items[c]);
    }
    return ctx.changes;
}
