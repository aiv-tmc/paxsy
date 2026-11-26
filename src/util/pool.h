#ifndef PAXSY_POOL_H
#define PAXSY_POOL_H

/*
 * pool.h - a minimal fixed-size worker pool for data-parallel driver
 * work (one independent job per source file). The pool spawns at
 * most as many pthread workers as there are jobs, so single-file
 * projects never pay for threads they cannot use. The worker count
 * honours the PAXSY_JOBS environment variable when it is set to a
 * positive integer and falls back to the number of online CPUs;
 * PAXSY_JOBS=1 forces the fully sequential path.
 *
 * Contract: every job index in [0, count) is delivered to fn exactly
 * once; job indices are claimed through a shared counter, so a slow
 * file never blocks an idle worker and the completion order is
 * unspecified by design. fn receives the job index and the opaque
 * context pointer; all cross-job coordination (result slots indexed
 * by the job index, print mutexes, atomics) is the caller's
 * responsibility, keeping this unit free of any policy.
 */

/* Runs fn(index, ctx) for every index in [0, count). Returns
 * immediately when count is zero or negative. When more than one
 * worker is available the calls execute concurrently; when PAXSY_JOBS
 * resolves to 1 (or the thread table cannot be allocated) the calls
 * run on the calling thread in index order. */
void pool_run(void (*fn)(int index, void *ctx), int count, void *ctx);

#endif /* PAXSY_POOL_H */
