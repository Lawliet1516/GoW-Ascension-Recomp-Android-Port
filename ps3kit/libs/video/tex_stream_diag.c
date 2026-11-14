/* Texture streaming checks for God of War: Ascension (TEXTURE-STREAMING helper, 2026-11-04).
 *
 * The game streams its textures into RSX local memory through its own VRAM
 * manager: every texture owns a block descriptor (128 bytes, pool at [0x96E178];
 * +0xC size, +0x1C the block's RSX address 0xC0000000 + offset) and each block
 * has a 16-byte entry in the table at [0x96E174] whose halfword +2 holds the
 * streaming state (0x8000 "streamed, top mips not there", 0x2000 "top mips
 * resident", 0x4000 "wanted", 0x200 drawn this frame). The live blocks are the
 * non-zero words of the slot array 0x96B51C (count at 0x96DDBC). Everything
 * here only READS that state, except TEX_FLAG_FIX (below).
 *
 * TEX_DIAG=1: per-bind checks on the renderer thread, one line every 5 s:
 *   [tex-diag] binds, distinct textures; "overrun" = a local texture whose
 *   mip chain is larger than the block it sits in (a full-size descriptor
 *   over a block that holds only the low mips: drawn as garbage), "stuck" =
 *   overrun for more than 1 s; "no block" = a texture inside the texture heap
 *   with no live block under it (freed memory); "low" = bound while its block
 *   is in the streamed low state (0x8000 without 0x2000), with how many of
 *   those have been low for over 10 s; "zero" = decoded while its first bytes
 *   are all zero (bound before its data came in), "late" = such a texture
 *   that later got real data. The first overruns are printed with details.
 * TEX_FLAGWATCH=1: a scanner thread (every ~1 ms) shadows each live block's
 *   entry flags and reports a block that loses 0x8000 while its size stays
 *   the same ([flagwatch] LOST); blocks that just got 0x8000 are watched with
 *   the CPU's debug registers (4 at a time), so the store that clears it is
 *   caught as it happens: host thread, guest thread name, code address (rva:
 *   look it up in AscensionRecomp.map) and the host call chain.
 *   TEX_FLAGWATCH_MS=<n> scan period (default 1).
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* vm_base;
extern unsigned long long ps3_qpc_us(void);

/* ------------------------------------------------------------------ guest */
static struct { uint32_t slots, count, table, pool, frame, check; } C = {
    0x0096B51Cu, 0x0096DDBCu, 0x0096E174u, 0x0096E178u, 0x00975888u, 0x0036DD1Cu };
