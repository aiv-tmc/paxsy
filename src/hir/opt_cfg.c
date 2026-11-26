#include "common.h"
#include <stdint.h>
#include <limits.h>
#include "hir.h"
#include "opt.h"

/* Structured statement-level CFG shared by reachability, dominator, and
 * scalar-local SSA analysis. Expressions remain attached to their owning
 * statement and are represented as ordered SSA use/definition records. */
typedef struct {
    HNode *stmt;
    int *succ;
    int nsucc;
    int cap;
    unsigned char reachable;
} CFGBlock;

typedef struct {
    uintptr_t stmt_key;
    unsigned char reachable;
} CFGStmtIndex;

typedef struct {
    CFGBlock *blocks;
    int count;
    int cap;
    int valid;
    int nrpo;
    /* Analysis data is owned by the CFG and remains available to every
     * analysis in this optimization invocation. */
    int **pred;
    int *npred;
    int *pred_cap;
    int *idom;
    int *rpo;
    int *rpo_index;
    /* Half-open dominator-tree DFS intervals: dominance queries are O(1). */
    int *dom_pre;
    int *dom_end;
    int **dom_children;
    int *ndom_children;
    int *dom_child_cap;
    int **frontier;
    int *nfrontier;
    int *frontier_cap;
    CFGStmtIndex *stmt_index;
    int nstmt_index;
} CFG;

typedef struct LoopTarget LoopTarget;
struct LoopTarget {
    int break_to;
    int continue_to;
    const char *label;
    const LoopTarget *parent;
};

static int cfg_new_block(CFG *g, HNode *stmt) {
    if (g->count == g->cap) {
        int cap = g->cap ? g->cap * 2 : 32;
        CFGBlock *p = realloc(g->blocks, (size_t)cap * sizeof(*p));
        if (!p) { g->valid = 0; return -1; }
        g->blocks = p;
        g->cap = cap;
    }
    int id = g->count++;
    memset(&g->blocks[id], 0, sizeof(g->blocks[id]));
    g->blocks[id].stmt = stmt;
    return id;
}

static void cfg_edge(CFG *g, int from, int to) {
    if (from < 0 || to < 0 || !g->valid) return;
    CFGBlock *b = &g->blocks[from];
    for (int i = 0; i < b->nsucc; ++i)
        if (b->succ[i] == to) return;
    if (b->nsucc == b->cap) {
        int cap = b->cap ? b->cap * 2 : 2;
        int *p = realloc(b->succ, (size_t)cap * sizeof(*p));
        if (!p) { g->valid = 0; return; }
        b->succ = p;
        b->cap = cap;
    }
    b->succ[b->nsucc++] = to;
}

static int find_loop_target(const LoopTarget *loop, const char *label,
                            int is_continue) {
    for (const LoopTarget *p = loop; p; p = p->parent) {
        if (!label || (p->label && strcmp(label, p->label) == 0))
            return is_continue ? p->continue_to : p->break_to;
    }
    return -1;
}

static int cfg_build_stmt(CFG *g, HNode *n, int next,
                          const LoopTarget *loop) {
    if (!n || !g->valid) return next;
    if (n->kind == HK_BLOCK) {
        int entry = next;
        for (int i = n->nitems - 1; i >= 0; --i)
            entry = cfg_build_stmt(g, n->items[i], entry, loop);
        return entry;
    }

    if (n->kind == HK_LABEL) {
        /* Labelled arbitrary jumps need a label-resolution pass. */
        g->valid = 0;
        return next;
    }
    if (n->kind == HK_DEFER) {
        /* Deferred actions add implicit edges at every scope exit. */
        g->valid = 0;
        return next;
    }

    int id = cfg_new_block(g, n);
    if (id < 0) return next;

    switch (n->kind) {
    case HK_RETURN:
        return id;
    case HK_BREAK: {
        int target = find_loop_target(loop, n->name, 0);
        if (target < 0) g->valid = 0;
        else cfg_edge(g, id, target);
        return id;
    }
    case HK_CONTINUE: {
        int target = find_loop_target(loop, n->name, 1);
        if (target < 0) g->valid = 0;
        else cfg_edge(g, id, target);
        return id;
    }
    case HK_IF: {
        int then_entry = cfg_build_stmt(g, n->child[1], next, loop);
        int else_entry = n->child[2]
            ? cfg_build_stmt(g, n->child[2], next, loop) : next;
        cfg_edge(g, id, then_entry);
        cfg_edge(g, id, else_entry);
        return id;
    }
    case HK_WHILE: {
        LoopTarget target = { next, id, n->name, loop };
        int body = cfg_build_stmt(g, n->child[1], id, &target);
        cfg_edge(g, id, body);
        cfg_edge(g, id, next);
        return id;
    }
    case HK_DOWHILE: {
        int cond = cfg_new_block(g, NULL);
        if (cond < 0) return id;
        LoopTarget target = { next, cond, n->name, loop };
        int body = cfg_build_stmt(g, n->child[0], cond, &target);
        cfg_edge(g, cond, body);
        cfg_edge(g, cond, next);
        cfg_edge(g, id, body);
        return id;
    }
    case HK_FOR: {
        LoopTarget target = { next, id, n->name, loop };
        int body = cfg_build_stmt(g, n->child[2], id, &target);
        cfg_edge(g, id, body);
        cfg_edge(g, id, next);
        return id;
    }
    case HK_MATCH: {
        for (int i = 0; i < n->nitems; ++i) {
            HNode *arm = n->items[i];
            int entry = arm ? cfg_build_stmt(g, arm->child[1], next, loop) : next;
            cfg_edge(g, id, entry);
        }
        int otherwise = n->child[2]
            ? cfg_build_stmt(g, n->child[2], next, loop) : next;
        cfg_edge(g, id, otherwise);
        return id;
    }
    default:
        cfg_edge(g, id, next);
        return id;
    }
}


static int cfg_dfs_rpo(CFG *g, int entry, unsigned char *seen,
                       int *order, int *norder) {
    int *nodes = malloc((size_t)g->count * sizeof(*nodes));
    int *edges = calloc((size_t)g->count, sizeof(*edges));
    if (!nodes || !edges) { free(nodes); free(edges); return 0; }
    int top = 0;
    nodes[top++] = entry;
    seen[entry] = 1;
    while (top > 0) {
        int v = nodes[top - 1];
        if (edges[top - 1] < g->blocks[v].nsucc) {
            int to = g->blocks[v].succ[edges[top - 1]++];
            if (!seen[to]) {
                seen[to] = 1;
                nodes[top] = to;
                edges[top] = 0;
                top++;
            }
        } else {
            order[(*norder)++] = v;
            top--;
        }
    }
    free(nodes); free(edges);
    return 1;
}

static int cfg_int_push(int **items, int *count, int *cap, int value) {
    if (*count == *cap) {
        int next = *cap ? *cap * 2 : 2;
        int *p = realloc(*items, (size_t)next * sizeof(*p));
        if (!p) return 0;
        *items = p; *cap = next;
    }
    (*items)[(*count)++] = value;
    return 1;
}

static int cfg_intersect(const CFG *g, int a, int b) {
    while (a != b) {
        while (g->rpo_index[a] > g->rpo_index[b]) a = g->idom[a];
        while (g->rpo_index[b] > g->rpo_index[a]) b = g->idom[b];
    }
    return a;
}

/* Cooper-Harvey-Kennedy dominators. The same persistent graph owns
 * predecessors, immediate dominators, dominator-tree children and
 * dominance frontiers; later SSA placement consumes these arrays. */
static int cfg_compute_dominators(CFG *g, int entry) {
    int n = g->count;
    if (!g->valid || n <= 0 || entry < 0) return 0;
    g->pred = calloc((size_t)n, sizeof(*g->pred));
    g->npred = calloc((size_t)n, sizeof(*g->npred));
    g->pred_cap = calloc((size_t)n, sizeof(*g->pred_cap));
    g->idom = malloc((size_t)n * sizeof(*g->idom));
    g->rpo = malloc((size_t)n * sizeof(*g->rpo));
    g->rpo_index = malloc((size_t)n * sizeof(*g->rpo_index));
    g->dom_children = calloc((size_t)n, sizeof(*g->dom_children));
    g->ndom_children = calloc((size_t)n, sizeof(*g->ndom_children));
    g->dom_child_cap = calloc((size_t)n, sizeof(*g->dom_child_cap));
    g->frontier = calloc((size_t)n, sizeof(*g->frontier));
    g->nfrontier = calloc((size_t)n, sizeof(*g->nfrontier));
    g->frontier_cap = calloc((size_t)n, sizeof(*g->frontier_cap));
    unsigned char *seen = calloc((size_t)n, 1);
    int *post = malloc((size_t)n * sizeof(*post));
    if (!g->pred || !g->npred || !g->pred_cap || !g->idom || !g->rpo ||
        !g->rpo_index || !g->dom_children || !g->ndom_children ||
        !g->dom_child_cap || !g->frontier || !g->nfrontier ||
        !g->frontier_cap || !seen || !post) {
        free(seen); free(post); return 0;
    }
    for (int i = 0; i < n; ++i) g->idom[i] = -1;
    for (int from = 0; from < n; ++from)
        for (int j = 0; j < g->blocks[from].nsucc; ++j) {
            int to = g->blocks[from].succ[j];
            if (!cfg_int_push(&g->pred[to], &g->npred[to],
                              &g->pred_cap[to], from)) {
                free(seen); free(post); return 0;
            }
        }
    int npost = 0;
    if (!cfg_dfs_rpo(g, entry, seen, post, &npost)) {
        free(seen); free(post); return 0;
    }
    for (int i = 0; i < npost; ++i) {
        g->rpo[i] = post[npost - i - 1];
        g->rpo_index[g->rpo[i]] = i;
    }
    g->nrpo = npost;
    g->idom[entry] = entry;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int ri = 1; ri < npost; ++ri) {
            int b = g->rpo[ri], new_idom = -1;
            for (int j = 0; j < g->npred[b]; ++j) {
                int p = g->pred[b][j];
                if (g->idom[p] < 0) continue;
                new_idom = new_idom < 0 ? p : cfg_intersect(g, p, new_idom);
            }
            if (new_idom >= 0 && g->idom[b] != new_idom) {
                g->idom[b] = new_idom; changed = 1;
            }
        }
    }
    for (int ri = 1; ri < npost; ++ri) {
        int b = g->rpo[ri], parent = g->idom[b];
        if (parent >= 0 && !cfg_int_push(&g->dom_children[parent],
                &g->ndom_children[parent], &g->dom_child_cap[parent], b)) {
            free(seen); free(post); return 0;
        }
    }
    /* Number the dominator tree with half-open DFS intervals. A block d
     * dominates b exactly when b's preorder number lies in d's subtree
     * interval. This replaces repeated idom-chain walks in SSA verification
     * with constant-time queries. The explicit frame stack avoids recursion
     * on machine-generated, deeply nested CFGs. */
    g->dom_pre = malloc((size_t)n * sizeof(*g->dom_pre));
    g->dom_end = malloc((size_t)n * sizeof(*g->dom_end));
    int *dom_stack = malloc((size_t)n * sizeof(*dom_stack));
    int *dom_next = calloc((size_t)n, sizeof(*dom_next));
    if (!g->dom_pre || !g->dom_end || !dom_stack || !dom_next) {
        free(dom_stack); free(dom_next); free(seen); free(post); return 0;
    }
    for (int i = 0; i < n; ++i) g->dom_pre[i] = g->dom_end[i] = -1;
    int dom_clock = 0, dom_top = 0;
    dom_stack[dom_top++] = entry;
    g->dom_pre[entry] = dom_clock++;
    while (dom_top > 0) {
        int b = dom_stack[dom_top - 1];
        if (dom_next[b] < g->ndom_children[b]) {
            int child = g->dom_children[b][dom_next[b]++];
            if (child < 0 || child >= n || g->dom_pre[child] >= 0) {
                free(dom_stack); free(dom_next); free(seen); free(post); return 0;
            }
            g->dom_pre[child] = dom_clock++;
            dom_stack[dom_top++] = child;
        } else {
            g->dom_end[b] = dom_clock;
            --dom_top;
        }
    }
    free(dom_stack);
    free(dom_next);

    /* Dominance frontiers are defined over the entry-reachable graph.
     * The CFG builder intentionally keeps unreachable source blocks until
     * pruning, so unreachable predecessors must not contribute. A per-runner
     * generation stamp replaces a linear scan of the already-built frontier
     * list: for a given target block b, each runner is appended at most once. */
    int *frontier_stamp = malloc((size_t)n * sizeof(*frontier_stamp));
    if (!frontier_stamp) { free(seen); free(post); return 0; }
    for (int i = 0; i < n; ++i) frontier_stamp[i] = -1;
    for (int b = 0; b < n; ++b) {
        if (!g->blocks[b].reachable || g->npred[b] < 2 || g->idom[b] < 0)
            continue;
        for (int j = 0; j < g->npred[b]; ++j) {
            int runner = g->pred[b][j];
            if (runner < 0 || !g->blocks[runner].reachable) continue;
            while (runner >= 0 && runner != g->idom[b]) {
                if (frontier_stamp[runner] != b) {
                    frontier_stamp[runner] = b;
                    if (!cfg_int_push(&g->frontier[runner],
                            &g->nfrontier[runner], &g->frontier_cap[runner], b)) {
                        free(frontier_stamp); free(seen); free(post); return 0;
                    }
                }
                if (g->idom[runner] < 0 || g->idom[runner] == runner) break;
                runner = g->idom[runner];
            }
        }
    }
    free(frontier_stamp);
    free(seen); free(post);
    return 1;
}

/* Keep the dominator representation honest before later passes consume it.
 * Every reachable non-entry node must have a reachable immediate dominator,
 * and every dominator-tree edge must agree with the idom array. */
