#include "common.h"
#include "sema_int.h"

/*
 * sema_walk.c - the structural walkers: blocks (register-then-walk so
 * declaration order does not matter), functions (parameter scope, error
 * return flag, label stack reset), modules (private child scope),
 * declarations and statements (loops and labels, select arms, use
 * directives).
 */

static void walk_func(Sema *sm, Node *fn) {
    int saved_err = sm->fn_err, saved_labels = sm->nlabels;
    sm->fn_err = fn->child[0] && fn->child[0]->kind == NK_TYPE_ERROR;
    sm->nlabels = 0;
    if (!(fn->flags & NF_PROTOTYPE)) {
        Scope *saved = sm->cur;
        sm->cur = malloc(sizeof(Scope));
        sm->cur->head = NULL;
        sm->cur->parent = saved;
        for (int i = 0; i < fn->item_count; i++) {
            sm_walk_type(sm, fn->items[i]->child[0]);
            sm_scope_put(sm, fn->items[i]->name, fn->items[i], 0);
        }
        sm_walk_block(sm, fn->child[3]);
        free(sm->cur);
        sm->cur = saved;
    }
    sm->fn_err = saved_err;
    sm->nlabels = saved_labels;
}

void sm_walk_block(Sema *sm, Node *b) {
    if (!b) return;
    Scope *saved = sm->cur;
    sm->cur = malloc(sizeof(Scope));
    sm->cur->head = NULL;
    sm->cur->parent = saved;
    for (int i = 0; i < b->item_count; i++)
        sm_register_decl(sm, b->items[i]);
    for (int i = 0; i < b->item_count; i++)
        sm_walk_decl(sm, b->items[i]);
    free(sm->cur);
    sm->cur = saved;
}

static void walk_module(Sema *sm, Node *mod) {
    Scope *saved = sm->cur;
    sm->cur = malloc(sizeof(Scope));
    sm->cur->head = NULL;
    sm->cur->parent = saved;
    for (int i = 0; i < mod->item_count; i++)
        sm_register_decl(sm, mod->items[i]);
    for (int i = 0; i < mod->item_count; i++)
        sm_walk_decl(sm, mod->items[i]);
    free(sm->cur);
    sm->cur = saved;
}

void sm_walk_decl(Sema *sm, Node *d) {
    if (!d) return;
    switch (d->kind) {
    case NK_FUNC:     walk_func(sm, d); return;
    case NK_MODULE:   walk_module(sm, d); return;
    case NK_STRUCT: case NK_UNION:
        for (int i = 0; i < d->item_count; i++)
            sm_walk_type(sm, d->items[i]->child[0]);
        return;
    case NK_ENUM:
        for (int i = 0; i < d->item_count; i++)
            sm_walk_expr(sm, d->items[i]->child[0]);
        return;
    case NK_COMPTIME_FN:
        walk_func(sm, d);
        return;
    case NK_COMPTIME_CONST:
        /* Type alias when the right side is a bare name. */
        if (d->child[0] && d->child[0]->kind == NK_NAME) {
            if (!sm_type_resolves(sm, d->child[0]->name))
                sm_error(sm, &d->child[0]->tok, "unknown type '%s'",
                         d->child[0]->name);
        } else sm_walk_expr(sm, d->child[0]);
        return;
    case NK_COMPTIME_BLOCK:
        for (int i = 0; i < d->item_count; i++)
            sm_walk_decl(sm, d->items[i]);
        return;
    case NK_LET:
        sm_walk_type(sm, d->child[0]);
        sm_walk_expr(sm, d->child[1]);
        return;
    case NK_LET_MULTI:
        sm_walk_expr(sm, d->child[0]);
        for (int i = 0; i < d->item_count; i++)
            sm_walk_decl(sm, d->items[i]);
        return;
    case NK_USE: {
        char first[64];
        const char *end = strstr(d->name, "::");
        size_t len = end ? (size_t)(end - d->name) : strlen(d->name);
        if (len >= sizeof(first)) len = sizeof(first) - 1;
        memcpy(first, d->name, len);
        first[len] = '\0';
        if (!sm_module_known(sm, first))
            sm_warn(sm, &d->tok, "unknown module '%s' in use directive", first);
        return;
    }
    default:
        sm_walk_stmt(sm, d);
        return;
    }
}

