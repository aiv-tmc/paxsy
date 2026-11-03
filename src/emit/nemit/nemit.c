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
    return len > 6 && strcmp(name + len - 7, "::main") == 0;
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
    free(s->str_off);
    free(s->str_len);
    for (int i = 0; i < s->nfn; i++) free((char *)s->fn[i].name);
    free(s->fn);
    for (int i = 0; i < s->ngl; i++) free((char *)s->gl[i].name);
    free(s->gl);
    free(s->lsz);
    free(s->loff);
}

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

    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++)
            if (root->items[i]->kind == HK_GLOBAL)
                ncollect_global(s, root->items[i]);
    }

    nrt_emit(s);
    for (int m = 0; m < nmods; m++) {
        const HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            const HNode *f = root->items[i];
            if (f->kind == HK_FUNC && f->child[3]) nsel_func(s, f);
        }
    }
    if (s->err) return 1;
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
    nelf_layout(text.len, ro.len, s.data_len, 0, 0, &L);
    for (int i = 0; i < s.nabs; i++) {
        uint64_t base = s.abs[i].sect == NSECT_RO ? L.va_ro : L.va_data;
        uint64_t v = base + s.abs[i].off;
        memcpy(text.buf + s.abs[i].pos, &v, 8);
    }
    int rc = nelf_write(out_path, &L, text.buf, text.len,
                        ro.buf, ro.len, s.data, s.data_len, 0);
    if (rc != 0)
        diag_report(diag, DK_ERROR, "<nemit>", 0, 0,
                    "native backend: cannot write '%s'", out_path);
    else
        chmod(out_path, 0755);
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
    NEReloc *rel = calloc((size_t)(s.nabs > 0 ? s.nabs : 1), sizeof(NEReloc));
    for (int i = 0; i < s.nabs; i++) {
        rel[i].pos = s.abs[i].pos;
        rel[i].sect = s.abs[i].sect == NSECT_RO ? NELO_SEC_RODATA
                                                : NELO_SEC_DATA;
        rel[i].addend = s.abs[i].off;
    }
    int rc = nelo_write(out_path, text.buf, text.len,
                        ro.buf, ro.len, s.data, s.data_len,
                        syms, k, rel, s.nabs);
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
