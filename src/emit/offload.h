#ifndef PAXSY_OFFLOAD_H
#define PAXSY_OFFLOAD_H

/*
 * offload.h - the GPU offload analysis.
 * offload_report() classifies every for-range loop of every typed
 * HIR module as a GPU candidate or as CPU-only, printing one line per
 * loop together with the reason. A loop is a candidate when its body
 * carries none of the effect flags that forbid device execution
 * (channel operations, heap allocation, defer, spawns) and consists
 * only of node kinds the device subset supports. The classification
 * is advisory in this version: the code generators still emit host
 * code for every loop.
 */

#include "hir.h"

void offload_report(HModule **mods, int nmods, FILE *out);

/* Classifies one for-range loop for device execution. Returns NULL
 * when the loop is a GPU candidate, or a static string naming the
 * first blocking reason (channel effects, allocation, defer, spawns,
 * open bounds, missing loop variable, unsupported body shape). */
const char *offload_candidate(const HNode *loop);

/* Recognizes the supported parallel body patterns of a candidate
 * loop: "sum" for acc += <expr(i)> (acc_slot receives the slot) and
 * "map" for arr[i] = <expr(i)>. Returns NULL when the body matches
 * no supported pattern; the loop then stays on the host. */
const char *offload_pattern(const HNode *loop, int *acc_slot);

/* Simulates every sum-reduce candidate with constant bounds on the
 * CPU: the serial order against a 256-thread chunked partial-sum
 * reduction, which mirrors the grid the CUDA launcher uses. Integer
 * arithmetic is associative, so both orders must agree; a mismatch
 * would flag a compiler bug. Non-constant loops are reported as
 * skipped. */
void offload_simulate(HModule **mods, int nmods, FILE *out);

/* Collects distinct PARAM slots read by a device expression. */
void offload_expr_params(const HNode *e, int *slots, int *cnt);

#endif /* PAXSY_OFFLOAD_H */
