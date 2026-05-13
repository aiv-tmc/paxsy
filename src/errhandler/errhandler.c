#include "errhandler.h"
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#define ERROR_MESSAGE_BUFFER_SIZE       1024    /* max length of a formatted message */
#define CONTEXT_BUFFER_SIZE             8       /* max length of a context tag */
#define INITIAL_CAPACITY                8       /* starting number of slots per array */
#define MAX_EXPONENTIAL_CAPACITY        1024    /* threshold for switching to linear growth */
#define LINEAR_INCREMENT                256     /* slots added each time after exponential phase */
#define TAB_SIZE                        8       /* spaces per tab stop */
#define EXPAND_TABS_BUFFER_SIZE         2048    /* local buffer for tab expansion */

/* ANSI terminal escape codes for colouring output */
#define ANSI_RED    "\033[31m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_RESET  "\033[0m"

/*
 * A single diagnostic entry.
 */
typedef struct {
    char*   message;          /* owned formatted human‑readable text */
    char*   filename;         /* owned copy of the source filename */
    uint16_t line;            /* 1‑based line number */
    uint8_t  column;          /* 1‑based column (byte offset) */
    uint8_t  length;          /* length of the underlined token in bytes */
    ErrorLevel level;         /* severity */
    char     context[CONTEXT_BUFFER_SIZE]; /* short subsystem tag */
    uint16_t error_code;      /* 4‑hex‑digit error code */
    char*   source_line_copy; /* owned copy of the whole source line for
                                 caret display */
} ErrorEntry;

/*
 * Global state – there is exactly one instance, accessed only through the
 * functions in this file.
 */
typedef struct {
    ErrorEntry* error_entries;       /* dynamic array of ERROR/FATAL */
    ErrorEntry* warning_entries;     /* dynamic array of WARNING */
    uint32_t    error_count;         /* number of used error entries */
    uint32_t    warning_count;
    uint32_t    error_capacity;      /* allocated size of error_entries */
    uint32_t    warning_capacity;
    const char** source_lines;       /* pointer to source lines array */
    uint32_t    source_line_count;
    bool        owns_source_lines;   /* if true, source_lines was allocated
                                        by us and must be freed */
    bool        warnings_as_errors;
    bool        suppress_warnings;
} ErrorManager;

static ErrorManager em = {
    .error_entries    = NULL,
    .warning_entries  = NULL,
    .error_count      = 0,
    .warning_count    = 0,
    .error_capacity   = 0,
    .warning_capacity = 0,
    .source_lines     = NULL,
    .source_line_count = 0,
    .owns_source_lines = false,
    .warnings_as_errors = false,
    .suppress_warnings  = false
};

/* Current source filename, copied when set. */
static char* current_filename = NULL;

static bool ensure_capacity(ErrorEntry** array, uint32_t* capacity,
                            uint32_t count);
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
 * Duplicate a null‑terminated string.  Returns NULL if str is NULL or if
 * allocation fails.
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
 * Return the number of decimal digits required to represent a uint16_t.
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
 * Reject the invalid zero error code (reserved).
 */
static bool validate_error_code(uint16_t error_code) {
    return error_code != 0;
}

/*
 * Ensures that *array has room for at least (count+1) elements.
 *
 * Growth policy:
 *   - when capacity == 0                  -> INITIAL_CAPACITY
 *   - when capacity < MAX_EXPONENTIAL_CAPACITY  -> double capacity
 *   - otherwise                           -> add LINEAR_INCREMENT
 *
 * Returns true on success, false on memory allocation failure.
 * On failure *array and *capacity are left untouched.
 */
static bool ensure_capacity(ErrorEntry** array, uint32_t* capacity,
                            uint32_t count) {
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
        /* overflow guard */
        fprintf(stderr, ANSI_RED "ERROR" ANSI_RESET
                ": error entries capacity overflow\n");
        return false;
    }

    ErrorEntry* new_array = (ErrorEntry*)realloc(*array, new_cap * sizeof(ErrorEntry));
    if (!new_array) {
        fprintf(stderr, ANSI_RED "ERROR" ANSI_RESET
                ": failed to allocate memory for error entries\n");
        return false;
    }

    *array = new_array;
    *capacity = new_cap;
    return true;
}

/*
 * Append a diagnostic entry to the appropriate array.
 *
 * The message and current filename are always duplicated if provided.
 * If source lines are available, the offending line is also duplicated
 * and stored inside the entry, guaranteeing that the entry can be printed
 * at any later time regardless of what happens to the external source array.
 */
