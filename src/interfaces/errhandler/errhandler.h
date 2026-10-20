#ifndef ERRHANDLER_H
#define ERRHANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

/*
 * Error severity levels.
 */
typedef enum {
    ERROR_LEVEL_NOTE,     /* informative note */
    ERROR_LEVEL_WARNING,  /* warning */
    ERROR_LEVEL_ERROR,    /* error */
    ERROR_LEVEL_FATAL     /* fatal error */
} ErrorLevel;

/*
 * Report an error with full details.
 * 'context' is a short string (up to 8 chars) indicating the compilation stage,
 * e.g. "lexer", "parser", "semantic", "ir", "opt".
 * 'line' is 1‑based, 'column' is 1‑based byte offset (tabs expanded for display).
 * 'length' is the number of bytes in the erroneous token.
 * The message is formatted like printf.
 */
void errhandler__report_error_ex
    ( ErrorLevel level
    , uint32_t line
    , uint32_t column
    , uint32_t length
    , const char* context
    , const char* format
    , ...
    );

/*
 * Same as above but takes a va_list (for internal use).
 */
void errhandler__report_error_ex_va
    ( ErrorLevel level
    , uint32_t line
    , uint32_t column
    , uint32_t length
    , const char* context
    , const char* format
    , va_list args
    );

/*
 * Convenience wrapper: default length = 1, level = ERROR.
 */
static inline void errhandler__report_error
    ( uint32_t line
    , uint32_t column
    , const char* context
    , const char* format
    , ...
    ) {
    va_list args;
    va_start(args, format);
    errhandler__report_error_ex_va(ERROR_LEVEL_ERROR, line, column, 1,
                                   context, format, args);
    va_end(args);
}

/*
 * Set the current source file name (copied internally).
 */
void errhandler__set_current_filename(const char* filename);

/*
 * Provide an external array of source lines (not copied).
 * The array must remain valid until clear_source_code is called.
 */
void errhandler__set_source_code(const char** source_lines, uint32_t line_count);

/*
 * Release any source lines owned by the manager.
 */
void errhandler__clear_source_code(void);

/*
 * Load a source file from disk and store its lines internally.
 * Returns 0 on success, -1 on error.
 */
int errhandler__load_source_file(const char* filename);

/*
 * Print all recorded errors (and warnings if warnings_as_errors is set).
 */
void errhandler__print_errors(void);

/*
 * Print all recorded warnings and notes (unless suppressed or treated as errors).
 */
void errhandler__print_warnings(void);

/*
 * Query whether any errors (or warnings-as-errors) have been recorded.
 */
bool errhandler__has_errors(void);

/*
 * Query whether any warnings have been recorded.
 */
bool errhandler__has_warnings(void);

/*
 * Free all memory used by the error manager (including stored messages and source).
 */
void errhandler__free_error_manager(void);

/*
 * Return the number of errors (or errors + warnings if warnings_as_errors).
 */
uint16_t errhandler__get_error_count(void);

/*
 * Return the number of warnings.
 */
uint16_t errhandler__get_warning_count(void);

/*
 * Convert an ErrorLevel to a string.
 */
const char* errhandler__get_error_level_string(ErrorLevel level);

/*
 * Treat warnings as errors (they will be printed by print_errors and count as errors).
 */
void errhandler__set_warnings_as_errors(bool enable);

/*
 * Suppress printing of warnings (but they are still recorded).
 */
void errhandler__set_suppress_warnings(bool suppress);

#endif
