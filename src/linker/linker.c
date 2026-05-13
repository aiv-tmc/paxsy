#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Maximum number of input object files the linker can handle at once. */
#define MAX_OBJECTS 256

/* Maximum number of sections per object file. */
#define MAX_SECTIONS 16

/* Maximum number of symbols per object file. */
#define MAX_SYMBOLS 1024

/* Maximum number of relocations per section. */
#define MAX_RELOCS 4096

/*
 * Enumeration of supported executable formats.
 */
typedef enum {
    FORMAT_ELF,    /* Executable and Linking Format (Linux, BSD, etc.) */
    FORMAT_PE,     /* Portable Executable (Windows .exe) */
    FORMAT_MACHO   /* Mach-O (macOS, iOS) */
} OutputFormat;

/*
 * Representation of a section inside an object file.
 * Each section holds raw data and metadata needed for linking.
 */
typedef struct {
    char name[32];              /* Section name, e.g. ".text", ".data" */
    uint8_t *data;              /* Raw contents of the section */
    size_t size;                /* Size of the data in bytes */
    size_t offset_in_output;    /* Final offset after layout (set during linking) */
    uint32_t flags;             /* Section attributes: read/write/execute */
} Section;

/*
 * A symbol definition or reference. Symbols are the "glue" between
 * different object files and between the program and libraries.
 */
typedef struct {
    char name[64];              /* Symbol name */
    uint32_t section_index;     /* Index of the section this symbol belongs to,
                                   or special value for undefined/absolute */
    size_t value;               /* Offset within the section or absolute address */
    int is_defined;             /* 1 if the symbol provides a definition, 0 if undefined */
    int is_global;              /* 1 if the symbol is visible to other object files */
} Symbol;

/*
 * A single relocation entry. Relocations instruct the linker how to
 * patch section data once final addresses are known.
 */
typedef struct {
    uint32_t section_index;     /* Index of the section containing the reference */
    size_t offset;              /* Byte offset within the section where the fixup is applied */
    uint32_t symbol_index;      /* Index of the symbol this relocation refers to */
    int type;                   /* Relocation type (architecture-specific) */
    int64_t addend;             /* Constant addend used in the relocation formula */
} Relocation;

/*
 * Internal representation of one input object file (.o).
 * The linker fills this structure by parsing the raw file.
 */
typedef struct {
    char filename[256];         /* Original file name (for diagnostics) */
    Section sections[MAX_SECTIONS];
    int num_sections;
    Symbol symbols[MAX_SYMBOLS];
    int num_symbols;
    Relocation relocs[MAX_RELOCS];
    int num_relocs;
} ObjectFile;

/*
 * The central linker context. It holds all input files, the merged
 * output image and configuration parameters.
 */
typedef struct {
    ObjectFile objects[MAX_OBJECTS];
    int num_objects;

    OutputFormat output_format; /* Target executable format chosen by the user */

    char entry_symbol[64];      /* Name of the entry point (e.g. "_start") */
    size_t entry_address;       /* Final virtual address of the entry point (set during link) */

    /* Merged section table after symbol resolution and layout. */
    Section merged_sections[MAX_SECTIONS];
    int num_merged_sections;

    /* Output buffer containing the final executable image. */
    uint8_t *output_data;
    size_t output_size;
} Linker;

static int parse_elf_object(const char *filename, ObjectFile *obj);
static int parse_pe_object(const char *filename, ObjectFile *obj);
static int parse_macho_object(const char *filename, ObjectFile *obj);
static int resolve_symbols(Linker *linker);
static int merge_sections(Linker *linker);
static int perform_relocations(Linker *linker);
static int layout_sections(Linker *linker);
static int generate_elf_output(Linker *linker);
static int generate_pe_output(Linker *linker);
static int generate_macho_output(Linker *linker);
static void linker_error(const char *msg);

/*
 * Initialize a new linker session. All fields are set to safe defaults.
 * The caller must call linker_destroy() when done.
 */
void linker_init(Linker *linker) {
    memset(linker, 0, sizeof(*linker));
    linker->output_format = FORMAT_ELF;   /* default format */
    linker->entry_address = 0;
}

/*
 * Add an object file to the linker's internal list. The file is parsed
 * according to its format (ELF, COFF/PE object, Mach-O object) and all
 * sections, symbols and relocations are extracted into the ObjectFile
 * structure.
 */
