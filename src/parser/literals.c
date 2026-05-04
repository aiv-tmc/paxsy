#include "literals.h"
#include "../errhandler/errhandler.h"
#include "../lexer/lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Macro to safely advance the lexer position and column */
#define ADVANCE_LEXER(lexer) do { (lexer)->position++; (lexer)->column++; } while (0)

/* Macro to obtain the current source character without advancing */
#define CURRENT_CHAR(lexer) ((lexer)->source[(lexer)->position])

/*
 * Skips any whitespace characters (space, tab, newline) and updates line/column
 * information accordingly.
 */
static inline void skip_whitespace(Lexer* lexer) {
    const char* source = lexer->source;
    uint32_t pos = lexer->position;
    uint16_t line = lexer->line;
    uint16_t col = lexer->column;

    while (pos < lexer->source_length) {
        char c = source[pos];
        if (c == ' ' || c == '\t') {
            pos++;
            col++;
        } else if (c == '\n') {
            pos++;
            line++;
            col = 1;
        } else {
            break;
        }
    }
    lexer->position = pos;
    lexer->line = line;
    lexer->column = col;
}

/*
 * Checks whether the next non‑whitespace character is a quote (single or double),
 * i.e. whether another string or character literal follows immediately.
 */
static inline int is_next_string_or_char(Lexer* lexer) {
    Lexer temp = *lexer;
    skip_whitespace(&temp);
    if (temp.position >= temp.source_length) return 0;
    char c = temp.source[temp.position];
    return (c == '"' || c == '\'');
}

/* Checks if a character is a valid digit for the given numeric base. */
static inline bool is_valid_digit_for_base(char c, uint8_t base) {
    switch (base) {
        case 2:  return (c == '0' || c == '1');
        case 8:  return (c >= '0' && c <= '7');
        case 10: return (c >= '0' && c <= '9');
        case 16:
            return (c >= '0' && c <= '9') ||
                   (c >= 'A' && c <= 'F') ||
                   (c >= 'a' && c <= 'f');
        default: return false;
    }
}

/*
 * Skips any number of underscore characters used as digit separators.
 */
static inline void skip_underscores(Lexer* lexer) {
    while (lexer->position < lexer->source_length &&
           lexer->source[lexer->position] == '_') {
        ADVANCE_LEXER(lexer);
    }
}

/*
 * Ensures that the given buffer has at least required_capacity bytes.
 * If the current buffer is too small, it is reallocated.
 * Returns true on success, false on memory allocation failure.
 */
static bool ensure_buffer_capacity(char** buffer, uint32_t* buf_size, uint32_t required_capacity) {
    if (*buf_size >= required_capacity) return true;
    uint32_t new_size = *buf_size;
    while (new_size < required_capacity) new_size *= 2;
    char* new_buf = (char*)realloc(*buffer, new_size);
    if (!new_buf) return false;
    *buffer = new_buf;
    *buf_size = new_size;
    return true;
}

/*
 * Parses the integer part of a numeric literal in the specified base.
 * Returns true if at least one valid digit was consumed.
 */
static bool parse_integer_part(Lexer* lexer, uint8_t base) {
    bool has_digits = false;
    const char* input = lexer->source;

    while (lexer->position < lexer->source_length) {
        skip_underscores(lexer);
        if (lexer->position >= lexer->source_length) break;
        char c = input[lexer->position];
        /* Stop if we encounter a character that cannot belong to the integer part */
        if (c == '.' || c == '(' || c == 'e' || c == 'E') break;
        if (!is_valid_digit_for_base(c, base)) {
            if (has_digits) break;
            return false;
        }
        has_digits = true;
        ADVANCE_LEXER(lexer);
    }
    return has_digits;
}

/*
 * Parses a period group of the form "(digits)" used in base‑10 floating literals.
 */
