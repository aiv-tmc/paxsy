#include "common.h"
#include "cemit_int.h"
#include "cuda.h"
#include "offload.h"

/*
 * cuda.c - CUDA emission for the offload candidates.
 *
 * cuda_emit_modules() walks the same modules, functions and for
 * loops in the same order as the C backend and assigns the same
 * kernel ordinal to the same loops (the C side dispatches under
 * PAXSY_HAS_CUDA to pax_gpu_sum_<k> / pax_gpu_map_<k>). Only loops
 * the offload analysis accepts with a sum or map pattern produce a
 * kernel. The single body statement of a pattern loop arrives wrapped
 * in an expression statement and is unwrapped first.
 *
 * The device expression is the pattern's expr(i) printed through the
 * shared cexpr() with the loop variable slot aliased to "px_i": the
 * CE alias fields redirect that one LOCAL to the thread index, every
 * other construct prints exactly as it does for the host. Globals
 * the kernels may read become extern declarations in the unit.
 *
 * Grid: 256 threads per block (the same chunk width the simulation
 * step uses), enough blocks to cover the range; the sum kernel
 * atomicAdds into one device word the launcher reads back. Launch
 * configuration uses named blocks/threads locals matching the
 * source-level grid description. The launchers are extern "C" so
 * the host C unit links against them when the driver compiles this
 * file with nvcc and defines PAXSY_HAS_CUDA.
 */

/* Prints "extern <type> <mangled>;" for a module-level global so
 * device code can read it; arrays keep their bound. */
static void extern_global(CE *c, const char *name) {
    for (int m = 0; m < c->nmods; m++) {
        const HNode *root = hir_root(c->mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            const HNode *g = root->items[i];
            char nm[96];
            if (g->kind != HK_GLOBAL || strcmp(g->name, name) != 0)
                continue;
            cname(name, nm, sizeof(nm));
            fputs("extern ", c->out);
            if (g->type && g->type->kind == HTY_ARRAY) {
                cty(c, g->type->u.array.elem);
                fprintf(c->out, " %s[%ld];\n", nm, g->type->u.array.count);
            } else {
                cty(c, g->type);
                fprintf(c->out, " %s;\n", nm);
            }
            return;
        }
    }
}

/* Prints the device expression: the bare loop variable needs no cast
 * (it already is the 64-bit thread index), anything else narrows
 * through a long long cast. */
static void device_expr(CE *c, const HNode *loop, const HNode *e) {
    c->alias_slot = loop->u.index;
    c->alias_name = "px_i";
    if (e->kind == HK_LOCAL && e->u.index == loop->u.index) {
        fputs("px_i", c->out);
    } else {
        fputs("(long long)(", c->out);
        cexpr(c, e);
        fputs(")", c->out);
    }
    c->alias_slot = -1;
    c->alias_name = NULL;
}

/* Emits one kernel plus its launcher for a sum pattern. */
static void emit_sum(CE *c, const HNode *loop, const HNode *e, long long k) {
    fprintf(c->out,
            "__global__ void px_kernel_%lld(long long *px_out, long long lo,"
            " long long hi) {\n"
            "    long long px_i = lo + (long long)blockIdx.x * blockDim.x "
            "+ threadIdx.x;\n"
            "    if (px_i < hi)\n"
            "        atomicAdd(px_out, ", k);
    device_expr(c, loop, e);
    fputs(");\n}\n", c->out);
    fprintf(c->out,
            "extern \"C\" long long pax_gpu_sum_%lld(long long lo, "
            "long long hi) {\n"
            "    long long n = hi - lo;\n"
            "    unsigned blocks = (unsigned)((n + 255) / 256), "
            "threads = 256;\n"
            "    long long *px_out = 0;\n"
            "    cudaMalloc(&px_out, sizeof(long long));\n"
            "    cudaMemset(px_out, 0, sizeof(long long));\n"
            "    if (n > 0)\n"
            "        px_kernel_%lld<<<blocks, threads>>>(px_out, lo, hi);\n"
            "    long long h = 0;\n"
            "    cudaMemcpy(&h, px_out, sizeof(long long), "
            "cudaMemcpyDeviceToHost);\n"
            "    cudaFree(px_out);\n"
            "    return h;\n"
            "}\n", k, k);
}

/* Emits one kernel plus its launcher for a map pattern. */
static void emit_map(CE *c, const HNode *loop, const HNode *e, long long k) {
    fprintf(c->out,
            "__global__ void px_kernel_%lld(long long *px_arr, long long lo,"
            " long long hi) {\n"
            "    long long px_i = lo + (long long)blockIdx.x * blockDim.x "
            "+ threadIdx.x;\n"
            "    if (px_i < hi)\n"
            "        px_arr[px_i] = ", k);
    device_expr(c, loop, e);
    fputs(";\n}\n", c->out);
    fprintf(c->out,
            "extern \"C\" void pax_gpu_map_%lld(long long *px_arr, "
            "long long lo, long long hi) {\n"
            "    long long n = hi - lo;\n"
            "    unsigned blocks = (unsigned)((n + 255) / 256), "
            "threads = 256;\n"
            "    if (n > 0)\n"
            "        px_kernel_%lld<<<blocks, threads>>>(px_arr, lo, hi);\n"
            "}\n", k, k);
}

/* Emits a kernel for every offloaded loop of one function body,
 * consuming kernel ordinals in cemit-identical order. */
static void walk_loops(CE *c, const HNode *n) {
    if (!n) return;
    if (n->kind == HK_FOR && !offload_candidate(n)) {
        int acc;
        const char *pat = offload_pattern(n, &acc);
        if (pat) {
            const HNode *store = n->child[2]->items[0];
            long long k = c->kernel++;
            /* unwrap the expression-statement wrapper */
            if (store->kind == HK_EXPR_STMT) store = store->child[0];
            if (strcmp(pat, "sum") == 0) {
                const HNode *add = store->child[1];
                const HNode *e = add->child[0]->kind == HK_LOCAL
                    ? add->child[1] : add->child[0];
                emit_sum(c, n, e, k);
            } else {
                emit_map(c, n, store->child[1], k);
            }
        }
    }
    for (int i = 0; i < 4; i++) walk_loops(c, n->child[i]);
    for (int i = 0; i < n->nitems; i++) walk_loops(c, n->items[i]);
}

int cuda_emit_modules(HModule **mods, int nmods, Diag *diag, FILE *out) {
    CE c;
    memset(&c, 0, sizeof(c));
    c.mods = mods;
    c.nmods = nmods;
    c.diag = diag;
    c.out = out;
    c.alias_slot = -1;
    int errs0 = diag_error_count(diag);
    fputs("/* paxsy CUDA unit: compile with nvcc, link with the host "
          "C unit under -DPAXSY_HAS_CUDA */\n"
          "#include <cuda_runtime.h>\n\n", out);
    /* externs for every module-level global (harmless when unused) */
    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++)
            if (root->items[i]->kind == HK_GLOBAL)
                extern_global(&c, root->items[i]->name);
    }
    fputs("\n", out);
    /* px_ushr for USHR inside device expressions */
    fputs("__device__ static long long px_ushr(unsigned long long v, "
          "long long n) {\n    return (long long)(v >> (n & 63));\n}\n\n",
          out);
    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++)
            if (root->items[i]->kind == HK_FUNC)
                walk_loops(&c, root->items[i]->child[3]);
    }
    return diag_error_count(diag) - errs0;
}
