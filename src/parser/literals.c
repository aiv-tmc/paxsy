#include "literals.h"
#include "../errhandler/errhandler.h"
#include "../lexer/lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Skip whitespace characters in input stream.
 * This is a copy of the lexer's whitespace skipping function, used here
 * to check for concatenated literals.
 */
static inline void skip_whitespace(Lexer* lexer) {
    const char* source = lexer->source;
    uint32_t pos = lexer->position;
    uint16_t line = lexer->line;
    uint16_t col = lexer->column;
    
    while (pos < lexer->source_length) {
        char current_char = source[pos];
        
        if (current_char == ' ' || current_char == '\t') {
            pos++;
            col++;
        } else if (current_char == '\n') {
            pos++;
            line++;
            col = 1;
        } else break;
    }
    
    lexer->position = pos;
    lexer->line = line;
    lexer->column = col;
}

/*
 * Check if the next character after the current position is a string or char literal.
 * Used for concatenating adjacent literals.
 */
static inline int is_next_string_or_char(Lexer* lexer) {
    Lexer temp_lexer = *lexer;
    skip_whitespace(&temp_lexer);
    
    if (temp_lexer.position >= temp_lexer.source_length)
        return 0;
    
    char next_char = temp_lexer.source[temp_lexer.position];
    return (next_char == '"' || next_char == '\'');
}

/*
 * Check if character is uppercase hexadecimal digit.
 * @param c: character to check
 * @return: true if character is A-F, false otherwise
 */
static inline bool is_uppercase_hex_digit(char c) {
    return (c >= 'A' && c <= 'F');
}

/*
 * Check if character is lowercase hexadecimal digit.
 * @param c: character to check
 * @return: true if character is a-f, false otherwise
 */
static inline bool is_lowercase_hex_digit(char c) {
    return (c >= 'a' && c <= 'f');
}

/*
 * Check if character is valid digit for specified base.
 * @param c: character to check
 * @param base: numeric base (2, 8, 10, 16)
 * @return: true if character is valid digit for base, false otherwise
 */
static inline bool is_valid_digit_for_base(char c, uint8_t base) {
    switch (base) {
        case 2:  return (c == '0' || c == '1');
        case 8:  return (c >= '0' && c <= '7');
        case 10: return (c >= '0' && c <= '9');
        case 16: return (c >= '0' && c <= '9') || 
                         is_uppercase_hex_digit(c) || 
                         is_lowercase_hex_digit(c);
        default: return false;
    }
}

/*
 * Skip underscore characters in numbers (used as digit separators).
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
 * Parse integer part of a number.
 * @param lexer: lexer instance
 * @param base: numeric base for digit validation
 * @param allow_period: allow period '(' character in integer part (for floating-point numbers)
 * @return: true if at least one valid digit was parsed, false otherwise
 */
