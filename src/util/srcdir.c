#include "common.h"
#include "srcdir.h"
#include "stdpath.h"
#include "toml.h"

#include <dirent.h>
#include <sys/stat.h>

/*
 * srcdir.c - deterministic discovery of Paxsy source files.
 *
 * Every consumer of the project sources - build, check, clippy, bench,
 * test and the archive emitters - must see one identical file order
 * regardless of the directory iteration order of the file system.
 * srcdir_collect() therefore:
 *
 *   1. walks <root>/src (recursively when asked, descending only into
 *      subdirectories, never into symlinks); the conventional root
 *      src/mod.px is kept as the project's module entry point;
 *   2. adds [include.<name>] source paths from the project TOML before
 *      dependency scanning; relative paths are rooted at the project,
 *      directory dependencies are recursively collected, and missing
 *      required dependencies fail the collection;
 *   3. scans all collected project/include sources for references to std
 *      package - both `use std::inout;` directives and qualified
 *      names like `std::inout::println(...)` - and appends exactly
 *      the library sources they mention: each `std::<name>` prefix
 *      pulls the <name> library, and a bare `use std;` pulls the
 *      whole package including the root sources under src/. Libraries
 *      the program never names are never parsed, so an unrelated
 *      library with a syntax error cannot break a build that does
 *      not use it. A missing std package is not a discovery error
 *      either: `check`-style commands analyse without emitting code,
 *      and a program that calls into std gets a precise "unknown
 *      function" diagnostic from the backend naming the missing entry
 *      point;
 *   4. keeps regular files ending in '.px'; with tests_only set the
 *      suffix must be '_test.px' instead, which is how the test
 *      command isolates the test programs of a project;
 *   5. stores each path into the caller's growable SrcList, failing
 *      the whole collection (-1) when a path does not fit
 *      PAXSY_PATH_MAX so truncation can never alias two files;
 *   6. sorts the combined list by full path with plain strcmp before
 *      returning, giving the stable order every downstream phase
 *      relies on.
 *
 * A missing <root>/src directory is not an error: the project simply
 * has no sources and the caller reports that fact.
 */

/* Growable-path-array helpers -------------------------------------- */

/* Lexically resolves '.' and '..' segments so the same file reached
 * by different spellings (exe-relative vs absolute) deduplicates;
 * the stored spelling is left untouched for diagnostics. */
static void srcdir_normalize(char *out, size_t cap, const char *path) {
    char tmp[PAXSY_PATH_MAX];
    char *segs[PAXSY_PATH_MAX / 2];
    int n = 0;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *s = strtok(tmp, "/"); s; s = strtok(NULL, "/")) {
        if (strcmp(s, ".") == 0) continue;
        if (strcmp(s, "..") == 0) {
            if (n > 0 && strcmp(segs[n - 1], "..") != 0) n--;
            else segs[n++] = s;
            continue;
        }
        segs[n++] = s;
    }
    {
        size_t off = 0;
        int absolute = path[0] == '/';
        if (absolute && cap > 1) out[off++] = '/';
        for (int i = 0; i < n; i++) {
            int w = snprintf(out + off, cap - off, "%s%s", i ? "/" : "", segs[i]);
            if (w < 0 || (size_t)w >= cap - off) break;
            off += (size_t)w;
        }
        if (off == 0) snprintf(out, cap, "%s", absolute ? "/" : ".");
        else out[off] = '\0';
    }
}

int srclist_push(SrcList *list, const char *path) {
    size_t need = strlen(path) + 1;
    if (need > PAXSY_PATH_MAX) return -1;
    /* Transitive library collection revisits directories on every
     * scan round, and the same file can be reached by different
     * spellings (exe-relative vs absolute); a source path is
     * meaningful exactly once, so the list stays a set keyed by the
     * canonical path. */
    {
        char key[PAXSY_PATH_MAX];
        srcdir_normalize(key, sizeof(key), path);
        for (int i = 0; i < list->count; i++) {
            char have[PAXSY_PATH_MAX];
            srcdir_normalize(have, sizeof(have), list->paths[i]);
            if (strcmp(have, key) == 0) return 0;
        }
    }
    if (list->count == list->cap) {
        int cap = list->cap ? list->cap * 2 : 16;
        void *p = realloc(list->paths, (size_t)cap * PAXSY_PATH_MAX);
        if (!p) return -1;
        list->paths = p;
        list->cap = cap;
    }
    memcpy(list->paths[list->count], path, need);
    list->count++;
    return 0;
}

