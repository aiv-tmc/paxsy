#include "macro.h"
#include "../../../utils/str_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * Initial number of hash buckets.  Must be a power of two.
 * 64 is a reasonable starting point that keeps the initial load factor
 * low for typical numbers of macros.
 */
#define MACRO_TABLE_INITIAL_BUCKETS 64

/*
 * Maximum load factor expressed as a rational number: numerator / denominator.
 * When count > (bucket_count * MACRO_LOAD_NUM / MACRO_LOAD_DEN), the table
 * is resized to double the number of buckets.
 */
#define MACRO_LOAD_NUM 3
#define MACRO_LOAD_DEN 4

/*
 * Internal node of the hash table chain.
 * Each node contains a full Macro record (including all dynamically
 * allocated fields) and a pointer to the next node in the chain.
 */
typedef struct MacroNode {
    Macro             macro;
    struct MacroNode* next;
} MacroNode;

/*
 * Opaque structure that holds all the actual table data.
 * MacroTable.intern points to an instance of this structure.
 */
typedef struct MacroTableImpl {
    MacroNode** buckets;        /* array of bucket head pointers */
    size_t      bucket_count;   /* current number of buckets (power of 2) */
    size_t      count;          /* total number of macros stored */
} MacroTableImpl;

/*
 * djb2‑based hash function for strings.
 *
 * str – null‑terminated string.
 *
 * Returns a 32‑bit hash value.
 */
static uint32_t hash_string(const char* str) {
    uint32_t hash = 5381;
    int c;

    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + c;   /* hash * 33 + c */
    }
    return hash;
}

/*
 * Free all resources owned by a single MacroNode, including the
 * macro name, replacement text, and (if function‑like) the parameter
 * name array and its strings.
 */
static void free_macro_node_resources(MacroNode* node) {
    if (!node) return;

    free(node->macro.name);
    free(node->macro.value);

    if (node->macro.has_parameters && node->macro.param_names) {
        for (size_t i = 0; i < node->macro.param_count; i++) {
            free(node->macro.param_names[i]);
        }
        free(node->macro.param_names);
    }
}

/*
 * Create a new, empty macro table.
 */
MacroTable* macro_table_create(void) {
    MacroTable* table = malloc(sizeof(MacroTable));
    if (!table) return NULL;

    MacroTableImpl* impl = malloc(sizeof(MacroTableImpl));
    if (!impl) {
        free(table);
        return NULL;
    }

    impl->bucket_count = MACRO_TABLE_INITIAL_BUCKETS;
    impl->count        = 0;
    impl->buckets      = calloc(impl->bucket_count, sizeof(MacroNode*));
    if (!impl->buckets) {
        free(impl);
        free(table);
        return NULL;
    }

    table->internal = impl;
    return table;
}

/*
 * Destroy a macro table and all memory it owns.
 */
void macro_table_destroy(MacroTable* table) {
    if (!table) return;

    MacroTableImpl* impl = (MacroTableImpl*)table->internal;
    if (impl) {
        for (size_t i = 0; i < impl->bucket_count; i++) {
            MacroNode* node = impl->buckets[i];
            while (node) {
                MacroNode* next = node->next;
                free_macro_node_resources(node);
                free(node);
                node = next;
            }
        }
        free(impl->buckets);
        free(impl);
    }
    free(table);
}

/*
 * Resize the hash table to the next power of two larger than the
 * current bucket count.  All existing entries are rehashed and
 * inserted into the new bucket array.
 *
 * Returns 1 on success, 0 on memory allocation failure (original
 * table remains untouched).
 */
static int resize_macro_table(MacroTableImpl* impl) {
    size_t new_bucket_count = impl->bucket_count << 1;
    MacroNode** new_buckets = calloc(new_bucket_count, sizeof(MacroNode*));
    if (!new_buckets) return 0;

    /* Rehash all existing entries into the new buckets. */
    for (size_t i = 0; i < impl->bucket_count; i++) {
        MacroNode* node = impl->buckets[i];
        while (node) {
            MacroNode* next = node->next;

            /* Compute new bucket index using the new bucket count. */
            size_t new_idx = hash_string(node->macro.name) & (new_bucket_count - 1);

            /* Prepend the node to the new bucket chain. */
            node->next = new_buckets[new_idx];
            new_buckets[new_idx] = node;

            node = next;
        }
    }

    free(impl->buckets);
    impl->buckets      = new_buckets;
    impl->bucket_count = new_bucket_count;

    return 1;
}

/*
 * Helper: create a deep copy of a parameter name array.
 *
 * param_names – source array of strings.
 * count       – number of parameters.
 *
 * Returns a newly allocated array of newly allocated strings on success,
 * or NULL on allocation failure.  The caller must free the returned array
 * and its strings when no longer needed.
 */
