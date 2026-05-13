#ifndef linker_H
#define linker_H

#include <stdint.h>
#include <stddef.h>

/*
 * Supported executable output formats.
 * The linker can produce the native format for the target platform
 * or cross-compile to any of the three formats listed below.
 */
typedef enum {
    FORMAT_ELF,    /* Executable and Linking Format (Linux, BSD, ...) */
    FORMAT_PE,     /* Portable Executable (Windows .exe) */
    FORMAT_MACHO   /* Mach-O (macOS, iOS, ...) */
} OutputFormat;

/*
 * Opaque linker context. All state is maintained inside this
 * structure and must be initialised with linker__init() before use.
 * When the structure is no longer needed, call linker__destroy()
 * to free all associated resources.
 */
typedef struct {
    /* Internal fields - do not access directly. */
    struct ObjectFile objects[256];  /* max objects, defined in .c */
    int num_objects;
    OutputFormat output_format;
    char entry_symbol[64];
    size_t entry_address;
    struct Section merged_sections[16];
    int num_merged_sections;
    uint8_t *output_data;
    size_t output_size;
} Linker;

/*
 * Initialise a new linker session. All fields are set to safe
 * defaults (output format = ELF, no entry point).
 */
void linker__init(Linker *linker);

/*
 * Add an input object file to the linking process. The file format
 * is guessed from the extension (.o for ELF objects, .obj for
 * COFF/PE objects, .macho for Mach-O objects). All sections,
 * symbols and relocations are extracted and stored internally.
 *
 * Returns 0 on success, -1 on error (e.g. unsupported format,
 * too many input files, or parse failure).
 */
int linker__add_object(Linker *linker, const char *filename);

/*
 * Set the desired output executable format. Must be called before
 * linker__link(). The default is FORMAT_ELF.
 */
void linker__set_output_format(Linker *linker, OutputFormat fmt);

/*
 * Specify the name of the entry point symbol (e.g. "_start",
 * "main", "WinMain"). The linker will resolve this symbol during
 * the linking phase and record its final address. If not set, no
 * entry point is forced.
 */
void linker__set_entry(Linker *linker, const char *symbol_name);

/*
 * Run all linking phases in sequence: symbol resolution, section
 * merging, layout, relocation, and output generation. After this
 * call, the final executable image is available for writing.
 *
 * Returns 0 on success, -1 if any phase fails (unresolved symbols,
 * unsupported relocations, etc.).
 */
int linker__link(Linker *linker);

/*
 * Write the generated executable to a file. The file is created
 * or truncated. linker__link() must have been called first.
 *
 * Returns 0 on success, -1 on I/O error.
 */
int linker__write_to_file(Linker *linker, const char *outpath);

/*
 * Free all dynamic memory allocated during the linking session.
 * After this call, the Linker structure is reset to zero and can
 * be reused or discarded.
 */
void linker__destroy(Linker *linker);

#endif
