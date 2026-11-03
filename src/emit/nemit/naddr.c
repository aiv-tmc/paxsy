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
    nx_b(b, 0x48);                        /* REX.W                 */
    nx_b(b, 0x8D);                        /* lea                   */
    nx_b(b, 0x04);                        /* ModRM: [SIB], dst rax */
    nx_b(b, (uint8_t)((3 << 6) | 0x08));  /* scale 8, index rcx,
                                             base rax              */
}
