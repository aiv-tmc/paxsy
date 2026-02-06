#include "errhandler.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

/* Configuration constants */
#define ERROR_ID_BUFFER_SIZE 9          /* 8 hex digits + null terminator */
#define ERROR_MESSAGE_BUFFER_SIZE 1024  /* Max length of formatted error message */
#define CONTEXT_BUFFER_SIZE 8           /* Max length of context identifier */
#define INITIAL_CAPACITY 8              /* Initial capacity of error entries array */
#define MAX_CAPACITY_BEFORE_INCREMENT 1024  /* Switch to linear growth after this */
#define CAPACITY_INCREMENT 256          /* Linear growth increment */
#define TAB_SIZE 8                      /* Number of spaces per tab character */
#define MAX_LINE_DIGITS 4               /* Maximum digits for line number */
#define MAX_COL_DIGITS 3                /* Maximum digits for column number */

/* Structure definitions */

/**
 * @brief Error entry structure storing comprehensive error information
 * 
 * Each error entry contains all information needed to display a detailed
 * error message including source context, location, and timing.
 */
typedef struct {
    char* message;          /* Formatted error message string (dynamically allocated) */
    uint16_t line;          /* Line number where error occurred (1-based) */
    uint8_t column;         /* Column number where error occurred (1-based, byte offset) */
    uint8_t length;         /* Length of problematic token/segment (in characters) */
    ErrorLevel level;       /* Severity level of the error */
    char context[CONTEXT_BUFFER_SIZE];  /* Context identifier */
    uint32_t timestamp;     /* Relative timestamp in seconds since first error */
    char error_code[ERROR_CODE_SIZE];  /* 6-character error code + null terminator */
    char* source_line;      /* Copy of source line where error occurred */
} ErrorEntry;

/**
 * @brief Error manager state structure
 * 
 * Global state container for the error management system.
 * Maintains all error entries, counters, and source code references.
 */
typedef struct {
    ErrorEntry* entries;            /* Dynamic array of error entries */
    uint16_t count;                 /* Current number of error entries */
    uint16_t capacity;              /* Capacity of the entries array */
    uint16_t error_count;           /* Total count of errors (excluding warnings) */
    uint16_t warning_count;         /* Total count of warnings */
    const char** source_lines;      /* Source code lines for contextual display */
    uint16_t source_line_count;     /* Number of source code lines available */
    time_t start_time;              /* Program start time for relative timestamps */
    uint32_t error_id_counter;      /* Counter for generating unique error IDs */
} ErrorManager;

/* Global state - single instance of error manager */

/**
 * @brief Global error manager instance
 * 
 * Static global variable holding all error management state.
 * Initialized to zero at program start.
 */
static ErrorManager error_manager = { 
    .entries = NULL,
    .count = 0,
    .capacity = 0,
    .error_count = 0,
    .warning_count = 0,
    .source_lines = NULL,
    .source_line_count = 0,
    .start_time = 0,
    .error_id_counter = 0
};

/* Private helper functions forward declarations */
static uint32_t get_timestamp(void);
static uint32_t generate_error_id(void);
static bool ensure_capacity(void);
static void add_error_entry(uint16_t line, uint8_t column, uint8_t length,
                           ErrorLevel level, const char* context, 
                           const char* error_code, const char* message);
static void report_error_va(ErrorLevel level, const char* error_code,
                           uint16_t line, uint8_t column, uint8_t length,
                           const char* context, const char* format, va_list args);
static uint16_t calculate_visual_column(const char* line, uint16_t target_column);
static uint16_t calculate_visual_length(const char* segment, uint8_t segment_len, 
                                       uint16_t start_visual_col);
static uint16_t calculate_visual_line_length(const char* line);
static void print_error_source_line(const char* source_line, uint16_t line, 
                                   uint8_t column, uint8_t length, 
                                   bool is_warning);
static void print_error_entry(const ErrorEntry* entry, bool is_warning);
static bool validate_error_code(const char* error_code);
static int count_digits(uint16_t number);
static char* duplicate_string(const char* str);

/**
 * @brief Duplicate a string with error handling
 * 
 * @param str String to duplicate
 * @return char* Duplicated string or NULL on failure
 */
static char* duplicate_string(const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, str, len);
        dup[len] = '\0';
    }
    return dup;
}

/**
 * @brief Count digits in a number
 * 
 * @param number Number to count digits for
 * @return int Number of digits
 */
