#include "preprocessor.h"
#include "preprocessor_state.h"
#include "directive/include/include.h"
#include "directive/define/define.h"
#include "directive/define/macro.h"
#include "directive/conditional/conditional.h"
#include "../errhandler/errhandler.h"
#include "defmacros/defmacros.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Forward declarations of internal functions */
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

/* Character classification helpers (C99 compatible) */
static int is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_alnum(char c) {
    return is_digit(c) || is_alpha(c);
}

static int is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int is_identifier_char(char c) {
    return is_alnum(c) || c == '_';
}

/**
 * @brief Ensures that the output buffer has enough capacity for at least
 *        'needed' additional characters.
 * @param state  Preprocessor state.
 * @param needed Number of bytes required.
 * @return 1 on success, 0 on allocation failure.
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
 * @brief Appends a single character to the output buffer.
 * @param state Preprocessor state.
 * @param c     Character to append.
 *
 * Output is suppressed if the current conditional context indicates skipping.
 */
static void add_to_output(PreprocessorState* state, char c) {
    if (conditional_should_output(state)) {
        if (ensure_output_capacity(state, 1)) {
            state->output[state->output_pos++] = c;
        }
    }
}

/**
 * @brief Appends a string to the output buffer.
 * @param state Preprocessor state.
 * @param str   String to append (may be NULL).
 *
 * Updates line/column counters unconditionally, but output is only written
 * if conditional context permits.
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
 * @brief Clears the directive buffer.
 */
static void clear_directive_buffer(PreprocessorState* state) {
    state->directive_pos = 0;
    state->directive_buffer[0] = '\0';
}

/**
 * @brief Appends a character to the directive buffer.
 */
static void add_to_directive_buffer(PreprocessorState* state, char c) {
    if (state->directive_pos < sizeof(state->directive_buffer) - 1) {
        state->directive_buffer[state->directive_pos++] = c;
        state->directive_buffer[state->directive_pos] = '\0';
    }
}

/**
 * @brief Checks for a backslash-newline line continuation.
 * @return 1 if a line continuation is present, 0 otherwise.
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
 * @brief Skips a backslash-newline sequence and updates line/column counters.
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
 * @brief Processes characters inside a single-line comment (// ...).
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
 * @brief Processes characters inside a multi-line comment (/* ... * /).
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
 * @brief Processes a preprocessor directive line, handling line continuations.
 *
 * Collects characters into directive_buffer until a newline that is not
 * preceded by a line continuation backslash. Backslash-newline sequences
 * are skipped entirely and do not appear in the buffer.
 */
