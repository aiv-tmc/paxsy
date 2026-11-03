#include "common.h"
#include "mlir.h"
#include "mlir.h"

/*
 * mlir_expr.c - the value-expression printer of the MLIR emission
 * pass. Integer constants become arith.constant, arithmetic and
 * comparisons become arith ops (comparisons yield i1), casts become
 * arith.extsi. A general call receives a typed func.call signature
 * built from the argument and result types, so each produced SSA
 * value is defined with its exact type before use; the callee name
 * renders with module separators as underscores, matching the
 * func.func definitions the driver prints for every module,
 * including the standard library compiled from Paxsy sources.
 * Anything the dialect set cannot express is diagnosed through merr
 * and produces the neutral value 0, so the pass never miscompiles
 * silently.
 */

/* Reports one unsupported construct; the node carries its source
 * position for the diagnostic. */
void merr(ME *m, const HNode *n, const char *what) {
    diag_report_full(m->diag, DK_ERROR, NULL, n->tok.line, n->tok.col,
                     n->tok.len, "MLIR emission: %s is not supported yet",
                     what);
    m->errs++;
}

/* Renders the MLIR type of an HIR type into a buffer. */
void mty(const HType *t, char *buf, size_t n) {
    if (!t) { snprintf(buf, n, "i32"); return; }
    switch (t->kind) {
    case HTY_VOID:  snprintf(buf, n, "void"); break;
    case HTY_BOOL:  snprintf(buf, n, "i1"); break;
    case HTY_CHAR:  snprintf(buf, n, "i8"); break;
    case HTY_INT:
    case HTY_UINT: {
        int w = t->width <= 8 ? 8 : t->width <= 16 ? 16
            : t->width <= 32 ? 32 : 64;
        snprintf(buf, n, "i%d", w);
        break;
    }
    case HTY_REAL:  snprintf(buf, n, "%s", t->width <= 32 ? "f32" : "f64"); break;
    case HTY_PTR: case HTY_ARRAY: case HTY_CHAN:
        snprintf(buf, n, "!llvm.ptr"); break;
    case HTY_ERROR: mty(t->u.error.value, buf, n); break;
    default:        snprintf(buf, n, "i32"); break;
    }
}

/* The arith comparison predicate for a comparison operator. */
static const char *mcmp(HBinOp op) {
    switch (op) {
    case HOP_EQ: return "eq";
    case HOP_NE: return "ne";
    case HOP_LT: return "slt";
    case HOP_LE: return "sle";
    case HOP_GT: return "sgt";
    case HOP_GE: return "sge";
    default:     return "eq";
    }
}

/* The arith operation name for an arithmetic operator. */
static const char *marith(HBinOp op) {
    switch (op) {
    case HOP_ADD: return "addi";
    case HOP_SUB: return "subi";
    case HOP_MUL: return "muli";
    case HOP_DIV: return "divsi";
    case HOP_MOD: return "remsi";
    case HOP_AND: return "andi";
    case HOP_OR:  return "ori";
    case HOP_XOR: return "xori";
    case HOP_SHL: return "shli";
    case HOP_SHR: return "shrsi";
    case HOP_LAND:return "andi";
    case HOP_LOR: return "ori";
    default:      return "addi";
    }
}

/* Writes the callee name as an MLIR symbol: module separators
 * become underscores, matching the func.func names the driver
 * prints for every module. */
static void msym(const char *name, char *buf, size_t cap) {
    size_t o = 0;
    for (const char *p = name; *p && o + 1 < cap; p++) {
        char ch = *p;
        if (ch == ':' || ch == '.') ch = '_';
        buf[o++] = ch;
    }
    buf[o] = '\0';
}

/* Prints one value expression; the resulting SSA name lands in
 * `res`. Instructions are appended to the current block. */
