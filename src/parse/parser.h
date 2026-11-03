#ifndef PAXSY_PARSER_H
#define PAXSY_PARSER_H

/*
 * parser.h - recursive-descent parser interface.
 * The Parser struct carries everything the parsing functions need:
 * the token stream, the diagnostics sink, the error-unwinding state
 * and a context flag that resolves Paxsy's context-sensitive '{'
 * ambiguity:
 *   block_next - the token '{' ahead opens a block, never a struct
 *                literal (set while parsing conditions and match
 *                subjects); cleared while parsing initializers.
 * Error handling is statement-level recovery: pp_err() unwinds to the
 * innermost active recovery frame (installed by pp_parse_block,
 * pp_parse_module, pp_parse_comptime or parser_run), which skips the
 * broken statement and continues parsing; only when no frame is left
 * does the fatal on_error target abort the parse with NULL.
 */

#include <setjmp.h>
#include "ast.h"
#include "diag.h"
#include "lexer.h"

typedef struct {
    const Token *toks;
    int count, pos;
    Diag *diag;
    const char *path;
    int cur_fn_err;     /* enclosing function returns an error type */
    int block_next;     /* a following '{' opens a block, not a literal */
    jmp_buf on_error;   /* pp_err() unwinds here when no frame is left */
    int sync_depth;     /* active statement-recovery frames */
    int panicking;      /* an error was reported; skip re-reporting */
    jmp_buf sync_envs[32]; /* recovery target of each active frame */
} Parser;

Node *parser_run(const TokenVec *tv, Diag *diag, const char *path);

/* Parses exactly one expression from a token stream (used for string
 * interpolation fragments). Returns the expression node, or NULL when
 * the fragment does not parse. */
Node *parser_parse_expr(const TokenVec *tv, Diag *diag, const char *path);

#endif /* PAXSY_PARSER_H */
