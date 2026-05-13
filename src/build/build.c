#include "build.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* BUILD constants (only the ones we actually need) */
#define BUILD_MAGIC       0x464C457F
#define BUILDCLASS32      1
#define BUILDDATA2LSB     1
#define EV_CURRENT      1
#define ET_REL          1
#define EM_386          3
#define SHN_UNDEF       0
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_NOBITS      8
#define SHT_STRTAB      3
#define SHT_SYMTAB      2
#define SHF_WRITE       (1 << 0)
#define SHF_ALLOC       (1 << 1)
#define SHF_EXECINSTR   (1 << 2)
#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STT_NOTYPE      0
#define STT_OBJECT      1
#define STT_FUNC        2

/* Standard BUILD32 types */
typedef uint32_t Build32_Addr;
typedef uint32_t Build32_Off;
typedef uint16_t Build32_Half;
typedef uint32_t Build32_Word;
typedef int32_t  Build32_Sword;

/* BUILD header */
typedef struct {
    unsigned char e_ident[16];
    Build32_Half    e_type;
    Build32_Half    e_machine;
    Build32_Word    e_version;
    Build32_Addr    e_entry;
    Build32_Off     e_phoff;
    Build32_Off     e_shoff;
    Build32_Word    e_flags;
    Build32_Half    e_ehsize;
    Build32_Half    e_phentsize;
    Build32_Half    e_phnum;
    Build32_Half    e_shentsize;
    Build32_Half    e_shnum;
    Build32_Half    e_shstrndx;
} Build32_Ehdr;

/* Section header */
typedef struct {
    Build32_Word    sh_name;
    Build32_Word    sh_type;
    Build32_Word    sh_flags;
    Build32_Addr    sh_addr;
    Build32_Off     sh_offset;
    Build32_Word    sh_size;
    Build32_Word    sh_link;
    Build32_Word    sh_info;
    Build32_Word    sh_addralign;
    Build32_Word    sh_entsize;
} Build32_Shdr;

/* Symbol table entry */
typedef struct {
    Build32_Word    st_name;
    Build32_Addr    st_value;
    Build32_Word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    Build32_Half    st_shndx;
} Build32_Sym;

/* Internal representation of a section we are building */
typedef struct {
    char        name[32];        /* section name (limited to 31 chars) */
    Build32_Word  type;            /* SHT_PROGBITS or SHT_NOBITS */
    Build32_Word  flags;           /* SHF_ALLOC, SHF_EXECINSTR, etc. */
    uint8_t    *data;            /* raw data (NULL for .bss) */
    size_t      data_size;       /* size of data in bytes */
    uint32_t    alignment;       /* alignment */
    Build32_Word  name_offset;     /* offset in .shstrtab (filled later) */
    size_t      file_offset;     /* where data begins in the file (filled later) */
} SectionInfo;

/* Internal symbol representation */
typedef struct {
    char        name[64];        /* symbol name */
    Build32_Word  name_offset;     /* offset in .strtab */
    Build32_Addr  value;
    Build32_Word  size;
    unsigned char bind;
    unsigned char type;
    Build32_Half  shndx;
} SymbolInfo;

/* Top-level writer object */
struct BuildObjectWriter {
    const char  *output_path;
    FILE        *file;

    /* Sections we have added */
    SectionInfo sections[BUILD_WRITER_MAX_SECTIONS];
    int          section_count;

    /* Symbols we have added */
    SymbolInfo   symbols[BUILD_WRITER_MAX_SYMBOLS];
    int          symbol_count;

    /* Name of the entry point, if any */
    char         entry_name[64];
};

/* Helper: write a little-endian half-word to the file */
static int write_le16(FILE *f, uint16_t val) {
    uint8_t buf[2] = { val & 0xFF, (val >> 8) & 0xFF };
    return fwrite(buf, 1, 2, f) == 2 ? 0 : -1;
}

