#include "char_utils.h"

int u__char_is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int u__char_is_alnum(char c) {
    return u__char_is_alpha(c) || u__char_is_digit(c);
}

int u__char_is_digit(char c) {
    return c >= '0' && c <= '9';
}

int u__char_is_hex_digit(char c) {
    return (c >= '0' && c <= '9')
        || (c >= 'A' && c <= 'F')
        || (c >= 'a' && c <= 'f');
}

int u__char_is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

int u__char_is_identifier_start(char c) {
    return u__char_is_alpha(c) || c == '_';
}

int u__char_is_identifier_char(char c) {
    return u__char_is_alnum(c) || c == '_';
}

int u__char_is_operator_start(char c) {
    return !(u__char_is_alpha(c) || u__char_is_digit(c) || c == '_');
}

int u__char_is_path_char(char c) {
    return u__char_is_alnum(c)
        || c == '_'
        || c == '-'
        || c == '.'
        || c == '/'
        || c == '\\'
        || c == ':'
        || c == '~';
}

int u__char_is_line_break(char c) {
    return c == '\n' || c == '\r';
}

char u__char_to_lower(char c) {
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

char u__char_to_upper(char c) {
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    return c;
}

int u__char_is_printable(char c) {
    return (c >= 32 && c <= 126) || (c >= 128 && c <= 255);
}

int u__char_is_escape_char(char c) {
    switch (c) {
        case 'n':
        case 't':
        case 'r':
        case 'b':
        case 'f':
        case 'v':
        case 'a':
        case '\\':
        case '\'':
        case '"':
        case '?':
        case '0':
            return 1;
        default:
            return 0;
    }
}
