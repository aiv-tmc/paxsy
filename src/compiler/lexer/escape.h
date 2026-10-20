#ifndef ESCAPE_H
#define ESCAPE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Decode the escape sequences in the raw body of a string or character
 * literal (the bytes between the quotes) into the bytes the literal denotes.
 * Supported escapes: \n \t \r \0 \\ \" \' and \xHH (two hex digits).
 *
 * 'line'/'column' give the source position of raw[0]; diagnostics point at
 * the exact offending escape. Errors are reported but decoding continues
 * (an unknown escape keeps the character after the backslash), so one bad
 * literal does not cascade.
 *
 * When 'allow_nul' is false (string literals), a decoded NUL byte is an
 * error and is not emitted; the terminator is appended implicitly.
 *
 * Returns a newly allocated buffer holding *out_len decoded bytes (not
 * NUL-terminated by the decoder beyond a trailing zero for convenience),
 * or NULL on allocation failure. The caller owns the buffer.
 */
char* escape__decode(const char* raw, uint32_t raw_len,
                     uint32_t line, uint32_t column,
                     bool allow_nul, uint32_t* out_len);

#endif
