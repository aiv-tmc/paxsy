#include "common.h"
#include "hir.h"
#include "opt.h"

typedef struct {
    unsigned char *live;
    int nslots;
    long changes;
    int analysis_only;
} StoreCtx;

static void mark_expr_reads(StoreCtx *ctx, const HNode *n) {
    if (!n) return;
    if (n->kind == HK_LOCAL && n->u.index >= 0 &&
        n->u.index < ctx->nslots) {
        ctx->live[n->u.index] = 1;
        return;
    }
    for (int i = 0; i < 4; ++i) mark_expr_reads(ctx, n->child[i]);
    for (int i = 0; i < n->nitems; ++i) mark_expr_reads(ctx, n->items[i]);
}

static void mark_all_live(StoreCtx *ctx) {
    if (ctx->nslots > 0) memset(ctx->live, 1, (size_t)ctx->nslots);
}


static void optimize_block(StoreCtx *ctx, HNode *block);
static unsigned char *live_copy(const StoreCtx *ctx);
static void live_union(StoreCtx *ctx, const unsigned char *a,
                       const unsigned char *b);

/* A loop with an unlabelled break/continue or a nested loop has extra
 * control-flow edges that this tree-level analysis does not model yet.
 * Keep such loops conservative rather than guessing at their live-outs. */
static int loop_has_unsupported_exit(const HNode *n, int root) {
    if (!n) return 0;
    if (n->kind == HK_BREAK || n->kind == HK_CONTINUE) return 1;
    if (!root && (n->kind == HK_WHILE || n->kind == HK_DOWHILE ||
                  n->kind == HK_FOR)) return 1;
    for (int i = 0; i < 4; ++i)
        if (loop_has_unsupported_exit(n->child[i], 0)) return 1;
    for (int i = 0; i < n->nitems; ++i)
        if (loop_has_unsupported_exit(n->items[i], 0)) return 1;
    return 0;
}

static int live_equal(const StoreCtx *ctx, const unsigned char *a,
                      const unsigned char *b) {
    return ctx->nslots <= 0 ||
           memcmp(a, b, (size_t)ctx->nslots) == 0;
}

/* Solve the loop-carried liveness equation to a fixed point before
 * allowing any store to be deleted. The first pass is analysis-only;
 * the final pass uses the stable header set and performs rewrites. */
static void optimize_loop(StoreCtx *ctx, HNode *loop) {
    HNode *body = NULL, *cond = NULL;
    int loop_slot = -1;
    if (loop->kind == HK_WHILE) {
        cond = loop->child[0]; body = loop->child[1];
    } else if (loop->kind == HK_DOWHILE) {
        body = loop->child[0]; cond = loop->child[1];
    } else {
        body = loop->child[2]; loop_slot = loop->u.index;
    }

    if (loop_has_unsupported_exit(body, 1)) {
        mark_all_live(ctx);
        if (loop->kind == HK_FOR) {
            mark_expr_reads(ctx, loop->child[0]);
            mark_expr_reads(ctx, loop->child[1]);
        } else {
            mark_expr_reads(ctx, cond);
        }
        return;
    }

    unsigned char *base = live_copy(ctx);
    unsigned char *header = live_copy(ctx);
    unsigned char *previous = live_copy(ctx);
    if (ctx->nslots > 0 && (!base || !header || !previous)) {
        free(base); free(header); free(previous);
        mark_all_live(ctx);
        return;
    }
    if (ctx->nslots > 0) {
        memcpy(base, ctx->live, (size_t)ctx->nslots);
        memcpy(header, base, (size_t)ctx->nslots);
    }

    int saved_analysis = ctx->analysis_only;
    ctx->analysis_only = 1;
    for (int iteration = 0; iteration <= ctx->nslots + 1; ++iteration) {
        if (ctx->nslots > 0) {
            memcpy(previous, header, (size_t)ctx->nslots);
            memcpy(ctx->live, header, (size_t)ctx->nslots);
        }
        if (loop->kind == HK_DOWHILE) mark_expr_reads(ctx, cond);
        optimize_block(ctx, body);
        if (loop->kind == HK_WHILE) mark_expr_reads(ctx, cond);
        if (loop->kind == HK_FOR) {
            if (loop_slot >= 0 && loop_slot < ctx->nslots)
                ctx->live[loop_slot] = 0;
            mark_expr_reads(ctx, loop->child[0]);
            mark_expr_reads(ctx, loop->child[1]);
        }
        if (ctx->nslots > 0) {
            live_union(ctx, ctx->live, base);
            memcpy(header, ctx->live, (size_t)ctx->nslots);
            if (live_equal(ctx, header, previous)) break;
        } else {
            break;
        }
    }
    ctx->analysis_only = saved_analysis;

    if (ctx->nslots > 0) memcpy(ctx->live, header, (size_t)ctx->nslots);
    if (loop->kind == HK_DOWHILE) mark_expr_reads(ctx, cond);
    optimize_block(ctx, body);
    if (loop->kind == HK_WHILE) mark_expr_reads(ctx, cond);
    if (loop->kind == HK_FOR) {
        if (loop_slot >= 0 && loop_slot < ctx->nslots)
            ctx->live[loop_slot] = 0;
        mark_expr_reads(ctx, loop->child[0]);
        mark_expr_reads(ctx, loop->child[1]);
    }
    if (ctx->nslots > 0) live_union(ctx, ctx->live, base);
    free(base); free(header); free(previous);
}

