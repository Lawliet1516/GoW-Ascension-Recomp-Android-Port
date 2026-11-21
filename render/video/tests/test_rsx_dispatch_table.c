/*
 * rsx_dispatch_method: the method-kind table (RSX_DSP_TABLE=1) against the
 * old test chain (=0), and a timing of both on a combat-frame-shaped stream.
 *
 *   test_rsx_dispatch_table [streams]   random + frame-shaped method streams
 *                                       through two dispatchers, one per mode;
 *                                       every sink callback (kind, arguments,
 *                                       and a hash of the dispatcher state at
 *                                       that moment) and the final dispatcher
 *                                       state must be identical
 *   test_rsx_dispatch_table bench       frame-shaped stream, words per second
 *
 * Built by hand, from ps3kit\libs\video:
 *   clang-cl /nologo /O2 -msse4.1 -I. -I..\..\include tests\test_rsx_dispatch_table.c
 *     rsx_dispatch.c /Fe:test_rsx_dispatch_table.exe
 */
#include "../rsx_dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static unsigned long long mix(unsigned long long h, unsigned long long v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 31);
}
static unsigned long long state_hash(const rsx_dispatch* r)
{
    /* the parts a sink reads: registers, program, constants, BEGIN state */
    unsigned long long h = 1;
    const unsigned long long* p = (const unsigned long long*)r->regs;
    for (size_t i = 0; i < sizeof r->regs / 8; i += 7) h = mix(h, p[i]);
    h = mix(h, (unsigned long long)r->in_begin_end);
    h = mix(h, r->current_primitive);
    return h;
}
typedef struct { unsigned long long h; unsigned long long n; } trace;
static void ev(void* u, const rsx_dispatch* r, u32 kind, u32 a, u32 b)
{
    trace* t = (trace*)u;
    t->h = mix(mix(mix(mix(t->h, kind), a), b), state_hash(r));
    t->n++;
}
static void s_clear(void* u, const rsx_dispatch* r, u32 m) { ev(u, r, 1, m, 0); }
static void s_begin(void* u, const rsx_dispatch* r, u32 p) { ev(u, r, 2, p, 0); }
static void s_end(void* u, const rsx_dispatch* r) { ev(u, r, 3, 0, 0); }
static void s_da(void* u, const rsx_dispatch* r, u32 f, u32 c) { ev(u, r, 4, f, c); }
static void s_dia(void* u, const rsx_dispatch* r, u32 f, u32 c) { ev(u, r, 5, f, c); }
static void s_inl(void* u, const rsx_dispatch* r, const u8* d, u32 n)
{
    unsigned long long h = 0;
    for (u32 i = 0; i < n; i++) h = mix(h, d[i]);
    ev(u, r, 6, n, (u32)h);
}
static void s_flip(void* u, const rsx_dispatch* r, u32 a) { ev(u, r, 7, a, 0); }
static void s_imm(void* u, const rsx_dispatch* r) { ev(u, r, 8, 0, 0); }

static u64 rng = 0x2545F4914F6CDD1Dull;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)(rng >> 11); }

typedef struct { u32 m, a; } word;

