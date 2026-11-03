#include "common.h"
#include "nx86.h"

/*
 * nx86.c - x86-64 instruction encoding. Every helper appends the
 * exact byte sequence for one instruction form to an NXBuf; 64-bit
 * operand size is fixed. Memory operands use RBP-relative
 * addressing with an 8-bit displacement when it fits and a 32-bit
 * displacement otherwise, so stack frames of any size need no extra
 * encoding path. Control transfers take symbolic labels resolved
 * by nx_resolve() after all code is emitted.
 */

static void nx_grow(NXBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + extra) cap *= 2;
    b->buf = realloc(b->buf, cap);
    b->cap = cap;
}

void nx_init(NXBuf *b) { memset(b, 0, sizeof *b); }

void nx_free(NXBuf *b) {
    free(b->buf);
    free(b->lbl);
    free(b->fx);
    nx_init(b);
}

uint32_t nx_here(const NXBuf *b) { return (uint32_t)b->len; }

int nx_label(NXBuf *b) {
    if (b->nlbl == b->caplbl) {
        b->caplbl = b->caplbl ? b->caplbl * 2 : 16;
        b->lbl = realloc(b->lbl, (size_t)b->caplbl * sizeof(uint32_t));
    }
    b->lbl[b->nlbl] = NX_UNDEF;
    return b->nlbl++;
}

int nx_label_raw(void) {
    return nx_label((NXBuf *)NULL);   /* see note below */
}

void nx_def(NXBuf *b, int l) { b->lbl[l] = nx_here(b); }

void nx_fixup(NXBuf *b, int l) {
    if (b->nfx == b->capfx) {
        b->capfx = b->capfx ? b->capfx * 2 : 32;
        b->fx = realloc(b->fx, (size_t)b->capfx * sizeof(b->fx[0]));
    }
    b->fx[b->nfx].pos = nx_here(b) - 4;
    b->fx[b->nfx].lbl = l;
    b->nfx++;
}

int nx_resolve(NXBuf *b) {
    int bad = 0;
    for (int i = 0; i < b->nfx; i++) {
        uint32_t t = b->lbl[b->fx[i].lbl];
        if (t == NX_UNDEF) { bad++; continue; }
        int32_t rel = (int32_t)t - (int32_t)(b->fx[i].pos + 4);
        memcpy(b->buf + b->fx[i].pos, &rel, 4);
    }
    return bad;
}

uint32_t nx_label_off(const NXBuf *b, int l) {
    return b->lbl[l];
}

void nx_b(NXBuf *b, uint8_t v) { nx_grow(b, 1); b->buf[b->len++] = v; }

void nx_imm32(NXBuf *b, int32_t v) {
    nx_grow(b, 4);
    memcpy(b->buf + b->len, &v, 4);
    b->len += 4;
}

void nx_imm64(NXBuf *b, uint64_t v) {
    nx_grow(b, 8);
    memcpy(b->buf + b->len, &v, 8);
    b->len += 8;
}

/* Emits a REX prefix when any of its fields is set. */
static void rex(NXBuf *b, int w, int r, int x, int b3) {
    if (w || r || x || b3)
        nx_b(b, (uint8_t)(0x40 | (w ? 8 : 0) | (r ? 4 : 0) |
                          (x ? 2 : 0) | (b3 ? 1 : 0)));
}

/* Emits ModRM plus the displacement for an RBP-relative operand. */
static void modrm_mem(NXBuf *b, int reg, NXReg base, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        nx_b(b, (uint8_t)(0x40 | ((reg & 7) << 3) | (base & 7)));
        nx_b(b, (uint8_t)disp);
    } else {
        nx_b(b, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
        nx_imm32(b, disp);
    }
}

