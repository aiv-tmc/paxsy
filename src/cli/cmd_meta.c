#include "common.h"
#include "commands.h"
#include "cli.h"
#include "toml.h"

#include <sys/stat.h>
#include <dirent.h>

/*
 * cmd_meta.c - project information commands:
 *   meta   prints the [project] metadata as a table;
 *   config reads or writes a single section.key value;
 *   doc    regenerates doc.md from /// documentation comments.
 */

static void get_or_base(const char *cfg, const char *key, char *out, int n,
                        const char *fallback) {
    if (!toml_get(cfg, "project", key, out, n)) snprintf(out, (size_t)n, "%s", fallback);
}

int cmd_meta(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: no configuration file in '%s'\n", root);
        return 2;
    }
    char name[128], version[64], desc[256], authors[256], target[32], opt[32];
    get_or_base(cfg, "name", name, sizeof(name), "?");
    get_or_base(cfg, "version", version, sizeof(version), "0.1.0");
    get_or_base(cfg, "description", desc, sizeof(desc), "");
    get_or_base(cfg, "authors", authors, sizeof(authors), "[]");
    get_or_base(cfg, "target", target, sizeof(target), "elf");
    if (!toml_get(cfg, "build", "target", target, sizeof(target)))
        snprintf(target, sizeof(target), "elf");
    if (!toml_get(cfg, "build", "optimization", opt, sizeof(opt)))
        snprintf(opt, sizeof(opt), "2");
    printf("project:     %s\n", name);
    printf("version:     %s\n", version);
    printf("description: %s\n", desc);
    printf("authors:     %s\n", authors);
    printf("target:      %s\n", target);
    printf("optimization: %s\n", opt);
    return 0;
}

int cmd_config(const Cli *cli) {
    const char *spec = cli->path;
    if (!spec) {
        fprintf(stderr, "paxsy: config requires section.key\n");
        return 2;
    }
    const char *dot = strchr(spec, '.');
    if (!dot || dot == spec || !dot[1]) {
        fprintf(stderr, "paxsy: key must have the form section.key\n");
        return 2;
    }
    char section[96], key[96];
    snprintf(section, sizeof(section), "%.*s", (int)(dot - spec), spec);
    snprintf(key, sizeof(key), "%s", dot + 1);
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(".", cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: not inside a project (no configuration file)\n");
        return 1;
    }
    const char *value = cli->arg;
    if (!value) {
        char out[256];
        if (!toml_get(cfg, section, key, out, sizeof(out))) {
            fprintf(stderr, "paxsy: '%s' is not set\n", spec);
            return 1;
        }
        printf("%s\n", out);
        return 0;
    }
    /* Strings are quoted, numbers and booleans pass through verbatim. */
    char raw[300];
    int numeric = value[0] && strspn(value, "0123456789.-") == strlen(value);
    int boolean = strcmp(value, "true") == 0 || strcmp(value, "false") == 0;
    if (value[0] == '"' || numeric || boolean) snprintf(raw, sizeof(raw), "%s", value);
    else snprintf(raw, sizeof(raw), "\"%s\"", value);
    if (toml_set(cfg, section, key, raw) != 0) {
        fprintf(stderr, "paxsy: cannot update '%s'\n", cfg);
        return 1;
    }
    printf("%s = %s\n", spec, raw);
    return 0;
}

/* One documentation block collected from a source file. */
typedef struct {
    char decl[160];             /* declaration line following the block */
    char lines[16][200];        /* the /// text without the markers */
    int line_count;
} DocBlock;

/* Streams a file and appends every /// block to the markdown output. */
static void extract_docs(const char *path, FILE *out) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    DocBlock blk;
    memset(&blk, 0, sizeof(blk));
    int in_block = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "///", 3) == 0) {
            in_block = 1;
            if (blk.line_count < 16) {
                char *text = line + 3;
                while (*text == ' ') text++;
                /* %.199s bounds the copy so the compiler can prove the
                 * result always fits the 200-byte field. */
                snprintf(blk.lines[blk.line_count], 200, "%.199s", text);
                blk.line_count++;
            }
            continue;
        }
        if (in_block) {
            in_block = 0;
            char *p = line;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p) {
                snprintf(blk.decl, sizeof(blk.decl), "%.*s", 150, p);
                char *nl = strchr(blk.decl, '\n');
                if (nl) *nl = '\0';
            }
            if (blk.line_count) {
                fprintf(out, "### `%s`\n\n", blk.decl[0] ? blk.decl : "item");
                for (int i = 0; i < blk.line_count; i++)
                    fprintf(out, "%s", blk.lines[i]);
                fprintf(out, "\n");
            }
            memset(&blk, 0, sizeof(blk));
        }
    }
    fclose(f);
}

static void doc_dir(const char *dir, FILE *out) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[PAXSY_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { doc_dir(path, out); continue; }
        size_t len = strlen(e->d_name);
        if (len > 3 && strcmp(e->d_name + len - 3, ".px") == 0) {
            fprintf(out, "#### `%s`\n\n", path);
            extract_docs(path, out);
        }
    }
    closedir(d);
}

int cmd_doc(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: no configuration file in '%s'\n", root);
        return 2;
    }
    char name[128] = "";
    toml_get(cfg, "project", "name", name, sizeof(name));
    if (!name[0]) {
        const char *base = strrchr(cfg, '/');
        base = base ? base + 1 : cfg;
        snprintf(name, sizeof(name), "%.*s", (int)(strlen(base) - 5), base);
    }
    char doc_path[PAXSY_PATH_MAX];
    snprintf(doc_path, sizeof(doc_path), "%s/doc.md", root);
    FILE *out = fopen(doc_path, "w");
    if (!out) {
        fprintf(stderr, "paxsy: cannot write '%s'\n", doc_path);
        return 1;
    }
    fprintf(out, "# %s\n\nDocumentation generated from /// doc comments.\n\n",
            name);
    fprintf(out, "## API reference\n\n");
    char srcdir[PAXSY_PATH_MAX];
    snprintf(srcdir, sizeof(srcdir), "%s/src", root);
    doc_dir(srcdir, out);
    fclose(out);
    if (!cli->quiet) printf("regenerated %s\n", doc_path);
    return 0;
}
