#ifndef MACRO_H
#define MACRO_H

#include <stdint.h>
#include <stddef.h>

/*
 * Structure representing a single macro definition.
 *
 * name              – macro name (null‑terminated).
 * value             – replacement text (null‑terminated).
 * name_len          – length of the macro name (excluding null).
 * has_parameters    – 0 = object‑like, 1 = function‑like.
 * param_names       – array of parameter name strings (NULL if none).
 * param_count       – number of parameters (0 for object‑like).
 */
typedef struct {
    char*   name;
    char*   value;
    size_t  name_len;
    int     has_parameters;
    char**  param_names;
    size_t  param_count;
} Macro;

/*
 * Opaque handle to a macro table.  The internal implementation uses
 * a hash table with separate chaining; all access is through the
 * functions declared below.
 */
typedef struct MacroTable {
    void*   internal;   /* pointer to hidden implementation */
} MacroTable;

/*
 * Create and initialise a new, empty macro table.
 *
 * Returns a pointer to the table on success, or NULL if memory
 * allocation fails.
 */
MacroTable* macro_table_create(void);

/*
 * Destroy a macro table and free all associated resources.
 *
 * table – table to destroy (may be NULL, in which case the call is a no‑op).
 */
void macro_table_destroy(MacroTable* table);

/*
 * Add a new macro definition or update an existing one.
 *
 * For function‑like macros, a deep copy of the parameter name strings
 * is made.  The table takes ownership of these copies; the caller retains
 * ownership of the original strings.
 *
 * table          – macro table.
 * name           – macro name (will be copied).
 * value          – replacement text (will be copied).
 * has_parameters – 1 for function‑like, 0 for object‑like.
 * param_names    – array of parameter names (can be NULL if count == 0).
 * param_count    – number of parameters.
 *
 * Returns 1 on success, 0 on failure (out of memory).
 */
int macro_table_add(MacroTable* table,
                    const char* name,
                    const char* value,
                    int has_parameters,
                    const char** param_names,
                    size_t param_count);

/*
 * Look up a macro by name.
 *
 * table – macro table.
 * name  – macro name to search for.
 *
 * Returns a pointer to the (constant) macro entry, or NULL if not found.
 */
const Macro* macro_table_find(const MacroTable* table, const char* name);

/*
 * Remove a macro definition by name.
 *
 * table – macro table.
 * name  – name of the macro to remove.
 *
 * Returns 1 if the macro was found and removed, 0 otherwise.
 */
int macro_table_remove(MacroTable* table, const char* name);

#endif