static int count_digits(uint16_t number) {
    if (number == 0) return 1;
    
    int digits = 0;
    while (number > 0) {
        number /= 10;
        digits++;
    }
    return digits;
}

/**
 * @brief Validate error code format
 * 
 * @param error_code 6-character error code to validate
 * @return true if code is valid (exactly 6 alphanumeric characters)
 * @return false if code is invalid
 */
static bool validate_error_code(const char* error_code) {
    if (!error_code) return false;
    
    /* Check length */
    size_t len = strlen(error_code);
    if (len != 4) return false;
    
    /* Check that all characters are alphanumeric */
    for (int i = 0; i < 4; i++) {
        if (!isalnum((unsigned char)error_code[i])) {
            return false;
        }
    }
    
    return true;
}

/**
 * @brief Get current timestamp in seconds
 * 
 * @return uint32_t Current Unix timestamp in seconds
 */
static uint32_t get_timestamp(void) {
    return (uint32_t)time(NULL);
}

/**
 * @brief Generate unique error ID combining timestamp and counter
 * 
 * Creates a 32-bit unique ID with:
 * - High 16 bits: timestamp base (shared by all errors in session)
 * - Low 16 bits: sequential counter
 * 
 * @return uint32_t Unique error identifier
 */
static uint32_t generate_error_id(void) {
    static uint32_t base_id = 0;
    
    /* Initialize base ID on first call (timestamp << 16) */
    if (base_id == 0) {
        base_id = (get_timestamp() & 0xFFFF) << 16;
    }
    
    error_manager.error_id_counter++;
    return base_id | (error_manager.error_id_counter & 0xFFFF);
}

/**
 * @brief Resize error entries array if needed
 * 
 * Implements exponential growth up to MAX_CAPACITY_BEFORE_INCREMENT,
 * then switches to linear growth to avoid excessive memory usage.
 * 
 * @return true if capacity ensured or no resize needed
 * @return false if memory allocation failed
 */
static bool ensure_capacity(void) {
    if (error_manager.count < error_manager.capacity) {
        return true;  /* Current capacity is sufficient */
    }
    
    uint16_t new_capacity;
    
    /* Determine new capacity based on current size */
    if (error_manager.capacity == 0) {
        new_capacity = INITIAL_CAPACITY;
    } else if (error_manager.capacity < MAX_CAPACITY_BEFORE_INCREMENT) {
        new_capacity = error_manager.capacity * 2;  /* Exponential growth */
    } else {
        new_capacity = error_manager.capacity + CAPACITY_INCREMENT;  /* Linear growth */
    }
    
    /* Check for integer overflow */
    if (new_capacity <= error_manager.capacity) {
        fprintf(stderr, "\033[31mERROR\033[0m: Error entries capacity overflow\n");
        return false;
    }
    
    /* Reallocate memory for entries array */
    ErrorEntry* new_entries = realloc(error_manager.entries, 
                                      new_capacity * sizeof(ErrorEntry));
    if (new_entries == NULL) {
        fprintf(stderr, "\033[31mERROR\033[0m: Failed to allocate memory for error entries\n");
        return false;
    }
    
    error_manager.entries = new_entries;
    error_manager.capacity = new_capacity;
    
    return true;
}

/**
 * @brief Add a new error entry with comprehensive information
 * 
 * Creates a new error entry with all provided information and stores it
 * in the error manager. Allocates memory for error message and source line.
 * 
 * @param line Line number where error occurred (1-based)
 * @param column Column number where error started (1-based)
 * @param length Length of problematic token/segment
 * @param level Error severity level
 * @param context Context identifier
 * @param error_code 6-character error code
 * @param message Formatted error message (will be copied)
 */
