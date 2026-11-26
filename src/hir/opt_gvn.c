#include "common.h"
#include "hir.h"
#include "opt.h"

#include <stdint.h>
#include <limits.h>

typedef struct {
    HNode *expr;
    int holder;
    uint64_t hash;
    unsigned char valid;
} GvnEntry;

/* Slot-indexed facts plus a hash-bucket index. The entry array remains the
 * canonical snapshot representation used at CFG joins; buckets/next are a
 * rebuildable acceleration structure and are never copied independently. */
typedef struct {
    HModule *module;
    GvnEntry *entries;
    int *buckets;
    int *next;
    int nbuckets;
    int nslots;
    /* Reverse dependency graph: each expression holder owns a chain of
     * edges, while each read slot owns the reverse chain of its users. */
    int *dep_heads;
    int *holder_heads;
    int *dep_next_slot;
    int *dep_prev_slot;
    int *dep_next_holder;
    int *dep_slot;
    int *dep_holder;
    int dep_count;
    int dep_capacity;
    int dep_free_head;
    int dep_failed;
    long changes;
} GvnCtx;

typedef struct {
    uint64_t hash;
    int safe;
} GvnExprInfo;

static int scalar_int_type(const HType *t) {
    return t && (t->kind == HTY_BOOL || t->kind == HTY_CHAR ||
                 t->kind == HTY_INT || t->kind == HTY_UINT);
}

static int gvn_candidate(const HNode *n) {
    return n && (n->kind == HK_BINOP || n->kind == HK_UNOP ||
                 n->kind == HK_CAST);
}

static int gvn_expr_safe(const HNode *n, int nslots) {
    if (!n || !scalar_int_type(n->type)) return 0;
    switch (n->kind) {
    case HK_CONST_INT:
    case HK_CONST_CHAR:
        return 1;
    case HK_LOCAL:
    case HK_PARAM:
        return n->u.index >= 0 && n->u.index < nslots;
    case HK_BINOP:
        if (n->u.binop == HOP_DIV || n->u.binop == HOP_MOD)
            return 0;
        return gvn_expr_safe(n->child[0], nslots) &&
               gvn_expr_safe(n->child[1], nslots);
    case HK_UNOP:
        if (n->u.unop == HUN_DEREF || n->u.unop == HOP_ADDR_OF)
            return 0;
        return gvn_expr_safe(n->child[0], nslots);
    case HK_CAST:
        return gvn_expr_safe(n->child[0], nslots);
    default:
        return 0;
    }
}

static uint64_t hash_mix(uint64_t h, uint64_t v) {
    h ^= v;
    h *= UINT64_C(1099511628211);
    return h;
}

static int gvn_commutative(HBinOp op) {
    return op == HOP_ADD || op == HOP_MUL || op == HOP_AND ||
           op == HOP_OR || op == HOP_XOR || op == HOP_EQ || op == HOP_NE;
}

static uint64_t gvn_hash(const HNode *n) {
    uint64_t h = UINT64_C(1469598103934665603);
    h = hash_mix(h, (uint64_t)n->kind);
    h = hash_mix(h, (uint64_t)(uintptr_t)n->type);
    switch (n->kind) {
    case HK_CONST_INT:
        h = hash_mix(h, (uint64_t)n->u.ival);
        break;
    case HK_CONST_CHAR:
        h = hash_mix(h, (uint64_t)n->u.slice.len);
        for (int i = 0; i < n->u.slice.len; ++i)
            h = hash_mix(h, (unsigned char)n->u.slice.start[i]);
        break;
    case HK_LOCAL:
    case HK_PARAM:
        h = hash_mix(h, (uint64_t)n->u.index);
        break;
    case HK_BINOP: {
        uint64_t a = gvn_hash(n->child[0]);
        uint64_t b = gvn_hash(n->child[1]);
        if (gvn_commutative(n->u.binop) && a > b) {
            uint64_t tmp = a;
            a = b;
            b = tmp;
        }
        h = hash_mix(h, (uint64_t)n->u.binop);
        h = hash_mix(h, a);
        h = hash_mix(h, b);
        break;
    }
    case HK_UNOP:
        h = hash_mix(h, (uint64_t)n->u.unop);
        h = hash_mix(h, gvn_hash(n->child[0]));
        break;
    case HK_CAST:
        h = hash_mix(h, gvn_hash(n->child[0]));
        break;
    default:
        break;
    }
    return h;
}

