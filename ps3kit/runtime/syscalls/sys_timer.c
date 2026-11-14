/*
 * ps3recomp - Timer and time syscalls (implementation)
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* tm_gmtoff on glibc */
#endif
#include "sys_timer.h"
#include <time.h>
#include "sys_event.h"
#include "../memory/vm.h"
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_timer_info g_sys_timers[SYS_TIMER_MAX];

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

int64_t lv2_usec_deadline(uint64_t usec)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    return now.QuadPart +
        (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
}

int lv2_deadline_passed(int64_t deadline)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart >= deadline;
}
#endif

/* ---------------------------------------------------------------------------
 * PPU timebase (mftb/mftbu) -- THE guest clock.
 *
 * One global monotonic counter scaled to the PS3 timebase (79.8 MHz),
 * anchored at first use. The old lifter emission was a per-call-site
 * static that advanced 16667 ticks PER READ -- not a clock at all: every
 * guest timing loop (media pacers, throttles, profilers) computed garbage
 * from it, and each call site had a PRIVATE counter that only moved when
 * polled. Overflow-safe split multiply (rem < qpf, so
 * rem * 79.8e6 < ~8e14 << 2^63).
 * -----------------------------------------------------------------------*/
uint64_t ppu_timebase_now(void)
{
#ifdef _WIN32
    static LONGLONG t0 = 0;
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!t0) t0 = now.QuadPart;   /* benign race: same anchor either way */
    uint64_t d = (uint64_t)(now.QuadPart - t0);
    uint64_t q = (uint64_t)s_qpc_freq.QuadPart;
    return (d / q) * PS3_TIMEBASE_FREQ + (d % q) * PS3_TIMEBASE_FREQ / q;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * PS3_TIMEBASE_FREQ +
           (uint64_t)ts.tv_nsec * PS3_TIMEBASE_FREQ / 1000000000ull;
#endif
}

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static void write_be64(uint32_t addr, uint64_t val)
{
    uint64_t* p = (uint64_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 56) & 0xFFULL) |
          ((val >> 40) & 0xFF00ULL) |
          ((val >> 24) & 0xFF0000ULL) |
          ((val >>  8) & 0xFF000000ULL) |
          ((val <<  8) & 0xFF00000000ULL) |
          ((val << 24) & 0xFF0000000000ULL) |
          ((val << 40) & 0xFF000000000000ULL) |
          ((val << 56) & 0xFF00000000000000ULL);
#endif
    *p = val;
}


/* ===========================================================================
 * speed/main (Track M, 2026-11-12; host\speed\main\NOTES.md). TEST switches for the guest MAIN thread's waits,
 * all OFF by default (= main's behaviour; with nothing set each hook is a load and a not-taken branch).
 * None of them reads or writes guest state (no ppu_context field, no guest memory): they change only how the host
 * thread spends the time the guest asked to wait or spin.
 *   M_USLEEP=<n>   short sys_timer_usleep (below SYS_USLEEP_SPIN_MAX_US) on the main thread only:
 *                  1 = PAUSE-spin to the deadline (no SwitchToThread); 2 = PAUSE-spin M_USLEEP_SPIN_US (default 10),
 *                  then the old SwitchToThread loop; 3 = SwitchToThread only every 4th PAUSE burst.
 *   M_SPIN=<n>     the guest spin / job-wait loops (m_spin_hint sites in the private lift, any guest thread):
 *                  1 = PAUSE per iteration; 2 = PAUSE, and SwitchToThread once the loop has run M_SPIN_US
 *                  (default 20) without a gap; 3 = as 2 but the yield on the main thread only.
 *   M_PRIO=<n>     the main thread's THREAD_PRIORITY level (1 above normal, 2 highest) while the switch side is on.
 *   M_AB=1         the switches above only in the odd 15 s windows from process creation ("[m-ab] window N ON|OFF";
 *                  M_AB=2: in the even windows instead (speed/cand: swaps the sides against the scene's own timeline),
 *                  same windows as g6thr.exe / GEN6_AB / JP_AB); else on for the whole run.
 *   M_STATS=1      one "[m-stats]" line per 5 s: main-thread usleeps (count, asked, taken, worst), spin-hint
 *                  iterations / yields (main, others), main-thread CPU ms.
 * ======================================================================== */
#ifdef _WIN32
#include <intrin.h>
extern DWORD host_ppu_main_thread_id(void);
static volatile int s_m_init, s_m_usleep, s_m_spin, s_m_prio, s_m_ab, s_m_stats, s_m_ab_inv;
static unsigned s_m_usleep_spin_us = 10, s_m_spin_us = 20;
volatile int g_m_on = 1;
static unsigned long long s_m_tsc_per_us = 4000;
static volatile long long s_mst[12];   /* 0 usleep n, 1 asked us, 2 taken us, 3 worst us, 4 hint main, 5 yields main,
                                          6 hint other, 7 yields other */
