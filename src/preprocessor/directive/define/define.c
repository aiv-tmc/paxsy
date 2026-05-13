#include "define.h"
#include "../../preprocessor_state.h"
#include "../../../errhandler/errhandler.h"
#include "../../../utils/common.h"
#include "../../../utils/str_utils.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*
 * Maximum nesting depth allowed during macro expansion of the
 * replacement text.  Exceeding this limit reports an error and
 * leaves the text unexpanded.
 */
#define MAX_EXPANSION_DEPTH 256

/*
 * Offset (in characters) from the beginning of the directive line to the
 * first character after the "define" keyword.  This value is used when
 * computing column numbers for error messages.
 *
 * "define" is 6 characters, plus one space (the typical separation) gives 7.
 */
#define DEFINE_OFFSET 7

/*
 * Offset for #undef similarly – "undef" has length 5, plus one space = 6.
 */
#define UNDEF_OFFSET 6

/* Forward declarations of internal helper functions. */
static ResultCode parse_object_like_macro(PreprocessorState *state, char *args);
static ResultCode parse_function_like_macro(PreprocessorState *state,
                                            char *name_start);
static char *extract_macro_name(char *str, size_t *name_len);
static char *skip_macro_whitespace(char *str);
static char *find_macro_replacement_end(char *str);
static ResultCode add_macro_to_table(PreprocessorState *state,
                                     char *name,
                                     char *value,
                                     int is_function_like,
                                     const char **params,
                                     size_t param_count);
static char *expand_macros_in_text(const MacroTable *table,
                                   const char *text,
                                   const char **expanding_stack,
                                   int expanding_count);

/*
 * DPPF__define - Process a #define directive.
 *
 * Dispatches to one of the specialised parsing routines depending
 * on whether the macro is object‑like or function‑like.
 */
void DPPF__define(PreprocessorState *state, char *args) {
    assert(state != NULL);
    assert(args != NULL);

    /* Skip leading whitespace after the directive keyword. */
    char *ptr = skip_macro_whitespace(args);
    if (is_empty(ptr)) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + DEFINE_OFFSET,
            "preproc",
            "Empty #define directive");
        return;
    }

    /*
     * Locate the macro name – it consists of identifier characters only.
     * We advance `ptr` until the first character that is not part of an
     * identifier.
     */
    char *name_start = ptr;
    while (*ptr && u__char_is_identifier_char(*ptr)) {
        ptr++;
    }

    /*
     * Determine the macro type: if the next non‑whitespace character after
     * the name is '(', the macro is function‑like; otherwise it is object‑like.
     */
    ptr = skip_macro_whitespace(ptr);
    ResultCode result;

    if (*ptr == '(') {
        result = parse_function_like_macro(state, name_start);
    } else {
        result = parse_object_like_macro(state, name_start);
    }

    if (result != RESULT_OK) {
        /* Error details have already been reported by the parsing routines. */
        return;
    }
}

/*
 * Parse a simple #define without parameters.
 *
 * Format: #define NAME [replacement text]
 *
 * The replacement text is trimmed of trailing whitespace, then any macros
 * that appear in it are expanded (except the macro NAME itself).
 */
static ResultCode parse_object_like_macro(PreprocessorState *state, char *args) {
    size_t name_len;
    char *name = extract_macro_name(args, &name_len);
    if (name == NULL) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column
                + DEFINE_OFFSET + (int)(args - state->directive_buffer),
            "preproc",
            "Invalid macro name");
        return RESULT_INVALID_ARGUMENT;
    }

    /* Locate the replacement text. */
    char *value_start = skip_macro_whitespace(args + name_len);
    char *value_end = find_macro_replacement_end(value_start);

    /* Trim trailing whitespace. */
    while (value_end > value_start && u__char_is_whitespace(*(value_end - 1))) {
        value_end--;
    }

    /* Extract the trimmed replacement text. */
    size_t value_len = value_end - value_start;
    char *value = malloc(value_len + 1);
    if (value == NULL) {
        free(name);
        return RESULT_OUT_OF_MEMORY;
    }
    if (value_len > 0) {
        memcpy(value, value_start, value_len);
    }
    value[value_len] = '\0';

    /*
     * Expand macros in the replacement text.
     * The name of the macro being defined is added to the expansion stack
     * so that it is never expanded during this process.
     */
    const char *exp_stack[1];
    exp_stack[0] = name;
    char *expanded = expand_macros_in_text(state->macro_table,
                                           value, exp_stack, 1);
    if (!expanded) {
        free(name);
        free(value);
        return RESULT_OUT_OF_MEMORY;
    }
    free(value);
    value = expanded;   /* use the fully expanded text */

    /* Register the macro. */
    ResultCode result = add_macro_to_table(state, name, value, 0, NULL, 0);

    free(name);
    free(value);
    return result;
}