static int gvn_equal(const HNode *a, const HNode *b) {
    if (a == b) return 1;
    if (!a || !b || a->kind != b->kind || a->type != b->type)
        return 0;
    switch (a->kind) {
    case HK_CONST_INT:
        return a->u.ival == b->u.ival;
    case HK_CONST_CHAR:
        return a->u.slice.len == b->u.slice.len &&
               a->u.slice.start && b->u.slice.start &&
               memcmp(a->u.slice.start, b->u.slice.start,
                      (size_t)a->u.slice.len) == 0;
    case HK_LOCAL:
    case HK_PARAM:
        return a->u.index == b->u.index;
    case HK_BINOP:
        if (a->u.binop != b->u.binop) return 0;
        if (gvn_equal(a->child[0], b->child[0]) &&
            gvn_equal(a->child[1], b->child[1])) return 1;
        return gvn_commutative(a->u.binop) &&
               gvn_equal(a->child[0], b->child[1]) &&
               gvn_equal(a->child[1], b->child[0]);
    case HK_UNOP:
        return a->u.unop == b->u.unop &&
               gvn_equal(a->child[0], b->child[0]);
    case HK_CAST:
        return gvn_equal(a->child[0], b->child[0]);
    default:
        return 0;
    }
}

static int gvn_reads_slot(const HNode *n, int slot) {
    if (!n) return 0;
    if ((n->kind == HK_LOCAL || n->kind == HK_PARAM) &&
        n->u.index == slot)
        return 1;
    for (int i = 0; i < 4; ++i)
        if (gvn_reads_slot(n->child[i], slot)) return 1;
    for (int i = 0; i < n->nitems; ++i)
        if (gvn_reads_slot(n->items[i], slot)) return 1;
    return 0;
}

static void gvn_index_remove(GvnCtx *ctx, int holder) {
    if (!ctx->buckets || holder < 0 || holder >= ctx->nslots) return;
    GvnEntry *entry = &ctx->entries[holder];
    if (!entry->valid) return;
    int bucket = (int)(entry->hash & (uint64_t)(ctx->nbuckets - 1));
    int *link = &ctx->buckets[bucket];
    while (*link >= 0) {
        if (*link == holder) {
            *link = ctx->next[holder];
            ctx->next[holder] = -1;
            return;
        }
        link = &ctx->next[*link];
    }
}

static void gvn_index_insert(GvnCtx *ctx, int holder) {
    if (!ctx->buckets || holder < 0 || holder >= ctx->nslots ||
        !ctx->entries[holder].valid) return;
    int bucket = (int)(ctx->entries[holder].hash &
                       (uint64_t)(ctx->nbuckets - 1));
    ctx->next[holder] = ctx->buckets[bucket];
    ctx->buckets[bucket] = holder;
}

static void gvn_index_rebuild(GvnCtx *ctx) {
    if (!ctx->buckets) return;
    for (int i = 0; i < ctx->nbuckets; ++i) ctx->buckets[i] = -1;
    for (int i = 0; i < ctx->nslots; ++i) ctx->next[i] = -1;
    for (int i = 0; i < ctx->nslots; ++i) gvn_index_insert(ctx, i);
}

static void gvn_dep_remove_holder(GvnCtx *ctx, int holder) {
    if (!ctx->holder_heads) return;
    int edge = ctx->holder_heads[holder];
    while (edge >= 0) {
        int next = ctx->dep_next_holder[edge];
        int slot = ctx->dep_slot[edge];
        int prev = ctx->dep_prev_slot[edge];
        int after = ctx->dep_next_slot[edge];
        if (prev >= 0) ctx->dep_next_slot[prev] = after;
        else ctx->dep_heads[slot] = after;
        if (after >= 0) ctx->dep_prev_slot[after] = prev;
        ctx->dep_next_holder[edge] = ctx->dep_free_head;
        ctx->dep_free_head = edge;
        edge = next;
    }
    ctx->holder_heads[holder] = -1;
}

