#define _POSIX_C_SOURCE 200809L

/* Path buffers are bounded by PAXSY_PATH_MAX; snprintf truncation is
 * always safe (NUL-terminated output), so the compiler warning about
 * theoretical truncation is disabled for this file. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "common.h"
#include "commands.h"
#include "cli.h"
#include "toml.h"
#include "fsutil.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

/*
 * cmd_pkg.c - the package manager.
 *
 * The canonical index lives at https://paxsy.org/packages/index.txt
 * (one "name version description" line per package); package tarballs
 * live next to it as <name>.tar.gz and <name>-<version>.tar.gz.
 * fetch downloads the index, install/update download and unpack
 * tarballs into the local cache; every step falls back to the local
 * cache ($PAXSY_LIBRARY_DIR or ~/.paxsy) when the network is
 * unreachable, so the manager works offline as well.
 */

#define PKG_BASE "https://paxsy.org/packages"

/* Resolves the cache root, creating its subdirectories. */
static int cache_root(char *out, int n) {
    const char *env = getenv("PAXSY_LIBRARY_DIR");
    if (env && env[0]) snprintf(out, (size_t)n, "%s", env);
    else {
        const char *home = getenv("HOME");
        snprintf(out, (size_t)n, "%s/%s", home ? home : ".", ".paxsy");
    }
    char sub[PAXSY_PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/cache", out);
    if (fs_mkdir_p(sub) != 0) return -1;
    snprintf(sub, sizeof(sub), "%s/installed", out);
    if (fs_mkdir_p(sub) != 0) return -1;
    return 0;
}

/* Runs argv[0] with argv in a child process; returns 0 when the tool
 * exits successfully and -1 when it is missing or fails. A tool that
 * is not installed exits 127, which is reported as -1. */
static int run_tool(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Downloads url to dest, trying curl and then wget. Returns 0 on
 * success and -1 when no downloader is available or the transfer
 * failed. */
static int download(const char *url, const char *dest) {
    char *const curl_argv[] = { "curl", "-fsSL", "-o", (char *)dest, (char *)url, NULL };
    if (run_tool(curl_argv) == 0) return 0;
    char *const wget_argv[] = { "wget", "-qO", (char *)dest, (char *)url, NULL };
    if (run_tool(wget_argv) == 0) return 0;
    return -1;
}

/* Unpacks a tarball into dir. */
static int unpack(const char *archive, const char *dir) {
    char *const argv[] = { "tar", "-xzf", (char *)archive, "-C", (char *)dir, NULL };
    return run_tool(argv);
}

/* Makes sure cache/<pkg> exists: downloads the package tarball from
 * the repository and unpacks it when the cache has no copy yet. */
static int ensure_cached(const char *root, const char *pkg, const char *version) {
    char cachedir[PAXSY_PATH_MAX];
    snprintf(cachedir, sizeof(cachedir), "%s/cache/%s", root, pkg);
    if (fs_is_dir(cachedir)) return 0;

    char url[640];
    if (version && version[0])
        snprintf(url, sizeof(url), "%s/%s-%s.tar.gz", PKG_BASE, pkg, version);
    else
        snprintf(url, sizeof(url), "%s/%s.tar.gz", PKG_BASE, pkg);

    char archive[PAXSY_PATH_MAX];
    snprintf(archive, sizeof(archive), "%s.tar.gz.part", cachedir);
    if (download(url, archive) != 0) {
        unlink(archive);
        return -1;
    }
    if (fs_mkdir_p(cachedir) != 0 || unpack(archive, cachedir) != 0) {
        unlink(archive);
        return -1;
    }
    unlink(archive);
    return 0;
}

/* Finds the first *.toml directly inside dir (package manifest). */
static int first_toml(const char *dir, char *out, int n) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) && found != 0) {
        size_t len = strlen(e->d_name);
        if (len > 5 && strcmp(e->d_name + len - 5, ".toml") == 0) {
            snprintf(out, (size_t)n, "%s/%s", dir, e->d_name);
            found = 0;
        }
    }
    closedir(d);
    return found;
}

