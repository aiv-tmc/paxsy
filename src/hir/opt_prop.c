#include "common.h"
#include "hir.h"
#include "opt.h"
#include <limits.h>

typedef struct {
    const HNode **val;
    unsigned char *alias_safe;
    int nslots;
    long changes;
} PropCtx;

static void prop_subtree(PropCtx *ctx, HNode *n);
static void prop_stmt(PropCtx *ctx, HNode *stmt);

static int prop_aliasable_scalar(const HNode *n) {
    if (!n || !n->type) return 0;
    /* Real32 assignments round to binary32. Replacing a local read
     * with its original binary64 expression would erase that rounding
     * point, so keep these locals out of copy propagation. */
    if (n->type->kind == HTY_REAL && n->type->width == 32)
        return 0;
    switch (n->type->kind) {
    case HTY_BOOL:
    case HTY_CHAR:
    case HTY_INT:
    case HTY_UINT:
    case HTY_REAL:
    case HTY_PTR:
        return 1;
    default:
        return 0;
    }
}

static int prop_is_copyable(const PropCtx *ctx, const HNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case HK_CONST_INT:
    case HK_CONST_FLOAT:
    case HK_CONST_STRING:
    case HK_CONST_CHAR:
        return 1;
    case HK_PARAM:
    case HK_LOCAL:
        return prop_aliasable_scalar(n) && n->u.index >= 0 &&
               n->u.index < ctx->nslots &&
               ctx->alias_safe[n->u.index];
    default:
        return 0;
    }
}

static int prop_is_const(const HNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case HK_CONST_INT:
    case HK_CONST_FLOAT:
    case HK_CONST_STRING:
    case HK_CONST_CHAR:
        return 1;
    case HK_BINOP: {
        const HNode *a = n->child[0], *b = n->child[1];
        if (!prop_is_const(a) || !prop_is_const(b)) return 0;
        if (b && b->kind == HK_CONST_INT) {
            if ((n->u.binop == HOP_DIV || n->u.binop == HOP_MOD) &&
                b->u.ival == 0) return 0;
            if ((n->u.binop == HOP_SHL || n->u.binop == HOP_SHR ||
                 n->u.binop == HOP_USHR) &&
                (b->u.ival < 0 || b->u.ival > 63)) return 0;
            if ((n->u.binop == HOP_DIV || n->u.binop == HOP_MOD) &&
                a && a->kind == HK_CONST_INT &&
                a->u.ival == LLONG_MIN && b->u.ival == -1 &&
                (!a->type || a->type->kind != HTY_UINT)) return 0;
        }
        return 1;
    }
    case HK_UNOP:
        return n->u.unop != HOP_ADDR_OF && n->u.unop != HUN_DEREF &&
               prop_is_const(n->child[0]);
    case HK_CAST:
        return prop_is_const(n->child[0]);
    default:
        return 0;
    }
}

static void prop_find_slots(const HNode *n, int *max_slot) {
    if (!n) return;
    if ((n->kind == HK_LOCAL || n->kind == HK_PARAM ||
         n->kind == HK_LET || n->kind == HK_FOR) &&
        n->u.index >= 0 && n->u.index > *max_slot)
        *max_slot = n->u.index;
    for (int i = 0; i < 4; i++)
        prop_find_slots(n->child[i], max_slot);
    for (int i = 0; i < n->nitems; i++)
        prop_find_slots(n->items[i], max_slot);
}

static void prop_invalidate(PropCtx *ctx) {
    if (ctx->val)
        memset(ctx->val, 0, (size_t)ctx->nslots * sizeof(*ctx->val));
}

static HNode *prop_value(PropCtx *ctx, HNode *n) {
    for (int depth = 0; n && n->kind == HK_LOCAL && depth < ctx->nslots;
         ++depth) {
        int slot = n->u.index;
        if (slot < 0 || slot >= ctx->nslots || !ctx->val[slot])
            break;
        n = (HNode *)ctx->val[slot];
    }
    return n;
}