static void add_error_entry(uint16_t line, uint8_t column, uint8_t length,
                           ErrorLevel level, const char* context, 
                           const char* error_code, const char* message) {
    /* Initialize start time on first error (for relative timestamps) */
    if (error_manager.start_time == 0) {
        error_manager.start_time = time(NULL);
    }
    
    /* Validate error code */
    if (!validate_error_code(error_code)) {
        fprintf(stderr, "\033[33mWARNING\033[0m: Invalid error code format: %s\n", 
                error_code ? error_code : "(null)");
        error_code = "7A00";  /* Use default error code */
    }
    
    /* Ensure capacity in entries array */
    if (!ensure_capacity()) {
        return;  /* Cannot store error due to memory allocation failure */
    }
    
    /* Initialize new error entry */
    ErrorEntry* entry = &error_manager.entries[error_manager.count];
    memset(entry, 0, sizeof(ErrorEntry));
    
    /* Allocate and copy error message */
    if (message != NULL) {
        size_t msg_len = strlen(message);
        entry->message = malloc(msg_len + 1);
        if (entry->message == NULL) {
            fprintf(stderr, "\033[31mERROR\033[0m: Failed to allocate memory for error message\n");
            return;
        }
        memcpy(entry->message, message, msg_len);
        entry->message[msg_len] = '\0';
    }
    
    /* Fill entry fields */
    entry->line = line;
    entry->column = column;
    entry->length = length;
    entry->level = level;
    
    /* Copy error code (6 characters + null terminator) */
    if (error_code != NULL) {
        strncpy(entry->error_code, error_code, ERROR_CODE_SIZE - 1);
        entry->error_code[ERROR_CODE_SIZE - 1] = '\0';
    } else {
        strcpy(entry->error_code, "2E0000");
    }
    
    /* Copy context string (safe with bounds checking) */
    if (context != NULL) {
        strncpy(entry->context, context, sizeof(entry->context) - 1);
        entry->context[sizeof(entry->context) - 1] = '\0';
    } else {
        entry->context[0] = '\0';
    }
    
    /* Calculate relative timestamp (seconds since first error) */
    time_t current_time = time(NULL);
    entry->timestamp = (uint32_t)difftime(current_time, 
                                         error_manager.start_time);
    
    /* Copy source line if available for contextual display */
    if (line > 0 && error_manager.source_lines != NULL && 
        line <= error_manager.source_line_count) {
        const char* src_line = error_manager.source_lines[line - 1];  /* 1-based to 0-based */
        if (src_line != NULL) {
            entry->source_line = duplicate_string(src_line);
        }
    }
    
    /* Update counters */
    error_manager.count++;
    
    if (level == ERROR_LEVEL_WARNING) {
        error_manager.warning_count++;
    } else {
        error_manager.error_count++;
    }
}

/**
 * @brief Internal formatted error reporting with variable arguments
 * 
 * Formats the error message using vsnprintf and adds it to the error manager.
 * 
 * @param level Error severity level
 * @param error_code 6-character error code
 * @param line Line number where error occurred
 * @param column Column number where error started
 * @param length Length of problematic token/segment
 * @param context Context identifier
 * @param format printf-style format string
 * @param args Variable arguments list
 */
static void report_error_va(ErrorLevel level, const char* error_code,
                           uint16_t line, uint8_t column, uint8_t length,
                           const char* context, const char* format, va_list args) {
    char buffer[ERROR_MESSAGE_BUFFER_SIZE];
    
    /* Format error message with variable arguments */
    int written = vsnprintf(buffer, sizeof(buffer), format, args);
    
    /* Handle formatting errors */
    if (written < 0) {
        snprintf(buffer, sizeof(buffer), "Error message formatting failed");
    } else if ((size_t)written >= sizeof(buffer)) {
        buffer[sizeof(buffer) - 1] = '\0';  /* Ensure null termination */
    }
    
    /* Add formatted error to error manager */
    add_error_entry(line, column, length, level, context, error_code, buffer);
}

/**
 * @brief Calculate visual column with tab expansion
 * 
 * Converts byte-based column position to visual column position
 * by expanding tab characters to multiple spaces.
 * 
 * @param line Source line string (may contain tabs)
 * @param target_column Byte-based column position (0-based)
 * @return uint16_t Visual column position (0-based, tabs expanded)
 */
static uint16_t calculate_visual_column(const char* line, uint16_t target_column) {
    if (line == NULL) {
        return 0;
    }
    
    uint16_t visual_col = 0;
    uint16_t i = 0;
    
    /* Iterate through characters up to target column */
    while (i < target_column && line[i] != '\0') {
        if (line[i] == '\t') {
            /* Tab moves to next multiple of TAB_SIZE */
            visual_col = (visual_col / TAB_SIZE + 1) * TAB_SIZE;
        } else {
            visual_col++;
        }
        i++;
    }
    
    return visual_col;
}

