#include "common.h"
#include "hir.h"
#include "opt.h"

/*
 * opt_prop.c - constant and copy propagation over locals.
 *
 * The pass tracks the value bound to every local slot inside a
 * straight-line run of a block: a `let` that binds a constant (or a
 * reference the constant folder understands) records the binding,
 * and every later read of the same slot is replaced with the bound
 * node. Once a slot is re-bound or the straight-line run ends at a
 * control-flow statement (if, while, match), its entry is dropped, so
 * no read can observe a stale binding across a join point.
 *
 * Propagated constants feed opt_fold on the next pipeline round (a
 * branch condition becomes a literal) and the now-dead `let` is
 * removed by opt_dce: the three passes compose into a much stronger
 * simplifier than any of them alone.
 */

typedef struct {
    const HNode *val[64];   /* binding per local slot; NULL = unknown */
    long changes;
} PropCtx;

static void prop_subtree(PropCtx *ctx, HNode *n);

/* A binding is safe to propagate when replacing a read with it cannot
 * change program behaviour: constants and pure references. */
static int prop_is_copyable(const HNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case HK_CONST_INT:
    case HK_CONST_FLOAT:
    case HK_CONST_STRING:
    case HK_CONST_CHAR:
    case HK_GLOBAL_REF:
    case HK_PARAM:
        return 1;
    default:
        return 0;
    }
}

/* Constant-like nodes fold further: literals and operations over
 * literals. */
static int prop_is_const(const HNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case HK_CONST_INT:
    case HK_CONST_FLOAT:
    case HK_CONST_STRING:
    case HK_CONST_CHAR:
        return 1;
    case HK_BINOP:
    case HK_UNOP:
    case HK_CAST:
        return prop_is_const(n->child[0]) && prop_is_const(n->child[1]);
    default:
        return 0;
    }
}

/* Drops every binding: at a control-flow join nothing read after it
 * may reuse a binding from before the branches merged. */
static void prop_invalidate(PropCtx *ctx) {
    for (int i = 0; i < 64; i++) ctx->val[i] = NULL;
}

/* Rewrites one node to its current binding. */
static HNode *prop_value(PropCtx *ctx, HNode *n) {
    if (!n) return NULL;
    if (n->kind == HK_LOCAL && n->u.index < 64) {
        const HNode *v = ctx->val[n->u.index];
        if (v) return (HNode *)v;
    }
    return n;
}

/* Processes one statement: records fresh bindings from `let` nodes
 * and rewrites reads in every expression below the statement.
 * Control-flow statements invalidate the map on exit. */
static void prop_stmt(PropCtx *ctx, HNode *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
    case HK_LET: {
        HNode *value = stmt->child[0];
        int slot = stmt->u.index;
        if (slot >= 0 && slot < 64) {
            if (prop_is_const(value) || prop_is_copyable(value))
                ctx->val[slot] = value;
            else
                ctx->val[slot] = NULL;
        }
        prop_subtree(ctx, value);
        return;
    }
    case HK_IF:
    case HK_WHILE:
    case HK_MATCH:
    case HK_RETURN:
        /* Recurse into the parts, then drop all bindings: the join
         * point merges states the tracker cannot prove equal. */
        for (int i = 0; i < 4; i++)
            prop_subtree(ctx, stmt->child[i]);
        for (int i = 0; i < stmt->nitems; i++)
            prop_subtree(ctx, stmt->items[i]);
        prop_invalidate(ctx);
        return;
    default:
        break;
    }
    prop_subtree(ctx, stmt);
}

/* Rewrites all LOCAL reads in the subtree to their current
 * bindings. */
static void prop_subtree(PropCtx *ctx, HNode *n) {
    if (!n) return;
    for (int i = 0; i < 4; i++) {
        HNode *c = n->child[i];
        if (c && c->kind == HK_LOCAL) {
            HNode *v = prop_value(ctx, c);
            if (v != c) { n->child[i] = v; ctx->changes++; }
        } else {
            prop_subtree(ctx, c);
        }
    }
    for (int i = 0; i < n->nitems; i++) {
        HNode *c = n->items[i];
        if (c && c->kind == HK_LOCAL) {
            HNode *v = prop_value(ctx, c);
            if (v != c) { n->items[i] = v; ctx->changes++; }
        } else {
            prop_subtree(ctx, c);
        }
    }
}

/* Walks one function body: the body's items form the top-level
 * straight-line run; inner `let` slots are distinct (the local
 * counter is function-wide), so nested blocks share the map safely. */
static void prop_func(HNode *fn, PropCtx *ctx) {
    HNode *body = fn->child[3];
    if (!body) return;
    prop_invalidate(ctx);
    for (int i = 0; i < body->nitems; i++)
        prop_stmt(ctx, body->items[i]);
}

long opt_prop(HModule *m) {
    if (!m) return 0;
    PropCtx ctx;
    prop_invalidate(&ctx);
    ctx.changes = 0;
    HNode *root = hir_root(m);
    for (int i = 0; i < root->nitems; i++)
        if (root->items[i] && root->items[i]->kind == HK_FUNC)
            prop_func(root->items[i], &ctx);
    return ctx.changes;
}
