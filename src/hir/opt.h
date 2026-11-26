#ifndef PAXSY_OPT_H
#define PAXSY_OPT_H

/*
 * opt.h - the HIR optimisation pipeline: the public interface and, in
 * one header, the helpers shared by the individual passes.
 *
 * Optimization runs on typed HIR after type inference and before any
 * emission backend (C99, LLVM assembly, MLIR, CUDA). Every pass
 * mutates the HModule in place and reports how many modifications it
 * performed, so the driver can iterate the pipeline to a fixpoint
 * while progress is still being made. Nodes that are detached by a
 * pass stay owned by the module arena and are released together with
 * the module.
 */

#include "common.h"
#include "hir.h"

typedef struct {
    double cfg_seconds, prop_seconds, gvn_seconds, fold_seconds;
    double strength_seconds, store_seconds, dce_seconds, simplify_seconds;
    double sroa_seconds;
    unsigned long cfg_calls, prop_calls, gvn_calls, fold_calls;
    unsigned long strength_calls, store_calls, dce_calls, simplify_calls;
    unsigned long sroa_calls;
    long cfg_changes, prop_changes, gvn_changes, fold_changes;
    long strength_changes, store_changes, dce_changes, simplify_changes;
    long sroa_changes;
    unsigned long rounds;
} OptProfile;

typedef struct {
    int fold_constants;  /* evaluate constant expressions, fold branches */
    int strength_reduce; /* replace expensive ops with cheaper ones    */
    int dce;             /* drop dead lets and pure expression stmts   */
    int prop;            /* constant/copy propagation over locals      */
    int cfg;             /* CFG simplification and scalar SSA/SCCP     */
    int gvn;             /* global value numbering                     */
    int store;           /* dead-store elimination                     */
    int sroa;            /* scalar replacement of aggregates           */
    int simplify;        /* structural and expression simplification   */
    int max_iterations;  /* fixpoint bound for the whole pipeline      */
} OptConfig;

/* Returns the strongest supported optimisation configuration. */
OptConfig opt_defaults(void);
/* Builds a validated public optimisation tier (0..3). */
OptConfig opt_config_for_level(int level);

/* Runs the enabled passes over every function of the module and
 * returns the total number of modifications; zero means the module
 * is already stable. */
long opt_run(HModule *m, const OptConfig *cfg);
/* Like opt_run, but optionally records per-pass CPU timings and work. */
long opt_run_profile(HModule *m, const OptConfig *cfg, OptProfile *profile);

/* ------------------------------------------------------------------ */
/* Helpers shared by the passes; nothing outside src/hir/opt_*.c       */
/* should use them.                                                    */
/* ------------------------------------------------------------------ */

/* Visitor signature for the generic tree walker: `n` is the visited
 * node and `ctx` is an opaque pointer supplied by the caller. */
typedef void (*OptVisit)(HNode *n, void *ctx);

/* Walks the subtree rooted at `n` depth-first: `pre` fires before the
 * children, `post` fires after them. NULL callbacks are skipped. */
void opt_walk(HNode *n, OptVisit pre, OptVisit post, void *ctx);

/* Points every slot of `parent` that references `old_node` at
 * `new_node` instead. Returns 1 when at least one slot was rewired. */
int opt_rewire(HNode *parent, HNode *old_node, HNode *new_node);

/* Clears the slot of `parent` that references `child`, detaching the
 * child from the tree. The detached node remains owned by the HIR
 * module arena. */
void opt_detach(HNode *parent, HNode *child);

/* Returns 1 when evaluating `n` has no observable effect: reading
 * constants, locals and parameters, and pure arithmetic on them.
 * Calls, allocations, channel operations and inline asm are never
 * pure, which keeps the check conservative and safe. */
int opt_is_pure(const HNode *n);

/* ------------------------------------------------------------------ */
/* Individual passes; each returns the number of modifications made.  */
/* ------------------------------------------------------------------ */

long opt_fold(HModule *m);          /* opt_fold.c      */
long opt_strength(HModule *m);      /* opt_strength.c  */
long opt_dce(HModule *m);           /* opt_dce.c       */
long opt_prop(HModule *m);          /* opt_prop.c      */
long opt_simplify(HModule *m);      /* opt_simplify.c  */
long opt_sroa(HModule *m);          /* opt_simplify.c  */
long opt_store(HModule *m);         /* opt_store.c     */
long opt_gvn(HModule *m);           /* opt_gvn.c       */
long opt_cfg(HModule *m);           /* opt_cfg.c       */

#endif /* PAXSY_OPT_H */
