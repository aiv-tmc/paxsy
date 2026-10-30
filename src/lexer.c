#include "common.h"
#include "lexer.h"
#include "source.h"

/*
 * lexer.c - hand-written tokenizer for Paxsy source text.
 *
 * The lexer is a small state machine over the source buffer with one
 * character of effective lookahead (peek). Responsibilities:
 *   - whitespace and comment skipping (// line, block)
 *   - identifiers and keywords (Struct/Union/Enum are keywords)
 *   - integer literals in bases 2/8/10/16 with '_' separators and U
 *     suffix; float literals with '.', e/E or hex p/P exponents and f
 *     suffix
 *   - string literals with escape sequences and {interpolation} brace
 *     balancing; char literals including multi-byte code units
 *   - the full operator set incl. <<=, >>>=, ::, .., <-
 */

typedef struct {
    const Source *src;
    const char *text;
    size_t len, pos;
    int line, col;     /* position of src->text[pos] */
    Diag *diag;
    TokenVec *out;
} Lexer;

static char lx_peek(Lexer *l, int n) {
    return (l->pos + (size_t)n < l->len) ? l->text[l->pos + (size_t)n] : '\0';
}

static char lx_advance(Lexer *l) {
    char c = lx_peek(l, 0);
    if (c == '\n') { l->line++; l->col = 1; } else l->col++;
    if (c) l->pos++;
    return c;
}

static void lx_emit(Lexer *l, TokenKind kind, const char *start, int len,
                    int line, int col) {
    if (l->out->count == l->out->cap) {
        l->out->cap = l->out->cap ? l->out->cap * 2 : 256;
        l->out->items = realloc(l->out->items,
                                (size_t)l->out->cap * sizeof(Token));
        if (!l->out->items) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    Token *t = &l->out->items[l->out->count++];
    t->kind = kind; t->start = start; t->len = len;
    t->line = line; t->col = col;
}

void token_vec_free(TokenVec *v) {
    free(v->items);
    memset(v, 0, sizeof(*v));
}

/* Whitespace and comments. Block comments are not nested. */
static void lex_skip_trivia(Lexer *l) {
    for (;;) {
        char c = lx_peek(l, 0);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '\f' || c == '\v') { lx_advance(l); continue; }
        if (c == '/' && lx_peek(l, 1) == '/') {
            while (lx_peek(l, 0) && lx_peek(l, 0) != '\n') lx_advance(l);
            continue;
        }
        if (c == '/' && lx_peek(l, 1) == '*') {
            int sl = l->line, sc = l->col;
            lx_advance(l); lx_advance(l);
            while (lx_peek(l, 0) &&
                   !(lx_peek(l, 0) == '*' && lx_peek(l, 1) == '/'))
                lx_advance(l);
            if (!lx_peek(l, 0)) {
                diag_report(l->diag, DK_ERROR, l->src->path, sl, sc,
                            "unterminated block comment");
                return;
            }
            lx_advance(l); lx_advance(l);
            continue;
        }
        return;
    }
}

static void lex_ident(Lexer *l, int line, int col) {
    const char *start = l->text + l->pos;
    while (isalnum((unsigned char)lx_peek(l, 0)) || lx_peek(l, 0) == '_')
        lx_advance(l);
    int len = (int)(l->text + l->pos - start);
    lx_emit(l, token_lookup_keyword(start, len), start, len, line, col);
}

static int is_hexd(char c) { return isxdigit((unsigned char)c); }