static void prop_lvalue(PropCtx *ctx, HNode *n) {
    if (!n || n->kind == HK_LOCAL) return;
    if (n->kind == HK_ADDR ||
        (n->kind == HK_UNOP && n->u.unop == HOP_ADDR_OF)) {
        prop_lvalue(ctx, n->child[0]);
        return;
    }
    if (n->kind == HK_DEREF ||
        (n->kind == HK_UNOP && n->u.unop == HUN_DEREF)) {
        prop_subtree(ctx, n->child[0]);
        return;
    }
    for (int i = 0; i < 4; i++) {
        HNode *c = n->child[i];
        if (!c) continue;
        if (c->kind == HK_LOCAL) {
            HNode *v = prop_value(ctx, c);
            if (v != c) {
                n->child[i] = v;
                ctx->changes++;
            }
        } else {
            prop_subtree(ctx, c);
        }
    }
    for (int i = 0; i < n->nitems; i++) {
        HNode *c = n->items[i];
        if (!c) continue;
        if (c->kind == HK_LOCAL) {
            HNode *v = prop_value(ctx, c);
            if (v != c) {
                n->items[i] = v;
                ctx->changes++;
            }
        } else {
            prop_subtree(ctx, c);
        }
    }
}

/* A structured control-flow join is the tree-HIR equivalent of a
 * trivial SSA phi: a value survives only when every incoming edge carries
 * the same immutable value. Different values are conservatively unknown. */
static int prop_same_value(const HNode *a, const HNode *b) {
    if (a == b) return 1;
    if (!a || !b || a->kind != b->kind || a->type != b->type) return 0;
    switch (a->kind) {
    case HK_CONST_INT: return a->u.ival == b->u.ival;
    case HK_CONST_FLOAT:
        return memcmp(&a->u.fval, &b->u.fval, sizeof(a->u.fval)) == 0;
    case HK_CONST_CHAR:
    case HK_CONST_STRING:
        return a->u.slice.len == b->u.slice.len &&
               a->u.slice.start && b->u.slice.start &&
               memcmp(a->u.slice.start, b->u.slice.start,
                      (size_t)a->u.slice.len) == 0;
    default: return 0;
    }
}

static void prop_join(PropCtx *ctx, const HNode **out,
                      const HNode **incoming) {
    for (int i = 0; i < ctx->nslots; ++i)
        out[i] = prop_same_value(out[i], incoming[i]) ? out[i] : NULL;
}

/* Collect every local whose value may change while a loop executes.
 * Address-taken locals are also killed across calls; ordinary locals are
 * not externally mutable because their addresses never escape. */
static void prop_loop_writes(const PropCtx *ctx, const HNode *n,
                             unsigned char *writes) {
    if (!n) return;
    if (n->kind == HK_LET || n->kind == HK_FOR) {
        int slot = n->u.index;
        if (slot >= 0 && slot < ctx->nslots) writes[slot] = 1;
    }
    if (n->kind == HK_STORE) {
        const HNode *lhs = n->child[0];
        if (lhs && lhs->kind == HK_LOCAL) {
            int slot = lhs->u.index;
            if (slot >= 0 && slot < ctx->nslots) writes[slot] = 1;
        } else {
            /* An indirect/global store may alias any local whose address
             * escaped. Conservatively kill all facts if its target is not
             * a plain local slot. */
            memset(writes, 1, (size_t)ctx->nslots);
        }
    }
    if (n->kind == HK_CALL) {
        for (int i = 0; i < ctx->nslots; ++i)
            if (!ctx->alias_safe[i]) writes[i] = 1;
    }
    if (n->kind == HK_INLINE_ASM || n->kind == HK_ASYNC ||
        n->kind == HK_GO_ROUTINE || n->kind == HK_SELECT) {
        memset(writes, 1, (size_t)ctx->nslots);
    }
    for (int i = 0; i < 4; ++i)
        prop_loop_writes(ctx, n->child[i], writes);
    for (int i = 0; i < n->nitems; ++i)
        prop_loop_writes(ctx, n->items[i], writes);
}

static void prop_kill_writes(const PropCtx *ctx, const unsigned char *writes,
                             const HNode **values) {
    for (int i = 0; i < ctx->nslots; ++i)
        if (writes[i]) values[i] = NULL;
}

/* Loop-aware propagation keeps facts for locals untouched by every loop
 * path. Modified locals are unknown at the header and after the loop, so
 * values are never frozen across iterations. This is a conservative
 * loop-carried data-flow step; it does not yet materialize SSA phi nodes. */
