#ifndef ERRHANDLER_H
#define ERRHANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

/*
 * Error severity levels.
 * WARNING : non‑critical issue, compilation may continue.
 * ERROR   : critical issue, compilation cannot succeed but may continue to find more errors.
 * FATAL   : severe error, immediate termination required.
 */
typedef enum {
    ERROR_LEVEL_WARNING,
    ERROR_LEVEL_ERROR,
    ERROR_LEVEL_FATAL
} ErrorLevel;

/*
 * Primary diagnostic entry point (variadic).
 * level       - severity level
 * error_code  - 16‑bit hex error code
 * line        - 1‑based line number
 * column      - 1‑based column (byte offset)
 * length      - length of the erroneous token in bytes
 * context     - subsystem identifier (e.g., "syntax", "semantic")
 * format      - printf‑style format string
 * ...         - format arguments
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

/*
 * Same as errhandler__report_error_ex, but accepts a va_list instead of variadic
 * arguments. This is the actual worker function; both the variadic version and
 * the simplified wrappers call this one.
 */
void errhandler__report_error_ex_va
    ( ErrorLevel level
    , uint16_t error_code
    , uint16_t line
    , uint8_t column
    , uint8_t length
    , const char* context
    , const char* format
    , va_list args
    );

/*
 * Set the current source filename (copied internally).
 * Pass NULL to clear.
 */
void errhandler__set_current_filename(const char* filename);

/*
 * Provide source lines for contextual display.
 * The manager does not take ownership unless copy_source_lines is enabled.
 */
void errhandler__set_source_code(const char** source_lines, uint16_t line_count);

/*
 * Clear any stored source lines.
 */
void errhandler__clear_source_code(void);

/*
 * Control whether source lines are copied or merely referenced.
 * enable = true  -> copy lines (default, safe)
 * enable = false -> store pointer only (caller must keep lines alive)
 */
void errhandler__set_copy_source(bool enable);

/*
 * Load a source file from disk, store lines as copies.
 * Returns 0 on success, -1 on failure.
 */
int errhandler__load_source_file(const char* filename);

/*
 * Print all ERROR and FATAL entries to stdout.
 * WARNING entries are printed with error formatting if warnings_as_errors is set.
 */
void errhandler__print_errors(void);

/*
 * Print all WARNING entries to stdout, unless suppressed or treated as errors.
 */
void errhandler__print_warnings(void);

/*
 * True if any ERROR or FATAL entry exists (or WARNING when warnings_as_errors is on).
 */
bool errhandler__has_errors(void);

/*
 * True if any WARNING entry exists.
 */
bool errhandler__has_warnings(void);

/*
 * Free all memory and reset state.
 */
void errhandler__free_error_manager(void);

/*
 * Return number of error entries (ERROR + FATAL), capped to UINT16_MAX.
 * Warnings are included if warnings_as_errors is enabled.
 */
uint16_t errhandler__get_error_count(void);

/*
 * Return number of warning entries, capped to UINT16_MAX.
 */
uint16_t errhandler__get_warning_count(void);

/*
 * Convert an ErrorLevel to a human‑readable string.
 */
const char* errhandler__get_error_level_string(ErrorLevel level);

/*
 * Decompose a 4‑hex‑digit error code string (TTGGNN).
 * Returns true on success, false if input is not exactly 4 hex digits.
 */
bool errhandler__parse_error_code
    ( const char* error_code_str
    , char type[2]
    , char group[2]
    , char number[3]
    );

/*
 * Treat warnings as errors for counting and printing.
 */
void errhandler__set_warnings_as_errors(bool enable);

/*
 * Suppress all warning output.
 */
void errhandler__set_suppress_warnings(bool suppress);

/* Predefined error codes. */
#define ERROR_CODE_SYNTAX_GENERIC               0x7A00
#define ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN      0x7A01
#define ERROR_CODE_SYNTAX_UNEXPECTED_EOF        0x7A02
#define ERROR_CODE_SYNTAX_INVALID_CHAR          0x7A03
#define ERROR_CODE_SYNTAX_MISSING_SEMICOLON     0x7A04
#define ERROR_CODE_SYNTAX_INVALID_STATEMENT     0x7A05
#define ERROR_CODE_SYNTAX_UNCLOSED_QUOTE        0x7A06
#define ERROR_CODE_SYNTAX_MISMATCHED_PAREN      0x7A07

#define ERROR_CODE_LEXER_INVALID_NUMBER         0xE000
#define ERROR_CODE_LEXER_INVALID_ESCAPE         0xE001
#define ERROR_CODE_LEXER_UNCLOSED_STRING        0xE002
#define ERROR_CODE_LEXER_UNKNOWN_CHAR           0xE003

