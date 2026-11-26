#ifndef PAXSY_LOWER_H
#define PAXSY_LOWER_H

/*
 * lower.h - the AST to HIR lowering pass.
 * lower_run() walks one parsed program and produces the module-level
 * HIR for every declaration it understands, reporting a diagnostic
 * (and returning NULL) for constructs that require later phases
 * (static evaluation, channel typing). The AST stays alive for the
 * lifetime of the HIR: node names are borrowed, never copied.
 */

#include "ast.h"
#include "diag.h"
#include "hir.h"

HModule *lower_run(const Node *program, Diag *diag);
void lower_set_generic_sources(const Node *const *programs, int count);

#endif /* PAXSY_LOWER_H */
