#include "preprocessor.h"
#include "preprocessor_state.h"
//#include "DPP__go/DPPF__go.h"
//#include "DPP__program/DPPF__program.h"
//#include "DPP__inclib/DPPF__inclib.h"
//#include "DPP__incfile/DPPF__incfile.h"
//#include "DPP__define/DPPF__define.h"
//#include "DPP__if/DPPF__if.h"
//#include "DPP__ifdef/DPPF__ifdef.h"
//#include "DPP__ifndef/DPPF__ifndef.h"
//#include "DPP__elif/DPPF__elif.h"
//#include "DPP__else/DPPF__else.h"
//#include "DPP__endif/DPPF__endif.h"
#include "../errhandler/errhandler.h"
#include <stdlib.h>
#include <string.h>

/**
 * Ensure output buffer has enough capacity
 * @param state: PreprocessorState containing output buffer
 * @param needed: Additional capacity needed
 * @return: 1 if successful, 0 if allocation failed
 */
static int ensure_output_capacity(PreprocessorState* state, size_t needed) {
    if (state->output_pos + needed < state->output_capacity) return 1;
    
    size_t new_capacity = state->output_capacity * 2;
    if (new_capacity < state->output_pos + needed) 
        new_capacity = state->output_pos + needed + 1024;
    
    char* new_output = realloc(state->output, new_capacity);
    if (!new_output) return 0;
    
    state->output = new_output;
    state->output_capacity = new_capacity;
    return 1;
}

/**
 * Add character to output buffer
 * @param state: PreprocessorState containing output buffer
 * @param c: Character to add
 */
static void add_to_output(PreprocessorState* state, char c) {
    if (ensure_output_capacity(state, 1))
        state->output[state->output_pos++] = c;
}

/**
 * Check if we're at the start of a configuration macro
 * @param state: PreprocessorState to check
 * @return: Non-zero if at macro start, zero otherwise
 */
