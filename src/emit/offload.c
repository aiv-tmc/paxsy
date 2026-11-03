#include "common.h"
#include "offload.h"

/*
 * offload.c - the GPU offload analysis, reporting and simulation.
 *
 * The analysis answers two questions per HK_FOR loop.
 * offload_candidate() decides whether the loop may run on the device
 * at all: it needs a named loop variable, a closed upper bound and a
 * body from the device subset only - no channels, allocation, defer,
 * spawns, unknown or declared calls, no nested loops. Each rejection
 * returns one static reason string; NULL means "candidate".
 *
 * offload_pattern() classifies a candidate body: "sum" is one
 * accumulator statement `acc += expr(i)`, "map" is one element store
 * `arr[i] = expr(i)` whose base may be a module global, a local array
 * or a pointer parameter. Both restrict expr() to pure arithmetic over
 * constants, the loop variable, parameters and array reads, so the
 * CUDA emitter can re-emit the same expression with the loop
 * variable replaced by the thread index. A bare assignment statement
 * arrives wrapped in an expression statement and is unwrapped first.
 *
 * offload_report() prints one line per loop (line, verdict, pattern)
 * for the --dump-offload flag. offload_simulate() evaluates every
 * sum-pattern loop twice - a serial sweep and the 256-thread chunked
 * partial-sum reduction mirroring the CUDA grid - and reports
 * agreement; mismatches print as MISMATCH lines. Integer arithmetic is
 * associative, so both orders must agree. '..' is exclusive at the
 * upper bound; only the '..<' form extends it by one.
 */

/* The device value subset: pure arithmetic over what the kernel can
 * also see (constants, slots, parameters, array reads). */
static int device_expr(const HNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case HK_CONST_INT: case HK_CONST_FLOAT:
    case HK_LOCAL: case HK_PARAM:
        return 1;
    case HK_BINOP: case HK_UNOP: case HK_CAST:
        return device_expr(n->child[0]) && device_expr(n->child[1]);
    case HK_INDEX:
        return device_expr(n->child[0]) && device_expr(n->child[1]);
    case HK_FIELD:
        return n->u.index >= 0 && device_expr(n->child[0]);
    default:
        return 0;
    }
}

/* Body walker: every node must be in the subset and carry no device
 * effect flags. */
static int device_body(const HNode *n, const char **why) {
    if (!n) return 1;
    if (n->flags & (HF_SEND_RECV | HF_ALLOC | HF_DEFER | HF_SPAWN |
                    HF_UNKNOWN_CALL)) {
        *why = "host effects (channels/alloc/defer/spawn/unknown call)";
        return 0;
    }
    switch (n->kind) {
    case HK_BLOCK: case HK_STORE: case HK_LET: case HK_EXPR_STMT:
    case HK_IF:
        break;
    case HK_FOR: case HK_WHILE: case HK_DOWHILE:
        *why = "nested loops are not offloaded";
        return 0;
    case HK_CALL:
        *why = "calls are not allowed in a device loop";
        return 0;
    default:
        if (!device_expr(n)) {
            *why = "construct outside the device subset";
            return 0;
        }
    }
    for (int i = 0; i < 4; i++)
        if (!device_body(n->child[i], why)) return 0;
    for (int i = 0; i < n->nitems; i++)
        if (!device_body(n->items[i], why)) return 0;
    return 1;
}

const char *offload_candidate(const HNode *loop) {
    static char why[96];
    if (loop->flags & (HF_SEND_RECV | HF_ALLOC | HF_DEFER | HF_SPAWN |
                       HF_UNKNOWN_CALL)) {
        snprintf(why, sizeof(why),
                 "host effects (channels/alloc/defer/spawn/unknown call)");
        return why;
    }
    if (!loop->name) return "missing loop variable";
    if (!loop->child[1]) return "open upper bound";
    const char *w = NULL;
    if (!device_body(loop->child[2], &w)) return w;
    return NULL;
}

/* True when `n` never reads slot `acc`. */
static int avoids_slot(const HNode *n, int acc) {
    if (!n) return 1;
    if (n->kind == HK_LOCAL && n->u.index == acc) return 0;
    for (int i = 0; i < 4; i++)
        if (!avoids_slot(n->child[i], acc)) return 0;
    for (int i = 0; i < n->nitems; i++)
        if (!avoids_slot(n->items[i], acc)) return 0;
    return 1;
}

/* expr() may only touch the loop variable slot plus constants,
 * parameters and reads; other slots are runtime state the kernel
 * cannot see. */
