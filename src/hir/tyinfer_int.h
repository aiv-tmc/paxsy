#ifndef PAXSY_TYINFER_INT_H
#define PAXSY_TYINFER_INT_H

/*
 * tyinfer_int.h - internals shared by the typing pass (tyinfer.c)
 * and its constant-folding helper (fold.c).
 */

#include "hir.h"

/* Attempts to fold one typed expression node in place; returns 1
 * when the node became a constant. */
int tfold_expr(HNode *n);

#endif /* PAXSY_TYINFER_INT_H */
