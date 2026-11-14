/*
 * rsx_tex_decode.h (LD_TEX_FAST) against rsx_live_draw.c's loop, copied
 * verbatim below (log2_u32, morton_index, remap_comp, decode_texel and the
 * per-level loop): every format the loop takes, random and special remaps,
 * swizzled power-of-two levels 1x1 .. 1024x1024 and odd/pitched linear
 * levels, random bytes plus every half-float pattern. Output byte for byte.
 *   test_rsx_tex_decode [iterations] | bench
 *
 * From ps3kit/libs/video (the runtime flags for C):
 *   clang-cl /nologo /O2 /std:c17 -msse4.1 /clang:-frounding-math /clang:-ffp-contract=off
 *     -I. -I../../include tests/test_rsx_tex_decode.c /Fe:test_rsx_tex_decode.exe
 */
#include "../rsx_tex_decode.h"
#include "../rsx_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define TEX_FMT_B8         0x81
#define TEX_FMT_A1R5G5B5   0x82
#define TEX_FMT_A4R4G4B4   0x83
#define TEX_FMT_R5G6B5     0x84
#define TEX_FMT_A8R8G8B8   0x85
#define TEX_FMT_G8B8       0x8B
#define TEX_FMT_DEPTH24_D8 0x90
#define TEX_FMT_X16        0x94

/* ---- rsx_live_draw.c, verbatim ---------------------------------------- */
static u32 log2_u32(u32 v) { u32 n = 0; while (v > 1) { v >>= 1; n++; } return n; }
static u32 morton_index(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 idx = 0, shift = 0;
    while (lw || lh) {
        if (lw) { idx |= (x & 1) << shift; x >>= 1; shift++; lw--; }
        if (lh) { idx |= (y & 1) << shift; y >>= 1; shift++; lh--; }
    }
    return idx;
}
static u8 remap_comp(const u8 s[4], u32 remap, u32 comp)
{
    const u32 op  = (remap >> (8 + comp * 2)) & 3;
    const u32 sel = (remap >> (comp * 2)) & 3;
    if (op == 0) return 0;
    if (op == 1) return 255;
    return s[sel];
}
static void decode_texel(u32 base_fmt, const u8* p, u32 remap, u8 d[4])
{
    u8 s[4];
    switch (base_fmt) {
    case TEX_FMT_B8: s[0] = 255; s[1] = s[2] = s[3] = p[0]; break;
    case TEX_FMT_A4R4G4B4: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = (u8)(((v >> 12) & 0xF) * 17); s[1] = (u8)(((v >> 8) & 0xF) * 17);
        s[2] = (u8)(((v >> 4) & 0xF) * 17);  s[3] = (u8)((v & 0xF) * 17); break;
    }
    case TEX_FMT_A1R5G5B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        { static int fo = -1;
          if (fo < 0) fo = getenv("LD_FORCE_A1_OPAQUE") ? 1 : 0;
          s[0] = fo ? 255 : ((v & 0x8000) ? 255 : 0); }
        s[1] = (u8)(((v >> 10) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x1F) * 255 / 31);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    }
    case TEX_FMT_R5G6B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = 255;
        s[1] = (u8)(((v >> 11) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x3F) * 255 / 63);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    }
    case TEX_FMT_G8B8:
        s[0] = 255;
        s[1] = s[2] = p[0];
        s[3] = p[1];
        break;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: {
        for (int c = 0; c < 4; c++) {
            const u16 hbits = (u16)((p[c * 2] << 8) | p[c * 2 + 1]);
            const int  exp  = (hbits >> 10) & 0x1F;
            const int  man  = hbits & 0x3FF;
            float f;
            if (exp == 0)        f = (float)man * (1.0f / 16384.0f) * (1.0f / 64.0f);
            else if (exp == 31)  f = 1.0f;
            else {
                float m = 1.0f + (float)man / 1024.0f;
                int   e = exp - 15;
                while (e > 0) { m *= 2.0f; e--; }
                while (e < 0) { m *= 0.5f; e++; }
                f = m;
            }
            if (hbits & 0x8000) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            const u8 v = (u8)(f * 255.0f + 0.5f);
            if (c == 3) s[0] = v; else s[c + 1] = v;
        }
        break;
    }
    case TEX_FMT_DEPTH24_D8: s[0] = 255; s[1] = s[2] = s[3] = p[0]; break;
    case TEX_FMT_X16: s[0] = p[0]; s[1] = 255; s[2] = p[0]; s[3] = 255; break;
    default: s[0] = p[0]; s[1] = p[1]; s[2] = p[2]; s[3] = p[3]; break;
    }
    d[0] = remap_comp(s, remap, 1);
    d[1] = remap_comp(s, remap, 2);
    d[2] = remap_comp(s, remap, 3);
    d[3] = remap_comp(s, remap, 0);
}
/* the per-level loop of decode_guest_texture */
static void old_level(u32 base_fmt, int linear, const u8* level_src, u32 pitch, u32 texel_size,
                      u32 mw, u32 mh, u32 remap, u8* out)
{
    const u32 lw = log2_u32(mw), lh = log2_u32(mh);
    for (u32 y = 0; y < mh; y++)
        for (u32 x = 0; x < mw; x++) {
            const u8* pixel = linear
                ? level_src + (size_t)y * pitch + (size_t)x * texel_size
                : level_src + (size_t)morton_index(x, y, lw, lh) * texel_size;
            decode_texel(base_fmt, pixel, remap, out + ((size_t)y * mw + x) * 4);
        }
}
/* ------------------------------------------------------------------------ */

