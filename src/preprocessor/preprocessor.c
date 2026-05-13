#include "preprocessor.h"
#include "preprocessor_state.h"
#include "directive/conditional/conditional.h"
#include "directive/define/define.h"
#include "directive/define/macro.h"
#include "directive/error/error.h"
#include "directive/include/include.h"
#include "../errhandler/errhandler.h"
#include "defmacros/defmacros.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Character classification helpers (avoid reliance on locale-dependent isalpha etc.). */
#define IS_ALPHA(c)  (((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z'))
#define IS_DIGIT(c)  ((c) >= '0' && (c) <= '9')
#define IS_ALNUM(c)  (IS_DIGIT(c) || IS_ALPHA(c))
#define IS_IDENT_CHAR(c) (IS_ALNUM(c) || (c) == '_')
#define IS_WHITESPACE(c) ((c) == ' ' || (c) == '\t' || (c) == '\r')

/* Internal buffer sizes */
#define IDENT_BUF_SIZE 256
#define DIRECTIVE_BUF_SIZE 256
#define ARG_BUF_SIZE 1024

/* Forward declarations of internal static functions */
static int ensure_output_capacity(PreprocessorState* state, size_t needed);
static void add_to_output(PreprocessorState* state, char c);
static void add_string_to_output(PreprocessorState* state, const char* str);
static void clear_directive_buffer(PreprocessorState* state);
static void add_to_directive_buffer(PreprocessorState* state, char c);
static void process_directive(PreprocessorState* state);
static int is_line_continuation(PreprocessorState* state);
static void handle_line_continuation(PreprocessorState* state);
static void process_normal_character(PreprocessorState* state);
static void process_single_line_comment(PreprocessorState* state);
static void process_multi_line_comment(PreprocessorState* state);
static void process_preprocessor_directive(PreprocessorState* state);
static void process_string_literal(PreprocessorState* state);
static void process_char_literal(PreprocessorState* state);
static int collect_identifier(PreprocessorState* state);
static void process_identifier(PreprocessorState* state);
static int process_buffer(PreprocessorState* state, const char* input, const char* filename);

/* Macro expansion helper functions */
static int collect_macro_arguments(PreprocessorState* state, const char* input,
                                   size_t* pos, char*** args_out, size_t* arg_count_out);
static char* expand_function_macro(const Macro* macro, char** args, size_t arg_count,
                                   PreprocessorState* state);

/* Ensures that the output buffer has enough capacity for at least 'needed'
   additional characters. Returns 1 on success, 0 on allocation failure. */
static int ensure_output_capacity(PreprocessorState* state, size_t needed) {
    if (state->output_pos + needed < state->output_capacity) {
        return 1;
    }
    size_t new_capacity = state->output_capacity * 2;
    if (new_capacity < state->output_pos + needed) {
        new_capacity = state->output_pos + needed + 1024;
    }
    char* new_output = realloc(state->output, new_capacity);
    if (!new_output) {
        return 0;
    }
    state->output = new_output;
    state->output_capacity = new_capacity;
    return 1;
}

/* Appends a single character to the output buffer, respecting conditional state. */
static void add_to_output(PreprocessorState* state, char c) {
    if (conditional_should_output(state)) {
        if (ensure_output_capacity(state, 1)) {
            state->output[state->output_pos++] = c;
        }
    }
}

/* Appends a string to the output buffer.
   Updates line/column counters unconditionally, but output is written only
   when conditional context allows. */
static void add_string_to_output(PreprocessorState* state, const char* str) {
    if (!str) return;
    size_t len = strlen(str);
    if (len == 0) return;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') {
            state->line++;
            state->column = 1;
        } else {
            state->column++;
        }
    }
    if (conditional_should_output(state)) {
        if (ensure_output_capacity(state, len)) {
            memcpy(state->output + state->output_pos, str, len);
            state->output_pos += len;
        }
    }
}

/* Clears the directive buffer. */
static void clear_directive_buffer(PreprocessorState* state) {
    state->directive_pos = 0;
    state->directive_buffer[0] = '\0';
}

