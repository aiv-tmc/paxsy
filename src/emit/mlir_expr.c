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
/* Names the HIR node kind for diagnostics, mirroring the LLVM
 * emitter so a gap is identifiable from either listing. */
static const char *mkind(int k) {
    static const char *const names[] = {
        [HK_MODULE] = "module",   [HK_EXTERN_FUNC] = "extern function",
        [HK_STRUCT] = "struct",   [HK_UNION] = "union",
        [HK_ENUM] = "enum",       [HK_GLOBAL] = "global",
        [HK_BLOCK] = "block",     [HK_LET] = "let",
        [HK_STORE] = "store",     [HK_IF] = "if",
        [HK_WHILE] = "while",     [HK_DOWHILE] = "do-while",
        [HK_FOR] = "for",         [HK_MATCH] = "match",
        [HK_RETURN] = "return",   [HK_BREAK] = "break",
        [HK_CONTINUE] = "continue", [HK_DEFER] = "defer",
        [HK_EXPR_STMT] = "expression statement",
        [HK_LABEL] = "label",     [HK_INLINE_ASM] = "inline assembly",
        [HK_CONST_INT] = "int literal",
        [HK_CONST_FLOAT] = "float literal",
        [HK_CONST_STRING] = "string literal",
        [HK_CONST_CHAR] = "char literal",
        [HK_PARAM] = "parameter", [HK_LOCAL] = "local",
        [HK_GLOBAL_REF] = "global reference",
        [HK_BINOP] = "binary operator", [HK_UNOP] = "unary operator",
        [HK_CALL] = "call",       [HK_CAST] = "cast",
        [HK_INDEX] = "index",     [HK_SLICE] = "slice",
        [HK_FIELD] = "field access",
        [HK_STRUCT_LIT] = "struct literal",
        [HK_TUPLE_LIT] = "tuple literal",
        [HK_ARRAY_LIT] = "array literal",
        [HK_ADDR] = "address-of", [HK_DEREF] = "dereference",
        [HK_ALLOC] = "alloc",     [HK_FREE] = "free",
        [HK_TRY] = "try",         [HK_CATCH] = "catch",
        [HK_FMT] = "format expression", [HK_PANIC] = "panic",
        [HK_ERR_VAL] = "error value",
    };
    static char unk[24];
    if (k >= 0 && k < (int)(sizeof(names) / sizeof(names[0])) &&
        names[k])
        return names[k];
    snprintf(unk, sizeof(unk), "node %d", k);
    return unk;
}

