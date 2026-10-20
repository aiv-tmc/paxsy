#ifndef U__FILE_H
#define U__FILE_H

#include <stddef.h>

char* read_entire_file(const char* path);
const char** split_into_lines(const char* text, size_t* out_count);
void free_lines(const char** lines, size_t count);

#endif
