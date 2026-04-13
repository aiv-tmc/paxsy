#include "macro.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * Default initial capacity for the macro table.
 * Must be a power of two to enable efficient doubling via left shift.
 */
#define MACRO_TABLE_INITIAL_CAPACITY 32

/*
 * Internal: duplicate a string using malloc.
 * (Standard strdup may not be available on all platforms.)
 */
static char* strdup(const char* s) {
    if (!s) return NULL;

    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

/*
 * Create a new macro table.
 */
MacroTable* macro_table_create(void) {
    MacroTable* table = malloc(sizeof(MacroTable));
    if (!table) return NULL;

    table->capacity = MACRO_TABLE_INITIAL_CAPACITY;
    table->count = 0;
    table->macros = malloc(sizeof(Macro) * table->capacity);

    if (!table->macros) {
        free(table);
        return NULL;
    }

    return table;
}

/*
 * Destroy a macro table and all macros stored in it.
 */
void macro_table_destroy(MacroTable* table) {
    if (!table) return;

    for (size_t i = 0; i < table->count; i++) {
        Macro* macro = &table->macros[i];
        free(macro->name);
        free(macro->value);

        if (macro->has_parameters && macro->param_names) {
            for (size_t j = 0; j < macro->param_count; j++) {
                free(macro->param_names[j]);
            }
            free(macro->param_names);
        }
    }

    free(table->macros);
    free(table);
}

/*
 * Ensure that the table has enough capacity for one more macro.
 * If the table is full, its capacity is doubled.
 *
 * @return 1 on success, 0 on allocation failure.
 */
static int ensure_table_capacity(MacroTable* table) {
    if (table->count < table->capacity) return 1;

    /* Double the capacity using bit shift (faster than multiplication). */
    size_t new_capacity = table->capacity << 1;
    Macro* new_macros = realloc(table->macros, sizeof(Macro) * new_capacity);
    if (!new_macros) return 0;

    table->macros = new_macros;
    table->capacity = new_capacity;
    return 1;
}

/*
 * Add or update a macro.
 */
int macro_table_add(MacroTable* table, const char* name, const char* value,
                    int has_parameters, char** param_names, size_t param_count) {
    if (!table || !name || !value) return 0;

    /* Linear search for an existing macro with the same name (O(n)). */
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->macros[i].name, name) == 0) {
            /* Update existing macro. */
            Macro* macro = &table->macros[i];

            /* Free old resources. */
            free(macro->value);
            if (macro->has_parameters && macro->param_names) {
                for (size_t j = 0; j < macro->param_count; j++) {
                    free(macro->param_names[j]);
                }
                free(macro->param_names);
            }

            /* Set new values. */
            macro->value = strdup(value);
            macro->has_parameters = has_parameters;
            macro->param_count = param_count;
            macro->param_names = param_names;   /* Takes ownership. */

            return macro->value ? 1 : 0;
        }
    }

    /* Add a new macro. */
    if (!ensure_table_capacity(table)) return 0;

    Macro* macro = &table->macros[table->count];

    macro->name = strdup(name);
    macro->value = strdup(value);
    macro->name_len = strlen(name);
    macro->has_parameters = has_parameters;
    macro->param_count = param_count;
    macro->param_names = param_names;           /* Takes ownership. */

    if (!macro->name || !macro->value) {
        /* Allocation failed – clean up partially allocated resources. */
        free(macro->name);
        free(macro->value);
        free(macro->param_names);
        return 0;
    }

    table->count++;
    return 1;
}

/*
 * Find a macro by name (linear search).
 */
const Macro* macro_table_find(const MacroTable* table, const char* name) {
    if (!table || !name) return NULL;

    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->macros[i].name, name) == 0) {
            return &table->macros[i];
        }
    }

    return NULL;
}

/*
 * Remove a macro by name.
 */
int macro_table_remove(MacroTable* table, const char* name) {
    if (!table || !name) return 0;

    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->macros[i].name, name) == 0) {
            Macro* macro = &table->macros[i];

            /* Free resources of the macro being removed. */
            free(macro->name);
            free(macro->value);

            if (macro->has_parameters && macro->param_names) {
                for (size_t j = 0; j < macro->param_count; j++) {
                    free(macro->param_names[j]);
                }
                free(macro->param_names);
            }

            /* Replace the removed entry with the last one (if any). */
            if (i < table->count - 1) {
                table->macros[i] = table->macros[table->count - 1];
            }

            table->count--;
            return 1;
        }
    }

    return 0;
}

/*
 * Convenience wrapper around macro_table_find.
 */
int macro_table_exists(const MacroTable* table, const char* name) {
    return macro_table_find(table, name) != NULL;
}