static unsigned char *live_copy(const StoreCtx *ctx) {
    if (ctx->nslots <= 0) return NULL;
    unsigned char *copy = malloc((size_t)ctx->nslots);
    if (copy) memcpy(copy, ctx->live, (size_t)ctx->nslots);
    return copy;
}

static void live_union(StoreCtx *ctx, const unsigned char *a,
                       const unsigned char *b) {
    for (int i = 0; i < ctx->nslots; ++i)
        ctx->live[i] = (unsigned char)(a[i] || b[i]);
}

static void optimize_if(StoreCtx *ctx, HNode *stmt) {
    if (stmt->flags & HF_EXPR) {
        mark_all_live(ctx);
        return;
    }
    unsigned char *out = live_copy(ctx);
    unsigned char *then_live = live_copy(ctx);
    unsigned char *else_live = live_copy(ctx);
    if (ctx->nslots > 0 && (!out || !then_live || !else_live)) {
        free(out);
        free(then_live);
        free(else_live);
        mark_all_live(ctx);
        return;
    }

    optimize_block(ctx, stmt->child[1]);
    if (then_live) memcpy(then_live, ctx->live, (size_t)ctx->nslots);

    if (out) memcpy(ctx->live, out, (size_t)ctx->nslots);
    optimize_block(ctx, stmt->child[2]);
    if (else_live) memcpy(else_live, ctx->live, (size_t)ctx->nslots);

    if (ctx->nslots > 0) {
        if (!stmt->child[2])
            memcpy(else_live, out, (size_t)ctx->nslots);
        live_union(ctx, then_live, else_live);
    }
    mark_expr_reads(ctx, stmt->child[0]);
    free(out);
    free(then_live);
    free(else_live);
}

static void optimize_match(StoreCtx *ctx, HNode *stmt) {
    if (stmt->flags & HF_EXPR) {
        mark_all_live(ctx);
        return;
    }
    unsigned char *out = live_copy(ctx);
    unsigned char *merged = live_copy(ctx);
    unsigned char *branch = live_copy(ctx);
    if (ctx->nslots > 0 && (!out || !merged || !branch)) {
        free(out);
        free(merged);
        free(branch);
        mark_all_live(ctx);
        return;
    }
    if (ctx->nslots > 0 && merged)
        memset(merged, 0, (size_t)ctx->nslots);

    for (int i = 0; i < stmt->nitems; ++i) {
        HNode *arm = stmt->items[i];
        if (out) memcpy(ctx->live, out, (size_t)ctx->nslots);
        if (arm) {
            optimize_block(ctx, arm->child[1]);
            for (int k = 0; k < arm->nitems; ++k)
                mark_expr_reads(ctx, arm->items[k]);
        }
        if (branch) {
            memcpy(branch, ctx->live, (size_t)ctx->nslots);
            live_union(ctx, merged, branch);
            memcpy(merged, ctx->live, (size_t)ctx->nslots);
        }
    }

    if (out) memcpy(ctx->live, out, (size_t)ctx->nslots);
    if (stmt->child[2]) {
        optimize_block(ctx, stmt->child[2]);
        if (branch) {
            memcpy(branch, ctx->live, (size_t)ctx->nslots);
            live_union(ctx, merged, branch);
            memcpy(merged, ctx->live, (size_t)ctx->nslots);
        }
    } else if (out && merged) {
        live_union(ctx, merged, out);
        memcpy(merged, ctx->live, (size_t)ctx->nslots);
    }
    if (merged) memcpy(ctx->live, merged, (size_t)ctx->nslots);
    mark_expr_reads(ctx, stmt->child[0]);
    free(out);
    free(merged);
    free(branch);
}

