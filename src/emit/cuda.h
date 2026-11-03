#ifndef PAXSY_CUDA_H
#define PAXSY_CUDA_H

/*
 * cuda.h - the CUDA C++ emission pass (GPU artifact backend).
 * cuda_emit_modules() writes one CUDA translation unit carrying a
 * device kernel and a host launcher for every for-range loop that the
 * offload analysis classifies as a GPU candidate and whose body
 * matches a supported parallel pattern:
 *   sum-reduce: acc += <expr(i)>  -> one atomicAdd per thread;
 *   map:        arr[i] = <expr(i)> -> one store per thread.
 * Loops that are candidates but match neither pattern are skipped
 * with a note; CPU-only loops are not emitted at all. The unit is an
 * artifact of the build: it compiles with nvcc into the same project
 * when a CUDA toolchain is present, while the C backend keeps
 * producing a working host binary.
 */

#include "hir.h"
#include "diag.h"

int cuda_emit_modules(HModule **mods, int nmods, Diag *diag, FILE *out);

#endif /* PAXSY_CUDA_H */