int linker_add_object(Linker *linker, const char *filename) {
    if (linker->num_objects >= MAX_OBJECTS) {
        linker_error("Too many object files");
        return -1;
    }

    ObjectFile *obj = &linker->objects[linker->num_objects];

    /* Try to guess the object format from the file extension. A real
       linker would inspect the magic bytes at the start of the file. */
    const char *ext = strrchr(filename, '.');
    if (ext == NULL) {
        linker_error("Cannot determine object file format from extension");
        return -1;
    }

    int rc = -1;
    if (strcmp(ext, ".o") == 0 || strcmp(ext, ".elf") == 0) {
        rc = parse_elf_object(filename, obj);
    } else if (strcmp(ext, ".obj") == 0) {
        rc = parse_pe_object(filename, obj);
    } else if (strcmp(ext, ".macho") == 0) {
        rc = parse_macho_object(filename, obj);
    } else {
        linker_error("Unsupported object file format");
        return -1;
    }

    if (rc != 0) {
        linker_error("Failed to parse object file");
        return -1;
    }

    strncpy(obj->filename, filename, sizeof(obj->filename) - 1);
    obj->filename[sizeof(obj->filename) - 1] = '\0';
    linker->num_objects++;
    return 0;
}

/*
 * Choose the output executable format.
 * Must be called before linker_link().
 */
void linker_set_output_format(Linker *linker, OutputFormat fmt) {
    linker->output_format = fmt;
}

/*
 * Specify the entry point symbol name (e.g. "main" or "_start").
 * The linker will look for this symbol during resolution and record its
 * final address.
 */
void linker_set_entry(Linker *linker, const char *symbol_name) {
    strncpy(linker->entry_symbol, symbol_name, sizeof(linker->entry_symbol) - 1);
    linker->entry_symbol[sizeof(linker->entry_symbol) - 1] = '\0';
}

/*
 * Main linking procedure:
 *   1. Global symbol resolution across all object files.
 *   2. Section merging (combine sections with the same name from different files).
 *   3. Layout: assign final virtual addresses to every byte in every section.
 *   4. Relocation: apply all fixups using the final addresses.
 *   5. Generate the output image according to the chosen format.
 * Returns 0 on success, -1 on error.
 */
int linker_link(Linker *linker) {
    if (linker->num_objects == 0) {
        linker_error("No object files provided");
        return -1;
    }

    /* Phase 1: build a global symbol table and resolve undefined references. */
    if (resolve_symbols(linker) != 0) {
        linker_error("Symbol resolution failed");
        return -1;
    }

    /* Phase 2: merge sections from all objects into a unified set. */
    if (merge_sections(linker) != 0) {
        linker_error("Section merging failed");
        return -1;
    }

    /* Phase 3: assign final virtual addresses (layout). */
    if (layout_sections(linker) != 0) {
        linker_error("Layout failed");
        return -1;
    }

    /* Phase 4: apply relocations, patching section contents with final addresses. */
    if (perform_relocations(linker) != 0) {
        linker_error("Relocation failed");
        return -1;
    }

    /* Phase 5: create the final executable image in the requested format. */
    switch (linker->output_format) {
        case FORMAT_ELF:
            return generate_elf_output(linker);
        case FORMAT_PE:
            return generate_pe_output(linker);
        case FORMAT_MACHO:
            return generate_macho_output(linker);
        default:
            linker_error("Unknown output format");
            return -1;
    }
}

/*
 * Write the generated executable image to a file.
 * The file is created/truncated and the entire output data is written.
 */
int linker_write_to_file(Linker *linker, const char *outpath) {
    if (linker->output_data == NULL || linker->output_size == 0) {
        linker_error("No output data to write; call linker_link() first");
        return -1;
    }

    FILE *fp = fopen(outpath, "wb");
    if (!fp) {
        linker_error("Cannot open output file for writing");
        return -1;
    }

    size_t written = fwrite(linker->output_data, 1, linker->output_size, fp);
    fclose(fp);

    if (written != linker->output_size) {
        linker_error("Failed to write complete output file");
        return -1;
    }

    return 0;
}

/*
 * Free all resources allocated during the link process.
 */
void linker_destroy(Linker *linker) {
    for (int i = 0; i < linker->num_objects; i++) {
        ObjectFile *obj = &linker->objects[i];
        for (int j = 0; j < obj->num_sections; j++) {
            free(obj->sections[j].data);
        }
    }
    free(linker->output_data);
    memset(linker, 0, sizeof(*linker));
}

/*
 * Parse an ELF object file. In a full implementation this would read
 * ELF header, section headers, symbol table and relocation tables.
 * Here it is a stub that always succeeds with empty content.
 */