/**
 * @brief Calculate visual length considering tabs
 * 
 * Calculates the visual length of a segment considering tab expansion.
 * 
 * @param segment Pointer to segment start in source line
 * @param segment_len Byte length of segment
 * @param start_visual_col Visual column at segment start
 * @return uint16_t Visual length of segment (with tabs expanded)
 */
static uint16_t calculate_visual_length(const char* segment, uint8_t segment_len, 
                                       uint16_t start_visual_col) {
    uint16_t visual_length = 0;
    uint16_t current_col = start_visual_col;
    
    /* Calculate visual length by processing each character */
    for (uint8_t i = 0; i < segment_len; i++) {
        char c = segment[i];
        if (c == '\0') {
            break;  /* End of string reached */
        }
        if (c == '\t') {
            /* Tab expands to spaces until next tab stop */
            uint16_t tab_spaces = TAB_SIZE - (current_col % TAB_SIZE);
            if (tab_spaces == 0) {
                tab_spaces = TAB_SIZE;  /* Tab at exact tab stop */
            }
            visual_length += tab_spaces;
            current_col += tab_spaces;
        } else {
            visual_length++;
            current_col++;
        }
    }
    
    return visual_length;
}

/**
 * @brief Calculate visual length of entire line considering tabs
 * 
 * Calculates the visual length of entire source line with tab expansion.
 * 
 * @param line Source line string
 * @return uint16_t Visual length of line (with tabs expanded)
 */
static uint16_t calculate_visual_line_length(const char* line) {
    if (line == NULL) {
        return 0;
    }
    
    uint16_t visual_length = 0;
    
    /* Calculate visual length by processing each character */
    for (const char* p = line; *p != '\0'; p++) {
        if (*p == '\t') {
            /* Tab expands to spaces until next tab stop */
            uint16_t tab_spaces = TAB_SIZE - (visual_length % TAB_SIZE);
            if (tab_spaces == 0) {
                tab_spaces = TAB_SIZE;  /* Tab at exact tab stop */
            }
            visual_length += tab_spaces;
        } else {
            visual_length++;
        }
    }
    
    return visual_length;
}

/**
 * @brief Print source line with error highlighting
 * 
 * Displays a source line with line number and visual indicator
 * pointing to the error location. Handles tab expansion correctly.
 * 
 * @param source_line Source line string to display
 * @param line Line number to display (1-based)
 * @param column Error column position (1-based, byte offset)
 * @param length Error token length (in characters)
 * @param is_warning True if warning, false if error/fatal
 */
static void print_error_source_line(const char* source_line, uint16_t line, 
                                   uint8_t column, uint8_t length, bool is_warning) {
    if (source_line == NULL) {
        return;
    }
    
    size_t line_len = strlen(source_line);
    if (line_len == 0) {
        return;
    }
    
    /* Calculate visual line length */
    uint16_t visual_line_length = calculate_visual_line_length(source_line);
    
    /* Calculate visual column and length for error marker */
    uint16_t visual_col = 0;
    uint16_t visual_token_length = (length > 0) ? length : 1;
    
    if (column > 0 && column <= line_len) {
        /* Calculate visual column position (with tab expansion) */
        uint16_t column_0_based = column - 1;
        visual_col = calculate_visual_column(source_line, column_0_based);
        
        /* Calculate visual token length considering tabs */
        if (length > 0) {
            visual_token_length = calculate_visual_length(
                source_line + column_0_based, 
                length, 
                visual_col
            );
        }
        
        /* Ensure visual_col + visual_token_length doesn't exceed visual_line_length */
        if (visual_col + visual_token_length > visual_line_length) {
            visual_token_length = visual_line_length - visual_col;
            if (visual_token_length == 0) {
                visual_token_length = 1;
            }
        }
    } else if (column > line_len) {
        /* If column is beyond line length, point to the end */
        visual_col = visual_line_length;
        visual_token_length = 1;
    } else {
        /* If column is 0 or invalid, point to beginning */
        visual_col = 0;
        visual_token_length = 1;
    }
    
    /* Calculate padding for line and column numbers */
    int line_digits = count_digits(line);
    int col_digits = count_digits(column);
    
    /* Ensure minimum width */
    if (line_digits < MAX_LINE_DIGITS) line_digits = MAX_LINE_DIGITS;
    if (col_digits < MAX_COL_DIGITS) col_digits = MAX_COL_DIGITS;
    
    /* Print line number and source line */
    printf("%*u:%-*u | %s\n", line_digits, line, col_digits, column, source_line);
    
    /* Print error indicator line */
    printf("%*s | ", line_digits + col_digits + 1, "");
    
    /* Set color based on error/warning */
    if (is_warning) {
        printf("\033[33m");  /* Yellow for warnings */
    } else {
        printf("\033[31m");  /* Red for errors */
    }
    
    /* Print '^' for the error token */
    for (uint16_t i = 0; i < visual_col && i < visual_line_length; i++) {
        if (i < visual_token_length && visual_col == 0) {
            putchar('^');  /* If error at beginning, use '^' */
        } else {
            putchar('~');
        }
    }
    
    /* Print '^' for the error token */
    for (uint16_t i = 0; i < visual_token_length && 
         (visual_col + i) < visual_line_length; i++) {
        putchar('^');
    }
    
    /* Print '~' for the rest of the line */
    uint16_t pos = visual_col + visual_token_length;
    for (uint16_t i = pos; i < visual_line_length; i++) {
        putchar('~');
    }
    
    printf("\033[0m");
}

