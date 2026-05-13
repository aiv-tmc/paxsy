#ifndef LITERALS_H
#define LITERALS_H

#include "../lexer/lexer.h"
#include "../errhandler/errhandler.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Parses a numeric literal (integer or floating point) according to the
 * language rules. Supports:
 *   - Binary (0b), octal (0o), decimal, hexadecimal (0x) prefixes.
 *   - Underscores as digit separators.
 *   - Period groups using parentheses, e.g., 3.(14)15.
 *   - Exponent notation with 'e' or 'E'.
 *
 * Returns a token of type TOKEN_NUMBER, or TOKEN_ERRORCODE on failure.
 */
Token literal__parse_number(Lexer *lexer);

/*
 * Parses a character literal enclosed in single quotes. Escape sequences are
 * processed. The literal must contain exactly one character after escapes.
 * Returns a token of type TOKEN_CHAR, or TOKEN_ERRORCODE on failure.
 */
Token literal__parse_char(Lexer *lexer);

/*
 * Parses a string literal enclosed in double quotes. Escape sequences and
 * embedded newlines are allowed. Returns a token of type TOKEN_STRING, or
 * TOKEN_ERRORCODE on failure.
 */
Token literal__parse_string(Lexer *lexer);

/*
 * Parses a sequence of adjacent string and character literals separated only
 * by whitespace, concatenating their contents into a single string token.
 * Example: "Hello" ' ' "World"  ->  "Hello World".
 * Returns a token of type TOKEN_STRING, or TOKEN_ERRORCODE on failure.
 */
Token literal__parse_concatenated(Lexer *lexer);

#endif