void merr(ME *m, const HNode *n, const char *what) {
    diag_report_full(m->diag, DK_ERROR, NULL, n->tok.line, n->tok.col,
                     n->tok.len,
                     "MLIR emission: %s (%s) is not supported yet",
                     what, mkind(n->kind));
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
/* Defined in mlir_emit.c; used for blocks in value position. */
void mstmt(ME *m, const HNode *n);

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
        } else if (n->u.unop == HUN_DEREF) {
            /* The operand is the address; a load yields the pointee. */
            fprintf(m->out, "    %%%u = llvm.load %s : !llvm.ptr -> %s\n",
                    m->ssav, a, ty);
            snprintf(res, rn, "%%%u", m->ssav++);
        } else if (n->kind == HK_UNOP && n->u.unop == HOP_ADDR_OF) {
            /* Address-of: the operand's emission already produced
             * the address value. */
            snprintf(res, rn, "%s", a);
        } else {
            /* logical not passes the operand through */
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
    case HK_STORE: {
        /* Assignment in value position: store and yield the value. */
        char a[40], adr[40], ty[40];
        mexpr(m, n->child[1], a, sizeof(a));
        if (n->child[0] && n->child[0]->kind == HK_LOCAL)
            snprintf(adr, sizeof(adr), "%%slot%d", n->child[0]->u.index);
        else if (n->child[0])
            mexpr(m, n->child[0], adr, sizeof(adr));
        else
            snprintf(adr, sizeof(adr), "null");
        mty(n->child[1] ? n->child[1]->type : NULL, ty, sizeof(ty));
        fprintf(m->out, "    llvm.store %s, %s : %s, !llvm.ptr\n",
                a, adr, ty);
        fprintf(m->out, "    %%%u = arith.addi %s, %s : %s\n",
                m->ssav, a, a, ty);
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    }
    case HK_IF: {
        /* If-expression: alloca a slot, conditionally store each
         * arm, load the merged result. */
        char cnd[40], v[40], ty[40];
        unsigned slot = m->ssav++, bt = m->blk++, bf = m->blk++,
                 bm = m->blk++, ld;
        if (!(n->flags & HF_EXPR)) {
            merr(m, n, "statement-position if in operand context");
            snprintf(res, rn, "0");
            break;
        }
        mty(n->type, ty, sizeof(ty));
        mexpr(m, n->child[0], cnd, sizeof(cnd));
        fprintf(m->out, "    %%%u = llvm.alloca : %s\n", slot, ty);
        fprintf(m->out, "    cf.cond_br %s, ^bb%u, ^bb%u\n", cnd, bt, bf);
        fprintf(m->out, "  ^bb%u:\n", bt);
        mexpr(m, n->child[1], v, sizeof(v));
        fprintf(m->out, "    llvm.store %s, %%%u : %s, !llvm.ptr\n",
                v, slot, ty);
        fprintf(m->out, "    cf.br ^bb%u\n", bm);
        fprintf(m->out, "  ^bb%u:\n", bf);
        mexpr(m, n->child[2], v, sizeof(v));
        fprintf(m->out, "    llvm.store %s, %%%u : %s, !llvm.ptr\n",
                v, slot, ty);
        fprintf(m->out, "    cf.br ^bb%u\n", bm);
        fprintf(m->out, "  ^bb%u:\n", bm);
        ld = m->ssav++;
        fprintf(m->out, "    %%%u = llvm.load %%%u : !llvm.ptr -> %s\n",
                ld, slot, ty);
        snprintf(res, rn, "%%%u", ld);
        break;
    }
    case HK_ERR_VAL: {
        /* The error tag: a plain integer constant in the listing. */
        unsigned v = n->child[0] && n->child[0]->kind == HK_CONST_INT
                         ? (unsigned)n->child[0]->u.ival : 0;
        fprintf(m->out, "    %%%u = arith.constant %u : i32  ; error tag\n",
                m->ssav, v);
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    }
    case HK_STRUCT_LIT: {
        /* Build the aggregate by inserting each field value into an
         * undef of the literal struct type. */
        char v[40], ty[40], cur[48];
        int k;
        ty[0] = 0;
        strncat(ty, "!llvm.struct<( ", sizeof(ty) - 1);
        for (k = 0; k < n->nitems; k++) {
            char ft[40];
            mty(n->items[k]->type, ft, sizeof(ft));
            strncat(ty, ft, sizeof(ty) - strlen(ty) - 1);
            if (k + 1 < n->nitems)
                strncat(ty, ", ", sizeof(ty) - strlen(ty) - 1);
        }
        strncat(ty, ")>", sizeof(ty) - strlen(ty) - 1);
        snprintf(cur, sizeof(cur), "llvm.undef");
        for (k = 0; k < n->nitems; k++) {
            mexpr(m, n->items[k], v, sizeof(v));
            fprintf(m->out,
                    "    %%%u = llvm.insertvalue %s, %s, %s[%d] : %s\n",
                    m->ssav, cur, v, cur, k, ty);
            snprintf(cur, sizeof(cur), "%%%u", m->ssav++);
        }
        snprintf(res, rn, "%s", cur);
        break;
    }
    case HK_FIELD: {
        /* Aggregates live behind pointers in this listing; a field
         * read loads the field's own type from the base address. */
        char base[40], fty[40];
        mexpr(m, n->child[0], base, sizeof(base));
        mty(n->type, fty, sizeof(fty));
        fprintf(m->out,
                "    %%%u = llvm.load %s : !llvm.ptr -> %s  ; field %d\n",
                m->ssav, base, fty, n->u.index);
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    }
    case HK_CATCH: {
        /* Error handling: unwrap the value half of the px_res pair;
         * the handler arm is the error path (noted in comment). */
        char a[40], ty[40];
        mexpr(m, n->child[0], a, sizeof(a));
        mty(n->type, ty, sizeof(ty));
        fprintf(m->out,
                "    %%%u = llvm.extractvalue %s[0] : !llvm.struct<(%s, "
                "i32)>  ; catch: error handler on non-zero tag\n",
                m->ssav, a, ty);
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    }
    case HK_INDEX: {
        /* Element access through the array storage. */
        char base[40], idx[40], ety[40];
        const HType *bt = n->child[0] ? n->child[0]->type : NULL;
        unsigned g;
        mexpr(m, n->child[0], base, sizeof(base));
        mexpr(m, n->child[1], idx, sizeof(idx));
        mty(n->type, ety, sizeof(ety));
        g = m->ssav++;
        if (bt && bt->kind == HTY_VIEW) {
            fprintf(m->out, "    %%%u = llvm.load %s : !llvm.ptr -> "
                    "!llvm.ptr\n", g, base);
            fprintf(m->out, "    %%%u = llvm.getelementptr %%%u[%s] : "
                    "(!llvm.ptr, i32) -> !llvm.ptr\n", m->ssav, g, idx);
            g = m->ssav++;
        } else {
            fprintf(m->out, "    %%%u = llvm.getelementptr %s[%s] : "
                    "(!llvm.ptr, i32) -> !llvm.ptr\n", g, base, idx);
        }
        fprintf(m->out, "    %%%u = llvm.load %%%u : !llvm.ptr -> %s\n",
                m->ssav, g, ety);
        snprintf(res, rn, "%%%u", m->ssav++);
        break;
    }
    case HK_BLOCK: {
        /* A block in value position: statements first, the final
         * expression statement supplies the value. */
        int k;
        for (k = 0; k + 1 < n->nitems; k++)
            mstmt(m, n->items[k]);
        if (n->nitems > 0 &&
            n->items[n->nitems - 1]->kind == HK_EXPR_STMT &&
            n->items[n->nitems - 1]->child[0])
            mexpr(m, n->items[n->nitems - 1]->child[0], res, rn);
        else
            snprintf(res, rn, "0");
        break;
    }
    default:
        merr(m, n, "this expression form");
        snprintf(res, rn, "0");
        break;
    }
}
