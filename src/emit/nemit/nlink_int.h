#ifndef PAXSY_NLINK_INT_H
#define PAXSY_NLINK_INT_H

#include "common.h"

#include <stdint.h>

#define NL_MAX_OBJS 64

typedef struct {
    uint8_t *text, *ro, *data;
    size_t text_len, ro_len, data_len;
    uint64_t text_base, ro_base, data_base;
    struct { char *name; uint8_t sect; uint64_t value; } *sym;
    int nsym;
    struct { uint64_t pos; uint8_t sect; uint8_t type; uint64_t addend; } *rel;
    int nrel;
} NLObj;

int nlink_load_input(NLObj *objs, int *count, const char *path, Diag *diag);
void nlink_objs_release(NLObj *objs, int count);

#endif