static inline int ok_ea(uint32_t a) { return a >= 0x10000u && a < 0x30000000u; }
static inline uint32_t g32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint16_t g16(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t envhex(const char* n, uint32_t d) { const char* e = getenv(n); return e && e[0] ? (uint32_t)strtoul(e, NULL, 0) : d; }
/* The right game: the VRAM manager's allocation function starts with stdu/stwu r1. */
static int game_ok(void)
{
    static int ok = -1;
    if (ok >= 0) return ok;
    if (!vm_base) return 0;
    C.slots = envhex("TEX_DIAG_SLOTS", C.slots); C.count = envhex("TEX_DIAG_COUNT", C.count);
    C.table = envhex("TEX_DIAG_TABLE", C.table); C.pool = envhex("TEX_DIAG_POOL", C.pool);
    C.frame = envhex("TEX_DIAG_FRAME", C.frame);
    const uint32_t w = g32(C.check) >> 16;
    ok = (w == 0xF821u || w == 0x9421u);
    fprintf(stderr, "[tex-diag] VRAM manager tables %s (code at 0x%08X: 0x%04X)\n", ok ? "found" : "NOT found, checks off", C.check, w);
    return ok;
}
static inline uint32_t entry_of(uint32_t blk, uint32_t tb, uint32_t ps) { return tb + (uint32_t)(((int32_t)(blk - ps) / 128) * 16); }

/* ------------------------------------------------------------- block map */
typedef struct { uint32_t off, size, blk; uint16_t flags, pad; } tdblk;
static tdblk s_bm[8192];
static int s_bm_n;
static uint32_t s_heap_lo, s_heap_hi;
static int bm_cmp(const void* a, const void* b)
{
    const uint32_t x = ((const tdblk*)a)->off, y = ((const tdblk*)b)->off;
    return x < y ? -1 : x > y;
}
static void bm_build(void)
{
    uint32_t cnt = g32(C.count);
    if (cnt > 8192u) cnt = 8192u;
    const uint32_t tb = g32(C.table), ps = g32(C.pool);
    int n = 0;
    uint32_t lo = ~0u, hi = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint32_t b = g32(C.slots + i * 4u);
        if (!b || !ok_ea(b)) continue;
        const uint32_t ea = g32(b + 0x1C);
        if (ea < 0xC0000000u || ea >= 0xD0000000u) continue;
        const uint32_t ent = entry_of(b, tb, ps);
        tdblk* d = &s_bm[n++];
        d->off = ea - 0xC0000000u; d->size = g32(b + 0xC); d->blk = b;
        d->flags = ok_ea(ent) ? g16(ent + 2) : 0;
        if (d->off < lo) lo = d->off;
        if (d->off + d->size > hi) hi = d->off + d->size;
    }
    qsort(s_bm, (size_t)n, sizeof s_bm[0], bm_cmp);
    s_bm_n = n; s_heap_lo = n ? lo : 0; s_heap_hi = n ? hi : 0;
}
static const tdblk* bm_find(uint32_t off)
{
    int lo = 0, hi = s_bm_n - 1, k = -1;
    while (lo <= hi) { const int m = (lo + hi) / 2; if (s_bm[m].off <= off) { k = m; lo = m + 1; } else hi = m - 1; }
    /* a zero-size block sorts at the same address as the next one: take the one that holds it */
    while (k > 0 && s_bm[k].off == off && !s_bm[k].size && s_bm[k - 1].off == off) k--;
    if (k < 0 || off >= s_bm[k].off + s_bm[k].size) return NULL;
    return &s_bm[k];
}

/* ----------------------------------------------------------- texture keys */
typedef struct {
    uint64_t key;
    uint32_t win;            /* last 5 s window it was counted in */
    uint32_t low_since_ms;   /* bound low since (0: not low) */
    uint32_t ovr_since_ms;   /* overrun since (0: not) */
    uint32_t zero_ms;        /* decoded as all zero at (0: no) */
    uint32_t nob_since_ms;   /* bound with no block under it since */
    uint8_t  ovr_logged, stuck_counted, low_counted, nob_logged, nob_stuck, pad[3];
} tdkey;
#define TD_KEYS (1u << 15)
static tdkey s_keys[TD_KEYS];
static tdkey* key_get(uint64_t k)
{
    uint32_t i = (uint32_t)(k >> 40) & (TD_KEYS - 1u);
    for (uint32_t n = 0; n < 64; n++, i = (i + 1u) & (TD_KEYS - 1u)) {
        if (s_keys[i].key == k) return &s_keys[i];
        if (!s_keys[i].key) { memset(&s_keys[i], 0, sizeof s_keys[i]); s_keys[i].key = k; return &s_keys[i]; }
    }
    /* full neighbourhood: reuse the home slot */
    i = (uint32_t)(k >> 40) & (TD_KEYS - 1u);
    memset(&s_keys[i], 0, sizeof s_keys[i]); s_keys[i].key = k;
    return &s_keys[i];
}
static uint64_t key_of(uint32_t loc, uint32_t off, uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips)
{
    uint64_t x = 1469598103934665603ull;
    const uint32_t f[6] = { loc, off, fmt, w, h, mips };
    for (int i = 0; i < 6; i++) x = (x ^ f[i]) * 1099511628211ull;
    return x | 1u;
}

/* --------------------------------------------------------------- counters */
static struct {
    long long binds, local, mainmem, distinct, low, low10, ovr, stuck, noblk, zero, late, inside, mism;
    uint32_t oldest_low_ms;
} W;                                  /* this 5 s window */
static struct { long long ovr, stuck, noblk, zero, late, lost, lost_caught, low10, nob_stuck; } T;   /* whole run */
static uint32_t s_win = 1;
static unsigned long long s_win_t0;
static uint32_t s_last_frame = ~0u;
volatile long long g_texdiag_flag_lost, g_texdiag_flag_lost_caught;   /* TEX_FLAGWATCH, for the line */
static int diag_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TEX_DIAG"); on = e && e[0] == '1'; }
    return on;
}
static uint32_t now_ms(void) { const uint32_t t = (uint32_t)(ps3_qpc_us() / 1000ull); return t ? t : 1u; }

