/* Verifies the AArch64 FPCR float fast path (-DSPU_A64_FASTFP) against the exact references, on the phone.
 *  1. fa/fs/fm/fma/fms/fnms in the SPU float mode (FPCR RZ|FZ) == spu_*_ref, bit for bit.
 *  2. Every other float helper must give the same bits in the SPU mode as in the standard mode
 *     (they were written for the standard mode and now run with FPCR in RZ|FZ).
 */
#ifndef SPU_A64_FASTFP
#error build with -DSPU_A64_FASTFP
#endif
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t r32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint32_t rf(void)
{
    uint32_t sign = (r32() & 1u) << 31, man = r32() & 0x7FFFFFu, e;
    switch (r32() % 14) {
    case 0: e = 0; break;                              /* zero / denormal */
    case 1: e = 1 + r32() % 4; break;                  /* bottom of the normal range */
    case 2: e = 248 + r32() % 8; break;                /* top, into extended range (253..255) */
    case 3: return r32();
    case 4: return sign;
    case 5: e = 253 + r32() % 3; break;                /* right at the fast path limit */
    case 6: e = 126 + r32() % 4; man &= 0xF; break;    /* near 1.0, tiny mantissa: exact sums and ties */
    case 7: e = 127; man = 0; break;                   /* exactly +-1 */
    case 8: e = 100 + r32() % 56; man &= 0x7FF000u; break; /* few bits: many exact results */
    default: e = 110 + r32() % 36; break;
    }
    return sign | (e << 23) | man;
}
static u128 rv(void) { u128 v; for (int i = 0; i < 4; i++) v._u32[i] = rf(); return v; }
static long bad;
static int eq(u128 a, u128 b) { return !memcmp(&a, &b, 16); }
#define PR(nm, a, b, x, y) do { if (bad++ < 12) printf("MISMATCH %s: a=%08x %08x %08x %08x b=%08x.. got %08x %08x %08x %08x want %08x %08x %08x %08x\n", nm, \
    a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], x._u32[0], x._u32[1], x._u32[2], x._u32[3], y._u32[0], y._u32[1], y._u32[2], y._u32[3]); } while (0)

int main(void)
{
    const long N = 4000000;
    const uint64_t saved = getenv("NOMODE") ? spu__fpcr_get() : spu_a64_fp_enter();   /* NOMODE=1: negative control, must FAIL */
    for (long k = 0; k < N; k++) {
        u128 a = rv(), b = rv(), c = rv(), x, y;
        x = spu_fa(a, b);   y = spu_fa_ref(a, b);   if (!eq(x, y)) PR("fa", a, b, x, y);
        x = spu_fs(a, b);   y = spu_fs_ref(a, b);   if (!eq(x, y)) PR("fs", a, b, x, y);
        x = spu_fm(a, b);   y = spu_fm_ref(a, b);   if (!eq(x, y)) PR("fm", a, b, x, y);
        x = spu_fma(a, b, c);  y = spu_fma_ref(a, b, c);  if (!eq(x, y)) PR("fma", a, b, x, y);
        x = spu_fms(a, b, c);  y = spu_fms_ref(a, b, c);  if (!eq(x, y)) PR("fms", a, b, x, y);
        x = spu_fnms(a, b, c); y = spu_fnms_ref(a, b, c); if (!eq(x, y)) PR("fnms", a, b, x, y);
    }
    printf("part 1 (fa fs fm fma fms fnms vs exact reference, SPU float mode): %s (%ld mismatches over %ld vectors per op)\n",
           bad ? "FAILED" : "ok", bad, N);
    long bad1 = bad;
    /* part 2: SPU mode vs standard mode */
#define INV1(nm, expr) do { for (long k = 0; k < 400000; k++) { u128 a = rv(), b = rv(), t = rv(); (void)b; (void)t; u128 in, out; \
        spu__fpcr_set(saved); in = (expr); spu__fpcr_set(saved | SPU_FPCR_SPU_BITS); out = (expr); if (!eq(in, out)) PR(nm, a, b, out, in); } } while (0)
    INV1("frest", spu_frest(a)); INV1("frsqest", spu_frsqest(a)); INV1("fi", spu_fi(a, b));
    INV1("frds", spu_frds(a)); INV1("fesd", spu_fesd(a));
    INV1("cflts", spu_cflts(a, (int)(k % 100))); INV1("cfltu", spu_cfltu(a, (int)(k % 100)));
    INV1("csflt", spu_csflt(a, (int)(k % 100))); INV1("cuflt", spu_cuflt(a, (int)(k % 100)));
    INV1("dfma", spu_dfma(a, b, t)); INV1("dfms", spu_dfms(a, b, t)); INV1("dfnma", spu_dfnma(a, b, t)); INV1("dfnms", spu_dfnms(a, b, t));
    INV1("fceq", spu_fceq(a, b)); INV1("fcgt", spu_fcgt(a, b)); INV1("fcmeq", spu_fcmeq(a, b)); INV1("fcmgt", spu_fcmgt(a, b));
    printf("part 2 (other float helpers identical in standard and SPU float mode): %s (%ld mismatches)\n", bad - bad1 ? "FAILED" : "ok", bad - bad1);
    spu_a64_fp_leave(saved);
    return bad != 0;
}
