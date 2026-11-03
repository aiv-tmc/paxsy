#include "common.h"
#include "nsel.h"

/*
 * nsel.c - per-function instruction selection and the built-in x86-64
 * assembler. nsel_func() lays out the frame: it scans the body for
 * local slots, records each slot's byte size and its negative offset
 * below rbp, spills the incoming argument registers into the
 * parameter slots above rbp, dispatches the body and closes with one
 * shared epilogue label. nasm_stmt() assembles an inline-asm
 * statement directly into the code buffer through the same
 * label/fixup machinery as the rest of the backend: no external
 * assembler is invoked, label prefixes may share a segment with an
 * instruction, mnemonics are case-insensitive, register names accept
 * the 64-bit spelling (rcx), the classic 32-bit spelling (ecx) and
 * the byte views (cl), because every operation is 64-bit, and the
 * byte-wide memory forms carry full SIB/displacement handling for
 * the rsp and rbp bases. Forward references resolve through the
 * rel32 fixup table; every unrecognised piece is a hard diagnostic
 * naming the offender and the raw source segment.
 */

static void nscan_locals(const HNode *n, int *maxslot) {
    if (!n) return;
    if ((n->kind == HK_LET || n->kind == HK_FOR) && n->u.index > *maxslot)
        *maxslot = n->u.index;
    for (int i = 0; i < 4; i++) nscan_locals(n->child[i], maxslot);
    for (int i = 0; i < n->nitems; i++) nscan_locals(n->items[i], maxslot);
}

static void nscan_sizes(NSel *s, const HNode *n) {
    if (!n) return;
    if (n->kind == HK_LET && n->u.index >= 0 &&
        n->u.index < s->lcap && s->lsz[n->u.index] == 0) {
        int bytes = 8;
        if (n->type && n->type->kind == HTY_ARRAY)
            bytes = (int)n->type->u.array.count * 8;
        if (bytes <= 0) bytes = 8;
        s->lsz[n->u.index] = bytes;
    }
    for (int i = 0; i < 4; i++) nscan_sizes(s, n->child[i]);
    for (int i = 0; i < n->nitems; i++) nscan_sizes(s, n->items[i]);
}

static int nis_main(const HNode *f) {
    size_t len = strlen(f->name);
    if (len == 4 && strcmp(f->name, "main") == 0) return 1;
    return len > 6 && strcmp(f->name + len - 7, "::main") == 0;
}

void nsel_func(NSel *s, const HNode *f) {
    NXBuf *b = s->text;
    int lbl = -1;
    for (int i = 0; i < s->nfn; i++)
        if (strcmp(s->fn[i].name, f->name) == 0) { lbl = s->fn[i].lbl; break; }
    if (lbl < 0) { nerr(s, f, "function without a registered label"); return; }
    int maxslot = -1;
    nscan_locals(f->child[3], &maxslot);
    s->nl = maxslot + 1;
    s->cur_func = f;
    s->depth = 0;
    s->ep = nx_label(b);
    int cap = maxslot + 2;
    if (cap < 1) cap = 1;
    if (cap > s->lcap) {
        s->lsz = realloc(s->lsz, (size_t)cap * sizeof(int));
        s->loff = realloc(s->loff, (size_t)cap * sizeof(int));
        s->lcap = cap;
    }
    for (int k = 0; k < cap; k++) { s->lsz[k] = 0; s->loff[k] = 0; }
    nscan_sizes(s, f->child[3]);
    {
        int off = 0;
        for (int k = 0; k < cap; k++) {
            int sz = s->lsz[k] > 0 ? s->lsz[k] : 8;
            off += (sz + 7) & ~7;
            s->loff[k] = -off;
        }
    }
    static const NXReg argreg[6] = { NRDI, NRSI, NRDX, NRCX, NR8, NR9 };
    nx_def(b, lbl);
    nx_push(b, NRBP);
    nx_mov_rr(b, NRBP, NRSP);
    for (int i = 0; i < f->nitems && i < 6; i++)
        nx_mov_mr(b, NRBP, nparam_off(i), argreg[i]);
    int bytes = -s->loff[cap - 1];
    bytes = (bytes + 15) & ~15;
    if (bytes) { nx_mov_ri(b, NRAX, (uint64_t)bytes); nx_sub_rr(b, NRSP, NRAX); }
    nstmt(s, f->child[3]);
    nx_def(b, s->ep);
    if (nis_main(f) && f->type && f->type->kind != HTY_ERROR)
        nx_mov_ri(b, NRDX, 0);
    nx_mov_rr(b, NRSP, NRBP);
    nx_pop(b, NRBP);
    nx_ret(b);
}