static void window_report(void)
{
    fprintf(stderr, "[tex-diag] per 5s: %lld binds (%lld local, %lld main), %lld textures; low-res %lld (%lld low over 10 s, oldest %.1f s); "
                    "overrun %lld (%lld stuck over 1 s); other size mismatches %lld; no block %lld; inside a block %lld; zero-data decodes %lld, late data %lld; "
                    "flag losses %lld (%lld caught) | run: overrun %lld stuck %lld noblock %lld (stuck %lld) zero %lld late %lld low10 %lld%c",
            W.binds, W.local, W.mainmem, W.distinct, W.low, W.low10, W.oldest_low_ms / 1000.0, W.ovr, W.stuck, W.mism, W.noblk, W.inside,
            W.zero, W.late, (long long)g_texdiag_flag_lost, (long long)g_texdiag_flag_lost_caught,
            T.ovr, T.stuck, T.noblk, T.nob_stuck, T.zero, T.late, T.low10, 10);
    memset(&W, 0, sizeof W);
    s_win++;
}

void tex_diag_bind(uint32_t loc, uint32_t off, uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t span, uint32_t frame)
{
    if (!diag_on() || !game_ok()) return;
    const unsigned long long t = ps3_qpc_us();
    if (!s_win_t0) s_win_t0 = t;
    if (t - s_win_t0 >= 5000000ull) { s_win_t0 = t; window_report(); }
    if (frame != s_last_frame) { s_last_frame = frame; bm_build(); }
    W.binds++;
    if (loc != 0) { W.mainmem++; return; }
    W.local++;
    const uint32_t ms = now_ms();
    tdkey* k = key_get(key_of(loc, off, fmt, w, h, mips));
    const int first_in_win = k->win != s_win;
    if (first_in_win) { k->win = s_win; W.distinct++; }
    const tdblk* b = bm_find(off);
    if (!b) {
        if (off >= s_heap_lo && off < s_heap_hi) {
            /* inside the texture heap but on no live block: the block was freed (or moved) and the
             * memory may already hold something else. Counted once a window; "stuck" past 1 s. */
            if (!k->nob_since_ms) k->nob_since_ms = ms;
            if (first_in_win) { W.noblk++; T.noblk++; }
            if (ms - k->nob_since_ms > 1000u && !k->nob_stuck) { k->nob_stuck = 1; T.nob_stuck++; }
            static int logged;
            if (!k->nob_logged && logged < 24) {
                logged++; k->nob_logged = 1;
                /* the nearest blocks below and above */
                const tdblk* lo = NULL; for (int i = 0; i < s_bm_n && s_bm[i].off <= off; i++) lo = &s_bm[i];
                fprintf(stderr, "[tex-diag] NO BLOCK frame %u: texture 0x%08X fmt 0x%02X %ux%u mips %u span 0x%X; block below: 0x%08X at "
                                "0x%08X size 0x%X flags 0x%04X%c", frame, off, fmt & 0xFF, w, h, mips, span, lo ? lo->blk : 0,
                        lo ? lo->off : 0, lo ? lo->size : 0, lo ? lo->flags : 0, 10);
            }
        }
        k->low_since_ms = 0;
        return;
    }
    k->nob_since_ms = 0;
    if (off != b->off && first_in_win) W.inside++;
    const int low = (b->flags & 0x8000u) && !(b->flags & 0x2000u);
    if (low) {
        if (!k->low_since_ms) k->low_since_ms = ms;
        if (first_in_win) {
            W.low++;
            const uint32_t age = ms - k->low_since_ms;
            if (age > W.oldest_low_ms) W.oldest_low_ms = age;
            if (age > 10000u) { W.low10++; if (!k->low_counted) { k->low_counted = 1; T.low10++; } }
        }
    } else k->low_since_ms = 0;
    const uint32_t avail = b->size - (off - b->off);
    /* The streamed-flag bug: a descriptor at the block's own address that is larger than the block (a
     * full-size chain over the low mips). A texture that starts inside a block, or misses by a few bytes
     * (the span rounds the last mips), is counted apart: render targets and textures the walker draws a
     * frame after the manager moved them land there for a moment during loads. */
    if (span > avail + 0x100u && off != b->off) { if (first_in_win) W.mism++; k->ovr_since_ms = 0; return; }
    if (span > avail + 0x100u) {
        if (!k->ovr_since_ms) k->ovr_since_ms = ms;
        if (first_in_win) { W.ovr++; if (!k->ovr_logged) T.ovr++; }
        if (ms - k->ovr_since_ms > 1000u && !k->stuck_counted) { k->stuck_counted = 1; T.stuck++; }
        if (first_in_win && ms - k->ovr_since_ms > 1000u) W.stuck++;
        static int logged;
        if (!k->ovr_logged && logged < 40) {
            logged++;
            fprintf(stderr, "[tex-diag] OVERRUN frame %u: texture 0x%08X fmt 0x%02X %ux%u mips %u needs 0x%X, block 0x%08X at 0x%08X "
                            "size 0x%X (0x%X left) flags 0x%04X%c", frame, off, fmt & 0xFF, w, h, mips, span, b->blk, b->off, b->size,
                    avail, b->flags, 10);
        }
        k->ovr_logged = 1;
    } else k->ovr_since_ms = 0;
}

