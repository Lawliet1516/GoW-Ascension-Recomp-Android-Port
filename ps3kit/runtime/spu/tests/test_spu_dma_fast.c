/*
 * SPU_DMA_FAST's two data-path helpers in spu_dma.h, against what they
 * replace:
 *   spu_dma_lines_reserved(a0, a1)  vs  spu_coh_is_reserved per 128-byte line
 *   spu_dma_copy(d, s, n)           vs  memcpy
 * over random bitmaps (sparse and dense), every span length a DMA can have
 * (1 byte .. 16 KB, any alignment, bitmap byte and word boundaries, the top of
 * the 4 GB space), armed and not armed.
 *
 * From ps3kit\runtime\spu:
 *   clang-cl /nologo /O2 -msse4.1 -I. -I..\..\include tests\test_spu_dma_fast.c
 */
#include "spu_dma.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* what spu_coherency.c exports, and its reader, as there */
int g_spu_coh_armed;
unsigned char g_spu_coh_bitmap[1u << 22];
uint8_t* vm_base;
__declspec(noinline) int ref_is_reserved(uint32_t addr)
{
    if (!g_spu_coh_armed) return 0;
    uint32_t line = addr >> 7;
    return (g_spu_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}
static int ref_span(uint32_t a0, uint32_t a1)
{
    for (uint32_t a = a0; ; a += 128u) {
        if (ref_is_reserved(a)) return 1;
        if (a == a1) break;
    }
    return 0;
}

static unsigned long long rng = 88172645463325252ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ns(void) { static LARGE_INTEGER f; LARGE_INTEGER c; if (!f.QuadPart) QueryPerformanceFrequency(&f);
                             QueryPerformanceCounter(&c); return (double)c.QuadPart * 1e9 / (double)f.QuadPart; }
/* A PUT's scan + copy + rescan as before (an out-of-line call per line, twice,
 * and memcpy) and as now, armed with nothing reserved near the span. */
static void bench(void)
{
    static uint8_t ls[16384 + 64], mem[16384 + 64];
    memset(g_spu_coh_bitmap, 0, sizeof g_spu_coh_bitmap);
    g_spu_coh_armed = 1;
    const uint32_t sizes[] = { 16, 32, 64, 128, 256, 1024, 4096, 16384 };
    printf("PUT bookkeeping + copy, nothing reserved, ns per PUT (best of 7 x 20000):\n");
    for (int k = 0; k < 8; k++) {
        const uint32_t n = sizes[k], ea = 0x40001000u;
        const uint32_t a0 = ea & ~127u, a1 = (ea + n - 1u) & ~127u;
        double best[2] = { 1e30, 1e30 };
        volatile int sink = 0;
        for (int rep = 0; rep < 7; rep++)
            for (int v = 0; v < 2; v++) {
                const double t0 = now_ns();
                for (int it = 0; it < 20000; it++) {
                    if (!v) {
                        int r = 0;
                        for (uint32_t a = a0; ; a += 128u) { if (ref_is_reserved(a)) { r = 1; break; } if (a == a1) break; }
                        memcpy(mem, ls, n);
                        for (uint32_t a = a0; ; a += 128u) { if (ref_is_reserved(a)) r++; if (a == a1) break; }
                        sink += r;
                    } else {
                        int r = spu_dma_lines_reserved(a0, a1);
                        spu_dma_copy(mem, ls, n);
                        if (spu_dma_lines_reserved(a0, a1)) r++;
                        sink += r;
                    }
                    ls[it & 15]++;
                    __asm__ volatile("" : : "r"(mem), "r"(ls) : "memory");   /* keep the copies */
                }
                const double dt = (now_ns() - t0) / 20000.0;
                if (dt < best[v]) best[v] = dt;
            }
        printf("  %5u bytes: before %7.1f ns  now %7.1f ns  (%.2fx)\n", n, best[0], best[1], best[0] / best[1]);
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "bench")) { bench(); return 0; }
    long long spans = 0, fails = 0;
    for (int round = 0; round < 400; round++) {
        memset(g_spu_coh_bitmap, 0, sizeof g_spu_coh_bitmap);
        g_spu_coh_armed = round % 7 != 0;
        const uint32_t dens = round % 4;   /* 0: none, 1: very sparse, 2: sparse, 3: dense */
        const uint32_t nbits = dens == 0 ? 0 : dens == 1 ? 200 : dens == 2 ? 20000 : 2000000;
        for (uint32_t k = 0; k < nbits; k++) {
            const uint32_t l = (rnd() ^ (rnd() << 16)) & 0x1FFFFFFu;
            g_spu_coh_bitmap[l >> 3] |= (unsigned char)(1u << (l & 7));
        }
        for (int t = 0; t < 20000; t++) {
            uint32_t ea = rnd() ^ (rnd() << 16);
            if (t % 5 == 0) ea = 0xFFFFC000u + (rnd() & 0x3FFFu);             /* top of the space */
            if (t % 5 == 1) ea = (rnd() & 0xFFFFu) << 10;                      /* bitmap byte boundary */
            const uint32_t sizes[] = { 1, 2, 4, 8, 16, 32, 128, 256, 1024, 4096, 16384 };
            uint32_t size = (t & 1) ? sizes[rnd() % 11] : 1 + rnd() % 16384;
            const uint32_t a0 = ea & ~127u, a1 = (ea + size - 1u) & ~127u;
            if (a0 > a1) continue;   /* wrapping spans keep the per-line scan */
            spans++;
            if (spu_dma_lines_reserved(a0, a1) != ref_span(a0, a1)) {
                if (++fails < 10) printf("span 0x%08X..0x%08X armed %d: fast %d per-line %d\n",
                                         a0, a1, g_spu_coh_armed, spu_dma_lines_reserved(a0, a1), ref_span(a0, a1));
            }
        }
    }
    /* copies */
    static uint8_t src[20000], d1[20000], d2[20000];
    long long copies = 0;
    for (int t = 0; t < 200000; t++) {
        const uint32_t n = (t & 3) ? 16u * (1 + rnd() % 20) : 1 + rnd() % 400;
        const uint32_t so = rnd() % 4096, dof = rnd() % 4096;
        for (uint32_t i = 0; i < n; i++) src[so + i] = (uint8_t)rnd();
        memset(d1, 0x5A, sizeof d1); memset(d2, 0x5A, sizeof d2);
        spu_dma_copy(d1 + dof, src + so, n);
        memcpy(d2 + dof, src + so, n);
        copies++;
        if (memcmp(d1, d2, sizeof d1)) { if (++fails < 10) printf("copy n=%u differs\n", n); }
    }
    printf("%s: %lld spans, %lld copies, %lld failures\n", fails ? "FAIL" : "PASS", spans, copies, fails);
    return fails ? 1 : 0;
}