static void add_error_entry(ErrorLevel level, uint16_t error_code,
                            uint16_t line, uint8_t column, uint8_t length,
                            const char* context, const char* message) {
    if (!validate_error_code(error_code)) {
        fprintf(stderr, ANSI_YELLOW "WARNING" ANSI_RESET
                ": invalid error code 0x%04X, replaced by generic syntax code\n",
                error_code);
        error_code = ERROR_CODE_SYNTAX_GENERIC;
    }

    bool is_warning = (level == ERROR_LEVEL_WARNING);
    ErrorEntry** array = is_warning ? &em.warning_entries : &em.error_entries;
    uint32_t* count    = is_warning ? &em.warning_count    : &em.error_count;
    uint32_t* cap      = is_warning ? &em.warning_capacity : &em.error_capacity;

    if (!ensure_capacity(array, cap, *count)) {
        return; /* allocation failure – entry is lost, but we survive */
    }

    ErrorEntry* entry = &(*array)[*count];
    memset(entry, 0, sizeof(ErrorEntry));

    /* copy message and filename */
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

    /* always store a copy of the affected source line if available */
    if (line > 0 && em.source_lines != NULL && line <= em.source_line_count) {
        const char* src_line = em.source_lines[line - 1]; /* 0‑based */
        if (src_line) {
            entry->source_line_copy = duplicate_string(src_line);
        }
    }

    (*count)++;
}

/*
 * Format the user message using a va_list and delegate to add_error_entry.
 */
static void report_error_va(ErrorLevel level, uint16_t error_code,
                            uint16_t line, uint8_t column, uint8_t length,
                            const char* context, const char* format,
                            va_list args) {
    char buffer[ERROR_MESSAGE_BUFFER_SIZE];
    int written = vsnprintf(buffer, sizeof(buffer), format, args);

    if (written < 0) {
        snprintf(buffer, sizeof(buffer), "Error message formatting failed");
    } else if ((size_t)written >= sizeof(buffer)) {
        buffer[sizeof(buffer) - 1] = '\0'; /* ensure proper termination */
    }

    add_error_entry(level, error_code, line, column, length, context, buffer);
}

/*
 * Expand tab characters in src to spaces (assuming a fixed tab width)
 * and write the result into dst, which has dst_size bytes.
 * Stops when dst is full or src is exhausted.
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
 * Compute the visual column (monospaced, after expanding tabs) that
 * corresponds to a 0‑based byte offset 'byte_col' in string 'line'.
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
 * Return the visual length (after expanding tabs) of the token that starts
 * at byte offset segment and spans byte_len bytes.  start_visual_col must be
 * the visual column of the beginning of the token.
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
 * Print the source line referenced by entry with a caret (^^^^) under
 * the offending token.  The line is printed as‑is (tabs are NOT expanded),
 * but the caret column and width are computed considering tab expansion
 * so that the caret aligns with the visual position.
 */
static void print_error_source_line(const ErrorEntry* entry, bool is_warning) {
    if (!entry) return;

    const char* raw_line = entry->source_line_copy;
    if (!raw_line || raw_line[0] == '\0') return;

    uint16_t line_num = entry->line;
    uint8_t  col_byte = entry->column;  /* 1‑based */
    uint8_t  token_len = entry->length;

    /* total visual width of the raw line */
    uint16_t total_visual_len = visual_column(raw_line,
                                              (uint16_t)strlen(raw_line));

    uint16_t visual_col = 0;
    uint16_t visual_len = 1;

    if (col_byte > 0) {
        visual_col = visual_column(raw_line, col_byte - 1);

        size_t raw_len = strlen(raw_line);
        /* clamp token length to what remains */
        if (col_byte - 1 + token_len > raw_len)
            token_len = (uint8_t)(raw_len - (col_byte - 1));

        if (token_len > 0) {
            visual_len = visual_token_length(raw_line + (col_byte - 1),
                                             token_len, visual_col);
        }

        /* safety clamping */
        if (visual_col >= total_visual_len) visual_col = total_visual_len;
        if (visual_col + visual_len > total_visual_len)
            visual_len = (uint16_t)(total_visual_len - visual_col);
        if (visual_len == 0) visual_len = 1;
    } else {
        visual_col = 0;
        visual_len = 1;
    }

    /* print line number gutter */
    int gutter_width = count_digits(line_num);
    printf("  %*u | %s\n", gutter_width, line_num, raw_line);
    printf("  %*s | ", gutter_width, "");

    /* colour */
    if (is_warning)
        printf(ANSI_YELLOW);
    else
        printf(ANSI_RED);

    /* print spaces then carets */
    for (uint16_t i = 0; i < visual_col && i < total_visual_len; i++)
        putchar(' ');
    for (uint16_t i = 0; i < visual_len && (visual_col + i) < total_visual_len; i++)
        putchar('^');

    printf(ANSI_RESET "\n");
}