/* After a decode (refresh = the content of a cached texture changed). */
void tex_diag_decoded(uint32_t loc, uint32_t off, uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t span,
                      const uint8_t* src, int refresh)
{
    if (!diag_on() || !src || span < 64u) return;
    const uint32_t n = span < 65536u ? span : 65536u;
    int zero = 1;
    const uint64_t* q = (const uint64_t*)src;
    for (uint32_t i = 0; i < n / 8u; i++) if (q[i]) { zero = 0; break; }
    tdkey* k = key_get(key_of(loc, off, fmt, w, h, mips));
    if (zero) {
        if (!k->zero_ms) { k->zero_ms = now_ms(); W.zero++; T.zero++; }
    } else if (k->zero_ms) {
        const uint32_t dt = now_ms() - k->zero_ms;
        W.late++; T.late++;
        static int logged;
        if (logged++ < 24)
            fprintf(stderr, "[tex-diag] late data: texture %u:0x%08X fmt 0x%02X %ux%u decoded all-zero, real data %.2f s later (%s)%c",
                    loc, off, fmt & 0xFF, w, h, dt / 1000.0, refresh ? "refresh" : "new entry", 10);
        k->zero_ms = 0;
    }
}

/* ============================================================ flag watch */
#ifdef _WIN32
typedef struct { uint16_t flags; uint16_t seen; uint32_t size; uint32_t scan; uint32_t set_ms; } fwshadow;
static fwshadow s_fw[32768];
static uint32_t s_fw_scan;
typedef struct { uint32_t ea; uint32_t blk; uint32_t armed_ms; } fwslot;
static fwslot s_dr[4];
static volatile LONG s_dr_gen;          /* bumped when s_dr changes: re-arm */
static int s_dr_on;
static DWORD s_scanner_tid;
/* debug-register hits, filled by the VEH, printed by the scanner */
typedef struct { uint32_t ea, blk; uint16_t before, after; DWORD tid; uint64_t rip; uint64_t chain[10]; int nchain; unsigned long long t; } fwhit;
static fwhit s_hits[1024];
static volatile LONG s_hit_w, s_hit_r;
static uint64_t s_image_base;

static const char* guest_thread_name(DWORD tid, unsigned* gtid)
{
    /* runtime/syscalls/sys_ppu_thread.c: g_ppu_threads[] (slot 0 = main, host tid not stored) */
    extern DWORD host_ppu_main_thread_id(void);
    if (tid == host_ppu_main_thread_id()) { *gtid = 1; return "main"; }
    extern const char* ppu_thread_name_of_host_tid(DWORD tid, unsigned* gtid);
    const char* n = ppu_thread_name_of_host_tid(tid, gtid);
    return n ? n : "(not a PPU thread)";
}

