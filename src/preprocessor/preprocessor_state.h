#ifndef PREPROCESSOR_STATE_H
#define PREPROCESSOR_STATE_H

#include <stdint.h>
#include <stddef.h>
#include "directive/define/macro.h"

#define PREPROC_MAX_COND_DEPTH 64

struct ConditionalContext;
typedef struct ConditionalContext ConditionalContext;

typedef struct PreprocessorState {
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
    uint8_t in_macro_expansion : 1;
    uint8_t bracket_depth;

    char directive_buffer[256];
    size_t directive_pos;
    uint16_t directive_start_line;
    uint8_t directive_start_column;

    MacroTable* macro_table;
    const char* current_file;

    char identifier_buffer[256];
    size_t identifier_pos;

    char expansion_buffer[1024];
    size_t expansion_pos;

    uint8_t cond_depth;
    uint8_t cond_false_count;
    uint8_t cond_taken[PREPROC_MAX_COND_DEPTH];
    
    ConditionalContext* conditional_ctx;
} PreprocessorState;

#endif