static void prop_loop(PropCtx *ctx, HNode *stmt) {
    if (ctx->nslots <= 0) {
        for (int i = 0; i < 4; ++i) prop_subtree(ctx, stmt->child[i]);
        for (int i = 0; i < stmt->nitems; ++i) prop_subtree(ctx, stmt->items[i]);
        return;
    }

    const HNode **entry = malloc((size_t)ctx->nslots * sizeof(*entry));
    const HNode **after_test = malloc((size_t)ctx->nslots * sizeof(*after_test));
    unsigned char *writes = calloc((size_t)ctx->nslots, 1);
    if (!entry || !after_test || !writes) {
        free(entry); free(after_test); free(writes);
        prop_invalidate(ctx);
        for (int i = 0; i < 4; ++i) prop_subtree(ctx, stmt->child[i]);
        for (int i = 0; i < stmt->nitems; ++i) prop_subtree(ctx, stmt->items[i]);
        prop_invalidate(ctx);
        return;
    }

    HNode *body = NULL;
    HNode *condition = NULL;
    if (stmt->kind == HK_WHILE) {
        condition = stmt->child[0];
        body = stmt->child[1];
    } else if (stmt->kind == HK_DOWHILE) {
        body = stmt->child[0];
        condition = stmt->child[1];
    } else {
        /* The range expressions are evaluated once before entering for. */
        prop_subtree(ctx, stmt->child[0]);
        prop_subtree(ctx, stmt->child[1]);
        body = stmt->child[2];
    }

    memcpy(entry, ctx->val, (size_t)ctx->nslots * sizeof(*entry));
    prop_loop_writes(ctx, body, writes);
    if (stmt->kind == HK_FOR && stmt->u.index >= 0 &&
        stmt->u.index < ctx->nslots)
        writes[stmt->u.index] = 1;

    if (stmt->kind == HK_WHILE) {
        /* The condition is re-evaluated on the backedge. A value assigned
         * in the body is therefore unknown even on the first test: folding
         * the initial constant test could turn a terminating loop infinite. */
        prop_kill_writes(ctx, writes, ctx->val);
        prop_subtree(ctx, condition);
    }

    if (stmt->kind == HK_DOWHILE) {
        /* A do-while body executes at least once. Its condition sees the
         * post-body state, but no value written in the body is invariant. */
        memcpy(ctx->val, entry, (size_t)ctx->nslots * sizeof(*entry));
        prop_kill_writes(ctx, writes, ctx->val);
        prop_subtree(ctx, body);
        prop_kill_writes(ctx, writes, ctx->val);
        prop_subtree(ctx, condition);
        memcpy(after_test, ctx->val,
               (size_t)ctx->nslots * sizeof(*after_test));
    } else {
        memcpy(after_test, ctx->val,
               (size_t)ctx->nslots * sizeof(*after_test));
        memcpy(ctx->val, after_test,
               (size_t)ctx->nslots * sizeof(*after_test));
        prop_kill_writes(ctx, writes, ctx->val);
        prop_subtree(ctx, body);
    }

    /* The loop may execute zero, one or many times. Only facts not written
     * by any path can safely flow out of its exit. */
    memcpy(ctx->val, after_test,
           (size_t)ctx->nslots * sizeof(*after_test));
    prop_kill_writes(ctx, writes, ctx->val);
    free(entry);
    free(after_test);
    free(writes);
}

