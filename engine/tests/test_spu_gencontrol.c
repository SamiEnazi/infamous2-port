/* engine/tests/test_spu_gencontrol.c -- the x86 generate-controls (spu_cbd/chd/cdd and the x forms,
 * spu_helpers.h "deck-perf") against the byte-built *_pos references, for every address the
 * instructions can form (the position is the address mod 16; a wide sweep of bases, offsets, wraps).
 *   cc -O2 -march=x86-64-v3 -I engine/runtime/spu -I engine/include engine/tests/test_spu_gencontrol.c */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "spu_helpers.h"

static int same(u128 x, u128 y) { return memcmp(&x, &y, 16) == 0; }
static u128 W(uint32_t v, uint32_t j) { u128 r; r._u32[0] = v; r._u32[1] = j * 7u; r._u32[2] = ~v; r._u32[3] = j; return r; }

int main(void)
{
    long bad = 0, n = 0;
    static const uint32_t bases[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 13, 15, 16, 0x3FFF0, 0x3FFFF, 0x7FFFFFF0u, 0xFFFFFFF0u, 0xFFFFFFFFu, 0x12345677u};
    for (unsigned bi = 0; bi < sizeof bases / sizeof bases[0]; bi++)
        for (uint32_t k = 0; k < 4096; k++) {
            const uint32_t a = bases[bi] + k;
            for (int i7 = -64; i7 < 64; i7++) {
                const u128 A = W(a, k);
                n++;
                if (!same(spu_cbd(A, i7), spu_cbd_pos((int)a + i7))) bad++;
                if (!same(spu_chd(A, i7), spu_chd_pos((int)a + i7))) bad++;
                if (!same(spu_cwd(A, i7), spu_cwd_pos((int)a + i7))) bad++;
                if (!same(spu_cdd(A, i7), spu_cdd_pos((int)a + i7))) bad++;
                const u128 B = W((uint32_t)i7 * 0x01010101u, (uint32_t)i7);
                if (!same(spu_cbx(A, B), spu_cbd_pos((int)(a + B._u32[0])))) bad++;
                if (!same(spu_chx(A, B), spu_chd_pos((int)(a + B._u32[0])))) bad++;
                if (!same(spu_cwx(A, B), spu_cwd_pos((int)(a + B._u32[0])))) bad++;
                if (!same(spu_cdx(A, B), spu_cdd_pos((int)(a + B._u32[0])))) bad++;
                if (bad && bad < 5) { fprintf(stderr, "mismatch at a=%08X i7=%d\n", a, i7); }
            }
        }
    printf("generate-controls: %ld cases x 8 forms, %ld mismatches\n", n, bad);
    return bad != 0;
}