static int parse_elf_object(const char *filename, ObjectFile *obj) {
    (void)filename;   /* unused in stub */
    memset(obj, 0, sizeof(*obj));
    /* Example: create a dummy .text section and a "main" symbol */
    obj->num_sections = 1;
    strcpy(obj->sections[0].name, ".text");
    obj->sections[0].size = 64;
    obj->sections[0].data = (uint8_t *)calloc(1, 64);
    obj->sections[0].flags = 0x5; /* read + execute */

    obj->num_symbols = 1;
    strcpy(obj->symbols[0].name, "main");
    obj->symbols[0].section_index = 0; /* .text */
    obj->symbols[0].value = 0;
    obj->symbols[0].is_defined = 1;
    obj->symbols[0].is_global = 1;

    return 0;
}

/*
 * Parse a Windows COFF object file (.obj). Stub.
 */
static int parse_pe_object(const char *filename, ObjectFile *obj) {
    (void)filename;
    memset(obj, 0, sizeof(*obj));
    /* Minimal stub similar to ELF */
    obj->num_sections = 1;
    strcpy(obj->sections[0].name, ".text");
    obj->sections[0].size = 64;
    obj->sections[0].data = (uint8_t *)calloc(1, 64);
    obj->sections[0].flags = 0x5;
    obj->num_symbols = 1;
    strcpy(obj->symbols[0].name, "main");
    obj->symbols[0].section_index = 0;
    obj->symbols[0].value = 0;
    obj->symbols[0].is_defined = 1;
    obj->symbols[0].is_global = 1;
    return 0;
}

/*
 * Parse a Mach-O object file. Stub.
 */
static int parse_macho_object(const char *filename, ObjectFile *obj) {
    (void)filename;
    memset(obj, 0, sizeof(*obj));
    obj->num_sections = 1;
    strcpy(obj->sections[0].name, "__text");
    obj->sections[0].size = 64;
    obj->sections[0].data = (uint8_t *)calloc(1, 64);
    obj->sections[0].flags = 0x5;
    obj->num_symbols = 1;
    strcpy(obj->symbols[0].name, "_main");
    obj->symbols[0].section_index = 0;
    obj->symbols[0].value = 0;
    obj->symbols[0].is_defined = 1;
    obj->symbols[0].is_global = 1;
    return 0;
}

/*
 * Global symbol resolution.
 * Builds a merged symbol table from all object files, resolves
 * undefined references against definitions, and reports any
 * unresolved symbols.
 */
static int resolve_symbols(Linker *linker) {
    /*
     * In this simplified version we just verify that the entry symbol
     * exists somewhere. A full implementation would maintain a hash
     * table for fast lookup and handle multiple definitions and
     * common symbols.
     */
    int found_entry = 0;

    for (int i = 0; i < linker->num_objects; i++) {
        ObjectFile *obj = &linker->objects[i];
        for (int s = 0; s < obj->num_symbols; s++) {
            Symbol *sym = &obj->symbols[s];
            if (sym->is_defined && sym->is_global &&
                strcmp(sym->name, linker->entry_symbol) == 0) {
                found_entry = 1;
                /*
                 * Record a temporary value; the final address is
                 * computed during section layout.
                 */
                linker->entry_address = (size_t)-1; /* placeholder */
            }
        }
    }

    if (!found_entry && linker->entry_symbol[0] != '\0') {
        linker_error("Entry symbol not found");
        return -1;
    }

    return 0;
}

/*
 * Merge sections: for each unique section name, concatenate the
 * contents of all input sections with that name. The merged sections
 * will later be placed sequentially in the output.
 */
