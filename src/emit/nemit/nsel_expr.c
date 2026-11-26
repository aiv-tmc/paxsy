#include "common.h"
#include <limits.h>
#include "nsel.h"

/*
 * nsel_expr.c - instruction selection for value expressions. Every
 * expression evaluates into rax; rcx, rdx, r10-r11 are scratch.
 * Integer and pointer values are full 64-bit words, so casts between
 * them are no-ops. Array elements are words too: an array occupies
 * count * 8 bytes and every element access uses an 8-byte stride.
 * Calls returning T! leave the payload in rax and the error code in
 * rdx. Every call is an ordinary function reference resolved against
 * the function table: the standard library is compiled from Paxsy
 * sources like any other code, so the selector carries no built-in
 * callees. An interpolated string (HK_FMT) becomes one write syscall
 * per literal chunk (chunk lengths are known at compile time) plus an
 * inline unsigned-decimal conversion and write per expression part;
 * the conversion reuses the red-zone digit algorithm of the standard
 * library and emits no external calls. Failures name the offender.
 */

/* Loads [rax] into rax honoring the value width: narrow types
 * zero-extend a single byte, everything else is a full slot. */
static void nload_typed(NSel *s, NXBuf *b, const HType *t) {
    (void)s;
    if (t && t->kind == HTY_REAL && t->width == 32) {
        /* Memory stores Real32 as IEEE-754 binary32; expressions use
         * the backend's uniform binary64 register representation. */
        nx_b(b, 0xF3); nx_b(b, 0x0F); nx_b(b, 0x10); nx_b(b, 0x00); /* movss xmm0,[rax] */
        nx_b(b, 0xF3); nx_b(b, 0x0F); nx_b(b, 0x5A); nx_b(b, 0xC0); /* cvtss2sd */
        nx_movq_rx(b, NRAX, 0);
        return;
    }
    /* Load exactly the cell width and bring the value to a full
     * register so plain consumers (and the conditional code) see the
     * properly truncated value. */
    if (t && (t->kind == HTY_CHAR || t->kind == HTY_BOOL)) {
        nx_mov8_r0(b, NRAX, NRAX);
        nx_b(b, 0x0F); nx_b(b, 0xB6); nx_b(b, 0xC0);
    } else if (t && (t->kind == HTY_INT || t->kind == HTY_CHAR)) {
        /* memory forms: the value sits at [rax] */
        long sz = hty_size_bytes(t);
        if (sz == 1) { nx_b(b, 0x48); nx_b(b, 0x0F); nx_b(b, 0xBE); nx_b(b, 0x00); }
        else if (sz == 2) { nx_b(b, 0x48); nx_b(b, 0x0F); nx_b(b, 0xBF); nx_b(b, 0x00); }
        else if (sz == 4) { nx_b(b, 0x48); nx_b(b, 0x63); nx_b(b, 0x00); }
        else nx_mov_rm(b, NRAX, NRAX, 0);
    } else if (t && t->kind == HTY_UINT) {
        long sz = hty_size_bytes(t);
        if (sz == 1) { nx_b(b, 0x0F); nx_b(b, 0xB6); nx_b(b, 0x00); }
        else if (sz == 2) { nx_b(b, 0x0F); nx_b(b, 0xB7); nx_b(b, 0x00); }
        else if (sz == 4) { nx_b(b, 0x8B); nx_b(b, 0x00); }
        else nx_mov_rm(b, NRAX, NRAX, 0);
    } else {
        nx_mov_rm(b, NRAX, NRAX, 0);
    }
}

static const char *nstruct_tail(const char *name) {
    const char *tail = strrchr(name ? name : "", ':');
    return tail && tail[1] ? tail + 1 : (name ? name : "");
}

static const HNode *nfind_struct(NSel *s, const char *name) {
    const HNode *match = NULL;
    for (int i = 0; i < s->nst; i++) {
        if (!s->st[i]->name) continue;
        if (strcmp(s->st[i]->name, name) == 0) return s->st[i];
        if (strcmp(nstruct_tail(s->st[i]->name), nstruct_tail(name)) == 0) {
            if (match) return NULL;
            match = s->st[i];
        }
    }
    return match;
}

/* Allocates scratch for an uninitialized named object and leaves its
 * base address in rax. The size is the sum of the natural-aligned
 * field sizes, so the object layout matches field addressing. */
void nalloc_named(NSel *s, const HType *t) {
    NXBuf *b = s->text;
    const char *sn = (t && t->kind == HTY_NAMED) ? t->u.named.name : NULL;
    const HNode *st = nfind_struct(s, sn ? sn : "");
    long total = 0;
    for (int fi = 0; st && fi < st->nitems; fi++) {
        const HType *ft = st->items[fi]->type;
        long fs = ft ? hty_size_bytes(ft) : 8;
        if (fs <= 0) fs = 8;
        total += (fs + 7) & ~7L;
    }
    if (total <= 0) total = 8;
    long base = s->scratch_base + s->scratch_used;
    s->scratch_used += (total + 7) & ~7L;
    if (s->scratch_used > 4096) {
        nerr(s, st, "too many live named objects (scratch exhausted)");
        return;
    }
    nx_lea(b, NRAX, NRBP, -(int32_t)(s->scratch_base + 4096 - base));
}

/* Materializes an implicit numeric conversion of the value in rax
 * when a store target of a different numeric interpretation needs
 * it (Int-width differences are no-ops: values are kept sign-extended
 * in 64-bit registers). */
void nstore_convert(NSel *s, const HType *tgt, const HType *val) {
    NXBuf *b = s->text;
    if (!tgt || !val) return;
    if (tgt == val && !(tgt->kind == HTY_REAL && tgt->width == 32))
        return;
    int ti = (tgt->kind == HTY_INT || tgt->kind == HTY_UINT ||
              tgt->kind == HTY_REAL || tgt->kind == HTY_CHAR);
    int vi = (val->kind == HTY_INT || val->kind == HTY_UINT ||
              val->kind == HTY_REAL || val->kind == HTY_CHAR);
    if (!ti || !vi) return;
    if (tgt->kind == HTY_REAL && tgt->width == 32) {
        if (val->kind != HTY_REAL) {
            nx_cvtsi2sd_0r(b, NRAX);
            nx_movq_rx(b, NRAX, 0);
        } else {
            nx_movq_xr(b, 0, NRAX);
        }
        /* Round to binary32, then widen back to the native binary64
         * expression representation held in local/parameter slots. */
        nx_b(b, 0xF2); nx_b(b, 0x0F); nx_b(b, 0x5A); nx_b(b, 0xC0); /* cvtsd2ss */
        nx_b(b, 0xF3); nx_b(b, 0x0F); nx_b(b, 0x5A); nx_b(b, 0xC0); /* cvtss2sd */
        nx_movq_rx(b, NRAX, 0);
    } else if (tgt->kind == HTY_REAL && val->kind != HTY_REAL) {
        nx_cvtsi2sd_0r(b, NRAX);
        nx_movq_rx(b, NRAX, 0);
    } else if (val->kind == HTY_REAL && tgt->kind != HTY_REAL) {
        nx_movq_xr(b, 0, NRAX);
        nx_cvttsd2si_r0(b, NRAX);
    }
}