static int cfg_verify_dominators(const CFG *g, int entry) {
    if (!g || !g->valid || entry < 0 || entry >= g->count ||
        !g->blocks[entry].reachable || g->idom[entry] != entry)
        return 0;

    /* Verify the dominator tree in one traversal. The former verifier walked
     * the idom chain independently for every block, which is quadratic for a
     * deep, mostly linear CFG. A tree walk checks parent/child consistency
     * and reachability in O(V + E_dom). */
    unsigned char *seen = calloc((size_t)g->count, 1);
    int *stack = malloc((size_t)g->count * sizeof(*stack));
    if (!seen || !stack) { free(seen); free(stack); return 0; }
    int top = 0, visited = 0;
    stack[top++] = entry;
    seen[entry] = 1;
    while (top > 0) {
        int b = stack[--top];
        ++visited;
        for (int i = 0; i < g->ndom_children[b]; ++i) {
            int child = g->dom_children[b][i];
            if (child < 0 || child >= g->count ||
                !g->blocks[child].reachable || g->idom[child] != b ||
                seen[child]) {
                free(seen); free(stack); return 0;
            }
            seen[child] = 1;
            stack[top++] = child;
        }
        for (int i = 0; i < g->nfrontier[b]; ++i) {
            int target = g->frontier[b][i];
            if (target < 0 || target >= g->count ||
                !g->blocks[target].reachable) {
                free(seen); free(stack); return 0;
            }
        }
    }
    for (int b = 0; b < g->count; ++b) {
        if (g->blocks[b].reachable != (seen[b] != 0)) {
            free(seen); free(stack); return 0;
        }
        if (b != entry && g->blocks[b].reachable &&
            (g->idom[b] < 0 || g->idom[b] >= g->count || g->idom[b] == b)) {
            free(seen); free(stack); return 0;
        }
    }
    free(seen); free(stack);
    return visited == g->nrpo;
}

static int cfg_mark_reachable(CFG *g, int entry) {
    if (entry < 0 || !g->valid) return 0;
    int *queue = malloc((size_t)g->count * sizeof(*queue));
    if (!queue) return 0;
    int head = 0, tail = 0;
    g->blocks[entry].reachable = 1;
    queue[tail++] = entry;
    while (head < tail) {
        int id = queue[head++];
        CFGBlock *b = &g->blocks[id];
        for (int i = 0; i < b->nsucc; ++i) {
            int to = b->succ[i];
            if (!g->blocks[to].reachable) {
                g->blocks[to].reachable = 1;
                queue[tail++] = to;
            }
        }
    }
    free(queue);
    return 1;
}

static int cfg_stmt_index_cmp(const void *a, const void *b) {
    uintptr_t ka = ((const CFGStmtIndex *)a)->stmt_key;
    uintptr_t kb = ((const CFGStmtIndex *)b)->stmt_key;
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

/* Build one pointer-sorted index after reachability is known. Pruning asks
 * whether each nested statement belongs to a reachable CFG block; scanning
 * all blocks for every statement made this phase quadratic on large HIRs. */
static int cfg_build_stmt_index(CFG *g) {
    if (g->count <= 0) return 1;
    g->stmt_index = malloc((size_t)g->count * sizeof(*g->stmt_index));
    if (!g->stmt_index) return 0;
    for (int i = 0; i < g->count; ++i) {
        const HNode *stmt = g->blocks[i].stmt;
        if (!stmt) continue;
        g->stmt_index[g->nstmt_index++] = (CFGStmtIndex){
            (uintptr_t)stmt, g->blocks[i].reachable
        };
    }
    qsort(g->stmt_index, (size_t)g->nstmt_index,
          sizeof(*g->stmt_index), cfg_stmt_index_cmp);
    /* A HIR statement should map to one CFG block. Merge duplicate keys
     * defensively so an unusual shared node is retained if any block that
     * references it is reachable. */
    int out = 0;
    for (int i = 0; i < g->nstmt_index; ++i) {
        if (out && g->stmt_index[out - 1].stmt_key ==
                   g->stmt_index[i].stmt_key) {
            g->stmt_index[out - 1].reachable |= g->stmt_index[i].reachable;
        } else {
            g->stmt_index[out++] = g->stmt_index[i];
        }
    }
    g->nstmt_index = out;
    return 1;
}

static int cfg_stmt_reachable(const CFG *g, const HNode *stmt) {
    if (!stmt) return 1;
    uintptr_t key = (uintptr_t)stmt;
    int lo = 0, hi = g->nstmt_index;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (g->stmt_index[mid].stmt_key < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= g->nstmt_index || g->stmt_index[lo].stmt_key != key)
        return 1;
    return g->stmt_index[lo].reachable != 0;
}

static int cfg_prune(CFG *g, HNode *n) {
    if (!n) return 0;
    int changes = 0;
    for (int i = 0; i < 4; ++i) {
        HNode *child = n->child[i];
        if (child && cfg_stmt_reachable(g, child))
            changes += cfg_prune(g, child);
        else if (child) {
            n->child[i] = NULL;
            changes++;
        }
    }
    int w = 0;
    for (int i = 0; i < n->nitems; ++i) {
        HNode *item = n->items[i];
        if (!item) continue;
        if (!cfg_stmt_reachable(g, item)) {
            changes++;
            continue;
        }
        changes += cfg_prune(g, item);
        n->items[w++] = item;
    }
    n->nitems = w;
    return changes;
}

/* ------------------------------------------------------------------ */
/* Scalar-local SSA construction                                      */
/*                                                                    */
/* This is a real, temporary SSA IR layered over the function CFG.    */
/* Each local definition receives a unique value number, uses are     */
/* rewritten to those numbers in the SSA records, and join points     */
/* receive explicit Phi records from iterated dominance frontiers.   */
/* The source HIR remains immutable: emitters continue to consume HIR  */
/* until the SSA instruction selection/backend migration is complete. */
/* ------------------------------------------------------------------ */
typedef struct {
    int slot;
    int value;
    int *preds;
    int *incoming;
    int nincoming;
    int cap;
} SSAPhi;

typedef struct {
    int slot;
    int value;
    int block;
    HNode *stmt;             /* HIR statement that owns this definition. */
    HNode *expr;             /* RHS that defines this SSA value, if any. */
} SSADef;

typedef struct {
    int slot;
    int value;
    HNode *node;             /* Original HIR local/parameter read. */
} SSAUse;

typedef struct {
    uintptr_t node_key;
    int value;
} SSAUseIndex;

typedef struct {
    int value;
    int block;
    HNode *node;
} SSAValueUseIndex;

typedef struct {
    int a;
    int b;
} SSAInterferenceEdge;

typedef struct {
    int node;
    uint64_t version;
    long double priority;
} SSAColorHeapItem;

typedef struct {
    SSAPhi *phis;
    int nphis, phi_cap;
    SSADef *defs;
    int ndefs, def_cap;
    SSAUse *uses;
    int nuses, use_cap;
} SSABlock;

typedef struct {
    CFG *cfg;
    SSABlock *blocks;
    int entry;
    int nslots;
    int nparams;
    int next_value;
    int valid;
    unsigned char *has_def;
    unsigned char *placed;
    int *current;
    int *rename_slots;
    int *rename_values;
    int nrename_log, rename_log_cap;
    int *aliases;             /* canonical SSA value for copy/trivial-Phi values */
    unsigned *use_count;     /* ordinary uses plus Phi-edge operands */
    SSAUseIndex *use_index;  /* sorted node-pointer -> SSA value lookup */
    int nuse_index;
    SSAValueUseIndex *value_use_index; /* sorted canonical value -> ordinary uses */
    int nvalue_use_index;
    int single_use_phis;
    int simplified_copies;
    int simplified_phis;
    int sunk_exprs;
    int dead_defs;
    uint64_t *live_in;
    uint64_t *live_out;
    int live_words;
    int peak_live;
    SSAInterferenceEdge *interference;
    int ninterference;
    int interference_cap;
    /* Open-addressed membership set prevents repeated live-range points
     * from appending the same edge many times before the final sort. */
    uint64_t *interference_set;
    size_t interference_set_cap;
    int max_interference_degree;
    int abstract_color_count;
    int abstract_spill_count;
} SSA;

static int ssa_push_int(int **p, int *n, int *cap, int v) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 4;
        int *q = realloc(*p, (size_t)nc * sizeof(*q));
        if (!q) return 0;
        *p = q; *cap = nc;
    }
    (*p)[(*n)++] = v;
    return 1;
}

static int ssa_add_phi(SSABlock *b, int slot) {
    if (b->nphis == b->phi_cap) {
        int nc = b->phi_cap ? b->phi_cap * 2 : 4;
        SSAPhi *p = realloc(b->phis, (size_t)nc * sizeof(*p));
        if (!p) return 0;
        b->phis = p; b->phi_cap = nc;
    }
    SSAPhi *p = &b->phis[b->nphis++];
    memset(p, 0, sizeof(*p));
    p->slot = slot;
    p->value = -1;
    return 1;
}

static int ssa_add_def(SSABlock *b, int block, int slot, HNode *stmt, HNode *expr) {
    if (b->ndefs == b->def_cap) {
        int nc = b->def_cap ? b->def_cap * 2 : 4;
        SSADef *p = realloc(b->defs, (size_t)nc * sizeof(*p));
        if (!p) return 0;
        b->defs = p; b->def_cap = nc;
    }
    b->defs[b->ndefs++] = (SSADef){ slot, -1, block, stmt, expr };
    return 1;
}

static int ssa_local_ref(const HNode *n) {
    return n && (n->kind == HK_LOCAL || n->kind == HK_PARAM);
}

static int ssa_add_use(SSABlock *b, int slot, HNode *node) {
    if (b->nuses == b->use_cap) {
        int nc = b->use_cap ? b->use_cap * 2 : 8;
        SSAUse *p = realloc(b->uses, (size_t)nc * sizeof(*p));
        if (!p) return 0;
        b->uses = p; b->use_cap = nc;
    }
    b->uses[b->nuses++] = (SSAUse){ slot, -1, node };
    return 1;
}

static int ssa_is_stmt(HNodeKind k) {
    return k == HK_BLOCK || k == HK_LET || k == HK_STORE || k == HK_IF ||
        k == HK_WHILE || k == HK_DOWHILE || k == HK_FOR || k == HK_MATCH ||
        k == HK_RETURN || k == HK_BREAK || k == HK_CONTINUE ||
        k == HK_DEFER || k == HK_EXPR_STMT || k == HK_LABEL ||
        k == HK_SEND || k == HK_RECV || k == HK_SELECT || k == HK_CLOSE ||
        k == HK_GO_ROUTINE || k == HK_ASYNC;
}

/* Visit expression operands but never descend into nested statement bodies:
 * those belong to their own CFG blocks and must not leak uses across edges. */
static void ssa_expr_uses(SSA *s, SSABlock *b, HNode *n) {
    if (!n || !s->valid) return;
    if (n->kind == HK_LOCAL || n->kind == HK_PARAM) {
        if (n->u.index >= 0 && n->u.index < s->nslots &&
            !ssa_add_use(b, n->u.index, n)) s->valid = 0;
        return;
    }
    if (ssa_is_stmt(n->kind)) return;
    for (int i = 0; i < 4; ++i) ssa_expr_uses(s, b, n->child[i]);
    for (int i = 0; i < n->nitems; ++i) ssa_expr_uses(s, b, n->items[i]);
}

static void ssa_scan_max_slot(HNode *n, int *max_slot) {
    if (!n) return;
    if ((n->kind == HK_LOCAL || n->kind == HK_PARAM ||
         n->kind == HK_LET || n->kind == HK_FOR) &&
        n->u.index > *max_slot) *max_slot = n->u.index;
    for (int i = 0; i < 4; ++i) ssa_scan_max_slot(n->child[i], max_slot);
    for (int i = 0; i < n->nitems; ++i) ssa_scan_max_slot(n->items[i], max_slot);
}

static void ssa_collect_block(SSA *s, int bi) {
    HNode *n = s->cfg->blocks[bi].stmt;
    SSABlock *b = &s->blocks[bi];
    if (!n) return;
    switch (n->kind) {
    case HK_LET:
        ssa_expr_uses(s, b, n->child[1]);
        if (n->u.index >= 0 && n->u.index < s->nslots &&
            !ssa_add_def(b, bi, n->u.index, n, n->child[1])) s->valid = 0;
        break;
    case HK_STORE:
        if (ssa_local_ref(n->child[0])) {
            ssa_expr_uses(s, b, n->child[1]);
            int slot = n->child[0]->u.index;
            if (slot >= 0 && slot < s->nslots && !ssa_add_def(b, bi, slot, n, n->child[1]))
                s->valid = 0;
        } else {
            ssa_expr_uses(s, b, n->child[0]);
            ssa_expr_uses(s, b, n->child[1]);
        }
        break;
    case HK_EXPR_STMT:
        if (n->child[0] && n->child[0]->kind == HK_STORE) {
            HNode *st = n->child[0];
            if (ssa_local_ref(st->child[0])) {
                ssa_expr_uses(s, b, st->child[1]);
                int slot = st->child[0]->u.index;
                if (slot >= 0 && slot < s->nslots && !ssa_add_def(b, bi, slot, n, st->child[1]))
                    s->valid = 0;
            } else {
                ssa_expr_uses(s, b, st->child[0]);
                ssa_expr_uses(s, b, st->child[1]);
            }
        } else {
            ssa_expr_uses(s, b, n->child[0]);
        }
        break;
    case HK_FOR:
        ssa_expr_uses(s, b, n->child[0]);
        ssa_expr_uses(s, b, n->child[1]);
        if (n->u.index >= 0 && n->u.index < s->nslots &&
            !ssa_add_def(b, bi, n->u.index, n, n->child[1])) s->valid = 0;
        break;
    case HK_IF:
    case HK_WHILE:
        ssa_expr_uses(s, b, n->child[0]);
        break;
    case HK_DOWHILE:
        ssa_expr_uses(s, b, n->child[1]);
        break;
    case HK_MATCH:
        ssa_expr_uses(s, b, n->child[0]);
        break;
    case HK_RETURN: case HK_SEND: case HK_RECV:
    case HK_CLOSE: case HK_AWAIT: case HK_PANIC:
        for (int i = 0; i < 4; ++i) ssa_expr_uses(s, b, n->child[i]);
        break;
    default:
        break;
    }
    for (int i = 0; i < b->ndefs; ++i) {
        int slot = b->defs[i].slot;
        s->has_def[(size_t)bi * (size_t)s->nslots + (size_t)slot] = 1;
    }
}