/*
 * Parse a #define with a parameter list.
 *
 * Format: #define NAME( param1 , param2 , ... ) [replacement text]
 *
 * The replacement text is processed identically to object‑like macros:
 * trailing whitespace is removed, and macros appearing in it are expanded
 * (except NAME itself).
 */
static ResultCode parse_function_like_macro(PreprocessorState *state,
                                            char *name_start) {
    size_t name_len;
    char *name = extract_macro_name(name_start, &name_len);
    if (name == NULL) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column
                + DEFINE_OFFSET + (int)(name_start - state->directive_buffer),
            "preproc",
            "Invalid macro name");
        return RESULT_INVALID_ARGUMENT;
    }

    /* Advance past the name and any whitespace to reach the opening '('. */
    char *ptr = name_start + name_len;
    ptr = skip_macro_whitespace(ptr);
    if (*ptr != '(') {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column
                + DEFINE_OFFSET + (int)(ptr - state->directive_buffer),
            "preproc",
            "Expected '(' after macro name");
        free(name);
        return RESULT_INVALID_ARGUMENT;
    }
    ptr++;  /* skip '(' */

    /*
     * Parse the parameter list.
     * Parameters are separated by commas; the list is terminated by ')'.
     */
    char *param_names[MAX_MACRO_PARAMS];
    size_t param_count = 0;
    ResultCode result = RESULT_OK;

    while (*ptr && *ptr != ')') {
        ptr = skip_macro_whitespace(ptr);
        if (*ptr == ')') {
            /* Empty parameter list – just break out. */
            break;
        }

        /* Variadic macros are not supported yet. */
        if (*ptr == '.' && ptr[1] == '.' && ptr[2] == '.') {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column
                    + DEFINE_OFFSET + (int)(ptr - state->directive_buffer),
                "preproc",
                "Variadic macros are not yet supported");
            result = RESULT_ERROR;
            break;
        }

        /* A parameter name must start with a valid identifier character. */
        if (!u__char_is_identifier_start(*ptr)) {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column
                    + DEFINE_OFFSET + (int)(ptr - state->directive_buffer),
                "preproc",
                "Invalid parameter name");
            result = RESULT_INVALID_ARGUMENT;
            break;
        }

        /* Extract the parameter name. */
        char *param_start = ptr;
        while (u__char_is_identifier_char(*ptr)) {
            ptr++;
        }
        size_t param_len = ptr - param_start;

        /* Allocate and store the parameter name temporarily. */
        param_names[param_count] = malloc(param_len + 1);
        if (param_names[param_count] == NULL) {
            result = RESULT_OUT_OF_MEMORY;
            break;
        }
        memcpy(param_names[param_count], param_start, param_len);
        param_names[param_count][param_len] = '\0';
        param_count++;

        if (param_count >= MAX_MACRO_PARAMS) {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column
                    + DEFINE_OFFSET + (int)(ptr - state->directive_buffer),
                "preproc",
                "Too many macro parameters (maximum %d)",
                MAX_MACRO_PARAMS);
            result = RESULT_INVALID_ARGUMENT;
            break;
        }

        /* Skip whitespace after the parameter name and expect ',' or ')'. */
        ptr = skip_macro_whitespace(ptr);
        if (*ptr == ',') {
            ptr++;
        } else if (*ptr != ')') {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column
                    + DEFINE_OFFSET + (int)(ptr - state->directive_buffer),
                "preproc",
                "Expected ',' or ')' in parameter list");
            result = RESULT_INVALID_ARGUMENT;
            break;
        }
    }

    /* On error, release all temporary parameter strings and return. */
    if (result != RESULT_OK) {
        free(name);
        for (size_t i = 0; i < param_count; i++) {
            free(param_names[i]);
        }
        return result;
    }

    /* Ensure the parameter list is properly closed. */
    if (*ptr != ')') {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column
                + DEFINE_OFFSET + (int)(ptr - state->directive_buffer),
            "preproc",
            "Expected ')' to close parameter list");
        free(name);
        for (size_t i = 0; i < param_count; i++) {
            free(param_names[i]);
        }
        return RESULT_INVALID_ARGUMENT;
    }
    ptr++;  /* skip ')' */

    /* Locate and trim the replacement text. */
    ptr = skip_macro_whitespace(ptr);
    char *value_start = ptr;
    char *value_end = find_macro_replacement_end(value_start);
    while (value_end > value_start && u__char_is_whitespace(*(value_end - 1))) {
        value_end--;
    }

    size_t value_len = value_end - value_start;
    char *value = malloc(value_len + 1);
    if (value == NULL) {
        free(name);
        for (size_t i = 0; i < param_count; i++) {
            free(param_names[i]);
        }
        return RESULT_OUT_OF_MEMORY;
    }
    if (value_len > 0) {
        memcpy(value, value_start, value_len);
    }
    value[value_len] = '\0';

    /*
     * Expand macros in the replacement text (with the macro being defined
     * placed on the expansion stack to prevent self‑expansion).
     */
    const char *exp_stack[1];
    exp_stack[0] = name;
    char *expanded = expand_macros_in_text(state->macro_table,
                                           value, exp_stack, 1);
    if (!expanded) {
        free(name);
        for (size_t i = 0; i < param_count; i++) free(param_names[i]);
        free(value);
        return RESULT_OUT_OF_MEMORY;
    }
    free(value);
    value = expanded;

    /* Register the function‑like macro. */
    result = add_macro_to_table(state, name, value, 1,
                                (const char**)param_names, param_count);

    free(name);
    free(value);
    /* The table made its own copies of parameter names; we free ours. */
    for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
    }

    return result;
}

