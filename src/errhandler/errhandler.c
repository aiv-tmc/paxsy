#include "errhandler.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#define ERROR_MESSAGE_BUFFER_SIZE       1024    /* Max formatted message length */
#define CONTEXT_BUFFER_SIZE            8       /* Max context identifier length */
#define INITIAL_CAPACITY               8       /* Initial capacity for arrays */
#define MAX_EXPONENTIAL_CAPACITY       1024    /* Switch to linear growth after this */
#define LINEAR_INCREMENT              256     /* Linear growth step */
#define TAB_SIZE                      8       /* Spaces per tab character */
#define MAX_LINE_DIGITS               4       /* Minimum width for line numbers */
#define MAX_COL_DIGITS                3       /* Minimum width for column numbers */
#define EXPAND_TABS_BUFFER_SIZE       2048    /* Local buffer for tab expansion */

typedef struct {
    char* message;               /* Formatted error message (dynamically allocated) */
    char* filename;              /* Source file name (dynamically allocated) */
    uint16_t line;               /* 1‑based line number */
    uint8_t column;             /* 1‑based column (byte offset) */
    uint8_t length;            /* Token length in bytes */
    ErrorLevel level;          /* Severity */
    char context[CONTEXT_BUFFER_SIZE]; /* Context string */
    uint16_t error_code;       /* 16‑bit hexadecimal error code */
    char* source_line_copy;    /* Owned copy of source line (if copy enabled) */
    const char* source_line_ref; /* Reference to user‑provided line (if copy disabled) */
} ErrorEntry;

typedef struct {
    ErrorEntry* error_entries;      /* Array for ERROR and FATAL entries */
    ErrorEntry* warning_entries;    /* Array for WARNING entries */
    uint32_t error_count;          /* Number of used slots in error_entries */
    uint32_t warning_count;        /* Number of used slots in warning_entries */
    uint32_t error_capacity;       /* Allocated capacity of error_entries */
    uint32_t warning_capacity;     /* Allocated capacity of warning_entries */
    const char** source_lines;     /* User‑provided source lines (may be NULL) */
    uint32_t source_line_count;    /* Number of lines in source_lines */
    bool copy_source_lines;        /* If true, copy source lines; else reference */
} ErrorManager;

static ErrorManager em = {
    .error_entries = NULL,
    .warning_entries = NULL,
    .error_count = 0,
    .warning_count = 0,
    .error_capacity = 0,
    .warning_capacity = 0,
    .source_lines = NULL,
    .source_line_count = 0,
    .copy_source_lines = true      /* default: safe copying */
};

static char* current_filename = NULL;   /* Current source file name (dynamically allocated) */

static bool ensure_capacity(ErrorEntry** array, uint32_t* capacity, uint32_t count);
static void add_error_entry(ErrorLevel level, uint16_t error_code,
                            uint16_t line, uint8_t column, uint8_t length,
                            const char* context, const char* message);
static char* duplicate_string(const char* str);
static uint16_t count_digits(uint16_t number);
static uint16_t visual_column(const char* line, uint16_t byte_col);
static uint16_t visual_token_length(const char* segment, uint8_t byte_len,
                                    uint16_t start_visual_col);
static uint16_t visual_line_length(const char* line);
static void expand_tabs(const char* src, char* dst, size_t dst_size);
static void print_error_source_line(const ErrorEntry* entry, bool is_warning);
static void print_error_entry(const ErrorEntry* entry, bool is_warning);
static bool validate_error_code(uint16_t error_code);

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

static uint16_t count_digits(uint16_t number) {
    if (number == 0) return 1;
    uint16_t digits = 0;
    while (number) {
        digits++;
        number /= 10;
    }
    return digits;
}

static bool validate_error_code(uint16_t error_code) {
    return error_code != 0;
}

