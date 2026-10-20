#include "literals.h"
#include "../lexer/escape.h"
#include "../../interfaces/errhandler/errhandler.h"
#include "../../interfaces/utils/memory.h"
#include "../../interfaces/utils/str.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 *  Internal helpers
*/

/*
 *  Append a character to a dynamically growing buffer.
 *  Returns 0 on success, -1 on allocation failure.
 */
static int buffer_append_char(char **buf, size_t *cap, size_t *len, char c) {
    if (*len + 1 >= *cap) {
        size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
        char *new_buf = (char *)memory_reallocate_zero(*buf, *cap, new_cap);
        if (!new_buf) return -1;
        *buf = new_buf;
        *cap = new_cap;
    }
    (*buf)[(*len)++] = c;
    return 0;
}

/*
 *  Append a range of characters to a dynamic buffer.
 *  Returns 0 on success, -1 on allocation failure.
 */
static int buffer_append_range(char **buf, size_t *cap, size_t *len,
                               const char *start, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (buffer_append_char(buf, cap, len, start[i]) != 0) return -1;
    }
    return 0;
}

/*
 *  Consume a digit sequence in the given base, skipping underscores.
 *  Underscores are only allowed between digits (a trailing underscore
 *  is left unconsumed). Returns true if at least one digit was consumed.
 */
static bool consume_digits(Lexer *lexer, int base) {
    const char *src = lexer->source;
    uint32_t pos = lexer->position;
    bool has_digit = false;

    while (pos < lexer->source_length) {
        char c = src[pos];
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else if (c == '_' && has_digit && pos + 1 < lexer->source_length) {
            char n = src[pos + 1];
            int ndigit = -1;
            if (n >= '0' && n <= '9') ndigit = n - '0';
            else if (base == 16 && n >= 'A' && n <= 'F') ndigit = n - 'A' + 10;
            if (ndigit >= 0 && ndigit < base) { pos++; continue; }
            break;
        } else break;
        if (digit >= base) break;
        has_digit = true;
        pos++;
    }

    if (has_digit) lexer->position = pos;
    return has_digit;
}

/*
 *  Append a range of digit characters to a dynamic buffer, dropping
 *  underscore digit separators. Returns 0 on success, -1 on failure.
 */
static int buffer_append_digits(char **buf, size_t *cap, size_t *len,
                                const char *start, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (start[i] == '_') continue;
        if (buffer_append_char(buf, cap, len, start[i]) != 0) return -1;
    }
    return 0;
}

/*
 *  Consume an exponent part for decimal floating point: 'e' [sign] digits.
 *  Returns true on success.
 */
static bool consume_exponent(Lexer *lexer) {
    const char *src = lexer->source;
    if (lexer->position >= lexer->source_length || src[lexer->position] != 'e')
        return false;

    lexer->position++;
    if (lexer->position < lexer->source_length) {
        char c = src[lexer->position];
        if (c == '+' || c == '-') lexer->position++;
    }
    return consume_digits(lexer, 10);
}

/*
 *  Check if the current position looks like a hexadecimal floating literal
 *  (i.e. after "0x" there is a dot or a 'p' later). A dot directly followed
 *  by another dot is a range operator, not a fractional part.
 */
static bool is_hex_float_candidate(Lexer *lexer) {
    uint32_t pos = lexer->position;
    const char *src = lexer->source;
    while (pos < lexer->source_length) {
        char c = src[pos];
        if (isxdigit((unsigned char)c) || c == '_') pos++;
        else break;
    }
    if (pos < lexer->source_length) {
        char c = src[pos];
        if (c == 'p') return true;
        if (c == '.' && (pos + 1 >= lexer->source_length || src[pos + 1] != '.'))
            return true;
    }
    return false;
}

/*
 *  Create an error token with the current line/column.
 *  Advances the lexer position by at least one character to avoid infinite loops.
 */