/* Appends a character to the directive buffer. */
static void add_to_directive_buffer(PreprocessorState* state, char c) {
    if (state->directive_pos < sizeof(state->directive_buffer) - 1) {
        state->directive_buffer[state->directive_pos++] = c;
        state->directive_buffer[state->directive_pos] = '\0';
    }
}

/* Checks for a backslash-newline line continuation. */
static int is_line_continuation(PreprocessorState* state) {
    const char* input = state->input;
    size_t pos = state->input_pos;
    if (input[pos] == '\\') {
        char next = input[pos + 1];
        if (next == '\n' || next == '\r') {
            if (next == '\r' && input[pos + 2] == '\n')
                return 1;
            return 1;
        }
    }
    return 0;
}

/* Skips a backslash-newline sequence and updates line/column counters. */
static void handle_line_continuation(PreprocessorState* state) {
    char next = state->input[state->input_pos + 1];
    state->input_pos++;
    state->column++;
    if (next == '\r' && state->input[state->input_pos + 1] == '\n') {
        state->input_pos += 2;
        state->line++;
        state->column = 1;
    } else if (next == '\n' || next == '\r') {
        state->input_pos++;
        state->line++;
        state->column = 1;
    }
}

/* Processes characters inside a single-line comment. */
static void process_single_line_comment(PreprocessorState* state) {
    if (is_line_continuation(state)) {
        handle_line_continuation(state);
        return;
    }
    char current = state->input[state->input_pos];
    if (current == '\n') {
        add_to_output(state, '\n');
        state->input_pos++;
        state->line++;
        state->column = 1;
        state->in_single_line_comment = 0;
    } else {
        state->input_pos++;
        state->column++;
    }
}

/* Processes characters inside a multi-line comment. */
static void process_multi_line_comment(PreprocessorState* state) {
    if (is_line_continuation(state)) {
        handle_line_continuation(state);
        return;
    }
    char current = state->input[state->input_pos];
    char next = state->input[state->input_pos + 1];
    if (current == '*' && next == '/') {
        state->in_multi_line_comment = 0;
        state->input_pos += 2;
        state->column += 2;
    } else {
        if (current == '\n') {
            state->line++;
            state->column = 1;
        } else {
            state->column++;
        }
        state->input_pos++;
    }
}

/* Processes a preprocessor directive line, handling line continuations. */
static void process_preprocessor_directive(PreprocessorState* state) {
    if (is_line_continuation(state)) {
        handle_line_continuation(state);
        return;
    }
    char current = state->input[state->input_pos];
    if (current == '\n') {
        process_directive(state);
        state->in_preprocessor_directive = 0;
        clear_directive_buffer(state);
        add_to_output(state, '\n');
        state->input_pos++;
        state->line++;
        state->column = 1;
    } else {
        add_to_directive_buffer(state, current);
        state->input_pos++;
        state->column++;
    }
}

/* Processes a string literal, including escape sequences. */
static void process_string_literal(PreprocessorState* state) {
    char current = state->input[state->input_pos];
    char next = state->input[state->input_pos + 1];
    add_to_output(state, current);
    state->input_pos++;
    state->column++;
    if (current == '\\' && next != '\0') {
        add_to_output(state, next);
        state->input_pos++;
        state->column++;
    } else if (current == '"') {
        state->in_string = 0;
    } else if (current == '\n') {
        state->line++;
        state->column = 1;
    }
}

/* Processes a character literal, including escape sequences. */
static void process_char_literal(PreprocessorState* state) {
    char current = state->input[state->input_pos];
    char next = state->input[state->input_pos + 1];
    add_to_output(state, current);
    state->input_pos++;
    state->column++;
    if (current == '\\' && next != '\0') {
        add_to_output(state, next);
        state->input_pos++;
        state->column++;
    } else if (current == '\'') {
        state->in_char = 0;
    } else if (current == '\n') {
        state->line++;
        state->column = 1;
    }
}

/* Collects an identifier from the input starting at current position.
   Returns 1 on success, 0 otherwise. */
