#include "common.h"
#include "sema_int.h"

/* node_new() builds the synthetic loop-variable and select-arm
 * declarations below; without this prototype the implicit int
 * return truncates the pointer and crashes the walker. */
#include "node.h"

/*
 * sema_walk.c - the declaration and statement walkers of the semantic
 * analysis. Statements are walked for control-flow structure (loop
 * depth for break/continue, the enclosing-function error flag around
 * try), and declarations register names into the current scope.
 * Blocks introduce one scope per nesting level; the optional catch
 * name lives in a scope of its own between the guarded expression and
 * the handler block, so it resolves inside the handler without
 * leaking outside.
 */

static void sm_walk_fn(Sema *sm, Node *fn) {
    int saved_err = sm->fn_err;
    sm->fn_err = fn->child[0] && fn->child[0]->kind == NK_TYPE_ERROR;
    /* Parameters live in a scope of their own enclosing the body, so
     * argument names resolve inside the body without leaking out.
     * A static parameter names a type, so it registers as one. */
    Scope *saved = sm->cur;
    sm->cur = malloc(sizeof(Scope));
    sm->cur->head = NULL;
    sm->cur->parent = saved;
    for (int i = 0; i < fn->item_count; i++) {
        Node *pr = fn->items[i];
        if (!(pr->flags & NF_STATIC) || !pr->name) continue;
        int duplicate = 0;
        for (Sym *entry = sm->cur->head; entry; entry = entry->next)
            if (strcmp(entry->name, pr->name) == 0) duplicate = 1;
        if (duplicate) {
            sm_error(sm, &pr->tok,
                     "duplicate static parameter '%s'", pr->name);
            continue;
        }
        for (Scope *outer = sm->cur->parent; outer; outer = outer->parent) {
            if (sm_scope_find(outer, pr->name)) {
                sm_error(sm, &pr->tok,
                         "static parameter '%s' conflicts with an existing declaration",
                         pr->name);
                break;
            }
        }
        sm_scope_put(sm, pr->name, pr, 1);
    }
    for (int i = 0; i < fn->item_count; i++) {
        Node *pr = fn->items[i];
        if (pr->child[0]) sm_walk_type(sm, pr->child[0]);
        if (!(pr->flags & NF_STATIC) && pr->name)
            sm_scope_put(sm, pr->name, pr, 0);
    }
    if (fn->child[0]) sm_walk_type(sm, fn->child[0]);
    if (fn->child[2]) sm_walk_expr(sm, fn->child[2]);   /* = expr body */
    if (fn->child[3]) sm_walk_block(sm, fn->child[3]);
    sm_scope_destroy(sm->cur);
    sm->cur = saved;
    sm->fn_err = saved_err;
}

static void sm_walk_let(Sema *sm, Node *d) {
    if (d->child[0]) sm_walk_type(sm, d->child[0]);
    if (d->child[1]) sm_walk_expr(sm, d->child[1]);
    /* The name binds after the initialiser is checked, so a
     * 'let x = x' reads the outer x; from here on the new
     * variable shadows it for the rest of the block. */
    if (d->name) sm_scope_put(sm, d->name, d, 0);
}

static void sm_walk_for(Sema *sm, Node *s) {
    Node *r = s->child[0];
    if (r->child[0]) sm_walk_expr(sm, r->child[0]);
    if (r->child[1]) sm_walk_expr(sm, r->child[1]);
    if (s->child[2]) sm_walk_type(sm, s->child[2]);
    if (s->name) {
        Node *vd = node_new(NK_LET, s->tok);
        vd->name = s->name;
        vd->child[0] = s->child[2];
        /* Synthetic variable: nothing registered it, so the walk
         * owns the binding. */
        sm_walk_let(sm, vd);
    }
    sm->loop_depth++;
    sm_walk_block(sm, s->child[3]);
    sm->loop_depth--;
}

