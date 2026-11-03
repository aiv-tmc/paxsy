#ifndef PAXSY_CEMIT_H
#define PAXSY_CEMIT_H

/*
 * cemit.h - the C99 emission pass (executable backend, variant C).
 * cemit_modules() prints one self-contained C99 translation unit for
 * a set of typed HIR modules: prototypes for every function come
 * first, then the bodies. Control flow maps directly onto C, integer
 * widths map onto stdint fixed-width types, and the standard output
 * calls of the seed library map onto stdio. The entry point main()
 * returns the process exit status; a Void! return yields 0.
 * Returns the number of errors found (unsupported constructs are
 * diagnosed, never silently dropped).
 */

#include "hir.h"
#include "diag.h"

int cemit_modules(HModule **mods, int nmods, Diag *diag, FILE *out);

#endif /* PAXSY_CEMIT_H */