/* Helper: write a little-endian word */
static int write_le32(FILE *f, uint32_t val) {
    uint8_t buf[4] = {
        val & 0xFF,
        (val >> 8) & 0xFF,
        (val >> 16) & 0xFF,
        (val >> 24) & 0xFF
    };
    return fwrite(buf, 1, 4, f) == 4 ? 0 : -1;
}

/* Write the BUILD ident bytes (first 16 bytes of header) */
static int write_ident(FILE *f) {
    unsigned char ident[16] = {0};
    ident[0] = 0x7F;
    ident[1] = 'E';
    ident[2] = 'L';
    ident[3] = 'F';
    ident[4] = BUILDCLASS32;
    ident[5] = BUILDDATA2LSB;
    ident[6] = EV_CURRENT;
    ident[7] = 0;               /* OS/ABI: System V */
    ident[8] = 0;               /* ABI version */
    /* bytes 9-15 are padding */
    return fwrite(ident, 1, 16, f) == 16 ? 0 : -1;
}

/* Compute the size of the .shstrtab section (names of all sections) */
static size_t shstrtab_size(const SectionInfo *sections, int count) {
    size_t total = 1;  /* first byte is always NUL */
    for (int i = 0; i < count; i++) {
        total += strlen(sections[i].name) + 1;
    }
    return total;
}

/* Compute the size of the .strtab section (symbol names) */
static size_t strtab_size(const SymbolInfo *syms, int count) {
    size_t total = 1;
    for (int i = 0; i < count; i++) {
        total += strlen(syms[i].name) + 1;
    }
    return total;
}

/* Write the .shstrtab section and fill name_offset in each SectionInfo */
static int write_shstrtab(FILE *f, SectionInfo *sections, int count,
                          size_t *sizes, Build32_Off current_offset) {
    /* First byte must be '\0' */
    if (fputc('\0', f) == EOF) return -1;
    size_t offset = 1;

    for (int i = 0; i < count; i++) {
        sections[i].name_offset = (Build32_Word)offset;
        size_t len = strlen(sections[i].name) + 1;
        if (fwrite(sections[i].name, 1, len, f) != len) return -1;
        offset += len;
    }
    return 0;
}

/* Write the .strtab section and fill name_offset in each SymbolInfo */
static int write_strtab(FILE *f, SymbolInfo *syms, int count) {
    if (fputc('\0', f) == EOF) return -1;
    size_t offset = 1;

    for (int i = 0; i < count; i++) {
        syms[i].name_offset = (Build32_Word)offset;
        size_t len = strlen(syms[i].name) + 1;
        if (fwrite(syms[i].name, 1, len, f) != len) return -1;
        offset += len;
    }
    return 0;
}

/* Write all section data blobs (except .shstrtab and .strtab) */
static int write_section_data(FILE *f, SectionInfo *sections, int count,
                              size_t *file_offsets) {
    for (int i = 0; i < count; i++) {
        /* For SHT_NOBITS (.bss) we write nothing, but the offset still points
         * past the end of the previous section; file_offsets[i] already set. */
        if (sections[i].type == SHT_PROGBITS && sections[i].data != NULL) {
            if (fwrite(sections[i].data, 1, sections[i].data_size, f) != sections[i].data_size)
                return -1;
        }
    }
    return 0;
}

/* Write a single section header */
static int write_section_header(FILE *f, const SectionInfo *sec) {
    if (write_le32(f, sec->name_offset) != 0) return -1;
    if (write_le32(f, sec->type) != 0) return -1;
    if (write_le32(f, sec->flags) != 0) return -1;
    if (write_le32(f, sec->sh_addr) != 0) return -1;      /* address 0 in relocatable */
    if (write_le32(f, (uint32_t)sec->file_offset) != 0) return -1;
    if (write_le32(f, (uint32_t)sec->data_size) != 0) return -1;
    /* sh_link and sh_info are 0 for most sections, except symtab and strtab */
    if (write_le32(f, 0) != 0) return -1;
    if (write_le32(f, 0) != 0) return -1;
    if (write_le32(f, sec->alignment) != 0) return -1;
    if (write_le32(f, 0) != 0) return -1;  /* sh_entsize */
    return 0;
}

