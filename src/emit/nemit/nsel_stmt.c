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


/* Binds the innermost pending named label entry to this loop's exit
 * and continue targets. Returns the entry index, or -1 when no
 * named label is waiting for this loop. */
static int nlabel_bind(NSel *s, int lbrk, int lcont) {
    if (s->nnamed > 0 && s->named[s->nnamed - 1].brk == -1) {
        int i = s->nnamed - 1;
        s->named[i].brk = lbrk;
        s->named[i].cont = lcont;
        return i;
    }
    return -1;
}

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
        /* A named-type binding without an initializer still owns its
         * scratch object: point the slot at fresh scratch so that
         * later field stores write through a valid address. */
        if (!init && n->type && n->type->kind == HTY_NAMED) {
            nalloc_named(s, n->type);
            nx_mov_mr(b, NRBP, off, NRAX);
            break;
        }
        /* A tuple literal yields a pointer: the slot stores it as a
         * whole. Homogeneous arrays keep the in-place expansion. */
        if (init && init->kind == HK_ARRAY_LIT &&
            !(init->type && init->type->kind == HTY_TUPLE)) {
            const HType *elem = n->type && n->type->kind == HTY_ARRAY
                                   ? n->type->u.array.elem : NULL;
            long stride = ncell_bytes(elem);
            for (int i = 0; i < init->nitems; i++) {
                const HNode *item = init->items[i];
                nexpr(s, item);
                if (elem && elem->kind == HTY_REAL && elem->width == 32) {
                    nstore_convert_mem(s, elem, item ? item->type : NULL);
                    nx_mov32_mr(b, NRBP, off + (int)(stride * i), NRAX);
                } else {
                    nx_mov_mr(b, NRBP, off + (int)(stride * i), NRAX);
                }
            }
            break;
        }
        if (init) {
            nexpr(s, init);
            nstore_convert(s, n->type, init->type);
        } else nx_mov_ri(b, NRAX, 0);
        nx_mov_mr(b, NRBP, off, NRAX);
        /* error-typed slots hold the px_res pair: the tag word
         * travels in rdx and must be stored beside the value. */
        if (n->type && n->type->kind == HTY_ERROR)
            nx_mov_mr(b, NRBP, off + 8, NRDX);
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
            if (lv->type && lv->type->kind == HTY_REAL &&
                lv->type->width == 32 &&
                lv->kind != HK_LOCAL && lv->kind != HK_PARAM) {
                nstore_convert_mem(s, lv->type, rv->type);
                nx_mov_rr(b, NRCX, NRAX);
                nx_pop(b, NRAX);
                nx_mov32_mr(b, NRAX, 0, NRCX);
            } else {
                nstore_convert(s, lv->type, rv->type);
                nx_mov_rr(b, NRCX, NRAX);
                nx_pop(b, NRAX);
                nx_mov_mr(b, NRAX, 0, NRCX);
            }
            break;
        }
        if (n->kind == HK_EXPR_STMT && e->kind == HK_TRY) {
            ntry_call(s, e->child[0]);
            break;
        }
        if (n->kind == HK_EXPR_STMT && e->kind == HK_ERR_VAL) {
            /* An error expression used as a catch-handler terminator
             * is a non-returning panic path in the native backend. */
            nx_b(b, 0x0f);
            nx_b(b, 0x0b);
            break;
        }
        if (n->kind == HK_EXPR_STMT) nexpr(s, e);
        else {
            nlval(s, lv);
            nx_push(b, NRAX);
            nexpr(s, rv);
            if (lv->type && lv->type->kind == HTY_REAL &&
                lv->type->width == 32 &&
                lv->kind != HK_LOCAL && lv->kind != HK_PARAM) {
                nstore_convert_mem(s, lv->type, rv->type);
                nx_mov_rr(b, NRCX, NRAX);
                nx_pop(b, NRAX);
                nx_mov32_mr(b, NRAX, 0, NRCX);
            } else {
                nstore_convert(s, lv->type, rv->type);
                nx_mov_rr(b, NRCX, NRAX);
                nx_pop(b, NRAX);
                nx_mov_mr(b, NRAX, 0, NRCX);
            }
        }
        break;
    }
    case HK_IF: {
        /* an if carrying a value (value arm / interpolated nested
         * conditional) emits through the expression path so both
         * arms leave their result in RAX */
        if (n->flags & HF_EXPR) {
            nexpr(s, n);
            break;
        }
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
        /* value-position matches are also handled here: every arm
         * block leaves its trailing value in RAX (HF_ARMVAL keeps
         * it), so the shared dispatch below produces the result */
        int nc = n->nitems;
        /* folded constant scrutinee: select the matching arm (or the
         * else block) at emit time; no dispatch code is produced */
        if (n->child[0] && n->child[0]->kind == HK_CONST_INT) {
            long v = n->child[0]->u.ival;
            const HNode *pick = NULL;
            for (int i = 0; i < nc && !pick; i++) {
                const HNode *c = n->items[i];
                for (int j = 0; j < c->nitems; j++)
                    if (c->items[j]->kind == HK_CONST_INT &&
                        c->items[j]->u.ival == v) { pick = c; break; }
            }
            if (!pick) pick = n->child[2];
            if (pick) nstmt(s, pick->child[1] ? pick->child[1] : pick);
            break;
        }
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
        if (n->child[2]) {
            nstmt(s, n->child[2]);
            nx_jmp(b, lend);   /* else must not fall into the arms */
        }
        for (int i = 0; i < nc; i++) {
            nx_def(b, lb[i]);
            nstmt(s, n->items[i]->child[1]);
            nx_jmp(b, lend);
        }
        nx_def(b, lend);
        /* drop the scrutinee copy; R10 keeps RAX free for a
         * value-position match result */
        nx_add_ri(b, NRSP, 8);
        free(lb);
        break;
    }
    case HK_WHILE: {
        int ltop = nx_label(b), ldone = nx_label(b);
        nlabel_bind(s, ldone, ltop);
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
        int ltop = nx_label(b), ldone = nx_label(b);
        nlabel_bind(s, ldone, ltop);
        if (s->depth < 16) {
            s->brk[s->depth] = ldone;
            s->cont[s->depth] = ltop;
            s->depth++;
        }
        nx_def(b, ltop);
        nstmt(s, n->child[0]);
        nexpr(s, n->child[1]);
        nx_test_rr(b, NRAX, NRAX);
        nx_jcc(b, NCC_NE, ltop);
        nx_def(b, ldone);
        if (s->depth <= 16) s->depth--;
        break;
    }
    case HK_FOR: {
        int ltop = nx_label(b), ldone = nx_label(b), lstep = nx_label(b);
        int voff = nslot_off(s, n->u.index);
        int toff = nslot_off(s, s->nl);
        nlabel_bind(s, ldone, lstep);
        if (s->depth < 16) {
            s->brk[s->depth] = ldone;
            s->cont[s->depth] = lstep;
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
            nx_jcc(b, n->flags & HF_INCLUSIVE ? NCC_G : NCC_GE, ldone);
        }
        nstmt(s, n->child[2]);
        nx_def(b, lstep);
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
            long code = -1;
            if (v->child[0] && v->child[0]->kind == HK_CONST_INT) {
                code = v->child[0]->u.ival;
            } else if (v->child[0] && v->child[0]->kind == HK_CAST &&
                       v->child[0]->child[0] &&
                       (v->child[0]->child[0]->kind == HK_LOCAL ||
                        v->child[0]->child[0]->kind == HK_PARAM)) {
                /* 'error (e as Int32)': the cast is a no-op at machine
                 * level; propagate the slot value. */
                nx_mov_rm(b, NRDX, NRBP, s->loff[v->child[0]->child[0]->u.index]);
                nx_mov_ri(b, NRAX, 0);
                nx_jmp(b, s->ep);
                break;
            } else if (v->child[0] &&
                       (v->child[0]->kind == HK_LOCAL ||
                        v->child[0]->kind == HK_PARAM)) {
                /* Propagating a caught (or computed) error value:
                 * load the slot into RDX. */
                nx_mov_rm(b, NRDX, NRBP, s->loff[v->child[0]->u.index]);
                nx_mov_ri(b, NRAX, 0);
                nx_jmp(b, s->ep);
                break;
            } else if (v->child[0] && v->child[0]->kind == HK_GLOBAL_REF) {
                /* A named error code resolves through its constant
                 * global's initializer. */
                for (int i = 0; i < s->ngl; i++)
                    if (strcmp(s->gl[i].name, v->child[0]->name) == 0 &&
                        s->gl[i].node && s->gl[i].node->child[0] &&
                        s->gl[i].node->child[0]->kind == HK_CONST_INT)
                        code = s->gl[i].node->child[0]->u.ival;
            }
            if (code < 0) {
                const HNode *pl = v->child[0];
                if (pl && pl->kind == HK_GLOBAL_REF)
                    for (int i = 0; i < s->ngl; i++)
                        if (strcmp(s->gl[i].name, pl->name) == 0) { }
                nerr(s, v, "error value must be a code literal");
                break;
            }
            nx_mov_ri(b, NRAX, 0);
            nx_mov_ri(b, NRDX, (uint64_t)code);
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
        if (n->name) {
            /* Search the named label stack inside-out: the nearest
             * declaration wins, matching lexical scoping. */
            int i, found = -1;
            for (i = s->nnamed - 1; i >= 0; i--)
                if (s->named[i].name &&
                    strcmp(s->named[i].name, n->name) == 0) {
                    found = i; break;
                }
            if (found < 0) {
                char what[192];
                snprintf(what, sizeof(what),
                         "break refers to unknown label '%s'", n->name);
                nerr(s, n, what);
            } else if (s->named[found].brk == -1) {
                nerr(s, n, "break label does not mark a loop");
            } else {
                nx_jmp(b, s->named[found].brk);
            }
        }
        else if (s->depth == 0) nerr(s, n, "break outside a loop");
        else nx_jmp(b, s->brk[s->depth - 1]);
        break;
    case HK_CONTINUE:
        if (n->name) {
            int i, found = -1;
            for (i = s->nnamed - 1; i >= 0; i--)
                if (s->named[i].name &&
                    strcmp(s->named[i].name, n->name) == 0) {
                    found = i; break;
                }
            if (found < 0) {
                char what[192];
                snprintf(what, sizeof(what),
                         "continue refers to unknown label '%s'", n->name);
                nerr(s, n, what);
            } else if (s->named[found].cont == -1) {
                nerr(s, n, "continue label does not mark a loop");
            } else {
                nx_jmp(b, s->named[found].cont);
            }
        }
        else if (s->depth == 0) nerr(s, n, "continue outside a loop");
        else nx_jmp(b, s->cont[s->depth - 1]);
        break;
    case HK_LABEL: {
        /* A label wraps the loop it names. The entry is pushed with
         * the -1 sentinel; the loop binds it through nlabel_bind so
         * labeled break/continue inside any nesting depth resolve to
         * this loop's exit and step targets. */
        if (s->nnamed >= 16) {
            nerr(s, n, "too many nested loop labels");
            break;
        }
        int top = s->nnamed++;
        s->named[top].name = n->name;
        s->named[top].brk = -1;
        s->named[top].cont = -1;
        nstmt(s, n->child[0]);
        if (s->nnamed > top) s->nnamed = top;
        break;
    }
    case HK_INLINE_ASM:
        nasm_stmt(s, n);
        break;
    default:
        nerr(s, n, "this statement has no native form yet");
        break;
    }
}
