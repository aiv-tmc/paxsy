#include "common.h"
#include "nsel.h"

/*
 * nsel_stmt.c - instruction selection for statements. Control flow
 * maps directly onto conditional and unconditional jumps; range loops
 * keep their upper bound in a reserved stack slot so it is evaluated
 * exactly once. Array literals as let initialisers store one word per
 * element into the slot inline. Match parks the subject on the stack
 * and compares it against every case pattern in order; the first
 * equal pattern enters the case body (the subject is reloaded with a
 * pop/push pair because an RSP-based ModRM load is not encodable).
 * Blocks run their defer items at the closing point. Unlabeled
 * break/continue target the innermost enclosing loop. An inline-asm
 * statement is handed to the built-in assembler. A T! function
 * propagates a callee's error code by jumping to the shared epilogue
 * with rdx untouched; success paths zero rdx before the jump.
 */

static int ncur_is_err(NSel *s) {
    return s->cur_func && s->cur_func->type &&
           s->cur_func->type->kind == HTY_ERROR;
}

static void ntry_call(NSel *s, const HNode *call) {
    NXBuf *b = s->text;
    int lok = nx_label(b);
    nexpr(s, call);
    nx_test_rr(b, NRDX, NRDX);
    nx_jcc(b, NCC_E, lok);
    nx_jmp(b, s->ep);
    nx_def(b, lok);
}

static void nret_success(NSel *s) {
    NXBuf *b = s->text;
    if (ncur_is_err(s)) nx_mov_ri(b, NRDX, 0);
    nx_jmp(b, s->ep);
}

