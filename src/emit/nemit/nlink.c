#include "common.h"
#include "nlink.h"
#include "nlink_int.h"
#include "nelf.h"

#include <string.h>
#include <limits.h>
#include <sys/stat.h>

/* Combines loaded objects, resolves relocations, and writes ELF output. */
int nlink_exec(Diag *diag, const char *out_path, char **objs, int nobjs) {
    NLObj *loaded = calloc(NL_MAX_OBJS, sizeof(*loaded));
    uint8_t *text = NULL, *ro = NULL, *data = NULL;
    const char **symbol_names = NULL;
    uint32_t *symbol_offsets = NULL;
    char *string_table = NULL;
    int count = 0, result = 1, symbol_count = 0;
    size_t text_len = 0, ro_len = 0, data_len = 0;
    uint64_t entry = UINT64_MAX;
    uint32_t string_len = 1;
    NELayout layout;

    if (!loaded) {
        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                    "linker: out of memory while allocating object list");
        return 1;
    }
    for (int i = 0; i < nobjs; i++) {
        if (nlink_load_input(loaded, &count, objs[i], diag) != 0) goto cleanup;
    }
    if (count == 0) {
        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                    "linker: no object files");
        goto cleanup;
    }
    for (int i = 0; i < count; i++) {
        size_t aligned_text, aligned_ro, aligned_data;
        if (loaded[i].text_len > SIZE_MAX - 15u ||
            loaded[i].ro_len > SIZE_MAX - 15u ||
            loaded[i].data_len > SIZE_MAX - 7u) {
            diag_report(diag, DK_ERROR, "<link>", 0, 0,
                        "linker: input section alignment overflow");
            goto cleanup;
        }
        aligned_text = (loaded[i].text_len + 15u) & ~(size_t)15u;
        aligned_ro = (loaded[i].ro_len + 15u) & ~(size_t)15u;
        aligned_data = (loaded[i].data_len + 7u) & ~(size_t)7u;
        if (aligned_text > SIZE_MAX - text_len ||
            aligned_ro > SIZE_MAX - ro_len ||
            aligned_data > SIZE_MAX - data_len) {
            diag_report(diag, DK_ERROR, "<link>", 0, 0,
                        "linker: combined section size overflow");
            goto cleanup;
        }
        loaded[i].text_base = text_len;
        loaded[i].ro_base = ro_len;
        loaded[i].data_base = data_len;
        text_len += aligned_text;
        ro_len += aligned_ro;
        data_len += aligned_data;
    }
    text = calloc(text_len ? text_len : 1, 1);
    ro = calloc(ro_len ? ro_len : 1, 1);
    data = calloc(data_len ? data_len : 1, 1);
    if (!text || !ro || !data) {
        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                    "linker: out of memory while combining sections");
        goto cleanup;
    }
    for (int i = 0; i < count; i++) {
        if (loaded[i].text_len) memcpy(text + loaded[i].text_base,
                                       loaded[i].text, loaded[i].text_len);
        if (loaded[i].ro_len) memcpy(ro + loaded[i].ro_base,
                                     loaded[i].ro, loaded[i].ro_len);
        if (loaded[i].data_len) memcpy(data + loaded[i].data_base,
                                       loaded[i].data, loaded[i].data_len);
    }
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < loaded[i].nsym; j++) {
            if (strcmp(loaded[i].sym[j].name, "_start") == 0) {
                if (loaded[i].sym[j].sect != 1) {
                    diag_report(diag, DK_ERROR, "<link>", 0, 0,
                                "linker: _start is not in the text section");
                    goto cleanup;
                }
                if (entry != UINT64_MAX) {
                    diag_report(diag, DK_ERROR, "<link>", 0, 0,
                                "linker: duplicate _start");
                    goto cleanup;
                }
                entry = loaded[i].text_base + loaded[i].sym[j].value;
            }
            for (int k = i + 1; k < count; k++) {
                for (int m = 0; m < loaded[k].nsym; m++) {
                    if (strcmp(loaded[i].sym[j].name,
                               loaded[k].sym[m].name) == 0) {
                        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                                    "linker: duplicate symbol '%s'",
                                    loaded[i].sym[j].name);
                        goto cleanup;
                    }
                }
            }
        }
    }
    if (entry == UINT64_MAX) {
        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                    "linker: _start not found");
        goto cleanup;
    }
    if (text_len > UINT32_MAX || entry > UINT32_MAX) {
        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                    "linker: text section exceeds the executable format limit");
        goto cleanup;
    }
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < loaded[i].nsym; j++) {
            if (loaded[i].sym[j].sect == 1) {
                size_t add = strlen(loaded[i].sym[j].name) + 1;
                if (add > UINT32_MAX - string_len || symbol_count == INT_MAX) {
                    diag_report(diag, DK_ERROR, "<link>", 0, 0,
                                "linker: symbol table is too large");
                    goto cleanup;
                }
                string_len += (uint32_t)add;
                symbol_count++;
            }
        }
    }
    symbol_names = calloc((size_t)(symbol_count ? symbol_count : 1),
                          sizeof(*symbol_names));
    symbol_offsets = calloc((size_t)(symbol_count ? symbol_count : 1),
                            sizeof(*symbol_offsets));
    string_table = calloc(string_len, 1);
    if (!symbol_names || !symbol_offsets || !string_table) {
        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                    "linker: out of memory while building symbol table");
        goto cleanup;
    }
    {
        uint32_t index = 0, offset = 1;
        for (int i = 0; i < count; i++) {
            for (int j = 0; j < loaded[i].nsym; j++) {
                if (loaded[i].sym[j].sect != 1) continue;
                symbol_names[index] = loaded[i].sym[j].name;
                symbol_offsets[index] = (uint32_t)(loaded[i].text_base +
                                                    loaded[i].sym[j].value);
                strcpy(string_table + offset, loaded[i].sym[j].name);
                offset += (uint32_t)strlen(loaded[i].sym[j].name) + 1;
                index++;
            }
        }
    }
    nelf_layout(text_len, ro_len, data_len, 0, (uint32_t)entry, &layout,
                (uint32_t)symbol_count, string_len);
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < loaded[i].nrel; j++) {
            uint64_t position = loaded[i].text_base + loaded[i].rel[j].pos;
            uint64_t target;
            uint64_t value;
            if (loaded[i].rel[j].pos > loaded[i].text_len ||
                loaded[i].text_len - loaded[i].rel[j].pos < 4 ||
                position > text_len || text_len - position < 4) {
                diag_report(diag, DK_ERROR, "<link>", 0, 0,
                            "linker: relocation is outside the text section");
                goto cleanup;
            }
            target = loaded[i].rel[j].sect == 2
                ? layout.va_ro + loaded[i].ro_base
                : (loaded[i].rel[j].sect == 1
                    ? layout.va_text + loaded[i].text_base
                    : layout.va_data + loaded[i].data_base);
            value = target + loaded[i].rel[j].addend;
            if (loaded[i].rel[j].type == 2) {
                uint64_t place = layout.va_text + position;
                uint64_t displacement = value - (place + 4);
                uint32_t low = (uint32_t)displacement;
                int32_t relative;
                memcpy(&relative, &low, sizeof(relative));
                if ((uint64_t)(int64_t)relative != displacement) {
                    diag_report(diag, DK_ERROR, "<link>", 0, 0,
                                "linker: PC-relative relocation is out of range");
                    goto cleanup;
                }
                memcpy(text + position, &relative, 4);
            } else if (loaded[i].rel[j].type == 1 &&
                       text_len - position >= 8) {
                memcpy(text + position, &value, 8);
            } else {
                diag_report(diag, DK_ERROR, "<link>", 0, 0,
                            "linker: unsupported relocation type %u",
                            loaded[i].rel[j].type);
                goto cleanup;
            }
        }
    }
    if (nelf_write(out_path, &layout, text, text_len, ro, ro_len, data,
                   data_len, 0, symbol_names, symbol_offsets,
                   (uint32_t)symbol_count, string_table, string_len) != 0) {
        diag_report(diag, DK_ERROR, "<link>", 0, 0,
                    "linker: cannot write '%s'", out_path);
        goto cleanup;
    }
    chmod(out_path, 0755);
    result = 0;
cleanup:
    free(text);
    free(ro);
    free(data);
    free(symbol_names);
    free(symbol_offsets);
    free(string_table);
    nlink_objs_release(loaded, count);
    return result;
}

