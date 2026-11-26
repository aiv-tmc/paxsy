#ifndef PAXSY_STDPATH_H
#define PAXSY_STDPATH_H

/* Maximum number of discovered module names returned to callers. */
#define PAXSY_MAX_MODULES 128

/*
 * stdpath.h - location of the Paxsy standard library package, the
 * package context used while lowering its sources, and filtered
 * collection of its libraries. The library is a Paxsy package
 * (fetched from paxsy.org in normal operation, carried under lib/std
 * in the toolchain repository) with a TOML manifest (<pkgname>.toml)
 * whose module names are discovered from source declarations rather
 * than enumerated in a [modules] manifest table. Four candidate
 * locations are tried in order for the std
 * package itself: the package cache given by PAXSY_LIBRARY_DIR, the
 * canonical install tree next to the running executable, the build
 * tree beside the executable, and the default per-user cache.
 * Installed packages are looked up the same way under
 * <cache>/installed/<pkg>.
 *
 * Library sources use plain module declarations: one file of the
 * package declares the root module (module std), every library file
 * declares its own module (module inout). Lowering announces the
 * package prefix (std::) for files inside the package and prepends
 * it to every top-level module, so the sources carry no package
 * syntax of their own. The driver announces the file with
 * stdpath_set_pkg_prefix(); lower.c reads the prefix back through
 * stdpath_pkg_prefix() and the package name through
 * stdpath_pkg_name().
 *
 * Only the modules a project actually references are compiled:
 * std_collect_libs() takes a comma-separated list of module names
 * (or "*" for the whole package) and appends source files declaring those modules, so
 * an unrelated library with a syntax error never breaks a build that
 * does not use it.
 */

#include "srcdir.h"

/* Writes the directory that holds the std package into `out` and
 * returns 0; returns -1 when no candidate exists. */
int std_root(char *out, size_t cap);

/* Appends the .px files of the requested modules to `list`.
 * `wanted` is a comma-separated list of module names discovered from
 * source declarations, or "*" for every module plus the package
 * root sources (src/), which is what a bare `use std;` selects.
 * Returns 0 on success, -1 when the std package is absent or a path
 * does not fit PAXSY_PATH_MAX. */
int std_collect_libs(SrcList *list, const char *wanted);

/* Discovers module names by scanning the package source tree for
 * `module name {` declarations. Returns unique names, or -1 when the
 * package source tree cannot be found. */
int pkg_modules(const char *pkg, char names[][64], int max);

/* When `path` lives inside the std package, writes "<pkg>::" into
 * `out` and returns 0; writes an empty string and returns -1
 * otherwise. */
int std_pkg_prefix(const char *path, char *out, size_t cap);

/* Announces the source file whose package prefix subsequent lowering
 * must apply; pass any non-package path to clear it. */
void stdpath_set_pkg_prefix(const char *src_path);

/* Returns the prefix announced by the last stdpath_set_pkg_prefix
 * call ("std::" for library sources, "" otherwise). */
const char *stdpath_pkg_prefix(void);

/* Returns the announced package name ("std" for library sources, ""
 * otherwise). */
const char *stdpath_pkg_name(void);

#endif /* PAXSY_STDPATH_H */