static bool parse_period_part(Lexer* lexer, uint8_t base) {
    const char* input = lexer->source;
    if (lexer->position >= lexer->source_length || input[lexer->position] != '(')
        return false;
    ADVANCE_LEXER(lexer);  /* consume '(' */

    if (!parse_integer_part(lexer, base)) {
        errhandler__report_error(ERROR_CODE_SYNTAX_GENERIC,
                                 lexer->line, lexer->column, "syntax",
                                 "Empty period in number literal");
        return false;
    }
    if (lexer->position >= lexer->source_length || input[lexer->position] != ')') {
        errhandler__report_error(ERROR_CODE_SYNTAX_GENERIC,
                                 lexer->line, lexer->column, "syntax",
                                 "Unclosed period in number literal");
        return false;
    }
    ADVANCE_LEXER(lexer);  /* consume ')' */
    return true;
}

/*
 * Parses the exponent part of a floating literal: [eE][+-]? digits.
 */
static bool parse_exponent_part(Lexer* lexer) {
    const char* input = lexer->source;
    if (lexer->position >= lexer->source_length) return false;
    char c = input[lexer->position];
    if (c != 'e' && c != 'E') return false;
    ADVANCE_LEXER(lexer);

    if (lexer->position < lexer->source_length) {
        c = input[lexer->position];
        if (c == '+' || c == '-') {
            ADVANCE_LEXER(lexer);
        }
    }
    return parse_integer_part(lexer, 10);
}

/*
 * Parses a complete numeric literal (integer or floating point). Supports:
 *   - Binary (0b), octal (0o), decimal, hexadecimal (0x) prefixes.
 *   - Underscores as digit separators.
 *   - Period groups for base‑10 floating literals, e.g. 3.(14)15.
 *   - Exponent notation [eE][+-]?digits (base 10 only).
 * Returns a token of type TOKEN_NUMBER or TOKEN_ERRORCODE on failure.
 */
Token literal__parse_number(Lexer* lexer) {
    const char* input = lexer->source;
    const uint32_t start_pos = lexer->position;
    const uint16_t start_line = lexer->line;
    const uint16_t start_col = lexer->column;

    uint8_t base = 10;
    bool has_prefix = false;
    bool is_integer_only = false;
    bool error = false;

    /* Handle base prefixes: 0b, 0o, 0x */
    if (lexer->position < lexer->source_length && input[lexer->position] == '0') {
        if (lexer->position + 1 < lexer->source_length) {
            char next = input[lexer->position + 1];
            if (next == 'b' || next == 'B') {
                base = 2; has_prefix = true; is_integer_only = true;
            } else if (next == 'o' || next == 'O') {
                base = 8; has_prefix = true; is_integer_only = true;
            } else if (next == 'x' || next == 'X') {
                base = 16; has_prefix = true; is_integer_only = true;
            }
            if (has_prefix) {
                lexer->position += 2;
                lexer->column += 2;
                if (lexer->position >= lexer->source_length ||
                    !is_valid_digit_for_base(input[lexer->position], base)) {
                    errhandler__report_error(ERROR_CODE_LEXER_INVALID_NUMBER,
                                             lexer->line, lexer->column, "syntax",
                                             "Invalid digit after base prefix for base %d", base);
                    error = true;
                }
            }
        }
    }

    if (!error) {
        if (!parse_integer_part(lexer, base)) {
            errhandler__report_error(ERROR_CODE_LEXER_INVALID_NUMBER,
                                     lexer->line, lexer->column, "syntax",
                                     "Number must contain at least one digit");
            error = true;
        }
    }

    /* Parse fractional part: . digits [ (digits) ... ] */
    if (!error && lexer->position < lexer->source_length && input[lexer->position] == '.') {
        if (is_integer_only) {
            errhandler__report_error(ERROR_CODE_LEXER_INVALID_NUMBER,
                                     lexer->line, lexer->column, "syntax",
                                     "Fractional part not allowed for this base");
            error = true;
        } else {
            ADVANCE_LEXER(lexer);  /* consume '.' */
            bool has_fraction = false;
            if (parse_integer_part(lexer, base)) has_fraction = true;

            while (!error && lexer->position < lexer->source_length &&
                   input[lexer->position] == '(') {
                if (!parse_period_part(lexer, base)) {
                    error = true;
                    break;
                }
                has_fraction = true;
                if (parse_integer_part(lexer, base)) has_fraction = true;
            }

            if (!has_fraction && !error) {
                errhandler__report_error(ERROR_CODE_LEXER_INVALID_NUMBER,
                                         lexer->line, lexer->column, "syntax",
                                         "Empty fractional part");
                error = true;
            }
        }
    }

    /* Parse exponent part (only allowed for base 10) */
    if (!error && lexer->position < lexer->source_length &&
        (input[lexer->position] == 'e' || input[lexer->position] == 'E')) {
        uint32_t saved_pos = lexer->position;
        uint16_t saved_col = lexer->column;
        if (parse_exponent_part(lexer)) {
            if (is_integer_only) {
                errhandler__report_error(ERROR_CODE_LEXER_INVALID_NUMBER,
                                         lexer->line, lexer->column, "syntax",
                                         "Exponent notation not allowed for integer literals");
                error = true;
            }
        } else {
            /* Rollback: exponent was incomplete */
            lexer->position = saved_pos;
            lexer->column = saved_col;
        }
    }

    /* Build the token */
    Token token;
    token.line = start_line;
    token.column = start_col;
    token.length = lexer->position - start_pos;
    if (error) {
        token.type = TOKEN_ERRORCODE;
        token.value = NULL;
        /* Reset lexer to start of invalid literal */
        lexer->position = start_pos;
        lexer->column = start_col;
    } else {
        token.type = TOKEN_NUMBER;
        token.value = (char*)malloc(token.length + 1);
        if (token.value) {
            memcpy(token.value, input + start_pos, token.length);
            token.value[token.length] = '\0';
        } else {
            token.type = TOKEN_ERRORCODE;
        }
    }
    return token;
}

