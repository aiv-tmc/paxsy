#include "common.h"
#include "nemit.h"
#include "nsel.h"
#include "nelf.h"
#include "nelo.h"
#include "npe.h"

/*
 * nemit.c - the native emission driver. The pass registers one text
 * label per function definition and binds the entry point's label to
 * the main function (a file-scope `main` or a module-nested
 * `<module>::main`), lays out global storage in the writable data
 * image, emits the start-up sequence and every function body,
 * resolves code-relative fixups, and then serialises the unit as a
 * static executable (nemit_build), a relocatable object (nemit_obj)
 * or a PE32+ image (nemit_pe). Any diagnostic aborts the write, so an
 * output file only ever appears for a fully supported program. The
 * binary needs no runtime library: every service is either emitted
 * as start-up code or written in Paxsy inside the standard library
 * and compiled like any other function.
 */

#include <sys/stat.h>

static char *ndup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int nis_main_name(const char *name) {
    size_t len = strlen(name);
    if (len == 4 && strcmp(name, "main") == 0) return 1;
    return len > 6 && strcmp(name + len - 6, "::main") == 0;
}

static int nfunc_index(const NSel *s, const char *name) {
    if (!name) return -1;
    for (int i = 0; i < s->nfn; i++)
        if (strcmp(s->fn[i].name, name) == 0) return i;
    return -1;
}

static int nmark_name(NSel *s, const char *name) {
    int fi = nfunc_index(s, name);
    if (fi < 0 || s->fn[fi].live) return 0;
    s->fn[fi].live = 1;
    return 1;
}

static int nmark_asm_calls(NSel *s, const HNode *n) {
    const char *p = n->u.slice.start;
    const char *end = p + n->u.slice.len;
    int changed = 0;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' ||
                           *p == '\n' || *p == ';' || *p == ':' ||
                           *p == ',')) p++;
        const char *word = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\r' &&
               *p != '\n' && *p != ';' && *p != ':' && *p != ',') p++;
        size_t len = (size_t)(p - word);
        if (len == 4 && (word[0] == 'c' || word[0] == 'C') &&
            (word[1] == 'a' || word[1] == 'A') &&
            (word[2] == 'l' || word[2] == 'L') &&
            (word[3] == 'l' || word[3] == 'L')) {
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            const char *target = p;
            while (p < end && *p != ' ' && *p != '\t' && *p != '\r' &&
                   *p != '\n' && *p != ';' && *p != ',') p++;
            size_t nlen = (size_t)(p - target);
            if (nlen > 0 && nlen < 256) {
                char name[256];
                memcpy(name, target, nlen);
                name[nlen] = '\0';
                changed |= nmark_name(s, name);
            }
        }
        if (p < end && (*p == ';' || *p == '\n')) p++;
    }
    return changed;
}

static int nmark_refs(NSel *s, const HNode *n) {
    if (!n) return 0;
    int changed = 0;
    if (n->kind == HK_INLINE_ASM)
        changed |= nmark_asm_calls(s, n);
    if (n->kind == HK_CALL && n->child[0] &&
        n->child[0]->kind == HK_GLOBAL_REF)
        changed |= nmark_name(s, n->child[0]->name);
    for (int i = 0; i < 4; i++)
        changed |= nmark_refs(s, n->child[i]);
    for (int i = 0; i < n->nitems; i++)
        changed |= nmark_refs(s, n->items[i]);
    return changed;
}

static void nmark_reachable(NSel *s) {
    for (int i = 0; i < s->nfn; i++)
        if (nis_main_name(s->fn[i].name)) s->fn[i].live = 1;

    for (;;) {
        int changed = 0;
        for (int i = 0; i < s->nfn; i++)
            if (s->fn[i].live)
                changed |= nmark_refs(s, s->fn[i].node->child[3]);
        for (int m = 0; m < s->nmods; m++) {
            const HNode *root = hir_root(s->mods[m]);
            for (int i = 0; i < root->nitems; i++)
                if (root->items[i]->kind == HK_GLOBAL)
                    changed |= nmark_refs(s, root->items[i]->child[0]);
        }
        if (!changed) break;
    }
}

