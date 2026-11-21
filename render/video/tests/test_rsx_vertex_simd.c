/*
 * rsx_vertex_fetch_many: the SSE4.1 loops (LD_VTX_SIMD=1) against the scalar
 * loops (=0) and the per-vertex rsx_vertex_fetch_one, byte for byte.
 *
 *   test_rsx_vertex_simd [iterations]     random plans, every format, every
 *                                         size 0..4 (and 5..7), any stride,
 *                                         NaN / inf / denormal / -0 bit
 *                                         patterns, out-of-span references,
 *                                         instanced and unmapped attributes,
 *                                         INLINE_ARRAY streams, under five
 *                                         MXCSR modes. Every vertex span ends
 *                                         at a no-access page, so a read past
 *                                         it faults.
 *   test_rsx_vertex_simd bench            timing of typical layouts
 *
 * Built by hand (no CMake target), from ps3kit\libs\video:
 *   clang-cl /nologo /O2 -msse4.1 /clang:-frounding-math /clang:-ffp-contract=off
 *     -I. -I..\..\include -I..\..\runtime tests\test_rsx_vertex_simd.c
 *     rsx_vertex_compact.c /Fe:test_rsx_vertex_simd.exe
 * (the runtime's own flags for this file: -O2 -msse4.1 -frounding-math
 * -ffp-contract=off).
 */
#include "../rsx_vertex_compact.h"
#include "../rsx_vertex_formats.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* rsx_vertex_compact.c's plan_init / inline_layout use these; the test fills
 * its plans directly and never calls them. */
void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out)
{ (void)rsx; (void)index; memset(out, 0, sizeof *out); }
u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx) { (void)rsx; return 0; }
void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 index, float out[4])
{ (void)rsx; (void)index; out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; }

/* ---- guest memory: one region per attribute, its span ending at a guard page */
#define REGION (1u << 20)
typedef struct {
    u8* mem[RSX_DSP_NUM_VERTEX_ATTR + 1];   /* [16]: the inline stream */
    u32 span_fail;                           /* bit a: refuse attr a's whole span */
} guest;
static guest G;

static u8* region_alloc(void)
{
    u8* p = (u8*)VirtualAlloc(NULL, REGION + 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    if (!p || !VirtualProtect(p + REGION, 4096, PAGE_NOACCESS, &old)) { fprintf(stderr, "VirtualAlloc failed\n"); exit(2); }
    return p;
}

/* location = attribute index (the test's convention); offsets index its region. */
static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    if (location >= RSX_DSP_NUM_VERTEX_ATTR) return NULL;
    if (((G.span_fail >> location) & 1u) && min_bytes > 64) return NULL;
    if ((u64)offset + min_bytes > REGION) return NULL;
    return G.mem[location] + offset;
}

static u64 rng_s = 0x9E3779B97F4A7C15ull;
static u32 rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (u32)(rng_s >> 16);
}

static const u32 F32_SPECIAL[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x007FFFFFu, 0x00800000u,
    0x7F800000u, 0xFF800000u, 0x7F800001u, 0xFF800001u, 0x7FC00000u, 0xFFC00000u,
    0x7FBFFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu };
static const u16 F16_SPECIAL[] = {
    0x0000, 0x8000, 0x0001, 0x8001, 0x03FF, 0x83FF, 0x0400, 0x8400, 0x7BFF, 0xFBFF,
    0x7C00, 0xFC00, 0x7C01, 0xFC01, 0x7DFF, 0x7E00, 0xFE00, 0x7FFF, 0xFFFF, 0x3C00 };
static const u16 S16_SPECIAL[] = { 0x0000, 0x8000, 0x8001, 0x7FFF, 0xFFFF, 0x0001, 0x7FFE };

static void fill_region(u8* m)
{
    for (u32 i = 0; i < REGION; i += 4) { u32 r = rnd(); memcpy(m + i, &r, 4); }
}

