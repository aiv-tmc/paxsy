#include "optimizer_private.h"

/* Definition of global flags. */
int opt_level = 1;
bool opt_debug = false;

/* Declarations of all pass functions (including new ones). */
extern bool pass_constant_folding(Program* ast);
extern bool pass_constant_propagation(Program* ast);
extern bool pass_dead_code_elimination(Program* ast);
extern bool pass_remove_unused_vars(Program* ast);
extern bool pass_interprocedural_const_prop(Program* ast);
extern bool pass_loop_invariant_hoisting(Program* ast);
extern bool pass_dead_assignment_elimination(Program* ast);

/* Level 0: none. */
static const OptimizationPass passes_level0[] = {
    { NULL, NULL }
};

/* Level 1: basic optimisations. */
static const OptimizationPass passes_level1[] = {
    pass_constant_folding,
    pass_dead_code_elimination,
    pass_remove_unused_vars
};

/* Level 2: full optimisations. */
static const OptimizationPass passes_level2[] = {
    pass_constant_folding,
    pass_constant_propagation,
    pass_dead_code_elimination,
    pass_remove_unused_vars,
    pass_interprocedural_const_prop,
    pass_loop_invariant_hoisting,
    pass_dead_assignment_elimination
};

/*
 * Return the list of passes for the current optimisation level.
 * Stores the number of passes in *count.
 */
const OptimizationPass* get_passes(int* count) {
    if (opt_level <= 0) {
        *count = 0;
        return passes_level0;
    }
    if (opt_level == 1) {
        *count = sizeof(passes_level1) / sizeof(passes_level1[0]);
        return passes_level1;
    }
    /* opt_level >= 2 */
    *count = sizeof(passes_level2) / sizeof(passes_level2[0]);
    return passes_level2;
}