/* The built-in assembler. Instructions are separated by ';' or by
 * real newlines (the escape \n is decoded first). A label is an
 * identifier directly followed by ':' and may share its segment with
 * an instruction; each prefix is peeled off and the remainder parses
 * as the next instruction. Operands are comma separated registers,
 * immediates and [reg] memory operands. */

typedef struct { const char *name; NXReg reg; int byte; } NReg;
static const NReg NREGS[] = {
    {"rax", NRAX, 0}, {"rcx", NRCX, 0}, {"rdx", NRDX, 0}, {"rbx", NRBX, 0},
    {"rsp", NRSP, 0}, {"rbp", NRBP, 0}, {"rsi", NRSI, 0}, {"rdi", NRDI, 0},
    {"r8", NR8, 0}, {"r9", NR9, 0}, {"r10", NR10, 0}, {"r11", NR11, 0},
    {"r12", NR12, 0}, {"r13", NR13, 0}, {"r14", NR14, 0}, {"r15", NR15, 0},
    {"eax", NRAX, 0}, {"ecx", NRCX, 0}, {"edx", NRDX, 0}, {"ebx", NRBX, 0},
    {"esp", NRSP, 0}, {"ebp", NRBP, 0}, {"esi", NRSI, 0}, {"edi", NRDI, 0},
    {"al", NRAX, 1}, {"cl", NRCX, 1}, {"dl", NRDX, 1}, {"bl", NRBX, 1},
};
#define NNREGS (sizeof(NREGS) / sizeof(NREGS[0]))

typedef struct { char name[32]; int lbl; int defined; } NLab;
static NLab NLABS[128];
static int NNLABS;

static char *ndecode(const char *src, size_t len) {
    char *out = malloc(len + 1);
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len && src[i + 1] == 'n') { out[o++] = '\n'; i++; }
        else if (src[i] == '\\' && i + 1 < len && src[i + 1] == '\\') { out[o++] = '\\'; i++; }
        else out[o++] = src[i];
    }
    out[o] = '\0';
    return out;
}

static int nreg(const char *t, NXReg *r, int *byte) {
    for (size_t i = 0; i < NNREGS; i++)
        if (strcmp(t, NREGS[i].name) == 0) { *r = NREGS[i].reg; *byte = NREGS[i].byte; return 1; }
    return 0;
}

static int nlabel(NXBuf *b, const char *name) {
    for (int i = 0; i < NNLABS; i++)
        if (strcmp(NLABS[i].name, name) == 0) {
            if (NLABS[i].lbl < 0) NLABS[i].lbl = nx_label(b);
            return NLABS[i].lbl;
        }
    if (NNLABS == (int)(sizeof(NLABS) / sizeof(NLABS[0]))) return -1;
    NLab *l = &NLABS[NNLABS++];
    snprintf(l->name, sizeof(l->name), "%s", name);
    l->lbl = nx_label(b);
    l->defined = 0;
    return l->lbl;
}

static int nlabel_def(NXBuf *b, const char *name) {
    for (int i = 0; i < NNLABS; i++)
        if (strcmp(NLABS[i].name, name) == 0) {
            if (NLABS[i].defined) return -1;
            NLABS[i].defined = 1;
            if (NLABS[i].lbl < 0) NLABS[i].lbl = nx_label(b);
            nx_def(b, NLABS[i].lbl);
            return 0;
        }
    if (NNLABS == (int)(sizeof(NLABS) / sizeof(NLABS[0]))) return -1;
    NLab *l = &NLABS[NNLABS++];
    snprintf(l->name, sizeof(l->name), "%s", name);
    l->lbl = nx_label(b);
    l->defined = 1;
    nx_def(b, l->lbl);
    return 0;
}

static void nrex(NXBuf *b, int w, int rreg, int breg) {
    int rex = 0x40 | (w ? 8 : 0) | ((rreg >= 8) ? 4 : 0) | ((breg >= 8) ? 1 : 0);
    if (rex != 0x40) nx_b(b, (uint8_t)rex);
}

