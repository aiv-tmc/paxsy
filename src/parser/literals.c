#include "literals.h"
#include "../errman/errman.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Check if character is uppercase hexadecimal digit
 * @param c: character to check
 * @return: true if character is A-Z, false otherwise
 */
static inline bool is_uppercase_hex_digit(char c) {
    return (c >= 'A' && c <= 'Z');
}

/*
 * Check if character is valid digit for specified base
 * @param c: character to check
 * @param base: numeric base (2-36)
 * @return: true if character is valid digit for base, false otherwise
 */
static inline bool is_valid_digit_for_base(char c, uint8_t base) {
    if (base <= 10) return (c >= '0' && c < '0' + base);
    return (c >= '0' && c <= '9') || 
           (is_uppercase_hex_digit(c) && (uint8_t)(c - 'A' + 10) < base);
}

/*
 * Skip underscore characters in numbers
 * @param lexer: lexer instance
 */
static inline void skip_underscores(Lexer* lexer) {
    while (lexer->position < lexer->source_length && 
           lexer->source[lexer->position] == '_') {
        lexer->position++;
        lexer->column++;
    }
}

/*
 * Parse integer part of a number
 * @param lexer: lexer instance
 * @param base: numeric base for digit validation
 * @param allow_period: allow period '(' character in integer part
 * @return: true if at least one valid digit was parsed, false otherwise
 */
static bool parse_integer_part(Lexer* lexer, uint8_t base, bool allow_period) {
    bool has_digits = false;
    const char* input = lexer->source;
    
    while (lexer->position < lexer->source_length) {
        skip_underscores(lexer);
        if (lexer->position >= lexer->source_length) break;
        
        char c = input[lexer->position];
        if ((allow_period && c == '(') || c == '.' || c == 'e') break;
        
        if (!is_valid_digit_for_base(c, base)) {
            if (has_digits) break;
            return false;
        }
        
        has_digits = true;
        lexer->position++;
        lexer->column++;
    }
    
    return has_digits;
}

/*
 * Parse period part in fractional number
 * @param lexer: lexer instance
 * @param base: numeric base for digit validation
 * @return: true if period was parsed successfully, false otherwise
 */
static bool parse_period_part(Lexer* lexer, uint8_t base) {
    const char* input = lexer->source;
    
    if (lexer->position >= lexer->source_length || input[lexer->position] != '(') 
        return false;
    
    lexer->position++;
    lexer->column++;
    
    if (!parse_integer_part(lexer, base, false)) {
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "Empty period in number literal");
        errman__report_error
            ( lexer->line
            , lexer->column
            , error_msg
        );
        return false;
    }
    
    if (lexer->position >= lexer->source_length || input[lexer->position] != ')') {
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "Unclosed period in number literal");
        errman__report_error
            ( lexer->line
            , lexer->column
            , error_msg
        );
        return false;
    }
    
    lexer->position++;
    lexer->column++;
    return true;
}

/*
 * Parse fractional part of a number (after decimal point)
 * @param lexer: lexer instance
 * @param base: numeric base for digit validation
 * @return: true if fractional part was parsed successfully, false otherwise
 */
static bool parse_fractional_part(Lexer* lexer, uint8_t base) {
    const char* input = lexer->source;
    
    if (lexer->position >= lexer->source_length || input[lexer->position] != '.') 
        return false;
    
    lexer->position++;
    lexer->column++;
    
    bool has_content = false;
    
    /* Parse initial digits before first period */
    if (parse_integer_part(lexer, base, true)) {
        has_content = true;
    }
    
    /* Parse periods and digits between periods */
    while (lexer->position < lexer->source_length && 
           input[lexer->position] == '(') {
        if (!parse_period_part(lexer, base)) {
            return false;
        }
        has_content = true;
        
        /* Parse digits after period */
        if (parse_integer_part(lexer, base, true)) {
            has_content = true;
        }
    }
    
    return has_content;
}

/*
 * Parse exponent part of a number
 * @param lexer: lexer instance
 * @param base: numeric base for digit validation (exponent can use same base)
 * @return: true if exponent was parsed successfully, false otherwise
 */
