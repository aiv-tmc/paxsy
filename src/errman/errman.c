#include "errman.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

/**
 * Comprehensive error/warning information storage structure
 */
typedef struct __attribute__((packed)) {
    char* message;          /* Formatted error message string */
    uint16_t line;          /* Line number where error occurred */
    uint8_t column;         /* Column number where error occurred */
    uint8_t length;         /* Length of problematic token/segment */
    ErrorLevel level;       /* Severity level of the error */
    char context[8];        /* Context identifier */
    uint64_t timestamp;     /* Timestamp in milliseconds */
    uint64_t error_code;    /* Unique error code */
} ErrorEntry;

/**
 * Global error manager state
 */
static struct {
    ErrorEntry* entries;            /* Dynamic array of error entries */
    uint16_t count;                 /* Current number of error entries */
    uint16_t capacity;              /* Capacity of the entries array */
    uint16_t error_count;           /* Total count of errors */
    uint16_t warning_count;         /* Total count of warnings */
    const char** source_lines;      /* Source code lines for contextual display */
    uint16_t source_line_count;     /* Number of source code lines available */
    time_t start_time;              /* Program start time for relative timestamps */
} error_manager = { NULL, 0, 0, 0, 0, NULL, 0, 0 };

/**
 * Get current timestamp in milliseconds
 */
static uint64_t get_timestamp_ms(void) {
    /* Using standard C99 functions instead of POSIX-specific clock_gettime */
    time_t current_time;
    struct tm *time_info;
    
    time(&current_time);
    return (uint64_t)(current_time) * 1000;
}

/**
 * Convert error code to string representation
 */
static void error_code_to_string(uint64_t code, char* buffer) {
    const char* alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (int i = 7; i >= 0; i--) {
        buffer[i] = alphabet[code & 0x1F];
        code >>= 5;
    }
    buffer[8] = '\0';
}

/**
 * Map error context and message to appropriate error code
 */
static uint64_t map_error_code(const char* context, const char* message) {
    /* Default error codes based on context and message content */
    if (strstr(message, "Unknow type") != NULL) return ERROR_UNKNOWN_TYPE;
    if (strstr(message, "Invalid extension") != NULL) return ERROR_INVALID_FILE_EXT;
    if (strstr(message, "No input file") != NULL) return ERROR_NO_INPUT_FILE;
    if (strstr(context, "Syntax") != NULL) return ERROR_SYNTAX;
    if (strstr(message, "Memory allocation") != NULL) return ERROR_MEMORY_ALLOC;
    if (strstr(message, "Undefined") != NULL) return ERROR_UNDEFINED_SYMBOL;
    if (strstr(message, "Type mismatch") != NULL) return ERROR_TYPE_MISMATCH;
    if (strstr(message, "invalid operator") != NULL) return ERROR_INVALID_OPERATOR;
    
    /* Default fallback code */
    return ERROR_SYNTAX;
}

/**
 * Add a new error entry with comprehensive information
 */
