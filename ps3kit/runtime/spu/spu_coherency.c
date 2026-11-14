/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store.
 */

#include <stdlib.h>
#include "spu_coherency.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>   /* memcpy (spu_coh_store_fast) */

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * spinlock, same semantics, now shared. _InterlockedExchange is a clang-cl/MSVC
 * intrinsic (no runtime library symbol needed); elsewhere use the C11
 * equivalent, which lowers to the same LL/SC or lock-xchg on every target.
 * ===========================================================================*/
/* Hot shared state goes on cache lines of its own. A store, even of the value
 * already there, takes a line away from every other core holding it; every
 * guest store on every PPU thread reads g_spu_coh_armed and the bitmap, and
 * every SPU atomic takes the lock. */
#if defined(_MSC_VER)
#  define COH_ALIGN64 __declspec(align(64))
#else
#  define COH_ALIGN64 __attribute__((aligned(64)))
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
static COH_ALIGN64 struct { volatile long v; volatile uint32_t holder; char pad[64 - sizeof(long) - 4]; } s_lockline;
#define g_lockline s_lockline.v
/* SPU_LL_STATS=1 (diagnostic, helper #11): per call site and thread, lock takes,
 * contended takes, cycles to acquire and held, and for callers that name their
 * line (g_spu_ll_hint = line | site code, set only while stats are on) whether
 * a contended take waited on a holder of the SAME line or ANOTHER one -- the
 * part a lock per line could remove. A thread prints 5 s deltas ([llstat]). */
int g_spu_ll_stats = -1;
__declspec(thread) uint32_t g_spu_ll_hint;
static void ll_stat_lock(uintptr_t ra);
static void ll_stat_unlock(void);
static int ll_stats_init(void)
{
    const char* e = getenv("SPU_LL_STATS");
    g_spu_ll_stats = (e && e[0] == '1') ? 1 : 0;
    return g_spu_ll_stats;
}
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
static inline void ll_g_lock(uintptr_t ra)   /* the one lock (SPU_LL_STRIPE=0: as before) */
{
    if (__builtin_expect(g_spu_ll_stats != 0, 0) && (g_spu_ll_stats > 0 || ll_stats_init())) {
        ll_stat_lock(ra); return;
    }
    unsigned spins = 0;
    while (_InterlockedExchange(&g_lockline, 1)) {
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (g_lockline);
    }
}
static inline void ll_g_unlock(void)
{
    if (__builtin_expect(g_spu_ll_stats > 0, 0)) ll_stat_unlock();
    _InterlockedExchange(&g_lockline, 0);
}

/* ===========================================================================
 * SPU_LL_STRIPE=1 (helper #11): the lock-line lock as 64 stripes by line.
 *
 * One process-wide lock made every lock-line transaction wait for every other
 * one, related or not, and moved its cache line between all six SPU threads and
 * the PPU on every take. A stripe is a TTAS spinlock like the one lock, on a
 * 128-byte slot of its own. What a stripe protects (scratchpad ll/INVARIANTS.md):
 *  - the bytes of every line that maps to it;
 *  - the SPU_COH_LINEIDX chain of every slot that maps to it, and the `line`
 *    field of every row on those chains (a line's stripe is a function of its
 *    chain slot: ll_stripe_l7 = slot >> 4);
 *  - the reservation (resv_ea/resv_valid, and the LR bit as notify raises it)
 *    of every context whose row is on a line of that stripe. A context's row
 *    takes the line of its every GETLLAR in the same hold, so a context with a
 *    valid reservation on L has its row on L.
 * The registry (the list, the hash, the rows array, its size and order) changes
 * only with EVERY stripe held, so holding any one stripe makes it safe to read.
 * Several stripes are always taken in ascending order. Not with SPU_COH_DENSE=0
 * (notify walks every context) or SPU_COH_LIVE=1 (its byte map and lock-free
 * stores assume the one lock): one lock there, as before.
 *
 *   spu_lockline_lock()            every stripe (the sites that write several
 *                                  lines, or were not audited)
 *   spu_lockline_lock_ea(ea)       ea's line (PUTLLC, PUTLLUC, PPU stwcx.)
 *   spu_lockline_lock_range(ea,n)  every line of [ea, ea+n) (DMA PUT, SDCRZ,
 *                                  block and crossing stores)
 *   spu_coh_getllar_lock()         GETLLAR: the new line's stripe and, when the
 *                                  context's row leaves a line of another
 *                                  stripe, that one too
 *
 * Mode switch (SPU_LL_AB): spu_ll_set_mode holds the one lock and every stripe;
 * every taker re-reads the mode once it holds its lock(s) and starts again on a
 * change, so a critical section runs entirely in one mode. Holding any lock
 * pins the mode, which is why an unlock may recompute what it releases.
 * ===========================================================================*/