void srclist_free(SrcList *list) {
    free(list->paths);
    list->paths = NULL;
    list->count = list->cap = 0;
}

/* Classification ----------------------------------------------------- */

/* A candidate entry is a kept source when its name ends in '.px';
 * tests_only narrows the suffix to '_test.px'. Directory names can
 * never match because they carry no file suffix. */
static int src_kept(const char *name, int tests_only) {
    size_t n = strlen(name);
    if (tests_only) {
        const char *suf = "_test.px";
        size_t sl = strlen(suf);
        return n > sl && strcmp(name + n - sl, suf) == 0;
    }
    return n > 3 && strcmp(name + n - 3, ".px") == 0;
}

static int is_reg(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* One directory level. Returns 0, or -1 as soon as a path cannot be
 * stored; the error propagates straight up and aborts the walk. */
static int walk_dir(const char *dir, int recursive, int tests_only,
                    SrcList *list) {
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL && rc == 0) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char path[PAXSY_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) { rc = -1; break; }
        if (recursive) {
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                rc = walk_dir(path, recursive, tests_only, list);
                continue;
            }
        }
        if (src_kept(de->d_name, tests_only) && is_reg(path))
            rc = srclist_push(list, path);
    }
    closedir(dp);
    return rc;
}

static int cmp_paths(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* std reference scanning ------------------------------------------------ */

/* True when the identifier `name` (of `len` bytes) appears as one
 * entry of the comma-separated `list`. */
static int lib_in_list(const char *list, const char *name, size_t len) {
    const char *p = list;
    while (*p) {
        while (*p == ',') p++;
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == len && strncmp(p, name, len) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* Appends `name` (of `len` bytes) to the comma-separated `wanted`
 * list when it is not already present; the list is capped so a
 * source file with very many references cannot grow it without
 * bound. */
static void wanted_add(char *wanted, size_t cap, const char *name,
                       size_t len) {
    if (lib_in_list(wanted, name, len)) return;
    size_t cur = strlen(wanted);
    if (cur + (cur ? 1 : 0) + len + 1 >= cap) return;
    if (cur) strcat(wanted, ",");
    strncat(wanted, name, len);
}

/* Scans one source file for references to the std package: every
 * `std::<name>` qualifier - in a use directive, in a qualified call,
 * in a type name - adds <name> to the wanted list, and a bare
 * `use std;` selects the whole package ("*"). Inside a package
 * library file, a bare `use <name>;` refers to a sibling library
 * (package files reach siblings unqualified), so it is collected
 * too when `package_file` is set; the same spelling in a project
 * file names a project module and is ignored. The scan is lexical
 * and conservative: it collects every identifier that follows a
 * "std::" prefix, which is exactly the shape the module grammar
 * produces. */
static void scan_use_std(const char *path, char *wanted, size_t cap,
                         int *want_all, int package_file) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return;
    }
    buf[sz] = '\0';
    fclose(fp);
    /* Both passes are lexical, so mentions inside comments must not
     * register as dependencies; blank them out in a working copy. */
    {
        int block = 0, line_c = 0;
        for (long k = 0; k < sz; k++) {
            if (block) {
                if (buf[k] == '*' && buf[k + 1] == '/') { block = 0; buf[k] = buf[k + 1] = ' '; k++; }
                else buf[k] = ' ';
            } else if (line_c) {
                if (buf[k] == '\n') line_c = 0;
                else buf[k] = ' ';
            } else if (buf[k] == '/' && buf[k + 1] == '/') { line_c = 1; buf[k] = buf[k + 1] = ' '; k++; }
            else if (buf[k] == '/' && buf[k + 1] == '*') { block = 1; buf[k] = buf[k + 1] = ' '; k++; }
        }
    }
    const char *q = buf;
    while ((q = strstr(q, "std::")) != NULL) {
        const char *r = q + 5;
        if (isalpha((unsigned char)*r) || *r == '_') {
            const char *e = r;
            while (isalnum((unsigned char)*e) || *e == '_') e++;
            wanted_add(wanted, cap, r, (size_t)(e - r));
            q = e;
        } else {
            q = r;
        }
    }
    /* The alloc/free operators lower to std::heap calls without any
     * use directive (the language reference uses them bare), so a
     * source that mentions either keyword pulls the heap library. */
    for (const char *k = buf; *k; k++) {
        if ((!isalpha((unsigned char)*k) && *k != '_') ||
            (k != buf && (isalnum((unsigned char)k[-1]) || k[-1] == '_')))
            continue;
        const char *e = k;
        while (isalnum((unsigned char)*e) || *e == '_') e++;
        size_t len = (size_t)(e - k);
        if ((len == 5 && strncmp(k, "alloc", 5) == 0) ||
            (len == 4 && strncmp(k, "free", 4) == 0))
            wanted_add(wanted, cap, "heap", 4);
        k = len > 0 ? e - 1 : e;
    }
    if (strstr(buf, "use std;")) *want_all = 1;
    if (package_file) {
        /* Sibling-library references: 'use heap;', 'use sys;'. Only
         * the single-identifier form is collected; qualified uses
         * were handled by the std:: pass above. */
        const char *u = buf;
        while ((u = strstr(u, "use")) != NULL) {
            int boundary = (u == buf || (!isalnum((unsigned char)u[-1]) && u[-1] != '_'))
                           && !isalpha((unsigned char)u[3]) && u[3] != '_';
            if (boundary) {
                const char *r = u + 3;
                while (*r == ' ' || *r == '\t' || *r == '\n') r++;
                if (isalpha((unsigned char)*r) || *r == '_') {
                    const char *e = r;
                    while (isalnum((unsigned char)*e) || *e == '_') e++;
                    const char *s = e;
                    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
                    /* The package's own name selects everything and
                     * is handled by want_all, not as a library. */
                    if (*s == ';' &&
                        !((e - r) == 3 && strncmp(r, "std", 3) == 0))
                        wanted_add(wanted, cap, r, (size_t)(e - r));
                }
            }
            u += 3;
        }
    }
    free(buf);
}


/* Project include tables -------------------------------------------------
 *
 * A project may attach source trees with explicit tables:
 *
 *   [include.geometry]
 *   path = "vendor/geometry/src"
 *   optional = false
 *
 * Paths are relative to the project root (absolute paths are accepted).
 * A path may name one .px file or a directory, which is traversed
 * recursively. Missing required paths fail source discovery; optional
 * paths are skipped. Keeping this here makes build/check/compile/clippy
 * and artifact commands consume the same source graph.
 */
static int srcdir_trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    return (int)strlen(s);
}

