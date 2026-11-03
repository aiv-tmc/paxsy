#include "common.h"
#include "mlir.h"
#include "mlir.h"

/*
 * mlir_emit.c - the driver of the MLIR emission pass.
 *
 * Every function becomes one func.func: parameters arrive as %p0,
 * %p1, ... and every mutable local gets an llvm.alloca addressed by
 * its HIR slot, so later MLIR passes can raise the code to SSA
 * without further information. Control flow is explicit: named blocks
 * (^bb0 is always the entry block, further labels are numbered from
 * one) connected by cf.br and cf.cond_br. String constants are
 * collected per module and printed as llvm.mlir.globals (payload
 * wrapped in \22...\22, the MLIR string escape) before any function
 * body, so addressof references resolve forward. The entry block
 * label precedes every alloca, because a region's instructions belong
 * to blocks, never to the region itself. A block that already ended
 * in a terminator (return or a conditional merge) never receives a
 * second one; the trailing func.return is emitted only for functions
 * whose body fell through. Function names render with module
 * separators as underscores, so the func.func of every module -
 * including the standard library compiled from Paxsy sources - is
 * the exact symbol call sites reference, and the unit needs no
 * external declarations of its own.
 */

/* Emits one llvm.alloca per local slot found in the subtree, so every
 * slot reference lowers to llvm.load / llvm.store. */
void mallocs(ME *m, const HNode *n) {
    if (!n) return;
    if (n->kind == HK_LET && n->u.index >= 0) {
        char ty[32];
        mty(n->type, ty, sizeof(ty));
        fprintf(m->out, "    %%slot%d = llvm.alloca %s : !llvm.ptr\n",
                n->u.index, ty);
    }
    for (int i = 0; i < 4; i++) mallocs(m, n->child[i]);
    for (int i = 0; i < n->nitems; i++) mallocs(m, n->items[i]);
}

/* Assigns the next string-table id to every string/char constant in
 * the subtree and prints it as an llvm.mlir.global. Identical slices
 * share one global through a linear scan of the table; programs carry
 * few distinct strings, so the quadratic check is not measurable next
 * to file IO. */
static void mstring_walk(ME *m, const HNode *n, char table[][80], int *ntab) {
    if (!n) return;
    if ((n->kind == HK_CONST_STRING || n->kind == HK_CONST_CHAR) &&
        n->aux < 0) {
        char key[76];
        int klen = n->u.slice.len < 70 ? (int)n->u.slice.len : 70;
        memcpy(key, n->u.slice.start, (size_t)klen);
        key[klen] = '\0';
        int id = -1;
        for (int i = 0; i < *ntab; i++)
            if (strcmp(table[i], key) == 0) { id = i; break; }
        if (id < 0) {
            id = (*ntab)++;
            snprintf(table[id], 80, "%s", key);
            fprintf(m->out,
                    "  llvm.mlir.global private @px_s%d(\"\\22%s\\22\")"
                    " : !llvm.array<%d x i8>\n",
                    id, key,
                    (int)n->u.slice.len > 0 ? (int)n->u.slice.len : 1);
        }
        ((HNode *)n)->aux = id;
    }
    for (int i = 0; i < 4; i++) mstring_walk(m, n->child[i], table, ntab);
    for (int i = 0; i < n->nitems; i++) mstring_walk(m, n->items[i], table, ntab);
}

/* Runs the string-table pass over every module. */
static void mglobals(ME *m, HModule **mods, int nmods) {
    char table[64][80];
    int ntab = 0;
    for (int i = 0; i < nmods; i++) {
        HNode *root = hir_root(mods[i]);
        if (!root) continue;
        for (int j = 0; j < root->nitems; j++)
            mstring_walk(m, root->items[j], table, &ntab);
    }
}

/* Emits one statement into the current block. Terminators close the
 * block and set the terminated flag; the caller opens the next block
 * with a label. */
