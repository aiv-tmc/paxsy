#ifndef U__CHAR_UTILS_H
#define U__CHAR_UTILS_H

#include <stdint.h>

/**
 * Check if character is alphabetic (A-Z, a-z)
 * @param c: Character to check
 * @return: 1 if character is alphabetic, 0 otherwise
 */
int char_is_alpha(char c);

/**
 * Check if character is alphanumeric (A-Z, a-z, 0-9)
 * @param c: Character to check
 * @return: 1 if character is alphanumeric, 0 otherwise
 */
int char_is_alnum(char c);

/**
 * Check if character is a digit (0-9)
 * @param c: Character to check
 * @return: 1 if character is a digit, 0 otherwise
 */
int char_is_digit(char c);

/**
 * Check if character is hexadecimal digit (0-9, A-F, a-f)
 * @param c: Character to check
 * @return: 1 if character is hexadecimal digit, 0 otherwise
 */
int char_is_hex_digit(char c);

/**
 * Check if character is whitespace (space, tab, carriage return)
 * @param c: Character to check
 * @return: 1 if character is whitespace, 0 otherwise
 */
int char_is_whitespace(char c);

/**
 * Check if character can start an identifier (letter or underscore)
 * @param c: Character to check
 * @return: 1 if character can start identifier, 0 otherwise
 */
int char_is_identifier_start(char c);

/**
 * Check if character can be in identifier (alphanumeric or underscore)
 * @param c: Character to check
 * @return: 1 if character can be in identifier, 0 otherwise
 */
int char_is_identifier_char(char c);

/**
 * Check if character can be start of operator
 * @param c: Character to check
 * @return: 1 if character can start operator, 0 otherwise
 */
int char_is_operator_start(char c);

/**
 * Check if character is valid in a path (alphanumeric, underscore, dash, dot, slash)
 * @param c: Character to check
 * @return: 1 if character is valid in path, 0 otherwise
 */
int char_is_path_char(char c);

/**
 * Check if character is a line break character
 * @param c: Character to check
 * @return: 1 if character is newline or carriage return, 0 otherwise
 */
int char_is_line_break(char c);

/**
 * Convert character to lowercase
 * @param c: Character to convert
 * @return: Lowercase version of character
 */
char char_to_lower(char c);

/**
 * Convert character to uppercase
 * @param c: Character to convert
 * @return: Uppercase version of character
 */
char char_to_upper(char c);

/**
 * Check if character is printable (not control character)
 * @param c: Character to check
 * @return: 1 if character is printable, 0 otherwise
 */
int char_is_printable(char c);

/**
 * Check if character is a valid escape sequence character
 * @param c: Character to check
 * @return: 1 if character can be escaped, 0 otherwise
 */
int char_is_escape_char(char c);

#endif
