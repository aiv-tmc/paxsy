#include "common.h"
#include "node.h"

/*
 * node.c - constructors for the AST.
 */

Node *node_new(int kind, Token tok) {
    Node *n = calloc(1, sizeof(Node));
    if (!n) { fprintf(stderr, "out of memory\n"); exit(1); }
    n->kind = kind;
    n->tok = tok;
    return n;
}

void node_add(Node *parent, Node *child) {
    if (parent->item_count == parent->item_cap) {
        parent->item_cap = parent->item_cap ? parent->item_cap * 2 : 8;
        parent->items = realloc(parent->items,
                              (size_t)parent->item_cap * sizeof(Node *));
        if (!parent->items) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    parent->items[parent->item_count++] = child;
}