static void add_error_entry(uint16_t line, uint8_t column, uint8_t length, 
                           ErrorLevel level, const char* context, 
                           const char* message) {
    /* Check capacity and resize if needed */
    if (error_manager.count >= error_manager.capacity) {
        uint16_t new_capacity = error_manager.capacity ? 
                               (error_manager.capacity < UINT16_MAX/2 ? 
                                error_manager.capacity * 2 : UINT16_MAX) : 16;
        
        if (new_capacity > error_manager.capacity) {
            ErrorEntry* new_entries = realloc(error_manager.entries, 
                                            new_capacity * sizeof(ErrorEntry));
            if (new_entries) {
                error_manager.entries = new_entries;
                error_manager.capacity = new_capacity;
            } else {
                return;
            }
        } else {
            return;
        }
    }

    /* Initialize error manager start time on first error */
    if (error_manager.start_time == 0) {
        time(&error_manager.start_time);
    }

    /* Initialize new error entry */
    ErrorEntry* entry = &error_manager.entries[error_manager.count];
    entry->line = line;
    entry->column = column;
    entry->length = length;
    entry->level = level;
    
    /* Calculate timestamp relative to start time */
    time_t current_time;
    time(&current_time);
    entry->timestamp = (uint64_t)(difftime(current_time, error_manager.start_time) * 1000);
    
    entry->error_code = map_error_code(context, message);
    
    /* Copy context with fixed size */
    if (context) {
        strncpy(entry->context, context, sizeof(entry->context) - 1);
        entry->context[sizeof(entry->context) - 1] = '\0';
    } else {
        entry->context[0] = '\0';
    }
    
    /* Allocate and copy error message */
    size_t msg_len = strlen(message);
    entry->message = malloc(msg_len + 1);
    if (entry->message) {
        memcpy(entry->message, message, msg_len + 1);
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
 * Formatted error reporting with variable arguments
 */
static void report_error_va(ErrorLevel level, uint16_t line, uint8_t column, uint8_t length,
                           const char* context, const char* format, va_list args) {
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, args);
    
    add_error_entry(line, column, length, level, context, buffer);
}

/* Public API implementations */
void errman__report_error_ex(ErrorLevel level, uint16_t line, uint8_t column, uint8_t length,
                    const char* context, const char* format, ...) {
    va_list args;
    va_start(args, format);
    report_error_va(level, line, column, length, context, format, args);
    va_end(args);
}

void errman__set_source_code(const char** source_lines, uint16_t line_count) {
    error_manager.source_lines = source_lines;
    error_manager.source_line_count = line_count;
}

void errman__clear_source_code(void) {
    error_manager.source_lines = NULL;
    error_manager.source_line_count = 0;
}

/**
 * Calculate visual column with tab expansion
 */
static uint8_t calculate_visual_column(const char* line, uint8_t target_column) {
    uint8_t visual_col = 0;
    uint8_t i = 0;
    
    while (i < target_column && line[i] != '\0') {
        if (line[i] == '\t') {
            visual_col = (visual_col + 8) & ~7; /* Tab to next multiple of 8 */
        } else {
            visual_col++;
        }
        i++;
    }
    return visual_col;
}

/**
 * Print source code context with error highlighting
 */
static void print_error_context(uint16_t line, uint8_t column, uint8_t length) {
    if (!error_manager.source_lines || line == 0 || line > error_manager.source_line_count) {
        return;
    }
    
    const char* source_line = error_manager.source_lines[line - 1];
    if (!source_line) {
        return;
    }
    
    /* Print source line */
    fprintf(stderr, "\t%d:%d\t|\t%s\n", line, column, source_line);
    
    /* Calculate and print error underline */
    if (column > 0) {
        uint8_t line_len = (uint8_t)strlen(source_line);
        if (column <= line_len) {
            uint8_t visual_col = calculate_visual_column(source_line, column - 1);
            uint8_t underline_length = length;
            
            /* Adjust length to not exceed line boundaries */
            if (column - 1 + underline_length > line_len) {
                underline_length = line_len - column + 1;
            }
            
            /* Calculate visual length considering tabs */
            uint8_t visual_length = 0;
            for (uint8_t i = 0; i < underline_length; i++) {
                char c = source_line[column - 1 + i];
                if (c == '\t') {
                    visual_length += 8 - (visual_col + visual_length) % 8;
                } else {
                    visual_length++;
                }
            }
            
            /* Print underline */
            fprintf(stderr, "\t\t\t|\t%*s", visual_col, "");
            for (uint8_t i = 0; i < visual_length; i++) {
                fputc('~', stderr);
            }
            fputc('\n', stderr);
        }
    }
}

void errman__print_errors(void) {
    char code_buffer[9];
    
    for (uint16_t i = 0; i < error_manager.count; i++) {
        ErrorEntry* entry = &error_manager.entries[i];
        if (entry->level != ERROR_LEVEL_WARNING) {
            error_code_to_string(entry->error_code, code_buffer);
            
            const char* level_str = (entry->level == ERROR_LEVEL_FATAL) ? "FATAL" : "ERROR";
            
            fprintf(stderr, "%llums\t%s[%s]: %s: %s\n", 
                    (unsigned long long)entry->timestamp, level_str, code_buffer, 
                    entry->context[0] ? entry->context : "unknown", 
                    entry->message);
            
            if (entry->line > 0) {
                print_error_context(entry->line, entry->column, entry->length);
            }
        }
    }
}

void errman__print_warnings(void) {
    char code_buffer[9];
    
    for (uint16_t i = 0; i < error_manager.count; i++) {
        ErrorEntry* entry = &error_manager.entries[i];
        if (entry->level == ERROR_LEVEL_WARNING) {
            error_code_to_string(entry->error_code, code_buffer);
            
            fprintf(stderr, "%llums\tWARNING[%s]: %s: %s\n", 
                    (unsigned long long)entry->timestamp, code_buffer,
                    entry->context[0] ? entry->context : "unknown", 
                    entry->message);
            
            if (entry->line > 0) {
                print_error_context(entry->line, entry->column, entry->length);
            }
        }
    }
}

void errman__report_error(uint16_t line, uint8_t column, const char* format, ...) {
    va_list args;
    va_start(args, format);
    report_error_va(ERROR_LEVEL_ERROR, line, column, 1, "parser", format, args);
    va_end(args);
}

void errman__report_warning(uint16_t line, uint8_t column, const char* format, ...) {
    va_list args;
    va_start(args, format);
    report_error_va(ERROR_LEVEL_WARNING, line, column, 1, "parser", format, args);
    va_end(args);
}

bool errman__has_errors(void) {
    return error_manager.error_count > 0;
}

bool errman__has_warnings(void) {
    return error_manager.warning_count > 0;
}

uint16_t errman__get_error_count(void) {
    return error_manager.error_count;
}

uint16_t errman__get_warning_count(void) {
    return error_manager.warning_count;
}

const char* errman__get_error_level_string(ErrorLevel level) {
    switch (level) {
        case ERROR_LEVEL_WARNING: return "WARNING";
        case ERROR_LEVEL_ERROR: return "ERROR";
        case ERROR_LEVEL_FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

void errman__free_error_manager(void) {
    for (uint16_t i = 0; i < error_manager.count; i++) {
        free(error_manager.entries[i].message);
    }
    free(error_manager.entries);
    
    /* Reset to initial state */
    error_manager.entries = NULL;
    error_manager.count = 0;
    error_manager.capacity = 0;
    error_manager.error_count = 0;
    error_manager.warning_count = 0;
    error_manager.source_lines = NULL;
    error_manager.source_line_count = 0;
    error_manager.start_time = 0;
}
