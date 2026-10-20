#ifndef OPTIMIZER_PASSES_H
#define OPTIMIZER_PASSES_H

#include "../parser/parser.h"

/* Constant folding pass – evaluates constant expressions at compile time */
bool pass_constant_folding(Program* ast);

/* Dead code elimination – removes code that can never be executed */
bool pass_dead_code_elimination(Program* ast);

/* Constant propagation – replaces variables with known constant values */
bool pass_constant_propagation(Program* ast);

/* Remove unused local variables – deletes declarations of variables never read */
bool pass_remove_unused_vars(Program* ast);

/* Interprocedural constant propagation – inlines pure functions with constant args */
bool pass_interprocedural_const_prop(Program* ast);

/* Loop invariant hoisting – moves loop-invariant expressions out of loops */
bool pass_loop_invariant_hoisting(Program* ast);

/* Dead assignment elimination – removes assignments to variables that are never used */
bool pass_dead_assignment_elimination(Program* ast);

#endif