static void ncollect_global(NSel *s, const HNode *g) {
    size_t bytes = 8;
    if (g->type && g->type->kind == HTY_ARRAY)
        bytes = (size_t)g->type->u.array.count * 8;
    if (s->ngl == s->capgl) {
        s->capgl = s->capgl ? s->capgl * 2 : 8;
        s->gl = realloc(s->gl, (size_t)s->capgl * sizeof(s->gl[0]));
    }
    s->gl[s->ngl].name = ndup(g->name);
    s->gl[s->ngl].off = (uint32_t)s->data_len;
    s->gl[s->ngl].node = g;
    s->ngl++;
    size_t old = s->data_len;
    s->data_len += bytes;
    if (s->data_len > s->data_cap) {
        s->data_cap = s->data_cap ? s->data_cap * 2 : 64;
        while (s->data_cap < s->data_len) s->data_cap *= 2;
        s->data = realloc(s->data, s->data_cap);
    }
    memset(s->data + old, 0, bytes);
    const HNode *init = g->child[0];
    if (init && init->kind == HK_ARRAY_LIT) {
        for (int e = 0; e < init->nitems && (size_t)e * 8 < bytes; e++) {
            const HNode *el = init->items[e];
            if (el->kind == HK_CONST_INT) {
                uint64_t v = (uint64_t)el->u.ival;
                memcpy(s->data + old + (size_t)e * 8, &v, 8);
            }
        }
    } else if (init && init->kind == HK_CONST_FLOAT) {
        /* The image stores the IEEE-754 bit pattern exactly as the
         * register convention carries it. */
        union { double d; uint64_t u; } fb;
        fb.d = init->u.fval;
        memcpy(s->data + old, &fb.u, 8);
    } else if (init && init->kind == HK_CONST_INT) {
        uint64_t v = (uint64_t)init->u.ival;
        memcpy(s->data + old, &v, 8);
    }
}

static void nfree(NSel *s, NXBuf *text, NXBuf *ro) {
    nx_free(text);
    nx_free(ro);
    free(s->data);
    free(s->abs);
    free(s->dfx);
    free(s->st);
    free(s->str_off);
    free(s->str_len);
    for (int i = 0; i < s->nfn; i++) free((char *)s->fn[i].name);
    free(s->fn);
    for (int i = 0; i < s->ngl; i++) free((char *)s->gl[i].name);
    free(s->gl);
    free(s->lsz);
    free(s->loff);
}

static void npeephole(NSel *s, NXBuf *t);

static int nemit_prepare(NSel *s, NXBuf *text, NXBuf *ro,
                         HModule **mods, int nmods, Diag *diag) {
    memset(s, 0, sizeof(*s));
    s->mods = mods;
    s->nmods = nmods;
    s->diag = diag;
    s->text = text;
    s->ro = ro;

    s->rt_start = nx_label(text);
    s->lbl_main = -1;

    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            const HNode *f = root->items[i];
            if (f->kind != HK_FUNC) continue;
            if (s->nfn == s->capfn) {
                s->capfn = s->capfn ? s->capfn * 2 : 16;
                s->fn = realloc(s->fn, (size_t)s->capfn * sizeof(s->fn[0]));
            }
            s->fn[s->nfn].name = ndup(f->name);
            s->fn[s->nfn].lbl = nx_label(text);
            s->fn[s->nfn].node = f;
            s->fn[s->nfn].live = 0;
            if (nis_main_name(f->name)) s->lbl_main = s->fn[s->nfn].lbl;
            s->nfn++;
        }
    }
    if (s->lbl_main < 0) {
        char names[512];
        size_t used = 0;
        names[0] = '\0';
        for (int i = 0; i < s->nfn && used < sizeof(names) - 80; i++) {
            int w = snprintf(names + used, sizeof(names) - used, "%s%s[%zu]",
                             i ? ", " : "", s->fn[i].name,
                             strlen(s->fn[i].name));
            if (w > 0) used += (size_t)w;
        }
        diag_report(diag, DK_ERROR, "<nemit>", 0, 0,
                    "native backend: no main function found "
                    "(%d function(s): %s)", s->nfn, names);
        return 1;
    }

    nmark_reachable(s);

    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            if (root->items[i]->kind == HK_STRUCT) {
                const HNode *sn = root->items[i];
                if (s->nst == s->capst) {
                    s->capst = s->capst ? s->capst * 2 : 8;
                    s->st = realloc(s->st, (size_t)s->capst * sizeof(*s->st));
                }
                s->st[s->nst++] = sn;
            }
            if (root->items[i]->kind == HK_GLOBAL)
                ncollect_global(s, root->items[i]);
        }
    }

    nrt_emit(s);
    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            const HNode *f = root->items[i];
            if (f->kind == HK_FUNC && f->child[3]) {
                int fi = nfunc_index(s, f->name);
                if (fi >= 0 && s->fn[fi].live) nsel_func(s, f);
            }
        }
    }
    if (s->err) return 1;
    nresolve_call_fixups(s);
    npeephole(s, text);
    if (nx_resolve(text)) {
        diag_report(diag, DK_ERROR, "<nemit>", 0, 0,
                    "native backend: unresolved code reference");
        return 1;
    }
    return 0;
}