static int is_config_macro_start(PreprocessorState* state) {
    const char* input = state->input;
    size_t pos = state->input_pos;
    
    if (input[pos] != '_' || input[pos + 1] != '_') return 0;
    
    char c = input[pos + 2];
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* Common character classification functions */
static int is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int is_alnum(char c) {
    return (c >= '0' && c <= '9') || is_alpha(c);
}

static int is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/**
 * Clear directive buffer
 */
static void clear_directive_buffer(PreprocessorState* state) {
    state->directive_pos = 0;
    state->directive_buffer[0] = '\0';
}

/**
 * Add character to directive buffer
 */
static void add_to_directive_buffer(PreprocessorState* state, char c) {
    if (state->directive_pos < sizeof(state->directive_buffer) - 1) {
        state->directive_buffer[state->directive_pos++] = c;
        state->directive_buffer[state->directive_pos] = '\0';
    }
}

/**
 * Process preprocessor directive
 */
static void process_directive(PreprocessorState* state) {
    char* directive = state->directive_buffer;
    
    if (state->directive_pos == 0) return;
    
    /* Skip leading whitespace after '#' */
    char* ptr = directive;
    while (*ptr && is_whitespace(*ptr)) ptr++;
    
    /* Check if this is actually a directive */
    if (*ptr != '#') return;
    ptr++; /* Skip '#' */
    
    /* Skip whitespace after '#' */
    while (*ptr && is_whitespace(*ptr)) ptr++;
    
    if (*ptr == '\0') {
        /* Empty directive, ignore */
        return;
    }
    
    /* Extract directive command */
    char* command_start = ptr;
    while (*ptr && !is_whitespace(*ptr) && *ptr != '\n' && *ptr != '\r') ptr++;
    
    size_t command_len = ptr - command_start;
    if (command_len == 0) return;
    
    char command[32];
    if (command_len >= sizeof(command)) {
        errhandler__report_error(state->directive_start_line, 
                             state->directive_start_column + (command_start - directive), "syntax",
                             "Preprocessor directive command too long");
        return;
    }
    
    memcpy(command, command_start, command_len);
    command[command_len] = '\0';
    
    /* Skip whitespace before arguments */
    while (*ptr && is_whitespace(*ptr)) ptr++;
    
    // char* args = ptr;
    
    /* Process known directives - строго определенные команды */
    int known_directive = 1;
    
    //if (strcmp(command, "go") == 0) {
    //    DPPF__go(state, args);
    //} else if (strcmp(command, "program") == 0) {
    //    DPPF__program(state, args);
    //} else if (strcmp(command, "inclib") == 0) {
    //    DPPF__inclib(state, args);
    //} else if (strcmp(command, "incfile") == 0) {
    //    DPPF__incfile(state, args);
    //} else if (strcmp(command, "define") == 0) {
    //    DPPF__define(state, args);
    //} else if (strcmp(command, "if") == 0) {
    //    DPPF__if(state, args);
    //} else if (strcmp(command, "ifdef") == 0) {
    //    DPPF__ifdef(state, args);
    //} else if (strcmp(command, "ifndef") == 0) {
    //    DPPF__ifndef(state, args);
    //} else if (strcmp(command, "elif") == 0) {
    //    DPPF__elif(state, args);
    //} else if (strcmp(command, "else") == 0) {
    //    DPPF__else(state, args);
    //} else if (strcmp(command, "endif") == 0) {
    //    DPPF__endif(state, args);
    //} else {
    //    known_directive = 0;
    //}
    
    /* Report error for unknown directive */
    if (!known_directive) {
        errhandler__report_error(state->directive_start_line, 
                             state->directive_start_column + (command_start - directive), "syntax",
                             "Unknown preprocessor directive: %s", command);
    }
}

/**
 * Check if line continuation with backslash
 * @param state: PreprocessorState to check
 * @return: 1 if line continuation, 0 otherwise
 */
static int is_line_continuation(PreprocessorState* state) {
    const char* input = state->input;
    size_t pos = state->input_pos;
    
    // Check for backslash followed by newline
    if (input[pos] == '\\' && (input[pos + 1] == '\n' || input[pos + 1] == '\r')) {
        // Handle \r\n sequence
        if (input[pos + 1] == '\r' && input[pos + 2] == '\n') {
            return 1;
        }
        return 1;
    }
    return 0;
}

/**
 * Handle line continuation - skip backslash and newline
 * @param state: PreprocessorState to update
 */
static void handle_line_continuation(PreprocessorState* state) {
    char current = state->input[state->input_pos];
    char next = state->input[state->input_pos + 1];
    
    // Skip backslash
    state->input_pos++;
    state->column++;
    
    // Handle different newline sequences
    if (next == '\r' && state->input[state->input_pos + 1] == '\n') {
        // \r\n sequence
        state->input_pos += 2;
        state->line++;
        state->column = 1;
    } else if (next == '\n') {
        // \n sequence
        state->input_pos++;
        state->line++;
        state->column = 1;
    } else if (next == '\r') {
        // \r sequence (old Mac)
        state->input_pos++;
        state->line++;
        state->column = 1;
    }
}

/**
 * Main preprocessor function
 * @param input: Source code to preprocess
 * @param filename: Name of the file being processed (for error reporting)
 * @param error: Pointer to store error code (0 = success, non-zero = error)
 * @return: Preprocessed code (must be freed by caller) or NULL on error
 */
char* preprocess
    ( const char* input
    , const char* filename
    , int* error
    ) {
    /* Mark filename as unused to avoid compiler warning */
    (void)filename;
    
    /* Initialize state structure */
    size_t input_len = strlen(input);
    PreprocessorState state;
    state.input = input;
    state.input_pos = 0;
    state.output_pos = 0;
    state.output_capacity = input_len * 2;
    state.output = malloc(state.output_capacity);
    state.line = 1;
    state.column = 1;
    state.in_single_line_comment = 0;
    state.in_multi_line_comment = 0;
    state.in_string = 0;
    state.in_char = 0;
    state.in_preprocessor_directive = 0;
    state.in_config_macro = 0;
    state.bracket_depth = 0;
    
    /* Initialize directive buffer */
    state.directive_pos = 0;
    state.directive_start_line = 0;
    state.directive_start_column = 0;
    
    if (!state.output) {
        if (error) *error = 1;
        return NULL;
    }
    
    /* Main processing loop */
    while (state.input[state.input_pos] != '\0') {
        char current = state.input[state.input_pos];
        char next = state.input[state.input_pos + 1];
        
        /* Check for line continuation */
        if (is_line_continuation(&state)) {
            handle_line_continuation(&state);
            continue;
        }
        
        /* Handle string literal */
        if (state.in_string) {
            add_to_output(&state, current);
            state.input_pos++;
            state.column++;
            
            if (current == '\\' && next != '\0') {
                add_to_output(&state, next);
                state.input_pos++;
                state.column++;
            } else if (current == '"') state.in_string = 0;
            else if (current == '\n') {
                state.line++;
                state.column = 1;
            }
            continue;
        }
        
        /* Handle character literal */
        if (state.in_char) {
            add_to_output(&state, current);
            state.input_pos++;
            state.column++;
            
            if (current == '\\' && next != '\0') {
                add_to_output(&state, next);
                state.input_pos++;
                state.column++;
            } else if (current == '\'') state.in_char = 0;
            else if (current == '\n') {
                state.line++;
                state.column = 1;
            }
            continue;
        }
        
        /* Handle single-line comment */
        if (state.in_single_line_comment) {
            // Check for line continuation at the end of comment
            if (is_line_continuation(&state)) {
                handle_line_continuation(&state);
                // Stay in single-line comment state for continuation
                continue;
            }
            
            if (current == '\n') {
                add_to_output(&state, '\n');
                state.input_pos++;
                state.line++;
                state.column = 1;
                state.in_single_line_comment = 0;
            } else {
                state.input_pos++;
                state.column++;
            }
            continue;
        }
        
        /* Handle multi-line comment */
        if (state.in_multi_line_comment) {
            // Check for line continuation in multi-line comment
            if (is_line_continuation(&state)) {
                handle_line_continuation(&state);
                continue;
            }
            
            if (current == '*' && next == '/') {
                state.in_multi_line_comment = 0;
                state.input_pos += 2;
                state.column += 2;
            } else {
                if (current == '\n') {
                    state.line++;
                    state.column = 1;
                } else state.column++;
                state.input_pos++;
            }
            continue;
        }
        
        /* Handle preprocessor directive */
        if (state.in_preprocessor_directive) {
            if (current == '\n') {
                /* Check for line continuation before newline */
                if (state.directive_pos > 0 && 
                    state.directive_buffer[state.directive_pos - 1] == '\\') {
                    // Remove the backslash from directive buffer
                    state.directive_pos--;
                    state.directive_buffer[state.directive_pos] = '\0';
                    
                    // Skip the newline and continue directive on next line
                    state.input_pos++;
                    state.line++;
                    state.column = 1;
                } else {
                    /* End of directive line */
                    process_directive(&state);
                    state.in_preprocessor_directive = 0;
                    clear_directive_buffer(&state);
                    
                    add_to_output(&state, '\n');
                    state.input_pos++;
                    state.line++;
                    state.column = 1;
                }
            } else if (current == '\\' && next == '\n') {
                /* Continuation line - add backslash to directive buffer */
                add_to_directive_buffer(&state, current);
                state.input_pos += 2; /* Skip backslash and newline */
                state.line++;
                state.column = 1;
            } else {
                /* Add to directive buffer */
                add_to_directive_buffer(&state, current);
                state.input_pos++;
                state.column++;
            }
            continue;
        }
        
        /* Handle configuration macro */
        if (state.in_config_macro) {
            add_to_output(&state, current);
            state.input_pos++;
            state.column++;
            
            if (current == '_' && next == '_' && !is_alnum(state.input[state.input_pos + 1])) {
                add_to_output(&state, '_');
                state.input_pos++;
                state.column++;
                state.in_config_macro = 0;
            }
            continue;
        }
        
        /* Check for new states */
        if (current == '/' && next == '/') {
            state.in_single_line_comment = 1;
            state.input_pos += 2;
            state.column += 2;
        } else if (current == '/' && next == '*') {
            state.in_multi_line_comment = 1;
            state.input_pos += 2;
            state.column += 2;
        } else if (current == '"') {
            add_to_output(&state, '"');
            state.in_string = 1;
            state.input_pos++;
            state.column++;
        } else if (current == '\'') {
            add_to_output(&state, '\'');
            state.in_char = 1;
            state.input_pos++;
            state.column++;
        } else if (current == '#') {
            /* Start of preprocessor directive */
            state.in_preprocessor_directive = 1;
            clear_directive_buffer(&state);
            state.directive_start_line = state.line;
            state.directive_start_column = state.column;
            
            /* Add '#' to directive buffer */
            add_to_directive_buffer(&state, '#');
            state.input_pos++;
            state.column++;
        } else if (is_config_macro_start(&state)) {
            state.in_config_macro = 1;
        } else {
            /* Normal character */
            add_to_output(&state, current);
            
            if (current == '\n') {
                state.line++;
                state.column = 1;
            } else state.column++;
            state.input_pos++;
        }
    }
    
    /* Process any remaining directive */
    if (state.in_preprocessor_directive) {
        process_directive(&state);
    }
    
    /* Null-terminate the output */
    add_to_output(&state, '\0');
    
    /* Create result string */
    size_t result_len = strlen(state.output);
    char* result = malloc(result_len + 1);
    if (result) {
        memcpy(result, state.output, result_len);
        result[result_len] = '\0';
    }
    
    /* Clean up */
    free(state.output);
    
    if (error) *error = result ? 0 : 1;
    return result;
}
