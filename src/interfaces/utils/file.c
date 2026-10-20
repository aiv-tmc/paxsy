#include "file.h"
#include "memory.h"
#include "../errhandler/errhandler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* read_entire_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        errhandler__report_error(0, 0, "file", "Cannot open file: %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        errhandler__report_error(0, 0, "file", "Cannot seek in file: %s", path);
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)memory_allocate_zero((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read_bytes != (size_t)len) {
        memory_free_safe((void**)&buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

const char** split_into_lines(const char* text, size_t* out_count) {
    *out_count = 0;
    if (!text || !*text) return NULL;
    size_t line_cnt = 0;
    const char* p = text;
    while (*p) { if (*p == '\n') ++line_cnt; ++p; }
    if (line_cnt == 0) line_cnt = 1;
    else if (*(p-1) != '\n') ++line_cnt;
    const char** lines = (const char**)memory_allocate_zero(line_cnt * sizeof(char*));
    if (!lines) return NULL;
    size_t idx = 0;
    const char* start = text;
    p = text;
    while (*p) {
        if (*p == '\n') {
            size_t len = p - start;
            char* copy = (char*)memory_allocate_zero(len + 1);
            if (!copy) {
                for (size_t i = 0; i < idx; ++i) memory_free_safe((void**)&lines[i]);
                memory_free_safe((void**)&lines);
                return NULL;
            }
            memcpy(copy, start, len);
            copy[len] = '\0';
            lines[idx++] = copy;
            start = p + 1;
        }
        ++p;
    }
    if (start < p) {
        size_t len = p - start;
        char* copy = (char*)memory_allocate_zero(len + 1);
        if (!copy) {
            for (size_t i = 0; i < idx; ++i) memory_free_safe((void**)&lines[i]);
            memory_free_safe((void**)&lines);
            return NULL;
        }
        memcpy(copy, start, len);
        copy[len] = '\0';
        lines[idx++] = copy;
    }
    *out_count = line_cnt;
    return lines;
}

void free_lines(const char** lines, size_t count) {
    if (!lines) return;
    for (size_t i = 0; i < count; ++i) memory_free_safe((void**)&lines[i]);
    memory_free_safe((void**)&lines);
}
