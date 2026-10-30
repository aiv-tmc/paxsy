#include "common.h"
#include "parser_int.h"

/*
 * parser_expr.c - the expression chain, one function per precedence
 * level exactly as specified by the language reference (highest to
 * lowest): as > multiplicative > additive > shift > & > ^ > $ >
 * comparison > and > or > catch > <- (right-associative). Assignment
 * and the compound assignment forms sit below everything and are
 * right-associative.
 */

static Node *pp_parse_catch(Parser *p);
static Node *pp_parse_chan(Parser *p);
static Node *pp_parse_assign(Parser *p);
static Node *pp_parse_or(Parser *p);
static Node *pp_parse_and(Parser *p);
static Node *pp_parse_cmp(Parser *p);
static Node *pp_parse_bitxor(Parser *p);
static Node *pp_parse_bitand(Parser *p);
static Node *pp_parse_shift(Parser *p);
static Node *pp_parse_add(Parser *p);
static Node *pp_parse_mul(Parser *p);
static Node *pp_parse_as(Parser *p);

static Node *pp_mkbin(TokenKind op, Node *l, Node *r, Token t) {
    Node *n = node_new(NK_BINARY, t);
    n->op = op;
    n->child[0] = l;
    n->child[1] = r;
    return n;
}

/* Assignment and compound assignment are the loosest operators and
 * associate to the right. */
static Node *pp_parse_assign(Parser *p) {
    Node *l = pp_parse_chan(p);
    TokenKind k = pp_pk(p, 0)->kind;
    if (k == TK_ASSIGN || k == TK_PLUS_EQ || k == TK_MINUS_EQ ||
        k == TK_STAR_EQ || k == TK_SLASH_EQ || k == TK_PERCENT_EQ ||
        k == TK_AMP_EQ || k == TK_DOLLAR_EQ || k == TK_CARET_EQ ||
        k == TK_SHL_EQ || k == TK_SHR_EQ || k == TK_USHR_EQ) {
        Token t = *pp_pk(p, 0);
        p->pos++;
        Node *n = node_new(NK_BINARY, t);
        n->op = k;
        n->child[0] = l;
        n->child[1] = pp_parse_assign(p);
        return n;
    }
    return l;
}

Node *pp_parse_expr(Parser *p) { return pp_parse_assign(p); }

static Node *pp_parse_catch(Parser *p) {
    Node *l = pp_parse_or(p);
    if (pp_at(p, TK_KW_CATCH)) {
        Token t = *pp_pk(p, 0);
        p->pos++;
        Node *n = node_new(NK_CATCH, t);
        n->child[0] = l;
        pp_ex(p, TK_PIPE, "'|' before catch parameter");
        Token v = *pp_ex(p, TK_IDENT, "catch parameter");
        n->name = pp_strndup(v.start, v.len);
        pp_ex(p, TK_PIPE, "'|' after catch parameter");
        n->child[1] = pp_parse_block(p);
        return n;
    }
    return l;
}

static Node *pp_parse_chan(Parser *p) {
    Node *l = pp_parse_catch(p);
    if (pp_at(p, TK_SENDRECV)) {
        Token t = *pp_pk(p, 0);
        p->pos++;
        Node *n = node_new(NK_SEND, t);     /* ch <- value (right-assoc) */
        n->child[0] = l;
        n->child[1] = pp_parse_chan(p);
        return n;
    }
    return l;
}

static Node *pp_parse_or(Parser *p) {
    Node *l = pp_parse_and(p);
    while (pp_at(p, TK_KW_OR)) {
        Token t = *pp_pk(p, 0); p->pos++;
        l = pp_mkbin(TK_KW_OR, l, pp_parse_and(p), t);
    }
    return l;
}

static Node *pp_parse_and(Parser *p) {
    Node *l = pp_parse_cmp(p);
    while (pp_at(p, TK_KW_AND)) {
        Token t = *pp_pk(p, 0); p->pos++;
        l = pp_mkbin(TK_KW_AND, l, pp_parse_cmp(p), t);
    }
    return l;
}

static Node *pp_parse_cmp(Parser *p) {
    Node *l = pp_parse_bitor(p);
    for (;;) {
        TokenKind k = pp_pk(p, 0)->kind;
        if (k != TK_EQ && k != TK_NE && k != TK_LT && k != TK_GT &&
            k != TK_LE && k != TK_GE) break;
        Token t = *pp_pk(p, 0); p->pos++;
        l = pp_mkbin(k, l, pp_parse_bitor(p), t);
    }
    return l;
}

Node *pp_parse_bitor(Parser *p) {           /* $ */
    Node *l = pp_parse_bitxor(p);
    while (pp_at(p, TK_DOLLAR)) {
        Token t = *pp_pk(p, 0); p->pos++;
        l = pp_mkbin(TK_DOLLAR, l, pp_parse_bitxor(p), t);
    }
    return l;
}

static Node *pp_parse_bitxor(Parser *p) {   /* ^ */
    Node *l = pp_parse_bitand(p);
    while (pp_at(p, TK_CARET)) {
        Token t = *pp_pk(p, 0); p->pos++;
        l = pp_mkbin(TK_CARET, l, pp_parse_bitand(p), t);
    }
    return l;
}

static Node *pp_parse_bitand(Parser *p) {   /* & */
    Node *l = pp_parse_shift(p);
    while (pp_at(p, TK_AMP)) {
        Token t = *pp_pk(p, 0); p->pos++;
        l = pp_mkbin(TK_AMP, l, pp_parse_shift(p), t);
    }
    return l;
}

static Node *pp_parse_shift(Parser *p) {
    Node *l = pp_parse_add(p);
    for (;;) {
        TokenKind k = pp_pk(p, 0)->kind;
        if (k != TK_SHL && k != TK_SHR && k != TK_USHR) break;
        Token t = *pp_pk(p, 0); p->pos++;
        l = pp_mkbin(k, l, pp_parse_add(p), t);
    }
    return l;
}

static Node *pp_parse_add(Parser *p) {
    Node *l = pp_parse_mul(p);
    while (pp_at(p, TK_PLUS) || pp_at(p, TK_MINUS)) {
        Token t = *pp_pk(p, 0); p->pos++;
        l = pp_mkbin(t.kind, l, pp_parse_mul(p), t);
    }
    return l;
}

static Node *pp_parse_mul(Parser *p) {
    Node *l = pp_parse_as(p);
    while (pp_at(p, TK_STAR) || pp_at(p, TK_SLASH) || pp_at(p, TK_PERCENT)) {
        Token t = *pp_pk(p, 0); p->pos++;
        l = pp_mkbin(t.kind, l, pp_parse_as(p), t);
    }
    return l;
}

static Node *pp_parse_as(Parser *p) {       /* as : left-assoc, above mul */
    Node *l = pp_parse_unary(p);
    while (pp_at(p, TK_KW_AS)) {
        Token t = *pp_pk(p, 0); p->pos++;
        Node *n = node_new(NK_CAST, t);
        n->child[0] = l;
        n->child[1] = pp_parse_type(p);
        l = n;
    }
    return l;
}
