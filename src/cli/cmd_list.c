#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "commands.h"
#include "cli.h"
#include "toml.h"
#include "fsutil.h"
#include "pkgdir.h"

#include <sys/stat.h>
#include <dirent.h>

/*
 * cmd_list.c - the list command: prints every package installed in
 * the local cache together with its version and description, read
 * from the package manifest.
 */

int cmd_list(const Cli *cli) {
    (void)cli;
    const char *env = getenv("PAXSY_LIBRARY_DIR");
    char root[512];
    if (env && env[0]) snprintf(root, sizeof(root), "%s", env);
    else snprintf(root, sizeof(root), "%s/.paxsy",
                  getenv("HOME") ? getenv("HOME") : ".");

    char dir[600];
    snprintf(dir, sizeof(dir), "%s/installed", root);

    DIR *d = opendir(dir);
    if (!d) { printf("no packages installed\n"); return 0; }

    printf("%-16s %-9s %s\n", "name", "version", "description");
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char pkgdir[900];
        snprintf(pkgdir, sizeof(pkgdir), "%s/%s", dir, e->d_name);
        if (!fs_is_dir(pkgdir)) continue;
        char toml[1200];
        DIR *pd = opendir(pkgdir);
        if (!pd) continue;
        struct dirent *pe;
        int found = 0;
        char name[128] = "", version[64] = "-", desc[256] = "";
        while ((pe = readdir(pd))) {
            size_t len = strlen(pe->d_name);
            if (len > 5 && strcmp(pe->d_name + len - 5, ".toml") == 0) {
                snprintf(toml, sizeof(toml), "%s/%s", pkgdir, pe->d_name);
                toml_get(toml, "project", "name", name, sizeof(name));
                toml_get(toml, "project", "version", version, sizeof(version));
                toml_get(toml, "project", "description", desc, sizeof(desc));
                found = 1;
                break;
            }
        }
        closedir(pd);
        if (!found) continue;
        if (!name[0]) snprintf(name, sizeof(name), "%.127s", e->d_name);
        printf("%-16s %-9s %s\n", name, version, desc);
        count++;
    }
    closedir(d);
    if (!count) printf("no packages installed\n");
    return 0;
}