static Token make_error_token(Lexer *lexer, uint32_t line, uint32_t col) {
    Token err;
    err.type = TOKEN_ERRORCODE;
    err.line = line;
    err.column = col;
    err.length = 0;
    err.value = NULL;

    /* Consume at least one character to prevent infinite loop */
    if (lexer->position < lexer->source_length) {
        lexer->position++;
        lexer->column++;
    }
    return err;
}

/*
 *  Numeric literal parsers
*/

/*
 *  Parse a decimal integer or floating-point literal.
 *  Handles fractional part, period groups, exponent, and suffixes.
 *  Returns a TOKEN_NUMBER_LITERAL or TOKEN_ERRORCODE.
 */
static Token parse_decimal_number(Lexer *lexer, uint32_t start_line,
                                  uint32_t start_col) {
    const char *src = lexer->source;
    bool is_float = false;
    bool error = false;

    char *buf = NULL;
    size_t cap = 0, len = 0;

    /* Integer part */
    uint32_t int_start = lexer->position;
    bool has_int = consume_digits(lexer, 10);
    if (has_int) {
        uint32_t int_end = lexer->position;
        if (buffer_append_digits(&buf, &cap, &len, src + int_start,
                                 int_end - int_start) != 0) {
            errhandler__report_error(start_line, start_col, "lexer",
                                     "Memory allocation failed");
            error = true;
            goto cleanup;
        }
    }

    /*
     * Fractional part: '.' followed by digits (or a period group).
     * A '.' followed by another '.' starts a range operator instead,
     * so it must not be consumed as part of the number.
     */
    if (!error && lexer->position < lexer->source_length &&
        src[lexer->position] == '.' &&
        lexer->position + 1 < lexer->source_length &&
        ((src[lexer->position + 1] >= '0' && src[lexer->position + 1] <= '9') ||
         src[lexer->position + 1] == '(')) {
        is_float = true;
        lexer->position++;
        if (buffer_append_char(&buf, &cap, &len, '.') != 0) {
            error = true;
            goto cleanup;
        }
        uint32_t frac_start = lexer->position;
        if (!consume_digits(lexer, 10)) {
            errhandler__report_error(lexer->line, lexer->column, "lexer",
                                     "Expected digits after decimal point");
            error = true;
            goto cleanup;
        }
        uint32_t frac_end = lexer->position;
        if (buffer_append_digits(&buf, &cap, &len, src + frac_start,
                                 frac_end - frac_start) != 0) {
            error = true;
            goto cleanup;
        }

        /* Period groups: .(digits) */
        while (!error && lexer->position < lexer->source_length &&
               src[lexer->position] == '(') {
            lexer->position++;
            if (buffer_append_char(&buf, &cap, &len, '(') != 0) {
                error = true;
                break;
            }
            uint32_t group_start = lexer->position;
            if (!consume_digits(lexer, 10)) {
                errhandler__report_error(lexer->line, lexer->column, "lexer",
                                         "Empty period group");
                error = true;
                break;
            }
            uint32_t group_end = lexer->position;
            if (lexer->position >= lexer->source_length ||
                src[lexer->position] != ')') {
                errhandler__report_error(lexer->line, lexer->column, "lexer",
                                         "Unclosed period group");
                error = true;
                break;
            }
            lexer->position++; // consume ')'
            if (buffer_append_digits(&buf, &cap, &len, src + group_start,
                                     group_end - group_start) != 0) {
                error = true;
                break;
            }
            if (buffer_append_char(&buf, &cap, &len, ')') != 0) {
                error = true;
                break;
            }
            /* allow trailing digits after group */
            consume_digits(lexer, 10);
        }
    }

    /* Exponent (also valid without a fractional part, e.g. 1e40) */
    if (!error && lexer->position < lexer->source_length &&
        src[lexer->position] == 'e') {
        is_float = true;
        uint32_t exp_start = lexer->position;
        if (!consume_exponent(lexer)) {
            errhandler__report_error(lexer->line, lexer->column, "lexer",
                                     "Invalid exponent");
            error = true;
        } else {
            uint32_t exp_end = lexer->position;
            if (buffer_append_digits(&buf, &cap, &len, src + exp_start,
                                     exp_end - exp_start) != 0) {
                error = true;
            }
        }
    }

    /* Suffixes */
    if (!error && lexer->position < lexer->source_length) {
        char nxt = src[lexer->position];
        if (nxt == 'U') {
            if (is_float) {
                errhandler__report_error(lexer->line, lexer->column, "lexer",
                                         "suffix not allowed on this literal");
                error = true;
            } else {
                lexer->position++;
                if (buffer_append_char(&buf, &cap, &len, 'U') != 0) error = true;
            }
        } else if (nxt == 'f') {
            if (!is_float) {
                errhandler__report_error(lexer->line, lexer->column, "lexer",
                                         "suffix not allowed on this literal");
                error = true;
            } else {
                lexer->position++;
                if (buffer_append_char(&buf, &cap, &len, 'f') != 0) error = true;
            }
        }
    }

    /* Any other letter directly after the literal is an invalid suffix */
    if (!error && lexer->position < lexer->source_length) {
        char nxt = src[lexer->position];
        if ((nxt >= 'a' && nxt <= 'z') || (nxt >= 'A' && nxt <= 'Z')) {
            errhandler__report_error(lexer->line, lexer->column, "lexer",
                                     "invalid numeric suffix");
            error = true;
        }
    }

cleanup:
    if (error || len == 0) {
        memory_free_safe((void **)&buf);
        return make_error_token(lexer, start_line, start_col);
    }

    buf[len] = '\0';
    char *dup = u__strduplic(buf);
    memory_free_safe((void **)&buf);
    if (!dup) return make_error_token(lexer, start_line, start_col);

    Token tok;
    tok.type = TOKEN_NUMBER_LITERAL;
    tok.line = start_line;
    tok.column = start_col;
    tok.length = (uint32_t)len;
    tok.value = dup;
    return tok;
}

