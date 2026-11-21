/* Representative SPU kernels built from the same helpers the lifted game code calls
 * (ps3kit/runtime/spu/spu_helpers.h), run on the phone.
 *
 * Unlike a one-instruction microbenchmark these keep the mixes the game's SPU jobs run:
 *   skin   : vec4 transform, splat (shufb) + fm/fma/fa, quadword load/store   (EDGE geometry-like)
 *   recip  : dot product + reciprocal estimate + refine (frest/fi/fnms/fma)    (normalise, lighting)
 *   bits   : integer / shuffle / select / compare chain                        (zlib-like, MLAA, index decode)
 *   copy   : lqd/stqd copy through the big-endian LS accessors                  (DMA-ish / list processing)
 *
 *   spu_kernels [--seconds N] [--threads N] [--cores 4,5,6,7] [--sustain S]
 * Default: each kernel for --seconds (default 2) on one core, ns/iteration and Mquad-ops/s.
 * --sustain S: all kernels round-robin on --threads threads for S seconds, printing every 5 s the
 *   throughput, CPU temperature and the cluster frequencies (thermal throttling shows up as a drop).
 */
#define _GNU_SOURCE
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#define NV 4096                         /* vec4 elements per kernel pass (64 KB: fits L1/L2, like an LS) */

typedef struct { uint8_t* ls; } mem;    /* private 256 KB local store per thread */
static inline u128 ldq(const mem* m, uint32_t a) { u128 v; const uint8_t* p = m->ls + (a & 0x3FFF0u); for (int i = 0; i < 4; i++) v._u32[i] = __builtin_bswap32(((const uint32_t*)p)[i]); return v; }
static inline void stq(mem* m, uint32_t a, u128 v) { uint8_t* p = m->ls + (a & 0x3FFF0u); for (int i = 0; i < 4; i++) ((uint32_t*)p)[i] = __builtin_bswap32(v._u32[i]); }
static u128 pat(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{ u128 p; uint8_t* q = (uint8_t*)&p; for (int i = 0; i < 16; i += 4) { q[i] = b0; q[i+1] = b1; q[i+2] = b2; q[i+3] = b3; } return p; }
static u128 fvec(float a, float b, float c, float d)
{ u128 v; float f[4] = { a, b, c, d }; for (int i = 0; i < 4; i++) { uint32_t u; memcpy(&u, &f[i], 4); v._u32[i] = u; } return v; }

/* ---- kernels: one call = NV iterations, returns a checksum so nothing is dropped ---- */
static uint32_t k_skin(mem* m)
{
    /* host-order SPU words: shufb byte indices address the 16 bytes of a u128 */
    const u128 sx = pat(0,1,2,3), sy = pat(4,5,6,7), sz = pat(8,9,10,11);
    const u128 m0 = fvec(1, .1f, .2f, 0), m1 = fvec(.1f, 1, .3f, 0), m2 = fvec(.2f, .3f, 1, 0), m3 = fvec(5, 6, 7, 1);
    u128 acc = fvec(0, 0, 0, 0);
    for (int i = 0; i < NV; i++) {
        u128 v = ldq(m, i * 16u);
        u128 x = spu_shufb(v, v, sx), y = spu_shufb(v, v, sy), z = spu_shufb(v, v, sz);
        u128 r = spu_fm(m0, x);
        r = spu_fma(m1, y, r); r = spu_fma(m2, z, r); r = spu_fa(r, m3);
        stq(m, (NV + i) * 16u, r);
        acc = spu_fa(acc, r);
    }
    return acc._u32[0] ^ acc._u32[1] ^ acc._u32[2] ^ acc._u32[3];
}
static uint32_t k_recip(mem* m)
{
    const u128 one = fvec(1, 1, 1, 1), sw = pat(4,5,6,7);
    u128 acc = fvec(0, 0, 0, 0);
    for (int i = 0; i < NV; i++) {
        u128 v = ldq(m, i * 16u);
        u128 sq = spu_fm(v, v);
        u128 s2 = spu_fa(sq, spu_shufb(sq, sq, sw));                 /* partial dot */
        u128 e = spu_frest(s2), f = spu_fi(s2, e);                    /* 1/x estimate + interpolate */
        u128 r = spu_fma(spu_fnms(s2, f, one), f, f);                 /* one Newton step */
        acc = spu_fa(acc, spu_fm(v, r));
    }
    return acc._u32[0] ^ acc._u32[3];
}
static uint32_t k_bits(mem* m)
{
    u128 acc = spu_splat_u32(0x1234567u);
    const u128 mask = spu_splat_u32(0x00FF00FFu), sh = pat(1,2,3,0);
    for (int i = 0; i < NV; i++) {
        u128 v = ldq(m, i * 16u);
        u128 t = spu_and(v, mask);
        t = spu_rotqby(t, spu_splat_u32(i & 15));
        u128 c = spu_cgt(t, acc);
        acc = spu_selb(spu_a(acc, t), spu_shufb(acc, v, sh), c);
        acc = spu_ai(acc, 3);
        acc = spu_or(acc, spu_ceq(v, t));
    }
    return acc._u32[0] ^ acc._u32[1] ^ acc._u32[2] ^ acc._u32[3];
}
static uint32_t k_copy(mem* m)
{
    uint32_t s = 0;
    for (int i = 0; i < NV; i++) { u128 v = ldq(m, i * 16u); stq(m, (NV + i) * 16u, v); s += v._u32[0]; }
    return s;
}
typedef uint32_t (*kfn)(mem*);
static const struct { const char* name; kfn fn; } K[] = { {"skin", k_skin}, {"recip", k_recip}, {"bits", k_bits}, {"copy", k_copy} };
enum { NK = 4 };

#ifdef KERNELS_FPMODE
#define FP_ENTER() spu_a64_fp_enter()   /* per thread: FPCR is thread state */
#else
#define FP_ENTER() 0
#endif
static mem mem_new(void)
{
    mem m; if (posix_memalign((void**)&m.ls, 4096, 256 * 1024)) abort();
    uint32_t* w = (uint32_t*)m.ls; uint32_t s = 1;
    for (int i = 0; i < 256 * 1024 / 4; i++) { s = s * 1664525u + 1013904223u; float f = (float)(s >> 9) / (float)(1 << 23); uint32_t u; f += 0.5f; memcpy(&u, &f, 4); w[i] = __builtin_bswap32(u); }
    return m;
}
static void pin(const int* cores, int n, int idx)
{
    if (!n) return; cpu_set_t set; CPU_ZERO(&set); CPU_SET(cores[idx % n], &set); sched_setaffinity(0, sizeof set, &set);
}
static int parse_cores(const char* s, int* out)
{ int n = 0; while (s && *s) { out[n++] = atoi(s); s = strchr(s, ','); if (s) s++; } return n; }

/* ---- sampling of the thermal / frequency state ---- */
static long rd(const char* path) { FILE* f = fopen(path, "r"); if (!f) return -1; long v = -1; if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); return v; }
static double cpu_temp(void)
{
    double best = -1; char p[128], t[64];
    for (int z = 0; z < 80; z++) {
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", z);
        FILE* f = fopen(p, "r"); if (!f) continue; if (!fgets(t, sizeof t, f)) t[0] = 0; fclose(f);
        if (strncmp(t, "cpu", 3)) continue;
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        long v = rd(p); if (v < 0) continue; double c = v > 1000 ? v / 1000.0 : v; if (c > best) best = c;
    }
    return best;
}
static long khz(int cpu) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu); return rd(p); }