static void mstmt(ME *m, const HNode *n) {
    if (!n) return;
    switch (n->kind) {
    case HK_BLOCK:
        for (int i = 0; i < n->nitems; i++) mstmt(m, n->items[i]);
        break;
    case HK_EXPR_STMT: {
        char r[32];
        mexpr(m, n->child[0], r, sizeof(r));
        break;
    }
    case HK_LET: {
        char ty[32], v[32];
        if (!n->child[1]) break;
        mty(n->type, ty, sizeof(ty));
        mexpr(m, n->child[1], v, sizeof(v));
        fprintf(m->out, "    llvm.store %s, %%slot%d : %s, !llvm.ptr\n",
                v, n->u.index, ty);
        break;
    }
    case HK_RETURN: {
        char v[32];
        if (n->child[0]) {
            mexpr(m, n->child[0], v, sizeof(v));
            fprintf(m->out, "    func.return %s\n", v);
        } else {
            fputs("    func.return\n", m->out);
        }
        m->terminated = 1;
        break;
    }
    case HK_IF: {
        char c[32];
        unsigned bt = m->blk++, bf = m->blk++, bm = m->blk++;
        mexpr(m, n->child[0], c, sizeof(c));
        fprintf(m->out, "    cf.cond_br %s, ^bb%u, ^bb%u\n", c, bt, bf);
        fprintf(m->out, "  ^bb%u:\n", bt);
        m->terminated = 0;
        mstmt(m, n->child[1]);
        fprintf(m->out, "    cf.br ^bb%u\n  ^bb%u:\n", bm, bf);
        m->terminated = 0;
        if (n->child[2]) mstmt(m, n->child[2]);
        fprintf(m->out, "    cf.br ^bb%u\n  ^bb%u:\n", bm, bm);
        m->terminated = 0;
        break;
    }
    case HK_WHILE: {
        char c[32];
        unsigned bh = m->blk++, bb = m->blk++, be = m->blk++;
        fprintf(m->out, "    cf.br ^bb%u\n  ^bb%u:\n", bh, bh);
        m->terminated = 0;
        mexpr(m, n->child[0], c, sizeof(c));
        fprintf(m->out, "    cf.cond_br %s, ^bb%u, ^bb%u\n", c, bb, be);
        fprintf(m->out, "  ^bb%u:\n", bb);
        m->terminated = 0;
        mstmt(m, n->child[1]);
        fprintf(m->out, "    cf.br ^bb%u\n  ^bb%u:\n", bh, be);
        m->terminated = 0;
        break;
    }
    default:
        merr(m, n, "this statement form");
        break;
    }
}

/* Emits one function definition (extern prototypes stay declarations
 * only and are skipped). The name renders with module separators as
 * underscores, matching the symbols call sites reference. */
static void mfunc(ME *m, const HNode *f) {
    char rt[32], fname[96];
    if (f->kind == HK_EXTERN_FUNC) return;
    mty(f->type, rt, sizeof(rt));
    {
        size_t o = 0;
        const char *p = f->name;
        for (; *p && o + 1 < sizeof(fname); p++) {
            char ch = *p;
            if (ch == ':' || ch == '.') ch = '_';
            fname[o++] = ch;
        }
        fname[o] = '\0';
    }
    fprintf(m->out, "func.func @%s(", fname);
    for (int i = 0; i < f->nitems; i++) {
        char ty[32];
        mty(f->items[i]->type, ty, sizeof(ty));
        fprintf(m->out, "%s%%p%d: %s", i ? ", " : "", i, ty);
    }
    fprintf(m->out, ") -> %s {\n", rt);
    m->ssav = 0;
    m->blk = 1;               /* ^bb0 is the entry block */
    m->terminated = 0;
    fputs("  ^bb0:\n", m->out);
    mallocs(m, f->child[3]);
    mstmt(m, f->child[3]);
    if (!m->terminated)
        fputs("    func.return\n", m->out);
    fputs("}\n", m->out);
}

int mlir_modules(HModule **mods, int nmods, Diag *diag, FILE *out) {
    ME m;
    memset(&m, 0, sizeof(m));
    m.diag = diag;
    m.out = out;
    fputs("builtin.module {\n", out);
    mglobals(&m, mods, nmods);
    for (int i = 0; i < nmods; i++) {
        HNode *root = hir_root(mods[i]);
        if (!root) continue;
        for (int j = 0; j < root->nitems; j++)
            if (root->items[j] && root->items[j]->kind == HK_FUNC)
                mfunc(&m, root->items[j]);
    }
    fputs("}\n", out);
    return m.errs;
}