/*
 *  Parse a hexadecimal floating literal (e.g., 0x1.2p3).
 *  Requires 'p' exponent and uppercase hex digits.
 *  The 'prefix' parameter points to the two-character base prefix (0x)
 *  already consumed by the caller; it is retained in the token value.
 *  Returns a TOKEN_NUMBER_LITERAL or TOKEN_ERRORCODE.
 */
static Token parse_hex_float(Lexer *lexer, const char *prefix,
                             uint32_t start_line, uint32_t start_col) {
    const char *src = lexer->source;
    bool error = false;
    bool has_digit_before_dot = false;
    bool has_digit_after_dot = false;
    bool seen_dot = false;

    char *buf = NULL;
    size_t cap = 0, len = 0;

    if (buffer_append_range(&buf, &cap, &len, prefix, 2) != 0) {
        memory_free_safe((void **)&buf);
        return make_error_token(lexer, start_line, start_col);
    }

    /* Mantissa: hex digits, underscores, and at most one dot */
    while (lexer->position < lexer->source_length) {
        char c = src[lexer->position];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) {
            if (buffer_append_char(&buf, &cap, &len, c) != 0) {
                error = true;
                break;
            }
            lexer->position++;
            if (seen_dot) has_digit_after_dot = true;
            else has_digit_before_dot = true;
        } else if (c == '_') {
            lexer->position++; // digit separator, not stored
        } else if (c == '.') {
            if (seen_dot) {
                errhandler__report_error(lexer->line, lexer->column, "lexer",
                                         "Multiple dots in hex float");
                error = true;
                break;
            }
            seen_dot = true;
            if (buffer_append_char(&buf, &cap, &len, '.') != 0) {
                error = true;
                break;
            }
            lexer->position++;
        } else {
            break;
        }
    }

    if (!error && lexer->position < lexer->source_length &&
        src[lexer->position] == '(') {
        errhandler__report_error(lexer->line, lexer->column, "lexer",
                                 "Period groups not allowed in hex float");
        error = true;
    }

    if (!error && !has_digit_before_dot && !has_digit_after_dot) {
        errhandler__report_error(lexer->line, lexer->column, "lexer",
                                 "Hex float must have at least one digit");
        error = true;
    }

    /* Exponent: 'p' followed by optional sign and decimal digits */
    if (!error && lexer->position < lexer->source_length &&
        src[lexer->position] == 'p') {
        if (buffer_append_char(&buf, &cap, &len, 'p') != 0) {
            error = true;
            goto cleanup;
        }
        lexer->position++;
        if (lexer->position < lexer->source_length &&
            (src[lexer->position] == '+' || src[lexer->position] == '-')) {
            if (buffer_append_char(&buf, &cap, &len, src[lexer->position]) != 0) {
                error = true;
                goto cleanup;
            }
            lexer->position++;
        }
        bool exp_has_digit = false;
        while (lexer->position < lexer->source_length) {
            char c = src[lexer->position];
            if (c >= '0' && c <= '9') {
                if (buffer_append_char(&buf, &cap, &len, c) != 0) {
                    error = true;
                    goto cleanup;
                }
                exp_has_digit = true;
                lexer->position++;
            } else {
                break;
            }
        }
        if (!exp_has_digit) {
            errhandler__report_error(lexer->line, lexer->column, "lexer",
                                     "Expected decimal digits in exponent");
            error = true;
            goto cleanup;
        }
    } else if (!error) {
        errhandler__report_error(lexer->line, lexer->column, "lexer",
                                 "Hex float requires 'p' exponent");
        error = true;
    }

    /* Optional 'f' suffix */
    if (!error && lexer->position < lexer->source_length &&
        src[lexer->position] == 'f') {
        if (buffer_append_char(&buf, &cap, &len, 'f') != 0) {
            error = true;
            goto cleanup;
        }
        lexer->position++;
    }

    /* Any other letter directly after the literal is an invalid suffix */
    if (!error && lexer->position < lexer->source_length) {
        char nxt = src[lexer->position];
        if ((nxt >= 'a' && nxt <= 'z') || (nxt >= 'A' && nxt <= 'Z')) {
            errhandler__report_error(lexer->line, lexer->column, "lexer",
                                     "invalid numeric suffix");
            error = true;
        }
    }

