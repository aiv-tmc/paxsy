#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "recovery_metadata.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define META_MAGIC "PAXSRC1\0"
#define META_MAGIC_SIZE 8u
#define META_MAX_SIZE (128u * 1024u * 1024u)
#define META_MAX_FILES 4096u
#define META_MAX_PATH 4096u

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const unsigned char *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void wr16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}
static void wr32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void wr64(unsigned char *p, uint64_t v) {
    wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));
}

static int add_size(size_t *value, size_t add) {
    if (add > SIZE_MAX - *value) return -1;
    *value += add;
    return 0;
}

static int safe_relative_source(const char *root, const char *path,
                                char *relative, size_t cap) {
    char prefix[PAXSY_PATH_MAX];
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    if (n + 6 >= sizeof(prefix)) return 0;
    memcpy(prefix, root, n);
    prefix[n] = '/';
    memcpy(prefix + n + 1, "src/", 5);
    if (strncmp(path, prefix, n + 5) != 0) return 0;
    const char *p = path + n + 1;
    size_t len = strlen(p);
    if (len == 0 || len >= cap || len > META_MAX_PATH) return 0;
    if (p[0] == '/' || strchr(p, '\\')) return 0;
    const char *seg = p;
    for (const char *q = p; ; q++) {
        if (*q == '/' || *q == '\0') {
            size_t sl = (size_t)(q - seg);
            if (sl == 0 || (sl == 1 && seg[0] == '.') ||
                (sl == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
            if (!*q) break;
            seg = q + 1;
        }
    }
    memcpy(relative, p, len + 1);
    return 1;
}

static int read_file(const char *path, unsigned char **data, size_t *size) {
    FILE *f = fopen(path, "rb");
    long end;
    unsigned char *buf;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0 || (unsigned long)end > META_MAX_SIZE) {
        fclose(f); return -1;
    }
    buf = malloc((size_t)end ? (size_t)end : 1);
    if (!buf) { fclose(f); return -1; }
    if (end && fread(buf, 1, (size_t)end, f) != (size_t)end) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    *data = buf;
    *size = (size_t)end;
    return 0;
}

static int write_all(int fd, const unsigned char *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

int paxsy_embed_sources(const char *binary, const char *root,
                        const SrcList *sources) {
    unsigned char *payload = NULL, *image = NULL, *result = NULL;
    size_t payload_size = META_MAGIC_SIZE + 4, payload_cap = 0;
    size_t image_size = 0, result_size = 0, count = 0;
    struct stat st;
    char tmp[PAXSY_PATH_MAX];
    int fd = -1, rc = -1;
    uint64_t shoff, shstr_off, shstr_size, payload_off, new_shstr_off;
    uint64_t new_shoff, new_size;
    uint16_t shentsize, shnum, shstrndx;
    unsigned char *shstr = NULL, *new_shstr = NULL;
    uint32_t new_name_off;

    if (!binary || !root || !sources || stat(binary, &st) != 0 ||
        !S_ISREG(st.st_mode)) return -1;
    payload_cap = payload_size;
    payload = malloc(payload_cap);
    if (!payload) return -1;
    memcpy(payload, META_MAGIC, META_MAGIC_SIZE);
    wr32(payload + META_MAGIC_SIZE, 0);

    for (int i = 0; i < sources->count; i++) {
        char rel[META_MAX_PATH + 1];
        unsigned char *data = NULL;
        size_t data_size = 0, rel_len;
        if (!safe_relative_source(root, sources->paths[i], rel, sizeof(rel)))
            continue;
        if (read_file(sources->paths[i], &data, &data_size) != 0) goto done;
        rel_len = strlen(rel);
        if (payload_size > META_MAX_SIZE ||
            add_size(&payload_size, 12) != 0 ||
            add_size(&payload_size, rel_len) != 0 ||
            add_size(&payload_size, data_size) != 0 ||
            payload_size > META_MAX_SIZE) {
            free(data); goto done;
        }
        if (payload_size > payload_cap) {
            size_t nc = payload_cap;
            while (nc < payload_size) {
                if (nc > META_MAX_SIZE / 2) { nc = META_MAX_SIZE; break; }
                nc *= 2;
            }
            unsigned char *next = realloc(payload, nc);
            if (!next) { free(data); goto done; }
            payload = next; payload_cap = nc;
        }
        {
            size_t record = payload_size - rel_len - data_size - 12;
            wr32(payload + record, (uint32_t)rel_len);
            wr64(payload + record + 4, (uint64_t)data_size);
            memcpy(payload + record + 12, rel, rel_len);
            if (data_size) memcpy(payload + record + 12 + rel_len, data, data_size);
        }
        free(data);
        count++;
    }
    if (count == 0 || count > META_MAX_FILES) goto done;
    wr32(payload + META_MAGIC_SIZE, (uint32_t)count);

    if (read_file(binary, &image, &image_size) != 0 || image_size < 64 ||
        memcmp(image, "\x7f" "ELF", 4) != 0 || image[4] != 2 || image[5] != 1)
        goto done;
    shoff = rd64(image + 40);
    shentsize = rd16(image + 58);
    shnum = rd16(image + 60);
    shstrndx = rd16(image + 62);
    if (shentsize != 64 || shnum == 0 || shnum >= 0xff00 ||
        shstrndx >= shnum || shoff > image_size ||
        (uint64_t)shnum * shentsize > image_size - shoff) goto done;
    {
        const unsigned char *ss = image + shoff + (uint64_t)shstrndx * 64;
        shstr_off = rd64(ss + 24);
        shstr_size = rd64(ss + 32);
    }
    if (shstr_off > image_size || shstr_size > image_size - shstr_off ||
        shstr_size > SIZE_MAX - 12) goto done;
    shstr = malloc((size_t)shstr_size);
    if (!shstr) goto done;
    memcpy(shstr, image + shstr_off, (size_t)shstr_size);
    for (uint16_t i = 1; i < shnum; i++) {
        const unsigned char *sec = image + shoff + (uint64_t)i * 64;
        uint32_t nameoff = rd32(sec);
        if (nameoff < shstr_size &&
            memchr(shstr + nameoff, 0, (size_t)(shstr_size - nameoff)) &&
            strcmp((const char *)shstr + nameoff, ".paxsy.src") == 0)
            goto done;
    }

    new_shstr = malloc((size_t)shstr_size + sizeof(".paxsy.src"));
    if (!new_shstr) goto done;
    memcpy(new_shstr, shstr, (size_t)shstr_size);
    new_name_off = (uint32_t)shstr_size;
    memcpy(new_shstr + shstr_size, ".paxsy.src", sizeof(".paxsy.src"));

    payload_off = (image_size + 7u) & ~7u;
    new_shstr_off = (payload_off + payload_size + 7u) & ~7u;
    new_shoff = (new_shstr_off + shstr_size + sizeof(".paxsy.src") + 7u) & ~7u;
    new_size = new_shoff + (uint64_t)(shnum + 1u) * 64u;
    if (new_size > SIZE_MAX || new_size > (uint64_t)UINT32_MAX * 4u) goto done;
    result_size = (size_t)new_size;
    result = calloc(result_size, 1);
    if (!result) goto done;
    memcpy(result, image, image_size);
    memcpy(result + payload_off, payload, payload_size);
    memcpy(result + new_shstr_off, new_shstr,
           (size_t)shstr_size + sizeof(".paxsy.src"));
    memcpy(result + new_shoff, image + shoff, (size_t)shnum * 64u);
    {
        unsigned char *old_names = result + new_shoff + (uint64_t)shstrndx * 64;
        wr64(old_names + 24, new_shstr_off);
        wr64(old_names + 32, shstr_size + sizeof(".paxsy.src"));
    }
    {
        unsigned char *sec = result + new_shoff + (uint64_t)shnum * 64;
        wr32(sec, new_name_off);
        wr32(sec + 4, 1);              /* SHT_PROGBITS */
        wr64(sec + 8, 0);              /* not mapped */
        wr64(sec + 16, 0);
        wr64(sec + 24, payload_off);
        wr64(sec + 32, payload_size);
        wr32(sec + 40, 0);
        wr32(sec + 44, 0);
        wr64(sec + 48, 1);
        wr64(sec + 56, 0);
    }
    wr64(result + 40, new_shoff);
    wr16(result + 60, (uint16_t)(shnum + 1));

    if (snprintf(tmp, sizeof(tmp), "%s.paxsy-tmp-XXXXXX", binary) >=
        (int)sizeof(tmp)) goto done;
    fd = mkstemp(tmp);
    if (fd < 0) goto done;
    {
        int failed = fchmod(fd, st.st_mode & 07777) != 0;
        if (!failed && write_all(fd, result, result_size) != 0) failed = 1;
        if (!failed && fsync(fd) != 0) failed = 1;
        if (close(fd) != 0) failed = 1;
        fd = -1;
        if (failed) { unlink(tmp); goto done; }
    }
    if (rename(tmp, binary) != 0) { unlink(tmp); goto done; }
    rc = 0;
done:
    if (fd >= 0) { close(fd); unlink(tmp); }
    free(payload); free(image); free(result); free(shstr); free(new_shstr);
    return rc;
}

static int valid_source_path(const unsigned char *p, size_t n) {
    size_t start = 0;
    if (n < 5 || n > META_MAX_PATH || memcmp(p, "src/", 4) != 0 ||
        memchr(p, 0, n) || memchr(p, '\\', n)) return 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || p[i] == '/') {
            size_t len = i - start;
            if (!len || (len == 1 && p[start] == '.') ||
                (len == 2 && p[start] == '.' && p[start + 1] == '.')) return 0;
            start = i + 1;
        }
    }
    return 1;
}

