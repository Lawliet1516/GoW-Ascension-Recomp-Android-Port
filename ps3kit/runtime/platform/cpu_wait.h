/*
 * cpu_wait.h (cpu, 2026-11-09): a small lock-free histogram of wait times in
 * microseconds, for the [cpu-wait] line that libs/spurs/cellSpurs.c prints
 * every 5 s with the [spurs] line (SYS_USLEEP_REFWAIT, SPURS_PRIO_SKIP_SAME,
 * SPURS_KERNEL_PARK, SPURS_KERNEL_EVWAIT), with CPU_WAIT_STATS on. Buckets: 1 us wide up to 100 us,
 * 20 us wide up to 2 ms, then one bucket for 2 ms and more. Relaxed atomics: a
 * racing window boundary can move one sample to the next window, nothing else.
 */
#ifndef PS3_CPU_WAIT_H
#define PS3_CPU_WAIT_H

#include <stdlib.h>   /* getenv */

/* CPU_WAIT_STATS: the counters, histograms and the [cpu-wait] line. Unset: on only with SPURS_WID_STATS=1 (test
 * runs), so a player's log and hot paths carry none of it; =1 on, =0 off. */
static inline int cw_stats_on(void)
{
    static volatile int on = -1;
    int v = on;
    if (v < 0) {
        const char* e = getenv("CPU_WAIT_STATS");
        if (e && *e) v = *e != '0';
        else { const char* w = getenv("SPURS_WID_STATS"); v = w && *w == '1'; }
        on = v;
    }
    return v;
}

/* CPU_WAIT_AB=<ms> (TEST ONLY, host\cpu\NOTES.md 4): an in-run A/B of the cpu switches that are on. Every <ms> from
 * the first SPURS kernel's start the "off" side (g_cpu_wait_ab_off = 1: SYS_USLEEP_REFWAIT, SPURS_PRIO_SKIP_SAME,
 * SPURS_KERNEL_PARK and SPURS_KERNEL_EVWAIT take their old code paths) and the "on" side alternate, with
 * "[cpu-ab] on|off t=<us>" lines (cellSpurs.c). Unset: always the "on" side. */
extern volatile int g_cpu_wait_ab_off;
static inline int cw_ab_off(void) { return __atomic_load_n(&g_cpu_wait_ab_off, __ATOMIC_RELAXED); }

#define CW_NB 196
typedef struct { volatile long long n[CW_NB]; } cw_hist;

static inline int cw_bucket(long long us)
{
    if (us < 0) us = 0;
    if (us < 100) return (int)us;
    if (us < 2000) return 100 + (int)((us - 100) / 20);
    return CW_NB - 1;
}
static inline long long cw_bucket_us(int b)
{
    return b < 100 ? b : b < CW_NB - 1 ? 100 + (long long)(b - 100) * 20 : 2000;
}
static inline void cw_hist_add(cw_hist* h, long long us)
{
    __atomic_add_fetch(&h->n[cw_bucket(us)], 1, __ATOMIC_RELAXED);
}
/* Takes (and zeroes) the window: returns the count; p50 / p99 are bucket
 * floors in us (2000 = 2 ms or more). */
static inline long long cw_hist_take(cw_hist* h, long long* p50, long long* p99)
{
    long long c[CW_NB], tot = 0, acc = 0;
    int got50 = 0;
    for (int i = 0; i < CW_NB; i++) { c[i] = __atomic_exchange_n(&h->n[i], 0, __ATOMIC_RELAXED); tot += c[i]; }
    *p50 = *p99 = 0;
    if (!tot) return 0;
    for (int i = 0; i < CW_NB; i++) {
        acc += c[i];
        if (!got50 && acc * 2 >= tot) { *p50 = cw_bucket_us(i); got50 = 1; }
        if (acc * 100 >= tot * 99) { *p99 = cw_bucket_us(i); break; }
    }
    return tot;
}

#endif /* PS3_CPU_WAIT_H */
