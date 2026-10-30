#define _POSIX_C_SOURCE 200809L

/* Path buffers are bounded by PAXSY_PATH_MAX; snprintf truncation is
 * always safe (NUL-terminated output), so the compiler warning about
 * theoretical truncation is disabled for this file. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "common.h"
#include "commands.h"
#include "color.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/*
 * cmd_project.c - project lifecycle commands:
 *   init / new  create a minimal project: src/main.px printing a
 *               greeting and <project>.toml, then initialise a git
 *               repository when git is available;
 *   clean       delete the generated build/, target/ and debug/ trees;
 *   tree        render the project layout with box-drawing characters.
 * The init confirmation is a single line; nothing else is printed.
 */

/* Creates every missing directory component of path (mkdir -p). */
static int mkdir_p(const char *path) {
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

/* Writes content to path, creating or truncating the file. */
static int write_text(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Recursively deletes a directory tree (rm -rf equivalent). */
static int remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) return unlink(path);
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[PAXSY_PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) { rc = -1; continue; }
        if (S_ISDIR(st.st_mode)) rc |= remove_tree(child);
        else if (unlink(child) != 0) rc = -1;
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

/* Initialises a git repository inside dir. Returns 0 on success, -1
 * when git is unavailable or the initialisation failed. */
static int git_init(const char *dir) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            dup2(nullfd, 1);
            dup2(nullfd, 2);
        }
        execlp("git", "git", "-C", dir, "init", (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Last path component, used to derive the config file name. */
static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Validates a project name: lower snake_case, matching the module
 * naming rule of the code style guide. */
static int valid_project_name(const char *name) {
    if (!name[0]) return 0;
    for (const char *p = name; *p; p++) {
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_'))
            return 0;
    }
    return 1;
}

/* Seed source: a greeting program with a license header, no module
 * wrapper, and the entry point at the top level. */
static const char *MAIN_PX_TEMPLATE =
    "/*\n"
    " * Copyright (C) 2026 %s authors\n"
    " * Distributed under the MIT License.\n"
    " *\n"
    " * Entry point of the %s application.\n"
    " */\n"
    "\n"
    "/// Prints a greeting and exits.\n"
    "func main(): Void! {\n"
    "    try std::inout::println(\"Hello, World\");\n"
    "}\n";

int cmd_init(const Cli *cli) {
    const char *name = cli->path;
    if (!name) {
        fprintf(stderr, "%spaxsy:%s init requires a project name%s\n",
                c_red, c_reset, c_reset);
        return 2;
    }
    if (!valid_project_name(name)) {
        fprintf(stderr, "%spaxsy:%s '%s%s%s' is not a valid project name; "
                        "use lower snake_case letters, digits and '_'\\n%s",
                c_red, c_reset, c_yellow, name, c_reset, c_reset);
        return 2;
    }
    if (is_dir(name)) {
        fprintf(stderr, "%spaxsy:%s directory '%s%s%s' already exists\\n%s",
                c_red, c_reset, c_yellow, name, c_reset, c_reset);
        return 1;
    }

    char path[PAXSY_PATH_MAX];

    /* The project consists of exactly two files. */
    snprintf(path, sizeof(path), "%s/src", name);
    if (mkdir_p(path) != 0) {
        fprintf(stderr, "paxsy: cannot create '%s': %s\n", path, strerror(errno));
        return 1;
    }

    snprintf(path, sizeof(path), "%s/src/main.px", name);
    char main_px[1600];
    snprintf(main_px, sizeof(main_px), MAIN_PX_TEMPLATE, name, name);
    if (write_text(path, main_px) != 0) {
        fprintf(stderr, "paxsy: cannot write '%s'\n", path);
        return 1;
    }

    char toml_path[PAXSY_PATH_MAX];
    snprintf(toml_path, sizeof(toml_path), "%s.toml", base_name(name));
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", name, toml_path);
    char toml[512];
    snprintf(toml, sizeof(toml),
        "[project]\n"
        "name = \"%s\"\n"
        "version = \"0.1.0\"\n"
        "description = \"\"\n"
        "authors = []\n"
        "\n"
        "[build]\n"
        "target = \"elf\"\n"
        "optimization = 2\n"
        "debug = false\n"
        "\n"
        "[dependencies]\n"
        "\n"
        "[modules]\n", base_name(name));
    if (write_text(full, toml) != 0) {
        fprintf(stderr, "paxsy: cannot write '%s'\n", full);
        return 1;
    }

    /* One minimal confirmation line; the file list is not repeated
     * because the project consists of exactly two files. */
    printf("%sCreated project '%s'%s", c_green, name, c_reset);
    if (git_init(name) == 0)
        printf(" %s(git initialized)%s\n", c_dim, c_reset);
    else
        printf(" %s(git not found)%s\n", c_dim, c_reset);
    return 0;
}

int cmd_clean(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    const char *dirs[] = { "build", "target", "debug" };
    int removed = 0;
    for (int i = 0; i < 3; i++) {
        char path[PAXSY_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
        if (is_dir(path)) {
            if (remove_tree(path) != 0) {
                fprintf(stderr, "paxsy: cannot remove '%s': %s\n",
                        path, strerror(errno));
                return 1;
            }
            if (!cli->quiet)
                printf("%sremoved%s %s\n", c_green, c_reset, path);
            removed++;
        }
    }
    if (!removed && !cli->quiet)
        printf("%snothing to clean%s\n", c_dim, c_reset);
    return 0;
}

/* Directories that are generated and therefore hidden from tree. */
static int tree_hidden(const char *name) {
    return name[0] == '.' ||
           strcmp(name, "build") == 0 || strcmp(name, "target") == 0 ||
           strcmp(name, "debug") == 0;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Prints one directory level: sorted names, box-drawing branches. */
static void tree_walk(const char *path, const char *prefix) {
    DIR *d = opendir(path);
    if (!d) return;
    const char **names = malloc(256 * sizeof(char *));
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < 256) {
        if (tree_hidden(e->d_name)) continue;
        names[count++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)count, sizeof(char *), cmp_names);
    for (int i = 0; i < count; i++) {
        int last = (i == count - 1);
        printf("%s%s%s%s%s%s\n", prefix, c_dim,
               last ? "└── " : "├── ", c_reset, names[i], c_reset);
        char child[PAXSY_PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, names[i]);
        if (is_dir(child)) {
            char next_prefix[PAXSY_PATH_MAX];
            snprintf(next_prefix, sizeof(next_prefix), "%s%s%s%s",
                     prefix, c_dim, last ? "    " : "│   ", c_reset);
            tree_walk(child, next_prefix);
        }
        free((void *)names[i]);
    }
    free(names);
}

int cmd_tree(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    if (!is_dir(root)) {
        fprintf(stderr, "%spaxsy:%s '%s' is not a directory%s\n",
                c_red, c_reset, root, c_reset);
        return 1;
    }
    printf("%s%s%s\n", c_cyan, base_name(root), c_reset);
    tree_walk(root, "");
    return 0;
}
