#ifndef MACRO_H
#define MACRO_H

#include <stdint.h>
#include <stddef.h>

/*
 * Structure representing a single macro definition.
 */
typedef struct {
    char* name;                    /* Macro name (null-terminated) */
    char* value;                   /* Replacement text (null-terminated) */
    size_t name_len;               /* Length of macro name (excluding null) */
    int has_parameters;            /* 0 = object-like, 1 = function-like */
    char** param_names;            /* Array of parameter names (if function-like) */
    size_t param_count;            /* Number of parameters */
} Macro;

/*
 * Structure holding a table (dynamic array) of macros.
 */
typedef struct {
    Macro* macros;                 /* Array of macro entries */
    size_t count;                  /* Current number of macros */
    size_t capacity;               /* Allocated capacity (in entries) */
} MacroTable;

/*
 * Create and initialize a new macro table.
 *
 * @return  Pointer to newly allocated table, or NULL on failure.
 */
MacroTable* macro_table_create(void);

/*
 * Destroy a macro table and free all associated memory.
 *
 * @param table  Table to destroy (may be NULL).
 */
void macro_table_destroy(MacroTable* table);

/*
 * Add a new macro definition or update an existing one.
 * For function-like macros, the caller must supply an array of parameter names.
 * The table takes ownership of the 'param_names' array and its strings.
 *
 * @param table          Macro table.
 * @param name           Macro name (will be copied).
 * @param value          Replacement text (will be copied).
 * @param has_parameters 1 for function-like, 0 for object-like.
 * @param param_names    Array of parameter names (can be NULL if count == 0).
 * @param param_count    Number of parameters.
 * @return               1 on success, 0 on failure (out of memory).
 */
int macro_table_add(MacroTable* table, const char* name, const char* value,
                    int has_parameters, char** param_names, size_t param_count);

/*
 * Find a macro by name.
 *
 * @param table  Macro table.
 * @param name   Macro name to look for.
 * @return       Pointer to the macro entry, or NULL if not found.
 */
const Macro* macro_table_find(const MacroTable* table, const char* name);

/*
 * Remove a macro definition by name.
 *
 * @param table  Macro table.
 * @param name   Name of macro to remove.
 * @return       1 if found and removed, 0 otherwise.
 */
int macro_table_remove(MacroTable* table, const char* name);

/*
 * Check whether a macro with the given name exists.
 *
 * @param table  Macro table.
 * @param name   Macro name.
 * @return       1 if exists, 0 otherwise.
 */
int macro_table_exists(const MacroTable* table, const char* name);

#endif
