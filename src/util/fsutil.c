#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "fsutil.h"

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

/*
 * fsutil.c - plain file-system operations used by the project and
 * package commands. All paths are bounded by PAXSY_PATH_MAX; callers
 * assemble them with snprintf.
 */

int fs_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int fs_is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int fs_mkdir_p(const char *path) {
    char tmp[PAXSY_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t n = strlen(tmp);
    if (n && tmp[n - 1] == '/') tmp[n - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int fs_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return -1; }
    char buf[8192];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    fclose(in);
    fclose(out);
    return rc;
}

int fs_copy_tree(const char *src, const char *dst) {
    if (fs_mkdir_p(dst) != 0) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) && rc == 0) {
        if (e->d_name[0] == '.') continue;
        char s[PAXSY_PATH_MAX], t[PAXSY_PATH_MAX];
        snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
        struct stat st;
        if (stat(s, &st) != 0) { rc = -1; continue; }
        if (S_ISDIR(st.st_mode)) rc = fs_copy_tree(s, t);
        else rc = fs_copy_file(s, t);
    }
    closedir(d);
    return rc;
}

int fs_remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) return unlink(path);
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char child[PAXSY_PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) { rc = -1; continue; }
        if (S_ISDIR(st.st_mode)) rc |= fs_remove_tree(child);
        else if (unlink(child) != 0) rc = -1;
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}