/* Converts an expression value to the physical representation used
 * by an addressed memory cell. Real32 is the one scalar whose memory
 * width differs from the native expression-register width. */
void nstore_convert_mem(NSel *s, const HType *tgt, const HType *val) {
    NXBuf *b = s->text;
    if (tgt && tgt->kind == HTY_REAL && tgt->width == 32) {
        if (val && (val->kind == HTY_INT || val->kind == HTY_UINT ||
                    val->kind == HTY_CHAR)) {
            nx_b(b, 0xF3); nx_b(b, 0x48); nx_b(b, 0x0F); nx_b(b, 0x2A); nx_b(b, 0xC0); /* cvtsi2ss xmm0,rax */
        } else {
            nx_movq_xr(b, 0, NRAX);
            nx_b(b, 0xF2); nx_b(b, 0x0F); nx_b(b, 0x5A); nx_b(b, 0xC0); /* cvtsd2ss */
        }
        nx_b(b, 0x66); nx_b(b, 0x0F); nx_b(b, 0x7E); nx_b(b, 0xC0); /* movd eax,xmm0 */
        return;
    }
    nstore_convert(s, tgt, val);
}

/* Byte offset of the struct field at position 'idx', laid out in
 * declaration order with natural alignment; mirrors hty_size_bytes
 * so allocation sizes and field addressing agree. */
static long nfield_off(NSel *s, const char *st_name, long idx) {
    const HNode *st = nfind_struct(s, st_name);
    long off = 0;
    if (!st) return idx * 8;
    for (long i = 0; i < idx && i < st->nitems; i++) {
        long sz = st->items[i]->type ? hty_size_bytes(st->items[i]->type) : 8;
        long al = sz < 8 ? (sz > 1 ? sz : 1) : 8;
        off = (off + al - 1) / al * al;
        off += sz > 0 ? sz : 8;
    }
    return off;
}

void nerr(NSel *s, const HNode *n, const char *what) {
    diag_report(s->diag, DK_ERROR, "<nemit>",
                n ? n->tok.line : 0, n ? n->tok.col : 0,
                "native backend: %s", what);
    s->err++;
}

void nabs(NSel *s, int sect, uint32_t off) {
    NXBuf *b = s->text;
    if (s->nabs == s->capabs) {
        s->capabs = s->capabs ? s->capabs * 2 : 16;
        s->abs = realloc(s->abs, (size_t)s->capabs * sizeof(s->abs[0]));
    }
    s->abs[s->nabs].pos = nx_here(b) - 8;
    s->abs[s->nabs].sect = sect;
    s->abs[s->nabs].off = off;
    s->nabs++;
}

/* Appends `len` bytes to the read-only data image and returns the
 * offset of the first byte. */
static uint32_t nro_intern(NSel *s, const char *ptr, size_t len) {
    uint32_t off = (uint32_t)s->ro->len;
    s->ro->buf = realloc(s->ro->buf, s->ro->len + len + 1);
    memcpy(s->ro->buf + s->ro->len, ptr, len);
    s->ro->buf[s->ro->len + len] = 0;
    s->ro->len += len + 1;
    s->ro->cap = s->ro->len;
    return off;
}

static int nglobal(NSel *s, const HNode *n, uint32_t *off) {
    for (int i = 0; i < s->ngl; i++)
        if (strcmp(s->gl[i].name, n->name) == 0) {
            *off = s->gl[i].off;
            return 1;
        }
    nerr(s, n, "reference to an unknown global");
    *off = 0;
    return 0;
}

static int ncc_of(HBinOp op) {
    switch (op) {
    case HOP_EQ: return NCC_E;
    case HOP_NE: return NCC_NE;
    case HOP_LT: return NCC_L;
    case HOP_LE: return NCC_LE;
    case HOP_GT: return NCC_G;
    default:     return NCC_GE;
    }
}

/* Emits write(1, buf, len) with literal register operands. */

/* Converts the signed value in rax to decimal and writes it to fd 1.
 * The digit buffer lives in the x86-64 red zone below rsp, which the
 * kernel never touches across a syscall; labels are local to this
 * emission block and resolved by the shared fixup pass. */

/* Decimal formatting of the signed value in RAX into a caller
 * buffer: digits are written downward from the `base` register, so
 * at least 24 bytes must sit below it. Output: RSI = start, RDX =
 * length (RSI + RDX == base always, so the NUL goes at base). */
static void nitoa_buf(NSel *s, NXReg base) {
    NXBuf *b = s->text;
    int labs = nx_label(b), ldig = nx_label(b), lwr = nx_label(b);
    nx_test_rr(b, NRAX, NRAX);
    nx_jcc(b, NCC_NS, labs);
    nx_neg_r(b, NRAX);
    nx_mov_ri(b, NR8, 1);
    nx_def(b, labs);
    nx_mov_rr(b, NRSI, base);
    nx_mov_ri(b, NR9, 10);
    nx_def(b, ldig);
    nx_mov_ri(b, NRDX, 0);
    {
        /* unsigned divide rdx:rax / r9, emitted locally: the helpers
         * nrex/nmodrm are private to nsel.c */
        int rex = 0x48 | ((NR9 >= 8) ? 1 : 0);
        nx_b(b, (uint8_t)rex);
        nx_b(b, 0xF7);
        nx_b(b, (uint8_t)(0xC0 | (6 << 3) | (NR9 & 7)));
    }
    nx_mov_ri(b, NR10, 48);
    nx_add_rr(b, NRDX, NR10);
    nx_dec_r(b, NRSI);
    nx_mov8_m0(b, NRSI, NRDX);
    nx_test_rr(b, NRAX, NRAX);
    nx_jcc(b, NCC_NE, ldig);
    nx_test_rr(b, NR8, NR8);
    nx_jcc(b, NCC_E, lwr);
    nx_dec_r(b, NRSI);
    nx_mov_ri(b, NR10, 45);
    nx_mov8_m0(b, NRSI, NR10);
    nx_def(b, lwr);
    nx_mov_rr(b, NRDX, base);
    nx_sub_rr(b, NRDX, NRSI);
}

/* Scalar-to-string conversion of the value in RAX: the decimal
 * digits and the NUL terminator land in a fresh 32-byte frame-scratch
 * slot and RAX receives the pointer to the start of the string. The
 * slot layout is [A, A+32): digits are written downward from A+21, so
 * at most 20 digits plus the terminator at A+21 always fit. */
static void nscalar_str(NSel *s, const HNode *n) {
    NXBuf *b = s->text;
    long base = s->scratch_base + s->scratch_used;
    s->scratch_used += 32;
    if (s->scratch_used > 4096) {
        nerr(s, n, "too many scalar-to-string conversions "
                   "(scratch exhausted)");
        nx_mov_ri(b, NRAX, 0);
        return;
    }
    int32_t a_off = -(int32_t)(s->scratch_base + 4096 - base);
    nx_mov_ri(b, NR8, 0);            /* sign flag clear */
    nx_lea(b, NRDI, NRBP, a_off + 21);
    nitoa_buf(s, NRDI);
    /* NUL-terminate at RSI + RDX (== the base register). */
    nx_mov_rr(b, NR10, NRSI);
    nx_add_rr(b, NR10, NRDX);
    nx_mov_ri(b, NR9, 0);
    nx_mov8_m0(b, NR10, NR9);
    nx_mov_rr(b, NRAX, NRSI);
}