/* Emits ModRM for a register-direct operand (mod 3). */
static void modrm_reg(NXBuf *b, int reg, NXReg rm) {
    nx_b(b, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

/* Emits ModRM for a plain [reg] memory operand (mod 0, no SIB). */
static void modrm_mem0(NXBuf *b, int reg, NXReg base) {
    nx_b(b, (uint8_t)(((reg & 7) << 3) | (base & 7)));
}

void nx_mov_ri(NXBuf *b, NXReg d, uint64_t imm) {
    rex(b, 1, 0, 0, d > 7);
    nx_b(b, (uint8_t)(0xB8 + (d & 7)));
    nx_imm64(b, imm);
}

void nx_mov_rr(NXBuf *b, NXReg d, NXReg s) {
    rex(b, 1, s > 7, 0, d > 7);
    nx_b(b, 0x89);
    modrm_reg(b, s, d);
}

void nx_mov_rm(NXBuf *b, NXReg d, NXReg base, int32_t disp) {
    rex(b, 1, d > 7, 0, base > 7);
    nx_b(b, 0x8B);
    modrm_mem(b, d, base, disp);
}

void nx_mov_mr(NXBuf *b, NXReg base, int32_t disp, NXReg s) {
    rex(b, 1, s > 7, 0, base > 7);
    nx_b(b, 0x89);
    modrm_mem(b, s, base, disp);
}

void nx_lea(NXBuf *b, NXReg d, NXReg base, int32_t disp) {
    rex(b, 1, d > 7, 0, base > 7);
    nx_b(b, 0x8D);
    modrm_mem(b, d, base, disp);
}

void nx_push(NXBuf *b, NXReg r) {
    rex(b, 0, 0, 0, r > 7);
    nx_b(b, (uint8_t)(0x50 + (r & 7)));
}

void nx_pop(NXBuf *b, NXReg r) {
    rex(b, 0, 0, 0, r > 7);
    nx_b(b, (uint8_t)(0x58 + (r & 7)));
}

/* One ALU r/m64, r64 instruction (opcode in `op`). */
static void alu_rr(NXBuf *b, uint8_t op, NXReg d, NXReg s) {
    rex(b, 1, s > 7, 0, d > 7);
    nx_b(b, op);
    modrm_reg(b, s, d);
}

void nx_add_rr(NXBuf *b, NXReg d, NXReg s) { alu_rr(b, 0x01, d, s); }
void nx_sub_rr(NXBuf *b, NXReg d, NXReg s) { alu_rr(b, 0x29, d, s); }
void nx_and_rr(NXBuf *b, NXReg d, NXReg s) { alu_rr(b, 0x21, d, s); }
void nx_or_rr(NXBuf *b, NXReg d, NXReg s)  { alu_rr(b, 0x09, d, s); }
void nx_xor_rr(NXBuf *b, NXReg d, NXReg s) { alu_rr(b, 0x31, d, s); }
void nx_cmp_rr(NXBuf *b, NXReg d, NXReg s) { alu_rr(b, 0x39, d, s); }
void nx_test_rr(NXBuf *b, NXReg d, NXReg s){ alu_rr(b, 0x85, d, s); }

void nx_imul_rr(NXBuf *b, NXReg d, NXReg s) {
    rex(b, 1, d > 7, 0, s > 7);
    nx_b(b, 0x0F);
    nx_b(b, 0xAF);
    modrm_reg(b, d, s);
}

static void unary(NXBuf *b, uint8_t op, int sub, NXReg r) {
    rex(b, 1, 0, 0, r > 7);
    nx_b(b, op);
    modrm_reg(b, sub, r);
}

void nx_inc_r(NXBuf *b, NXReg r)  { unary(b, 0xFF, 0, r); }
void nx_dec_r(NXBuf *b, NXReg r)  { unary(b, 0xFF, 1, r); }
void nx_neg_r(NXBuf *b, NXReg r)  { unary(b, 0xF7, 3, r); }
void nx_not_r(NXBuf *b, NXReg r)  { unary(b, 0xF7, 2, r); }

void nx_cqo(NXBuf *b) { rex(b, 1, 0, 0, 0); nx_b(b, 0x99); }

void nx_idiv_r(NXBuf *b, NXReg s) { unary(b, 0xF7, 7, s); }

void nx_shl_cl(NXBuf *b, NXReg d) { unary(b, 0xD3, 4, d); }
void nx_shr_cl(NXBuf *b, NXReg d) { unary(b, 0xD3, 5, d); }
void nx_sar_cl(NXBuf *b, NXReg r)  { unary(b, 0xD3, 7, r); }

void nx_setcc(NXBuf *b, NXReg d, int cc) {
    rex(b, 0, 0, 0, d > 7);
    nx_b(b, 0x0F);
    nx_b(b, (uint8_t)(0x90 + cc));
    modrm_reg(b, 0, d);
}

void nx_call(NXBuf *b, int l) { nx_b(b, 0xE8); nx_imm32(b, 0); nx_fixup(b, l); }

void nx_jmp(NXBuf *b, int l)  { nx_b(b, 0xE9); nx_imm32(b, 0); nx_fixup(b, l); }

void nx_jcc(NXBuf *b, int cc, int l) {
    nx_b(b, 0x0F);
    nx_b(b, (uint8_t)(0x80 + cc));
    nx_imm32(b, 0);
    nx_fixup(b, l);
}

void nx_ret(NXBuf *b)     { nx_b(b, 0xC3); }
void nx_syscall(NXBuf *b){ nx_b(b, 0x0F); nx_b(b, 0x05); }

/* Loads the byte at [addr] into the low byte of `d` (8A /r). */
void nx_mov8_r0(NXBuf *b, NXReg d, NXReg addr) {
    rex(b, 0, d > 7, 0, addr > 7);
    nx_b(b, 0x8A);
    modrm_mem0(b, d, addr);
}

/* Stores the low byte of `src` at [addr] (88 /r). */
void nx_mov8_m0(NXBuf *b, NXReg addr, NXReg src) {
    rex(b, 0, src > 7, 0, addr > 7);
    nx_b(b, 0x88);
    modrm_mem0(b, src, addr);
}

/* Stores the immediate byte `v` at [addr] (C6 /0). */
void nx_mov8_mi0(NXBuf *b, NXReg addr, uint8_t v) {
    rex(b, 0, 0, 0, addr > 7);
    nx_b(b, 0xC6);
    modrm_mem0(b, 0, addr);
    nx_b(b, v);
}

/* Adds the immediate byte to the low byte of `r` (80 /0). */
void nx_add8_ri(NXBuf *b, NXReg r, uint8_t imm) {
    rex(b, 0, 0, 0, r > 7);
    nx_b(b, 0x80);
    modrm_reg(b, 0, r);
    nx_b(b, imm);
}

/* Tests the low bytes of two registers (84 /r). */
void nx_test8_rr(NXBuf *b, NXReg d, NXReg s) {
    rex(b, 0, s > 7, 0, d > 7);
    nx_b(b, 0x84);
    modrm_reg(b, s, d);
}

/* Loads a 32-bit word, zero-extending into the 64-bit register. */
void nx_mov32_rm(NXBuf *b, NXReg d, NXReg base, int32_t disp) {
    rex(b, 0, d > 7, 0, base > 7);
    nx_b(b, 0x8B);
    modrm_mem(b, d, base, disp);
}

/* Stores the low 32 bits of a register. */
void nx_mov32_mr(NXBuf *b, NXReg base, int32_t disp, NXReg s) {
    rex(b, 0, s > 7, 0, base > 7);
    nx_b(b, 0x89);
    modrm_mem(b, s, base, disp);
}

/* Loads a 32-bit word with sign extension into the 64-bit register
 * (the form used for signed Int32 array elements). */
void nx_movsxd_rm(NXBuf *b, NXReg d, NXReg base, int32_t disp) {
    rex(b, 1, d > 7, 0, base > 7);
    nx_b(b, 0x63);
    modrm_mem(b, d, base, disp);
}