static int srcdir_is_px(const char *path) {
    size_t n = strlen(path);
    return n > 3 && strcmp(path + n - 3, ".px") == 0;
}

static int srcdir_add_include(const char *root, const char *value,
                              int optional, SrcList *list) {
    char path[PAXSY_PATH_MAX];
    if (!value || !value[0]) return optional ? 0 : -1;
    if (value[0] == '/') {
        if (snprintf(path, sizeof(path), "%s", value) >= (int)sizeof(path))
            return -1;
    } else if (snprintf(path, sizeof(path), "%s/%s", root, value) >=
               (int)sizeof(path)) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return optional ? 0 : -1;
    if (S_ISDIR(st.st_mode)) return walk_dir(path, 1, 0, list);
    if (S_ISREG(st.st_mode) && srcdir_is_px(path))
        return srclist_push(list, path);
    return -1;
}

static int srcdir_collect_includes(const char *root, SrcList *list) {
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) return 0;
    FILE *fp = fopen(cfg, "r");
    if (!fp) return 0;
    char line[2048];
    int in_include = 0, optional = 0, have_path = 0, rc = 0;
    char include_path[PAXSY_PATH_MAX] = "";
    while (fgets(line, sizeof(line), fp)) {
        srcdir_trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '[') {
            if (in_include && (!have_path ||
                srcdir_add_include(root, include_path, optional, list) != 0)) {
                rc = -1; break;
            }
            in_include = strncmp(line, "[include.", 9) == 0 &&
                          line[strlen(line) - 1] == ']';
            optional = 0; have_path = 0; include_path[0] = '\0';
            continue;
        }
        if (!in_include) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0'; srcdir_trim(line); srcdir_trim(eq);
        if (strcmp(line, "path") == 0) {
            size_t n = strlen(eq);
            if (n >= 2 && ((eq[0] == '"' && eq[n - 1] == '"') ||
                           (eq[0] == '\'' && eq[n - 1] == '\''))) {
                eq[n - 1] = '\0';
                snprintf(include_path, sizeof(include_path), "%s", eq + 1);
                have_path = 1;
            }
        } else if (strcmp(line, "optional") == 0) {
            optional = strcmp(eq, "true") == 0;
        }
    }
    if (!rc && in_include)
        rc = have_path ? srcdir_add_include(root, include_path, optional, list)
                       : -1;
    fclose(fp);
    return rc;
}