#ifndef SPU_LL_STRIPE_DEFAULT
#define SPU_LL_STRIPE_DEFAULT 0
#endif
#define LL_NS 64u
static __declspec(align(128)) struct { volatile long v; char pad[128 - sizeof(long)]; } s_ll_stripe[LL_NS];
volatile int g_spu_ll_mode = -1;   /* -1 not read yet, 0 the one lock, 1 stripes */
static int spu_coh_dense_on(void);
int spu_coh_live_on(void);
static int ll_mode_allowed(void)
{
    if (g_spu_ll_stats < 0) ll_stats_init();
    return spu_coh_dense_on() && !spu_coh_live_on() && g_spu_ll_stats == 0;
}
static int ll_mode_init(void)
{
    const char* e = getenv("SPU_LL_STRIPE");
    int m = e ? (e[0] == '1') : SPU_LL_STRIPE_DEFAULT;
    if (m && !ll_mode_allowed()) m = 0;
    if (_InterlockedCompareExchange((volatile long*)&g_spu_ll_mode, m, -1) == -1)
        fprintf(stderr, "[ll] lock-line lock: %s\n", m ? "64 stripes (SPU_LL_STRIPE=1)" : "one lock");
    return g_spu_ll_mode;
}
static inline int ll_mode(void) { const int m = g_spu_ll_mode; return m >= 0 ? m : ll_mode_init(); }
static inline unsigned ll_stripe_l7(uint32_t l7) { return (l7 * 2654435761u) >> 26; }
static inline void ll_s_lock(unsigned i)
{
    volatile long* l = &s_ll_stripe[i].v;
    unsigned spins = 0;
    while (_InterlockedExchange(l, 1)) {
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (*l);
    }
}
static inline int ll_s_trylock(unsigned i) { return !s_ll_stripe[i].v && !_InterlockedExchange(&s_ll_stripe[i].v, 1); }
static inline void ll_s_unlock(unsigned i) { __atomic_store_n(&s_ll_stripe[i].v, 0, __ATOMIC_RELEASE); }
static inline void ll_set_lock(uint64_t set)   { for (; set; set &= set - 1) ll_s_lock((unsigned)__builtin_ctzll(set)); }
static inline void ll_set_unlock(uint64_t set) { for (; set; set &= set - 1) ll_s_unlock((unsigned)__builtin_ctzll(set)); }
/* The stripes of every line of [ea, ea + n), n >= 1; all of them for a span
 * that wraps past 4 GB or has 64 lines or more. */
static inline uint64_t ll_range_set(uint32_t ea, uint32_t n)
{
    const uint32_t l0 = ea >> 7, l1 = (uint32_t)(ea + (n ? n - 1u : 0u)) >> 7;
    if (l1 == l0) return 1ull << ll_stripe_l7(l0);
    if (l1 < l0 || l1 - l0 >= LL_NS) return ~0ull;
    uint64_t s = 0;
    for (uint32_t l = l0; ; l++) { s |= 1ull << ll_stripe_l7(l); if (l == l1) break; }
    return s;
}
/* Take [ea, ea+n)'s stripes (all = every stripe) in the current mode; returns
 * what is held: 0 = the one lock, else the stripe set. */
static uint64_t ll_acquire(uint32_t ea, uint32_t n, int all, uintptr_t ra)
{
    for (;;) {
        if (ll_mode() == 0) {
            ll_g_lock(ra);
            if (g_spu_ll_mode == 0) return 0;
            ll_g_unlock();
            continue;
        }
        const uint64_t set = all ? ~0ull : ll_range_set(ea, n);
        ll_set_lock(set);
        if (g_spu_ll_mode == 1) return set;
        ll_set_unlock(set);
    }
}
static inline void ll_release(uint64_t held) { if (!held) ll_g_unlock(); else ll_set_unlock(held); }

void spu_lockline_lock(void)   { (void)ll_acquire(0, 1, 1, (uintptr_t)_ReturnAddress()); }
void spu_lockline_unlock(void) { ll_release(g_spu_ll_mode == 1 ? ~0ull : 0); }
void spu_lockline_lock_ea(uint32_t ea)   { (void)ll_acquire(ea, 1, 0, (uintptr_t)_ReturnAddress()); }
void spu_lockline_unlock_ea(uint32_t ea) { ll_release(g_spu_ll_mode == 1 ? ll_range_set(ea, 1) : 0); }
void spu_lockline_lock_range(uint32_t ea, uint32_t n)   { (void)ll_acquire(ea, n, 0, (uintptr_t)_ReturnAddress()); }
void spu_lockline_unlock_range(uint32_t ea, uint32_t n) { ll_release(g_spu_ll_mode == 1 ? ll_range_set(ea, n) : 0); }
void spu_coh_unlock_tok(uint64_t held) { ll_release(held); }
int  spu_ll_striped(void) { return ll_mode(); }
/* SPU_LL_AB: switch modes; holds the one lock and every stripe meanwhile. */
void spu_ll_set_mode(int m)
{
    m = m ? 1 : 0;
    if (ll_mode() == m || (m && !ll_mode_allowed())) return;
    unsigned spins = 0;
    while (_InterlockedExchange(&g_lockline, 1)) {
        do { _mm_pause(); if (++spins >= 4096) { spins = 0; SwitchToThread(); } } while (g_lockline);
    }
    for (unsigned i = 0; i < LL_NS; i++) ll_s_lock(i);
    g_spu_ll_mode = m;
    for (unsigned i = 0; i < LL_NS; i++) ll_s_unlock(i);
    _InterlockedExchange(&g_lockline, 0);
}
/* Registration (stripes): every stripe must be held. A debug check, as the
 * registration path is rare. */
static int ll_all_held(void)
{
    if (g_spu_ll_mode != 1) return 1;
    for (unsigned i = 0; i < LL_NS; i++) if (!s_ll_stripe[i].v) return 0;
    return 1;
}

