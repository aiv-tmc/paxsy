#ifndef ERRHANDLER_H
#define ERRHANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

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
 */
typedef enum {
    ERROR_LEVEL_WARNING,    /**< Non-critical issues that don't stop compilation */
    ERROR_LEVEL_ERROR,      /**< Critical errors that prevent successful compilation */
    ERROR_LEVEL_FATAL       /**< Severe errors that force immediate termination */
} ErrorLevel;

/**
 * @brief Report an error with extended information and error code
 *
 * @param level         Error severity level
 * @param error_code    16-bit hexadecimal error code
 * @param line          Line number where error occurred (1-based)
 * @param column        Column number where error started (1-based, byte offset)
 * @param length        Length of problematic token/segment (bytes)
 * @param context       Context identifier (e.g., "syntax", "semantic")
 * @param format        printf-style format string
 * @param ...           Variable arguments for format string
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

/**
 * @brief Set the current source filename for subsequent errors.
 *        The string is copied internally.
 * @param filename  Name of the source file being processed, or NULL to clear.
 */
void errhandler__set_current_filename(const char* filename);

/**
 * @brief Set source code lines for contextual error display
 *
 * @param source_lines  Array of strings, each representing a source line
 * @param line_count    Number of lines in the source_lines array
 *
 * @note The error manager does not take ownership of the strings unless
 *       source line copying is enabled (default). Use errhandler__set_copy_source()
 *       to change this behaviour.
 */
void errhandler__set_source_code(const char** source_lines, uint16_t line_count);

/**
 * @brief Clear source code reference from error manager
 */
void errhandler__clear_source_code(void);

/**
 * @brief Enable/disable copying of source lines into error entries
 *
 * When enabled (default), each error entry stores its own copy of the relevant
 * source line. When disabled, only a pointer to the line in the user‑provided
 * array is kept – the user must ensure the array remains valid until all
 * errors have been processed.
 *
 * @param enable  true to copy source lines, false to reference only
 */
void errhandler__set_copy_source(bool enable);

/**
 * @brief Print all stored error entries (ERROR and FATAL) to stdout
 */
void errhandler__print_errors(void);

/**
 * @brief Print all stored warning entries to stdout
 */
void errhandler__print_warnings(void);

/**
 * @brief Check if any errors have been reported
 * @return true if at least one ERROR or FATAL entry exists
 */
bool errhandler__has_errors(void);

/**
 * @brief Check if any warnings have been reported
 * @return true if at least one WARNING entry exists
 */
bool errhandler__has_warnings(void);

/**
 * @brief Free all memory allocated by error manager and reset state
 */
void errhandler__free_error_manager(void);

/**
 * @brief Get total count of error entries (ERROR + FATAL)
 * @return uint16_t Number of error entries
 */
uint16_t errhandler__get_error_count(void);

/**
 * @brief Get total count of warning entries
 * @return uint16_t Number of warning entries
 */
uint16_t errhandler__get_warning_count(void);

/**
 * @brief Convert error level enum to string representation
 * @param level Error level enum value
 * @return const char* "WARNING", "ERROR", "FATAL", or "UNKNOWN"
 */
const char* errhandler__get_error_level_string(ErrorLevel level);

/**
 * @brief Parse error code string into type, group, number components
 *
 * @param error_code_str 4-character hex error code string (e.g., "7A00")
 * @param type           Output buffer for 1‑character type (must hold 2+ chars)
 * @param group          Output buffer for 1‑character group (must hold 2+ chars)
 * @param number         Output buffer for 2‑character number (must hold 3+ chars)
 * @return true if code parsed successfully, false otherwise
 */
bool errhandler__parse_error_code
    ( const char* error_code_str
    , char type[2]
    , char group[2]
    , char number[3]
    );

/* Syntax */
#define ERROR_CODE_SYNTAX_GENERIC           0x7A00
#define ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN  0x7A01
#define ERROR_CODE_SYNTAX_UNEXPECTED_EOF    0x7A02
#define ERROR_CODE_SYNTAX_INVALID_CHAR      0x7A03
#define ERROR_CODE_SYNTAX_MISSING_SEMICOLON 0x7A04
#define ERROR_CODE_SYNTAX_INVALID_STATEMENT 0x7A05
#define ERROR_CODE_SYNTAX_UNCLOSED_QUOTE    0x7A06
#define ERROR_CODE_SYNTAX_MISMATCHED_PAREN  0x7A07

