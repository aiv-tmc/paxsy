#include "escape.h"
#include "../../interfaces/errhandler/errhandler.h"
#include "../../interfaces/utils/memory.h"

/*
 * Convert a hexadecimal digit to its value, or -1 if invalid.
 */
static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char* escape__decode(const char* raw, uint32_t raw_len,
                     uint32_t line, uint32_t column,
                     bool allow_nul, uint32_t* out_len) {
    char* buf = (char*)memory_allocate_zero(raw_len + 1);
    if (!buf) return NULL;

    uint32_t len = 0;
    uint32_t cur_line = line;
    uint32_t cur_col = column;
    uint32_t i = 0;

    while (i < raw_len) {
        char c = raw[i];
        if (c != '\\') {
            buf[len++] = c;
            if (c == '\n') {
                cur_line++;
                cur_col = 1;
            } else {
                cur_col++;
            }
            i++;
            continue;
        }

        /* Escape sequence: cur points at the backslash */
        uint32_t esc_line = cur_line;
        uint32_t esc_col = cur_col;
        cur_col++;
        i++;
        if (i >= raw_len) {
            /* Trailing backslash: no character follows */
            errhandler__report_error(esc_line, esc_col, "lexer",
                                     "unknown escape sequence");
            break;
        }

        char e = raw[i];
        char decoded;
        switch (e) {
            case 'n':  decoded = '\n'; break;
            case 't':  decoded = '\t'; break;
            case 'r':  decoded = '\r'; break;
            case '0':  decoded = '\0'; break;
            case '\\': decoded = '\\'; break;
            case '"':  decoded = '"';  break;
            case '\'': decoded = '\''; break;
            case 'x': {
                int hi = (i + 1 < raw_len) ? hex_digit_value(raw[i + 1]) : -1;
                int lo = (i + 2 < raw_len) ? hex_digit_value(raw[i + 2]) : -1;
                if (hi < 0 || lo < 0) {
                    errhandler__report_error(esc_line, esc_col, "lexer",
                                             "invalid \\x escape sequence");
                    /* Keep the 'x' and continue after it */
                    buf[len++] = 'x';
                    cur_col++;
                    i++;
                    continue;
                }
                decoded = (char)((hi << 4) | lo);
                cur_col += 3; /* x and the two hex digits */
                i += 3;
                break;
            }
            default:
                errhandler__report_error(esc_line, esc_col, "lexer",
                                         "unknown escape sequence");
                /* Keep the character after the backslash */
                buf[len++] = e;
                cur_col++;
                i++;
                continue;
        }

        if (decoded == '\0' && !allow_nul) {
            errhandler__report_error(esc_line, esc_col, "lexer",
                                     "embedded NUL in string literal is not supported");
        } else {
            buf[len++] = decoded;
        }
        if (e != 'x') {
            cur_col++;
            i++;
        }
    }

    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}