static int merge_sections(Linker *linker) {
    linker->num_merged_sections = 0;

    for (int i = 0; i < linker->num_objects; i++) {
        ObjectFile *obj = &linker->objects[i];
        for (int s = 0; s < obj->num_sections; s++) {
            Section *in_sec = &obj->sections[s];
            /* Look for an existing merged section with the same name. */
            int found = -1;
            for (int m = 0; m < linker->num_merged_sections; m++) {
                if (strcmp(linker->merged_sections[m].name, in_sec->name) == 0) {
                    found = m;
                    break;
                }
            }

            if (found == -1) {
                /* Create a new merged section entry. */
                if (linker->num_merged_sections >= MAX_SECTIONS) {
                    linker_error("Too many merged sections");
                    return -1;
                }
                found = linker->num_merged_sections++;
                strcpy(linker->merged_sections[found].name, in_sec->name);
                linker->merged_sections[found].size = 0;
                linker->merged_sections[found].data = NULL;
                linker->merged_sections[found].flags = in_sec->flags;
            }

            /*
             * Append the input section's data to the merged section.
             * The offset_in_output field is set during layout; here
             * we only remember where this chunk will sit relative to
             * the start of the merged section.
             */
            in_sec->offset_in_output = linker->merged_sections[found].size;

            size_t new_size = linker->merged_sections[found].size + in_sec->size;
            uint8_t *new_data = realloc(linker->merged_sections[found].data, new_size);
            if (!new_data) {
                linker_error("Out of memory during section merge");
                return -1;
            }
            memcpy(new_data + linker->merged_sections[found].size,
                   in_sec->data, in_sec->size);
            linker->merged_sections[found].data = new_data;
            linker->merged_sections[found].size = new_size;
        }
    }
    return 0;
}

/*
 * Layout: assign virtual addresses to every merged section and
 * compute the final address of the entry point. The layout strategy
 * shown here simply places sections one after another starting at
 * a base address (e.g. 0x400000 for 64-bit executables).
 */
