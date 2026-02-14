#include "char_utils.h"

int char_is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int char_is_alnum(char c) {
    return char_is_alpha(c) || char_is_digit(c);
}

int char_is_digit(char c) {
    return c >= '0' && c <= '9';
}

int char_is_hex_digit(char c) {
    return (c >= '0' && c <= '9')
        || (c >= 'A' && c <= 'F')
        || (c >= 'a' && c <= 'f');
}

int char_is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

int char_is_identifier_start(char c) {
    return char_is_alpha(c) || c == '_';
}

int char_is_identifier_char(char c) {
    return char_is_alnum(c) || c == '_';
}

int char_is_operator_start(char c) {
    return !(char_is_alpha(c) || char_is_digit(c) || c == '_');
}

int char_is_path_char(char c) {
    return char_is_alnum(c)
        || c == '_'
        || c == '-'
        || c == '.'
        || c == '/'
        || c == '\\'
        || c == ':'
        || c == '~';
}

int char_is_line_break(char c) {
    return c == '\n' || c == '\r';
}

char char_to_lower(char c) {
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

char char_to_upper(char c) {
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    return c;
}

int char_is_printable(char c) {
    return (c >= 32 && c <= 126) || (c >= 128 && c <= 255);
}

int char_is_escape_char(char c) {
    switch (c) {
        case 'n':  // newline
        case 't':  // tab
        case 'r':  // carriage return
        case 'b':  // backspace
        case 'f':  // form feed
        case 'v':  // vertical tab
        case 'a':  // alert
        case '\\': // backslash
        case '\'': // single quote
        case '"':  // double quote
        case '?':  // question mark
        case '0':  // null character
            return 1;
        default:
            return 0;
    }
}