int nemit_build(HModule **mods, int nmods, Diag *diag, const char *out_path) {
    NSel s;
    NXBuf text, ro;
    nx_init(&text);
    nx_init(&ro);
    if (nemit_prepare(&s, &text, &ro, mods, nmods, diag)) {
        nfree(&s, &text, &ro);
        return 1;
    }
    NELayout L;
    /* Symbol table for the integrated debugger and binutils: one
     * entry per emitted function. */
    const char **sym_names = calloc((size_t)s.nfn, sizeof(char *));
    uint32_t *sym_offs = calloc((size_t)s.nfn, sizeof(uint32_t));
    uint32_t str_len = 1, nsyms = 0;
    for (int i = 0; i < s.nfn; i++) {
        uint32_t off = nx_label_off(&text, s.fn[i].lbl);
        if (off == NX_UNDEF || !s.fn[i].name) continue;
        sym_names[nsyms] = s.fn[i].name;
        sym_offs[nsyms] = off;
        str_len += (uint32_t)strlen(s.fn[i].name) + 1;
        nsyms++;
    }
    char *strtab = calloc(str_len, 1);
    {
        uint32_t p = 1;
        for (uint32_t i = 0; i < nsyms; i++) {
            strcpy(strtab + p, sym_names[i]);
            p += (uint32_t)strlen(sym_names[i]) + 1;
        }
    }
    nelf_layout(text.len, ro.len, s.data_len, 0, 0, &L, nsyms, str_len);
    for (int i = 0; i < s.nabs; i++) {
        uint64_t base = s.abs[i].sect == NSECT_RO ? L.va_ro : L.va_data;
        uint64_t v = base + s.abs[i].off;
        memcpy(text.buf + s.abs[i].pos, &v, 8);
    }
    /* RIP-relative fixups recorded for [symbol] asm operands. */
    for (int i = 0; i < s.ndfx; i++) {
        int32_t rel = (int32_t)((int64_t)(L.va_data + s.dfx[i].data_off)
                                - (int64_t)(L.va_text + s.dfx[i].pos + 4));
        memcpy(text.buf + s.dfx[i].pos, &rel, 4);
    }
    int rc = nelf_write(out_path, &L, text.buf, text.len,
                        ro.buf, ro.len, s.data, s.data_len, 0,
                        sym_names, sym_offs, nsyms, strtab, str_len);
    if (rc != 0)
        diag_report(diag, DK_ERROR, "<nemit>", 0, 0,
                    "native backend: cannot write '%s'", out_path);
    else
        chmod(out_path, 0755);
    free(sym_names);
    free(sym_offs);
    free(strtab);
    nfree(&s, &text, &ro);
    return rc ? 1 : 0;
}

