#include "common.h"
#include "llic_int.h"

/*
 * llic.c - the LLVM IR emission pass: shared context, the LLVM type
 * printer, the slot type helpers, the result-struct namer, string
 * constants and the expression printer. Statements, function bodies
 * and the entry point live in llic_stmt.c; interpolated-string
 * printing lives in llic_fmt.c.
 *
 * Values are unnamed SSA registers numbered per function; locals live
 * in allocas addressed by their HIR slot, typed by the normalized
 * class recorded in the slot table (sub-word integers share i32,
 * Int64 is i64, Bool is i1, pointers are opaque ptr), which avoids
 * mixed-width operands inside a single instruction. String constants
 * are collected before their function and printed as private globals;
 * the constant id lives in the aux field so it never collides with
 * the slice payload. Types map onto LLVM integers (i8..i256),
 * float/double, i1 for Bool and opaque ptr for pointers. A function
 * declared T! names the px_res_<T> struct as its return type; error
 * values are constant structs and success values are built with
 * insertvalue. Call arguments are materialised before the call line
 * because computing an argument may emit instructions that must
 * precede the call. Calls to undeclared names are emitted as calls to
 * extern globals, matching the C backend's treatment; every standard
 * library function is compiled from Paxsy sources like any other
 * code, so the unit declares its own callees.
 */

/* Normalizes an HIR type to the slot width class the backend uses:
 * sub-word integers share i32, Int64 is i64, Bool is i1, pointers are
 * opaque ptr. Keeping one width class per kind avoids mixed-width
 * operands inside a single instruction. */
void lslot_norm(const HType *ty, char *buf, size_t n) {
    if (!ty) { snprintf(buf, n, "i32"); return; }
    switch (ty->kind) {
    case HTY_BOOL:  snprintf(buf, n, "i1"); break;
    case HTY_CHAR:  snprintf(buf, n, "i8"); break;
    case HTY_INT: case HTY_UINT:
        snprintf(buf, n, "%s", ty->width > 32 ? "i64" : "i32"); break;
    case HTY_REAL:  snprintf(buf, n, "%s", ty->width <= 32 ? "float" : "double"); break;
    case HTY_PTR: case HTY_ARRAY: case HTY_CHAN:
        snprintf(buf, n, "ptr"); break;
    case HTY_ERROR: lslot_norm(ty->u.error.value, buf, n); break;
    default:        snprintf(buf, n, "i32"); break;
    }
}

/* Records the normalized type of a local slot, growing the table on
 * demand; reads fall back to i32 for unrecorded slots. */
void lslot_set(LE *l, int slot, const HType *ty) {
    if (slot < 0) return;
    if (slot >= l->nslot_ty) {
        int cap = slot + 1;
        l->slot_ty = realloc(l->slot_ty, (size_t)cap * 8);
        for (int i = l->nslot_ty; i < cap; i++)
            snprintf(l->slot_ty[i], 8, "i32");
        l->nslot_ty = cap;
    }
    lslot_norm(ty, l->slot_ty[slot], 8);
}

const char *lslot_ty(LE *l, int slot) {
    if (slot >= 0 && slot < l->nslot_ty) return l->slot_ty[slot];
    return "i32";
}

void lerr_(LE *l, const HNode *n, const char *what) {
    diag_report_full(l->diag, DK_ERROR, NULL, n->tok.line, n->tok.col,
                     n->tok.len, "LLVM emission: %s is not supported yet",
                     what);
}

/* Names the LLVM result struct of a T! type with the given value
 * type, e.g. px_res_i32; the struct bodies are emitted once per unit
 * by llic_stmt.c. */
void lres_name(const HType *value_type, char *buf, size_t n) {
    const char *s = "i32";
    if (value_type) {
        switch (value_type->kind) {
        case HTY_VOID:  s = "void"; break;
        case HTY_INT: case HTY_UINT: {
            int w = value_type->width <= 8 ? 8 : value_type->width <= 16 ? 16
                : value_type->width <= 32 ? 32 : 64;
            s = w == 8 ? "i8" : w == 16 ? "i16" : w == 32 ? "i32" : "i64";
            break;
        }
        case HTY_REAL:  s = value_type->width <= 32 ? "float" : "double"; break;
        case HTY_BOOL:  s = "i1"; break;
        case HTY_CHAR:  s = "i8"; break;
        default:        s = "i32"; break;
        }
    }
    snprintf(buf, n, "px_res_%s", s);
}

/* The value type carried by a T! function, or NULL. */
const HType *lfunc_res_value(const HNode *f) {
    if (f && f->type && f->type->kind == HTY_ERROR)
        return f->type->u.error.value;
    return NULL;
}