/*
 * Scans an escape sequence starting after a backslash. Returns the decoded
 * character and updates lexer position and column. Reports an error if the
 * sequence is invalid.
 */
static char parse_escape_sequence(Lexer* lexer) {
    const char* input = lexer->source;
    if (lexer->position >= lexer->source_length) {
        errhandler__report_error(ERROR_CODE_LEXER_INVALID_ESCAPE,
                                 lexer->line, lexer->column, "syntax",
                                 "Incomplete escape sequence");
        return 0;
    }
    char escaped = input[lexer->position];
    ADVANCE_LEXER(lexer);

    switch (escaped) {
        case 'b': return '\b';
        case 'f': return '\f';
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case '0': return '\0';
        case '"': return '"';
        case '\'': return '\'';
        case '\\': return '\\';
        case 'a': return '\a';
        case 'v': return '\v';
        case 'e': return '\x1B';
        default:
            errhandler__report_error(ERROR_CODE_LEXER_INVALID_ESCAPE,
                                     lexer->line, lexer->column, "syntax",
                                     "Unknown escape sequence: \\%c", escaped);
            return escaped;  /* Keep the character as fallback */
    }
}

/*
 * Parses the content between a given opening quote character and its matching
 * closing quote. Escape sequences are processed, newlines are kept as part of
 * the string. The function appends decoded characters to a caller‑supplied
 * dynamic buffer, reallocating it as necessary.
 *
 * Parameters:
 *   lexer        - current lexer state, must be positioned just *after* the
 *                  opening quote.
 *   quote_char   - the quote character used to start the literal.
 *   buffer       - pointer to a malloc'd buffer; will be reallocated if needed.
 *   buf_size     - pointer to the current size of buffer.
 *   out_length   - pointer where the final number of characters written will be
 *                  stored (excluding null terminator).
 *
 * Returns:
 *   true on success, false if a syntax error occurs (unclosed literal or memory
 *   allocation failure). On failure, the lexer state is undefined and the
 *   caller should not use the returned buffer.
 */