typedef struct { int id; const int* cores; int nc; volatile int stop; volatile unsigned long long iters; } worker;
static void* worker_main(void* a)
{
    worker* w = a; pin(w->cores, w->nc, w->id); (void)FP_ENTER(); mem m = mem_new(); volatile uint32_t sink = 0;
    while (!w->stop) { for (int k = 0; k < NK; k++) sink += K[k].fn(&m); w->iters += NK; }
    (void)sink; return NULL;
}

int main(int argc, char** argv)
{
    double secs = 2, sustain = 0; int threads = 1, cores[16], nc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sustain") && i + 1 < argc) sustain = atof(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cores") && i + 1 < argc) nc = parse_cores(argv[++i], cores);
    }
    if (sustain > 0) {
        worker w[16]; pthread_t th[16]; if (threads > 16) threads = 16;
        for (int i = 0; i < threads; i++) { w[i] = (worker){ i, cores, nc, 0, 0 }; pthread_create(&th[i], NULL, worker_main, &w[i]); }
        printf("sustained: %d threads, %.0f s. columns: t(s)  Mpass/s  temp(C)  cpu0 MHz  cpu4 MHz  cpu7 MHz\n", threads, sustain);
        unsigned long long last = 0; double t0 = now(), tl = t0;
        while (now() - t0 < sustain) {
            sleep(5); double t = now(); unsigned long long tot = 0; for (int i = 0; i < threads; i++) tot += w[i].iters;
            printf("%5.0f  %8.3f  %6.1f  %6ld  %6ld  %6ld\n", t - t0, (tot - last) / (t - tl) / 1e6 * NV, cpu_temp(), khz(0) / 1000, khz(4) / 1000, khz(7) / 1000);
            fflush(stdout); last = tot; tl = t;
        }
        for (int i = 0; i < threads; i++) w[i].stop = 1;
        for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
        return 0;
    }
    pin(cores, nc, 0); (void)FP_ENTER(); mem m = mem_new();
    printf("%-8s %12s %14s\n", "kernel", "ns/iter", "Miter/s");
    for (int k = 0; k < NK; k++) {
        volatile uint32_t sink = 0; K[k].fn(&m);
        double best = 1e9; double t0 = now(); long runs = 0;
        while (now() - t0 < secs) { double a = now(); sink += K[k].fn(&m); double d = now() - a; if (d < best) best = d; runs++; }
        printf("%-8s %12.2f %14.2f   (best of %ld passes)\n", K[k].name, best / NV * 1e9, NV / best / 1e6, runs);
    }
    return 0;
}
