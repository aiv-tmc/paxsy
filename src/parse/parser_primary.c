#include "common.h"
#include "parser_int.h"

/*
 * parser_primary.c - the operand end of the expression grammar:
 * unary operators (including try/await/alloc/free/error and channel
 * creation/receive), postfix call/index/slice/field chains, and
 * primary expressions (literals, parenthesised expressions, array and
 * struct literals, if/match expressions, async blocks and names).
 * The block_next flag prevents 'if cond {' from being read as a
 * struct literal named after the condition's last identifier.
 */

/* Array literal {..} or struct literal Name {..}; elements may be
 * plain expressions or named initializers of the form .field = expr. */
static Node *pp_parse_literal_list(Parser *p, int kind, Token t, char *name) {
    Node *s = node_new(kind, t);
    s->name = name;
    p->pos++;                                /* '{' */
    while (!pp_at(p, TK_RBRACE) && !pp_at(p, TK_EOF)) {
        if (pp_at(p, TK_DOT)) {
            Token dt = *pp_pk(p, 0);
            p->pos++;
            Token f = *pp_ex(p, TK_IDENT, "field name in initializer");
            Node *ni = node_new(NK_NAMED_INIT, dt);
            ni->name = pp_strndup(f.start, f.len);
            pp_ex(p, TK_ASSIGN, "'=' in named initializer");
            ni->child[0] = pp_parse_expr(p);
            node_add(s, ni);
        } else {
            node_add(s, pp_parse_expr(p));
        }
        if (!pp_ac(p, TK_COMMA)) break;
    }
    pp_ex(p, TK_RBRACE, "'}' to close literal");
    return s;
}

static Node *pp_parse_postfix(Parser *p) {
    Node *l;
    {
        Token t = *pp_pk(p, 0);
        switch (t.kind) {
        case TK_INT_LIT: case TK_FLOAT_LIT: case TK_STRING_LIT: case TK_CHAR_LIT:
            p->pos++;
            l = node_new(NK_LITERAL, t);
            break;
        case TK_LPAREN:
            p->pos++;
            l = pp_parse_expr(p);
            pp_ex(p, TK_RPAREN, "')' after expression");
            break;
        case TK_LBRACE:
            l = pp_parse_literal_list(p, NK_ARRAY_LIT, t, NULL);
            break;
        case TK_KW_IF:
            l = pp_parse_if(p, 1);
            break;
        case TK_KW_MATCH:
            l = pp_parse_match(p, 1);
            break;
        case TK_KW_ASYNC: {
            p->pos++;
            Node *a = node_new(NK_ASYNC, t);
            if (pp_at(p, TK_LBRACE)) a->child[0] = pp_parse_block(p);
            else a->child[0] = pp_parse_postfix(p);   /* async f(x) */
            l = a;
            break;
        }
        case TK_IDENT: {
            char *name = pp_qualify(p);
            if (pp_at(p, TK_LBRACE) && !p->block_next)
                l = pp_parse_literal_list(p, NK_STRUCT_LIT, t, name);
            else {
                l = node_new(NK_NAME, t);
                l->name = name;
            }
            break;
        }
        default:
            pp_err(p, "unexpected token '%.*s' in expression", t.len, t.start);
            return NULL;
        }
    }
    for (;;) {                               /* postfix chain */
        if (pp_at(p, TK_LPAREN)) {           /* call */
            Token t = *pp_pk(p, 0);
            p->pos++;
            Node *c = node_new(NK_CALL, t);
            c->child[0] = l;
            if (!pp_at(p, TK_RPAREN)) {
                for (;;) {
                    node_add(c, pp_parse_expr(p));
                    if (!pp_ac(p, TK_COMMA)) break;
                }
            }
            pp_ex(p, TK_RPAREN, "')' after arguments");
            l = c;
        } else if (pp_at(p, TK_LBRACKET)) {  /* index or slice */
            Token t = *pp_pk(p, 0);
            p->pos++;
            Node *ix = node_new(NK_INDEX, t);
            ix->child[0] = l;
            if (!pp_at(p, TK_RBRACKET) && !pp_at(p, TK_DOTDOT))
                ix->child[1] = pp_parse_bitor(p);
            if (pp_at(p, TK_DOTDOT)) {
                p->pos++;
                ix->kind = NK_SLICE;
                if (pp_ac(p, TK_LT)) ix->flags |= NF_EXCLUSIVE;
                if (!pp_at(p, TK_RBRACKET)) ix->child[2] = pp_parse_bitor(p);
            }
            pp_ex(p, TK_RBRACKET, "']' after index");
            l = ix;
        } else if (pp_at(p, TK_DOT)) {       /* field access */
            Token t = *pp_pk(p, 0);
            p->pos++;
            Token f = *pp_ex(p, TK_IDENT, "field name after '.'");
            Node *fa = node_new(NK_FIELD_ACCESS, t);
            fa->child[0] = l;
            fa->name = pp_strndup(f.start, f.len);
            l = fa;
        } else break;
    }
    return l;
}

Node *pp_parse_unary(Parser *p) {
    Token t = *pp_pk(p, 0);
    switch (t.kind) {
    case TK_MINUS: case TK_TILDE: case TK_KW_NOT: case TK_AMP:
    case TK_AT: case TK_KW_AWAIT: case TK_KW_ERROR: case TK_KW_FREE:
        p->pos++;
        { Node *n = node_new(NK_UNARY, t); n->op = t.kind;
          n->child[0] = pp_parse_unary(p); return n; }
    case TK_KW_TRY:
        p->pos++;
        if (!p->cur_fn_err)
            pp_err(p, "'try' used in a function that does not return "
                      "an error type");
        { Node *n = node_new(NK_UNARY, t); n->op = TK_KW_TRY;
          n->child[0] = pp_parse_unary(p); return n; }
    case TK_KW_ALLOC:
        p->pos++;
        { Node *n = node_new(NK_ALLOC, t);
          n->child[0] = pp_parse_type(p); return n; }
    case TK_KW_CHAN: {                      /* chan / chan N */
        p->pos++;
        Node *n = node_new(NK_CHAN_NEW, t);
        if (pp_at(p, TK_INT_LIT)) { n->child[0] = node_new(NK_LITERAL, *pp_pk(p, 0)); p->pos++; }
        return n;
    }
    case TK_SENDRECV:                       /* receive: <- ch */
        p->pos++;
        { Node *n = node_new(NK_RECV, t);
          n->child[0] = pp_parse_unary(p); return n; }
    default:
        return pp_parse_postfix(p);
    }
}