static size_t ssa_slot_bit(const SSA *s, int block, int slot) {
    return (size_t)block * (size_t)s->nslots + (size_t)slot;
}

/* Iterated dominance-frontier placement. `placed` is a dense membership
 * index parallel to has_def: checking whether a Phi already exists is O(1)
 * instead of scanning the block's growing Phi vector for every frontier edge. */
static void ssa_place_phis(SSA *s) {
    CFG *g = s->cfg;
    int *work = NULL;
    int work_cap = 0;
    unsigned char *queued = calloc((size_t)g->count, 1);
    unsigned *def_blocks = calloc((size_t)s->nslots, sizeof(*def_blocks));
    if (!queued || !def_blocks) {
        free(queued); free(def_blocks); s->valid = 0; return;
    }

    /* A slot defined in only one reachable block cannot need a merge Phi.
     * Count blocks, not individual assignments: multiple assignments in one
     * block are already ordered by the rename pass. Parameters count as defs. */
    for (int b = 0; b < g->count; ++b) {
        if (!g->blocks[b].reachable) continue;
        for (int slot = 0; slot < s->nslots; ++slot)
            if (s->has_def[ssa_slot_bit(s, b, slot)] &&
                def_blocks[slot] != UINT_MAX)
                def_blocks[slot]++;
    }

    for (int slot = 0; slot < s->nslots && s->valid; ++slot) {
        if (def_blocks[slot] < 2) continue;
        memset(queued, 0, (size_t)g->count);
        int nw = 0;
        for (int b = 0; b < g->count; ++b)
            if (g->blocks[b].reachable && s->has_def[ssa_slot_bit(s, b, slot)]) {
                if (!ssa_push_int(&work, &nw, &work_cap, b)) {
                    s->valid = 0; break;
                }
                queued[b] = 1;
            }
        while (nw && s->valid) {
            int x = work[--nw]; queued[x] = 0;
            for (int j = 0; j < g->nfrontier[x]; ++j) {
                int y = g->frontier[x][j];
                size_t bit = ssa_slot_bit(s, y, slot);
                if (!g->blocks[y].reachable || s->placed[bit]) continue;
                if (!ssa_add_phi(&s->blocks[y], slot)) { s->valid = 0; break; }
                s->placed[bit] = 1;
                if (!s->has_def[bit] && !queued[y]) {
                    if (!ssa_push_int(&work, &nw, &work_cap, y)) {
                        s->valid = 0; break;
                    }
                    queued[y] = 1;
                }
            }
        }
    }
    free(work); free(queued); free(def_blocks);
}

static int ssa_new_value(SSA *s) { return s->next_value++; }

static int ssa_log_current(SSA *s, int slot) {
    if (s->nrename_log == s->rename_log_cap) {
        int cap = s->rename_log_cap ? s->rename_log_cap * 2 : 32;
        if (cap < s->rename_log_cap ||
            (size_t)cap > SIZE_MAX / sizeof(*s->rename_slots)) return 0;
        int *slots = realloc(s->rename_slots, (size_t)cap * sizeof(*slots));
        if (!slots) return 0;
        s->rename_slots = slots;
        int *values = realloc(s->rename_values, (size_t)cap * sizeof(*values));
        if (!values) return 0;
        s->rename_values = values;
        s->rename_log_cap = cap;
    }
    s->rename_slots[s->nrename_log] = slot;
    s->rename_values[s->nrename_log] = s->current[slot];
    s->nrename_log++;
    return 1;
}

typedef struct {
    int block;
    int next_child;
    int checkpoint;
    unsigned char entered;
} SSARenameFrame;

/* Rename the dominator tree iteratively. A recursive walk used one C stack
 * frame per CFG block, so a long straight-line function could exhaust the
 * process stack even though the CFG and all analysis arrays fit in memory.
 * Each explicit frame preserves the same per-subtree rename-log checkpoint:
 * descendants see their parent's definitions, then restore only their own
 * changes before the parent continues with its next child. */
static void ssa_rename(SSA *s, int entry) {
    if (!s->valid || entry < 0 || entry >= s->cfg->count) return;
    CFG *g = s->cfg;
    size_t capacity = (size_t)g->nrpo;
    if (!capacity || capacity > SIZE_MAX / sizeof(SSARenameFrame)) {
        s->valid = 0;
        return;
    }
    SSARenameFrame *stack = malloc(capacity * sizeof(*stack));
    if (!stack) { s->valid = 0; return; }
    size_t top = 1;
    stack[0] = (SSARenameFrame){ entry, 0, 0, 0 };

    while (top && s->valid) {
        SSARenameFrame *frame = &stack[top - 1];
        int bi = frame->block;
        if (!frame->entered) {
            SSABlock *b = &s->blocks[bi];
            frame->checkpoint = s->nrename_log;
            frame->entered = 1;

            for (int i = 0; i < b->nphis; ++i) {
                SSAPhi *p = &b->phis[i];
                if (!ssa_log_current(s, p->slot)) { s->valid = 0; break; }
                p->value = ssa_new_value(s);
                s->current[p->slot] = p->value;
            }
            for (int i = 0; i < b->nuses && s->valid; ++i) {
                b->uses[i].value = s->current[b->uses[i].slot];
                if (b->uses[i].value < 0) s->valid = 0;
            }
            for (int i = 0; i < b->ndefs && s->valid; ++i) {
                int slot = b->defs[i].slot;
                if (!ssa_log_current(s, slot)) { s->valid = 0; break; }
                b->defs[i].value = ssa_new_value(s);
                s->current[slot] = b->defs[i].value;
            }
            for (int si = 0; si < g->blocks[bi].nsucc && s->valid; ++si) {
                int succ = g->blocks[bi].succ[si];
                if (!g->blocks[succ].reachable) continue;
                SSABlock *sb = &s->blocks[succ];
                for (int pi = 0; pi < sb->nphis; ++pi) {
                    SSAPhi *p = &sb->phis[pi];
                    int cap = p->cap;
                    if (!ssa_push_int(&p->preds, &p->nincoming, &cap, bi)) {
                        s->valid = 0; break;
                    }
                    /* Keep parallel arrays with identical capacity/order. */
                    p->cap = cap;
                    int *in = realloc(p->incoming,
                                      (size_t)p->cap * sizeof(*in));
                    if (!in) { s->valid = 0; break; }
                    p->incoming = in;
                    p->incoming[p->nincoming - 1] = s->current[p->slot];
                    if (p->incoming[p->nincoming - 1] < 0) s->valid = 0;
                }
            }
        }

        if (s->valid && frame->next_child < g->ndom_children[bi]) {
            int child = g->dom_children[bi][frame->next_child++];
            if (top >= capacity || child < 0 || child >= g->count) {
                s->valid = 0;
                continue;
            }
            stack[top++] = (SSARenameFrame){ child, 0, 0, 0 };
            continue;
        }

        /* On failure, unwind every active frame and restore all local state
         * before returning; callers may safely discard the incomplete SSA. */
        while (s->nrename_log > frame->checkpoint) {
            --s->nrename_log;
            s->current[s->rename_slots[s->nrename_log]] =
                s->rename_values[s->nrename_log];
        }
        --top;
    }
    /* If a child failed, its frame and all ancestors still need unwinding. */
    while (top) {
        SSARenameFrame *frame = &stack[top - 1];
        while (s->nrename_log > frame->checkpoint) {
            --s->nrename_log;
            s->current[s->rename_slots[s->nrename_log]] =
                s->rename_values[s->nrename_log];
        }
        --top;
    }
    free(stack);
}

static int ssa_dominates(const CFG *g, int dominator, int block) {
    if (!g || dominator < 0 || block < 0 ||
        dominator >= g->count || block >= g->count ||
        !g->dom_pre || !g->dom_end ||
        g->dom_pre[dominator] < 0 || g->dom_pre[block] < 0)
        return 0;
    return g->dom_pre[dominator] <= g->dom_pre[block] &&
           g->dom_pre[block] < g->dom_end[dominator];
}

static int ssa_verify(const SSA *s) {
    if (!s->valid) return 0;
    unsigned char *defined = calloc((size_t)s->next_value, 1);
    int *defblock = malloc((size_t)s->next_value * sizeof(*defblock));
    if (s->next_value && (!defined || !defblock)) {
        free(defined); free(defblock); return 0;
    }
    for (int i = 0; i < s->next_value; ++i) defblock[i] = -1;
    for (int i = 0; i < s->nparams; ++i) {
        if (i >= s->nslots || s->current[i] < 0 ||
            s->current[i] >= s->next_value || defined[s->current[i]]) {
            free(defined); free(defblock); return 0;
        }
        defined[s->current[i]] = 1;
        defblock[s->current[i]] = s->entry;
    }
    for (int b = 0; b < s->cfg->count; ++b) {
        if (!s->cfg->blocks[b].reachable) continue;
        const SSABlock *sb = &s->blocks[b];
        for (int i = 0; i < sb->ndefs; ++i) {
            int v = sb->defs[i].value;
            if (v < 0 || v >= s->next_value || defined[v]) {
                free(defined); free(defblock); return 0;
            }
            defined[v] = 1;
            defblock[v] = b;
        }
        for (int i = 0; i < sb->nphis; ++i) {
            const SSAPhi *p = &sb->phis[i];
            int expected = 0;
            for (int j = 0; j < s->cfg->npred[b]; ++j)
                if (s->cfg->blocks[s->cfg->pred[b][j]].reachable) expected++;
            if (p->value < 0 || p->value >= s->next_value || defined[p->value] ||
                p->nincoming != expected) {
                free(defined); free(defblock); return 0;
            }
            defined[p->value] = 1;
            defblock[p->value] = b;
            for (int j = 0; j < p->nincoming; ++j) {
                int pred = p->preds[j];
                int edge = 0;
                for (int k = 0; k < s->cfg->npred[b]; ++k)
                    if (s->cfg->pred[b][k] == pred &&
                        s->cfg->blocks[pred].reachable) edge = 1;
                for (int k = 0; k < j; ++k)
                    if (p->preds[k] == pred) edge = 0;
                if (!edge || p->incoming[j] < 0 ||
                    p->incoming[j] >= s->next_value) {
                    free(defined); free(defblock); return 0;
                }
            }
        }
    }
    for (int i = 0; i < s->next_value; ++i)
        if (!defined[i] || defblock[i] < 0) {
            free(defined); free(defblock); return 0;
        }
    /* Every ordinary use is dominated by its definition. Phi operands are
     * checked against the predecessor edge on which that operand arrives. */
    for (int b = 0; b < s->cfg->count; ++b) {
        if (!s->cfg->blocks[b].reachable) continue;
        const SSABlock *sb = &s->blocks[b];
        for (int i = 0; i < sb->nuses; ++i) {
            int v = sb->uses[i].value;
            if (v < 0 || v >= s->next_value ||
                !ssa_dominates(s->cfg, defblock[v], b)) {
                free(defined); free(defblock); return 0;
            }
        }
        for (int i = 0; i < sb->nphis; ++i) {
            const SSAPhi *p = &sb->phis[i];
            for (int j = 0; j < p->nincoming; ++j) {
                int v = p->incoming[j];
                if (v < 0 || v >= s->next_value ||
                    !ssa_dominates(s->cfg, defblock[v], p->preds[j])) {
                    free(defined); free(defblock); return 0;
                }
            }
        }
    }
    free(defined); free(defblock);
    return 1;
}


/* SCCP value lattice over the constructed SSA graph.  The ordering is
 * UNDEFINED < CONSTANT < OVERDEFINED. Unsupported expressions conservatively
 * become OVERDEFINED; arithmetic is evaluated only for known integer
 * constants when the operation is safe to fold. */
enum { SSA_UNDEF, SSA_CONST, SSA_OVERDEF };
typedef struct { int state; long long value; } SSALattice;

static SSALattice ssa_overdef(void) {
    SSALattice v = { SSA_OVERDEF, 0 }; return v;
}

static SSALattice ssa_lattice_join(SSALattice a, SSALattice b) {
    if (a.state == SSA_UNDEF) return b;
    if (b.state == SSA_UNDEF) return a;
    if (a.state == SSA_OVERDEF || b.state == SSA_OVERDEF) return ssa_overdef();
    if (a.value == b.value) return a;
    return ssa_overdef();
}

/* Compute SSA liveness with Phi operands attached to predecessor edges.
 * Phi results are definitions in their destination block, while each incoming
 * operand is used only on its corresponding CFG edge. Treating Phi operands
 * as ordinary successor-block uses would incorrectly make them live on every
 * predecessor. The analysis is temporary groundwork for interference-based
 * register allocation; it does not rewrite HIR or claim backend allocation. */
