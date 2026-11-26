#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "nlink.h"
#include "nlink_int.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>

/* Loads relocatable objects and archives into owned linker records. */
/* Copies a string with the C allocator; a local helper keeps the
 * strict C99 build free of POSIX-only declarations. */
static char *ndup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void nlobj_release(NLObj *obj) {
    if (obj->sym)
        for (int i = 0; i < obj->nsym; i++) free(obj->sym[i].name);
    free(obj->sym);
    free(obj->rel);
    free(obj->text);
    free(obj->ro);
    free(obj->data);
    memset(obj, 0, sizeof(*obj));
}

void nlink_objs_release(NLObj *objs, int count) {
    if (!objs) return;
    for (int i = 0; i < count; i++) nlobj_release(&objs[i]);
    free(objs);
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
    uint8_t *buf;
    long size;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc(size > 0 ? (size_t)size : 1);
    if (!buf || (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    if (fclose(f) != 0) { free(buf); return NULL; }
    *len = (size_t)size;
    return buf;
}

/* Parses one relocatable object from `buf`; appends to `objs` at
 * index `*no` and bumps the counter on success. */
static int load_obj(NLObj *objs, int *no, const uint8_t *buf, size_t len,
                    Diag *diag, const char *path) {
    uint64_t shoff, sym_len = 0, rela_len = 0;
    uint64_t str_off = 0, str_len = 0;
    int shnum;
    const uint8_t *symtab = NULL, *strtab = NULL, *rela = NULL;
    NLObj obj;
    if (!buf) {
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: empty input buffer for '%s'", path);
        return -1;
    }
    if (*no >= NL_MAX_OBJS) {
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: object limit (%d) exceeded", NL_MAX_OBJS);
        return -1;
    }
    if (len < 64 || buf[0] != 0x7f || buf[1] != 'E' ||
        buf[4] != 2 || rd16(buf + 16) != 1) {
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: '%s' is not a 64-bit relocatable object", path);
        return -1;
    }
    shoff = rd64(buf + 40);
    shnum = rd16(buf + 60);
    if (!shnum || shoff > len || (uint64_t)shnum * 64 > len - shoff) {
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: corrupt section table in '%s'", path);
        return -1;
    }
    memset(&obj, 0, sizeof(obj));
    for (int i = 0; i < shnum; i++) {
        const uint8_t *section = buf + shoff + (uint64_t)i * 64;
        uint64_t off = rd64(section + 24), size = rd64(section + 32);
        uint32_t type = rd32(section + 4);
        if (off > len || size > len - off) {
            diag_report(diag, DK_ERROR, path, 0, 0,
                        "linker: corrupt section in '%s'", path);
            return -1;
        }
        if (i == 1) { obj.text = (uint8_t *)buf + off; obj.text_len = (size_t)size; }
        if (i == 2) { obj.ro = (uint8_t *)buf + off; obj.ro_len = (size_t)size; }
        if (i == 3) { obj.data = (uint8_t *)buf + off; obj.data_len = (size_t)size; }
        if (type == 2) {
            uint32_t link = rd32(section + 40);
            if (link >= (uint32_t)shnum) {
                diag_report(diag, DK_ERROR, path, 0, 0,
                            "linker: invalid symbol string-table index in '%s'", path);
                return -1;
            }
            symtab = buf + off;
            sym_len = size;
            const uint8_t *str_section = buf + shoff + (uint64_t)link * 64;
            str_off = rd64(str_section + 24);
            str_len = rd64(str_section + 32);
            if (str_off > len || str_len > len - str_off) {
                diag_report(diag, DK_ERROR, path, 0, 0,
                            "linker: invalid symbol string table in '%s'", path);
                return -1;
            }
            strtab = buf + str_off;
        }
        if (type == 4) { rela = buf + off; rela_len = size; }
    }
    if (!obj.text || !symtab || !strtab || sym_len % 24 || rela_len % 24 ||
        sym_len / 24 > INT_MAX || rela_len / 24 > INT_MAX) {
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: '%s' misses text or has invalid symbol data", path);
        return -1;
    }
    {
        uint8_t *text_copy = malloc(obj.text_len ? obj.text_len : 1);
        uint8_t *ro_copy = malloc(obj.ro_len ? obj.ro_len : 1);
        uint8_t *data_copy = malloc(obj.data_len ? obj.data_len : 1);
        if (!text_copy || !ro_copy || !data_copy) {
            free(text_copy); free(ro_copy); free(data_copy);
            diag_report(diag, DK_ERROR, path, 0, 0,
                        "linker: out of memory while loading '%s'", path);
            return -1;
        }
        if (obj.text_len) memcpy(text_copy, obj.text, obj.text_len);
        if (obj.ro_len) memcpy(ro_copy, obj.ro, obj.ro_len);
        if (obj.data_len) memcpy(data_copy, obj.data, obj.data_len);
        obj.text = text_copy;
        obj.ro = ro_copy;
        obj.data = data_copy;
    }
    obj.nsym = (int)(sym_len / 24);
    obj.sym = calloc((size_t)(obj.nsym ? obj.nsym : 1), sizeof(*obj.sym));
    obj.nrel = (int)(rela_len / 24);
    obj.rel = calloc((size_t)(obj.nrel ? obj.nrel : 1), sizeof(*obj.rel));
    if (!obj.sym || !obj.rel) goto allocation_failure;
    {
        int symbols = obj.nsym;
        obj.nsym = 0;
        for (int i = 4; i < symbols; i++) {
            const uint8_t *entry = symtab + (size_t)i * 24;
            uint32_t name_off = rd32(entry);
            uint8_t section = entry[6];
            const char *name;
            if (!section || section > 3) continue;
            if (name_off >= str_len ||
                !memchr(strtab + name_off, 0, (size_t)(str_len - name_off)))
                goto malformed_symbol;
            name = (const char *)strtab + name_off;
            obj.sym[obj.nsym].name = ndup_str(name);
            if (!obj.sym[obj.nsym].name) goto allocation_failure;
            obj.sym[obj.nsym].sect = section;
            obj.sym[obj.nsym].value = rd64(entry + 8);
            if ((section == 1 && obj.sym[obj.nsym].value >= obj.text_len) ||
                (section == 2 && obj.sym[obj.nsym].value >= obj.ro_len) ||
                (section == 3 && obj.sym[obj.nsym].value >= obj.data_len))
                goto malformed_symbol;
            obj.nsym++;
        }
    }
    for (int i = 0; i < obj.nrel; i++) {
        const uint8_t *entry = rela + (size_t)i * 24;
        obj.rel[i].pos = rd64(entry);
        obj.rel[i].sect = (uint8_t)(rd64(entry + 8) >> 32);
        obj.rel[i].type = (uint8_t)(rd32(entry + 8) & 0xffu);
        obj.rel[i].addend = rd64(entry + 16);
        if (obj.rel[i].pos > obj.text_len ||
            obj.rel[i].sect < 1 || obj.rel[i].sect > 3 ||
            (obj.rel[i].type == 1 &&
             obj.text_len - obj.rel[i].pos < 8) ||
            (obj.rel[i].type == 2 &&
             obj.text_len - obj.rel[i].pos < 4) ||
            (obj.rel[i].type != 1 && obj.rel[i].type != 2))
            goto malformed_symbol;
    }
    objs[*no] = obj;
    (*no)++;
    return 0;
malformed_symbol:
    diag_report(diag, DK_ERROR, path, 0, 0,
                "linker: malformed symbol or relocation in '%s'", path);
    nlobj_release(&obj);
    return -1;
allocation_failure:
    diag_report(diag, DK_ERROR, path, 0, 0,
                "linker: out of memory while loading '%s'", path);
    nlobj_release(&obj);
    return -1;
}

/* Opens an input: plain object or ar archive (members loaded in
 * order). */
int nlink_load_input(NLObj *objs, int *no, const char *path, Diag *diag) {
    size_t len;
    uint8_t *buf = slurp(path, &len);
    if (!buf) {
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: cannot read '%s'", path);
        return -1;
    }
    if (len >= 8 && memcmp(buf, "!<arch>\n", 8) == 0) {
        size_t cur = 8;
        int members = 0;
        while (cur < len) {
            char size_text[11];
            char *end = NULL;
            unsigned long member_size;
            if (cur > len || len - cur < 60 ||
                memcmp(buf + cur + 58, "`\n", 2) != 0)
                goto archive_error;
            memcpy(size_text, buf + cur + 48, 10);
            size_text[10] = 0;
            member_size = strtoul(size_text, &end, 10);
            if (end == size_text || (*end && *end != ' ') ||
                member_size > len - cur - 60)
                goto archive_error;
            if (member_size > 0) {
                if (load_obj(objs, no, buf + cur + 60,
                             (size_t)member_size, diag, path) != 0) {
                    free(buf);
                    return -1;
                }
                members++;
            }
            cur += 60 + (size_t)member_size + ((size_t)member_size & 1u);
            if (cur > len) goto archive_error;
        }
        free(buf);
        if (!members) {
            diag_report(diag, DK_ERROR, path, 0, 0,
                        "linker: archive '%s' contains no object members", path);
            return -1;
        }
        return 0;
archive_error:
        free(buf);
        diag_report(diag, DK_ERROR, path, 0, 0,
                    "linker: malformed archive '%s'", path);
        return -1;
    }
    int rc = load_obj(objs, no, buf, len, diag, path);
    free(buf);
    return rc;
}