#define ERROR_CODE_SEM_MISMATCH                 0xA400
#define ERROR_CODE_SEM_INVALID_CAST             0xA401
#define ERROR_CODE_SEM_UNDEFINED_VAR            0xA402
#define ERROR_CODE_SEM_INVALID_OPERATION        0xA403
#define ERROR_CODE_SEM_REDECLARATION            0xA404
#define ERROR_CODE_SEM_UNDECLARED_SYMBOL        0xA405
#define ERROR_CODE_SEM_UNINITIALIZED            0xA406
#define ERROR_CODE_SEM_ASSIGN_TO_CONST          0xA407
#define ERROR_CODE_SEM_TYPE_ERROR               0xA408
#define ERROR_CODE_SEM_UNUSED_VARIABLE          0xA409
#define ERROR_CODE_SEM_MISSING_RETURN           0xA40A
#define ERROR_CODE_SEM_TYPE_SWITCH_CONST        0xA40B
#define ERROR_CODE_SEM_TYPE_SWITCH_NON_VAR      0xA40C
#define ERROR_CODE_SEM_DEF_VAR_MISSING_TYPE     0xA40D
#define ERROR_CODE_SEM_DEF_VAR_MISSING_INIT     0xA40E
#define ERROR_CODE_SEM_PRO_VAR_HAS_INIT         0xA40F
#define ERROR_CODE_SEM_PRO_VAR_MISSING_TYPE     0xA410
#define ERROR_CODE_SEM_DEL_INVALID_TARGET       0xA411
#define ERROR_CODE_SEM_PRO_FUNC_HAS_BODY        0xA412
#define ERROR_CODE_SEM_DEF_FUNC_MISSING_BODY    0xA413
#define ERROR_CODE_SEM_DEF_FUNC_NONE_BODY       0xA414
#define ERROR_CODE_SEM_PROTO_PARAM_DEFAULT      0xA415
#define ERROR_CODE_SEM_RETURN_TYPE_MISMATCH     0xA416
#define ERROR_CODE_SEM_FUNC_DUPLICATE_BODY      0xA417
#define ERROR_CODE_SEM_FUNC_DIFFERENT_KIND      0xA418
#define ERROR_CODE_SEM_RETURN_VOID_VALUE        0xA419
#define ERROR_CODE_SEM_CAST_CONST               0xA41A

#define ERROR_CODE_PP_UNKNOW_DIR                0x4C00
#define ERROR_CODE_PP_DIR_TOO_LONG              0x4C01
#define ERROR_CODE_PP_MACRO_DEF_FAILED          0x4C02
#define ERROR_CODE_PP_INVALID_DIR               0x4C03
#define ERROR_CODE_PP_UNDEFINED                 0x4C04
#define ERROR_CODE_PP_MACRO_RECURSION           0x4C05
#define ERROR_CODE_PP_DUPLICATE_DIR             0x4C06
#define ERROR_CODE_PP_ERROR_DIR                 0x4C07

#define ERROR_CODE_COM_FAILCREATE               0xFF00

#define ERROR_CODE_MEMORY_ALLOCATION            0x6B00
#define ERROR_CODE_MEMORY_OVERFLOW              0x6B01
#define ERROR_CODE_MEMORY_INVALID_FREE          0x6B02

#define ERROR_CODE_RUNTIME_DIV_BY_ZERO          0x2300
#define ERROR_CODE_RUNTIME_OUT_OF_BOUNDS        0x2301
#define ERROR_CODE_RUNTIME_OVERFLOW             0x2302

#define ERROR_CODE_IO_FILE_NOT_FOUND            0x8200
#define ERROR_CODE_IO_DOUBLE_FILE               0x8201
#define ERROR_CODE_IO_PERMISSION_DENIED         0x8202
#define ERROR_CODE_IO_READ                      0x8203
#define ERROR_CODE_IO_WRITE                     0x8204

#define ERROR_CODE_INPUT_MULTI_MOD_FLAGS        0x8900
#define ERROR_CODE_INPUT_INVALID_FLAG           0x8901
#define ERROR_CODE_INPUT_NO_SOURCE              0x8902

/*
 * Simplified error reporter (level = ERROR, length = 1).
 * Delegates to errhandler__report_error_ex_va.
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
    errhandler__report_error_ex_va(ERROR_LEVEL_ERROR, error_code, line, column, 1,
                                   context, format, args);
    va_end(args);
}

/*
 * Simplified warning reporter (level = WARNING, code = SYNTAX_GENERIC, length = 1).
 * Delegates to errhandler__report_error_ex_va.
 */
static inline void errhandler__report_warning
    ( uint16_t line
    , uint8_t column
    , const char* format
    , ...
    ) {
    va_list args;
    va_start(args, format);
    errhandler__report_error_ex_va(ERROR_LEVEL_WARNING, ERROR_CODE_SYNTAX_GENERIC,
                                   line, column, 1, "syntax", format, args);
    va_end(args);
}

/*
 * Perform a graceful shutdown after a fatal error.
 * Prints all accumulated diagnostics to stderr, then frees the error manager.
 */
static inline void errhandler__shutdown_on_fatal(void) {
    fprintf(stderr, "\033[31mFATAL ERROR\033[0m – compilation aborted.\n");
    errhandler__print_errors();
    errhandler__free_error_manager();
}

#endif