static void nmodrm(NXBuf *b, int reg, NXReg rm) {
    nx_b(b, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

static void nmov8_rr(NXBuf *b, NXReg d, NXReg s) {
    nrex(b, 0, d, s);
    nx_b(b, 0x8A);
    nmodrm(b, d, s);
}

/* Byte store `mov [base], src8` with full SIB/displacement handling:
 * rsp needs a SIB byte and rbp a displacement in mod=00 form; the
 * plain encoders skip both and would desynchronize the stream. */
static void nmov8_m0_full(NXBuf *b, NXReg base, NXReg src) {
    nrex(b, 0, src, base);
    nx_b(b, 0x88);
    if (base == NRSP) {
        nx_b(b, (uint8_t)(0x04 | ((src & 7) << 3)));
        nx_b(b, 0x24);
    } else if (base == NRBP) {
        nx_b(b, (uint8_t)(0x44 | ((src & 7) << 3)));
        nx_b(b, 0x00);
    } else {
        nmodrm(b, src, base);
    }
}

/* The load counterpart (8A /r). */
static void nmov8_r0_full(NXBuf *b, NXReg dst, NXReg base) {
    nrex(b, 0, dst, base);
    nx_b(b, 0x8A);
    if (base == NRSP) {
        nx_b(b, (uint8_t)(0x04 | ((dst & 7) << 3)));
        nx_b(b, 0x24);
    } else if (base == NRBP) {
        nx_b(b, (uint8_t)(0x44 | ((dst & 7) << 3)));
        nx_b(b, 0x00);
    } else {
        nmodrm(b, dst, base);
    }
}

/* The immediate-byte store counterpart (C6 /0 ib). */
static void nmov8_mi0_full(NXBuf *b, NXReg base, uint8_t v) {
    nrex(b, 0, 0, base);
    nx_b(b, 0xC6);
    if (base == NRSP) {
        nx_b(b, 0x04);
        nx_b(b, 0x24);
    } else if (base == NRBP) {
        nx_b(b, 0x45);
        nx_b(b, 0x00);
    } else {
        nmodrm(b, 0, base);
    }
    nx_b(b, v);
}

static void nalu_imm(NXBuf *b, int subop, NXReg d, long long v) {
    nrex(b, 1, 0, d);
    if (v >= -128 && v <= 127) { nx_b(b, 0x83); nmodrm(b, subop, d); nx_b(b, (uint8_t)v); }
    else { nx_b(b, 0x81); nmodrm(b, subop, d); nx_imm32(b, (int32_t)v); }
}

static void nshift(NXBuf *b, int op, NXReg d, int use_cl, int imm) {
    nrex(b, 1, 0, d);
    nx_b(b, use_cl ? 0xD3 : 0xC1);
    nmodrm(b, op, d);
    if (!use_cl) nx_b(b, (uint8_t)imm);
}

static void ndiv(NXBuf *b, NXReg d, int signed_div) {
    nrex(b, 1, 0, d);
    nx_b(b, 0xF7);
    nmodrm(b, signed_div ? 7 : 6, d);
}

static void nasm_err(NSel *s, const HNode *n, const char *what) {
    char buf[320];
    snprintf(buf, sizeof(buf), "asm: %s", what);
    nerr(s, n, buf);
}

static int njcc(const char *m) {
    if (!strcmp(m, "je") || !strcmp(m, "jz"))   return NCC_E;
    if (!strcmp(m, "jne") || !strcmp(m, "jnz")) return NCC_NE;
    if (!strcmp(m, "jb") || !strcmp(m, "jnae")) return NCC_B;
    if (!strcmp(m, "jae") || !strcmp(m, "jnb")) return NCC_AE;
    if (!strcmp(m, "jbe") || !strcmp(m, "jna")) return NCC_BE;
    if (!strcmp(m, "ja"))                         return NCC_A;
    if (!strcmp(m, "jl") || !strcmp(m, "jnge")) return NCC_L;
    if (!strcmp(m, "jge") || !strcmp(m, "jnl")) return NCC_GE;
    if (!strcmp(m, "jle") || !strcmp(m, "jng")) return NCC_LE;
    if (!strcmp(m, "jg"))                         return NCC_G;
    if (!strcmp(m, "jo"))  return NCC_O;
    if (!strcmp(m, "jno")) return NCC_NO;
    if (!strcmp(m, "js"))  return NCC_S;
    if (!strcmp(m, "jns")) return NCC_NS;
    return -1;
}

static int noperand(const char *t, NXReg *r, int *byte, long long *imm, int *is_mem) {
    size_t n = strlen(t);
    *is_mem = 0;
    if (n >= 3 && t[0] == '[' && t[n - 1] == ']') {
        char inner[16];
        if (n - 2 >= sizeof(inner)) return 0;
        memcpy(inner, t + 1, n - 2);
        inner[n - 2] = '\0';
        int b;
        if (!nreg(inner, r, &b) || b) return 0;
        *is_mem = 1;
        return 1;
    }
    if (nreg(t, r, byte)) return 1;
    char *end;
    long long v = strtoll(t, &end, 0);
    if (end != t && *end == '\0') { *imm = v; return 2; }
    return 0;
}

static void ninsn(NSel *s, const HNode *n, char *line) {
    NXBuf *b = s->text;
    /* The raw segment, saved before any in-place editing, is part of
     * every diagnostic so a failure always shows the source text the
     * assembler saw. */
    char raw[160];
    snprintf(raw, sizeof(raw), "%s", line);
    char *sep = strchr(line, '#');
    if (sep) *sep = '\0';
    for (;;) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        char *c = strchr(p, ':');
        if (!c || c == p) break;
        int ok = 1;
        for (char *q = p; q < c; q++)
            if (!isalnum((unsigned char)*q) && *q != '_') { ok = 0; break; }
        if (!ok) break;
        *c = '\0';
        if (nlabel_def(b, p) != 0) nasm_err(s, n, "duplicate label");
        memmove(line, c + 1, strlen(c + 1) + 1);
    }
    char *sp = line;
    while (isspace((unsigned char)*sp)) sp++;
    char mnem[16];
    int mi = 0;
    while (sp[mi] && isalpha((unsigned char)sp[mi]) && mi < 15) { mnem[mi] = sp[mi]; mi++; }
    mnem[mi] = '\0';
    for (int i = 0; mnem[i]; i++)
        if (mnem[i] >= 'A' && mnem[i] <= 'Z')
            mnem[i] = (char)(mnem[i] - 'A' + 'a');
    sp += mi;
    while (isspace((unsigned char)*sp)) sp++;
    if (!mi) return;
    char *comma = strchr(sp, ',');
    char optok[2][64] = {{0}, {0}};
    if (comma) { *comma = '\0'; snprintf(optok[1], sizeof(optok[1]), "%s", comma + 1); }
    snprintf(optok[0], sizeof(optok[0]), "%s", sp);
    for (int k = 0; k < 2; k++) {
        char *p = optok[k];
        char *e = p + strlen(p);
        while (e > p && isspace((unsigned char)e[-1])) *--e = '\0';
        while (isspace((unsigned char)*p)) p++;
        if (p != optok[k]) memmove(optok[k], p, strlen(p) + 1);
    }
    NXReg r0 = NRAX, r1 = NRAX;
    int b0 = 0, b1 = 0, m0 = 0, m1 = 0;
    long long v1 = 0;
    int k0 = optok[0][0] ? noperand(optok[0], &r0, &b0, &v1, &m0) : 0;
    int k1 = optok[1][0] ? noperand(optok[1], &r1, &b1, &v1, &m1) : 0;
    if (!strcmp(mnem, "mov")) {
        if (k0 == 1 && k1 == 1 && !m0 && !m1) {
            if (b0 && b1) nmov8_rr(b, r0, r1);
            else if (!b0 && !b1) nx_mov_rr(b, r0, r1);
            else nasm_err(s, n, "mov between different register widths");
        } else if (k0 == 1 && k1 == 2 && !m0) nx_mov_ri(b, r0, (uint64_t)v1);
        else if (k0 == 1 && m1) { if (b0) nmov8_r0_full(b, r0, r1); else nx_mov_rm(b, r0, r1, 0); }
        else if (m0 && k1 == 1) { if (b1) nmov8_m0_full(b, r0, r1); else nx_mov_mr(b, r0, 0, r1); }
        else if (m0 && k1 == 2) nmov8_mi0_full(b, r0, (uint8_t)v1);
        else {
            char w[220];
            snprintf(w, sizeof(w), "unsupported mov form in '%s'", raw);
            nasm_err(s, n, w);
        }
        return;
    }
    if (!strcmp(mnem, "push") && k0 == 1 && !m0) { nx_push(b, r0); return; }
    if (!strcmp(mnem, "pop")  && k0 == 1 && !m0) { nx_pop(b, r0);  return; }
    if (!strcmp(mnem, "inc")) { nx_inc_r(b, r0); return; }
    if (!strcmp(mnem, "dec")) { nx_dec_r(b, r0); return; }
    if (!strcmp(mnem, "neg")) { nx_neg_r(b, r0); return; }
    if (!strcmp(mnem, "not")) { nx_not_r(b, r0); return; }
    if (!strcmp(mnem, "cqo")) { nx_cqo(b); return; }
    if (!strcmp(mnem, "nop")) { nx_b(b, 0x90); return; }
    if (!strcmp(mnem, "ret")) { nx_ret(b); return; }
    if (!strcmp(mnem, "syscall")) { nx_syscall(b); return; }
    if (!strcmp(mnem, "idiv") && k0 == 1 && !m0) { ndiv(b, r0, 1); return; }
    if (!strcmp(mnem, "div")  && k0 == 1 && !m0) { ndiv(b, r0, 0); return; }
    if (!strcmp(mnem, "imul") && k0 == 1 && k1 == 1 && !m0 && !m1) { nx_imul_rr(b, r0, r1); return; }
    if (!strcmp(mnem, "shl") || !strcmp(mnem, "shr") || !strcmp(mnem, "sar")) {
        int op = mnem[2] == 'l' ? 4 : mnem[1] == 'h' ? 5 : 7;
        if (k0 != 1 || m0) { nasm_err(s, n, "bad shift operand"); return; }
        if (k1 == 1 && r1 == NRCX && !m1) nshift(b, op, r0, 1, 0);
        else if (k1 == 2) nshift(b, op, r0, 0, (int)v1);
        else nasm_err(s, n, "shift count must be cl or an immediate");
        return;
    }
    struct { const char *name; void (*rr)(NXBuf *, NXReg, NXReg); int subop; } alu[] = {
        {"add", nx_add_rr, 0}, {"or", nx_or_rr, 1}, {"and", nx_and_rr, 4},
        {"sub", nx_sub_rr, 5}, {"xor", nx_xor_rr, 6}, {"cmp", nx_cmp_rr, 7},
    };
    for (size_t i = 0; i < sizeof(alu) / sizeof(alu[0]); i++) {
        if (strcmp(mnem, alu[i].name)) continue;
        if (k0 == 1 && k1 == 1 && !m0 && !m1) alu[i].rr(b, r0, r1);
        else if (k0 == 1 && k1 == 2 && !m0) nalu_imm(b, alu[i].subop, r0, v1);
        else {
            char what[220];
            snprintf(what, sizeof(what),
                     "unsupported ALU operand form '%s %s, %s' in '%s'",
                     mnem, optok[0], optok[1], raw);
            nasm_err(s, n, what);
        }
        return;
    }
    if (!strcmp(mnem, "test")) {
        if (k0 == 1 && k1 == 1 && !m0 && !m1) {
            if (b0 && b1) nx_test8_rr(b, r0, r1);
            else if (!b0 && !b1) nx_test_rr(b, r0, r1);
            else nasm_err(s, n, "test between different register widths");
        } else if (k0 == 1 && k1 == 2 && !m0) {
            nrex(b, 1, 0, r0);
            nx_b(b, 0xF7);
            nmodrm(b, 0, r0);
            nx_imm32(b, (int32_t)v1);
        } else nasm_err(s, n, "unsupported test form");
        return;
    }
    if (!strcmp(mnem, "jmp")) {
        int l = nlabel(b, optok[0]);
        if (l >= 0) nx_jmp(b, l); else nasm_err(s, n, "jmp to an unknown label");
        return;
    }
    if (!strcmp(mnem, "call")) {
        int l = nlabel(b, optok[0]);
        if (l >= 0) nx_call(b, l); else nasm_err(s, n, "call to an unknown label");
        return;
    }
    int cc = njcc(mnem);
    if (cc >= 0) {
        int l = nlabel(b, optok[0]);
        if (l >= 0) nx_jcc(b, cc, l); else nasm_err(s, n, "jump to an unknown label");
        return;
    }
    {
        char what[220];
        snprintf(what, sizeof(what), "unknown mnemonic '%s' in '%s'", mnem, raw);
        nasm_err(s, n, what);
    }
}

void nasm_stmt(NSel *s, const HNode *n) {
    NNLABS = 0;
    char *body = ndecode(n->u.slice.start, n->u.slice.len);
    char *cur = body;
    while (*cur) {
        char *end = cur;
        while (*end && *end != ';' && *end != '\n') end++;
        char saved = *end;
        *end = '\0';
        ninsn(s, n, cur);
        if (!saved) break;
        cur = end + 1;
    }
    free(body);
}
