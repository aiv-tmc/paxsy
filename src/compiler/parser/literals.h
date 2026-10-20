#ifndef LITERALS_H
#define LITERALS_H

#include "../lexer/lexer.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Parses a numeric literal (integer or floating point) according to the language rules.
 * Supports binary (0b), octal (0o), decimal, and hexadecimal (0x) prefixes,
 * underscores as separators, and exponent notation for floating point.
 * Recognises a 'U' suffix on decimal integers (unsigned) and an 'f' suffix
 * on floating-point literals; any other trailing letter is an error.
 *
 * Returns a token of type TOKEN_NUMBER_LITERAL, or TOKEN_ERRORCODE on failure.
 * The token's 'value' is the raw literal text including suffix.
 */
Token literal__parse_number(Lexer *lexer);

/*
 * Parses a character literal enclosed in single quotes. The body is
 * validated: after escape expansion it must denote exactly one code point.
 * Returns a token of type TOKEN_CHAR_LITERAL, or TOKEN_ERRORCODE on failure.
 * The token's 'value' is the raw literal text including the quotes.
 */
Token literal__parse_char(Lexer *lexer);

/*
 * Parses a string literal enclosed in double quotes. Embedded newlines are
 * supported. If the string contains an unescaped '{' followed by an
 * expression (until '}'), the token type becomes TOKEN_INTERPOLATED_STRING.
 * Returns a token of type TOKEN_STRING_LITERAL or TOKEN_INTERPOLATED_STRING,
 * or TOKEN_ERRORCODE on failure.
 * The token's 'value' is the raw literal text including the quotes; escape
 * sequences are decoded by the concatenation stage or the parser.
 */
Token literal__parse_string(Lexer *lexer);

/*
 * Parses a sequence of adjacent string and character literals separated only
 * by whitespace, concatenating them into a single token.
 * A lone character literal yields a TOKEN_CHAR_LITERAL carrying the decoded
 * code point. Plain string results carry the decoded text; interpolated
 * results carry the raw text and are decoded per literal segment by the
 * parser.
 * Returns a token of type TOKEN_STRING_LITERAL, TOKEN_CHAR_LITERAL or
 * TOKEN_INTERPOLATED_STRING, or TOKEN_ERRORCODE on failure.
 */
Token literal__parse_concatenated(Lexer *lexer);

#endif
