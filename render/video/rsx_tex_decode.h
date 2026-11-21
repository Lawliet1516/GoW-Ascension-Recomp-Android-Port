/*
 * ps3recomp - fast RSX texture decode to RGBA8 (LD_TEX_FAST)
 *
 * One mip level of an uncompressed RSX texture, swizzled or pitch-linear,
 * into tightly packed RGBA8 for rsx_live_draw.c's decode_guest_texture: the
 * same bytes as its per-texel loop (morton_index + decode_texel + remap_comp)
 * without that loop's per-texel swizzle walk, format switch and four remap
 * branches. Header-only (static), for the one file that decodes textures.
 *
 * Same bytes, by construction:
 *  - the swizzle: morton_index(x, y) puts x's and y's bits in disjoint
 *    positions, so it is morton_index(x, 0) | morton_index(0, y); both
 *    halves are tabled per level by a copy of that function, and since the
 *    texel size is a power of two the OR survives the multiply;
 *  - the format: decode_texel's components s[0..3] (A, R, G, B) for every
 *    16-bit value, and the half-to-byte conversion of W16Z16Y16X16_FLOAT, are
 *    tables built by copies of its expressions; B8, DEPTH24_D8 and A8R8G8B8
 *    are byte moves;
 *  - the remap: each output byte is s[sel], 0 or 255 by the same two bit
 *    fields remap_comp reads, as one byte shuffle and an OR per texel.
 * tests/test_rsx_tex_decode.c runs a verbatim copy of the old loop against
 * it (every format, random and identity remaps, swizzled 1x1..1024x1024 and
 * odd pitched linear levels, every half value): byte-identical, 13-31x
 * faster. LD_TEX_FAST=0: the caller's own loop.
 *
 * rsx_tex_decode_rgba8: `src` the level's first texel, `pitch` its row pitch
 * (linear only), `texel_size` bytes per texel, mw x mh texels, lw/lh =
 * log2 of mw/mh as the caller computes them, `remap` the component remap,
 * `dst` mw * mh * 4 bytes. Returns 1 when done, 0 when it does not take the
 * format or size (or LD_TEX_FAST=0): the caller then runs its own loop.
 */
#ifndef PS3RECOMP_RSX_TEX_DECODE_H
#define PS3RECOMP_RSX_TEX_DECODE_H

#include "ps3emu/ps3types.h"
#include <stdlib.h>
#include <string.h>
#include <smmintrin.h>

#define RSX_TD_B8         0x81u
#define RSX_TD_A1R5G5B5   0x82u
#define RSX_TD_A4R4G4B4   0x83u
#define RSX_TD_R5G6B5     0x84u
#define RSX_TD_A8R8G8B8   0x85u
#define RSX_TD_G8B8       0x8Bu
#define RSX_TD_DEPTH24_D8 0x90u
#define RSX_TD_X16        0x94u
#define RSX_TD_W16Z16Y16X16_FLOAT 0x9Au

/* -1: read LD_TEX_FAST on first use; tests set 0/1. */
static int rsx_tex_decode_fast = -1;

static u32 rsx_td_morton(u32 x, u32 y, u32 lw, u32 lh)   /* rsx_live_draw.c morton_index */
{
    u32 idx = 0, shift = 0;
    while (lw || lh) {
        if (lw) { idx |= (x & 1) << shift; x >>= 1; shift++; lw--; }
        if (lh) { idx |= (y & 1) << shift; y >>= 1; shift++; lh--; }
    }
    return idx;
}

/* decode_texel's s[] for a 16-bit format, as one little-endian word
 * (byte k = s[k]); same expressions. */
