#ifndef PAXSY_TOML_H
#define PAXSY_TOML_H

/*
 * toml.h - minimal reader/writer for <project>.toml.
 * Only the subset used by the driver is supported: section headers,
 * `key = value` lines with string/number/bool/array values. Unrelated
 * lines are preserved byte-for-byte by the writer.
 */

/* Locates the configuration file of the project at root: prefers
 * <basename>.toml, otherwise the first *.toml in root. Returns 0 when
 * a configuration file was found. */
int toml_find(const char *root, char *out, int out_size);

/* Reads `key` from `section`. On success stores the value (quotes
 * stripped) in out and returns 1; returns 0 when absent. */
int toml_get(const char *path, const char *section, const char *key,
             char *out, int out_size);

/* Writes `key = raw_value` into `section`, replacing an existing entry
 * or appending a new one. raw_value is inserted verbatim (quote string
 * values at the call site when needed). Returns 0 on success. */
int toml_set(const char *path, const char *section, const char *key,
             const char *raw_value);

/* Removes the `key = ...` line from `section`. Returns 1 when a line
 * was removed and 0 when the key was not present. */
int toml_unset(const char *path, const char *section, const char *key);

#endif /* PAXSY_TOML_H */