cleanup:
    if (error || len == 0) {
        memory_free_safe((void **)&buf);
        return make_error_token(lexer, start_line, start_col);
    }

    buf[len] = '\0';
    char *dup = u__strduplic(buf);
    memory_free_safe((void **)&buf);
    if (!dup) return make_error_token(lexer, start_line, start_col);

    Token tok;
    tok.type = TOKEN_NUMBER_LITERAL;
    tok.line = start_line;
    tok.column = start_col;
    tok.length = (uint32_t)len;
    tok.value = dup;
    return tok;
}

/*
 *  Parse a non‑decimal integer (binary, octal, plain hex).
 *  The 'prefix' parameter points to the two-character base prefix (0b/0o/0x)
 *  already consumed by the caller; it is retained in the token value so that
 *  later stages can recover the base.
 *  Returns a TOKEN_NUMBER_LITERAL or TOKEN_ERRORCODE.
 */
static Token parse_non_decimal_integer(Lexer *lexer, int base, const char *prefix,
                                       uint32_t start_line, uint32_t start_col) {
    const char *src = lexer->source;
    bool error = false;
    char *buf = NULL;
    size_t cap = 0, len = 0;

    if (buffer_append_range(&buf, &cap, &len, prefix, 2) != 0) {
        error = true;
        goto cleanup;
    }

    uint32_t digit_start = lexer->position;
    if (!consume_digits(lexer, base)) {
        errhandler__report_error(lexer->line, lexer->column, "lexer",
                                 "Expected at least one digit");
        error = true;
        goto cleanup;
    }
    uint32_t digit_end = lexer->position;
    if (buffer_append_digits(&buf, &cap, &len, src + digit_start,
                             digit_end - digit_start) != 0) {
        error = true;
        goto cleanup;
    }

    /* No fractional part allowed (a range operator '..' is fine) */
    if (!error && lexer->position < lexer->source_length &&
        src[lexer->position] == '.' &&
        (lexer->position + 1 >= lexer->source_length ||
         src[lexer->position + 1] != '.')) {
        errhandler__report_error(lexer->line, lexer->column, "lexer",
                                 "Fractional part not allowed in non‑decimal");
        error = true;
    }

    /* No suffixes are recognised in non-decimal literals; a lowercase
     * a-f after hexadecimal digits is an invalid digit, any other
     * trailing letter is an invalid suffix. */
    if (!error && lexer->position < lexer->source_length) {
        char nxt = src[lexer->position];
        if ((nxt >= 'a' && nxt <= 'z') || (nxt >= 'A' && nxt <= 'Z')) {
            if (base == 16 && nxt >= 'a' && nxt <= 'f') {
                errhandler__report_error(lexer->line, lexer->column, "lexer",
                                         "invalid digit");
            } else {
                errhandler__report_error(lexer->line, lexer->column, "lexer",
                                         "invalid numeric suffix");
            }
            error = true;
        }
    }

cleanup:
    if (error || len == 0) {
        memory_free_safe((void **)&buf);
        return make_error_token(lexer, start_line, start_col);
    }

    buf[len] = '\0';
    char *dup = u__strduplic(buf);
    memory_free_safe((void **)&buf);
    if (!dup) return make_error_token(lexer, start_line, start_col);

    Token tok;
    tok.type = TOKEN_NUMBER_LITERAL;
    tok.line = start_line;
    tok.column = start_col;
    tok.length = (uint32_t)len;
    tok.value = dup;
    return tok;
}

