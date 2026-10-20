#include "errhandler.h"
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/*
 * Buffer sizes and growth parameters.
 */
#define ERROR_MESSAGE_BUFFER_SIZE       1024
#define CONTEXT_BUFFER_SIZE             16
#define INITIAL_CAPACITY                8
#define MAX_EXPONENTIAL_CAPACITY        1024
#define LINEAR_INCREMENT                256
#define TAB_SIZE                        8
#define EXPAND_TABS_BUFFER_SIZE         2048

/*
 * ANSI colour codes (optional).
 */
#define ANSI_RED    "\033[91m"
#define ANSI_YELLOW "\033[93m"
#define ANSI_BLUE   "\033[94m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_RESET  "\033[0m"

/*
 * One error/warning/note entry.
 */
typedef struct {
    char*   message;              /* formatted error message (dynamically allocated) */
    char*   filename;             /* file name (copied) */
    char*   source_line_copy;     /* copy of the source line at reporting time (if any) */
    uint32_t line;                /* 1‑based line number */
    uint32_t column;              /* 1‑based byte column of the token start */
    uint32_t length;              /* byte length of the token */
    ErrorLevel level;             /* severity */
    char     context[CONTEXT_BUFFER_SIZE]; /* compilation stage (e.g. "semantic") */
} ErrorEntry;

/*
 * Global state.
 */
