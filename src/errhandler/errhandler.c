#include "errhandler.h"
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

/*
 * Internal constants.
 */
#define ERROR_MESSAGE_BUFFER_SIZE       1024    /* max length of a formatted message */
#define CONTEXT_BUFFER_SIZE             8       /* max length of a context identifier */
#define INITIAL_CAPACITY                8       /* initial slots per array */
#define MAX_EXPONENTIAL_CAPACITY        1024    /* threshold for switching to linear growth */
#define LINEAR_INCREMENT                256     /* slots added after exponential phase */
#define TAB_SIZE                        8       /* spaces per tab stop */
#define EXPAND_TABS_BUFFER_SIZE         2048    /* local buffer for tab expansion */

/*
 * Structure representing a single diagnostic entry.
 */
typedef struct {
    char* message;               /* formatted error message */
    char* filename;              /* source file name (owned copy) */
    uint16_t line;               /* 1‑based line number */
    uint8_t column;              /* 1‑based column (byte offset) */
    uint8_t length;              /* length in bytes of the highlighted token */
    ErrorLevel level;            /* severity level */
    char context[CONTEXT_BUFFER_SIZE]; /* subsystem identifier */
    uint16_t error_code;         /* 16‑bit hexadecimal error code */
    char* source_line_copy;      /* owned copy of the source line */
    const char* source_line_ref; /* reference to user‑provided line (if not copied) */
} ErrorEntry;

/*
 * Global state of the error manager.
 */
typedef struct {
    ErrorEntry* error_entries;
    ErrorEntry* warning_entries;
    uint32_t error_count;
    uint32_t warning_count;
    uint32_t error_capacity;
    uint32_t warning_capacity;
    const char** source_lines;
    uint32_t source_line_count;
    bool copy_source_lines;      /* if true, copy source lines; else reference */
    bool warnings_as_errors;
    bool suppress_warnings;
} ErrorManager;

/*
 * Singleton instance – all functions operate on this global structure.
 */
static ErrorManager em = {
    .error_entries = NULL,
    .warning_entries = NULL,
    .error_count = 0,
    .warning_count = 0,
    .error_capacity = 0,
    .warning_capacity = 0,
    .source_lines = NULL,
    .source_line_count = 0,
    .copy_source_lines = true,
    .warnings_as_errors = false,
    .suppress_warnings = false
};

/*
 * Current source filename, set by errhandler__set_current_filename().
 * Copied so that it remains valid even if the original pointer is freed.
 */
static char* current_filename = NULL;

/* Forward declarations of internal helpers. */
static bool ensure_capacity(ErrorEntry** array, uint32_t* capacity, uint32_t count);
static void add_error_entry(ErrorLevel level, uint16_t error_code,
                            uint16_t line, uint8_t column, uint8_t length,
                            const char* context, const char* message);
static char* duplicate_string(const char* str);
static uint16_t count_digits(uint16_t number);
static uint16_t visual_column(const char* line, uint16_t byte_col);
static uint16_t visual_token_length(const char* segment, uint8_t byte_len,
                                    uint16_t start_visual_col);
static void expand_tabs(const char* src, char* dst, size_t dst_size);
static void print_error_source_line(const ErrorEntry* entry, bool is_warning);
static void print_error_entry(const ErrorEntry* entry, bool is_warning);
static bool validate_error_code(uint16_t error_code);

/*
 * Duplicate a null‑terminated string. Returns NULL on failure or NULL input.
 */
static char* duplicate_string(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = (char*)malloc(len + 1);
    if (dup) {
        memcpy(dup, str, len);
        dup[len] = '\0';
    }
    return dup;
}

/*
 * Return the number of decimal digits needed to represent a uint16_t.
 */
static uint16_t count_digits(uint16_t number) {
    if (number == 0) return 1;
    uint16_t digits = 0;
    while (number) {
        digits++;
        number /= 10;
    }
    return digits;
}

/*
 * Verify that an error code is not zero (the reserved invalid code).
 * If zero, it is replaced by a generic syntax error.
 */
static bool validate_error_code(uint16_t error_code) {
    return error_code != 0;
}

/*
 * Ensure a dynamic array of error entries has room for one more element.
 * Growth strategy: exponential up to MAX_EXPONENTIAL_CAPACITY, then linear.
 * Returns true on success, false on allocation failure.
 */