typedef struct { uintptr_t key; unsigned long long calls, cont, acq, spin, hold, same, diff, spin_same, spin_diff; } ll_site;
typedef struct { uint32_t line; unsigned long long calls, cont; } ll_line;
#define LL_SITES 128
#define LL_LINES 512
typedef struct ll_tab {
    DWORD tid;
    ll_site s[LL_SITES];
    ll_line l[LL_LINES];
    unsigned long long t_acq; ll_site* cur;
} ll_tab;
static ll_tab* volatile s_ll_tabs[1024];
static volatile long s_ll_ntab;
static __declspec(thread) ll_tab* t_ll;
volatile long long g_ll_reg_add, g_ll_reg_del;   /* registry inserts / removals (stats only) */
static DWORD WINAPI ll_dumper(void* p);
static ll_tab* ll_me(void)
{
    ll_tab* t = t_ll;
    if (t) return t;
    t = (ll_tab*)calloc(1, sizeof *t);
    if (!t) abort();
    t->tid = GetCurrentThreadId();
    long i = _InterlockedIncrement(&s_ll_ntab) - 1;
    if (i < 1024) s_ll_tabs[i] = t;
    if (i == 0) CreateThread(NULL, 0, ll_dumper, NULL, 0, NULL);
    t_ll = t;
    return t;
}
static ll_site* ll_site_of(ll_tab* t, uintptr_t key)
{
    unsigned h = (unsigned)(((unsigned long long)key * 0x9E3779B97F4A7C15ull) >> 57);
    for (unsigned k = 0; k < LL_SITES; k++) {
        ll_site* s = &t->s[(h + k) & (LL_SITES - 1)];
        if (s->key == key) return s;
        if (!s->key) { s->key = key; return s; }
    }
    return &t->s[h & (LL_SITES - 1)];
}
static void ll_line_note(ll_tab* t, uint32_t line, int cont)
{
    unsigned h = (line >> 7) * 2654435761u >> 23;
    for (unsigned k = 0; k < LL_LINES; k++) {
        ll_line* l = &t->l[(h + k) & (LL_LINES - 1)];
        if (l->calls && l->line != line) continue;
        l->line = line; l->calls++; l->cont += (unsigned)cont;
        return;
    }
}
static void ll_stat_lock(uintptr_t ra)
{
    ll_tab* t = ll_me();
    const uint32_t hint = g_spu_ll_hint; g_spu_ll_hint = 0;
    const uintptr_t key = hint ? (uintptr_t)(hint & 127u) : ra;
    const unsigned long long t0 = __rdtsc();
    int cont = 0; uint32_t holder = 0xFFFFFFFFu;
    unsigned spins = 0;
    while (_InterlockedExchange(&g_lockline, 1)) {
        if (!cont) { cont = 1; holder = s_lockline.holder; }
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (g_lockline);
    }
    const unsigned long long t1 = __rdtsc();
    s_lockline.holder = hint ? (hint & ~127u) : 0xFFFFFFFFu;
    ll_site* s = ll_site_of(t, key);
    s->calls++; s->acq += t1 - t0;
    if (cont) {
        s->cont++; s->spin += t1 - t0;
        if (hint && holder != 0xFFFFFFFFu) {
            if (holder == (hint & ~127u)) { s->same++; s->spin_same += t1 - t0; }
            else { s->diff++; s->spin_diff += t1 - t0; }
        }
    }
    if (hint) ll_line_note(t, hint & ~127u, cont);
    t->t_acq = t1; t->cur = s;
}
static void ll_stat_unlock(void)
{
    ll_tab* t = t_ll;
    if (t && t->cur) { t->cur->hold += __rdtsc() - t->t_acq; t->cur = NULL; }
}
static const char* ll_code_name(uintptr_t k)
{
    static const char* n[] = { "?", "GETLLAR", "PUTLLC", "PUTLLUC", "PPU-store", "PPU-stwcx", "PPU-stdcx",
                               "DMA-PUT-span", "DMA-PUT-late", "SDCRZ", "EventAck", "PPU-vecstore", "TempArea" };
    return k < sizeof n / sizeof n[0] ? n[k] : "?";
}
static DWORD WINAPI ll_dumper(void* p)
{
    (void)p;
    enum { NA = 4096, NT = 1024, NL = 8192 };
    static ll_site cur[NA], prev[NA]; static unsigned ncur, nprev;
    static unsigned long long tprev[NT][5];
    static struct { uint32_t line; unsigned long long calls, cont; } lc[NL], lp[NL]; static unsigned nlc, nlp;
    static long long radd0, rdel0;
    LARGE_INTEGER f, q0, q1; QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0); unsigned long long c0 = __rdtsc();
    const uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    for (;;) {
        Sleep(5000);
        QueryPerformanceCounter(&q1); const unsigned long long c1 = __rdtsc();
        const double hz = (double)(c1 - c0) / ((double)(q1.QuadPart - q0.QuadPart) / (double)f.QuadPart);
        const double ms = 1000.0 / hz;
        q0 = q1; c0 = c1;
        long ntab = s_ll_ntab; if (ntab > NT) ntab = NT;
        ncur = 0; nlc = 0;
        unsigned long long tot[5] = {0};
        char tbuf[8192]; int tp = 0;
        for (long i = 0; i < ntab; i++) {
            ll_tab* t = s_ll_tabs[i]; if (!t) continue;
            unsigned long long tt[5] = {0};
            for (unsigned k = 0; k < LL_SITES; k++) {
                const ll_site* s = &t->s[k]; if (!s->key) continue;
                unsigned j = 0; for (; j < ncur && cur[j].key != s->key; j++) {}
                if (j == ncur) { if (ncur == NA) continue; memset(&cur[j], 0, sizeof cur[j]); cur[j].key = s->key; ncur++; }
                cur[j].calls += s->calls; cur[j].cont += s->cont; cur[j].acq += s->acq; cur[j].spin += s->spin;
                cur[j].hold += s->hold; cur[j].same += s->same; cur[j].diff += s->diff;
                cur[j].spin_same += s->spin_same; cur[j].spin_diff += s->spin_diff;
                tt[0] += s->calls; tt[1] += s->cont; tt[2] += s->acq; tt[3] += s->spin; tt[4] += s->hold;
            }
            for (unsigned k = 0; k < LL_LINES; k++) {
                if (!t->l[k].calls) continue;
                unsigned j = 0; for (; j < nlc && lc[j].line != t->l[k].line; j++) {}
                if (j == nlc) { if (nlc == NL) continue; lc[j].line = t->l[k].line; lc[j].calls = lc[j].cont = 0; nlc++; }
                lc[j].calls += t->l[k].calls; lc[j].cont += t->l[k].cont;
            }
            unsigned long long d[5]; for (int z = 0; z < 5; z++) { d[z] = tt[z] - tprev[i][z]; tprev[i][z] = tt[z]; tot[z] += d[z]; }
            if (d[0] >= 1000 && tp < (int)sizeof tbuf - 200)
                tp += snprintf(tbuf + tp, sizeof tbuf - (size_t)tp,
                               "[llstat] tid=%lu calls=%llu cont=%llu (%.1f%%) acq=%.1fms spin=%.1fms hold=%.1fms\n",
                               (unsigned long)t->tid, d[0], d[1], d[0] ? 100.0 * d[1] / d[0] : 0.0, d[2] * ms, d[3] * ms, d[4] * ms);
        }
        const long long ra = g_ll_reg_add, rd = g_ll_reg_del;
        fprintf(stderr, "[llstat] ---- 5 s (tsc %.2f GHz) registry +%lld -%lld ----\n%s", hz / 1e9, ra - radd0, rd - rdel0, tbuf);
        radd0 = ra; rdel0 = rd;
        fprintf(stderr, "[llstat] ALL calls=%llu cont=%llu (%.1f%%) acq=%.1fms (uncont %.1fms) spin=%.1fms hold=%.1fms\n",
                tot[0], tot[1], tot[0] ? 100.0 * tot[1] / tot[0] : 0.0, tot[2] * ms, (tot[2] - tot[3]) * ms, tot[3] * ms, tot[4] * ms);
        static ll_site dl[NA]; unsigned nd = 0;
        for (unsigned j = 0; j < ncur; j++) {
            ll_site d = cur[j];
            for (unsigned q = 0; q < nprev; q++) if (prev[q].key == d.key) {
                d.calls -= prev[q].calls; d.cont -= prev[q].cont; d.acq -= prev[q].acq; d.spin -= prev[q].spin;
                d.hold -= prev[q].hold; d.same -= prev[q].same; d.diff -= prev[q].diff;
                d.spin_same -= prev[q].spin_same; d.spin_diff -= prev[q].spin_diff; break; }
            if (d.calls) dl[nd++] = d;
        }
        for (unsigned r = 0; r < 24 && r < nd; r++) {
            unsigned b = r;
            for (unsigned q = r + 1; q < nd; q++) if (dl[q].acq + dl[q].hold > dl[b].acq + dl[b].hold) b = q;
            ll_site x = dl[b]; dl[b] = dl[r]; dl[r] = x;
            char nm[64];
            if (x.key < 128) snprintf(nm, sizeof nm, "%s", ll_code_name(x.key));
            else snprintf(nm, sizeof nm, "rva=0x%llX", (unsigned long long)(x.key - base));
            fprintf(stderr, "[llstat]  %-16s calls=%-9llu cont=%-8llu (%5.1f%%) acq=%7.1fms spin=%7.1fms hold=%7.1fms same=%llu/%.1fms diff=%llu/%.1fms\n",
                    nm, x.calls, x.cont, 100.0 * x.cont / x.calls, x.acq * ms, x.spin * ms, x.hold * ms,
                    x.same, x.spin_same * ms, x.diff, x.spin_diff * ms);
        }
        memcpy(prev, cur, ncur * sizeof cur[0]); nprev = ncur;
        static struct { uint32_t line; unsigned long long calls, cont; } ld[NL]; unsigned nld = 0;
        for (unsigned j = 0; j < nlc; j++) {
            unsigned long long c = lc[j].calls, n = lc[j].cont;
            for (unsigned q = 0; q < nlp; q++) if (lp[q].line == lc[j].line) { c -= lp[q].calls; n -= lp[q].cont; break; }
            if (c) { ld[nld].line = lc[j].line; ld[nld].calls = c; ld[nld].cont = n; nld++; }
        }
        for (unsigned r = 0; r < 20 && r < nld; r++) {
            unsigned b = r;
            for (unsigned q = r + 1; q < nld; q++) if (ld[q].calls > ld[b].calls) b = q;
            uint32_t xl = ld[b].line; unsigned long long xc = ld[b].calls, xn = ld[b].cont;
            ld[b] = ld[r]; ld[r].line = xl; ld[r].calls = xc; ld[r].cont = xn;
            fprintf(stderr, "[llstat]  line 0x%08X calls=%llu cont=%llu (%.1f%%)\n", xl, xc, xn, 100.0 * xn / xc);
        }
        fprintf(stderr, "[llstat]  lines seen %u\n", nld);
        memcpy(lp, lc, nlc * sizeof lc[0]); nlp = nlc;
        fflush(stderr);
    }
    return 0;
}
#else
#include <stdatomic.h>
volatile long long g_ll_reg_add, g_ll_reg_del;
static COH_ALIGN64 atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
/* SPU_LL_STRIPE is Windows-only: everything here is the one lock. */
volatile int g_spu_ll_mode = 0;
void spu_lockline_lock_ea(uint32_t ea)   { (void)ea; spu_lockline_lock(); }
void spu_lockline_unlock_ea(uint32_t ea) { (void)ea; spu_lockline_unlock(); }
void spu_lockline_lock_range(uint32_t ea, uint32_t n)   { (void)ea; (void)n; spu_lockline_lock(); }
void spu_lockline_unlock_range(uint32_t ea, uint32_t n) { (void)ea; (void)n; spu_lockline_unlock(); }
void spu_coh_unlock_tok(uint64_t held) { (void)held; spu_lockline_unlock(); }
int  spu_ll_striped(void) { return 0; }
void spu_ll_set_mode(int m) { (void)m; }
static int ll_all_held(void) { return 1; }
#endif

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINE_SHIFT  7                                  /* 128-byte lines */
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