static int ssa_compute_liveness(SSA *s) {
    if (!s || !s->valid || s->next_value < 0) return 0;
    CFG *g = s->cfg;
    size_t values = (size_t)s->next_value;
    size_t words = (values + 63u) / 64u;
    if (!words) { s->live_words = 0; s->peak_live = 0; return 1; }
    if (words > (size_t)INT_MAX ||
        (size_t)g->count > SIZE_MAX / words ||
        (size_t)g->count * words > SIZE_MAX / sizeof(uint64_t)) return 0;
    size_t cells = (size_t)g->count * words;
    if (cells > SIZE_MAX / (4u * sizeof(uint64_t))) return 0;

    int *def_block = malloc(values * sizeof(*def_block));
    uint64_t *sets = calloc(cells * 4u, sizeof(*sets));
    uint64_t *out_tmp = calloc(words, sizeof(*out_tmp));
    int *worklist = malloc((size_t)g->count * sizeof(*worklist));
    unsigned char *queued = calloc((size_t)g->count, 1);
    s->live_in = calloc(cells, sizeof(*s->live_in));
    s->live_out = calloc(cells, sizeof(*s->live_out));
    if (!def_block || !sets || !out_tmp || !worklist || !queued ||
        !s->live_in || !s->live_out) {
        free(def_block); free(sets); free(out_tmp);
        free(worklist); free(queued);
        free(s->live_in); free(s->live_out);
        s->live_in = s->live_out = NULL;
        return 0;
    }
    s->live_words = (int)words;
    for (size_t v = 0; v < values; ++v) def_block[v] = -1;
    /* The initial SSA values are function parameters defined at entry. */
    for (int v = 0; v < s->nparams && (size_t)v < values; ++v)
        def_block[v] = s->entry;

    uint64_t *uses = sets;
    uint64_t *defs = sets + cells;
    uint64_t *phi_defs = sets + cells * 2u;
    /* Phi operands are edge uses. Their union per predecessor is exactly
     * the contribution needed by that predecessor's live-out equation, so
     * compute it once instead of rescanning every successor Phi on each
     * fixed-point visit. */
    uint64_t *phi_edge_uses = sets + cells * 3u;
    for (int b = 0; b < g->count; ++b) {
        if (!g->blocks[b].reachable) continue;
        SSABlock *sb = &s->blocks[b];
        for (int i = 0; i < sb->nphis; ++i) {
            int v = sb->phis[i].value;
            if (v < 0 || (size_t)v >= values || def_block[v] >= 0) goto fail;
            def_block[v] = b;
            phi_defs[(size_t)b * words + (size_t)v / 64u] |=
                UINT64_C(1) << ((unsigned)v & 63u);
        }
        for (int i = 0; i < sb->ndefs; ++i) {
            int v = sb->defs[i].value;
            if (v < 0 || (size_t)v >= values || def_block[v] >= 0) goto fail;
            def_block[v] = b;
        }
    }
    for (size_t v = 0; v < values; ++v)
        if (def_block[v] < 0) goto fail;

    for (int b = 0; b < g->count; ++b) {
        if (!g->blocks[b].reachable) continue;
        SSABlock *sb = &s->blocks[b];
        uint64_t *ub = uses + (size_t)b * words;
        uint64_t *db = defs + (size_t)b * words;
        for (int i = 0; i < sb->nphis; ++i) {
            int v = sb->phis[i].value;
            db[(size_t)v / 64u] |= UINT64_C(1) << ((unsigned)v & 63u);
        }
        for (int i = 0; i < sb->ndefs; ++i) {
            int v = sb->defs[i].value;
            db[(size_t)v / 64u] |= UINT64_C(1) << ((unsigned)v & 63u);
        }
        for (int i = 0; i < sb->nuses; ++i) {
            int v = sb->uses[i].value;
            if (v < 0 || (size_t)v >= values) goto fail;
            if (def_block[v] != b)
                ub[(size_t)v / 64u] |= UINT64_C(1) << ((unsigned)v & 63u);
        }
        for (int ei = 0; ei < g->blocks[b].nsucc; ++ei) {
            int succ = g->blocks[b].succ[ei];
            if (!g->blocks[succ].reachable) continue;
            SSABlock *dest = &s->blocks[succ];
            uint64_t *edge = phi_edge_uses + (size_t)b * words;
            for (int pi = 0; pi < dest->nphis; ++pi) {
                SSAPhi *phi = &dest->phis[pi];
                for (int k = 0; k < phi->nincoming; ++k) {
                    if (phi->preds[k] != b) continue;
                    int v = phi->incoming[k];
                    if (v < 0 || (size_t)v >= values) goto fail;
                    edge[(size_t)v / 64u] |=
                        UINT64_C(1) << ((unsigned)v & 63u);
                }
            }
        }
    }

    /* Dependency-driven fixed point. A block is revisited only when one
     * of its successors changes live-in. Phi operands remain edge uses.
     * The queue holds each block at most once, so its capacity is |CFG|. */
    int qhead = 0, qtail = 0, qcount = 0;
    for (int ri = g->nrpo - 1; ri >= 0; --ri) {
        int b = g->rpo[ri];
        worklist[qtail] = b;
        qtail = (qtail + 1) % g->count;
        ++qcount;
        queued[b] = 1;
    }
    while (qcount) {
        int b = worklist[qhead];
        qhead = (qhead + 1) % g->count;
        --qcount;
        queued[b] = 0;
        uint64_t *in = s->live_in + (size_t)b * words;
        uint64_t *out = s->live_out + (size_t)b * words;
        uint64_t *ub = uses + (size_t)b * words;
        uint64_t *db = defs + (size_t)b * words;
        memset(out_tmp, 0, words * sizeof(*out_tmp));
        for (int ei = 0; ei < g->blocks[b].nsucc; ++ei) {
            int succ = g->blocks[b].succ[ei];
            if (!g->blocks[succ].reachable) continue;
            const uint64_t *sin = s->live_in + (size_t)succ * words;
            const uint64_t *sphi = phi_defs + (size_t)succ * words;
            for (size_t w = 0; w < words; ++w)
                out_tmp[w] |= sin[w] & ~sphi[w];
        }
        const uint64_t *edge_uses = phi_edge_uses + (size_t)b * words;
        for (size_t w = 0; w < words; ++w)
            out_tmp[w] |= edge_uses[w];
        int in_changed = 0;
        for (size_t w = 0; w < words; ++w) {
            uint64_t next_in = ub[w] | (out_tmp[w] & ~db[w]);
            if (in[w] != next_in) in_changed = 1;
            out[w] = out_tmp[w];
            in[w] = next_in;
        }
        if (in_changed) {
            for (int pi = 0; pi < g->npred[b]; ++pi) {
                int pred = g->pred[b][pi];
                if (!g->blocks[pred].reachable || queued[pred]) continue;
                if (qcount >= g->count) goto fail;
                worklist[qtail] = pred;
                qtail = (qtail + 1) % g->count;
                ++qcount;
                queued[pred] = 1;
            }
        }
    }
    free(worklist); worklist = NULL;
    free(queued); queued = NULL;

    s->peak_live = 0;
    for (int b = 0; b < g->count; ++b) {
        if (!g->blocks[b].reachable) continue;
        const uint64_t *in = s->live_in + (size_t)b * words;
        const uint64_t *out = s->live_out + (size_t)b * words;
        int live_in = 0, live_out = 0;
        for (size_t w = 0; w < words; ++w) {
            live_in += __builtin_popcountll(in[w]);
            live_out += __builtin_popcountll(out[w]);
        }
        if (live_in > s->peak_live) s->peak_live = live_in;
        if (live_out > s->peak_live) s->peak_live = live_out;
    }
    free(def_block); free(sets); free(out_tmp);
    return 1;

fail:
    free(def_block); free(sets); free(out_tmp);
    free(worklist); free(queued);
    free(s->live_in); free(s->live_out);
    s->live_in = s->live_out = NULL;
    s->live_words = 0;
    return 0;
}

static int ssa_interference_edge_cmp(const void *a, const void *b) {
    const SSAInterferenceEdge *x = a, *y = b;
    if (x->a != y->a) return x->a < y->a ? -1 : 1;
    return x->b < y->b ? -1 : x->b > y->b;
}

static uint64_t ssa_interference_hash(uint64_t key) {
    /* SplitMix64 finalizer: deterministic, inexpensive, and well distributed
     * for the packed (low, high) SSA value pair. */
    key ^= key >> 30;
    key *= UINT64_C(0xbf58476d1ce4e5b9);
    key ^= key >> 27;
    key *= UINT64_C(0x94d049bb133111eb);
    key ^= key >> 31;
    return key;
}

static int ssa_interference_set_grow(SSA *s, size_t requested) {
    size_t cap = s->interference_set_cap ? s->interference_set_cap : 128;
    while (cap < requested) {
        if (cap > SIZE_MAX / 2) return 0;
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof(*s->interference_set)) return 0;
    uint64_t *set = calloc(cap, sizeof(*set));
    if (!set) return 0;
    for (size_t i = 0; i < s->interference_set_cap; ++i) {
        uint64_t key = s->interference_set[i];
        if (!key) continue;
        size_t slot = (size_t)ssa_interference_hash(key) & (cap - 1);
        while (set[slot]) slot = (slot + 1) & (cap - 1);
        set[slot] = key;
    }
    free(s->interference_set);
    s->interference_set = set;
    s->interference_set_cap = cap;
    return 1;
}

static int ssa_interference_add(SSA *s, int a, int b) {
    if (a == b) return 1;
    if (a < 0 || b < 0 || a >= s->next_value || b >= s->next_value ||
        s->ninterference == INT_MAX) return 0;
    if (a > b) { int t = a; a = b; b = t; }

    /* a < b guarantees a nonzero packed key, leaving zero as the empty-slot
     * sentinel. The set is kept below 70% load to bound linear probing. */
    uint64_t key = ((uint64_t)(uint32_t)a << 32) | (uint32_t)b;
    size_t needed = (size_t)s->ninterference + 1;
    if (!s->interference_set_cap ||
        needed >= s->interference_set_cap - s->interference_set_cap / 3) {
        size_t target = s->interference_set_cap
            ? s->interference_set_cap * 2 : 128;
        if (target < s->interference_set_cap ||
            !ssa_interference_set_grow(s, target)) return 0;
    }
    size_t slot = (size_t)ssa_interference_hash(key) &
                  (s->interference_set_cap - 1);
    while (s->interference_set[slot]) {
        if (s->interference_set[slot] == key) return 1;
        slot = (slot + 1) & (s->interference_set_cap - 1);
    }

    if (s->ninterference == s->interference_cap) {
        if (s->interference_cap > INT_MAX / 2) return 0;
        int cap = s->interference_cap ? s->interference_cap * 2 : 64;
        if ((size_t)cap > SIZE_MAX / sizeof(*s->interference)) return 0;
        SSAInterferenceEdge *p = realloc(s->interference,
                                         (size_t)cap * sizeof(*p));
        if (!p) return 0;
        s->interference = p;
        s->interference_cap = cap;
    }
    s->interference_set[slot] = key;
    s->interference[s->ninterference++] = (SSAInterferenceEdge){ a, b };
    return 1;
}

/* Build an interference graph by walking each block's SSA events backwards.
 * The CFG still supplies block live-out sets (including edge-specific Phi
 * operands), but conflicts are created at definition points rather than by
 * making every block-boundary set a clique. A statement's collected ordinary
 * uses are evaluated before its scalar definition, so reverse order is:
 * statement defs, statement uses, Phi defs, then entry parameters. This is
 * materially more precise for dead and short-lived values within a block.
 * The model remains statement-granular: it does not claim machine-instruction
 * live ranges or perform physical register allocation. */
static int ssa_live_bit_count(const uint64_t *bits, int words) {
    int count = 0;
    for (int i = 0; i < words; ++i)
        count += __builtin_popcountll(bits[i]);
    return count;
}

