#ifndef PAXSY_CEMIT_INT_H
#define PAXSY_CEMIT_INT_H

/*
 * cemit_int.h - shared internals of the C99 emission pass: the CE
 * context, the C type printer, the result-struct namer and the
 * printers for expressions, statements, branches, function
 * signatures and bodies, plus the module-level emission helpers.
 */

#include "cemit.h"

typedef struct {
    HModule **mods;
    int nmods;
    Diag *diag;
    FILE *out;
    const HNode *cur_func;  /* function under emission, for the T! ABI */
    int temp;               /* scratch result counter for try/catch   */
    int kernel;             /* GPU kernel ordinal, mirrors cuda-emit  */
    int nstr;               /* string constant counter (node aux ids) */
    int glbl;               /* ordinal for generated label names      */
    const char *cur_inc;    /* active for-loop increment label, or
                               NULL: unlabeled continue targets it so
                               the increment always runs              */
    struct {
        const char *name;   /* source label                */
        char brk[32];       /* generated break target      */
        char cont[32];      /* generated continue target   */
    } lbl[16];
    int nlbl;
    int alias_slot;         /* LOCAL slot printed as alias_name, -1 off */
    const char *alias_name; /* e.g. "px_i" inside CUDA kernels         */
} CE;

/* Reports one unsupported construct through the shared stream. */
void cerr_(CE *c, const HNode *n, const char *what);

/* One indentation level (four spaces) written `depth` times. */
void cindent(CE *c, int depth);

/* Renders the C name of a local slot into the caller's buffer, so
 * nested value-branch emissions cannot clobber each other's target
 * names. */
void slotname(int slot, char *buf, size_t n);

/* Emits an if/match expression as nested C assignments into target. */
void cassign_branch(CE *c, const HNode *v, const char *target, int depth);

/* Emits a block's statements and assigns its trailing expression
 * value into target; returns 1 when the block produced a value. */
int cblock_into(CE *c, const HNode *blk, const char *target, int depth);

void cty(CE *c, const HType *t);
void cexpr(CE *c, const HNode *n);
void cstmt(CE *c, const HNode *n, int depth);

/* Control-flow helpers living in cemit_ctrl.c (try/catch lowering,
 * range loops, labeled loops, select). */
void cur_res_name(CE *c, char *buf, size_t n);
void ctry_let(CE *c, const HNode *call, const char *target,
              const HType *ty, int depth);
void ccatch_let(CE *c, const HNode *cat, const char *target,
                const HType *ty, int depth);
void cfor_host(CE *c, const HNode *n, int depth);
int cfor_gpu(CE *c, const HNode *n, int depth);
void clabeled(CE *c, const HNode *lab, int depth);
void cselect(CE *c, const HNode *n, int depth);
int cstmt_aux(CE *c, const HNode *n, int depth);
const char *find_lbl(CE *c, const char *name, int is_break);

void cfunc_sig(CE *c, const HNode *f);
void cfunc_body(CE *c, const HNode *f);

/* Emits struct/union/enum definitions for every module. */
void cmod_aggregates(CE *c);

/* Emits the channel runtime (only called when channels are used). */
void cmod_runtime(CE *c);

/* Emits module-level variable definitions. */
void cmod_globals(CE *c);

/* Emits the std::inout::println shims for the used variants. */
void cmod_stdlib(CE *c, int need_i, int need_s);

/* Finds a function declaration by name across all modules. */
const HNode *cfind_func(CE *c, const char *name);

void cres_name(const HType *value_type, char *buf, size_t n);
const HType *cfunc_res_value(const HNode *f);
int cis_main(CE *c);

/* Shared low-level helpers (cemit.c). */
void cname(const char *src, char *buf, size_t n);
void cslot(char *buf, int is_param, int index);
void cesc(CE *c, const char *s, int len);
int cneeds_runtime(HModule **mods, int nmods);

#endif /* PAXSY_CEMIT_INT_H */
