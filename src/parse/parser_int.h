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
void pp_err_previous_help(Parser *p, const char *message, const char *help);
void pp_sync(Parser *p);
char *pp_strndup(const char *s, int n);
char *pp_qualify(Parser *p);

/* True when the token kind may serve as a member name directly after
 * '::' (identifiers plus the alloc/free memory keywords, which stay
 * operators in expression position but may name qualified members). */
int pp_member_kind(TokenKind k);

/* The keywords 'alloc', 'free' and 'close' act as operators in
 * statement position but double as library function names
 * (std::heap::alloc, std::sys::close) everywhere a name is read.
 * One predicate keeps the three accepting sites in agreement. */
static inline int pp_is_name_tok(TokenKind k) {
    return k == TK_KW_ALLOC || k == TK_KW_FREE || k == TK_KW_CLOSE;
}
Token pp_name_ex(Parser *p, const char *what);

Node *pp_parse_top(Parser *p);
Node *pp_parse_use(Parser *p);
Node *pp_parse_module(Parser *p);
Node *pp_parse_func(Parser *p);
Node *pp_parse_static(Parser *p);
Node *pp_parse_let(Parser *p);
Node *pp_parse_typed_struct_init(Parser *p, Node *type);
Node *pp_parse_type(Parser *p);
Node *pp_parse_block(Parser *p);
Node *pp_parse_stmt(Parser *p);
Node *pp_parse_if(Parser *p, int in_expr);
Node *pp_parse_match(Parser *p, int in_expr);
Node *pp_parse_expr(Parser *p);
Node *pp_parse_assign(Parser *p);
Node *pp_parse_or(Parser *p);
Node *pp_parse_and(Parser *p);
Node *pp_parse_bitxor(Parser *p);
Node *pp_parse_bitand(Parser *p);
Node *pp_parse_eq(Parser *p);
Node *pp_parse_rel(Parser *p);
Node *pp_parse_shift(Parser *p);
Node *pp_parse_add(Parser *p);
Node *pp_parse_mul(Parser *p);
Node *pp_parse_catch(Parser *p);
Node *pp_parse_ternary(Parser *p);
Node *pp_parse_chan(Parser *p);
Node *pp_parse_await(Parser *p);
Node *pp_parse_range(Parser *p);
Node *pp_parse_bitor(Parser *p);
Node *pp_parse_unary(Parser *p);

#endif /* PAXSY_PARSER_INT_H */
