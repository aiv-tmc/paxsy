#ifndef PAXSY_LOWER_INT_H
#define PAXSY_LOWER_INT_H

/*
 * lower_int.h - shared internals of the lowering pass: the Lower
 * context (module, diagnostics, scope stack, per-function local
 * counter, module prefix and import-alias state) and the mutually
 * recursive lowering routines. lower.h supplies the AST (Node) and
 * diagnostics (Diag) types this header builds on.
 */

#include "lower.h"

typedef struct { const char *name; int index; int is_param; } LSlot;
typedef struct { LSlot *v; int n, cap; } LScope;

typedef struct {
    HModule *mod;
    Diag *diag;
    LScope scopes[32];
    int depth;
    int next_local;
    /* Module prefix: empty at top level, "m::" inside module m,
     * "a::b::" inside nested module b. */
    char prefix[256];
    /* Bare names of the current module's members; used to decide
     * whether a global reference needs prefixing. */
    char **members;
    int nmembers, members_cap;
    /* Import alias stack (use directives), scoped by module marks:
     * entering a named module pushes a mark, leaving pops it. */
    struct { char *alias; char *target; } *aliases;
    int n_aliases, a_cap;
    int a_marks[32];
    int a_depth;
    /* Module member registry used to expand whole-module imports;
     * entries pair a module's qualified name with one member name. */
    struct { char *mod; char *member; } *registry;
    int nreg, reg_cap;
} Lower;

/* Available integer widths, shared with the type-name mapping. */
extern const int lwidths[6];

HNode *lnode(Lower *l, HNodeKind kind, Token tok);
void lflags(HNode *n);
void lerr(Lower *l, const Token *t, const char *what);
long long lint_value(const char *s, int len);

void lscope_push(Lower *l);
void lscope_pop(Lower *l);
void lscope_put(Lower *l, const char *name, int index, int is_param);
int lscope_find(Lower *l, const char *name, int *is_param);

/* Returns the fully-qualified name when `name` is a bare member of
 * the module currently being lowered; returns `name` unchanged
 * otherwise (qualified names, non-members, top level). */
const char *lower_resolve_name(Lower *l, const char *name);

/* Import support (lower_use.c). The registry is filled by a pre-pass
 * in lower_run so that a use directive resolves no matter where its
 * module is declared. */
void luse_init(Lower *l);
void luse_push(Lower *l);
void luse_pop(Lower *l);
void luse_apply(Lower *l, const Node *use);
void luse_registry_build(Lower *l, const Node *program);

/* Resolves a bare global name end to end: an import alias wins, then
 * the current module's member prefix, then the name stays bare. */
const char *lglobal_name(Lower *l, const char *name);

/* Operator tables shared with the expression walker. */
HBinOp l_binop_map(int op);
int l_compound_base(int op);
int l_is_compound_assign(int op);

const HType *lower_type(Lower *l, const Node *t);
HNode *lower_string(Lower *l, const Node *n);
HNode *lower_expr(Lower *l, const Node *n);
HNode *lower_stmt(Lower *l, const Node *n);
HNode *lower_block(Lower *l, const Node *b);
void lower_decl(Lower *l, const Node *d);

#endif /* PAXSY_LOWER_INT_H */