static int collect_identifier(PreprocessorState* state) {
    if (!IS_IDENT_CHAR(state->input[state->input_pos])) return 0;
    state->identifier_pos = 0;
    while (IS_IDENT_CHAR(state->input[state->input_pos]) &&
           state->identifier_pos < IDENT_BUF_SIZE - 1) {
        state->identifier_buffer[state->identifier_pos++] = state->input[state->input_pos];
        state->input_pos++;
        state->column++;
    }
    state->identifier_buffer[state->identifier_pos] = '\0';
    return 1;
}

/* Collects arguments for a function-like macro call.
   Assumes the opening parenthesis has been consumed. Arguments are separated
   by commas at the top level of parenthesis nesting.
   On success, *args_out contains an array of argument strings (newly allocated),
   and *arg_count_out holds their count. The caller must free each argument and
   the array itself. */
static int collect_macro_arguments(PreprocessorState* state, const char* input,
                                   size_t* pos, char*** args_out, size_t* arg_count_out) {
    size_t local_pos = *pos;
    int paren_depth = 1;
    int arg_start = local_pos;
    char** args = NULL;
    size_t arg_count = 0;
    size_t arg_capacity = 0;
    while (input[local_pos] && paren_depth > 0) {
        char c = input[local_pos];
        if (c == '(') paren_depth++;
        else if (c == ')') paren_depth--;
        else if (c == ',' && paren_depth == 1) {
            size_t arg_len = local_pos - arg_start;
            while (arg_len > 0 && IS_WHITESPACE(input[arg_start])) {
                arg_start++; arg_len--;
            }
            while (arg_len > 0 && IS_WHITESPACE(input[arg_start + arg_len - 1])) arg_len--;
            if (arg_count >= arg_capacity) {
                size_t new_cap = arg_capacity == 0 ? 4 : arg_capacity * 2;
                char** new_arr = realloc(args, new_cap * sizeof(char*));
                if (!new_arr) {
                    for (size_t i = 0; i < arg_count; i++) free(args[i]);
                    free(args);
                    return 0;
                }
                args = new_arr;
                arg_capacity = new_cap;
            }
            args[arg_count] = malloc(arg_len + 1);
            if (!args[arg_count]) {
                for (size_t i = 0; i < arg_count; i++) free(args[i]);
                free(args);
                return 0;
            }
            memcpy(args[arg_count], input + arg_start, arg_len);
            args[arg_count][arg_len] = '\0';
            arg_count++;
            arg_start = local_pos + 1;
        }
        local_pos++;
    }
    if (paren_depth != 0) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 state->line, state->column,
                                 "preproc", "Unmatched '(' in macro call");
        for (size_t i = 0; i < arg_count; i++) free(args[i]);
        free(args);
        return 0;
    }
    size_t last_arg_len = local_pos - arg_start - 1;
    while (last_arg_len > 0 && IS_WHITESPACE(input[arg_start])) {
        arg_start++; last_arg_len--;
    }
    while (last_arg_len > 0 && IS_WHITESPACE(input[arg_start + last_arg_len - 1])) last_arg_len--;
    if (arg_count >= arg_capacity) {
        size_t new_cap = arg_capacity == 0 ? 1 : arg_capacity * 2;
        char** new_arr = realloc(args, new_cap * sizeof(char*));
        if (!new_arr) {
            for (size_t i = 0; i < arg_count; i++) free(args[i]);
            free(args);
            return 0;
        }
        args = new_arr;
        arg_capacity = new_cap;
    }
    args[arg_count] = malloc(last_arg_len + 1);
    if (!args[arg_count]) {
        for (size_t i = 0; i < arg_count; i++) free(args[i]);
        free(args);
        return 0;
    }
    memcpy(args[arg_count], input + arg_start, last_arg_len);
    args[arg_count][last_arg_len] = '\0';
    arg_count++;
    *pos = local_pos;
    *args_out = args;
    *arg_count_out = arg_count;
    return 1;
}

/* Expands a function-like macro by substituting parameter names with
   the provided arguments. The result is a newly allocated string.
   For simplicity, only simple substitution (no '#' or '##' operators) is done,
   respecting whole-word boundaries. */
