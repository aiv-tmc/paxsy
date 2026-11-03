#include "common.h"
#include "srcdir.h"
#include "stdpath.h"

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
 *      subdirectories, never into symlinks);
 *   2. scans the collected project sources for references to the std
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
 *   3. keeps regular files ending in '.px'; with tests_only set the
 *      suffix must be '_test.px' instead, which is how the test
 *      command isolates the test programs of a project;
 *   4. stores each path into the caller's growable SrcList, failing
 *      the whole collection (-1) when a path does not fit
 *      PAXSY_PATH_MAX so truncation can never alias two files;
 *   5. sorts the combined list by full path with plain strcmp before
 *      returning, giving the stable order every downstream phase
 *      relies on.
 *
 * A missing <root>/src directory is not an error: the project simply
 * has no sources and the caller reports that fact.
 */

/* Growable-path-array helpers -------------------------------------- */

int srclist_push(SrcList *list, const char *path) {
    size_t need = strlen(path) + 1;
    if (need > PAXSY_PATH_MAX) return -1;
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
 * `use std;` selects the whole package ("*"). The scan is lexical
 * and conservative: it collects every identifier that follows a
 * "std::" prefix, which is exactly the shape the module grammar
 * produces. */
static void scan_use_std(const char *path, char *wanted, size_t cap,
                         int *want_all) {
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
    if (strstr(buf, "use std;")) *want_all = 1;
    free(buf);
}

int srcdir_collect(const char *root, int recursive, int tests_only,
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
    char dir[PAXSY_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/src", root);
    if (n < 0 || (size_t)n >= sizeof(dir)) return -1;
    if (walk_dir(dir, recursive, tests_only, list) != 0) return -1;
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
        int nsrc = list->count;
        for (int i = 0; i < nsrc; i++)
            scan_use_std(list->paths[i], wanted, sizeof(wanted), &want_all);
        if (want_all)
            std_collect_libs(list, "*");
        else if (wanted[0])
            std_collect_libs(list, wanted);
    }
    if (list->count > 1)
        qsort(list->paths, (size_t)list->count, PAXSY_PATH_MAX, cmp_paths);
    return list->count;
}