void sm_walk_stmt(Sema *sm, Node *s) {
    if (!s) return;
    switch (s->kind) {
    case NK_LET: case NK_LET_MULTI:
        sm_walk_decl(sm, s);
        return;
    case NK_BLOCK: case NK_ROUTINE:
        sm_walk_block(sm, s->kind == NK_BLOCK ? s : s->child[0]);
        return;
    case NK_IF:
        sm_walk_expr(sm, s->child[0]);
        sm_walk_block(sm, s->child[1]);
        if (s->child[2])
            s->child[2]->kind == NK_IF ? sm_walk_stmt(sm, s->child[2])
                                       : sm_walk_block(sm, s->child[2]);
        return;
    case NK_IF_EXISTS:
        sm_walk_block(sm, s->child[0]);
        if (s->child[1]) sm_walk_block(sm, s->child[1]);
        return;
    case NK_MATCH:
        sm_walk_expr(sm, s->child[0]);
        for (int i = 0; i < s->item_count; i++) {
            Node *cse = s->items[i];
            for (int j = 0; j < cse->item_count; j++)
                sm_walk_expr(sm, cse->items[j]);
            sm_walk_block(sm, cse->child[1]);
        }
        return;
    case NK_WHILE: case NK_DOWHILE: {
        int d = s->kind == NK_DOWHILE;
        sm_walk_expr(sm, s->child[d ? 1 : 0]);
        sm->loop_depth++;
        sm_walk_block(sm, s->child[d ? 0 : 1]);
        sm->loop_depth--;
        return;
    }
    case NK_FOR: {
        Node *r = s->child[0];
        sm_walk_expr(sm, r->child[0]);
        sm_walk_expr(sm, r->child[1]);
        Scope *saved = sm->cur;
        sm->cur = malloc(sizeof(Scope));
        sm->cur->head = NULL;
        sm->cur->parent = saved;
        if (s->name) sm_scope_put(sm, s->name, s, 0);
        sm_walk_type(sm, s->child[2]);
        sm->loop_depth++;
        sm_walk_block(sm, s->child[3]);
        sm->loop_depth--;
        free(sm->cur);
        sm->cur = saved;
        return;
    }
    case NK_LABEL:
        if (sm->nlabels < 16) sm->labels[sm->nlabels++] = s->name;
        sm_walk_stmt(sm, s->child[0]);
        sm->nlabels--;
        return;
    case NK_BREAK: case NK_CONTINUE:
        if (s->name) {
            int i;
            for (i = sm->nlabels - 1; i >= 0; i--)
                if (strcmp(sm->labels[i], s->name) == 0) break;
            if (i < 0)
                sm_error(sm, &s->tok, "unknown loop label '%s'", s->name);
        } else if (sm->loop_depth == 0) {
            sm_error(sm, &s->tok, "'%s' outside of any loop",
                     s->kind == NK_BREAK ? "break" : "continue");
        }
        return;
    case NK_DEFER: case NK_VOLATILE: case NK_EXPR_STMT: case NK_RETURN:
    case NK_CLOSE: case NK_PASS: case NK_ASM:
        for (int i = 0; i < 4; i++) sm_walk_expr(sm, s->child[i]);
        return;
    case NK_SELECT:
        for (int i = 0; i < s->item_count; i++) {
            Node *arm = s->items[i];
            sm_walk_expr(sm, arm->child[0]);
            sm_walk_expr(sm, arm->child[1]);
            sm_walk_expr(sm, arm->child[2]);
            sm_walk_block(sm, arm->child[3]);
        }
        sm_walk_block(sm, s->child[3]);
        return;
    default:
        sm_walk_decl(sm, s);
        return;
    }
}