static bool parse_integer_part(Lexer* lexer, uint8_t base, bool allow_period) {
    bool has_digits = false;
    const char* input = lexer->source;
    
    while (lexer->position < lexer->source_length) {
        skip_underscores(lexer);
        if (lexer->position >= lexer->source_length) break;
        
        char c = input[lexer->position];
        
        // Stop parsing integer part if we encounter decimal point, period, or exponent
        if ((allow_period && c == '(') || c == '.' || c == 'e' || c == 'E') break;
        
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
 * Parse period part in fractional number (used for grouping digits in floating‑point).
 * Example: 123(456) where digits inside parentheses form a group.
 * @param lexer: lexer instance
 * @param base: numeric base for digit validation (always 10 for floating‑point)
 * @return: true if period was parsed successfully, false otherwise
 */
static bool parse_period_part(Lexer* lexer, uint8_t base) {
    const char* input = lexer->source;
    
    if (lexer->position >= lexer->source_length || input[lexer->position] != '(') 
        return false;
    
    lexer->position++;
    lexer->column++;
    
    if (!parse_integer_part(lexer, base, false)) {
        errhandler__report_error
            ( ERROR_CODE_SYNTAX_GENERIC
            , lexer->line
            , lexer->column
            , "syntax"
            , "Empty period in number literal"
        );
        return false;
    }
    
    if (lexer->position >= lexer->source_length || input[lexer->position] != ')') {
        errhandler__report_error
            ( ERROR_CODE_SYNTAX_GENERIC
            , lexer->line
            , lexer->column
            , "syntax"
            , "Unclosed period in number literal"
        );
        return false;
    }
    
    lexer->position++;
    lexer->column++;
    return true;
}

/*
 * Parse exponent part of a floating‑point number.
 * @param lexer: lexer instance
 * @return: true if exponent was parsed successfully, false otherwise
 */
static bool parse_exponent_part(Lexer* lexer) {
    const char* input = lexer->source;
    
    // Exponent can be either lowercase 'e' or uppercase 'E'
    if (lexer->position >= lexer->source_length || 
        (input[lexer->position] != 'e' && input[lexer->position] != 'E')) {
        return false;
    }
    
    lexer->position++;
    lexer->column++;
    
    // Optional sign for exponent (+ or -)
    if (lexer->position < lexer->source_length) {
        char next_char = input[lexer->position];
        if (next_char == '+' || next_char == '-') {
            lexer->position++;
            lexer->column++;
        }
    }
    
    // Parse exponent value (always uses base 10 for exponent)
    return parse_integer_part(lexer, 10, false);
}

/*
 * Parse numeric literal with the following rules:
 * - Integer literals can have bases: 2 (0b), 8 (0o), 10 (default), 16 (0x)
 * - Floating‑point literals can only be in base 10
 * - Floating‑point literals support scientific notation with 'e' or 'E'
 * - Underscores can be used as digit separators
 * @param lexer: lexer instance
 * @return: token representing the number literal
 */
Token literal__parse_number(Lexer* lexer) {
    const char* input = lexer->source;
    const uint32_t start_pos = lexer->position;
    const uint16_t start_line = lexer->line;
    const uint16_t start_col = lexer->column;
    
    uint8_t base = 10;
    bool has_prefix = false;
    bool is_integer_only = false; // If true, number cannot have fractional part or exponent
    bool error = false;
    
    // Check for base prefix (only for integer literals)
    if (lexer->position < lexer->source_length && input[lexer->position] == '0') {
        if (lexer->position + 1 < lexer->source_length) {
            char next_char = input[lexer->position + 1];
            
            // Only allow bases 2, 8, 16 with prefixes
            if (next_char == 'b' || next_char == 'B') { 
                base = 2; 
                has_prefix = true; 
                is_integer_only = true; // Binary numbers must be integers
            }
            else if (next_char == 'o' || next_char == 'O') { 
                base = 8; 
                has_prefix = true; 
                is_integer_only = true; // Octal numbers must be integers
            }
            else if (next_char == 'x' || next_char == 'X') { 
                base = 16; 
                has_prefix = true; 
                is_integer_only = true; // Hexadecimal numbers must be integers
            }
            
            if (has_prefix) {
                lexer->position += 2;
                lexer->column += 2;
                
                // Check that there's at least one valid digit after prefix
                if (lexer->position >= lexer->source_length || 
                    !is_valid_digit_for_base(input[lexer->position], base)) {
                    errhandler__report_error
                        ( ERROR_CODE_LEXER_INVALID_NUMBER
                        , lexer->line
                        , lexer->column
                        , "syntax"
                        , "Invalid number after base prefix, expected valid digit for base %d"
                        , base
                    );
                    error = true;
                }
            }
        }
    }
    
    if (!error) {
        // Parse integer part (mandatory for all numbers)
        bool has_integer = parse_integer_part(lexer, base, false);
        
        if (!has_integer) {
            errhandler__report_error
                ( ERROR_CODE_LEXER_INVALID_NUMBER
                , lexer->line
                , lexer->column
                , "syntax"
                , "Number must start with at least one digit"
            );
            error = true;
        }
    }
    
    if (!error) {
        // Parse fractional part (if present) - only allowed for base 10 numbers
        bool has_fraction = false;
        if (lexer->position < lexer->source_length && 
            input[lexer->position] == '.') {
            
            // Check if fractional part is allowed (only for base 10)
            if (is_integer_only) {
                errhandler__report_error
                    ( ERROR_CODE_LEXER_INVALID_NUMBER
                    , lexer->line
                    , lexer->column
                    , "syntax"
                    , "Floating-point numbers are only allowed in base 10"
                );
                error = true;
            } else {
                has_fraction = true;
                lexer->position++;
                lexer->column++;
                
                bool has_content = false;
                
                // Parse initial digits before first period
                if (parse_integer_part(lexer, base, true)) {
                    has_content = true;
                }
                
                // Parse periods and digits between periods
                while (!error && lexer->position < lexer->source_length && 
                       input[lexer->position] == '(') {
                    if (!parse_period_part(lexer, base)) {
                        error = true;
                        break;
                    }
                    has_content = true;
                    
                    // Parse digits after period
                    if (parse_integer_part(lexer, base, true)) {
                        has_content = true;
                    }
                }
                
                if (!has_content && !error) {
                    errhandler__report_error
                        ( ERROR_CODE_LEXER_INVALID_NUMBER
                        , lexer->line
                        , lexer->column
                        , "syntax"
                        , "Empty fractional part"
                    );
                    error = true;
                }
            }
        }
        
        // Parse exponent (if present) - only allowed for floating-point numbers (base 10)
        if (!error && lexer->position < lexer->source_length && 
            (input[lexer->position] == 'e' || input[lexer->position] == 'E')) {
            
            // Save position before checking exponent
            uint32_t before_exp_pos = lexer->position;
            uint16_t before_exp_col = lexer->column;
            
            // Try to parse exponent
            if (parse_exponent_part(lexer)) {
                // Exponent is valid only for floating-point numbers (base 10)
                if (is_integer_only) {
                    errhandler__report_error
                        ( ERROR_CODE_LEXER_INVALID_NUMBER
                        , lexer->line
                        , lexer->column
                        , "syntax"
                        , "Exponent notation is only allowed for base 10 floating-point numbers"
                    );
                    error = true;
                } else if (!has_fraction) {
                    // Exponent without decimal point is allowed (e.g., 123e10)
                    // This is valid - do nothing
                }
            } else {
                // No valid exponent, restore position
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
 * Parse escape sequence in character or string literal.
 * @param lexer: lexer instance
 * @return: escaped character value
 */
static inline char parse_escape_sequence(Lexer* lexer) {
    const char* input = lexer->source;
    
    if (lexer->position >= lexer->source_length) {
        errhandler__report_error
            ( ERROR_CODE_LEXER_INVALID_ESCAPE
            , lexer->line
            , lexer->column
            , "syntax"
            , "Incomplete escape sequence"
        );
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
        case 'e': return '\x1B'; // ESC character
        default:
            errhandler__report_error
                ( ERROR_CODE_LEXER_INVALID_ESCAPE
                , lexer->line
                , lexer->column 
                , "syntax"
                , "Unknown escape sequence: \\%c"
                , escaped
            );
            return escaped;
    }
}

/*
 * Parse character literal with escape sequence support and multiline support.
 * @param lexer: lexer instance
 * @return: token representing the character literal
 */
Token literal__parse_char(Lexer* lexer) {
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
        errhandler__report_error
            ( ERROR_CODE_SYNTAX_GENERIC
            , lexer->line
            , lexer->column
            , "syntax"
            , "Expected character literal"
        );
        return token;
    }
    
    lexer->position++;
    lexer->column++;
    
    // String buffer for character literal (may contain newlines)
    uint32_t buf_size = 128;
    char* buffer = (char*)malloc(buf_size);
    uint32_t buf_index = 0;
    
    if (!buffer) {
        errhandler__report_error
            ( ERROR_CODE_MEMORY_ALLOCATION
            , lexer->line
            , lexer->column
            , "syntax"
            , "Memory allocation failed"
        );
        return token;
    }
    
    while (lexer->position < lexer->source_length) {
        if (input[lexer->position] == '\'') {
            // Found closing quote
            lexer->position++;
            lexer->column++;
            break;
        }
        
        if (buf_index >= buf_size - 1) {
            buf_size <<= 1; // Multiply by 2
            char* new_buffer = (char*)realloc(buffer, buf_size);
            if (!new_buffer) {
                errhandler__report_error
                    ( ERROR_CODE_MEMORY_ALLOCATION
                    , lexer->line
                    , lexer->column
                    , "syntax"
                    , "Memory reallocation failed"
                );
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
            // Handle newline in multiline character literal
            buffer[buf_index++] = input[lexer->position++];
            lexer->line++;
            lexer->column = 1;
        } else {
            buffer[buf_index++] = input[lexer->position++];
            lexer->column++;
        }
    }
    
    if (lexer->position > start_pos && input[lexer->position - 1] != '\'') {
        // Check if we found the closing quote
        if (lexer->position < lexer->source_length && input[lexer->position] == '\'') {
            lexer->position++;
            lexer->column++;
        } else {
            errhandler__report_error
                ( ERROR_CODE_LEXER_UNCLOSED_STRING
                , lexer->line
                , lexer->column
                , "syntax"
                , "Unclosed character literal"
            );
            free(buffer);
            return token;
        }
    }
    
    buffer[buf_index] = '\0';
    
    // For character literal, we expect exactly one character or a valid escape sequence
    if (buf_index != 1) {
        // Check if it's a valid escape sequence that may produce one character
        if (buf_index == 0) {
            errhandler__report_error
                ( ERROR_CODE_SYNTAX_GENERIC
                , lexer->line
                , lexer->column
                , "syntax"
                , "Empty character literal"
            );
            free(buffer);
            return token;
        } else if (buffer[0] == '\\' && buf_index == 2) {
            // Single escape sequence like \n, \t, etc. – valid
            // Keep as is
        } else {
            errhandler__report_error
                ( ERROR_CODE_SYNTAX_GENERIC
                , lexer->line
                , lexer->column
                , "syntax"
                , "Character literal must contain exactly one character"
            );
            free(buffer);
            return token;
        }
    }
    
    token.type = TOKEN_CHAR;
    token.length = 1; // Character literals always have length 1 in terms of value
    token.value = (char*)malloc(2);
    if (token.value) {
        token.value[0] = buffer[0]; // Store the actual character value
        token.value[1] = '\0';
    }
    
    free(buffer);
    return token;
}

/*
 * Parse string literal with escape sequence support and multiline support.
 * @param lexer: lexer instance
 * @return: token representing the string literal
 */
Token literal__parse_string(Lexer* lexer) {
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
        errhandler__report_error
            ( ERROR_CODE_SYNTAX_GENERIC
            , lexer->line
            , lexer->column
            , "syntax"
            , "Expected string literal"
        );
        return token;
    }
    
    lexer->position++;
    lexer->column++;
    
    // String buffer with dynamic allocation
    uint32_t buf_size = 128;
    char* buffer = (char*)malloc(buf_size);
    uint32_t buf_index = 0;
    
    if (!buffer) {
        errhandler__report_error
            ( ERROR_CODE_MEMORY_ALLOCATION
            , lexer->line
            , lexer->column
            , "syntax"
            , "Memory allocation failed"
        );
        return token;
    }
    
    while (lexer->position < lexer->source_length) {
        if (input[lexer->position] == '"') {
            // Found closing quote
            lexer->position++;
            lexer->column++;
            break;
        }
        
        if (buf_index >= buf_size - 1) {
            buf_size <<= 1; // Multiply by 2
            char* new_buffer = (char*)realloc(buffer, buf_size);
            if (!new_buffer) {
                errhandler__report_error
                    ( ERROR_CODE_MEMORY_ALLOCATION
                    , lexer->line
                    , lexer->column
                    , "syntax"
                    , "Memory reallocation failed"
                );
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
            // Handle newline in multiline string literal
            buffer[buf_index++] = input[lexer->position++];
            lexer->line++;
            lexer->column = 1;
        } else {
            buffer[buf_index++] = input[lexer->position++];
            lexer->column++;
        }
    }
    
    if (lexer->position > start_pos && input[lexer->position - 1] != '"') {
        // Check if we found the closing quote
        if (lexer->position < lexer->source_length && input[lexer->position] == '"') {
            lexer->position++;
            lexer->column++;
        } else {
            errhandler__report_error
                ( ERROR_CODE_LEXER_UNCLOSED_STRING
                , lexer->line
                , lexer->column
                , "syntax"
                , "Unclosed string literal"
            );
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

/*
 * Parse and concatenate adjacent string and character literals.
 * For example: "Hello " 'W' "orld" becomes a single string token.
 * @param lexer: Lexer instance
 * @return: Combined token (type TOKEN_STRING)
 */
Token literal__parse_concatenated(Lexer* lexer) {
    const char* source = lexer->source;
    uint32_t start_pos = lexer->position;
    uint16_t start_line = lexer->line;
    uint16_t start_col = lexer->column;
    
    // Buffer to store concatenated string
    uint32_t buffer_size = 128;
    char* buffer = (char*)malloc(buffer_size);
    uint32_t buffer_index = 0;
    
    if (!buffer) {
        Token error_token;
        error_token.type = TOKEN_ERROR;
        error_token.value = NULL;
        error_token.line = start_line;
        error_token.column = start_col;
        error_token.length = 0;
        return error_token;
    }
    
    while (lexer->position < lexer->source_length) {
        char current_char = source[lexer->position];
        
        if (current_char == '\'') {
            Token char_token = literal__parse_char(lexer);
            
            if (char_token.type == TOKEN_ERROR) {
                free(buffer);
                return char_token;
            }
            
            // Add character to buffer
            if (char_token.value) {
                uint32_t needed_size = buffer_index + 1;
                if (needed_size >= buffer_size) {
                    buffer_size *= 2;
                    char* new_buffer = (char*)realloc(buffer, buffer_size);
                    if (!new_buffer) {
                        free(buffer);
                        free(char_token.value);
                        Token error_token;
                        error_token.type = TOKEN_ERROR;
                        error_token.value = NULL;
                        error_token.line = start_line;
                        error_token.column = start_col;
                        error_token.length = 0;
                        return error_token;
                    }
                    buffer = new_buffer;
                }
                
                buffer[buffer_index++] = char_token.value[0];
                free(char_token.value);
            }
        } else if (current_char == '"') {
            Token string_token = literal__parse_string(lexer);
            
            if (string_token.type == TOKEN_ERROR) {
                free(buffer);
                return string_token;
            }
            
            // Add string to buffer
            if (string_token.value) {
                uint32_t str_len = string_token.length;
                uint32_t needed_size = buffer_index + str_len;
                if (needed_size >= buffer_size) {
                    while (needed_size >= buffer_size) {
                        buffer_size *= 2;
                    }
                    char* new_buffer = (char*)realloc(buffer, buffer_size);
                    if (!new_buffer) {
                        free(buffer);
                        free(string_token.value);
                        Token error_token;
                        error_token.type = TOKEN_ERROR;
                        error_token.value = NULL;
                        error_token.line = start_line;
                        error_token.column = start_col;
                        error_token.length = 0;
                        return error_token;
                    }
                    buffer = new_buffer;
                }
                
                memcpy(buffer + buffer_index, string_token.value, str_len);
                buffer_index += str_len;
                free(string_token.value);
            }
        } else {
            break;
        }
        
        // Check if there's another literal immediately after
        if (!is_next_string_or_char(lexer)) {
            break;
        }
    }
    
    // Create the combined token
    buffer[buffer_index] = '\0';
    
    Token combined_token;
    combined_token.type = TOKEN_STRING;
    combined_token.value = (char*)malloc(buffer_index + 1);
    
    if (combined_token.value) {
        memcpy(combined_token.value, buffer, buffer_index + 1);
    } else {
        combined_token.type = TOKEN_ERROR;
    }
    
    combined_token.line = start_line;
    combined_token.column = start_col;
    combined_token.length = buffer_index;
    
    free(buffer);
    return combined_token;
}
