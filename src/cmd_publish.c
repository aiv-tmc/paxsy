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

/*
 * cmd_publish.c - the publish command: assembles dist/<name>-<version>/
 * containing the sources, the configuration and the documentation,
 * ready to be archived and uploaded to the package repository.
 */

int cmd_publish(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: no configuration file in '%s'\n", root);
        return 2;
    }
    char name[128] = "", version[64] = "0.1.0";
    toml_get(cfg, "project", "name", name, sizeof(name));
    toml_get(cfg, "project", "version", version, sizeof(version));
    if (!name[0]) {
        const char *base = strrchr(cfg, '/');
        base = base ? base + 1 : cfg;
        snprintf(name, sizeof(name), "%.*s", (int)(strlen(base) - 5), base);
    }
    char dist[1024];
    snprintf(dist, sizeof(dist), "%s/dist/%s-%s", root, name, version);
    if (fs_is_dir(dist)) {
        fprintf(stderr, "paxsy: '%s' already exists\n", dist);
        return 1;
    }
    if (fs_mkdir_p(dist) != 0) {
        fprintf(stderr, "paxsy: cannot create '%s'\n", dist);
        return 1;
    }
    char srcdir[PAXSY_PATH_MAX], dstdir[1024];
    snprintf(srcdir, sizeof(srcdir), "%s/src", root);
    snprintf(dstdir, sizeof(dstdir), "%s/src", dist);
    if (fs_is_dir(srcdir) && fs_copy_tree(srcdir, dstdir) != 0) {
        fprintf(stderr, "paxsy: failed to copy sources\n");
        return 1;
    }
    const char *base = strrchr(cfg, '/');
    base = base ? base + 1 : cfg;
    snprintf(dstdir, sizeof(dstdir), "%s/%s", dist, base);
    if (fs_copy_file(cfg, dstdir) != 0) {
        fprintf(stderr, "paxsy: failed to copy the configuration\n");
        return 1;
    }
    snprintf(srcdir, sizeof(srcdir), "%s/doc.md", root);
    snprintf(dstdir, sizeof(dstdir), "%s/doc.md", dist);
    if (fs_is_file(srcdir)) fs_copy_file(srcdir, dstdir);
    printf("published %s-%s -> %s\n", name, version, dist);
    return 0;
}