/* Write one symbol table entry */
static int write_symbol(FILE *f, const SymbolInfo *sym) {
    if (write_le32(f, sym->name_offset) != 0) return -1;
    if (write_le32(f, sym->value) != 0) return -1;
    if (write_le32(f, sym->size) != 0) return -1;
    unsigned char info = (sym->bind << 4) | (sym->type & 0xF);
    if (fputc(info, f) == EOF) return -1;
    if (fputc(0, f) == EOF) return -1;     /* st_other */
    if (write_le16(f, sym->shndx) != 0) return -1;
    return 0;
}

/* Public API implementations */

BuildObjectWriter* build__create(const char *output_path) {
    BuildObjectWriter *w = calloc(1, sizeof(BuildObjectWriter));
    if (!w) return NULL;
    w->output_path = output_path;

    /* The first symbol (index 0) is always the undefined null symbol.
     * It is required by the BUILD standard. */
    w->symbols[0].name[0] = '\0';
    w->symbols[0].name_offset = 0;
    w->symbols[0].value = 0;
    w->symbols[0].size = 0;
    w->symbols[0].bind = STB_LOCAL;
    w->symbols[0].type = STT_NOTYPE;
    w->symbols[0].shndx = SHN_UNDEF;
    w->symbol_count = 1;

    return w;
}

uint8_t build__add_section(BuildObjectWriter *w, SectionType type,
                         const char *name, const uint8_t *data,
                         size_t data_size, uint32_t alignment) {
    if (!w || w->section_count >= BUILD_WRITER_MAX_SECTIONS) return 0;
    if (!name || strlen(name) > 31) return 0;

    SectionInfo *sec = &w->sections[w->section_count];
    strncpy(sec->name, name, sizeof(sec->name) - 1);
    sec->name[sizeof(sec->name) - 1] = '\0';

    switch (type) {
    case SECTION_TEXT:
        sec->type = SHT_PROGBITS;
        sec->flags = SHF_ALLOC | SHF_EXECINSTR;
        break;
    case SECTION_DATA:
        sec->type = SHT_PROGBITS;
        sec->flags = SHF_ALLOC | SHF_WRITE;
        break;
    case SECTION_BSS:
        sec->type = SHT_NOBITS;
        sec->flags = SHF_ALLOC | SHF_WRITE;
        break;
    default:
        return 0;
    }

    sec->alignment = alignment;
    sec->data_size = data_size;

    /* For SHT_NOBITS we do not store any data */
    if (sec->type == SHT_PROGBITS) {
        sec->data = malloc(data_size);
        if (!sec->data) return 0;
        if (data) {
            memcpy(sec->data, data, data_size);
        } else {
            /* If no data pointer is given, fill with zeros */
            memset(sec->data, 0, data_size);
        }
    } else {
        sec->data = NULL;
    }

    w->section_count++;
    /* Section index: add 1 because the null section at index 0 will be
     * created automatically when we write the section header table. */
    return (uint8_t)(w->section_count);  /* first user section has index 1 */
}