static bool parse_quoted_content(Lexer* lexer,
                                 char quote_char,
                                 char** buffer,
                                 uint32_t* buf_size,
                                 uint32_t* out_length) {
    const char* input = lexer->source;
    uint32_t write_pos = 0;

    while (lexer->position < lexer->source_length) {
        char c = input[lexer->position];

        if (c == quote_char) {
            ADVANCE_LEXER(lexer);  /* consume closing quote */
            *out_length = write_pos;
            (*buffer)[write_pos] = '\0';
            return true;
        }

        /* Ensure buffer capacity (need room for next char plus null) */
        if (!ensure_buffer_capacity(buffer, buf_size, write_pos + 2)) {
            errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                     lexer->line, lexer->column, "syntax",
                                     "Memory allocation failed");
            return false;
        }

        if (c == '\\') {
            ADVANCE_LEXER(lexer);  /* skip backslash */
            (*buffer)[write_pos++] = parse_escape_sequence(lexer);
        } else if (c == '\n') {
            /* Newline is part of the literal content */
            (*buffer)[write_pos++] = c;
            ADVANCE_LEXER(lexer);
            lexer->line++;
            lexer->column = 1;
        } else {
            (*buffer)[write_pos++] = c;
            ADVANCE_LEXER(lexer);
        }
    }

    /* Reached end of source without closing quote */
    errhandler__report_error(ERROR_CODE_LEXER_UNCLOSED_STRING,
                             lexer->line, lexer->column, "syntax",
                             "Unclosed literal");
    return false;
}

/*
 * Parses a character literal enclosed in single quotes. The literal must
 * contain exactly one character after escape processing. Returns a token of
 * type TOKEN_CHAR or TOKEN_ERRORCODE on failure.
 */
Token literal__parse_char(Lexer* lexer) {
    const char* input = lexer->source;
    const uint16_t start_line = lexer->line;
    const uint16_t start_col = lexer->column;

    Token token = {0};
    token.type = TOKEN_ERRORCODE;
    token.line = start_line;
    token.column = start_col;
    token.value = NULL;
    token.length = 0;

    if (lexer->position >= lexer->source_length || input[lexer->position] != '\'') {
        errhandler__report_error(ERROR_CODE_SYNTAX_GENERIC,
                                 lexer->line, lexer->column, "syntax",
                                 "Expected character literal");
        return token;
    }
    ADVANCE_LEXER(lexer);  /* skip opening quote */

    /* Use a temporary buffer (max 4 chars is enough because a character can be
       at most one Unicode code point, but here we only store a single byte). */
    char temp_buf[8];
    uint32_t buf_size = sizeof(temp_buf);
    char* buffer = temp_buf;
    uint32_t out_len = 0;
    bool success = parse_quoted_content(lexer, '\'', &buffer, &buf_size, &out_len);

    if (!success) {
        /* Error already reported */
        return token;
    }

    /* Character literal must contain exactly one decoded character */
    if (out_len != 1) {
        if (out_len == 0)
            errhandler__report_error(ERROR_CODE_SYNTAX_GENERIC,
                                     start_line, start_col, "syntax",
                                     "Empty character literal");
        else
            errhandler__report_error(ERROR_CODE_SYNTAX_GENERIC,
                                     start_line, start_col, "syntax",
                                     "Character literal must contain exactly one character");
        return token;
    }

    token.type = TOKEN_CHAR;
    token.length = 1;
    token.value = (char*)malloc(2);
    if (token.value) {
        token.value[0] = buffer[0];
        token.value[1] = '\0';
    } else {
        token.type = TOKEN_ERRORCODE;
    }

    /* If we used heap allocation inside parse_quoted_content, free it. */
    if (buffer != temp_buf) free(buffer);
    return token;
}

/*
 * Parses a string literal enclosed in double quotes. Escape sequences and
 * embedded newlines are handled. Returns a token of type TOKEN_STRING or
 * TOKEN_ERRORCODE on failure.
 */