static bool ensure_capacity(ErrorEntry** array, uint32_t* capacity, uint32_t count) {
    if (count < *capacity) return true;

    uint32_t new_cap;
    if (*capacity == 0) {
        new_cap = INITIAL_CAPACITY;
    } else if (*capacity < MAX_EXPONENTIAL_CAPACITY) {
        new_cap = *capacity * 2;          /* exponential growth */
    } else {
        new_cap = *capacity + LINEAR_INCREMENT; /* linear growth */
    }

    /* overflow check */
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

static void add_error_entry(ErrorLevel level, uint16_t error_code,
                            uint16_t line, uint8_t column, uint8_t length,
                            const char* context, const char* message) {
    /* Validate error code – replace invalid with generic syntax error */
    if (!validate_error_code(error_code)) {
        fprintf(stderr, "\033[33mWARNING\033[0m: Invalid error code: 0x%04X, using default\n", error_code);
        error_code = ERROR_CODE_SYNTAX_GENERIC;
    }

    /* Choose the correct array */
    bool is_warning = (level == ERROR_LEVEL_WARNING);
    ErrorEntry** array = is_warning ? &em.warning_entries : &em.error_entries;
    uint32_t* count   = is_warning ? &em.warning_count   : &em.error_count;
    uint32_t* cap     = is_warning ? &em.warning_capacity : &em.error_capacity;

    /* Ensure capacity */
    if (!ensure_capacity(array, cap, *count)) {
        return;   /* allocation failure – skip storing this error */
    }

    /* Initialize new entry */
    ErrorEntry* entry = &(*array)[*count];
    memset(entry, 0, sizeof(ErrorEntry));

    /* Copy message */
    if (message) {
        entry->message = duplicate_string(message);
    }

    /* Copy current filename */
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

    /* Source line handling */
    if (line > 0 && em.source_lines != NULL && line <= em.source_line_count) {
        const char* src_line = em.source_lines[line - 1]; /* 1‑based to 0‑based */
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

static void report_error_va(ErrorLevel level, uint16_t error_code,
                            uint16_t line, uint8_t column, uint8_t length,
                            const char* context, const char* format, va_list args) {
    char buffer[ERROR_MESSAGE_BUFFER_SIZE];
    int written = vsnprintf(buffer, sizeof(buffer), format, args);

    if (written < 0) {
        snprintf(buffer, sizeof(buffer), "Error message formatting failed");
    } else if ((size_t)written >= sizeof(buffer)) {
        buffer[sizeof(buffer) - 1] = '\0'; /* ensure termination */
    }

    add_error_entry(level, error_code, line, column, length, context, buffer);
}

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

static uint16_t visual_line_length(const char* line) {
    if (!line) return 0;
    uint16_t vis = 0;
    for (const char* p = line; *p; p++) {
        if (*p == '\t')
            vis = (vis / TAB_SIZE + 1) * TAB_SIZE;
        else
            vis++;
    }
    return vis;
}

static void print_error_source_line(const ErrorEntry* entry, bool is_warning) {
    if (!entry) return;

    /* Obtain the source line (either copied or referenced) */
    const char* raw_line = entry->source_line_copy ?
                           entry->source_line_copy : entry->source_line_ref;
    if (!raw_line) return;

    /* Expand tabs into a local buffer */
    char expanded[EXPAND_TABS_BUFFER_SIZE];
    expand_tabs(raw_line, expanded, sizeof(expanded));
    if (expanded[0] == '\0') return;

    uint16_t line_num = entry->line;
    uint8_t col_byte = entry->column;
    uint8_t token_len = entry->length;

    /* Visual metrics on the expanded string */
    uint16_t visual_col = 0;
    uint16_t visual_len = 1;
    size_t expanded_len = strlen(expanded);

    if (col_byte > 0) {
        /* Convert byte column to visual column using original line (tabs) */
        visual_col = visual_column(raw_line, col_byte - 1);
        /* Clip token length to line bounds */
        size_t raw_len = strlen(raw_line);
        if (col_byte - 1 + token_len > raw_len)
            token_len = (uint8_t)(raw_len - (col_byte - 1));
        /* Visual length of token */
        if (token_len > 0) {
            visual_len = visual_token_length(raw_line + (col_byte - 1),
                                             token_len, visual_col);
        }
        /* Ensure marker fits in expanded line */
        if (visual_col + visual_len > expanded_len)
            visual_len = (uint16_t)(expanded_len - visual_col);
        if (visual_len == 0) visual_len = 1;
    } else {
        /* Column 0 or invalid – mark beginning */
        visual_col = 0;
        visual_len = 1;
    }

    /* Print line number and the expanded source line */
    printf("  %*u | %s\n", count_digits(line_num), line_num, expanded);
    printf("  %*s | ", count_digits(line_num), "");
    if (is_warning)
        printf("\033[33m");   /* yellow for warnings */
    else
        printf("\033[31m");   /* red for errors */

    /* Print spaces before marker */
    for (uint16_t i = 0; i < visual_col && i < expanded_len; i++)
        putchar(' ');

    /* Print caret(s) for the token */
    for (uint16_t i = 0; i < visual_len && (visual_col + i) < expanded_len; i++)
        putchar('^');

    printf("\033[0m\n");
}

static void print_error_entry(const ErrorEntry* entry, bool is_warning) {
    /* Print filename */
    printf("%s: ", entry->filename);
    
    /* Print severity with color */
    if (is_warning) {
        printf("\033[33mWARNING\033[0m");
    } else {
        if (entry->level == ERROR_LEVEL_FATAL)
            printf("\033[31mFATAL\033[0m");
        else
            printf("\033[31mERROR\033[0m");
    }

    /* Print error code, context and message */
    printf("[%04X]", entry->error_code);
    if (entry->context[0] != '\0')
        printf("(%s): ", entry->context);
    printf("%s\n", entry->message ? entry->message : "(no message)");

    /* Source context */
    if (entry->line > 0) {
        print_error_source_line(entry, is_warning);
    }
}

void errhandler__report_error_ex(ErrorLevel level, uint16_t error_code,
                                 uint16_t line, uint8_t column, uint8_t length,
                                 const char* context, const char* format, ...) {
    va_list args;
    va_start(args, format);
    report_error_va(level, error_code, line, column, length, context, format, args);
    va_end(args);
}

void errhandler__set_current_filename(const char* filename) {
    /* Free old filename */
    if (current_filename) {
        free(current_filename);
        current_filename = NULL;
    }
    /* Copy new filename if not NULL */
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

void errhandler__print_errors(void) {
    for (uint32_t i = 0; i < em.error_count; i++) {
        print_error_entry(&em.error_entries[i], false);
    }
}

void errhandler__print_warnings(void) {
    for (uint32_t i = 0; i < em.warning_count; i++) {
        print_error_entry(&em.warning_entries[i], true);
    }
}

bool errhandler__has_errors(void) {
    return em.error_count > 0;
}

bool errhandler__has_warnings(void) {
    return em.warning_count > 0;
}

void errhandler__free_error_manager(void) {
    /* Free error entries */
    for (uint32_t i = 0; i < em.error_count; i++) {
        free(em.error_entries[i].message);
        free(em.error_entries[i].filename);
        free(em.error_entries[i].source_line_copy);
    }
    free(em.error_entries);
    em.error_entries = NULL;
    em.error_count = 0;
    em.error_capacity = 0;

    /* Free warning entries */
    for (uint32_t i = 0; i < em.warning_count; i++) {
        free(em.warning_entries[i].message);
        free(em.warning_entries[i].filename);
        free(em.warning_entries[i].source_line_copy);
    }
    free(em.warning_entries);
    em.warning_entries = NULL;
    em.warning_count = 0;
    em.warning_capacity = 0;

    /* Clear source references */
    em.source_lines = NULL;
    em.source_line_count = 0;
    em.copy_source_lines = true;   /* reset to default */

    /* Free current filename */
    if (current_filename) {
        free(current_filename);
        current_filename = NULL;
    }
}

uint16_t errhandler__get_error_count(void) {
    /* Public API returns uint16_t, but internal count is uint32_t.
       If the count exceeds 65535 we cap – extremely unlikely. */
    return (em.error_count > UINT16_MAX) ? UINT16_MAX : (uint16_t)em.error_count;
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

    /* Validate hex digits */
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