static void prop_stmt(PropCtx *ctx, HNode *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
    case HK_BLOCK:
        for (int i = 0; i < stmt->nitems; i++)
            prop_stmt(ctx, stmt->items[i]);
        return;
    case HK_LET: {
        HNode *value = stmt->child[1];
        prop_subtree(ctx, value);
        int slot = stmt->u.index;
        if (slot >= 0 && slot < ctx->nslots)
            ctx->val[slot] = (prop_is_const(value) ||
                              prop_is_copyable(ctx, value)) ? value : NULL;
        return;
    }
    case HK_STORE: {
        HNode *lhs = stmt->child[0];
        prop_subtree(ctx, stmt->child[1]);
        if (lhs && lhs->kind == HK_LOCAL) {
            int slot = lhs->u.index;
            if (slot >= 0 && slot < ctx->nslots)
                ctx->val[slot] = (prop_is_const(stmt->child[1]) ||
                                  prop_is_copyable(ctx, stmt->child[1]))
                                     ? stmt->child[1] : NULL;
        } else {
            prop_lvalue(ctx, lhs);
            prop_invalidate(ctx);
        }
        return;
    }
    case HK_IF: {
        prop_subtree(ctx, stmt->child[0]);
        const HNode **before = NULL, **joined = NULL;
        if (ctx->nslots > 0) {
            before = malloc((size_t)ctx->nslots * sizeof(*before));
            joined = malloc((size_t)ctx->nslots * sizeof(*joined));
            if (before && joined)
                memcpy(before, ctx->val, (size_t)ctx->nslots * sizeof(*before));
        }
        if (ctx->nslots > 0 && (!before || !joined)) {
            free(before); free(joined); prop_invalidate(ctx);
            prop_subtree(ctx, stmt->child[1]);
            prop_invalidate(ctx); prop_subtree(ctx, stmt->child[2]);
            prop_invalidate(ctx); return;
        }
        prop_subtree(ctx, stmt->child[1]);
        if (ctx->nslots > 0)
            memcpy(joined, ctx->val, (size_t)ctx->nslots * sizeof(*joined));
        if (before) memcpy(ctx->val, before,
                           (size_t)ctx->nslots * sizeof(*before));
        prop_subtree(ctx, stmt->child[2]);
        if (joined && stmt->child[2])
            prop_join(ctx, joined, ctx->val);
        else if (joined)
            prop_join(ctx, joined, before); /* implicit false edge */
        if (joined) memcpy(ctx->val, joined,
                           (size_t)ctx->nslots * sizeof(*joined));
        free(before); free(joined);
        return;
    }
    case HK_MATCH: {
        prop_subtree(ctx, stmt->child[0]);
        const HNode **before = NULL, **joined = NULL;
        if (ctx->nslots > 0) {
            before = malloc((size_t)ctx->nslots * sizeof(*before));
            joined = malloc((size_t)ctx->nslots * sizeof(*joined));
            if (before && joined)
                memcpy(before, ctx->val, (size_t)ctx->nslots * sizeof(*before));
        }
        if (ctx->nslots > 0 && (!before || !joined)) {
            free(before); free(joined); prop_invalidate(ctx);
            for (int i = 0; i < stmt->nitems; ++i) prop_subtree(ctx, stmt->items[i]);
            prop_invalidate(ctx); prop_subtree(ctx, stmt->child[2]);
            prop_invalidate(ctx); return;
        }
        int have_edge = 0;
        for (int i = 0; i < stmt->nitems; ++i) {
            if (before) memcpy(ctx->val, before,
                               (size_t)ctx->nslots * sizeof(*before));
            prop_subtree(ctx, stmt->items[i]);
            if (!have_edge && joined) {
                memcpy(joined, ctx->val, (size_t)ctx->nslots * sizeof(*joined));
                have_edge = 1;
            } else if (joined) prop_join(ctx, joined, ctx->val);
        }
        if (before) memcpy(ctx->val, before,
                           (size_t)ctx->nslots * sizeof(*before));
        prop_subtree(ctx, stmt->child[2]);
        if (stmt->child[2]) {
            if (!have_edge && joined) {
                memcpy(joined, ctx->val, (size_t)ctx->nslots * sizeof(*joined));
                have_edge = 1;
            } else if (joined) prop_join(ctx, joined, ctx->val);
        } else if (joined) {
            /* A non-exhaustive match has an implicit unchanged edge. */
            if (!have_edge) {
                memcpy(joined, before, (size_t)ctx->nslots * sizeof(*joined));
                have_edge = 1;
            } else prop_join(ctx, joined, before);
        }
        if (joined && have_edge)
            memcpy(ctx->val, joined, (size_t)ctx->nslots * sizeof(*joined));
        else prop_invalidate(ctx);
        free(before); free(joined);
        return;
    }
    case HK_WHILE:
    case HK_DOWHILE:
    case HK_FOR:
        prop_loop(ctx, stmt);
        return;
    case HK_RETURN:
        prop_subtree(ctx, stmt->child[0]);
        prop_invalidate(ctx);
        return;
    case HK_BREAK:
    case HK_CONTINUE:
        prop_invalidate(ctx);
        return;
    case HK_CALL:
        for (int i = 0; i < stmt->nitems; i++)
            prop_subtree(ctx, stmt->items[i]);
        prop_invalidate(ctx);
        return;
    case HK_CATCH:
        prop_subtree(ctx, stmt->child[0]);
        prop_subtree(ctx, stmt->child[1]);
        prop_invalidate(ctx);
        return;
    case HK_SEND:
    case HK_RECV:
    case HK_CLOSE:
    case HK_AWAIT:
    case HK_ALLOC:
    case HK_FREE:
    case HK_PANIC:
        for (int i = 0; i < 4; i++)
            prop_subtree(ctx, stmt->child[i]);
        for (int i = 0; i < stmt->nitems; i++)
            prop_subtree(ctx, stmt->items[i]);
        prop_invalidate(ctx);
        return;
    case HK_DEFER:
        return;
    case HK_INLINE_ASM:
    case HK_ASYNC:
    case HK_GO_ROUTINE:
    case HK_SELECT:
        prop_invalidate(ctx);
        return;
    default:
        prop_subtree(ctx, stmt);
        return;
    }
}

