#include "common.h"
#include "toml.h"
#include <dirent.h>

/*
 * toml.c - line-oriented TOML access.
 * The file is loaded into an array of lines; every operation works on
 * that array and the whole file is rewritten afterwards, which keeps
 * foreign content (profiles, scripts) intact.
 */

#define TOML_MAX_LINES 512

static int read_lines(const char *path, char *lines[TOML_MAX_LINES]) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[1024];
    int n = 0;
    while (n < TOML_MAX_LINES && fgets(buf, sizeof(buf), f)) {
        lines[n] = malloc(strlen(buf) + 1);
        strcpy(lines[n], buf);
        n++;
    }
    fclose(f);
    return n;
}

static void free_lines(char *lines[TOML_MAX_LINES], int n) {
    for (int i = 0; i < n; i++) free(lines[i]);
}

static int write_lines(const char *path, char *lines[TOML_MAX_LINES], int n) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (int i = 0; i < n; i++) fputs(lines[i], f);
    fclose(f);
    return 0;
}

/* True when the line is exactly `[name]` (trailing whitespace allowed). */
static int is_section(const char *line, const char *name) {
    char want[128];
    snprintf(want, sizeof(want), "[%s]", name);
    size_t n = strlen(want), m = strlen(line);
    while (m && (line[m-1] == '\n' || line[m-1] == '\r' || line[m-1] == ' '))
        m--;
    return m == n && strncmp(line, want, n) == 0;
}

/* True when the line assigns `key` (key followed by blanks and '='). */
static int is_key(const char *line, const char *key) {
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return 0;
    const char *p = line + klen;
    while (*p == ' ' || *p == '\t') p++;
    return *p == '=';
}

int toml_find(const char *root, char *out, int out_size) {
    const char *base = strrchr(root, '/');
    base = base ? base + 1 : root;
    snprintf(out, out_size, "%s/%s.toml", root, base);
    FILE *f = fopen(out, "r");
    if (f) { fclose(f); return 0; }

    /* Fall back to any .toml file directly inside root. */
    char dirbuf[PAXSY_PATH_MAX];
    snprintf(dirbuf, sizeof(dirbuf), "%s", root);
    DIR *d = opendir(dirbuf);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) && found != 0) {
        size_t len = strlen(e->d_name);
        if (len > 5 && strcmp(e->d_name + len - 5, ".toml") == 0) {
            snprintf(out, out_size, "%s/%s", root, e->d_name);
            found = 0;
        }
    }
    closedir(d);
    return found;
}

int toml_get(const char *path, const char *section, const char *key,
             char *out, int out_size) {
    char *lines[TOML_MAX_LINES];
    int n = read_lines(path, lines);
    if (n < 0) return 0;
    int in_section = 0, ok = 0;
    for (int i = 0; i < n; i++) {
        if (lines[i][0] == '[') in_section = is_section(lines[i], section);
        if (!in_section || !is_key(lines[i], key)) continue;
        const char *v = strchr(lines[i], '=');
        if (!v) continue;
        v++;
        while (*v == ' ' || *v == '\t') v++;
        size_t len = strlen(v);
        while (len && (v[len-1] == '\n' || v[len-1] == '\r' || v[len-1] == ' '))
            len--;
        if (len >= 2 && v[0] == '"' && v[len-1] == '"') { v++; len -= 2; }
        if ((int)len >= out_size) len = (size_t)out_size - 1;
        memcpy(out, v, len);
        out[len] = '\0';
        ok = 1;
        break;
    }
    free_lines(lines, n);
    return ok;
}

int toml_set(const char *path, const char *section, const char *key,
             const char *raw_value) {
    char *lines[TOML_MAX_LINES];
    int n = read_lines(path, lines);
    if (n < 0) return -1;
    if (n == TOML_MAX_LINES) { free_lines(lines, n); return -1; }

    char newline[1024];
    snprintf(newline, sizeof(newline), "%s = %s\n", key, raw_value);

    /* Locate the section header. */
    int sec = -1;
    for (int i = 0; i < n; i++)
        if (is_section(lines[i], section)) { sec = i; break; }

    if (sec < 0) {
        /* Append the section and the key at the end of the file. */
        char header[160];
        snprintf(header, sizeof(header), "\n[%s]\n", section);
        lines[n] = malloc(strlen(header) + 1); strcpy(lines[n], header); n++;
        lines[n] = malloc(strlen(newline) + 1); strcpy(lines[n], newline); n++;
    } else {
        /* Replace an existing key line or insert right after header. */
        int replaced = -1;
        int end = sec + 1;
        while (end < n && lines[end][0] != '[') {
            if (replaced < 0 && is_key(lines[end], key)) replaced = end;
            end++;
        }
        if (replaced >= 0) {
            free(lines[replaced]);
            lines[replaced] = malloc(strlen(newline) + 1);
            strcpy(lines[replaced], newline);
        } else {
            for (int i = n; i > sec + 1; i--) lines[i] = lines[i - 1];
            lines[sec + 1] = malloc(strlen(newline) + 1);
            strcpy(lines[sec + 1], newline);
            n++;
        }
    }
    int rc = write_lines(path, lines, n);
    free_lines(lines, n);
    return rc;
}

int toml_unset(const char *path, const char *section, const char *key) {
    char *lines[TOML_MAX_LINES];
    int n = read_lines(path, lines);
    if (n < 0) return 0;
    int in_section = 0, removed = 0;
    for (int i = 0; i < n; i++) {
        if (lines[i][0] == '[') in_section = is_section(lines[i], section);
        if (!in_section || !is_key(lines[i], key)) continue;
        free(lines[i]);
        for (int j = i; j < n - 1; j++) lines[j] = lines[j + 1];
        n--;
        removed = 1;
        break;
    }
    if (removed) write_lines(path, lines, n);
    free_lines(lines, n);
    return removed;
}