static char* expand_function_macro(const Macro* macro, char** args, size_t arg_count,
                                   PreprocessorState* state) {
    (void)state;
    const char* body = macro->value;
    size_t body_len = strlen(body);
    char* result = NULL;
    size_t result_size = 0;
    size_t result_cap = body_len + 1;
    result = malloc(result_cap);
    if (!result) return NULL;
    result[0] = '\0';
    const char* p = body;
    while (*p) {
        if (IS_IDENT_CHAR(*p)) {
            const char* start = p;
            while (IS_IDENT_CHAR(*p)) p++;
            size_t id_len = p - start;
            int replaced = 0;
            for (size_t i = 0; i < macro->param_count; i++) {
                if (macro->param_names[i] &&
                    strlen(macro->param_names[i]) == id_len &&
                    strncmp(start, macro->param_names[i], id_len) == 0) {
                    const char* arg = (i < arg_count) ? args[i] : "";
                    size_t arg_len = strlen(arg);
                    while (result_size + arg_len + 1 > result_cap) {
                        result_cap *= 2;
                        char* tmp = realloc(result, result_cap);
                        if (!tmp) { free(result); return NULL; }
                        result = tmp;
                    }
                    memcpy(result + result_size, arg, arg_len);
                    result_size += arg_len;
                    replaced = 1;
                    break;
                }
            }
            if (!replaced) {
                while (result_size + id_len + 1 > result_cap) {
                    result_cap *= 2;
                    char* tmp = realloc(result, result_cap);
                    if (!tmp) { free(result); return NULL; }
                    result = tmp;
                }
                memcpy(result + result_size, start, id_len);
                result_size += id_len;
            }
        } else {
            while (result_size + 2 > result_cap) {
                result_cap *= 2;
                char* tmp = realloc(result, result_cap);
                if (!tmp) { free(result); return NULL; }
                result = tmp;
            }
            result[result_size++] = *p;
            p++;
        }
    }
    result[result_size] = '\0';
    return result;
}

/* Processes an identifier: performs macro expansion if applicable.
   Supports object-like and function-like macros. */
static void process_identifier(PreprocessorState* state) {
    if (state->identifier_pos == 0) return;
    const Macro* macro = macro_table_find(state->macro_table, state->identifier_buffer);
    if (macro && !state->in_macro_expansion) {
        if (macro->has_parameters) {
            size_t saved_pos = state->input_pos;
            size_t saved_col = state->column;
            const char* p = state->input + saved_pos;
            while (IS_WHITESPACE(*p)) { p++; saved_col++; }
            if (*p == '(') {
                state->in_macro_expansion = 1;
                state->input_pos = (p - state->input) + 1;
                state->column = saved_col + 1;
                char** args = NULL;
                size_t arg_count = 0;
                if (collect_macro_arguments(state, state->input, &state->input_pos,
                                            &args, &arg_count)) {
                    char* expanded = expand_function_macro(macro, args, arg_count, state);
                    if (expanded) {
                        add_string_to_output(state, expanded);
                        free(expanded);
                    } else {
                        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                                 state->line, state->column, "memory",
                                                 "Out of memory expanding macro '%s'",
                                                 state->identifier_buffer);
                    }
                }
                if (args) {
                    for (size_t i = 0; i < arg_count; i++) free(args[i]);
                    free(args);
                }
                state->in_macro_expansion = 0;
                return;
            } else {
                state->input_pos = saved_pos;
                state->column = saved_col;
            }
        } else {
            state->in_macro_expansion = 1;
            add_string_to_output(state, macro->value);
            state->in_macro_expansion = 0;
            return;
        }
    }
    add_string_to_output(state, state->identifier_buffer);
}

