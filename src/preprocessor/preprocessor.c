#include "preprocessor.h"
#include "preprocessor_state.h"
#include "directive/include/include.h"
#include "directive/define/define.h"
#include "directive/define/macro.h"
#include "directive/conditional/conditional.h"
#include "../errhandler/errhandler.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Forward declarations */
static int ensure_output_capacity(PreprocessorState* state, size_t needed);
static void add_to_output(PreprocessorState* state, char c);
static void add_string_to_output(PreprocessorState* state, const char* str);
static int is_config_macro_start(PreprocessorState* state);
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
static void process_config_macro(PreprocessorState* state);
static int collect_identifier(PreprocessorState* state);
static void process_identifier(PreprocessorState* state);

/* Character classification helpers (C99 compatible) */
static int is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int is_alnum(char c) {
    return (c >= '0' && c <= '9') || is_alpha(c);
}

static int is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int is_identifier_char(char c) {
    return is_alnum(c) || c == '_';
}

/**
 * Ensure output buffer has enough capacity.
 * Reallocates if necessary.
 */
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

/**
 * Add a single character to the output buffer.
 * Output is suppressed if conditional context says skip.
 */
static void add_to_output(PreprocessorState* state, char c) {
    if (conditional_should_output(state)) {
        if (ensure_output_capacity(state, 1)) {
            state->output[state->output_pos++] = c;
        }
    }
}

/**
 * Add a string to the output buffer.
 * Line/column counters are always updated; output is written only if allowed.
 */
static void add_string_to_output(PreprocessorState* state, const char* str) {
    if (!str) {
        return;
    }

    size_t len = strlen(str);
    if (len == 0) {
        return;
    }

    /* Update line and column counters unconditionally */
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') {
            state->line++;
            state->column = 1;
        } else {
            state->column++;
        }
    }

    /* Write to output only if conditional state permits */
    if (conditional_should_output(state)) {
        if (ensure_output_capacity(state, len)) {
            memcpy(state->output + state->output_pos, str, len);
            state->output_pos += len;
        }
    }
}

/**
 * Detect the start of a configuration macro (e.g., __FOO).
 */
