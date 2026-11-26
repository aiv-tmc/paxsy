#ifndef PAXSY_LLIC_H
#define PAXSY_LLIC_H

/*
 * llic.h - the LLVM IR emission pass.
 * llic_modules() prints one LLVM assembly translation unit for a set
 * of typed HIR modules. It mirrors the C99 backend: every function
 * becomes a definition with slot-indexed parameters (%p0, %p1) and
 * slot-indexed allocas for locals, control flow lowers onto named
 * basic blocks, and integer widths map onto LLVM integer types.
 * The emission targets modern LLVM with opaque pointers. String
 * constants are emitted as module-level private globals. The seed
 * println maps onto puts for strings and printf for integers.
 * Returns the number of errors found; unsupported constructs are
 * diagnosed, never silently dropped.
 */

#include "hir.h"
#include "diag.h"

int llic_modules(HModule **mods, int nmods, Diag *diag, FILE *out);

#endif /* PAXSY_LLIC_H */