int nemit_obj(HModule **mods, int nmods, Diag *diag, const char *out_path) {
    NSel s;
    NXBuf text, ro;
    nx_init(&text);
    nx_init(&ro);
    if (nemit_prepare(&s, &text, &ro, mods, nmods, diag)) {
        nfree(&s, &text, &ro);
        return 1;
    }
    int nsyms = s.nfn + s.ngl + 1;
    NESym *syms = calloc((size_t)nsyms, sizeof(NESym));
    int k = 0;
    {
        uint32_t off = nx_label_off(&text, s.rt_start);
        if (off != NX_UNDEF) {
            syms[k].name = "_start";
            syms[k].value = off;
            syms[k].info = (uint8_t)(1u << 4) | 2u;
            syms[k].shndx = NELO_SEC_TEXT;
            k++;
        }
    }
    for (int i = 0; i < s.nfn; i++) {
        uint32_t off = nx_label_off(&text, s.fn[i].lbl);
        if (off == NX_UNDEF) continue;
        syms[k].name = s.fn[i].name;
        syms[k].value = off;
        syms[k].info = (uint8_t)(1u << 4) | 2u;
        syms[k].shndx = NELO_SEC_TEXT;
        k++;
    }
    for (int i = 0; i < s.ngl; i++) {
        syms[k].name = s.gl[i].name;
        syms[k].value = s.gl[i].off;
        syms[k].info = (uint8_t)(1u << 4) | 1u;
        syms[k].shndx = NELO_SEC_DATA;
        k++;
    }
    /* Absolute address constants plus the rip-relative fixups the
     * assembler recorded for [symbol] operands; both become
     * relocations, the former 8-byte absolute, the latter 4-byte
     * pc-relative against .data. */
    int nrel = s.nabs + s.ndfx;
    NEReloc *rel = calloc((size_t)(nrel > 0 ? nrel : 1), sizeof(NEReloc));
    for (int i = 0; i < s.nabs; i++) {
        rel[i].pos = s.abs[i].pos;
        rel[i].sect = s.abs[i].sect == NSECT_RO ? NELO_SEC_RODATA
                                                : NELO_SEC_DATA;
        rel[i].type = NELO_R_X86_64_64;
        rel[i].addend = s.abs[i].off;
    }
    for (int i = 0; i < s.ndfx; i++) {
        rel[s.nabs + i].pos = s.dfx[i].pos;
        rel[s.nabs + i].sect = NELO_SEC_DATA;
        rel[s.nabs + i].type = NELO_R_X86_64_PC32;
        rel[s.nabs + i].addend = s.dfx[i].data_off;
    }
    /* nelo expects the relocations in ascending position order. */
    for (int i = 1; i < nrel; i++) {
        NEReloc r = rel[i];
        int j = i - 1;
        while (j >= 0 && rel[j].pos > r.pos) {
            rel[j + 1] = rel[j];
            j--;
        }
        rel[j + 1] = r;
    }
    int rc = nelo_write(out_path, text.buf, text.len,
                        ro.buf, ro.len, s.data, s.data_len,
                        syms, k, rel, nrel);
    if (rc != 0)
        diag_report(diag, DK_ERROR, "<nemit>", 0, 0,
                    "native backend: cannot write '%s'", out_path);
    free(syms);
    free(rel);
    nfree(&s, &text, &ro);
    return rc ? 1 : 0;
}

int nemit_pe(HModule **mods, int nmods, Diag *diag, const char *out_path) {
    NSel s;
    NXBuf text, ro;
    nx_init(&text);
    nx_init(&ro);
    if (nemit_prepare(&s, &text, &ro, mods, nmods, diag)) {
        nfree(&s, &text, &ro);
        return 1;
    }
    uint64_t va_text = 0x1000;
    uint64_t va_ro = va_text + ((text.len + 0xFFF) & ~0xFFF);
    uint64_t va_data = va_ro + ((ro.len + 0xFFF) & ~0xFFF);
    for (int i = 0; i < s.nabs; i++) {
        uint64_t base = s.abs[i].sect == NSECT_RO ? va_ro : va_data;
        uint64_t v = 0x400000 + base + s.abs[i].off;
        memcpy(text.buf + s.abs[i].pos, &v, 8);
    }
    int rc = npe_write(out_path, text.buf, text.len,
                       ro.buf, ro.len, s.data, s.data_len, 0x400000,
                       va_text, va_ro, va_data);
    if (rc != 0)
        diag_report(diag, DK_ERROR, "<nemit>", 0, 0,
                    "native backend: cannot write '%s'", out_path);
    nfree(&s, &text, &ro);
    return rc ? 1 : 0;
}


