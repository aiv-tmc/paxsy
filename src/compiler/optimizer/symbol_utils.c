#include "symbol_utils.h"
#include "../semantic/semantic.h"

/*
 * Look up a symbol by name in the given semantic context.
 * Returns the Symbol if found, or NULL.
 */
Symbol* u__lookup(SemanticContext* ctx, const char* name) {
    if (!ctx || !name) return NULL;
    return scope_lookup(ctx->current_scope, name);
}