/* One combat-like draw: state, textures, vertex arrays, constants, the draw. */
static u32 gen_draw(word* w, u32 n)
{
    static const u32 state[] = { 0x0310, 0x0314, 0x0318, 0x031C, 0x0320, 0x0324, 0x0A6C, 0x0A70, 0x0A74,
                                 0x0304, 0x0308, 0x030C, 0x08E4, 0x1D60, 0x1FEC, 0x0360, 0x1830, 0x1834,
                                 0x1D88, 0x0B40 };
    for (u32 k = 0; k < 20; k++) w[n++] = (word){ state[k], rnd() };
    for (u32 u = 0; u < 2; u++)                                   /* two texture units */
        for (u32 k = 0; k < 8; k++) w[n++] = (word){ 0x1A00 + u * 0x20 + k * 4, rnd() };
    for (u32 a = 0; a < 4; a++) w[n++] = (word){ 0x1680 + a * 4, rnd() };   /* vertex arrays */
    for (u32 a = 0; a < 4; a++) w[n++] = (word){ 0x1740 + a * 4, rnd() };
    w[n++] = (word){ 0x1EFC, rnd() % 400 };                       /* constants: load + 24 words */
    for (u32 k = 0; k < 24; k++) w[n++] = (word){ 0x1F00 + (k & 31) * 4, rnd() };
    w[n++] = (word){ 0x1808, 5 };                                 /* BEGIN_END(TRIANGLES) */
    w[n++] = (word){ 0x1824, (rnd() & 0xFFFFFF) | (63u << 24) };   /* DRAW_INDEX_ARRAY */
    w[n++] = (word){ 0x1808, 0 };
    return n;
}

static word* g_words;
static u32 gen_stream(int frame_like)
{
    u32 n = 0;
    if (frame_like) {
        for (u32 d = 0; d < 2500; d++) n = gen_draw(g_words, n);
        return n;
    }
    /* random: any method 0..0xFFFFC (out-of-range ones too), weighted toward the
     * interesting ranges, with BEGIN/END, inline arrays and immediate vertices */
    const u32 len = 1 + rnd() % 20000;
    for (u32 i = 0; i < len; i++) {
        u32 m;
        switch (rnd() % 12) {
        case 0: m = (rnd() % 0x40000u) << 2; break;                     /* anything, out of range too */
        case 1: m = 0x1500 + (rnd() % 0x500) / 4 * 4; break;             /* DATA* folds, arrays */
        case 2: m = 0x1E40 + (rnd() % 0x40) / 4 * 4; break;
        case 3: m = 0x0A80 + (rnd() % 0x80) / 4 * 4; break;
        case 4: m = 0x0B80 + (rnd() % 0x80) / 4 * 4; break;              /* program upload */
        case 5: m = 0x1F00 + (rnd() % 0x80) / 4 * 4; break;              /* constant upload */
        case 6: { static const u32 sp[] = { 0x1D94, 0x1808, 0x1818, 0x1814, 0x1824, 0xE920, 0xE924,
                                            0xE944, 0x1508, 0x1C0C, 0x1E9C, 0x1EFC };
                  m = sp[rnd() % 12]; break; }
        case 7: m = ((rnd() % 8) << 13) | ((rnd() % 0x800) << 2); break; /* any subchannel */
        default: m = (rnd() % 0x800) << 2; break;                        /* subchannel 0 */
        }
        u32 a = rnd();
        if (m == 0x1808) a = (rnd() & 1) ? 0 : 1 + rnd() % 10;
        if (m == 0x1EFC || m == 0x1E9C) a = rnd() % 600;
        g_words[i] = (word){ m, a };
    }
    return len;
}

static void b_cnt2(void* u, const rsx_dispatch* r) { (void)r; ((trace*)u)->n++; }
static void b_cnt3(void* u, const rsx_dispatch* r, u32 a) { (void)r; ((trace*)u)->n += a; }
static void b_cnt4(void* u, const rsx_dispatch* r, u32 a, u32 b) { (void)r; ((trace*)u)->n += a + b; }
static void run(rsx_dispatch* r, const word* w, u32 n)
{
    for (u32 i = 0; i < n; i++) rsx_dispatch_method(r, w[i].m, w[i].a);
}