int srcdir_collect_path(const char *directory, int recursive, int tests_only,
                        SrcList *list) {
    /* Normalise storage that cannot describe a valid list (a negative
     * count or a capacity smaller than the count is only possible
     * when the caller handed in an uninitialised struct); starting
     * from an empty list turns that caller bug into a clean walk
     * instead of a realloc() on a wild pointer. */
    if (list->count < 0 || list->cap < list->count) {
        list->paths = NULL;
        list->count = 0;
        list->cap = 0;
    }
    if (walk_dir(directory, recursive, tests_only, list) != 0) return -1;
    /* A source-root `mod.px` is the canonical module entry point. */
    if (!tests_only) {
        char entry[PAXSY_PATH_MAX];
        int en = snprintf(entry, sizeof(entry), "%s/mod.px", directory);
        if (en >= 0 && (size_t)en < sizeof(entry) && is_reg(entry) &&
            srclist_push(list, entry) != 0) return -1;
    }
    /* Figure out which std libraries the project references, then
     * append exactly their sources. A bare `use std;` pulls the
     * whole package; named libraries pull only the named trees. An
     * absent package or an absent library leaves the list untouched:
     * analysis-only commands need nothing from it, and the backend
     * names any std entry point a program calls but the package
     * does not provide. */
    {
        char wanted[256] = "";
        int want_all = 0;
        /* Library files themselves reference further libraries
         * ('inout' uses 'sys' and 'heap'), so the scan repeats over
         * every file appended by the previous round until no new
         * library name appears. The wanted list is shared across
         * rounds, which makes the fixpoint a single pass of scans. */
        char libroot[PAXSY_PATH_MAX];
        int have_root = std_root(libroot, sizeof(libroot)) == 0;
        size_t root_len = have_root ? strlen(libroot) : 0;
        /* Libraries added in one round are scanned in the next, so
         * a library's own use directives pull in its siblings
         * transitively; the loop ends on a stable round. */
        int before = -1;
        while (before != list->count && !want_all) {
            before = list->count;
            for (int i = 0; i < list->count; i++) {
                /* Files inside the package tree may name siblings
                 * with a bare use; project files may not. */
                int package_file =
                    have_root && strncmp(list->paths[i], libroot,
                                         root_len) == 0;
                scan_use_std(list->paths[i], wanted,
                             sizeof(wanted), &want_all, package_file);
            }
            if (want_all) {
                std_collect_libs(list, "*");
            } else if (wanted[0]) {
                std_collect_libs(list, wanted);
                wanted[0] = '\0';
            }
        }
    }
    if (list->count > 1)
        qsort(list->paths, (size_t)list->count, PAXSY_PATH_MAX, cmp_paths);
    return list->count;
}

int srcdir_collect(const char *root, int recursive, int tests_only,
                   SrcList *list) {
    char dir[PAXSY_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/src", root);
    if (n < 0 || (size_t)n >= sizeof(dir)) return -1;
    /* Resolve project include tables before the standard-library fixpoint
     * in srcdir_collect_path, so dependencies of included sources are
     * discovered in the same transitive scan. */
    if (!tests_only && srcdir_collect_includes(root, list) != 0) return -1;
    int count = srcdir_collect_path(dir, recursive, tests_only, list);
    if (count < 0) return -1;
    if (list->count > 1)
        qsort(list->paths, (size_t)list->count, PAXSY_PATH_MAX, cmp_paths);
    return list->count;
}
