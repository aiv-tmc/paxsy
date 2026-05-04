#include "error.h"
#include "../../../errhandler/errhandler.h"
#include <string.h>
#include <ctype.h>

/* Trim whitespace from the beginning and end of a string.
 * Returns a pointer inside the original string (no allocation). */
static char *trim_whitespace(char *str) {
    if (!str) return str;
    while (*str && isspace((unsigned char)*str)) ++str;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) --end;
    end[1] = '\0';
    return str;
}

/* Remove leading and trailing matching quotes from a string.
 * If the string starts with a quote (single or double) and ends with the same quote,
 * the quotes are stripped. Otherwise the string is returned unchanged.
 * Returns a pointer inside the original string (no allocation). */
static char *strip_quotes(char *str) {
    if (!str || *str == '\0') return str;
    char quote = *str;
    if (quote != '"' && quote != '\'') return str;
    size_t len = strlen(str);
    if (len < 2 || str[len - 1] != quote) return str;
    str[len - 1] = '\0';    /* remove closing quote */
    return str + 1;         /* skip opening quote */
}

/* Implementation of the #error directive.
 * Reports a preprocessing error with the message provided in args.
 * The message may be quoted or unquoted; whitespace is trimmed. */
void DPPF__error(PreprocessorState *state, char *args) {
    if (!args) return;
    char *trimmed = trim_whitespace(args);
    char *msg = strip_quotes(trimmed);
    errhandler__report_error(ERROR_CODE_PP_ERROR_DIR,
                             state->directive_start_line,
                             state->directive_start_column,
                             "preproc",
                             "%s", msg);
}