static int ssa_build_interference(SSA *s) {
    if (!s || !s->valid || !s->live_in || !s->live_out ||
        s->live_words <= 0) return s && s->valid && s->next_value == 0;
    int n = s->next_value;
    size_t words = (size_t)s->live_words;
    if ((size_t)n > SIZE_MAX / sizeof(unsigned) ||
        words > SIZE_MAX / sizeof(uint64_t)) return 0;

    uint64_t *live = malloc(words * sizeof(*live));
    unsigned *degree = calloc((size_t)n, sizeof(*degree));
    if (!live || !degree) { free(live); free(degree); return 0; }

    for (int b = 0; b < s->cfg->count; ++b) {
        if (!s->cfg->blocks[b].reachable) continue;
        const uint64_t *out = s->live_out + (size_t)b * words;
        memcpy(live, out, words * sizeof(*live));
        int point_live = ssa_live_bit_count(live, s->live_words);
        if (point_live > s->peak_live) s->peak_live = point_live;
        SSABlock *sb = &s->blocks[b];

        /* Reverse the statement: its RHS/condition uses precede its defs. */
        for (int i = sb->ndefs - 1; i >= 0; --i) {
            int v = sb->defs[i].value;
            if (v < 0 || v >= n) goto fail;
            uint64_t value_mask = UINT64_C(1) << ((unsigned)v & 63u);
            size_t value_word = (size_t)v / 64u;
            /* A dead definition has no live range and must not create
             * artificial pressure against values that happen to be live. */
            if (live[value_word] & value_mask) {
                for (int w = 0; w < s->live_words; ++w) {
                    uint64_t x = live[w];
                    while (x) {
                        unsigned bit = (unsigned)__builtin_ctzll(x);
                        size_t other_index = (size_t)w * 64u + bit;
                        x &= x - 1;
                        if (other_index >= (size_t)n) break;
                        if (!ssa_interference_add(s, v, (int)other_index))
                            goto fail;
                    }
                }
            }
            live[value_word] &= ~value_mask;
            point_live = ssa_live_bit_count(live, s->live_words);
            if (point_live > s->peak_live) s->peak_live = point_live;
        }
        for (int i = sb->nuses - 1; i >= 0; --i) {
            int v = sb->uses[i].value;
            if (v < 0 || v >= n) goto fail;
            live[(size_t)v / 64u] |=
                UINT64_C(1) << ((unsigned)v & 63u);
        }
        point_live = ssa_live_bit_count(live, s->live_words);
        if (point_live > s->peak_live) s->peak_live = point_live;

        /* Phi results are simultaneous definitions at block entry. Add
         * conflicts among all live Phi results before killing any of them;
         * otherwise the first result removed from the set would hide the
         * conflict with later Phi results. Dead Phi results need no node
         * pressure, just like dead ordinary definitions. */
        for (int i = sb->nphis - 1; i >= 0; --i) {
            int v = sb->phis[i].value;
            if (v < 0 || v >= n) goto fail;
            size_t vw = (size_t)v / 64u;
            uint64_t vm = UINT64_C(1) << ((unsigned)v & 63u);
            if (!(live[vw] & vm)) continue;
            for (int w = 0; w < s->live_words; ++w) {
                uint64_t x = live[w];
                while (x) {
                    unsigned bit = (unsigned)__builtin_ctzll(x);
                    size_t other_index = (size_t)w * 64u + bit;
                    x &= x - 1;
                    if (other_index >= (size_t)n) break;
                    if (!ssa_interference_add(s, v, (int)other_index))
                        goto fail;
                }
            }
        }
        for (int i = 0; i < sb->nphis; ++i) {
            int v = sb->phis[i].value;
            live[(size_t)v / 64u] &=
                ~(UINT64_C(1) << ((unsigned)v & 63u));
        }
        point_live = ssa_live_bit_count(live, s->live_words);
        if (point_live > s->peak_live) s->peak_live = point_live;

        /* Initial parameter values are definitions before block-entry Phis. */
        if (b == s->entry) {
            if (s->nparams < 0 || s->nparams > n) goto fail;
            for (int i = s->nparams - 1; i >= 0; --i) {
                int v = i;
                size_t vw = (size_t)v / 64u;
                uint64_t vm = UINT64_C(1) << ((unsigned)v & 63u);
                if (live[vw] & vm) {
                    for (int w = 0; w < s->live_words; ++w) {
                        uint64_t x = live[w];
                        while (x) {
                            unsigned bit = (unsigned)__builtin_ctzll(x);
                            size_t other_index = (size_t)w * 64u + bit;
                            x &= x - 1;
                            if (other_index >= (size_t)n) break;
                            if (!ssa_interference_add(s, v, (int)other_index))
                                goto fail;
                        }
                    }
                }
                live[vw] &= ~vm;
            }
            point_live = ssa_live_bit_count(live, s->live_words);
            if (point_live > s->peak_live) s->peak_live = point_live;
        }
    }

    if (s->ninterference > 1)
        qsort(s->interference, (size_t)s->ninterference,
              sizeof(*s->interference), ssa_interference_edge_cmp);
    int unique = 0;
    for (int i = 0; i < s->ninterference; ++i) {
        if (unique && s->interference[unique - 1].a == s->interference[i].a &&
            s->interference[unique - 1].b == s->interference[i].b)
            continue;
        s->interference[unique++] = s->interference[i];
    }
    s->ninterference = unique;
    for (int i = 0; i < unique; ++i) {
        int a = s->interference[i].a, b = s->interference[i].b;
        if (degree[a] == UINT_MAX || degree[b] == UINT_MAX) goto fail;
        ++degree[a]; ++degree[b];
        if ((int)degree[a] > s->max_interference_degree)
            s->max_interference_degree = (int)degree[a];
        if ((int)degree[b] > s->max_interference_degree)
            s->max_interference_degree = (int)degree[b];
    }
    free(live); free(degree);
    return 1;

fail:
    free(live); free(degree);
    free(s->interference);
    s->interference = NULL;
    free(s->interference_set);
    s->interference_set = NULL;
    s->interference_set_cap = 0;
    s->ninterference = s->interference_cap = 0;
    s->max_interference_degree = 0;
    return 0;
}

/* A deterministic min-heap for optimistic spill candidates. Priorities are
 * lazily refreshed whenever a neighbour's degree changes; version numbers
 * make stale heap entries cheap to discard. */
static int ssa_color_heap_less(const SSAColorHeapItem *a,
                               const SSAColorHeapItem *b) {
    if (a->priority != b->priority) return a->priority < b->priority;
    return a->node < b->node;
}

static int ssa_color_heap_push(SSAColorHeapItem *heap, int *count,
                               SSAColorHeapItem item) {
    int i = (*count)++;
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (!ssa_color_heap_less(&item, &heap[parent])) break;
        heap[i] = heap[parent];
        i = parent;
    }
    heap[i] = item;
    return 1;
}

static SSAColorHeapItem ssa_color_heap_pop(SSAColorHeapItem *heap,
                                            int *count) {
    SSAColorHeapItem result = heap[0];
    SSAColorHeapItem tail = heap[--(*count)];
    if (*count) {
        int i = 0;
        for (;;) {
            int left = i * 2 + 1;
            if (left >= *count) break;
            int right = left + 1;
            int child = right < *count &&
                ssa_color_heap_less(&heap[right], &heap[left]) ? right : left;
            if (!ssa_color_heap_less(&heap[child], &tail)) break;
            heap[i] = heap[child];
            i = child;
        }
        heap[i] = tail;
    }
    return result;
}

/* Run optimistic graph colouring as an analysis-only allocation probe. The
 * graph is intentionally conservative and its colours are abstract: no
 * target register names, ABI constraints, instruction positions, or spills
 * are emitted. This validates the simplify/select machinery before backend
 * integration. */
static int ssa_color_interference(SSA *s, int color_limit) {
    if (!s || color_limit <= 0 || color_limit > 64 || s->next_value < 0)
        return 0;
    int n = s->next_value;
    if (!n) return 1;
    if ((size_t)n > SIZE_MAX / sizeof(int) ||
        (size_t)n > SIZE_MAX / sizeof(uint64_t) ||
        (size_t)n > SIZE_MAX / sizeof(SSAColorHeapItem) ||
        s->ninterference < 0 ||
        (size_t)n + (size_t)s->ninterference > (size_t)INT_MAX)
        return 0;

    int *offsets = calloc((size_t)n + 1, sizeof(*offsets));
    int *neighbors = NULL;
    int *degree = calloc((size_t)n, sizeof(*degree));
    int *stack = malloc((size_t)n * sizeof(*stack));
    int *colors = malloc((size_t)n * sizeof(*colors));
    int *queue = malloc((size_t)n * sizeof(*queue));
    unsigned char *active = malloc((size_t)n);
    unsigned char *queued = calloc((size_t)n, 1);
    uint64_t *version = calloc((size_t)n, sizeof(*version));
    SSAColorHeapItem *heap = NULL;
    int ok = 0;
    if (!offsets || !degree || !stack || !colors || !queue || !active ||
        !queued || !version) goto done;

    for (int i = 0; i < s->ninterference; ++i) {
        int a = s->interference[i].a, b = s->interference[i].b;
        if (a < 0 || b < 0 || a >= n || b >= n || a == b ||
            offsets[a + 1] == INT_MAX || offsets[b + 1] == INT_MAX)
            goto done;
        ++offsets[a + 1]; ++offsets[b + 1];
    }
    for (int i = 1; i <= n; ++i) {
        if (offsets[i] > INT_MAX - offsets[i - 1]) goto done;
        offsets[i] += offsets[i - 1];
    }
    if (offsets[n] < 0 ||
        (size_t)offsets[n] > SIZE_MAX / sizeof(*neighbors)) goto done;
    if (offsets[n]) {
        neighbors = malloc((size_t)offsets[n] * sizeof(*neighbors));
        if (!neighbors) goto done;
    }
    int *cursor = malloc((size_t)n * sizeof(*cursor));
    if (!cursor) goto done;
    memcpy(cursor, offsets, (size_t)n * sizeof(*cursor));
    for (int i = 0; i < s->ninterference; ++i) {
        int a = s->interference[i].a, b = s->interference[i].b;
        neighbors[cursor[a]++] = b;
        neighbors[cursor[b]++] = a;
    }
    free(cursor);

    size_t heap_cap = (size_t)n + (size_t)offsets[n];
    if (heap_cap < (size_t)n || heap_cap > SIZE_MAX / sizeof(*heap))
        goto done;
    heap = malloc(heap_cap * sizeof(*heap));
    if (!heap) goto done;

    int queue_head = 0, queue_tail = 0, heap_count = 0, stack_count = 0;
    for (int v = 0; v < n; ++v) {
        degree[v] = offsets[v + 1] - offsets[v];
        active[v] = 1;
        colors[v] = -1;
        long double cost = (long double)(s->use_count ? s->use_count[v] : 0u) + 1.0L;
        long double priority = cost / ((long double)degree[v] + 1.0L);
        ssa_color_heap_push(heap, &heap_count,
                            (SSAColorHeapItem){v, 0, priority});
        if (degree[v] < color_limit) {
            queue[queue_tail++] = v;
            queued[v] = 1;
        }
    }

    int remaining = n;
    while (remaining) {
        int v = -1;
        while (queue_head < queue_tail) {
            int candidate = queue[queue_head++];
            if (active[candidate] && degree[candidate] < color_limit) {
                v = candidate;
                break;
            }
        }
        if (v < 0) {
            while (heap_count) {
                SSAColorHeapItem item = ssa_color_heap_pop(heap, &heap_count);
                int candidate = item.node;
                if (!active[candidate] || item.version != version[candidate])
                    continue;
                v = candidate;
                break;
            }
            if (v < 0) goto done;
        }
        active[v] = 0;
        stack[stack_count++] = v;
        --remaining;
        for (int i = offsets[v]; i < offsets[v + 1]; ++i) {
            int other = neighbors[i];
            if (!active[other]) continue;
            if (degree[other] <= 0 || version[other] == UINT64_MAX)
                goto done;
            --degree[other];
            ++version[other];
            long double cost =
                (long double)(s->use_count ? s->use_count[other] : 0u) + 1.0L;
            long double priority = cost / ((long double)degree[other] + 1.0L);
            ssa_color_heap_push(heap, &heap_count,
                (SSAColorHeapItem){other, version[other], priority});
            if (degree[other] < color_limit && !queued[other]) {
                queue[queue_tail++] = other;
                queued[other] = 1;
            }
        }
    }

    int spills = 0, color_count = 0;
    while (stack_count) {
        int v = stack[--stack_count];
        uint64_t unavailable = 0;
        for (int i = offsets[v]; i < offsets[v + 1]; ++i) {
            int color = colors[neighbors[i]];
            if (color >= 0 && color < color_limit)
                unavailable |= UINT64_C(1) << (unsigned)color;
        }
        int chosen = -1;
        for (int c = 0; c < color_limit && c < 64; ++c)
            if (!(unavailable & (UINT64_C(1) << (unsigned)c))) {
                chosen = c;
                break;
            }
        if (chosen < 0) {
            ++spills;
        } else {
            colors[v] = chosen;
            if (chosen + 1 > color_count) color_count = chosen + 1;
        }
    }

    /* A final invariant check protects this diagnostic from ever reporting
     * an invalid colouring if graph construction changes in a later pass. */
    for (int i = 0; i < s->ninterference; ++i) {
        int a = s->interference[i].a, b = s->interference[i].b;
        if (colors[a] >= 0 && colors[a] == colors[b]) goto done;
    }
    s->abstract_color_count = color_count;
    s->abstract_spill_count = spills;
    ok = 1;

done:
    free(offsets); free(neighbors); free(degree); free(stack); free(colors);
    free(queue); free(active); free(queued); free(version); free(heap);
    return ok;
}

static int ssa_use_index_cmp(const void *a, const void *b) {
    const SSAUseIndex *x = a, *y = b;
    return x->node_key < y->node_key ? -1 : x->node_key > y->node_key;
}

/* Build once after renaming, when each block's use array is stable. This
 * replaces repeated whole-function scans in copy folding and SCCP with a
 * logarithmic lookup. The index stores pointer-sized integer keys so its
 * ordering is defined independently of relational pointer comparisons. */
static int ssa_build_use_index(SSA *s) {
    size_t count = 0;
    for (int b = 0; b < s->cfg->count; ++b)
        count += (size_t)s->blocks[b].nuses;
    if (count > (size_t)INT_MAX || count > SIZE_MAX / sizeof(*s->use_index))
        return 0;
    if (!count) return 1;
    s->use_index = malloc(count * sizeof(*s->use_index));
    if (!s->use_index) return 0;
    for (int b = 0; b < s->cfg->count; ++b) {
        SSABlock *block = &s->blocks[b];
        for (int i = 0; i < block->nuses; ++i) {
            SSAUse *use = &block->uses[i];
            s->use_index[s->nuse_index++] = (SSAUseIndex){
                (uintptr_t)use->node, use->value
            };
        }
    }
    qsort(s->use_index, (size_t)s->nuse_index,
          sizeof(*s->use_index), ssa_use_index_cmp);
    return 1;
}

static int ssa_value_use_index_cmp(const void *a, const void *b) {
    const SSAValueUseIndex *x = a, *y = b;
    if (x->value != y->value) return x->value < y->value ? -1 : 1;
    if (x->block != y->block) return x->block < y->block ? -1 : 1;
    uintptr_t xkey = (uintptr_t)x->node, ykey = (uintptr_t)y->node;
    return xkey < ykey ? -1 : xkey > ykey;
}

/* Build after alias canonicalisation, so the index keys match use_count.
 * It makes finding the sole ordinary consumer logarithmic rather than
 * rescanning every block for every candidate definition. Phi-edge uses are
 * intentionally absent: use_count still includes them, preventing an
 * expression from being moved when its only consumer is a Phi operand. */
static int ssa_build_value_use_index(SSA *s) {
    size_t count = 0;
    for (int b = 0; b < s->cfg->count; ++b) {
        size_t n = (size_t)s->blocks[b].nuses;
        if (n > SIZE_MAX - count) return 0;
        count += n;
    }
    if (count > (size_t)INT_MAX || count > SIZE_MAX / sizeof(*s->value_use_index))
        return 0;
    if (!count) return 1;
    s->value_use_index = malloc(count * sizeof(*s->value_use_index));
    if (!s->value_use_index) return 0;
    for (int b = 0; b < s->cfg->count; ++b) {
        SSABlock *block = &s->blocks[b];
        for (int i = 0; i < block->nuses; ++i) {
            SSAUse *use = &block->uses[i];
            s->value_use_index[s->nvalue_use_index++] =
                (SSAValueUseIndex){ use->value, b, use->node };
        }
    }
    qsort(s->value_use_index, (size_t)s->nvalue_use_index,
          sizeof(*s->value_use_index), ssa_value_use_index_cmp);
    return 1;
}

