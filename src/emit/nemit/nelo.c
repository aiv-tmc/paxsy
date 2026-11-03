#include "common.h"
#include "nelo.h"

#include <stdio.h>

/*
 * nelo.c - ELF64 relocatable-object serialisation. The file layout
 * is: ELF header, .text, .rodata, .data, .rela.text, .symtab,
 * .strtab, .shstrtab, then the section headers. The relocation
 * stream uses RELA entries: the addend lives in the entry itself,
 * so the placeholder bytes in .text are written as zeros and the
 * consumer computes section_base + addend for every absolute
 * address constant. The symbol table starts with the mandatory
 * null entry followed by one STT_SECTION symbol per allocatable
 * section; the caller's globals come last and are all global
 * bindings, so sh_info of .symtab is fixed at 4.
 */

#define NELO_SHT_NULL     0
#define NELO_SHT_PROGBITS 1
#define NELO_SHT_SYMTAB   2
#define NELO_SHT_STRTAB   3
#define NELO_SHT_RELA     4

#define NELO_SHF_WRITE    0x1
#define NELO_SHF_ALLOC    0x2
#define NELO_SHF_EXEC     0x4

#define NELO_R_X86_64_64  1

#define NELO_STT_OBJECT   1
#define NELO_STT_FUNC     2
#define NELO_STT_SECTION  3

/* Section-header name offsets into NELO_SHSTR. */
static const char NELO_SHSTR[] =
    "\0.text\0.rodata\0.data\0.rela.text\0.symtab\0.strtab\0.shstrtab\0";
enum {
    NELO_N_TEXT = 1, NELO_N_RODATA = 7, NELO_N_DATA = 15,
    NELO_N_RELA = 21, NELO_N_SYMTAB = 31, NELO_N_STRTAB = 39,
    NELO_N_SHSTRTAB = 47
};

static void wr(FILE *f, const void *p, size_t n) { fwrite(p, 1, n, f); }
static void w8(FILE *f, uint8_t v)  { wr(f, &v, 1); }
static void w16(FILE *f, uint16_t v) { wr(f, &v, 2); }
static void w32(FILE *f, uint32_t v) { wr(f, &v, 4); }
static void w64(FILE *f, uint64_t v) { wr(f, &v, 8); }

/* Writes one section header at the given file offset. */
static void shdr(FILE *f, uint32_t name, uint32_t type, uint64_t flags,
                 uint64_t off, uint64_t size, uint32_t link,
                 uint32_t info, uint64_t align, uint64_t entsize) {
    w32(f, name);
    w32(f, type);
    w64(f, flags);
    w64(f, 0);            /* sh_addr: unused in ET_REL */
    w64(f, off);
    w64(f, size);
    w32(f, link);
    w32(f, info);
    w64(f, align);
    w64(f, entsize);
}

/* Writes one symbol-table entry. */
static void sym(FILE *f, uint32_t name, uint8_t info, uint16_t shndx,
                uint64_t value, uint64_t size) {
    w32(f, name);
    w8(f, info);
    w8(f, 0);             /* st_other */
    w16(f, shndx);
    w64(f, value);
    w64(f, size);
}

