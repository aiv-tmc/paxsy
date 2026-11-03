#include "common.h"
#include "hir.h"
#include "opt.h"
#include "opt.h"

/*
 * opt.c - the optimization pass manager plus the shared tree
 * utilities. The manager runs the enabled passes in a fixed order
 * (constant folding before strength reduction before dead-code
 * elimination) and repeats the round while any pass still makes
 * progress, so folds exposed by earlier removals get picked up.
 */

OptConfig opt_defaults(void) {
    OptConfig cfg;
    cfg.fold_constants = 1;
    cfg.strength_reduce = 1;
    cfg.dce = 1;
    cfg.prop = 1;
    cfg.max_iterations = 8;
    return cfg;
}

void opt_walk(HNode *n, OptVisit pre, OptVisit post, void *ctx) {
    if (!n) return;
    if (pre) pre(n, ctx);
    for (int i = 0; i < 4; i++)
        opt_walk(n->child[i], pre, post, ctx);
    for (int i = 0; i < n->nitems; i++)
        opt_walk(n->items[i], pre, post, ctx);
    if (post) post(n, ctx);
}

int opt_rewire(HNode *parent, HNode *old_node, HNode *new_node) {
    if (!parent || !old_node) return 0;
    int rewired = 0;
    for (int i = 0; i < 4; i++) {
        if (parent->child[i] == old_node) {
            parent->child[i] = new_node;
            rewired = 1;
        }
    }
    for (int i = 0; i < parent->nitems; i++) {
        if (parent->items[i] == old_node) {
            parent->items[i] = new_node;
            rewired = 1;
        }
    }
    return rewired;
}

void opt_detach(HNode *parent, HNode *child) {
    if (!parent || !child) return;
    for (int i = 0; i < 4; i++)
        if (parent->child[i] == child) parent->child[i] = NULL;
    for (int i = 0; i < parent->nitems; i++)
        if (parent->items[i] == child) parent->items[i] = NULL;
}

int opt_is_pure(const HNode *n) {
    if (!n) return 1;
    switch (n->kind) {
    case HK_CONST_INT:
    case HK_CONST_FLOAT:
    case HK_CONST_STRING:
    case HK_CONST_CHAR:
    case HK_PARAM:
    case HK_LOCAL:
    case HK_GLOBAL_REF:
        return 1;
    case HK_BINOP:
    case HK_UNOP:
    case HK_CAST:
        return opt_is_pure(n->child[0]) && opt_is_pure(n->child[1]);
    case HK_INDEX:
    case HK_FIELD:
        return opt_is_pure(n->child[0]) && opt_is_pure(n->child[1]);
    case HK_ADDR:
    case HK_DEREF:
        return opt_is_pure(n->child[0]);
    default:
        return 0;
    }
}

long opt_run(HModule *m, const OptConfig *cfg) {
    if (!m || !cfg) return 0;
    long total = 0;
    for (int iter = 0; iter < cfg->max_iterations; iter++) {
        long round = 0;
        if (cfg->prop)           round += opt_prop(m);
        if (cfg->fold_constants) round += opt_fold(m);
        if (cfg->strength_reduce) round += opt_strength(m);
        if (cfg->dce)            round += opt_dce(m);
        total += round;
        if (round == 0) break;
    }
    return total;
}