/* Special bit patterns written into the components of elements: NaNs with
 * every payload class, infinities, denormals, signed zeros, the s16 extremes.
 * Half of the components of the elements first..last of an attribute. */
static void sprinkle(u8* elem0, u32 stride, u32 nelem, u32 type, u32 size, u8* limit)
{
    const u32 n = size < 4 ? size : 4;
    for (u32 e = 0; e < nelem; e++) {
        u8* s = elem0 + (size_t)e * stride;
        for (u32 c = 0; c < n; c++) {
            if (rnd() & 1) continue;
            if (type == RSX_VTX_TYPE_FLOAT && s + c * 4 + 4 <= limit) {
                u32 v = F32_SPECIAL[rnd() % (sizeof F32_SPECIAL / 4)];
                if (rnd() & 1) v ^= rnd() & 0x007FFFFFu;   /* other payloads / denormals */
                u8 be[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v };
                memcpy(s + c * 4, be, 4);
            } else if (type == RSX_VTX_TYPE_HALF && s + c * 2 + 2 <= limit) {
                u16 v = F16_SPECIAL[rnd() % (sizeof F16_SPECIAL / 2)];
                if (rnd() & 1) v ^= (u16)(rnd() & 0x03FF);
                s[c * 2] = (u8)(v >> 8); s[c * 2 + 1] = (u8)v;
            } else if ((type == RSX_VTX_TYPE_SNORM16 || type == RSX_VTX_TYPE_SINT16) && s + c * 2 + 2 <= limit) {
                u16 v = S16_SPECIAL[rnd() % (sizeof S16_SPECIAL / 2)];
                s[c * 2] = (u8)(v >> 8); s[c * 2 + 1] = (u8)v;
            } else if ((type == RSX_VTX_TYPE_UNORM8 || type == RSX_VTX_TYPE_UINT8) && s + c + 1 <= limit) {
                static const u8 b8[] = { 0, 1, 127, 128, 254, 255 };
                s[c] = b8[rnd() % 6];
            }
        }
    }
}

static float rand_float(void)
{
    u32 v = (rnd() & 3) ? (rnd() % 2000) : rnd();
    float f; memcpy(&f, &v, 4);
    return (rnd() & 1) ? (float)(int)(rnd() % 7) - 3.0f : f;
}

/* ---- one random case ---------------------------------------------------- */
#define MAXV 600
static rsx_vertex_ref refs[MAXV];
static u8 out_new[MAXV * 16 * 16], out_old[MAXV * 16 * 16], one[16 * 16];