/* Inline strlen: input RAX = pointer, output RCX = byte length
 * before the NUL. Clobbers RCX and flags only. */
static void nstrlen_inline(NSel *s) {
    /* Layout: mov rcx,0; L: cmp byte [rax+rcx],0; je E; inc rcx;
     * jmp L; E: ...  The two rel8 slots are patched once the sizes
     * of nx_mov_ri/nx_inc_r are known. */
    NXBuf *b = s->text;
    nx_mov_ri(b, NRCX, 0);
    size_t at_cmp = b->len;
    nx_b(b, 0x80);               /* cmp byte [rax+rcx], 0 */
    nx_b(b, 0x3C);
    nx_b(b, 0x08);
    nx_b(b, 0x00);
    size_t at_je = b->len;
    nx_b(b, 0x74);               /* je exit (patched below) */
    nx_b(b, 0x00);
    nx_inc_r(b, NRCX);
    size_t at_jmp = b->len;
    nx_b(b, 0xEB);               /* jmp at_cmp (patched below) */
    nx_b(b, 0x00);
    b->buf[at_je + 1] = (uint8_t)((at_jmp + 2) - (at_je + 2));
    b->buf[at_jmp + 1] = (uint8_t)(at_cmp - (at_jmp + 2));
}

static int nbinop_immediate(NSel *s, const HNode *n) {
    const HNode *left = n->child[0];
    const HNode *right = n->child[1];
    if (!left || !right || right->kind != HK_CONST_INT ||
        (n->type && n->type->kind == HTY_REAL) ||
        (left->type && left->type->kind == HTY_REAL))
        return 0;

    long long value = right->u.ival;
    long long scaled = value;
    if (n->u.binop == HOP_ADD || n->u.binop == HOP_SUB) {
        if (left->type && left->type->kind == HTY_PTR) {
            long scale = ncell_bytes(left->type->u.ptr.elem);
            if (scale > 1) {
                if (value > LLONG_MAX / scale || value < LLONG_MIN / scale)
                    return 0;
                scaled = value * scale;
            }
        }
        if (scaled < INT32_MIN || scaled > INT32_MAX) return 0;
        nexpr(s, left);
        if (n->u.binop == HOP_ADD) nx_add_ri(s->text, NRAX, (int32_t)scaled);
        else nx_sub_ri(s->text, NRAX, (int32_t)scaled);
        return 1;
    }
    if (n->u.binop == HOP_MUL && value >= INT32_MIN && value <= INT32_MAX) {
        nexpr(s, left);
        nx_imul_ri(s->text, NRAX, (int32_t)value);
        return 1;
    }
    if ((n->u.binop == HOP_AND || n->u.binop == HOP_OR ||
         n->u.binop == HOP_XOR || n->u.binop == HOP_EQ ||
         n->u.binop == HOP_NE || n->u.binop == HOP_LT ||
         n->u.binop == HOP_LE || n->u.binop == HOP_GT ||
         n->u.binop == HOP_GE) &&
        value >= INT32_MIN && value <= INT32_MAX) {
        nexpr(s, left);
        switch (n->u.binop) {
        case HOP_AND: nx_and_ri(s->text, NRAX, (int32_t)value); break;
        case HOP_OR:  nx_or_ri(s->text, NRAX, (int32_t)value); break;
        case HOP_XOR: nx_xor_ri(s->text, NRAX, (int32_t)value); break;
        default:
            nx_cmp_ri(s->text, NRAX, (int32_t)value);
            nx_setcc(s->text, NRAX, ncc_of(n->u.binop));
            nx_b(s->text, 0x0F);
            nx_b(s->text, 0xB6);
            nx_b(s->text, 0xC0);
            break;
        }
        return 1;
    }
    if ((n->u.binop == HOP_SHL || n->u.binop == HOP_SHR ||
         n->u.binop == HOP_USHR) && value >= 0 && value < 64) {
        nexpr(s, left);
        if (n->u.binop == HOP_SHL)
            nx_shl_ri(s->text, NRAX, (uint8_t)value);
        else if (n->u.binop == HOP_SHR)
            nx_sar_ri(s->text, NRAX, (uint8_t)value);
        else
            nx_shr_ri(s->text, NRAX, (uint8_t)value);
        return 1;
    }
    return 0;
}

