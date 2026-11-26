#ifndef PAXSY_DUMP_H
#define PAXSY_DUMP_H

/*
 * dump.h - human-readable rendering of compiler data structures.
 * dump_tokens() prints the lexed token stream; dump_ast() prints the
 * abstract syntax tree indented by depth.
 */

#include "lexer.h"
#include "ast.h"

void dump_tokens(const TokenVec *tv, FILE *out);
void dump_ast(const Node *n, FILE *out, int depth);

#endif /* PAXSY_DUMP_H */
