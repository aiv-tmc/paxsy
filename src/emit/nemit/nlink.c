#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "nlink.h"
#include "nelf.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/*
 * nlink.c - static linking of the toolchain's own object files.
 * Objects follow the layout nelo writes: section 1 is .text, 2 is
 * .rodata, 3 is .data, symbol 0 is null, symbols 1..3 are the
 * section symbols, and every global symbol after them carries an
 * absolute value inside its section. Archives follow the classic ar
 * layout and are opened recursively, so an archive member can be
 * listed like a plain object. Linking steps: load every input,
 * concatenate sections (text aligned to 16, read-only data to 16,
 * writable data to 8), resolve the global symbols with a linear
 * table (duplicates are errors), patch every relocation with the
 * final virtual address of its target section plus the addend, and
 * write the executable through nelf_write with _start as the entry
 * point.
 */

#define NL_MAX_OBJS 64

/* One loaded input object. */
typedef struct {
    uint8_t *text, *ro, *data;
    size_t text_len, ro_len, data_len;
    /* Bases inside the combined image. */
    uint64_t text_base, ro_base, data_base;
    /* Symbols (global only) with values relative to their section. */
    struct { char *name; uint8_t sect; uint64_t value; } *sym;
    int nsym;
    /* Relocations: offset inside this object's text, target section
     * (2 = .rodata, 3 = .data) and section-relative addend. */
    struct { uint64_t pos; uint8_t sect; uint64_t addend; } *rel;
    int nrel;
} NLObj;

/* Copies a string with the C allocator; a local helper keeps the
 * strict C99 build free of POSIX-only declarations. */
static char *ndup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);      /* host is little-endian */
    return v;
}

/* Reads a whole file; returns NULL on failure. */
static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Parses one relocatable object from `buf`; appends to `objs` at
 * index `*no` and bumps the counter on success. */
static int load_obj(NLObj *objs, int *no, const uint8_t *buf, size_t len,
                    Diag *diag, const char *path) {
    if (len < 64 || buf[0] != 0x7f || buf[1] != 'E' ||
        buf[4] != 2 || rd16(buf + 16) != 1) {      /* ET_REL, 64-bit */
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: '%s' is not a 64-bit relocatable object",
                    path);
        return -1;
    }
    uint64_t shoff = rd64(buf + 40);
    int shnum = buf[63];
    if (!shnum || shoff + (uint64_t)shnum * 64 > len) {
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: corrupt section table in '%s'", path);
        return -1;
    }
    NLObj o;
    memset(&o, 0, sizeof(o));
    const uint8_t *sh = buf + shoff;
    const uint8_t *symtab = NULL, *strtab = NULL;
    uint64_t sym_len = 0;
    const uint8_t *rela = NULL;
    uint64_t rela_len = 0;
    for (int i = 0; i < shnum; i++, sh += 64) {
        uint64_t off = rd64(sh + 24), sz = rd64(sh + 32);
        uint32_t type = rd32(sh + 4);
        if (off + sz > len) {
            diag_report(diag, DK_ERROR, path, 0, 0,
                        "linker: corrupt section in '%s'", path);
            return -1;
        }
        if (i == 1) { o.text = (uint8_t *)buf + off; o.text_len = (size_t)sz; }
        if (i == 2) { o.ro = (uint8_t *)buf + off; o.ro_len = (size_t)sz; }
        if (i == 3) { o.data = (uint8_t *)buf + off; o.data_len = (size_t)sz; }
        if (type == 2) { symtab = buf + off; sym_len = sz; }
        if (type == 3 && i > 5) strtab = buf + off;   /* .strtab */
        if (type == 4) { rela = buf + off; rela_len = sz; }
    }
    if (!o.text || !symtab || !strtab || !rela) {
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: '%s' misses text, symbols or relocations",
                    path);
        return -1;
    }
    /* Own copies of the payload sections: the buffer may be freed
     * before the link completes. */
    uint8_t *tc = malloc(o.text_len ? o.text_len : 1);
    uint8_t *rc = malloc(o.ro_len ? o.ro_len : 1);
    uint8_t *dc = malloc(o.data_len ? o.data_len : 1);
    memcpy(tc, o.text, o.text_len);
    memcpy(rc, o.ro, o.ro_len);
    memcpy(dc, o.data, o.data_len);
    o.text = tc; o.ro = rc; o.data = dc;

    int nsym = (int)(sym_len / 24);
    o.sym = calloc((size_t)(nsym > 0 ? nsym : 1), sizeof(*o.sym));
    for (int i = 4; i < nsym; i++) {
        const uint8_t *s = symtab + (size_t)i * 24;
        uint32_t name = rd32(s);
        uint8_t shndx = s[6];
        if (!shndx || shndx > 3) continue;         /* not a data/code sym */
        o.sym[o.nsym].name = ndup_str((const char *)strtab + name);
        o.sym[o.nsym].sect = shndx;
        o.sym[o.nsym].value = rd64(s + 8);
        o.nsym++;
    }
    o.nrel = (int)(rela_len / 24);
    o.rel = calloc((size_t)(o.nrel > 0 ? o.nrel : 1), sizeof(*o.rel));
    for (int i = 0; i < o.nrel; i++) {
        const uint8_t *r = rela + (size_t)i * 24;
        o.rel[i].pos = rd64(r);
        o.rel[i].sect = (uint8_t)(rd64(r + 8) >> 32);   /* section sym */
        o.rel[i].addend = rd64(r + 16);
    }
    objs[*no] = o;
    (*no)++;
    return 0;
}