/*
 * Print a complete error entry: filename, severity tag, error code, context,
 * message, and (if applicable) the source line with caret.
 */
static void print_error_entry(const ErrorEntry* entry, bool is_warning) {
    printf("%s: ", entry->filename ? entry->filename : "(unknown)");

    if (is_warning) {
        printf(ANSI_YELLOW "WARNING" ANSI_RESET);
    } else {
        if (entry->level == ERROR_LEVEL_FATAL)
            printf(ANSI_RED "FATAL" ANSI_RESET);
        else
            printf(ANSI_RED "ERROR" ANSI_RESET);
    }

    printf("[%04X]", entry->error_code);
    if (entry->context[0] != '\0')
        printf("(%s): ", entry->context);
    printf("%s\n", entry->message ? entry->message : "(no message)");

    if (entry->line > 0) {
        print_error_source_line(entry, is_warning);
    }
}

void errhandler__report_error_ex(ErrorLevel level, uint16_t error_code,
                                 uint16_t line, uint8_t column, uint8_t length,
                                 const char* context, const char* format, ...) {
    va_list args;
    va_start(args, format);
    report_error_va(level, error_code, line, column, length, context,
                    format, args);
    va_end(args);
}

void errhandler__report_error_ex_va(ErrorLevel level, uint16_t error_code,
                                    uint16_t line, uint8_t column, uint8_t length,
                                    const char* context, const char* format,
                                    va_list args) {
    report_error_va(level, error_code, line, column, length, context,
                    format, args);
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

void errhandler__set_source_code(const char** source_lines,
                                 uint16_t line_count) {
    /*
     * We do NOT take ownership; the caller retains responsibility for
     * the memory.  Individual error entries already copy the lines they
     * need, so this pointer is only used at the moment of reporting.
     */
    em.source_lines = source_lines;
    em.source_line_count = line_count;
    em.owns_source_lines = false;
}

void errhandler__clear_source_code(void) {
    if (em.owns_source_lines && em.source_lines) {
        for (uint32_t i = 0; i < em.source_line_count; i++) {
            free((void*)em.source_lines[i]);
        }
        free((void*)em.source_lines);
    }
    em.source_lines = NULL;
    em.source_line_count = 0;
    em.owns_source_lines = false;
}

int errhandler__load_source_file(const char* filename) {
    if (!filename) return -1;

    FILE* f = fopen(filename, "r");
    if (!f) return -1;

    /* free any previously owned source lines */
    if (em.owns_source_lines && em.source_lines) {
        for (uint32_t i = 0; i < em.source_line_count; i++) {
            free((void*)em.source_lines[i]);
        }
        free((void*)em.source_lines);
        em.source_lines = NULL;
        em.source_line_count = 0;
        em.owns_source_lines = false;
    }

    char** lines = NULL;
    uint16_t line_count = 0;
    char buf[4096];

    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';

        char* line = duplicate_string(buf);
        if (!line) {
            /* cleanup */
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
    em.owns_source_lines = true;   /* we own every line and the array */
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
    if (em.warnings_as_errors) return;   /* already shown as errors */
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
    /* free each error entry's owned strings */
    for (uint32_t i = 0; i < em.error_count; i++) {
        free(em.error_entries[i].message);
        free(em.error_entries[i].filename);
        free(em.error_entries[i].source_line_copy);
    }
    free(em.error_entries);
    em.error_entries = NULL;
    em.error_count = 0;
    em.error_capacity = 0;

    /* free each warning entry's owned strings */
    for (uint32_t i = 0; i < em.warning_count; i++) {
        free(em.warning_entries[i].message);
        free(em.warning_entries[i].filename);
        free(em.warning_entries[i].source_line_copy);
    }
    free(em.warning_entries);
    em.warning_entries = NULL;
    em.warning_count = 0;
    em.warning_capacity = 0;

    /* free the global source lines if we own them */
    if (em.owns_source_lines && em.source_lines) {
        for (uint32_t i = 0; i < em.source_line_count; i++) {
            free((void*)em.source_lines[i]);
        }
        free((void*)em.source_lines);
    }
    em.source_lines = NULL;
    em.source_line_count = 0;
    em.owns_source_lines = false;

    free(current_filename);
    current_filename = NULL;
}

uint16_t errhandler__get_error_count(void) {
    uint32_t count = em.error_count;
    if (em.warnings_as_errors) count += em.warning_count;
    return (count > UINT16_MAX) ? UINT16_MAX : (uint16_t)count;
}

uint16_t errhandler__get_warning_count(void) {
    return (em.warning_count > UINT16_MAX) ? UINT16_MAX
                                            : (uint16_t)em.warning_count;
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