static int build_case(rsx_vertex_fetch_plan* plan, u32* count, u32* prep_count, int use_inline)
{
    memset(plan, 0, sizeof *plan);
    u32 mask = 0, nattr = 1 + rnd() % 6;
    for (u32 k = 0; k < nattr; k++) mask |= 1u << (rnd() % 16);
    if (rnd() & 1) mask |= 1u;
    rsx_vertex_layout_plan layout;
    rsx_vertex_layout_plan_init(&layout, mask);
    plan->layout = layout;
    plan->guest_ptr = guest_ptr;
    plan->base_offset = 0;
    plan->divider_mask = rnd() & 0xFFFFu;
    G.span_fail = (rnd() % 4 == 0) ? rnd() & 0xFFFFu : 0;

    /* references: a base vertex plus consecutive, shuffled or repeated ids */
    const u32 n = 1 + rnd() % MAXV;
    const u32 vbase = rnd() % 2000, bidx = (rnd() & 3) ? 0 : rnd() % 300;
    const u32 mode = rnd() % 3;
    for (u32 i = 0; i < n; i++) {
        refs[i].vertex_id = mode == 0 ? vbase + i : mode == 1 ? vbase + rnd() % (n + 1) : vbase + (i ^ (rnd() & 7));
        refs[i].base_index = bidx;
    }
    *count = n;
    /* prepare on a prefix sometimes, so later references fall outside the span */
    *prep_count = (rnd() % 5 == 0) ? 1 + rnd() % n : n;

    u32 inline_packed = 0;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* f = &plan->attr[a];
        u32 type = rnd() % 8;
        if (rnd() % 32 == 0) type = 8 + rnd() % 8;              /* unknown encodings */
        if (rnd() % 3 == 0) type = RSX_VTX_TYPE_FLOAT;           /* weight the hot ones */
        u32 size = rnd() % 5;
        if (rnd() % 16 == 0) size = 5 + rnd() % 3;
        if (size == 0 && rnd() % 4) size = 1 + rnd() % 4;
        f->desc.type = type;
        f->desc.size = size;
        f->elem_size = rsx_vertex_attrib_size(type, size);
        u32 stride = 0;
        switch (rnd() % 4) {
        case 0: stride = 0; break;                                   /* tightly packed */
        case 1: stride = f->elem_size + rnd() % 40; break;
        case 2: stride = 16 + (rnd() % 6) * 8; break;                /* typical interleaved */
        default: stride = 1 + rnd() % 64; break;                     /* anything, overlaps too */
        }
        f->desc.stride = stride;
        f->stride = stride ? stride : f->elem_size;
        f->desc.frequency = (rnd() % 8 == 0) ? 2 + rnd() % 4 : rnd() % 2;
        f->desc.location = a;
        for (int c = 0; c < 4; c++) f->default_value[c] = rand_float();
        if (f->elem_size && f->stride) {
            /* Put the span of [min element, max element] flush against the
             * guard page: desc.offset = REGION - (last * stride + elem_size). */
            u32 mn = 0xFFFFFFFFu, mx = 0;
            for (u32 i = 0; i < *prep_count; i++) {
                const u32 e = rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,
                                                      f->desc.frequency, (plan->divider_mask >> a) & 1u);
                if (e < mn) mn = e;
                if (e > mx) mx = e;
            }
            const u64 need = (u64)mx * f->stride + f->elem_size;
            f->desc.offset = need <= REGION ? (u32)(REGION - need) : 0;
            if (need <= REGION && mx >= mn && mx - mn < 4096)
                sprinkle(G.mem[a] + f->desc.offset + (u64)mn * f->stride, f->stride, mx - mn + 1,
                         type, size, G.mem[a] + REGION);
        }
        plan->inline_off[a] = inline_packed;
        if (type && size) inline_packed += f->elem_size;
    }
    if (use_inline) {
        const u32 stride = inline_packed + (rnd() % 3) * 4;
        const u32 verts = stride ? 1 + rnd() % 800 : 0;
        if (stride && (u64)stride * verts <= REGION) {
            plan->inline_stride = stride;
            plan->inline_bytes = stride * verts;
            plan->inline_data = G.mem[16] + REGION - plan->inline_bytes;
            for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++)
                if (plan->attr[a].desc.type && plan->attr[a].desc.size)
                    sprinkle((u8*)plan->inline_data + plan->inline_off[a], stride, verts,
                             plan->attr[a].desc.type, plan->attr[a].desc.size, G.mem[16] + REGION);
        }
    }
    return 1;
}