static int ssa_find_sole_ordinary_use(const SSA *s, int value,
                                     int *block, HNode **node) {
    int lo = 0, hi = s->nvalue_use_index;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (s->value_use_index[mid].value < value) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= s->nvalue_use_index ||
        s->value_use_index[lo].value != value ||
        (lo + 1 < s->nvalue_use_index &&
         s->value_use_index[lo + 1].value == value))
        return 0;
    *block = s->value_use_index[lo].block;
    *node = s->value_use_index[lo].node;
    return 1;
}

static int ssa_find_use_value(const SSA *s, const HNode *node, int *value) {
    if (!s->use_index || !node) return 0;
    uintptr_t key = (uintptr_t)node;
    int lo = 0, hi = s->nuse_index;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        uintptr_t found = s->use_index[mid].node_key;
        if (found < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= s->nuse_index || s->use_index[lo].node_key != key)
        return 0;
    *value = s->use_index[lo].value;
    return 1;
}

/* Resolve a value through copy and trivial-Phi aliases.  A bounded walk
 * makes malformed cycles fail closed rather than looping forever. */
static int ssa_resolve_alias(const SSA *s, int value) {
    if (!s->aliases || value < 0 || value >= s->next_value) return value;
    for (int steps = 0; steps <= s->next_value; ++steps) {
        int next = s->aliases[value];
        if (next < 0 || next == value) return value;
        value = next;
        if (value < 0 || value >= s->next_value) return -1;
    }
    return -1;
}

static HNode *ssa_copy_source(HNode *expr) {
    while (expr && expr->kind == HK_CAST && expr->child[0] &&
           expr->child[0]->type && expr->type &&
           expr->child[0]->type->kind == expr->type->kind &&
           expr->child[0]->type->width == expr->type->width)
        expr = expr->child[0];
    return expr && (expr->kind == HK_LOCAL || expr->kind == HK_PARAM)
        ? expr : NULL;
}

/* Build a def-use index and collapse SSA copies and trivial Phi nodes.
 * A Phi is trivial when every incoming value, after alias resolution, is
 * either itself or one identical external value.  This handles chains and
 * loop-carried self inputs while conservatively leaving multi-value cycles
 * intact.  Phi operands are edge uses and count as consumers too. */
static int ssa_simplify_values(SSA *s) {
    if (!s->valid || s->next_value <= 0) return 0;
    s->aliases = malloc((size_t)s->next_value * sizeof(*s->aliases));
    s->use_count = calloc((size_t)s->next_value, sizeof(*s->use_count));
    if (!s->aliases || !s->use_count) return 0;
    for (int v = 0; v < s->next_value; ++v) s->aliases[v] = -1;

    /* A definition whose RHS is just a local read is an SSA copy. */
    for (int b = 0; b < s->cfg->count; ++b) {
        SSABlock *sb = &s->blocks[b];
        for (int i = 0; i < sb->ndefs; ++i) {
            SSADef *d = &sb->defs[i];
            HNode *copy = ssa_copy_source(d->expr);
            if (!copy || d->value < 0 || d->value >= s->next_value) continue;
            int source = -1;
            if (ssa_find_use_value(s, copy, &source) &&
                source >= 0 && source < s->next_value && source != d->value) {
                s->aliases[d->value] = source;
                s->simplified_copies++;
            }
        }
    }

    /* Iterate because one trivial Phi can make another Phi trivial. */
    int changed;
    int rounds = s->next_value + 1;
    do {
        changed = 0;
        for (int b = 0; b < s->cfg->count; ++b) {
            SSABlock *sb = &s->blocks[b];
            for (int i = 0; i < sb->nphis; ++i) {
                SSAPhi *p = &sb->phis[i];
                if (p->value < 0 || p->value >= s->next_value ||
                    s->aliases[p->value] >= 0) continue;
                int candidate = -1;
                int trivial = 1;
                for (int j = 0; j < p->nincoming; ++j) {
                    int v = ssa_resolve_alias(s, p->incoming[j]);
                    if (v < 0) { trivial = 0; break; }
                    if (v == p->value) continue;
                    if (candidate < 0) candidate = v;
                    else if (candidate != v) { trivial = 0; break; }
                }
                if (trivial && candidate >= 0 && candidate != p->value) {
                    s->aliases[p->value] = candidate;
                    s->simplified_phis++;
                    changed = 1;
                }
            }
        }
    } while (changed && --rounds > 0);

    /* Canonicalize the graph edges before SCCP and compute exact consumer
     * counts.  Multiple Phi edges are distinct uses, as in SSA def-use. */
    for (int b = 0; b < s->cfg->count; ++b) {
        SSABlock *sb = &s->blocks[b];
        for (int i = 0; i < sb->nuses; ++i) {
            int v = ssa_resolve_alias(s, sb->uses[i].value);
            if (v < 0 || v >= s->next_value) { s->valid = 0; return 0; }
            sb->uses[i].value = v;
            s->use_count[v]++;
        }
        for (int i = 0; i < sb->nphis; ++i) {
            SSAPhi *p = &sb->phis[i];
            for (int j = 0; j < p->nincoming; ++j) {
                int v = ssa_resolve_alias(s, p->incoming[j]);
                if (v < 0 || v >= s->next_value) { s->valid = 0; return 0; }
                p->incoming[j] = v;
                s->use_count[v]++;
            }
        }
    }
    for (int b = 0; b < s->cfg->count; ++b)
        for (int i = 0; i < s->blocks[b].nphis; ++i) {
            int v = s->blocks[b].phis[i].value;
            if (v >= 0 && v < s->next_value && s->aliases[v] < 0 &&
                s->use_count[v] == 1)
                s->single_use_phis++;
        }
    return s->valid;
}

static long long ssa_normalize_integer(const HType *type, uint64_t bits) {
    if (type && (type->kind == HTY_INT || type->kind == HTY_UINT) &&
        type->width > 0 && type->width < 64) {
        uint64_t mask = (1ULL << type->width) - 1ULL;
        bits &= mask;
        if (type->kind == HTY_INT && (bits & (1ULL << (type->width - 1))))
            bits |= ~mask;
    }
    long long result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static SSALattice ssa_eval_expr(const SSA *s, const SSALattice *values,
                                const HNode *n) {
    if (!n) return ssa_overdef();
    if (n->kind == HK_CONST_INT) {
        SSALattice v = { SSA_CONST, n->u.ival }; return v;
    }
    if (n->kind == HK_LOCAL || n->kind == HK_PARAM) {
        int id = -1;
        if (!ssa_find_use_value(s, n, &id) || id < 0 || id >= s->next_value)
            return ssa_overdef();
        return values[id];
    }
    if (n->kind == HK_CAST) {
        const HType *from = n->child[0] ? n->child[0]->type : NULL;
        const HType *to = n->type;
        /* A cast is transparent only when it cannot change the scalar
         * representation. Narrowing and signedness-changing casts stay
         * conservative until their exact language rules are modelled. */
        if (!from || !to || from->kind != to->kind || from->width != to->width)
            return ssa_overdef();
        return ssa_eval_expr(s, values, n->child[0]);
    }
    if (n->kind == HK_UNOP) {
        SSALattice a = ssa_eval_expr(s, values, n->child[0]);
        if (a.state != SSA_CONST) return a;
        uint64_t x = (uint64_t)a.value, r;
        switch (n->u.unop) {
        case HUN_NEG: r = 0ULL - x; break;
        case HUN_BITNOT: r = ~x; break;
        case HUN_LNOT: r = !x; break;
        default: return ssa_overdef();
        }
        SSALattice v = { SSA_CONST, ssa_normalize_integer(n->type, r) }; return v;
    }
    if (n->kind == HK_BINOP) {
        SSALattice a = ssa_eval_expr(s, values, n->child[0]);
        SSALattice b = ssa_eval_expr(s, values, n->child[1]);
        if (a.state == SSA_OVERDEF || b.state == SSA_OVERDEF) return ssa_overdef();
        if (a.state != SSA_CONST || b.state != SSA_CONST) {
            SSALattice v = { SSA_UNDEF, 0 }; return v;
        }
        uint64_t x = (uint64_t)a.value, y = (uint64_t)b.value, r = 0;
        int uns = n->child[0] && n->child[0]->type &&
                  n->child[0]->type->kind == HTY_UINT;
        switch (n->u.binop) {
        case HOP_ADD: r = x + y; break;
        case HOP_SUB: r = x - y; break;
        case HOP_MUL: r = x * y; break;
        case HOP_AND: r = x & y; break;
        case HOP_OR: r = x | y; break;
        case HOP_XOR: r = x ^ y; break;
        case HOP_SHL: case HOP_SHR: case HOP_USHR:
            if (b.value < 0 || b.value > 63) return ssa_overdef();
            if (n->u.binop == HOP_SHL) r = x << (unsigned)b.value;
            else if (n->u.binop == HOP_USHR || uns) r = x >> (unsigned)b.value;
            else if (b.value == 0) r = x;
            else if (a.value >= 0) r = x >> (unsigned)b.value;
            else r = (x >> (unsigned)b.value) |
                     (~0ULL << (64 - (unsigned)b.value));
            break;
        case HOP_DIV: case HOP_MOD:
            if (!b.value) return ssa_overdef();
            if (uns) r = n->u.binop == HOP_DIV ? x / y : x % y;
            else {
                if (a.value == LLONG_MIN && b.value == -1) return ssa_overdef();
                r = (uint64_t)(n->u.binop == HOP_DIV ? a.value / b.value : a.value % b.value);
            }
            break;
        case HOP_EQ: r = a.value == b.value; break;
        case HOP_NE: r = a.value != b.value; break;
        case HOP_LT: r = uns ? x < y : a.value < b.value; break;
        case HOP_LE: r = uns ? x <= y : a.value <= b.value; break;
        case HOP_GT: r = uns ? x > y : a.value > b.value; break;
        case HOP_GE: r = uns ? x >= y : a.value >= b.value; break;
        case HOP_LAND: r = a.value && b.value; break;
        case HOP_LOR: r = a.value || b.value; break;
        default: return ssa_overdef();
        }
        SSALattice v = { SSA_CONST, ssa_normalize_integer(n->type, r) }; return v;
    }
    return ssa_overdef();
}

static int ssa_edge_is_executable(const CFG *g,
                                  unsigned char *const *exec_edges,
                                  int pred, int succ) {
    if (pred < 0 || pred >= g->count) return 0;
    for (int i = 0; i < g->blocks[pred].nsucc; ++i)
        if (g->blocks[pred].succ[i] == succ)
            return exec_edges[pred] && exec_edges[pred][i];
    return 0;
}

/* Sparse conditional constant propagation. Unlike a plain SSA constant
 * propagation pass, this tracks executable CFG edges independently from
 * value states. A branch with a constant condition activates only one edge;
 * Phi nodes join only values arriving over executable predecessor edges. */
static int ssa_sccp(SSA *s) {
    if (!s->valid || s->next_value <= 0 || s->cfg->count <= 0) return 0;
    CFG *g = s->cfg;
    const int nvalues = s->next_value;
    const int nblocks = g->count;
    SSALattice *values = calloc((size_t)nvalues, sizeof(*values));
    unsigned char *exec_blocks = calloc((size_t)nblocks, 1);
    unsigned char **exec_edges = calloc((size_t)nblocks, sizeof(*exec_edges));
    unsigned char *queued = calloc((size_t)nblocks, 1);
    int *queue = malloc((size_t)nblocks * sizeof(*queue));
    size_t *dep_offsets = calloc((size_t)nvalues + 1, sizeof(*dep_offsets));
    size_t *dep_cursor = NULL;
    int *dependents = NULL;
    if (!values || !exec_blocks || !exec_edges || !queued || !queue ||
        !dep_offsets) {
        free(values); free(exec_blocks); free(exec_edges); free(queued);
        free(queue); free(dep_offsets); return 0;
    }

    int alloc_ok = 1;
    size_t ndeps = 0;
    for (int b = 0; b < nblocks; ++b) {
        if (g->blocks[b].nsucc) {
            exec_edges[b] = calloc((size_t)g->blocks[b].nsucc, 1);
            if (!exec_edges[b]) { alloc_ok = 0; break; }
        }
        SSABlock *sb = &s->blocks[b];
        if ((size_t)sb->nuses > SIZE_MAX - ndeps) { alloc_ok = 0; break; }
        ndeps += (size_t)sb->nuses;
        for (int i = 0; i < sb->nphis; ++i) {
            if ((size_t)sb->phis[i].nincoming > SIZE_MAX - ndeps) {
                alloc_ok = 0; break;
            }
            ndeps += (size_t)sb->phis[i].nincoming;
        }
        if (!alloc_ok) break;
    }
    if (alloc_ok && ndeps > SIZE_MAX / sizeof(*dependents)) alloc_ok = 0;
    if (alloc_ok) {
        dependents = ndeps ? malloc(ndeps * sizeof(*dependents)) : NULL;
        dep_cursor = malloc((size_t)nvalues * sizeof(*dep_cursor));
        if ((ndeps && !dependents) || !dep_cursor) alloc_ok = 0;
    }
    if (alloc_ok) {
        /* Count every ordinary and Phi-edge use by value. */
        for (int b = 0; b < nblocks && alloc_ok; ++b) {
            SSABlock *sb = &s->blocks[b];
            for (int i = 0; i < sb->nuses; ++i) {
                int v = sb->uses[i].value;
                if (v < 0 || v >= nvalues || dep_offsets[v + 1] == SIZE_MAX) {
                    alloc_ok = 0; break;
                }
                dep_offsets[v + 1]++;
            }
            for (int i = 0; i < sb->nphis && alloc_ok; ++i)
                for (int j = 0; j < sb->phis[i].nincoming; ++j) {
                    int v = sb->phis[i].incoming[j];
                    if (v < 0 || v >= nvalues || dep_offsets[v + 1] == SIZE_MAX) {
                        alloc_ok = 0; break;
                    }
                    dep_offsets[v + 1]++;
                }
        }
    }
    if (alloc_ok) {
        for (int v = 0; v < nvalues; ++v) {
            if (dep_offsets[v + 1] > SIZE_MAX - dep_offsets[v]) {
                alloc_ok = 0; break;
            }
            dep_offsets[v + 1] += dep_offsets[v];
            dep_cursor[v] = dep_offsets[v];
        }
        if (dep_offsets[nvalues] != ndeps) alloc_ok = 0;
    }
    if (alloc_ok) {
        for (int b = 0; b < nblocks; ++b) {
            SSABlock *sb = &s->blocks[b];
            for (int i = 0; i < sb->nuses; ++i) {
                int v = sb->uses[i].value;
                dependents[dep_cursor[v]++] = b;
            }
            for (int i = 0; i < sb->nphis; ++i)
                for (int j = 0; j < sb->phis[i].nincoming; ++j) {
                    int v = sb->phis[i].incoming[j];
                    dependents[dep_cursor[v]++] = b;
                }
        }
    }
    if (!alloc_ok) {
        for (int b = 0; b < nblocks; ++b) free(exec_edges[b]);
        free(dependents); free(dep_cursor); free(dep_offsets);
        free(queue); free(queued); free(exec_edges); free(exec_blocks);
        free(values);
        return 0;
    }

    /* Parameters start overdefined. Every lattice transition is monotone:
     * UNDEF -> CONST -> OVERDEF (or UNDEF -> OVERDEF). */
    for (int i = 0; i < s->nparams; ++i)
        if (i < s->nslots && s->current[i] >= 0 &&
            s->current[i] < nvalues)
            values[s->current[i]] = ssa_overdef();

    int qhead = 0, qtail = 0, qcount = 0;
    #define SCCP_ENQUEUE(block_) do { \
        int _b = (block_); \
        if (_b >= 0 && _b < nblocks && !queued[_b]) { \
            queue[qtail] = _b; \
            qtail = (qtail + 1) % nblocks; \
            qcount++; queued[_b] = 1; \
        } \
    } while (0)
    #define SCCP_NOTIFY(value_) do { \
        int _v = (value_); \
        for (size_t _k = dep_offsets[_v]; _k < dep_offsets[_v + 1]; ++_k) \
            SCCP_ENQUEUE(dependents[_k]); \
    } while (0)

    exec_blocks[s->entry] = 1;
    SCCP_ENQUEUE(s->entry);
    while (qcount) {
        int b = queue[qhead];
        qhead = (qhead + 1) % nblocks;
        qcount--; queued[b] = 0;
        SSABlock *sb = &s->blocks[b];

        /* Phi nodes are recomputed only when an incoming value or edge makes
         * this block live. Inactive predecessor edges contribute nothing. */
        for (int i = 0; i < sb->nphis; ++i) {
            SSAPhi *p = &sb->phis[i];
            if (p->value < 0 || p->value >= nvalues) continue;
            SSALattice joined = { SSA_UNDEF, 0 };
            for (int j = 0; j < p->nincoming; ++j) {
                int pred = p->preds[j], v = p->incoming[j];
                if (v >= 0 && v < nvalues &&
                    ssa_edge_is_executable(g, exec_edges, pred, b))
                    joined = ssa_lattice_join(joined, values[v]);
            }
            SSALattice old = values[p->value];
            SSALattice next = ssa_lattice_join(old, joined);
            if (old.state != next.state ||
                (old.state == SSA_CONST && old.value != next.value)) {
                values[p->value] = next;
                SCCP_NOTIFY(p->value);
            }
        }

        for (int i = 0; i < sb->ndefs; ++i) {
            SSADef *d = &sb->defs[i];
            if (d->value < 0 || d->value >= nvalues) continue;
            SSALattice next = d->expr ? ssa_eval_expr(s, values, d->expr)
                                      : ssa_overdef();
            SSALattice old = values[d->value];
            next = ssa_lattice_join(old, next);
            if (old.state != next.state ||
                (old.state == SSA_CONST && old.value != next.value)) {
                values[d->value] = next;
                SCCP_NOTIFY(d->value);
            }
        }

        /* A changed condition requeues its block through the ordinary-use
         * dependency index. Only newly executable edges enqueue successors. */
        CFGBlock *cb = &g->blocks[b];
        int first = 0, last = cb->nsucc;
        if (cb->stmt && (cb->stmt->kind == HK_IF ||
                         cb->stmt->kind == HK_WHILE) && cb->nsucc >= 2) {
            SSALattice cond = ssa_eval_expr(s, values, cb->stmt->child[0]);
            if (cond.state == SSA_UNDEF) continue;
            if (cond.state == SSA_CONST) {
                first = cond.value ? 0 : 1;
                last = first + 1;
            }
        }
        for (int ei = first; ei < last; ++ei) {
            if (exec_edges[b][ei]) continue;
            exec_edges[b][ei] = 1;
            int to = cb->succ[ei];
            if (!exec_blocks[to]) {
                exec_blocks[to] = 1;
                SCCP_ENQUEUE(to);
            } else {
                /* A newly active incoming edge can change destination Phis. */
                SCCP_ENQUEUE(to);
            }
        }
    }
    #undef SCCP_NOTIFY
    #undef SCCP_ENQUEUE

    int rewrites = 0;
    for (int b = 0; b < nblocks; ++b) {
        if (!exec_blocks[b]) continue;
        SSABlock *sb = &s->blocks[b];
        for (int i = 0; i < sb->nuses; ++i) {
            SSAUse *u = &sb->uses[i];
            if (!u->node || u->value < 0 || u->value >= nvalues ||
                values[u->value].state != SSA_CONST) continue;
            u->node->kind = HK_CONST_INT;
            u->node->u.ival = values[u->value].value;
            for (int c = 0; c < 4; ++c) u->node->child[c] = NULL;
            u->node->nitems = 0;
            rewrites++;
        }
    }
    /* Preserve the original topology until SSA teardown; later CFG cleanup
     * consumes this executable-block solution. */
    for (int b = 0; b < nblocks; ++b)
        g->blocks[b].reachable = exec_blocks[b];

    for (int b = 0; b < nblocks; ++b) free(exec_edges[b]);
    free(dependents); free(dep_cursor); free(dep_offsets);
    free(queue); free(queued); free(exec_edges); free(exec_blocks);
    free(values);
    return rewrites;
}