static unsigned long long rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)(rng >> 16); }
static const u32 FMTS[] = { TEX_FMT_B8, TEX_FMT_A1R5G5B5, TEX_FMT_A4R4G4B4, TEX_FMT_R5G6B5, TEX_FMT_A8R8G8B8,
                            TEX_FMT_G8B8, TEX_FMT_DEPTH24_D8, TEX_FMT_X16, RSX_TEX_FMT_W16Z16Y16X16_FLOAT };
static u32 tsize(u32 f) { return f == TEX_FMT_B8 ? 1 : f == TEX_FMT_A8R8G8B8 || f == TEX_FMT_DEPTH24_D8 ? 4 :
                                  f == RSX_TEX_FMT_W16Z16Y16X16_FLOAT ? 8 : 2; }

int main(int argc, char** argv)
{
    const size_t SRC = 64u << 20;
    u8* src = (u8*)malloc(SRC);
    u8* a = (u8*)malloc(4096u * 4096u * 4u);
    u8* b = (u8*)malloc(4096u * 4096u * 4u);
    for (size_t i = 0; i < SRC; i += 4) { u32 r = rnd(); memcpy(src + i, &r, 4); }
    /* every half value somewhere: the first 512 KB holds 0..65535 big-endian, twice */
    for (u32 v = 0; v < 131072u; v++) { src[v * 2] = (u8)(v >> 8); src[v * 2 + 1] = (u8)v; }

    if (argc > 1 && !strcmp(argv[1], "bench")) {
        static const u32 sz[][2] = { { 64, 64 }, { 256, 256 }, { 512, 512 }, { 1024, 1024 } };
        printf("one level, swizzled, remap 0xAAE4 (identity), ns per texel (best of 5):\n");
        for (u32 fi = 0; fi < 9; fi++)
            for (u32 si = 1; si < 4; si += 2) {
                const u32 f = FMTS[fi], w = sz[si][0], h = sz[si][1];
                double best[2] = { 1e30, 1e30 };
                for (int rep = 0; rep < 5; rep++)
                    for (int v = 0; v < 2; v++) {
                        LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
                        if (v) { rsx_tex_decode_fast = 1;
                                 rsx_tex_decode_rgba8(f, 0, src + 4096, 0, tsize(f), w, h, log2_u32(w), log2_u32(h), 0xAAE4, b); }
                        else old_level(f, 0, src + 4096, 0, tsize(f), w, h, 0xAAE4, a);
                        QueryPerformanceCounter(&t1);
                        const double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)fq.QuadPart / (w * h);
                        if (ns < best[v]) best[v] = ns;
                    }
                printf("  fmt 0x%02X %4ux%-4u  old %6.2f ns  fast %5.2f ns  %5.1fx\n", f, w, h, best[0], best[1],
                       best[0] / best[1]);
            }
        return 0;
    }

    const int iters = argc > 1 ? atoi(argv[1]) : 3000;
    long long cases = 0, texels = 0, fails = 0;
    rsx_tex_decode_fast = 1;
    for (int it = 0; it < iters; it++) {
        const u32 f = FMTS[rnd() % 9], ts = tsize(f);
        const int linear = rnd() % 3 == 0;
        u32 w, h, pitch = 0;
        if (linear) { w = 1 + rnd() % 300; h = 1 + rnd() % 200; pitch = w * ts + (rnd() % 3) * 16; }
        else { w = 1u << (rnd() % 11); h = 1u << (rnd() % 11); if ((u64)w * h > 512u * 512u) h = (512u * 512u) / w; }
        u32 remap;
        switch (rnd() % 4) {
        case 0: remap = 0xAAE4; break;                 /* identity */
        case 1: remap = rnd() & 0xFFFF; break;         /* anything */
        case 2: remap = (rnd() & 0xFF) | 0xAA00; break; /* selects only */
        default: remap = 0xAAE4 ^ (rnd() & 0xFF00); break;
        }
        const size_t need = linear ? (size_t)pitch * h : (size_t)w * h * ts;
        const size_t off = (it % 7 == 0) ? 0 : (rnd() % (SRC - need - 16)) & ~(size_t)(ts - 1);
        memset(a, 0x11, (size_t)w * h * 4); memset(b, 0x22, (size_t)w * h * 4);
        old_level(f, linear, src + off, pitch, ts, w, h, remap, a);
        if (!rsx_tex_decode_rgba8(f, linear, src + off, pitch, ts, w, h, log2_u32(w), log2_u32(h), remap, b)) {
            fails++; if (fails < 10) printf("not taken: fmt 0x%02X %ux%u\n", f, w, h); continue;
        }
        cases++; texels += (long long)w * h;
        if (memcmp(a, b, (size_t)w * h * 4)) {
            fails++;
            if (fails < 10) {
                size_t i = 0; while (a[i] == b[i]) i++;
                printf("DIFF fmt 0x%02X %s %ux%u pitch %u remap 0x%04X at texel %zu byte %zu: old %02X fast %02X\n",
                       f, linear ? "linear" : "swizzled", w, h, pitch, remap, i / 4, i % 4, a[i], b[i]);
            }
        }
    }
    printf("%s: %lld levels, %lld texels, %lld failures\n", fails ? "FAIL" : "PASS", cases, texels, fails);
    return fails ? 1 : 0;
}