void nstmt(NSel *s, const HNode *n) {
    NXBuf *b = s->text;
    if (!n) return;
    switch (n->kind) {
    case HK_BLOCK:
        for (int i = 0; i < n->nitems; i++) {
            if (n->items[i]->kind == HK_DEFER) continue;
            nstmt(s, n->items[i]);
        }
        for (int i = 0; i < n->nitems; i++) {
            if (n->items[i]->kind == HK_DEFER && n->items[i]->child[0])
                nstmt(s, n->items[i]->child[0]);
        }
        break;
    case HK_LET: {
        const HNode *init = n->child[1];
        int off = nslot_off(s, n->u.index);
        if (init && init->kind == HK_TRY) {
            const HNode *call = init->child[0];
            int lok = nx_label(b);
            nexpr(s, call);
            nx_test_rr(b, NRDX, NRDX);
            nx_jcc(b, NCC_E, lok);
            nx_jmp(b, s->ep);
            nx_def(b, lok);
            nx_mov_mr(b, NRBP, off, NRAX);
            break;
        }
        if (init && init->kind == HK_ARRAY_LIT) {
            for (int i = 0; i < init->nitems; i++) {
                nexpr(s, init->items[i]);
                nx_mov_mr(b, NRBP, off + 8 * i, NRAX);
            }
            break;
        }
        if (init) nexpr(s, init);
        else nx_mov_ri(b, NRAX, 0);
        nx_mov_mr(b, NRBP, off, NRAX);
        break;
    }
    case HK_STORE:
    case HK_EXPR_STMT: {
        const HNode *e = n->kind == HK_EXPR_STMT ? n->child[0] : n;
        const HNode *lv = n->kind == HK_EXPR_STMT ? e->child[0] : n->child[0];
        const HNode *rv = n->kind == HK_EXPR_STMT ? e->child[1] : n->child[1];
        if (n->kind == HK_EXPR_STMT && !e) break;
        if (n->kind == HK_EXPR_STMT && e->kind == HK_STORE) {
            nlval(s, lv);
            nx_push(b, NRAX);
            nexpr(s, rv);
            nx_mov_rr(b, NRCX, NRAX);
            nx_pop(b, NRAX);
            nx_mov_mr(b, NRAX, 0, NRCX);
            break;
        }
        if (n->kind == HK_EXPR_STMT && e->kind == HK_TRY) {
            ntry_call(s, e->child[0]);
            break;
        }
        if (n->kind == HK_EXPR_STMT) nexpr(s, e);
        else {
            nlval(s, lv);
            nx_push(b, NRAX);
            nexpr(s, rv);
            nx_mov_rr(b, NRCX, NRAX);
            nx_pop(b, NRAX);
            nx_mov_mr(b, NRAX, 0, NRCX);
        }
        break;
    }
    case HK_IF: {
        int lelse = nx_label(b), ldone = nx_label(b);
        nexpr(s, n->child[0]);
        nx_test_rr(b, NRAX, NRAX);
        nx_jcc(b, NCC_E, lelse);
        if (n->child[1]) nstmt(s, n->child[1]);
        nx_jmp(b, ldone);
        nx_def(b, lelse);
        if (n->child[2]) nstmt(s, n->child[2]);
        nx_def(b, ldone);
        break;
    }
    case HK_MATCH: {
        if (n->flags & HF_EXPR) {
            nerr(s, n, "match in value position");
            break;
        }
        int nc = n->nitems;
        int *lb = malloc((size_t)(nc > 0 ? nc : 1) * sizeof(int));
        for (int i = 0; i < nc; i++) lb[i] = nx_label(b);
        int lend = nx_label(b);
        nexpr(s, n->child[0]);
        nx_push(b, NRAX);
        for (int i = 0; i < nc; i++) {
            const HNode *c = n->items[i];
            for (int j = 0; j < c->nitems; j++) {
                nexpr(s, c->items[j]);
                nx_mov_rr(b, NRCX, NRAX);
                nx_pop(b, NRAX);
                nx_push(b, NRAX);
                nx_cmp_rr(b, NRAX, NRCX);
                nx_jcc(b, NCC_E, lb[i]);
            }
        }
        if (n->child[2]) nstmt(s, n->child[2]);
        for (int i = 0; i < nc; i++) {
            nx_def(b, lb[i]);
            nstmt(s, n->items[i]->child[1]);
            nx_jmp(b, lend);
        }
        nx_def(b, lend);
        nx_mov_ri(b, NRAX, 8);
        nx_add_rr(b, NRSP, NRAX);
        free(lb);
        break;
    }
    case HK_WHILE: {
        int ltop = nx_label(b), ldone = nx_label(b);
        if (s->depth < 16) {
            s->brk[s->depth] = ldone;
            s->cont[s->depth] = ltop;
            s->depth++;
        }
        nx_def(b, ltop);
        nexpr(s, n->child[0]);
        nx_test_rr(b, NRAX, NRAX);
        nx_jcc(b, NCC_E, ldone);
        nstmt(s, n->child[1]);
        nx_jmp(b, ltop);
        nx_def(b, ldone);
        if (s->depth <= 16) s->depth--;
        break;
    }
    case HK_DOWHILE: {
        int ltop = nx_label(b);
        nx_def(b, ltop);
        nstmt(s, n->child[0]);
        nexpr(s, n->child[1]);
        nx_test_rr(b, NRAX, NRAX);
        nx_jcc(b, NCC_NE, ltop);
        break;
    }
    case HK_FOR: {
        int ltop = nx_label(b), ldone = nx_label(b);
        int voff = nslot_off(s, n->u.index);
        int toff = nslot_off(s, s->nl);
        if (s->depth < 16) {
            s->brk[s->depth] = ldone;
            s->cont[s->depth] = ltop;
            s->depth++;
        }
        if (n->child[0]) {
            nexpr(s, n->child[0]);
            nx_mov_mr(b, NRBP, voff, NRAX);
        } else {
            nx_mov_ri(b, NRAX, 0);
            nx_mov_mr(b, NRBP, voff, NRAX);
        }
        if (n->child[1]) {
            nexpr(s, n->child[1]);
            nx_mov_mr(b, NRBP, toff, NRAX);
        }
        nx_def(b, ltop);
        if (n->child[1]) {
            nx_mov_rm(b, NRAX, NRBP, voff);
            nx_mov_rm(b, NRCX, NRBP, toff);
            nx_cmp_rr(b, NRAX, NRCX);
            nx_jcc(b, n->flags & HF_EXCLUSIVE ? NCC_G : NCC_GE, ldone);
        }
        nstmt(s, n->child[2]);
        nx_mov_rm(b, NRAX, NRBP, voff);
        nx_inc_r(b, NRAX);
        nx_mov_mr(b, NRBP, voff, NRAX);
        nx_jmp(b, ltop);
        nx_def(b, ldone);
        if (s->depth <= 16) s->depth--;
        break;
    }
    case HK_RETURN: {
        const HNode *v = n->child[0];
        if (!v) {
            if (ncur_is_err(s)) nx_mov_ri(b, NRDX, 0);
            nx_jmp(b, s->ep);
            break;
        }
        if (v->kind == HK_ERR_VAL) {
            if (!v->child[0] || v->child[0]->kind != HK_CONST_INT) {
                nerr(s, v, "error value must be a code literal");
                break;
            }
            nx_mov_ri(b, NRAX, 0);
            nx_mov_ri(b, NRDX, (uint64_t)v->child[0]->u.ival);
            nx_jmp(b, s->ep);
            break;
        }
        if (v->kind == HK_TRY) {
            const HNode *call = v->child[0];
            int lok = nx_label(b);
            nexpr(s, call);
            nx_test_rr(b, NRDX, NRDX);
            nx_jcc(b, NCC_E, lok);
            nx_jmp(b, s->ep);
            nx_def(b, lok);
            if (ncur_is_err(s)) nx_mov_ri(b, NRDX, 0);
            nx_jmp(b, s->ep);
            break;
        }
        nexpr(s, v);
        nret_success(s);
        break;
    }
    case HK_BREAK:
        if (n->name) nerr(s, n, "labeled break is not supported yet");
        else if (s->depth == 0) nerr(s, n, "break outside a loop");
        else nx_jmp(b, s->brk[s->depth - 1]);
        break;
    case HK_CONTINUE:
        if (n->name) nerr(s, n, "labeled continue is not supported yet");
        else if (s->depth == 0) nerr(s, n, "continue outside a loop");
        else nx_jmp(b, s->cont[s->depth - 1]);
        break;
    case HK_INLINE_ASM:
        nasm_stmt(s, n);
        break;
    default:
        nerr(s, n, "this statement has no native form yet");
        break;
    }
}