static bool parse_exponent_part(Lexer* lexer, uint8_t base) {
    const char* input = lexer->source;
    
    /* Exponent must be lowercase 'e' */
    if (lexer->position >= lexer->source_length || input[lexer->position] != 'e')
        return false;
    
    lexer->position++;
    lexer->column++;
    
    /* Optional sign for exponent */
    if (lexer->position < lexer->source_length) {
        char next_char = input[lexer->position];
        if (next_char == '+' || next_char == '-') {
            lexer->position++;
            lexer->column++;
        }
    }
    
    /* Parse exponent value (uses same base as number) */
    return parse_integer_part(lexer, base, false);
}

/*
 * Parse numeric literal with support for:
 * - Different bases (2-36) via prefixes
 * - Decimal points and periods
 * - Scientific notation (exponent)
 * - Underscore separators
 * @param lexer: lexer instance
 * @return: token representing the number literal
 */
Token parse_number_literal(Lexer* lexer) {
    const char* input = lexer->source;
    const uint32_t start_pos = lexer->position;
    const uint16_t start_line = lexer->line;
    const uint16_t start_col = lexer->column;
    
    uint8_t base = 10;
    bool has_prefix = false;
    bool error = false;
    
    /* Check for base prefix */
    if (lexer->position < lexer->source_length && input[lexer->position] == '0') {
        if (lexer->position + 1 < lexer->source_length) {
            char next_char = input[lexer->position + 1];
            
            if (next_char == 'b') { base = 2; has_prefix = true; }
            else if (next_char == 'q') { base = 4; has_prefix = true; }
            else if (next_char == 'o') { base = 8; has_prefix = true; }
            else if (next_char == 'd') { base = 10; has_prefix = true; }
            else if (next_char == 'x') { base = 16; has_prefix = true; }
            else if (next_char == 't') { base = 32; has_prefix = true; }
            else if (next_char == 's') { base = 36; has_prefix = true; }
            
            if (has_prefix) {
                lexer->position += 2;
                lexer->column += 2;
                
                /* Check that there's at least one digit after prefix */
                if (lexer->position >= lexer->source_length || 
                    (
                        (input[lexer->position] < '0' || 
                         input[lexer->position] > '9') &&
                        !is_uppercase_hex_digit(input[lexer->position])
                    )
                ) {
                    char error_msg[100];
                    snprintf(error_msg, sizeof(error_msg), "Invalid number after base prefix");
                    errman__report_error
                        ( lexer->line
                        , lexer->column
                        , error_msg
                    );
                    error = true;
                }
            }
        }
    }
    
    if (!error) {
        /* Parse integer part (mandatory) */
        bool has_integer = parse_integer_part(lexer, base, false);
        
        if (!has_integer) {
            char error_msg[100];
            snprintf(error_msg, sizeof(error_msg), "Number must start with at least one digit");
            errman__report_error
                ( lexer->line
                , lexer->column
                , error_msg
            );
            error = true;
        }
    }
    
    if (!error) {
        /* Parse fractional part (if present) */
        bool has_fraction = false;
        if (lexer->position < lexer->source_length && 
            input[lexer->position] == '.') {
            has_fraction = true;
            lexer->position++;
            lexer->column++;
            
            bool has_content = false;
            
            /* Parse initial digits before first period */
            if (parse_integer_part(lexer, base, true)) {
                has_content = true;
            }
            
            /* Parse periods and digits between periods */
            while (!error && lexer->position < lexer->source_length && 
                   input[lexer->position] == '(') {
                if (!parse_period_part(lexer, base)) {
                    error = true;
                    break;
                }
                has_content = true;
                
                /* Parse digits after period */
                if (parse_integer_part(lexer, base, true)) {
                    has_content = true;
                }
            }
            
            if (!has_content && !error) {
                char error_msg[100];
                snprintf(error_msg, sizeof(error_msg), "Empty fractional part");
                errman__report_error
                    ( lexer->line
                    , lexer->column
                    , error_msg
                );
                error = true;
            }
        }
        
        /* Parse exponent (if present) - allowed for any floating point number */
        if (!error && lexer->position < lexer->source_length && 
            input[lexer->position] == 'e') {
            
            /* Save position before checking exponent */
            uint32_t before_exp_pos = lexer->position;
            uint16_t before_exp_col = lexer->column;
            
            /* Try to parse exponent */
            if (parse_exponent_part(lexer, base)) {
                /* Exponent is valid only for floating point numbers */
                if (!has_fraction) {
                    char error_msg[100];
                    snprintf(error_msg, sizeof(error_msg), "Exponent requires decimal point");
                    errman__report_error
                        ( lexer->line
                        , lexer->column
                        , error_msg
                    );
                    error = true;
                }
            } else {
                /* No valid exponent, restore position */
                lexer->position = before_exp_pos;
                lexer->column = before_exp_col;
            }
        }
    }
    
    Token token;
    token.type = error ? TOKEN_ERROR : TOKEN_NUMBER;
    token.line = start_line;
    token.column = start_col;
    token.length = lexer->position - start_pos;
    
    if (error) {
        token.value = NULL;
        lexer->position = start_pos;
        lexer->column = start_col;
    } else {
        token.value = (char*)malloc(token.length + 1);
        if (token.value) {
            memcpy
                ( token.value
                , input + start_pos
                , token.length
            );
            token.value[token.length] = '\0';
        }
    }
    
    return token;
}