/*
 *  Public numeric literal entry point
*/

/*
 *  Parse a numeric literal from the lexer's current position.
 *  Detects base prefixes (0b, 0o, 0x) and dispatches to the appropriate
 *  parser. Returns a token of type TOKEN_NUMBER_LITERAL or TOKEN_ERRORCODE.
 */
Token literal__parse_number(Lexer *lexer) {
    const char *src = lexer->source;
    uint32_t start_line = lexer->line;
    uint32_t start_col = lexer->column;
    uint32_t start_pos = lexer->position;
    Token result;

    if (lexer->position < lexer->source_length &&
        src[lexer->position] == '0' &&
        lexer->position + 1 < lexer->source_length) {
        char next = src[lexer->position + 1];
        if (next == 'b') {
            lexer->position += 2;
            lexer->column += 2;
            result = parse_non_decimal_integer(lexer, 2, src + start_pos, start_line, start_col);
            goto out;
        } else if (next == 'o') {
            lexer->position += 2;
            lexer->column += 2;
            result = parse_non_decimal_integer(lexer, 8, src + start_pos, start_line, start_col);
            goto out;
        } else if (next == 'x') {
            lexer->position += 2;
            lexer->column += 2;
            if (is_hex_float_candidate(lexer)) {
                result = parse_hex_float(lexer, src + start_pos, start_line, start_col);
                goto out;
            }
            result = parse_non_decimal_integer(lexer, 16, src + start_pos, start_line, start_col);
            goto out;
        }
    }

    /* Decimal (integer or floating‑point) */
    result = parse_decimal_number(lexer, start_line, start_col);

out:
    /* Numbers never span lines, so the column advance equals chars consumed */
    lexer->column = (uint32_t)(start_col + (lexer->position - start_pos));
    return result;
}

/*
 *  Character literal parser
*/

/*
 *  Check whether the bytes form exactly one UTF-8 code point: either a
 *  single byte or one complete multi-byte sequence.
 */
static bool is_single_code_point(const char *s, uint32_t len) {
    if (len == 1) return true;
    if (len < 2 || len > 4) return false;
    unsigned char lead = (unsigned char)s[0];
    uint32_t need = 0;
    if (lead >= 0xC2 && lead <= 0xDF) need = 2;
    else if (lead >= 0xE0 && lead <= 0xEF) need = 3;
    else if (lead >= 0xF0 && lead <= 0xF4) need = 4;
    else return false;
    if (need != len) return false;
    for (uint32_t i = 1; i < need; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) return false;
    }
    return true;
}

