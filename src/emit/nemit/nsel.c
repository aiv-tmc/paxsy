#include "common.h"
#include "nsel.h"

char *sm_strdup(const char *s);  /* sema: owned string copy */

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
 * both the 64-bit spelling (rcx) and the classic 32-bit spelling
 * (ecx) since every operation is 64-bit, forward references resolve
 * through the rel32 fixup table, and every unrecognised piece is a
 * hard diagnostic that names the offender and shows the raw source
 * segment it came from.
 */

static void nscan_locals(const HNode *n, int *maxslot) {
    if (!n) return;
    /* Count surviving local references as well as declarations. Optimizer
     * rewrites may remove a LET while retaining a STORE/local lvalue that
     * still addresses the same slot; sizing only from declarations then
     * under-allocates the frame and makes nslot_off read past loff. */
    if ((n->kind == HK_LOCAL || n->kind == HK_LET || n->kind == HK_FOR ||
         (n->kind == HK_CATCH && n->name)) && n->u.index > *maxslot)
        *maxslot = n->u.index;
    for (int i = 0; i < 4; i++) nscan_locals(n->child[i], maxslot);
    for (int i = 0; i < n->nitems; i++) nscan_locals(n->items[i], maxslot);
}

static void nscan_sizes(NSel *s, const HNode *n) {
    if (!n) return;
    if ((n->kind == HK_LET || (n->kind == HK_CATCH && n->name)) &&
        n->u.index >= 0 &&
        n->u.index < s->lcap && s->lsz[n->u.index] == 0) {
        int bytes = 8;
        if (n->type && n->type->kind == HTY_ARRAY)
            bytes = (int)n->type->u.array.count * 8;
        else if (n->type && n->type->kind == HTY_ERROR)
            bytes = 16;   /* the px_res pair: value word, tag word */
        if (bytes <= 0) bytes = 8;
        s->lsz[n->u.index] = bytes;
    }
    for (int i = 0; i < 4; i++) nscan_sizes(s, n->child[i]);
    for (int i = 0; i < n->nitems; i++) nscan_sizes(s, n->items[i]);
}