typedef struct {
    ErrorEntry* error_entries;    /* array of errors (and fatal) */
    ErrorEntry* warning_entries;  /* array of warnings and notes */
    uint32_t    error_count;
    uint32_t    warning_count;
    uint32_t    error_capacity;
    uint32_t    warning_capacity;
    const char** source_lines;    /* external source lines (if set) */
    uint32_t    source_line_count;
    bool        owns_source_lines; /* whether we allocated source_lines ourselves */
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

static char* current_filename = NULL;  /* current source file name (copied) */

/*
 * Forward declarations of static helpers.
 */
static bool ensure_capacity(ErrorEntry** array, uint32_t* capacity,
                            uint32_t count);
static void add_error_entry(ErrorLevel level, uint32_t line, uint32_t column,
                            uint32_t length, const char* context,
                            const char* message);
static char* duplicate_string(const char* str);
static uint16_t count_digits(uint32_t number);
static uint32_t visual_column(const char* line, uint32_t byte_col);
static uint32_t visual_token_length(const char* segment, uint32_t byte_len,
                                    uint32_t start_visual_col);
static void expand_tabs(const char* src, char* dst, size_t dst_size);
static bool use_colours(void);
static const char* level_string_lower(ErrorLevel level);
static int severity_rank(ErrorLevel level);
static bool same_location(const ErrorEntry* a, const ErrorEntry* b);
static bool has_source_line(const ErrorEntry* entry);
static void print_source_line(const ErrorEntry* entry);
static void print_summary(const ErrorEntry* primary, bool has_body);
static void print_indented_message(const ErrorEntry* entry);
static void print_grouped_entries(ErrorEntry** entries, size_t count);
static int compare_entries(const void* a, const void* b);

/*
 * Count the number of decimal digits in a positive integer.
 */
static uint16_t count_digits(uint32_t number) {
    if (number == 0) return 1;
    uint16_t digits = 0;
    while (number) {
        digits++;
        number /= 10;
    }
    return digits;
}

/*
 * Duplicate a string using malloc.
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
 * Ensure that the dynamic array has enough capacity for at least 'count' elements.
 * Uses exponential growth up to a threshold, then linear increments.
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
        fwrite("ERROR: error entries capacity overflow\n", 1, 38, stderr);
        return false;
    }

    ErrorEntry* new_array = (ErrorEntry*)realloc(*array, new_cap * sizeof(ErrorEntry));
    if (!new_array) {
        fwrite("ERROR: failed to allocate memory for error entries\n", 1, 50, stderr);
        return false;
    }

    *array = new_array;
    *capacity = new_cap;
    return true;
}

/*
 * Add an error entry to the appropriate array (errors or warnings/notes).
 * The message is already formatted.
 */
static void add_error_entry(ErrorLevel level, uint32_t line, uint32_t column,
                            uint32_t length, const char* context,
                            const char* message) {
    bool is_warning = (level == ERROR_LEVEL_WARNING);
    bool is_note = (level == ERROR_LEVEL_NOTE);
    ErrorEntry** array = (is_warning || is_note) ? &em.warning_entries : &em.error_entries;
    uint32_t* count    = (is_warning || is_note) ? &em.warning_count : &em.error_count;
    uint32_t* cap      = (is_warning || is_note) ? &em.warning_capacity : &em.error_capacity;

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

    if (context) {
        strncpy(entry->context, context, CONTEXT_BUFFER_SIZE - 1);
        entry->context[CONTEXT_BUFFER_SIZE - 1] = '\0';
    } else {
        entry->context[0] = '\0';
    }

    /* Store a copy of the source line if available. */
    if (line > 0 && em.source_lines != NULL && line <= em.source_line_count) {
        const char* src_line = em.source_lines[line - 1];
        if (src_line) {
            entry->source_line_copy = duplicate_string(src_line);
        }
    }

    (*count)++;
}

/*
 * Common implementation for reporting an error with a va_list.
 */
static void report_error_va(ErrorLevel level, uint32_t line, uint32_t column,
                            uint32_t length, const char* context,
                            const char* format, va_list args) {
    char buffer[ERROR_MESSAGE_BUFFER_SIZE];
    int written = vsnprintf(buffer, sizeof(buffer), format, args);

    if (written < 0) {
        snprintf(buffer, sizeof(buffer), "Error message formatting failed");
    } else if ((size_t)written >= sizeof(buffer)) {
        buffer[sizeof(buffer) - 1] = '\0';
    }

    add_error_entry(level, line, column, length, context, buffer);
}

/*
 * Public API: report error with variable arguments.
 */
void errhandler__report_error_ex(ErrorLevel level, uint32_t line,
                                 uint32_t column, uint32_t length,
                                 const char* context, const char* format, ...) {
    va_list args;
    va_start(args, format);
    report_error_va(level, line, column, length, context, format, args);
    va_end(args);
}

/*
 * Public API: report error with va_list.
 */
void errhandler__report_error_ex_va(ErrorLevel level, uint32_t line,
                                    uint32_t column, uint32_t length,
                                    const char* context, const char* format,
                                    va_list args) {
    report_error_va(level, line, column, length, context, format, args);
}

/*
 * Set the current filename (copied).
 */
void errhandler__set_current_filename(const char* filename) {
    if (current_filename) {
        free(current_filename);
        current_filename = NULL;
    }
    if (filename) {
        current_filename = duplicate_string(filename);
    }
}

/*
 * Provide an external array of source lines (not copied).
 */
void errhandler__set_source_code(const char** source_lines,
                                 uint32_t line_count) {
    em.source_lines = source_lines;
    em.source_line_count = line_count;
    em.owns_source_lines = false;
}

/*
 * Clear any source lines owned by the manager.
 */
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

/*
 * Load a source file from disk and store its lines internally.
 */
int errhandler__load_source_file(const char* filename) {
    if (!filename) return -1;

    FILE* f = fopen(filename, "r");
    if (!f) return -1;

    /* Free any previously owned source. */
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
    uint32_t line_count = 0;
    char buf[4096];

    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';

        char* line = duplicate_string(buf);
        if (!line) {
            for (uint32_t i = 0; i < line_count; i++) free(lines[i]);
            free(lines);
            fclose(f);
            return -1;
        }

        char** new_lines = realloc(lines, (line_count + 1) * sizeof(char*));
        if (!new_lines) {
            free(line);
            for (uint32_t i = 0; i < line_count; i++) free(lines[i]);
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
    em.owns_source_lines = true;
    return 0;
}

/*
 * Expand tabs in a source line to spaces (for display).
 * The output buffer must be large enough.
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
 * Compute the visual column (0‑based) of a byte offset in a line,
 * expanding tabs.
 */
static uint32_t visual_column(const char* line, uint32_t byte_col) {
    if (!line) return 0;
    uint32_t vis = 0;
    for (uint32_t i = 0; i < byte_col && line[i]; i++) {
        if (line[i] == '\t')
            vis = (vis / TAB_SIZE + 1) * TAB_SIZE;
        else
            vis++;
    }
    return vis;
}

/*
 * Compute the visual length of a token (starting at 'segment') of given byte length,
 * given the visual column where the segment starts.
 */
static uint32_t visual_token_length(const char* segment, uint32_t byte_len,
                                    uint32_t start_visual_col) {
    if (!segment) return 1;
    uint32_t vis_len = 0;
    uint32_t cur_col = start_visual_col;
    for (uint32_t i = 0; i < byte_len && segment[i]; i++) {
        if (segment[i] == '\t') {
            uint32_t spaces = TAB_SIZE - (cur_col % TAB_SIZE);
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

/* ---------- new printing helpers ---------- */

/* Determine if ANSI colours should be used. */
static bool use_colours(void) {
    static int cached = -1;
    if (cached == -1) {
        cached = isatty(STDOUT_FILENO) ? 1 : 0;   /* STDOUT_FILENO avoids fileno() */
    }
    return cached;
}

/* Get lowercase level name. */
static const char* level_string_lower(ErrorLevel level) {
    switch (level) {
        case ERROR_LEVEL_NOTE:    return "note";
        case ERROR_LEVEL_WARNING: return "warning";
        case ERROR_LEVEL_ERROR:   return "error";
        case ERROR_LEVEL_FATAL:   return "fatal";
        default:                  return "unknown";
    }
}

/* Severity rank for primary selection (higher = more severe). */
static int severity_rank(ErrorLevel level) {
    switch (level) {
        case ERROR_LEVEL_FATAL:   return 3;
        case ERROR_LEVEL_ERROR:   return 2;
        case ERROR_LEVEL_WARNING: return 1;
        case ERROR_LEVEL_NOTE:    return 0;
        default:                  return 0;
    }
}

/* Compare two entries by (filename, line, column) for sorting. */
static int compare_entries(const void* a, const void* b) {
    const ErrorEntry* ea = *(const ErrorEntry**)a;
    const ErrorEntry* eb = *(const ErrorEntry**)b;
    const char* fa = ea->filename ? ea->filename : "";
    const char* fb = eb->filename ? eb->filename : "";
    int cmp = strcmp(fa, fb);
    if (cmp != 0) return cmp;
    if (ea->line != eb->line) return (ea->line > eb->line) - (ea->line < eb->line);
    return (ea->column > eb->column) - (ea->column < eb->column);
}

/* Test if two entries share the same location (filename, line, column). */
static bool same_location(const ErrorEntry* a, const ErrorEntry* b) {
    const char* fa = a->filename ? a->filename : "";
    const char* fb = b->filename ? b->filename : "";
    if (strcmp(fa, fb) != 0) return false;
    return (a->line == b->line && a->column == b->column);
}

/* Check if an entry has a source line available (from external source or stored copy). */
static bool has_source_line(const ErrorEntry* entry) {
    if (entry->source_line_copy) return true;
    if (em.source_lines && entry->line > 0 && entry->line <= em.source_line_count) {
        const char* line = em.source_lines[entry->line - 1];
        if (line) return true;
    }
    return false;
}

/* Print a single source line with caret underline (no extra context lines). */
static void print_source_line(const ErrorEntry* entry) {
    if (!entry) return;
    uint32_t line = entry->line;
    if (line == 0) return;

    const char* raw_line = NULL;
    if (em.source_lines && line <= em.source_line_count) {
        raw_line = em.source_lines[line - 1];
    }
    if (!raw_line) {
        raw_line = entry->source_line_copy;
    }
    if (!raw_line) return;

    int width = count_digits(em.source_line_count > 0 ? em.source_line_count : line);
    char expanded[EXPAND_TABS_BUFFER_SIZE];
    expand_tabs(raw_line, expanded, sizeof(expanded));
    printf("%*u | %s\n", width, line, expanded);

    uint32_t byte_col = entry->column - 1;
    uint32_t vis_col = visual_column(raw_line, byte_col);
    uint32_t vis_len = visual_token_length(raw_line + byte_col, entry->length, vis_col);
    printf("%*s | ", width, "");
    for (uint32_t j = 0; j < vis_col; j++) putchar(' ');
    for (uint32_t j = 0; j < vis_len; j++) putchar('^');
    putchar('\n');
}

/* Print the summary line for a group (uses the primary entry). */
static void print_summary(const ErrorEntry* primary, bool has_body) {
    /* Filename */
    if (primary->filename) {
        printf("%s ", primary->filename);
    }

    /* Line[:column] */
    if (primary->line > 0) {
        if (use_colours()) printf(ANSI_BOLD);
        printf("%u", primary->line);
        if (primary->column > 0) {
            printf(":%u", primary->column);
        }
        if (use_colours()) printf(ANSI_RESET);
        printf(":");
    }

    /* Level (coloured) */
    const char* level_str = level_string_lower(primary->level);
    if (use_colours()) {
        const char* colour = "";
        switch (primary->level) {
            case ERROR_LEVEL_FATAL:
            case ERROR_LEVEL_ERROR: colour = ANSI_RED; break;
            case ERROR_LEVEL_WARNING: colour = ANSI_YELLOW; break;
            case ERROR_LEVEL_NOTE: colour = ANSI_BLUE; break;
            default: colour = ANSI_RESET; break;
        }
        printf("%s%s%s", colour, level_str, ANSI_RESET);
    } else {
        printf("%s", level_str);
    }

    /* Context (if non-empty) */
    if (primary->context[0] != '\0') {
        printf("(%s)", primary->context);
    }
    printf(":");

    /* Message (if non-empty) */
    if (primary->message && primary->message[0] != '\0') {
        printf(" %s", primary->message);
    }

    /* Final punctuation: colon if body follows, else dot */
    printf("%c\n", has_body ? ':' : '.');
}

/* Print an indented secondary message (no location, no final punctuation). */
static void print_indented_message(const ErrorEntry* entry) {
    printf("  ");
    const char* level_str = level_string_lower(entry->level);
    if (use_colours()) {
        const char* colour = "";
        switch (entry->level) {
            case ERROR_LEVEL_FATAL:
            case ERROR_LEVEL_ERROR: colour = ANSI_RED; break;
            case ERROR_LEVEL_WARNING: colour = ANSI_YELLOW; break;
            case ERROR_LEVEL_NOTE: colour = ANSI_BLUE; break;
            default: colour = ANSI_RESET; break;
        }
        printf("%s%s%s", colour, level_str, ANSI_RESET);
    } else {
        printf("%s", level_str);
    }
    if (entry->context[0] != '\0') {
        printf("(%s)", entry->context);
    }
    printf(":");
    if (entry->message && entry->message[0] != '\0') {
        printf(" %s", entry->message);
    }
    printf("\n");
}

/* Print a group of entries (already sorted and grouped by location). */
static void print_grouped_entries(ErrorEntry** entries, size_t count) {
    if (count == 0) return;
    qsort(entries, count, sizeof(ErrorEntry*), compare_entries);

    size_t i = 0;
    while (i < count) {
        size_t j = i + 1;
        while (j < count && same_location(entries[i], entries[j])) j++;
        size_t group_count = j - i;
        ErrorEntry** group = entries + i;

        /* Find primary entry (most severe). */
        ErrorEntry* primary = group[0];
        int max_rank = severity_rank(primary->level);
        for (size_t k = 1; k < group_count; k++) {
            int r = severity_rank(group[k]->level);
            if (r > max_rank) {
                max_rank = r;
                primary = group[k];
            }
        }

        /* Check if any entry in the group has a source line. */
        bool has_body = false;
        for (size_t k = 0; k < group_count; k++) {
            if (has_source_line(group[k])) { has_body = true; break; }
        }

        /* Print the summary line. */
        print_summary(primary, has_body);

        /* Print other messages indented. */
        for (size_t k = 0; k < group_count; k++) {
            if (group[k] == primary) continue;
            print_indented_message(group[k]);
        }

        /* Print source body if available. */
        if (has_body) {
            ErrorEntry* src_entry = primary;
            if (!has_source_line(src_entry)) {
                for (size_t k = 0; k < group_count; k++) {
                    if (has_source_line(group[k])) {
                        src_entry = group[k];
                        break;
                    }
                }
            }
            print_source_line(src_entry);
        }

        /* Blank line between groups. */
        putchar('\n');

        i = j;
    }
}

/* ---------- public printing functions ---------- */

void errhandler__print_errors(void) {
    size_t total = em.error_count + (em.warnings_as_errors ? em.warning_count : 0);
    if (total == 0) return;

    ErrorEntry** list = (ErrorEntry**)malloc(total * sizeof(ErrorEntry*));
    if (!list) return;

    size_t idx = 0;
    for (uint32_t i = 0; i < em.error_count; i++) {
        list[idx++] = &em.error_entries[i];
    }
    if (em.warnings_as_errors) {
        for (uint32_t i = 0; i < em.warning_count; i++) {
            list[idx++] = &em.warning_entries[i];
        }
    }
    print_grouped_entries(list, total);
    free(list);
}

void errhandler__print_warnings(void) {
    if (em.suppress_warnings || em.warnings_as_errors) return;
    if (em.warning_count == 0) return;

    ErrorEntry** list = (ErrorEntry**)malloc(em.warning_count * sizeof(ErrorEntry*));
    if (!list) return;

    for (uint32_t i = 0; i < em.warning_count; i++) {
        list[i] = &em.warning_entries[i];
    }
    print_grouped_entries(list, em.warning_count);
    free(list);
}

/* ---------- remaining public functions (unchanged) ---------- */

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
        case ERROR_LEVEL_NOTE:    return "NOTE";
        case ERROR_LEVEL_WARNING: return "WARNING";
        case ERROR_LEVEL_ERROR:   return "ERROR";
        case ERROR_LEVEL_FATAL:   return "FATAL";
        default:                  return "UNKNOWN";
    }
}

void errhandler__set_warnings_as_errors(bool enable) {
    em.warnings_as_errors = enable;
}

void errhandler__set_suppress_warnings(bool suppress) {
    em.suppress_warnings = suppress;
}