static void prop_subtree(PropCtx *ctx, HNode *n) {
    if (!n) return;
    if (n->kind == HK_BLOCK || n->kind == HK_LET ||
        n->kind == HK_STORE || n->kind == HK_IF ||
        n->kind == HK_WHILE || n->kind == HK_DOWHILE ||
        n->kind == HK_FOR || n->kind == HK_MATCH ||
        n->kind == HK_RETURN || n->kind == HK_BREAK ||
        n->kind == HK_CONTINUE || n->kind == HK_CALL ||
        n->kind == HK_CATCH || n->kind == HK_SEND ||
        n->kind == HK_RECV || n->kind == HK_CLOSE ||
        n->kind == HK_AWAIT || n->kind == HK_ALLOC ||
        n->kind == HK_FREE || n->kind == HK_PANIC ||
        n->kind == HK_SELECT || n->kind == HK_DEFER ||
        n->kind == HK_INLINE_ASM || n->kind == HK_ASYNC ||
        n->kind == HK_GO_ROUTINE) {
        prop_stmt(ctx, n);
        return;
    }
    if (n->kind == HK_ADDR ||
        (n->kind == HK_UNOP && n->u.unop == HOP_ADDR_OF)) {
        prop_lvalue(ctx, n->child[0]);
        return;
    }
    for (int i = 0; i < 4; i++) {
        HNode *c = n->child[i];
        if (c && c->kind == HK_LOCAL) {
            HNode *v = prop_value(ctx, c);
            if (v != c) {
                n->child[i] = v;
                ctx->changes++;
            }
        } else {
            prop_subtree(ctx, c);
        }
    }
    for (int i = 0; i < n->nitems; i++) {
        HNode *c = n->items[i];
        if (c && c->kind == HK_LOCAL) {
            HNode *v = prop_value(ctx, c);
            if (v != c) {
                n->items[i] = v;
                ctx->changes++;
            }
        } else {
            prop_subtree(ctx, c);
        }
    }
}

static void prop_mark_alias_barriers(PropCtx *ctx, const HNode *n) {
    if (!n) return;
    if (n->kind == HK_STORE && n->child[0] &&
        n->child[0]->kind == HK_LOCAL) {
        int slot = n->child[0]->u.index;
        if (slot >= 0 && slot < ctx->nslots)
            ctx->alias_safe[slot] = 0;
    }
    if (n->kind == HK_FOR && n->u.index >= 0 &&
        n->u.index < ctx->nslots)
        ctx->alias_safe[n->u.index] = 0;
    if (n->kind == HK_ADDR ||
        (n->kind == HK_UNOP && n->u.unop == HOP_ADDR_OF)) {
        const HNode *v = n->child[0];
        if (v && v->kind == HK_LOCAL && v->u.index >= 0 &&
            v->u.index < ctx->nslots)
            ctx->alias_safe[v->u.index] = 0;
    }
    for (int i = 0; i < 4; ++i)
        prop_mark_alias_barriers(ctx, n->child[i]);
    for (int i = 0; i < n->nitems; ++i)
        prop_mark_alias_barriers(ctx, n->items[i]);
}

static long prop_func(HNode *fn) {
    HNode *body = fn->child[3];
    if (!body) return 0;
    int max_slot = -1;
    prop_find_slots(fn, &max_slot);
    PropCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.nslots = max_slot + 1;
    if (ctx.nslots > 0) {
        ctx.val = calloc((size_t)ctx.nslots, sizeof(*ctx.val));
        ctx.alias_safe = malloc((size_t)ctx.nslots);
        if (!ctx.val || !ctx.alias_safe) {
            free(ctx.val);
            free(ctx.alias_safe);
            return 0;
        }
        memset(ctx.alias_safe, 1, (size_t)ctx.nslots);
        prop_mark_alias_barriers(&ctx, body);
    }
    prop_stmt(&ctx, body);
    long changes = ctx.changes;
    free(ctx.alias_safe);
    free(ctx.val);
    return changes;
}

long opt_prop(HModule *m) {
    if (!m) return 0;
    HNode *root = hir_root(m);
    if (!root) return 0;
    long changes = 0;
    for (int i = 0; i < root->nitems; i++) {
        HNode *fn = root->items[i];
        if (fn && fn->kind == HK_FUNC)
            changes += prop_func(fn);
    }
    return changes;
}
