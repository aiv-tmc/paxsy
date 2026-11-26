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

/* True when a string literal holds at least one interpolation
 * marker (an unescaped '{' at brace depth zero; the lexer guarantees
 * the braces balance). */
static int pp_has_interp(Token t) {
    int depth = 0, any = 0;
    for (int i = 0; i < (int)t.len; i++) {
        char c = t.start[i];
        if (c == '\\' && i + 1 < (int)t.len) { i++; continue; }
        if (c == '"') break;
        if (c == '{') { if (depth == 0) any = 1; depth++; }
        else if (c == '}' && depth > 0) depth--;
    }
    return any;
}

/* Append a literal chunk to an interpolated string; the chunk keeps
 * its escape sequences verbatim, exactly like a plain literal. */
static void pp_fmt_chunk(Parser *p, Node *fmt, const char *text, int len,
                         Token at) {
    (void)p;
    Token ct = at;
    ct.kind = TK_STRING_LIT;
    ct.start = text;
    ct.len = len;
    node_add(fmt, node_new(NK_LITERAL, ct));
}

/* The fragment lexer starts at line 1, column 1. Rebase every AST
 * token to the containing string's source coordinates before semantic
 * analysis so unknown names inside {interpolation} point at the actual
 * identifier rather than at the first import in the file. */
static void pp_rebase_interp_node(Node *n, int line, int base_col) {
    if (!n) return;
    n->tok.line += line - 1;
    if (n->tok.line == line) n->tok.col += base_col - 1;
    for (int i = 0; i < 4; i++)
        pp_rebase_interp_node(n->child[i], line, base_col);
    for (int i = 0; i < n->item_count; i++)
        pp_rebase_interp_node(n->items[i], line, base_col);
}

/* Parse one interpolation expression with a lexer/parser pair over
 * the fragment source. */
static Node *pp_interp_expr(Parser *p, const char *text, int len, Token at) {
    Source sub = {0};
    sub.text = (char *)text;
    sub.len = (size_t)len;
    sub.path = p->path;
    sub.line_start = calloc(1, sizeof(size_t));
    sub.line_count = 1;
    TokenVec tv = {0};
    if (lexer_run(&sub, &tv, p->diag) != 0) {
        free(sub.line_start);
        return NULL;
    }
    Node *e = parser_parse_expr(&tv, p->diag, p->path);
    if (e) {
        int base_line = at.line, base_col = at.col;
        for (const char *q = at.start; q < text; q++) {
            if (*q == '\n') { base_line++; base_col = 1; }
            else base_col++;
        }
        pp_rebase_interp_node(e, base_line, base_col);
    }
    free(sub.line_start);
    if (!e)
        pp_err(p, "invalid expression in string interpolation");
    return e;
}

/* Split "text {expr} text..." into chunks and expressions. Chunks
 * occupy even item slots, expressions the odd ones; the lexer has
 * already balanced every brace inside the string. */
static Node *pp_interp_string(Parser *p, Token t) {
    Node *fmt = node_new(NK_FMT, t);
    const char *txt = t.start;
    int len = (int)t.len;
    int i = 0, chunk = 0;
    while (i < len) {
        char c = txt[i];
        if (c == '\\' && i + 1 < len) { i += 2; continue; }
        if (c != '{') { i++; continue; }
        pp_fmt_chunk(p, fmt, txt + chunk, i - chunk, t);
        int depth = 1, j = i + 1;
        while (j < len && depth > 0) {
            char d = txt[j];
            if (d == '\\' && j + 1 < len) { j += 2; continue; }
            if (d == '{') depth++;
            else if (d == '}') depth--;
            j++;
        }
        if (depth != 0) {
            pp_err(p, "unbalanced '{' in interpolated string");
            return fmt;
        }
        Node *e = pp_interp_expr(p, txt + i + 1, j - i - 2, t);
        if (e) node_add(fmt, e);
        i = j;
        chunk = j;
    }
    pp_fmt_chunk(p, fmt, txt + chunk, len - chunk, t);
    return fmt;
}