static int pure_in(const HNode *n, int var_slot) {
    if (!n) return 0;
    if (n->kind == HK_LOCAL) return n->u.index == var_slot;
    if (n->kind == HK_PARAM || n->kind == HK_CONST_INT ||
        n->kind == HK_CONST_FLOAT)
        return 1;
    switch (n->kind) {
    case HK_BINOP: case HK_UNOP: case HK_CAST:
        return pure_in(n->child[0], var_slot) &&
               pure_in(n->child[1], var_slot);
    case HK_INDEX:
        return pure_in(n->child[0], var_slot) &&
               pure_in(n->child[1], var_slot);
    default:
        return 0;
    }
}

const char *offload_pattern(const HNode *loop, int *acc_slot) {
    const HNode *body = loop->child[2];
    if (!body || body->nitems != 1) return NULL;
    const HNode *s = body->items[0];
    /* a bare assignment statement arrives wrapped in an expression
     * statement; unwrap it before classifying the body */
    if (s->kind == HK_EXPR_STMT) s = s->child[0];
    if (!s || s->kind != HK_STORE) return NULL;
    /* sum: acc += expr(i) */
    if (s->child[0]->kind == HK_LOCAL &&
        s->child[1]->kind == HK_BINOP &&
        s->child[1]->u.binop == HOP_ADD) {
        int acc = s->child[0]->u.index;
        const HNode *a = s->child[1]->child[0];
        const HNode *b = s->child[1]->child[1];
        const HNode *e = NULL;
        if (a->kind == HK_LOCAL && a->u.index == acc &&
            pure_in(b, loop->u.index) && avoids_slot(b, acc))
            e = b;
        else if (b->kind == HK_LOCAL && b->u.index == acc &&
                 pure_in(a, loop->u.index) && avoids_slot(a, acc))
            e = a;
        if (e) {
            *acc_slot = acc;
            return "sum";
        }
    }
    /* map: arr[i] = expr(i); the base may be a global, a local
     * array or a pointer parameter */
    if (s->child[0]->kind == HK_INDEX &&
        s->child[0]->child[1]->kind == HK_LOCAL &&
        s->child[0]->child[1]->u.index == loop->u.index &&
        (s->child[0]->child[0]->kind == HK_GLOBAL_REF ||
         s->child[0]->child[0]->kind == HK_LOCAL ||
         s->child[0]->child[0]->kind == HK_PARAM) &&
        pure_in(s->child[1], loop->u.index)) {
        *acc_slot = -1;
        return "map";
    }
    return NULL;
}

/* Collects the distinct PARAM slots an expression reads, so device
 * code can receive those pointers as extra kernel arguments. */
void offload_expr_params(const HNode *e, int *slots, int *cnt) {
    if (!e) return;
    if (e->kind == HK_PARAM) {
        int i;
        for (i = 0; i < *cnt; i++)
            if (slots[i] == e->u.index) break;
        if (i == *cnt && *cnt < 4) slots[(*cnt)++] = e->u.index;
    }
    for (int i = 0; i < 4; i++) offload_expr_params(e->child[i], slots, cnt);
    for (int i = 0; i < e->nitems; i++)
        offload_expr_params(e->items[i], slots, cnt);
}

/* Visits every for loop of every function body (child[3]). */
static void for_each_loop(const HNode *n,
                          void (*fn)(const HNode *loop, void *ctx),
                          void *ctx) {
    if (!n) return;
    if (n->kind == HK_FOR) fn(n, ctx);
    for (int i = 0; i < 4; i++) for_each_loop(n->child[i], fn, ctx);
    for (int i = 0; i < n->nitems; i++) for_each_loop(n->items[i], fn, ctx);
}

typedef struct { FILE *out; } ReportCtx;

static void report_loop(const HNode *loop, void *ctx) {
    FILE *out = ((ReportCtx *)ctx)->out;
    const char *why = offload_candidate(loop);
    int acc;
    const char *pat = why ? NULL : offload_pattern(loop, &acc);
    fprintf(out, "  loop at L%d: ", loop->tok.line);
    if (why) fprintf(out, "CPU-only (%s)\n", why);
    else if (pat) fprintf(out, "GPU candidate (%s)\n", pat);
    else fprintf(out, "GPU candidate (generic)\n");
}

static void report_func(const HNode *f, void *ctx) {
    fprintf(((ReportCtx *)ctx)->out, "fn %s:\n", f->name);
    for_each_loop(f->child[3], report_loop, ctx);
}

void offload_report(HModule **mods, int nmods, FILE *out) {
    ReportCtx ctx = { out };
    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++)
            if (root->items[i]->kind == HK_FUNC)
                report_func(root->items[i], &ctx);
    }
}

/* Evaluates one device expression with the loop variable bound to
 * `i`; rejected constructs clear `ok`. */
