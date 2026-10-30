#include "common.h"
#include "parser_int.h"

/*
 * parser.c - parser core: token-stream helpers, the error primitive,
 * the entry point and the small declaration parsers (use/module/let).
 * Parsing never aborts the process: pp_err() records a diagnostic and
 * longjmps back to parser_run(), which then reports failure.
 */

/* Token window clamped at EOF; every helper reads through this. */
Token *pp_pk(Parser *p, int off) {
    int i = p->pos + off;
    if (i >= p->count) i = p->count - 1;
    return (Token *)&p->toks[i];
}

int pp_at(Parser *p, TokenKind kind) { return pp_pk(p, 0)->kind == kind; }

int pp_ac(Parser *p, TokenKind kind) {
    if (pp_at(p, kind)) { p->pos++; return 1; }
    return 0;
}

Token *pp_ex(Parser *p, TokenKind kind, const char *what) {
    if (!pp_at(p, kind))
        pp_err(p, "expected %s but found '%.*s'", what,
               pp_pk(p, 0)->len, pp_pk(p, 0)->start);
    return (Token *)&p->toks[p->pos++];
}

/* Reports an error at the current token and unwinds to parser_run.
 * The token length travels with the diagnostic so the renderer can
 * underline the whole offending token with carets. */
void pp_err(Parser *p, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Token *t = pp_pk(p, 0);
    diag_report_full(p->diag, DK_ERROR, p->path, t->line, t->col,
                     t->len, "%s", buf);
    longjmp(p->on_error, 1);
}

char *pp_strndup(const char *s, int n) {
    char *r = malloc((size_t)n + 1);
    memcpy(r, s, (size_t)n);
    r[n] = '\0';
    return r;
}

/* Builds a qualified name: ident (:: ident)*. The caller handles any
 * trailing '.method' part. */
char *pp_qualify(Parser *p) {
    Token t = *pp_ex(p, TK_IDENT, "identifier");
    char *s = pp_strndup(t.start, t.len);
    while (pp_ac(p, TK_COLONCOLON)) {
        Token m = *pp_ex(p, TK_IDENT, "identifier after '::'");
        size_t n = strlen(s);
        s = realloc(s, n + 2 + (size_t)m.len + 1);
        strcat(s, "::");
        strncat(s, m.start, (size_t)m.len);
    }
    return s;
}

Node *parser_run(const TokenVec *tv, Diag *diag, const char *path) {
    Parser p;
    memset(&p, 0, sizeof(p));
    p.toks = tv->items;
    p.count = tv->count;
    p.diag = diag;
    p.path = path;
    if (setjmp(p.on_error)) return NULL;
    Node *prog = node_new(NK_PROGRAM, tv->items[0]);
    while (!pp_at(&p, TK_EOF)) node_add(prog, pp_parse_top(&p));
    return prog;
}

Node *pp_parse_top(Parser *p) {
    switch (pp_pk(p, 0)->kind) {
    case TK_KW_USE:     return pp_parse_use(p);
    case TK_KW_MODULE:  return pp_parse_module(p);
    case TK_KW_COMPTIME:return pp_parse_comptime(p);
    case TK_KW_FUNC:    return pp_parse_func(p);
    case TK_KW_LET:     return pp_parse_let(p);
    default:
        pp_err(p, "unexpected token '%.*s' at top level",
               pp_pk(p, 0)->len, pp_pk(p, 0)->start);
    }
    return NULL;
}

Node *pp_parse_use(Parser *p) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_USE);
    Node *u = node_new(NK_USE, t);
    u->name = pp_qualify(p);
    pp_ex(p, TK_SEMI, "';' after use directive");
    return u;
}

Node *pp_parse_module(Parser *p) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_MODULE);
    Node *m = node_new(NK_MODULE, t);
    if (pp_at(p, TK_IDENT)) {
        m->name = pp_strndup(pp_pk(p, 0)->start, pp_pk(p, 0)->len);
        p->pos++;
    }
    pp_ex(p, TK_LBRACE, "'{' to open module body");
    while (!pp_at(p, TK_RBRACE) && !pp_at(p, TK_EOF))
        node_add(m, pp_parse_stmt(p));
    pp_ex(p, TK_RBRACE, "'}' to close module body");
    return m;
}

Node *pp_parse_let(Parser *p) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_LET);

    if (pp_at(p, TK_LPAREN)) {          /* let (a: T = v, b: T) / let (a, b) = t; */
        Node *m = node_new(NK_LET_MULTI, t);
        p->pos++;
        for (;;) {
            Token n = *pp_ex(p, TK_IDENT, "variable name");
            Node *v = node_new(NK_LET, n);
            v->name = pp_strndup(n.start, n.len);
            if (pp_ac(p, TK_COLON)) v->child[0] = pp_parse_type(p);
            if (pp_ac(p, TK_ASSIGN)) v->child[1] = pp_parse_expr(p);
            node_add(m, v);
            if (!pp_ac(p, TK_COMMA)) break;
        }
        pp_ex(p, TK_RPAREN, "')' in multiple declaration");
        if (pp_ac(p, TK_ASSIGN)) m->child[0] = pp_parse_expr(p);
        pp_ex(p, TK_SEMI, "';' after declaration");
        return m;
    }

    Token n = *pp_ex(p, TK_IDENT, "variable name");
    Node *v = node_new(NK_LET, n);
    v->name = pp_strndup(n.start, n.len);
    if (pp_ac(p, TK_COLON)) v->child[0] = pp_parse_type(p);
    if (pp_ac(p, TK_ASSIGN)) v->child[1] = pp_parse_expr(p);
    pp_ex(p, TK_SEMI, "';' after declaration");
    return v;
}
