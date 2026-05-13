#ifndef ERRHANDLER_H
#define ERRHANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

/*
 * Severity levels.
 *   ERROR_LEVEL_WARNING – non‑critical, compilation may continue.
 *   ERROR_LEVEL_ERROR   – critical, compilation should not succeed but more errors
 *                         may be collected.
 *   ERROR_LEVEL_FATAL   – immediate termination is required.
 */
typedef enum {
    ERROR_LEVEL_WARNING,
    ERROR_LEVEL_ERROR,
    ERROR_LEVEL_FATAL
} ErrorLevel;

/*
 * Report a diagnostic with full detail (variadic).
 *
 * level      : one of the ErrorLevel values.
 * error_code : 16‑bit hex code (see predefined codes below).
 * line       : 1‑based line number.
 * column     : 1‑based column (byte offset).
 * length     : length of the erroneous token in bytes.
 * context    : short subsystem tag (e.g. "syntax", "semantic").
 * format     : printf‑style message format.
 * ...        : format arguments.
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
 * Same as errhandler__report_error_ex but takes a va_list.
 * This is the real worker; the variadic version simply unpacks the va_list
 * and calls this one.
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
 * Store the current source filename.
 * The filename is copied internally, so the caller may free its own copy.
 * Pass NULL to clear the stored name.
 */
void errhandler__set_current_filename(const char* filename);

/*
 * Provide the text of the source file as an array of null‑terminated lines.
 * The manager does NOT take ownership; if the caller later frees the lines
 * diagnostics that were already emitted still hold their own copies of the
 * relevant source line.
 *
 * line_count must equal the number of elements in source_lines.
 */
void errhandler__set_source_code(const char** source_lines, uint16_t line_count);

/*
 * Discard the externally provided source lines.
 * If the manager itself owns the lines (from errhandler__load_source_file),
 * those are freed; otherwise only the pointer is forgotten.
 */
void errhandler__clear_source_code(void);

/*
 * Load a whole source file into memory, storing each line as an owned copy.
 * Returns 0 on success, -1 on failure (file not found, allocation failure, ...).
 */
int errhandler__load_source_file(const char* filename);

/*
 * Print every ERROR and FATAL diagnostic to stdout.
 * If warnings_as_errors is enabled, WARNING entries are also printed here
 * (but without the usual warning colour).
 */
void errhandler__print_errors(void);

/*
 * Print every WARNING diagnostic to stdout, unless they are suppressed or
 * are already shown as errors.
 */
void errhandler__print_warnings(void);

/*
 * Return true if at least one ERROR or FATAL entry exists.
 * When warnings_as_errors is on, the presence of any WARNING also makes this
 * function return true.
 */
bool errhandler__has_errors(void);

/*
 * Return true if any WARNING entry exists.
 */
bool errhandler__has_warnings(void);

/*
 * Release all memory owned by the manager (entries, copies, source lines)
 * and reset every piece of internal state to the initial defaults.
 */
void errhandler__free_error_manager(void);

/*
 * Get the current number of error‑level entries (ERROR + FATAL).
 * If warnings_as_errors is active, warnings are also counted.
 * The value is capped to UINT16_MAX.
 */
uint16_t errhandler__get_error_count(void);

/*
 * Get the current number of warning‑level entries, capped to UINT16_MAX.
 */
uint16_t errhandler__get_warning_count(void);

/*
 * Map an ErrorLevel value to a readable string: "WARNING", "ERROR", "FATAL".
 */
const char* errhandler__get_error_level_string(ErrorLevel level);

/*
 * Decompose a 4‑hex‑digit error code string (form TTGGNN) into its three
 * components.  Returns true if the string consists of exactly four hex digits.
 */
bool errhandler__parse_error_code
    ( const char* error_code_str
    , char type[2]
    , char group[2]
    , char number[3]
    );

/*
 * When enabled, WARNING entries are counted as errors and printed together
 * with them, suppressing the separate warning output.
 */
void errhandler__set_warnings_as_errors(bool enable);

/*
 * When enabled, no warning output is produced at all; the entries still
 * exist and are counted, but errhandler__print_warnings prints nothing.
 */
void errhandler__set_suppress_warnings(bool suppress);

#define ERROR_CODE_SYNTAX_GENERIC               0x7A00
#define ERROR_CODE_SYNTAX_UNEXPECTED_TOKEN      0x7A01
#define ERROR_CODE_SYNTAX_UNEXPECTED_EOF        0x7A02
#define ERROR_CODE_SYNTAX_INVALID_CHAR          0x7A03
#define ERROR_CODE_SYNTAX_MISSING_SEMICOLON     0x7A04
#define ERROR_CODE_SYNTAX_INVALID_STATEMENT     0x7A05
#define ERROR_CODE_SYNTAX_UNCLOSED_QUOTE        0x7A06
#define ERROR_CODE_SYNTAX_MISMATCHED_PAREN      0x7A07
#define ERROR_CODE_SYNTAX_EMPTY_PARENS          0x7A08

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

#define ERROR_CODE_PP_UNKNOW_DIR                0x4C00
#define ERROR_CODE_PP_DIR_TOO_LONG              0x4C01
#define ERROR_CODE_PP_MACRO_DEF_FAILED          0x4C02
#define ERROR_CODE_PP_INVALID_DIR               0x4C03
#define ERROR_CODE_PP_UNDEFINED                 0x4C04
#define ERROR_CODE_PP_MACRO_RECURSION           0x4C05
#define ERROR_CODE_PP_DUPLICATE_DIR             0x4C06
#define ERROR_CODE_PP_ERROR_DIR                 0x4C07

#define ERROR_CODE_IR_TYPE_MISMATCH             0xB100 
#define ERROR_CODE_IR_UNDEFINED_VAR             0xB101 
#define ERROR_CODE_IR_INVALID_INSTR             0xB102 
#define ERROR_CODE_IR_BUFFER_OVERFLOW           0xB103 
#define ERROR_CODE_IR_UNSUPPORTED_NODE          0xB104
#define ERROR_CODE_IR_MEMORY_ALLOCATION         0xB105
#define ERROR_CODE_IR_INVALID_ARGUMENT          0xB106

#define ERROR_CODE_COM_FAILCREATE               0xFF00

#define ERROR_CODE_MEMORY_ALLOCATION            0x6B00
#define ERROR_CODE_MEMORY_OVERFLOW              0x6B01
#define ERROR_CODE_MEMORY_INVALID_FREE          0x6B02

#define ERROR_CODE_OPTIM_MEMORY_ALLOCATION      0xD001
#define ERROR_CODE_OPTIM_INTERNAL_ERROR         0xD002
#define ERROR_CODE_OPTIM_INLINE_WRITE           0xD003 // new

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
 * Convenience wrapper: report an error of level ERROR with length = 1.
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
 * Convenience wrapper: report a warning with level WARNING, default
 * SYNTAX_GENERIC error code, and length = 1.
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
 * Prints all accumulated diagnostics to stderr, then releases the manager.
 */
static inline void errhandler__shutdown_on_fatal(void) {
    fprintf(stderr, "\033[31mFATAL ERROR\033[0m – compilation aborted.\n");
    errhandler__print_errors();
    errhandler__free_error_manager();
}

#endif