/* Exported (with g_spu_coh_armed) for the inline PPU stores in ppu_recomp.h,
 * which test a line's bit themselves before taking the out-of-line path.
 *
 * Nothing here is written unless its value changes. GETLLAR re-reserves the
 * same lines millions of times a second; setting the bit and the armed flag
 * every time took their cache lines away from every PPU thread, whose next
 * store near those lines then missed. */
COH_ALIGN64 unsigned char g_spu_coh_bitmap[SPU_COH_BITMAP_SZ];
#define s_coh_bitmap g_spu_coh_bitmap

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code, and keeps every existing
 * test that stores through vm_write* paying one compare against a hot global.
 * Written once, under the lock; read unlocked, where a stale zero costs at most
 * one missed event on the very first reservation of the run. */
COH_ALIGN64 int g_spu_coh_armed;
#define s_coh_armed g_spu_coh_armed

/* Every live virtual context may retain a reservation between scheduling
 * quanta; physical SPU count is not a bound on this set. All access is under
 * the lock-line lock. Nodes are removed before their context's lifetime ends.
 *
 * The list (next/prev) is what notify walks, in the same order as ever. The
 * hash chains (hnext) only answer "is this context listed": every GETLLAR asks
 * that, and walking the whole list for it, with the lock held, was most of
 * the lock's hold time (SPU_COH_HASH=0 walks again). */