/* Processes a character that is not inside any special state. */
static void process_normal_character(PreprocessorState* state) {
    char current = state->input[state->input_pos];
    char next = state->input[state->input_pos + 1];
    if (current == '/' && next == '/') {
        state->in_single_line_comment = 1;
        state->input_pos++;
        state->column++;
    } else if (current == '/' && next == '*') {
        state->in_multi_line_comment = 1;
        state->input_pos += 2;
        state->column += 2;
    } else if (current == '"') {
        add_to_output(state, '"');
        state->in_string = 1;
        state->input_pos++;
        state->column++;
    } else if (current == '\'') {
        add_to_output(state, '\'');
        state->in_char = 1;
        state->input_pos++;
        state->column++;
    } else if (current == '#') {
        state->in_preprocessor_directive = 1;
        clear_directive_buffer(state);
        state->directive_start_line = state->line;
        state->directive_start_column = state->column;
        add_to_directive_buffer(state, '#');
        state->input_pos++;
        state->column++;
    } else if (IS_IDENT_CHAR(current) && !state->in_macro_expansion) {
        size_t saved_pos = state->input_pos;
        size_t saved_column = state->column;
        if (collect_identifier(state)) {
            process_identifier(state);
        } else {
            state->input_pos = saved_pos;
            state->column = saved_column;
            add_to_output(state, current);
            if (current == '\n') { state->line++; state->column = 1; }
            else state->column++;
            state->input_pos++;
        }
    } else {
        add_to_output(state, current);
        if (current == '\n') { state->line++; state->column = 1; }
        else state->column++;
        state->input_pos++;
    }
}

/* Parses and executes a preprocessor directive from the collected buffer. */
static void process_directive(PreprocessorState* state) {
    char* directive = state->directive_buffer;
    if (state->directive_pos == 0) return;
    char* ptr = directive;
    while (*ptr && IS_WHITESPACE(*ptr)) ptr++;
    if (*ptr != '#') return;
    ptr++;
    while (*ptr && IS_WHITESPACE(*ptr)) ptr++;
    if (*ptr == '\0') return;
    char* command_start = ptr;
    while (*ptr && !IS_WHITESPACE(*ptr) && *ptr != '\n' && *ptr != '\r') ptr++;
    size_t command_len = ptr - command_start;
    if (command_len == 0) return;
    char command[32];
    if (command_len >= sizeof(command)) {
        errhandler__report_error(ERROR_CODE_PP_DIR_TOO_LONG,
                                 state->directive_start_line,
                                 state->directive_start_column + (command_start - directive),
                                 "preproc", "Preprocessor directive command too long");
        return;
    }
    memcpy(command, command_start, command_len);
    command[command_len] = '\0';
    while (*ptr && IS_WHITESPACE(*ptr)) ptr++;
    char* args = ptr;
    if (strcmp(command, "define") == 0) {
        DPPF__define(state, args);
    } else if (strcmp(command, "undef") == 0) {
        DPPF__undef(state, args);
    } else if (strcmp(command, "include") == 0) {
        DPPF__include(state, args);
    } else if (strcmp(command, "using") == 0) {
        DPPF__using(state, args);
    } else if (strcmp(command, "if") == 0) {
        DPPF__if(state, args);
    } else if (strcmp(command, "ifdef") == 0) {
        DPPF__ifdef(state, args);
    } else if (strcmp(command, "ifndef") == 0) {
        DPPF__ifndef(state, args);
    } else if (strcmp(command, "elif") == 0) {
        DPPF__elif(state, args);
    } else if (strcmp(command, "else") == 0) {
        DPPF__else(state, args);
    } else if (strcmp(command, "endif") == 0) {
        DPPF__endif(state, args);
    } else if (strcmp(command, "error") == 0) {
        DPPF__error(state, args);
    } else {
        errhandler__report_error(ERROR_CODE_PP_UNKNOW_DIR,
                                 state->directive_start_line,
                                 state->directive_start_column + (command_start - directive),
                                 "preproc", "Unknown preprocessor directive: %s", command);
    }
}

/* Internal function: processes a buffer of source code using an existing state.
   Saves and restores the current context to allow recursive processing. */