/* Lexical */
#define ERROR_CODE_LEXER_INVALID_NUMBER     0xE000
#define ERROR_CODE_LEXER_INVALID_ESCAPE     0xE001
#define ERROR_CODE_LEXER_UNCLOSED_STRING    0xE002
#define ERROR_CODE_LEXER_UNKNOWN_CHAR       0xE003

/* Semantic */
#define ERROR_CODE_SEM_MISMATCH             0xA400
#define ERROR_CODE_SEM_INVALID_CAST         0xA401
#define ERROR_CODE_SEM_UNDEFINED_VAR        0xA402
#define ERROR_CODE_SEM_INVALID_OPERATION    0xA403
#define ERROR_CODE_SEM_REDECLARATION        0xA404
#define ERROR_CODE_SEM_UNDECLARED_SYMBOL    0xA405
#define ERROR_CODE_SEM_UNINITIALIZED        0xA406
#define ERROR_CODE_SEM_ASSIGN_TO_CONST      0xA407
#define ERROR_CODE_SEM_TYPE_ERROR           0xA408
#define ERROR_CODE_SEM_UNUSED_VARIABLE      0xA409
#define ERROR_CODE_SEM_MISSING_RETURN       0xA40A

/* Preprocessor */
#define ERROR_CODE_PP_UNKNOW_DIR            0x4C00
#define ERROR_CODE_PP_DIR_TOO_LONG          0x4C01
#define ERROR_CODE_PP_MACRO_DEF_FAILED      0x4C02
#define ERROR_CODE_PP_INVALID_DIR           0x4C03
#define ERROR_CODE_PP_UNDEFINED             0x4C04
#define ERROR_CODE_PP_MACRO_RECURSION       0x4C05
#define ERROR_CODE_PP_DUPLICATE_DIR         0x4C06

/* Compile */
#define ERROR_CODE_COM_FAILCREATE           0xFF00

/* Memory */
#define ERROR_CODE_MEMORY_ALLOCATION        0x6B00
#define ERROR_CODE_MEMORY_OVERFLOW          0x6B01
#define ERROR_CODE_MEMORY_INVALID_FREE      0x6B02

/* Runtime */
#define ERROR_CODE_RUNTIME_DIV_BY_ZERO      0x2300
#define ERROR_CODE_RUNTIME_OUT_OF_BOUNDS    0x2301
#define ERROR_CODE_RUNTIME_OVERFLOW         0x2302

/* I/O */
#define ERROR_CODE_IO_FILE_NOT_FOUND        0x8200
#define ERROR_CODE_IO_DOUBLE_FILE           0x8201
#define ERROR_CODE_IO_PERMISSION_DENIED     0x8202
#define ERROR_CODE_IO_READ                  0x8203
#define ERROR_CODE_IO_WRITE                 0x8204

/* Input flags */
#define ERROR_CODE_INPUT_MULTI_MOD_FLAGS    0x8900
#define ERROR_CODE_INPUT_INVALID_FLAG       0x8901
#define ERROR_CODE_INPUT_NO_SOURCE          0x8902

/**
 * @brief Simplified error reporting (backward compatibility)
 * @note Uses default length of 1 and the provided error code & context.
 */
static inline void errhandler__report_error
    ( uint16_t error_code
    , uint16_t line
    , uint8_t column
    , const char* context
    , const char* format
    , ...
    ) {
    va_list args;
    va_start(args, format);
    errhandler__report_error_ex
        ( ERROR_LEVEL_ERROR
        , error_code
        , line
        , column
        , 1
        , context
        , format
        , args
        );
    va_end(args);
}

/**
 * @brief Simplified warning reporting (backward compatibility)
 * @note Uses default length of 1, default error code ERROR_CODE_SYNTAX_GENERIC,
 *       and default context "syntax".
 */
static inline void errhandler__report_warning
    ( uint16_t line
    , uint8_t column
    , const char* format
    , ...
    ) {
    va_list args;
    va_start(args, format);
    errhandler__report_error_ex
        ( ERROR_LEVEL_WARNING
        , ERROR_CODE_SYNTAX_GENERIC
        , line
        , column
        , 1
        , "syntax"
        , format
        , args
        );
    va_end(args);
}

#endif
