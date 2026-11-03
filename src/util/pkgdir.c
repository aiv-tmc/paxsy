/* dirent and the package cache layout require POSIX names. */
#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "pkgdir.h"

#include <dirent.h>
#include <sys/stat.h>

/*
 * pkgdir.c - lists the installed packages. The cache root is
 * $PAXSY_LIBRARY_DIR or ~/.paxsy, matching the package manager.
 */

int pkg_list_installed(char names[][PKG_NAME_MAX], int max) {
    const char *env = getenv("PAXSY_LIBRARY_DIR");
    char root[512];
    if (env && env[0]) snprintf(root, sizeof(root), "%s", env);
    else snprintf(root, sizeof(root), "%s/.paxsy", getenv("HOME") ? getenv("HOME") : ".");

    char dir[600];
    snprintf(dir, sizeof(dir), "%s/installed", root);

    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        char path[900];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        snprintf(names[n], PKG_NAME_MAX, "%.63s", e->d_name);
        n++;
    }
    closedir(d);
    return n;
}