/* Numbers: 0b/0o/0x prefixes, '_' separators, suffixes U and f. */
static void lex_number(Lexer *l, int line, int col) {
    const char *start = l->text + l->pos;
    int is_float = 0;
    if (lx_peek(l, 0) == '0' && (lx_peek(l, 1) == 'x' || lx_peek(l, 1) == 'X')) {
        lx_advance(l); lx_advance(l);
        while (is_hexd(lx_peek(l, 0)) || lx_peek(l, 0) == '_') lx_advance(l);
        if (lx_peek(l, 0) == '.' && lx_peek(l, 1) != '.') {
            is_float = 1; lx_advance(l);
            while (is_hexd(lx_peek(l, 0)) || lx_peek(l, 0) == '_') lx_advance(l);
        }
        if (lx_peek(l, 0) == 'p' || lx_peek(l, 0) == 'P') {
            is_float = 1; lx_advance(l);
            if (lx_peek(l, 0) == '+' || lx_peek(l, 0) == '-') lx_advance(l);
            while (isdigit((unsigned char)lx_peek(l, 0)) || lx_peek(l, 0) == '_')
                lx_advance(l);
        }
    } else if (lx_peek(l, 0) == '0' &&
               (lx_peek(l, 1) == 'b' || lx_peek(l, 1) == 'B')) {
        lx_advance(l); lx_advance(l);
        while (lx_peek(l, 0) == '0' || lx_peek(l, 0) == '1' ||
               lx_peek(l, 0) == '_') lx_advance(l);
    } else if (lx_peek(l, 0) == '0' &&
               (lx_peek(l, 1) == 'o' || lx_peek(l, 1) == 'O')) {
        lx_advance(l); lx_advance(l);
        while ((lx_peek(l, 0) >= '0' && lx_peek(l, 0) <= '7') ||
               lx_peek(l, 0) == '_') lx_advance(l);
    } else {
        while (isdigit((unsigned char)lx_peek(l, 0)) || lx_peek(l, 0) == '_')
            lx_advance(l);
        if (lx_peek(l, 0) == '.' && lx_peek(l, 1) != '.') {
            is_float = 1; lx_advance(l);
            while (isdigit((unsigned char)lx_peek(l, 0)) || lx_peek(l, 0) == '_')
                lx_advance(l);
        }
        if (lx_peek(l, 0) == 'e' || lx_peek(l, 0) == 'E') {
            is_float = 1; lx_advance(l);
            if (lx_peek(l, 0) == '+' || lx_peek(l, 0) == '-') lx_advance(l);
            while (isdigit((unsigned char)lx_peek(l, 0)) || lx_peek(l, 0) == '_')
                lx_advance(l);
        }
    }
    if (lx_peek(l, 0) == 'U' || lx_peek(l, 0) == 'u') lx_advance(l);
    else if (lx_peek(l, 0) == 'f' || lx_peek(l, 0) == 'F') {
        is_float = 1; lx_advance(l);
    }
    lx_emit(l, is_float ? TK_FLOAT_LIT : TK_INT_LIT, start,
            (int)(l->text + l->pos - start), line, col);
}

/* On malformed input this reports the error and resynchronizes at the
 * next newline without emitting a token. */
static void lex_string(Lexer *l, int line, int col) {
    lx_advance(l);                     /* opening quote */
    const char *start = l->text + l->pos;
    int depth = 0;
    while (lx_peek(l, 0) && lx_peek(l, 0) != '"') {
        if (lx_peek(l, 0) == '\n') {
            diag_report(l->diag, DK_ERROR, l->src->path, line, col,
                        "unterminated string literal");
            return;                    /* resync at the newline */
        }
        if (lx_peek(l, 0) == '\\') {
            lx_advance(l);
            if (!lx_peek(l, 0)) break;
            lx_advance(l);
            continue;
        }
        if (lx_peek(l, 0) == '{') depth++;
        else if (lx_peek(l, 0) == '}' && depth > 0) depth--;
        lx_advance(l);
    }
    if (!lx_peek(l, 0)) {
        diag_report(l->diag, DK_ERROR, l->src->path, line, col,
                    "unterminated string literal");
        return;
    }
    if (depth != 0)
        diag_report(l->diag, DK_ERROR, l->src->path, line, col,
                    "unbalanced braces in string interpolation");
    lx_emit(l, TK_STRING_LIT, start, (int)(l->text + l->pos - start), line, col);
    lx_advance(l);                     /* closing quote */
}

static void lex_char(Lexer *l, int line, int col) {
    lx_advance(l);                     /* opening quote */
    const char *start = l->text + l->pos;
    if (lx_peek(l, 0) == '\\') {
        lx_advance(l);
        if (!lx_peek(l, 0)) {
            diag_report(l->diag, DK_ERROR, l->src->path, line, col,
                        "unterminated char literal");
            return;
        }
        lx_advance(l);
    } else if (lx_peek(l, 0) && lx_peek(l, 0) != '\'' &&
               lx_peek(l, 0) != '\n') {
        lx_advance(l);
    }
    /* Consume remaining code units up to the closing quote so that
     * multi-byte code units such as 'e'-with-accent stay intact. */
    while (lx_peek(l, 0) && lx_peek(l, 0) != '\'') {
        if (lx_peek(l, 0) == '\n') break;
        lx_advance(l);
    }
    if (!lx_peek(l, 0)) {
        diag_report(l->diag, DK_ERROR, l->src->path, line, col,
                    "unterminated char literal");
        return;
    }
    lx_emit(l, TK_CHAR_LIT, start, (int)(l->text + l->pos - start), line, col);
    lx_advance(l);                     /* closing quote */
}