static u32 rsx_td_s16(u32 fmt, u32 v, int a1_opaque)
{
    u8 s[4];
    switch (fmt) {
    case RSX_TD_A4R4G4B4:
        s[0] = (u8)(((v >> 12) & 0xF) * 17); s[1] = (u8)(((v >> 8) & 0xF) * 17);
        s[2] = (u8)(((v >> 4) & 0xF) * 17);  s[3] = (u8)((v & 0xF) * 17); break;
    case RSX_TD_A1R5G5B5:
        s[0] = a1_opaque ? 255 : ((v & 0x8000) ? 255 : 0);
        s[1] = (u8)(((v >> 10) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x1F) * 255 / 31);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    case RSX_TD_R5G6B5:
        s[0] = 255;
        s[1] = (u8)(((v >> 11) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x3F) * 255 / 63);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    case RSX_TD_G8B8: {
        const u8 p0 = (u8)(v >> 8), p1 = (u8)v;
        s[0] = 255; s[1] = s[2] = p0; s[3] = p1; break;
    }
    default: {   /* RSX_TD_X16 */
        const u8 p0 = (u8)(v >> 8);
        s[0] = p0; s[1] = 255; s[2] = p0; s[3] = 255; break;
    }
    }
    return (u32)s[0] | ((u32)s[1] << 8) | ((u32)s[2] << 16) | ((u32)s[3] << 24);
}

/* decode_texel's W16Z16Y16X16_FLOAT component conversion, same expressions. */
static u8 rsx_td_half_u8(u32 hbits)
{
    const int exp = (hbits >> 10) & 0x1F;
    const int man = hbits & 0x3FF;
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
    return (u8)(f * 255.0f + 0.5f);
}

/* Tables, built on first use (by the one thread that decodes textures; a
 * race would only build the same table twice). */
static u32* s_rsx_td_lut16[5];          /* A4R4G4B4, A1R5G5B5, R5G6B5, G8B8, X16 */
static u8*  s_rsx_td_half;
static int  s_rsx_td_a1_opaque = -1;
static const u32* rsx_td_lut16(u32 fmt)
{
    const int k = fmt == RSX_TD_A4R4G4B4 ? 0 : fmt == RSX_TD_A1R5G5B5 ? 1 : fmt == RSX_TD_R5G6B5 ? 2 :
                  fmt == RSX_TD_G8B8 ? 3 : 4;
    if (!s_rsx_td_lut16[k]) {
        if (s_rsx_td_a1_opaque < 0) s_rsx_td_a1_opaque = getenv("LD_FORCE_A1_OPAQUE") ? 1 : 0;
        u32* t = (u32*)malloc(65536u * sizeof(u32));
        if (!t) return NULL;
        for (u32 v = 0; v < 65536u; v++) t[v] = rsx_td_s16(fmt, v, s_rsx_td_a1_opaque);
        s_rsx_td_lut16[k] = t;
    }
    return s_rsx_td_lut16[k];
}
static const u8* rsx_td_half_lut(void)
{
    if (!s_rsx_td_half) {
        u8* t = (u8*)malloc(65536u);
        if (!t) return NULL;
        for (u32 v = 0; v < 65536u; v++) t[v] = rsx_td_half_u8(v);
        s_rsx_td_half = t;
    }
    return s_rsx_td_half;
}

