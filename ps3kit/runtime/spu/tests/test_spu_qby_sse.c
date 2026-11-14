/* The SSE4.1 quadword byte rotates/shifts and shufb must match their _ref
 * byte loops for every shift count and selector. Build on x86-64:
 *   clang-cl -O2 -msse4.1 -I.. -I../.. -I../../.. test_spu_qby_sse.c
 * and run with no arguments. */
#include "../spu_helpers.h"
#include <stdio.h>
#include <string.h>

static uint64_t s_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}
static u128 rnd128(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = rnd32(); return r; }

static long s_bad;
static void check(const char* op, u128 got, u128 ref, uint32_t arg)
{
    if (memcmp(&got, &ref, 16) == 0) return;
    if (s_bad++ < 10)
        printf("%s(arg 0x%08X): got %08X %08X %08X %08X want %08X %08X %08X %08X\n", op, arg,
               got._u32[0], got._u32[1], got._u32[2], got._u32[3], ref._u32[0], ref._u32[1], ref._u32[2], ref._u32[3]);
}

int main(void)
{
#if !defined(__SSE4_1__)
    puts("test_spu_qby_sse: no SSE4.1, nothing to compare");
    return 0;
#else
    long n = 0;
    for (int it = 0; it < 20000; it++) {
        const u128 a = rnd128();
        /* every shift count in the low bits, random upper bits (ignored or not) */
        for (uint32_t k = 0; k < 64; k++) {
            u128 b = rnd128();
            b._u32[0] = (it & 1) ? k : (rnd32() & ~0x3FFu) | k;
            u128 b8 = b; b8._u32[0] = (b._u32[0] & ~0x1FFu) | (k << 3) | (rnd32() & 7);
            check("shlqby",    spu_shlqby(a, b),    spu_shlqby_ref(a, b),    b._u32[0]);
            check("rotqby",    spu_rotqby(a, b),    spu_rotqby_ref(a, b),    b._u32[0]);
            check("rotqmby",   spu_rotqmby(a, b),   spu_rotqmby_ref(a, b),   b._u32[0]);
            check("shlqbybi",  spu_shlqbybi(a, b8), spu_shlqbybi_ref(a, b8), b8._u32[0]);
            check("rotqbybi",  spu_rotqbybi(a, b8), spu_rotqbybi_ref(a, b8), b8._u32[0]);
            check("rotqmbybi", spu_rotqmbybi(a, b8), spu_rotqmbybi_ref(a, b8), b8._u32[0]);
            check("rotqmbyi",  spu_rotqmbyi(a, (int)k - 32), spu_rotqmbyi_ref(a, (int)k - 32), k);
            check("rotqmbyi",  spu_rotqmbyi(a, (int)k),      spu_rotqmbyi_ref(a, (int)k), k);
            n += 8;
        }
        /* shufb: random selectors, ordinary ones (< 0x80) and all-special mixes */
        for (int j = 0; j < 8; j++) {
            const u128 b = rnd128();
            u128 c = rnd128();
            if (j < 4) for (int q = 0; q < 4; q++) c._u32[q] &= 0x1F1F1F1Fu;
            check("shufb", spu_shufb(a, b, c), spu_shufb_ref(a, b, c), c._u32[0]);
            check("cntb", spu_cntb(c), spu_cntb_ref(c), c._u32[0]);
            n += 2;
        }
    }
    printf("test_spu_qby_sse: %s (%ld cases)\n", s_bad ? "FAILED" : "ok", n);
    return s_bad ? 1 : 0;
#endif
}
