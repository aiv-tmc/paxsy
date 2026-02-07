#ifndef ERRHANDLER_H
#define ERRHANDLER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file errhandler.h
 * @brief Error management system for the Paxsy compiler
 * 
 * Provides comprehensive error reporting and management system
 * with severity levels, contextual error messages, source code
 * visualization, and error statistics.
 */

/**
 * @brief Error severity levels for classification
 * 
 * Defines three levels of error severity:
 * - WARNING: Non-critical issues that don't stop compilation
 * - ERROR: Critical errors that prevent successful compilation  
 * - FATAL: Severe errors that force immediate termination
 */
typedef enum {
    ERROR_LEVEL_WARNING,    /* Non-critical issues that don't stop compilation */
    ERROR_LEVEL_ERROR,      /* Critical errors that prevent successful compilation */
    ERROR_LEVEL_FATAL       /* Severe errors that force immediate termination */
} ErrorLevel;

/* Error code system - 16-bit hexadecimal values */

/**
 * @brief Report an error with extended information and error code
 * 
 * @param level Error severity level (WARNING, ERROR, FATAL)
 * @param error_code 16-bit hexadecimal error code
 * @param line Line number where error occurred (1-based)
 * @param column Column number where error started (1-based)
 * @param length Length of the problematic token/segment
 * @param context Context identifier
 * @param format printf-style format string for error message
 * @param ... Variable arguments for format string
 */
void errhandler__report_error_ex
    ( ErrorLevel level
    , uint16_t error_code
    , uint16_t line
    , uint8_t column
    , uint8_t length
    , const char* context
    , const char* format
    , ...
);

/* Error context management */

/**
 * @brief Set source code lines for contextual error display
 * 
 * @param source_lines Array of strings, each representing a source line
 * @param line_count Number of lines in the source_lines array
 * 
 * @note The error manager does not take ownership of the strings,
 *       they must remain valid while errors are being reported
 */
void errhandler__set_source_code
    ( const char** source_lines
    , uint16_t line_count
);

/**
 * @brief Clear source code reference from error manager
 * 
 * Removes the current source code reference without freeing it.
 * Call this when the source code is no longer available.
 */
void errhandler__clear_source_code(void);

/* Error management functions */

/**
 * @brief Print all stored error entries to stdout
 * 
 * Prints errors with source context, line numbers, and visual indicators.
 * Only prints entries with ERROR_LEVEL_ERROR or ERROR_LEVEL_FATAL.
 */
void errhandler__print_errors(void);

/**
 * @brief Print all stored warning entries to stdout
 * 
 * Prints warnings with source context, line numbers, and visual indicators.
 * Only prints entries with ERROR_LEVEL_WARNING.
 */
void errhandler__print_warnings(void);

/**
 * @brief Check if any errors have been reported
 * 
 * @return true if at least one ERROR_LEVEL_ERROR or ERROR_LEVEL_FATAL exists
 * @return false if no errors have been reported
 */
bool errhandler__has_errors(void);

/**
 * @brief Check if any warnings have been reported
 * 
 * @return true if at least one ERROR_LEVEL_WARNING exists
 * @return false if no warnings have been reported
 */
bool errhandler__has_warnings(void);

/**
 * @brief Free all memory allocated by error manager and reset state
 * 
 * Releases all dynamically allocated memory including error messages
 * and source line copies. Resets all counters to zero.
 */
void errhandler__free_error_manager(void);

/* Utility functions */

/**
 * @brief Get total count of error entries
 * 
 * @return uint16_t Number of ERROR_LEVEL_ERROR and ERROR_LEVEL_FATAL entries
 */
uint16_t errhandler__get_error_count(void);

/**
 * @brief Get total count of warning entries
 * 
 * @return uint16_t Number of ERROR_LEVEL_WARNING entries
 */
uint16_t errhandler__get_warning_count(void);

/**
 * @brief Convert error level enum to string representation
 * 
 * @param level Error level enum value
 * @return const char* String representation ("WARNING", "ERROR", "FATAL", "UNKNOWN")
 */
const char* errhandler__get_error_level_string(ErrorLevel level);