/*
 * Helper: add a macro to the table and report an error on failure.
 */
static ResultCode add_macro_to_table(PreprocessorState *state,
                                     char *name,
                                     char *value,
                                     int is_function_like,
                                     const char **params,
                                     size_t param_count) {
    if (macro_table_add(state->macro_table, name, value,
                        is_function_like, params, param_count)) {
        return RESULT_OK;
    }

    errhandler__report_error(
        ERROR_CODE_PP_MACRO_DEF_FAILED,
        state->directive_start_line,
        state->directive_start_column + DEFINE_OFFSET,
        "preproc",
        "Failed to define macro: %s",
        name);
    return RESULT_ERROR;
}

/*
 * Extract a valid C identifier from the beginning of a string.
 *
 * str      – input string.
 * name_len – receives the length of the extracted identifier (excluding null).
 *
 * Returns a newly allocated string containing the identifier, or NULL if
 * the string does not start with a valid identifier character.
 */
static char *extract_macro_name(char *str, size_t *name_len) {
    assert(str != NULL);
    assert(name_len != NULL);

    if (!u__char_is_identifier_start(*str)) {
        return NULL;
    }

    char *end = str;
    while (u__char_is_identifier_char(*end)) {
        end++;
    }

    *name_len = end - str;

    char *name = malloc(*name_len + 1);
    if (name == NULL) {
        return NULL;
    }

    memcpy(name, str, *name_len);
    name[*name_len] = '\0';
    return name;
}

/*
 * Skip spaces and horizontal tabs.  Newline characters are NOT skipped
 * because they signal the end of the preprocessor directive line.
 */
static char *skip_macro_whitespace(char *str) {
    if (str == NULL) return str;
    while (u__char_is_whitespace(*str)) {
        str++;
    }
    return str;
}

/*
 * Return a pointer to the end of the macro replacement text.
 * The replacement always ends at the first newline character (or at the
 * terminating null, whichever comes first).
 */
static char *find_macro_replacement_end(char *str) {
    if (str == NULL) return str;
    while (*str && !u__char_is_line_break(*str)) {
        str++;
    }
    return str;
}

/*
 * Expand all object‑like macros appearing in `text` that are not
 * currently on the `expanding_stack`.
 *
 * The expansion is recursive: after a macro is replaced by its value,
 * that value is itself expanded.  The `expanding_stack` keeps track of
 * macros currently being expanded; any macro whose name appears in this
 * stack is left unexpanded, preventing infinite recursion.
 *
 * table           – macro table used for lookups.
 * text            – null‑terminated input text.
 * expanding_stack – array of macro names currently being expanded.
 * expanding_count – number of entries in `expanding_stack`.
 *
 * Returns a newly allocated string containing the expanded text, or NULL
 * on memory allocation failure.
 */
