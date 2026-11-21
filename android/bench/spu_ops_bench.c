/* Per-operation latency and throughput of the SPU helpers, for the three implementations the port can
 * dispatch between. Build it once per variant (android/bench/build.sh):
 *   scalar : -DSPU_SCALAR_HELPERS -DSPU_SCALAR_INT   the pure-C helpers
 *   neon   : (default)                               the NEON helpers; float ops emulate through f64
 *   fastfp : -DSPU_A64_FASTFP                        NEON + FPCR RZ|FZ fast path for fa/fs/fm/fma/fms/fnms
 * Output: one line per op "name lat_ns tput_ns" -- lat = dependent chain, tput = 8 independent chains
 * (ns per op). android/bench/compare_ops.py joins the three runs. */
#define _GNU_SOURCE
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>

#ifdef SPU_A64_COUNT
unsigned long long spu_a64_slow_calls;
#endif
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs; }
/* ordinary-range floats (exponent 110..140) so the float fast paths are the common case, as in game data */
static u128 vf(void) { u128 v; for (int i = 0; i < 4; i++) v._u32[i] = ((rnd() & 1u) << 31) | ((110u + (rnd() >> 8) % 30u) << 23) | ((rnd() >> 9) & 0x7FFFFFu); return v; }
/* a multiplier of exactly 1.0 for the float chains: with any other constant a dependent chain of fm/fma
 * drifts exponentially to 0 or overflow, which would measure the exact slow path instead of the op */
static u128 vone(void) { u128 v; for (int i = 0; i < 4; i++) v._u32[i] = 0x3F800000u; return v; }
/* keep the compiler from folding the chain: materialise X in a vector register it cannot see through */
#define OPQ(X) do { uint32x4_t q_ = vld1q_u32((X)._u32); __asm__ volatile("" : "+w"(q_)); vst1q_u32((X)._u32, q_); } while (0)

#define LATN  3000000L
#define TPN   600000L
#define DEF(name, EXPR) \
static void b_##name(double* lat, double* tp) { \
    u128 B = vf(), BF = vone(), C = vf(), A0 = vf(); (void)BF; double best = 1e9; \
    for (int rep = 0; rep < 5; rep++) { u128 X = A0; double t0 = now(); \
        for (long i = 0; i < LATN; i++) { X = (EXPR); OPQ(X); } double d = now() - t0; if (d < best) best = d; \
        __asm__ volatile("" :: "r"(X._u32[0])); } \
    *lat = best / LATN * 1e9; best = 1e9; \
    for (int rep = 0; rep < 5; rep++) { u128 xs[8]; for (int k = 0; k < 8; k++) xs[k] = vf(); double t0 = now(); \
        for (long i = 0; i < TPN; i++) for (int k = 0; k < 8; k++) { u128 X = xs[k]; X = (EXPR); OPQ(X); xs[k] = X; } \
        double d = now() - t0; if (d < best) best = d; __asm__ volatile("" :: "r"(xs[0]._u32[0])); } \
    *tp = best / (TPN * 8) * 1e9; }

/* integer / logical / shuffle */
DEF(a, spu_a(X, B)) DEF(sf, spu_sf(X, B)) DEF(ai, spu_ai(X, 7)) DEF(and, spu_and(X, B)) DEF(or, spu_or(X, B))
DEF(xor, spu_xor(X, B)) DEF(andc, spu_andc(X, B)) DEF(eqv, spu_eqv(X, B)) DEF(ah, spu_ah(X, B))
DEF(ceq, spu_ceq(X, B)) DEF(ceqb, spu_ceqb(X, B)) DEF(cgt, spu_cgt(X, B)) DEF(clgt, spu_clgt(X, B))
DEF(selb, spu_selb(X, B, C)) DEF(shufb, spu_shufb(X, B, C)) DEF(rotqby, spu_rotqby(X, B)) DEF(rotqbyi, spu_rotqbyi(X, 3))
DEF(rotqbi, spu_rotqbi(X, B)) DEF(rot, spu_rot(X, B)) DEF(roti, spu_roti(X, 5)) DEF(shli, spu_shli(X, 3)) DEF(rotm, spu_rotm(X, B))
DEF(mpy, spu_mpy(X, B)) DEF(mpyu, spu_mpyu(X, B)) DEF(mpyh, spu_mpyh(X, B)) DEF(mpya, spu_mpya(X, B, C))
DEF(clz, spu_clz(X)) DEF(cntb, spu_cntb(X)) DEF(gb, spu_gb(X)) DEF(avgb, spu_avgb(X, B)) DEF(absdb, spu_absdb(X, B)) DEF(orx, spu_orx(X))
/* float: the ops the game's geometry and audio jobs run */
DEF(fa, spu_fa(X, BF)) DEF(fs, spu_fs(X, BF)) DEF(fm, spu_fm(X, BF)) DEF(fma, spu_fma(X, BF, C)) DEF(fms, spu_fms(X, BF, C))
DEF(fnms, spu_fnms(X, BF, C)) DEF(frest, spu_frest(X)) DEF(frsqest, spu_frsqest(X)) DEF(fi, spu_fi(X, BF))
DEF(csflt, spu_csflt(X, 3)) DEF(cflts, spu_cflts(X, 3)) DEF(fceq, spu_fceq(X, BF)) DEF(fcgt, spu_fcgt(X, BF)) DEF(fcmgt, spu_fcmgt(X, BF))

#ifdef SPU_A64_COUNT
static unsigned long long last_slow;
#define SLOWREPORT(n) do { fprintf(stderr, "  %-8s slow-path calls: %llu\n", n, spu_a64_slow_calls - last_slow); last_slow = spu_a64_slow_calls; } while (0)
#else
#define SLOWREPORT(n) ((void)0)
#endif
#define RUN(name) do { double l, t; b_##name(&l, &t); printf("%s %.3f %.3f\n", #name, l, t); SLOWREPORT(#name); fflush(stdout); } while (0)
int main(int argc, char** argv)
{
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(argc > 1 ? atoi(argv[1]) : 7, &set); sched_setaffinity(0, sizeof set, &set);
#ifdef SPU_A64_FASTFP
    spu_a64_fp_enter();
#endif
    RUN(a); RUN(sf); RUN(ai); RUN(and); RUN(or); RUN(xor); RUN(andc); RUN(eqv); RUN(ah); RUN(ceq); RUN(ceqb); RUN(cgt); RUN(clgt);
    RUN(selb); RUN(shufb); RUN(rotqby); RUN(rotqbyi); RUN(rotqbi); RUN(rot); RUN(roti); RUN(shli); RUN(rotm);
    RUN(mpy); RUN(mpyu); RUN(mpyh); RUN(mpya); RUN(clz); RUN(cntb); RUN(gb); RUN(avgb); RUN(absdb); RUN(orx);
    RUN(fa); RUN(fs); RUN(fm); RUN(fma); RUN(fms); RUN(fnms); RUN(frest); RUN(frsqest); RUN(fi);
    RUN(csflt); RUN(cflts); RUN(fceq); RUN(fcgt); RUN(fcmgt);
    return 0;
}