/* Longest-match dispatch for every operator and punctuation token. */
static void lex_op(Lexer *l, int line, int col) {
    const char *start = l->text + l->pos;
    char c = lx_peek(l, 0);
    lx_advance(l);
    TokenKind k;
    switch (c) {
    case '(': k = TK_LPAREN; break;   case ')': k = TK_RPAREN; break;
    case '{': k = TK_LBRACE; break;   case '}': k = TK_RBRACE; break;
    case '[': k = TK_LBRACKET; break; case ']': k = TK_RBRACKET; break;
    case ',': k = TK_COMMA; break;    case ';': k = TK_SEMI; break;
    case '~': k = TK_TILDE; break;    case '@': k = TK_AT; break;
    case '|': k = TK_PIPE; break;
    case '.': k = (lx_peek(l, 0) == '.') ? (lx_advance(l), TK_DOTDOT) : TK_DOT; break;
    case ':': k = (lx_peek(l, 0) == ':') ? (lx_advance(l), TK_COLONCOLON) : TK_COLON; break;
    case '+': k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_PLUS_EQ) : TK_PLUS; break;
    case '-': k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_MINUS_EQ) : TK_MINUS; break;
    case '*': k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_STAR_EQ) : TK_STAR; break;
    case '/': k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_SLASH_EQ) : TK_SLASH; break;
    case '%': k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_PERCENT_EQ) : TK_PERCENT; break;
    case '^': k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_CARET_EQ) : TK_CARET; break;
    case '&': k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_AMP_EQ) : TK_AMP; break;
    case '$': k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_DOLLAR_EQ) : TK_DOLLAR; break;
    case '=': k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_EQ) : TK_ASSIGN; break;
    case '!': k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_NE) : TK_BANG; break;
    case '<':
        if (lx_peek(l, 0) == '-') { lx_advance(l); k = TK_SENDRECV; }
        else if (lx_peek(l, 0) == '<') {
            lx_advance(l);
            k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_SHL_EQ) : TK_SHL;
        } else k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_LE) : TK_LT;
        break;
    case '>':
        if (lx_peek(l, 0) == '>') {
            lx_advance(l);
            if (lx_peek(l, 0) == '>') {
                lx_advance(l);
                k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_USHR_EQ) : TK_USHR;
            } else {
                k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_SHR_EQ) : TK_SHR;
            }
        } else k = (lx_peek(l, 0) == '=') ? (lx_advance(l), TK_GE) : TK_GT;
        break;
    default:
        diag_report(l->diag, DK_ERROR, l->src->path, line, col,
                    "unexpected character '%c'", c);
        return;                            /* skip the character */
    }
    lx_emit(l, k, start, (int)(l->text + l->pos - start), line, col);
}

static void lex_token(Lexer *l) {
    int line = l->line, col = l->col;
    char c = lx_peek(l, 0);
    if (isalpha((unsigned char)c) || c == '_') lex_ident(l, line, col);
    else if (isdigit((unsigned char)c)) lex_number(l, line, col);
    else if (c == '"') lex_string(l, line, col);
    else if (c == '\'') lex_char(l, line, col);
    else lex_op(l, line, col);
}

int lexer_run(const Source *src, TokenVec *out, Diag *diag) {
    Lexer l = { src, src->text, src->len, 0, 1, 1, diag, out };
    memset(out, 0, sizeof(*out));
    for (;;) {
        lex_skip_trivia(&l);
        if (l.pos >= l.len) {
            lx_emit(&l, TK_EOF, "", 0, l.line, l.col);
            break;
        }
        lex_token(&l);
        if (diag_error_count(diag) >= diag->max_errors) {
            lx_emit(&l, TK_EOF, "", 0, l.line, l.col);
            break;
        }
    }
    return diag_error_count(diag);
}
