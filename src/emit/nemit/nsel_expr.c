#include "common.h"
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
static void nwrite_lit(NSel *s, NXReg bufreg, uint64_t len) {
    NXBuf *b = s->text;
    nx_mov_ri(b, NRAX, 1);          /* SYS_write */
    nx_mov_ri(b, NRDI, 1);          /* fd 1      */
    nx_mov_rr(b, NRSI, bufreg);
    nx_mov_ri(b, NRDX, len);
    nx_syscall(b);
}

/* Converts the signed value in rax to decimal and writes it to fd 1.
 * The digit buffer lives in the x86-64 red zone below rsp, which the
 * kernel never touches across a syscall; labels are local to this
 * emission block and resolved by the shared fixup pass. */
static void nitoa_write(NSel *s) {
    NXBuf *b = s->text;
    int labs = nx_label(b), ldig = nx_label(b), lwr = nx_label(b);
    nx_test_rr(b, NRAX, NRAX);
    nx_jcc(b, NCC_NS, labs);
    nx_neg_r(b, NRAX);
    nx_mov_ri(b, NR8, 1);
    nx_def(b, labs);
    nx_mov_rr(b, NRSI, NRSP);
    nx_mov_ri(b, NR10, 8);
    nx_sub_rr(b, NRSI, NR10);
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
    nx_mov_rr(b, NRDX, NRSP);
    nx_mov_ri(b, NR10, 8);
    nx_sub_rr(b, NRDX, NR10);
    nx_sub_rr(b, NRDX, NRSI);
    nwrite_lit(s, NRSI, 0);   /* rdx already holds the length */
}

void nexpr(NSel *s, const HNode *n) {
    NXBuf *b = s->text;
    uint32_t goff;
    if (!n) { nx_mov_ri(b, NRAX, 0); return; }
    switch (n->kind) {
    case HK_CONST_INT:
        nx_mov_ri(b, NRAX, (uint64_t)n->u.ival);
        break;
    case HK_CONST_STRING:
    case HK_CONST_CHAR: {
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
        /* One write per literal chunk, one decimal conversion per
         * expression part. */
        for (int i = 0; i < n->nitems; i++) {
            const HNode *p = n->items[i];
            if (p->kind == HK_CONST_STRING) {
                if (p->u.slice.len <= 0) continue;
                uint32_t off = nro_intern(s, p->u.slice.start,
                                          (size_t)p->u.slice.len);
                nx_mov_ri(b, NRSI, 0);
                nabs(s, NSECT_RO, off);
                nwrite_lit(s, NRSI, (uint64_t)p->u.slice.len);
            } else {
                nexpr(s, p);
                nitoa_write(s);
            }
        }
        nx_mov_ri(b, NRAX, 0);
        break;
    }
    case HK_PARAM:
        nx_mov_rm(b, NRAX, NRBP, nparam_off(n->u.index));
        break;
    case HK_LOCAL:
        if (n->type && n->type->kind == HTY_ARRAY)
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
        if (op == HOP_LAND || op == HOP_LOR) {
            int lshort = nx_label(b), ldone = nx_label(b);
            nexpr(s, n->child[0]);
            nx_test_rr(b, NRAX, NRAX);
            nx_jcc(b, op == HOP_LAND ? NCC_E : NCC_NE, lshort);
            nexpr(s, n->child[1]);
            nx_test_rr(b, NRAX, NRAX);
            nx_jcc(b, op == HOP_LAND ? NCC_E : NCC_NE, lshort);
            nx_mov_ri(b, NRAX, 1);
            nx_jmp(b, ldone);
            nx_def(b, lshort);
            nx_mov_ri(b, NRAX, 0);
            nx_def(b, ldone);
            break;
        }
        nexpr(s, n->child[0]);
        nx_push(b, NRAX);
        nexpr(s, n->child[1]);
        nx_mov_rr(b, NRCX, NRAX);
        nx_pop(b, NRAX);
        switch (op) {
        case HOP_ADD: nx_add_rr(b, NRAX, NRCX); break;
        case HOP_SUB: nx_sub_rr(b, NRAX, NRCX); break;
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
            nx_cmp_rr(b, NRAX, NRCX);
            nx_xor_rr(b, NRAX, NRAX);
            nx_setcc(b, NRAX, ncc_of(op));
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
            nx_neg_r(b, NRAX);
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
        default:
            nerr(s, n, "unsupported unary operator");
            break;
        }
        break;
    case HK_CAST:
        if (n->type &&
            (n->type->kind == HTY_INT || n->type->kind == HTY_UINT ||
             n->type->kind == HTY_BOOL || n->type->kind == HTY_PTR ||
             n->type->kind == HTY_CHAR)) {
            nexpr(s, n->child[0]);
        } else {
            nerr(s, n, "unsupported cast target");
        }
        break;
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
        if (n->nitems > 6) {
            nerr(s, n, "calls with more than 6 arguments");
            break;
        }
        for (int i = 0; i < n->nitems; i++) {
            nexpr(s, n->items[i]);
            nx_push(b, NRAX);
        }
        for (int i = n->nitems - 1; i >= 0; i--)
            nx_pop(b, argreg[i]);
        if (n->nitems & 1) {
            nx_mov_ri(b, NR10, 8);
            nx_sub_rr(b, NRSP, NR10);
        }
        nx_call(b, lbl);
        if (n->nitems & 1) {
            nx_mov_ri(b, NR10, 8);
            nx_add_rr(b, NRSP, NR10);
        }
        break;
    }
    case HK_ARRAY_LIT:
        nerr(s, n, "array literals are supported only as let initialisers");
        nx_mov_ri(b, NRAX, 0);
        break;
    case HK_INDEX:
        naddr_index(s, n);
        nx_mov_rm(b, NRAX, NRAX, 0);
        break;
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
    case HK_DEREF:
        nexpr(s, n->child[0]);
        break;
    default:
        nerr(s, n, "this l-value has no address form yet");
        nx_mov_ri(b, NRAX, 0);
        break;
    }
}
