#ifndef BUILD_H
#define BUILD_H

#include <stdint.h>
#include <stddef.h>

/* Opaque handle to an object file under construction */
typedef struct BuildObjectWriter BuildObjectWriter;

/* Maximum number of sections we support in a single object */
#define BUILD_WRITER_MAX_SECTIONS 16
/* Maximum number of symbols */
#define BUILD_WRITER_MAX_SYMBOLS  256

/* Section types */
typedef enum {
    SECTION_TEXT,   /* executable code (.text) */
    SECTION_DATA,   /* initialized data (.data) */
    SECTION_BSS     /* uninitialized data (.bss) */
} SectionType;

/* Symbol binding */
typedef enum {
    SYMBOL_LOCAL,
    SYMBOL_GLOBAL
} SymbolBinding;

/* Description of a single symbol to be placed in the symbol table */
typedef struct {
    const char *name;        /* symbol name (must be null-terminated) */
    uint32_t    value;       /* address or offset within the section */
    uint32_t    size;        /* size of the object in bytes */
    uint8_t     section_index; /* index of the section (0 = UNDEF) */
    SymbolBinding binding;   /* local or global */
} BuildSymbol;

/* Create a new object writer instance. The file will be written to
 * output_path when build__finalize() is called. Returns NULL on error.
 */
BuildObjectWriter* build__create(const char *output_path);

/* Add a new section to the object file.
 * type      - kind of section (text, data, bss)
 * name      - section name (e.g., ".text", ".data", ".bss"); will be copied
 * data      - pointer to the raw bytes for the section (NULL for .bss)
 * data_size - number of bytes (section size); for .bss this is the
 *             amount of zero-initialized space required
 * alignment - power-of-two alignment (e.g., 4 means 16-byte aligned)
 * Returns the section index (1-based) that can be used in symbol
 * definitions, or 0 on error.
 */
uint8_t build__add_section(BuildObjectWriter *w, SectionType type,
                         const char *name, const uint8_t *data,
                         size_t data_size, uint32_t alignment);

/* Add a symbol definition to the symbol table.
 * Returns the symbol index (0-based) in the symbol table, or -1 on error.
 * The first symbol (index 0) is always the undefined symbol placeholder.
 */
int build__add_symbol(BuildObjectWriter *w, const BuildSymbol *sym);

/* Set the entry point symbol name. If non-NULL, the symbol's value
 * will be used as the BUILD entry point. This is optional.
 */
void build__set_entry(BuildObjectWriter *w, const char *entry_name);

/* Finalize the object file: write all data to disk and close.
 * Returns 0 on success, non-zero on error.
 * The BuildObjectWriter handle is freed by this call.
 */
int build__finalize(BuildObjectWriter *w);

#endif
