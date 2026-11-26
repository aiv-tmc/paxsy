#include "debug_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static char *debug_strdup(const char *s) {
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

/* Reads the ELF64 symbol table and retains named function symbols. */
int elf_load_syms(const char *path, Sym **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    uint8_t eh[64];
    uint64_t shoff, file_size;
    uint16_t shnum, shentsize;
    uint8_t *sh = NULL;
    int rc = -1;
    size_t count = 0, cap = 0;
    Sym *syms = NULL;
    int i;

    *out = NULL;
    *n = 0;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) goto done;
    {
        long end = ftell(f);
        if (end < 64) goto done;
        file_size = (uint64_t)end;
    }
    if (fseek(f, 0, SEEK_SET) != 0 || fread(eh, 1, 64, f) != 64)
        goto done;
    if (memcmp(eh, "\x7f" "ELF", 4) != 0 || eh[4] != 2 || eh[5] != 1)
        goto done;
    memcpy(&shoff, eh + 40, 8);
    memcpy(&shentsize, eh + 58, 2);
    memcpy(&shnum, eh + 60, 2);
    if (shentsize < 64 || shnum == 0 || shoff > file_size ||
        (uint64_t)shentsize * shnum > file_size - shoff ||
        shoff > (uint64_t)LONG_MAX)
        goto done;
    sh = malloc((size_t)shentsize * shnum);
    if (!sh) goto done;
    if (fseek(f, (long)shoff, SEEK_SET) != 0 ||
        fread(sh, shentsize, shnum, f) != shnum)
        goto done;

    for (i = 0; i < shnum; i++) {
        uint8_t *section = sh + (size_t)i * shentsize;
        uint32_t type, link;
        uint64_t off, size, entsize;
        uint8_t *tab = NULL;
        size_t j, nent;
        memcpy(&type, section + 4, 4);
        memcpy(&off, section + 24, 8);
        memcpy(&size, section + 32, 8);
        memcpy(&link, section + 40, 4);
        memcpy(&entsize, section + 56, 8);
        if (type != 2) continue;
        if (link >= shnum || entsize < 24 || size % entsize != 0 ||
            off > file_size || size > file_size - off ||
            off > (uint64_t)LONG_MAX || size > SIZE_MAX)
            goto done;
        tab = malloc((size_t)size);
        if (!tab) goto done;
        if (fseek(f, (long)off, SEEK_SET) != 0 ||
            fread(tab, 1, (size_t)size, f) != (size_t)size) {
            free(tab);
            goto done;
        }
        {
            uint8_t *strs = sh + (size_t)link * shentsize;
            uint64_t str_off, str_size;
            char *strtab;
            uint32_t str_type;
            memcpy(&str_type, strs + 4, 4);
            memcpy(&str_off, strs + 24, 8);
            memcpy(&str_size, strs + 32, 8);
            if (str_type != 3 || str_off > file_size ||
                str_size > file_size - str_off ||
                str_off > (uint64_t)LONG_MAX || str_size > SIZE_MAX - 1) {
                free(tab);
                goto done;
            }
            strtab = malloc((size_t)str_size + 1);
            if (!strtab || fseek(f, (long)str_off, SEEK_SET) != 0 ||
                fread(strtab, 1, (size_t)str_size, f) != (size_t)str_size) {
                free(strtab);
                free(tab);
                goto done;
            }
            strtab[str_size] = 0;
            nent = (size_t)(size / entsize);
            for (j = 1; j < nent; j++) {
                uint8_t *entry = tab + j * (size_t)entsize;
                uint32_t nameoff;
                uint8_t info;
                uint64_t value;
                char *name;
                memcpy(&nameoff, entry, 4);
                info = entry[4];
                memcpy(&value, entry + 8, 8);
                if (nameoff >= str_size || (info & 0x0f) != 2) continue;
                name = strtab + nameoff;
                if (!*name || !memchr(name, 0, (size_t)(str_size - nameoff)))
                    continue;
                if (count == cap) {
                    Sym *next;
                    cap = cap ? cap * 2 : 64;
                    next = realloc(syms, cap * sizeof(*syms));
                    if (!next) {
                        free(strtab);
                        free(tab);
                        goto done;
                    }
                    syms = next;
                }
                syms[count].name = debug_strdup(name);
                if (!syms[count].name) {
                    free(strtab);
                    free(tab);
                    goto done;
                }
                syms[count].addr = value;
                count++;
            }
            free(strtab);
        }
        free(tab);
    }
    *out = syms;
    *n = count;
    rc = 0;
done:
    if (rc != 0) {
        size_t j;
        for (j = 0; j < count; j++) free(syms[j].name);
        free(syms);
    }
    free(sh);
    fclose(f);
    return rc;
}
