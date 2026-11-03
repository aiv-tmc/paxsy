#include "common.h"
#include "nelf.h"

/*
 * nelf.c - static ELF64 executable serialisation. The image begins
 * with the ELF header and both program headers, followed by the code
 * page, the read-only data, the writable data, the section-header
 * string table and the section headers themselves. The section-
 * header string table sits in the padding between the writable data and
 * the section headers so it never overlaps a mapped region. The
 * program headers drive the mapping (the kernel never reads the
 * section headers); the sections exist so inspection tools can name
 * the regions. All integers are written little-endian, matching the
 * x86-64 target.
 */

#include <stdint.h>

#define NE_BASE   0x400000u   /* conventional non-PIE load address */
#define NE_OFF_TX 0x1000u     /* code starts on the second page    */
#define NE_X      1u
#define NE_W      2u
#define NE_R      4u

/* Section-header string table, shared by layout and serialisation:
 * the name offsets below index into this exact byte sequence. */
static const char NE_SHSTR[] =
    "\0.text\0.rodata\0.data\0.bss\0.shstrtab\0";
#define NE_SHSTR_LEN ((uint32_t)(sizeof(NE_SHSTR) - 1))

static void wr(FILE *f, const void *p, size_t n) { fwrite(p, 1, n, f); }
static void w16(FILE *f, uint16_t v) { wr(f, &v, 2); }
static void w32(FILE *f, uint32_t v) { wr(f, &v, 4); }
static void w64(FILE *f, uint64_t v) { wr(f, &v, 8); }
static void wz(FILE *f, size_t n) {
    static const uint8_t zeros[64];
    while (n) { size_t k = n > 64 ? 64 : n; wr(f, zeros, k); n -= k; }
}

void nelf_layout(size_t text_len, size_t ro_len, size_t data_len,
                 size_t bss_len, uint32_t entry_off, NELayout *L) {
    size_t tx = (text_len + 15u) & ~15u;
    size_t ro = (ro_len + 15u) & ~15u;
    L->off_text = NE_OFF_TX;
    L->va_text = NE_BASE + NE_OFF_TX;
    L->off_ro = NE_OFF_TX + (uint32_t)tx;
    L->va_ro = L->va_text + tx;
    /* The writable segment starts on a fresh page so two load
     * segments never share a page with different permissions. */
    L->off_data = NE_OFF_TX + (uint32_t)((tx + ro + 0xFFFu) & ~0xFFFu);
    L->va_data = NE_BASE + L->off_data;
    L->entry = L->va_text + entry_off;
    /* The section headers follow the writable data plus the
     * string-table bytes; keeping the table inside the padding
     * guarantees it never overlaps the data mapping. */
    uint32_t after = L->off_data + (uint32_t)((data_len + 15u) & ~15u);
    uint32_t shoff = (after + NE_SHSTR_LEN + 7u) & ~7u;
    L->file_size = shoff + 6u * 64u;
    (void)bss_len;   /* the bss tail rides in the data-segment memsz */
}

int nelf_write(const char *path, const NELayout *L,
               const uint8_t *text, size_t text_len,
               const uint8_t *ro, size_t ro_len,
               const uint8_t *data, size_t data_len, size_t bss_len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t shoff = L->file_size - 6u * 64u;
    uint32_t shstr_off = shoff - NE_SHSTR_LEN;

    /* ELF header */
    static const uint8_t ident[16] = {
        0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    wr(f, ident, 16);
    w16(f, 2);                       /* ET_EXEC */
    w16(f, 0x3E);                    /* EM_X86_64 */
    w32(f, 1);                       /* EV_CURRENT */
    w64(f, L->entry);                /* e_entry   */
    w64(f, 64);                      /* e_phoff   */
    w64(f, shoff);                   /* e_shoff   */
    w32(f, 0);                       /* e_flags   */
    w16(f, 64);                      /* e_ehsize  */
    w16(f, 56);                      /* e_phentsize */
    w16(f, 2);                       /* phnum     */
    w16(f, 64);                      /* e_shentsize */
    w16(f, 6);                       /* shnum     */
    w16(f, 5);                       /* shstrtab index */

    /* Program header 1: headers + code + read-only data, R+X */
    w32(f, 1);                       /* PT_LOAD */
    w32(f, NE_R | NE_X);
    w64(f, 0);
    w64(f, NE_BASE);
    w64(f, NE_BASE);
    w64(f, L->off_ro + ro_len);
    w64(f, L->off_ro + ro_len);
    w64(f, 0x1000);

    /* Program header 2: writable data + bss, R+W */
    w32(f, 1);
    w32(f, NE_R | NE_W);
    w64(f, L->off_data);
    w64(f, L->va_data);
    w64(f, L->va_data);
    w64(f, data_len);
    w64(f, data_len + bss_len);
    w64(f, 0x1000);

    /* Section bodies */
    fseek(f, (long)L->off_text, SEEK_SET);
    wr(f, text, text_len);
    fseek(f, (long)L->off_ro, SEEK_SET);
    wr(f, ro, ro_len);
    fseek(f, (long)L->off_data, SEEK_SET);
    wr(f, data, data_len);

    /* Section-header string table, inside the padding before the
     * section headers. */
    fseek(f, (long)shstr_off, SEEK_SET);
    wr(f, NE_SHSTR, NE_SHSTR_LEN);

    /* Section headers */
    fseek(f, (long)shoff, SEEK_SET);
    wz(f, 64);                       /* section 0: the null section */
    /* .text */
    w32(f, 1); w32(f, 1); w64(f, 6);
    w64(f, L->va_text); w64(f, L->off_text);
    w64(f, text_len); w32(f, 0); w32(f, 0);
    w64(f, 16); w64(f, 0);
    /* .rodata */
    w32(f, 7); w32(f, 1); w64(f, 2);
    w64(f, L->va_ro); w64(f, L->off_ro);
    w64(f, ro_len); w32(f, 0); w32(f, 0);
    w64(f, 16); w64(f, 0);
    /* .data */
    w32(f, 15); w32(f, 1); w64(f, 3);
    w64(f, L->va_data); w64(f, L->off_data);
    w64(f, data_len); w32(f, 0); w32(f, 0);
    w64(f, 8); w64(f, 0);
    /* .bss */
    w32(f, 21); w32(f, 8); w64(f, 3);
    w64(f, L->va_data + data_len);
    w64(f, L->off_data + data_len);
    w64(f, bss_len); w32(f, 0); w32(f, 0);
    w64(f, 8); w64(f, 0);
    /* .shstrtab */
    w32(f, 26); w32(f, 3); w64(f, 0);
    w64(f, 0); w64(f, shstr_off);
    w64(f, NE_SHSTR_LEN); w32(f, 0); w32(f, 0);
    w64(f, 1); w64(f, 0);

    fclose(f);
    return 0;
}