static bool ensure_capacity(ErrorEntry** array, uint32_t* capacity, uint32_t count) {
    if (count < *capacity) return true;

    uint32_t new_cap;
    if (*capacity == 0) {
        new_cap = INITIAL_CAPACITY;
    } else if (*capacity < MAX_EXPONENTIAL_CAPACITY) {
        new_cap = *capacity * 2;
    } else {
        new_cap = *capacity + LINEAR_INCREMENT;
    }

    if (new_cap <= *capacity) {
        fprintf(stderr, "\033[31mERROR\033[0m: Error entries capacity overflow\n");
        return false;
    }

    ErrorEntry* new_array = (ErrorEntry*)realloc(*array, new_cap * sizeof(ErrorEntry));
    if (!new_array) {
        fprintf(stderr, "\033[31mERROR\033[0m: Failed to allocate error entries\n");
        return false;
    }

    *array = new_array;
    *capacity = new_cap;
    return true;
}

/*
 * Internal function that creates a diagnostic entry and stores it in the
 * appropriate array (errors or warnings).
 */
static void add_error_entry(ErrorLevel level, uint16_t error_code,
                            uint16_t line, uint8_t column, uint8_t length,
                            const char* context, const char* message) {
    if (!validate_error_code(error_code)) {
        fprintf(stderr, "\033[33mWARNING\033[0m: Invalid error code: 0x%04X, using default\n", error_code);
        error_code = ERROR_CODE_SYNTAX_GENERIC;
    }

    bool is_warning = (level == ERROR_LEVEL_WARNING);
    ErrorEntry** array = is_warning ? &em.warning_entries : &em.error_entries;
    uint32_t* count   = is_warning ? &em.warning_count   : &em.error_count;
    uint32_t* cap     = is_warning ? &em.warning_capacity : &em.error_capacity;

    if (!ensure_capacity(array, cap, *count)) {
        return;
    }

    ErrorEntry* entry = &(*array)[*count];
    memset(entry, 0, sizeof(ErrorEntry));

    if (message) {
        entry->message = duplicate_string(message);
    }
    if (current_filename) {
        entry->filename = duplicate_string(current_filename);
    }

    entry->line = line;
    entry->column = column;
    entry->length = (length > 0) ? length : 1;
    entry->level = level;
    entry->error_code = error_code;

    if (context) {
        strncpy(entry->context, context, CONTEXT_BUFFER_SIZE - 1);
        entry->context[CONTEXT_BUFFER_SIZE - 1] = '\0';
    }

    /* Store the source line if available. */
    if (line > 0 && em.source_lines != NULL && line <= em.source_line_count) {
        const char* src_line = em.source_lines[line - 1]; /* 0‑based */
        if (src_line) {
            if (em.copy_source_lines) {
                entry->source_line_copy = duplicate_string(src_line);
            } else {
                entry->source_line_ref = src_line;
            }
        }
    }

    (*count)++;
}

/*
 * Format the message using a va_list and pass it to add_error_entry.
 * This function is the core worker behind all public reporting functions.
 */
static void report_error_va(ErrorLevel level, uint16_t error_code,
                            uint16_t line, uint8_t column, uint8_t length,
                            const char* context, const char* format, va_list args) {
    char buffer[ERROR_MESSAGE_BUFFER_SIZE];
    int written = vsnprintf(buffer, sizeof(buffer), format, args);

    if (written < 0) {
        snprintf(buffer, sizeof(buffer), "Error message formatting failed");
    } else if ((size_t)written >= sizeof(buffer)) {
        buffer[sizeof(buffer) - 1] = '\0'; /* ensure null termination */
    }

    add_error_entry(level, error_code, line, column, length, context, buffer);
}

/*
 * Expand tab characters to spaces assuming a fixed tab width.
 */
static void expand_tabs(const char* src, char* dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return;
    size_t col = 0;
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if (*src == '\t') {
            size_t spaces = TAB_SIZE - (col % TAB_SIZE);
            if (spaces == 0) spaces = TAB_SIZE;
            for (size_t s = 0; s < spaces && i < dst_size - 1; s++) {
                dst[i++] = ' ';
                col++;
            }
        } else {
            dst[i++] = *src;
            col++;
        }
        src++;
    }
    dst[i] = '\0';
}

/*
 * Compute the visual column (in monospaced display) corresponding to a
 * byte offset in a line that may contain tabs.
 */
static uint16_t visual_column(const char* line, uint16_t byte_col) {
    if (!line) return 0;
    uint16_t vis = 0;
    for (uint16_t i = 0; i < byte_col && line[i]; i++) {
        if (line[i] == '\t')
            vis = (vis / TAB_SIZE + 1) * TAB_SIZE;
        else
            vis++;
    }
    return vis;
}

/*
 * Calculate the visual length of a token that starts at a given visual column,
 * accounting for tabs inside the token.
 */
