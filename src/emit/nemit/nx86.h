#ifndef PAXSY_NX86_H
#define PAXSY_NX86_H

/*
 * nx86.h - the x86-64 machine-code encoder used by the native
 * emission pass. Instructions are appended to a growable byte
 * buffer; branch and call targets reference symbolic labels that
 * are resolved to rel32 displacements once every label position
 * is known. Absolute 64-bit constants (addresses of string data)
 * are filled by the caller after the final layout is computed, so
 * the encoder only tracks code-relative fixups here.
 */

#include "common.h"

#include <stdint.h>

/* Integer register ids in encoding order (low 3 bits are the
 * register code, bit 3 marks the REX.B extension). */
typedef enum {
    NRAX, NRCX, NRDX, NRBX, NRSP, NRBP, NRSI, NRDI,
    NR8, NR9, NR10, NR11, NR12, NR13, NR14, NR15
} NXReg;

typedef struct {
    uint8_t *buf;   /* instruction bytes                    */
    size_t len;     /* bytes used                           */
    size_t cap;     /* bytes allocated                      */
    uint32_t *lbl;  /* offset of label i, NX_UNDEF unplaced */
    int nlbl;
    int caplbl;
    struct { uint32_t pos; int lbl; } *fx; /* rel32 fixups  */
    int nfx;
    int capfx;
} NXBuf;

#define NX_UNDEF 0xffffffffu

void nx_init(NXBuf *b);
void nx_free(NXBuf *b);

/* Current offset, used to record label and fixup positions. */
uint32_t nx_here(const NXBuf *b);

/* Creates a fresh unresolved label. */
int nx_label(NXBuf *b);

/* Creates a fresh unresolved label without binding it (used by the
 * inline assembler for forward references created on first
 * mention). */
int nx_label_raw(void);

/* Binds a label to the current position. */
void nx_def(NXBuf *b, int l);

/* Records a rel32 fixup for the 4 bytes ending at the buffer end. */
void nx_fixup(NXBuf *b, int l);

/* Patches every recorded fixup against the final label positions.
 * Returns the number of still-unresolved targets (must be 0). */
int nx_resolve(NXBuf *b);

/* Returns the code offset a label was defined at, or NX_UNDEF when
 * the label has not been defined. After a successful nx_resolve()
 * every referenced label carries its final offset. */
uint32_t nx_label_off(const NXBuf *b, int l);

/* Raw byte emitters. */
void nx_b(NXBuf *b, uint8_t v);
void nx_imm32(NXBuf *b, int32_t v);
void nx_imm64(NXBuf *b, uint64_t v);

/* Instruction forms (64-bit operand size throughout). */
void nx_mov_ri(NXBuf *b, NXReg d, uint64_t imm);      /* mov rd, imm64  */
void nx_mov_rr(NXBuf *b, NXReg d, NXReg s);          /* mov rd, rs     */
void nx_mov_rm(NXBuf *b, NXReg d, NXReg base, int32_t disp); /* load  */
void nx_mov_mr(NXBuf *b, NXReg base, int32_t disp, NXReg s);  /* store */
void nx_lea(NXBuf *b, NXReg d, NXReg base, int32_t disp);
void nx_push(NXBuf *b, NXReg r);
void nx_pop(NXBuf *b, NXReg r);

/* ALU register-register operations. */
void nx_add_rr(NXBuf *b, NXReg d, NXReg s);
void nx_sub_rr(NXBuf *b, NXReg d, NXReg s);
void nx_and_rr(NXBuf *b, NXReg d, NXReg s);
void nx_or_rr(NXBuf *b, NXReg d, NXReg s);
void nx_xor_rr(NXBuf *b, NXReg d, NXReg s);
void nx_cmp_rr(NXBuf *b, NXReg d, NXReg s);
void nx_test_rr(NXBuf *b, NXReg d, NXReg s);
void nx_imul_rr(NXBuf *b, NXReg d, NXReg s);
void nx_inc_r(NXBuf *b, NXReg r);
void nx_dec_r(NXBuf *b, NXReg r);
void nx_neg_r(NXBuf *b, NXReg r);
void nx_not_r(NXBuf *b, NXReg r);
void nx_cqo(NXBuf *b);
void nx_idiv_r(NXBuf *b, NXReg s);

/* Shifts use the count register CL implicitly. */
void nx_shl_cl(NXBuf *b, NXReg d);
void nx_shr_cl(NXBuf *b, NXReg d);
void nx_sar_cl(NXBuf *b, NXReg r);

/* Comparisons set the zero-extended 0/1 result in a register. */
void nx_setcc(NXBuf *b, NXReg d, int cc);

/* Condition codes in x86 encoding order. */
enum { NCC_O, NCC_NO, NCC_B, NCC_AE, NCC_E, NCC_NE, NCC_BE, NCC_A,
       NCC_S, NCC_NS, NCC_P, NCC_NP, NCC_L, NCC_GE, NCC_LE, NCC_G };

/* Control transfer. */
void nx_call(NXBuf *b, int l);
void nx_jmp(NXBuf *b, int l);
void nx_jcc(NXBuf *b, int cc, int l);
void nx_ret(NXBuf *b);
void nx_syscall(NXBuf *b);

/* Byte-sized forms for the runtime (the low byte of the named
 * register is the operand). */
void nx_mov8_r0(NXBuf *b, NXReg d, NXReg addr);   /* load byte  */
void nx_mov8_m0(NXBuf *b, NXReg addr, NXReg src);  /* store byte */
void nx_mov8_mi0(NXBuf *b, NXReg addr, uint8_t v); /* store imm  */
void nx_add8_ri(NXBuf *b, NXReg r, uint8_t imm);   /* add imm8   */
void nx_test8_rr(NXBuf *b, NXReg d, NXReg s);      /* test bytes */

/* Width-aware memory forms for array elements. */
void nx_mov32_rm(NXBuf *b, NXReg d, NXReg base, int32_t disp);
void nx_mov32_mr(NXBuf *b, NXReg base, int32_t disp, NXReg s);
void nx_movsxd_rm(NXBuf *b, NXReg d, NXReg base, int32_t disp);

#endif /* PAXSY_NX86_H */