Token literal__parse_string(Lexer* lexer) {
    const char* input = lexer->source;
    const uint16_t start_line = lexer->line;
    const uint16_t start_col = lexer->column;

    Token token = {0};
    token.type = TOKEN_ERRORCODE;
    token.line = start_line;
    token.column = start_col;
    token.value = NULL;
    token.length = 0;

    if (lexer->position >= lexer->source_length || input[lexer->position] != '"') {
        errhandler__report_error(ERROR_CODE_SYNTAX_GENERIC,
                                 lexer->line, lexer->column, "syntax",
                                 "Expected string literal");
        return token;
    }
    ADVANCE_LEXER(lexer);  /* skip opening quote */

    uint32_t buf_size = 128;
    char* buffer = (char*)malloc(buf_size);
    if (!buffer) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                 lexer->line, lexer->column, "syntax",
                                 "Memory allocation failed");
        return token;
    }

    uint32_t out_len = 0;
    bool success = parse_quoted_content(lexer, '"', &buffer, &buf_size, &out_len);

    if (!success) {
        free(buffer);
        return token;
    }

    token.type = TOKEN_STRING;
    token.length = out_len;
    token.value = (char*)malloc(out_len + 1);
    if (token.value) {
        memcpy(token.value, buffer, out_len + 1);
    } else {
        token.type = TOKEN_ERRORCODE;
    }
    free(buffer);
    return token;
}

/*
 * Parses a sequence of adjacent string and character literals separated only by
 * whitespace and concatenates their contents into a single string token.
 * Example: "Hello" ' ' "World"  ->  "Hello World".
 */
Token literal__parse_concatenated(Lexer* lexer) {
    const uint16_t start_line = lexer->line;
    const uint16_t start_col = lexer->column;

    Token combined = {0};
    combined.type = TOKEN_ERRORCODE;
    combined.line = start_line;
    combined.column = start_col;
    combined.value = NULL;
    combined.length = 0;

    uint32_t buf_size = 128;
    char* buffer = (char*)malloc(buf_size);
    if (!buffer) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                 start_line, start_col, "syntax",
                                 "Memory allocation failed");
        return combined;
    }
    uint32_t total_len = 0;

    while (lexer->position < lexer->source_length) {
        char c = CURRENT_CHAR(lexer);

        if (c == '\'') {
            Token ct = literal__parse_char(lexer);
            if (ct.type == TOKEN_ERRORCODE) {
                free(buffer);
                return ct;
            }
            if (ct.value) {
                /* Append the single character */
                if (!ensure_buffer_capacity(&buffer, &buf_size, total_len + 2)) {
                    free(ct.value);
                    free(buffer);
                    combined.type = TOKEN_ERRORCODE;
                    return combined;
                }
                buffer[total_len++] = ct.value[0];
                free(ct.value);
            }
        } else if (c == '"') {
            Token st = literal__parse_string(lexer);
            if (st.type == TOKEN_ERRORCODE) {
                free(buffer);
                return st;
            }
            if (st.value) {
                uint32_t add_len = st.length;
                if (!ensure_buffer_capacity(&buffer, &buf_size, total_len + add_len + 1)) {
                    free(st.value);
                    free(buffer);
                    combined.type = TOKEN_ERRORCODE;
                    return combined;
                }
                memcpy(buffer + total_len, st.value, add_len);
                total_len += add_len;
                free(st.value);
            }
        } else {
            break;  /* no more string/char literal */
        }

        /* Check if another literal follows after whitespace */
        if (!is_next_string_or_char(lexer)) {
            break;
        }
    }

    buffer[total_len] = '\0';
    combined.type = TOKEN_STRING;
    combined.length = total_len;
    combined.value = (char*)malloc(total_len + 1);
    if (combined.value) {
        memcpy(combined.value, buffer, total_len + 1);
    } else {
        combined.type = TOKEN_ERRORCODE;
    }
    free(buffer);
    return combined;
}
