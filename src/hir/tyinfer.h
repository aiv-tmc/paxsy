#ifndef PAXSY_TYINFER_H
#define PAXSY_TYINFER_H

/*
 * tyinfer.h - the HIR typing pass.
 * hir_tyinfer() walks one lowered module and assigns concrete types
 * where the source left them open: integer literals receive the
 * smallest platform-independent signed type that holds their value
 * (the family rule of the language specification), 'let' bindings
 * without an annotation inherit their initializer's type, binary
 * operations widen to their wider operand, and global references are
 * bound to the declaration they name. Calls to functions that are not
 * declared in the module are external symbols and stay untyped.
 * Returns the number of errors found; diagnostics carry the origin
 * token of the offending node.
 */

#include "hir.h"
#include "diag.h"

int hir_tyinfer(HModule *m, Diag *diag);

#endif /* PAXSY_TYINFER_H */