static int nis_main(const HNode *f) {
    size_t len = strlen(f->name);
    if (len == 4 && strcmp(f->name, "main") == 0) return 1;
    return len > 6 && strcmp(f->name + len - 6, "::main") == 0;
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
    s->scratch_used = 0;
    s->ep = nx_label(b);
    /* calls inside a function that returns an error propagate through
     * the epilogue by default; catch binds a handler label around its
     * callee */
    s->err_lbl = s->ep;
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
    /* stack arguments (beyond the sixth) must be copied out of the
     * incoming stack-arg area first: the register saves below reuse
     * those very addresses for the register parameters */
    for (int i = 6; i < f->nitems; i++) {
        nx_mov_rm(b, NRAX, NRBP, 16 + 8 * (i - 6));
        nx_mov_mr(b, NRBP, nparam_off(i), NRAX);
    }
    for (int i = 0; i < f->nitems && i < 6; i++)
        nx_mov_mr(b, NRBP, nparam_off(i), argreg[i]);
    int bytes = -s->loff[cap - 1];
    bytes = (bytes + 15) & ~15;
    s->scratch_base = bytes;
    bytes += 4096;
    if (bytes) nx_sub_ri(b, NRSP, bytes);
    nstmt(s, f->child[3]);
    /* A successful error-returning function reports code 0; the
     * zeroing sits before the epilogue label so that 'error' returns
     * and propagated call errors keep their code in RDX. */
    if (f->type && f->type->kind == HTY_ERROR)
        nx_mov_ri(b, NRDX, 0);
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
 * as the next instruction. Operands are comma separated registers
 * (64-bit names, 32-bit aliases and byte views), immediates and
 * [reg] memory operands. */

typedef struct { const char *name; NXReg reg; int byte; } NReg;
static const NReg NREGS[] = {
    /* 64-bit names */
    {"rax", NRAX, 0}, {"rcx", NRCX, 0}, {"rdx", NRDX, 0}, {"rbx", NRBX, 0},
    {"rsp", NRSP, 0}, {"rbp", NRBP, 0}, {"rsi", NRSI, 0}, {"rdi", NRDI, 0},
    {"r8", NR8, 0}, {"r9", NR9, 0}, {"r10", NR10, 0}, {"r11", NR11, 0},
    {"r12", NR12, 0}, {"r13", NR13, 0}, {"r14", NR14, 0}, {"r15", NR15, 0},
    /* 32-bit aliases: every operation is 64-bit, so the classic
     * 32-bit spelling names the same physical register */
    {"eax", NRAX, 0}, {"ecx", NRCX, 0}, {"edx", NRDX, 0}, {"ebx", NRBX, 0},
    {"esp", NRSP, 0}, {"ebp", NRBP, 0}, {"esi", NRSI, 0}, {"edi", NRDI, 0},
    /* byte views */
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
    char buf[512];
    snprintf(buf, sizeof(buf), "asm: %.400s", what);
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

/* Operand kinds: 1 = register (possibly byte-forced), 2 = immediate,
 * 3 = memory at a named data symbol (RIP-relative with a fixup). */
static int noperand(const char *t, NXReg *r, int *byte, long long *imm,
                    int *is_mem, char *sym, size_t symcap, NXReg *r2) {
    size_t n = strlen(t);
    *is_mem = 0;
    if (sym) sym[0] = '\0';
    if (n >= 3 && t[0] == '[' && t[n - 1] == ']') {
        char inner[64];
        if (n - 2 >= sizeof(inner)) return 0;
        memcpy(inner, t + 1, n - 2);
        inner[n - 2] = '\0';
        int b;
        if (nreg(inner, r, &b) && !b) { *is_mem = 1; return 1; }
        /* [reg+disp] / [reg-disp]: a register-relative cell. */
        {
            char *sp = strpbrk(inner, "+-");
            if (sp && sp != inner) {
                char rn[16];
                size_t rl = (size_t)(sp - inner);
                long long dv;
                char *endp;
                if (rl < sizeof(rn)) {
                    memcpy(rn, inner, rl);
                    rn[rl] = 0;
                    dv = strtoll(sp, &endp, 0);
                    if (nreg(rn, r, &b) && !b && endp && *endp == 0) {
                        *imm = dv;
                        *is_mem = 1;
                        return 4;
                    }
                }
            }
            /* [base+index*scale]: the indexed-cell addressing form. */
            {
                char bse[16], idx[16];
                int sc = 0;
                if (sscanf(inner, "%15[a-z0-9]+%15[a-z0-9]*%d",
                           bse, idx, &sc) == 3 &&
                    (sc == 1 || sc == 2 || sc == 4 || sc == 8)) {
                    int bi, ii;
                    if (nreg(bse, r, &bi) && !bi &&
                        nreg(idx, r2, &ii) && !ii) {
                        *imm = sc;
                        *is_mem = 1;
                        return 5;
                    }
                }
            }
        }
        /* [symbol]: a data cell referenced by name (syscall stubs). */
        if (sym && n - 2 < symcap) {
            memcpy(sym, inner, n - 1);
            sym[n - 2] = '\0';
            return 3;
        }
        return 0;
    }
    if (nreg(t, r, byte)) return 1;
    char *end;
    long long v = strtoll(t, &end, 0);
    if (end != t && *end == '\0') { *imm = v; return 2; }
    return 0;
}

/* Records a RIP-relative fixup against a data symbol; patched after
 * the ELF layout knows the segment addresses. */
/* An asm operand names a data cell by its bare spelling, while the
 * cell was registered under its package-qualified name; matching on
 * the "::name" suffix covers both. */
static int nsym_match(const char *registered, const char *bare) {
    size_t n = strlen(registered), m = strlen(bare);
    if (strcmp(registered, bare) == 0) return 1;
    return n > m + 1 && registered[n - m - 1] == ':' &&
           registered[n - m - 2] == ':' &&
           strcmp(registered + n - m, bare) == 0;
}

static void ndfixup(NSel *s, const HNode *n, uint32_t pos, const char *sym) {
    for (int i = 0; i < s->ngl; i++)
        if (nsym_match(s->gl[i].name, sym)) {
            if (s->ndfx == s->capdfx) {
                s->capdfx = s->capdfx ? s->capdfx * 2 : 16;
                s->dfx = realloc(s->dfx, (size_t)s->capdfx * sizeof(*s->dfx));
            }
            s->dfx[s->ndfx].pos = pos;
            s->dfx[s->ndfx].data_off = s->gl[i].off;
            s->ndfx++;
            return;
        }
    char msg[160];
    snprintf(msg, sizeof(msg), "asm: unknown data symbol '%s'", sym);
    nerr(s, n, msg);
}

static void ninsn(NSel *s, const HNode *n, char *line) {
    NXBuf *b = s->text;
    /* The raw segment, saved before any in-place editing, is part of
     * every diagnostic so a reported failure always shows the exact
     * source text the assembler saw. */
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
    long long v1 = 0, v0 = 0;
    char sym0[64] = {0}, sym1[64] = {0};
    NXReg x2 = 0;
    int k0 = optok[0][0] ? noperand(optok[0], &r0, &b0, &v0, &m0, sym0, sizeof(sym0), &x2) : 0;
    int k1 = optok[1][0] ? noperand(optok[1], &r1, &b1, &v1, &m1, sym1, sizeof(sym1), &x2) : 0;
    if (!strncmp(mnem, "set", 3)) {
        /* setcc r8: the condition code lives in the opcode. */
        static const struct { const char *n; uint8_t op; } cc[] = {
            {"sete", 0x94}, {"setz", 0x94}, {"setne", 0x95}, {"setnz", 0x95},
            {"setl", 0x9C}, {"setnge", 0x9C}, {"setle", 0x9E}, {"setng", 0x9E},
            {"setg", 0x9F}, {"setnle", 0x9F}, {"setge", 0x9D}, {"setnl", 0x9D},
            {"seta", 0x97}, {"setnbe", 0x97}, {"setae", 0x93}, {"setnb", 0x93},
            {"setb", 0x92}, {"setnae", 0x92}, {"setbe", 0x96}, {"setna", 0x96},
            {"seto", 0x90}, {"setno", 0x91}, {"sets", 0x98}, {"setns", 0x99},
            {"setp", 0x9A}, {"setpe", 0x9A}, {"setnp", 0x9B}, {"setpo", 0x9B},
            {NULL, 0} };
        int qi, ok = 0;
        for (qi = 0; cc[qi].n; qi++)
            if (strcmp(mnem, cc[qi].n) == 0) { ok = 1; break; }
        if (ok && k0 == 1 && b0) {
            nx_b(b, 0x0F);
            nx_b(b, cc[qi].op);
            nx_b(b, (uint8_t)(0xC0 | (uint8_t)r0));
        } else if (ok) {
            nasm_err(s, n, "setcc wants a byte register");
        } else nasm_err(s, n, "unknown condition code");
        return;
    }
    if (!strcmp(mnem, "movzx")) {
        /* movzx r32, r8: zero-extending byte move. */
        if (k0 == 1 && k1 == 1 && b1 && !b0) {
            nx_b(b, 0x0F);
            nx_b(b, 0xB6);
            nx_b(b, (uint8_t)(0xC0 | (uint8_t)(r0 << 3) | (uint8_t)r1));
        } else nasm_err(s, n, "unsupported movzx operands");
        return;
    }
    if (!strcmp(mnem, "movsxd")) {
        /* movsxd r64, r32: sign-extending 32-bit move. */
        if (k0 == 1 && k1 == 1 && !b0 && !b1) {
            nx_b(b, 0x48);           /* REX.W */
            nx_b(b, 0x63);
            nx_b(b, (uint8_t)(0xC0 | (uint8_t)(r0 << 3) | (uint8_t)r1));
        } else nasm_err(s, n, "unsupported movsxd operands");
        return;
    }
    if (!strcmp(mnem, "movsx")) {
        /* movsx r64, r8: sign-extending byte move. */
        if (k0 == 1 && k1 == 1 && b1 && !b0) {
            nx_b(b, 0x48);           /* REX.W */
            nx_b(b, 0x0F);
            nx_b(b, 0xBE);
            nx_b(b, (uint8_t)(0xC0 | (uint8_t)(r0 << 3) | (uint8_t)r1));
        } else nasm_err(s, n, "unsupported movsx operands");
        return;
    }
    if (!strcmp(mnem, "lea")) {
        /* lea r, [sym]: address of a data cell, fixed up after
         * layout like mov r, [sym]. */
        if (k0 == 1 && k1 == 3 && !b0) {
            nx_b(b, 0x48); nx_b(b, 0x8D);
            nx_b(b, (uint8_t)(0x05 | (uint8_t)(r0 << 3)));
            uint32_t pos = (uint32_t)b->len;
            nx_b(b, 0); nx_b(b, 0); nx_b(b, 0); nx_b(b, 0);
            ndfixup(s, n, pos, sym1);
        }
        /* lea r, [base+disp]: address computation, no memory access. */
        else if (k0 == 1 && k1 == 4 && !b0) {
            uint8_t mod = (v1 == 0) ? 0x00 : (v1 >= -128 && v1 <= 127)
                                              ? 0x40 : 0x80;
            nx_b(b, 0x48);               /* REX.W              */
            nx_b(b, 0x8D);               /* lea                */
            if (r1 == NRSP || r1 == NR12) {
                /* rsp/r12 need the SIB byte */
                nx_b(b, (uint8_t)(mod | (uint8_t)(r0 << 3) | 0x04));
                nx_b(b, 0x24);           /* base rsp, no index */
            } else {
                nx_b(b, (uint8_t)(mod | (uint8_t)(r0 << 3) | (uint8_t)r1));
            }
            if (mod == 0x40) nx_b(b, (uint8_t)v1);
            else if (mod == 0x80) {
                int32_t d = (int32_t)v1;
                nx_b(b, (uint8_t)d); nx_b(b, (uint8_t)(d >> 8));
                nx_b(b, (uint8_t)(d >> 16)); nx_b(b, (uint8_t)(d >> 24));
            }
        }
        else if (k0 == 1 && k1 == 5 && !b0) {
            /* lea r, [base+index*scale] */
            int scale = (int)v1;
            nx_b(b, 0x48);
            nx_b(b, 0x8D);
            nx_b(b, (uint8_t)((uint8_t)(r0 << 3) | 0x04));
            nx_b(b, (uint8_t)((scale == 8 ? 3 : scale == 4 ? 2 :
                               scale == 2 ? 1 : 0) << 6 |
                              (uint8_t)(x2 << 3) | (uint8_t)r1));
        }
        else if (k0 == 1 && k1 == 1 && !b0 && !b1) {
            /* lea r, r is a form of add r, 0 */
            nx_mov_rr(b, r0, r1);
        }
        else nasm_err(s, n, "unsupported lea operands");
        return;
    }
    if (!strcmp(mnem, "mov")) {
        if (k0 == 1 && k1 == 1 && !m0 && !m1) {
            if (b0 && b1) nmov8_rr(b, r0, r1);
            else if (!b0 && !b1) nx_mov_rr(b, r0, r1);
            else nasm_err(s, n, "mov between different register widths");
        } else if (k0 == 1 && k1 == 2 && !m0) nx_mov_ri(b, r0, (uint64_t)v1);
        else if (k0 == 1 && k1 == 4 && !m0 && !b0) nx_mov_rm(b, r0, r1, (int32_t)v1);
        else if (k0 == 4 && k1 == 1 && !b1) nx_mov_mr(b, r0, (int32_t)v0, r1);
        else if (k0 == 1 && m1) { if (b0) nx_mov8_r0(b, r0, r1); else nx_mov_rm(b, r0, r1, 0); }
        else if (m0 && k1 == 1) { if (b1) nx_mov8_m0(b, r0, r1); else nx_mov_mr(b, r0, 0, r1); }
        else if (m0 && k1 == 2) nx_mov8_mi0(b, r0, (uint8_t)v1);
        else if (k0 == 1 && k1 == 3 && !b0) {
            /* mov r, [sym]: RIP-relative load, fixed up after layout. */
            nx_b(b, 0x48); nx_b(b, 0x8B);
            nx_b(b, (uint8_t)(0x05 | (r0 << 3)));
            uint32_t pos = (uint32_t)b->len;
            nx_b(b, 0); nx_b(b, 0); nx_b(b, 0); nx_b(b, 0);
            ndfixup(s, n, pos, sym1);
        }
        else if (k0 == 3 && k1 == 1 && !b1) {
            /* mov [sym], r: RIP-relative store. */
            nx_b(b, 0x48); nx_b(b, 0x89);
            nx_b(b, (uint8_t)(0x05 | (r1 << 3)));
            uint32_t pos = (uint32_t)b->len;
            nx_b(b, 0); nx_b(b, 0); nx_b(b, 0); nx_b(b, 0);
            ndfixup(s, n, pos, sym0);
        }
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
                     "unsupported ALU operand form '%.24s %.24s, %.24s' in '%.100s'",
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
        /* A call may name a module function (the decompiler emits
         * those) or a local label; functions bind to their entry
         * label in the shared text buffer. */
        int l = -1;
        for (int fi = 0; fi < s->nfn; fi++)
            if (strcmp(s->fn[fi].name, optok[0]) == 0) {
                l = s->fn[fi].lbl;
                break;
            }
        if (l < 0) {
            /* not emitted yet: record a fixup resolved after all
             * functions exist */
            if (s->ncfx >= s->capcfx) {
                s->capcfx = s->capcfx ? s->capcfx * 2 : 16;
                s->cfx = realloc(s->cfx,
                                 (size_t)s->capcfx * sizeof(*s->cfx));
            }
            nx_b(b, 0xE8);           /* call placeholder     */
            nx_b(b, 0x00); nx_b(b, 0x00);
            nx_b(b, 0x00); nx_b(b, 0x00);
            s->cfx[s->ncfx].pos = (uint32_t)b->len - 4;
            s->cfx[s->ncfx].name = sm_strdup(optok[0]);
            s->ncfx++;
            return;
        }
        nx_call(b, l);
        return;
    }
    int cc = njcc(mnem);
    if (cc >= 0) {
        int l = nlabel(b, optok[0]);
        if (l >= 0) nx_jcc(b, cc, l); else nasm_err(s, n, "jump to an unknown label");
        return;
    }
    {
        char what[224];
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

/* Emits `mov [base], src8` with full SIB/disp handling: the rsp and
 * rbp bases are not encodable in plain mod=00 form (rsp needs a SIB
 * byte, rbp a displacement), so without these special cases the
 * instruction stream would desynchronize. */
/* The load counterpart of nmov8_m0_full (8A /r). */
/* The immediate-byte store counterpart (C6 /0 ib). */


/* Binds inline-asm call fixups to function entries. */
void nresolve_call_fixups(NSel *s) {
    NXBuf *b = s->text;
    int i;
    for (i = 0; i < s->ncfx; i++) {
        int fi, found = 0;
        for (fi = 0; fi < s->nfn; fi++) {
            if (strcmp(s->fn[fi].name, s->cfx[i].name) == 0) {
                int lbl = s->fn[fi].lbl;
                if (lbl >= 0 && lbl < b->nlbl &&
                    b->lbl[lbl] != NX_UNDEF) {
                    int32_t rel = (int32_t)b->lbl[lbl] -
                                  (int32_t)(s->cfx[i].pos + 4);
                    uint8_t *p = b->buf + s->cfx[i].pos;
                    p[0] = (uint8_t)rel;
                    p[1] = (uint8_t)(rel >> 8);
                    p[2] = (uint8_t)(rel >> 16);
                    p[3] = (uint8_t)(rel >> 24);
                    found = 1;
                }
                break;
            }
        }
        if (!found) {
            diag_report(s->diag, DK_ERROR, "<nemit>", 0, 0,
                        "unresolved code reference: %s",
                        s->cfx[i].name);
            s->err = 1;
        }
    }
}