/**
 * @brief Print a single error entry with full context
 * 
 * Formats and displays an error entry with color coding,
 * error codes, location information, and source context.
 * 
 * @param entry Pointer to error entry to display
 * @param is_warning True if entry is a warning, false if error/fatal
 */
static void print_error_entry(const ErrorEntry* entry, bool is_warning) {
    /* Print error/warning indicator with appropriate color */
    if (is_warning) {
        printf("\033[33mWARNING\033[0m ");  /* Yellow for warnings */
    } else {
        if (entry->level == ERROR_LEVEL_FATAL) {
            printf("\033[31mFATAL\033[0m ");  /* Red for fatal errors */
        } else {
            printf("\033[31mERROR\033[0m ");  /* Red for regular errors */
        }
    }
    
    /* Print error code */
    printf("[%s]: ", entry->error_code);
    
    /* Print context if available */
    if (entry->context[0] != '\0') {
        printf("(%s) ", entry->context);
    }
    
    /* Print error message */
    printf("%s\n", entry->message ? entry->message : "(no message)");
    
    /* Print location information if available */
    if (entry->line > 0) {
        /* Print source file if available in context */
        if (entry->context[0] != '\0')
            printf(" in %s", entry->context);
        printf("\n");
        
        /* Print source line with highlighting */
        print_error_source_line(entry->source_line, entry->line, 
                               entry->column, entry->length, is_warning);
    }
    
    printf("\n");
}

/* Public API implementations */

/**
 * @brief Extended error reporting with severity level, context, token length, and error code
 * 
 * Public interface for reporting errors with full context information.
 * See function documentation in header for parameters.
 */
void errhandler__report_error_ex(ErrorLevel level, const char* error_code,
                            uint16_t line, uint8_t column, uint8_t length,
                            const char* context, const char* format, ...) {
    va_list args;
    va_start(args, format);
    report_error_va(level, error_code, line, column, length, context, format, args);
    va_end(args);
}

/**
 * @brief Set source code lines for contextual error display
 * 
 * Public interface for setting source code reference.
 * See function documentation in header for parameters.
 */
void errhandler__set_source_code(const char** source_lines, uint16_t line_count) {
    error_manager.source_lines = source_lines;
    error_manager.source_line_count = line_count;
}

/**
 * @brief Clear source code reference from error manager
 * 
 * Public interface for clearing source code reference.
 * See function documentation in header.
 */
void errhandler__clear_source_code(void) {
    error_manager.source_lines = NULL;
    error_manager.source_line_count = 0;
}

/**
 * @brief Print all stored error entries
 * 
 * Public interface for printing all error entries (excluding warnings).
 * See function documentation in header.
 */
void errhandler__print_errors(void) {
    if (error_manager.error_count == 0) return;
    
    for (uint16_t i = 0; i < error_manager.count; i++) {
        const ErrorEntry* entry = &error_manager.entries[i];
        if (entry != NULL && entry->level != ERROR_LEVEL_WARNING) {
            print_error_entry(entry, false);
        }
    }
}

/**
 * @brief Print all stored warning entries
 * 
 * Public interface for printing all warning entries.
 * See function documentation in header.
 */