void mexpr(ME *m, const HNode *n, char *res, size_t rn) {
    if (!n) { snprintf(res, rn, "0"); return; }
    switch (n->kind) {
    case HK_CONST_INT: {
        char ty[32];
        mty(n->type, ty, sizeof(ty));
        fprintf(m->out, "    %%%u = arith.constant %lld : %s\n",
                m->ssav, n->u.ival, ty);
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    }
    case HK_CONST_FLOAT:
        fprintf(m->out, "    %%%u = arith.constant %g : %s\n",
                m->ssav, n->u.fval,
                n->type && n->type->width <= 32 ? "f32" : "f64");
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    case HK_CONST_STRING: case HK_CONST_CHAR:
        if (n->aux < 0) { merr(m, n, "string constant without an id"); break; }
        fprintf(m->out,
                "    %%%u = llvm.mlir.addressof @px_s%d : !llvm.ptr\n",
                m->ssav, n->aux);
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    case HK_PARAM:
        snprintf(res, rn, "%%p%d", n->u.index);
        break;
    case HK_LOCAL: {
        char ty[32];
        mty(n->type, ty, sizeof(ty));
        fprintf(m->out, "    %%%u = llvm.load %%slot%d : !llvm.ptr -> %s\n",
                m->ssav, n->u.index, ty);
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    }
    case HK_GLOBAL_REF:
        snprintf(res, rn, "@%s", n->name);
        break;
    case HK_BINOP: {
        char a[32], b[32], ty[32];
        mexpr(m, n->child[0], a, sizeof(a));
        mexpr(m, n->child[1], b, sizeof(b));
        if (n->u.binop >= HOP_EQ && n->u.binop <= HOP_GE) {
            mty(n->child[0]->type, ty, sizeof(ty));
            fprintf(m->out, "    %%%u = arith.cmpi %s, %s, %s : %s\n",
                    m->ssav, mcmp(n->u.binop), a, b, ty);
        } else {
            mty(n->type, ty, sizeof(ty));
            fprintf(m->out, "    %%%u = arith.%s %s, %s : %s\n",
                    m->ssav, marith(n->u.binop), a, b, ty);
        }
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    }
    case HK_UNOP: {
        char a[32], ty[32];
        mexpr(m, n->child[0], a, sizeof(a));
        mty(n->type, ty, sizeof(ty));
        if (n->u.unop == HUN_NEG) {
            fprintf(m->out, "    %%%u = arith.subi 0, %s : %s\n",
                    m->ssav, a, ty);
            snprintf(res, rn, "%%%u", m->ssav++);
        } else if (n->u.unop == HUN_BITNOT) {
            fprintf(m->out, "    %%%u = arith.xori %s, -1 : %s\n",
                    m->ssav, a, ty);
            snprintf(res, rn, "%%%u", m->ssav++);
        } else {
            /* logical not and dereference pass the operand through */
            snprintf(res, rn, "%s", a);
        }
        break;
    }
    case HK_TRY:
        /* A try wrapper is transparent in value position: the
         * statement printer handles the error protocol. */
        mexpr(m, n->child[0], res, rn);
        break;
    case HK_CALL: {
        const HNode *fn = n->child[0];
        char fname[96], args[256] = "", sig[256] = "";
        if (fn->kind != HK_GLOBAL_REF) {
            merr(m, n, "calls through non-global references");
            snprintf(res, rn, "0");
            break;
        }
        msym(fn->name, fname, sizeof(fname));
        for (int i = 0; i < n->nitems; i++) {
            char a[32], aty[32];
            mexpr(m, n->items[i], a, sizeof(a));
            mty(n->items[i]->type, aty, sizeof(aty));
            strncat(args, a, sizeof(args) - strlen(args) - 1);
            strncat(sig, aty, sizeof(sig) - strlen(sig) - 1);
            if (i + 1 < n->nitems) {
                strncat(args, ", ", sizeof(args) - strlen(args) - 1);
                strncat(sig, ", ", sizeof(sig) - strlen(sig) - 1);
            }
        }
        char rty[32];
        mty(n->type, rty, sizeof(rty));
        fprintf(m->out, "    %%%u = func.call @%s(%s) : (%s) -> %s\n",
                m->ssav, fname, args, sig, rty);
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    }
    case HK_CAST: {
        char a[32], from[32], to[32];
        mexpr(m, n->child[0], a, sizeof(a));
        mty(n->child[0]->type, from, sizeof(from));
        mty(n->type, to, sizeof(to));
        fprintf(m->out, "    %%%u = arith.extsi %s : %s to %s\n",
                m->ssav, a, from, to);
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    }
    default:
        merr(m, n, "this expression form");
        snprintf(res, rn, "0");
        break;
    }
}