static long long g_cases, g_verts, g_fail;
static void run_case(u32 iter)
{
    rsx_vertex_fetch_plan plan;
    u32 count, prep;
    build_case(&plan, &count, &prep, rnd() % 8 == 0);
    rsx_vertex_fetch_plan_prepare(&plan, refs, prep);
    const u32 vs = plan.layout.stride;
    memset(out_new, 0xCD, (size_t)count * vs);
    memset(out_old, 0xAB, (size_t)count * vs);
    rsx_vertex_fetch_simd = 1;
    const int rn = rsx_vertex_fetch_many(&plan, refs, count, out_new);
    rsx_vertex_fetch_simd = 0;
    const int ro = rsx_vertex_fetch_many(&plan, refs, count, out_old);
    g_cases++; g_verts += count;
    if (rn != ro) {
        g_fail++;
        if (g_fail < 10) printf("iter %u: result simd %d scalar %d\n", iter, rn, ro);
        return;
    }
    if (!rn) return;
    for (u32 i = 0; i < count; i++) {
        const u8* a = out_new + (size_t)i * vs;
        const u8* b = out_old + (size_t)i * vs;
        int r1 = rsx_vertex_fetch_one(&plan, &refs[i], one);
        if (memcmp(a, b, vs) || !r1 || memcmp(a, one, vs)) {
            g_fail++;
            if (g_fail < 10) {
                for (u32 s = 0; s < plan.layout.count; s++) {
                    if (!memcmp(a + s * 16, b + s * 16, 16) && !memcmp(a + s * 16, one + s * 16, 16)) continue;
                    const rsx_vertex_fetch_attr* f = &plan.attr[plan.layout.attrs[s]];
                    u32 x[4], y[4], z[4];
                    memcpy(x, a + s * 16, 16); memcpy(y, b + s * 16, 16); memcpy(z, one + s * 16, 16);
                    printf("iter %u vertex %u attr %u type %u size %u stride %u: simd %08X %08X %08X %08X"
                           " scalar %08X %08X %08X %08X one %08X %08X %08X %08X\n",
                           iter, i, plan.layout.attrs[s], f->desc.type, f->desc.size, f->stride,
                           x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3], z[0], z[1], z[2], z[3]);
                }
            }
            return;
        }
    }
}

/* ---- bench: typical layouts ------------------------------------------- */
typedef struct { const char* name; u32 stride; u32 n; struct { u32 type, size, off; } a[6]; } layout_def;
static const layout_def LAYOUTS[] = {
    { "pos f32x3, nrm f32x3, uv f32x2 (32 B)", 32, 3,
      { { 2, 3, 0 }, { 2, 3, 12 }, { 2, 2, 24 } } },
    { "pos f32x3, col u8n x4, uv f32x2 (24 B)", 24, 3,
      { { 2, 3, 0 }, { 4, 4, 12 }, { 2, 2, 16 } } },
    { "pos f32x4, uv f16x2, nrm s16n x4 (32 B)", 32, 3,
      { { 2, 4, 0 }, { 3, 2, 16 }, { 1, 4, 20 } } },
    { "skinned: pos f32x3, nrm f32x3, uv f32x2, wgt u8n x4, idx u8 x4 (40 B)", 40, 5,
      { { 2, 3, 0 }, { 2, 3, 12 }, { 2, 2, 24 }, { 4, 4, 32 }, { 7, 4, 36 } } },
    { "2D: pos f32x2, col u8n x4, uv f32x2 (20 B)", 20, 3,
      { { 2, 2, 0 }, { 4, 4, 8 }, { 2, 2, 12 } } },
    { "pos f32x3, nrm cmp, uv f16x2, tan cmp (24 B)", 24, 4,
      { { 2, 3, 0 }, { 6, 1, 12 }, { 3, 2, 16 }, { 6, 1, 20 } } },
};
static double now_us(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}
static double g_tsc_per_us;
static void bench(void)
{
    { const double t0 = now_us(); const unsigned long long c0 = __rdtsc();
      while (now_us() - t0 < 200000.0) { }
      g_tsc_per_us = (double)(__rdtsc() - c0) / (now_us() - t0); }
    const u32 verts = 200, draws = 2500;           /* one combat frame, roughly */
    printf("rsx_vertex_fetch_many, %u draws x %u vertices, distinct data per draw (frame-sized working set)\n",
           draws, verts);
    double tot_old = 0, tot_new = 0;
    for (u32 L = 0; L < sizeof LAYOUTS / sizeof LAYOUTS[0]; L++) {
        const layout_def* d = &LAYOUTS[L];
        rsx_vertex_fetch_plan plan;
        memset(&plan, 0, sizeof plan);
        u32 mask = 0;
        for (u32 k = 0; k < d->n; k++) mask |= 1u << k;
        rsx_vertex_layout_plan_init(&plan.layout, mask);
        plan.guest_ptr = guest_ptr;
        G.span_fail = 0;
        for (u32 k = 0; k < d->n; k++) {
            rsx_vertex_fetch_attr* f = &plan.attr[k];
            f->desc.type = d->a[k].type; f->desc.size = d->a[k].size;
            f->desc.stride = d->stride; f->desc.offset = d->a[k].off; f->desc.location = 0;
            f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
            f->stride = d->stride;
            f->default_value[3] = 1.0f;
        }
        for (u32 i = 0; i < verts; i++) { refs[i].vertex_id = i; refs[i].base_index = 0; }
        u8* out = (u8*)_aligned_malloc((size_t)verts * plan.layout.stride, 64);
        double t[2] = { 1e30, 1e30 };
        for (int rep = 0; rep < 5; rep++) {
            for (int simd = 0; simd < 2; simd++) {
                rsx_vertex_fetch_simd = simd;
                unsigned long long ticks = 0;
                for (u32 dr = 0; dr < draws; dr++) {
                    /* each draw its own vertices: walk the 1 MB region */
                    const u32 off = (dr * verts * d->stride) % (REGION - verts * d->stride - 64);
                    for (u32 k = 0; k < d->n; k++) plan.attr[k].desc.offset = off + d->a[k].off;
                    rsx_vertex_fetch_plan_prepare(&plan, refs, verts);
                    const unsigned long long c0 = __rdtsc();
                    rsx_vertex_fetch_many(&plan, refs, verts, out);
                    ticks += __rdtsc() - c0;
                }
                const double dt = (double)ticks / g_tsc_per_us;
                if (dt < t[simd]) t[simd] = dt;
            }
        }
        _aligned_free(out);
        printf("  %-66s scalar %6.2f ns/vertex  simd %6.2f ns/vertex  speedup %.2fx\n", d->name,
               t[0] * 1e3 / (draws * verts), t[1] * 1e3 / (draws * verts), t[0] / t[1]);
        tot_old += t[0]; tot_new += t[1];
    }
    printf("  all layouts: %.2fx (rsx_vertex_fetch_many alone, prepare excluded: scalar %.0f us,"
           " simd %.0f us for 6 x %u draws)\n", tot_old / tot_new, tot_old, tot_new, draws);
}

