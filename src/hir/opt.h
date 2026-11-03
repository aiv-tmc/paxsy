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
    int fold_constants;  /* evaluate constant expressions, fold branches */
    int strength_reduce; /* replace expensive ops with cheaper ones    */
    int dce;             /* drop dead lets and pure expression stmts   */
    int prop;            /* constant/copy propagation over locals      */
    int max_iterations;  /* fixpoint bound for the whole pipeline      */
} OptConfig;

/* Returns the default configuration: every pass enabled, bounded at
 * eight fixpoint iterations so pathological inputs terminate. */
OptConfig opt_defaults(void);

/* Runs the enabled passes over every function of the module and
 * returns the total number of modifications; zero means the module
 * is already stable. */
long opt_run(HModule *m, const OptConfig *cfg);

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

#endif /* PAXSY_OPT_H */
