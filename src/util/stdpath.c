/* readlink on /proc/self/exe and the package cache layout require
 * POSIX names. */
#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "stdpath.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * stdpath.c - resolves the directory of the Paxsy standard library
 * package, answers which package context (if any) a source file
 * belongs to, and collects the package sources. The search order
 * for the std package directory is:
 *
 *   1. $PAXSY_LIBRARY_DIR/installed/std   (explicit package cache);
 *   2. <exe>/std                         (repository root and test
 *                                          staging layouts with the
 *                                          package directory beside
 *                                          the binary);
 *   3. <exe>/../lib/paxsy/std             (canonical install where
 *                                          the binary sits in bin/);
 *   4. <exe>/lib/std                      (legacy build-tree layout
 *                                          with the binary at the
 *                                          tree root);
 *   5. ~/.paxsy/installed/std             (default user cache).
 *
 * The first existing directory wins. std_collect_libs() serves requested
 * module names (or the whole package for "*") out of package src/. Source
 * declarations, not directory names or TOML tables, define module identity.
 * A matching directory may still group implementation files for a module;
 * otherwise the collector searches for a source file declaring that module.
 * This permits modules such as std::string to live in an unrelated source
 * directory. Collection follows srcdir rules: regular .px files only, with
 * path overflow treated as a hard error. pkg_modules() discovers unique
 * module declarations by scanning the package source tree; the package TOML
 * file contains metadata, not an export registry. The announced prefix is
 * per compile unit and single-threaded by construction: the driver announces
 * the file before lowering and lower.c consumes it while naming modules.
 */

/* True when `path` names a readable directory. */
static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* True when the path survives storage in PAXSY_PATH_MAX bytes. */
static int path_fits(const char *path) {
    return strlen(path) < PAXSY_PATH_MAX;
}

/* Writes the directory of the running executable into `out`.
 * Returns 0 on success, -1 when /proc/self/exe cannot be read. */
static int exe_dir(char *out, size_t cap) {
    char buf[1024];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char *slash = strrchr(buf, '/');
    if (!slash) return -1;
    *slash = '\0';
    if ((size_t)n >= cap) return -1;
    snprintf(out, cap, "%s", buf);
    return 0;
}

int std_root(char *out, size_t cap) {
    const char *env = getenv("PAXSY_LIBRARY_DIR");
    if (env && env[0]) {
        char cand[PAXSY_PATH_MAX];
        snprintf(cand, sizeof(cand), "%s/installed/std", env);
        if (is_dir(cand) && path_fits(cand)) {
            snprintf(out, cap, "%s", cand);
            return 0;
        }
    }
    char exe[1024];
    if (exe_dir(exe, sizeof(exe)) == 0) {
        char cand[1200];
        snprintf(cand, sizeof(cand), "%s/../lib/paxsy/std", exe);
        if (is_dir(cand) && path_fits(cand)) {
            snprintf(out, cap, "%s", cand);
            return 0;
        }
        snprintf(cand, sizeof(cand), "%s/std", exe);
        if (is_dir(cand) && path_fits(cand)) {
            snprintf(out, cap, "%s", cand);
            return 0;
        }
        snprintf(cand, sizeof(cand), "%s/lib/std", exe);
        if (is_dir(cand) && path_fits(cand)) {
            snprintf(out, cap, "%s", cand);
            return 0;
        }
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char cand[PAXSY_PATH_MAX];
        snprintf(cand, sizeof(cand), "%s/.paxsy/installed/std", home);
        if (is_dir(cand) && path_fits(cand)) {
            snprintf(out, cap, "%s", cand);
            return 0;
        }
    }
    return -1;
}

/* One directory level of a library tree; a missing directory is not
 * an error, a path overflow is. */
static int std_walk(const char *dir, SrcList *list) {
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
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            rc = std_walk(path, list);
            continue;
        }
        size_t nl = strlen(de->d_name);
        if (nl > 3 && strcmp(de->d_name + nl - 3, ".px") == 0)
            rc = srclist_push(list, path);
    }
    closedir(dp);
    return rc;
}

