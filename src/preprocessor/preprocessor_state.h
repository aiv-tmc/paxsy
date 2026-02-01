#ifndef PREPROCESSOR_STATE_H
#define PREPROCESSOR_STATE_H

#include <stdint.h>
#include <stddef.h>

/* Preprocessor state structure */
typedef struct {
    const char* input;
    char* output;
    size_t input_pos;
    size_t output_pos;
    size_t output_capacity;
    uint16_t line;
    uint16_t column;
    uint8_t in_single_line_comment : 1;
    uint8_t in_multi_line_comment : 1;
    uint8_t in_string : 1;
    uint8_t in_char : 1;
    uint8_t in_preprocessor_directive : 1;
    uint8_t in_config_macro : 1;
    uint8_t bracket_depth;
    
    /* Buffer for directive parsing */
    char directive_buffer[256];
    size_t directive_pos;
    uint16_t directive_start_line;
    uint8_t directive_start_column;
} PreprocessorState;

#endif
