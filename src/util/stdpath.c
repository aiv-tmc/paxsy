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
 *   2. <exe>/../lib/paxsy/std             (canonical install where
 *                                          the binary sits in bin/);
 *   3. <exe>/lib/std                      (build tree and test
 *                                          layouts with the binary
 *                                          at the tree root);
 *   4. ~/.paxsy/installed/std             (default user cache).
 *
 * The first existing directory wins. std_collect_libs() walks only
 * the requested library directories (or the whole package for "*"),
 * mirroring the deterministic rules of srcdir: regular files ending
 * in .px, full path stored with PAXSY_PATH_MAX truncation treated as
 * a hard error, and nothing else. pkg_exports() parses the package
 * manifest: the [modules] and [exports] table keys are the members
 * a use directive may name; the manifest is searched first in the
 * installed-package cache ($PAXSY_LIBRARY_DIR/installed/<pkg> and
 * ~/.paxsy/installed/<pkg>) and then, when the resolved std package
 * carries the requested name, inside the std package directory. The
 * announced prefix is per compile unit and single-threaded by
 * construction: the driver announces the file before lowering and
 * lower.c consumes the value while naming that file's modules.
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

/* True when `name` (a library directory name) appears in the
 * comma-separated `wanted` list. */
static int lib_wanted(const char *wanted, const char *name) {
    size_t nl = strlen(name);
    const char *p = wanted;
    while (*p) {
        while (*p == ',') p++;
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == nl && strncmp(p, name, nl) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int std_collect_libs(SrcList *list, const char *wanted) {
    char root[PAXSY_PATH_MAX];
    if (std_root(root, sizeof(root)) != 0) return -1;
    if (strcmp(wanted, "*") == 0) {
        /* The whole package: every library directory plus the
         * package root sources under src/. */
        char srcdir[PAXSY_PATH_MAX+8];
        snprintf(srcdir, sizeof(srcdir), "%s/src", root);
        if (std_walk(srcdir, list) != 0) return -1;
    }
    DIR *dp = opendir(root);
    if (!dp) return -1;
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL && rc == 0) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (strcmp(de->d_name, "src") == 0) continue;  /* root module: only with "*" */
        if (!lib_wanted(wanted, de->d_name)) continue;
        char dir[PAXSY_PATH_MAX];
        int n = snprintf(dir, sizeof(dir), "%s/%s", root, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(dir)) { rc = -1; break; }
        struct stat st;
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
            rc = std_walk(dir, list);
    }
    closedir(dp);
    return rc;
}

/* Joins dir, '/' and pkg, ".toml" with an explicit capacity check,
 * which keeps the build free of format-truncation warnings. */
static int join_toml(char *path, size_t cap, const char *dir,
                     const char *pkg) {
    size_t dl = strlen(dir), pl = strlen(pkg);
    if (dl + 1 + pl + 5 + 1 > cap) return -1;
    memcpy(path, dir, dl);
    path[dl] = '/';
    memcpy(path + dl + 1, pkg, pl);
    memcpy(path + dl + 1 + pl, ".toml", 6);
    return 0;
}

/* Parses the [modules] and [exports] table keys of one manifest
 * file; returns the member count, or -1 when the file is absent. */
static int read_exports_toml(const char *path, char names[][64], int max) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int count = 0;
    int in_members = 0;
    char line[128];
    while (count < max && fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        char *e = p + strlen(p);
        while (e > p && isspace((unsigned char)e[-1])) *--e = '\0';
        if (*p == '[') {
            in_members = strcmp(p, "[modules]") == 0 ||
                         strcmp(p, "[exports]") == 0;
            continue;
        }
        if (!in_members) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = p;
        char *ke = k + strlen(k);
        while (ke > k && isspace((unsigned char)ke[-1])) *--ke = '\0';
        if (!*k) continue;
        snprintf(names[count], 64, "%s", k);
        count++;
    }
    fclose(fp);
    return count;
}

int pkg_exports(const char *pkg, char names[][64], int max) {
    char path[PAXSY_PATH_MAX];
    char dir[PAXSY_PATH_MAX];
    int n;
    const char *env = getenv("PAXSY_LIBRARY_DIR");
    if (env && env[0]) {
        snprintf(dir, sizeof(dir), "%s/installed/%s", env, pkg);
        if (join_toml(path, sizeof(path), dir, pkg) == 0) {
            n = read_exports_toml(path, names, max);
            if (n >= 0) return n;
        }
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(dir, sizeof(dir), "%s/.paxsy/installed/%s", home, pkg);
        if (join_toml(path, sizeof(path), dir, pkg) == 0) {
            n = read_exports_toml(path, names, max);
            if (n >= 0) return n;
        }
    }
    char root[PAXSY_PATH_MAX];
    if (std_root(root, sizeof(root)) == 0) {
        const char *base = strrchr(root, '/');
        if (base && strcmp(base + 1, pkg) == 0) {
            if (join_toml(path, sizeof(path), root, pkg) == 0) {
                n = read_exports_toml(path, names, max);
                if (n >= 0) return n;
            }
        }
    }
    return -1;
}

int std_pkg_prefix(const char *path, char *out, size_t cap) {
    char root[PAXSY_PATH_MAX];
    out[0] = '\0';
    if (std_root(root, sizeof(root)) != 0) return -1;
    size_t rl = strlen(root);
    if (strncmp(path, root, rl) != 0 || path[rl] != '/') return -1;
    const char *base = strrchr(root, '/');
    snprintf(out, cap, "%s::", base ? base + 1 : root);
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