static LONG CALLBACK fw_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    const DWORD64 hit = c->Dr6 & 0xF;
    /* our debug registers are the only source of single steps in this process: never let one through */
    if (!hit) { c->Dr6 = 0; return EXCEPTION_CONTINUE_EXECUTION; }
    for (int k = 0; k < 4; k++) {
        if (!(hit & (1ull << k))) continue;
        const uint32_t ea = s_dr[k].ea;
        if (!ea) continue;
        const LONG w = InterlockedIncrement(&s_hit_w) - 1;
        if (w - s_hit_r >= 1024) continue;   /* ring full: drop */
        fwhit* h = &s_hits[w & 1023];
        h->ea = ea; h->blk = s_dr[k].blk; h->after = g16(ea);
        h->before = s_fw[((s_dr[k].blk - g32(C.pool)) >> 7) & 32767u].flags;
        h->tid = GetCurrentThreadId(); h->rip = c->Rip; h->t = ps3_qpc_us();
        /* host call chain from the faulting context */
        CONTEXT u = *c; int n = 0;
        for (; n < 10; n++) {
            DWORD64 ib = 0; PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(u.Rip, &ib, NULL);
            if (!f) break;
            void* hd = NULL; DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, u.Rip, f, &u, &hd, &ef, NULL);
            if (!u.Rip) break;
            h->chain[n] = u.Rip;
        }
        h->nchain = n;
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Put s_dr[] into every thread's debug registers (but this one). */
static void fw_arm_all(void)
{
    DWORD64 dr[4] = { 0, 0, 0, 0 }, dr7 = 0;
    for (int k = 0; k < 4; k++) {
        if (!s_dr[k].ea) continue;
        dr[k] = (DWORD64)(vm_base + s_dr[k].ea);
        dr7 |= (1ull << (2 * k)) | (1ull << (16 + 4 * k)) /* RW=01 write */ | (1ull << (18 + 4 * k)) /* LEN=01 2 bytes */;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == s_scanner_tid) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                c.Dr0 = dr[0]; c.Dr1 = dr[1]; c.Dr2 = dr[2]; c.Dr3 = dr[3]; c.Dr7 = dr7; c.Dr6 = 0;
                SetThreadContext(th, &c);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}

static void fw_print_hits(void)
{
    static long printed;
    while (s_hit_r != s_hit_w) {
        fwhit* h = &s_hits[s_hit_r & 1023];
        const int lost = (h->before & 0x8000u) && !(h->after & 0x8000u);
        if (lost) InterlockedIncrement64(&g_texdiag_flag_lost_caught);
        if (lost || printed < 200) {
            printed++;
            unsigned gt = 0;
            const char* nm = guest_thread_name(h->tid, &gt);
            fprintf(stderr, "[flagwatch] %s write: block 0x%08X entry flags 0x%04X -> 0x%04X by host tid %lu (guest %u \"%s\") rip rva 0x%llX chain",
                    lost ? "LOSS" : "watched", h->blk, h->before, h->after, h->tid, gt, nm, (unsigned long long)(h->rip - s_image_base));
            for (int i = 0; i < h->nchain; i++) fprintf(stderr, " 0x%llX", (unsigned long long)(h->chain[i] - s_image_base));
            fprintf(stderr, " t=%llu%c", h->t, 10);
        }
        s_hit_r++;
    }
}

/* Texture census (with TEX_FLAGWATCH, every 250 ms): the manager's texture
 * lists as func_00372B60 walks them (manager = [0x939BEC + 0x1C]; group list
 * at +0x1C, textures at group+0x44 - 0xC ... linked at +8). A streamable texture
 * (+0x12 bit 0) whose block is the reduced one (block size + cut <= full) must
 * have 0x8000 and not 0x2000 in its entry; without 0x8000 the game never
 * streams its top mips in (it stays low-res) and the next descriptor written
 * for it describes the full size over the low mips (garbage) unless
 * PPU_VRAM_DESC repairs it. Counted as "lost". */
