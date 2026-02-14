#ifndef DIRECTIVE_CONDITIONAL_H
#define DIRECTIVE_CONDITIONAL_H

#include "../../preprocessor_state.h"

/**
 * Conditional compilation frame.
 * Represents one level of #if / #ifdef / #ifndef ... #endif.
 */
typedef struct ConditionalFrame {
    int parent_skip;      /* whether the enclosing block is skipped */
    int skip;             /* whether this block should skip output */
    int taken;            /* whether any branch has been taken (true branch encountered) */
    int else_seen;        /* whether #else has already appeared in this group */
} ConditionalFrame;

/**
 * Context holding the conditional stack.
 */
typedef struct ConditionalContext {
    ConditionalFrame* stack;
    size_t capacity;
    size_t count;
} ConditionalContext;

/* Lifecycle */
ConditionalContext* conditional_context_create(void);
void conditional_context_destroy(ConditionalContext* ctx);

/**
 * Query: should the current character be emitted?
 * Called before every output operation.
 */
int conditional_should_output(PreprocessorState* state);

/* Preprocessor directive handlers */
void DPPF__if(PreprocessorState* state, const char* args);
void DPPF__ifdef(PreprocessorState* state, const char* args);
void DPPF__ifndef(PreprocessorState* state, const char* args);
void DPPF__elif(PreprocessorState* state, const char* args);
void DPPF__else(PreprocessorState* state, const char* args);
void DPPF__endif(PreprocessorState* state, const char* args);

#endif