static int gvn_dep_add(GvnCtx *ctx, int holder, int slot) {
    if (ctx->dep_free_head >= 0) {
        int edge = ctx->dep_free_head;
        ctx->dep_free_head = ctx->dep_next_holder[edge];
        int old = ctx->dep_heads[slot];
        ctx->dep_slot[edge] = slot;
        ctx->dep_holder[edge] = holder;
        ctx->dep_prev_slot[edge] = -1;
        ctx->dep_next_slot[edge] = old;
        if (old >= 0) ctx->dep_prev_slot[old] = edge;
        ctx->dep_heads[slot] = edge;
        ctx->dep_next_holder[edge] = ctx->holder_heads[holder];
        ctx->holder_heads[holder] = edge;
        return 1;
    }
    if (ctx->dep_count == ctx->dep_capacity) {
        int cap = ctx->dep_capacity ? ctx->dep_capacity * 2 : 64;
        if (cap < ctx->dep_capacity ||
            (size_t)cap > SIZE_MAX / sizeof(int)) return 0;
        int *a = realloc(ctx->dep_next_slot, (size_t)cap * sizeof(int));
        if (!a) return 0;
        ctx->dep_next_slot = a;
        a = realloc(ctx->dep_prev_slot, (size_t)cap * sizeof(int));
        if (!a) return 0;
        ctx->dep_prev_slot = a;
        a = realloc(ctx->dep_next_holder, (size_t)cap * sizeof(int));
        if (!a) return 0;
        ctx->dep_next_holder = a;
        a = realloc(ctx->dep_slot, (size_t)cap * sizeof(int));
        if (!a) return 0;
        ctx->dep_slot = a;
        a = realloc(ctx->dep_holder, (size_t)cap * sizeof(int));
        if (!a) return 0;
        ctx->dep_holder = a;
        ctx->dep_capacity = cap;
    }
    int edge = ctx->dep_count++;
    int old = ctx->dep_heads[slot];
    ctx->dep_slot[edge] = slot;
    ctx->dep_holder[edge] = holder;
    ctx->dep_prev_slot[edge] = -1;
    ctx->dep_next_slot[edge] = old;
    if (old >= 0) ctx->dep_prev_slot[old] = edge;
    ctx->dep_heads[slot] = edge;
    ctx->dep_next_holder[edge] = ctx->holder_heads[holder];
    ctx->holder_heads[holder] = edge;
    return 1;
}

static int gvn_dep_collect(GvnCtx *ctx, int holder, const HNode *n) {
    if (!n) return 1;
    if ((n->kind == HK_LOCAL || n->kind == HK_PARAM) &&
        n->u.index >= 0 && n->u.index < ctx->nslots &&
        !gvn_dep_add(ctx, holder, n->u.index)) return 0;
    for (int i = 0; i < 4; ++i)
        if (!gvn_dep_collect(ctx, holder, n->child[i])) return 0;
    for (int i = 0; i < n->nitems; ++i)
        if (!gvn_dep_collect(ctx, holder, n->items[i])) return 0;
    return 1;
}

static void gvn_dep_rebuild(GvnCtx *ctx) {
    if (!ctx->dep_heads || !ctx->holder_heads) return;
    for (int i = 0; i < ctx->nslots; ++i) {
        ctx->dep_heads[i] = -1;
        ctx->holder_heads[i] = -1;
    }
    ctx->dep_count = 0;
    ctx->dep_free_head = -1;
    ctx->dep_failed = 0;
    for (int i = 0; i < ctx->nslots; ++i)
        if (ctx->entries[i].valid &&
            !gvn_dep_collect(ctx, i, ctx->entries[i].expr)) {
            ctx->dep_failed = 1;
            break;
        }
}

static void gvn_clear(GvnCtx *ctx) {
    for (int i = 0; i < ctx->nslots; ++i) ctx->entries[i].valid = 0;
    if (ctx->buckets)
        for (int i = 0; i < ctx->nbuckets; ++i) ctx->buckets[i] = -1;
    if (ctx->dep_heads)
        for (int i = 0; i < ctx->nslots; ++i) ctx->dep_heads[i] = -1;
    if (ctx->holder_heads)
        for (int i = 0; i < ctx->nslots; ++i) ctx->holder_heads[i] = -1;
    ctx->dep_count = 0;
    ctx->dep_free_head = -1;
    ctx->dep_failed = 0;
}

static void gvn_invalidate_holder(GvnCtx *ctx, int holder) {
    if (holder < 0 || holder >= ctx->nslots || !ctx->entries[holder].valid)
        return;
    gvn_index_remove(ctx, holder);
    if (!ctx->dep_failed) gvn_dep_remove_holder(ctx, holder);
    ctx->entries[holder].valid = 0;
}