typedef struct spu_coh_entry {
    spu_context* ctx;
    struct spu_coh_entry* next;
    struct spu_coh_entry* prev;
    struct spu_coh_entry* hnext;
    unsigned didx;              /* its row in s_coh_dense (SPU_COH_DENSE) */
} spu_coh_entry;
static spu_coh_entry* s_coh_ctxs;

/* SPU_COH_DENSE=1: notify scans a packed array of (line last reserved,
 * context) rows instead of chasing every listed context. A context's row
 * takes the line of its every GETLLAR (spu_coh_reserve, the only path that
 * makes a reservation valid, runs just before it under the same lock hold),
 * so a context holding a valid reservation on line L always has row line L;
 * a row whose context has since lost it costs one extra look. The list stays
 * as it is for unregistering. God of War: Ascension registers a context per
 * workload and SPU lane; walking them all, one cache miss each, on every PPU
 * store into a reserved line, with the lock held, was ~6% of its main thread
 * (spu_coh_notify_write + spu_lockline_lock). 0: the list walk, as before. */
typedef struct { uint32_t line; spu_context* ctx; } spu_coh_row;
static spu_coh_row* s_coh_dense;
static unsigned s_coh_dense_n, s_coh_dense_cap;
/* SPU_COH_LINEIDX (default on with SPU_COH_DENSE; =0: scan): the rows chained
 * per line hash, so a notify visits only the rows of its line instead of
 * scanning all of them (64-128 rows, 2 KB, with the lock held, on every PPU
 * store into a reserved line, every PUTLLC commit and every PUT into a
 * reserved line). The same rows are visited, in another order; each visit
 * acts on its own context only. Lock held for every use, as the rows. */
#define COH_LIDX_BITS 10u
static int s_lidx_head[1u << COH_LIDX_BITS];   /* row + 1, 0 = empty */
static int* s_lidx_next; static int* s_lidx_prev;   /* row + 1, per row */
static unsigned s_lidx_cap;
static int spu_coh_lineidx_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPU_COH_LINEIDX"); on = !(e && e[0] == '0'); }
    return on;
}
static inline unsigned coh_lidx_slot(uint32_t l7) { return (l7 * 2654435761u) >> (32u - COH_LIDX_BITS); }
/* SPU_COH_LINEIDX_CHECK=1 (diagnostic): every notify also counts the rows of
 * its line by the full scan and by the chain; a difference prints [coh-lidx]. */
static void coh_lidx_check(uint32_t l7)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPU_COH_LINEIDX_CHECK"); on = e && e[0] == '1'; }
    if (!on) return;
    unsigned scan = 0, chain = 0, steps = 0;
    for (unsigned i = 0; i < s_coh_dense_n; i++) if (s_coh_dense[i].line == l7) scan++;
    for (int r = s_lidx_head[coh_lidx_slot(l7)]; r && steps < 100000u; r = s_lidx_next[r - 1], steps++)
        if ((unsigned)(r - 1) < s_coh_dense_n && s_coh_dense[r - 1].line == l7) chain++;
    static unsigned long long checks; static unsigned bad;
    checks++;
    if (scan != chain || steps >= 100000u) {
        if (bad++ < 20) fprintf(stderr, "[coh-lidx] MISMATCH line=0x%08X scan=%u chain=%u steps=%u rows=%u\n",
                                l7 << 7, scan, chain, steps, s_coh_dense_n);
    } else if ((checks & 0xFFFFFull) == 1) {
        fprintf(stderr, "[coh-lidx] %llu notifies checked, %u mismatches\n", checks, bad);
    }
}
static void coh_lidx_unlink(unsigned i)
{
    if (s_coh_dense[i].line == 0xFFFFFFFFu) return;   /* never linked */
    const int p = s_lidx_prev[i], n = s_lidx_next[i];
    if (p) s_lidx_next[p - 1] = n; else s_lidx_head[coh_lidx_slot(s_coh_dense[i].line)] = n;
    if (n) s_lidx_prev[n - 1] = p;
    s_lidx_prev[i] = s_lidx_next[i] = 0;
}
static void coh_lidx_link(unsigned i)
{
    if (s_coh_dense[i].line == 0xFFFFFFFFu) return;
    const unsigned s = coh_lidx_slot(s_coh_dense[i].line);
    const int h = s_lidx_head[s];
    s_lidx_prev[i] = 0; s_lidx_next[i] = h;
    if (h) s_lidx_prev[h - 1] = (int)i + 1;
    s_lidx_head[s] = (int)i + 1;
}
static int spu_coh_dense_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPU_COH_DENSE"); on = e && e[0] == '1'; }
    return on;
}
static void spu_coh_dense_add(spu_coh_entry* entry)
{
    if (s_coh_dense_n == s_coh_dense_cap) {
        unsigned cap = s_coh_dense_cap ? s_coh_dense_cap * 2 : 64;
        spu_coh_row* rows = (spu_coh_row*)realloc(s_coh_dense, cap * sizeof(*rows));
        if (!rows) {
            fprintf(stderr, "[spu-coh] cannot grow the reservation table to %u\n", cap);
            fflush(stderr);
            abort(); /* a context missing from the table would miss its events */
        }
        s_coh_dense = rows; s_coh_dense_cap = cap;
    }
    if (s_lidx_cap < s_coh_dense_cap) {
        int* nx = (int*)realloc(s_lidx_next, s_coh_dense_cap * sizeof(int));
        int* pv = nx ? (int*)realloc(s_lidx_prev, s_coh_dense_cap * sizeof(int)) : NULL;
        if (!nx || !pv) { fprintf(stderr, "[spu-coh] cannot grow the line index\n"); fflush(stderr); abort(); }
        s_lidx_next = nx; s_lidx_prev = pv;
        for (unsigned k = s_lidx_cap; k < s_coh_dense_cap; k++) s_lidx_next[k] = s_lidx_prev[k] = 0;
        s_lidx_cap = s_coh_dense_cap;
    }
    entry->didx = s_coh_dense_n++;
    s_coh_dense[entry->didx].line = 0xFFFFFFFFu;   /* no line yet */
    s_lidx_next[entry->didx] = s_lidx_prev[entry->didx] = 0;
    s_coh_dense[entry->didx].ctx = entry->ctx;
    if ((s_coh_dense_n & (s_coh_dense_n - 1)) == 0 && s_coh_dense_n >= 16)
        fprintf(stderr, "[spu-coh] %u contexts hold or held a reservation\n", s_coh_dense_n);
}
static spu_coh_entry* spu_coh_find(const spu_context* ctx);
static void spu_coh_dense_remove(spu_coh_entry* entry)
{
    const unsigned i = entry->didx, last = s_coh_dense_n - 1;
    coh_lidx_unlink(i);
    if (i != last) coh_lidx_unlink(last);
    --s_coh_dense_n;
    if (i != last) {
        s_coh_dense[i] = s_coh_dense[last];
        coh_lidx_link(i);
        spu_coh_entry* moved = spu_coh_find(s_coh_dense[i].ctx);   /* learns its new row */
        if (moved) moved->didx = i;
    }
}
#define SPU_COH_HASH_BITS 10
static spu_coh_entry* s_coh_hash[1u << SPU_COH_HASH_BITS];

