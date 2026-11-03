#ifndef PAXSY_MLIR_H
#define PAXSY_MLIR_H

/*
 * mlir.h - the MLIR emission pass: the public entry point and, in one
 * header, the shared state of the emission unit.
 *
 * mlir_modules() prints one MLIR translation unit (builtin func, arith,
 * cf and llvm dialects) for a set of typed HIR modules. Every function
 * becomes a func.func with slot-indexed parameters, every local lives
 * in an llvm.alloca addressed by its HIR slot, control flow lowers
 * onto cf.br / cf.cond_br between named blocks, and string constants
 * become llvm.mlir.globals. The output feeds mlir-translate
 * --mlir-to-llvmir unchanged. Returns the number of errors;
 * unsupported constructs are diagnosed, never silently dropped.
 */

#include "common.h"
#include "hir.h"
#include "diag.h"

int mlir_modules(HModule **mods, int nmods, Diag *diag, FILE *out);

/* ------------------------------------------------------------------ */
/* Shared state and helpers of the emission unit; nothing outside      */
/* src/emit/mlir_*.c should use them.                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    Diag *diag;
    FILE *out;
    int errs;          /* errors reported against this unit           */
    unsigned ssav;     /* next anonymous SSA value number             */
    unsigned blk;      /* next block label number                     */
    int terminated;    /* current block already ended in a terminator */
} ME;

/* Reports one unsupported construct; the node carries its source
 * position for the diagnostic. */
void merr(ME *m, const HNode *n, const char *what);

/* Renders the MLIR type of an HIR type into a buffer. */
void mty(const HType *t, char *buf, size_t n);

/* Prints one value expression; the resulting SSA name lands in `res`.
 * Instructions are appended to the current block. */
void mexpr(ME *m, const HNode *n, char *res, size_t rn);

/* Emits one llvm.alloca per local slot found in the subtree. */
void mallocs(ME *m, const HNode *n);

#endif /* PAXSY_MLIR_H */