/* Opens an input: plain object or ar archive (members loaded in
 * order). */
static int load_input(NLObj *objs, int *no, const char *path, Diag *diag) {
    size_t len;
    uint8_t *buf = slurp(path, &len);
    if (!buf) {
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: cannot read '%s'", path);
        return -1;
    }
    if (len >= 8 && memcmp(buf, "!<arch>\n", 8) == 0) {
        size_t cur = 8;
        while (cur + 60 <= len) {
            char mname[17];
            memcpy(mname, buf + cur, 16);
            mname[16] = '\0';
            unsigned long msize =
                strtoul((const char *)buf + cur + 48, NULL, 10);
            const uint8_t *mdata = buf + cur + 60;
            if (cur + 60 + msize > len) break;
            if (memcmp(buf + cur + 58, "`\n", 2) == 0 && msize > 0)
                load_obj(objs, no, mdata, msize, diag, path);
            cur += 60 + msize + (msize & 1);
        }
        free(buf);
        return 0;
    }
    int rc = load_obj(objs, no, buf, len, diag, path);
    free(buf);
    return rc;
}

int nlink_exec(Diag *diag, const char *out_path, char **objs, int nobjs) {
    NLObj *o = calloc(NL_MAX_OBJS, sizeof(NLObj));
    int no = 0;
    for (int i = 0; i < nobjs && no < NL_MAX_OBJS; i++)
        if (load_input(o, &no, objs[i], diag) != 0) return 1;
    if (no == 0) {
        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                    "linker: no object files");
        return 1;
    }

    /* Concatenate sections with per-object bases. */
    size_t tlen = 0, rlen = 0, dlen = 0;
    for (int i = 0; i < no; i++) {
        o[i].text_base = tlen;
        o[i].ro_base = rlen;
        o[i].data_base = dlen;
        tlen += (o[i].text_len + 15u) & ~15u;
        rlen += (o[i].ro_len + 15u) & ~15u;
        dlen += (o[i].data_len + 7u) & ~7u;
    }
    uint8_t *text = calloc(tlen ? tlen : 1, 1);
    uint8_t *ro = calloc(rlen ? rlen : 1, 1);
    uint8_t *data = calloc(dlen ? dlen : 1, 1);
    for (int i = 0; i < no; i++) {
        memcpy(text + o[i].text_base, o[i].text, o[i].text_len);
        memcpy(ro + o[i].ro_base, o[i].ro, o[i].ro_len);
        memcpy(data + o[i].data_base, o[i].data, o[i].data_len);
    }

    /* Resolve symbols: _start must exist exactly once; any other
     * duplicate means the same unit was linked twice. */
    uint64_t entry = UINT64_MAX;
    for (int i = 0; i < no; i++) {
        for (int j = 0; j < o[i].nsym; j++) {
            if (strcmp(o[i].sym[j].name, "_start") == 0) {
                if (entry != UINT64_MAX) {
                    diag_report(diag, DK_ERROR, "<link>", 0, 0,
                                "linker: duplicate _start");
                    return 1;
                }
                entry = o[i].text_base + o[i].sym[j].value;
            }
            for (int k = i + 1; k < no; k++)
                for (int m = 0; m < o[k].nsym; m++)
                    if (strcmp(o[i].sym[j].name,
                               o[k].sym[m].name) == 0) {
                        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                                    "linker: duplicate symbol '%s'",
                                    o[i].sym[j].name);
                        return 1;
                    }
        }
    }
    if (entry == UINT64_MAX) {
        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                    "linker: _start not found");
        return 1;
    }

    /* Apply relocations: final VA of the target section plus the
     * addend is patched into the combined text. The image geometry
     * is computed with the entry-point code offset. */
    NELayout L;
    nelf_layout(tlen, rlen, dlen, 0, (uint32_t)entry, &L);
    for (int i = 0; i < no; i++) {
        for (int j = 0; j < o[i].nrel; j++) {
            uint64_t base = o[i].rel[j].sect == 2
                ? L.va_ro + o[i].ro_base
                : L.va_data + o[i].data_base;
            uint64_t v = base + o[i].rel[j].addend;
            memcpy(text + o[i].text_base + o[i].rel[j].pos, &v, 8);
        }
    }

    int rc = nelf_write(out_path, &L, text, tlen, ro, rlen, data, dlen, 0);
    if (rc == 0) chmod(out_path, 0755);
    else
        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                    "linker: cannot write '%s'", out_path);
    return rc ? 1 : 0;
}