static void gvn_invalidate_slot(GvnCtx *ctx, int slot) {
    if (slot < 0 || slot >= ctx->nslots) {
        gvn_clear(ctx);
        return;
    }
    if (ctx->dep_failed || !ctx->dep_heads) {
        for (int i = 0; i < ctx->nslots; ++i) {
            GvnEntry *e = &ctx->entries[i];
            if (e->valid && (e->holder == slot ||
                             gvn_reads_slot(e->expr, slot)))
                gvn_invalidate_holder(ctx, i);
        }
        return;
    }
    gvn_invalidate_holder(ctx, slot);
    while (ctx->dep_heads[slot] >= 0) {
        int edge = ctx->dep_heads[slot];
        int holder = ctx->dep_holder[edge];
        if (holder >= 0 && holder < ctx->nslots)
            gvn_invalidate_holder(ctx, holder);
        else
            break;
    }
}

static GvnEntry *gvn_lookup(GvnCtx *ctx, HNode *expr, int avoid_slot,
                            uint64_t hash) {
    if (!ctx->buckets || ctx->nbuckets <= 0) return NULL;
    int bucket = (int)(hash & (uint64_t)(ctx->nbuckets - 1));
    for (int i = ctx->buckets[bucket]; i >= 0; i = ctx->next[i]) {
        GvnEntry *e = &ctx->entries[i];
        if (e->valid && e->holder != avoid_slot && e->hash == hash &&
            gvn_equal(e->expr, expr))
            return e;
    }
    return NULL;
}

static void gvn_record(GvnCtx *ctx, HNode *expr, int holder) {
    if (holder < 0 || holder >= ctx->nslots || !gvn_candidate(expr) ||
        !gvn_expr_safe(expr, ctx->nslots) || gvn_reads_slot(expr, holder))
        return;
    gvn_index_remove(ctx, holder);
    GvnEntry *e = &ctx->entries[holder];
    e->expr = expr;
    e->holder = holder;
    e->hash = gvn_hash(expr);
    e->valid = 1;
    gvn_index_insert(ctx, holder);
    if (!ctx->dep_failed && ctx->dep_heads &&
        !gvn_dep_collect(ctx, holder, expr))
        ctx->dep_failed = 1;
}

static int gvn_has_barrier(const HNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    /* Stores can appear nested inside an expression statement (rather than
     * as a top-level HK_STORE statement). They mutate scalar slots and must
     * invalidate every value-numbering fact before and after evaluation. */
    case HK_STORE:
    case HK_CALL:
    case HK_ADDR:
    case HK_DEREF:
    case HK_INDEX:
    case HK_FIELD:
    case HK_ALLOC:
    case HK_FREE:
    case HK_INLINE_ASM:
    case HK_ASYNC:
    case HK_GO_ROUTINE:
    case HK_AWAIT:
    case HK_SEND:
    case HK_RECV:
    case HK_CLOSE:
    case HK_SELECT:
        return 1;
    case HK_UNOP:
        if (n->u.unop == HUN_DEREF || n->u.unop == HOP_ADDR_OF)
            return 1;
        break;
    default:
        break;
    }
    for (int i = 0; i < 4; ++i)
        if (gvn_has_barrier(n->child[i])) return 1;
    for (int i = 0; i < n->nitems; ++i)
        if (gvn_has_barrier(n->items[i])) return 1;
    return 0;
}

static HNode *gvn_local(GvnCtx *ctx, HNode *at, int slot,
                        const HType *type) {
    HNode *n = hir_node(ctx->module, HK_LOCAL, at->tok);
    if (!n) return NULL;
    n->u.index = slot;
    n->type = type;
    return n;
}

/* Compute expression safety and structural hash bottom-up.  The previous
 * implementation called recursive gvn_expr_safe() and gvn_hash() for every
 * candidate node, which made a long left-associated expression quadratic.
 * Each child now contributes its already-computed result exactly once. */