static struct { int stream, low, lost, top, full; } s_cen, s_cen_max;
/* SPU_WTRACE=<table>-<table + 0x20000> (runtime/spu/spu_wtrace.c) records every PPU store and SPU DMA
 * into the flag table: at a loss, who wrote that entry's flags lately. */
static void wt_dump(uint32_t ea, const char* tag)
{
    extern uint32_t g_wtrace_span;
    extern void spu_wtrace_dump(uint32_t ea, uint32_t n, unsigned max, const char* tag);
    if (g_wtrace_span) spu_wtrace_dump(ea, 2, 16, tag);
}
static uint32_t s_lost_seen[256]; static int s_lost_n;
static uint32_t s_lprev[1024], s_lcur[1024]; static int s_lprev_n, s_lcur_n;
static void fw_census(void)
{
    const uint32_t mgr = g32(0x00939808u);   /* func_00366E18: r3 = [0x9397EC + 0x1C] for func_00372B60 */
    if (!ok_ea(mgr)) return;
    s_lcur_n = 0;
    const uint32_t tb = g32(C.table), ps = g32(C.pool);
    memset(&s_cen, 0, sizeof s_cen);
    uint32_t g = g32(mgr + 0x1C);
    for (int gn = 0; g && ok_ea(g) && gn < 4096; gn++) {
        uint32_t t = (g != 8u) ? g32(g - 0xC + 0x44) : 0;
        for (int tn = 0; t && ok_ea(t) && tn < 65536; tn++) {
            const uint32_t tex = t - 8, blk = g32(tex + 0x44);
            if (blk && ok_ea(blk) && (g16(tex + 0x12) & 1u)) {
                const uint32_t sz = g32(blk + 0xC), cut = g32(tex + 0x1C), full = g32(tex + 0x20);
                const uint32_t ent = entry_of(blk, tb, ps);
                const uint16_t f = ok_ea(ent) ? g16(ent + 2) : 0;
                s_cen.stream++;
                if (cut && (uint64_t)sz + cut <= full) {
                    if (!(f & 0x8000u)) {
                        /* only once it was so at the last census too (not the moment of its allocation) */
                        if (s_lcur_n < 1024) s_lcur[s_lcur_n++] = tex;
                        int before = 0;
                        for (int k = 0; k < s_lprev_n; k++) if (s_lprev[k] == tex) { before = 1; break; }
                        if (!before) goto next_tex;
                        s_cen.lost++;
                        int seen = 0;
                        for (int k = 0; k < s_lost_n; k++) if (s_lost_seen[k] == tex) { seen = 1; break; }
                        if (!seen && s_lost_n < 256) {
                            s_lost_seen[s_lost_n++] = tex;
                            if (s_lost_n <= 48)
                                {
                                fprintf(stderr, "[census] lost streamed flag: texture 0x%08X block 0x%08X 0x%X of 0x%X, flags 0x%04X, frame %u%c",
                                        tex, blk, sz, full, f, g32(C.frame), 10);
                                if (s_lost_n <= 12) wt_dump(ent + 2, "census-lost");
                            }
                        }
                    } else if (f & 0x2000u) s_cen.top++;
                    else s_cen.low++;
                } else s_cen.full++;
            }
        next_tex:
            t = g32(t);
        }
        g = g32(g);
    }
    memcpy(s_lprev, s_lcur, sizeof(uint32_t) * (size_t)s_lcur_n); s_lprev_n = s_lcur_n;
    if (s_cen.lost > s_cen_max.lost) s_cen_max.lost = s_cen.lost;
    if (s_cen.low > s_cen_max.low) s_cen_max.low = s_cen.low;
}
volatile long long g_texdiag_census_lost_now, g_texdiag_census_low_now;

