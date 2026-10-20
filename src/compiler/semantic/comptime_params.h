#ifndef COMPTIME_PARAMS_H
#define COMPTIME_PARAMS_H

#include "semantic.h"

/*
 * Specialize a call to a template function (a function declared with
 * comptime parameters). Each comptime parameter is bound to its
 * call-site argument — a type, or a compile-time constant — in a
 * fresh scope; ordinary arguments are type-checked against their
 * declared types with the bindings in place; the body is analyzed
 * with the bindings visible. Returns the call's result type, or NULL
 * after reporting an error.
 */
Type* specialize_template_call(struct SemanticContext* ctx, struct Symbol* tmpl,
                               struct CallExpr* call);

#endif
