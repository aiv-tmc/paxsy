#include "common.h"
#include "cemit_int.h"

/*
 * cemit.c - the C99 emission driver and shared helpers.
 *
 * cemit_modules() prints one self-contained C99 translation unit:
 *   1. the preamble (stdint/stdio/stdlib/string, plus pthread.h and
 *      time.h when channels are used),
 *   2. the string constant table: every CONST_STRING/CONST_CHAR node
 *      receives an aux id in collection order and becomes one static
 *      char array. String aux ids are reset before collection so the
 *      driver stays idempotent when the driver runs emission more
 *      than once against the same HIR,
 *   3. the T! result structs of every function that returns an
 *      error-typed value (layout { long long err; T value; }; names
 *      match the LLVM backend's px_res_<T> family),
 *   4. the std::inout::println shims for exactly the variants the
 *      program calls,
 *   5. the channel runtime when some module uses channels,
 *   6. aggregate definitions, prototypes, function bodies and
 *      finally module-level variables.
 * Error accounting: cerr_() reports into the shared diagnostic
 * stream and the driver measures the error-count delta, so the caller
 * can refuse to compile broken output.
 *
 * Name mangling: Paxsy names may contain ':' and '.', which C does
 * not allow, so cname() maps every other character to '_' and adds a
 * leading underscore when the first character is a digit. Every
 * emitter shares this routine, so a reference and its declaration
 * always mangle identically.
 *
 * Function ABI: a HK_FUNC node's type is its return type (HTY_ERROR
 * for T!), the parameters live in its items with their own types, and
 * the body sits in child[3] - the layout the LLVM backend consumes.
 */

void cerr_(CE *c, const HNode *n, const char *what) {
    int line = n ? n->tok.line : 0;
    int col = n ? n->tok.col : 0;
    diag_report(c->diag, DK_ERROR, "<cemit>", line, col, "C backend: %s",
                what);
}

void cindent(CE *c, int depth) {
    for (int i = 0; i < depth; i++) fputs("    ", c->out);
}

void slotname(int slot, char *buf, size_t n) {
    snprintf(buf, n, "loc%d", slot);
}

void cslot(char *buf, int is_param, int index) {
    snprintf(buf, 24, "%c%d", is_param ? 'p' : 'l', index);
}

void cname(const char *src, char *buf, size_t n) {
    size_t i = 0;
    if (!src || !*src) {
        snprintf(buf, n, "_");
        return;
    }
    if (src[0] >= '0' && src[0] <= '9' && n > 1) buf[i++] = '_';
    for (const char *s = src; *s && i + 1 < n; s++) {
        char ch = *s;
        buf[i++] = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '_' ? ch : '_';
    }
    buf[i] = '\0';
}

/* Prints one string body C-escaped (the surrounding quotes belong to
 * the caller). Control bytes become octal escapes, which always end
 * after exactly three digits and so cannot swallow a following hex
 * digit the way \x escapes can. */
void cesc(CE *c, const char *s, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
        case '"':  fputs("\\\"", c->out); break;
        case '\\': fputs("\\\\", c->out); break;
        case '\n': fputs("\\n", c->out);  break;
        case '\r': fputs("\\r", c->out);  break;
        case '\t': fputs("\\t", c->out);  break;
        default:
            if (ch < 0x20 || ch == 0x7f) fprintf(c->out, "\\%03o", ch);
            else fputc(ch, c->out);
        }
    }
}

/* Result-struct name for a T! value type. Names follow the LLVM
 * backend family: px_res_void, px_res_i8..i64, px_res_float,
 * px_res_double, px_res_p, px_res_<mangled>. */
void cres_name(const HType *value_type, char *buf, size_t n) {
    char nm[64];
    if (!value_type || value_type->kind == HTY_VOID) {
        snprintf(buf, n, "px_res_void");
        return;
    }
    switch (value_type->kind) {
    case HTY_INT: case HTY_UINT: {
        int w = value_type->width <= 8 ? 8 : value_type->width <= 16 ? 16
            : value_type->width <= 32 ? 32 : 64;
        snprintf(nm, sizeof(nm), "i%d", w);
        break;
    }
    case HTY_REAL:
        snprintf(nm, sizeof(nm), "%s", value_type->width <= 32
                 ? "float" : "double");
        break;
    case HTY_BOOL:  snprintf(nm, sizeof(nm), "i1"); break;
    case HTY_CHAR:  snprintf(nm, sizeof(nm), "i8"); break;
    case HTY_PTR:   snprintf(nm, sizeof(nm), "p"); break;
    case HTY_NAMED: cname(value_type->u.named.name, nm, sizeof(nm)); break;
    default:        snprintf(nm, sizeof(nm), "x"); break;
    }
    snprintf(buf, n, "px_res_%s", nm);
}

/* The payload type of a function returning T!, or NULL for a plain
 * function: a function node's type is its return type. */
const HType *cfunc_res_value(const HNode *f) {
    if (f && f->type && f->type->kind == HTY_ERROR)
        return f->type->u.error.value;
    return NULL;
}

/* True while the function under emission is the program entry point
 * (a function literally named main; the body emitter wraps it). */
/* True while the function under emission is the program entry point.
 * The entry point is either a top-level `main` or a `main` nested in
 * a named module (which lowering prefixes to "demo::main"); the body
 * emitter wraps it into px_user_main. */
int cis_main(CE *c) {
    const char *nm;
    size_t n;
    if (!c->cur_func || !c->cur_func->name) return 0;
    nm = c->cur_func->name;
    if (strcmp(nm, "main") == 0) return 1;
    n = strlen(nm);
    return n > 6 && strcmp(nm + n - 6, "::main") == 0;
}

