/* PPU VMX helpers: NEON vs the scalar reference (the _ref forms), bit for bit,
 * then a speed comparison. Build for the phone with android/bench/build.sh. */
#include "ppu/ppu_vmx.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t r32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
/* random float bits with weight on zeros, denormals, infinities, NaNs */
static uint32_t rf(void)
{
    uint32_t s = (r32() & 1u) << 31, m = r32() & 0x7FFFFFu;
    switch (r32() % 10) {
    case 0: return s;                                   /* +-0 */
    case 1: return s | m;                               /* denormal */
    case 2: return s | 0x7F800000u;                     /* inf */
    case 3: return s | 0x7F800000u | (m ? m : 1);       /* NaN */
    case 4: return r32();
    default: return s | ((100u + r32() % 55u) << 23) | m;
    }
}
static void fill(uint8_t* v) { for (int i = 0; i < 4; i++) vmx_sw(v, i, rf()); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static long fails;
#define CHECK3(nm, fn, ref) do { for (long k = 0; k < N; k++) { uint8_t a[16], b[16], c[16], x[16], y[16]; \
    fill(a); fill(b); fill(c); uint32_t vs = (k & 1) ? VMX_NJ : 0; fn(x, a, b, c, vs); ref(y, a, b, c, vs); \
    if (memcmp(x, y, 16)) { if (fails++ < 5) printf("MISMATCH %s k=%ld\n", nm, k); } } } while (0)
#define CHECK2(nm, fn, ref) do { for (long k = 0; k < N; k++) { uint8_t a[16], b[16], x[16], y[16]; \
    fill(a); fill(b); uint32_t vs = (k & 1) ? VMX_NJ : 0; fn(x, a, b, vs); ref(y, a, b, vs); \
    if (memcmp(x, y, 16)) { if (fails++ < 5) printf("MISMATCH %s k=%ld\n", nm, k); } } } while (0)
#define CHECK1(nm, fn, ref) do { for (long k = 0; k < N; k++) { uint8_t b[16], x[16], y[16]; fill(b); \
    uint32_t vs = (k & 1) ? VMX_NJ : 0; fn(x, b, vs); ref(y, b, vs); \
    if (memcmp(x, y, 16)) { if (fails++ < 5) printf("MISMATCH %s k=%ld\n", nm, k); } } } while (0)
#define CHECKC(nm, fn, ref) do { for (long k = 0; k < N; k++) { uint8_t a[16], b[16], x[16], y[16]; \
    fill(a); fill(b); if (k % 7 == 0) memcpy(b, a, 16); uint32_t rx = fn(x, a, b), ry = ref(y, a, b); \
    if (memcmp(x, y, 16) || rx != ry) { if (fails++ < 5) printf("MISMATCH %s k=%ld (cr %u vs %u)\n", nm, k, rx, ry); } } } while (0)

/* 64 independent vector sets rotated per call: no loop-invariant hoisting, no dependency chain
 * (throughput). The memory clobber stops the compiler from caching inputs or dropping stores. */
#define BENCH(label, call_fast, call_ref) do { static uint8_t A[64][16], B[64][16], C[64][16], D[64][16]; \
    for (int q = 0; q < 64; q++) { fill(A[q]); fill(B[q]); fill(C[q]); } \
    double t0 = now(); for (long i = 0; i < M; i++) { uint8_t *a = A[i & 63], *b = B[i & 63], *c = C[i & 63], *d = D[i & 63]; \
        call_fast; __asm__ volatile("" ::: "memory"); } double tf = now() - t0; \
    t0 = now(); for (long i = 0; i < M; i++) { uint8_t *a = A[i & 63], *b = B[i & 63], *c = C[i & 63], *d = D[i & 63]; \
        call_ref; __asm__ volatile("" ::: "memory"); } double tr = now() - t0; \
    printf("%-10s neon %6.2f ns/op   scalar %6.2f ns/op   x%.2f\n", label, tf / M * 1e9, tr / M * 1e9, tr / tf); } while (0)

int main(void)
{
    const long N = 3000000, M = 20000000;
    CHECK3("vmaddfp", vmx_vmaddfp, vmx_vmaddfp_ref);
    CHECK3("vnmsubfp", vmx_vnmsubfp, vmx_vnmsubfp_ref);
    CHECK2("vaddfp", vmx_vaddfp, vmx_vaddfp_ref);
    CHECK2("vsubfp", vmx_vsubfp, vmx_vsubfp_ref);
    CHECK1("vrefp", vmx_vrefp, vmx_vrefp_ref);
    CHECK1("vrsqrtefp", vmx_vrsqrtefp, vmx_vrsqrtefp_ref);
    CHECKC("vcmpeqfp", vmx_vcmpeqfp, vmx_vcmpeqfp_ref);
    CHECKC("vcmpgefp", vmx_vcmpgefp, vmx_vcmpgefp_ref);
    CHECKC("vcmpgtfp", vmx_vcmpgtfp, vmx_vcmpgtfp_ref);
    CHECKC("vcmpequw", vmx_vcmpequw, vmx_vcmpequw_ref);
    for (unsigned u = 0; u < 32; u++) for (long k = 0; k < 20000; k++) { uint8_t b[16], x[16], y[16]; fill(b);
        vmx_vcfsx(x, b, u); vmx_vcfsx_ref(y, b, u); if (memcmp(x, y, 16) && fails++ < 5) printf("MISMATCH vcfsx u=%u\n", u); }
    printf("bit-exactness: %s (%ld mismatches over %ld random/special vectors per op, NJ on and off)\n",
           fails ? "FAILED" : "ok", fails, N);
    BENCH("vmaddfp", vmx_vmaddfp(d, a, b, c, VMX_NJ), vmx_vmaddfp_ref(d, a, b, c, VMX_NJ));
    BENCH("vaddfp",  vmx_vaddfp(d, a, b, VMX_NJ),     vmx_vaddfp_ref(d, a, b, VMX_NJ));
    BENCH("vrefp",   vmx_vrefp(d, a, VMX_NJ),         vmx_vrefp_ref(d, a, VMX_NJ));
    BENCH("vcmpgtfp", vmx_vcmpgtfp(d, a, b),          vmx_vcmpgtfp_ref(d, a, b));
    return fails != 0;
}