int main(int argc, char** argv)
{
    for (int i = 0; i <= 16; i++) G.mem[i] = region_alloc();
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        fill_region(G.mem[0]);
        bench();
        return 0;
    }
    const u32 iters = argc > 1 ? (u32)atoi(argv[1]) : 4000;
    static const struct { unsigned bits; const char* name; } MODES[] = {
        { 0x1F80, "nearest" }, { 0x7F80, "toward zero" }, { 0x9FC0, "nearest+FTZ+DAZ" },
        { 0x5F80, "up" }, { 0xFFC0, "toward zero+FTZ+DAZ" } };
    const unsigned saved = _mm_getcsr();
    for (u32 m = 0; m < sizeof MODES / sizeof MODES[0]; m++) {
        for (u32 a = 0; a <= 16; a++) fill_region(G.mem[a]);
        _mm_setcsr(MODES[m].bits);
        const long long f0 = g_fail, c0 = g_cases;
        for (u32 it = 0; it < iters; it++) {
            if (it % 500 == 499)   /* new data now and then */
                fill_region(G.mem[rnd() % 17]);
            run_case(it);
        }
        _mm_setcsr(saved);
        printf("MXCSR %-22s %lld cases, %lld failures\n", MODES[m].name, g_cases - c0, g_fail - f0);
    }
    printf("%s: %lld cases, %lld vertices, %lld failures\n", g_fail ? "FAIL" : "PASS", g_cases, g_verts, g_fail);
    return g_fail ? 1 : 0;
}