static int process_buffer(PreprocessorState* state, const char* input, const char* filename) {
    const char* saved_input = state->input;
    size_t saved_input_pos = state->input_pos;
    uint16_t saved_line = state->line;
    uint16_t saved_column = state->column;
    const char* saved_current_file = state->current_file;
    uint8_t saved_flags[6];
    saved_flags[0] = state->in_single_line_comment;
    saved_flags[1] = state->in_multi_line_comment;
    saved_flags[2] = state->in_string;
    saved_flags[3] = state->in_char;
    saved_flags[4] = state->in_preprocessor_directive;
    saved_flags[5] = state->in_macro_expansion;
    uint8_t saved_bracket = state->bracket_depth;
    state->input = input;
    state->input_pos = 0;
    state->line = 1;
    state->column = 1;
    state->current_file = filename;
    state->in_single_line_comment = 0;
    state->in_multi_line_comment = 0;
    state->in_string = 0;
    state->in_char = 0;
    state->in_preprocessor_directive = 0;
    state->in_macro_expansion = 0;
    state->bracket_depth = 0;
    while (state->input[state->input_pos] != '\0') {
        if (is_line_continuation(state)) {
            handle_line_continuation(state);
            continue;
        }
        if (state->in_single_line_comment) {
            process_single_line_comment(state);
        } else if (state->in_multi_line_comment) {
            process_multi_line_comment(state);
        } else if (state->in_preprocessor_directive) {
            process_preprocessor_directive(state);
        } else if (state->in_string) {
            process_string_literal(state);
        } else if (state->in_char) {
            process_char_literal(state);
        } else {
            process_normal_character(state);
        }
    }
    if (state->in_preprocessor_directive) {
        process_directive(state);
    }
    state->input = saved_input;
    state->input_pos = saved_input_pos;
    state->line = saved_line;
    state->column = saved_column;
    state->current_file = saved_current_file;
    state->in_single_line_comment = saved_flags[0];
    state->in_multi_line_comment = saved_flags[1];
    state->in_string = saved_flags[2];
    state->in_char = saved_flags[3];
    state->in_preprocessor_directive = saved_flags[4];
    state->in_macro_expansion = saved_flags[5];
    state->bracket_depth = saved_bracket;
    return 0;
}

/* Public function: preprocess a buffer of source code using an existing state.
   Used by #include and #using for recursive preprocessing. */
int preprocess_content(PreprocessorState* state, const char* input, const char* filename) {
    if (!state || !input || !filename) return 1;
    return process_buffer(state, input, filename);
}

/* Main entry point: preprocesses the entire input source.
   Creates a new preprocessor state, initializes built-in macros, processes
   the input, and returns the preprocessed string. */
char* preprocess(const char* input, const char* filename, int* error) {
    if (!input || !filename) {
        if (error) *error = 1;
        return NULL;
    }
    size_t input_len = strlen(input);
    PreprocessorState state;
    memset(&state, 0, sizeof(state));
    state.input = input;
    state.output_capacity = input_len * 2 + 1024;
    state.output = malloc(state.output_capacity);
    if (!state.output) {
        if (error) *error = 1;
        return NULL;
    }
    state.line = 1;
    state.column = 1;
    state.current_file = filename;
    state.macro_table = macro_table_create();
    if (!state.macro_table) {
        free(state.output);
        if (error) *error = 1;
        return NULL;
    }
    state.conditional_ctx = conditional_context_create();
    if (!state.conditional_ctx) {
        macro_table_destroy(state.macro_table);
        free(state.output);
        if (error) *error = 1;
        return NULL;
    }
    builtin_macros_init(state.macro_table, filename);
    if (process_buffer(&state, input, filename) != 0) {
        free(state.output);
        macro_table_destroy(state.macro_table);
        conditional_context_destroy(state.conditional_ctx);
        if (error) *error = 1;
        return NULL;
    }
    add_to_output(&state, '\0');
    size_t result_len = state.output_pos;
    char* result = malloc(result_len + 1);
    if (result) {
        memcpy(result, state.output, result_len);
        result[result_len] = '\0';
    }
    free(state.output);
    macro_table_destroy(state.macro_table);
    conditional_context_destroy(state.conditional_ctx);
    free_included_registry();
    if (error) *error = result ? 0 : 1;
    return result;
}