/* Renders the LLVM type for an HIR type into a buffer. */
void lty_buf(const HType *t, char *buf, size_t n) {
    if (!t) { snprintf(buf, n, "i32"); return; }
    switch (t->kind) {
    case HTY_VOID:  snprintf(buf, n, "void"); break;
    case HTY_BOOL:  snprintf(buf, n, "i1"); break;
    case HTY_CHAR:  snprintf(buf, n, "i8"); break;
    case HTY_INT:
    case HTY_UINT: {
        int w = t->width <= 8 ? 8 : t->width <= 16 ? 16
            : t->width <= 32 ? 32 : t->width <= 64 ? 64
            : t->width <= 128 ? 128 : 256;
        snprintf(buf, n, "i%d", w);
        break;
    }
    case HTY_REAL:  snprintf(buf, n, "%s", t->width <= 32 ? "float" : "double"); break;
    case HTY_PTR:   snprintf(buf, n, "ptr"); break;
    case HTY_ERROR: lty_buf(t->u.error.value, buf, n); break;
    default:        snprintf(buf, n, "i32"); break;
    }
}

/* Emits the LLVM type for an HIR type; NULL becomes i32. */
void lty(LE *l, const HType *t) {
    char buf[32];
    lty_buf(t, buf, sizeof(buf));
    fputs(buf, l->out);
}

/* Writes the C-identifier form of a possibly qualified name. */
void lname(const HNode *n, char *buf, size_t nbuf) {
    if (n->name && strchr(n->name, ':')) {
        size_t k = 0;
        for (const char *p = n->name; *p && k + 1 < nbuf; p++)
            buf[k++] = (*p == ':' || *p == '.') ? '_' : *p;
        buf[k] = '\0';
    } else {
        snprintf(buf, nbuf, "%s", n->name ? n->name : "px_unknown");
    }
}

/* Prints a C-style escaped string body (no quotes). */
void lstr_body(LE *l, const char *s, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"' || ch == '\\') fprintf(l->out, "\\%02X", ch);
        else if (ch == '\n') fputs("\\0A", l->out);
        else if (ch == '\t') fputs("\\09", l->out);
        else if (ch < 0x20 || ch > 0x7E) fprintf(l->out, "\\%02X", ch);
        else fputc(ch, l->out);
    }
}

/* Emits the string constants of one function as private globals and
 * stamps each constant node with its global id in the aux field. */
void lcollect_strings(LE *l, HNode *n) {
    if (!n) return;
    lcollect_fmt(l, n);
    if ((n->kind == HK_CONST_STRING || n->kind == HK_CONST_CHAR) &&
        n->u.slice.len >= 0 && n->aux != -2) {
        fprintf(l->out, "@.str%d = private constant [%d x i8] c\"",
                l->strno, n->u.slice.len + 1);
        lstr_body(l, n->u.slice.start, n->u.slice.len);
        fputs("\\00\"\n", l->out);
        n->aux = l->strno;
        l->strno++;
    }
    for (int i = 0; i < 4; i++) lcollect_strings(l, n->child[i]);
    for (int i = 0; i < n->nitems; i++) lcollect_strings(l, n->items[i]);
}

static void lexpr_binop(LE *l, const HNode *n, char *res, size_t rn) {
    char a[32], b[32], ty[16];
    lexpr(l, n->child[0], a, sizeof(a));
    lexpr(l, n->child[1], b, sizeof(b));
    int r = ++l->reg;
    lslot_norm(n->type, ty, sizeof(ty));
    switch (n->u.binop) {
    case HOP_ADD: fprintf(l->out, "    %%%d = add %s %s, %s\n", r, ty, a, b); break;
    case HOP_SUB: fprintf(l->out, "    %%%d = sub %s %s, %s\n", r, ty, a, b); break;
    case HOP_MUL: fprintf(l->out, "    %%%d = mul %s %s, %s\n", r, ty, a, b); break;
    case HOP_DIV: fprintf(l->out, "    %%%d = sdiv %s %s, %s\n", r, ty, a, b); break;
    case HOP_MOD: fprintf(l->out, "    %%%d = srem %s %s, %s\n", r, ty, a, b); break;
    case HOP_AND: fprintf(l->out, "    %%%d = and %s %s, %s\n", r, ty, a, b); break;
    case HOP_OR:  fprintf(l->out, "    %%%d = or %s %s, %s\n", r, ty, a, b); break;
    case HOP_XOR: fprintf(l->out, "    %%%d = xor %s %s, %s\n", r, ty, a, b); break;
    case HOP_SHL: fprintf(l->out, "    %%%d = shl %s %s, %s\n", r, ty, a, b); break;
    case HOP_SHR: case HOP_USHR:
        fprintf(l->out, "    %%%d = ashr %s %s, %s\n", r, ty, a, b); break;
    case HOP_EQ: case HOP_NE: case HOP_LT:
    case HOP_LE: case HOP_GT: case HOP_GE: {
        const char *p = n->u.binop == HOP_EQ ? "eq" : n->u.binop == HOP_NE ? "ne"
            : n->u.binop == HOP_LT ? "slt" : n->u.binop == HOP_LE ? "sle"
            : n->u.binop == HOP_GT ? "sgt" : "sge";
        fprintf(l->out, "    %%%d = icmp %s %s %s, %s\n", r, p, ty, a, b);
        break;
    }
    case HOP_LAND: fprintf(l->out, "    %%%d = and i1 %s, %s\n", r, a, b); break;
    case HOP_LOR:  fprintf(l->out, "    %%%d = or i1 %s, %s\n", r, a, b); break;
    }
    snprintf(res, rn, "%%%d", r);
}

