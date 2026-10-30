#ifndef PAXSY_SEMA_INT_H
#define PAXSY_SEMA_INT_H

/*
 * sema_int.h - shared internals of the semantic analysis units.
 * The Sema state, the symbol table structures and every walker are
 * declared here so sema.c (rules and expression walking) and
 * sema_walk.c (declaration and statement walking) can cooperate.
 * These symbols are internal to the frontend and not exported.
 */

#include "sema.h"

typedef struct Sym { char *name; Node *decl; int is_type; struct Sym *next; } Sym;
typedef struct Scope { Sym *head; struct Scope *parent; } Scope;

typedef struct {
    Diag *diag;
    const char *path;
    Scope *cur;
    int fn_err;               /* enclosing function returns an error type */
    int loop_depth;
    char *modules[64];        /* known module names (std is implicit) */
    int nmodules;
    const char *labels[16];   /* active enclosing loop labels */
    int nlabels;
    /* Prototypes seen during registration; a later body resolves
     * them by name, the remainder are external symbols. */
    struct {
        char *name;
        Token tok;
        int defined;
    } protos[64];
    int nprotos;
} Sema;

/* Heap-owns a name; shared with the walkers that register package
 * and module names. */
char *sm_strdup(const char *s);

void sm_error(Sema *sm, const Token *t, const char *fmt, ...);
void sm_warn(Sema *sm, const Token *t, const char *fmt, ...);
Sym *sm_scope_find(Scope *s, const char *name);
void sm_scope_put(Sema *sm, const char *name, Node *decl, int is_type);
int sm_module_known(Sema *sm, const char *name);
int sm_type_resolves(Sema *sm, const char *name);
int sm_is_type_node(const Node *n);
void sm_walk_type(Sema *sm, Node *t);
void sm_walk_expr(Sema *sm, Node *n);
void sm_register_decl(Sema *sm, Node *d);
void sm_walk_block(Sema *sm, Node *b);
void sm_walk_decl(Sema *sm, Node *d);
void sm_walk_stmt(Sema *sm, Node *s);

#endif /* PAXSY_SEMA_INT_H */
