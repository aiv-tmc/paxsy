#ifndef PAXSY_PARSER_H
#define PAXSY_PARSER_H

/*
 * parser.h - recursive-descent parser interface.
 * The Parser struct carries everything the parsing functions need:
 * the token stream, the diagnostics sink, the longjmp target used for
 * error unwinding, and a context flag that resolves Paxsy's
 * context-sensitive '{' ambiguity:
 *   block_next - the token '{' ahead opens a block, never a struct
 *                literal (set while parsing conditions and match
 *                subjects); cleared while parsing initializers.
 * parser_run() parses a complete token stream into a NK_PROGRAM node;
 * it records errors into diag and returns NULL when parsing fails.
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
    jmp_buf on_error;   /* pp_err() unwinds here */
} Parser;

Node *parser_run(const TokenVec *tv, Diag *diag, const char *path);

#endif /* PAXSY_PARSER_H */