/* Expressions eligible for local sinking are deliberately narrow: no calls,
 * memory accesses, allocations, division, or operations whose evaluation can
 * trap.  A direct successor is required, so no intervening side effect or
 * control-flow edge can be crossed. */
static int ssa_expr_is_sinkable(const HNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case HK_CONST_INT: case HK_PARAM: case HK_LOCAL:
        return 1;
    case HK_CAST:
        return n->child[0] && n->child[0]->type && n->type &&
               n->child[0]->type->kind == n->type->kind &&
               n->child[0]->type->width == n->type->width &&
               ssa_expr_is_sinkable(n->child[0]);
    case HK_UNOP:
        return n->u.unop != HUN_DEREF && n->u.unop != HOP_ADDR_OF &&
               ssa_expr_is_sinkable(n->child[0]);
    case HK_BINOP:
        if (n->u.binop == HOP_DIV || n->u.binop == HOP_MOD) return 0;
        return ssa_expr_is_sinkable(n->child[0]) &&
               ssa_expr_is_sinkable(n->child[1]);
    default:
        return 0;
    }
}

static HNode *ssa_clone_expr(HModule *m, const HNode *src) {
    if (!src) return NULL;
    HNode *dst = hir_node(m, src->kind, src->tok);
    if (!dst) return NULL;
    *dst = *src;
    dst->items = NULL;
    dst->nitems = 0;
    for (int i = 0; i < 4; ++i) {
        dst->child[i] = ssa_clone_expr(m, src->child[i]);
        if (src->child[i] && !dst->child[i]) return NULL;
    }
    if (src->nitems) {
        dst->items = calloc((size_t)src->nitems, sizeof(*dst->items));
        if (!dst->items) return NULL;
        for (int i = 0; i < src->nitems; ++i) {
            dst->items[i] = ssa_clone_expr(m, src->items[i]);
            if (src->items[i] && !dst->items[i]) return NULL;
            dst->nitems++;
        }
    }
    return dst;
}

static int ssa_remove_stmt(HNode *tree, HNode *target) {
    if (!tree || !target) return 0;
    for (int i = 0; i < tree->nitems; ++i) {
        if (tree->items[i] == target) {
            for (int j = i + 1; j < tree->nitems; ++j)
                tree->items[j - 1] = tree->items[j];
            tree->nitems--;
            return 1;
        }
    }
    for (int i = 0; i < 4; ++i)
        if (ssa_remove_stmt(tree->child[i], target)) return 1;
    for (int i = 0; i < tree->nitems; ++i)
        if (ssa_remove_stmt(tree->items[i], target)) return 1;
    return 0;
}

/* Delete scalar stores whose SSA value has no consumers. This includes
 * overwritten initial values and copy-only temporaries after alias
 * canonicalisation. Restrict deletion to expressions proven side-effect
 * free by the same conservative whitelist used by expression sinking. */
static int ssa_eliminate_dead_defs(HNode *fn, SSA *s) {
    int changes = 0;
    if (!s->use_count || !s->aliases) return 0;
    for (int b = 0; b < s->cfg->count; ++b) {
        if (!s->cfg->blocks[b].reachable) continue;
        SSABlock *sb = &s->blocks[b];
        for (int i = 0; i < sb->ndefs; ++i) {
            SSADef *d = &sb->defs[i];
            if (!d->stmt || !d->expr ||
                (d->stmt->kind != HK_LET && d->stmt->kind != HK_STORE &&
                 d->stmt->kind != HK_EXPR_STMT) ||
                d->value < 0 || d->value >= s->next_value ||
                !ssa_expr_is_sinkable(d->expr))
                continue;
            /* Alias counts are canonicalised to the source value, but the
             * backend still executes HIR slots rather than SSA. Therefore
             * never delete an alias definition here: its HIR slot may still
             * be read. Only a genuinely unconsumed, non-aliased SSA value is
             * safe to erase. */
            if (s->aliases[d->value] >= 0 || s->use_count[d->value] != 0)
                continue;
            if (ssa_remove_stmt(fn->child[3], d->stmt)) {
                changes++;
                s->dead_defs++;
            }
        }
    }
    return changes;
}

static int ssa_sink_single_use_expr(HModule *m, HNode *fn, SSA *s) {
    int changes = 0;
    if (!s->use_count) return 0;
    /* Process definitions in reverse-postorder (dominator before child).
     * A chain such as `a = x + x; b = a * x; return b;` must sink `a`
     * into `b` before sinking `b` into the return. Numeric block IDs are
     * allocated backwards by the structured CFG builder, so iterating IDs
     * here used to rewrite a downstream use first and then mutate a stale,
     * detached expression node for its producer. */
    for (int ri = 0; ri < s->cfg->nrpo; ++ri) {
        int b = s->cfg->rpo[ri];
        SSABlock *sb = &s->blocks[b];
        for (int i = 0; i < sb->ndefs; ++i) {
            SSADef *d = &sb->defs[i];
            if (d->value < 0 || d->value >= s->next_value ||
                s->aliases[d->value] >= 0 || s->use_count[d->value] != 1 ||
                !d->stmt || d->stmt->kind != HK_LET ||
                d->stmt->child[1] != d->expr ||
                !ssa_expr_is_sinkable(d->expr)) continue;
            int use_block = -1;
            HNode *use_node = NULL;
            /* use_count includes Phi-edge operands. If it is one and the
             * value-use index contains one ordinary use, that use is unique;
             * if it contains none, the sole consumer is a Phi edge. */
            if (!ssa_find_sole_ordinary_use(s, d->value,
                                            &use_block, &use_node) ||
                use_block < 0 || !use_node ||
                use_node->kind != HK_LOCAL ||
                use_block >= s->cfg->count ||
                s->cfg->blocks[b].nsucc != 1 ||
                s->cfg->blocks[b].succ[0] != use_block ||
                s->cfg->idom[use_block] != b) continue;
            HNode *copy = ssa_clone_expr(m, d->expr);
            if (!copy) continue;
            *use_node = *copy;
            if (ssa_remove_stmt(fn->child[3], d->stmt)) {
                changes++;
                s->sunk_exprs++;
            }
        }
    }
    return changes;
}

static void ssa_free(SSA *s) {
    if (s->blocks) for (int b = 0; b < s->cfg->count; ++b) {
        SSABlock *sb = &s->blocks[b];
        for (int i = 0; i < sb->nphis; ++i) {
            free(sb->phis[i].preds); free(sb->phis[i].incoming);
        }
        free(sb->phis); free(sb->defs); free(sb->uses);
    }
    free(s->blocks); free(s->has_def); free(s->placed); free(s->current);
    free(s->rename_slots); free(s->rename_values);
    free(s->aliases); free(s->use_count); free(s->use_index);
    free(s->value_use_index);
    free(s->live_in); free(s->live_out);
    free(s->interference);
    free(s->interference_set);
}

static int ssa_type_supported(const HType *t) {
    if (!t) return 0;
    /* Rewriting an integer lattice constant into a non-integer HIR value
     * would change representation (notably Int -> Real). Keep the current
     * SSA scalar subset to integer-like values only. */
    return t->kind == HTY_BOOL || t->kind == HTY_CHAR ||
           t->kind == HTY_INT || t->kind == HTY_UINT;
}