static GvnExprInfo gvn_info_for_node(const HNode *n,
                                     const GvnExprInfo child[4]) {
    GvnExprInfo info = {0, 0};
    if (!n || !scalar_int_type(n->type)) return info;
    uint64_t h = UINT64_C(1469598103934665603);
    h = hash_mix(h, (uint64_t)n->kind);
    h = hash_mix(h, (uint64_t)(uintptr_t)n->type);
    switch (n->kind) {
    case HK_CONST_INT:
        h = hash_mix(h, (uint64_t)n->u.ival);
        info.safe = 1;
        break;
    case HK_CONST_CHAR:
        h = hash_mix(h, (uint64_t)n->u.slice.len);
        for (int i = 0; i < n->u.slice.len; ++i)
            h = hash_mix(h, (unsigned char)n->u.slice.start[i]);
        info.safe = 1;
        break;
    case HK_LOCAL:
    case HK_PARAM:
        if (n->u.index < 0) return info;
        h = hash_mix(h, (uint64_t)n->u.index);
        info.safe = 1;
        break;
    case HK_BINOP: {
        if (n->u.binop == HOP_DIV || n->u.binop == HOP_MOD ||
            !child[0].safe || !child[1].safe) return info;
        uint64_t a = child[0].hash, b = child[1].hash;
        if (gvn_commutative(n->u.binop) && a > b) {
            uint64_t tmp = a; a = b; b = tmp;
        }
        h = hash_mix(h, (uint64_t)n->u.binop);
        h = hash_mix(h, a);
        h = hash_mix(h, b);
        info.safe = 1;
        break;
    }
    case HK_UNOP:
        if (n->u.unop == HUN_DEREF || n->u.unop == HOP_ADDR_OF ||
            !child[0].safe) return info;
        h = hash_mix(h, (uint64_t)n->u.unop);
        h = hash_mix(h, child[0].hash);
        info.safe = 1;
        break;
    case HK_CAST:
        if (!child[0].safe) return info;
        h = hash_mix(h, child[0].hash);
        info.safe = 1;
        break;
    default:
        return info;
    }
    info.hash = h;
    return info;
}