static DWORD WINAPI fw_thread(LPVOID arg)
{
    (void)arg;
    s_scanner_tid = GetCurrentThreadId();
    const char* e = getenv("TEX_FLAGWATCH_MS");
    const int period = e ? atoi(e) : 1;
    while (!vm_base || !game_ok()) Sleep(200);
    /* wait for the manager to exist */
    while (!g32(C.table) || !g32(C.pool)) Sleep(50);
    fprintf(stderr, "[flagwatch] scanning the VRAM manager's block flags every %d ms (debug-register watch %s); "
                    "table 0x%08X pool 0x%08X slots %u (SPU_WTRACE=%08X-%08X covers 8192 entries)\n",
            period, s_dr_on ? "on" : "off", g32(C.table), g32(C.pool), g32(C.count), g32(C.table), g32(C.table) + 0x20000u);
    LONG armed_gen = -1;
    unsigned long long t_arm = 0;
    int dr_next = 0;
    for (;;) {
        if (period > 0) Sleep((DWORD)period); else SwitchToThread();
        s_fw_scan++;
        uint32_t cnt = g32(C.count);
        if (cnt > 8192u) cnt = 8192u;
        const uint32_t tb = g32(C.table), ps = g32(C.pool);
        const uint32_t ms = now_ms();
        for (uint32_t i = 0; i < cnt; i++) {
            const uint32_t b = g32(C.slots + i * 4u);
            if (!b || !ok_ea(b) || b < ps) continue;
            const uint32_t idx = (b - ps) >> 7;
            if (idx >= 32768u) continue;
            const uint32_t ent = entry_of(b, tb, ps);
            if (!ok_ea(ent)) continue;
            const uint16_t f = g16(ent + 2);
            const uint32_t sz = g32(b + 0xC);
            fwshadow* s = &s_fw[idx];
            const int cont = s->seen && s->scan == s_fw_scan - 1u;   /* live in the last scan too */
            if (cont && (s->flags & 0x8000u) && !(f & 0x8000u) && sz == s->size) {
                const long long n = InterlockedIncrement64(&g_texdiag_flag_lost);
                if (n <= 64) {
                    fprintf(stderr, "[flagwatch] LOST: block 0x%08X (slot %u) size 0x%X flags 0x%04X -> 0x%04X, %.1f ms after it got 0x8000, frame %u%c",
                            b, i, sz, s->flags, f, (double)(ms - s->set_ms), g32(C.frame), 10);
                    if (n <= 12) wt_dump(ent + 2, "flag-lost");
                }
            }
            if ((!cont || !(s->flags & 0x8000u)) && (f & 0x8000u)) {
                s->set_ms = ms;
                /* watch it: the oldest of the four slots goes */
                s_dr[dr_next].ea = ent + 2; s_dr[dr_next].blk = b; s_dr[dr_next].armed_ms = ms;
                dr_next = (dr_next + 1) & 3;
                InterlockedIncrement(&s_dr_gen);
            }
            s->flags = f; s->size = sz; s->seen = 1; s->scan = s_fw_scan;
        }
        const unsigned long long t = ps3_qpc_us();
        { static unsigned long long t_cen, t_rep;
          if (t - t_cen >= 250000ull) {
              t_cen = t;
              fw_census();
              g_texdiag_census_lost_now = s_cen.lost; g_texdiag_census_low_now = s_cen.low;
          }
          if (t - t_rep >= 5000000ull) {
              t_rep = t;
              fprintf(stderr, "[census] streamable textures %d: low-res %d, top mips arriving %d, full %d, LOST FLAG %d "
                              "(max low %d, max lost %d, distinct lost %d)%c", s_cen.stream, s_cen.low, s_cen.top, s_cen.full,
                      s_cen.lost, s_cen_max.low, s_cen_max.lost, s_lost_n, 10);
          } }
        /* re-arm on a change (at most every 2 ms) and every 500 ms for new threads */
        if (s_dr_on && ((s_dr_gen != armed_gen && t - t_arm >= 2000ull) || t - t_arm >= 500000ull)) {
            armed_gen = s_dr_gen; t_arm = t;
            fw_arm_all();
        }
        fw_print_hits();
    }
    return 0;
}

/* TEX_FLAG_MERGE=1 (the fix, default; =2 count only, =0 off): runtime/spu/spu_channels.c SPU_PUT_MERGE on the
 * VRAM manager's flag table ([0x96E174], 0xA280 bytes = 0xA28 entries, func_0037014C allocates it), so
 * the SPU job that PUTs entries back can no longer write over a 0x8000 the PPU set meanwhile. A thread
 * points the window at the table once it exists (and again if it moves) and reports every 5 s. */