void sm_walk_stmt(Sema *sm, Node *s) {
    switch (s->kind) {
    case NK_BLOCK:
        sm_walk_block(sm, s);
        break;
    case NK_IF:
        sm_walk_expr(sm, s->child[0]);
        sm_walk_block(sm, s->child[1]);
        if (s->child[2]) sm_walk_block(sm, s->child[2]);
        break;
    case NK_IF_EXISTS:
        if (s->child[0]) sm_walk_block(sm, s->child[0]);
        if (s->child[1]) sm_walk_block(sm, s->child[1]);
        break;
    case NK_WHILE:
        sm_walk_expr(sm, s->child[0]);
        sm->loop_depth++;
        sm_walk_block(sm, s->child[1]);
        sm->loop_depth--;
        break;
    case NK_DOWHILE:
        sm->loop_depth++;
        sm_walk_block(sm, s->child[0]);
        sm->loop_depth--;
        sm_walk_expr(sm, s->child[1]);
        break;
    case NK_FOR:
        sm_walk_for(sm, s);
        break;
    case NK_MATCH:
        sm_walk_expr(sm, s->child[0]);
        for (int i = 0; i < s->item_count; i++) {
            Node *c = s->items[i];
            for (int j = 0; j < c->item_count; j++)
                sm_walk_expr(sm, c->items[j]);
            sm_walk_block(sm, c->child[1]);
        }
        if (s->child[2]) sm_walk_block(sm, s->child[2]);
        break;
    case NK_SELECT:
        for (int i = 0; i < s->item_count; i++) {
            Node *a = s->items[i];
            if (a->name) {
                Node *vd = node_new(NK_LET, a->tok);
                vd->name = a->name;
                sm_walk_decl(sm, vd);
            }
            if (a->child[0]) sm_walk_expr(sm, a->child[0]);
            if (a->child[1]) sm_walk_expr(sm, a->child[1]);
            if (a->child[2]) sm_walk_expr(sm, a->child[2]);
            sm_walk_block(sm, a->child[3]);
        }
        if (s->child[3]) sm_walk_block(sm, s->child[3]);
        break;
    case NK_RETURN:
        if (s->child[0]) sm_walk_expr(sm, s->child[0]);
        break;
    case NK_DEFER:
        sm_walk_stmt(sm, s->child[0]);
        break;
    case NK_BREAK:
    case NK_CONTINUE:
        if (sm->loop_depth == 0)
            sm_error(sm, &s->tok, "'%s' outside of a loop",
                     s->kind == NK_BREAK ? "break" : "continue");
        break;
    case NK_ROUTINE:
    case NK_ASYNC:
        sm_walk_block(sm, s->child[0]);
        break;
    case NK_CLOSE:
        sm_walk_expr(sm, s->child[0]);
        break;
    case NK_VOLATILE:
        sm_walk_stmt(sm, s->child[0]);
        break;
    case NK_LET:
        /* Statement-level let: the statement walk owns the binding
         * (declaration-level lets are bound by registration). */
        sm_walk_let(sm, s);
        break;
    case NK_STATIC_CONST:
        /* Statement-level static constant: checked and bound like
         * a let (the statement walk owns the binding). */
        sm_walk_let(sm, s);
        break;
    case NK_LET_MULTI: {
        /* let (a, b, ...) = <expr>: the initialiser is checked
         * first, then every bound name enters the scope. */
        if (s->child[0]) sm_walk_expr(sm, s->child[0]);
        if (s->child[1]) sm_walk_type(sm, s->child[1]);
        for (int i = 0; i < s->item_count; i++) {
            Node *b = s->items[i];
            if (b->child[0]) sm_walk_type(sm, b->child[0]);
            if (b->child[1]) sm_walk_expr(sm, b->child[1]);
            if (b->name) sm_scope_put(sm, b->name, b, 0);
        }
        break;
    }
        break;
    case NK_PASS:
        break;
    case NK_ASM:
        /* The assembly text lives in the token; nothing to resolve. */
        break;
    case NK_EXPR_STMT:
        sm_walk_expr(sm, s->child[0]);
        break;
    case NK_USE:
        sm_import(sm, s);
        break;
    case NK_LABEL:
        if (sm->nlabels < 64) sm->labels[sm->nlabels++] = s->name;
        sm_walk_stmt(sm, s->child[0]);
        sm->nlabels--;
        break;
    default:
        sm_walk_decl(sm, s);
        break;
    }
}

