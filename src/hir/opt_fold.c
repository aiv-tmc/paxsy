#include "common.h"
#include "hir.h"
#include "opt.h"
#include "opt.h"

/*
 * opt_fold.c - constant folding over the HIR.
 *
 * Three rewrites are performed bottom-up on every function:
 *   1. arithmetic, bitwise, shift and comparison operators with two
 *      constant integer operands become a single constant;
 *   2. unary operators on a constant integer become a constant;
 *   3. a control statement with a known constant condition collapses
 *      into the reachable arm (if) or into an empty block (while and
 *      do-while with a false condition).
 * Value-position conditionals (HF_EXPR) are left untouched because
 * both arms produce a value and folding would require a select node.
 * Division and remainder by zero are never folded: folding them would
 * delete a runtime trap the language semantics preserve. Shift counts
 * outside 0..63 are left as-is for the same reason.
 */

typedef struct {
    HModule *mod;
    long changes;
} FoldCtx;

/* Truncates a folded constant to the width carried by `t`; types
 * without an integer width keep the full 64-bit value. */
static unsigned long long fold_mask(const HType *t) {
    if (!t || (t->kind != HTY_INT && t->kind != HTY_UINT)) return ~0ULL;
    if (t->width <= 0 || t->width >= 64) return ~0ULL;
    return (1ULL << t->width) - 1ULL;
}

/* Stores `v` as the result of `n`, turning it into a constant node. */
static void fold_to_const(HNode *n, long long v) {
    n->kind = HK_CONST_INT;
    n->u.ival = v & (long long)fold_mask(n->type);
    n->child[0] = NULL;
    n->child[1] = NULL;
}

/* Attempts rule 1 on `n`; returns 1 when the node was folded. */
static int fold_binop_ints(HNode *n) {
    HNode *a = n->child[0], *b = n->child[1];
    if (!a || !b || a->kind != HK_CONST_INT || b->kind != HK_CONST_INT)
        return 0;
    long long x = a->u.ival, y = b->u.ival, r = 0;
    switch (n->u.binop) {
    case HOP_ADD:  r = x + y; break;
    case HOP_SUB:  r = x - y; break;
    case HOP_MUL:  r = x * y; break;
    case HOP_DIV:
    case HOP_MOD:
        if (y == 0) return 0;          /* keep the runtime trap    */
        if (n->u.binop == HOP_DIV) r = x / y;
        else                       r = x % y;
        break;
    case HOP_AND:  r = x & y; break;
    case HOP_OR:   r = x | y; break;
    case HOP_XOR:  r = x ^ y; break;
    case HOP_SHL:
    case HOP_SHR:
    case HOP_USHR:
        if (y < 0 || y > 63) return 0; /* keep the runtime trap    */
        if (n->u.binop == HOP_SHL)      r = x << y;
        else if (n->u.binop == HOP_SHR) r = x >> y;
        else r = (long long)((unsigned long long)x >> y);
        break;
    case HOP_EQ:   r = x == y; break;
    case HOP_NE:   r = x != y; break;
    case HOP_LT:   r = x < y; break;
    case HOP_LE:   r = x <= y; break;
    case HOP_GT:   r = x > y; break;
    case HOP_GE:   r = x >= y; break;
    case HOP_LAND: r = x && y; break;
    case HOP_LOR:  r = x || y; break;
    default: return 0;
    }
    fold_to_const(n, r);
    return 1;
}

/* Attempts rule 2 on `n`; returns 1 when the node was folded. */
static int fold_unop_int(HNode *n) {
    HNode *a = n->child[0];
    if (!a || a->kind != HK_CONST_INT) return 0;
    long long x = a->u.ival, r;
    switch (n->u.unop) {
    case HUN_NEG:    r = -x; break;
    case HUN_BITNOT: r = ~x; break;
    case HUN_LNOT:   r = !x; break;
    default: return 0;
    }
    fold_to_const(n, r);
    return 1;
}

/* Points the slot of `parent` at `repl` (or detaches `n` when `repl`
 * is NULL) so a structural replacement stays visible to the owner. */
static void fold_replace(FoldCtx *ctx, HNode *parent, HNode *n, HNode *repl) {
    if (!parent || parent == n) return;
    if (repl) opt_rewire(parent, n, repl);
    else      opt_detach(parent, n);
    ctx->changes++;
}

/* The condition slot of a control node: while keeps it in child[0],
 * do-while in child[1] (its body is child[0]). */
static HNode *fold_cond(const HNode *n) {
    return n->kind == HK_DOWHILE ? n->child[1] : n->child[0];
}

/* Compacts an item array in place after elements were detached. */
static void fold_compact(HNode *n) {
    if (!n->items) return;
    int w = 0;
    for (int i = 0; i < n->nitems; i++)
        if (n->items[i]) n->items[w++] = n->items[i];
    n->nitems = w;
}

/* Rewrites the subtree rooted at `n` in place; `parent` receives any
 * structural replacement of `n` itself. */
static void fold_node(FoldCtx *ctx, HNode *parent, HNode *n) {
    if (!n) return;
    /* Recurse first so inner folds expose outer ones. */
    for (int i = 0; i < 4; i++) fold_node(ctx, n, n->child[i]);
    for (int i = 0; i < n->nitems; i++) fold_node(ctx, n, n->items[i]);

    switch (n->kind) {
    case HK_BINOP:
        if (fold_binop_ints(n)) ctx->changes++;
        break;
    case HK_UNOP:
        if (fold_unop_int(n)) ctx->changes++;
        break;
    case HK_IF: {
        HNode *cond = fold_cond(n);
        /* Value-producing ifs need both arms; statement ifs with a
         * known condition collapse into the reachable arm. */
        if (!(n->flags & HF_EXPR) && cond &&
            cond->kind == HK_CONST_INT) {
            HNode *arm = cond->u.ival ? n->child[1] : n->child[2];
            fold_replace(ctx, parent, n, arm ? arm : NULL);
        }
        break;
    }
    case HK_WHILE:
    case HK_DOWHILE: {
        HNode *cond = fold_cond(n);
        /* A loop with a constant false condition never runs; an
         * empty block preserves the statement slot without changing
         * the surrounding control flow. */
        if (cond && cond->kind == HK_CONST_INT && cond->u.ival == 0) {
            HNode *empty = hir_node(ctx->mod, HK_BLOCK, n->tok);
            fold_replace(ctx, parent, n, empty);
        }
        break;
    }
    default:
        break;
    }

    fold_compact(n);
}

long opt_fold(HModule *m) {
    HNode *root = hir_root(m);
    if (!root) return 0;
    FoldCtx ctx = { m, 0 };
    /* Fold each top-level function independently, walking its
     * children without assuming a particular body slot. */
    for (int i = 0; i < root->nitems; i++) {
        HNode *tl = root->items[i];
        if (!tl || tl->kind != HK_FUNC) continue;
        for (int c = 0; c < 4; c++) fold_node(&ctx, tl, tl->child[c]);
        for (int c = 0; c < tl->nitems; c++) fold_node(&ctx, tl, tl->items[c]);
    }
    return ctx.changes;
}
