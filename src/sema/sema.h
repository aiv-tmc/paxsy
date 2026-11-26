#ifndef PAXSY_SEMA_H
#define PAXSY_SEMA_H

/*
 * sema.h - semantic analysis interface.
 * sema_run() walks a parsed program, maintains scopes, resolves names
 * and enforces the semantic rules of the language (try placement,
 * match exhaustiveness in expression position, duplicate symbols,
 * unresolved identifiers, label targets, existence checks).
 * Returns the number of errors recorded.
 */

#include "ast.h"
#include "diag.h"

int sema_run(Node *program, Diag *diag, const char *path);

#endif /* PAXSY_SEMA_H */