int nelo_write(const char *path,
               const uint8_t *text, size_t text_len,
               const uint8_t *ro, size_t ro_len,
               const uint8_t *data, size_t data_len,
               const NESym *syms, int nsyms,
               const NEReloc *relocs, int nrelocs) {
    /* String table: the leading zero byte is the null symbol's
     * name; caller names are appended in order. */
    size_t str_len = 1;
    for (int i = 0; i < nsyms; i++) str_len += strlen(syms[i].name) + 1;

    uint64_t off = 64;                       /* ELF header size      */
    uint64_t off_text = off; off += (text_len + 15u) & ~15u;
    uint64_t off_ro = off; off += (ro_len + 15u) & ~15u;
    uint64_t off_data = off; off += (data_len + 7u) & ~7u;
    uint64_t off_rela = off;
    uint64_t rela_len = (uint64_t)nrelocs * 24u;
    off += rela_len;
    uint64_t off_sym = off;
    uint64_t sym_len = ((uint64_t)nsyms + 4u) * 24u;
    off += sym_len;
    uint64_t off_str = off; off += (str_len + 7u) & ~7u;
    uint64_t off_shstr = off;
    uint64_t shstr_len = sizeof(NELO_SHSTR) - 1;
    off += (shstr_len + 7u) & ~7u;
    uint64_t off_shdr = (off + 7u) & ~7u;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    /* ELF header: ET_REL, no program headers, 8 section headers. */
    static const uint8_t ident[16] = {
        0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    wr(f, ident, 16);
    w16(f, 1);                       /* ET_REL    */
    w16(f, 0x3E);                    /* EM_X86_64 */
    w32(f, 1);
    w64(f, 0);                       /* e_entry: none   */
    w64(f, 0);                       /* e_phoff: none   */
    w64(f, off_shdr);
    w32(f, 0);
    w16(f, 64);
    w16(f, 56);
    w16(f, 0);                       /* e_phnum   */
    w16(f, 64);
    w16(f, 8);                       /* e_shnum   */
    w16(f, 7);                       /* shstrtab  */

    /* .text with every absolute placeholder zeroed; the relocations
     * recorded below tell the consumer where the addends belong.
     * Caller relocations arrive in ascending position order because
     * the emitter appends them as the code grows. */
    fseek(f, (long)off_text, SEEK_SET);
    {
        uint64_t cur = 0;
        for (int i = 0; i < nrelocs; i++) {
            uint64_t p = relocs[i].pos;
            if (p > cur) wr(f, text + cur, (size_t)(p - cur));
            w64(f, 0);
            cur = p + 8;
        }
        if (cur < text_len) wr(f, text + cur, (size_t)(text_len - cur));
    }

    /* .rodata and .data are pure bytes with no fixups of their own. */
    fseek(f, (long)off_ro, SEEK_SET);
    wr(f, ro, ro_len);
    fseek(f, (long)off_data, SEEK_SET);
    wr(f, data, data_len);

    /* .rela.text: offset inside .text, section symbol of the target
     * section, section-relative addend. */
    fseek(f, (long)off_rela, SEEK_SET);
    for (int i = 0; i < nrelocs; i++) {
        w64(f, relocs[i].pos);
        w64(f, ((uint64_t)relocs[i].sect << 32) | NELO_R_X86_64_64);
        w64(f, relocs[i].addend);
    }

    /* .symtab: null, the three section symbols, then the globals. */
    fseek(f, (long)off_sym, SEEK_SET);
    sym(f, 0, 0, 0, 0, 0);
    sym(f, 0, NELO_STT_SECTION, NELO_SEC_TEXT, 0, 0);
    sym(f, 0, NELO_STT_SECTION, NELO_SEC_RODATA, 0, 0);
    sym(f, 0, NELO_STT_SECTION, NELO_SEC_DATA, 0, 0);
    {
        uint32_t name_off = 1;
        for (int i = 0; i < nsyms; i++) {
            sym(f, name_off, syms[i].info, syms[i].shndx,
                syms[i].value, syms[i].size);
            name_off += (uint32_t)strlen(syms[i].name) + 1;
        }
    }

    /* .strtab and .shstrtab. */
    fseek(f, (long)off_str, SEEK_SET);
    w8(f, 0);
    for (int i = 0; i < nsyms; i++) {
        wr(f, syms[i].name, strlen(syms[i].name));
        w8(f, 0);
    }
    fseek(f, (long)off_shstr, SEEK_SET);
    wr(f, NELO_SHSTR, shstr_len);

    /* Section headers. sh_info of .symtab is 4: the caller's globals
     * start right after null + three section symbols. */
    fseek(f, (long)off_shdr, SEEK_SET);
    shdr(f, 0, NELO_SHT_NULL, 0, 0, 0, 0, 0, 0, 0);
    shdr(f, NELO_N_TEXT, NELO_SHT_PROGBITS,
         NELO_SHF_ALLOC | NELO_SHF_EXEC, off_text, text_len, 0, 0, 16, 0);
    shdr(f, NELO_N_RODATA, NELO_SHT_PROGBITS,
         NELO_SHF_ALLOC, off_ro, ro_len, 0, 0, 16, 0);
    shdr(f, NELO_N_DATA, NELO_SHT_PROGBITS,
         NELO_SHF_ALLOC | NELO_SHF_WRITE, off_data, data_len, 0, 0, 8, 0);
    shdr(f, NELO_N_RELA, NELO_SHT_RELA, 0, off_rela, rela_len, 5, 1, 8, 24);
    shdr(f, NELO_N_SYMTAB, NELO_SHT_SYMTAB, 0, off_sym, sym_len, 6, 4, 8, 24);
    shdr(f, NELO_N_STRTAB, NELO_SHT_STRTAB, 0, off_str, str_len, 0, 0, 1, 0);
    shdr(f, NELO_N_SHSTRTAB, NELO_SHT_STRTAB, 0, off_shstr, shstr_len,
         0, 0, 1, 0);

    fclose(f);
    return 0;
}
