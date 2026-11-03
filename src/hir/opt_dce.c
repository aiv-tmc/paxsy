#include "common.h"
#include "hir.h"
#include "opt.h"
#include "opt.h"

/*
 * opt_dce.c - dead code elimination over the HIR.
 *
 * Per function, two kinds of statements are detached:
 *   1. a HK_LET whose local slot is never referenced and whose
 *      initialiser is pure (or absent) - the binding has no effect;
 *   2. a HK_EXPR_STMT whose expression is pure - it computes a value
 *      nobody reads and performs no observable action.
 * Local slots are numbered per function: parameters take 0..n-1 in
 * order, every let and every loop variable takes the next free slot,
 * and the slot number is stored in the let's u.index. The use counts
 * collected from HK_LOCAL references are therefore indexed by slot
 * number, never by the let's declaration ordinal. Removing a slot
 * renumbers every later slot; the pass builds a dense old-to-new map
 * over the whole slot space and rewrites all slot references in one
 * sweep from the function root. Parameter slots occupy the low end,
 * never carry the dead mark, and keep their numbering. Lets whose
 * initialiser has side effects (calls, allocations, channel ops) are
 * kept because those effects must still run. References made by dead
 * code count on the first sweep, so a let that only feeds another
 * dead let survives this round and is collected by the next fixpoint
 * iteration of the pass manager.
 */

typedef struct {
    HNode *parent;
    HNode *let;
} LetRef;

typedef struct {
    HNode *fn;        /* function root the scan belongs to          */
    int *uses;        /* use count per local slot (grown on demand) */
    int nslots;
    LetRef *lets;     /* lets in declaration order                  */
    int nlets;
    HNode **dstmt;    /* pure expression statements pending drop  */
    HNode **dparent;
    int ndstmt;
} DceCtx;

/* Grows the use-count array so slot `s` is addressable. */
static void dce_touch(DceCtx *ctx, int s) {
    if (s < ctx->nslots) return;
    int cap = ctx->nslots ? ctx->nslots * 2 : 16;
    while (cap <= s) cap *= 2;
    ctx->uses = realloc(ctx->uses, (size_t)cap * sizeof(int));
    for (int i = ctx->nslots; i < cap; i++) ctx->uses[i] = 0;
    ctx->nslots = cap;
}

/* Records locals, lets and pure expression statements; recurses over
 * the whole function body so every reference counts. */
static void dce_scan(DceCtx *ctx, HNode *parent, HNode *n) {
    if (!n) return;
    switch (n->kind) {
    case HK_LOCAL:
        dce_touch(ctx, n->u.index);
        ctx->uses[n->u.index]++;
        break;
    case HK_LET:
        ctx->lets = realloc(ctx->lets,
                            (size_t)(ctx->nlets + 1) * sizeof(LetRef));
        ctx->lets[ctx->nlets].parent = parent;
        ctx->lets[ctx->nlets].let = n;
        ctx->nlets++;
        break;
    case HK_EXPR_STMT:
        if (opt_is_pure(n->child[0])) {
            ctx->dstmt = realloc(ctx->dstmt,
                                 (size_t)(ctx->ndstmt + 1) * sizeof(HNode *));
            ctx->dparent = realloc(ctx->dparent,
                                   (size_t)(ctx->ndstmt + 1) * sizeof(HNode *));
            ctx->dstmt[ctx->ndstmt] = n;
            ctx->dparent[ctx->ndstmt] = parent;
            ctx->ndstmt++;
        }
        break;
    default:
        break;
    }
    for (int i = 0; i < 4; i++) dce_scan(ctx, n, n->child[i]);
    for (int i = 0; i < n->nitems; i++) dce_scan(ctx, n, n->items[i]);
}

/* Renames every slot reference through the old-to-new map and compacts
 * item arrays whose elements were detached. Both HK_LOCAL and HK_PARAM
 * nodes carry u.index, so parameters are renamed by the same map; with
 * the dense map built below their entries are identities. */
static void dce_rewrite(DceCtx *ctx, HNode *n, const int *map) {
    if (!n) return;
    if ((n->kind == HK_LOCAL || n->kind == HK_PARAM) &&
        n->u.index >= 0 && n->u.index < ctx->nslots)
        n->u.index = map[n->u.index];
    for (int i = 0; i < 4; i++) dce_rewrite(ctx, n->child[i], map);
    if (n->items) {
        int w = 0;
        for (int i = 0; i < n->nitems; i++)
            if (n->items[i]) n->items[w++] = n->items[i];
        n->nitems = w;
        for (int i = 0; i < n->nitems; i++)
            dce_rewrite(ctx, n->items[i], map);
    }
}

/* Detaches the statements chosen for removal and renumbers the
 * surviving slots so the numbering stays dense. Returns the number of
 * statements detached. */
static long dce_sweep(DceCtx *ctx) {
    if (ctx->nlets == 0 && ctx->ndstmt == 0) return 0;
    /* Mark removable slots: a let binds the local slot stored in its
     * u.index, so the use count is looked up by slot number, never by
     * the let's declaration ordinal. */
    char *deadslot = calloc((size_t)(ctx->nslots ? ctx->nslots : 1), 1);
    int removed = 0;
    for (int s = 0; s < ctx->nlets; s++) {
        HNode *let = ctx->lets[s].let;
        int slot = let->u.index;
        if (slot < 0 || slot >= ctx->nslots) continue;
        if (ctx->uses[slot] != 0) continue;
        if (let->child[0] && !opt_is_pure(let->child[0])) continue;
        deadslot[slot] = 1;
        removed++;
    }
    /* Dense slot map over the whole slot space: surviving slots are
     * renumbered downward past every removed slot; a removed slot has
     * no references left and maps onto the last surviving slot so the
     * value stays in range even if an unexpected reference survives. */
    int *map = malloc((size_t)(ctx->nslots ? ctx->nslots : 1) * sizeof(int));
    int next = 0;
    for (int s = 0; s < ctx->nslots; s++) {
        if (deadslot[s]) map[s] = next > 0 ? next - 1 : 0;
        else map[s] = next++;
    }
    /* Apply: detach the dead lets and the pure expression statements,
     * then rename every slot reference in one sweep from the function
     * root so every branch is covered. */
    for (int s = 0; s < ctx->nlets; s++)
        if (deadslot[ctx->lets[s].let->u.index])
            opt_detach(ctx->lets[s].parent, ctx->lets[s].let);
    for (int i = 0; i < ctx->ndstmt; i++)
        opt_detach(ctx->dparent[i], ctx->dstmt[i]);
    dce_rewrite(ctx, ctx->fn, map);
    free(map);
    free(deadslot);
    return (long)removed + ctx->ndstmt;
}

/* Releases the per-function scan storage. */
static void dce_ctx_free(DceCtx *ctx) {
    free(ctx->uses);
    free(ctx->lets);
    free(ctx->dstmt);
    free(ctx->dparent);
    memset(ctx, 0, sizeof(*ctx));
}

long opt_dce(HModule *m) {
    HNode *root = hir_root(m);
    if (!root) return 0;
    long changes = 0;
    for (int i = 0; i < root->nitems; i++) {
        HNode *tl = root->items[i];
        if (!tl || tl->kind != HK_FUNC) continue;
        DceCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.fn = tl;
        for (int c = 0; c < 4; c++) dce_scan(&ctx, tl, tl->child[c]);
        for (int c = 0; c < tl->nitems; c++) dce_scan(&ctx, tl, tl->items[c]);
        changes += dce_sweep(&ctx);
        dce_ctx_free(&ctx);
    }
    return changes;
}