static unsigned long long m_ms_since_creation(void)
{
    FILETIME cr, ex, ke, us, now;
    GetProcessTimes(GetCurrentProcess(), &cr, &ex, &ke, &us); GetSystemTimeAsFileTime(&now);
    const unsigned long long c = ((unsigned long long)cr.dwHighDateTime << 32) | cr.dwLowDateTime;
    const unsigned long long n = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return n > c ? (n - c) / 10000ull : 0;
}
static void m_apply_prio(int on)
{
    if (!s_m_prio) return;
    const DWORD tid = host_ppu_main_thread_id();
    if (!tid) return;
    HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return;
    SetThreadPriority(h, on ? s_m_prio : THREAD_PRIORITY_NORMAL);
    CloseHandle(h);
}
static DWORD WINAPI m_ab_thread(LPVOID arg)
{
    (void)arg;
    long last = -1; int prio_applied = -1;
    for (;;) {
        const unsigned long long t = m_ms_since_creation();
        const long win = s_m_ab ? (long)(t / 15000ull) : 1;
        const int on = (int)(win & 1) ^ s_m_ab_inv;
        if (win != last) {
            last = win;
            __atomic_store_n(&g_m_on, on, __ATOMIC_SEQ_CST);
            if (s_m_ab) fprintf(stderr, "[m-ab] window %ld %s (t %.1f s)\n", win, on ? "ON" : "OFF", (double)t / 1000.0);
        }
        if (prio_applied != on && host_ppu_main_thread_id()) { m_apply_prio(on); prio_applied = on; }
        if (!s_m_ab) { Sleep(200); continue; }
        const unsigned long long next = (unsigned long long)(win + 1) * 15000ull, now = m_ms_since_creation();
        DWORD ms = next > now ? (DWORD)(next - now) + 1 : 1;
        if (prio_applied != on && ms > 200) ms = 200;   /* main thread not known yet: look again soon */
        Sleep(ms);
    }
    return 0;
}
static DWORD WINAPI m_stat_thread(LPVOID arg)
{
    (void)arg;
    long long prev[12] = {0}; ULONG64 cy0 = 0; HANDLE mh = NULL;
    for (;;) {
        Sleep(5000);
        if (!mh && host_ppu_main_thread_id()) mh = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, host_ppu_main_thread_id());
        ULONG64 cy = 0; if (mh) QueryThreadCycleTime(mh, &cy);
        long long v[12]; for (int i = 0; i < 12; i++) v[i] = s_mst[i];
        const long long n = v[0] - prev[0];
        fprintf(stderr, "[m-stats] per 5s: main usleep %lld asked %lld ms taken %lld ms (avg %.1f us, worst %lld us) | "
                        "spin hints main %lld yields %lld, other %lld yields %lld | main cpu %.0f ms | side %s\n",
                n, (v[1] - prev[1]) / 1000, (v[2] - prev[2]) / 1000, n ? (double)(v[2] - prev[2]) / (double)n : 0.0, v[3],
                v[4] - prev[4], v[5] - prev[5], v[6] - prev[6], v[7] - prev[7],
                cy0 && cy ? (double)(cy - cy0) / (double)s_m_tsc_per_us / 1000.0 : 0.0, g_m_on ? "ON" : "OFF");
        s_mst[3] = 0;
        for (int i = 0; i < 12; i++) prev[i] = v[i];
        cy0 = cy;
    }
    return 0;
}
static void m_init(void)
{
    static volatile LONG s_once;
    if (InterlockedExchange(&s_once, 1)) { while (!s_m_init) YieldProcessor(); return; }
    const char* e;
    e = getenv("M_USLEEP"); s_m_usleep = e ? atoi(e) : 0;
    e = getenv("M_USLEEP_SPIN_US"); if (e) s_m_usleep_spin_us = (unsigned)atoi(e);
    e = getenv("M_SPIN"); s_m_spin = e ? atoi(e) : 0;
    e = getenv("M_SPIN_US"); if (e) s_m_spin_us = (unsigned)atoi(e);
    e = getenv("M_PRIO"); s_m_prio = e ? atoi(e) : 0; if (s_m_prio < 0 || s_m_prio > 2) s_m_prio = 0;
    e = getenv("M_AB"); s_m_ab = e && (*e == '1' || *e == '2'); s_m_ab_inv = e && *e == '2';
    e = getenv("M_STATS"); s_m_stats = e && *e == '1';
    { LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a); const unsigned long long t0 = __rdtsc();
      do QueryPerformanceCounter(&b); while ((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart < 2000);
      const unsigned long long t1 = __rdtsc(); const long long us = (b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart;
      if (us > 0 && t1 > t0) s_m_tsc_per_us = (t1 - t0) / (unsigned long long)us; }
    if (s_m_ab) g_m_on = s_m_ab_inv;
    if (s_m_ab || s_m_prio) { HANDLE h = CreateThread(NULL, 64u << 10, m_ab_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
    if (s_m_stats) { HANDLE h = CreateThread(NULL, 64u << 10, m_stat_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
    if (s_m_usleep || s_m_spin || s_m_prio || s_m_stats)
        fprintf(stderr, "[m] usleep %d (spin %u us) spin %d (yield after %u us) prio %d ab %d stats %d tsc %llu/us\n",
                s_m_usleep, s_m_usleep_spin_us, s_m_spin, s_m_spin_us, s_m_prio, s_m_ab, s_m_stats, s_m_tsc_per_us);
    s_m_init = 1;
}
static __declspec(thread) unsigned long long t_m_start, t_m_last;
static __declspec(thread) unsigned t_m_site;
static __declspec(thread) int t_m_main = -1;
static inline int m_is_main(void)
{
    if (t_m_main < 0) { const DWORD m = host_ppu_main_thread_id(); if (!m) return 0; t_m_main = GetCurrentThreadId() == m; }
    return t_m_main;
}
void m_spin_hint(unsigned site)
{
    if (__builtin_expect(!s_m_init, 0)) m_init();
    if (!s_m_spin && !s_m_stats) return;
    const int mainth = m_is_main();
    if (s_m_stats) __atomic_add_fetch(&s_mst[mainth ? 4 : 6], 1, __ATOMIC_RELAXED);
    if (!s_m_spin || !g_m_on) return;
    _mm_pause();
    if (s_m_spin < 2 || (s_m_spin == 3 && !mainth)) return;
    const unsigned long long now = __rdtsc();
    if (site != t_m_site || now - t_m_last > 200ull * s_m_tsc_per_us) { t_m_site = site; t_m_start = now; }
    t_m_last = now;
    if (now - t_m_start > (unsigned long long)s_m_spin_us * s_m_tsc_per_us) {
        SwitchToThread();
        if (s_m_stats) __atomic_add_fetch(&s_mst[mainth ? 5 : 7], 1, __ATOMIC_RELAXED);
        t_m_last = __rdtsc();
    }
}
/* sys_timer_usleep's short path for the main thread: 1 = waited to qpc_deadline, 0 = take the old path
 * (after mode 2's PAUSE prefix). */
static int m_usleep_short(int64_t qpc_deadline)
{
    if (__builtin_expect(!s_m_init, 0)) m_init();
    if (!s_m_usleep || !g_m_on || !m_is_main()) return 0;
    LARGE_INTEGER now;
    if (s_m_usleep == 1) {
        do { for (int k = 0; k < 32; k++) YieldProcessor(); QueryPerformanceCounter(&now); } while (now.QuadPart < qpc_deadline);
        return 1;
    }
    if (s_m_usleep == 2) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&now);
        const int64_t pend = now.QuadPart + (int64_t)((uint64_t)s_m_usleep_spin_us * (uint64_t)f.QuadPart / 1000000ull);
        const int64_t end = pend < qpc_deadline ? pend : qpc_deadline;
        do { for (int k = 0; k < 32; k++) YieldProcessor(); QueryPerformanceCounter(&now); } while (now.QuadPart < end);
        return now.QuadPart >= qpc_deadline;
    }
    if (s_m_usleep == 3) {
        int i = 0;
        do { for (int k = 0; k < 32; k++) YieldProcessor(); if ((++i & 3) == 0) SwitchToThread(); QueryPerformanceCounter(&now); }
        while (now.QuadPart < qpc_deadline);
        return 1;
    }
    return 0;
}
static inline void m_usleep_stat(int64_t qpc_start, uint64_t usec)
{
    if (!s_m_stats || !m_is_main()) return;
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    const long long us = (now.QuadPart - qpc_start) * 1000000ll / f.QuadPart;
    __atomic_add_fetch(&s_mst[0], 1, __ATOMIC_RELAXED); __atomic_add_fetch(&s_mst[1], (long long)usec, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_mst[2], us, __ATOMIC_RELAXED);
    if (us > s_mst[3]) s_mst[3] = us;
}
#endif

/* ---------------------------------------------------------------------------
 * sys_timer_usleep
 *
 * r3 = microseconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_usleep(ppu_context* ctx)
{
    uint64_t usec = LV2_ARG_U64(ctx, 0);
    { extern unsigned long long ps3_qpc_us(void);
      static int n=0; if (n < 60 && ++n)
        fprintf(stderr, "[WAIT] t=%lluus timer_usleep(%llu us) lr=0x%08llX cia=0x%08llX\n", ps3_qpc_us(),
        (unsigned long long)usec, (unsigned long long)ctx->lr, (unsigned long long)ctx->cia); }
    /* PS3_POLLTOP=<seconds>: name the usleep poll sites.
     *
     * A title that is "hung" is usually not blocked on anything -- every thread
     * is spinning on sys_timer_usleep waiting for a flag, and [BLOCKSUM] only
     * says "syscall 141, 12s". This histograms the callers over a window and
     * prints the top ones, so the answer is a guest address rather than a
     * count. Keyed on ctx->lr: a host-stack backtrace through lifted code has
     * no unwind tables and invents chains (see the Guitar Hero III writeup),
     * and lr is the only guest chain that survives.
     *
     * Deliberately unlocked: the counts are a diagnostic, and a lost increment
     * under a race cannot change which site is at the top of a 30k-per-second
     * poll. */
    { static long s_pt = -1;
      if (s_pt < 0) { const char* e = getenv("PS3_POLLTOP");
                      s_pt = e ? (long)strtoul(e, 0, 10) : 0;
                      if (s_pt < 0) s_pt = 0; }
      if (s_pt > 0) {
        enum { PT_MAX = 128 };
        static uint32_t pt_lr[PT_MAX]; static uint32_t pt_n[PT_MAX];
        static uint64_t pt_us[PT_MAX]; static int pt_used = 0;
        static uint32_t pt_tid[PT_MAX];
        static unsigned long long pt_win = 0;
        uint32_t lr = (uint32_t)ctx->lr;
        int i = 0;
        for (; i < pt_used; i++) if (pt_lr[i] == lr) break;
        if (i == pt_used && pt_used < PT_MAX) {
            pt_lr[pt_used] = lr;
            /* The thread matters as much as the site: "who is waiting" is the
             * half that says whether a poll is the engine idling or the main
             * thread stuck. */
            pt_tid[pt_used] = (uint32_t)ctx->thread_id;
            pt_used++;
        }
        if (i < PT_MAX) { pt_n[i]++; pt_us[i] += usec; }
        { extern unsigned long long ps3_qpc_us(void);
          unsigned long long now = ps3_qpc_us();
          if (!pt_win) pt_win = now;
          else if (now - pt_win >= (unsigned long long)s_pt * 1000000ull) {
            fprintf(stderr, "--- usleep poll sites (last %lds) ---\n", s_pt);
            for (int k = 0; k < 8; k++) {
              int best = -1;
              for (int j = 0; j < pt_used; j++)
                if (pt_n[j] && (best < 0 || pt_n[j] > pt_n[best])) best = j;
              if (best < 0) break;
              fprintf(stderr, "  %8u calls  %6llums slept  tid=%-3u lr=0x%08X\n",
                      pt_n[best], (unsigned long long)(pt_us[best] / 1000),
                      pt_tid[best], pt_lr[best]);
              pt_n[best] = 0;
            }
            fflush(stderr);
            for (int j = 0; j < pt_used; j++) { pt_n[j] = 0; pt_us[j] = 0; }
            pt_win = now;
          } } } }

    /* PS3_WAIT_OBJ=<lr-hex>: when a usleep spin is reached from this return
     * address, dump the registers and the object they point at. A poll loop
     * tells you WHERE it is spinning; this tells you WHAT it is spinning on,
     * which is the part you actually need to find who never releases it. */
    { static long s_wo = -1;
      if (s_wo < 0) { const char* e = getenv("PS3_WAIT_OBJ");
                      s_wo = e ? (long)strtoul(e, 0, 16) : 0; }
      if (s_wo && (uint32_t)ctx->lr == (uint32_t)s_wo) {
        static int _n = 0;
        if (_n++ < 8) {
          /* Dump the words at EVERY plausible object register, not just r29.
           * Which register holds the object is per-title -- r29 was right for
           * the title this was written for and is a spin COUNTER in Guitar
           * Hero III, where the object is in r30/r31. Printing one guess makes
           * the tool silently report the wrong memory as "the thing it waits
           * on", which is worse than printing nothing. */
          fprintf(stderr, "[wait-obj] lr=0x%08X sleep=%lluus", (uint32_t)ctx->lr,
                  (unsigned long long)usec);
          static const int regs[] = { 28, 29, 30, 31, 3 };
          for (unsigned ri = 0; ri < sizeof regs / sizeof regs[0]; ri++) {
            const int r = regs[ri];
            const uint32_t o = (uint32_t)ctx->gpr[r];
            fprintf(stderr, "  r%d=0x%08X", r, o);
            /* Only deref something that looks like a guest pointer: a small
             * integer is a count, and dumping "memory at 4" is noise. */
            if (vm_base && o >= 0x10000u) {
              fprintf(stderr, "[");
              for (int w = 0; w < 4; w++) {
                const uint8_t* q = vm_base + o + (unsigned)w * 4u;
                uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                             ((uint32_t)q[2] << 8)  | (uint32_t)q[3];
                fprintf(stderr, "%s%08X", w ? " " : "", v);
              }
              fprintf(stderr, "]");
            }
          }
          fprintf(stderr, "\n"); fflush(stderr);
        } } }

    /* SYS_USLEEP_DIV=<n> (experiment, off): every guest sleep is n times
     * shorter -- does a load wait on the game's own poll loops or on work? */
    { static long s_div = -1;
      if (s_div < 0) { const char* e = getenv("SYS_USLEEP_DIV"); s_div = e ? atol(e) : 0; }
      if (s_div > 1) usec /= (uint64_t)s_div; }

    /* POLLSITE: resolve the host chain of the 1ms poller (LBP bringup) to
     * guest functions -- names the stage that is starving. */
    { static int _ps = -1; if (_ps < 0) _ps = getenv("POLLSITE") ? 12 : 0;
      if (_ps > 0 && usec == 1000) { _ps--;
        extern void ppu_log_host_chain(const char*);
        ppu_log_host_chain("usleep1ms"); } }

#ifdef _WIN32
    /* From SYS_USLEEP_SPIN_MAX_US (default 200) up, sleep on a per-thread
     * high-resolution waitable timer (Windows 10 1803+): it wakes within a
     * fraction of a millisecond without spinning. The spin below was meant for
     * microsecond waits; God of War: Ascension polls with 800 us sleeps on one
     * thread, which kept a whole core busy doing nothing. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    static long s_spin_max = -1;
    if (s_spin_max < 0) { const char* e = getenv("SYS_USLEEP_SPIN_MAX_US"); s_spin_max = e ? atol(e) : 200; }
    static __declspec(thread) HANDLE t_hr_timer;
    static __declspec(thread) int t_hr_failed;
    if (usec >= (uint64_t)s_spin_max && usec > 0 && !t_hr_failed) {
        if (!t_hr_timer) {
            t_hr_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!t_hr_timer) t_hr_failed = 1;
        }
        if (t_hr_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -((LONGLONG)usec * 10);   /* 100 ns units, negative = relative */
            if (SetWaitableTimer(t_hr_timer, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(t_hr_timer, INFINITE);
                return CELL_OK;
            }
        }
    }
    /* Use high-resolution sleep via waitable timer for better precision */
    if (usec >= 1000) {
        HANDLE timer = CreateWaitableTimerW(NULL, TRUE, NULL);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -((LONGLONG)usec * 10); /* 100ns units, negative = relative */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
        } else {
            Sleep((DWORD)(usec / 1000));
        }
    } else if (usec > 0) {
        /* SYS_USLEEP_REFWAIT=<lr>[,<lr>...] (cpu, 2026-11-09; hex guest return addresses; unset or 0: off, the
         * title sets it): a short usleep from one of these sites, right after the same thread read the RSX ref
         * register, is a poll for the ref. It blocks in cellGcm_ref_wait (libs/video/cellGcmSys.c) until a fence
         * is published or the read gate (one queued fence per ~200 us of reads) opens -- the only moments a read
         * there can see or do anything new -- at most SYS_USLEEP_REFWAIT_CAP_US (250), instead of yield-spinning
         * the 30 us (a fence already queued behind the gate: it spins to the gate as before). God of War:
         * Ascension's flip-pacing wait (lr 0x375A78, func_00375A50) spun 119-356 ms a
         * second of the PPU main thread at the 60 cap (host\cpu\NOTES.md 1.3.3). Any other site, or the ref
         * not read just before, keeps the spin below. */
        { static int s_rw_n = -1; static uint32_t s_rw_lr[8]; static unsigned s_rw_cap = 250;
          if (s_rw_n < 0) {
              int k = 0;
              const char* e = getenv("SYS_USLEEP_REFWAIT");
              while (e && *e && k < 8) {
                  char* end; const unsigned long v = strtoul(e, &end, 16);
                  if (end == e) break;
                  if (v) s_rw_lr[k++] = (uint32_t)v;
                  e = (*end == ',') ? end + 1 : NULL;
              }
              const char* c = getenv("SYS_USLEEP_REFWAIT_CAP_US");
              if (c && atol(c) > 0) s_rw_cap = (unsigned)atol(c);
              if (k) fprintf(stderr, "[usleep] SYS_USLEEP_REFWAIT: %d ref-poll site(s), first lr=0x%06X, cap %u us\n",
                             k, s_rw_lr[0], s_rw_cap);
              s_rw_n = k;
          }
          if (s_rw_n > 0) {
              const uint32_t lr = (uint32_t)ctx->lr;
              for (int k = 0; k < s_rw_n; k++)
                  if (s_rw_lr[k] == lr) {
                      extern int cellGcm_ref_wait(unsigned cap_us) __attribute__((weak));
                      if (cellGcm_ref_wait && cellGcm_ref_wait(s_rw_cap) > 0) return CELL_OK;
                      break;
                  }
          } }
        /* Sub-millisecond sleep. Sleep()/waitable timers round up to the
         * ~0.5-1 ms scheduler quantum, far too coarse for a guest that relies
         * on accurate microsecond pacing -- a short usleep() spin waiting on
         * another thread to make progress otherwise collapses to a single yield
         * and busy-spins at full speed (so the wait is effectively a no-op).
         * Busy-wait to a precise QPC deadline, yielding inside the spin
         * (SwitchToThread) so a sibling host thread the guest may be waiting on
         * still gets the core. Mirrors RPCS3's "Usleep Only" TSC busy-tail. */
        ensure_qpc_init();
        LARGE_INTEGER qpc_start, qpc_now;
        QueryPerformanceCounter(&qpc_start);
        const int64_t qpc_deadline = qpc_start.QuadPart +
            (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
        if (m_usleep_short(qpc_deadline)) { m_usleep_stat(qpc_start.QuadPart, usec); return CELL_OK; }   /* speed/main */
        /* SYS_USLEEP_PAUSE=1 (off by default): a thread that PS3_HOST_PIN
         * gave a logical processor of its own (the guest main thread in the
         * 8-core plan) has nobody to yield to there, so it spins on PAUSE
         * instead of a SwitchToThread syscall per iteration. God of War:
         * Ascension's main thread spends ~55% of a heavy frame in usleep(30)
         * polls (func_003E798C 0x3E7A24, HANDOFF 0.12); as syscalls they take
         * issue slots from the SMT sibling (the SPURS kernel) and package
         * power from the SPU cores. Any other thread keeps the yield. */
        static int s_pause = -1;
        if (s_pause < 0) { const char* e = getenv("SYS_USLEEP_PAUSE"); s_pause = e && *e == '1'; }
        extern int host_pin_self_exclusive(void);
        if (s_pause && host_pin_self_exclusive()) {
            do {
                for (int k = 0; k < 32; k++) YieldProcessor();   /* PAUSE x32: ~0.5 us */
                QueryPerformanceCounter(&qpc_now);
            } while (qpc_now.QuadPart < qpc_deadline);
        } else
        do {
            SwitchToThread();
            QueryPerformanceCounter(&qpc_now);
        } while (qpc_now.QuadPart < qpc_deadline);
        m_usleep_stat(qpc_start.QuadPart, usec);   /* speed/main */
    }
#else
    if (usec > 0) {
        struct timespec ts;
        ts.tv_sec  = (time_t)(usec / 1000000);
        ts.tv_nsec = (long)((usec % 1000000) * 1000);
        nanosleep(&ts, NULL);
    }
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_sleep
 *
 * r3 = seconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_sleep(ppu_context* ctx)
{
    uint32_t sec = LV2_ARG_U32(ctx, 0);

#ifdef _WIN32
    Sleep(sec * 1000);
#else
    sleep(sec);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_current_time
 *
 * r3 = pointer to receive seconds (u64*)
 * r4 = pointer to receive nanoseconds (u64*)
 * -----------------------------------------------------------------------*/
/* LV2 144: r3/r4 point to signed BE32 minute offsets. The first is the
 * standard UTC offset; the second is the current seasonal adjustment.
 * Derive the adjustment instead of assuming every DST shift is 60 minutes. */
int64_t sys_time_get_timezone(ppu_context* ctx)
{
    uint32_t zone_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t dst_addr = LV2_ARG_PTR(ctx, 1);
    if (!zone_addr || !dst_addr || zone_addr > UINT32_MAX - 3 || dst_addr > UINT32_MAX - 3)
        return (int32_t)CELL_EFAULT;

    time_t now = time(NULL);
    struct tm local;
    int32_t standard_minutes, daylight_minutes;
#ifdef _WIN32
    _tzset();
    long west_seconds, daylight_bias;
    if (localtime_s(&local, &now) || _get_timezone(&west_seconds) || _get_dstbias(&daylight_bias))
        return (int32_t)CELL_EAGAIN;
    standard_minutes = (int32_t)(-west_seconds / 60);
    daylight_minutes = local.tm_isdst > 0 ? (int32_t)(-daylight_bias / 60) : 0;
#else
    if (!localtime_r(&now, &local)) return (int32_t)CELL_EAGAIN;
    daylight_minutes = 0;
    if (local.tm_isdst > 0) {
        struct tm standard = local;
        standard.tm_isdst = 0;
        time_t standard_epoch = mktime(&standard);
        if (standard_epoch == (time_t)-1) return (int32_t)CELL_EAGAIN;
        daylight_minutes = (int32_t)(difftime(standard_epoch, now) / 60);
    }
    standard_minutes = (int32_t)(local.tm_gmtoff / 60) - daylight_minutes;
#endif
    write_be32(zone_addr, (uint32_t)standard_minutes);
    write_be32(dst_addr, (uint32_t)daylight_minutes);
    return CELL_OK;
}

int64_t sys_time_get_current_time(ppu_context* ctx)
{
    uint32_t sec_addr  = LV2_ARG_PTR(ctx, 0);
    uint32_t nsec_addr = LV2_ARG_PTR(ctx, 1);

    uint64_t sec, nsec;

#ifdef _WIN32
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    /* Convert QPC to seconds + nanoseconds */
    sec  = (uint64_t)(now.QuadPart / s_qpc_freq.QuadPart);
    uint64_t remainder = (uint64_t)(now.QuadPart % s_qpc_freq.QuadPart);
    nsec = (remainder * 1000000000ULL) / (uint64_t)s_qpc_freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    sec  = (uint64_t)ts.tv_sec;
    nsec = (uint64_t)ts.tv_nsec;
#endif

    if (sec_addr != 0) {
        write_be64(sec_addr, sec);
    }
    if (nsec_addr != 0) {
        write_be64(nsec_addr, nsec);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_timebase_frequency
 *
 * Returns the PS3 timebase frequency in r3.
 * -----------------------------------------------------------------------*/
int64_t sys_time_get_timebase_frequency(ppu_context* ctx)
{
    (void)ctx;
    return (int64_t)PS3_TIMEBASE_FREQ;
}

/* ---------------------------------------------------------------------------
 * Periodic timer thread
 * -----------------------------------------------------------------------*/

/* Forward declaration */
static void timer_send_event(sys_timer_info* t);

#ifdef _WIN32
static DWORD WINAPI timer_thread_proc(LPVOID param)
{
    sys_timer_info* t = (sys_timer_info*)param;

    while (t->running) {
        DWORD ms = (DWORD)(t->period_usec / 1000);
        if (ms == 0) ms = 1;

        DWORD result = WaitForSingleObject(t->stop_event, ms);
        if (result == WAIT_OBJECT_0) {
            break; /* stop signaled */
        }

        if (t->running) {
            timer_send_event(t);
        }
    }

    return 0;
}
#else
static void* timer_thread_proc(void* param)
{
    sys_timer_info* t = (sys_timer_info*)param;

    pthread_mutex_lock(&t->mtx);
    while (!t->stop_flag) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(t->period_usec / 1000000);
        ts.tv_nsec += (long)((t->period_usec % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        int rc = pthread_cond_timedwait(&t->cv, &t->mtx, &ts);
        if (t->stop_flag) break;

        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&t->mtx);
            timer_send_event(t);
            pthread_mutex_lock(&t->mtx);
        }
    }
    pthread_mutex_unlock(&t->mtx);

    return NULL;
}
#endif

static void timer_send_event(sys_timer_info* t)
{
    if (t->event_queue_id <= 0 || t->event_queue_id > SYS_EVENT_QUEUE_MAX)
        return;

    sys_event_queue_info* q = &g_sys_event_queues[t->event_queue_id - 1];
    if (!q->active)
        return;

    sys_event_t evt;
    evt.source = t->source;
    evt.data1  = t->data1;
    evt.data2  = 0;
    evt.data3  = 0;

    /* Push event (ignore if queue full) */
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    if (q->count < q->capacity) {
        q->buffer[q->tail] = evt;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
#ifdef _WIN32
        WakeConditionVariable(&q->not_empty);
#else
        pthread_cond_signal(&q->not_empty);
#endif
    }

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}

/* ---------------------------------------------------------------------------
 * sys_timer_create
 *
 * r3 = pointer to receive timer ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_timer_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);

    int slot = -1;
    for (int i = 0; i < SYS_TIMER_MAX; i++) {
        if (!g_sys_timers[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_timer_info* t = &g_sys_timers[slot];
    memset(t, 0, sizeof(*t));
    t->active  = 1;
    t->running = 0;
    t->event_queue_id = 0;

#ifdef _WIN32
    t->stop_event    = NULL;
    t->thread_handle = NULL;
    t->timer_handle  = NULL;
#else
    pthread_mutex_init(&t->mtx, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->stop_flag = 0;
#endif

    uint32_t timer_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, timer_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_destroy
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_destroy(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Stop if running */
    if (t->running) {
        t->running = 0;
#ifdef _WIN32
        if (t->stop_event) SetEvent(t->stop_event);
        if (t->thread_handle) {
            WaitForSingleObject(t->thread_handle, 3000);
            CloseHandle(t->thread_handle);
        }
        if (t->stop_event) CloseHandle(t->stop_event);
#else
        pthread_mutex_lock(&t->mtx);
        t->stop_flag = 1;
        pthread_cond_signal(&t->cv);
        pthread_mutex_unlock(&t->mtx);
        pthread_join(t->thread, NULL);
        pthread_mutex_destroy(&t->mtx);
        pthread_cond_destroy(&t->cv);
#endif
    }

    t->active = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_connect_event_queue
 *
 * r3 = timer_id
 * r4 = queue_id
 * r5 = source
 * r6 = data1
 * -----------------------------------------------------------------------*/
int64_t sys_timer_connect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);
    uint32_t queue_id = LV2_ARG_U32(ctx, 1);
    uint64_t source   = LV2_ARG_U64(ctx, 2);
    uint64_t data1    = LV2_ARG_U64(ctx, 3);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = (int32_t)queue_id;
    t->source         = source;
    t->data1          = data1;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_disconnect_event_queue
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_disconnect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_start
 *
 * r3 = timer_id
 * r4 = base_time (absolute start time, 0 = now)
 * r5 = period_usec
 * -----------------------------------------------------------------------*/
int64_t sys_timer_start(ppu_context* ctx)
{
    uint32_t timer_id    = LV2_ARG_U32(ctx, 0);
    /* uint64_t base_time = LV2_ARG_U64(ctx, 1); -- ignored for now */
    uint64_t period      = LV2_ARG_U64(ctx, 2);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (t->running)
        return (int64_t)(int32_t)CELL_EBUSY;

    if (period == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    t->period_usec = period;
    t->running     = 1;

#ifdef _WIN32
    t->stop_event    = CreateEventA(NULL, TRUE, FALSE, NULL);
    t->thread_handle = CreateThread(NULL, 0, timer_thread_proc, t, 0, NULL);
#else
    t->stop_flag = 0;
    pthread_create(&t->thread, NULL, timer_thread_proc, t);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_stop
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_stop(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (!t->running)
        return CELL_OK;

    t->running = 0;

#ifdef _WIN32
    if (t->stop_event) SetEvent(t->stop_event);
    if (t->thread_handle) {
        WaitForSingleObject(t->thread_handle, 3000);
        CloseHandle(t->thread_handle);
        t->thread_handle = NULL;
    }
    if (t->stop_event) {
        CloseHandle(t->stop_event);
        t->stop_event = NULL;
    }
#else
    pthread_mutex_lock(&t->mtx);
    t->stop_flag = 1;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mtx);
    pthread_join(t->thread, NULL);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 *
 * NOTE: There are syscall number collisions in the existing table:
 *   SYS_TIMER_USLEEP (141) == SYS_EVENT_FLAG_WAIT (141)
 *   SYS_TIMER_SLEEP  (142) == SYS_EVENT_FLAG_TRYWAIT (142)
 *   SYS_TIME_GET_CURRENT_TIME (145) == SYS_EVENT_FLAG_CANCEL (145)
 * The event flag handlers are registered separately and will win.
 * Timer sleep/time functions should be called via wrapper functions
 * or fixed syscall numbers should be assigned.
 * For now, we register the timer-specific syscalls that don't collide.
 * -----------------------------------------------------------------------*/
void sys_timer_init(lv2_syscall_table* tbl)
{
    memset(g_sys_timers, 0, sizeof(g_sys_timers));

#ifdef _WIN32
    ensure_qpc_init();
#endif

    lv2_syscall_register(tbl, SYS_TIMER_CREATE,                   sys_timer_create);
    lv2_syscall_register(tbl, SYS_TIMER_DESTROY,                  sys_timer_destroy);
    lv2_syscall_register(tbl, SYS_TIMER_START,                    sys_timer_start);
    lv2_syscall_register(tbl, SYS_TIMER_STOP,                     sys_timer_stop);
    lv2_syscall_register(tbl, SYS_TIMER_CONNECT_EVENT_QUEUE,      sys_timer_connect_event_queue);
    lv2_syscall_register(tbl, SYS_TIMER_DISCONNECT_EVENT_QUEUE,   sys_timer_disconnect_event_queue);

    /* These have conflicting numbers with event flag syscalls.
     * Register them here -- last registration wins. If events are
     * registered after timers, the event handlers will override.
     * The runtime should dispatch timer_usleep/sleep/get_current_time
     * via direct function calls instead. */
    lv2_syscall_register(tbl, SYS_TIME_GET_TIMEBASE_FREQUENCY, sys_time_get_timebase_frequency);
    lv2_syscall_register(tbl, SYS_TIME_GET_TIMEZONE, sys_time_get_timezone);

    /* Register these but be aware of collisions */
    lv2_syscall_register(tbl, SYS_TIMER_USLEEP,            sys_timer_usleep);
    lv2_syscall_register(tbl, SYS_TIMER_SLEEP,             sys_timer_sleep);
    lv2_syscall_register(tbl, SYS_TIME_GET_CURRENT_TIME,   sys_time_get_current_time);
}
