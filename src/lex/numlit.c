#include "common.h"
#include "diag.h"
#include "numlit.h"

/*
 * numlit.c - validation of numeric literal token slices.
 *
 * The lexer hands the raw character run of a numeric literal here
 * and this pass re-walks it, reporting every shape violation through
 * the shared diagnostic stream:
 *
 *   - the base prefix 0x/0o/0b selects the allowed digit set; a digit
 *     outside the base is reported at its own offset;
 *   - '_' is a readability separator only: it must sit between two
 *     digits, never leading, trailing or doubled;
 *   - a float is decimal only: one '.', an optional e/E exponent
 *     that must carry at least one digit after its optional sign.
 *     A '.' in a non-decimal literal names a hexadecimal float,
 *     which Paxsy spells with a 'p' exponent, so the lone '.' is
 *     reported as a missing exponent rather than a generic bad
 *     character.
 *
 * The return value tells the lexer which token kind to emit: 1 for a
 * float literal, 0 for an integer literal.
 */

/* Reports one violation positioned at slice offset `off` (0-based,
 * converted to the one-based column the diagnostic stream expects). */
static void lit_error(Diag *diag, const char *path, const char *s, int len,
                      int line, int col, int off, const char *msg) {
    (void)s; (void)len;
    diag_report(diag, DK_ERROR, path, line, col + off, "%s", msg);
}

/* Digit value of `c` in `base`, or -1 when it is not a valid digit. */
static int digit_val(int c, int base) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return -1;
    return v < base ? v : -1;
}

int numlit_validate(Diag *diag, const char *src_path, const char *s,
                    int len, int line, int col) {
    int i = 0;
    int base = 10;
    int is_float = 0;
    int saw_digit = 0;
    int prev_sep = 0;   /* previous char was '_' */

    if (len <= 0) {
        lit_error(diag, src_path, s, len, line, col, 0, "empty numeric literal");
        return 0;
    }

    /* Base prefix: '0' followed by x/o/b (also uppercase). */
    if (len >= 2 && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X' || s[1] == 'o' || s[1] == 'O' ||
         s[1] == 'b' || s[1] == 'B')) {
        base = s[1] == 'x' || s[1] == 'X' ? 16 :
               s[1] == 'o' || s[1] == 'O' ?  8 : 2;
        i = 2;
    }

    /* Mantissa digits with '_' separators. */
    for (; i < len; i++) {
        char c = s[i];
        if (c == '_') {
            if (!saw_digit)
                lit_error(diag, src_path, s, len, line, col, i,
                          "'_' must follow a digit");
            else if (prev_sep)
                lit_error(diag, src_path, s, len, line, col, i,
                          "consecutive '_' separators");
            prev_sep = 1;
            continue;
        }
        prev_sep = 0;
        if (c == '.' && base == 10) {
            if (is_float)
                lit_error(diag, src_path, s, len, line, col, i,
                          "multiple '.' in a float literal");
            else if (i + 1 >= len || digit_val((unsigned char)s[i + 1], 10) < 0)
                lit_error(diag, src_path, s, len, line, col, i,
                          "'.' must be followed by a digit");
            is_float = 1;
            continue;
        }
        if ((c == 'e' || c == 'E') && base == 10 && saw_digit) {
            /* Exponent: optional sign, then >= 1 digit. */
            int j = i + 1, exp_digits = 0;
            if (j < len && (s[j] == '+' || s[j] == '-')) j++;
            for (; j < len; j++) {
                if (digit_val((unsigned char)s[j], 10) >= 0) exp_digits++;
                else if (s[j] == '_') {
                    if (j + 1 < len && digit_val((unsigned char)s[j + 1], 10) >= 0)
                        continue;
                    lit_error(diag, src_path, s, len, line, col, j,
                              "'_' must sit between exponent digits");
                } else break;
            }
            if (!exp_digits)
                lit_error(diag, src_path, s, len, line, col, i,
                          "exponent needs at least one digit");
            is_float = 1;
            i = j - 1;
            prev_sep = 0;
            continue;
        }
        if (digit_val((unsigned char)c, base) >= 0) {
            saw_digit = 1;
            continue;
        }
        if (base != 10 && digit_val((unsigned char)c, 10) >= 0) {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "invalid digit '%c' in numeric literal", c);
            lit_error(diag, src_path, s, len, line, col, i, msg);
            continue;
        }
        if (base == 16 && c == '.') {
            lit_error(diag, src_path, s, len, line, col, i,
                      "hexadecimal float requires a 'p' exponent");
            break;
        }
        lit_error(diag, src_path, s, len, line, col, i,
                  "invalid character in numeric literal");
        break;
    }

    if (prev_sep)
        lit_error(diag, src_path, s, len, line, col, len - 1,
                  "trailing '_' separator");
    if (!saw_digit)
        lit_error(diag, src_path, s, len, line, col, 0,
                  "numeric literal has no digits");
    return is_float;
}