void sm_walk_block(Sema *sm, Node *b) {
    Scope *saved = sm->cur;
    sm->cur = malloc(sizeof(Scope));
    sm->cur->head = NULL;
    sm->cur->parent = saved;
    for (int i = 0; i < b->item_count; i++)
        sm_walk_stmt(sm, b->items[i]);
    sm_scope_destroy(sm->cur);
    sm->cur = saved;
}

/* Walks a declaration after registration. Functions open their
 * parameter scope and walk the body; aggregates walk field types (and
 * explicit enum discriminant values); static constants walk their
 * defining expression or type alias; modules and static blocks
 * recurse into their members. Prototypes have no body to walk: only
 * their signature types are checked. Anything else is a statement
 * shape and defers to sm_walk_stmt. */
void sm_walk_decl(Sema *sm, Node *d) {
    if (!d) return;
    switch (d->kind) {
    case NK_FUNC:
        if (d->flags & NF_PROTOTYPE) {
            if (d->child[0]) sm_walk_type(sm, d->child[0]);
            for (int i = 0; i < d->item_count; i++)
                if (d->items[i]->child[0])
                    sm_walk_type(sm, d->items[i]->child[0]);
            return;
        }
        /* A body-ful function walks like any other function. */
        sm_walk_fn(sm, d);
        break;
    case NK_STATIC_FN:
        sm_walk_fn(sm, d);
        break;
    case NK_STRUCT:
    case NK_UNION:
        for (int i = 0; i < d->item_count; i++)
            if (d->items[i]->child[0])
                sm_walk_type(sm, d->items[i]->child[0]);
        break;
    case NK_ENUM:
        for (int i = 0; i < d->item_count; i++)
            if (d->items[i]->child[0])
                sm_walk_expr(sm, d->items[i]->child[0]);
        break;
    case NK_STATIC_CONST:
        if (d->child[0]) sm_walk_type(sm, d->child[0]);
        if (d->child[1]) sm_walk_expr(sm, d->child[1]);
        break;
    case NK_MODULE:
        if (d->name) {
            /* A named module forms a scope: every member is visible
             * to the others by its short name (the spec's module
             * examples use struct types and constants bare). The
             * registration pass already published the qualified
             * 'm::x' spellings; here we only add the bare ones. */
            Scope *saved = sm->cur;
            sm->cur = malloc(sizeof(Scope));
            sm->cur->head = NULL;
            sm->cur->parent = saved;
            for (int i = 0; i < d->item_count; i++) {
                Node *it = d->items[i];
                if (!it->name) continue;
                sm_scope_put(sm, it->name, it,
                             it->kind == NK_STRUCT || it->kind == NK_UNION ||
                             it->kind == NK_ENUM ||
                             (it->kind == NK_STATIC_CONST &&
                              it->child[1] && it->child[1]->kind == NK_NAME &&
                              sm_type_resolves(sm, it->child[1]->name)));
            }
            for (int i = 0; i < d->item_count; i++)
                sm_walk_decl(sm, d->items[i]);
            sm_scope_destroy(sm->cur);
            sm->cur = saved;
        } else {
            for (int i = 0; i < d->item_count; i++)
                sm_walk_decl(sm, d->items[i]);
        }
        break;
    case NK_STATIC_BLOCK:
    case NK_LET_MULTI:
        for (int i = 0; i < d->item_count; i++)
            sm_walk_decl(sm, d->items[i]);
        break;
    case NK_LET:
        /* The registration pass already bound this name; only the
         * type and the initialiser are walked here. Statement-level
         * lets (sm_walk_let) bind themselves, declarations do not. */
        if (d->child[0]) sm_walk_type(sm, d->child[0]);
        if (d->child[1]) sm_walk_expr(sm, d->child[1]);
        break;
    case NK_USE:
        sm_import(sm, d);
        break;
    default:
        sm_walk_stmt(sm, d);
        break;
    }
}
