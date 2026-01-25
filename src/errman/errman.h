#ifndef ERRHANDLER_H
#define ERRHANDLER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Error severity levels for classification
 */
typedef enum {
    ERROR_LEVEL_WARNING,    /* Non-critical issues that don't stop compilation */
    ERROR_LEVEL_ERROR,      /* Critical errors that prevent successful compilation */
    ERROR_LEVEL_FATAL       /* Severe errors that force immediate termination */
} ErrorLevel;

/* Extended error reporting with severity level, context and token length */
void errman__report_error_ex(ErrorLevel level, uint16_t line, uint8_t column, uint8_t length,
                    const char* context, const char* format, ...);

/* Error context management */
void errman__set_source_code(const char** source_lines, uint16_t line_count);
void errman__clear_source_code(void);

/* Error management functions */
void errman__print_errors(void);
void errman__print_warnings(void);
bool errman__has_errors(void);
bool errman__has_warnings(void);
void errman__free_error_manager(void);

/* Utility functions */
uint16_t errman__get_error_count(void);
uint16_t errman__get_warning_count(void);
const char* errman__get_error_level_string(ErrorLevel level);

/* Backward compatibility functions (default length = 1) */
void errman__report_error(uint16_t line, uint8_t column, const char* format, ...);
void errman__report_warning(uint16_t line, uint8_t column, const char* format, ...);

/* Error code table */
typedef enum {
    ERROR_UNKNOWN_TYPE = 0x4D53303631373352,  // "MS06173R"
    ERROR_INVALID_FILE_EXT = 0x30325238345437, // "02R84T7S"
    ERROR_NO_INPUT_FILE = 0x3031485338333945,  // "01HS839E"
    ERROR_SYNTAX = 0x53594E5450583031,         // "SYNTPX01"
    ERROR_MEMORY_ALLOC = 0x4D454D414C4C4F43,   // "MEMALLOC"
    ERROR_UNDEFINED_SYMBOL = 0x554E4453463031, // "UNDSF01"
    ERROR_TYPE_MISMATCH = 0x545950454D495353,  // "TYPEMISS"
    ERROR_INVALID_OPERATOR = 0x494E564F503032, // "INVOP02"
} ErrorCode;

#endif