void nexpr(NSel *s, const HNode *n) {
    NXBuf *b = s->text;
    uint32_t goff;
    if (!n) { nx_mov_ri(b, NRAX, 0); return; }
    switch (n->kind) {
    case HK_CONST_INT:
        nx_mov_ri(b, NRAX, (uint64_t)n->u.ival);
        break;
    case HK_CONST_FLOAT: {
        /* The literal travels as its raw IEEE-754 bit pattern in a
         * general register; consumers needing the numeric value use
         * the scalar-double forms. */
        union { double d; uint64_t u; } fb;
        fb.d = n->u.fval;
        nx_mov_ri(b, NRAX, fb.u);
        break;
    }
    case HK_CONST_CHAR: {
        /* A Char is a UTF-8 code unit, not the address of a string.
         * Keep it numeric in the native backend, matching the C emitter.
         * Character escapes are decoded here because the lexer preserves
         * the source slice verbatim for diagnostics and formatting. */
        const unsigned char *p = (const unsigned char *)n->u.slice.start;
        unsigned long value = p[0];
        if (p[0] == '\\' && n->u.slice.len >= 2) {
            switch (p[1]) {
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            case '0': value = 0; break;
            case '\\': value = '\\'; break;
            case '\'': value = '\''; break;
            case '"': value = '"'; break;
            default: value = p[1]; break;
            }
        }
        nx_mov_ri(b, NRAX, value);
        break;
    }
    case HK_CONST_STRING: {
        /* The aux id may be stale when the optimiser cloned the node
         * without carrying the string table state, so the id is
         * validated against the table and the constant is re-interned
         * whenever it does not match exactly. */
        size_t len = n->name ? strlen(n->name) : (size_t)n->u.slice.len;
        int id = n->aux;
        if (id < 0 || id >= s->nstr || s->str_len[id] != (int)len) {
            id = s->nstr++;
            s->str_off = realloc(s->str_off,
                                 (size_t)s->nstr * sizeof(uint32_t));
            s->str_len = realloc(s->str_len,
                                 (size_t)s->nstr * sizeof(int));
            s->str_off[id] = nro_intern(s, n->name ? n->name
                                                   : n->u.slice.start,
                                        len);
            s->str_len[id] = (int)len;
            ((HNode *)n)->aux = id;
        }
        nx_mov_ri(b, NRAX, 0);
        nabs(s, NSECT_RO, s->str_off[id]);
        break;
    }
        case HK_FMT: {
        /* Interpolated string as a value. Parts are evaluated once
         * into frame scratch, the byte length is summed at run time
         * and the parts are assembled in a stack buffer that stays
         * live until the function returns, like a string literal.
         * Items alternate: even slots literal chunks, odd slots
         * expressions (scalars format as decimal, strings copy). */
        int np = n->nitems;
        NXBuf *b = s->text;
        if (np <= 1) {
            /* degenerate: a single chunk, no expressions */
            nexpr(s, n->items[0]);
            break;
        }
        int nexprs = np / 2;
        long base = s->scratch_base + s->scratch_used;
        s->scratch_used += (long)(nexprs + 1) * 8;
        if (s->scratch_used > 4096) {
            nerr(s, n, "string interpolation scratch exhausted");
            nx_mov_ri(b, NRAX, 0);
            break;
        }
        int32_t a_off = -(int32_t)(s->scratch_base + 4096 - base);
        /* slot k lives at rbp + a_off + 8*k: values at 0..nexprs-1,
         * the length accumulator at slot nexprs */
        int acc = nexprs;
        nx_lea(b, NRDI, NRBP, a_off + 8 * acc);
        nx_mov_ri(b, NRAX, 0);
        nx_mov_mr(b, NRDI, 0, NRAX);
        for (int i = 0; i < np; i++) {
            if (i % 2 == 0) {
                /* literal chunk: compile-time length */
                int litlen = n->items[i]->u.slice.len;
                nx_lea(b, NRDI, NRBP, a_off + 8 * acc);
                nx_mov_rm(b, NRCX, NRDI, 0);
                nx_mov_ri(b, NRDX, litlen);
                nx_add_rr(b, NRCX, NRDX);
                nx_mov_mr(b, NRDI, 0, NRCX);
            } else {
                const HNode *e = n->items[i];
                /* evaluate, stash the value */
                nexpr(s, e);
                nx_lea(b, NRDI, NRBP, a_off + 8 * (i / 2));
                nx_mov_mr(b, NRDI, 0, NRAX);
                const HType *et = e->type;
                /* An error-typed value formats as its payload: the
                 * px_res value word sits in the slot like a plain
                 * scalar. */
                if (et && et->kind == HTY_ERROR) et = et->u.error.value;
                int scalar = et &&
                    (et->kind == HTY_INT || et->kind == HTY_UINT ||
                     et->kind == HTY_BOOL || et->kind == HTY_CHAR);
                if (scalar) {
                    /* decimal digits fit in 21 bytes incl. sign */
                    nx_lea(b, NRDI, NRBP, a_off + 8 * acc);
                    nx_mov_rm(b, NRCX, NRDI, 0);
                    nx_mov_ri(b, NRDX, 21);
                    nx_add_rr(b, NRCX, NRDX);
                    nx_mov_mr(b, NRDI, 0, NRCX);
                } else {
                    /* string part: strlen adds the exact length */
                    nstrlen_inline(s);         /* rax=ptr -> rcx=len */
                    nx_lea(b, NRDI, NRBP, a_off + 8 * acc);
                    nx_mov_rm(b, NRDX, NRDI, 0);
                    nx_add_rr(b, NRDX, NRCX);
                    nx_mov_mr(b, NRDI, 0, NRDX);
                }
            }
        }
        /* total = acc + 1 (NUL), rounded up to 16 */
        nx_lea(b, NRDI, NRBP, a_off + 8 * acc);
        nx_mov_rm(b, NRAX, NRDI, 0);
        nx_mov_ri(b, NRCX, 1);
        nx_add_rr(b, NRAX, NRCX);
        nx_mov_ri(b, NRCX, 15);
        nx_add_rr(b, NRAX, NRCX);
        nx_mov_ri(b, NRCX, -16);
        nx_and_rr(b, NRAX, NRCX);
        nx_sub_rr(b, NRSP, NRAX);
        nx_mov_rr(b, NR15, NRSP);          /* cursor */
        for (int i = 0; i < np; i++) {
            if (i % 2 == 0) {
                /* literal chunk copy */
                int litlen = n->items[i]->u.slice.len;
                if (litlen <= 0) continue;
                nexpr(s, n->items[i]);       /* rax = rodata ptr */
                nx_mov_rr(b, NRSI, NRAX);
                nx_mov_ri(b, NRCX, litlen);
                nx_mov_rr(b, NR8, NRCX);     /* save len (rep eats rcx) */
                nx_mov_rr(b, NRDI, NR15);
                nx_b(b, 0xF3);               /* rep movsb */
                nx_b(b, 0xA4);
                nx_add_rr(b, NR15, NR8);
            } else {
                const HNode *e = n->items[i];
                nx_lea(b, NRDI, NRBP, a_off + 8 * (i / 2));
                nx_mov_rm(b, NRAX, NRDI, 0); /* reload value */
                const HType *et = e->type;
                /* An error-typed value formats as its payload: the
                 * px_res value word sits in the slot like a plain
                 * scalar. */
                if (et && et->kind == HTY_ERROR) et = et->u.error.value;
                int scalar = et &&
                    (et->kind == HTY_INT || et->kind == HTY_UINT ||
                     et->kind == HTY_BOOL || et->kind == HTY_CHAR);
                if (scalar) {
                    nx_mov_ri(b, NR8, 0);    /* sign flag clear */
                    nx_mov_rr(b, NRDI, NR15);
                    nx_mov_ri(b, NRCX, 21);
                    nx_add_rr(b, NRDI, NRCX);
                    nitoa_buf(s, NRDI);      /* rsi=start, rdx=len */
                    /* digits sit below the +21 scratch window:
                     * copy them up to the cursor */
                    nx_mov_rr(b, NRCX, NRDX);
                    nx_mov_rr(b, NRDI, NR15);
                    nx_b(b, 0xF3);           /* rep movsb */
                    nx_b(b, 0xA4);
                    nx_add_rr(b, NR15, NRDX);
                } else {
                    nstrlen_inline(s);     /* rax=ptr -> rcx=len */
                    nx_mov_rr(b, NR8, NRCX);
                    nx_mov_rr(b, NRSI, NRAX);
                    nx_mov_rr(b, NRDI, NR15);
                    nx_b(b, 0xF3);
                    nx_b(b, 0xA4);
                    nx_add_rr(b, NR15, NR8);
                }
            }
        }
        /* NUL-terminate and yield the buffer start */
        nx_mov_ri(b, NR9, 0);
        nx_mov8_m0(b, NR15, NR9);
        nx_mov_rr(b, NRAX, NRSP);
        break;
    }
    case HK_PARAM:
        nx_mov_rm(b, NRAX, NRBP, nparam_off(n->u.index));
        break;
    case HK_MATCH:
        if (n->flags & HF_EXPR) {
            /* value match: the statement emitter's dispatch leaves
             * the matching arm's value in RAX */
            nstmt(s, n);
            break;
        }
        nerr(s, n, "match used as an expression");
        break;
    case HK_LOCAL:
        /* A fixed-size array local is its own storage: reading it
         * yields the slot address. An unsized array (Char[]) is a
         * fat pointer stored in the slot: reading it yields the
         * pointed-to data. */
        if (n->type && n->type->kind == HTY_ARRAY &&
            n->type->u.array.count > 0)
            nx_lea(b, NRAX, NRBP, nslot_off(s, n->u.index));
        else
            nx_mov_rm(b, NRAX, NRBP, nslot_off(s, n->u.index));
        break;
    case HK_GLOBAL_REF:
        if (nglobal(s, n, &goff)) {
            nx_mov_ri(b, NRAX, 0);
            nabs(s, NSECT_DATA, goff);
            if (!(n->type && n->type->kind == HTY_ARRAY))
                nx_mov_rm(b, NRAX, NRAX, 0);
        } else nx_mov_ri(b, NRAX, 0);
        break;
    case HK_BINOP: {
        HBinOp op = n->u.binop;
        if (op != HOP_LAND && op != HOP_LOR &&
            nbinop_immediate(s, n))
            break;
        if (op == HOP_LAND || op == HOP_LOR) {
            /* Short-circuit evaluation. For 'and' a false operand
             * lands on the 0 label; for 'or' a true operand lands
             * on the 1 label, so each operator keeps its neutral
             * fall-through value. */
            int lshort = nx_label(b), ldone = nx_label(b);
            nexpr(s, n->child[0]);
            nx_test_rr(b, NRAX, NRAX);
            nx_jcc(b, op == HOP_LAND ? NCC_E : NCC_NE, lshort);
            nexpr(s, n->child[1]);
            nx_test_rr(b, NRAX, NRAX);
            nx_jcc(b, op == HOP_LAND ? NCC_E : NCC_NE, lshort);
            /* both operands decided the result: fall-through value */
            nx_mov_ri(b, NRAX, op == HOP_LAND ? 1 : 0);
            nx_jmp(b, ldone);
            nx_def(b, lshort);
            nx_mov_ri(b, NRAX, op == HOP_LAND ? 0 : 1);
            nx_def(b, ldone);
            break;
        }
        nexpr(s, n->child[0]);
        nx_push(b, NRAX);
        nexpr(s, n->child[1]);
        nx_mov_rr(b, NRCX, NRAX);
        nx_pop(b, NRAX);
        if (n->type && n->type->kind == HTY_REAL) {
            /* Real arithmetic: both operands travel as raw IEEE-754
             * bit patterns; xmm0/xmm1 do the scalar-double work. */
            nx_movq_xr(b, 0, NRAX);
            nx_movq_xr(b, 1, NRCX);
            switch (op) {
            case HOP_ADD: nx_sdsd(b, 0x58); break;
            case HOP_SUB: nx_sdsd(b, 0x5C); break;
            case HOP_MUL: nx_sdsd(b, 0x59); break;
            case HOP_DIV: nx_sdsd(b, 0x5E); break;
            default:
                nerr(s, n, "unsupported real arithmetic operator");
                break;
            }
            nx_movq_rx(b, NRAX, 0);
            break;
        }
        if (n->child[0]->type &&
            n->child[0]->type->kind == HTY_REAL) {
            /* Real comparison: comisd sets the flags in unsigned
             * order (CF below, ZF equal). The parity flag reports
             * an unordered (NaN) result, which must make every
             * comparison false except !=; every case combines the
             * ordering flag with setnp through and/or. */
            int fcc = NCC_E, combine = 0;
            switch (op) {
            case HOP_EQ: fcc = NCC_E;  combine = 0; break;
            case HOP_NE: fcc = NCC_NE; combine = 1; break;
            case HOP_LT: fcc = NCC_B;  combine = 0; break;
            case HOP_LE: fcc = NCC_BE; combine = 0; break;
            case HOP_GT: fcc = NCC_A;  combine = 0; break;
            case HOP_GE: fcc = NCC_AE; combine = 0; break;
            default:
                nerr(s, n, "unsupported real comparison");
                break;
            }
            nx_movq_xr(b, 0, NRAX);
            nx_movq_xr(b, 1, NRCX);
            nx_comisd(b);
            nx_mov_ri(b, NRAX, 0);
            nx_mov_ri(b, NRCX, 0);
            nx_b(b, 0x0F); nx_b(b, 0x9B); nx_b(b, 0xC1); /* setnp cl */
            nx_setcc(b, NRAX, fcc);
            if (combine) nx_or_rr(b, NRAX, NRCX);
            else         nx_and_rr(b, NRAX, NRCX);
            nx_b(b, 0x0F); nx_b(b, 0xB6); nx_b(b, 0xC0);
            break;
        }
        switch (op) {
        case HOP_ADD: {
                /* pointer + int scales the offset by the element size */
                const HType *lt = n->child[0]->type, *rt = n->child[1]->type;
                long esz = 0;
                int scale_left = 0;
                if (lt && lt->kind == HTY_PTR) esz = ncell_bytes(lt->u.ptr.elem);
                else if (rt && rt->kind == HTY_PTR) { esz = ncell_bytes(rt->u.ptr.elem); scale_left = 1; }
                /* after the pops rcx = left, rax = right: scale the
                 * integer operand, never the pointer */
                if (esz > 1) {
                    nx_mov_ri(b, NRDX, esz);
                    nx_imul_rr(b, scale_left ? NRAX : NRCX, NRDX);
                }
                nx_add_rr(b, NRAX, NRCX); break;
            }
            case HOP_SUB: {
                const HType *lt = n->child[0]->type;
                const HType *rt = n->child[1]->type;
                if (lt && lt->kind == HTY_PTR &&
                    rt && rt->kind == HTY_PTR) {
                    /* Pointer difference: the byte distance divided
                     * by the element size (1 when unknown). The
                     * scaling applies to the distance, never to one
                     * of the operands. */
                    long esz = ncell_bytes(lt->u.ptr.elem);
                    nx_sub_rr(b, NRAX, NRCX);
                    if (esz > 1) {
                        nx_mov_ri(b, NRCX, esz);
                        nx_cqo(b);
                        nx_idiv_r(b, NRCX);
                    }
                    break;
                }
                long esz = (lt && lt->kind == HTY_PTR)
                               ? ncell_bytes(lt->u.ptr.elem) : 0;
                if (esz > 1) { nx_mov_ri(b, NRDX, esz); nx_imul_rr(b, NRCX, NRDX); }
                nx_sub_rr(b, NRAX, NRCX); break;
            }
        case HOP_MUL: nx_imul_rr(b, NRAX, NRCX); break;
        case HOP_DIV: case HOP_MOD:
            nx_cqo(b);
            nx_idiv_r(b, NRCX);
            if (op == HOP_MOD) nx_mov_rr(b, NRAX, NRDX);
            break;
        case HOP_AND: nx_and_rr(b, NRAX, NRCX); break;
        case HOP_OR:  nx_or_rr(b, NRAX, NRCX);  break;
        case HOP_XOR: nx_xor_rr(b, NRAX, NRCX); break;
        case HOP_SHL: nx_shl_cl(b, NRAX); break;
        case HOP_SHR: nx_sar_cl(b, NRAX); break;
        case HOP_USHR: nx_shr_cl(b, NRAX); break;
        case HOP_EQ: case HOP_NE: case HOP_LT:
        case HOP_LE: case HOP_GT: case HOP_GE:
            /* setcc writes AL only; zero-extend it afterwards. A
             * zeroing before the comparison would clobber the left
             * operand sitting in RAX and destroy the flags. */
            nx_cmp_rr(b, NRAX, NRCX);
            nx_setcc(b, NRAX, ncc_of(op));
            /* movzx eax, al: the conditional set wrote AL only. */
            nx_b(b, 0x0F); nx_b(b, 0xB6); nx_b(b, 0xC0);
            break;
        default:
            nerr(s, n, "unsupported binary operator");
            break;
        }
        break;
    }
    case HK_UNOP:
        switch (n->u.unop) {
        case HUN_NEG:
            nexpr(s, n->child[0]);
            if (n->child[0]->type &&
                n->child[0]->type->kind == HTY_REAL) {
                /* Flipping the IEEE-754 sign bit negates the value;
                 * this is exact for every bit pattern, NaN kept. */
                nx_mov_ri(b, NRCX, 0x8000000000000000ULL);
                nx_xor_rr(b, NRAX, NRCX);
            } else {
                nx_neg_r(b, NRAX);
            }
            break;
        case HUN_BITNOT:
            nexpr(s, n->child[0]);
            nx_not_r(b, NRAX);
            break;
        case HUN_LNOT:
            nexpr(s, n->child[0]);
            nx_test_rr(b, NRAX, NRAX);
            nx_mov_ri(b, NRAX, 0);
            nx_setcc(b, NRAX, NCC_E);
            break;
        case HOP_ADDR_OF:
            nlval(s, n->child[0]);
            break;
        case HUN_DEREF: {
            /* Read through a pointer: evaluate it, then load by the
             * pointee width (a Char loads a single byte). The typing
             * pass may leave the node untyped; take the width from
             * the pointer operand then. */
            /* The load width is the pointee's: a fused 'as' cast may
             * have widened this node's type, but the memory cell is
             * still one element wide. */
            const HType *t = (n->child[0]->type &&
                              n->child[0]->type->kind == HTY_PTR)
                             ? n->child[0]->type->u.ptr.elem
                             : n->type;
            nexpr(s, n->child[0]);
            nload_typed(s, b, t);
            break;
        }
        default:
            nerr(s, n, "unsupported unary operator");
            break;
        }
        break;
    case HK_FIELD: {
        /* A view's len/ptr are the two halves of the fat pair the
         * view value addresses. */
        const HType *vt = n->child[0]->type;
        if (vt && vt->kind == HTY_VIEW) {
            nexpr(s, n->child[0]);
            if (strcmp(n->name, "len") == 0)
                nx_mov_rm(b, NRAX, NRAX, 8);
            else if (strcmp(n->name, "ptr") == 0)
                nx_mov_rm(b, NRAX, NRAX, 0);
            else
                nerr(s, n, "views only have 'len' and 'ptr' members");
            break;
        }
        /* Read a struct field: address of the base value plus the
         * field offset, then a load. */
        const HType *bt = n->child[0]->type;
        const char *sn = (bt && bt->kind == HTY_NAMED) ? bt->u.named.name : NULL;
        long off = nfield_off(s, sn ? sn : "", n->u.index);
        nlval(s, n->child[0]);
        /* A struct literal yields a pointer, so a variable of a
         * named struct type holds that pointer in its slot; a
         * literal used directly is already the data address. */
        if ((n->child[0]->kind == HK_LOCAL ||
             n->child[0]->kind == HK_PARAM) &&
            bt && bt->kind == HTY_NAMED)
            nx_mov_rm(b, NRAX, NRAX, 0);   /* slot holds the pointer */
        if (off) { nx_mov_ri(b, NRDX, off); nx_add_rr(b, NRAX, NRDX); }
        /* load exactly the field width */
        if (n->type && (n->type->kind == HTY_INT ||
                        n->type->kind == HTY_UINT) &&
            hty_size_bytes(n->type) == 4)
            nx_movsxd_rm(b, NRAX, NRAX, 0);
        else
            nload_typed(s, b, n->type);
        break;
    }
    case HK_CAST: {
        int tk = n->type ? (int)n->type->kind : -1;
        if (tk == HTY_INT || tk == HTY_UINT || tk == HTY_BOOL ||
            tk == HTY_PTR || tk == HTY_CHAR || tk == HTY_REAL ||
            tk == HTY_VOID || tk == HTY_ARRAY) {
            /* An array cast is a view over the same address; the
             * value in RAX is the pointer either way. */
            /* Reinterpretations between register-width values are
             * no-ops at machine level, except that a narrow target
             * must bring the value to its width with correct sign
             * extension so full-register consumers see the
             * truncated value. */
            nexpr(s, n->child[0]);
            const HType *srct = n->child[0]->type;
            if (tk == HTY_REAL &&
                !(srct && srct->kind == HTY_REAL)) {
                /* Integer to double conversion. */
                nx_cvtsi2sd_0r(b, NRAX);
                nx_movq_rx(b, NRAX, 0);
                if (n->type->width == 32) {
                    nx_movq_xr(b, 0, NRAX);
                    nx_b(b, 0xF2); nx_b(b, 0x0F); nx_b(b, 0x5A); nx_b(b, 0xC0);
                    nx_b(b, 0xF3); nx_b(b, 0x0F); nx_b(b, 0x5A); nx_b(b, 0xC0);
                    nx_movq_rx(b, NRAX, 0);
                }
            } else if (tk == HTY_REAL && n->type->width == 32 &&
                       srct && srct->kind == HTY_REAL) {
                /* Real64 -> Real32 is rounded to binary32, then
                 * widened for the native expression-register ABI. */
                nx_movq_xr(b, 0, NRAX);
                nx_b(b, 0xF2); nx_b(b, 0x0F); nx_b(b, 0x5A); nx_b(b, 0xC0);
                nx_b(b, 0xF3); nx_b(b, 0x0F); nx_b(b, 0x5A); nx_b(b, 0xC0);
                nx_movq_rx(b, NRAX, 0);
            } else if ((tk == HTY_INT || tk == HTY_UINT) &&
                       srct && srct->kind == HTY_REAL) {
                /* Double to integer conversion, truncating towards
                 * zero like the language cast semantics. */
                nx_movq_xr(b, 0, NRAX);
                nx_cvttsd2si_r0(b, NRAX);
            } else if (tk != HTY_PTR && tk != HTY_ARRAY && tk != HTY_VOID) {
                long sz = hty_size_bytes(n->type);
                int sgn = tk == HTY_INT || tk == HTY_CHAR;
                if (tk == HTY_UINT) sgn = 0;
                if (tk == HTY_BOOL) sgn = 0;
                if (sgn) {
                    if (sz == 1) { nx_b(b,0x48); nx_b(b,0x0F); nx_b(b,0xBE); nx_b(b,0xC0); }
                    else if (sz == 2) { nx_b(b,0x48); nx_b(b,0x0F); nx_b(b,0xBF); nx_b(b,0xC0); }
                    else if (sz == 4) { nx_b(b,0x48); nx_b(b,0x63); nx_b(b,0xC0); }
                } else {
                    if (sz == 1) { nx_b(b,0x0F); nx_b(b,0xB6); nx_b(b,0xC0); }
                    else if (sz == 2) { nx_b(b,0x0F); nx_b(b,0xB7); nx_b(b,0xC0); }
                    else if (sz == 4 && tk == HTY_UINT) { nx_b(b,0x89); nx_b(b,0xC0); }
                }
            }
        } else {
            nerr(s, n, "unsupported cast target");
        }
        break;
    }
    case HK_CALL: {
        const HNode *fn = n->child[0];
        if (fn->kind != HK_GLOBAL_REF) {
            nerr(s, n, "calls through non-global references");
            break;
        }
        int lbl = -1;
        for (int i = 0; i < s->nfn; i++)
            if (strcmp(s->fn[i].name, fn->name) == 0) { lbl = s->fn[i].lbl; break; }
        if (lbl < 0) {
            char what[192];
            snprintf(what, sizeof(what),
                     "call to an unknown function '%s'", fn->name);
            nerr(s, n, what);
            break;
        }
        static const NXReg argreg[6] = { NRDI, NRSI, NRDX, NRCX, NR8, NR9 };
        int stk = n->nitems > 6 ? n->nitems - 6 : 0;
        int pad = stk ? stk : n->nitems;
        /* Stack arguments must keep their [rsp]-relative slots, so the
         * pad goes before the pushes when they exist. Without stack
         * arguments the pad goes after the register pops: an argument
         * that materialised at [rsp] (an interpolated string buffer)
         * then sits ABOVE the call, outside the callee's incoming
         * parameter-save area. */
        if (stk > 0 && (pad & 1)) {
            nx_sub_ri(b, NRSP, 8);
        }
        for (int i = 0; i < n->nitems; i++) {
            const HNode *a = n->items[i];
            nexpr(s, a);
            /* A scalar bound to a Char[] parameter (flagged by the
             * typing pass) is formatted into frame scratch; the
             * pointer replaces the value in RAX. */
            if (a->flags & HF_FSTR)
                nscalar_str(s, n);
            nx_push(b, NRAX);
        }
        /* arguments beyond the sixth stay on the stack as stack
         * arguments (the System V ABI); they are already in the right
         * order: the 7th argument sits at [rsp] after the register
         * pops. */
        int nreg = n->nitems < 6 ? n->nitems : 6;
        for (int i = nreg - 1; i >= 0; i--)
            nx_pop(b, argreg[i]);
        if (stk == 0 && (pad & 1)) {
            nx_sub_ri(b, NRSP, 8);
        }
        nx_call(b, lbl);
        /* drop the alignment pad and any remaining stack arguments */
        if (pad & 1) {
            nx_add_ri(b, NRSP, 8 + 8 * stk);
        }
        /* Error protocol: a callee returning an error type reports
         * the code in RDX (0 on success). Route failures to the
         * current error target: the epilogue, or a catch handler.
         * The node's own type is already the unwrapped payload, so
         * the callee's declared return type decides. */
        if (fn->type && fn->type->kind == HTY_ERROR) {
            int ok = nx_label(b);
            nx_test_rr(b, NRDX, NRDX);
            nx_jcc(b, NCC_E, ok);
            nx_jmp(b, s->err_lbl);
            nx_def(b, ok);
        }
        break;
    }
    case HK_BLOCK:
        /* A block in value position: run its statements; the last
         * expression leaves the value in RAX. */
        nstmt(s, n);
        break;
    case HK_TRY:
        /* The operand's calls already propagate errors to the
         * current target; on success RDX is 0 and RAX the value. */
        nexpr(s, n->child[0]);
        break;
    case HK_CATCH: {
        /* On error the operand's calls jump to HANDLER with the code
         * in RDX; the handler binds it and runs, then falls through
         * to END. On success control skips the handler. */
        int end = nx_label(b), hnd = nx_label(b), saved = s->err_lbl;
        s->err_lbl = hnd;
        nexpr(s, n->child[0]);
        s->err_lbl = saved;
        nx_jmp(b, end);
        nx_def(b, hnd);
        if (n->name)
            nx_mov_mr(b, NRBP, s->loff[n->u.index], NRDX);
        /* RDX carries the caught error code at handler entry. Once it
         * has been copied into the binding, clear the return channel so
         * calls made by the handler start with the success sentinel. */
        nx_mov_ri(b, NRDX, 0);
        nstmt(s, n->child[1]);
        nx_def(b, end);
        break;
    }
    case HK_IF: {
        /* if-expression: both arms produce their value in RAX. A
         * folded constant condition selects one arm at emit time,
         * so dead branches never reach the binary. */
        const HNode *c = n->child[0];
        if (c->kind == HK_CONST_INT) {
            nstmt(s, c->u.ival ? n->child[1] : n->child[2]);
            break;
        }
        int lelse = nx_label(b), lend = nx_label(b);
        nexpr(s, n->child[0]);
        nx_test_rr(b, NRAX, NRAX);
        nx_jcc(b, NCC_E, lelse);
        nstmt(s, n->child[1]);
        nx_jmp(b, lend);
        nx_def(b, lelse);
        nstmt(s, n->child[2]);
        nx_def(b, lend);
        break;
    }
    case HK_STRUCT_LIT: {
        /* Build the struct in the frame scratch area and hand back
         * its address; field positions were resolved by the typing
         * pass (each item carries u.index). */
        const HType *bt = n->type;
        const char *sn = (bt && bt->kind == HTY_NAMED) ? bt->u.named.name : NULL;
        long total = (bt && hty_size_bytes(bt) > 0)
                     ? hty_size_bytes(bt)
                     : 8 * (n->nitems ? n->nitems : 1);
        long base = s->scratch_base + s->scratch_used;
        s->scratch_used += (total + 7) & ~7L;
        if (s->scratch_used > 4096) {
            nerr(s, n, "too many live struct literals (scratch exhausted)");
            break;
        }
        /* rbp - scratch_base - 4096 + base .. +total is this area. */
        nx_lea(b, NRAX, NRBP, -(int32_t)(s->scratch_base + 4096 - base));
        nx_mov_rr(b, NR10, NRAX);            /* base kept in r10 */
        nx_mov_ri(b, NRAX, 0);
        for (long z = 0; z < total; z += 8)
            nx_mov_mr(b, NR10, (int32_t)z, NRAX);
        for (int k = 0; k < n->nitems; k++) {
            const HNode *it = n->items[k];
            if (!it) continue;   /* gap left by named binding: zeroed */
            long off = nfield_off(s, sn ? sn : "", k);
            const HType *ft = NULL;
            const HNode *decl = nfind_struct(s, sn ? sn : "");
            if (decl && k < decl->nitems && decl->items[k])
                ft = decl->items[k]->type;
            nexpr(s, it);
            if (ft && ft->kind == HTY_REAL && ft->width == 32) {
                nstore_convert_mem(s, ft, it->type);
                nx_mov32_mr(b, NR10, (int32_t)off, NRAX);
            } else {
                nx_mov_mr(b, NR10, (int32_t)off, NRAX);
            }
        }
        nx_mov_rr(b, NRAX, NR10);
        break;
    }
    case HK_SLICE: {
        /* Materialize the fat pair {ptr, len} in frame scratch and
         * yield its address. Bounds are element indices: a closed
         * '..' includes its upper bound, '..<' excludes it, and an
         * open bound runs to the end of the base. */
        NXBuf *b = s->text;
        const HType *bt = n->child[0]->type;
        int base_view = bt && bt->kind == HTY_VIEW;
        nexpr(s, n->child[0]);
        if (base_view) {
            nx_mov_rm(b, NRDI, NRAX, 0);    /* data pointer           */
            nx_mov_rm(b, NR8, NRAX, 8);     /* element count          */
        } else {
            nx_mov_rr(b, NRDI, NRAX);       /* first element address  */
            long count = (bt && bt->kind == HTY_ARRAY)
                             ? bt->u.array.count : 0;
            nx_mov_ri(b, NR8, count);
        }
        if (n->child[1]) {                  /* start -> rcx           */
            nexpr(s, n->child[1]);
            nx_mov_rr(b, NRCX, NRAX);
        } else {
            nx_mov_ri(b, NRCX, 0);
        }
        if (n->child[2]) {                  /* end-exclusive -> r8    */
            nexpr(s, n->child[2]);
            nx_mov_rr(b, NR8, NRAX);
            if (!(n->flags & HF_EXCLUSIVE))
                nx_inc_r(b, NR8);
        }
        /* ptr = base + start * cell: arrays and views use 8-byte
         * cells; a pointer base walks the pointee's natural size. */
        long cell = 8;
        if (bt && bt->kind == HTY_PTR) cell = ncell_bytes(bt->u.ptr.elem);
        if (cell == 1) {
            nx_mov_rr(b, NRAX, NRDI);
            nx_add_rr(b, NRAX, NRCX);
        } else {
            int scale = cell == 2 ? 1 : cell == 4 ? 2 : 3;
            nx_b(b, 0x48); nx_b(b, 0x8D);   /* lea rax, [rdi+rcx*scale] */
            nx_b(b, 0x04);
            nx_b(b, (uint8_t)((scale << 6) | (1 << 3) | 7));
        }
        nx_sub_rr(b, NR8, NRCX);            /* len = end_excl - start */
        long base = s->scratch_base + s->scratch_used;
        s->scratch_used += 16;
        if (s->scratch_used > 4096) {
            nerr(s, n, "too many live views (scratch exhausted)");
            nx_mov_ri(b, NRAX, 0);
            break;
        }
        int32_t a_off = -(int32_t)(s->scratch_base + 4096 - base);
        nx_lea(b, NR10, NRBP, a_off);       /* pair base in r10          */
        nx_mov_mr(b, NR10, 0, NRAX);        /* pair.ptr                  */
        nx_mov_rr(b, NRAX, NR8);
        nx_mov_mr(b, NR10, 8, NRAX);        /* pair.len                  */
        nx_lea(b, NRAX, NRBP, a_off);       /* yield the handle          */
        break;
    }
    case HK_ARRAY_LIT:
        /* A tuple type builds one 8-byte slot per element in frame
         * scratch and yields the pointer; homogeneous arrays are
         * materialised by the let-init special case. */
        if (n->type && n->type->kind == HTY_TUPLE) {
            int nt = n->nitems;
            long base = s->scratch_base + s->scratch_used;
            s->scratch_used += (long)(nt > 0 ? nt : 1) * 8;
            if (s->scratch_used > 4096) {
                nerr(s, n, "tuple literal scratch exhausted");
                nx_mov_ri(b, NRAX, 0);
                break;
            }
            int32_t a_off = -(int32_t)(s->scratch_base + 4096 - base);
            nx_lea(b, NR10, NRBP, a_off);
            for (int k = 0; k < nt; k++) {
                nexpr(s, n->items[k]);
                nx_mov_mr(b, NR10, k * 8, NRAX);
            }
            nx_lea(b, NRAX, NRBP, a_off);
            break;
        }
        nerr(s, n, "array literals are supported only as let initialisers");
        nx_mov_ri(b, NRAX, 0);
        break;
    case HK_INDEX: {
        /* Read one element: the width comes from the element type,
         * falling back to the indexed pointer's pointee. A Char
         * element is a single byte, zero-extended. */
        const HType *t = (n->child[0]->type &&
                          n->child[0]->type->kind == HTY_PTR)
                         ? n->child[0]->type->u.ptr.elem
                         : n->type;
        naddr_index(s, n);
        nload_typed(s, b, t);
        break;
    }
    default:
        nerr(s, n, "this expression has no native form yet");
        nx_mov_ri(b, NRAX, 0);
        break;
    }
}