/*
 *  Parse a character literal enclosed in single quotes.
 *  The raw body is validated: after escape expansion it must denote exactly
 *  one code point. The token value is the raw literal including the quotes;
 *  escape decoding happens in literal__parse_concatenated.
 *  Returns a token of type TOKEN_CHAR_LITERAL or TOKEN_ERRORCODE.
 */
Token literal__parse_char(Lexer *lexer) {
    const char *src = lexer->source;
    uint32_t start_line = lexer->line;
    uint32_t start_col = lexer->column;

    if (lexer->position >= lexer->source_length || src[lexer->position] != '\'') {
        errhandler__report_error(start_line, start_col, "lexer",
                                 "Expected '''");
        return make_error_token(lexer, start_line, start_col);
    }

    lexer->position++; // consume opening quote
    uint32_t start_pos = lexer->position;

    /* Find the closing quote, handling escapes */
    while (lexer->position < lexer->source_length &&
           src[lexer->position] != '\'') {
        if (src[lexer->position] == '\\') {
            lexer->position++; // skip backslash
            if (lexer->position >= lexer->source_length) break;
            // consume escaped char
            lexer->position++;
        } else {
            lexer->position++;
        }
    }

    if (lexer->position >= lexer->source_length || src[lexer->position] != '\'') {
        errhandler__report_error(start_line, start_col, "lexer",
                                 "Unterminated character literal");
        return make_error_token(lexer, start_line, start_col);
    }

    uint32_t end_pos = lexer->position; // position of closing quote
    uint32_t len = end_pos - start_pos;
    if (len == 0) {
        errhandler__report_error(start_line, start_col, "lexer",
                                 "Empty character literal");
        return make_error_token(lexer, start_line, start_col);
    }

    /* Validate the body: exactly one code point after escape expansion */
    uint32_t decoded_len = 0;
    char *decoded = escape__decode(src + start_pos, len,
                                   start_line, start_col + 1,
                                   true, &decoded_len);
    if (!decoded) {
        errhandler__report_error(start_line, start_col, "lexer",
                                 "Memory allocation failed");
        return make_error_token(lexer, start_line, start_col);
    }
    bool single = is_single_code_point(decoded, decoded_len);
    memory_free_safe((void **)&decoded);
    if (!single) {
        errhandler__report_error(start_line, start_col, "lexer",
                                 "Character literal must contain exactly one character");
        return make_error_token(lexer, start_line, start_col);
    }

    /* Store the raw literal including the quotes */
    char *raw = (char *)memory_allocate_zero(len + 3);
    if (!raw) {
        errhandler__report_error(start_line, start_col, "lexer",
                                 "Memory allocation failed");
        return make_error_token(lexer, start_line, start_col);
    }
    raw[0] = '\'';
    memcpy(raw + 1, src + start_pos, len);
    raw[1 + len] = '\'';
    raw[len + 2] = '\0';

    /* Advance past closing quote */
    lexer->position++;
    lexer->column += (len + 2);

    Token tok;
    tok.type = TOKEN_CHAR_LITERAL;
    tok.line = start_line;
    tok.column = start_col;
    tok.length = (uint32_t)(len + 2);
    tok.value = raw;
    return tok;
}

/*
 *  String literal parser
*/

/*
 *  Parse a string literal enclosed in double quotes.
 *  The token value is the raw literal including the quotes; escape decoding
 *  happens later (in literal__parse_concatenated for plain strings, in the
 *  parser for interpolated literal segments). If an unescaped '{' is found,
 *  the token type becomes TOKEN_INTERPOLATED_STRING. Raw newlines inside the
 *  literal are accepted; the line and column counters are advanced across
 *  them so that positions after the literal stay correct. Returns a token of
 *  type TOKEN_STRING_LITERAL or TOKEN_INTERPOLATED_STRING, or TOKEN_ERRORCODE.
 */
