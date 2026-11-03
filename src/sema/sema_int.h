#ifndef PAXSY_SEMA_INT_H
#define PAXSY_SEMA_INT_H

/*
 * sema_int.h - shared internals of the semantic analysis units.
 * The Sema state, the symbol table structures and every walker are
 * declared here so sema.c (rules and expression walking), sema_walk.c
 * (declaration and statement walking) and sema_mod.c (module
 * namespaces and imports) can cooperate. These symbols are internal
 * to the frontend and not exported. Registration tables grow
 * dynamically: module names, module member lists and function
 * prototypes arrive in unbounded numbers, so all are heap-allocated
 * with on-demand doubling instead of fixed arrays that would silently
 * drop entries past their bound.
 */

#include "sema.h"

typedef struct Sym { char *name; Node *decl; int is_type; struct Sym *next; } Sym;
typedef struct Scope { Sym *head; struct Scope *parent; } Scope;

/* One known module namespace. `local` marks modules declared in the
 * program (their member lists are known and validated); namespaces
 * from installed packages and `std` are external, their members are
 * opaque and any qualified path is accepted. */
typedef struct {
    char *name;
    int local;
    char **members;
    int nmembers, members_cap;
} ModSym;

typedef struct {
    Diag *diag;
    const char *path;
    Scope *cur;
    int fn_err;               /* enclosing function returns an error type */
    int loop_depth;
    ModSym *mods;             /* known module namespaces, std included */
    int nmods, mods_cap;
    const char *labels[64];   /* active enclosing loop labels */
    int nlabels;
    /* Prototypes seen during registration; a later body resolves
     * them by name, the remainder are external symbols. */
    struct {
        char *name;
        Token tok;
        int defined;
    } *protos;
    int nprotos, protos_cap;
} Sema;

/* Heap-owns a name; shared with the walkers that register package
 * and module names. */
char *sm_strdup(const char *s);

void sm_error(Sema *sm, const Token *t, const char *fmt, ...);
void sm_warn(Sema *sm, const Token *t, const char *fmt, ...);
Sym *sm_scope_find(Scope *s, const char *name);
void sm_scope_put(Sema *sm, const char *name, Node *decl, int is_type);
void sm_proto_add(Sema *sm, char *name, Token tok);

/* Module namespaces (sema_mod.c). sm_module_add is find-or-create:
 * re-declaring a namespace merges the local flag and returns the
 * existing entry. */
ModSym *sm_module_add(Sema *sm, const char *name, int local);
ModSym *sm_module_find(Sema *sm, const char *name);
int sm_module_known(Sema *sm, const char *name);

/* Registers a declaration under `prefix`-qualified names ("m::") into
 * the current scope; used for the members of named modules so that
 * `m::f` resolves from anywhere in the program. */
void sm_member_record(ModSym *m, const char *name);
void sm_register_qualified(Sema *sm, const char *prefix, Node *d);

/* Applies a use directive to the current scope: whole-module imports
 * alias every member to its bare name, single-symbol imports alias
 * one member, and external packages alias the bare name of a
 * qualified import so the reference passes resolution. */
void sm_import(Sema *sm, Node *use);

int sm_type_resolves(Sema *sm, const char *name);
int sm_is_type_node(const Node *n);
void sm_walk_type(Sema *sm, Node *t);
void sm_walk_expr(Sema *sm, Node *n);
void sm_register_decl(Sema *sm, Node *d);
void sm_walk_block(Sema *sm, Node *b);
void sm_walk_decl(Sema *sm, Node *d);
void sm_walk_stmt(Sema *sm, Node *s);

#endif /* PAXSY_SEMA_INT_H */
