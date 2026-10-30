#ifndef PAXSY_AST_H
#define PAXSY_AST_H

/*
 * ast.h - node kinds, the Node record and per-kind flag bits.
 * Every parsed construct is represented as a Node. Children live in the
 * fixed child[4] slots whose role depends on the kind; variadic children
 * (statements, parameters, cases, ...) additionally live in the growable
 * items list.
 */

#include "token.h"

typedef struct Node Node;
struct Node {
    int kind;
    Token tok;      /* location and raw source text of the construct */
    char *name;     /* heap-owned identifier text, or NULL */
    int op;         /* operator token kind for NK_UNARY/NK_BINARY/NK_CAST */
    int flags;      /* bit flags defined below */
    Node *child[4]; /* fixed semantic slots a,b,c,d */
    Node **items;   /* growable list for variadic children */
    int item_count, item_cap;
};

enum NodeKind {
    NK_PROGRAM, NK_USE, NK_MODULE, NK_FUNC,
    NK_COMPTIME_CONST, NK_COMPTIME_FN, NK_COMPTIME_BLOCK,
    NK_STRUCT, NK_UNION, NK_ENUM, NK_FIELD, NK_VARIANT, NK_PARAM,
    NK_TYPE_NAME, NK_TYPE_PTR, NK_TYPE_ARRAY, NK_TYPE_TUPLE,
    NK_TYPE_CHAN, NK_TYPE_ERROR,
    NK_BLOCK, NK_EXPR_STMT, NK_LET, NK_LET_MULTI, NK_DEFER, NK_RETURN,
    NK_IF, NK_IF_EXISTS, NK_MATCH, NK_MATCH_CASE, NK_WHILE, NK_DOWHILE,
    NK_FOR, NK_RANGE, NK_BREAK, NK_CONTINUE, NK_PASS, NK_LABEL, NK_ASM,
    NK_SELECT, NK_SELECT_ARM, NK_CLOSE, NK_ROUTINE, NK_ASYNC,
    NK_UNARY, NK_BINARY, NK_CALL, NK_INDEX, NK_SLICE, NK_FIELD_ACCESS,
    NK_NAME, NK_LITERAL, NK_ARRAY_LIT, NK_STRUCT_LIT, NK_NAMED_INIT,
    NK_SEND, NK_RECV, NK_CAST, NK_ALLOC, NK_CHAN_NEW, NK_VOLATILE,
    NK_CATCH
};

/* Flag bits used in Node.flags. */
#define NF_PROTOTYPE  0x02  /* NK_FUNC: declared without a body */
#define NF_COMPTIME   0x04  /* NK_PARAM: compile-time parameter */
#define NF_EXCLUSIVE  0x08  /* NK_RANGE/NK_SLICE: upper bound excluded */
#define NF_RECV       0x10  /* NK_SELECT_ARM: receive arm, else send arm */
#define NF_EXPR       0x20  /* NK_MATCH/NK_IF: parsed in expression position */
#define NF_OPEN_LO    0x40  /* NK_RANGE: lower bound omitted */
#define NF_OPEN_HI    0x80  /* NK_RANGE: upper bound omitted */

#endif /* PAXSY_AST_H */