static char *expand_macros_in_text(const MacroTable *table,
                                   const char *text,
                                   const char **expanding_stack,
                                   int expanding_count) {
    if (!table || !text) return NULL;

    /*
     * We build the result in a dynamically resized buffer.
     * Start with a reasonable initial capacity equal to the input length
     * (plus one for the null terminator).
     */
    size_t buf_size = strlen(text) + 1;
    char *result = malloc(buf_size);
    if (!result) return NULL;
    size_t result_len = 0;

    const char *p = text;
    while (*p) {
        /* Copy non‑identifier characters verbatim. */
        if (!u__char_is_identifier_start(*p)) {
            char c = *p++;
            /* Ensure buffer is large enough. */
            if (result_len + 1 >= buf_size) {
                buf_size = buf_size * 2 + 1;
                char *tmp = realloc(result, buf_size);
                if (!tmp) {
                    free(result);
                    return NULL;
                }
                result = tmp;
            }
            result[result_len++] = c;
            continue;
        }

        /* Read the full identifier. */
        const char *start = p;
        while (u__char_is_identifier_char(*p)) {
            p++;
        }
        size_t id_len = p - start;

        /*
         * Check whether this identifier names an object‑like macro
         * that is not already on the expansion stack.
         */
        const Macro *macro = macro_table_find(table, start);   /* linear search inside table */
        int do_expand = 0;
        if (macro && !macro->has_parameters) {
            /* Macro found; see if it is excluded. */
            int excluded = 0;
            for (int i = 0; i < expanding_count; i++) {
                if (strlen(expanding_stack[i]) == id_len &&
                    memcmp(expanding_stack[i], start, id_len) == 0) {
                    excluded = 1;
                    break;
                }
            }
            if (!excluded) {
                do_expand = 1;
            }
        }

        if (do_expand) {
            /*
             * Expand the macro's value recursively, adding this macro's name
             * to the expansion stack.
             */
            const char *new_stack[MAX_EXPANSION_DEPTH];
            int new_count = expanding_count;
            if (new_count < MAX_EXPANSION_DEPTH) {
                memcpy(new_stack, expanding_stack, new_count * sizeof(char*));
                new_stack[new_count++] = macro->name;
            } else {
                /* Too deep – skip expansion. */
                do_expand = 0;
            }

            if (do_expand) {
                char *expanded_val = expand_macros_in_text(table,
                                                           macro->value,
                                                           new_stack, new_count);
                if (!expanded_val) {
                    free(result);
                    return NULL;
                }
                size_t expanded_len = strlen(expanded_val);
                /* Ensure result buffer can hold the expanded text. */
                if (result_len + expanded_len >= buf_size) {
                    size_t need = result_len + expanded_len + 1;
                    size_t new_buf = (buf_size > need) ? buf_size * 2 : need * 2;
                    char *tmp = realloc(result, new_buf);
                    if (!tmp) {
                        free(expanded_val);
                        free(result);
                        return NULL;
                    }
                    result = tmp;
                    buf_size = new_buf;
                }
                memcpy(result + result_len, expanded_val, expanded_len);
                result_len += expanded_len;
                free(expanded_val);
                continue;
            }
        }
        /*
         * Identifier not eligible for expansion – copy it verbatim.
         */
        if (result_len + id_len >= buf_size) {
            size_t need = result_len + id_len + 1;
            size_t new_buf = (buf_size > need) ? buf_size * 2 : need * 2;
            char *tmp = realloc(result, new_buf);
            if (!tmp) {
                free(result);
                return NULL;
            }
            result = tmp;
            buf_size = new_buf;
        }
        memcpy(result + result_len, start, id_len);
        result_len += id_len;
    }

    /* Null‑terminate the result. */
    result[result_len] = '\0';
    return result;
}

/*
 * DPPF__undef - Process an #undef directive.
 *
 * Removes the named macro from the table.  A warning is emitted when
 * the macro to be undefined does not actually exist.
 */
void DPPF__undef(PreprocessorState *state, char *args) {
    assert(state != NULL);
    assert(args != NULL);

    char *ptr = skip_macro_whitespace(args);
    if (is_empty(ptr)) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + UNDEF_OFFSET,
            "preproc",
            "Empty #undef directive");
        return;
    }

    /* Extract the macro name. */
    char *name_start = ptr;
    while (u__char_is_identifier_char(*ptr)) {
        ptr++;
    }

    size_t name_len = ptr - name_start;
    if (name_len == 0) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + UNDEF_OFFSET,
            "preproc",
            "Invalid macro name in #undef");
        return;
    }

    if (name_len >= MAX_MACRO_NAME_LEN) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + UNDEF_OFFSET,
            "preproc",
            "Macro name too long in #undef (maximum %d characters)",
            MAX_MACRO_NAME_LEN - 1);
        return;
    }

    /* Copy the name into a local buffer for safety. */
    char name[MAX_MACRO_NAME_LEN];
    size_t copy_len = (name_len + 1 < MAX_MACRO_NAME_LEN) ? name_len + 1
                                                          : MAX_MACRO_NAME_LEN;
    u__str_copy_safe(name, name_start, copy_len);

    /* No extra tokens are allowed after the macro name. */
    ptr = skip_macro_whitespace(ptr);
    if (*ptr != '\0' && !u__char_is_line_break(*ptr)) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column
                + UNDEF_OFFSET + (int)(ptr - args),
            "preproc",
            "Extra characters after macro name in #undef");
        return;
    }

    /* Attempt to remove the macro; warn if it was not defined. */
    if (!macro_table_remove(state->macro_table, name)) {
        errhandler__report_error(
            ERROR_CODE_PP_UNDEFINED,
            state->directive_start_line,
            state->directive_start_column + UNDEF_OFFSET,
            "preproc",
            "Undefining undefined macro: %s",
            name);
    }
}