/*
 * Parse escape sequence in character or string literal
 * @param lexer: lexer instance
 * @return: escaped character value
 */
static inline char parse_escape_sequence(Lexer* lexer) {
    const char* input = lexer->source;
    
    if (lexer->position >= lexer->source_length) {
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "Incomplete escape sequence");
        errman__report_error(lexer->line, lexer->column, error_msg);
        return 0;
    }
    
    char escaped = input[lexer->position++];
    lexer->column++;
    
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
            char error_msg[100];
            snprintf(error_msg, sizeof(error_msg), "Unknown escape sequence: \\%c", escaped);
            errman__report_error(lexer->line, lexer->column, error_msg);
            return escaped;
    }
}

/*
 * Parse character literal with escape sequence support and multiline support
 * @param lexer: lexer instance
 * @return: token representing the character literal
 */
Token parse_char_literal(Lexer* lexer) {
    const char* input = lexer->source;
    const uint32_t start_pos = lexer->position;
    const uint16_t start_line = lexer->line;
    const uint16_t start_col = lexer->column;
    
    Token token;
    token.type = TOKEN_ERROR;
    token.line = start_line;
    token.column = start_col;
    token.value = NULL;
    
    if (lexer->position >= lexer->source_length || 
        input[lexer->position] != '\'') {
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "Expected character literal");
        errman__report_error(lexer->line, lexer->column, error_msg);
        return token;
    }
    
    lexer->position++;
    lexer->column++;
    
    /* String buffer for character literal (may contain newlines) */
    uint32_t buf_size = 128;
    char* buffer = (char*)malloc(buf_size);
    uint32_t buf_index = 0;
    
    if (!buffer) {
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "Memory allocation failed");
        errman__report_error(lexer->line, lexer->column, error_msg);
        return token;
    }
    
    while (lexer->position < lexer->source_length) {
        if (input[lexer->position] == '\'') {
            /* Found closing quote */
            lexer->position++;
            lexer->column++;
            break;
        }
        
        if (buf_index >= buf_size - 1) {
            buf_size <<= 1; /* Multiply by 2 */
            char* new_buffer = (char*)realloc(buffer, buf_size);
            if (!new_buffer) {
                char error_msg[100];
                snprintf(error_msg, sizeof(error_msg), "Memory reallocation failed");
                errman__report_error(lexer->line, lexer->column, error_msg);
                free(buffer);
                return token;
            }
            buffer = new_buffer;
        }
        
        if (input[lexer->position] == '\\') {
            lexer->position++;
            lexer->column++;
            buffer[buf_index++] = parse_escape_sequence(lexer);
        } else if (input[lexer->position] == '\n') {
            /* Handle newline in multiline character literal */
            buffer[buf_index++] = input[lexer->position++];
            lexer->line++;
            lexer->column = 1;
        } else {
            buffer[buf_index++] = input[lexer->position++];
            lexer->column++;
        }
    }
    
    if (lexer->position > start_pos && input[lexer->position - 1] != '\'') {
        /* Check if we found the closing quote */
        if (lexer->position < lexer->source_length && input[lexer->position] == '\'') {
            lexer->position++;
            lexer->column++;
        } else {
            char error_msg[100];
            snprintf(error_msg, sizeof(error_msg), "Unclosed character literal");
            errman__report_error(lexer->line, lexer->column, error_msg);
            free(buffer);
            return token;
        }
    }
    
    buffer[buf_index] = '\0';
    
    /* For character literal, we expect exactly one character or a valid escape sequence */
    if (buf_index != 1) {
        /* Check if it's a valid escape sequence that may produce one character */
        if (buf_index == 0) {
            char error_msg[100];
            snprintf(error_msg, sizeof(error_msg), "Empty character literal");
            errman__report_error(lexer->line, lexer->column, error_msg);
            free(buffer);
            return token;
        } else if (buffer[0] == '\\' && buf_index == 2) {
            /* Single escape sequence like \n, \t, etc. */
            /* This is valid - keep as is */
        } else {
            char error_msg[100];
            snprintf(error_msg, sizeof(error_msg), "Character literal must contain exactly one character");
            errman__report_error(lexer->line, lexer->column, error_msg);
            free(buffer);
            return token;
        }
    }
    
    token.type = TOKEN_CHAR;
    token.length = 1; /* Character literals always have length 1 in terms of value */
    token.value = (char*)malloc(2);
    if (token.value) {
        token.value[0] = buffer[0]; /* Store the actual character value */
        token.value[1] = '\0';
    }
    
    free(buffer);
    return token;
}