static unsigned spu_coh_hash_of(const spu_context* ctx)
{
    const uintptr_t p = (uintptr_t)ctx;
    return (unsigned)((p >> 6) ^ (p >> 16) ^ (p >> 26)) & ((1u << SPU_COH_HASH_BITS) - 1u);
}

static int spu_coh_hash_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPU_COH_HASH"); on = !(e && e[0] == '0'); }
    return on;
}

static spu_coh_entry* spu_coh_find(const spu_context* ctx)
{
    if (spu_coh_hash_on()) {
        for (spu_coh_entry* e = s_coh_hash[spu_coh_hash_of(ctx)]; e; e = e->hnext)
            if (e->ctx == ctx) return e;
        return NULL;
    }
    for (spu_coh_entry* e = s_coh_ctxs; e; e = e->next)
        if (e->ctx == ctx) return e;
    return NULL;
}

/* GETLLAR (spu_channels.c): take what reserving `ea` for `ctx` needs, for
 * spu_coh_reserve and the line read; returns what is held, for
 * spu_coh_unlock_tok. One lock: that lock, as before (0). Stripes: ea's stripe,
 * and when the context's row leaves a line of another stripe, that one too
 * (its chain links, and a reservation still valid on the old line, are that
 * stripe's). A context's first reservation registers it, which changes the
 * registry: every stripe for that once. Only the context's own thread moves
 * its row or registers it, so what is read here stays true until it acts. */
uint64_t spu_coh_getllar_lock(spu_context* ctx, uint32_t ea)
{
#if defined(_MSC_VER)
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    for (;;) {
        if (ll_mode() == 0) {
            ll_g_lock(ra);
            if (g_spu_ll_mode == 0) return 0;
            ll_g_unlock();
            continue;
        }
        const unsigned sb = ll_stripe_l7(ea >> SPU_COH_LINE_SHIFT);
        const uint64_t mb = 1ull << sb;
        ll_s_lock(sb);
        if (g_spu_ll_mode != 1) { ll_s_unlock(sb); continue; }
        if (!ctx) return mb;
        const spu_coh_entry* e = spu_coh_find(ctx);
        if (!e) {
            ll_s_unlock(sb);
            ll_set_lock(~0ull);
            if (g_spu_ll_mode == 1) return ~0ull;
            ll_set_unlock(~0ull);
            continue;
        }
        const uint32_t a7 = s_coh_dense[e->didx].line;
        if (a7 == 0xFFFFFFFFu) return mb;
        const unsigned sa = ll_stripe_l7(a7);
        if (sa == sb) return mb;
        const uint64_t ma = 1ull << sa;
        if (sa > sb) { ll_s_lock(sa); return ma | mb; }
        if (ll_s_trylock(sa)) return ma | mb;
        ll_s_unlock(sb);   /* ascending: the lower stripe first */
        ll_s_lock(sa);
        ll_s_lock(sb);
        if (g_spu_ll_mode == 1) return ma | mb;
        ll_s_unlock(sb);
        ll_s_unlock(sa);
    }
#else
    (void)ctx; (void)ea;
    spu_lockline_lock();
    return 0;
#endif
}

/* Out of the list and its hash chain, and freed. */
static void spu_coh_unlink(spu_coh_entry* entry)
{
    g_ll_reg_del++;   /* SPU_LL_STATS */
    if (spu_coh_dense_on()) spu_coh_dense_remove(entry);   /* SPU_COH_DENSE */
    if (entry->prev) entry->prev->next = entry->next;
    else s_coh_ctxs = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    for (spu_coh_entry** link = &s_coh_hash[spu_coh_hash_of(entry->ctx)]; *link; link = &(*link)->hnext)
        if (*link == entry) { *link = entry->hnext; break; }
    free(entry);
}