static void process_preprocessor_directive(PreprocessorState* state) {
    char current = state->input[state->input_pos];

    /* Handle line continuation (backslash followed by newline) */
    if (is_line_continuation(state)) {
        handle_line_continuation(state);
        /* Continue reading the directive on the next line */
        return;
    }

    if (current == '\n') {
        /* End of directive line – process it */
        process_directive(state);
        state->in_preprocessor_directive = 0;
        clear_directive_buffer(state);
        add_to_output(state, '\n');
        state->input_pos++;
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
 * @brief Processes a string literal, including escape sequences.
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
 * @brief Processes a character literal, including escape sequences.
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
 * @brief Collects an identifier from the input starting at the current position.
 * @param state Preprocessor state.
 * @return 1 if an identifier was collected, 0 otherwise.
 *
 * The collected identifier is stored in state->identifier_buffer.
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
 * @brief Processes an identifier: performs macro expansion if applicable.
 */
static void process_identifier(PreprocessorState* state) {
    if (state->identifier_pos == 0) {
        return;
    }

    /* Look up the macro (unless we are already expanding) */
    const Macro* macro = macro_table_find(state->macro_table, state->identifier_buffer);
    if (macro && !state->in_macro_expansion) {
        state->in_macro_expansion = 1;

        if (macro->has_parameters) {
            /* Function-like macro – for now just output the name (no expansion) */
            add_string_to_output(state, state->identifier_buffer);
        } else {
            /* Object-like macro – expand */
            add_string_to_output(state, macro->value);
        }

        state->in_macro_expansion = 0;
        return;
    }

    /* Not a macro, or we are already expanding – output the identifier as is */
    add_string_to_output(state, state->identifier_buffer);
}

/**
 * @brief Processes a character that is not inside any special state.
 *
 * Detects comments, string/character literals, directives, and identifiers.
 */
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
        /* Start of preprocessor directive */
        state->in_preprocessor_directive = 1;
        clear_directive_buffer(state);
        state->directive_start_line = state->line;
        state->directive_start_column = state->column;
        add_to_directive_buffer(state, '#');
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
 * @brief Parses and executes a preprocessor directive from the collected buffer.
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
 * @brief Internal function: processes a buffer of source code using an existing state.
 * @param state    Preprocessor state (must be initialized).
 * @param input    Source code to process.
 * @param filename Name of the file (for error reporting and __FILE__ macro).
 * @return 0 on success, non-zero on fatal error.
 *
 * This function temporarily replaces the state's input and position, processes
 * the given buffer, and then restores the original context. It is used for
 * recursive handling of $import and $using.
 */
static int process_buffer(PreprocessorState* state, const char* input, const char* filename) {
    /* Save current context */
    const char* saved_input = state->input;
    size_t saved_input_pos = state->input_pos;
    uint16_t saved_line = state->line;
    uint16_t saved_column = state->column;
    const char* saved_current_file = state->current_file;

    uint8_t saved_in_single_line_comment = state->in_single_line_comment;
    uint8_t saved_in_multi_line_comment = state->in_multi_line_comment;
    uint8_t saved_in_string = state->in_string;
    uint8_t saved_in_char = state->in_char;
    uint8_t saved_in_preprocessor_directive = state->in_preprocessor_directive;
    uint8_t saved_in_macro_expansion = state->in_macro_expansion;
    uint8_t saved_bracket_depth = state->bracket_depth;

    /* Set new context */
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

    /* Main processing loop */
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

    /* Flush any pending directive */
    if (state->in_preprocessor_directive) {
        process_directive(state);
    }

    /* Restore saved context */
    state->input = saved_input;
    state->input_pos = saved_input_pos;
    state->line = saved_line;
    state->column = saved_column;
    state->current_file = saved_current_file;

    state->in_single_line_comment = saved_in_single_line_comment;
    state->in_multi_line_comment = saved_in_multi_line_comment;
    state->in_string = saved_in_string;
    state->in_char = saved_in_char;
    state->in_preprocessor_directive = saved_in_preprocessor_directive;
    state->in_macro_expansion = saved_in_macro_expansion;
    state->bracket_depth = saved_bracket_depth;

    return 0;   /* Success (fatal errors not yet tracked) */
}

/**
 * @brief Public function: processes a source buffer using an existing state.
 *
 * This function is called by $import and $using to recursively preprocess
 * included files. The output is appended to the state's output buffer.
 *
 * @param state    Preprocessor state.
 * @param input    Source code to process.
 * @param filename Name of the file (for error reporting and __FILE__ macro).
 * @return 0 on success, non-zero on error.
 */
int preprocess_content(PreprocessorState* state, const char* input, const char* filename) {
    if (!state || !input || !filename) return 1;
    return process_buffer(state, input, filename);
}

/**
 * @brief Main entry point: preprocesses the entire input source.
 *
 * Creates a new preprocessor state, initializes built-in macros, and processes
 * the input. Returns a newly allocated string containing the preprocessed output.
 *
 * @param input    Source code to preprocess.
 * @param filename Name of the file (for error reporting and __FILE__ macro).
 * @param error    Pointer to store error code (0 = success, non-zero = error).
 * @return Preprocessed string (must be freed by caller) or NULL on error.
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

    /* Initialize built-in macros (__FILE__, __TIME__, etc.) */
    builtin_macros_init(state.macro_table, filename);

    /* Directive buffer */
    state.directive_pos = 0;
    state.directive_start_line = 0;
    state.directive_start_column = 0;
    state.directive_buffer[0] = '\0';

    /* Identifier buffer */
    state.identifier_pos = 0;
    state.identifier_buffer[0] = '\0';

    if (!state.output) {
        conditional_context_destroy(state.conditional_ctx);
        macro_table_destroy(state.macro_table);
        if (error) *error = 1;
        return NULL;
    }

    /* Process the input */
    if (process_buffer(&state, input, filename) != 0) {
        /* Fatal error */
        free(state.output);
        conditional_context_destroy(state.conditional_ctx);
        macro_table_destroy(state.macro_table);
        if (error) *error = 1;
        return NULL;
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
