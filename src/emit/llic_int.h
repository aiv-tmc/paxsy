#ifndef PAXSY_LLIC_INT_H
#define PAXSY_LLIC_INT_H

/*
 * llic_int.h - shared internals of the LLVM IR emission pass: the LE
 * context (modules, diagnostics, output, register and label counters,
 * the function under emission, its terminator state and the per-slot
 * type table), the LLVM type printer, the result-struct namer, the
 * slot type helpers, the name mangler, the string collector and the
 * printers for expressions and interpolated strings.
 */

#include "llic.h"

typedef struct {
    HModule **mods;
    int nmods;
    Diag *diag;
    FILE *out;
    int reg;            /* unnamed SSA register counter */
    int label;          /* basic block counter */
    int strno;          /* string constant counter */
    const HNode *cur_func;  /* function under emission, for the T! ABI */
    int terminated;         /* current block already ends in a terminator */
    char (*slot_ty)[8];     /* normalized LLVM type per local slot */
    int nslot_ty, capslot_ty;
} LE;

void lerr_(LE *l, const HNode *n, const char *what);
void lty(LE *l, const HType *t);
void lty_buf(const HType *t, char *buf, size_t n);
void lname(const HNode *n, char *buf, size_t nbuf);
void lstr_body(LE *l, const char *s, int len);
void lcollect_strings(LE *l, HNode *n);
void lcollect_fmt(LE *l, HNode *n);
void lexpr(LE *l, const HNode *n, char *res, size_t rn);
void lexpr_fmt(LE *l, const HNode *n, char *res, size_t rn);
void lstmt(LE *l, const HNode *n, int depth);
void lblock(LE *l, const HNode *n, int depth);
int lfunc_sig(LE *l, const HNode *f);
void lfunc_body(LE *l, const HNode *f);
void lres_name(const HType *value_type, char *buf, size_t n);
const HType *lfunc_res_value(const HNode *f);
void lslot_set(LE *l, int slot, const HType *ty);
const char *lslot_ty(LE *l, int slot);
void lslot_norm(const HType *ty, char *buf, size_t n);

#endif /* PAXSY_LLIC_INT_H */