Token literal__parse_string(Lexer *lexer) {
    const char *src = lexer->source;
    uint32_t start_line = lexer->line;
    uint32_t start_col = lexer->column;

    if (lexer->position >= lexer->source_length || src[lexer->position] != '"') {
        errhandler__report_error(start_line, start_col, "lexer",
                                 "Expected '\"'");
        return make_error_token(lexer, start_line, start_col);
    }

    lexer->position++; // consume opening quote
    lexer->column++;
    uint32_t start_pos = lexer->position;
    bool has_interpolation = false;

    /* Scan for closing quote, detecting escapes and interpolation markers */
    while (lexer->position < lexer->source_length &&
           src[lexer->position] != '"') {
        if (src[lexer->position] == '\n') {
            lexer->line++;
            lexer->column = 1;
            lexer->position++;
            continue;
        }
        if (src[lexer->position] == '\\') {
            lexer->position++; // skip backslash
            lexer->column++;
            if (lexer->position < lexer->source_length) {
                if (src[lexer->position] == '\n') {
                    lexer->line++;
                    lexer->column = 1;
                } else {
                    lexer->column++;
                }
                lexer->position++; // consume escaped char
            }
            continue;
        }
        if (src[lexer->position] == '{') {
            has_interpolation = true;
        }
        lexer->position++;
        lexer->column++;
    }

    if (lexer->position >= lexer->source_length || src[lexer->position] != '"') {
        errhandler__report_error(start_line, start_col, "lexer",
                                 "Unterminated string literal");
        return make_error_token(lexer, start_line, start_col);
    }

    uint32_t end_pos = lexer->position; // position of closing quote
    uint32_t len = end_pos - start_pos;

    /* Allocate raw string content (including quotes? We store the whole literal) */
    uint32_t total_len = len + 2; // include quotes
    char *raw = (char *)memory_allocate_zero(total_len + 1);
    if (!raw) {
        errhandler__report_error(start_line, start_col, "lexer",
                                 "Memory allocation failed");
        return make_error_token(lexer, start_line, start_col);
    }
    raw[0] = '"';
    memcpy(raw + 1, src + start_pos, len);
    raw[1 + len] = '"';
    raw[total_len] = '\0';

    /* Advance past closing quote */
    lexer->position++;
    lexer->column++;

    Token tok;
    tok.type = has_interpolation ? TOKEN_INTERPOLATED_STRING : TOKEN_STRING_LITERAL;
    tok.line = start_line;
    tok.column = start_col;
    tok.length = (uint32_t)total_len;
    tok.value = raw;
    return tok;
}

/*
 *  Concatenated literal parser
*/

/*
 *  Parse a sequence of adjacent string and character literals separated only
 *  by whitespace, concatenating their raw bodies into a single token.
 *  A lone character literal yields a TOKEN_CHAR_LITERAL whose value is the
 *  decoded byte sequence (exactly one code point, validated by
 *  literal__parse_char). Otherwise the raw bodies are concatenated; for a
 *  plain (non-interpolated) result the escapes are decoded here, while an
 *  interpolated result keeps the raw text so the parser can decode each
 *  literal segment outside the { ... } expressions itself. An empty string
 *  literal ("") yields a TOKEN_STRING_LITERAL with length 0.
 *  Returns a token of type TOKEN_STRING_LITERAL, TOKEN_CHAR_LITERAL or
 *  TOKEN_INTERPOLATED_STRING, or TOKEN_ERRORCODE.
 */