static void optimize_block(StoreCtx *ctx, HNode *block) {
    if (!block || block->kind != HK_BLOCK) return;

    for (int i = block->nitems - 1; i >= 0; --i) {
        HNode *stmt = block->items[i];
        if (!stmt) continue;
        if (stmt->kind == HK_LET) {
            int slot = stmt->u.index;
            int used = slot >= 0 && slot < ctx->nslots && ctx->live[slot];
            HNode *init = stmt->child[1];
            if (!ctx->analysis_only && !(stmt->flags & HF_ARMVAL) && !used &&
                (!init || opt_is_pure(init))) {
                block->items[i] = NULL;
                if (!ctx->analysis_only) ctx->changes++;
                continue;
            }
            if (slot >= 0 && slot < ctx->nslots) ctx->live[slot] = 0;
            mark_expr_reads(ctx, init);
            continue;
        }
        if (stmt->kind == HK_STORE && stmt->child[0] &&
            stmt->child[0]->kind == HK_LOCAL) {
            int slot = stmt->child[0]->u.index;
            HNode *rhs = stmt->child[1];
            int used = slot >= 0 && slot < ctx->nslots && ctx->live[slot];
            if (!ctx->analysis_only && !(stmt->flags & HF_ARMVAL) &&
                !used && opt_is_pure(rhs)) {
                block->items[i] = NULL;
                if (!ctx->analysis_only) ctx->changes++;
                continue;
            }
            if (slot >= 0 && slot < ctx->nslots) ctx->live[slot] = 0;
            mark_expr_reads(ctx, rhs);
            continue;
        }
        if (stmt->kind == HK_IF) {
            optimize_if(ctx, stmt);
            continue;
        }
        if (stmt->kind == HK_MATCH) {
            optimize_match(ctx, stmt);
            continue;
        }
        if (stmt->kind == HK_BLOCK) {
            optimize_block(ctx, stmt);
            continue;
        }
        if (stmt->kind == HK_WHILE || stmt->kind == HK_DOWHILE ||
            stmt->kind == HK_FOR) {
            optimize_loop(ctx, stmt);
            continue;
        }
        if (stmt->kind == HK_EXPR_STMT) {
            mark_expr_reads(ctx, stmt->child[0]);
            continue;
        }
        if (stmt->kind == HK_RETURN) {
            mark_expr_reads(ctx, stmt->child[0]);
            continue;
        }
        mark_all_live(ctx);
    }

    int w = 0;
    for (int i = 0; i < block->nitems; ++i)
        if (block->items[i]) block->items[w++] = block->items[i];
    block->nitems = w;
}

static void find_max_slot(const HNode *n, int *max_slot) {
    if (!n) return;
    if ((n->kind == HK_LOCAL || n->kind == HK_PARAM ||
         n->kind == HK_LET || n->kind == HK_FOR) &&
        n->u.index > *max_slot) *max_slot = n->u.index;
    for (int i = 0; i < 4; ++i) find_max_slot(n->child[i], max_slot);
    for (int i = 0; i < n->nitems; ++i) find_max_slot(n->items[i], max_slot);
}

static int store_pass_safe(const HNode *n) {
    if (!n) return 1;
    if (n->kind == HK_ADDR || n->kind == HK_DEFER ||
        n->kind == HK_INLINE_ASM || n->kind == HK_ASYNC ||
        n->kind == HK_GO_ROUTINE ||
        (n->kind == HK_UNOP && n->u.unop == HOP_ADDR_OF))
        return 0;
    for (int i = 0; i < 4; ++i)
        if (!store_pass_safe(n->child[i])) return 0;
    for (int i = 0; i < n->nitems; ++i)
        if (!store_pass_safe(n->items[i])) return 0;
    return 1;
}

long opt_store(HModule *m) {
    HNode *root = hir_root(m);
    if (!root) return 0;
    long changes = 0;
    for (int i = 0; i < root->nitems; ++i) {
        HNode *fn = root->items[i];
        if (!fn || fn->kind != HK_FUNC || !fn->child[3] ||
            !store_pass_safe(fn->child[3])) continue;
        int max_slot = -1;
        find_max_slot(fn, &max_slot);
        StoreCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.nslots = max_slot + 1;
        if (ctx.nslots > 0) {
            ctx.live = calloc((size_t)ctx.nslots, 1);
            if (!ctx.live) continue;
        }
        optimize_block(&ctx, fn->child[3]);
        changes += ctx.changes;
        free(ctx.live);
    }
    return changes;
}