static long long eval_at(const HNode *n, int var_slot, long long i,
                         int *ok) {
    if (!n) { *ok = 0; return 0; }
    switch (n->kind) {
    case HK_CONST_INT:   return n->u.ival;
    case HK_CONST_FLOAT: return (long long)n->u.fval;
    case HK_LOCAL:       return n->u.index == var_slot ? i : 0;
    case HK_PARAM:       return 0;
    case HK_CAST:        return eval_at(n->child[0], var_slot, i, ok);
    case HK_BINOP: {
        long long a = eval_at(n->child[0], var_slot, i, ok);
        long long b = eval_at(n->child[1], var_slot, i, ok);
        if (!*ok) return 0;
        unsigned long long ua = (unsigned long long)a;
        unsigned long long ub = (unsigned long long)b;
        switch (n->u.binop) {
        case HOP_ADD: return (long long)(ua + ub);
        case HOP_SUB: return (long long)(ua - ub);
        case HOP_MUL: return (long long)(ua * ub);
        case HOP_DIV: case HOP_MOD:
            if (b == 0) { *ok = 0; return 0; }
            return n->u.binop == HOP_DIV ? a / b : a % b;
        case HOP_AND: return (long long)(ua & ub);
        case HOP_OR:  return (long long)(ua | ub);
        case HOP_XOR: return (long long)(ua ^ ub);
        case HOP_SHL: return (long long)(ua << (ub & 63));
        case HOP_SHR: return (long long)((long long)ua >> (ub & 63));
        case HOP_USHR:return (long long)(ua >> (ub & 63));
        case HOP_EQ:  return a == b;
        case HOP_NE:  return a != b;
        case HOP_LT:  return a < b;
        case HOP_LE:  return a <= b;
        case HOP_GT:  return a > b;
        case HOP_GE:  return a >= b;
        case HOP_LAND:return a != 0 && b != 0;
        case HOP_LOR: return a != 0 || b != 0;
        }
        break;
    }
    case HK_UNOP: {
        long long a = eval_at(n->child[0], var_slot, i, ok);
        if (n->u.unop == HUN_NEG)
            return (long long)(0ULL - (unsigned long long)a);
        if (n->u.unop == HUN_BITNOT) return ~a;
        if (n->u.unop == HUN_LNOT) return a == 0;
        *ok = 0;
        return 0;
    }
    default:
        break;
    }
    *ok = 0;
    return 0;
}

typedef struct { FILE *out; int mismatches; } SimCtx;

static void sim_loop(const HNode *loop, void *ctx) {
    SimCtx *s = ctx;
    int acc;
    if (offload_candidate(loop) ||
        strcmp(offload_pattern(loop, &acc), "sum") != 0)
        return;
    const HNode *add = loop->child[2]->items[0]->child[1];
    const HNode *e = add->child[0]->kind == HK_LOCAL ? add->child[1]
                                                     : add->child[0];
    if (!loop->child[0] || !loop->child[1] ||
        loop->child[0]->kind != HK_CONST_INT ||
        loop->child[1]->kind != HK_CONST_INT) {
        fprintf(s->out, "  loop at L%d: skipped (non-constant bounds)\n",
                loop->tok.line);
        return;
    }
    long long lo = loop->child[0]->u.ival;
    long long hi = loop->child[1]->u.ival;
    /* '..' is exclusive; only '..<' extends the bound by one */
    if (loop->flags & HF_EXCLUSIVE) hi += 1;
    if (hi <= lo) {
        fprintf(s->out, "  loop at L%d: skipped (empty range)\n",
                loop->tok.line);
        return;
    }
    int ok = 1;
    long long serial = 0;
    for (long long i = lo; ok && i < hi; i++)
        serial += eval_at(e, loop->u.index, i, &ok);
    long long n = hi - lo;
    long long chunk = (n + 255) / 256;
    long long gpu = 0;
    for (long long t = 0; ok && t < 256 && lo + t * chunk < hi; t++) {
        long long end = lo + (t + 1) * chunk;
        if (end > hi) end = hi;
        long long partial = 0;
        for (long long i = lo + t * chunk; ok && i < end; i++)
            partial += eval_at(e, loop->u.index, i, &ok);
        gpu += partial;
    }
    if (!ok) {
        fprintf(s->out, "  loop at L%d: skipped (not evaluable)\n",
                loop->tok.line);
        return;
    }
    fprintf(s->out, "  loop at L%d: serial=%lld gpu=%lld %s\n",
            loop->tok.line, serial, gpu, serial == gpu ? "OK" : "MISMATCH");
    if (serial != gpu) s->mismatches++;
}

static void sim_func(const HNode *f, void *ctx) {
    fprintf(((SimCtx *)ctx)->out, "fn %s:\n", f->name);
    for_each_loop(f->child[3], sim_loop, ctx);
}

void offload_simulate(HModule **mods, int nmods, FILE *out) {
    SimCtx ctx = { out, 0 };
    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++)
            if (root->items[i]->kind == HK_FUNC)
                sim_func(root->items[i], &ctx);
    }
    /* mismatches are visible as MISMATCH lines in the report */
}