/* Appends one "name version description" line per package directory
 * directly inside dir to the index stream. */
static int index_scan(FILE *out, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char pkgdir[PAXSY_PATH_MAX];
        snprintf(pkgdir, sizeof(pkgdir), "%s/%s", dir, e->d_name);
        if (!fs_is_dir(pkgdir)) continue;
        char toml[PAXSY_PATH_MAX];
        if (first_toml(pkgdir, toml, sizeof(toml)) != 0) continue;
        char name[128] = "", version[64] = "0.1.0", desc[256] = "";
        toml_get(toml, "project", "name", name, sizeof(name));
        toml_get(toml, "project", "version", version, sizeof(version));
        toml_get(toml, "project", "description", desc, sizeof(desc));
        if (!name[0]) snprintf(name, sizeof(name), "%s", e->d_name);
        fprintf(out, "%s %s %s\n", name, version, desc);
        count++;
    }
    closedir(d);
    return count;
}

/* Case-insensitive substring test used by search. */
static int ci_contains(const char *line, const char *term) {
    char l[1024], t[256];
    snprintf(l, sizeof(l), "%s", line);
    snprintf(t, sizeof(t), "%s", term);
    for (char *p = l; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strstr(l, t) != NULL;
}

int cmd_fetch(const Cli *cli) {
    (void)cli;
    char root[PAXSY_PATH_MAX];
    if (cache_root(root, sizeof(root)) != 0) {
        fprintf(stderr, "paxsy: cannot create cache: %s\n", strerror(errno));
        return 1;
    }
    char index[PAXSY_PATH_MAX];
    snprintf(index, sizeof(index), "%s/index.txt", root);

    /* Online: refresh the index straight from the repository. */
    char tmp[PAXSY_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.part", index);
    if (download(PKG_BASE "/index.txt", tmp) == 0) {
        rename(tmp, index);
        int count = 0;
        char line[1024];
        FILE *f = fopen(index, "r");
        while (f && fgets(line, sizeof(line), f)) count++;
        if (f) fclose(f);
        printf("indexed %d package(s) from paxsy.org\n", count);
        return 0;
    }
    unlink(tmp);

    /* Offline: rebuild the index from everything available locally:
     * downloaded tarballs (cache/) and installed packages (installed/). */
    FILE *out = fopen(index, "w");
    if (!out) { fprintf(stderr, "paxsy: cannot write '%s'\n", index); return 1; }
    int count = 0;
    char dirbuf[PAXSY_PATH_MAX];
    snprintf(dirbuf, sizeof(dirbuf), "%s/cache", root);
    count += index_scan(out, dirbuf);
    snprintf(dirbuf, sizeof(dirbuf), "%s/installed", root);
    count += index_scan(out, dirbuf);
    fclose(out);
    printf("indexed %d package(s) from the local cache\n", count);
    return 0;
}

int cmd_search(const Cli *cli) {
    const char *term = cli->path;
    if (!term) { fprintf(stderr, "paxsy: search requires a term\n"); return 2; }
    char root[PAXSY_PATH_MAX];
    cache_root(root, sizeof(root));
    char index[PAXSY_PATH_MAX];
    snprintf(index, sizeof(index), "%s/index.txt", root);
    FILE *f = fopen(index, "r");
    if (!f) { fprintf(stderr, "paxsy: run 'paxsy fetch' first\n"); return 1; }
    char line[1024];
    int hits = 0;
    while (fgets(line, sizeof(line), f))
        if (ci_contains(line, term)) { fputs(line, stdout); hits++; }
    fclose(f);
    if (!hits) printf("no packages match '%s'\n", term);
    return 0;
}

int cmd_install(const Cli *cli) {
    const char *pkg = cli->path;
    if (!pkg) { fprintf(stderr, "paxsy: install requires a package name\n"); return 2; }
    char root[PAXSY_PATH_MAX];
    cache_root(root, sizeof(root));
    char dst[PAXSY_PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/installed/%s", root, pkg);
    if (fs_is_dir(dst)) {
        fprintf(stderr, "paxsy: '%s' is already installed\n", pkg);
        return 1;
    }
    /* Make the package available locally, then copy into installed/. */
    if (ensure_cached(root, pkg, cli->arg) != 0) {
        fprintf(stderr, "paxsy: package '%s' not found on paxsy.org "
                        "or in the local cache\n", pkg);
        return 1;
    }
    char src[PAXSY_PATH_MAX];
    snprintf(src, sizeof(src), "%s/cache/%s", root, pkg);
    if (fs_copy_tree(src, dst) != 0) {
        fprintf(stderr, "paxsy: failed to install '%s'\n", pkg);
        return 1;
    }
    const char *version = cli->arg ? cli->arg : "latest";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(".", cfg, sizeof(cfg)) == 0) {
        char quoted[300];
        snprintf(quoted, sizeof(quoted), "\"%s\"", version);
        toml_set(cfg, "dependencies", pkg, quoted);
    }
    printf("installed %s (%s)\n", pkg, version);
    return 0;
}

int cmd_uninstall(const Cli *cli) {
    const char *pkg = cli->path;
    if (!pkg) { fprintf(stderr, "paxsy: uninstall requires a package name\n"); return 2; }
    char root[PAXSY_PATH_MAX];
    cache_root(root, sizeof(root));
    char dst[PAXSY_PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/installed/%s", root, pkg);
    if (!fs_is_dir(dst)) {
        fprintf(stderr, "paxsy: '%s' is not installed\n", pkg);
        return 1;
    }
    if (fs_remove_tree(dst) != 0) {
        fprintf(stderr, "paxsy: failed to remove '%s'\n", dst);
        return 1;
    }
    printf("uninstalled %s\n", pkg);
    return 0;
}

int cmd_update(const Cli *cli) {
    const char *pkg = cli->path;
    if (!pkg) { fprintf(stderr, "paxsy: update requires a package name\n"); return 2; }
    char root[PAXSY_PATH_MAX];
    cache_root(root, sizeof(root));
    char dst[PAXSY_PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/installed/%s", root, pkg);
    if (!fs_is_dir(dst)) { fprintf(stderr, "paxsy: '%s' is not installed\n", pkg); return 1; }
    /* Refresh the cached copy (re-downloaded when the network allows)
     * and replace the installed tree with it. */
    char cachedir[PAXSY_PATH_MAX];
    snprintf(cachedir, sizeof(cachedir), "%s/cache/%s", root, pkg);
    fs_remove_tree(cachedir);
    if (ensure_cached(root, pkg, cli->arg) != 0) {
        fprintf(stderr, "paxsy: package '%s' not found on paxsy.org "
                        "or in the local cache\n", pkg);
        return 1;
    }
    if (fs_remove_tree(dst) != 0 || fs_copy_tree(cachedir, dst) != 0) {
        fprintf(stderr, "paxsy: failed to update '%s'\n", pkg);
        return 1;
    }
    printf("updated %s\n", pkg);
    return 0;
}

int cmd_add(const Cli *cli) {
    const char *pkg = cli->path;
    if (!pkg) { fprintf(stderr, "paxsy: add requires a package name\n"); return 2; }
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(".", cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: not inside a project (no configuration file)\n");
        return 1;
    }
    const char *version = cli->arg ? cli->arg : "latest";
    char quoted[300];
    snprintf(quoted, sizeof(quoted), "\"%s\"", version);
    if (toml_set(cfg, "dependencies", pkg, quoted) != 0) {
        fprintf(stderr, "paxsy: cannot update '%s'\n", cfg);
        return 1;
    }
    printf("added %s = \"%s\"\n", pkg, version);
    return 0;
}

int cmd_remove(const Cli *cli) {
    const char *pkg = cli->path;
    if (!pkg) { fprintf(stderr, "paxsy: remove requires a package name\n"); return 2; }
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(".", cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: not inside a project (no configuration file)\n");
        return 1;
    }
    if (!toml_unset(cfg, "dependencies", pkg)) {
        fprintf(stderr, "paxsy: '%s' is not a dependency\n", pkg);
        return 1;
    }
    printf("removed dependency %s\n", pkg);
    return 0;
}
