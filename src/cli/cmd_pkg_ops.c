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
#include "pkgint.h"

#include <unistd.h>

/*
 * cmd_pkg_ops.c - the package operations: install, uninstall,
 * update, add and remove. All commands fall back to the local cache
 * when the network is unreachable. install copies a package from the
 * cache into installed/ without touching the project manifest;
 * recording a dependency in <project>.toml is the job of 'paxsy add'.
 * update refreshes the cached copy first (the cache manager never
 * destroys a working entry on failure) and then rebuilds the
 * installed tree from the cache.
 */

int cmd_install(const Cli *cli) {
    const char *pkg = cli->path;
    if (!pkg) { fprintf(stderr, "paxsy: install requires a package name\n"); return 2; }
    char root[PAXSY_PATH_MAX];
    pkg_cache_root(root, sizeof(root));
    char dst[PAXSY_PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/installed/%s", root, pkg);
    if (fs_is_dir(dst)) {
        fprintf(stderr, "paxsy: '%s' is already installed\n", pkg);
        return 1;
    }
    /* Make the package available locally, then copy into installed/. */
    if (pkg_ensure_cached(root, pkg, cli->arg) != 0) {
        fprintf(stderr, "paxsy: package '%s' not found on paxsy.org "
                        "or in the local cache\n", pkg);
        return 1;
    }
    char srcdir[PAXSY_PATH_MAX];
    snprintf(srcdir, sizeof(srcdir), "%s/cache/%s", root, pkg);
    if (fs_copy_tree(srcdir, dst) != 0) {
        fprintf(stderr, "paxsy: failed to install '%s'\n", pkg);
        return 1;
    }
    const char *version = cli->arg ? cli->arg : "latest";
    printf("installed %s (%s)\n", pkg, version);
    printf("record it with: paxsy add %s%s%s\n",
           pkg, cli->arg ? " " : "", cli->arg ? cli->arg : "");
    return 0;
}

int cmd_uninstall(const Cli *cli) {
    const char *pkg = cli->path;
    if (!pkg) { fprintf(stderr, "paxsy: uninstall requires a package name\n"); return 2; }
    char root[PAXSY_PATH_MAX];
    pkg_cache_root(root, sizeof(root));
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
    pkg_cache_root(root, sizeof(root));
    char dst[PAXSY_PATH_MAX];
    snprintf(dst, sizeof(dst), "%s/installed/%s", root, pkg);
    if (!fs_is_dir(dst)) { fprintf(stderr, "paxsy: '%s' is not installed\n", pkg); return 1; }
    /* Refresh the cached copy when the network allows; the cache
     * manager keeps a working entry when the download fails. The
     * installed tree is then rebuilt from the cache. */
    if (pkg_ensure_cached(root, pkg, cli->arg) != 0) {
        fprintf(stderr, "paxsy: package '%s' not found on paxsy.org "
                        "or in the local cache\n", pkg);
        return 1;
    }
    char cachedir[PAXSY_PATH_MAX];
    snprintf(cachedir, sizeof(cachedir), "%s/cache/%s", root, pkg);
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
