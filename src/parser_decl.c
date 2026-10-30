#include "common.h"
#include "parser_int.h"

/*
 * parser_decl.c - the heavier declarations: functions (plain,
 * qualified, methods, multi-struct methods, prototypes, expression
 * shorthand) and comptime declarations (constants, type aliases,
 * Struct/Union/Enum, comptime functions and comptime blocks).
 * A prototype with no body anywhere in the program declares an
 * external symbol; the linker resolves it (no extern keyword).
 */

/* Struct/Union body fields or Enum variants until '}'. */
static Node *pp_parse_aggregate(Parser *p, int kind, Token nm) {
    Node *s = node_new(kind, nm);
    p->pos++;                               /* Struct/Union/Enum keyword */
    pp_ex(p, TK_LBRACE, "'{' to open type definition");
    while (!pp_at(p, TK_RBRACE) && !pp_at(p, TK_EOF)) {
        if (kind == NK_ENUM) {
            Token v = *pp_ex(p, TK_IDENT, "enum variant");
            Node *var = node_new(NK_VARIANT, v);
            var->name = pp_strndup(v.start, v.len);
            if (pp_ac(p, TK_ASSIGN)) var->child[0] = pp_parse_expr(p);
            node_add(s, var);
        } else {
            Token f = *pp_ex(p, TK_IDENT, "field name");
            Node *fd = node_new(NK_FIELD, f);
            fd->name = pp_strndup(f.start, f.len);
            pp_ex(p, TK_COLON, "':' before field type");
            fd->child[0] = pp_parse_type(p);
            node_add(s, fd);
        }
        if (!pp_ac(p, TK_COMMA) && !pp_ac(p, TK_SEMI)) break;
    }
    pp_ex(p, TK_RBRACE, "'}' to close type definition");
    return s;
}

static void pp_parse_params(Parser *p, Node *f) {
    pp_ex(p, TK_LPAREN, "'(' before parameter list");
    if (pp_ac(p, TK_RPAREN)) return;
    for (;;) {
        Token t = *pp_pk(p, 0);
        Node *param = node_new(NK_PARAM, t);
        if (pp_ac(p, TK_KW_COMPTIME)) {     /* comptime T */
            param->flags |= NF_COMPTIME;
            Token n = *pp_ex(p, TK_IDENT, "compile-time parameter name");
            param->name = pp_strndup(n.start, n.len);
        } else {
            Token n = *pp_ex(p, TK_IDENT, "parameter name");
            param->name = pp_strndup(n.start, n.len);
            pp_ex(p, TK_COLON, "':' before parameter type");
            param->child[0] = pp_parse_type(p);
        }
        node_add(f, param);
        if (!pp_ac(p, TK_COMMA)) break;
    }
    pp_ex(p, TK_RPAREN, "')' after parameter list");
}

Node *pp_parse_func(Parser *p) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_FUNC);
    Node *f = node_new(NK_FUNC, t);

    if (pp_at(p, TK_LBRACE)) {              /* func {A, B}.name(...) */
        p->pos++;
        char *names = NULL;
        for (;;) {
            char *one = pp_qualify(p);
            if (!names) names = one;
            else {
                size_t n = strlen(names);
                names = realloc(names, n + 1 + strlen(one) + 1);
                strcat(names, ","); strcat(names, one);
            }
            if (!pp_ac(p, TK_COMMA)) break;
        }
        pp_ex(p, TK_RBRACE, "'}' after struct list");
        pp_ex(p, TK_DOT, "'.' after struct list");
        Token m = *pp_ex(p, TK_IDENT, "method name");
        size_t n = strlen(names);
        names = realloc(names, n + 1 + (size_t)m.len + 1);
        strcat(names, "."); strncat(names, m.start, (size_t)m.len);
        f->name = names;
    } else {
        Token nm = *pp_ex(p, TK_IDENT, "function name");
        f->name = pp_strndup(nm.start, nm.len);
        while (pp_ac(p, TK_COLONCOLON)) {   /* qualified name */
            Token m = *pp_ex(p, TK_IDENT, "identifier after '::'");
            size_t n = strlen(f->name);
            f->name = realloc(f->name, n + 2 + (size_t)m.len + 1);
            strcat(f->name, "::"); strncat(f->name, m.start, (size_t)m.len);
        }
        if (pp_ac(p, TK_DOT)) {             /* method: Point.distance */
            Token m = *pp_ex(p, TK_IDENT, "method name");
            size_t n = strlen(f->name);
            f->name = realloc(f->name, n + 1 + (size_t)m.len + 1);
            strcat(f->name, "."); strncat(f->name, m.start, (size_t)m.len);
        }
    }

    pp_parse_params(p, f);

    /* parse_type() consumes the '!' suffix itself (T!), so the error
     * flag is read back from the node kind it produced. */
    if (pp_ac(p, TK_COLON)) f->child[0] = pp_parse_type(p);
    int is_err = f->child[0] && f->child[0]->kind == NK_TYPE_ERROR;

    if (pp_ac(p, TK_ASSIGN)) {              /* func f(...): T = expr; */
        f->child[2] = pp_parse_expr(p);
        pp_ex(p, TK_SEMI, "';' after function shorthand body");
        return f;
    }
    if (pp_ac(p, TK_SEMI)) {                /* prototype: body elsewhere
                                             * in the program or external */
        f->flags |= NF_PROTOTYPE;
        return f;
    }
    int saved = p->cur_fn_err;            /* body: enable try when T! */
    p->cur_fn_err = is_err;
    f->child[3] = pp_parse_block(p);
    p->cur_fn_err = saved;
    return f;
}

