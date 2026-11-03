#include "common.h"
#include "cemit_int.h"

/*
 * cemit_lit.c - the small literal and operator printers shared by
 * the C value printer and the statement printer. Kept in their own
 * translation unit so the expression printer file stays focused on
 * node dispatch.
 */

/* Prints one integer literal: values outside the signed long long
 * range round-trip through an unsigned suffix cast. */
void cint_lit(CE *c, long long v) {
    if (v >= (-9223372036854775807LL - 1))
        fprintf(c->out, "%lld", v);
    else
        fprintf(c->out, "(long long)%lluULL", (unsigned long long)v);
}

/* Prints one float literal, forcing a '.' or exponent so the token
 * stays a floating constant even for whole values. */
void cfloat_lit(CE *c, double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", v);
    fputs(buf, c->out);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') &&
        !strchr(buf, 'n') && !strchr(buf, 'i'))
        fputs(".0", c->out);
}

void cbinop(CE *c, HBinOp op) {
    switch (op) {
    case HOP_ADD: fputs("+", c->out); break;
    case HOP_SUB: fputs("-", c->out); break;
    case HOP_MUL: fputs("*", c->out); break;
    case HOP_DIV: fputs("/", c->out); break;
    case HOP_MOD: fputs("%", c->out); break;
    case HOP_AND: fputs("&", c->out); break;
    case HOP_OR:  fputs("|", c->out); break;
    case HOP_XOR: fputs("^", c->out); break;
    case HOP_SHL: fputs("<<", c->out); break;
    case HOP_SHR: fputs(">>", c->out); break;
    case HOP_EQ:  fputs("==", c->out); break;
    case HOP_NE:  fputs("!=", c->out); break;
    case HOP_LT:  fputs("<", c->out); break;
    case HOP_LE:  fputs("<=", c->out); break;
    case HOP_GT:  fputs(">", c->out); break;
    case HOP_GE:  fputs(">=", c->out); break;
    case HOP_LAND:fputs("&&", c->out); break;
    case HOP_LOR: fputs("||", c->out); break;
    case HOP_USHR:fputs("px_ushr", c->out); break; /* runtime helper */
    }
}