static GvnExprInfo gvn_expr(GvnCtx *ctx, HNode **slot, int avoid_slot,
                            int allow_cse, int check_barrier) {
    GvnExprInfo empty = {0, 0};
    HNode *n = slot ? *slot : NULL;
    if (!n) return empty;

    /* The barrier scan covers the complete expression subtree. Run it once
     * at the expression root, not again for every nested node. */
    if (allow_cse && check_barrier && gvn_has_barrier(n)) {
        gvn_clear(ctx);
        (void)gvn_expr(ctx, slot, avoid_slot, 0, 0);
        gvn_clear(ctx);
        return empty;
    }

    GvnExprInfo child[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    for (int i = 0; i < 4; ++i)
        child[i] = gvn_expr(ctx, &n->child[i], avoid_slot, allow_cse, 0);
    for (int i = 0; i < n->nitems; ++i)
        (void)gvn_expr(ctx, &n->items[i], avoid_slot, allow_cse, 0);

    GvnExprInfo info = gvn_info_for_node(n, child);
    if (!allow_cse || !info.safe || !gvn_candidate(n)) return info;
    GvnEntry *e = gvn_lookup(ctx, n, avoid_slot, info.hash);
    if (!e) return info;
    HNode *replacement = gvn_local(ctx, n, e->holder, n->type);
    if (!replacement) return info;
    *slot = replacement;
    ctx->changes++;
    GvnExprInfo local_child[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    return gvn_info_for_node(replacement, local_child);
}

static void gvn_stmt(GvnCtx *ctx, HNode *stmt, int root_block);

static void gvn_branch_body(GvnCtx *ctx, HNode *stmt) {
    if (stmt && stmt->kind == HK_BLOCK) {
        for (int i = 0; i < stmt->nitems; ++i)
            gvn_stmt(ctx, stmt->items[i], 0);
    } else {
        gvn_stmt(ctx, stmt, 0);
    }
}

/* A value-numbering fact may cross a control-flow join only when every
 * incoming path has the same expression available in the same holder.
 * This is deliberately stricter than expression equivalence alone: the
 * holder must dominate all later uses and its value must agree on every
 * predecessor. */
static int gvn_entry_same(const GvnEntry *a, const GvnEntry *b) {
    return a->valid && b->valid && a->holder == b->holder &&
           a->hash == b->hash && gvn_equal(a->expr, b->expr);
}

static void gvn_join_entries(GvnEntry *merged, const GvnEntry *incoming,
                             int count) {
    for (int i = 0; i < count; ++i)
        if (!gvn_entry_same(&merged[i], &incoming[i]))
            merged[i].valid = 0;
}

/* Determine which local slots may be changed by a loop. Calls and
 * non-local stores are barriers because they may mutate escaped locals.
 * This intentionally mirrors the conservative alias policy used by
 * propagation; a false positive only loses an optimization. */
static void gvn_loop_writes(const HNode *n, unsigned char *writes,
                            int nslots) {
    if (!n) return;
    if (n->kind == HK_LET || n->kind == HK_FOR) {
        int slot = n->u.index;
        if (slot >= 0 && slot < nslots) writes[slot] = 1;
    }
    if (n->kind == HK_STORE) {
        const HNode *lhs = n->child[0];
        if (lhs && (lhs->kind == HK_LOCAL || lhs->kind == HK_PARAM)) {
            int slot = lhs->u.index;
            if (slot >= 0 && slot < nslots) writes[slot] = 1;
        } else {
            memset(writes, 1, (size_t)nslots);
        }
    }
    switch (n->kind) {
    case HK_CALL:
    case HK_ADDR:
    case HK_DEREF:
    case HK_ALLOC:
    case HK_FREE:
    case HK_INLINE_ASM:
    case HK_ASYNC:
    case HK_GO_ROUTINE:
    case HK_AWAIT:
    case HK_SEND:
    case HK_RECV:
    case HK_CLOSE:
    case HK_SELECT:
        memset(writes, 1, (size_t)nslots);
        break;
    default:
        break;
    }
    if (n->kind == HK_UNOP &&
        (n->u.unop == HUN_DEREF || n->u.unop == HOP_ADDR_OF))
        memset(writes, 1, (size_t)nslots);
    for (int i = 0; i < 4; ++i)
        gvn_loop_writes(n->child[i], writes, nslots);
    for (int i = 0; i < n->nitems; ++i)
        gvn_loop_writes(n->items[i], writes, nslots);
}

static void gvn_kill_writes(GvnCtx *ctx, const unsigned char *writes) {
    if (ctx->dep_failed || !ctx->dep_heads) {
        for (int i = 0; i < ctx->nslots; ++i) {
            GvnEntry *e = &ctx->entries[i];
            if (!e->valid) continue;
            if (writes[e->holder]) {
                gvn_invalidate_holder(ctx, i);
                continue;
            }
            for (int slot = 0; slot < ctx->nslots; ++slot) {
                if (writes[slot] && gvn_reads_slot(e->expr, slot)) {
                    gvn_invalidate_holder(ctx, i);
                    break;
                }
            }
        }
        return;
    }
    for (int slot = 0; slot < ctx->nslots; ++slot)
        if (writes[slot]) gvn_invalidate_slot(ctx, slot);
}

static void gvn_loop(GvnCtx *ctx, HNode *stmt) {
    if (ctx->nslots <= 0) {
        gvn_clear(ctx);
        for (int i = 0; i < 4; ++i)
            gvn_expr(ctx, &stmt->child[i], -1, 0, 0);
        for (int i = 0; i < stmt->nitems; ++i)
            gvn_stmt(ctx, stmt->items[i], 0);
        gvn_clear(ctx);
        return;
    }

    unsigned char *writes = calloc((size_t)ctx->nslots, 1);
    GvnEntry *entry = malloc((size_t)ctx->nslots * sizeof(*entry));
    if (!writes || !entry) {
        free(writes); free(entry);
        gvn_clear(ctx);
        for (int i = 0; i < 4; ++i)
            gvn_expr(ctx, &stmt->child[i], -1, 0, 0);
        for (int i = 0; i < stmt->nitems; ++i)
            gvn_stmt(ctx, stmt->items[i], 0);
        gvn_clear(ctx);
        return;
    }

    HNode *body = NULL;
    if (stmt->kind == HK_WHILE) {
        body = stmt->child[1];
    } else if (stmt->kind == HK_DOWHILE) {
        body = stmt->child[0];
    } else {
        /* for-range bounds are evaluated once, before the loop. */
        gvn_expr(ctx, &stmt->child[0], -1, 1, 1);
        gvn_expr(ctx, &stmt->child[1], -1, 1, 1);
        body = stmt->child[2];
        if (stmt->u.index >= 0 && stmt->u.index < ctx->nslots)
            writes[stmt->u.index] = 1;
    }

    gvn_loop_writes(body, writes, ctx->nslots);
    memcpy(entry, ctx->entries,
           (size_t)ctx->nslots * sizeof(*entry));
    gvn_kill_writes(ctx, writes);

    if (stmt->kind == HK_WHILE)
        gvn_expr(ctx, &stmt->child[0], -1, 1, 1);
    if (body) gvn_branch_body(ctx, body);
    if (stmt->kind == HK_DOWHILE)
        gvn_expr(ctx, &stmt->child[1], -1, 1, 1);

    /* A loop may execute zero or many times. Only facts that were already
     * available on entry and whose holder and operands are loop-invariant
     * may leave the loop. Body-created facts are intentionally discarded. */
    memcpy(ctx->entries, entry,
           (size_t)ctx->nslots * sizeof(*entry));
    gvn_index_rebuild(ctx);
    gvn_dep_rebuild(ctx);
    gvn_kill_writes(ctx, writes);
    free(entry);
    free(writes);
}

static void gvn_branch_join(GvnCtx *ctx, HNode *const *branches,
                            int branch_count) {
    if (ctx->nslots <= 0) {
        for (int i = 0; i < branch_count; ++i)
            gvn_branch_body(ctx, branches[i]);
        return;
    }

    size_t bytes = (size_t)ctx->nslots * sizeof(*ctx->entries);
    GvnEntry *base = malloc(bytes);
    GvnEntry *merged = malloc(bytes);
    GvnEntry *branch = malloc(bytes);
    if (!base || !merged || !branch) {
        free(base); free(merged); free(branch);
        gvn_clear(ctx);
        for (int i = 0; i < branch_count; ++i) {
            gvn_stmt(ctx, branches[i], 0);
            gvn_clear(ctx);
        }
        return;
    }

    memcpy(base, ctx->entries, bytes);
    memcpy(merged, base, bytes);
    for (int i = 0; i < branch_count; ++i) {
        memcpy(ctx->entries, base, bytes);
        gvn_index_rebuild(ctx);
        gvn_dep_rebuild(ctx);
        if (branches[i]) gvn_branch_body(ctx, branches[i]);
        memcpy(branch, ctx->entries, bytes);
        gvn_join_entries(merged, branch, ctx->nslots);
    }
    memcpy(ctx->entries, merged, bytes);
    gvn_index_rebuild(ctx);
    gvn_dep_rebuild(ctx);
    free(base); free(merged); free(branch);
}

static void gvn_block(GvnCtx *ctx, HNode *block, int root_block) {
    if (!block || block->kind != HK_BLOCK) return;
    for (int i = 0; i < block->nitems; ++i)
        gvn_stmt(ctx, block->items[i], 0);
    if (!root_block) gvn_clear(ctx);
}

static void gvn_stmt(GvnCtx *ctx, HNode *stmt, int root_block) {
    if (!stmt) return;
    switch (stmt->kind) {
    case HK_BLOCK:
        gvn_block(ctx, stmt, root_block);
        return;
    case HK_LET: {
        int dest = stmt->u.index;
        gvn_expr(ctx, &stmt->child[1], dest, 1, 1);
        gvn_invalidate_slot(ctx, dest);
        gvn_record(ctx, stmt->child[1], dest);
        return;
    }
    case HK_STORE: {
        HNode *lhs = stmt->child[0];
        int dest = lhs && (lhs->kind == HK_LOCAL || lhs->kind == HK_PARAM)
                       ? lhs->u.index : -1;
        if (dest < 0) {
            gvn_clear(ctx);
            gvn_expr(ctx, &stmt->child[0], -1, 0, 0);
            gvn_expr(ctx, &stmt->child[1], -1, 0, 0);
            gvn_clear(ctx);
            return;
        }
        gvn_expr(ctx, &stmt->child[1], dest, 1, 1);
        gvn_invalidate_slot(ctx, dest);
        gvn_record(ctx, stmt->child[1], dest);
        return;
    }
    case HK_EXPR_STMT:
    case HK_RETURN:
        gvn_expr(ctx, &stmt->child[0], -1, 1, 1);
        return;
    case HK_IF: {
        gvn_expr(ctx, &stmt->child[0], -1, 1, 1);
        HNode *branches[2] = { stmt->child[1], stmt->child[2] };
        gvn_branch_join(ctx, branches, 2);
        return;
    }
    case HK_MATCH: {
        gvn_expr(ctx, &stmt->child[0], -1, 1, 1);
        int count = stmt->nitems + 1;
        HNode **branches = calloc((size_t)count, sizeof(*branches));
        if (!branches) {
            gvn_clear(ctx);
            for (int i = 0; i < stmt->nitems; ++i) {
                gvn_stmt(ctx, stmt->items[i], 0);
                gvn_clear(ctx);
            }
            gvn_stmt(ctx, stmt->child[2], 0);
            gvn_clear(ctx);
            return;
        }
        for (int i = 0; i < stmt->nitems; ++i)
            branches[i] = stmt->items[i] ? stmt->items[i]->child[1] : NULL;
        branches[count - 1] = stmt->child[2];
        gvn_branch_join(ctx, branches, count);
        free(branches);
        return;
    }
    case HK_WHILE:
    case HK_DOWHILE:
    case HK_FOR:
        gvn_loop(ctx, stmt);
        return;
    case HK_CALL:
    case HK_DEFER:
    case HK_INLINE_ASM:
    case HK_ASYNC:
    case HK_GO_ROUTINE:
    case HK_SELECT:
    case HK_SEND:
    case HK_RECV:
    case HK_CLOSE:
    case HK_AWAIT:
    case HK_ALLOC:
    case HK_FREE:
    case HK_PANIC:
    case HK_CATCH:
        gvn_clear(ctx);
        for (int i = 0; i < 4; ++i)
            gvn_expr(ctx, &stmt->child[i], -1, 0, 0);
        for (int i = 0; i < stmt->nitems; ++i)
            gvn_expr(ctx, &stmt->items[i], -1, 0, 0);
        gvn_clear(ctx);
        return;
    default:
        gvn_clear(ctx);
        for (int i = 0; i < 4; ++i)
            gvn_expr(ctx, &stmt->child[i], -1, 0, 0);
        for (int i = 0; i < stmt->nitems; ++i)
            gvn_expr(ctx, &stmt->items[i], -1, 0, 0);
        gvn_clear(ctx);
        return;
    }
}

static void gvn_find_max_slot(const HNode *n, int *max_slot) {
    if (!n) return;
    if ((n->kind == HK_LOCAL || n->kind == HK_PARAM ||
         n->kind == HK_LET || n->kind == HK_FOR) &&
        n->u.index > *max_slot)
        *max_slot = n->u.index;
    for (int i = 0; i < 4; ++i)
        gvn_find_max_slot(n->child[i], max_slot);
    for (int i = 0; i < n->nitems; ++i)
        gvn_find_max_slot(n->items[i], max_slot);
}

long opt_gvn(HModule *m) {
    HNode *root = hir_root(m);
    if (!root) return 0;
    long changes = 0;
    for (int i = 0; i < root->nitems; ++i) {
        HNode *fn = root->items[i];
        if (!fn || fn->kind != HK_FUNC || !fn->child[3]) continue;
        int max_slot = -1;
        gvn_find_max_slot(fn, &max_slot);
        GvnCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.module = m;
        ctx.nslots = max_slot + 1;
        if (ctx.nslots > 0) {
            size_t buckets = 8;
            while (buckets < (size_t)ctx.nslots * 2 &&
                   buckets <= (size_t)INT_MAX / 2)
                buckets *= 2;
            ctx.nbuckets = (int)buckets;
            ctx.entries = calloc((size_t)ctx.nslots, sizeof(*ctx.entries));
            ctx.next = malloc((size_t)ctx.nslots * sizeof(*ctx.next));
            ctx.buckets = malloc(buckets * sizeof(*ctx.buckets));
            ctx.dep_heads = malloc((size_t)ctx.nslots * sizeof(*ctx.dep_heads));
            ctx.holder_heads = malloc((size_t)ctx.nslots * sizeof(*ctx.holder_heads));
            ctx.dep_free_head = -1;
            if (!ctx.entries || !ctx.next || !ctx.buckets) {
                free(ctx.entries); free(ctx.next); free(ctx.buckets);
                free(ctx.dep_heads); free(ctx.holder_heads);
                continue;
            }
            if (!ctx.dep_heads || !ctx.holder_heads) ctx.dep_failed = 1;
            for (int j = 0; j < ctx.nbuckets; ++j) ctx.buckets[j] = -1;
            for (int j = 0; j < ctx.nslots; ++j) {
                ctx.next[j] = -1;
                if (ctx.dep_heads) ctx.dep_heads[j] = -1;
                if (ctx.holder_heads) ctx.holder_heads[j] = -1;
            }
        }
        gvn_block(&ctx, fn->child[3], 1);
        changes += ctx.changes;
        free(ctx.entries);
        free(ctx.next);
        free(ctx.buckets);
        free(ctx.dep_heads);
        free(ctx.holder_heads);
        free(ctx.dep_next_slot);
        free(ctx.dep_prev_slot);
        free(ctx.dep_next_holder);
        free(ctx.dep_slot);
        free(ctx.dep_holder);
    }
    return changes;
}
