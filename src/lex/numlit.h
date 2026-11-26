#ifndef PAXSY_NUMLIT_H
#define PAXSY_NUMLIT_H

#include "diag.h"

/*
 * numlit.h - validation of numeric literal token slices.
 * The lexer consumes the maximal run of characters that can appear
 * inside a numeric literal and hands the raw slice to
 * numlit_validate(), which checks the digits against the literal
 * base, the placement of '_' separators and the exponent shape, and
 * derives the literal kind.
 */

/* Reports every violation of `s` (a slice of `len` characters starting
 * at one-based line/col) through `diag`; `src_path` is only forwarded.
 * Returns 1 when the slice forms a float literal, 0 for an integer. */
int numlit_validate(Diag *diag, const char *src_path, const char *s,
                    int len, int line, int col);

#endif /* PAXSY_NUMLIT_H */