Token literal__parse_concatenated(Lexer *lexer) {
    char *buffer = NULL;
    size_t cap = 0, len = 0;
    bool is_interpolated = false;
    bool error = false;
    uint32_t component_count = 0;
    bool first_was_char = false;
    uint32_t first_line = 0;    /* position of the first literal's body */
    uint32_t first_col = 0;

    while (1) {
        /* Skip whitespace */
        while (lexer->position < lexer->source_length &&
               u__char_is_whitespace(lexer->source[lexer->position])) {
            if (lexer->source[lexer->position] == '\n') {
                lexer->line++;
                lexer->column = 1;
            } else {
                lexer->column++;
            }
            lexer->position++;
        }

        if (lexer->position >= lexer->source_length) break;

        char c = lexer->source[lexer->position];
        if (c != '"' && c != '\'') break;

        /* Parse the literal */
        Token tok;
        if (c == '"') {
            tok = literal__parse_string(lexer);
        } else { // '\''
            tok = literal__parse_char(lexer);
        }

        if (tok.type == TOKEN_ERRORCODE) {
            /* Error already reported; ensure we have advanced past the quote */
            /* make_error_token advances, but we need to ensure we don't loop infinitely */
            error = true;
            break;
        }

        /* Check if interpolated */
        if (tok.type == TOKEN_INTERPOLATED_STRING) is_interpolated = true;

        /* Append the literal's raw body (excluding the quotes) to our buffer */
        const char *content = tok.value;
        size_t content_len = strlen(content);
        char quote = (tok.type == TOKEN_CHAR_LITERAL) ? '\'' : '"';
        if (content_len >= 2 && content[0] == quote && content[content_len-1] == quote) {
            if (component_count == 0) {
                first_was_char = (tok.type == TOKEN_CHAR_LITERAL);
                first_line = tok.line;
                first_col = tok.column + 1;
            }
            if (buffer_append_range(&buffer, &cap, &len,
                                    content + 1, content_len - 2) != 0) {
                error = true;
            }
        } else {
            errhandler__report_error(tok.line, tok.column, "lexer",
                                     "Invalid string literal in concatenation");
            error = true;
        }
        component_count++;

        /* Free the token's value (it was allocated with malloc) */
        memory_free_safe((void **)&tok.value);
        if (error) break;

        /* Continue to see if more literals follow (separated by whitespace) */
    }

    if (!error && component_count == 1 && first_was_char) {
        /* Lone character literal: the token carries the decoded code point */
        uint32_t decoded_len = 0;
        char *decoded = escape__decode(buffer, (uint32_t)len,
                                       first_line, first_col,
                                       true, &decoded_len);
        memory_free_safe((void **)&buffer);
        if (!decoded) return make_error_token(lexer, lexer->line, lexer->column);

        Token result;
        result.type = TOKEN_CHAR_LITERAL;
        result.line = first_line;
        result.column = (first_col > 0) ? first_col - 1 : first_col;
        result.length = decoded_len;
        result.value = decoded;
        return result;
    }

    if (!error && !is_interpolated && len > 0) {
        /* Plain concatenation: decode the escapes of the whole body */
        uint32_t decoded_len = 0;
        char *decoded = escape__decode(buffer, (uint32_t)len,
                                       first_line, first_col,
                                       false, &decoded_len);
        if (!decoded) {
            memory_free_safe((void **)&buffer);
            return make_error_token(lexer, lexer->line, lexer->column);
        }
        memory_free_safe((void **)&buffer);
        buffer = decoded;
        len = decoded_len;
    }

    if (error) {
        memory_free_safe((void **)&buffer);
        /* Ensure we have consumed at least one character if we are at the start of a literal */
        if (lexer->position < lexer->source_length &&
            (lexer->source[lexer->position] == '"' || lexer->source[lexer->position] == '\'')) {
            lexer->position++;
            lexer->column++;
        }
        return make_error_token(lexer, lexer->line, lexer->column);
    }

    if (!buffer) {
        /* Empty string literal: no characters were ever appended */
        buffer = (char *)memory_allocate_zero(1);
        if (!buffer) return make_error_token(lexer, lexer->line, lexer->column);
    }

    buffer[len] = '\0';
    char *dup = u__strduplic(buffer);
    memory_free_safe((void **)&buffer);
    if (!dup) {
        return make_error_token(lexer, lexer->line, lexer->column);
    }

    Token result;
    result.type = is_interpolated ? TOKEN_INTERPOLATED_STRING : TOKEN_STRING_LITERAL;
    result.line = lexer->line;
    result.column = lexer->column;
    result.length = (uint32_t)len;
    result.value = dup;
    return result;
}
