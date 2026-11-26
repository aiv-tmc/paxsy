#include "common.h"
#include "sema_int.h"
#include "stdpath.h"

#include <dirent.h>
#include <sys/stat.h>

static int identifier_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

static int line_declares(const char *line, int *in_block,
                         const char *symbol) {
    const char *p = line;
    char word[128];
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (*in_block) {
            const char *end = strstr(p, "*/");
            if (!end) return 0;
            p = end + 2;
            *in_block = 0;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') return 0;
        if (p[0] == '/' && p[1] == '*') {
            *in_block = 1;
            p += 2;
            continue;
        }
        if (*p == '\'' || *p == '"') {
            char quote = *p++;
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (*p) p++;
            continue;
        }
        const char *start = p;
        while (identifier_char((unsigned char)*p)) p++;
        size_t n = (size_t)(p - start);
        if (n == 0) { p++; continue; }
        if ((n == 4 && memcmp(start, "func", 4) == 0) ||
            (n == 6 && memcmp(start, "static", 6) == 0) ||
            (n == 6 && memcmp(start, "struct", 6) == 0) ||
            (n == 4 && memcmp(start, "enum", 4) == 0) ||
            (n == 5 && memcmp(start, "union", 5) == 0)) {
            while (isspace((unsigned char)*p)) p++;
            start = p;
            while (identifier_char((unsigned char)*p)) p++;
            n = (size_t)(p - start);
            if (n == 0 || n >= sizeof(word)) continue;
            memcpy(word, start, n);
            word[n] = '\0';
            if (*p == '.') {
                const char *method = ++p;
                while (identifier_char((unsigned char)*p)) p++;
                size_t m = (size_t)(p - method);
                if (m > 0 && n + m + 1 < sizeof(word)) {
                    word[n] = '.';
                    memcpy(word + n + 1, method, m);
                    word[n + m + 1] = '\0';
                }
            }
            if (strcmp(word, symbol) == 0) return 1;
        }
    }
    return 0;
}

static int scan_file(const char *path, const char *symbol) {
    FILE *f = fopen(path, "r");
    char line[2048];
    int in_block = 0, found = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (line_declares(line, &in_block, symbol)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int scan_dir(const char *dir, const char *symbol, int depth) {
    DIR *d;
    struct dirent *entry;
    if (depth > 16 || !(d = opendir(dir))) return 0;
    while ((entry = readdir(d)) != NULL) {
        char path[PAXSY_PATH_MAX];
        struct stat st;
        if (entry->d_name[0] == '.') continue;
        int n = snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path) || stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (scan_dir(path, symbol, depth + 1)) {
                closedir(d);
                return 1;
            }
        } else if (S_ISREG(st.st_mode) &&
                   strlen(entry->d_name) > 3 &&
                   strcmp(entry->d_name + strlen(entry->d_name) - 3,
                          ".px") == 0 && scan_file(path, symbol)) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

int sm_external_member_exists(const char *package, const char *module,
                               const char *symbol) {
    char root[PAXSY_PATH_MAX], names[PAXSY_MAX_MODULES][64];
    char dir[PAXSY_PATH_MAX], module_path[256];
    int count, n, i;
    if (strcmp(package, "std") != 0) return -1;
    const char *sep = strstr(module, "::");
    size_t first_len = sep ? (size_t)(sep - module) : strlen(module);
    if (first_len == 0 || first_len >= sizeof(names[0])) return -1;
    char first[64];
    memcpy(first, module, first_len);
    first[first_len] = '\0';
    count = pkg_modules(package, names, PAXSY_MAX_MODULES);
    if (count < 0) return -1;
    for (i = 0; i < count; i++)
        if (strcmp(names[i], first) == 0) break;
    if (i == count) return -1;
    if (std_root(root, sizeof(root)) != 0) return -1;
    snprintf(module_path, sizeof(module_path), "%s", module);
    for (char *p = module_path; (p = strstr(p, "::")) != NULL; p += 1) {
        p[0] = '/';
        memmove(p + 1, p + 2, strlen(p + 2) + 1);
    }
    n = snprintf(dir, sizeof(dir), "%s/src/%s", root, module_path);
    if (n < 0 || (size_t)n >= sizeof(dir)) return -1;
    return scan_dir(dir, symbol, 0);
}