void lexpr(LE *l, const HNode *n, char *res, size_t rn) {
    if (!n) { snprintf(res, rn, "0"); return; }
    switch (n->kind) {
    case HK_CONST_INT:
        snprintf(res, rn, "%lld", n->u.ival);
        return;
    case HK_CONST_FLOAT:
        snprintf(res, rn, "%g", n->u.fval);
        return;
    case HK_CONST_STRING: case HK_CONST_CHAR: {
        int id = n->aux;
        if (id < 0) { lerr_(l, n, "string constant without an id"); snprintf(res, rn, "0"); return; }
        snprintf(res, rn, "@.str%d", id);
        return;
    }
    case HK_PARAM:
        snprintf(res, rn, "%%p%d", n->u.index);
        return;
    case HK_LOCAL: {
        int r = ++l->reg;
        fprintf(l->out, "    %%%d = load %s, ptr %%loc%d\n",
                r, lslot_ty(l, n->u.index), n->u.index);
        snprintf(res, rn, "%%%d", r);
        return;
    }
    case HK_GLOBAL_REF:
        lname(n, res, rn);
        return;
    case HK_BINOP:
        lexpr_binop(l, n, res, rn);
        return;
    case HK_UNOP: {
        char a[32];
        int r;
        lexpr(l, n->child[0], a, sizeof(a));
        r = ++l->reg;
        switch (n->u.unop) {
        case HUN_NEG:
            fprintf(l->out, "    %%%d = sub i32 0, %s\n", r, a);
            snprintf(res, rn, "%%%d", r);
            return;
        case HUN_BITNOT:
            fprintf(l->out, "    %%%d = xor i32 %s, -1\n", r, a);
            snprintf(res, rn, "%%%d", r);
            return;
        case HUN_LNOT:
            fprintf(l->out, "    %%%d = icmp eq i32 %s, 0\n", r, a);
            snprintf(res, rn, "%%%d", r);
            return;
        default:
            lerr_(l, n, "unsupported unary operator");
            snprintf(res, rn, "0");
            return;
        }
    }
    case HK_CALL: {
        HNode *callee = n->child[0];
        char name[128], args[8][32];
        lname(callee, name, sizeof(name));
        /* Materialise the arguments first: lexpr emits load
         * instructions, which must appear before the call line. */
        for (int i = 0; i < n->nitems && i < 8; i++)
            lexpr(l, n->items[i], args[i], sizeof(args[i]));
        int r = ++l->reg;
        fprintf(l->out, "    %%%d = call ", r);
        if (n->type && n->type->kind == HTY_ERROR) {
            char rn[64];
            lres_name(n->type->u.error.value, rn, sizeof(rn));
            fputs(rn, l->out);
        } else {
            lty(l, n->type);
        }
        fprintf(l->out, " %s(", name);
        for (int i = 0; i < n->nitems && i < 8; i++) {
            if (i) fputs(", ", l->out);
            fprintf(l->out, "i32 %s", args[i]);
        }
        fputs(")\n", l->out);
        snprintf(res, rn, "%%%d", r);
        return;
    }
    case HK_ERR_VAL: {
        /* error <code>: a failing result constant of the enclosing
         * function's result struct. */
        char rn2[64], code[32];
        const HType *vt = lfunc_res_value(l->cur_func);
        lres_name(vt ? vt : NULL, rn2, sizeof(rn2));
        lexpr(l, n->child[0], code, sizeof(code));
        snprintf(res, rn, "%s { i32 0, i32 %s }", rn2, code);
        return;
    }
    case HK_FMT:
        lexpr_fmt(l, n, res, rn);
        return;
    case HK_TRY:
        lerr_(l, n, "try in operand position (statement form only)");
        snprintf(res, rn, "0");
        return;
    default:
        lerr_(l, n, "this expression");
        snprintf(res, rn, "0");
        return;
    }
}