int paxsy_extract_sources(const char *binary, PaxsySourceArchive *archive) {
    unsigned char *image = NULL;
    size_t image_size = 0;
    uint64_t shoff, names_off, names_size, meta_off = 0, meta_size = 0;
    uint16_t shentsize, shnum, names_index;
    int rc = 1; /* no embedded Paxsy source */
    memset(archive, 0, sizeof(*archive));
    if (read_file(binary, &image, &image_size) != 0 || image_size < 64 ||
        memcmp(image, "\x7f" "ELF", 4) != 0 || image[4] != 2 || image[5] != 1)
        goto done;
    shoff = rd64(image + 40);
    shentsize = rd16(image + 58);
    shnum = rd16(image + 60);
    names_index = rd16(image + 62);
    if (shentsize != 64 || !shnum || names_index >= shnum ||
        shoff > image_size || (uint64_t)shnum * 64 > image_size - shoff)
        goto done;
    {
        const unsigned char *s = image + shoff + (uint64_t)names_index * 64;
        names_off = rd64(s + 24); names_size = rd64(s + 32);
    }
    if (names_off > image_size || names_size > image_size - names_off) goto done;
    for (uint16_t i = 1; i < shnum; i++) {
        const unsigned char *s = image + shoff + (uint64_t)i * 64;
        uint32_t no = rd32(s);
        if (no >= names_size ||
            !memchr(image + names_off + no, 0, (size_t)(names_size - no)))
            continue;
        if (strcmp((const char *)image + names_off + no, ".paxsy.src") == 0) {
            meta_off = rd64(s + 24); meta_size = rd64(s + 32); break;
        }
    }
    if (!meta_size) goto done;
    rc = -1; /* section exists but must pass complete validation */
    if (meta_off > image_size || meta_size > image_size - meta_off ||
        meta_size < META_MAGIC_SIZE + 4 || meta_size > META_MAX_SIZE ||
        memcmp(image + meta_off, META_MAGIC, META_MAGIC_SIZE) != 0) goto done;
    {
        const unsigned char *p = image + meta_off;
        size_t pos = META_MAGIC_SIZE + 4;
        uint32_t count = rd32(p + META_MAGIC_SIZE);
        if (!count || count > META_MAX_FILES) goto done;
        archive->files = calloc(count, sizeof(*archive->files));
        if (!archive->files) goto done;
        archive->count = count;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t path_len;
            uint64_t data_len;
            if (pos > meta_size || meta_size - pos < 12) goto done;
            path_len = rd32(p + pos); data_len = rd64(p + pos + 4); pos += 12;
            if (path_len > meta_size - pos || data_len > SIZE_MAX ||
                data_len > meta_size - pos - path_len ||
                !valid_source_path(p + pos, path_len)) goto done;
            archive->files[i].path = malloc((size_t)path_len + 1);
            archive->files[i].data = malloc(data_len ? (size_t)data_len : 1);
            if (!archive->files[i].path || !archive->files[i].data) goto done;
            memcpy(archive->files[i].path, p + pos, path_len);
            archive->files[i].path[path_len] = 0;
            pos += path_len;
            if (data_len) memcpy(archive->files[i].data, p + pos, (size_t)data_len);
            archive->files[i].size = (size_t)data_len;
            pos += (size_t)data_len;
        }
        if (pos != meta_size) goto done;
    }
    rc = 0;
done:
    free(image);
    if (rc != 0) paxsy_source_archive_free(archive);
    return rc;
}

void paxsy_source_archive_free(PaxsySourceArchive *archive) {
    if (!archive) return;
    for (size_t i = 0; i < archive->count; i++) {
        free(archive->files[i].path);
        free(archive->files[i].data);
    }
    free(archive->files);
    archive->files = NULL;
    archive->count = 0;
}
