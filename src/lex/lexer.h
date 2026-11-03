#ifndef PAXSY_LEXER_H
#define PAXSY_LEXER_H

/*
 * lexer.h - token stream production from a loaded source file.
 * The lexer never aborts on the first problem: it reports errors to the
 * diagnostics manager, skips the offending characters and keeps going so
 * that one run can surface several independent lexical errors.
 *
 * The Source record comes from source.h; including it here (rather than
 * re-declaring the typedef) keeps strict C99 translation units free of
 * the typedef-redefinition diagnostic that Clang emits otherwise.
 */

#include "token.h"
#include "diag.h"
#include "source.h"   /* the Source record itself */

typedef struct {
    Token *items;
    int count, cap;
} TokenVec;

void token_vec_free(TokenVec *v);

/* Tokenizes the whole source. Returns the number of errors recorded. */
int lexer_run(const Source *src, TokenVec *out, Diag *diag);

#endif /* PAXSY_LEXER_H */