/*
 * Parse string literal with escape sequence support and multiline support
 * @param lexer: lexer instance
 * @return: token representing the string literal
 */
Token parse_string_literal(Lexer* lexer) {
    const char* input = lexer->source;
    const uint32_t start_pos = lexer->position;
    const uint16_t start_line = lexer->line;
    const uint16_t start_col = lexer->column;
    
    Token token;
    token.type = TOKEN_ERROR;
    token.line = start_line;
    token.column = start_col;
    token.value = NULL;
    
    if (lexer->position >= lexer->source_length || 
        input[lexer->position] != '"') {
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "Expected string literal");
        errman__report_error(lexer->line, lexer->column, error_msg);
        return token;
    }
    
    lexer->position++;
    lexer->column++;
    
    /* String buffer with dynamic allocation */
    uint32_t buf_size = 128;
    char* buffer = (char*)malloc(buf_size);
    uint32_t buf_index = 0;
    
    if (!buffer) {
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "Memory allocation failed");
        errman__report_error(lexer->line, lexer->column, error_msg);
        return token;
    }
    
    while (lexer->position < lexer->source_length) {
        if (input[lexer->position] == '"') {
            /* Found closing quote */
            lexer->position++;
            lexer->column++;
            break;
        }
        
        if (buf_index >= buf_size - 1) {
            buf_size <<= 1; /* Multiply by 2 */
            char* new_buffer = (char*)realloc(buffer, buf_size);
            if (!new_buffer) {
                char error_msg[100];
                snprintf(error_msg, sizeof(error_msg), "Memory reallocation failed");
                errman__report_error(lexer->line, lexer->column, error_msg);
                free(buffer);
                return token;
            }
            buffer = new_buffer;
        }
        
        if (input[lexer->position] == '\\') {
            lexer->position++;
            lexer->column++;
            buffer[buf_index++] = parse_escape_sequence(lexer);
        } else if (input[lexer->position] == '\n') {
            /* Handle newline in multiline string literal */
            buffer[buf_index++] = input[lexer->position++];
            lexer->line++;
            lexer->column = 1;
        } else {
            buffer[buf_index++] = input[lexer->position++];
            lexer->column++;
        }
    }
    
    if (lexer->position > start_pos && input[lexer->position - 1] != '"') {
        /* Check if we found the closing quote */
        if (lexer->position < lexer->source_length && input[lexer->position] == '"') {
            lexer->position++;
            lexer->column++;
        } else {
            char error_msg[100];
            snprintf(error_msg, sizeof(error_msg), "Unclosed string literal");
            errman__report_error(lexer->line, lexer->column, error_msg);
            free(buffer);
            return token;
        }
    }
    
    buffer[buf_index] = '\0';
    token.type = TOKEN_STRING;
    token.length = buf_index;
    token.value = (char*)malloc(buf_index + 1);
    if (token.value) {
        memcpy
            ( token.value
            , buffer
            , buf_index + 1
        );
    }
    
    free(buffer);
    return token;
}
