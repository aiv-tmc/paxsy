#ifndef PAXSY_NSEL_H
#define PAXSY_NSEL_H

/*
 * nsel.h - shared state of the native instruction-selection pass.
 * One NSel carries the code and data buffers being built, the
 * absolute address fixups pending final layout, the string-constant
 * and global-storage tables and the per-function selection state.
 * The internal calling convention is fixed here: integer and pointer
 * values live in 64-bit registers; function arguments travel in rdi,
 * rsi, rdx, rcx, r8, r9 and are spilled into parameter slots by the
 * prologue; a plain result returns in rax; a T! result returns the
 * payload in rax and the error code in rdx. Locals occupy rbp-
 * relative slots with per-slot sizes (arrays inline), parameters stay
 * in their incoming stack slots above rbp.
 */

#include "common.h"
#include "hir.h"
#include "diag.h"
#include "nx86.h"

typedef struct {
    HModule **mods;    /* modules under emission                    */
    int nmods;
    Diag *diag;
    NXBuf *text;       /* growing code buffer                       */
    NXBuf *ro;         /* read-only data (string bytes)             */
    uint8_t *data;     /* writable data (globals)                   */
    size_t data_len, data_cap;
    /* Absolute-address fixups: the 8 bytes at code offset `pos`
     * receive va(sector `sect`) + `off` after layout. */
    struct { uint32_t pos; int sect; uint32_t off; } *abs;
    int nabs, capabs;
    int err;           /* diagnostics produced by selection         */
    /* String constant table, indexed by the node aux id. */
    uint32_t *str_off;
    int *str_len;
    int nstr;
    const HNode *cur_func;  /* function under selection, for the ABI */
    int nl;            /* first temporary slot of the current function */
    int ep;            /* epilogue label of the current function  */
    /* Per-slot frame layout of the current function: `lsz[k]` is
     * the byte size of slot k and `loff[k]` its negative offset from
     * rbp (filled by nsel_func before the body is emitted). */
    int *lsz;
    int *loff;
    int lcap;
    /* Global storage: source name to its offset inside the writable
     * data image. */
    struct { const char *name; uint32_t off; } *gl;
    int ngl, capgl;
    /* Start-up label, defined before user code. */
    int rt_start, lbl_main;
    /* User function table: source name to text label. */
    struct { const char *name; int lbl; } *fn;
    int nfn, capfn;
    /* Unlabeled break/continue targets of the enclosing loops. */
    int brk[16];
    int cont[16];
    int depth;
} NSel;

enum { NSECT_RO = 0, NSECT_DATA = 1 };

void nerr(NSel *s, const HNode *n, const char *what);

/* Emits the freestanding start-up sequence (the ELF entry point) at
 * the beginning of the text buffer (see nrt.c). */
void nrt_emit(NSel *s);

/* Assembles one inline-asm statement into the current code buffer
 * (see the nasm implementation in nsel.c). */
void nasm_stmt(NSel *s, const HNode *n);

/* Emits an imm64 placeholder at the buffer end minus 8 and records
 * an absolute fixup for sector `sect` offset `off`, patched with
 * the final virtual address after layout. */
void nabs(NSel *s, int sect, uint32_t off);

/* Emits one function (prologue, body, epilogue). */
void nsel_func(NSel *s, const HNode *f);

/* Emits one statement (see nsel_stmt.c). */
void nstmt(NSel *s, const HNode *n);

/* Emits one value expression into rax (see nsel_expr.c). */
void nexpr(NSel *s, const HNode *n);

/* Emits the address of an l-value into rax (see nsel_expr.c). */
void nlval(NSel *s, const HNode *n);

/* Emits the address of base[expr] into rax (see naddr.c). */
void naddr_index(NSel *s, const HNode *n);

/* Stack-byte offset of local slot `k` below rbp in the current
 * function's frame (valid only while a function is under selection;
 * loff[] is filled by nsel_func before the body runs). */
#define nslot_off(s, k) ((s)->loff[k])

/* Stack-byte offset of parameter `i` above rbp: the saved rbp and
 * the return address occupy the two words below the first
 * argument. */
#define nparam_off(i) (16 + 8 * (i))

#endif /* PAXSY_NSEL_H */