int build__add_symbol(BuildObjectWriter *w, const BuildSymbol *sym) {
    if (!w || w->symbol_count >= BUILD_WRITER_MAX_SYMBOLS) return -1;
    if (!sym || !sym->name || strlen(sym->name) > 63) return -1;

    SymbolInfo *si = &w->symbols[w->symbol_count];
    strncpy(si->name, sym->name, sizeof(si->name) - 1);
    si->name[sizeof(si->name) - 1] = '\0';
    si->value = sym->value;
    si->size = sym->size;
    si->bind = (sym->binding == SYMBOL_GLOBAL) ? STB_GLOBAL : STB_LOCAL;
    /* Guess type: function if section is text, otherwise object.
     * A more sophisticated writer would let the caller specify it. */
    if (sym->section_index > 0 && sym->section_index <= (uint8_t)w->section_count) {
        SectionInfo *sec = &w->sections[sym->section_index - 1];
        si->type = (sec->flags & SHF_EXECINSTR) ? STT_FUNC : STT_OBJECT;
    } else {
        si->type = STT_NOTYPE;
    }
    si->shndx = sym->section_index;

    int idx = w->symbol_count;
    w->symbol_count++;
    return idx;
}

void build__set_entry(BuildObjectWriter *w, const char *entry_name) {
    if (w && entry_name) {
        strncpy(w->entry_name, entry_name, sizeof(w->entry_name) - 1);
        w->entry_name[sizeof(w->entry_name) - 1] = '\0';
    }
}

