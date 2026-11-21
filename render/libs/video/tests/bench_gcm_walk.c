/*
 * The FIFO walker's ordinary-word loop, old (per word: IO-table lookup, lean
 * tests, read) against GCM_FIFO_BATCH (per packet), on a combat-frame-shaped
 * command buffer, each word going on to rsx_dispatch_method through an
 * out-of-line call the way rsx_live_draw_method hands it on. The two loops
 * are copies of cellGcmSys.c's (the walker itself needs the whole runtime);
 * keep them in step if that code changes.
 *
 *   bench_gcm_walk          old and new loops x RSX_DSP_TABLE 0/1, and a
 *                           check that both loops deliver the same
 *                           (method, argument, address) sequence
 *
 * From ps3kit\libs\video:
 *   clang-cl /nologo /O2 -msse4.1 -I. -I..\..\include tests\bench_gcm_walk.c rsx_dispatch.c
 */
#include "../rsx_dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static u8* vm_base;
static u16 s_ea_address_table[65536];
static inline u32 gcm_io2ea(u32 io)
{
    u32 page = io >> 20;
    if (page >= 65536) return 0;
    u16 ea_page = s_ea_address_table[page];
    if (ea_page == 0xFFFF) return 0;
    return ((u32)ea_page << 20) | (io & 0xFFFFFu);
}
static inline u32 vm_read32(u32 a) { u32 r; memcpy(&r, vm_base + a, 4); return __builtin_bswap32(r); }

static rsx_dispatch* g_rsx;
u32 g_gcm_method_ea;
static unsigned long long g_h;
static int g_hash;
/* stands in for rsx_live_draw_method: out of line, on to the dispatcher */
__declspec(noinline) void live_method(u32 m, u32 a)
{
    if (g_hash) g_h = (g_h ^ m ^ ((unsigned long long)a << 32) ^ ((unsigned long long)g_gcm_method_ea << 17)) * 0x100000001B3ull;
    rsx_dispatch_method(g_rsx, m, a);
}

static int gcm_walk_special(u32 m)
{
    return m == 0x50u || m == 0x1A8u || m == 0x17C8u || m == 0x1800u || m == 0x1D70u || m == 0x1D74u;
}
static int gcm_packet_has_special(u32 type, u32 method, u32 count)
{
    static const u32 sp[6] = { 0x50u, 0x1A8u, 0x17C8u, 0x1800u, 0x1D70u, 0x1D74u };
    if (type != 0) return gcm_walk_special(method);
    const u32 last = method + (count - 1u) * 4u;
    for (int k = 0; k < 6; k++)
        if (sp[k] >= method && sp[k] <= last) return 1;
    return 0;
}

/* walk [get, put) of method packets only (subchannel 0, lean words) */
static void walk(u32 get, u32 put, int batch)
{
    const int s_walk_lean = 1, s_walk_stateless = 1;
    while (get != put) {
        const u32 ea = gcm_io2ea(get);
        const u32 w = vm_read32(ea);
        const u32 type = w >> 29;
        const u32 count = (w >> 18) & 0x7FF, method = w & 0x1FFCu, subch = (w >> 13) & 7;
        if (batch && s_walk_lean && subch == 0 && count >= (u32)batch) {
            const u32 io0 = get + 4u, io1 = io0 + (count - 1u) * 4u;
            const u32 dea0 = (io0 >> 20) == (io1 >> 20) &&
                             !gcm_packet_has_special(type, method, count) ? gcm_io2ea(io0) : 0u;
            if (dea0) {
                const u32 step = type == 0 ? 4u : 0u;
                u32 m = method;
                for (u32 i = 0; i < count; i++, m += step) {
                    const u32 dea = dea0 + i * 4u;
                    const u32 arg = vm_read32(dea);
                    g_gcm_method_ea = dea;
                    live_method(m, arg);
                    if (!s_walk_stateless || m == 0x1D6Cu) { /* rsx_process_method */ }
                }
                get += 4 + count * 4;
                continue;
            }
        }
        for (u32 i = 0; i < count; i++) {
            u32 dea = gcm_io2ea(get + 4 + i * 4);
            if (!dea) break;
            u32 m = (type == 0) ? method + i * 4 : method;
            if (s_walk_lean && subch == 0 && m != 0x50u && m != 0x1A8u && m != 0x17C8u &&
                m != 0x1800u && m != 0x1D70u && m != 0x1D74u) {
                const u32 arg = vm_read32(dea);
                g_gcm_method_ea = dea;
                live_method(m, arg);
                if (!s_walk_stateless || m == 0x1D6Cu) { /* rsx_process_method */ }
                continue;
            }
        }
        get += 4 + count * 4;
    }
}