static uint16_t visual_token_length(const char* segment, uint8_t byte_len,
                                    uint16_t start_visual_col) {
    if (!segment) return 1;
    uint16_t vis_len = 0;
    uint16_t cur_col = start_visual_col;
    for (uint8_t i = 0; i < byte_len && segment[i]; i++) {
        if (segment[i] == '\t') {
            uint16_t spaces = TAB_SIZE - (cur_col % TAB_SIZE);
            if (spaces == 0) spaces = TAB_SIZE;
            vis_len += spaces;
            cur_col += spaces;
        } else {
            vis_len++;
            cur_col++;
        }
    }
    return (vis_len == 0) ? 1 : vis_len;
}

/*
 * Print the problematic source line with a caret marker highlighting the
 * exact location and length of the token.
 */
static void print_error_source_line(const ErrorEntry* entry, bool is_warning) {
    if (!entry) return;

    const char* raw_line = entry->source_line_copy ?
                           entry->source_line_copy : entry->source_line_ref;
    if (!raw_line) return;

    char expanded[EXPAND_TABS_BUFFER_SIZE];
    expand_tabs(raw_line, expanded, sizeof(expanded));
    if (expanded[0] == '\0') return;

    uint16_t line_num = entry->line;
    uint8_t col_byte = entry->column;
    uint8_t token_len = entry->length;

    uint16_t visual_col = 0;
    uint16_t visual_len = 1;
    size_t expanded_len = strlen(expanded);

    if (col_byte > 0) {
        visual_col = visual_column(raw_line, col_byte - 1);

        size_t raw_len = strlen(raw_line);
        if (col_byte - 1 + token_len > raw_len)
            token_len = (uint8_t)(raw_len - (col_byte - 1));

        if (token_len > 0) {
            visual_len = visual_token_length(raw_line + (col_byte - 1),
                                             token_len, visual_col);
        }

        if (visual_col + visual_len > expanded_len)
            visual_len = (uint16_t)(expanded_len - visual_col);
        if (visual_len == 0) visual_len = 1;
    } else {
        visual_col = 0;
        visual_len = 1;
    }

    printf("  %*u | %s\n", count_digits(line_num), line_num, expanded);
    printf("  %*s | ", count_digits(line_num), "");

    if (is_warning)
        printf("\033[33m");
    else
        printf("\033[31m");

    for (uint16_t i = 0; i < visual_col && i < expanded_len; i++)
        putchar(' ');

    for (uint16_t i = 0; i < visual_len && (visual_col + i) < expanded_len; i++)
        putchar('^');

    printf("\033[0m\n");
}

/*
 * Print a complete diagnostic entry (filename, severity, error code, message,
 * and optionally the source line with a caret).
 */
static void print_error_entry(const ErrorEntry* entry, bool is_warning) {
    printf("%s: ", entry->filename);

    if (is_warning) {
        printf("\033[33mWARNING\033[0m");
    } else {
        if (entry->level == ERROR_LEVEL_FATAL)
            printf("\033[31mFATAL\033[0m");
        else
            printf("\033[31mERROR\033[0m");
    }

    printf("[%04X]", entry->error_code);
    if (entry->context[0] != '\0')
        printf("(%s): ", entry->context);
    printf("%s\n", entry->message ? entry->message : "(no message)");

    if (entry->line > 0) {
        print_error_source_line(entry, is_warning);
    }
}

/*
 * Variadic wrappers for the public functions.
 * They simply extract the va_list and delegate to the va_list‑based worker.
 */
void errhandler__report_error_ex(ErrorLevel level, uint16_t error_code,
                                 uint16_t line, uint8_t column, uint8_t length,
                                 const char* context, const char* format, ...) {
    va_list args;
    va_start(args, format);
    report_error_va(level, error_code, line, column, length, context, format, args);
    va_end(args);
}

void errhandler__report_error_ex_va(ErrorLevel level, uint16_t error_code,
                                    uint16_t line, uint8_t column, uint8_t length,
                                    const char* context, const char* format,
                                    va_list args) {
    report_error_va(level, error_code, line, column, length, context, format, args);
}

void errhandler__set_current_filename(const char* filename) {
    if (current_filename) {
        free(current_filename);
        current_filename = NULL;
    }
    if (filename) {
        current_filename = duplicate_string(filename);
    }
}

void errhandler__set_source_code(const char** source_lines, uint16_t line_count) {
    em.source_lines = source_lines;
    em.source_line_count = line_count;
}

void errhandler__clear_source_code(void) {
    em.source_lines = NULL;
    em.source_line_count = 0;
}

void errhandler__set_copy_source(bool enable) {
    em.copy_source_lines = enable;
}

