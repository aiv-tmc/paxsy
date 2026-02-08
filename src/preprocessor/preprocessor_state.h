#ifndef PREPROCESSOR_STATE_H
#define PREPROCESSOR_STATE_H

#include <stdint.h>
#include <stddef.h>

/**
 * Preprocessor state structure
 * Tracks the current state during preprocessing including
 * position, flags, and buffers.
 */
typedef struct {
    const char* input;               /* Input source code */
    char* output;                    /* Output buffer */
    size_t input_pos;                /* Current position in input */
    size_t output_pos;               /* Current position in output */
    size_t output_capacity;          /* Capacity of output buffer */
    uint16_t line;                   /* Current line number */
    uint16_t column;                 /* Current column number */
    
    /* State flags */
    uint8_t in_single_line_comment : 1;   /* Inside single-line comment */
    uint8_t in_multi_line_comment : 1;    /* Inside multi-line comment */
    uint8_t in_string : 1;                /* Inside string literal */
    uint8_t in_char : 1;                  /* Inside character literal */
    uint8_t in_preprocessor_directive : 1;/* Processing preprocessor directive */
    uint8_t in_config_macro : 1;          /* Inside configuration macro */
    uint8_t bracket_depth;                /* Depth of bracket nesting */
    
    /* Buffer for directive parsing */
    char directive_buffer[256];      /* Buffer for directive content */
    size_t directive_pos;            /* Position in directive buffer */
    uint16_t directive_start_line;   /* Line where directive started */
    uint8_t directive_start_column;  /* Column where directive started */
    
    /* Current file being processed (for #incfile) */
    const char* current_file;        /* Current file path */
} PreprocessorState;

#endif
