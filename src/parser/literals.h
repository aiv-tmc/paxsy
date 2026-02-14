#ifndef LITERALS_H
#define LITERALS_H

#include "../lexer/lexer.h"
#include "../errhandler/errhandler.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Parse a numeric literal (integer or floating‑point) according to language rules.
 * @param lexer: lexer instance
 * @return: token of type TOKEN_NUMBER (or TOKEN_ERROR on failure)
 */
Token literal__parse_number(Lexer* lexer);

/*
 * Parse a character literal (single quotes) with escape sequences.
 * @param lexer: lexer instance
 * @return: token of type TOKEN_CHAR (or TOKEN_ERROR on failure)
 */
Token literal__parse_char(Lexer* lexer);

/*
 * Parse a string literal (double quotes) with escape sequences and multiline support.
 * @param lexer: lexer instance
 * @return: token of type TOKEN_STRING (or TOKEN_ERROR on failure)
 */
Token literal__parse_string(Lexer* lexer);

/*
 * Parse and concatenate adjacent string and character literals into a single string token.
 * @param lexer: lexer instance
 * @return: token of type TOKEN_STRING (or TOKEN_ERROR on failure)
 */
Token literal__parse_concatenated(Lexer* lexer);

#endif
