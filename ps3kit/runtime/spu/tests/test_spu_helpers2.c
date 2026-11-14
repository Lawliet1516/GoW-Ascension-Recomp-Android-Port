/* SPU_HELPERS2 check: every helper spu_helpers.h replaces under SPU_HELPERS2
 * against the form it replaces, bit for bit, on edge cases and random inputs,
 * in the SPU float mode the lifted code runs in (MXCSR 0xE040: round toward
 * zero, FTZ, DAZ).
 *
 * One file, compiled three times (the header can be included once per TU):
 *   cl <SPU flags> -DH2_SIDE=1 -DSPU_HELPERS2 -c test_spu_helpers2.c -Fo h2_on.obj
 *   cl <SPU flags> -DH2_SIDE=0                -c test_spu_helpers2.c -Fo h2_off.obj
 *   cl <SPU flags> -DH2_MAIN test_spu_helpers2.c h2_on.obj h2_off.obj
 * SPU flags: clang-cl /O2 -msse4.1 -mfma /clang:-frounding-math
 *   -DSPU_XFLOAT_APPROX -DSPU_ASSUME_SPU_FP -DSPU_LS_NO_HOOKS -I<ps3kit>/runtime/spu
 *   -I<ps3kit>/include
 * Usage: test_spu_helpers2 [millions of random vectors per op, default 4]
 * Exit code 0 = no difference. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_OPS(X)                                                         \
    X(cbd, I) X(chd, I) X(cwd, I) X(cdd, I)                               \
    X(cbx, B) X(chx, B) X(cwx, B) X(cdx, B)                               \
    X(shlqbi, B) X(rotqbi, B) X(rotqmbi, B)                               \
    X(shlqbii, I) X(rotqbii, I) X(rotqmbii, I)                            \
    X(csflt, I) X(cuflt, I) X(cfltu, I) X(cflts, I)                       \
    X(mpy, B) X(mpyu, B) X(mpyh, B)                                       \
    X(xsbh, U) X(xshw, U) X(gb, U) X(fsm, U) X(fsmh, U) X(fsmb, U)            X(fa, B) X(fs, B) X(fm, B) X(fma, C) X(fms, C) X(fnms, C)

#if defined(H2_SIDE)
#include "spu_helpers.h"
#if H2_SIDE
#  define H2_NAME(op) on_##op
#  if !SPU_H2
#    error "SPU_HELPERS2 did not take effect (needs SSE4.1)"
#  endif
#else
#  define H2_NAME(op) off_##op
#endif
#define H2_U(op) void H2_NAME(op)(const u128* a, const u128* b, int i, u128* r) { (void)b; (void)i; *r = spu_##op(*a); }
#define H2_B(op) void H2_NAME(op)(const u128* a, const u128* b, int i, u128* r) { (void)i; *r = spu_##op(*a, *b); }
#define H2_I(op) void H2_NAME(op)(const u128* a, const u128* b, int i, u128* r) { (void)b; *r = spu_##op(*a, i); }
/* three operands: c is b with its words rotated (another vector of the same mix) */
#define H2_C(op) void H2_NAME(op)(const u128* a, const u128* b, int i, u128* r) { (void)i; u128 c;     c._u32[0] = b->_u32[3] ^ (uint32_t)i; c._u32[1] = b->_u32[0]; c._u32[2] = b->_u32[1]; c._u32[3] = b->_u32[2]; *r = spu_##op(*a, *b, c); }
#define H2_DEF(op, kind) H2_##kind(op)
H2_OPS(H2_DEF)

#elif defined(H2_MAIN)
#include <xmmintrin.h>
typedef struct { _Alignas(16) uint32_t w[4]; } q;
typedef void (*h2_fn)(const q*, const q*, int, q*);
#define H2_DECL(op, kind) void on_##op(const q*, const q*, int, q*); void off_##op(const q*, const q*, int, q*);
H2_OPS(H2_DECL)
typedef struct { const char* name; h2_fn on, off; char kind; } h2_op;
#define H2_ENT(op, kind) { #op, on_##op, off_##op, #kind[0] },
static const h2_op k_ops[] = { H2_OPS(H2_ENT) };

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)(s_rng >> 16); }
/* words that stress every path: all exponents and signs, zeros, ends of ranges */
static uint32_t word(void)
{
    static const uint32_t k[] = { 0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF,
                                  0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 0x00800000, 0x007FFFFF,
                                  0x7F800000, 0x7F7FFFFF, 0xFF800000, 0x3F800000, 0x4F000000, 0x4F7FFFFF,
                                  0x4F800000, 0xCF000000, 0x5F7FFFFF, 0x7FFFFFFE };
    switch (rnd() % 8) {
    case 0: return k[rnd() % (sizeof k / sizeof k[0])];
    case 1: return (rnd() & 0x807FFFFF) | ((rnd() & 0xFF) << 23);           /* any exponent */
    case 2: return rnd() & 0xFF;
    case 3: return (uint32_t)((int32_t)(rnd() & 0xFFFF) - 0x8000);
    case 4: return rnd() >> (rnd() & 31);
    default: return rnd();
    }
}
static void fill(q* v) { for (int i = 0; i < 4; i++) v->w[i] = word(); }

int main(int argc, char** argv)
{
    const long long n = (argc > 1 ? atoll(argv[1]) : 4) * 1000000ll;
    _mm_setcsr(_mm_getcsr() | 0xE040u);          /* the SPU float mode */
    int bad_ops = 0;
    for (size_t o = 0; o < sizeof k_ops / sizeof k_ops[0]; o++) {
        const h2_op* op = &k_ops[o];
        long long diffs = 0, runs = 0;
        q a, b, r1, r2;
        for (long long t = 0; t < n; t++) {
            fill(&a); fill(&b);
            int imm = 0;
            if (op->kind == 'I') {
                /* immediates: i8 0..255 for conversions, i7 -64..63 otherwise;
                 * every value is swept by t for the first rounds */
                const int conv = op->name[0] == 'c' && op->name[1] != 'b' && op->name[1] != 'h' &&
                                 op->name[1] != 'w' && op->name[1] != 'd';
                imm = conv ? (int)(t & 0xFF) : (int)(t & 0x7F) - 64;
                if (t & 0x10000) imm = conv ? (int)(rnd() & 0xFF) : (int)(rnd() & 0x7F) - 64;
            }
            if (op->kind == 'B' && (t & 3) == 0) b.w[0] = (uint32_t)t;   /* shift counts / positions */
            op->on(&a, &b, imm, &r1);
            op->off(&a, &b, imm, &r2);
            runs++;
            if (memcmp(&r1, &r2, sizeof r1)) {
                if (diffs < 5)
                    printf("  %s a=%08X %08X %08X %08X b=%08X i=%d: new %08X %08X %08X %08X old %08X %08X %08X %08X\n",
                           op->name, a.w[0], a.w[1], a.w[2], a.w[3], b.w[0], imm,
                           r1.w[0], r1.w[1], r1.w[2], r1.w[3], r2.w[0], r2.w[1], r2.w[2], r2.w[3]);
                diffs++;
            }
        }
        printf("%-9s %lld vectors, %lld differences\n", op->name, runs, diffs);
        if (diffs) bad_ops++;
    }
    printf("%s\n", bad_ops ? "DIFFERENCES FOUND" : "all identical");
    return bad_ops ? 1 : 0;
}
#endif