static DWORD WINAPI merge_thread(LPVOID arg)
{
    const int mode = (int)(intptr_t)arg;
    extern uint32_t g_spu_merge_lo, g_spu_merge_span;
    extern int g_spu_merge_mode;
    extern volatile long long g_spu_merge_puts, g_spu_merge_words_saved, g_spu_merge_flag8000_saved, g_spu_merge_unmatched;
    extern volatile long long g_spu_merge_win_sum, g_spu_merge_win_n, g_spu_merge_win_max;
    while (!vm_base || !game_ok()) Sleep(100);
    long long p0 = 0, w0 = 0, f0 = 0, u0 = 0;
    unsigned long long t_rep = ps3_qpc_us();
    for (;;) {
        const uint32_t tb = g32(C.table);
        if (tb && ok_ea(tb) && tb != g_spu_merge_lo) {
            g_spu_merge_mode = mode;
            g_spu_merge_span = 0;
            MemoryBarrier();
            g_spu_merge_lo = tb;
            MemoryBarrier();
            g_spu_merge_span = 0xA280u;
            fprintf(stderr, "[flag-merge] SPU PUTs into the VRAM flag table 0x%08X-0x%08X %s%c", tb, tb + 0xA280u,
                    mode == 1 ? "write only the bits the SPU changed (TEX_FLAG_MERGE=1)" : "are only counted (TEX_FLAG_MERGE=2)", 10);
        }
        Sleep(250);
        const unsigned long long t = ps3_qpc_us();
        if (t - t_rep >= 5000000ull) {
            t_rep = t;
            const long long p = g_spu_merge_puts, w = g_spu_merge_words_saved, f = g_spu_merge_flag8000_saved, u = g_spu_merge_unmatched;
            const long long ws = InterlockedExchange64(&g_spu_merge_win_sum, 0), wn = InterlockedExchange64(&g_spu_merge_win_n, 0),
                            wm = InterlockedExchange64(&g_spu_merge_win_max, 0);
            /* a line when it kept (or, counting, saw) a PPU store, else one a minute */
            static int quiet;
            if (w != w0 || ++quiet >= 12) { quiet = 0;
            if (p != p0 || u != u0)
                fprintf(stderr, "[flag-merge] per 5s: %lld PUTs into the table matched to their GET, %lld not; %lld words where a PPU "
                                "store %s, %lld of them a 0x8000 (run: %lld / %lld); the SPU held the table %.1f ms on average, %lld ms max%c",
                        p - p0, u - u0, w - w0, mode == 1 ? "was kept" : "was written over", f - f0, w, f,
                        wn ? (double)ws / (double)wn : 0.0, wm, 10);
            }
            p0 = p; w0 = w; f0 = f; u0 = u;
        }
    }
    return 0;
}

void tex_flagwatch_start(void)
{
    static LONG started;
    if (InterlockedExchange(&started, 1)) return;
    { const char* m = getenv("TEX_FLAG_MERGE");
      const int mode = m ? atoi(m) : 1;   /* default on (the fix); 0 off, 2 count only */
      if (mode == 1 || mode == 2) { HANDLE h = CreateThread(NULL, 0, merge_thread, (LPVOID)(intptr_t)mode, 0, NULL); if (h) CloseHandle(h); } }
    const char* e = getenv("TEX_FLAGWATCH");
    if (!(e && e[0] == '1')) return;
    s_image_base = (uint64_t)GetModuleHandleA(NULL);
    /* TEX_FLAGWATCH_DR=1 (risky, diagnostic): the debug-register watch; a single step the VEH did not
     * take (seen at exit in run x4open1) ends the game, so it is off unless asked for. */
    { const char* d = getenv("TEX_FLAGWATCH_DR"); s_dr_on = d && d[0] == '1'; }
    if (s_dr_on) AddVectoredExceptionHandler(1, fw_veh);
    HANDLE h = CreateThread(NULL, 0, fw_thread, NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_HIGHEST); CloseHandle(h); }
}
#else
void tex_flagwatch_start(void) {}
#endif