static char** deep_copy_param_names(const char** param_names, size_t count) {
    if (count == 0) return NULL;

    char** copy = malloc(count * sizeof(char*));
    if (!copy) return NULL;

    for (size_t i = 0; i < count; i++) {
        copy[i] = u__strdup_safe(param_names[i]);
        if (!copy[i]) {
            /* Undo partial copies. */
            for (size_t j = 0; j < i; j++) free(copy[j]);
            free(copy);
            return NULL;
        }
    }
    return copy;
}

/*
 * Add or update a macro.  See macro.h for the public contract.
 */
int macro_table_add(MacroTable* table,
                    const char* name,
                    const char* value,
                    int has_parameters,
                    const char** param_names,
                    size_t param_count) {
    if (!table || !name || !value) return 0;

    MacroTableImpl* impl = (MacroTableImpl*)table->internal;
    uint32_t hash = hash_string(name);
    size_t bucket_idx = hash & (impl->bucket_count - 1);

    /* Search for an existing macro with the same name. */
    MacroNode* node = impl->buckets[bucket_idx];
    while (node) {
        if (strcmp(node->macro.name, name) == 0) {
            /* Update existing macro. */

            /* Free old replacement text and parameter data. */
            free(node->macro.value);
            if (node->macro.has_parameters && node->macro.param_names) {
                for (size_t i = 0; i < node->macro.param_count; i++) {
                    free(node->macro.param_names[i]);
                }
                free(node->macro.param_names);
            }

            /* Set new value. */
            node->macro.value = u__strdup_safe(value);
            if (!node->macro.value) return 0;

            /* Copy parameters. */
            node->macro.has_parameters = has_parameters;
            node->macro.param_count    = 0;
            node->macro.param_names    = NULL;

            if (has_parameters && param_count > 0) {
                node->macro.param_names =
                    deep_copy_param_names(param_names, param_count);
                if (!node->macro.param_names) {
                    free(node->macro.value);
                    return 0;
                }
                node->macro.param_count = param_count;
            }
            return 1;
        }
        node = node->next;
    }

    /* Macro not found – create a new node. */
    MacroNode* new_node = malloc(sizeof(MacroNode));
    if (!new_node) return 0;

    new_node->macro.name = u__strdup_safe(name);
    if (!new_node->macro.name) {
        free(new_node);
        return 0;
    }
    new_node->macro.name_len = strlen(name);
    new_node->macro.value = u__strdup_safe(value);
    if (!new_node->macro.value) {
        free(new_node->macro.name);
        free(new_node);
        return 0;
    }
    new_node->macro.has_parameters = has_parameters;
    new_node->macro.param_count    = 0;
    new_node->macro.param_names    = NULL;

    if (has_parameters && param_count > 0) {
        new_node->macro.param_names =
            deep_copy_param_names(param_names, param_count);
        if (!new_node->macro.param_names) {
            free(new_node->macro.name);
            free(new_node->macro.value);
            free(new_node);
            return 0;
        }
        new_node->macro.param_count = param_count;
    }

    /* Prepend to the bucket chain. */
    new_node->next = impl->buckets[bucket_idx];
    impl->buckets[bucket_idx] = new_node;
    impl->count++;

    /* Resize if load factor exceeded. */
    if (impl->count > (impl->bucket_count * MACRO_LOAD_NUM / MACRO_LOAD_DEN)) {
        if (!resize_macro_table(impl)) {
            /*
             * Resize failure is not fatal – the table remains usable,
             * just less efficient.  We simply continue.
             */
        }
    }

    return 1;
}

/*
 * Look up a macro by name.  See macro.h for the public contract.
 */
const Macro* macro_table_find(const MacroTable* table, const char* name) {
    if (!table || !name) return NULL;

    MacroTableImpl* impl = (MacroTableImpl*)table->internal;
    uint32_t hash = hash_string(name);
    size_t bucket_idx = hash & (impl->bucket_count - 1);

    MacroNode* node = impl->buckets[bucket_idx];
    while (node) {
        if (strcmp(node->macro.name, name) == 0) {
            return &node->macro;
        }
        node = node->next;
    }
    return NULL;
}

/*
 * Remove a macro by name.  See macro.h for the public contract.
 */
int macro_table_remove(MacroTable* table, const char* name) {
    if (!table || !name) return 0;

    MacroTableImpl* impl = (MacroTableImpl*)table->internal;
    uint32_t hash = hash_string(name);
    size_t bucket_idx = hash & (impl->bucket_count - 1);

    MacroNode** prev_ptr = &impl->buckets[bucket_idx];
    MacroNode* node = *prev_ptr;

    while (node) {
        if (strcmp(node->macro.name, name) == 0) {
            /* Unlink the node from the chain. */
            *prev_ptr = node->next;

            /* Free all resources owned by the node. */
            free_macro_node_resources(node);
            free(node);

            impl->count--;
            return 1;
        }
        prev_ptr = &node->next;
        node = node->next;
    }
    return 0;
}