void errhandler__print_warnings(void) {
    if (error_manager.warning_count == 0) return;
    
    for (uint16_t i = 0; i < error_manager.count; i++) {
        const ErrorEntry* entry = &error_manager.entries[i];
        if (entry != NULL && entry->level == ERROR_LEVEL_WARNING) {
            print_error_entry(entry, true);
        }
    }
}

/**
 * @brief Backward compatibility: report error with default length and code
 * 
 * Public interface for reporting errors with default values.
 * See function documentation in header for parameters.
 */
void errhandler__report_error
    ( uint16_t line
    , uint8_t column
    , const char* context
    , const char* format
    , ...
) {
    va_list args;
    va_start(args, format);
    report_error_va(ERROR_LEVEL_ERROR, ERROR_CODE_SYNTAX_GENERIC, 
                   line, column, 1, context, format, args);
    va_end(args);
}

/**
 * @brief Backward compatibility: report warning with default length and code
 * 
 * Public interface for reporting warnings with default values.
 * See function documentation in header for parameters.
 */
void errhandler__report_warning(uint16_t line, uint8_t column,
                           const char* format, ...) {
    va_list args;
    va_start(args, format);
    report_error_va(ERROR_LEVEL_WARNING, ERROR_CODE_SYNTAX_GENERIC,
                   line, column, 1, "syntax", format, args);
    va_end(args);
}

/**
 * @brief Check if any errors have been reported
 * 
 * Public interface for error presence check.
 * See function documentation in header for return value.
 */
bool errhandler__has_errors(void) {
    return error_manager.error_count > 0;
}

/**
 * @brief Check if any warnings have been reported
 * 
 * Public interface for warning presence check.
 * See function documentation in header for return value.
 */
bool errhandler__has_warnings(void) {
    return error_manager.warning_count > 0;
}

/**
 * @brief Get total count of error entries
 * 
 * Public interface for error count retrieval.
 * See function documentation in header for return value.
 */
uint16_t errhandler__get_error_count(void) {
    return error_manager.error_count;
}

/**
 * @brief Get total count of warning entries
 * 
 * Public interface for warning count retrieval.
 * See function documentation in header for return value.
 */
uint16_t errhandler__get_warning_count(void) {
    return error_manager.warning_count;
}

/**
 * @brief Convert error level enum to string representation
 * 
 * Public interface for error level string conversion.
 * See function documentation in header for parameters and return value.
 */
const char* errhandler__get_error_level_string(ErrorLevel level) {
    switch (level) {
        case ERROR_LEVEL_WARNING: return "WARNING";
        case ERROR_LEVEL_ERROR: return "ERROR";
        case ERROR_LEVEL_FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Parse error code components
 * 
 * Public interface for parsing error code into components.
 * See function documentation in header for parameters and return value.
 */
bool errhandler__parse_error_code(const char* error_code,
                                 char type[3], char group[3], char number[3]) {
    if (!error_code || !validate_error_code(error_code)) {
        return false;
    }
    
    /* Extract components */
    if (type) {
        type[0] = error_code[0];
        type[1] = error_code[1];
        type[2] = '\0';
    }
    
    if (group) {
        group[0] = error_code[2];
        group[1] = error_code[3];
        group[2] = '\0';
    }
    
    if (number) {
        number[0] = error_code[4];
        number[1] = error_code[5];
        number[2] = '\0';
    }
    
    return true;
}

/**
 * @brief Free all memory allocated by error manager and reset state
 * 
 * Public interface for cleaning up error manager resources.
 * See function documentation in header.
 */
void errhandler__free_error_manager(void) {
    /* Free all error messages and source lines */
    if (error_manager.entries != NULL) {
        for (uint16_t i = 0; i < error_manager.count; i++) {
            if (error_manager.entries[i].message != NULL) {
                free(error_manager.entries[i].message);
                error_manager.entries[i].message = NULL;
            }
            if (error_manager.entries[i].source_line != NULL) {
                free(error_manager.entries[i].source_line);
                error_manager.entries[i].source_line = NULL;
            }
        }
        free(error_manager.entries);
        error_manager.entries = NULL;
    }
    
    /* Reset to initial state */
    error_manager.count = 0;
    error_manager.capacity = 0;
    error_manager.error_count = 0;
    error_manager.warning_count = 0;
    error_manager.source_lines = NULL;
    error_manager.source_line_count = 0;
    error_manager.start_time = 0;
    error_manager.error_id_counter = 0;
}
