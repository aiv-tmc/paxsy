#include "common.h"
#include "npe.h"

#include <stdio.h>
#include <string.h>

/*
 * npe.c - PE32+ serialisation. File layout: 512 bytes of headers
 * (DOS stub with e_lfanew at 0x3C, "PE\0\0", COFF header, the
 * 240-byte PE32+ optional header, three 40-byte section headers),
 * then the raw section data aligned to the file alignment (512):
 * .text, .rdata, .data. Virtual layout: sections start at RVA
 * 0x1000 and step by their virtual sizes rounded to the section
 * alignment (4096). The optional header declares the console
 * subsystem, no DLL characteristics and an empty data-directory
 * array; SizeOfImage is the end of the last section rounded up.
 * The characteristics of each section follow the conventional
 * triple: code is execute+read, read-only data is read, writable
 * data is read+write.
 */

#define NPE_FILE_ALIGN 0x200
#define NPE_SECT_ALIGN 0x1000
#define NPE_E_LFANEW   0x80

static void w16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void padto(FILE *f, long off) {
    while (ftell(f) < off) fputc(0, f);
}

/* Writes one 40-byte section header. */
static void sect(FILE *f, const char *name, uint32_t vsize, uint32_t va,
                 uint32_t raw_size, uint32_t raw_off, uint32_t chars) {
    char nm[8] = {0};
    memcpy(nm, name, strlen(name) < 8 ? strlen(name) : 8);
    fwrite(nm, 1, 8, f);
    w32(f, vsize);
    w32(f, va);
    w32(f, raw_size);
    w32(f, raw_off);
    w32(f, 0);            /* PointerToRelocations    */
    w32(f, 0);            /* PointerToLinenumbers    */
    w16(f, 0);            /* NumberOfRelocations     */
    w16(f, 0);            /* NumberOfLinenumbers     */
    w32(f, chars);
}

int npe_write(const char *path,
              const uint8_t *text, size_t text_len,
              const uint8_t *ro, size_t ro_len,
              const uint8_t *data, size_t data_len,
              uint64_t image_base,
              uint64_t va_text, uint64_t va_ro, uint64_t va_data) {
    uint32_t rs_text = (uint32_t)((text_len + NPE_FILE_ALIGN - 1) &
                                  ~(size_t)(NPE_FILE_ALIGN - 1));
    uint32_t rs_ro = (uint32_t)((ro_len + NPE_FILE_ALIGN - 1) &
                                ~(size_t)(NPE_FILE_ALIGN - 1));
    uint32_t rs_data = (uint32_t)((data_len + NPE_FILE_ALIGN - 1) &
                                  ~(size_t)(NPE_FILE_ALIGN - 1));
    uint32_t off_text = NPE_FILE_ALIGN;
    uint32_t off_ro = off_text + rs_text;
    uint32_t off_data = off_ro + rs_ro;
    uint32_t last_end = (uint32_t)va_data +
        (uint32_t)((data_len + NPE_SECT_ALIGN - 1) & ~(size_t)(NPE_SECT_ALIGN - 1));
    uint32_t size_image = (last_end + NPE_SECT_ALIGN - 1) &
                          ~(uint32_t)(NPE_SECT_ALIGN - 1);

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    /* DOS header: magic, everything else zero, e_lfanew set. */
    {
        uint8_t dos[64];
        memset(dos, 0, sizeof(dos));
        dos[0] = 'M';
        dos[1] = 'Z';
        dos[0x3C] = NPE_E_LFANEW & 0xFF;
        dos[0x3D] = (NPE_E_LFANEW >> 8) & 0xFF;
        dos[0x3E] = (NPE_E_LFANEW >> 16) & 0xFF;
        dos[0x3F] = (NPE_E_LFANEW >> 24) & 0xFF;
        fwrite(dos, 1, sizeof(dos), f);
    }
    padto(f, NPE_E_LFANEW);
    fputs("PE\0\0", f);

    /* COFF header: x86-64, three sections, no symbol table. */
    w16(f, 0x8664);                    /* Machine                    */
    w16(f, 3);                         /* NumberOfSections           */
    w32(f, 0);                         /* TimeDateStamp              */
    w32(f, 0);                         /* PointerToSymbolTable       */
    w32(f, 0);                         /* NumberOfSymbols            */
    w16(f, 240);                       /* SizeOfOptionalHeader       */
    w16(f, 0x0002 | 0x0020);           /* EXECUTABLE_IMAGE | LARGE   */

    /* Optional header, PE32+. */
    w16(f, 0x20B);                     /* Magic                      */
    w16(f, 0);                         /* linker versions            */
    w32(f, rs_text);                   /* SizeOfCode                 */
    w32(f, rs_ro + rs_data);           /* SizeOfInitializedData      */
    w32(f, 0);                         /* SizeOfUninitializedData    */
    w32(f, (uint32_t)va_text);         /* AddressOfEntryPoint        */
    w32(f, (uint32_t)va_text);         /* BaseOfCode                 */
    w64(f, image_base);
    w32(f, NPE_SECT_ALIGN);
    w32(f, NPE_FILE_ALIGN);
    w16(f, 6); w16(f, 0);              /* OS version                 */
    w16(f, 0); w16(f, 0);              /* image version              */
    w16(f, 6); w16(f, 0);              /* subsystem version          */
    w32(f, 0);                         /* Win32VersionValue          */
    w32(f, size_image);
    w32(f, NPE_FILE_ALIGN);            /* SizeOfHeaders              */
    w32(f, 0);                         /* CheckSum                   */
    w16(f, 3);                         /* Subsystem: CONSOLE         */
    w16(f, 0);                         /* DllCharacteristics         */
    w64(f, 0x100000);                  /* SizeOfStackReserve         */
    w64(f, 0x1000);                    /* SizeOfStackCommit          */
    w64(f, 0x100000);                  /* SizeOfHeapReserve          */
    w64(f, 0x1000);                    /* SizeOfHeapCommit           */
    w32(f, 0);                         /* LoaderFlags                */
    w32(f, 16);                        /* NumberOfRvaAndSizes        */
    for (int i = 0; i < 16; i++) { w64(f, 0); w64(f, 0); }

    /* Section headers. */
    sect(f, ".text", (uint32_t)text_len, (uint32_t)va_text,
         rs_text, off_text, 0x60000020);
    sect(f, ".rdata", (uint32_t)ro_len, (uint32_t)va_ro,
         rs_ro, off_ro, 0x40000040);
    sect(f, ".data", (uint32_t)data_len, (uint32_t)va_data,
         rs_data, off_data, 0xC0000040);

    /* Raw data. */
    padto(f, off_text);
    fwrite(text, 1, text_len, f);
    padto(f, off_ro);
    fwrite(ro, 1, ro_len, f);
    padto(f, off_data);
    fwrite(data, 1, data_len, f);
    fclose(f);
    return 0;
}