/**
 * @brief Parse error code string components
 * 
 * @param error_code_str 4-character hex error code string
 * @param type Output buffer for 2-character type code
 * @param group Output buffer for 2-character group code
 * @param number Output buffer for 2-character number code
 * @return true if code parsed successfully
 * @return false if code is invalid
 */
bool errhandler__parse_error_code
    ( const char* error_code_str
    , char type[3]
    , char group[3]
    , char number[3]
);

/* Common error codes - 16-bit hexadecimal values */

/* Syntax errors */
#define ERROR_CODE_SYNTAX_GENERIC           0x7A00
#define ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN  0x7A01
#define ERROR_CODE_SYNTAX_UNEXPECTED_EOF    0x7A02
#define ERROR_CODE_SYNTAX_INVALID_CHAR      0x7A03
#define ERROR_CODE_SYNTAX_MISSING_SEMICOLON 0x7A04
#define ERROR_CODE_SYNTAX_INVALID_STATEMENT 0x7A05
#define ERROR_CODE_SYNTAX_UNCLOSED_QUOTE    0x7A06
#define ERROR_CODE_SYNTAX_MISMATCHED_PAREN  0x7A07

/* Lexical errors */
#define ERROR_CODE_LEXER_INVALID_NUMBER     0xE001
#define ERROR_CODE_LEXER_INVALID_ESCAPE     0xE002
#define ERROR_CODE_LEXER_UNCLOSED_STRING    0xE003
#define ERROR_CODE_LEXER_UNKNOWN_CHAR       0xE004

/* Semantic errors */
#define ERROR_CODE_SEM_MISMATCH             0xA401
#define ERROR_CODE_SEM_INVALID_CAST         0xA402
#define ERROR_CODE_SEM_UNDEFINED_VAR        0xA403
#define ERROR_CODE_SEM_INVALID_OPERATION    0xA404
#define ERROR_CODE_SEM_REDECLARATION        0xA405
#define ERROR_CODE_SEM_UNDECLARED_SYMBOL    0xA406
#define ERROR_CODE_SEM_UNINITIALIZED        0xA407
#define ERROR_CODE_SEM_ASSIGN_TO_CONST      0xA408
#define ERROR_CODE_SEM_TYPE_ERROR           0xA409
#define ERROR_CODE_SEM_UNUSED_VARIABLE      0xA40A

/* Memory errors */
#define ERROR_CODE_MEMORY_ALLOCATION        0x6B01
#define ERROR_CODE_MEMORY_OVERFLOW          0x6B02
#define ERROR_CODE_MEMORY_INVALID_FREE      0x6B03

/* Runtime errors */
#define ERROR_CODE_RUNTIME_DIV_BY_ZERO      0x2301
#define ERROR_CODE_RUNTIME_OUT_OF_BOUNDS    0x2302
#define ERROR_CODE_RUNTIME_OVERFLOW         0x2303

/* I/O errors */
#define ERROR_CODE_IO_FILE_NOT_FOUND        0x8201
#define ERROR_CODE_IO_PERMISSION_DENIED     0x8202
#define ERROR_CODE_IO_READ_ERROR            0x8203
#define ERROR_CODE_IO_WRITE_ERROR           0x8204

/* Backward compatibility functions (default length = 1, default code = 0x7A00) */

/**
 * @brief Report an error with default length and code (backward compatibility)
 * 
 * @param line Line number where error occurred (1-based)
 * @param column Column number where error started (1-based)
 * @param format printf-style format string for error message
 * @param ... Variable arguments for format string
 * 
 * @note Uses default length of 1, context "syntax", and code ERROR_CODE_SYNTAX_GENERIC
 */
void errhandler__report_error
    ( uint16_t line
    , uint8_t column
    , const char* context
    , const char* format
    , ...
);

/**
 * @brief Report a warning with default length and code (backward compatibility)
 * 
 * @param line Line number where warning occurred (1-based)
 * @param column Column number where warning started (1-based)
 * @param format printf-style format string for warning message
 * @param ... Variable arguments for format string
 * 
 * @note Uses default length of 1, context "syntax", and code ERROR_CODE_SYNTAX_GENERIC
 */
void errhandler__report_warning
    ( uint16_t line
    , uint8_t column
    , const char* format
    , ...
);

#endif
