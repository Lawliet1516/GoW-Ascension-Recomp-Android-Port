/* Dumps the exact references (spu_*_ref) on structured + random inputs as binary records, for the independent
 * Python model in spu_float_model.py to check. Record = 9 x u32 little endian: a b c fa fs fm fma fms fnms (lane 0).
 * Runs on any host (the _ref functions are integer code).   spu_float_dump <out.bin> [random_count] */
/* Two uses: (default) dump the exact references for the Python model; (-DDIFF_FAST on aarch64, with -DSPU_A64_FASTFP)
 * run the same inputs through the FPCR fast path, four different records per vector so lanes with different
 * exponent classes meet in one op, and compare with the exact reference lane by lane. */
#ifndef DIFF_FAST
#define SPU_SCALAR_HELPERS
#endif
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t r32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static FILE* out; static unsigned long long total;
#ifdef DIFF_FAST
static unsigned long long diffs; static uint32_t pa[4], pb[4], pc[4]; static int np;
static void check4(void)
{
    u128 A, B, C; for (int i = 0; i < 4; i++) { A._u32[i] = pa[i]; B._u32[i] = pb[i]; C._u32[i] = pc[i]; }
    u128 f[6] = { spu_fa(A, B), spu_fs(A, B), spu_fm(A, B), spu_fma(A, B, C), spu_fms(A, B, C), spu_fnms(A, B, C) };
    u128 r[6] = { spu_fa_ref(A, B), spu_fs_ref(A, B), spu_fm_ref(A, B), spu_fma_ref(A, B, C), spu_fms_ref(A, B, C), spu_fnms_ref(A, B, C) };
    static const char* nm[6] = { "fa", "fs", "fm", "fma", "fms", "fnms" };
    for (int k = 0; k < 6; k++) for (int i = 0; i < 4; i++) if (f[k]._u32[i] != r[k]._u32[i]) {
        if (diffs++ < 20) printf("DIFF %s lane%d a=%08x b=%08x c=%08x fast=%08x ref=%08x\n", nm[k], i, pa[i], pb[i], pc[i], f[k]._u32[i], r[k]._u32[i]); }
}
static void rec(uint32_t a, uint32_t b, uint32_t c)
{
    pa[np] = a; pb[np] = b; pc[np] = c; total++;
    if (++np == 4) { check4(); np = 0; }
}
#else
static void rec(uint32_t a, uint32_t b, uint32_t c)
{
    u128 A, B, C; for (int i = 0; i < 4; i++) { A._u32[i] = a; B._u32[i] = b; C._u32[i] = c; }
    uint32_t r[9] = { a, b, c, spu_fa_ref(A, B)._u32[0], spu_fs_ref(A, B)._u32[0], spu_fm_ref(A, B)._u32[0],
                      spu_fma_ref(A, B, C)._u32[0], spu_fms_ref(A, B, C)._u32[0], spu_fnms_ref(A, B, C)._u32[0] };
    fwrite(r, 4, 9, out); total++;
}
#endif
int main(int argc, char** argv)
{
#ifdef DIFF_FAST
    const uint64_t saved = spu_a64_fp_enter();
    (void)argv;
    out = NULL; long nrand = argc > 2 ? atol(argv[2]) : 300000;
#else
    out = fopen(argv[1], "wb"); long nrand = argc > 2 ? atol(argv[2]) : 300000;
#endif
    /* 1. every biased exponent against every biased exponent, mantissa/sign classes (two operands; c = a class too) */
    const uint32_t man[] = { 0, 1, 2, 0x400000, 0x7FFFFE, 0x7FFFFF, 0x2AAAAA, 0x155555 };
    for (uint32_t ea = 0; ea < 256; ea++) for (uint32_t eb = 0; eb < 256; eb += (eb < 4 || eb > 250 || (eb > 120 && eb < 134)) ? 1 : 7)
        for (int ma = 0; ma < 8; ma++) for (int mb = 0; mb < 8; mb++) for (int s = 0; s < 4; s++)
            rec(((s & 1) << 31) | (ea << 23) | man[ma], ((s >> 1) << 31) | (eb << 23) | man[mb], 0x3F800000u | man[(ma + mb) & 7]);
    /* 2. three operands at the interesting exponents (zero/denormal, bottom, 1.0, fast-path limit 253/254/255) */
    const uint32_t ex[] = { 0, 1, 2, 3, 126, 127, 128, 129, 252, 253, 254, 255 };
    for (int i = 0; i < 12; i++) for (int j = 0; j < 12; j++) for (int k = 0; k < 12; k++)
        for (int ma = 0; ma < 8; ma += 2) for (int mb = 0; mb < 8; mb += 3) for (int mc = 0; mc < 8; mc += 3) for (int s = 0; s < 8; s++)
            rec(((s & 1) << 31) | (ex[i] << 23) | man[ma], (((s >> 1) & 1) << 31) | (ex[j] << 23) | man[mb], ((s >> 2) << 31) | (ex[k] << 23) | man[mc]);
    /* 3. the product bound (ex+ey around 380) and the fma bound (379), full 253 limits */
    for (uint32_t ea = 100; ea <= 255; ea++) for (int d = -4; d <= 4; d++) { int eb = 380 - (int)ea + d; if (eb < 0 || eb > 255) continue;
        for (int ma = 0; ma < 8; ma += 3) rec((ea << 23) | man[ma], ((uint32_t)eb << 23) | man[(ma + 1) & 7], 0x3F800000u); }
    for (uint32_t ea = 100; ea <= 255; ea++) for (int d = -4; d <= 4; d++) { int eb = 379 - (int)ea + d; if (eb < 0 || eb > 255) continue;
        for (int ma = 0; ma < 8; ma += 3) rec((ea << 23) | man[ma], ((uint32_t)eb << 23) | man[(ma + 2) & 7], (253u << 23) | 0x7FFFFFu); }
    /* 4. cancellation / carries / truncation ties: a + (-a +- 1 ulp), 1 - 1ulp, x + x, products landing on exact values */
    for (int i = 0; i < 20000; i++) { uint32_t a = (r32() & 0x80000000u) | ((1 + r32() % 253) << 23) | (r32() & 0x7FFFFF);
        for (int d = -2; d <= 2; d++) rec(a, (a ^ 0x80000000u) + d, a); rec(a, a, a ^ 0x80000000u); }
    /* 5. random bit patterns and random normal-ish values */
    for (long i = 0; i < nrand; i++) { uint32_t a = r32(), b = r32(), c = r32();
        if (i & 1) { a = (a & 0x807FFFFFu) | ((100 + r32() % 56) << 23); b = (b & 0x807FFFFFu) | ((100 + r32() % 56) << 23); c = (c & 0x807FFFFFu) | ((100 + r32() % 56) << 23); }
        rec(a, b, c); }
#ifdef DIFF_FAST
    spu_a64_fp_leave(saved);
    printf("fast path vs exact reference: %llu records x 6 ops (4 mixed lanes per vector): %s (%llu differences)\n", total, diffs ? "FAILED" : "ALL EQUAL", diffs);
    return diffs != 0;
#else
    fclose(out); fprintf(stderr, "wrote %llu records\n", total); return 0;
#endif
}