void nlval(NSel *s, const HNode *n) {
    NXBuf *b = s->text;
    uint32_t goff;
    if (!n) { nx_mov_ri(b, NRAX, 0); return; }
    switch (n->kind) {
    case HK_LOCAL:
        nx_lea(b, NRAX, NRBP, nslot_off(s, n->u.index));
        break;
    case HK_PARAM:
        nx_lea(b, NRAX, NRBP, nparam_off(n->u.index));
        break;
    case HK_GLOBAL_REF:
        if (nglobal(s, n, &goff)) {
            nx_mov_ri(b, NRAX, 0);
            nabs(s, NSECT_DATA, goff);
        } else nx_mov_ri(b, NRAX, 0);
        break;
    case HK_INDEX:
        naddr_index(s, n);
        break;
    case HK_DEREF: {
        /* Read through a pointer by the pointee width; the typing
         * pass may leave the node untyped, so the pointer operand
         * tells the width too. */
        const HType *t = (n->child[0]->type &&
                          n->child[0]->type->kind == HTY_PTR)
                         ? n->child[0]->type->u.ptr.elem
                         : n->type;
        nexpr(s, n->child[0]);
        nload_typed(s, b, t);
        break;
    }
        case HK_UNOP:
        /* Storing through a pointer: the pointer value is the
         * address. */
        if (n->u.unop != HUN_DEREF) {
            nerr(s, n, "this unary form has no address");
            break;
        }
        nexpr(s, n->child[0]);
        break;
    case HK_FIELD: {
        const HType *bt = n->child[0]->type;
        const char *sn = (bt && bt->kind == HTY_NAMED) ? bt->u.named.name : NULL;
        long off = nfield_off(s, sn ? sn : "", n->u.index);
        nlval(s, n->child[0]);
        /* A named-typed local or parameter holds the address of the
         * scratch object in its slot, exactly like the read path:
         * the l-value address is the loaded pointer, not the slot. */
        if ((n->child[0]->kind == HK_LOCAL ||
             n->child[0]->kind == HK_PARAM) &&
            bt && bt->kind == HTY_NAMED)
            nx_mov_rm(b, NRAX, NRAX, 0);
        if (off) { nx_mov_ri(b, NRDX, off); nx_add_rr(b, NRAX, NRDX); }
        break;
    }
    case HK_CAST:
        /* A reinterpret between pointer-width types does not move
         * the storage. */
        nlval(s, n->child[0]);
        break;
default:
        nerr(s, n, "this l-value has no address form yet");
        nx_mov_ri(b, NRAX, 0);
        break;
    }
}
