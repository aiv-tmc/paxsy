#include "common.h"
#include "cemit_int.h"

/*
 * cemit_mod.c - module-level C emission: aggregates, the channel
 * runtime, function signatures and bodies, the std::inout shims and
 * global variables.
 *
 * Aggregates become C struct definitions with positional field names
 * (f0, f1, ...), so the field index the typing pass resolved is the
 * only fact a reference needs. A forward pass declares every tag
 * first, which lets aggregates reference each other in any order.
 * Enums become one preprocessor constant per variant (spelled with
 * the plain mangled variant name, matching how GLOBAL_REF prints)
 * plus a long long typedef for the enum type itself.
 *
 * The channel runtime is a pthread ring buffer of 64-bit slots with
 * the try-variants the select loop polls on; px_ushr, px_panic and
 * the recv0 discard helper live here too, emitted only when channels
 * or their constructs are used.
 *
 * Function ABI: a function node's type is its return type (HTY_ERROR
 * for T!), the parameters live in its items with their own types, and
 * the body sits in child[3]. main is emitted as a static
 * px_user_main with a real C main wrapper around it: the wrapper maps
 * a Void! result's error code to the process exit status and returns
 * 0 for a plain Void program.
 */

/* One struct field declaration "<cty> f<i>;" (arrays keep bounds). */
static void cfield(CE *c, const HNode *fd) {
    if (fd->type && fd->type->kind == HTY_ARRAY) {
        cty(c, fd->type->u.array.elem);
        fprintf(c->out, " f%d[%ld]; ", fd->u.index, fd->type->u.array.count);
    } else {
        cty(c, fd->type);
        fprintf(c->out, " f%d; ", fd->u.index);
    }
}

void cmod_aggregates(CE *c) {
    /* 128-bit typedefs: arithmetic on them is not generated, but the
     * types must exist so declarations still compile. */
    fputs("typedef struct { unsigned long long lo, hi; } px_i128;\n"
          "typedef px_i128 px_u128;\n", c->out);
    /* forward tags for arbitrary reference order */
    for (int m = 0; m < c->nmods; m++) {
        const HNode *root = hir_root(c->mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            const HNode *d = root->items[i];
            char nm[96];
            if (d->kind != HK_STRUCT && d->kind != HK_UNION) continue;
            cname(d->name, nm, sizeof(nm));
            fprintf(c->out, "struct px_%s;\n", nm);
        }
    }
    for (int m = 0; m < c->nmods; m++) {
        const HNode *root = hir_root(c->mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            const HNode *d = root->items[i];
            char nm[96];
            if (d->kind == HK_STRUCT || d->kind == HK_UNION) {
                cname(d->name, nm, sizeof(nm));
                fprintf(c->out, "struct px_%s { ", nm);
                for (int k = 0; k < d->nitems; k++) cfield(c, d->items[k]);
                fputs("};\n", c->out);
            } else if (d->kind == HK_ENUM) {
                for (int k = 0; k < d->nitems; k++) {
                    char vn[96];
                    cname(d->items[k]->name, vn, sizeof(vn));
                    fprintf(c->out, "#define %s %lld\n", vn,
                            d->items[k]->u.ival);
                }
                cname(d->name, nm, sizeof(nm));
                fprintf(c->out, "typedef long long px_enum_%s;\n", nm);
            }
        }
    }
    fputs("\n", c->out);
}

/* The channel runtime, emitted only when some module uses channels.
 * Every slot is a 64-bit word, so any Paxsy value fits. */
