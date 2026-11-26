#include "common.h"
#include "hir.h"
#include "opt.h"
#include <time.h>

/*
 * opt.c - the optimization pass manager plus the shared tree
 * utilities. The manager runs the enabled passes in a fixed order
 * (constant folding before strength reduction before dead-code
 * elimination) and repeats the round while any pass still makes
 * progress, so folds exposed by earlier removals get picked up.
 */

OptConfig opt_config_for_level(int level) {
    OptConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (level < 0 || level > 3) return cfg;
    if (level == 0) return cfg;

    /* O1 is deliberately cheap and predictable: local/tree rewrites only. */
    cfg.fold_constants = 1;
    cfg.strength_reduce = 1;
    cfg.dce = 1;
    cfg.prop = 1;
    cfg.simplify = 1;
    cfg.max_iterations = level == 1 ? 4 : (level == 2 ? 12 : 32);

    if (level >= 2) {
        cfg.cfg = 1;
        cfg.gvn = 1;
        cfg.store = 1;
    }
    if (level >= 3)
        cfg.sroa = 1;
    return cfg;
}

OptConfig opt_defaults(void) {
    return opt_config_for_level(3);
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
        if (n->u.binop == HOP_DIV || n->u.binop == HOP_MOD)
            return 0;
        return opt_is_pure(n->child[0]) && opt_is_pure(n->child[1]);
    case HK_UNOP:
        if (n->u.unop == HUN_DEREF) return 0;
        return opt_is_pure(n->child[0]);
    case HK_CAST:
        return opt_is_pure(n->child[0]);
    case HK_STRUCT_LIT:
    case HK_TUPLE_LIT:
    case HK_ARRAY_LIT:
        for (int i = 0; i < n->nitems; ++i)
            if (!opt_is_pure(n->items[i])) return 0;
        return 1;
    case HK_ADDR:
        return opt_is_pure(n->child[0]);
    case HK_INDEX:
    case HK_FIELD:
    case HK_DEREF:
        return 0;
    default:
        return 0;
    }
}

static double opt_cpu_seconds(clock_t begin, clock_t end) {
    return (double)(end - begin) / (double)CLOCKS_PER_SEC;
}

#define OPT_PROFILE_PASS(field, call_field, changes_field, expression) do { \
    clock_t _begin = profile ? clock() : (clock_t)0; \
    long _changes = (expression); \
    if (profile) { \
        profile->field##_seconds += opt_cpu_seconds(_begin, clock()); \
        profile->field##_calls++; \
        profile->field##_changes += _changes; \
    } \
    round += _changes; \
} while (0)

long opt_run_profile(HModule *m, const OptConfig *cfg, OptProfile *profile) {
    if (!m || !cfg) return 0;
    if (profile) memset(profile, 0, sizeof(*profile));
    long total = 0;
    /* Aggregate scalar replacement is a one-shot function analysis. Running
     * it once avoids rescanning unchanged aggregate candidates on every
     * fixpoint round; later DCE/propagation rounds consume its rewrites. */
    if (cfg->sroa && cfg->max_iterations > 0) {
        clock_t sroa_begin = profile ? clock() : (clock_t)0;
        long sroa_changes = opt_sroa(m);
        if (profile) {
            profile->sroa_seconds += opt_cpu_seconds(sroa_begin, clock());
            profile->sroa_calls++;
            profile->sroa_changes += sroa_changes;
        }
        total += sroa_changes;
    }
    for (int iter = 0; iter < cfg->max_iterations; iter++) {
        long round = 0;
        if (profile) profile->rounds++;
        if (cfg->cfg) OPT_PROFILE_PASS(cfg, cfg_calls, cfg_changes, opt_cfg(m));
        if (cfg->prop) OPT_PROFILE_PASS(prop, prop_calls, prop_changes, opt_prop(m));
        if (cfg->gvn) OPT_PROFILE_PASS(gvn, gvn_calls, gvn_changes, opt_gvn(m));
        if (cfg->fold_constants) OPT_PROFILE_PASS(fold, fold_calls, fold_changes, opt_fold(m));
        if (cfg->strength_reduce) OPT_PROFILE_PASS(strength, strength_calls, strength_changes, opt_strength(m));
        if (cfg->store) OPT_PROFILE_PASS(store, store_calls, store_changes, opt_store(m));
        if (cfg->dce) OPT_PROFILE_PASS(dce, dce_calls, dce_changes, opt_dce(m));
        if (cfg->simplify) OPT_PROFILE_PASS(simplify, simplify_calls, simplify_changes, opt_simplify(m));
        total += round;
        if (round == 0) break;
    }
    return total;
}

long opt_run(HModule *m, const OptConfig *cfg) {
    return opt_run_profile(m, cfg, NULL);
}