static int layout_sections(Linker *linker) {
    size_t current_address = 0x400000; /* Typical base for x86-64 ELF */
    for (int m = 0; m < linker->num_merged_sections; m++) {
        linker->merged_sections[m].offset_in_output = current_address;
        current_address += linker->merged_sections[m].size;
        /* Align to 16 bytes for simplicity (real linkers use per‑section alignment) */
        if (current_address % 16)
            current_address += 16 - (current_address % 16);
    }

    /*
     * Walk through all symbols to find the entry point and compute
     * its final address. We use the merged section's base address
     * plus the symbol's original offset (now adjusted because the
     * input section was appended to a merged section).
     */
    if (linker->entry_address == (size_t)-1) { /* placeholder was set */
        for (int i = 0; i < linker->num_objects; i++) {
            ObjectFile *obj = &linker->objects[i];
            for (int s = 0; s < obj->num_symbols; s++) {
                Symbol *sym = &obj->symbols[s];
                if (strcmp(sym->name, linker->entry_symbol) == 0) {
                    Section *in_sec = &obj->sections[sym->section_index];
                    /* Find the merged section that contains this input section */
                    for (int m = 0; m < linker->num_merged_sections; m++) {
                        if (strcmp(linker->merged_sections[m].name, in_sec->name) == 0) {
                            linker->entry_address =
                                linker->merged_sections[m].offset_in_output
                                + in_sec->offset_in_output
                                + sym->value;
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }
    return 0;
}

/*
 * Relocation processing. For each relocation entry we compute the
 * final address of the referenced symbol and patch the appropriate
 * bytes in the merged section data.
 */
static int perform_relocations(Linker *linker) {
    for (int i = 0; i < linker->num_objects; i++) {
        ObjectFile *obj = &linker->objects[i];
        for (int r = 0; r < obj->num_relocs; r++) {
            Relocation *rel = &obj->relocs[r];
            /* Retrieve the referenced symbol. */
            Symbol *sym = &obj->symbols[rel->symbol_index];

            /* Calculate the final virtual address of the symbol. */
            size_t sym_addr = 0;
            if (sym->is_defined) {
                Section *in_sec = &obj->sections[sym->section_index];
                /* Find the merged section that contains this symbol. */
                for (int m = 0; m < linker->num_merged_sections; m++) {
                    if (strcmp(linker->merged_sections[m].name, in_sec->name) == 0) {
                        sym_addr = linker->merged_sections[m].offset_in_output
                                   + in_sec->offset_in_output
                                   + sym->value;
                        break;
                    }
                }
            } else {
                /* Undefined symbol – in a full linker this would be an error or a
                   reference to a shared library. For this demonstration we
                   leave the address as zero. */
            }

            /*
             * Compute the location inside the merged data that must be patched.
             * The input section's data has been copied into the merged section,
             * so we add the merged section's base and the input section's
             * offset within the merged data.
             */
            Section *target_in_sec = &obj->sections[rel->section_index];
            for (int m = 0; m < linker->num_merged_sections; m++) {
                if (strcmp(linker->merged_sections[m].name, target_in_sec->name) == 0) {
                    size_t patch_offset = target_in_sec->offset_in_output + rel->offset;
                    uint8_t *patch_loc = linker->merged_sections[m].data + patch_offset;

                    /*
                     * Apply a simple x86-64 PC-relative relocation (type 1).
                     * The formula: value = S + A - P, where S = symbol address,
                     * A = addend, P = address of the location being patched.
                     */
                    if (rel->type == 1) {
                        size_t P = linker->merged_sections[m].offset_in_output + patch_offset;
                        int64_t value = (int64_t)(sym_addr + rel->addend - P);
                        memcpy(patch_loc, &value, sizeof(int32_t)); /* 32-bit relative */
                    }
                    break;
                }
            }
        }
    }
    return 0;
}

/*
 * Build an ELF executable. This stub creates a minimal ELF file
 * containing the merged section data and an ELF header. In a
 * complete linker it would also generate program headers, a
 * section header table, and proper alignment.
 */
static int generate_elf_output(Linker *linker) {
    /* Simple estimation: ELF header + all merged sections. */
    size_t elf_header_size = 64; /* minimal ELF header */
    size_t total_size = elf_header_size;
    for (int m = 0; m < linker->num_merged_sections; m++) {
        total_size += linker->merged_sections[m].size;
    }

    linker->output_data = (uint8_t *)calloc(1, total_size);
    if (!linker->output_data) {
        linker_error("Out of memory for ELF output");
        return -1;
    }
    linker->output_size = total_size;

    /* Write a minimal ELF header (magic, class, etc.). */
    uint8_t *p = linker->output_data;
    memcpy(p, "\x7f" "ELF", 4); p += 4; /* ELF magic */
    *p++ = 2;  /* 64-bit */
    *p++ = 1;  /* little endian */
    *p++ = 1;  /* ELF version */
    /* ... many fields omitted ... */

    /* Copy merged section data sequentially. */
    size_t data_offset = elf_header_size;
    for (int m = 0; m < linker->num_merged_sections; m++) {
        memcpy(linker->output_data + data_offset,
               linker->merged_sections[m].data,
               linker->merged_sections[m].size);
        data_offset += linker->merged_sections[m].size;
    }

    return 0;
}

/*
 * Build a Windows PE (.exe) file. Stub that creates a minimal PE
 * header and copies the merged sections.
 */
static int generate_pe_output(Linker *linker) {
    size_t pe_header_size = 512; /* rough size for DOS + PE headers */
    size_t total_size = pe_header_size;
    for (int m = 0; m < linker->num_merged_sections; m++) {
        total_size += linker->merged_sections[m].size;
    }

    linker->output_data = (uint8_t *)calloc(1, total_size);
    if (!linker->output_data) {
        linker_error("Out of memory for PE output");
        return -1;
    }
    linker->output_size = total_size;

    /* DOS header "MZ" */
    linker->output_data[0] = 'M';
    linker->output_data[1] = 'Z';
    /* PE signature "PE\0\0" at offset 0x3C (typically) */
    /* ... many fields omitted ... */

    size_t data_offset = pe_header_size;
    for (int m = 0; m < linker->num_merged_sections; m++) {
        memcpy(linker->output_data + data_offset,
               linker->merged_sections[m].data,
               linker->merged_sections[m].size);
        data_offset += linker->merged_sections[m].size;
    }

    return 0;
}

/*
 * Build a Mach-O executable. Stub that creates a minimal Mach-O
 * header and appends the merged sections.
 */
static int generate_macho_output(Linker *linker) {
    size_t mach_header_size = 256; /* approximate */
    size_t total_size = mach_header_size;
    for (int m = 0; m < linker->num_merged_sections; m++) {
        total_size += linker->merged_sections[m].size;
    }

    linker->output_data = (uint8_t *)calloc(1, total_size);
    if (!linker->output_data) {
        linker_error("Out of memory for Mach-O output");
        return -1;
    }
    linker->output_size = total_size;

    /* Mach-O magic number (64-bit, little endian): 0xFEEDFACF */
    uint32_t magic = 0xFEEDFACF;
    memcpy(linker->output_data, &magic, sizeof(magic));
    /* ... remaining header fields omitted ... */

    size_t data_offset = mach_header_size;
    for (int m = 0; m < linker->num_merged_sections; m++) {
        memcpy(linker->output_data + data_offset,
               linker->merged_sections[m].data,
               linker->merged_sections[m].size);
        data_offset += linker->merged_sections[m].size;
    }

    return 0;
}

/*
 * Print an error message. In a real tool this would be more
 * elaborate, perhaps including file and line information.
 */
static void linker_error(const char *msg) {
    fprintf(stderr, "Linker error: %s\n", msg);
}