Node *pp_parse_comptime(Parser *p) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_COMPTIME);

    if (pp_at(p, TK_LBRACE) ||
        (pp_at(p, TK_IDENT) && pp_pk(p, 1)->kind == TK_LBRACE)) {
        Node *b = node_new(NK_COMPTIME_BLOCK, t);   /* comptime [name] { } */
        if (pp_at(p, TK_IDENT)) {
            b->name = pp_strndup(pp_pk(p, 0)->start, pp_pk(p, 0)->len);
            p->pos++;
        }
        pp_ex(p, TK_LBRACE, "'{' to open comptime block");
        while (!pp_at(p, TK_RBRACE) && !pp_at(p, TK_EOF)) {
            if (pp_at(p, TK_KW_COMPTIME)) node_add(b, pp_parse_comptime(p));
            else node_add(b, pp_parse_stmt(p));
        }
        pp_ex(p, TK_RBRACE, "'}' to close comptime block");
        return b;
    }
    if (pp_ac(p, TK_KW_LET)) {              /* comptime let x = expr; */
        Node *c = node_new(NK_COMPTIME_CONST, t);
        Token n = *pp_ex(p, TK_IDENT, "name after 'comptime let'");
        c->name = pp_strndup(n.start, n.len);
        pp_ex(p, TK_ASSIGN, "'=' in comptime declaration");
        c->child[0] = pp_parse_expr(p);
        pp_ex(p, TK_SEMI, "';' after comptime declaration");
        return c;
    }

    Token nm = *pp_ex(p, TK_IDENT, "comptime name");

    if (pp_at(p, TK_LPAREN)) {              /* comptime function */
        Node *f = node_new(NK_COMPTIME_FN, nm);
        f->name = pp_strndup(nm.start, nm.len);
        pp_parse_params(p, f);
        if (pp_ac(p, TK_COLON)) f->child[0] = pp_parse_type(p);
        f->child[3] = pp_parse_block(p);
        return f;
    }

    pp_ex(p, TK_ASSIGN, "'=' in comptime declaration");
    if (pp_at(p, TK_KW_STRUCT)) { Node *s = pp_parse_aggregate(p, NK_STRUCT, nm); pp_ac(p, TK_SEMI); return s; }
    if (pp_at(p, TK_KW_UNION))  { Node *s = pp_parse_aggregate(p, NK_UNION, nm);  pp_ac(p, TK_SEMI); return s; }
    if (pp_at(p, TK_KW_ENUM))   { Node *s = pp_parse_aggregate(p, NK_ENUM, nm);   pp_ac(p, TK_SEMI); return s; }

    Node *c = node_new(NK_COMPTIME_CONST, nm);
    c->name = pp_strndup(nm.start, nm.len);
    c->child[0] = pp_parse_expr(p);       /* constant or type alias */
    pp_ex(p, TK_SEMI, "';' after comptime declaration");
    return c;
}
