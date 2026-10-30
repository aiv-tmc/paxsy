#ifndef PAXSY_PARSER_INT_H
#define PAXSY_PARSER_INT_H

/*
 * parser_int.h - internal interface shared by the parser translation
 * units. Declares the token-stream helpers, the error primitive (which
 * reports and unwinds to parser_run via longjmp) and every parsing
 * function so the mutually recursive units can call each other.
 */

#include "parser.h"
#include "node.h"

Token *pp_pk(Parser *p, int off);
int pp_at(Parser *p, TokenKind kind);
int pp_ac(Parser *p, TokenKind kind);
Token *pp_ex(Parser *p, TokenKind kind, const char *what);
void pp_err(Parser *p, const char *fmt, ...);
char *pp_strndup(const char *s, int n);
char *pp_qualify(Parser *p);

Node *pp_parse_top(Parser *p);
Node *pp_parse_use(Parser *p);
Node *pp_parse_module(Parser *p);
Node *pp_parse_func(Parser *p);
Node *pp_parse_comptime(Parser *p);
Node *pp_parse_let(Parser *p);
Node *pp_parse_type(Parser *p);
Node *pp_parse_block(Parser *p);
Node *pp_parse_stmt(Parser *p);
Node *pp_parse_if(Parser *p, int in_expr);
Node *pp_parse_match(Parser *p, int in_expr);
Node *pp_parse_expr(Parser *p);
Node *pp_parse_bitor(Parser *p);
Node *pp_parse_unary(Parser *p);

#endif /* PAXSY_PARSER_INT_H */