/* ---- peephole: byte-level dead instruction elimination -------- */

/* Returns non-zero when any recorded code position (jump fixups, label
 * targets, symbol starts, absolute or RIP-relative fixups) falls
 * strictly inside [a, b). */
static int npos_in_use(const NSel *s, const NXBuf *t, uint32_t a, uint32_t b) {
    int i;
    for (i = 0; i < t->nfx; i++)
        if (t->fx[i].pos > a && t->fx[i].pos < b) return 1;
    for (i = 0; i < t->nlbl; i++)
        if (t->lbl[i] != NX_UNDEF && t->lbl[i] > a && t->lbl[i] < b)
            return 1;
    for (i = 0; i < s->nabs; i++)
        if (s->abs[i].pos > a && s->abs[i].pos < b) return 1;
    for (i = 0; i < s->ndfx; i++)
        if (s->dfx[i].pos > a && s->dfx[i].pos < b) return 1;
    return 0;
}

/* Shifts every recorded position at or after 'at' left by 'len'. */
static void npos_shift(NSel *s, NXBuf *t, uint32_t at, uint32_t len) {
    int i;
    for (i = 0; i < t->nfx; i++)
        if (t->fx[i].pos >= at) t->fx[i].pos -= len;
    for (i = 0; i < t->nlbl; i++)
        if (t->lbl[i] != NX_UNDEF && t->lbl[i] >= at) t->lbl[i] -= len;
    for (i = 0; i < s->nabs; i++)
        if (s->abs[i].pos >= at) s->abs[i].pos -= len;
    for (i = 0; i < s->ndfx; i++)
        if (s->dfx[i].pos >= at) s->dfx[i].pos -= len;
}

/* Removes the byte range [a, a+len) from the code buffer. */
static void ncut(NSel *s, NXBuf *t, uint32_t a, uint32_t len) {
    memmove(t->buf + a, t->buf + a + len, t->len - a - len);
    t->len -= len;
    npos_shift(s, t, a + len, len);
}

/* Instruction matcher for the peephole. Recognises:
 *   L1: 48 C7 C0+r imm32   mov r64, imm32        (7 bytes)
 *   L2: 48 B8+r imm64      mov r64, imm64        (10 bytes)
 *   L3: 48 89 C0+r         mov r64, r64 (self)   (3 bytes)
 * Sets *len to the instruction length and returns the register code
 * (0-15) or -1 when the bytes do not match. */
static int nmatch(const uint8_t *p, uint32_t avail, uint32_t *len) {
    if (avail >= 7 && p[0] == 0x48 && p[1] == 0xC7 &&
        (p[2] & 0xF8) == 0xC0) {
        *len = 7;
        return p[2] & 7;
    }
    if (avail >= 10 && p[0] == 0x48 && p[1] >= 0xB8 && p[1] <= 0xBF) {
        *len = 10;
        return p[1] - 0xB8;
    }
    if (avail >= 3 && p[0] == 0x48 && p[1] == 0x89 &&
        (p[2] & 0xF8) == 0xC0) {
        *len = 3;
        return p[2] & 7;
    }
    return -1;
}

/* One elimination pass over the emitted code: a write to a register
 * that is immediately overwritten again carries no value - the first
 * write is removed. Only instructions whose interior no recorded
 * position refers to are candidates, so every relocation stays
 * attached to the instruction it belongs to. */
static void npeephole(NSel *s, NXBuf *t) {
    uint32_t i = 0;
    while (i + 3 <= t->len) {
        uint32_t l1 = 0, l2 = 0;
        int r1 = nmatch(t->buf + i, (uint32_t)t->len - i, &l1);
        if (r1 >= 0) {
            int r2 = nmatch(t->buf + i + l1, (uint32_t)t->len - i - l1, &l2);
            /* Same destination written twice in a row: the first
             * write is dead. Equal immediates collapse identically. */
            if (r2 == r1 && !npos_in_use(s, t, i, i + l1)) {
                ncut(s, t, i, l1);
                continue;
            }
        }
        i += l1 > 0 ? l1 : 1;
    }
}