static int is_config_macro_start(PreprocessorState* state) {
    const char* input = state->input;
    size_t pos = state->input_pos;

    if (input[pos] != '_' || input[pos + 1] != '_') {
        return 0;
    }

    char c = input[pos + 2];
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/**
 * Clear the directive buffer.
 */
static void clear_directive_buffer(PreprocessorState* state) {
    state->directive_pos = 0;
    state->directive_buffer[0] = '\0';
}

/**
 * Append a character to the directive buffer.
 */
static void add_to_directive_buffer(PreprocessorState* state, char c) {
    if (state->directive_pos < sizeof(state->directive_buffer) - 1) {
        state->directive_buffer[state->directive_pos++] = c;
        state->directive_buffer[state->directive_pos] = '\0';
    }
}

/**
 * Check for a backslash-newline line continuation.
 */
static int is_line_continuation(PreprocessorState* state) {
    const char* input = state->input;
    size_t pos = state->input_pos;

    if (input[pos] == '\\') {
        char next = input[pos + 1];
        if (next == '\n' || next == '\r') {
            if (next == '\r' && input[pos + 2] == '\n') {
                return 1;
            }
            return 1;
        }
    }
    return 0;
}

/**
 * Skip over a backslash-newline sequence and update line/column counters.
 */
static void handle_line_continuation(PreprocessorState* state) {
    char next = state->input[state->input_pos + 1];

    /* Skip backslash */
    state->input_pos++;
    state->column++;

    /* Handle different newline styles */
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

/**
 * Process characters inside a single-line comment (// ...).
 */
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

/**
 * Process characters inside a multi-line comment (/* ... * /).
 */
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

/**
 * Process a preprocessor directive line by collecting it into the buffer.
 * Handles line continuations and final dispatch.
 */
static void process_preprocessor_directive(PreprocessorState* state) {
    char current = state->input[state->input_pos];

    if (current == '\n') {
        /* Check for line continuation before newline */
        if (state->directive_pos > 0 &&
            state->directive_buffer[state->directive_pos - 1] == '\\') {
            /* Remove the backslash and continue on next line */
            state->directive_pos--;
            state->directive_buffer[state->directive_pos] = '\0';
            state->input_pos++;
            state->line++;
            state->column = 1;
        } else {
            /* End of directive line – process it */
            process_directive(state);
            state->in_preprocessor_directive = 0;
            clear_directive_buffer(state);
            add_to_output(state, '\n');
            state->input_pos++;
            state->line++;
            state->column = 1;
        }
    } else if (current == '\\' && state->input[state->input_pos + 1] == '\n') {
        /* Backslash at end of line: continuation */
        add_to_directive_buffer(state, current);
        state->input_pos += 2;
        state->line++;
        state->column = 1;
    } else {
        /* Normal directive character */
        add_to_directive_buffer(state, current);
        state->input_pos++;
        state->column++;
    }
}

/**
 * Process a string literal, including escape sequences.
 */
static void process_string_literal(PreprocessorState* state) {
    char current = state->input[state->input_pos];
    char next = state->input[state->input_pos + 1];

    add_to_output(state, current);
    state->input_pos++;
    state->column++;

    if (current == '\\' && next != '\0') {
        /* Escape sequence */
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

/**
 * Process a character literal, including escape sequences.
 */
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

/**
 * Process a configuration macro (__FOO__) – simply pass through.
 */
static void process_config_macro(PreprocessorState* state) {
    char current = state->input[state->input_pos];
    char next = state->input[state->input_pos + 1];

    add_to_output(state, current);
    state->input_pos++;
    state->column++;

    if (current == '_' && next == '_' &&
        !is_alnum(state->input[state->input_pos + 1])) {
        add_to_output(state, '_');
        state->input_pos++;
        state->column++;
        state->in_config_macro = 0;
    }
}

/**
 * Collect an identifier from the input starting at the current position.
 * Stores it in state->identifier_buffer and returns 1 on success.
 */
static int collect_identifier(PreprocessorState* state) {
    if (!is_identifier_char(state->input[state->input_pos])) {
        return 0;
    }

    state->identifier_pos = 0;
    while (is_identifier_char(state->input[state->input_pos]) &&
           state->identifier_pos < sizeof(state->identifier_buffer) - 1) {
        state->identifier_buffer[state->identifier_pos++] = state->input[state->input_pos];
        state->input_pos++;
        state->column++;
    }
    state->identifier_buffer[state->identifier_pos] = '\0';

    return 1;
}

/**
 * Process an identifier: handle configuration macros, object-like macros,
 * and function-like macros (call detection, no expansion yet).
 */
static void process_identifier(PreprocessorState* state) {
    if (state->identifier_pos == 0) {
        return;
    }

    /* Check for configuration macro (__FOO) */
    if (state->identifier_buffer[0] == '_' &&
        state->identifier_buffer[1] == '_' &&
        is_alnum(state->identifier_buffer[2])) {
        add_string_to_output(state, state->identifier_buffer);
        state->in_config_macro = 1;
        return;
    }

    /* Check for macro expansion (unless already expanding) */
    if (!state->in_macro_expansion) {
        const Macro* macro = macro_table_find(state->macro_table, state->identifier_buffer);
        if (macro) {
            state->in_macro_expansion = 1;

            if (macro->has_parameters && state->input[state->input_pos] == '(') {
                /* Function-like macro call – not expanded yet */
                add_string_to_output(state, state->identifier_buffer);
            } else if (!macro->has_parameters) {
                /* Object-like macro – expand value */
                add_string_to_output(state, macro->value);
            } else {
                /* Function-like macro without '(' – output name only */
                add_string_to_output(state, state->identifier_buffer);
            }

            state->in_macro_expansion = 0;
            return;
        }
    }

    /* Not a macro or in expansion – output identifier as is */
    add_string_to_output(state, state->identifier_buffer);
}

/**
 * Process a character that is not inside any special state.
 * Detects comments, literals, directives, configuration macros, identifiers.
 */
static void process_normal_character(PreprocessorState* state) {
    char current = state->input[state->input_pos];
    char next = state->input[state->input_pos + 1];

    if (current == '/' && next == '/') {
        state->in_single_line_comment = 1;
        state->input_pos += 2;
        state->column += 2;
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
        /* Start of preprocessor directive */
        state->in_preprocessor_directive = 1;
        clear_directive_buffer(state);
        state->directive_start_line = state->line;
        state->directive_start_column = state->column;
        add_to_directive_buffer(state, '#');
        state->input_pos++;
        state->column++;
    } else if (is_config_macro_start(state)) {
        state->in_config_macro = 1;
        add_to_output(state, current);
        state->input_pos++;
        state->column++;
    } else if (is_identifier_char(current) && !state->in_macro_expansion) {
        /* Try to collect and process an identifier */
        size_t saved_pos = state->input_pos;
        size_t saved_column = state->column;

        if (collect_identifier(state)) {
            process_identifier(state);
        } else {
            /* Not an identifier – restore and output as normal char */
            state->input_pos = saved_pos;
            state->column = saved_column;
            add_to_output(state, current);
            if (current == '\n') {
                state->line++;
                state->column = 1;
            } else {
                state->column++;
            }
            state->input_pos++;
        }
    } else {
        /* Ordinary character */
        add_to_output(state, current);
        if (current == '\n') {
            state->line++;
            state->column = 1;
        } else {
            state->column++;
        }
        state->input_pos++;
    }
}

/**
 * Parse and execute a preprocessor directive from the collected buffer.
 * Dispatches to appropriate handler functions.
 */
static void process_directive(PreprocessorState* state) {
    char* directive = state->directive_buffer;

    if (state->directive_pos == 0) {
        return;
    }

    char* ptr = directive;
    while (*ptr && is_whitespace(*ptr)) ptr++;
    if (*ptr != '#') return;
    ptr++;
    while (*ptr && is_whitespace(*ptr)) ptr++;

    if (*ptr == '\0') return;

    /* Extract directive name */
    char* command_start = ptr;
    while (*ptr && !is_whitespace(*ptr) && *ptr != '\n' && *ptr != '\r') ptr++;
    size_t command_len = ptr - command_start;
    if (command_len == 0) return;

    char command[32];
    if (command_len >= sizeof(command)) {
        errhandler__report_error(
            ERROR_CODE_PP_DIR_TOO_LONG,
            state->directive_start_line,
            state->directive_start_column + (command_start - directive),
            "preproc",
            "Preprocessor directive command too long"
        );
        return;
    }

    memcpy(command, command_start, command_len);
    command[command_len] = '\0';

    /* Skip whitespace to reach arguments */
    while (*ptr && is_whitespace(*ptr)) ptr++;
    char* args = ptr;

    int known_directive = 0;

    /* Macro definition directives */
    if (strcmp(command, "define") == 0) {
        DPPF__define(state, args);
        known_directive = 1;
    } else if (strcmp(command, "undef") == 0) {
        DPPF__undef(state, args);
        known_directive = 1;
    } else if (strcmp(command, "using") == 0) {
        DPPF__using(state, args);
        known_directive = 1;
    } else if (strcmp(command, "import") == 0) {
        DPPF__import(state, args);
        known_directive = 1;
    } else if (strcmp(command, "if") == 0) {
        DPPF__if(state, args);
        known_directive = 1;
    } else if (strcmp(command, "ifdef") == 0) {
        DPPF__ifdef(state, args);
        known_directive = 1;
    } else if (strcmp(command, "ifndef") == 0) {
        DPPF__ifndef(state, args);
        known_directive = 1;
    } else if (strcmp(command, "elif") == 0) {
        DPPF__elif(state, args);
        known_directive = 1;
    } else if (strcmp(command, "else") == 0) {
        DPPF__else(state, args);
        known_directive = 1;
    } else if (strcmp(command, "endif") == 0) {
        DPPF__endif(state, args);
        known_directive = 1;
    }

    if (!known_directive) {
        errhandler__report_error(
            ERROR_CODE_PP_UNKNOW_DIR,
            state->directive_start_line,
            state->directive_start_column + (command_start - directive),
            "preproc",
            "Unknown preprocessor directive: %s",
            command
        );
    }
}

/**
 * Main entry point: preprocess the input source.
 * Returns a newly allocated string with the preprocessed output.
 */
char* preprocess(const char* input, const char* filename, int* error) {
    if (!input || !filename) {
        if (error) *error = 1;
        return NULL;
    }

    size_t input_len = strlen(input);
    PreprocessorState state;

    /* Basic state initialization */
    state.input = input;
    state.input_pos = 0;
    state.output_pos = 0;
    state.output_capacity = input_len * 2 + 1024;
    state.output = malloc(state.output_capacity);
    state.line = 1;
    state.column = 1;

    /* State flags */
    state.in_single_line_comment = 0;
    state.in_multi_line_comment = 0;
    state.in_string = 0;
    state.in_char = 0;
    state.in_preprocessor_directive = 0;
    state.in_config_macro = 0;
    state.in_macro_expansion = 0;
    state.bracket_depth = 0;
    state.current_file = filename;

    /* Macro table */
    state.macro_table = macro_table_create();
    if (!state.macro_table) {
        free(state.output);
        if (error) *error = 1;
        return NULL;
    }

    /* Conditional compilation context */
    state.conditional_ctx = conditional_context_create();
    if (!state.conditional_ctx) {
        macro_table_destroy(state.macro_table);
        free(state.output);
        if (error) *error = 1;
        return NULL;
    }

    /* Directive buffer */
    state.directive_pos = 0;
    state.directive_start_line = 0;
    state.directive_start_column = 0;
    state.directive_buffer[0] = '\0';

    /* Identifier buffer */
    state.identifier_pos = 0;
    state.identifier_buffer[0] = '\0';

    /* Expansion buffer */
    state.expansion_pos = 0;
    state.expansion_buffer[0] = '\0';

    if (!state.output) {
        conditional_context_destroy(state.conditional_ctx);
        macro_table_destroy(state.macro_table);
        if (error) *error = 1;
        return NULL;
    }

    /* Main processing loop */
    while (state.input[state.input_pos] != '\0') {
        if (is_line_continuation(&state)) {
            handle_line_continuation(&state);
            continue;
        }

        if (state.in_single_line_comment) {
            process_single_line_comment(&state);
        } else if (state.in_multi_line_comment) {
            process_multi_line_comment(&state);
        } else if (state.in_preprocessor_directive) {
            process_preprocessor_directive(&state);
        } else if (state.in_string) {
            process_string_literal(&state);
        } else if (state.in_char) {
            process_char_literal(&state);
        } else if (state.in_config_macro) {
            process_config_macro(&state);
        } else {
            process_normal_character(&state);
        }
    }

    /* Flush any pending directive */
    if (state.in_preprocessor_directive) {
        process_directive(&state);
    }

    /* Null-terminate the output */
    add_to_output(&state, '\0');

    /* Allocate result string */
    size_t result_len = state.output_pos;
    char* result = malloc(result_len + 1);
    if (result) {
        memcpy(result, state.output, result_len);
        result[result_len] = '\0';
    }

    /* Cleanup */
    free(state.output);
    macro_table_destroy(state.macro_table);
    conditional_context_destroy(state.conditional_ctx);

    if (error) *error = result ? 0 : 1;
    return result;
}
