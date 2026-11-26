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
    int err_lbl;       /* where an unchecked call error goes: the
                        * epilogue, or a catch handler while its
                        * operand is being evaluated */
    long scratch_base, scratch_used;  /* struct-literal scratch in the frame */
    /* RIP-relative fixups against data symbols ([name] operands),
     * patched once the ELF layout fixes the segment addresses. */
    struct { uint32_t pos, data_off; } *dfx;
    int ndfx, capdfx;
    /* Per-slot frame layout of the current function: `lsz[k]` is
     * the byte size of slot k and `loff[k]` its negative offset from
     * rbp (filled by nsel_func before the body is emitted). */
    int *lsz;
    int *loff;
    int lcap;
    /* Global storage: source name to its offset inside the writable
     * data image, plus the declaration node (const initializers). */
    struct { const char *name; uint32_t off; const HNode *node; } *gl;
    int ngl, capgl;
    /* Struct declarations by name, for field layout. */
    const HNode **st;
    int nst, capst;
    /* Start-up label, defined before user code. */
    int rt_start, lbl_main;
    /* User function table: source name to text label. */
    struct { const char *name; int lbl; const HNode *node; int live; } *fn;
    int nfn, capfn;
    /* Inline-asm calls naming a not-yet-emitted function: patched
     * with the entry address once every function exists. */
    struct { uint32_t pos; const char *name; } *cfx;
    int ncfx, capcfx;
    /* Unlabeled break/continue targets of the enclosing loops. */
    int brk[16];
    int cont[16];
    int depth;
    /* Named loop labels (label: while/for ...). An entry is created
     * when the HK_LABEL statement is entered and filled with the
     * target labels by the loop statement it wraps; the sentinel -1
     * marks an entry whose loop has not started yet. */
    struct { const char *name; int brk; int cont; } named[16];
    int nnamed;
} NSel;

enum { NSECT_RO = 0, NSECT_DATA = 1 };

/* Native cell stride of a value type: the native ABI stores every
 * array element as a full-width word (8 bytes) except single-byte
 * cells (Char/Bool), which keep their natural size. Pointer scaling
 * and array indexing must agree on this stride. */
static inline long ncell_bytes(const HType *t) {
    if (!t) return 8;
    if (t->kind == HTY_CHAR || t->kind == HTY_BOOL) return 1;
    if ((t->kind == HTY_INT || t->kind == HTY_UINT) &&
        t->width == 8) return 1;
    if (t->kind == HTY_REAL && t->width == 32) return 4;
    if (t->kind == HTY_INT || t->kind == HTY_UINT ||
        t->kind == HTY_REAL || t->kind == HTY_PTR ||
        t->kind == HTY_TUPLE) return 8;
    if (t->kind == HTY_ARRAY) return ncell_bytes(t->u.array.elem);
    return 8;
}

void nerr(NSel *s, const HNode *n, const char *what);

/* Emits the freestanding start-up sequence (the ELF entry point) at
 * the beginning of the text buffer (see nrt.c). */
void nrt_emit(NSel *s);

/* Assembles one inline-asm statement into the current code buffer
 * (see the nasm implementation in nsel.c). */
void nasm_stmt(NSel *s, const HNode *n);

/* Binds pending inline-asm call fixups to function entries; call
 * after every function is emitted, before nx_resolve (see nsel.c). */
void nresolve_call_fixups(NSel *s);

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

/* Allocates scratch for an uninitialized named (struct/union)
 * object; the base address is left in rax (see nsel_expr.c). */
void nalloc_named(NSel *s, const HType *t);

/* Converts the value in rax to the store target's numeric
 * interpretation when needed (see nsel_expr.c). */
void nstore_convert(NSel *s, const HType *tgt, const HType *val);
void nstore_convert_mem(NSel *s, const HType *tgt, const HType *val);

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
