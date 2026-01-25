#include "preprocessor.h"
#include "DPP__begin/DPPF__begin.h"
#include "DPP__program/DPPF__program.h"
#include "DPP__include/DPPF__include.h"
#include "DPP__define/DPPF__define.h"
#include "DPP__ifdef/DPPF__ifdef.h"
#include "DPP__ifndef/DPPF__ifndef.h"
#include "DPP__if/DPPF__if.h"
#include "DPP__elif/DPPF__elif.h"
#include "DPP__else/DPPF__else.h"
#include "DPP__endif/DPPF__endif.h"

#include <stdlib.h>
#include <string.h>

/* Preprocessor state structure */
typedef struct {
    const char* input;
    char* output;
    size_t input_pos;
    size_t output_pos;
    size_t output_capacity;
    uint16_t line;
    uint16_t column;
    uint8_t in_single_line_comment : 1;
    uint8_t in_multi_line_comment : 1;
    uint8_t in_string : 1;
    uint8_t in_char : 1;
    uint8_t in_preprocessor_directive : 1;
    uint8_t in_config_macro : 1;
    uint8_t bracket_depth;
} PreprocessorState;

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
    
    if (!state.output) {
        if (error) *error = 1;
        return NULL;
    }
    
    /* Main processing loop */
    while (state.input[state.input_pos] != '\0') {
        char current = state.input[state.input_pos];
        char next = state.input[state.input_pos + 1];
        
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
                size_t check_pos = state.input_pos;
                while (state.input[check_pos + 1] == ' ' || 
                       state.input[check_pos + 1] == '\t') {
                    check_pos++;
                }
                if (state.input[check_pos + 1] != '#')
                    state.in_preprocessor_directive = 0;
                add_to_output(&state, '\n');
                state.input_pos++;
                state.line++;
                state.column = 1;
            } else {
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
            state.in_preprocessor_directive = 1;
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