int build__finalize(BuildObjectWriter *w) {
    if (!w) return -1;

    w->file = fopen(w->output_path, "wb");
    if (!w->file) {
        perror("build__finalize: fopen");
        free(w);
        return -1;
    }

    /* The number of section headers includes:
     *   - the mandatory null section (index 0)
     *   - one for each user section
     *   - .shstrtab
     *   - .symtab
     *   - .strtab
     * So total sections = 1 (null) + user_sections + 3.
     */
    int user_count = w->section_count;
    int total_sections = 1 + user_count + 3;  /* null, user, shstrtab, symtab, strtab */
    int shstrtab_index = 1 + user_count;      /* .shstrtab section index */
    int symtab_index   = 1 + user_count + 1;  /* .symtab */
    int strtab_index   = 1 + user_count + 2;  /* .strtab */

    /* Build dummy sections for the special string and symbol tables */
    SectionInfo special[3];
    memset(special, 0, sizeof(special));
    strcpy(special[0].name, ".shstrtab");
    special[0].type = SHT_STRTAB;
    special[0].flags = 0;
    special[0].data_size = shstrtab_size(w->sections, user_count);
    special[0].alignment = 1;

    strcpy(special[1].name, ".symtab");
    special[1].type = SHT_SYMTAB;
    special[1].flags = 0;
    special[1].data_size = w->symbol_count * sizeof(Build32_Sym);
    special[1].alignment = 4;
    special[1].sh_link = strtab_index;  /* link to string table for symbols */

    strcpy(special[2].name, ".strtab");
    special[2].type = SHT_STRTAB;
    special[2].flags = 0;
    special[2].data_size = strtab_size(w->symbols, w->symbol_count);
    special[2].alignment = 1;

    /* Plan file layout:
     * 1. BUILD header
     * 2. Section data (user sections in order, then .shstrtab, .symtab, .strtab)
     * 3. Section header table at the end.
     * We need to calculate file offsets for each section.
     */
    /* Current offset after BUILD header */
    size_t current_offset = sizeof(Build32_Ehdr);

    /* For each user section, record its file offset, then advance */
    for (int i = 0; i < user_count; i++) {
        w->sections[i].file_offset = current_offset;
        /* For SHT_NOBITS no data is written, but we still reserve no space */
        if (w->sections[i].type == SHT_PROGBITS)
            current_offset += w->sections[i].data_size;
    }

    /* Now the three special sections */
    special[0].file_offset = current_offset;
    current_offset += special[0].data_size;  /* .shstrtab data */
    special[1].file_offset = current_offset;
    current_offset += special[1].data_size;  /* .symtab data */
    special[2].file_offset = current_offset;
    current_offset += special[2].data_size;  /* .strtab data */

    /* Section header table will be written at this offset */
    size_t shoff = current_offset;

    /* Determine entry point value: look for the symbol named as entry */
    Build32_Addr entry = 0;
    if (w->entry_name[0] != '\0') {
        for (int i = 0; i < w->symbol_count; i++) {
            if (strcmp(w->symbols[i].name, w->entry_name) == 0) {
                entry = w->symbols[i].value;
                break;
            }
        }
    }

    /* Write BUILD header */
    if (write_ident(w->file) != 0) goto fail;
    if (write_le16(w->file, ET_REL) != 0) goto fail;  /* relocatable */
    if (write_le16(w->file, EM_386) != 0) goto fail;
    if (write_le32(w->file, EV_CURRENT) != 0) goto fail;
    if (write_le32(w->file, entry) != 0) goto fail;   /* e_entry */
    if (write_le32(w->file, 0) != 0) goto fail;       /* e_phoff (no program header) */
    if (write_le32(w->file, (uint32_t)shoff) != 0) goto fail; /* e_shoff */
    if (write_le32(w->file, 0) != 0) goto fail;       /* e_flags */
    if (write_le16(w->file, sizeof(Build32_Ehdr)) != 0) goto fail; /* e_ehsize */
    if (write_le16(w->file, 0) != 0) goto fail;       /* e_phentsize */
    if (write_le16(w->file, 0) != 0) goto fail;       /* e_phnum */
    if (write_le16(w->file, sizeof(Build32_Shdr)) != 0) goto fail; /* e_shentsize */
    if (write_le16(w->file, (Build32_Half)total_sections) != 0) goto fail; /* e_shnum */
    if (write_le16(w->file, (Build32_Half)shstrtab_index) != 0) goto fail; /* e_shstrndx */

    /* Write user section data */
    if (write_section_data(w->file, w->sections, user_count, NULL) != 0) goto fail;

    /* Write .shstrtab data */
    if (write_shstrtab(w->file, w->sections, user_count, NULL, 0) != 0) goto fail;

    /* Write .symtab data: each symbol as Build32_Sym */
    for (int i = 0; i < w->symbol_count; i++) {
        if (write_symbol(w->file, &w->symbols[i]) != 0) goto fail;
    }

    /* Write .strtab data */
    if (write_strtab(w->file, w->symbols, w->symbol_count) != 0) goto fail;

    /* Now write the section header table:
     * First the null section header (all zeros) */
    Build32_Shdr null_shdr;
    memset(&null_shdr, 0, sizeof(null_shdr));
    if (fwrite(&null_shdr, sizeof(null_shdr), 1, w->file) != 1) goto fail;

    /* Then user section headers. Their name_offset is already set by
     * write_shstrtab() (they point into .shstrtab). */
    for (int i = 0; i < user_count; i++) {
        if (write_section_header(w->file, &w->sections[i]) != 0) goto fail;
    }

    /* Headers for .shstrtab, .symtab, .strtab */
    /* .shstrtab */
    if (write_section_header(w->file, &special[0]) != 0) goto fail;
    /* .symtab: need to set sh_link and sh_info properly.
     * sh_link points to the associated string table.
     * sh_info = index of the first non-local symbol (one greater than
     * the number of local symbols). */
    {
        /* Count local symbols (including the null symbol if local) */
        int first_global = 0;
        while (first_global < w->symbol_count &&
               w->symbols[first_global].bind == STB_LOCAL)
            first_global++;
        special[1].sh_link = strtab_index;
        special[1].sh_info = first_global;
        special[1].sh_entsize = sizeof(Build32_Sym);  /* entsize for symtab */
        if (write_section_header(w->file, &special[1]) != 0) goto fail;
    }
    /* .strtab */
    if (write_section_header(w->file, &special[2]) != 0) goto fail;

    fclose(w->file);
    /* Free any allocated section data */
    for (int i = 0; i < user_count; i++) {
        free(w->sections[i].data);
    }
    free(w);
    return 0;

fail:
    fclose(w->file);
    for (int i = 0; i < user_count; i++) {
        free(w->sections[i].data);
    }
    free(w);
    return -1;
}