static int rsx_tex_decode_rgba8(u32 base_fmt, int linear, const u8* src, u32 pitch, u32 texel_size,
                                u32 mw, u32 mh, u32 lw, u32 lh, u32 remap, u8* dst)
{
    if (rsx_tex_decode_fast < 0) {
        const char* e = getenv("LD_TEX_FAST");
        rsx_tex_decode_fast = !(e && e[0] == '0');
    }
    if (!rsx_tex_decode_fast || !src || !dst || !mw || !mh || mw > 4096 || mh > 4096) return 0;
    const u32* lut = NULL;
    const u8* half = NULL;
    switch (base_fmt) {
    case RSX_TD_B8:         if (texel_size != 1) return 0; break;
    case RSX_TD_A4R4G4B4: case RSX_TD_A1R5G5B5: case RSX_TD_R5G6B5: case RSX_TD_G8B8: case RSX_TD_X16:
                        if (texel_size != 2 || !(lut = rsx_td_lut16(base_fmt))) return 0; break;
    case RSX_TD_A8R8G8B8: case RSX_TD_DEPTH24_D8:
                        if (texel_size != 4) return 0; break;
    case RSX_TD_W16Z16Y16X16_FLOAT:
                        if (texel_size != 8 || !(half = rsx_td_half_lut())) return 0; break;
    default: return 0;
    }

    /* the remap as a byte shuffle of s (A, R, G, B) and an OR: output byte k
     * is remap_comp(s, remap, (k + 1) & 3) */
    u8 shuf[16], orb[16];
    for (u32 k = 0; k < 4; k++) {
        const u32 comp = (k + 1) & 3;
        const u32 op = (remap >> (8 + comp * 2)) & 3, sel = (remap >> (comp * 2)) & 3;
        shuf[k] = op <= 1 ? 0x80 : (u8)sel;
        orb[k] = op == 1 ? 0xFF : 0;
    }
    for (u32 k = 4; k < 16; k++) { shuf[k] = 0x80; orb[k] = 0; }
    const __m128i vs = _mm_loadu_si128((const __m128i*)shuf), vo = _mm_loadu_si128((const __m128i*)orb);

    /* the swizzle, tabled: offset of texel (x, y) = (mx[x] | my[y]) * size
     * (the two bit sets are disjoint and the size a power of two) */
    u32 mx[4096], my[4096];
    if (!linear) {
        for (u32 x = 0; x < mw; x++) mx[x] = rsx_td_morton(x, 0, lw, lh) * texel_size;
        for (u32 y = 0; y < mh; y++) my[y] = rsx_td_morton(0, y, lw, lh) * texel_size;
    }

#define RSX_TD_LOOP(LOAD_S)                                                            \
    for (u32 y = 0; y < mh; y++) {                                                 \
        u32* o = (u32*)(dst + (size_t)y * mw * 4u);                                \
        if (linear) {                                                              \
            const u8* row = src + (size_t)y * pitch;                               \
            for (u32 x = 0; x < mw; x++) {                                         \
                const u8* p = row + (size_t)x * texel_size;                        \
                u32 s; LOAD_S;                                                     \
                o[x] = (u32)_mm_cvtsi128_si32(_mm_or_si128(                        \
                    _mm_shuffle_epi8(_mm_cvtsi32_si128((int)s), vs), vo));         \
            }                                                                      \
        } else {                                                                   \
            const u32 ry = my[y];                                                  \
            for (u32 x = 0; x < mw; x++) {                                         \
                const u8* p = src + (size_t)(mx[x] | ry);                          \
                u32 s; LOAD_S;                                                     \
                o[x] = (u32)_mm_cvtsi128_si32(_mm_or_si128(                        \
                    _mm_shuffle_epi8(_mm_cvtsi32_si128((int)s), vs), vo));         \
            }                                                                      \
        }                                                                          \
    }

    switch (base_fmt) {
    case RSX_TD_B8:         /* s = 255, p0, p0, p0 */
        RSX_TD_LOOP(s = 0xFFu | (0x010101u * p[0]) << 8)
        break;
    case RSX_TD_DEPTH24_D8: /* s = 255, p0, p0, p0 */
        RSX_TD_LOOP(s = 0xFFu | (0x010101u * p[0]) << 8)
        break;
    case RSX_TD_A8R8G8B8:   /* s = p0, p1, p2, p3 */
        RSX_TD_LOOP(memcpy(&s, p, 4))
        break;
    case RSX_TD_W16Z16Y16X16_FLOAT: /* s = W, X, Y, Z */
        RSX_TD_LOOP(s = (u32)half[((u32)p[6] << 8) | p[7]] |
                    ((u32)half[((u32)p[0] << 8) | p[1]] << 8) |
                    ((u32)half[((u32)p[2] << 8) | p[3]] << 16) |
                    ((u32)half[((u32)p[4] << 8) | p[5]] << 24))
        break;
    default:            /* the 16-bit formats */
        RSX_TD_LOOP(s = lut[((u32)p[0] << 8) | p[1]])
        break;
    }
#undef RSX_TD_LOOP
    return 1;
}

#endif /* PS3RECOMP_RSX_TEX_DECODE_H */
