#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "commands.h"
#include "cli.h"
#include "toml.h"

#include <sys/stat.h>

/*
 * cmd_meta.c - project information commands:
 *   meta   prints the [project] metadata as a table;
 *   config reads or writes a single section.key value.
 */

static void get_or_base(const char *cfg, const char *key, char *out, int n,
                        const char *fallback) {
    if (!toml_get(cfg, "project", key, out, n)) snprintf(out, (size_t)n, "%s", fallback);
}

int cmd_meta(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "no configuration file in '%s'\n", root);
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
        snprintf(opt, sizeof(opt), "3");
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
        fprintf(stderr, "config requires section.key\n");
        return 2;
    }
    const char *dot = strchr(spec, '.');
    if (!dot || dot == spec || !dot[1]) {
        fprintf(stderr, "key must have the form section.key\n");
        return 2;
    }
    char section[96], key[96];
    snprintf(section, sizeof(section), "%.*s", (int)(dot - spec), spec);
    snprintf(key, sizeof(key), "%s", dot + 1);
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(".", cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "not inside a project (no configuration file)\n");
        return 1;
    }
    const char *value = cli->arg;
    if (!value) {
        char out[256];
        if (!toml_get(cfg, section, key, out, sizeof(out))) {
            fprintf(stderr, "'%s' is not set\n", spec);
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
        fprintf(stderr, "cannot update '%s'\n", cfg);
        return 1;
    }
    printf("%s = %s\n", spec, raw);
    return 0;
}