void cmod_runtime(CE *c) {
    fputs("typedef struct px_chan {\n"
          "    pthread_mutex_t mu;\n"
          "    pthread_cond_t cv;\n"
          "    long long *slots;\n"
          "    long long cap, head, tail, n, closed;\n"
          "} px_chan;\n"
          "static px_chan *px_chan_new(long long cap) {\n"
          "    px_chan *c = calloc(1, sizeof *c);\n"
          "    pthread_mutex_init(&c->mu, 0);\n"
          "    pthread_cond_init(&c->cv, 0);\n"
          "    c->cap = cap > 0 ? cap : 1;\n"
          "    c->slots = malloc((size_t)c->cap * sizeof(long long));\n"
          "    return c;\n"
          "}\n"
          "static int px_chan_try_send(px_chan *c, long long v) {\n"
          "    pthread_mutex_lock(&c->mu);\n"
          "    int ok = c->n < c->cap && !c->closed;\n"
          "    if (ok) {\n"
          "        c->slots[c->tail] = v;\n"
          "        c->tail = (c->tail + 1) % c->cap;\n"
          "        c->n++;\n"
          "    }\n"
          "    pthread_mutex_unlock(&c->mu);\n"
          "    if (ok) pthread_cond_broadcast(&c->cv);\n"
          "    return ok;\n"
          "}\n"
          "static void px_chan_send(px_chan *c, long long v) {\n"
          "    while (!px_chan_try_send(c, v))\n"
          "        px_chan_pause();\n"
          "}\n"
          "static int px_chan_try_recv(px_chan *c, long long *out) {\n"
          "    pthread_mutex_lock(&c->mu);\n"
          "    int ok = c->n > 0;\n"
          "    if (ok) {\n"
          "        *out = c->slots[c->head];\n"
          "        c->head = (c->head + 1) % c->cap;\n"
          "        c->n--;\n"
          "    }\n"
          "    pthread_mutex_unlock(&c->mu);\n"
          "    if (ok) pthread_cond_broadcast(&c->cv);\n"
          "    return ok;\n"
          "}\n"
          "static void px_chan_recv(px_chan *c, long long *out) {\n"
          "    while (!px_chan_try_recv(c, out))\n"
          "        px_chan_pause();\n"
          "}\n"
          "static long long px_chan_recv0(px_chan *c) {\n"
          "    long long v;\n"
          "    px_chan_recv(c, &v);\n"
          "    return v;\n"
          "}\n"
          "static void px_chan_close(px_chan *c) {\n"
          "    pthread_mutex_lock(&c->mu);\n"
          "    c->closed = 1;\n"
          "    pthread_cond_broadcast(&c->cv);\n"
          "    pthread_mutex_unlock(&c->mu);\n"
          "}\n"
          "static void px_chan_pause(void) {\n"
          "    struct timespec ts = { 0, 1000000 };\n"
          "    nanosleep(&ts, 0);\n"
          "}\n"
          "static long long px_ushr(unsigned long long v, long long n) {\n"
          "    return (long long)(v >> (n & 63));\n"
          "}\n"
          "static void px_panic(long long code) {\n"
          "    fprintf(stderr, \"paxsy panic %lld\\n\", code);\n"
          "    abort();\n"
          "}\n\n", c->out);
}

/* "<ret> <mangled>(<params>)" without a body; static keeps the unit
 * self-contained. Error-returning functions use their result struct
 * as the C return type. */
/* Entry-point detection shared with the body pass: a top-level
 * `main` or a module-nested `main` (prefixed to "demo::main" by
 * lowering) drives the program. */
static int cis_main_name(const char *nm) {
    size_t n;
    if (!nm) return 0;
    if (strcmp(nm, "main") == 0) return 1;
    n = strlen(nm);
    return n > 6 && strcmp(nm + n - 6, "::main") == 0;
}

void cfunc_sig(CE *c, const HNode *f) {
    char nm[96], b[24];
    const HType *v = cfunc_res_value(f);
    fputs("static ", c->out);
    if (v) {
        char rn[64];
        cres_name(v, rn, sizeof(rn));
        fprintf(c->out, "struct %s ", rn);
    } else {
        cty(c, f->type);
        fputc(' ', c->out);
    }
    /* the entry point is decided by the function itself: the
     * prototype pass runs with no current function, so cur_func
     * cannot be consulted here */
    if (cis_main_name(f->name))
        snprintf(nm, sizeof(nm), "px_user_main");
    else
        cname(f->name, nm, sizeof(nm));
    fputs(nm, c->out);
    fputs("(", c->out);
    if (f->nitems > 0) {
        for (int i = 0; i < f->nitems; i++) {
            if (i) fputs(", ", c->out);
            cty(c, f->items[i]->type);
            cslot(b, 1, f->items[i]->u.index);
            fputc(' ', c->out);
            fputs(b, c->out);
        }
    } else {
        fputs("void", c->out);
    }
    fputs(")", c->out);
}

void cfunc_body(CE *c, const HNode *f) {
    const HNode *saved = c->cur_func;
    c->cur_func = f;
    cfunc_sig(c, f);
    fputs(" ", c->out);
    if (f->child[3]) cstmt(c, f->child[3], 0);
    else fputs(";\n", c->out);
    if (cis_main(c)) {
        const HType *v = cfunc_res_value(f);
        fputs("int main(void) {\n    ", c->out);
        if (f->nitems > 0)
            cerr_(c, f, "main with parameters is not supported");
        if (v) {
            char rn[64];
            cres_name(v, rn, sizeof(rn));
            fprintf(c->out, "struct %s px_r = px_user_main();\n    "
                            "return (int)px_r.err;\n}\n", rn);
        } else {
            fputs("px_user_main();\n    return 0;\n}\n", c->out);
        }
    }
    c->cur_func = saved;
}

void cmod_globals(CE *c) {
    for (int m = 0; m < c->nmods; m++) {
        const HNode *root = hir_root(c->mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            const HNode *g = root->items[i];
            char nm[96];
            if (g->kind != HK_GLOBAL) continue;
            cname(g->name, nm, sizeof(nm));
            fputs("static ", c->out);
            if (g->type && g->type->kind == HTY_ARRAY) {
                cty(c, g->type->u.array.elem);
                fprintf(c->out, " %s[%ld]", nm, g->type->u.array.count);
            } else {
                cty(c, g->type);
                fprintf(c->out, " %s", nm);
            }
            if (g->child[0]) {
                fputs(" = ", c->out);
                cexpr(c, g->child[0]);
            }
            fputs(";\n", c->out);
        }
    }
}