/* Walks a module for channel node kinds. */
static int needs_runtime_node(const HNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case HK_SEND: case HK_RECV: case HK_SELECT:
    case HK_CLOSE: case HK_NEW_CHAN:
        return 1;
    default: break;
    }
    for (int i = 0; i < 4; i++)
        if (needs_runtime_node(n->child[i])) return 1;
    for (int i = 0; i < n->nitems; i++)
        if (needs_runtime_node(n->items[i])) return 1;
    return 0;
}

int cneeds_runtime(HModule **mods, int nmods) {
    for (int m = 0; m < nmods; m++)
        if (needs_runtime_node(hir_root(mods[m]))) return 1;
    return 0;
}

/* Clears string-constant aux ids; the driver may emit the same HIR
 * twice (e.g. an error-counting pass before the real write), and each
 * run must collect ids from scratch to keep definitions unique. */
static void reset_strings(const HNode *n) {
    if (!n) return;
    if (n->kind == HK_CONST_STRING || n->kind == HK_CONST_CHAR ||
        n->kind == HK_FMT)
        ((HNode *)n)->aux = -1;
    for (int i = 0; i < 4; i++) reset_strings(n->child[i]);
    for (int i = 0; i < n->nitems; i++) reset_strings(n->items[i]);
}

/* Assigns string-constant aux ids in traversal order. */
static void collect_strings(CE *c, const HNode *n) {
    if (!n) return;
    if ((n->kind == HK_CONST_STRING || n->kind == HK_CONST_CHAR ||
         n->kind == HK_FMT) && n->aux == -1)
        ((HNode *)n)->aux = c->nstr++;
    for (int i = 0; i < 4; i++) collect_strings(c, n->child[i]);
    for (int i = 0; i < n->nitems; i++) collect_strings(c, n->items[i]);
}

/* Prints the string table; the traversal order is identical to the
 * collection walk, so ids print strictly in ascending order. */
static void emit_strings(CE *c, const HNode *n) {
    if (!n) return;
    if ((n->kind == HK_CONST_STRING || n->kind == HK_CONST_CHAR ||
         n->kind == HK_FMT) && n->aux >= 0) {
        /* every constant becomes a plain char array; a character is
         * just a one-byte array, which cexpr reads as needed */
        fprintf(c->out, "static const char px_s%d[] = \"", n->aux);
        cesc(c, n->u.slice.start, n->u.slice.len);
        fputs("\";\n", c->out);
    }
    for (int i = 0; i < 4; i++) emit_strings(c, n->child[i]);
    for (int i = 0; i < n->nitems; i++) emit_strings(c, n->items[i]);
}

/* Emits every result struct a function signature needs. */
static void emit_res_structs(CE *c) {
    for (int m = 0; m < c->nmods; m++) {
        const HNode *root = hir_root(c->mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            const HNode *f = root->items[i];
            const HType *v;
            char nm[64];
            if (f->kind != HK_FUNC || !(v = cfunc_res_value(f))) continue;
            cres_name(v, nm, sizeof(nm));
            fprintf(c->out, "struct %s { long long err; ", nm);
            if (v->kind != HTY_VOID) {
                cty(c, v);
                fputs(" value; ", c->out);
            }
            fputs("};\n", c->out);
        }
    }
    fputs("\n", c->out);
}

/* Finds a function declaration by name across all modules; used to
 * resolve the result ABI of the callee in a try/catch. */
const HNode *cfind_func(CE *c, const char *name) {
    for (int m = 0; m < c->nmods; m++) {
        const HNode *root = hir_root(c->mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            const HNode *d = root->items[i];
            if ((d->kind == HK_FUNC || d->kind == HK_EXTERN_FUNC) &&
                d->name && strcmp(d->name, name) == 0)
                return d;
        }
    }
    return NULL;
}

int cemit_modules(HModule **mods, int nmods, Diag *diag, FILE *out) {
    CE c;
    memset(&c, 0, sizeof(c));
    c.mods = mods;
    c.nmods = nmods;
    c.diag = diag;
    c.out = out;
    int errs0 = diag_error_count(diag);
    for (int m = 0; m < nmods; m++)
        reset_strings(hir_root(mods[m]));
    for (int m = 0; m < nmods; m++) collect_strings(&c, hir_root(mods[m]));
    fputs("#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
          "#include <string.h>\n", out);
    if (cneeds_runtime(mods, nmods))
        fputs("#include <pthread.h>\n#include <time.h>\n", out);
    fputs("\n", out);
    for (int m = 0; m < nmods; m++) emit_strings(&c, hir_root(mods[m]));
    if (c.nstr) fputs("\n", out);
    emit_res_structs(&c);

    if (cneeds_runtime(mods, nmods))
        cmod_runtime(&c);
    /* aggregates before anything else: prototypes and bodies both
     * reference the struct tags and enum constants. */
    cmod_aggregates(&c);
    /* prototypes first so call order is irrelevant */
    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++)
            if (root->items[i]->kind == HK_FUNC ||
                root->items[i]->kind == HK_EXTERN_FUNC) {
                cfunc_sig(&c, root->items[i]);
                fputs(";\n", out);
            }
    }
    fputs("\n", out);
    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++)
            if (root->items[i]->kind == HK_FUNC) {
                cfunc_body(&c, root->items[i]);
                fputs("\n", out);
            }
    }
    /* globals last: C resolves initializer cross-references itself */
    cmod_globals(&c);
    return diag_error_count(diag) - errs0;
}