COH_ALIGN64 unsigned long g_spu_lr_raise = 0;

/* SPU_COH_LIVE=1: a second, clearable bit per line -- "a reservation on this
 * line may be live" -- so a PPU store into a line some SPU reserved once, but
 * nobody holds now, skips the lock-line lock and the notify.
 *
 * The marks in s_coh_bitmap never clear (they route a line's stores out of
 * line); God of War: Ascension's main thread stored ~650,000 times a second
 * into three such lines (0x970F80, 0x970F00, 0x967E00), each a global lock
 * take and a notify with nobody to notify.
 *
 * The live bit is set by GETLLAR (spu_coh_reserve, lock held) BEFORE it reads
 * the line, with a full fence between, and cleared only by a locked PPU store
 * whose notify has just invalidated every reservation on the line. A PPU store
 * whose line reads not live stores, fences, and reads the bit again
 * (spu_coh_store_fast). That is Dekker's pattern: either the GETLLAR's read of
 * the line sees the store, or the store's second look sees the bit and takes
 * the lock to invalidate the reservation (whose snapshot may predate the
 * store). A reservation that began and ended inside the store's window was
 * either invalidated by the clearing store's notify, or committed a PUTLLC:
 * that compares the line against its snapshot (a store before the compare
 * fails it) and, under SPU_COH_LIVE, writes back only the bytes the SPU changed
 * (spu_channels.c), so a store landing between compare and commit survives
 * unless the SPU changed the same bytes -- a word the PPU plain-stores while an
 * SPU atomically updates it, which no title can rely on either way.
 * Atomic PPU stores (stwcx./stdcx.) keep the locked path.
 *
 * NOT SAFE TO ENABLE YET (2026-10-24): 1 of 4 God of War: Ascension runs with
 * it froze (no flips, the walker idle "caught-up"), and it bought no fps in
 * the RSX-bound enemy area (56.7/56.7 vs 55.3/57.8/56.3). The likely hole:
 * the first look at the live bit comes BEFORE the store, so a PPU thread
 * interrupted between the two (core 0 takes most interrupts) can store into
 * a line whose GETLLAR and PUTLLC both ran meanwhile; if the store lands
 * between the PUTLLC's compare and its commit, in a unit the SPU changed, the
 * commit overwrites it. Closing it: commit each changed 8-byte chunk with a
 * compare-and-swap against the snapshot and, if one fails, undo the chunks
 * already written and fail the PUTLLC. */
COH_ALIGN64 unsigned char g_spu_coh_live[SPU_COH_BITMAP_SZ];
int spu_coh_live_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPU_COH_LIVE"); on = e && e[0] == '1'; }
    return on;
}
COH_ALIGN64 unsigned long long g_spu_coh_fast, g_spu_coh_fast_late, g_spu_coh_locked;   /* PPU_COH_TOP */
static inline int coh_live(uint32_t line)
{
    return (__atomic_load_n(&g_spu_coh_live[line >> 3], __ATOMIC_RELAXED) >> (line & 7)) & 1u;
}
/* Lock held: the line's reservations were all just invalidated. */
static inline void coh_live_clear(uint32_t line)
{
    const unsigned char bit = (unsigned char)(1u << (line & 7));
    if (g_spu_coh_live[line >> 3] & bit) g_spu_coh_live[line >> 3] &= (unsigned char)~bit;
}
/* A PPU plain store of n bytes from src to guest `ea` (host `dst`), on a line
 * marked in s_coh_bitmap. Returns 1 with the store done (and any reservation
 * that may predate it invalidated); 0 when the line may hold a live
 * reservation -- the caller stores under the lock and calls
 * spu_coh_notify_write_clear. */