int main(int argc, char** argv)
{
    g_words = (word*)malloc(sizeof(word) * 400000);
    rsx_dispatch* a = (rsx_dispatch*)calloc(1, sizeof(rsx_dispatch));
    rsx_dispatch* b = (rsx_dispatch*)calloc(1, sizeof(rsx_dispatch));
    trace ta = { 0, 0 }, tb = { 0, 0 };
    rsx_dispatch_sink sk = { 0 };
    sk.clear = s_clear; sk.begin = s_begin; sk.end = s_end; sk.draw_arrays = s_da;
    sk.draw_index_array = s_dia; sk.inline_array = s_inl; sk.flip = s_flip; sk.immediate_vertex = s_imm;

    if (argc > 1 && !strcmp(argv[1], "bench")) {
        /* the draw callbacks cost the same in both modes: count them only */
        sk.clear = (void (*)(void*, const rsx_dispatch*, u32))b_cnt3; sk.begin = (void (*)(void*, const rsx_dispatch*, u32))b_cnt3;
        sk.end = (void (*)(void*, const rsx_dispatch*))b_cnt2; sk.draw_arrays = (void (*)(void*, const rsx_dispatch*, u32, u32))b_cnt4;
        sk.draw_index_array = (void (*)(void*, const rsx_dispatch*, u32, u32))b_cnt4;
        sk.inline_array = NULL; sk.flip = NULL; sk.immediate_vertex = NULL;
        const u32 n = gen_stream(1);
        double best[2] = { 1e30, 1e30 };
        for (int rep = 0; rep < 9; rep++)
            for (int mode = 0; mode < 2; mode++) {
                rsx_dispatch* r = mode ? b : a;
                sk.user = mode ? (void*)&tb : (void*)&ta;
                rsx_dispatch_init(r, &sk);
                rsx_dispatch_set_table(mode);
                LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&t0);
                for (int k = 0; k < 10; k++) run(r, g_words, n);
                QueryPerformanceCounter(&t1);
                const double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)f.QuadPart / 10.0;
                if (us < best[mode]) best[mode] = us;
            }
        printf("rsx_dispatch_method, one combat-shaped frame (%u words, 2500 draws), best of 9 x 10:\n", n);
        printf("  chain (RSX_DSP_TABLE=0) %7.1f us/frame  %5.2f ns/word\n", best[0], best[0] * 1e3 / n);
        printf("  table (RSX_DSP_TABLE=1) %7.1f us/frame  %5.2f ns/word   %.2fx\n", best[1], best[1] * 1e3 / n,
               best[0] / best[1]);
        return 0;
    }

    const int streams = argc > 1 ? atoi(argv[1]) : 400;
    long long words = 0, fails = 0;
    for (int s = 0; s < streams; s++) {
        const u32 n = gen_stream(s % 8 == 0);
        ta.h = tb.h = 0; ta.n = tb.n = 0;
        sk.user = &ta; rsx_dispatch_init(a, &sk);
        sk.user = &tb; rsx_dispatch_init(b, &sk);
        /* the same stream, one word at a time on each, alternating the mode */
        for (u32 i = 0; i < n; i++) {
            rsx_dispatch_set_table(0); rsx_dispatch_method(a, g_words[i].m, g_words[i].a);
            rsx_dispatch_set_table(1); rsx_dispatch_method(b, g_words[i].m, g_words[i].a);
        }
        words += n;
        const int same_state = !memcmp(a->regs, b->regs, sizeof a->regs) && !memcmp(a->vp, b->vp, sizeof a->vp) &&
                               !memcmp(a->constants, b->constants, sizeof a->constants) &&
                               !memcmp(a->seen, b->seen, sizeof a->seen) &&
                               a->in_begin_end == b->in_begin_end && a->current_primitive == b->current_primitive &&
                               a->inline_len == b->inline_len && a->inline_dropped == b->inline_dropped &&
                               !memcmp(a->inline_data, b->inline_data, a->inline_len);
        if (!same_state || ta.h != tb.h || ta.n != tb.n) {
            fails++;
            if (fails < 10) printf("stream %d (%u words): state %s, callbacks %llu/%llu hash %016llX/%016llX\n",
                                   s, n, same_state ? "same" : "DIFFERENT", ta.n, tb.n, ta.h, tb.h);
        }
    }
    printf("%s: %d streams, %lld words, %lld differing\n", fails ? "FAIL" : "PASS", streams, words, fails);
    return fails ? 1 : 0;
}