static Node *pp_parse_postfix(Parser *p) {
    Node *l;
    {
        Token t = *pp_pk(p, 0);
        switch (t.kind) {
        case TK_STRING_LIT:
            p->pos++;
            /* An interpolated string splits into a NK_FMT node whose
             * items alternate literal chunks and parsed expressions. */
            if (pp_has_interp(t))
                return pp_interp_string(p, t);
            l = node_new(NK_LITERAL, t);
            break;
        case TK_INT_LIT: case TK_FLOAT_LIT: case TK_CHAR_LIT:
            p->pos++;
            l = node_new(NK_LITERAL, t);
            break;
        case TK_KW_TRUE: case TK_KW_FALSE:
            /* Boolean literals are ordinary literals; lowering reads
             * the token kind and presets the Bool type. */
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
        case TK_IDENT: case TK_KW_CLOSE: {
            /* The contextual keywords double as library function
             * names in callee position (heap::alloc, sys::close). */
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

/* Parses one prefix-unary or postfix operand without the trailing
 * cast chain; pp_parse_unary() wraps the result in as-casts. */
static Node *pp_unary_operand(Parser *p) {
    Token t = *pp_pk(p, 0);
    switch (t.kind) {
    case TK_MINUS: case TK_TILDE: case TK_KW_NOT: case TK_AMP:
    case TK_AT: case TK_KW_AWAIT: case TK_KW_ERROR:
        p->pos++;
        /* The operand is parsed without its cast chain: the chain
         * in pp_parse_unary() then binds to the whole unary
         * expression, so '@x as T' casts the dereferenced value and
         * '-x as T' casts the negation. */
        { Node *n = node_new(NK_UNARY, t); n->op = t.kind;
          n->child[0] = pp_unary_operand(p); return n; }
    case TK_KW_TRY:
        p->pos++;
        if (!p->cur_fn_err)
            pp_err(p, "'try' used in a function that does not return "
                      "an error type");
        { Node *n = node_new(NK_UNARY, t); n->op = TK_KW_TRY;
          n->child[0] = pp_unary_operand(p); return n; }
    case TK_KW_ALLOC:
        /* Heap allocation operator. The operand is a unary
         * expression: a type name (alloc Int32[16]) or a size
         * expression (alloc(n)) are the same single construct;
         * semantics decides which meaning applies. */
        p->pos++;
        { Node *n = node_new(NK_ALLOC, t);
          /* A type operand starts with '&' or an uppercase type
           * name (Int32, Point, Int32[16], ...); anything else is
           * a size expression like alloc(n). */
          const Token pk = *pp_pk(p, 0);
          int is_type = pk.kind == TK_AMP ||
                        (pk.kind == TK_IDENT && pk.len > 0 &&
                         pk.start[0] >= 'A' && pk.start[0] <= 'Z');
          if (is_type) {
              n->flags |= NF_TYPE_OPERAND;
              n->child[0] = pp_parse_type(p);
          } else {
              n->child[0] = pp_unary_operand(p);
          }
          return n; }
    case TK_KW_CHAN: {                      /* chan / chan N */
        p->pos++;
        Node *n = node_new(NK_CHAN_NEW, t);
        if (pp_at(p, TK_INT_LIT)) { n->child[0] = node_new(NK_LITERAL, *pp_pk(p, 0)); p->pos++; }
        return n;
    }
    case TK_SENDRECV:                       /* receive: <- ch */
        p->pos++;
        { Node *n = node_new(NK_RECV, t);
          n->child[0] = pp_unary_operand(p); return n; }
    default:
        return pp_parse_postfix(p);
    }
}

Node *pp_parse_unary(Parser *p) {
    Node *n = pp_unary_operand(p);
    /* The as-cast binds tighter than every binary operator and looser
     * than prefix unary, so '-x as T' casts the negation and 'x as T
     * + 1' adds to the cast result. Casts chain left to right. */
    while (pp_at(p, TK_KW_AS)) {
        Token at = *pp_pk(p, 0);
        p->pos++;
        Node *c = node_new(NK_CAST, at);
        c->op = TK_KW_AS;
        c->child[0] = n;
        c->child[1] = pp_parse_type(p);
        n = c;
    }
    return n;
}