int errhandler__load_source_file(const char* filename) {
    if (!filename) return -1;
    FILE* f = fopen(filename, "r");
    if (!f) return -1;

    /* Free any previously stored source lines if they were copied. */
    if (em.source_lines && em.copy_source_lines) {
        for (uint32_t i = 0; i < em.source_line_count; i++) {
            free((void*)em.source_lines[i]);
        }
    }
    free((void*)em.source_lines);
    em.source_lines = NULL;
    em.source_line_count = 0;

    char** lines = NULL;
    uint16_t line_count = 0;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), f)) {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len-1] == '\n') buffer[len-1] = '\0';
        char* line = duplicate_string(buffer);
        if (!line) {
            for (uint16_t i = 0; i < line_count; i++) free(lines[i]);
            free(lines);
            fclose(f);
            return -1;
        }
        char** new_lines = realloc(lines, (line_count + 1) * sizeof(char*));
        if (!new_lines) {
            free(line);
            for (uint16_t i = 0; i < line_count; i++) free(lines[i]);
            free(lines);
            fclose(f);
            return -1;
        }
        lines = new_lines;
        lines[line_count++] = line;
    }
    fclose(f);

    em.source_lines = (const char**)lines;
    em.source_line_count = line_count;
    em.copy_source_lines = true;   /* we own these copies */
    return 0;
}

void errhandler__print_errors(void) {
    for (uint32_t i = 0; i < em.error_count; i++) {
        print_error_entry(&em.error_entries[i], false);
    }
    if (em.warnings_as_errors) {
        for (uint32_t i = 0; i < em.warning_count; i++) {
            print_error_entry(&em.warning_entries[i], false);
        }
    }
}

void errhandler__print_warnings(void) {
    if (em.suppress_warnings) return;
    if (em.warnings_as_errors) return;   /* already printed as errors */
    for (uint32_t i = 0; i < em.warning_count; i++) {
        print_error_entry(&em.warning_entries[i], true);
    }
}

bool errhandler__has_errors(void) {
    if (em.error_count > 0) return true;
    if (em.warnings_as_errors && em.warning_count > 0) return true;
    return false;
}

bool errhandler__has_warnings(void) {
    return em.warning_count > 0;
}

void errhandler__free_error_manager(void) {
    for (uint32_t i = 0; i < em.error_count; i++) {
        free(em.error_entries[i].message);
        free(em.error_entries[i].filename);
        free(em.error_entries[i].source_line_copy);
    }
    free(em.error_entries);
    em.error_entries = NULL;
    em.error_count = 0;
    em.error_capacity = 0;

    for (uint32_t i = 0; i < em.warning_count; i++) {
        free(em.warning_entries[i].message);
        free(em.warning_entries[i].filename);
        free(em.warning_entries[i].source_line_copy);
    }
    free(em.warning_entries);
    em.warning_entries = NULL;
    em.warning_count = 0;
    em.warning_capacity = 0;

    if (em.source_lines && em.copy_source_lines) {
        for (uint32_t i = 0; i < em.source_line_count; i++) {
            free((void*)em.source_lines[i]);
        }
    }
    free((void*)em.source_lines);
    em.source_lines = NULL;
    em.source_line_count = 0;
    em.copy_source_lines = true;

    if (current_filename) {
        free(current_filename);
        current_filename = NULL;
    }
}

uint16_t errhandler__get_error_count(void) {
    uint32_t count = em.error_count;
    if (em.warnings_as_errors) count += em.warning_count;
    return (count > UINT16_MAX) ? UINT16_MAX : (uint16_t)count;
}

uint16_t errhandler__get_warning_count(void) {
    return (em.warning_count > UINT16_MAX) ? UINT16_MAX : (uint16_t)em.warning_count;
}

const char* errhandler__get_error_level_string(ErrorLevel level) {
    switch (level) {
        case ERROR_LEVEL_WARNING: return "WARNING";
        case ERROR_LEVEL_ERROR:   return "ERROR";
        case ERROR_LEVEL_FATAL:   return "FATAL";
        default:                  return "UNKNOWN";
    }
}

bool errhandler__parse_error_code(const char* error_code_str,
                                  char type[2], char group[2], char number[3]) {
    if (!error_code_str || strlen(error_code_str) != 4)
        return false;

    for (int i = 0; i < 4; i++) {
        if (!isxdigit((unsigned char)error_code_str[i]))
            return false;
    }

    if (type) {
        type[0] = error_code_str[0];
        type[1] = '\0';
    }
    if (group) {
        group[0] = error_code_str[1];
        group[1] = '\0';
    }
    if (number) {
        number[0] = error_code_str[2];
        number[1] = error_code_str[3];
        number[2] = '\0';
    }
    return true;
}

void errhandler__set_warnings_as_errors(bool enable) {
    em.warnings_as_errors = enable;
}

void errhandler__set_suppress_warnings(bool suppress) {
    em.suppress_warnings = suppress;
}