/* True when the first 4 KiB of the .px file at `path` declares
 * `module <name> {`, ignoring matches that do not start a line (doc
 * comments mention module names freely, only a real declaration
 * counts). */
static int px_declares_module(const char *path, const char *name) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    char line[2048];
    size_t wanted = strlen(name);
    int found = 0;
    while (!found && fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "module", 6) != 0 ||
            !(p[6] == ' ' || p[6] == '\t')) continue;
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, name, wanted) != 0 ||
            (isalnum((unsigned char)p[wanted]) || p[wanted] == '_')) continue;
        p += wanted;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '{') found = 1;
    }
    fclose(fp);
    return found;
}

/* Recursive helper of std_collect_by_module: appends every .px file
 * under `dir` that declares `module <name> {`. */
static int std_collect_by_module_walk(const char *dir, SrcList *list,
                                      const char *name) {
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
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            rc = std_collect_by_module_walk(path, list, name);
            continue;
        }
        size_t nl = strlen(de->d_name);
        if (nl > 3 && strcmp(de->d_name + nl - 3, ".px") == 0 &&
            px_declares_module(path, name))
            rc = srclist_push(list, path);
    }
    closedir(dp);
    return rc;
}

/* Collects the sources of the library whose module is named `name`,
 * wherever it lives inside the package; used when no directory of
 * the wanted name exists (the string library, for example, lives in
 * the text directory of the package while the manual names it
 * std::string). */
static int std_collect_by_module(SrcList *list, const char *libroot,
                                 const char *name) {
    return std_collect_by_module_walk(libroot, list, name);
}

int std_collect_libs(SrcList *list, const char *wanted) {
    char root[PAXSY_PATH_MAX];
    if (std_root(root, sizeof(root)) != 0) return -1;
    /* Library sources live under the package's src/ directory; the
     * '*' library selects every directory of the package at once. */
    char libroot[PAXSY_PATH_MAX+8];
    snprintf(libroot, sizeof(libroot), "%s/src", root);
    if (!is_dir(libroot)) return -1;
    if (strcmp(wanted, "*") == 0) {
        return std_walk(libroot, list);
    }
    int rc = 0;
    const char *p = wanted;
    while (*p && rc == 0) {
        while (*p == ',') p++;
        if (!*p) break;
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char name[64];
        if (len >= sizeof(name)) return -1;
        memcpy(name, p, len);
        name[len] = '\0';
        /* A same-named directory is only a source-group optimisation: use
         * it only when it actually contains a declaration of this module.
         * Otherwise discover the declaration anywhere in the package tree.
         * Thus paths never determine module identity. */
        char dir[PAXSY_PATH_MAX + 16];
        snprintf(dir, sizeof(dir), "%.260s/%.260s", libroot, name);
        if (is_dir(dir)) {
            SrcList probe = { 0 };
            int found = std_collect_by_module_walk(dir, &probe, name);
            free(probe.paths);
            if (found != 0) { rc = -1; break; }
            if (probe.count > 0) rc = std_walk(dir, list);
            else rc = std_collect_by_module(list, libroot, name);
        } else {
            rc = std_collect_by_module(list, libroot, name);
        }
        if (!e) break;
        p = e + 1;
    }
    return rc;
}

/* Collect every declared module name from a package source tree.  Module
 * discovery is source-driven: the TOML manifest no longer enumerates module
 * names, and directory names are not treated as module identities. */
static int module_name_add(char names[][64], int *count, int max,
                           const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < *count; ++i)
        if (strcmp(names[i], name) == 0) return 0;
    if (*count >= max) return 0;
    snprintf(names[*count], 64, "%.63s", name);
    (*count)++;
    return 1;
}