static int ssa_supported(HNode *n) {
    if (!n) return 1;
    /* Expression-valued if nodes carry implicit value edges from both arms.
     * The current scalar SSA collector models statement CFG edges, but not
     * the expression result merge. Treating the condition/arms as ordinary
     * statements can make SCCP rewrite a value-producing branch incorrectly.
     * Decline SSA for the whole function until result values are explicit. */
    if (n->kind == HK_IF && (n->flags & HF_EXPR)) return 0;
    /* These constructs currently have implicit value edges or definitions
     * that are not explicit in the scalar-local HIR model. Decline SSA for
     * the whole function rather than manufacture incomplete value flows. */
    if (n->kind == HK_FOR || n->kind == HK_DOWHILE || n->kind == HK_MATCH ||
        n->kind == HK_DEFER || n->kind == HK_LABEL || n->kind == HK_SELECT ||
        n->kind == HK_GO_ROUTINE || n->kind == HK_ASYNC || n->kind == HK_AWAIT ||
        n->kind == HK_ADDR || n->kind == HK_INLINE_ASM || n->kind == HK_TRY ||
        n->kind == HK_CATCH || n->kind == HK_PANIC ||
        (n->kind == HK_UNOP && n->u.unop == HOP_ADDR_OF))
        return 0;
    if ((n->kind == HK_LOCAL || n->kind == HK_PARAM || n->kind == HK_LET) &&
        !ssa_type_supported(n->type))
        return 0;
    for (int i = 0; i < 4; ++i)
        if (!ssa_supported(n->child[i])) return 0;
    for (int i = 0; i < n->nitems; ++i)
        if (!ssa_supported(n->items[i])) return 0;
    return 1;
}

static void ssa_dump(const SSA *s, const char *name) {
    if (!getenv("PAXSY_DUMP_SSA")) return;
    fprintf(stderr, "SSA %s: %d block(s), %d value(s); max statement-point-live %d; interference %d edge(s), max degree %d; abstract colouring %d colour(s), %d spill candidate(s) (K=8); simplified %d copy/copies, %d Phi node(s), %d single-use Phi(s), sunk %d expression(s), removed %d dead definition(s)\n",
            name ? name : "<anonymous>", s->cfg->count, s->next_value,
            s->peak_live, s->ninterference, s->max_interference_degree,
            s->abstract_color_count, s->abstract_spill_count,
            s->simplified_copies, s->simplified_phis,
            s->single_use_phis, s->sunk_exprs, s->dead_defs);
    for (int b = 0; b < s->cfg->count; ++b) {
        if (!s->cfg->blocks[b].reachable) continue;
        const SSABlock *sb = &s->blocks[b];
        fprintf(stderr, "  bb%d:", b);
        for (int i = 0; i < sb->nphis; ++i) {
            const SSAPhi *p = &sb->phis[i];
            fprintf(stderr, " v%d = phi(slot%d", p->value, p->slot);
            for (int j = 0; j < p->nincoming; ++j)
                fprintf(stderr, ", bb%d:v%d", p->preds[j], p->incoming[j]);
            fputc(')', stderr);
        }
        for (int i = 0; i < sb->nuses; ++i)
            fprintf(stderr, " use(slot%d=v%d)", sb->uses[i].slot,
                    sb->uses[i].value);
        for (int i = 0; i < sb->ndefs; ++i)
            fprintf(stderr, " def(slot%d=v%d)", sb->defs[i].slot,
                    sb->defs[i].value);
        fputc('\n', stderr);
    }
    if (s->ninterference) {
        fputs("  interference:", stderr);
        for (int i = 0; i < s->ninterference; ++i)
            fprintf(stderr, " v%d-v%d", s->interference[i].a,
                    s->interference[i].b);
        fputc('\n', stderr);
    }
}

static int ssa_build_verify(HModule *m, HNode *fn, CFG *g, int entry, long *rewrites) {
    if (!ssa_supported(fn->child[3])) return 1;
    int max_slot = -1;
    for (int i = 0; i < fn->nitems; ++i)
        if (fn->items[i] && fn->items[i]->kind == HK_PARAM) {
            if (fn->items[i]->u.index + 1 > max_slot + 1)
                max_slot = fn->items[i]->u.index;
        }
    ssa_scan_max_slot(fn->child[3], &max_slot);
    if (max_slot < 0) return 1;
    SSA s;
    memset(&s, 0, sizeof(s));
    s.cfg = g; s.entry = entry; s.nslots = max_slot + 1; s.valid = 1;
    for (int i = 0; i < fn->nitems; ++i)
        if (fn->items[i] && fn->items[i]->kind == HK_PARAM) s.nparams++;
    s.blocks = calloc((size_t)g->count, sizeof(*s.blocks));
    size_t slot_cells = (size_t)g->count * (size_t)s.nslots;
    if (s.nslots > 0 && slot_cells / (size_t)s.nslots != (size_t)g->count)
        s.valid = 0;
    s.has_def = s.valid ? calloc(slot_cells, 1) : NULL;
    s.placed = s.valid ? calloc(slot_cells, 1) : NULL;
    s.current = malloc((size_t)s.nslots * sizeof(*s.current));
    if (!s.blocks || !s.has_def || !s.placed || !s.current) s.valid = 0;
    if (s.valid) {
        for (int i = 0; i < s.nslots; ++i) s.current[i] = -1;
        for (int i = 0; i < s.nparams; ++i) {
            s.current[i] = ssa_new_value(&s);
            s.has_def[(size_t)entry * (size_t)s.nslots + (size_t)i] = 1;
        }
        for (int i = 0; i < fn->nitems; ++i)
            if (fn->items[i] && fn->items[i]->kind == HK_PARAM &&
                !ssa_type_supported(fn->items[i]->type)) s.valid = 0;
        for (int b = 0; b < g->count && s.valid; ++b)
            if (g->blocks[b].reachable) ssa_collect_block(&s, b);
        ssa_place_phis(&s);
        if (s.valid) ssa_rename(&s, entry);
        if (s.valid && !ssa_build_use_index(&s)) s.valid = 0;
    }
    int ok = ssa_verify(&s);
    if (ok && s.valid && !ssa_compute_liveness(&s)) s.valid = 0;
    if (ok && s.valid && !ssa_build_interference(&s)) s.valid = 0;
    if (ok && s.valid && !ssa_simplify_values(&s)) s.valid = 0;
    /* The colouring probe uses SSA use counts as spill-cost estimates.
     * Populate/canonicalize those counts before invoking it; running the
     * probe before value simplification made every node appear equally cheap. */
    if (ok && s.valid && !ssa_color_interference(&s, 8)) s.valid = 0;
    if (ok && s.valid) {
        if (!ssa_build_value_use_index(&s)) s.valid = 0;
        if (rewrites && s.valid) {
            *rewrites += ssa_sccp(&s);
            *rewrites += ssa_sink_single_use_expr(m, fn, &s);
            *rewrites += ssa_eliminate_dead_defs(fn, &s);
        }
        ssa_dump(&s, fn->name);
    }
    ssa_free(&s);
    return ok;
}


static void cfg_free(CFG *g) {
    for (int i = 0; i < g->count; ++i) {
        free(g->blocks[i].succ);
        if (g->pred) free(g->pred[i]);
        if (g->dom_children) free(g->dom_children[i]);
        if (g->frontier) free(g->frontier[i]);
    }
    free(g->pred); free(g->npred); free(g->pred_cap);
    free(g->idom); free(g->rpo); free(g->rpo_index);
    free(g->dom_pre); free(g->dom_end);
    free(g->dom_children); free(g->ndom_children); free(g->dom_child_cap);
    free(g->frontier); free(g->nfrontier); free(g->frontier_cap);
    free(g->stmt_index);
    free(g->blocks);
}


/* Count references to a local slot across a function.  This deliberately
 * counts every syntactic occurrence (including nested expressions), making
 * the return-sinking transform conservative in the presence of aliases or
 * additional uses. */
static void cfg_count_local_uses(HNode *n, int slot, int *uses) {
    if (!n) return;
    if (n->kind == HK_LOCAL && n->u.index == slot) (*uses)++;
    for (int i = 0; i < 4; ++i) cfg_count_local_uses(n->child[i], slot, uses);
    for (int i = 0; i < n->nitems; ++i)
        cfg_count_local_uses(n->items[i], slot, uses);
}

/* Validate the complete nested arm tree before mutating any node. */
static int cfg_arm_return_eligible(HNode *arm) {
    if (!arm) return 0;
    if (arm->kind == HK_BLOCK && arm->nitems > 0) {
        HNode *tail = arm->items[arm->nitems - 1];
        if (tail && tail->kind == HK_IF && (tail->flags & HF_EXPR))
            return cfg_arm_return_eligible(tail);
        return tail && tail->kind == HK_EXPR_STMT &&
               (tail->flags & HF_ARMVAL) && tail->child[0];
    }
    if (arm->kind == HK_IF && (arm->flags & HF_EXPR))
        return cfg_arm_return_eligible(arm->child[1]) &&
               cfg_arm_return_eligible(arm->child[2]);
    return 0;
}

/* Turn the value-producing tail of one if-expression arm into a return.
 * Nested value-if expressions are handled recursively.  A plain arm value
 * is represented by an HF_ARMVAL expression statement by the HIR lowerer. */
static int cfg_sink_return_into_arm(HNode *arm) {
    if (!arm) return 0;
    if (arm->kind == HK_BLOCK && arm->nitems > 0) {
        HNode *tail = arm->items[arm->nitems - 1];
        if (!tail) return 0;
        if (tail->kind == HK_IF && (tail->flags & HF_EXPR))
            return cfg_sink_return_into_arm(tail);
        if (tail->kind != HK_EXPR_STMT || !(tail->flags & HF_ARMVAL) ||
            !tail->child[0]) return 0;
        tail->kind = HK_RETURN;
        tail->flags &= ~HF_ARMVAL;
        return 1;
    }
    if (arm->kind == HK_IF && (arm->flags & HF_EXPR)) {
        int a = cfg_sink_return_into_arm(arm->child[1]);
        int b = cfg_sink_return_into_arm(arm->child[2]);
        return a && b;
    }
    return 0;
}

/* `let result = if cond { expr } else { expr }; return result;`
 * carries no useful storage: the branch result can flow directly to the
 * function exit.  Reuse the existing arm expression nodes as HK_RETURNs,
 * replace the let with its if, and remove the now-redundant outer return.
 * Only adjacent let/return pairs with exactly one local read are eligible;
 * this prevents moving evaluation across intervening statements or changing
 * the value observed by another use. */
static long cfg_sink_if_return_in_tree(HNode *n) {
    if (!n) return 0;
    long changes = 0;
    if (n->kind == HK_BLOCK) {
        /* Reuse the same block-local matcher without recursively revisiting
         * the just-rewritten if nodes. */
        for (int i = 0; i + 1 < n->nitems; ++i) {
            HNode *let = n->items[i];
            if (!let || let->kind != HK_LET || !let->child[1] ||
                let->child[1]->kind != HK_IF || !(let->child[1]->flags & HF_EXPR))
                continue;

            /* Follow a chain of single-use local aliases.  Each alias must be
             * the immediately following statement, so no evaluation is moved
             * across an observable operation. */
            int slot = let->u.index;
            int end = i + 1;
            int found_return = 0;
            while (end < n->nitems) {
                HNode *next = n->items[end];
                if (next && next->kind == HK_LET && next->child[1] &&
                    next->child[1]->kind == HK_LOCAL &&
                    next->child[1]->u.index == slot) {
                    int uses = 0;
                    cfg_count_local_uses(n, slot, &uses);
                    if (uses != 1) break;
                    slot = next->u.index;
                    end++;
                    continue;
                }
                if (next && next->kind == HK_RETURN && next->child[0] &&
                    next->child[0]->kind == HK_LOCAL &&
                    next->child[0]->u.index == slot) {
                    int uses = 0;
                    cfg_count_local_uses(n, slot, &uses);
                    found_return = (uses == 1);
                }
                break;
            }
            if (!found_return) continue;

            HNode *iff = let->child[1];
            if (!iff->child[1] || !iff->child[2] ||
                !cfg_arm_return_eligible(iff->child[1]) ||
                !cfg_arm_return_eligible(iff->child[2])) continue;
            cfg_sink_return_into_arm(iff->child[1]);
            cfg_sink_return_into_arm(iff->child[2]);

            n->items[i] = iff;
            int remove_count = end - i;
            for (int j = i + 1; j + remove_count < n->nitems; ++j)
                n->items[j] = n->items[j + remove_count];
            for (int j = n->nitems - remove_count; j < n->nitems; ++j)
                n->items[j] = NULL;
            n->nitems -= remove_count;
            changes++;
        }
    }
    for (int i = 0; i < 4; ++i) changes += cfg_sink_if_return_in_tree(n->child[i]);
    for (int i = 0; i < n->nitems; ++i) changes += cfg_sink_if_return_in_tree(n->items[i]);
    return changes;
}


long opt_cfg(HModule *m) {
    HNode *root = hir_root(m);
    if (!root) return 0;
    long changes = 0;
    for (int i = 0; i < root->nitems; ++i) {
        HNode *fn = root->items[i];
        if (!fn || fn->kind != HK_FUNC || !fn->child[3]) continue;
        changes += cfg_sink_if_return_in_tree(fn->child[3]);
        CFG g;
        memset(&g, 0, sizeof(g));
        g.valid = 1;
        int exit = -1;
        int entry = cfg_build_stmt(&g, fn->child[3], exit, NULL);
        if (g.valid && cfg_mark_reachable(&g, entry)) {
            /* Keep CFG and its dominance analyses alive together until all
             * control-flow work for this function has completed. */
            if (cfg_compute_dominators(&g, entry) &&
                cfg_verify_dominators(&g, entry) &&
                cfg_build_stmt_index(&g) &&
                ssa_build_verify(m, fn, &g, entry, &changes))
                changes += cfg_prune(&g, fn->child[3]);
        }
        cfg_free(&g);
    }
    return changes;
}
