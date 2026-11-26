#ifndef PAXSY_NODE_H
#define PAXSY_NODE_H

/*
 * node.h - node allocation and child-list helpers.
 * Nodes are individually heap-allocated and never freed; a frontend run
 * is short-lived, so this arena-style policy keeps the code simple.
 */

#include "ast.h"

Node *node_new(int kind, Token tok);
void node_add(Node *parent, Node *child);

#endif /* PAXSY_NODE_H */