int spu_coh_store_fast(uint32_t ea, const void* src, size_t n, void* dst)
{
    const uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    if (coh_live(line)) return 0;
    memcpy(dst, src, n);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!coh_live(line)) { g_spu_coh_fast++; return 1; }
    /* A GETLLAR marked the line while the store was in flight: its snapshot
     * may predate the store. */
    spu_lockline_lock();
    spu_coh_notify_write(ea);
    coh_live_clear(line);
    spu_lockline_unlock();
    g_spu_coh_fast_late++;
    return 1;
}
/* Lock held, the store done: notify, and the line has no live reservation. */
void spu_coh_notify_write_clear(uint32_t ea)
{
    spu_coh_notify_write(ea);
    if (spu_coh_live_on()) coh_live_clear(ea >> SPU_COH_LINE_SHIFT);
    g_spu_coh_locked++;
}

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    const unsigned char bit = (unsigned char)(1u << (line & 7));
    /* Stripes: GETLLARs of two lines sharing this byte may hold different
     * stripes, so the mark is an atomic OR there. */
    if (!(s_coh_bitmap[line >> 3] & bit)) {
        if (g_spu_ll_mode == 1) __atomic_fetch_or(&s_coh_bitmap[line >> 3], bit, __ATOMIC_RELAXED);
        else s_coh_bitmap[line >> 3] |= bit;
    }
    if (!s_coh_armed) s_coh_armed = 1;
    if (spu_coh_live_on()) {   /* SPU_COH_LIVE: marked before the caller reads the line */
        if (!(g_spu_coh_live[line >> 3] & bit)) g_spu_coh_live[line >> 3] |= bit;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }

    if (!ctx) return;
    { spu_coh_entry* found = spu_coh_find(ctx);
      if (found) {
          if (spu_coh_dense_on()) {   /* SPU_COH_DENSE */
              const unsigned di = found->didx;
              if (s_coh_dense[di].line != line) {
                  coh_lidx_unlink(di);
                  s_coh_dense[di].line = line;
                  coh_lidx_link(di);
              }
          }
          return;
      } }
    spu_coh_entry* entry = (spu_coh_entry*)malloc(sizeof(*entry));
    if (!entry) {
        fprintf(stderr, "[spu-coh] cannot register reservation holder %p\n", (void*)ctx);
        fflush(stderr);
        abort(); /* A missed holder loses invalidation events, including ABA. */
    }
    g_ll_reg_add++;   /* SPU_LL_STATS */
    if (!ll_all_held()) {   /* stripes: registration needs every stripe (spu_coh_getllar_lock) */
        static int s_n;
        if (s_n++ < 8) fprintf(stderr, "[ll] BUG: context %p registered without every stripe held\n", (void*)ctx);
    }
    entry->ctx = ctx;
    entry->prev = NULL;
    entry->next = s_coh_ctxs;
    if (s_coh_ctxs) s_coh_ctxs->prev = entry;
    s_coh_ctxs = entry;
    const unsigned h = spu_coh_hash_of(ctx);
    entry->hnext = s_coh_hash[h];
    s_coh_hash[h] = entry;
    if (spu_coh_dense_on()) {   /* SPU_COH_DENSE */
        spu_coh_dense_add(entry);
        s_coh_dense[entry->didx].line = line;
        coh_lidx_link(entry->didx);
    }
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Stripes: removal needs every stripe, but most contexts that come here
     * never reserved anything; any one stripe answers "is it registered", and
     * only this context's own thread registers it. */
    if (spu_ll_striped()) {
        const uint32_t any = (uint32_t)((uintptr_t)ctx >> 6) << 7;   /* some stripe, spread by context */
        spu_lockline_lock_ea(any);
        const int listed = spu_coh_find(ctx) != NULL;
        spu_lockline_unlock_ea(any);
        if (!listed) return;
    }
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before removal would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    spu_coh_entry* entry = spu_coh_find(ctx);
    if (entry) spu_coh_unlink(entry);
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (spu_coh_entry* entry = s_coh_ctxs; entry;) {
        spu_coh_entry* next = entry->next;
        uintptr_t c = (uintptr_t)entry->ctx;
        if (c >= lo && c < hi) {
            static int _n = 0;
            if (_n++ < 16)
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, entry->ctx->image_id);
            spu_coh_unlink(entry);
        }
        entry = next;
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    if (!s_coh_armed) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (s_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    if (spu_coh_dense_on()) {   /* SPU_COH_DENSE: only the rows last reserved on this line */
        const uint32_t l7 = ea >> SPU_COH_LINE_SHIFT;
        const int idx = spu_coh_lineidx_on();
        if (idx) coh_lidx_check(l7);
        for (int r = idx ? s_lidx_head[coh_lidx_slot(l7)] : 1; idx ? r != 0 : (unsigned)(r - 1) < s_coh_dense_n;
             r = idx ? s_lidx_next[r - 1] : r + 1) {
            const unsigned i = (unsigned)(r - 1);
            if (s_coh_dense[i].line != l7) continue;
            spu_context* c = s_coh_dense[i].ctx;
            if (!c || (const void*)c == self) continue;
            if (c->resv_valid && (c->resv_ea & ~127u) == line) {
                __atomic_fetch_or(&c->event_status, SPU_EVENT_LR, __ATOMIC_SEQ_CST);   /* SPU_LL_STRIPE: WrEventAck may AND unlocked */
                c->resv_valid = 0;
                g_spu_lr_raise++;
                spu_ch_wake(c);
            }
        }
        return;
    }
    for (spu_coh_entry* entry = s_coh_ctxs; entry; entry = entry->next) {
        spu_context* c = entry->ctx;
        if (!c || (const void*)c == self) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            __atomic_fetch_or(&c->event_status, SPU_EVENT_LR, __ATOMIC_SEQ_CST);
            c->resv_valid = 0;
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
}

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    const int dense = spu_coh_dense_on();   /* SPU_COH_DENSE */
    const uint32_t l7 = ea >> SPU_COH_LINE_SHIFT;
    unsigned di = 0;
    const int lidx = dense && spu_coh_lineidx_on();
    int lr = lidx ? s_lidx_head[coh_lidx_slot(l7)] : 0;
    if (lidx) coh_lidx_check(l7);
    for (spu_coh_entry* entry = dense ? NULL : s_coh_ctxs; dense || entry; entry = dense ? NULL : entry->next) {
        spu_context* c;
        if (dense && lidx) {   /* SPU_COH_LINEIDX: the rows of l7's chain */
            while (lr && s_coh_dense[lr - 1].line != l7) lr = s_lidx_next[lr - 1];
            if (!lr) break;
            c = s_coh_dense[lr - 1].ctx;
            lr = s_lidx_next[lr - 1];
        } else if (dense) {
            while (di < s_coh_dense_n && s_coh_dense[di].line != l7) di++;
            if (di >= s_coh_dense_n) break;
            c = s_coh_dense[di++].ctx;
        } else c = entry->ctx;
        if (!c) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            __atomic_fetch_or(&c->event_status, SPU_EVENT_LR, __ATOMIC_SEQ_CST);
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
    }
}

/* SPU_WrEventAck: clear acknowledged event bits without losing an LR a
 * notifier raises meanwhile. One lock: under it, as before (every notifier
 * holds it). Stripes: an atomic AND, as every notifier sets LR with an atomic
 * OR; holding a stripe would need the context's row line, and no lock is
 * needed for the bits themselves. */
void spu_coh_event_ack(spu_context* ctx, uint32_t v)
{
#if defined(_MSC_VER)
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    for (;;) {
        if (ll_mode() == 1) { __atomic_fetch_and(&ctx->event_status, ~v, __ATOMIC_SEQ_CST); return; }
        ll_g_lock(ra);
        if (g_spu_ll_mode == 0) { ctx->event_status &= ~v; ll_g_unlock(); return; }
        ll_g_unlock();
    }
#else
    spu_lockline_lock();
    ctx->event_status &= ~v;
    spu_lockline_unlock();
#endif
}

#ifdef __cplusplus
}
#endif
