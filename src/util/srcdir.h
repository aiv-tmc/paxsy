#ifndef PAXSY_SRCDIR_H
#define PAXSY_SRCDIR_H

/*
 * srcdir.h - deterministic discovery of Paxsy source files.
 * srcdir_collect() walks root/src (recursively when asked), keeps .px
 * files (or only *_test.px files when tests_only is set) and returns
 * them sorted by path so every consumer - build, clippy, bench, the
 * test command and the archive emitters - sees one stable order
 * regardless of the directory iteration order of the file system.
 * The list grows dynamically; there is no fixed source count limit.
 *
 * The caller MUST pass a zero-initialised SrcList (plain `= {0}`):
 * srclist_push grows the array based on count/cap, so storage that
 * carries garbage would make realloc() operate on an invalid
 * pointer. srcdir_collect() additionally normalises an inconsistent
 * list (cap < count) back to empty before walking, which turns an
 * uninitialised list into a clean failure mode instead of a crash.
 */

typedef struct {
    char (*paths)[PAXSY_PATH_MAX];
    int count;
    int cap;
} SrcList;

/* Appends one path, growing the array on demand. Returns -1 when the
 * allocation fails or the path does not fit PAXSY_PATH_MAX. Exported
 * for commands that collect archive members alongside sources. */
int srclist_push(SrcList *list, const char *path);

/* Releases the path array. Safe on zero-initialised lists. */
void srclist_free(SrcList *list);

/* Collects matching sources. Returns the number of files found, or -1
 * when a path could not be stored (out of memory or a path component
 * longer than PAXSY_PATH_MAX). The list is sorted by full path before
 * returning. */
int srcdir_collect(const char *root, int recursive, int tests_only,
                   SrcList *list);

#endif /* PAXSY_SRCDIR_H */