static int px_collect_module_names(const char *path, char names[][64],
                                   int *count, int max) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "module", 6) != 0 ||
            !(p[6] == ' ' || p[6] == '\t')) continue;
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
        if (!(isalpha((unsigned char)*p) || *p == '_')) continue;
        char name[64];
        size_t n = 0;
        while ((isalnum((unsigned char)*p) || *p == '_') && n < sizeof(name)-1)
            name[n++] = *p++;
        name[n] = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '{') module_name_add(names, count, max, name);
    }
    fclose(fp);
    return 0;
}

static int pkg_scan_modules(const char *dir, char names[][64], int *count,
                            int max) {
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL && rc == 0) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char path[PAXSY_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) { rc = -1; break; }
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            rc = pkg_scan_modules(path, names, count, max);
            continue;
        }
        size_t nl = strlen(de->d_name);
        if (nl > 3 && !strcmp(de->d_name + nl - 3, ".px"))
            px_collect_module_names(path, names, count, max);
    }
    closedir(dp);
    return rc;
}

int pkg_modules(const char *pkg, char names[][64], int max) {
    if (!pkg || !*pkg || max <= 0) return -1;
    char root[PAXSY_PATH_MAX];
    const char *env = getenv("PAXSY_LIBRARY_DIR");
    if (env && env[0]) {
        snprintf(root, sizeof(root), "%s/installed/%s/src", env, pkg);
        if (is_dir(root)) {
            int count = 0;
            return pkg_scan_modules(root, names, &count, max) == 0 ? count : -1;
        }
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(root, sizeof(root), "%s/.paxsy/installed/%s/src", home, pkg);
        if (is_dir(root)) {
            int count = 0;
            return pkg_scan_modules(root, names, &count, max) == 0 ? count : -1;
        }
    }
    if (!strcmp(pkg, "std")) {
        char package[PAXSY_PATH_MAX];
        if (std_root(package, sizeof(package)) == 0) {
            size_t plen = strlen(package);
            if (plen + sizeof("/src") <= sizeof(root)) {
                memcpy(root, package, plen);
                memcpy(root + plen, "/src", sizeof("/src"));
            } else root[0] = '\0';
            if (root[0] && is_dir(root)) {
                int count = 0;
                return pkg_scan_modules(root, names, &count, max) == 0 ? count : -1;
            }
        }
    }
    return -1;
}

int std_pkg_prefix(const char *path, char *out, size_t cap) {
    char root[PAXSY_PATH_MAX];
    if (!path || !out || cap == 0) return -1;
    out[0] = '\0';
    if (std_root(root, sizeof(root)) != 0) return -1;
    size_t rl = strlen(root);
    if (strncmp(path, root, rl) != 0 || path[rl] != '/') return -1;
    const char *base = strrchr(root, '/');
    base = base ? base + 1 : root;
    size_t name_len = strlen(base);
    if (name_len > cap || cap - name_len < 3) return -1;
    memcpy(out, base, name_len);
    out[name_len] = ':';
    out[name_len + 1] = ':';
    out[name_len + 2] = '\0';
    return 0;
}

/* The package context of the file currently being lowered. */
static char cur_pkg_prefix[80];
static char cur_pkg_name[64];

void stdpath_set_pkg_prefix(const char *src_path) {
    char buf[80];
    if (src_path && std_pkg_prefix(src_path, buf, sizeof(buf)) == 0) {
        size_t bl = strlen(buf);
        snprintf(cur_pkg_prefix, sizeof(cur_pkg_prefix), "%s", buf);
        if (bl >= 2) bl -= 2;              /* strip the "::" suffix  */
        if (bl >= sizeof(cur_pkg_name)) bl = sizeof(cur_pkg_name) - 1;
        memcpy(cur_pkg_name, buf, bl);
        cur_pkg_name[bl] = '\0';
    } else {
        cur_pkg_prefix[0] = '\0';
        cur_pkg_name[0] = '\0';
    }
}

const char *stdpath_pkg_prefix(void) {
    return cur_pkg_prefix;
}

const char *stdpath_pkg_name(void) {
    return cur_pkg_name;
}