static u64 rng = 0x2545F4914F6CDD1Dull;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)(rng >> 11); }
static u8* g_fifo; static u32 g_put;
static void put_word(u32 w) { u32 b = __builtin_bswap32(w); memcpy(g_fifo + g_put, &b, 4); g_put += 4; }
static void packet(u32 method, u32 count, const u32* args, int ni)
{
    put_word((ni ? 0x40000000u : 0u) | (count << 18) | method);
    for (u32 i = 0; i < count; i++) put_word(args[i]);
}
/* A combat-like draw as libgcm packs it: state words one or a few per packet,
 * a texture unit as one 8-word packet, vertex arrays as runs, the constants as
 * LOAD + 24 words in one packet, then the draw. */
static void gen_draw(void)
{
    u32 a[32];
    static const u32 state[] = { 0x0310, 0x0A6C, 0x0304, 0x08E4, 0x1D60, 0x1FEC, 0x0360, 0x1830, 0x1D88, 0x0B40 };
    for (u32 k = 0; k < 10; k++) { a[0] = rnd(); a[1] = rnd(); packet(state[k], 1 + (k & 1), a, 0); }
    for (u32 u = 0; u < 2; u++) { for (u32 k = 0; k < 8; k++) a[k] = rnd(); packet(0x1A00 + u * 0x20, 8, a, 0); }
    for (u32 k = 0; k < 4; k++) a[k] = rnd();
    packet(0x1680, 4, a, 0);
    for (u32 k = 0; k < 4; k++) a[k] = rnd();
    packet(0x1740, 4, a, 0);
    a[0] = rnd() % 400; for (u32 k = 1; k < 25; k++) a[k] = rnd();
    packet(0x1EFC, 25, a, 0);
    a[0] = 5; packet(0x1808, 1, a, 0);
    a[0] = (rnd() & 0xFFFFFF) | (63u << 24); packet(0x1824, 1, a, 0);
    a[0] = 0; packet(0x1808, 1, a, 0);
}

int main(void)
{
    vm_base = (u8*)VirtualAlloc(NULL, 64u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memset(s_ea_address_table, 0xFF, sizeof s_ea_address_table);
    for (u32 p = 0; p < 16; p++) s_ea_address_table[p] = (u16)(32 + p);   /* IO 0..16 MB -> EA 32 MB.. */
    g_fifo = vm_base + (32u << 20);
    g_put = 0x0FFF00;              /* start just below a 1 MB boundary: packets straddle it */
    for (u32 d = 0; d < 2500; d++) gen_draw();
    const u32 get0 = 0x0FFF00, put = g_put;
    g_rsx = (rsx_dispatch*)calloc(1, sizeof(rsx_dispatch));
    rsx_dispatch_init(g_rsx, NULL);

    /* same sequence both ways */
    unsigned long long h[2];
    g_hash = 1;
    for (int b = 0; b < 2; b++) { g_h = 0; rsx_dispatch_set_table(1); walk(get0, put, b); h[b] = g_h; }
    g_hash = 0;
    printf("FIFO of %u bytes (%u words incl. headers), 2500 draws; method/argument/address hash old %016llX new %016llX: %s\n",
           put - get0, (put - get0) / 4, h[0], h[1], h[0] == h[1] ? "same" : "DIFFERENT");
    double best[2][2] = { { 1e30, 1e30 }, { 1e30, 1e30 } };
    const char* mb = getenv("BATCH_MIN");
    const int bmin = mb ? atoi(mb) : 4;   /* cellGcmSys.c batches packets of 4+ words */
    for (int rep = 0; rep < 9; rep++)
        for (int t = 0; t < 2; t++)
            for (int b = 0; b < 2; b++) {
                rsx_dispatch_set_table(t);
                LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&t0);
                for (int k = 0; k < 10; k++) walk(get0, put, b ? bmin : 0);
                QueryPerformanceCounter(&t1);
                const double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)f.QuadPart / 10.0;
                if (us < best[t][b]) best[t][b] = us;
            }
    printf("walk + dispatch of one frame (best of 9 x 10):\n");
    printf("  per-word loop, chain dispatch (before):          %7.1f us\n", best[0][0]);
    printf("  per-word loop, table dispatch (RSX_DSP_TABLE):   %7.1f us\n", best[1][0]);
    printf("  batched loop,  chain dispatch (GCM_FIFO_BATCH):  %7.1f us\n", best[0][1]);
    printf("  batched loop,  table dispatch (both, default):   %7.1f us   %.2fx\n", best[1][1], best[0][0] / best[1][1]);
    return h[0] == h[1] ? 0 : 1;
}
