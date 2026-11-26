#include "common.h"
#include "nsel.h"

/*
 * naddr.c - the address computation of an indexed access. The base
 * evaluates into rax and the index into rcx; the address lands in
 * rax as base + index * 8, because every array element is a
 * full-width word in this ABI. The lea uses the SIB addressing form
 * with scale 8, which keeps the computation in one instruction and
 * leaves both operand registers intact for the caller.
 */

void naddr_index(NSel *s, const HNode *n) {
    NXBuf *b = s->text;
    nexpr(s, n->child[0]);
    nx_push(b, NRAX);
    nexpr(s, n->child[1]);
    nx_mov_rr(b, NRCX, NRAX);
    nx_pop(b, NRAX);
    /* The element stride is the native cell width: a plain add for
     * single-byte cells, the single-instruction SIB lea with the
     * matching scale for wider words. */
    /* Pointer indexing scales by the pointee's natural size (a byte
     * pointer walks bytes); inline array storage always uses 8-byte
     * cells, so an array index always scales by 8. */
    const HType *bt = n->child[0]->type;
    const HType *elem = NULL;
    if (bt && bt->kind == HTY_PTR) elem = bt->u.ptr.elem;
    else if (bt && bt->kind == HTY_ARRAY) elem = bt->u.array.elem;
    else if (bt && bt->kind == HTY_VIEW) elem = n->type;
    long cell = elem ? ncell_bytes(elem) : 8;
    /* A view value is the handle of a fat pair; the element address
     * walks the data pointer stored in its first half. */
    if (bt && bt->kind == HTY_VIEW)
        nx_mov_rm(b, NRAX, NRAX, 0);
    if (cell == 1) {
        nx_add_rr(b, NRAX, NRCX);
    } else {
        int scale = cell == 2 ? 1 : cell == 4 ? 2 : 3;
        nx_b(b, 0x48);                        /* REX.W             */
        nx_b(b, 0x8D);                        /* lea               */
        nx_b(b, 0x04);                        /* ModRM: [SIB]      */
        nx_b(b, (uint8_t)((scale << 6) | 0x08));  /* scale, rcx,
                                                     base rax        */
    }
}
