#include "common.h"
#include "dump.h"

/*
 * dump.c - human-readable rendering of tokens and AST nodes used by
 * the --tokens and --ast switches. Node kinds are printed by name;
 * named nodes show the name, operator nodes show the operator.
 */

static const char *node_kind_name(int k) {
    switch (k) {
    case NK_PROGRAM: return "program";
    case NK_USE: return "use";
    case NK_MODULE: return "module";
    case NK_FUNC: return "func";
    case NK_COMPTIME_CONST: return "comptime-const";
    case NK_COMPTIME_FN: return "comptime-fn";
    case NK_COMPTIME_BLOCK: return "comptime-block";
    case NK_STRUCT: return "struct";
    case NK_UNION: return "union";
    case NK_ENUM: return "enum";
    case NK_FIELD: return "field";
    case NK_VARIANT: return "variant";
    case NK_PARAM: return "param";
    case NK_TYPE_NAME: return "type-name";
    case NK_TYPE_PTR: return "type-ptr";
    case NK_TYPE_ARRAY: return "type-array";
    case NK_TYPE_TUPLE: return "type-tuple";
    case NK_TYPE_CHAN: return "type-chan";
    case NK_TYPE_ERROR: return "type-error";
    case NK_BLOCK: return "block";
    case NK_EXPR_STMT: return "expr-stmt";
    case NK_LET: return "let";
    case NK_LET_MULTI: return "let-multi";
    case NK_DEFER: return "defer";
    case NK_RETURN: return "return";
    case NK_IF: return "if";
    case NK_IF_EXISTS: return "if-exists";
    case NK_MATCH: return "match";
    case NK_MATCH_CASE: return "case";
    case NK_WHILE: return "while";
    case NK_DOWHILE: return "do-while";
    case NK_FOR: return "for";
    case NK_RANGE: return "range";
    case NK_BREAK: return "break";
    case NK_CONTINUE: return "continue";
    case NK_PASS: return "pass";
    case NK_LABEL: return "label";
    case NK_ASM: return "asm";
    case NK_SELECT: return "select";
    case NK_SELECT_ARM: return "arm";
    case NK_CLOSE: return "close";
    case NK_ROUTINE: return "routine";
    case NK_ASYNC: return "async";
    case NK_UNARY: return "unary";
    case NK_BINARY: return "binary";
    case NK_CALL: return "call";
    case NK_INDEX: return "index";
    case NK_SLICE: return "slice";
    case NK_FIELD_ACCESS: return "field-access";
    case NK_NAME: return "name";
    case NK_LITERAL: return "literal";
    case NK_ARRAY_LIT: return "array-lit";
    case NK_STRUCT_LIT: return "struct-lit";
    case NK_NAMED_INIT: return "named-init";
    case NK_SEND: return "send";
    case NK_RECV: return "recv";
    case NK_CAST: return "cast";
    case NK_ALLOC: return "alloc";
    case NK_CHAN_NEW: return "chan-new";
    case NK_VOLATILE: return "volatile";
    case NK_CATCH: return "catch";
    default: return "?";
    }
}

void dump_tokens(const TokenVec *tv, FILE *out) {
    for (int i = 0; i < tv->count; i++) {
        const Token *t = &tv->items[i];
        fprintf(out, "%4d:%-3d %-16s '%.*s'\n", t->line, t->col,
                token_kind_name(t->kind), t->len, t->start);
    }
}

static void dump_node(const Node *n, FILE *out, int depth) {
    if (!n) return;
    fprintf(out, "%*s%s", depth * 2, "", node_kind_name(n->kind));
    if (n->name) fprintf(out, " '%s'", n->name);
    if (n->kind == NK_LITERAL)
        fprintf(out, " '%.*s'", n->tok.len, n->tok.start);
    if (n->kind == NK_UNARY || n->kind == NK_BINARY || n->kind == NK_CAST)
        fprintf(out, " op='%s'", token_kind_name((TokenKind)n->op));
    fprintf(out, " (%d:%d)\n", n->tok.line, n->tok.col);
    for (int i = 0; i < 4; i++) dump_node(n->child[i], out, depth + 1);
    for (int i = 0; i < n->item_count; i++) dump_node(n->items[i], out, depth + 1);
}

void dump_ast(const Node *n, FILE *out, int depth) {
    dump_node(n, out, depth);
}
