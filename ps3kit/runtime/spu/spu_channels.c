/*
 * ps3recomp - SPU channel + indirect-branch runtime glue
 *
 * Implements the externs the SPU lifter (tools/spu_lifter.py) emits:
 *   - spu_rdch / spu_rchcnt / spu_wrch : SPU channel access. MFC channels are
 *     routed to the DMA engine (spu_dma.h); mailboxes, signal notification,
 *     events and the decrementer use the spu_context channel fields.
 *   - spu_indirect_branch : resolves ctx->pc to a lifted spu_func_* via a
 *     registry that generated code populates by calling spu_recomp_register().
 *
 * The MFC engine state is kept per spu_context here (spu_context.h does not
 * embed one), in a small lazily-populated registry.
 */

#include "spu_dma.h"
#include "spu_coherency.h" /* lock-line lock + PPU-write coherence bitmap */
#include "spu_helpers.h"   /* spu_splat_u32 / spu_ls_read128 (SMC microstep) */
#include "spu_lockstep.h"
#include "spu_interp.h"    /* spu_lifted_fn / spu_lifted_lookup -- defined below */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#ifndef _WIN32
#include <sched.h>   /* sched_yield */
#endif
#include <time.h>
#include "../platform/win32_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The SPU decrementer ticks at the PS3 timebase, 79.8 MHz -- the same clock
 * sys_time_get_timebase_frequency reports to the PPU. Titles calibrate real
 * delays against it, so the rate has to be right, not merely non-zero. */
#define SPU_DECREMENTER_HZ  79800000ull

static uint64_t spu_host_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER s_freq;
    LARGE_INTEGER c;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&c);
    /* Split the divide to keep the multiply from overflowing: QPC counters are
     * large enough that (count * 1e9) wraps a u64 within hours of uptime. */
    return (uint64_t)(c.QuadPart / s_freq.QuadPart) * 1000000000ull
         + (uint64_t)(c.QuadPart % s_freq.QuadPart) * 1000000000ull
           / (uint64_t)s_freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ---------------------------------------------------------------------------
 * Clean SPU job abort (longjmp). The `br .` halt idiom and other terminal
 * spins can't be escaped by setting a status flag (lifted code never checks
 * it), so spu_halt() longjmps back to spu_run_with_halt() in the dispatcher.
 * -----------------------------------------------------------------------*/
#if defined(_MSC_VER)
#  define SPU_TLS __declspec(thread)
#else
#  define SPU_TLS __thread
#endif
/* Called after an SPU atomic commits a 128-byte line, with the lock-line lock
 * released. cellSpurs sets it so a PPU blocked in cellSpursEventFlagWait wakes
 * when a task sets the flag, instead of on its next 2 ms poll. */
void (*g_spu_line_commit_hook)(uint32_t line) = 0;
uint32_t (*g_edge6_atomic_hook)(spu_context* ctx, uint32_t cmd, uint32_t ea, uint32_t lsa) = 0;
/* spu_workload.c: wakes tasks parked in WAIT_SIGNAL on that line's taskset. */
void spu_taskset_line_committed(uint32_t line);
/* SPU_LS_WATCH=0x1BE80[,0x927D80...]: watch up to 4 16-byte LS lines (reads
 * and writes) in every SPU image. See spu_ls_watch_hit2 for the fast path. */
#define SPU_WATCH_MAX 4
int g_spu_ls_watch_n = -1;
static unsigned s_spu_ls_watch[SPU_WATCH_MAX];
unsigned* spu_ls_watch_list(int* out_n)
{
    if (g_spu_ls_watch_n < 0) {
        int n = 0; const char* e = getenv("SPU_LS_WATCH");
        while (e && *e && n < SPU_WATCH_MAX) {
            s_spu_ls_watch[n++] = (unsigned)strtoul(e, (char**)&e, 0) & ~0xFu;
            while (*e == ',' || *e == ' ') e++;
        }
        g_spu_ls_watch_n = n;
    }
    *out_n = g_spu_ls_watch_n;
    return s_spu_ls_watch;
}
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{
    int n; spu_ls_watch_list(&n);
    if (!n) return;
    uint32_t a = lsa & (SPU_LS_MASK & ~0xFu);
    for (int i = 0; i < g_spu_ls_watch_n; i++) {
        if (s_spu_ls_watch[i] == a) {
            fprintf(stderr, "[spu-watch %s 0x%05X pc=0x%05X lr=0x%05X] "
                "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                is_write ? "WR" : "rd", a, pc, lr,
                p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
            fflush(stderr);
            break;
        }
    }
}

static SPU_TLS jmp_buf s_spu_halt_env;
static SPU_TLS int     s_spu_halt_armed = 0;

/* Halts leave without unwinding. The CRT's x64 longjmp runs RtlUnwindEx over
 * every host frame between the halt and its landing pad whenever the jump
 * buffer records a frame, and clang-cl's setjmp always records one: ~1 us a
 * halt, ~4% of every SPU worker in combat (policy runs end in a halt, iret
 * and restart are jumps too). Nothing on that path needs unwinding: lifted
 * code and this runtime are C, and no function the SPU threads run between
 * a landing pad and a halt has an exception or termination handler (checked
 * in the executable's unwind tables). With the frame cleared the CRT restores
 * the same registers, MXCSR and x87 control word from the same buffer and
 * jumps. It requires the frame under control-flow guard or shadow stacks, so
 * those keep the unwind. SPU_HALT_UNWIND=1: always unwind (the old path). */
int spu_halt_noframe(void)
{
    static volatile long s_on = -1;
    if (s_on >= 0) return (int)s_on;
    long on = 0;
#if defined(_M_X64) && defined(_MSC_VER)
    const char* e = getenv("SPU_HALT_UNWIND");
    on = !(e && e[0] == '1');
    if (on) {
        /* GetProcessMitigationPolicy: 7 = control-flow guard, 15 = user
         * shadow stack; bit 0 of each is "enabled". Unknown means keep the
         * unwind. */
        typedef BOOL (WINAPI* gpmp_fn)(HANDLE, int, PVOID, SIZE_T);
        gpmp_fn gpmp = (gpmp_fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                     "GetProcessMitigationPolicy");
        DWORD cfg = 0, ss = 0;
        if (!gpmp || !gpmp(GetCurrentProcess(), 7, &cfg, sizeof cfg) || (cfg & 1)) on = 0;
        if (gpmp && gpmp(GetCurrentProcess(), 15, &ss, sizeof ss) && (ss & 1)) on = 0;
    }
#endif
    if (InterlockedCompareExchange(&s_on, on, -1) == -1)
        fprintf(stderr, "[spu] halt longjmp: %s\n", on ? "direct (no unwind)" : "unwinding");
    return (int)s_on;
}

/* Called right after a halt landing pad's setjmp returned 0. */
void spu_jmpbuf_noframe(jmp_buf env)
{
#if defined(_M_X64) && defined(_MSC_VER)
    if (spu_halt_noframe()) ((_JUMP_BUFFER*)env)->Frame = 0;
#else
    (void)env;
#endif
}

/* A non-local return replaces the old host call chain without changing
 * any architectural registers, local store, or pending guest work. */
void spu_restart_dispatch(spu_context* ctx)
{
    g_spu_trampoline_fn = 0;
    if (s_spu_halt_armed) longjmp(s_spu_halt_env, 2);
    fprintf(stderr, "[spu] non-local return outside execution driver at 0x%05X\n", ctx->pc);
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

/* SPU->PPU outbound-mailbox delivery hook. The SPU writing WrOutMbox /
 * WrOutIntrMbox must wake PPU code blocked on the SPURS event queue bound to
 * the SPU thread group (e.g. cellSpursInitialize). lv2_register.c installs a
 * handler that maps spu_group_id -> connected event queue and pushes an event.
 * NULL until installed (plain SPU jobs with no PPU listener stay a no-op). */
int (*g_spu_user_event_hook)(spu_context*, uint32_t) = NULL;
void (*g_spu_out_mbox_hook)(uint32_t group_id, uint32_t spu_id,
                            int is_intr, uint32_t value) = 0;

/* Every MFC PUT an SPU completes, if anyone is listening. Installed by the GCM
 * layer (libs/video/cellGcmSys.c), which needs it to find the RSX pushbuffer:
 * a title whose SPUs build the command stream never tells libgcm where that
 * stream is, so the only evidence is the DMA that writes it. NULL until
 * installed, so a title with no SPU-built FIFO pays nothing. */
SPU_THREAD_LOCAL void (*t_spu_ksvc_hook)(spu_context*, uint32_t, void (*)(spu_context*)) = 0;   /* nexec/enative replay */
SPU_THREAD_LOCAL int (*t_spu_atomic_hook)(spu_context*, uint32_t, int (*)(spu_context*, uint32_t)) = 0;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size) = 0;

/* GCM_PUT_ORDER=2 is the default (below); 0 = plain copies.
 * GCM_PUT_ORDER=1 (spu_dma.h; test only): PUTs into RSX-visible memory,
 * back to front, one 16-byte store at a time (x86 makes stores visible in
 * program order) with a compiler barrier between them so the order is the one
 * written here. Off: it reorders nearly every SPU PUT (the IO map covers
 * ~194 MB), and one PUT is only part of a segment anyway -- GCM_PARK_HOLD_US
 * (cellGcmSys.c) keeps the walker off a rewritten park until the whole run
 * that wrote it has ended, which covers the inside of one PUT too.
 *
 * GCM_TEAR_TEST=<every>[,<us>[,post]] forces the race: every <every>-th such
 * PUT that covers a park word (a JUMP to its own IO address) stops <us>
 * (default 5000, past the walker's longest sleep). Without "post": between
 * the park's 16 bytes and the rest (front to back: the park already
 * rewritten; back to front and the GCM_PUT_ORDER=2 default with the park at
 * the head: everything after it written, the park not yet). With "post": after the whole PUT, i.e. between it and the job's later DMAs
 * into the same segment -- the window GCM_PARK_HOLD_US closes (keep <us>
 * under its bound). Never while the PUT holds the lock-line lock. Test only. */
const uint16_t* volatile g_gcm_ea_io_tab = 0;

static int s_put_order = -1;
volatile int g_gcm_put_order_on = 1; /* GCM_PUT_ORDER_AB flips it (rsx_live_draw.c) */
static unsigned s_tear_every, s_tear_us, s_tear_post;
static long long s_tear_n;

static __attribute__((noinline)) void spu_dma_rsx_env(void)
{
    const char* e = getenv("GCM_TEAR_TEST");
    if (e && atoi(e) > 0) {
        const char* c = strchr(e, ',');
        s_tear_every = (unsigned)atoi(e);
        s_tear_us = c && atoi(c + 1) > 0 ? (unsigned)atoi(c + 1) : 5000u;
        s_tear_post = strstr(e, "post") != NULL;
    }
    e = getenv("GCM_PUT_ORDER");
    const int order = e && e[0] == '1' ? 1 : e && e[0] == '0' ? 0 : 2;
    if (order != 2 || s_tear_every)
        fprintf(stderr, "[spu-dma] PUTs into RSX memory: %s%s\n",
                order == 1 ? "back to front (GCM_PUT_ORDER=1)" :
                order == 2 ? "a park at the head written last (GCM_PUT_ORDER=2)" : "front to back",
                !s_tear_every ? "" : s_tear_post ? ", GCM_TEAR_TEST on (after the PUT)" : ", GCM_TEAR_TEST on");
    s_put_order = order;
}

/* GCM_PUT_ORDER=2 (default): a PUT whose first word is, as it stands, a park (a JUMP to
 * its own IO address). That is how this title's SPUs fill a segment hole: one
 * PUT of the hole, the park at its head. The walker moves on as soon as that
 * word changes, so it is written last: the rest of the PUT first, then the
 * head's 16 bytes as two 8-byte stores, the park's half last. A copying thread
 * preempted halfway (six SPU threads on a 4-core PC) then leaves the walker
 * parked instead of walking into the half that is not written yet. Freeze
 * hunt 2026-11-03 (hunt6_4): the walker ran 8 KB through EDGE holes 314 us
 * after its last JUMP and lost step at a 16-byte boundary inside one -- new
 * words before 0x5B6080, the previous lap's after. Cost: one compare per PUT
 * into RSX memory; only hole PUTs (~10^3/s) are reordered. Freeze setting
 * (4 cores, pinned, heavy save, 240 s): 3 freezes in 17 runs without it,
 * 0 in 25 with it. */
static inline int spu_dma_head_is_park(const uint8_t* d, uint32_t ea)
{
    const uint32_t pg = g_gcm_ea_io_tab[ea >> 20];
    if (pg == 0xFFFFu) return 0;
    const uint32_t w = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    return w == (0x20000000u | (pg << 20 | (ea & 0xFFFFFu)));
}
static unsigned long long s_park_head_n; /* 64-bit: ~10^3/s wraps a 32-bit long in ~3 weeks */

/* The offset in [ea, ea+n) of the park the walker stands on, not yet
 * rewritten, or n. Only that one: the ring is full of parks from earlier laps,
 * and pausing on PUTs over those slowed the game to 3 fps and tested nothing. */
static uint32_t spu_dma_find_park(const uint8_t* d, uint32_t n, uint32_t ea)
{
    static const volatile uint32_t* pp;
    if (!pp) { extern const volatile uint32_t* cellGcm_park_ea_ptr(void); pp = cellGcm_park_ea_ptr(); }
    const uint32_t pe = *pp, k = pe - ea;
    if (!pe || k >= n || n - k < 4u) return n;
    const uint32_t pg = g_gcm_ea_io_tab[pe >> 20];
    if (pg == 0xFFFFu) return n;
    const uint32_t w = (uint32_t)d[k] << 24 | (uint32_t)d[k + 1] << 16 | (uint32_t)d[k + 2] << 8 | d[k + 3];
    return w == (0x20000000u | (pg << 20 | (pe & 0xFFFFFu))) ? k : n;
}

static void spu_dma_tear_pause(void)
{
    extern unsigned long long ps3_qpc_us(void);
    const unsigned long long t0 = ps3_qpc_us();
    while (ps3_qpc_us() - t0 < s_tear_us) { }
}

/* GCM_PUT_AUDIT=1 (test only): how the park the walker stands on gets
 * rewritten -- at a PUT's head (what GCM_PUT_ORDER=2 covers), inside a PUT
 * (not covered), by a PUT of 16 bytes or less (one store), or by a 128-byte
 * atomic line write (PUTLLC/PUTLLUC, not through here at all). The atomic
 * writes into RSX memory are counted too. A line at every power of 2. */
static int s_put_audit = -1;
static unsigned long long s_aud_head, s_aud_mid, s_aud_small, s_aud_ll, s_aud_llpark;
static void spu_dma_audit_line(const char* why, uint32_t ea, uint32_t n, uint32_t k)
{
    fprintf(stderr, "[put-audit] %s ea 0x%08X size 0x%X park +0x%X | head %llu mid %llu small %llu atomic-io %llu atomic-park %llu\n",
            why, ea, n, k, s_aud_head, s_aud_mid, s_aud_small, s_aud_ll, s_aud_llpark);
}
static inline int spu_put_audit_on(void)
{
    if (__builtin_expect(s_put_audit < 0, 0)) { const char* e = getenv("GCM_PUT_AUDIT"); s_put_audit = e && *e == '1'; }
    return s_put_audit;
}
static void spu_dma_audit_put(const uint8_t* d, uint32_t n, uint32_t ea, int locked)
{
    const uint32_t k = spu_dma_find_park(d, n, ea);
    if (k >= n) return;
    unsigned long long* c = n <= 16u ? &s_aud_small : k == 0 ? &s_aud_head : &s_aud_mid;
    const unsigned long long v = __atomic_add_fetch(c, 1, __ATOMIC_RELAXED);
    if (!locked && (v <= 8 || (v & (v - 1)) == 0))
        spu_dma_audit_line(c == &s_aud_small ? "small PUT over the park" : c == &s_aud_head ? "park at PUT head" : "park INSIDE a PUT", ea, n, k);
}
static void spu_dma_audit_atomic(const uint8_t* line_before, uint32_t ea)
{
    const uint16_t* t = g_gcm_ea_io_tab;
    if (!t || t[ea >> 20] == 0xFFFFu) return;
    const unsigned long long v = __atomic_add_fetch(&s_aud_ll, 1, __ATOMIC_RELAXED);
    const uint32_t k = spu_dma_find_park(line_before, 128u, ea & ~127u);
    if (k < 128u) { __atomic_add_fetch(&s_aud_llpark, 1, __ATOMIC_RELAXED); spu_dma_audit_line("ATOMIC line write over the park", ea, 128u, k); }
    else if (v <= 8 || (v & (v - 1)) == 0) spu_dma_audit_line("atomic line write into RSX memory", ea, 128u, 128u);
}

void spu_dma_copy_rsx(uint8_t* d, const uint8_t* s, uint32_t n, uint32_t ea, int locked)
{
    if (__builtin_expect(s_put_order < 0, 0)) spu_dma_rsx_env();
    if (__builtin_expect(s_put_audit != 0, 0) && spu_put_audit_on()) spu_dma_audit_put(d, n, ea, locked);
    int order = s_put_order;
    if (order == 2) {
        if (n > 16u && g_gcm_put_order_on && !(n & 15u) && !((uintptr_t)d & 15u) && spu_dma_head_is_park(d, ea)) {
            spu_dma_copy(d + 16, s + 16, n - 16u);
            /* GCM_TEAR_TEST on the fix itself: the pause between the rest
             * and the head (with ",post": after the head) of a PUT over the
             * park the walker stands on -- it must stay parked meanwhile. */
            int pause = 0;
            if (s_tear_every && !locked && spu_dma_find_park(d, n, ea) == 0) {
                const long long c = __atomic_add_fetch(&s_tear_n, 1, __ATOMIC_RELAXED);
                if (c % (long long)s_tear_every == 0) {
                    pause = 1;
                    if (c <= 8 * (long long)s_tear_every || c % (1000 * (long long)s_tear_every) == 0)
                        fprintf(stderr, "[spu-dma] tear test #%lld: park-head PUT ea 0x%08X size 0x%X, pause %s the head\n",
                                c, ea, n, s_tear_post ? "after" : "before");
                }
            }
            if (pause && !s_tear_post) spu_dma_tear_pause();
            uint64_t hi, lo;
            memcpy(&hi, s + 8, 8);
            memcpy(&lo, s, 8);
            __atomic_store_n((uint64_t*)(void*)(d + 8), hi, __ATOMIC_RELEASE);
            __atomic_store_n((uint64_t*)(void*)d, lo, __ATOMIC_RELEASE);
            if (pause && s_tear_post) spu_dma_tear_pause();
            const unsigned long long c = __atomic_add_fetch(&s_park_head_n, 1, __ATOMIC_RELAXED);
            if (!locked && (c <= 4 || (c & (c - 1)) == 0))  /* never print under the lock-line lock */
                fprintf(stderr, "[spu-dma] park-head PUT #%llu written last: ea 0x%08X size 0x%X\n", c, ea, n);
            return;
        }
        if (!s_tear_every) { spu_dma_copy(d, s, n); return; }
        order = 0;                          /* the tear test on the other PUTs: front to back */
    }
    if (!order && !s_tear_every) { spu_dma_copy(d, s, n); return; }
    uint32_t cut = n;                       /* GCM_TEAR_TEST: where to stop, n = nowhere */
    if (s_tear_every && !locked) {
        const uint32_t k = spu_dma_find_park(d, n, ea);
        long long c;
        if (k < n && (c = __atomic_add_fetch(&s_tear_n, 1, __ATOMIC_RELAXED)) % (long long)s_tear_every == 0) {
            cut = (k & ~15u) + 16u;         /* just past the park's 16 bytes */
            if (c <= 8 * (long long)s_tear_every || c % (1000 * (long long)s_tear_every) == 0)
                fprintf(stderr, "[spu-dma] tear test #%lld: PUT ea 0x%08X size 0x%X, park at +0x%X\n", c, ea, n, k);
        }
    }
    if (s_tear_post && cut < n) {           /* the whole PUT, then the pause */
        spu_dma_copy(d, s, n);
        spu_dma_tear_pause();
        return;
    }
    if (!order) {
        if (cut >= n) { spu_dma_copy(d, s, n); return; }
        memcpy(d, s, cut);
        spu_dma_tear_pause();
        memcpy(d + cut, s + cut, n - cut);
        return;
    }
    uint32_t o = n;
    if (o & 15u) {                          /* a DMA under 16 bytes */
        o &= ~15u;
        memcpy(d + o, s + o, n - o);
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
    }
    while (o) {
        if (o == cut) spu_dma_tear_pause();
        o -= 16u;
#if defined(__SSE2__) || defined(_M_X64)
        _mm_storeu_si128((__m128i*)(d + o), _mm_loadu_si128((const __m128i*)(s + o)));
#else
        for (int i = 12; i >= 0; i -= 4) {
            uint32_t w; memcpy(&w, s + o + i, 4);
            __atomic_store_n((uint32_t*)(d + o + i), w, __ATOMIC_RELEASE);
        }
#endif
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
    }
}

void spu_halt(spu_context* ctx)
{
    /* Register/context dump at halt-asserts: the WWS job's parameter guard
     * (heqi 0x1650 in job_wws_7BD900) checks (r12 bit0) & (r9 16-aligned) &
     * (r15 != 0) -- job descriptor/ABI values our execute path must supply.
     * Dump the low registers + the job's parameter area so a firing assert
     * names exactly which input is wrong. */
    { static int _n = 0;
      if (_n < 6 && ctx->status == SPU_STATUS_STOPPED_BY_HALT) { _n++;
        fprintf(stderr, "[spu-halt] img=%d pc=0x%05X regs:", ctx->image_id,
                ctx->pc & SPU_LS_MASK);
        for (int r = 3; r <= 16; r++)
            fprintf(stderr, " r%d=%08X", r, ctx->gpr[r]._u32[0]);
        fprintf(stderr, " r80=%08X r85=%08X\n",
                ctx->gpr[80]._u32[0], ctx->gpr[85]._u32[0]);
        fflush(stderr);
      } }
    if (s_spu_halt_armed) { s_spu_halt_armed = 0; longjmp(s_spu_halt_env, 1); }
}

/* Diagnostic: dump the taskset-policy scheduler's working tables (LS 0x2700..)
 * at func_00000E60 entry, to reverse why it computes "no runnable task". Env
 * YDKJ_E60. Called from the lifted taskset policy. */
void spu_dbg_e60(spu_context* ctx)
{
    static int s_e = -1; if (s_e < 0) s_e = getenv("YDKJ_E60") ? 1 : 0;
    if (!s_e) return;
    static int _d = 0; if (_d++ >= 6) return;
    const uint8_t* L = ctx->ls;
    #define RD(o) (((uint32_t)L[(o)]<<24)|((uint32_t)L[(o)+1]<<16)|((uint32_t)L[(o)+2]<<8)|L[(o)+3])
    fprintf(stderr, "[E60] r20=%08X r24=%08X | run2700=%08X ready2710=%08X pend2720=%08X en2730=%08X sig2740=%08X wait2750=%08X x2770=%08X\n",
            ctx->gpr[20]._u32[0], ctx->gpr[24]._u32[0],
            RD(0x2700), RD(0x2710), RD(0x2720), RD(0x2730), RD(0x2740), RD(0x2750), RD(0x2770));
    #undef RD
    fflush(stderr);
}

/* SPU_STOP0_DIAG (default on; SPU_STOP0_DIAG=0: off). `stop 0` inside a job
 * module is how God of War: Ascension's module 216 command interpreter
 * (0xD100 / 0xD46C, jump table at 0xE850) ends on a command it does not know --
 * opcode above 8 or table index above 61 goes to 0xDB58, `nop; stop 0`. The
 * stop returns (as a policy's stop-and-signal must), the pc is still the stop,
 * and the drain resumes it forever: the drain-resume freeze. What made the
 * command bad is the open question, so the first such stop prints the
 * interpreter's registers and the 64 bytes around its command pointer (r83),
 * and writes the whole local store once to spu_stop0_ovl<ovl>_<pc>.ls in
 * SPU_STOP0_DIR (default: the working directory). Later ones print a short
 * line at the 2nd, 4th, 8th ... Only prints; nothing changes. */
/* The last 64 GETs on this host thread (spu_overlay_note_get sees every one,
 * list elements included): where the command stream at r83 came from. */
static int spu_stop0_on(void)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("SPU_STOP0_DIAG"); s_on = !(e && e[0] == '0'); }
    return s_on;
}
/* rc11: 256 entries (the list diagnostic looks further back), and the walk
 * starts at the newest entry -- (seq - 1 - k); it used to start at the slot
 * about to be overwritten, i.e. the OLDEST, so the "newest first" list began
 * with the oldest GET and an old GET could win over a newer one. */
#define SPU_STOP0_RING 256u
typedef struct { const spu_context* ctx; uint32_t ea, lsa, size, seq; } spu_stop0_get_t;
static SPU_THREAD_LOCAL spu_stop0_get_t t_stop0_gets[SPU_STOP0_RING];
static SPU_THREAD_LOCAL uint32_t t_stop0_seq;
static void spu_stop0_note_get(const spu_context* ctx, uint32_t ea, uint32_t lsa, uint32_t size)
{
    spu_stop0_get_t* g = &t_stop0_gets[t_stop0_seq & (SPU_STOP0_RING - 1u)];
    g->ctx = ctx; g->ea = ea; g->lsa = lsa; g->size = size; g->seq = ++t_stop0_seq;
}
/* The k-th newest GET on this thread (k = 0: the last one). */
static const spu_stop0_get_t* spu_stop0_get_at(uint32_t k)
{
    return &t_stop0_gets[(t_stop0_seq - 1u - k) & (SPU_STOP0_RING - 1u)];
}
/* The GET that brought the bytes at lsa, its main memory compared with the
 * local-store copy now, and that main memory saved next to the LS dump: the
 * same bytes means the stream was bad when fetched (or read at the wrong
 * place); different bytes means it was rewritten after the job fetched it. */
static void spu_stop0_source(const spu_context* ctx, uint32_t lsa, const char* dir, int ovl, uint32_t pc)
{
    extern uint8_t* vm_base;
    const spu_stop0_get_t* hit = NULL;
    for (uint32_t k = 0; k < SPU_STOP0_RING; k++) {
        const spu_stop0_get_t* g = spu_stop0_get_at(k);
        if (!g->seq || g->ctx != ctx) continue;
        if (lsa >= g->lsa && lsa - g->lsa < g->size) { hit = g; break; }
    }
    fprintf(stderr, "[spu] stop 0: last GETs on this thread (newest first):");
    for (uint32_t k = 0, n = 0; k < SPU_STOP0_RING && n < 8; k++) {
        const spu_stop0_get_t* g = spu_stop0_get_at(k);
        if (!g->seq || g->ctx != ctx) continue;
        fprintf(stderr, " ea=0x%08X->ls=0x%05X+0x%X", g->ea, g->lsa, g->size);
        n++;
    }
    fprintf(stderr, "\n");
    if (!hit) { fprintf(stderr, "[spu] stop 0: no recent GET covers LS 0x%05X\n", lsa); return; }
    const uint32_t n = hit->size, ls0 = hit->lsa & SPU_LS_MASK;
    uint32_t diff = 0, first = ~0u;
    for (uint32_t i = 0; i < n && ls0 + i < SPU_LS_SIZE; i++)
        if (ctx->ls[ls0 + i] != vm_base[hit->ea + i]) { diff++; if (first == ~0u) first = i; }
    fprintf(stderr, "[spu] stop 0: LS 0x%05X came from GET ea=0x%08X -> ls=0x%05X size=0x%X (%u GETs ago); "
                    "main memory there now %s",
            lsa, hit->ea, ls0, n, t_stop0_seq - hit->seq,
            diff ? "DIFFERS from the LS copy" : "matches the LS copy");
    if (diff) fprintf(stderr, " (%u bytes, first at +0x%X)", diff, first);
    fprintf(stderr, "\n");
    char path[512];
    snprintf(path, sizeof path, "%s%sspu_stop0_ovl%d_%05X.ea%08X.bin", dir && *dir ? dir : "", dir && *dir ? "/" : "",
             ovl, pc, hit->ea);
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(vm_base + hit->ea, 1, n, f); fclose(f);
             fprintf(stderr, "[spu] stop 0: that main memory written to %s\n", path); }
}

/* SPU_WTRACE: the write history of the job's own descriptor -- the newest
 * GETs on this thread from the traced range that are big enough to be one
 * (>= 0x100 bytes; module 216's are a bytecode stream, a 16-byte header and
 * the DMA list, 0x170-0x200 bytes), up to two, each with every recorded write
 * and read ('G') that touched it, newest first. Shows whether the bytes the
 * job ran on were written before or after its fetch, and by whom. */
#include "spu_wtrace.h"
static void spu_stop0_wtrace_desc(const spu_context* ctx, const char* tag)
{
    if (!g_wtrace_span) return;
    unsigned shown = 0;
    for (uint32_t k = 0; k < SPU_STOP0_RING && shown < 2u; k++) {
        const spu_stop0_get_t* g = spu_stop0_get_at(k);
        if (!g->seq || g->ctx != ctx || g->size < 0x100u || !spu_wtrace_hit(g->ea, g->size)) continue;
        char t[96];
        snprintf(t, sizeof t, "%s desc ea=0x%08X+0x%X (%u GETs ago)", tag, g->ea, g->size, t_stop0_seq - g->seq);
        spu_wtrace_dump(g->ea, g->size, 64u, t);
        shown++;
    }
    if (!shown) fprintf(stderr, "[wtrace] %s: no recent GET of 0x100+ bytes from the traced range\n", tag);
    spu_wtrace_dump_hdr(48u, tag);
}

/* Every other place in guest main memory (below 0x40000000, committed pages)
 * that holds the same aligned quadword: where a bad list element's bytes also
 * live -- the PPU's staging copy of the descriptor, or the command stream the
 * record really belonged to. Up to 12 hits, each with the 64 bytes around it. */
static void spu_find_quad_copies(const uint8_t* q, uint32_t skip_ea, const char* tag)
{
#ifdef _WIN32
    extern uint8_t* vm_base;
    unsigned hits = 0;
    for (uint64_t a = 0x10000u; a < 0x40000000u && hits < 12u;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) break;
        const uint64_t lo = (uint64_t)((uint8_t*)mbi.BaseAddress - vm_base);
        uint64_t hi = lo + mbi.RegionSize;
        if (hi > 0x40000000u) hi = 0x40000000u;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (uint64_t p = (a + 15u) & ~15ull; p + 16u <= hi && hits < 12u; p += 16u) {
                if (memcmp(vm_base + p, q, 16) || (uint32_t)p == skip_ea) continue;
                hits++;
                fprintf(stderr, "[mfc-list] %s: the same 16 bytes also at 0x%08X:", tag, (uint32_t)p);
                for (int k = -32; k < 32; k += 4) {
                    const uint8_t* w = vm_base + p + k;
                    fprintf(stderr, "%s%02X%02X%02X%02X", k == 0 ? " |" : " ", w[0], w[1], w[2], w[3]);
                }
                fprintf(stderr, "\n");
                if (spu_wtrace_hit((uint32_t)p, 16u)) {   /* who wrote that copy */
                    char t[80];
                    snprintf(t, sizeof t, "%s copy@0x%08X", tag, (uint32_t)p);
                    spu_wtrace_dump((uint32_t)p - 16u, 48u, 40u, t);
                }
            }
        }
        a = hi > a ? hi : a + 0x10000u;
    }
    if (!hits) fprintf(stderr, "[mfc-list] %s: the same 16 bytes are nowhere else in main memory\n", tag);
#else
    (void)q; (void)skip_ea; (void)tag;
#endif
}

/* SPU_LIST_DIAG (default on; SPU_LIST_DIAG=0: no prints) and SPU_LIST_BAD_STOP
 * (default 0). mfc_run_list (spu_dma.h) calls this for a DMA-list element
 * real hardware would not run -- why: 1 reserved bits set, 2 size above 16 KB,
 * 4 size not 1/2/4/8 or a multiple of 16, 8 EA not aligned to the size; 2/4/8
 * are DMA alignment errors on hardware. Our runner masks the size to 15 bits,
 * rounds the LS cursor up and goes on, so every later element lands shifted
 * (rc10noirq1: module 216's GETL ran a 0x37E2-byte element, the next chunk
 * landed 0x37F0 bytes late and the interpreter read SPU code -> stop 0).
 * The first 16 print the element and command, the whole list, and the GET
 * that brought the list into LS with that main memory compared now (DIFFERS:
 * it was written after the job fetched it); after that a count line at 32,
 * 64, 128 ... SPU_LIST_BAD_STOP=1: the list ends at an element with why 2/4/8
 * (the rest is not transferred; the tag completes); =2: only that element is
 * skipped. Returns what mfc_run_list does: 0 run it as before, 1 stop, 2 skip.
 * pc is the context's last dispatched pc (lifted code updates it at branches). */
static SPU_THREAD_LOCAL struct { const spu_context* ctx; uint32_t list0, n0, dest0; } t_list_park[32];
void spu_mfc_list_park_note(const spu_context* ctx, uint32_t tag, uint32_t list0, uint32_t n0, uint32_t dest0)
{
    t_list_park[tag & 31u].ctx = ctx;    t_list_park[tag & 31u].list0 = list0;
    t_list_park[tag & 31u].n0 = n0;      t_list_park[tag & 31u].dest0 = dest0;
}
void spu_mfc_list_park_get(const spu_context* ctx, uint32_t tag, uint32_t* list0, uint32_t* n0, uint32_t* dest0)
{
    if (t_list_park[tag & 31u].ctx != ctx) return;   /* parked elsewhere: keep the resume point */
    *list0 = t_list_park[tag & 31u].list0; *n0 = t_list_park[tag & 31u].n0; *dest0 = t_list_park[tag & 31u].dest0;
}
static uint32_t spu_list_rd32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int spu_mfc_list_bad_report(spu_context* ctx, uint32_t list0, uint32_t n0, uint32_t dest0,
                                   uint32_t el, uint32_t word0, uint32_t eal, uint32_t dest,
                                   uint32_t cmd, uint32_t tag, int why)
{
    static int s_diag = -1, s_stop = -1;
    if (s_diag < 0) { const char* e = getenv("SPU_LIST_DIAG"); s_diag = !(e && e[0] == '0'); }
    if (s_stop < 0) {
        const char* e = getenv("SPU_LIST_BAD_STOP");
        const int v = e ? atoi(e) : 0;
        s_stop = (v == 1 || v == 2) ? v : 0;
        if (s_stop) fprintf(stderr, "[mfc-list] SPU_LIST_BAD_STOP=%d: at an element hardware faults on, %s\n",
                            s_stop, s_stop == 1 ? "the list stops" : "that element is skipped");
    }
    const int act = (why & 0xE) ? s_stop : 0;
    if (!s_diag) return act;
    static const char* const acts[3] = { "run as before", "list STOPPED here", "element SKIPPED" };
    static volatile long s_n, s_lock;
    const long n = __atomic_add_fetch(&s_n, 1, __ATOMIC_RELAXED);
    const uint32_t idx = ((el - list0) & SPU_LS_MASK) / 8u;
    if (n > 16) {
        if ((n & (n - 1)) == 0)
            fprintf(stderr, "[mfc-list] BAD element: %ld so far (latest: img=%d ovl=%d cmd=0x%02X elem %u "
                            "word0=%08X ea=%08X why=0x%X, %s)\n",
                    n, ctx->image_id, ctx->resident_ovl, cmd | 4u, idx, word0, eal, (unsigned)why, acts[act]);
        return act;
    }
    while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) { }
    fprintf(stderr, "[mfc-list] BAD element #%ld (why=0x%X:%s%s%s%s; %s): img=%d ovl=%d pc=0x%05X lr=0x%05X "
                    "cmd=0x%02X tag=%u list@0x%05X (%u elems) list lsa=0x%05X; elem %u @0x%05X word0=%08X "
                    "ea=%08X size=0x%X -> ls 0x%05X\n",
            n, (unsigned)why, (why & 1) ? " reserved-bits" : "", (why & 2) ? " size>16K" : "",
            (why & 4) ? " bad-size" : "", (why & 8) ? " misaligned-ea" : "", acts[act],
            ctx->image_id, ctx->resident_ovl, ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
            cmd | 4u, tag & 31u, list0, n0, dest0, idx, el, word0, eal, word0 & 0x7FFFu, dest & SPU_LS_MASK);
    fprintf(stderr, "[mfc-list] BAD element #%ld list (word0:ea, * = the bad one):", n);
    for (uint32_t i = 0; i < n0 && i < 512u; i++) {
        const uint32_t a = (list0 + i * 8u) & SPU_LS_MASK;
        if ((i & 3u) == 0) fprintf(stderr, "\n[mfc-list]   %4u @0x%05X:", i, a);
        fprintf(stderr, " %08X:%08X%s", spu_list_rd32(ctx->ls + a),
                spu_list_rd32(ctx->ls + ((a + 4u) & SPU_LS_MASK)), a == el ? "*" : "");
    }
    fprintf(stderr, "%s\n", n0 > 512u ? "\n[mfc-list]   ... (first 512 shown)" : "");
    if (!spu_stop0_on()) {
        fprintf(stderr, "[mfc-list] BAD element #%ld: GET ring off (SPU_STOP0_DIAG=0), list source unknown\n", n);
    } else {
        extern uint8_t* vm_base;
        const spu_stop0_get_t* hit = NULL;
        for (uint32_t k = 0; k < SPU_STOP0_RING; k++) {
            const spu_stop0_get_t* g = spu_stop0_get_at(k);
            if (!g->seq || g->ctx != ctx) continue;
            if (el >= g->lsa && el - g->lsa < g->size) { hit = g; break; }
        }
        if (!hit) {
            fprintf(stderr, "[mfc-list] BAD element #%ld: no GET among the last %u on this thread covers LS 0x%05X\n",
                    n, SPU_STOP0_RING, el);
        } else {
            const uint32_t g0 = hit->lsa, g1 = hit->lsa + hit->size, lend = list0 + n0 * 8u;
            const uint32_t l0 = list0 > g0 ? list0 : g0, l1 = lend < g1 ? lend : g1;
            uint32_t diff = 0, first = ~0u;
            for (uint32_t a = l0; a < l1 && a < SPU_LS_SIZE; a++)
                if (ctx->ls[a] != vm_base[hit->ea + (a - g0)]) { diff++; if (first == ~0u) first = a; }
            const uint8_t* m = vm_base + hit->ea + (el - g0);
            fprintf(stderr, "[mfc-list] BAD element #%ld: the list came from GET ea=0x%08X -> ls=0x%05X size=0x%X "
                            "(%u GETs ago); main memory there now %s the LS copy of the list",
                    n, hit->ea, g0, hit->size, t_stop0_seq - hit->seq, diff ? "DIFFERS from" : "matches");
            if (diff) fprintf(stderr, " (%u bytes, first at LS 0x%05X)", diff, first);
            if (el + 8u <= g1)
                fprintf(stderr, "; the bad element there now: %08X:%08X\n", spu_list_rd32(m), spu_list_rd32(m + 4));
            else
                fprintf(stderr, "\n");
            if (diff) {   /* the elements that changed, as main memory has them now */
                fprintf(stderr, "[mfc-list] BAD element #%ld: main memory now, changed elements:", n);
                for (uint32_t a = l0 & ~7u, k = 0; a + 8u <= l1 && k < 32u; a += 8u) {
                    const uint8_t* mm = vm_base + hit->ea + (a - g0);
                    if (memcmp(ctx->ls + a, mm, 8) == 0) continue;
                    fprintf(stderr, " %u:%08X:%08X", (a - list0) / 8u, spu_list_rd32(mm), spu_list_rd32(mm + 4));
                    k++;
                }
                fprintf(stderr, "\n");
            }
            /* SPU_WTRACE: every write that touched the bad element's quadword
             * in main memory, newest first -- who put those bytes there. */
            { extern void spu_wtrace_dump(uint32_t, uint32_t, unsigned, const char*);
              char wtag[48];
              snprintf(wtag, sizeof wtag, "BAD element #%ld", n);
              spu_wtrace_dump((hit->ea + (el - g0)) & ~15u, 16u, 32u, wtag);
              /* and of the whole GET the list came in with (the descriptor) */
              snprintf(wtag, sizeof wtag, "BAD element #%ld desc ea=0x%08X+0x%X", n, hit->ea, hit->size);
              spu_wtrace_dump(hit->ea, hit->size, 64u, wtag);
              spu_wtrace_dump_hdr(48u, wtag); }
            { char ftag[48];
              snprintf(ftag, sizeof ftag, "BAD element #%ld", n);
              const uint32_t qea = (hit->ea + (el - g0)) & ~15u;
              spu_find_quad_copies(vm_base + qea, qea, ftag);
              /* The guest r30 of the PPU write that left the quadword: in the
               * batch builder's descriptor copy (func_0046368C) the builder,
               * whose fields hold the staging buffers. */
              /* The source of that write when it was the builder's lvx/stvx
               * copy (r29 = source + 16): who wrote the bytes there. */
              { const uint32_t r29 = spu_wtrace_last_ppu_d(qea, 16u);
                if (r29 >= 0x10090u && spu_wtrace_hit(r29 - 16u, 16u)) {
                    char t[80];
                    snprintf(t, sizeof t, "%s copy source 0x%08X", ftag, r29 - 16u);
                    spu_wtrace_dump(r29 - 0x90u, 0xA0u, 64u, t);   /* r29 steps 0x80 per unrolled block */
                } else if (r29)
                    fprintf(stderr, "[mfc-list] %s: writer r29 0x%08X (outside the traced ranges)\n", ftag, r29); }
              uint32_t b = spu_wtrace_last_ppu_e(qea, 16u);
              { const char* o = getenv("SPU_WTRACE_OBJ");   /* or a given guest object */
                if (o) b = (uint32_t)strtoul(o, NULL, 16); }
              if (b >= 0x10000u && b < 0x3FFFFF00u && mfc_ea_range_committed(b, 0xC0u))
                  for (uint32_t o = 0; o < 0xC0u; o += 16u)
                      fprintf(stderr, "[mfc-list] %s: writer r30 0x%08X +0x%02X: %08X %08X %08X %08X\n", ftag, b, o,
                              spu_list_rd32(vm_base + b + o), spu_list_rd32(vm_base + b + o + 4),
                              spu_list_rd32(vm_base + b + o + 8), spu_list_rd32(vm_base + b + o + 12)); }
        }
    }
    fflush(stderr);
    __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE);
    return act;
}

/* SPU_LIST_REFETCH=1 (default off; SPU_LIST_REFETCH_US: how long to wait for
 * main memory, default 2000). At an element hardware would fault on (why
 * 2/4/8), re-read the aligned quadword holding it from the main memory the
 * list was fetched from -- only when that GET is known (the stop-0 GET ring:
 * SPU_STOP0_DIAG on), the rest of the list there still matches the LS copy
 * byte for byte (the buffer was not reused since), and the quadword there
 * holds well-formed elements. While main memory still holds the bad bytes it
 * waits, up to SPU_LIST_REFETCH_US, for the producer. A producer that wrote
 * the list after the job fetched it, and a store that hit the list in LS after
 * the fetch, both leave in main memory the elements the hardware run would
 * have used (rc10: elements 6-7 of module 216's 18-element GETL list held a
 * 16-byte 216 command instead). Anything else: no change. Returns 1 when the
 * LS bytes were replaced; mfc_run_list then reads the element again. */
static int spu_list_refetch(spu_context* ctx, uint32_t list0, uint32_t n0, uint32_t el)
{
    static int s_on = -1;
    static unsigned s_us;
    if (s_on < 0) {
        const char* e = getenv("SPU_LIST_REFETCH");
        const char* u = getenv("SPU_LIST_REFETCH_US");
        s_us = u ? (unsigned)strtoul(u, NULL, 0) : 2000u;
        s_on = e && e[0] == '1';
        if (s_on) fprintf(stderr, "[mfc-list] SPU_LIST_REFETCH=1: an element hardware faults on is re-read "
                                  "from the main memory its list came from (waiting up to %u us)\n", s_us);
    }
    if (!s_on) return 0;
    extern uint8_t* vm_base;
    extern unsigned long long ps3_qpc_us(void);
    static volatile long s_n, s_done;
    const long n = __atomic_add_fetch(&s_n, 1, __ATOMIC_RELAXED);
    const uint32_t lend = list0 + n0 * 8u;
    const uint32_t r0 = (el & ~15u) > list0 ? (el & ~15u) : list0;      /* the quadword, list part only */
    const uint32_t r1 = (el & ~15u) + 16u < lend ? (el & ~15u) + 16u : lend;
    const char* no = NULL;
    const spu_stop0_get_t* hit = NULL;
    unsigned long long waited = 0;
    uint8_t old[16], got[16];
    memcpy(old, ctx->ls + r0, r1 - r0);
    if (!spu_stop0_on()) no = "GET ring off (SPU_STOP0_DIAG=0)";
    else {
        for (uint32_t k = 0; k < SPU_STOP0_RING; k++) {
            const spu_stop0_get_t* g = spu_stop0_get_at(k);
            if (!g->seq || g->ctx != ctx) continue;
            if (el >= g->lsa && el - g->lsa < g->size) { hit = g; break; }
        }
        if (!hit) no = "no recent GET covers it";
        else if (r0 < hit->lsa || r1 > hit->lsa + hit->size) no = "the quadword spans two GETs";
    }
    if (!no) {   /* the rest of the list, where this GET brought it, unchanged in main memory */
        const uint32_t g0 = hit->lsa, g1 = hit->lsa + hit->size;
        const uint32_t l0 = list0 > g0 ? list0 : g0, l1 = lend < g1 ? lend : g1;
        for (uint32_t a = l0; a < l1 && !no; a++)
            if ((a < r0 || a >= r1) && ctx->ls[a] != vm_base[hit->ea + (a - g0)])
                no = "the rest of the list in main memory changed (buffer reused)";
    }
    if (!no) {   /* well-formed elements there: now, or within the wait */
        const volatile uint8_t* m = vm_base + hit->ea + (r0 - hit->lsa);
        const unsigned long long t0 = ps3_qpc_us();
        for (;;) {
            int good = 1;
            for (uint32_t i = 0; i < r1 - r0; i++) got[i] = m[i];
            for (uint32_t i = 0; i + 8u <= r1 - r0; i += 8u)
                if (mfc_list_elem_bad(spu_list_rd32(got + i), spu_list_rd32(got + i + 4)) & 0xE) good = 0;
            if (good && memcmp(got, old, r1 - r0) != 0) break;
            waited = ps3_qpc_us() - t0;
            if (waited >= s_us) { no = good ? "main memory holds the same bytes" : "main memory is bad too"; break; }
            __builtin_ia32_pause();
        }
    }
    if (!no) {
        memcpy(ctx->ls + r0, got, r1 - r0);
        __atomic_add_fetch(&s_done, 1, __ATOMIC_RELAXED);
    }
    if (n <= 16 || (n & (n - 1)) == 0) {
        fprintf(stderr, "[mfc-list] REFETCH #%ld (%ld re-read so far): img=%d ovl=%d elem %u @0x%05X, LS 0x%05X-0x%05X ",
                n, s_done, ctx->image_id, ctx->resident_ovl, ((el - list0) & SPU_LS_MASK) / 8u, el, r0, r1);
        if (no) fprintf(stderr, "NOT re-read: %s (waited %llu us)\n", no, waited);
        else {
            fprintf(stderr, "re-read from ea=0x%08X after %llu us:", hit->ea + (r0 - hit->lsa), waited);
            for (uint32_t i = 0; i < r1 - r0; i += 4u) fprintf(stderr, " %08X", spu_list_rd32(old + i));
            fprintf(stderr, " ->");
            for (uint32_t i = 0; i < r1 - r0; i += 4u) fprintf(stderr, " %08X", spu_list_rd32(got + i));
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    return no == NULL;
}

/* mfc_run_list's hook for a bad list element: report it (SPU_LIST_DIAG),
 * re-read it (SPU_LIST_REFETCH), else stop or skip (SPU_LIST_BAD_STOP).
 * Returns 0 run it as before, 1 stop the list, 2 skip it, 3 read it again. */
int spu_mfc_list_bad(spu_context* ctx, uint32_t list0, uint32_t n0, uint32_t dest0,
                     uint32_t el, uint32_t word0, uint32_t eal, uint32_t dest,
                     uint32_t cmd, uint32_t tag, int why)
{
    const int act = spu_mfc_list_bad_report(ctx, list0, n0, dest0, el, word0, eal, dest, cmd, tag, why);
    if ((why & 0xE) && spu_list_refetch(ctx, list0, n0, el)) return 3;
    return act;
}

static void spu_stop0_diag(spu_context* ctx)
{
    if (!spu_stop0_on()) return;
    /* rc10: every stop and stopd (code 0x3FFF) in a job module, not only stop 0
     * -- module 216's interpreter also stops with stopd on a zero buffer
     * pointer (0xD594) and at 0xD58C. The first time at each (module, pc,
     * code): the full line, the LS dump (up to 8) and the GET source; after
     * that a count line at the 2nd, 4th, 8th ... stop overall. */
    static long s_n;
    static struct { int ovl; uint32_t pc, code; } s_sites[32];
    static int s_nsites, s_dumps;
    static volatile long s_lock;
    const uint32_t pc = ctx->pc & SPU_LS_MASK;
    const uint32_t code = ctx->stop_code;
    int fresh = 1;
    while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) {}
    for (int i = 0; i < s_nsites; i++)
        if (s_sites[i].ovl == ctx->resident_ovl && s_sites[i].pc == pc && s_sites[i].code == code) { fresh = 0; break; }
    if (fresh && s_nsites < 32) { s_sites[s_nsites].ovl = ctx->resident_ovl; s_sites[s_nsites].pc = pc; s_sites[s_nsites].code = code; s_nsites++; }
    const int dump = fresh && s_dumps < 8;
    if (dump) s_dumps++;
    __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE);
    const long n = __atomic_add_fetch(&s_n, 1, __ATOMIC_ACQ_REL);
    if (!fresh) {
        if (!(n & (n - 1)))
            fprintf(stderr, "[spu] stop in job module: %ld so far (img=%d ovl=%d pc=0x%05X code=0x%X depth=%u)\n",
                    n, ctx->image_id, ctx->resident_ovl, pc, code, (unsigned)ctx->host_depth);
        fflush(stderr);
        return;
    }
    const uint32_t c = ctx->gpr[83]._u32[0] & SPU_LS_MASK & ~15u;
    char bytes[64 * 3 + 8]; int o = 0;
    for (uint32_t k = 0; k < 64; k++) {
        const uint32_t a = (c - 32u + k) & SPU_LS_MASK;
        o += snprintf(bytes + o, sizeof bytes - (size_t)o, "%s%02X", k == 32 ? "|" : (k && !(k & 3) ? " " : ""), ctx->ls[a]);
    }
    const uint8_t* m = ctx->ls + 0x4000;
    fprintf(stderr, "[spu] stop 0x%X in job module (first at this site): img=%d ovl=%d pc=0x%05X depth=%u drain_ret=0x%05X "
                    "module %02X%02X%02X%02X int_enable=%d irq_saved=%d irq_resume=0x%05X srr0=0x%05X irq_frame=%s "
                    "r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X "
                    "r80=%08X r81=%08X r82=%08X r83=%08X r84=%08X r85=%08X r86=%08X r87=%08X "
                    "ls[r83-0x20..r83+0x20]=%s\n",
            code, ctx->image_id, ctx->resident_ovl, pc, (unsigned)ctx->host_depth, (unsigned)(ctx->drain_ret_pc & SPU_LS_MASK),
            m[0], m[1], m[2], m[3], (int)ctx->int_enable, (int)ctx->irq_saved, ctx->irq_resume_pc & SPU_LS_MASK,
            ctx->srr0 & SPU_LS_MASK, ctx->irq_frame ? "armed" : "none",
            ctx->gpr[0]._u32[0], ctx->gpr[1]._u32[0], ctx->gpr[2]._u32[0], ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
            ctx->gpr[80]._u32[0], ctx->gpr[81]._u32[0], ctx->gpr[82]._u32[0], ctx->gpr[83]._u32[0],
            ctx->gpr[84]._u32[0], ctx->gpr[85]._u32[0], ctx->gpr[86]._u32[0], ctx->gpr[87]._u32[0], bytes);
    if (!dump) { fflush(stderr); return; }
    const char* dir = getenv("SPU_STOP0_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s%sspu_stop0_ovl%d_%05X.ls", dir && *dir ? dir : "", dir && *dir ? "/" : "",
             ctx->resident_ovl, pc);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(ctx->ls, 1, SPU_LS_SIZE, f);
        fclose(f);
        char full[600];
#ifdef _WIN32
        if (!_fullpath(full, path, sizeof full))
#endif
            snprintf(full, sizeof full, "%s", path);
        fprintf(stderr, "[spu] stop 0: local store written to %s\n", full);
    } else
        fprintf(stderr, "[spu] stop 0: could not write %s\n", path);
    spu_stop0_source(ctx, c, dir, ctx->resident_ovl, pc);   /* where r83's stream came from */
    spu_stop0_wtrace_desc(ctx, "stop 0");
    fflush(stderr);
}

/* A job module that branched to a `stop` word with no lifted code there
 * (module 216 calling into a batch kernel slot that holds zeros) ends the job
 * as an exit; the batch it belonged to then never completes. The first few:
 * the last GETs, one LS dump and the descriptor's write history (SPU_WTRACE). */
static void spu_job_synth_stop_diag(spu_context* ctx, uint32_t p0)
{
    if (!spu_stop0_on() || !ctx->resident_ovl) return;
    static volatile long s_n;
    const long n = __atomic_add_fetch(&s_n, 1, __ATOMIC_ACQ_REL);
    if (n > 4) return;
    fprintf(stderr, "[spu] job exit at a stop word: img=%d ovl=%d pc=0x%05X lr=0x%05X r3=%08X r80=%08X r83=%08X\n",
            ctx->image_id, ctx->resident_ovl, p0, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
            ctx->gpr[3]._u32[0], ctx->gpr[80]._u32[0], ctx->gpr[83]._u32[0]);
    fprintf(stderr, "[spu] job exit: last GETs on this thread (newest first):");
    for (uint32_t k = 0, m = 0; k < SPU_STOP0_RING && m < 12; k++) {
        const spu_stop0_get_t* g = spu_stop0_get_at(k);
        if (!g->seq || g->ctx != ctx) continue;
        fprintf(stderr, " ea=0x%08X->ls=0x%05X+0x%X", g->ea, g->lsa, g->size);
        m++;
    }
    fprintf(stderr, "\n");
    if (n == 1) {
        const char* dir = getenv("SPU_STOP0_DIR");
        char path[512];
        snprintf(path, sizeof path, "%s%sspu_jobexit_ovl%d_%05X.ls", dir && *dir ? dir : "", dir && *dir ? "/" : "",
                 ctx->resident_ovl, p0);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(ctx->ls, 1, SPU_LS_SIZE, f); fclose(f);
                 fprintf(stderr, "[spu] job exit: local store written to %s\n", path); }
    }
    spu_stop0_wtrace_desc(ctx, "job exit");
    fflush(stderr);
}

/* SPU `stop` / stop-and-signal. A real SPU stop HALTS the core; the previous
 * lifted emission only did `status=...; return;`, which unwound ONE frame and
 * let the caller's service loop keep running -- so a SPURS policy doing
 * stop-and-signal in a loop spun millions of times instead of yielding. Halt the
 * host SPU thread (longjmp to spu_run_with_halt) so the job stops AT the stop,
 * with ctx->status + the outbound mailbox value available for the dispatcher to
 * service. Env YDKJ_STOP_NOHALT restores the old (looping) behavior for A/B. */
void spu_stop(spu_context* ctx)
{
    /* A lifted `stop` sets status and RETURNS to its caller -- for a SPURS
     * policy this is stop-and-signal: it returns up into the kernel/policy
     * service loop, which continues (i.e. the SPU "resumes" past the stop). That
     * resume-by-return behavior is correct; the previous spin was caused by the
     * lack of (a) a PPU listener for the outbound mailbox and (b) mailbox
     * backpressure -- both handled in the channel write path, not here. So by
     * default DO NOT halt. Env YDKJ_STOP_HALT forces a hard halt (longjmp) for
     * A/B experiments (it makes the kernel terminate, which the game restarts). */
    static int s_halt = -1;
    if (s_halt < 0) s_halt = getenv("YDKJ_STOP_HALT") ? 1 : 0;
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    if (ctx->resident_ovl >= 200 && ctx->resident_ovl < 300)
        spu_stop0_diag(ctx);   /* SPU_STOP0_DIAG: every stop/stopd in a job module, prints only */
    if (s_halt) spu_halt(ctx);
}

/* Run a lifted SPU entry with a halt landing pad. Returns 1 if the job halted
 * (via spu_halt), 0 if it returned normally. */
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx)
{
    int halted = 0;
    /* Job-args probe: image-1 (the WWS job binary) entries arrive at the crt
     * with r3/r4 zeroed in SOME runs while the jm2 runner sets them -- count
     * and stamp every image-1 run here to find the argless caller. */
    if (ctx->image_id == 1) {
        static int _n = 0;
        if (_n < 12) { _n++;
            fprintf(stderr, "[rwh] #%d img=1 ctx=%p r3=%08X r4=%08X entry=%p\n",
                    _n, (void*)ctx, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                    (void*)entry);
            fflush(stderr); }
    }
    /* Also discard any saved register file keyed to this context pointer:
     * contexts are stack locals, so the address recurs and a stale save from
     * an earlier run would be restored over this job's arguments. */
    { extern void spu_irq_regs_forget(spu_context*); spu_irq_regs_forget(ctx); }
    ctx->steps = 0;   /* fresh run: step 0 is the entry */
    /* The host stack is empty here by definition, so no lifted caller frame
     * is live. A persistent context (a SPURS policy module re-entered every
     * scheduling quantum) otherwise carries the depth its last quantum
     * halted at: two frames per quantum for an idle job manager, and the
     * recursion guard halted the SPU at 2000 after a few minutes of idling. */
    ctx->host_depth = 0;
    ctx->irq_frame = 0;
    ctx->drain_ret_pc = 0;
    /* Nest-safe: a job run synchronously from inside another SPU's execution
     * re-enters here on the same host thread. The halt target is per-thread,
     * so save the outer one and put it back on the way out -- otherwise the
     * outer run's next halt longjmps into this (by then dead) frame, and a
     * stack-local job context skips its coherency unregister (GH3: crashes in
     * spu_coh_notify_write on a freed stack). */
    jmp_buf outer_env; const int outer_armed = s_spu_halt_armed;
    if (outer_armed) memcpy(outer_env, s_spu_halt_env, sizeof(jmp_buf));
    s_spu_halt_armed = 1;
    g_spu_trampoline_fn = 0;                        /* no stale transfer pending */
    extern SPU_THREAD_LOCAL int g_spu_drain_anyret;
    const int outer_anyret = g_spu_drain_anyret;     /* an interpreter's drain is no frame of this run */
    g_spu_drain_anyret = 0;
    /* SPU_DIRECT (spu_context.h): the top-level drain takes every step; an
     * outer run's drain state comes back at the end. */
    const int dir_on = spu_direct_on();
    spu_direct_state outer_direct = { ~0u, 0u };
    if (dir_on) { outer_direct = t_spu_direct; t_spu_direct.depth = ~0u; }
    /* Lockstep gate (env SPU_LOCKSTEP, default off): join the round-robin
     * ring and BLOCK until this ctx holds the run token, so only one lifted SPU
     * executes at a time. No-op when unarmed. The thread-local halt env above
     * makes a token pause/resume mid-run safe. */
    yz_lockstep_register(ctx);
    /* The SPU's float mode for the run (spu_helpers.h, spu__xs_fa): single
     * precision truncates and flushes denormals, which MXCSR's RZ/FTZ/DAZ do
     * in one instruction; the helpers that need round-to-nearest switch back
     * for their own span. SPU_FAST_FP=0 keeps the standard mode (every float
     * op on the exact path, as before). longjmp restores the mode setjmp saw,
     * so a halt lands in this mode and the restore below still runs. */
#if defined(__x86_64__) || defined(_M_X64)
    static int s_fast_fp = -1;
    if (s_fast_fp < 0) { const char* e = getenv("SPU_FAST_FP"); s_fast_fp = !(e && e[0] == '0'); }
    const unsigned csr_saved = _mm_getcsr();
    if (s_fast_fp) _mm_setcsr(csr_saved | SPU_MXCSR_SPU_BITS);
#endif
    /* spu_drain.c: SPU_DRAIN with the step check inlined (SPU_STEP_INLINE). */
    extern int spu_step_inline(void);
    extern void spu_drain_top(spu_context*);
    switch (setjmp(s_spu_halt_env)) {
    case 1:
        halted = 1;
        g_spu_trampoline_fn = 0;
        if (dir_on) t_spu_direct.depth = ~0u;
        break;
    case 2:
        /* A one-way guest stack reset invalidates every lifted host caller.
         * Re-enter its target with the same guest state on the driver stack. */
        ctx->host_depth = 0;
        g_spu_trampoline_fn = 0;
        g_spu_drain_anyret = 0;
        if (dir_on) t_spu_direct.depth = ~0u;
        spu_indirect_branch(ctx);
        if (spu_step_inline()) spu_drain_top(ctx);
        else SPU_DRAIN(ctx);
        break;
    default:
        spu_jmpbuf_noframe(s_spu_halt_env);
        /* SPU_DRAIN trampoline model: the top-level entry runs until its first
         * cross-function tail transfer, which sets g_spu_trampoline_fn and
         * returns; the drain loop re-enters each queued target until the SPU
         * halts (stop -> longjmp) or the trampoline empties. Nested brsl/bisl
         * calls drain inside their own call brackets (see the lifter). */
        entry(ctx);
        { static int _e = 0; static int s_aw = -1;
          if (s_aw < 0) s_aw = getenv("SPU_ARGWATCH") ? 1 : 0;
          if (s_aw && _e < 8) { _e++;
              fprintf(stderr, "[argwatch] after entry(): img=%d pc=0x%05X "
                      "r2=0x%08X r3=0x%08X r4=0x%08X\n", ctx->image_id,
                      (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[2]._u32[0],
                      ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0]);
              fflush(stderr); } }
        if (spu_step_inline()) spu_drain_top(ctx);
        else SPU_DRAIN(ctx);
    }
#if defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(csr_saved);
#endif
    yz_lockstep_unregister(ctx);   /* leave the ring; hand the token onward */
    s_spu_halt_armed = outer_armed;
    if (outer_armed) memcpy(s_spu_halt_env, outer_env, sizeof(jmp_buf));
    g_spu_drain_anyret = outer_anyret;
    if (dir_on) t_spu_direct = outer_direct;
    return halted;
}

/* ===========================================================================
 * One private MFC engine per live virtual context. Persistent policy contexts
 * outnumber physical SPUs even when their scheduling quanta run sequentially.
 * Registry nodes have stable addresses and survive a quantum, including stalled
 * lists and tag state. The registry lock protects lookup/publication/removal;
 * the context's single execution owner protects its engine after lookup.
 * Release is permitted only after that owner has stopped using the context.
 * ===========================================================================*/
typedef struct spu_mfc_slot {
    spu_context* ctx;
    mfc_engine mfc;
    struct spu_mfc_slot* next;
} spu_mfc_slot;

static spu_mfc_slot* s_mfc_slots;
static SRWLOCK s_mfc_claim_lock = SRWLOCK_INIT;
/* Bumped whenever a slot is freed: invalidates every thread's cache below. */
static volatile long s_mfc_gen = 1;

/* The per-thread last hit of mfc_for, one block (one thread-pointer lookup). */
static SPU_TLS struct { spu_context* ctx; mfc_engine* mfc; long gen; } t_mfc_last;
#define s_ch_fast g_spu_ch_fast   /* exported: spu_wrch_k / spu_rdch_k (spu_context.h) read it */
extern int s_ch_fast;   /* (defined with spu_ch_fast below) */
static __attribute__((noinline)) mfc_engine* mfc_for_full(spu_context* ctx);
/* SPU_CH_FAST: the cached hit inline at the call; everything else as before. */
static inline mfc_engine* mfc_for(spu_context* ctx)
{
    if (__builtin_expect(s_ch_fast > 0, 1)) {
        const long gen = __atomic_load_n(&s_mfc_gen, __ATOMIC_ACQUIRE);
        if (t_mfc_last.ctx == ctx && t_mfc_last.gen == gen) return t_mfc_last.mfc;
    }
    return mfc_for_full(ctx);
}
static __attribute__((noinline)) mfc_engine* mfc_for_full(spu_context* ctx)
{
    /* Each channel write of a DMA command looked its engine up under the claim
     * lock, walking the list -- ~3% of a busy SPU job thread. A context runs
     * on one host thread at a time, so a per-thread last hit is almost always
     * right; the generation drops it when any slot is freed. */
#define t_ctx t_mfc_last.ctx
#define t_mfc t_mfc_last.mfc
#define t_gen t_mfc_last.gen
    const long gen = __atomic_load_n(&s_mfc_gen, __ATOMIC_ACQUIRE);
    if (t_ctx == ctx && t_gen == gen) return t_mfc;
    AcquireSRWLockShared(&s_mfc_claim_lock);
    for (spu_mfc_slot* slot = s_mfc_slots; slot; slot = slot->next) {
        if (slot->ctx == ctx) {
            ReleaseSRWLockShared(&s_mfc_claim_lock);
            t_ctx = ctx; t_mfc = &slot->mfc; t_gen = gen;
            return &slot->mfc;
        }
    }
    ReleaseSRWLockShared(&s_mfc_claim_lock);

    /* A concurrent miss may publish while the read lock is released. Recheck
     * under the writer lock before creating this context's private engine. */
    AcquireSRWLockExclusive(&s_mfc_claim_lock);
    for (spu_mfc_slot* slot = s_mfc_slots; slot; slot = slot->next) {
        if (slot->ctx == ctx) {
            ReleaseSRWLockExclusive(&s_mfc_claim_lock);
            return &slot->mfc;
        }
    }
    spu_mfc_slot* slot = (spu_mfc_slot*)malloc(sizeof(*slot));
    if (!slot) {
        fprintf(stderr, "[SPU] cannot allocate private MFC engine for context %p\n", (void*)ctx);
        fflush(stderr);
        abort(); /* Shared fallback state would silently corrupt DMA completion. */
    }
    slot->ctx = ctx;
    mfc_engine_init(&slot->mfc);
    slot->next = s_mfc_slots;
    s_mfc_slots = slot;
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
    return &slot->mfc;
#undef t_ctx
#undef t_mfc
#undef t_gen
}

void spu_mfc_release(spu_context* ctx)
{
    if (!ctx) return;
    AcquireSRWLockExclusive(&s_mfc_claim_lock);
    for (spu_mfc_slot** link = &s_mfc_slots; *link; link = &(*link)->next) {
        if ((*link)->ctx != ctx) continue;
        spu_mfc_slot* slot = *link;
        *link = slot->next;
        __atomic_add_fetch(&s_mfc_gen, 1, __ATOMIC_RELEASE);
        free(slot);
        break;
    }
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
}

/* Exit safety net for contexts unwound off a departing thread's stack.
 * Pointer comparisons suffice: the abandoned context need not be readable. */
/* The MFC tag groups that have completed, for diagnostics (SPURS_PM_RUNLOG). */
uint32_t spu_mfc_tag_completed(spu_context* ctx) { return mfc_for(ctx)->tag_completed; }

/* For the title's SPU function recorder (spu_iojrn.h): the MFC engine lives
 * outside the context, so a replayed run gets its tag state from here. */
static void spu_mfc_tag_completed_set(spu_context* ctx, uint32_t v) { mfc_for(ctx)->tag_completed = v; }
static const spu_iojrn_rt s_spu_iojrn_rt = { SPU_IOJRN_VERSION, spu_mfc_tag_completed, spu_mfc_tag_completed_set };
__attribute__((constructor)) static void spu_iojrn_rt_announce(void) { g_spu_iojrn_rt = &s_spu_iojrn_rt; }

void spu_mfc_forget_range(uintptr_t lo, uintptr_t hi)
{
    AcquireSRWLockExclusive(&s_mfc_claim_lock);
    for (spu_mfc_slot** link = &s_mfc_slots; *link;) {
        spu_mfc_slot* slot = *link;
        uintptr_t owner = (uintptr_t)slot->ctx;
        if (owner >= lo && owner < hi) {
            *link = slot->next;
            __atomic_add_fetch(&s_mfc_gen, 1, __ATOMIC_RELEASE);
            free(slot);
        } else {
            link = &slot->next;
        }
    }
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
}

/* ===========================================================================
 * Atomic reservation (GETLLAR / PUTLLC / PUTLLUC) -- real lock-line semantics
 *
 * Multiple SPU kernel threads (the SPURS workload runtime runs several SPUs on
 * one shared lock-free queue) issue GETLLAR/PUTLLC on the SAME 128-byte lines.
 * Without honoring the reservation, two SPUs both "claim" the same slot and the
 * queue corrupts (observed: the 2nd claim returns garbage [1,1,1,1] -> the SPU
 * traps). PUTLLC must FAIL when the line changed since GETLLAR. We implement the
 * compare-and-swap under one global lock across all SPU host threads.
 * ===========================================================================*/
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * SPU_PUT_MERGE (spu_dma.h): a PUT into the window writes only the bits the
 * SPU changed since its own GET of those bytes.
 *
 * God of War: Ascension keeps one 16-byte entry per VRAM block (flags at +2:
 * 0x8000 = streamed, top mips not resident) and an SPU job (jobmod
 * e9f3f00d551e) fetches entries, updates its own fields and PUTs them back
 * whole. The PPU sets 0x8000 when it allocates a streamed texture's block; a
 * job whose GET came before that and whose PUT comes after writes the old
 * flags back and the texture loses its streaming state (drawn as garbage at
 * its next descriptor, never streamed in). On the PS3 the job is over in tens
 * of microseconds; here, on a busy 4-6 core PC, the job waits and runs for
 * milliseconds while the PPU is creating a whole area's textures.
 *
 * Per SPU context: a shadow of the window as that SPU last fetched each
 * 16-byte chunk, the local store address it went to and when. A PUT of a
 * chunk this SPU fetched into the same LS address (within 2 s) is merged
 * 8 bytes at a time with a compare-and-swap: new = (mem & ~d) | (ls & d),
 * d = ls ^ fetched. Bytes the SPU did not fetch are copied as before.
 * g_spu_merge_mode 2 = count only (the PUT stays a plain copy): how many PPU
 * bits a plain PUT wrote over ("saved" counters) without changing anything.
 * ---------------------------------------------------------------------------*/
uint32_t g_spu_merge_lo, g_spu_merge_span;
int g_spu_merge_mode = 1;            /* 1 merge, 2 count only */
volatile long long g_spu_merge_puts, g_spu_merge_words_saved, g_spu_merge_flag8000_saved, g_spu_merge_unmatched;
/* how long the SPU held the bytes between its GET and this PUT (ms): sum, count, max (reset by the reporter) */
volatile long long g_spu_merge_win_sum, g_spu_merge_win_n, g_spu_merge_win_max;
typedef struct { const void* spu; uint8_t* volatile shadow; uint32_t* lsa1; uint32_t* t_ms; } spu_merge_ctx;
#define SPU_MERGE_CTX 64
static spu_merge_ctx s_mc[SPU_MERGE_CTX];
static spu_merge_ctx* spu_merge_ctx_of(const void* spu, int create)
{
    for (int i = 0; i < SPU_MERGE_CTX; i++)
        if (s_mc[i].spu == spu) return s_mc[i].shadow ? &s_mc[i] : NULL;
    if (!create) return NULL;
    for (int i = 0; i < SPU_MERGE_CTX; i++)
        if (!s_mc[i].spu && InterlockedCompareExchangePointer((void* volatile*)&s_mc[i].spu, (void*)spu, NULL) == NULL) {
            const uint32_t chunks = (g_spu_merge_span + 15u) / 16u;
            s_mc[i].lsa1 = (uint32_t*)calloc(chunks, 4);
            s_mc[i].t_ms = (uint32_t*)calloc(chunks, 4);
            uint8_t* sh = (uint8_t*)calloc(chunks, 16);
            if (!sh || !s_mc[i].lsa1 || !s_mc[i].t_ms) return NULL;
            MemoryBarrier();
            s_mc[i].shadow = sh;
            return &s_mc[i];
        }
    return NULL;
}
void spu_merge_note_get(const void* spu, uint32_t lsa, uint32_t ea, uint32_t size, const uint8_t* ls)
{
    spu_merge_ctx* m = spu_merge_ctx_of(spu, 1);
    if (!m) return;
    const uint32_t lo = g_spu_merge_lo, hi = g_spu_merge_lo + g_spu_merge_span;
    const uint32_t now = (uint32_t)GetTickCount64();
    const uint32_t a = (ea > lo ? ea : lo) & ~15u, e = ea + size < hi ? ea + size : hi;
    for (uint32_t c = a; c < e; c += 16u) {
        const uint32_t k = (c - lo) / 16u;
        if (c < ea || c + 16u > ea + size) { m->lsa1[k] = 0; continue; }   /* partly fetched: not a base */
        memcpy(m->shadow + (size_t)k * 16u, ls + (c - ea), 16);
        m->lsa1[k] = lsa + (c - ea) + 1u;
        m->t_ms[k] = now;
    }
}
int spu_merge_put(const void* spu, uint32_t lsa, uint32_t ea, uint32_t size, const uint8_t* ls, uint8_t* mem)
{
    spu_merge_ctx* m = spu_merge_ctx_of(spu, 0);
    if (!m || (ea & 15u) || (size & 15u)) { InterlockedIncrement64(&g_spu_merge_unmatched); return 0; }
    const uint32_t lo = g_spu_merge_lo, hi = g_spu_merge_lo + g_spu_merge_span;
    const uint32_t now = (uint32_t)GetTickCount64();
    int any = 0;
    for (uint32_t c = ea; c < ea + size; c += 16u)
        if (c >= lo && c + 16u <= hi) {
            const uint32_t k = (c - lo) / 16u;
            if (m->lsa1[k] == lsa + (c - ea) + 1u && now - m->t_ms[k] < 2000u) {
                any = 1;
                const long long held = (long long)(now - m->t_ms[k]);
                InterlockedAdd64(&g_spu_merge_win_sum, held); InterlockedIncrement64(&g_spu_merge_win_n);
                for (long long mx = g_spu_merge_win_max; held > mx; mx = g_spu_merge_win_max)
                    if (InterlockedCompareExchange64(&g_spu_merge_win_max, held, mx) == mx) break;
                break;
            }
        }
    if (!any) { InterlockedIncrement64(&g_spu_merge_unmatched); return 0; }
    /* TEX_FLAG_PUT_DELAY_US=<us> (stress harness only): hold every matched PUT this long before it lands, as
     * a slow, busy PC does to the job, so the PPU's stores fall between the job's GET and PUT on every load. */
    { static long dly = -1;
      if (dly < 0) { const char* e = getenv("TEX_FLAG_PUT_DELAY_US"); dly = e ? atol(e) : 0; if (dly < 0) dly = 0; }
      if (dly) {
          LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
          if (dly >= 2000) Sleep((DWORD)(dly / 1000 - 1));
          do { QueryPerformanceCounter(&t); SwitchToThread(); } while ((t.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart < dly);
      } }
    const int merge = g_spu_merge_mode == 1;
    extern int  spu_coh_is_reserved(uint32_t);
    extern void spu_coh_notify_write_except(uint32_t, const void*);
    /* The first 0x8000 this PUT would have cleared, printed after the lock-line
     * lock is let go (nothing is printed under it). */
    int log_n = 0; uint32_t log_c = 0; uint64_t log_b = 0, log_cur = 0, log_l = 0; long long log_seq = 0;
    spu_lockline_lock_range(ea, size);
    for (uint32_t c = ea; c < ea + size; c += 16u) {
        const uint32_t o = c - ea;
        const uint32_t k = (c - lo) / 16u;
        const int based = c >= lo && c + 16u <= hi && m->lsa1[k] == lsa + o + 1u && now - m->t_ms[k] < 2000u;
        for (uint32_t w = 0; w < 16u; w += 8u) {
            uint64_t l, b;
            memcpy(&l, ls + o + w, 8);
            volatile uint64_t* p = (volatile uint64_t*)(mem + o + w);
            if (!based) { *p = l; continue; }
            memcpy(&b, m->shadow + (size_t)k * 16u + w, 8);
            const uint64_t d = l ^ b;
            uint64_t cur = *p, nw;
            for (;;) {
                const uint64_t kept = (cur ^ b) & ~d;          /* bits the PPU changed that the SPU did not */
                if (kept) {
                    InterlockedIncrement64(&g_spu_merge_words_saved);
                    /* entry flags at +2 (big-endian halfword in bytes 2-3 of the entry's first 8 bytes):
                     * 0x8000 is bit 7 of byte 2 = bit 23 of the little-endian load */
                    if (w == 0 && (kept & (1ull << 23)) && (cur & (1ull << 23))) {
                        const long long n = InterlockedIncrement64(&g_spu_merge_flag8000_saved);
                        if (n <= 24 && !log_seq) { log_c = c; log_b = b; log_cur = cur; log_l = l; log_seq = n; }
                        log_n++;
                    }
                }
                if (!merge) { *p = l; break; }
                nw = (cur & ~d) | (l & d);
                if (nw == cur) break;
                const uint64_t seen = (uint64_t)InterlockedCompareExchange64((volatile LONG64*)p, (LONG64)nw, (LONG64)cur);
                if (seen == cur) break;
                cur = seen;
            }
        }
        if (based) memcpy(m->shadow + (size_t)k * 16u, ls + o, 16);   /* the SPU's view now */
    }
    for (uint32_t a = ea & ~127u; a < ea + size; a += 128u)
        if (spu_coh_is_reserved(a)) spu_coh_notify_write_except(a, spu);
    spu_lockline_unlock_range(ea, size);
    InterlockedIncrement64(&g_spu_merge_puts);
    if (log_seq)
        fprintf(stderr, "[flag-merge] %s: SPU PUT of entry 0x%08X would clear 0x8000 the PPU set after "
                        "the SPU fetched it (flags fetched 0x%02X%02X, now 0x%02X%02X, SPU wrote 0x%02X%02X; %d in this PUT)\n",
                merge ? "kept" : "LOST (count-only mode)", log_c, (unsigned)((log_b >> 16) & 0xFF), (unsigned)((log_b >> 24) & 0xFF),
                (unsigned)((log_cur >> 16) & 0xFF), (unsigned)((log_cur >> 24) & 0xFF),
                (unsigned)((log_l >> 16) & 0xFF), (unsigned)((log_l >> 24) & 0xFF), log_n);
    return 1;
}

/* The spinlock guarding all atomic line ops now lives in spu_coherency.c as
 * spu_lockline_lock/unlock. It used to be a file-static here, which serialized
 * SPU against SPU but left the PPU free to store into a line in the middle of
 * a PUTLLC's compare-and-commit -- the update the SPU was about to make would
 * be written over, and the SPU would never learn the line had changed. Same
 * lock, same critical sections, now shared with the PPU store paths. */

/* Total PUTLLC attempts (all SPUs). The PM flow trace (spurs_policy.c) reads
 * the delta across one policy run to find the run that performed a claim. */
volatile unsigned g_spu_putllc_count = 0;
/* PUTLLCs that hit the WATCHED sync line (g_barrier_sync_watch) -- isolates the
 * work-run on the stalled job queue from the dozen idle jobmanager instances. */
volatile unsigned g_spu_putllc_sync_hit = 0;

/* Lowest LS address treated as "not job code". Jobs load at 0 and
 * their stacks top out well below this; a branch at or above it with no
 * lifted code is a runaway, not a service call. */
#define SPU_JM2_KERNEL_BASE 0x15000u

/* Link-register value handed to a SPURS job so its final `bi $r0` lands
 * somewhere we recognise. The real job manager passes a return address into
 * itself; we have no manager, so we pass this and treat arriving here as job
 * completion. Chosen above the context/descriptor we park near the top of LS. */
#define SPU_JOB_RETURN_LS   0x3FF00u

/* The last 16 MFC commands of this SPU thread (mfc_submit notes each), printed
 * by SPU_LLAR0_TRAP: what the module fetched just before it went wrong. */
static SPU_THREAD_LOCAL struct { uint32_t cmd, lsa, size, tag, pc, ls0; uint64_t ea; } t_dmaring[16];
static SPU_THREAD_LOCAL unsigned t_dmaring_n;
void spu_dmaring_note(const spu_context* ctx, uint32_t cmd, uint32_t lsa, uint64_t ea,
                      uint32_t size, uint32_t tag)
{
    unsigned i = t_dmaring_n++ & 15u;
    t_dmaring[i].cmd = cmd; t_dmaring[i].lsa = lsa; t_dmaring[i].ea = ea;
    t_dmaring[i].size = size; t_dmaring[i].tag = tag;
    t_dmaring[i].pc = (uint32_t)ctx->pc & SPU_LS_MASK;
    t_dmaring[i].ls0 = 0;
}
void spu_dmaring_dump(void)
{
    unsigned n = t_dmaring_n < 16u ? t_dmaring_n : 16u;
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (t_dmaring_n - n + k) & 15u;
        fprintf(stderr, "[llar0]   dma[-%u] cmd=0x%02X lsa=0x%05X ea=0x%09llX size=0x%X tag=%u pc=0x%05X\n",
                n - k, t_dmaring[i].cmd & 0xFFu, t_dmaring[i].lsa, (unsigned long long)t_dmaring[i].ea,
                t_dmaring[i].size, t_dmaring[i].tag, t_dmaring[i].pc);
    }
}

/* SPU_EVLOG=1 [SPU_EVLOG_WID=<wid>]: the interrupt path of a policy module
 * in order -- stall-notify raised, interrupt taken, stall acknowledged, run
 * begun and ended -- for one workload (wklCurrentId, LS 0x1DC) or all. */
void spu_evlog(const spu_context* ctx, const char* what, uint32_t a, uint32_t b)
{
    static int s_on = -1; static int s_wid = -1;
    if (s_on < 0) { s_on = getenv("SPU_EVLOG") ? 1 : 0;
                    const char* w = getenv("SPU_EVLOG_WID"); s_wid = w ? atoi(w) : -1; }
    if (!s_on || !ctx->policy_mode) return;
    const unsigned wid = ctx->ls[0x1DF];
    if (s_wid >= 0 && wid != (unsigned)s_wid) return;
    static volatile LONG n;
    if (InterlockedIncrement(&n) > 20000) return;
    fprintf(stderr, "[evlog] t=%llu wid=%u spu=%u pc=0x%05X %s 0x%X 0x%X evmask=0x%X evstat=0x%X ie=%u parked=0x%X\n",
            (unsigned long long)GetTickCount64(), wid, ctx->ls[0x1CB], (unsigned)(ctx->pc & SPU_LS_MASK),
            what, a, b, ctx->event_mask, ctx->event_status, ctx->int_enable, ctx->list_stall_mask);
}

/* SPU_LL_INLINE (default on; =0: memcpy/memmove/memcmp calls as before): the
 * 128-byte line copies and the PUTLLC compare under the lock-line lock as
 * eight 16-byte loads and stores. They were library calls (PGO keeps the
 * atomic path cold), each a few ns more of every SPU's and the PPU's wait
 * on that one global lock. Same bytes: each copy loads all eight quadwords
 * before it stores any (memmove semantics, which MSVC's memcpy also has), and
 * the compare is equal exactly when memcmp returns 0. Used only where the LS
 * side of the line lies inside local store (lsa <= SPU_LS_SIZE - 128). */
static int s_ll_inline = -1;
static inline int spu_ll_inline(void)
{
    if (__builtin_expect(s_ll_inline < 0, 0)) { const char* e = getenv("SPU_LL_INLINE"); s_ll_inline = !(e && e[0] == '0'); }
    return s_ll_inline;
}
static inline void spu_ll_copy128(void* d, const void* s)
{
    const __m128i* q = (const __m128i*)s; __m128i* o = (__m128i*)d;
    const __m128i v0 = _mm_loadu_si128(q + 0), v1 = _mm_loadu_si128(q + 1), v2 = _mm_loadu_si128(q + 2), v3 = _mm_loadu_si128(q + 3);
    const __m128i v4 = _mm_loadu_si128(q + 4), v5 = _mm_loadu_si128(q + 5), v6 = _mm_loadu_si128(q + 6), v7 = _mm_loadu_si128(q + 7);
    _mm_storeu_si128(o + 0, v0); _mm_storeu_si128(o + 1, v1); _mm_storeu_si128(o + 2, v2); _mm_storeu_si128(o + 3, v3);
    _mm_storeu_si128(o + 4, v4); _mm_storeu_si128(o + 5, v5); _mm_storeu_si128(o + 6, v6); _mm_storeu_si128(o + 7, v7);
}
/* GETLLAR's pair: the line into local store, and the snapshot from what landed there. */
static inline void spu_ll_copy128x2(void* d1, void* d2, const void* s)
{
    const __m128i* q = (const __m128i*)s; __m128i* o1 = (__m128i*)d1; __m128i* o2 = (__m128i*)d2;
    __m128i v[8];
    for (int k = 0; k < 8; k++) v[k] = _mm_loadu_si128(q + k);
    for (int k = 0; k < 8; k++) _mm_storeu_si128(o1 + k, v[k]);
    for (int k = 0; k < 8; k++) _mm_storeu_si128(o2 + k, v[k]);
}
static inline int spu_ll_eq128(const void* a, const void* b)
{
    const __m128i* p = (const __m128i*)a; const __m128i* q = (const __m128i*)b;
    __m128i x = _mm_xor_si128(_mm_loadu_si128(p), _mm_loadu_si128(q));
    for (int k = 1; k < 8; k++) x = _mm_or_si128(x, _mm_xor_si128(_mm_loadu_si128(p + k), _mm_loadu_si128(q + k)));
    return _mm_movemask_epi8(_mm_cmpeq_epi8(x, _mm_setzero_si128())) == 0xFFFF;
}

/* Returns 1 if `cmd` is an atomic line op and was handled here, else 0. */
static int spu_mfc_atomic(spu_context* ctx, uint32_t cmd)
{
    /* Classify FIRST. Every MFC_Cmd write lands here, and only the switch at
     * the bottom used to filter -- so the uncommitted-EA guard below reported
     * ORDINARY DMA as `[spu-atomic]`. A plain put (0x20) to a garbage EA read
     * as an atomic spin, which is a genuinely misleading place to start
     * debugging from. Non-atomic commands belong to the DMA engine. */
    if (cmd != MFC_GETLLAR_CMD && cmd != MFC_PUTLLC_CMD &&
        cmd != MFC_PUTLLUC_CMD && cmd != MFC_PUTQLLUC_CMD)
        return 0;

    uint32_t ea  = ctx->mfc_eal & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK;
    uint8_t* ls  = &ctx->ls[lsa];
    uint8_t* mem = vm_base + ea;

    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_n = 0; static uint32_t s_lastea = 0;
        if ((++s_n % 2000000) == 0 || ea != s_lastea) {
          if ((s_n % 2000000) == 0)
            fprintf(stderr, "[atomcnt] %llu atomic ops; last cmd=0x%X ea=0x%08X\n",
                    (unsigned long long)s_n, cmd, ea);
          s_lastea = ea; } } }
    /* SPU_ATOMTRACE=1 keeps the old 40-line cap; =<N> raises it. A busy image
     * (FMOD) exhausts 40 before a quieter one issues its first atomic, which
     * reads as "that image never does atomics" when it simply never got a line. */
    { static int s_at = -1;
      if (s_at < 0) { const char* e = getenv("SPU_ATOMTRACE");
                      int v = e ? atoi(e) : 0; s_at = e ? (v > 1 ? v : 40) : 0; }
      if (s_at) { static int _a=0; if (_a < s_at && ++_a)
        fprintf(stderr, "[atom] cmd=0x%02X ea=0x%08X (img=%d)\n", cmd, ea, ctx->image_id); } }
    /* cri task (img22) atomic on the taskset: dump the loaded bitset line so we can
     * see if the task reads MY taskset (0x4005E000) with my READY bit, or elsewhere. */
    { static int s_ct=-1; if(s_ct<0) s_ct=getenv("SPU_ATOMTRACE")?1:0;
      if(s_ct && ctx->image_id==22 && cmd==0xD0 && mfc_ea_range_committed(ea,16)) {
        static int _c=0; if(_c < 24 && ++_c){
          uint8_t* m=vm_base+ea;
          #define BW(o) (((uint32_t)m[o]<<24)|((uint32_t)m[o+1]<<16)|((uint32_t)m[o+2]<<8)|m[o+3])
          fprintf(stderr,"[cri-atom] GETLLAR ea=0x%08X line[0..0x30]: %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X\n",
            ea, BW(0),BW(4),BW(8),BW(0xC), BW(0x10),BW(0x14),BW(0x18),BW(0x1C), BW(0x20),BW(0x24),BW(0x28),BW(0x2C));
          #undef BW
        } } }
    /* YDKJ_CRI_R4: dump the CellSpursTaskset bitsets when the policy atomically
     * touches my taskset (0x0F000000), to watch the task-activation state machine
     * (why task0 isn't selected+first-run). running@0 ready@0x10 pending@0x20
     * enabled@0x30 signalled@0x40 waiting@0x50 (each 16B; word0 = MSB, task0=bit127). */
    { static int s_td = -1; if (s_td < 0) s_td = (getenv("YDKJ_CRI_CHAIN") && getenv("SPU_ATOMTRACE")) ? 1 : 0;
      if (s_td && ea >= 0x0F000000u && ea < 0x0F001900u) {
        extern uint8_t* vm_base;
        static int _t=0; if (vm_base && _t++ < 24) {
            uint8_t* t = vm_base + 0x0F000000u;
            #define TW(o) (((uint32_t)t[o]<<24)|((uint32_t)t[o+1]<<16)|((uint32_t)t[o+2]<<8)|t[o+3])
            fprintf(stderr, "[tset] %s run=%08X rdy=%08X pnd=%08X ena=%08X sig=%08X wait=%08X | wid=%08X last=%02X\n",
                    cmd==0xD0?"GET":cmd==0xB4?"PUT":"?", TW(0x00), TW(0x10), TW(0x20), TW(0x30), TW(0x40), TW(0x50), TW(0x74), t[0x73]);
            #undef TW
        }
      } }

    /* Guard atomic line ops against an uncommitted/garbage EA (e.g. a SPURS
     * policy computing a lock-line address from an incomplete instance context).
     * Same rationale as the DMA EA guard: a bad guest atomic must not segfault
     * the host. GETLLAR returns a zeroed line (no reservation); PUTLLC fails. */
    if (!mfc_ea_range_committed(ea, MFC_ATOMIC_LINE)) {
        static int s_w = 0;
        if (s_w++ < 16)
            fprintf(stderr, "[spu-atomic] cmd=0x%X ea=0x%08X pc=0x%05X img=%d uncommitted -- skipped\n",
                    cmd, ea, (uint32_t)ctx->pc & SPU_LS_MASK, ctx->image_id);
        if (cmd == MFC_GETLLAR_CMD) {
            memset(ls, 0, MFC_ATOMIC_LINE);
            ctx->resv_ea = ea; ctx->resv_valid = 0; ctx->atomic_stat = 4;
        } else {
            ctx->atomic_stat = 1;   /* PUTLLC failure (line "moved") */
        }
        return 1;
    }

    /* SPU_ATOM_EA=<hex>: log every lock-line atomic touching that EA`s 128B line.
     * SPU_ATOM_FULL=1 additionally dumps 32 bytes of the line before and after.
     *
     * Built into ONE buffer and written with a single fprintf: several SPU host
     * threads and the PPU all write stderr, so a dump split across many fprintf
     * calls comes out shredded and unreadable exactly when it matters. */
    { static int s_ae = -1; static uint32_t s_aea;
      if (s_ae < 0) { const char* e = getenv("SPU_ATOM_EA");
        s_aea = e ? (uint32_t)strtoul(e, 0, 16) & ~127u : 0; s_ae = s_aea ? 1 : 0;
        if (s_ae == 1) { extern uint32_t g_barrier_sync_watch;
            g_barrier_sync_watch = s_aea; } /* arm PUTLLC OK/FAIL verdict log */ }
      if (s_ae == 1 && ((uint32_t)ea & ~127u) == s_aea) {
          static int _n = 0, s_cap = -1;
          /* SPU_ATOM_EA_MAX=<n>: lines to print (default 48, 0 = unlimited). */
          if (s_cap < 0) { const char* m = getenv("SPU_ATOM_EA_MAX"); s_cap = m ? atoi(m) : 48; }
          if (!s_cap || _n++ < s_cap) {
              extern uint8_t* vm_base;
              const uint8_t* r = vm_base + ((uint32_t)ea & ~127u);
              static int s_af = -1;
              if (s_af < 0) { const char* f = getenv("SPU_ATOM_FULL"); s_af = f ? (atoi(f) > 1 ? 2 : 1) : 0; }
              char buf[1024]; int p = 0;
              /* lr (gpr[0]) as well as pc: these atomics sit in generic helpers,
               * so pc names the HELPER and only lr names the real caller. */
              p += snprintf(buf + p, sizeof buf - p,
                            "[atom-ea] cmd=0x%X img=%d pc=0x%05X lr=0x%05X ea=0x%08X",
                            cmd, ctx->image_id, (uint32_t)ctx->pc & SPU_LS_MASK,
                            ctx->gpr[0]._u32[0] & SPU_LS_MASK, (uint32_t)ea);
              /* Under a policy module, which workload (wklCurrentId, LS 0x1DC)
               * and SPU (spuNum, LS 0x1C8): one image serves many workloads. */
              if (ctx->policy_mode)
                  p += snprintf(buf + p, sizeof buf - p, " wid=%u spu=%u", ctx->ls[0x1DF], ctx->ls[0x1CB]);
              int nb = s_af == 2 ? 128 : s_af ? 64 : 8;   /* 64 covers +0x30 pendingRecv; =2 the whole line */
              p += snprintf(buf + p, sizeof buf - p, " RAM=");
              for (int i = 0; i < nb && p < (int)sizeof buf - 4; i++)
                  p += snprintf(buf + p, sizeof buf - p, "%02X%s", r[i],
                                (i % 4 == 3) ? " " : "");
              if (cmd == MFC_PUTLLC_CMD) {
                  p += snprintf(buf + p, sizeof buf - p, " STORE=");
                  for (int i = 0; i < nb && p < (int)sizeof buf - 4; i++)
                      p += snprintf(buf + p, sizeof buf - p, "%02X%s", ls[i],
                                    (i % 4 == 3) ? " " : "");
              }
              fprintf(stderr, "%s\n", buf); fflush(stderr);
          }
      } }
    /* Bink sync-line atomic trace (armed by the PPU-side producer probe). */
    { extern uint32_t g_barrier_sync_watch;
      uint32_t b = g_barrier_sync_watch;
      if (b && ea >= (b & ~127u) && ea < ((b + 0xC0 + 127) & ~127u)) {
          static int _n = 0;
          if (_n++ < 64)
              fprintf(stderr, "[sync-atomic] %s img=%d pc=0x%05X ea=0x%08X\n",
                      cmd == MFC_GETLLAR_CMD ? "GETLLAR" :
                      cmd == MFC_PUTLLC_CMD ? "PUTLLC" : "PUTLLUC",
                      ctx->image_id, (uint32_t)ctx->pc, ea);
      } }
    /* EDGE6 (ascension host/spu/wip/edge6/edge6.c): sees every atomic line op
     * with a committed EA and may move the EA of an EDGE per-SPU output record
     * (base + 128 + spuNum*128) onto another line. 0 = not installed. */
    if (g_edge6_atomic_hook) {
        uint32_t nea = g_edge6_atomic_hook(ctx, cmd, ea, lsa);
        if (nea != ea && mfc_ea_range_committed(nea, MFC_ATOMIC_LINE)) { ea = nea; mem = vm_base + ea; }
    }
    switch (cmd) {
    case MFC_GETLLAR_CMD:
        /* Lockstep tick: a guest GETLLAR..PUTLLC poll loop is intra-function
         * (all gotos), so it never crosses the SPU_DRAIN tick site -- without a
         * tick here a polling SPU would hold the run token forever while the
         * peer that must WRITE the line waits for it (canersaka ticks the
         * GETLLAR fast+slow paths for exactly this reason). */
        yz_lockstep_tick(ctx);
        /* SPU_LLAR0_TRAP=1: the first GETLLAR of EA 0 from each SPU, with every
         * register's preferred word and the return addresses saved on the SPU
         * stack. Nothing reserves EA 0; a module that does has computed its
         * line from a field that was never filled. */
        if (ea == 0) {
            static int s_t = -1;
            if (s_t < 0) s_t = getenv("SPU_LLAR0_TRAP") ? 1 : 0;
            static SPU_THREAD_LOCAL int t_done;
            if (s_t && !t_done) {
                t_done = 1;
                fprintf(stderr, "[llar0] img=%d spu%u pc=0x%05X lr=0x%05X lsa=0x%05X ls4000=%02X%02X%02X%02X\n",
                        ctx->image_id, (unsigned)(ctx->spu_id & 7u), (unsigned)(ctx->pc & SPU_LS_MASK),
                        ctx->gpr[0]._u32[0] & SPU_LS_MASK, lsa,
                        ctx->ls[0x4000], ctx->ls[0x4001], ctx->ls[0x4002], ctx->ls[0x4003]);
                for (int r = 0; r < 128; r += 8)
                    fprintf(stderr, "[llar0]   r%-3d %08X %08X %08X %08X %08X %08X %08X %08X\n", r,
                            ctx->gpr[r]._u32[0], ctx->gpr[r+1]._u32[0], ctx->gpr[r+2]._u32[0], ctx->gpr[r+3]._u32[0],
                            ctx->gpr[r+4]._u32[0], ctx->gpr[r+5]._u32[0], ctx->gpr[r+6]._u32[0], ctx->gpr[r+7]._u32[0]);
                { extern void spu_dmaring_dump(void); spu_dmaring_dump(); }
                static const uint32_t s_ls_spans[][2] = { {0x80, 0x40}, {0x100, 0x80}, {0x1C0, 0x20} };
                for (unsigned s = 0; s < sizeof s_ls_spans / sizeof s_ls_spans[0]; s++)
                    for (uint32_t o = s_ls_spans[s][0]; o < s_ls_spans[s][0] + s_ls_spans[s][1]; o += 16) {
                        const uint8_t* q = ctx->ls + o;
                        fprintf(stderr, "[llar0]   LS %05X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n", o,
                                q[0],q[1],q[2],q[3], q[4],q[5],q[6],q[7], q[8],q[9],q[10],q[11], q[12],q[13],q[14],q[15]);
                    }
                /* SPU ABI frames: the back chain at 0($sp), the caller's link
                 * saved at 16 bytes into the caller's frame. */
                uint32_t sp = ctx->gpr[1]._u32[0] & SPU_LS_MASK;
                for (int d = 0; d < 12 && sp && sp < 0x40000u - 32; d++) {
                    uint32_t back = __builtin_bswap32(*(const uint32_t*)(ctx->ls + sp));
                    uint32_t link = back && back < 0x40000u - 32
                                  ? __builtin_bswap32(*(const uint32_t*)(ctx->ls + back + 16)) : 0;
                    fprintf(stderr, "[llar0]   frame sp=0x%05X back=0x%05X link=0x%05X\n", sp, back, link);
                    if (back <= sp) break;
                    sp = back;
                }
                fflush(stderr);
            }
        }
        SPU_LL_HINT(ea, 1);
        const uint64_t ll_held = spu_coh_getllar_lock(ctx, ea);   /* SPU_LL_STRIPE */
        /* Tell the PPU store paths this line is live, so a store into it goes
         * through the lock and raises SPU_EVENT_LR here instead of landing
         * unannounced. Nothing else about this transaction changes. */
        spu_coh_reserve(ctx, ea);

        /* ONE read of guest memory, then the snapshot from that copy.
         *
         * This used to memcpy from `mem` twice -- once to the local store, once
         * to resv_line -- and a PPU store landing BETWEEN the two reads gave the
         * SPU a stale line and a fresh snapshot. That is a permanent deadlock,
         * and it is the one this port spent a session chasing: the SPU compares
         * its (stale) copy, finds produced == consumed, and sleeps on
         * MFC_LLR_LOST_EVENT; spu_resv_lost_poll then compares memory against a
         * snapshot that ALREADY has the new value, finds no difference, and
         * never raises the event. Measured at a freeze: mirror 0x9E, snapshot
         * 0x9F, memory 0x9F, and the SPU polling rchcnt 1.6 billion times.
         *
         * Hardware cannot produce that state: GETLLAR is a single atomic
         * 128-byte read, so the reserved data and the reservation come from the
         * same instant. Copying the snapshot from `ls` restores that property,
         * and a store that lands after the read now leaves BOTH stale -- which
         * is what makes the lost-reservation event fire. */
        const int ll_inline = spu_ll_inline() && lsa <= SPU_LS_SIZE - MFC_ATOMIC_LINE;   /* SPU_LL_INLINE */
        if (ll_inline) spu_ll_copy128x2(ls, ctx->resv_line, mem);   /* both copies below, at once */
        else
        memcpy(ls, mem, MFC_ATOMIC_LINE);              /* line -> local store */
        /* Snapshot from `ls`, NOT a second read of `mem`. Hardware GETLLAR is a
         * single atomic 128-byte read, so the reserved data and the reservation
         * come from the same instant. Reading guest memory twice lets a PPU store
         * land between them, leaving the SPU a STALE line and a FRESH snapshot --
         * and then both halves of the wait fail: the SPU compares its stale copy,
         * sees produced == consumed and sleeps on MFC_LLR_LOST_EVENT, while the
         * reservation poll compares memory against a snapshot that already holds
         * the new value, finds no difference, and never raises the event. Nobody
         * wakes anybody. Snapshotting from `ls` makes a later store leave BOTH
         * stale, which is exactly what makes the lost-reservation event fire. */
        if (!ll_inline)
        memcpy(ctx->resv_line, ls, MFC_ATOMIC_LINE);   /* snapshot for compare */
        /* MFC_RdAtomicStat after a GETLLAR reads 4 (GETLLAR complete), not 0:
         * Havok's SPU allocator (GH3 collide task, LS 0xA66C) re-issues the
         * GETLLAR until that bit is set, and spun forever on 0. PUTLLUC is 2. */
        ctx->resv_ea = ea; ctx->resv_valid = 1; ctx->atomic_stat = 4;
        ctx->dbg_getllar++;
        /* A GETLLAR re-issued millions of times on one line is an SPU spin-wait
         * that nobody is releasing (a ticket lock, a counter barrier). Say so
         * once per line with the line's head, so the stuck word is visible
         * without a per-function probe. */
        { static SPU_THREAD_LOCAL uint32_t t_ea; static SPU_THREAD_LOCAL unsigned long t_n;
          if (ea != t_ea) { t_ea = ea; t_n = 0; }
          else if (++t_n == 4000000) {
              const uint8_t* m = vm_base + ea;
              fprintf(stderr, "[spu-spin] img=%d pc=0x%05X lr=0x%05X GETLLAR ea=0x%08X x4M; line:"
                              " %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                      ctx->image_id, (unsigned)(ctx->pc & SPU_LS_MASK), ctx->gpr[0]._u32[0] & SPU_LS_MASK, ea,
                      m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]);
          } }
        spu_coh_unlock_tok(ll_held);
        /* SPU_LLARWATCH=<hex EA>: every GETLLAR of that line, with the LSA it
         * used and the first word as it lands in BOTH places.
         *
         * The deadlock this settles: at a freeze the reservation snapshot holds
         * the fresh produced value (0xC9) while the SPU's local-store mirror
         * still holds the stale one (0xC8) -- and the compare reads the mirror.
         * Both memcpys above copy from the same `mem`, so that can only happen
         * if `lsa` was not where the SPU asked. Reading ctx->mfc_lsa from a
         * later poll cannot prove it (by then the SPU has issued other DMA and
         * overwritten the field, which is exactly the compare-two-moments
         * mistake that has cost this port ten retractions). So log it HERE, at
         * the GETLLAR, with the word that actually landed. */
        { static int s_lw2 = -2; static uint32_t s_lwea;
          if (s_lw2 == -2) { const char* e6 = getenv("SPU_LLARWATCH");
                             s_lw2 = e6 ? 1 : 0;
                             s_lwea = e6 ? (uint32_t)strtoul(e6, 0, 16) & ~127u : 0u; }
          if (s_lw2 && (ea & ~127u) == s_lwea) {
              static unsigned long ln;
              /* A histogram of the LSAs this line's GETLLARs use, not a
               * sample of them. resv_line has exactly one writer -- this
               * GETLLAR -- and it copies to &ls[mfc_lsa] from the same source,
               * so a snapshot that is fresh while LS 0x10800 is stale can only
               * mean some GETLLAR ran with a DIFFERENT mfc_lsa. Sampling every
               * 256th hit would miss exactly those. Count them all and print
               * every LSA seen. */
              { static uint32_t seen[8]; static unsigned long cnt[8]; static int ns;
                int f = -1;
                for (int z = 0; z < ns; z++) if (seen[z] == lsa) { f = z; break; }
                if (f < 0 && ns < 8) { f = ns; seen[ns] = lsa; cnt[ns] = 0; ns++; }
                if (f >= 0) cnt[f]++;
                if ((ln % 4096) == 0) {
                    fprintf(stderr, "[llar-lsa] %lu GETLLARs on 0x%08X;", ln, ea);
                    for (int z = 0; z < ns; z++)
                        fprintf(stderr, " lsa=0x%05X x%lu", seen[z], cnt[z]);
                    fprintf(stderr, "\n"); fflush(stderr);
                } }
              if (++ln <= 8 || (ln % 256) == 0) {
                  const uint8_t* q = (const uint8_t*)ls;
                  const uint32_t landed = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16)
                                        | ((uint32_t)q[2] << 8) | q[3];
                  const uint8_t* m8 = (const uint8_t*)mem;
                  const uint32_t frommem = ((uint32_t)m8[0] << 24) | ((uint32_t)m8[1] << 16)
                                         | ((uint32_t)m8[2] << 8) | m8[3];
                  const uint8_t* mir = ctx->ls + 0x10800u;
                  const uint32_t mirw = ((uint32_t)mir[0] << 24) | ((uint32_t)mir[1] << 16)
                                      | ((uint32_t)mir[2] << 8) | mir[3];
                  fprintf(stderr, "[llar] n=%lu spu%u pc=0x%05X lsa=0x%05X"
                                  " ea=0x%08X mem=0x%08X landed=0x%08X"
                                  " mirror[0x10800]=0x%08X r90=0x%08X\n",
                          ln, (unsigned)(ctx->spu_id & 7u),
                          (uint32_t)ctx->pc & SPU_LS_MASK, lsa, ea,
                          frommem, landed, mirw, ctx->gpr[90]._u32[0]);
                  fflush(stderr);
              }
          } }
        return 1;

    case MFC_PUTLLC_CMD:
        /* g_spu_putllc_count is read nowhere (spurs_policy.c only names it), and
         * its increment from every SPU thread kept its cache line -- shared with
         * g_spu_put_hook, read on every PUT -- moving between cores (~0.3-0.5% of
         * SPU thread time in the heavy fight). Not counted any more. */
        { extern uint32_t g_barrier_sync_watch;
          uint32_t b = g_barrier_sync_watch;
          if (b && (ea & ~127u) == (b & ~127u)) g_spu_putllc_sync_hit++; }
        SPU_LL_HINT(ea, 2);
        spu_lockline_lock_ea(ea);   /* SPU_LL_STRIPE: this line only */
        if (ctx->resv_valid && ctx->resv_ea == ea &&
            (spu_ll_inline() ? spu_ll_eq128(mem, ctx->resv_line)
                             : memcmp(mem, ctx->resv_line, MFC_ATOMIC_LINE) == 0)) {   /* SPU_LL_INLINE */
            { extern void spu_lockguard_check(spu_context*, uint32_t, const uint8_t*, uint32_t, const char*);
              spu_lockguard_check(ctx, ea, ls, MFC_ATOMIC_LINE, "PUTLLC"); }
            if (__builtin_expect(s_put_audit > 0, 0)) spu_dma_audit_atomic((const uint8_t*)mem, ea);   /* GCM_PUT_AUDIT */
            /* SPU_COMMIT_EA=<hex>: every committed PUTLLC on that line, under
             * the lock-line lock: the words that change, before and after. */
            { static int s_ce = -2; static uint32_t s_cl;
              if (s_ce == -2) { const char* e = getenv("SPU_COMMIT_EA");
                                s_cl = e ? (uint32_t)strtoul(e, 0, 16) & ~127u : 0; s_ce = s_cl ? 1 : 0; }
              if (s_ce && (ea & ~127u) == s_cl) {
                  char buf[512]; int p = snprintf(buf, sizeof buf, "[commit] img=%d pc=0x%05X ea=0x%08X",
                                                  ctx->image_id, (unsigned)(ctx->pc & SPU_LS_MASK), (uint32_t)ea);
                  for (int w = 0; w < 32 && p < (int)sizeof buf - 40; w++) {
                      uint32_t o, n; memcpy(&o, (const uint8_t*)mem + 4*w, 4); memcpy(&n, (const uint8_t*)ls + 4*w, 4);
                      if (o != n) p += snprintf(buf + p, sizeof buf - p, " +%02X:%08X->%08X", 4*w,
                                                __builtin_bswap32(o), __builtin_bswap32(n));
                  }
                  fprintf(stderr, "%s\n", buf);
              } }
            /* SPU_COH_LIVE: PPU plain stores into a line nobody held a
             * reservation on skip this lock (spu_coherency.c), so one that
             * took its look just before this reservation began can land
             * between the compare above and this commit. Write back only what
             * the SPU changed -- per 8-byte chunk, the smallest aligned unit
             * holding the changed bytes -- so such a store survives unless the
             * SPU changed the same unit. Otherwise the whole line, as before;
             * with every writer locked out the two are the same. */
            if (spu_coh_live_on()) {
                uint8_t* m8 = (uint8_t*)mem;
                const uint8_t* l8 = (const uint8_t*)ls;
                const uint8_t* r8 = (const uint8_t*)ctx->resv_line;
                for (unsigned c = 0; c < MFC_ATOMIC_LINE; c += 8) {
                    uint64_t lv, rv; memcpy(&lv, l8 + c, 8); memcpy(&rv, r8 + c, 8);
                    if (lv == rv) continue;
                    unsigned lo = 8, hi = 0;
                    for (unsigned b = 0; b < 8; b++)
                        if (l8[c + b] != r8[c + b]) { if (b < lo) lo = b; hi = b; }
                    unsigned sz = 1;
                    while ((lo & ~(sz - 1)) + sz <= hi) sz <<= 1;   /* aligned unit covering lo..hi */
                    const unsigned o = c + (lo & ~(sz - 1));
                    switch (sz) {
                    case 1: m8[o] = l8[o]; break;
                    case 2: { uint16_t v; memcpy(&v, l8 + o, 2); __atomic_store_n((uint16_t*)(m8 + o), v, __ATOMIC_RELAXED); } break;
                    case 4: { uint32_t v; memcpy(&v, l8 + o, 4); __atomic_store_n((uint32_t*)(m8 + o), v, __ATOMIC_RELAXED); } break;
                    default: __atomic_store_n((uint64_t*)(m8 + c), lv, __ATOMIC_RELAXED); break;
                    }
                }
            } else if (spu_ll_inline() && lsa <= SPU_LS_SIZE - MFC_ATOMIC_LINE)   /* SPU_LL_INLINE */
                spu_ll_copy128(mem, ls);               /* commit local store */
            else
            memcpy(mem, ls, MFC_ATOMIC_LINE);          /* commit local store */
            /* SPU_WTRACE (PUTLLC): commits into a traced range only (it used to
             * record every PUTLLC anywhere, which flooded the ring with the
             * kernel's own lines); on the second range, also report a commit
             * that changed another SPU's Wws minActive slot. */
            if (g_wtrace_span && spu_wtrace_hit(ea & ~127u, MFC_ATOMIC_LINE)) {
              spu_wtrace_rec('A', ea & ~127u, MFC_ATOMIC_LINE, ls, (uint32_t)ctx->image_id, (uint32_t)ctx->resident_ovl, (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK, NULL);
              spu_wtrace_hdr_commit(ea, (const uint8_t*)ctx->resv_line, ls,
                                    (uint32_t)ctx->ls[0x1C8] << 24 | (uint32_t)ctx->ls[0x1C9] << 16 | (uint32_t)ctx->ls[0x1CA] << 8 | ctx->ls[0x1CB],
                                    (uint32_t)ctx->image_id, (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK); }
            /* A committing PUTLLC is a line write like any other, so every
             * PEER reservation on it is lost and its SPU takes SPU_EVENT_LR.
             * Silent, this is the same lost update the PPU half was added to
             * close, with an SPU on the writing side: a peer is never told to
             * re-read, so it goes on polling a line it believes it still owns,
             * and a peer parked on RdEventStat sleeps through the commit it
             * was waiting for.
             *
             * Our own reservation is dropped FIRST so the notify does not
             * raise a self-LR: hardware CONSUMES the reservation on a
             * successful PUTLLC, it does not report it lost. Already under the
             * lock-line lock, which is what spu_coh_notify_write expects. */
            ctx->resv_valid = 0;
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 100000) == 1)
                      /* ...and the first four words of the line being written.
                       * A succeeding PUTLLC that repeats millions of times is a
                       * livelock, and the only useful question is what the SPU
                       * keeps seeing there -- the count alone cannot answer it. */
                      { uint32_t w[8] = {0,0,0,0,0,0,0,0};
                        for (int _i = 0; _i < 8; _i++) {
                            uint32_t t = 0;
                            memcpy(&t, (const unsigned char*)ctx->ls + ((lsa + _i*4) & SPU_LS_MASK), 4);
                            w[_i] = __builtin_bswap32(t);
                        }
                        fprintf(stderr, "[putllc-ok] %llu: img=%d ctx=%p ea=0x%08X"
                                        " line=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                                n, ctx->image_id, (void*)ctx, ea,
                                w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]); } } }
            spu_coh_notify_write(ea);
            ctx->atomic_stat = 0;                      /* PUTLLC_SUCCESS */
        } else {
            ctx->atomic_stat = 1;                      /* PUTLLC_FAILURE -> retry */
            /* SPU_PUTLLC_WHY=1: which of the three conditions failed.
             * A PUTLLC retry loop that never succeeds is a livelock, and
             * "failed" alone cannot distinguish a lost reservation from a line
             * another agent is rewriting -- opposite causes, opposite fixes. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) {
                  static unsigned long long n_noresv, n_ea, n_line;
                  if (!ctx->resv_valid)            n_noresv++;
                  else if (ctx->resv_ea != ea)     n_ea++;
                  else                             n_line++;
                  static unsigned long long total;
                  if ((++total % 500000) == 0) {
                      fprintf(stderr, "[putllc-why] %llu fails: no-reservation=%llu"
                                      " ea-mismatch=%llu line-changed=%llu"
                                      " (last ea=0x%08X img=%d resv_ea=0x%08X"
                                      " getllar=%llu)\n",
                              total, n_noresv, n_ea, n_line, ea, ctx->image_id,
                              ctx->resv_ea, (unsigned long long)ctx->dbg_getllar);
                      fflush(stderr);
                  }
              } }
        }
        { extern uint32_t g_barrier_sync_watch;
          uint32_t b = g_barrier_sync_watch;
          if (b && ea >= (b & ~127u) && ea < ((b + 0xC0 + 127) & ~127u)) {
              static int _n = 0;
              if (_n++ < 96 && ctx->atomic_stat == 0) {
                  uint32_t sn = ((uint32_t)ctx->ls[0x1C8]<<24)|((uint32_t)ctx->ls[0x1C9]<<16)|
                                ((uint32_t)ctx->ls[0x1CA]<<8)|ctx->ls[0x1CB];
                  extern uint8_t* vm_base;
                  const uint8_t* r = vm_base + b + 0x70;   /* row 3 (sync+0x40+16*3) */
                  fprintf(stderr, "[sync-atomic] PUTLLC-OK spuNum=%u ea=+0x%X lanes@row3={%u %u %u %u %u %u %u}\n",
                          sn, ea - b,
                          (r[2]<<8)|r[3],(r[4]<<8)|r[5],(r[6]<<8)|r[7],(r[8]<<8)|r[9],
                          (r[10]<<8)|r[11],(r[12]<<8)|r[13],(r[14]<<8)|r[15]);
              }
          } }
        /* SPU_ATOM_EA: the [atom-ea] line above prints every ATTEMPT, before
         * the compare. Say which of them committed, or a retried failure reads
         * as a lost update. */
        { static int s_v = -1; static uint32_t s_vl;
          if (s_v < 0) { const char* e = getenv("SPU_ATOM_EA");
                         s_vl = e ? (uint32_t)strtoul(e, 0, 16) & ~127u : 0; s_v = s_vl ? 1 : 0; }
          if (s_v && (ea & ~127u) == s_vl)
          { extern unsigned long long ps3_qpc_us(void);
            /* t= and the line's words after the commit, so commits line up
             * with the GPU's [GET_REPORT] and the PPU's [ww] on one clock. */
            const uint8_t* r = vm_base + (ea & ~127u);
            char w[400]; int o = 0;
            for (int i = 0; i < 128 && o < (int)sizeof w - 10; i += 4)
                o += snprintf(w + o, sizeof w - (size_t)o, "%s%02X%02X%02X%02X", i ? " " : "",
                              r[i], r[i + 1], r[i + 2], r[i + 3]);
            fprintf(stderr, "[atom-ea] t=%lluus PUTLLC %s lr=0x%05X ea=0x%08X wid=%u RAM=%s\n",
                    ps3_qpc_us(), ctx->atomic_stat ? "FAIL" : "OK",
                    ctx->gpr[0]._u32[0] & SPU_LS_MASK, ea,
                    ctx->policy_mode ? ctx->ls[0x1DF] : 255u, w); } }
        ctx->resv_valid = 0;                           /* reservation consumed */
        spu_lockline_unlock_ea(ea);
        if (ctx->atomic_stat == 0) {
            spu_taskset_line_committed(ea & ~127u);
            if (g_spu_line_commit_hook) g_spu_line_commit_hook(ea & ~127u);
        }
        return 1;

    case MFC_PUTLLUC_CMD:
    case MFC_PUTQLLUC_CMD:
        SPU_LL_HINT(ea, 3);
        spu_lockline_lock_ea(ea);   /* SPU_LL_STRIPE */
        { extern void spu_lockguard_check(spu_context*, uint32_t, const uint8_t*, uint32_t, const char*);
          spu_lockguard_check(ctx, ea, ls, MFC_ATOMIC_LINE, "PUTLLUC"); }
        if (__builtin_expect(s_put_audit > 0, 0)) spu_dma_audit_atomic((const uint8_t*)mem, ea);   /* GCM_PUT_AUDIT */
        if (spu_ll_inline() && lsa <= SPU_LS_SIZE - MFC_ATOMIC_LINE) spu_ll_copy128(mem, ls);   /* SPU_LL_INLINE */
        else
        memcpy(mem, ls, MFC_ATOMIC_LINE);              /* unconditional store */
        if (g_wtrace_span && spu_wtrace_hit(ea & ~127u, MFC_ATOMIC_LINE))   /* SPU_WTRACE (PUTLLUC) */
          spu_wtrace_rec('A', ea & ~127u, MFC_ATOMIC_LINE, ls, (uint32_t)ctx->image_id, (uint32_t)ctx->resident_ovl, (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK, NULL);
        /* Unconditional, so it invalidates EVERY reservation on the line --
         * this SPU's included, which is why the notify runs before the
         * bookkeeping below rather than after it. A peer left holding a
         * reservation here would commit a PUTLLC against a snapshot this store
         * has already overwritten. */
        spu_coh_notify_write(ea);
        ctx->resv_valid = 0; ctx->atomic_stat = 2;   /* PUTLLUC complete */
        spu_lockline_unlock_ea(ea);
        spu_taskset_line_committed(ea & ~127u);
        if (g_spu_line_commit_hook) g_spu_line_commit_hook(ea & ~127u);
        return 1;

    default:
        return 0;
    }
}

/* The MFC engine is only touched by commands, tag-status updates and list
 * stall acks; the other MFC channels are context fields, and
 * mfc_channel_read never uses it. Looking it up on every MFC channel access
 * (five or six per DMA) was ~1.5% of an SPU thread in combat. A context's
 * engine is created zeroed at its first lookup, so creating it at the first
 * command rather than the first LSA write leaves it the same.
 * SPU_MFC_LAZY=0: look the engine up on every MFC channel access. */
static int s_mfc_lazy = -1;
static inline int spu_mfc_lazy(void)
{
    if (s_mfc_lazy < 0) { const char* e = getenv("SPU_MFC_LAZY"); s_mfc_lazy = !(e && e[0] == '0'); }
    return s_mfc_lazy;
}

static int channel_is_mfc(uint32_t ch)
{
    switch (ch) {
    case MFC_WrMSSyncReq: case MFC_RdTagMask:  case MFC_LSA:
    case MFC_EAH:         case MFC_EAL:         case MFC_Size:
    case MFC_TagID:       case MFC_Cmd:         case MFC_WrTagMask:
    case MFC_WrTagUpdate: case MFC_RdTagStat:   case MFC_RdListStallStat:
    case MFC_WrListStallAck: case MFC_RdAtomicStat:
        return 1;
    default:
        return 0;
    }
}

/* ===========================================================================
 * Channel write
 * ===========================================================================*/
/* SPU_MFCLOG_IMG / _LSA / _EA / _MAX / _BYTES (see spu_wrch): out of line,
 * so spu_wrch keeps a small frame without the 960-byte line buffer (and the
 * stack cookie that buffer brought) on every channel write. */
static __attribute__((noinline)) void spu_wrch_mfclog(spu_context* ctx, uint32_t v)
{
    static int s_mi = -2, s_nea; static long s_mn, s_mmax; static uint32_t s_mea[8];
    /* SPU_MFCLOG_LSA=<lo>,<hi> (hex): only transfers whose LS start is in [lo, hi). */
    static uint32_t s_mlo, s_mhi = SPU_LS_SIZE;
    if (s_mi == -2) { const char* l = getenv("SPU_MFCLOG_LSA");
                      if (l) { char* end; s_mlo = (uint32_t)strtoul(l, &end, 16);
                               if (*end == ',') s_mhi = (uint32_t)strtoul(end + 1, 0, 16); } }
    if (s_mi == -2) { const char* e = getenv("SPU_MFCLOG_IMG"); s_mi = e ? atoi(e) : -1;
                      const char* m = getenv("SPU_MFCLOG_MAX"); s_mmax = m ? atol(m) : 20000;
                      const char* a = getenv("SPU_MFCLOG_EA");
                      while (a && *a && s_nea < 8) { char* end; s_mea[s_nea++] = (uint32_t)strtoul(a, &end, 16) & ~127u;
                                                    a = (*end == ',') ? end + 1 : NULL; } }
    /* SPU_MFCLOG_EA=<hex>[,<hex>...]: any image's GET/PUT that touches one of those 128-byte lines. */
    int hit_ea = 0;
    for (int k = 0; k < s_nea; k++)
        if ((ctx->mfc_eal & ~127u) == s_mea[k] || ((ctx->mfc_eal + ctx->mfc_size - 1u) & ~127u) == s_mea[k]) hit_ea = 1;
    const uint32_t mlsa = ctx->mfc_lsa & SPU_LS_MASK;
    if (((s_mi >= 0 && ctx->image_id == s_mi) || hit_ea) &&
        mlsa >= s_mlo && mlsa < s_mhi && s_mn++ < s_mmax) {
        /* SPU_MFCLOG_BYTES=<n>: bytes of data per line (default 64,
         * at most 256) -- a job object's fields past +0x40 were cut. */
        static int s_mb = -1;
        if (s_mb < 0) { const char* e = getenv("SPU_MFCLOG_BYTES"); s_mb = e ? atoi(e) : 64;
                        if (s_mb < 0) s_mb = 0; if (s_mb > 256) s_mb = 256; }
        const uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK, n = ctx->mfc_size < (uint32_t)s_mb ? ctx->mfc_size : (uint32_t)s_mb;
        extern int spu_workload_current_task(void);
        char line[960]; int o = snprintf(line, sizeof line, "[mfclog] img=%d task=%d spu=%u cmd=%02X lsa=%05X ea=%08X size=%X tag=%u:",
                                          ctx->image_id, spu_workload_current_task(), ctx->spu_id, v & 0xFFu, lsa, ctx->mfc_eal, ctx->mfc_size, ctx->mfc_tag);
        for (uint32_t k = 0; k < n && lsa + k < SPU_LS_SIZE; k++)
            o += snprintf(line + o, sizeof line - (size_t)o, (k & 3) ? "%02X" : " %02X", ctx->ls[lsa + k]);
        fprintf(stderr, "%s\n", line);
    }
}

static __attribute__((noinline)) int spu_mfc_atomic_call(spu_context* ctx, uint32_t cmd)
{
    /* A recorded or replayed SPU function run (spu_iojrn.h) sees the atomic. */
    const spu_iojrn_ops* j = t_spu_iojrn;
    if (__builtin_expect(j != 0, 0)) return j->atomic(ctx, cmd, spu_mfc_atomic);
    if (__builtin_expect(t_spu_atomic_hook != 0, 0)) return t_spu_atomic_hook(ctx, cmd, spu_mfc_atomic);
    return spu_mfc_atomic(ctx, cmd);
}
/* SPU_CH_FAST (default on; =0: every channel access takes the full path).
 * The MFC parameter channels are plain context fields: MFC_LSA/EAH/EAL/Size/
 * TagID and MFC_WrTagMask writes, MFC_RdTagStat/RdTagMask/RdAtomicStat reads
 * (five to eight of them per DMA). Their full path reads the SPU_CHHIST and
 * SPU_POLLTRACE gates, classifies the channel twice, picks the engine
 * (SPU_MFC_LAZY: none for these) and dispatches through mfc_channel_write /
 * mfc_channel_read to the same one field store or load; WrTagMask also
 * calls spu_evlog (SPU_EVLOG). With none of those diagnostics set and
 * SPU_MFC_LAZY on, the fast path does the store or load alone; anything
 * else takes the full path. On, it also skips the call to spu_wrch_mfclog on
 * every MFC_Cmd when neither SPU_MFCLOG_IMG nor SPU_MFCLOG_EA is set (with
 * both unset it can print nothing), and lets mfc_for answer a cached hit
 * inline. Read once. */
int s_ch_fast = -1;   /* declared above (mfc_for) */
static int s_mfclog_any = 1;
static __attribute__((noinline)) int spu_ch_fast_init(void)
{
    const char* e = getenv("SPU_CH_FAST");
    s_mfclog_any = getenv("SPU_MFCLOG_IMG") || getenv("SPU_MFCLOG_EA");
    s_ch_fast = !(e && e[0] == '0') && spu_mfc_lazy() &&
                !getenv("SPU_CHHIST") && !getenv("SPU_POLLTRACE") && !getenv("SPU_EVLOG");
    return s_ch_fast;
}
static inline int spu_ch_fast(void)
{
    const int v = s_ch_fast;
    return __builtin_expect(v >= 0, 1) ? v : spu_ch_fast_init();
}
static __attribute__((noinline)) void spu_wrch_full(spu_context* ctx, uint32_t channel, const u128* pvalue);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    if (__builtin_expect(spu_ch_fast() > 0, 1)) {   /* SPU_CH_FAST */
        const uint32_t fv = value._u32[0];
        if (channel - MFC_LSA <= MFC_TagID - MFC_LSA) {
            if (channel == MFC_LSA)       ctx->mfc_lsa = fv;
            else if (channel == MFC_EAH)  ctx->mfc_eah = fv;
            else if (channel == MFC_EAL)  ctx->mfc_eal = fv;
            else if (channel == MFC_Size) ctx->mfc_size = fv;
            else                          ctx->mfc_tag = fv & 0x1F;
            return;
        }
        if (channel == MFC_WrTagMask) { ctx->mfc_tag_mask = fv; return; }
    }
    spu_wrch_full(ctx, channel, &value);   /* by address: no copy of the value here */
}
static __attribute__((noinline)) void spu_wrch_full(spu_context* ctx, uint32_t channel, const u128* pvalue)
{
    const u128 value = *pvalue;
    /* SPU_CHHIST=1: which channels a job actually touches, and how often. A
     * worker that never issues an MFC command is either not being handed its
     * work descriptor or is waiting on a channel we never satisfy; the channel
     * mix distinguishes those. */
    { static int s_c = -1; if (s_c < 0) { const char* e = getenv("SPU_CHHIST"); s_c = e ? (atoi(e) > 0 ? atoi(e) : 2000) : 0; }
      if (s_c) { static unsigned long long w[128]; static unsigned long long n;
          w[channel & 127]++;
          if ((++n % (unsigned long long)s_c) == 0) { fprintf(stderr, "[chw] %llu writes:%c", n, 10);
              for (int i = 0; i < 128; i++) if (w[i])
                  fprintf(stderr, "   wrch ch%-3d %llu%c", i, w[i], 10); } } }
    uint32_t v = value._u32[0];  /* channel writes use the preferred slot */

    if (channel_is_mfc(channel)) {
        /* Atomic line ops (GETLLAR/PUTLLC/...) need real reservation semantics,
         * not the plain GET/PUT the DMA engine would do. */
        /* Only the four atomic commands reach the atomic path (the others it
         * refuses at once), and through an out-of-line call: inlined, its
         * diagnostic buffers gave every channel write a 1.4 KB frame with a
         * stack cookie. */
        /* OCCL_RDIAG (cellGcmSys.c): a reservation read of the ZPASS report banks. */
        if (__builtin_expect(g_occl_rdiag_fn != 0, 0) && channel == MFC_Cmd && v == MFC_GETLLAR_CMD &&
            (ctx->mfc_eal & ~127u) < 0x11264000u && (ctx->mfc_eal & ~127u) + 128u > 0x11238000u)
            g_occl_rdiag_fn(ctx->mfc_eal & ~127u, 128u, 3, ctx->image_id, ctx->mfc_lsa);
        if (channel == MFC_Cmd && (v == MFC_GETLLAR_CMD || v == MFC_PUTLLC_CMD || v == MFC_PUTLLUC_CMD ||
                                   v == MFC_PUTQLLUC_CMD) && spu_mfc_atomic_call(ctx, v))
            return;
        mfc_channel_write(!spu_mfc_lazy() || channel == MFC_Cmd || channel == MFC_WrTagUpdate ||
                          channel == MFC_WrListStallAck ? mfc_for(ctx) : NULL, ctx, channel, v);
        /* SPU_MFCLOG_IMG=<image> [SPU_MFCLOG_MAX=<n>]: every plain GET/PUT that
         * image issues (cmd, LS, EA, size and up to 64 bytes, read after the
         * transfer), for lifted code the interpreter hooks never see. */
        if (channel == MFC_Cmd && (s_ch_fast <= 0 || s_mfclog_any)) spu_wrch_mfclog(ctx, v);
        /* SPU_EVLOG=1: MFC_WrTagMask / MFC_WrTagUpdate writes under a policy
         * module, with the event mask they meet (first 400). */
        if (channel == MFC_WrTagUpdate || channel == MFC_WrTagMask || channel == MFC_WrListStallAck)
            spu_evlog(ctx, channel == MFC_WrTagUpdate ? "TagUpdate" : channel == MFC_WrTagMask ? "TagMask" : "StallAck", v, 0);
        /* MFC tag-status-update EVENT producer (channel-stall milestone). Our
         * DMA completes synchronously, so the instant the program arms tag
         * notification (MFC_WrTagUpdate with a non-zero mode = any/all), the
         * tags in MFC_WrTagMask are already complete -- pend the tag event
         * (bit 0, MFC_TAG_STATUS_UPDATE_EVENT) when the SPU has it enabled, and
         * wake any host thread blocked in rdch SPU_RdEventStat. This is the
         * long-missing producer for event_status (previously only ever cleared
         * via WrEventAck -> RdEventStat waits could never complete). */
        if (channel == MFC_WrTagUpdate && v != 0 && (ctx->event_mask & 0x1u)) {
            ctx->event_status |= 0x1u;
            spu_ch_wake(ctx);
        }
        return;
    }

    switch (channel) {
    case SPU_WrOutMbox:
        /* A raw SPU's outbound mailbox is one entry deep and wrch stalls while
         * it is full. Buffering instead let two replies queue up, and a PPU
         * that tests bit 0 of the count -- Tornado Outbreak's boot handshake,
         * func_000D5A10 -- read count 2 as empty and spun forever (an
         * intermittent boot hang). Raw SPUs only: their reader is the PPU
         * polling the MMIO window, whereas SPURS-thread mailboxes are drained
         * by paths that do not wake a stalled writer. A raw SPU's local store
         * is the guest window, not its own buffer. */
        if (ctx->ls != ctx->ls_store)
            while (ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH && ctx->status != SPU_STATUS_STOPPED)
                Sleep(0);
        spu_channel_write(&ctx->ch_out_mbox, v);
        /* SPU_DBG_MBOX=1: depth after the write. The image-1 SPUs publish their
         * LS work buffer here (LS 0x11D4) and then poll it, and the guest sets
         * class-2 mask 0x3 -- which excludes the plain-mailbox bit 0x10 -- so
         * this write raises no interrupt and the PPU must POLL to see it. If the
         * depth climbs and stays, nobody is polling and the message is stranded.
         * That is the difference between "the PPU is slow" and "the PPU never
         * looks", which no other probe here distinguishes. */
        { static int _d = -1; if (_d < 0) _d = getenv("SPU_DBG_MBOX") ? 1 : 0;
          if (_d) { static unsigned long _n = 0; if (++_n <= 24)
            fprintf(stderr, "[spu-outmbox] spu=%X wrote 0x%08X depth=%u\n",
                    ctx->spu_id, v, (unsigned)ctx->ch_out_mbox.count); } }
        { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_MBOXTRACE") ? 1 : 0;
          if (s_t) fprintf(stderr, "[spu-mbox] OUT  grp=0x%X spu=0x%X val=0x%08X\n",
                           ctx->spu_group_id, ctx->spu_id, v); }
        /* Plain mailbox data is consumed by the following interrupt request. */
        break;
    case SPU_WrOutIntrMbox:
        { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_MBOXTRACE") ? 1 : 0;
          if (s_t) fprintf(stderr, "[spu-mbox] INTR grp=0x%X spu=0x%X val=0x%08X\n",
                           ctx->spu_group_id, ctx->spu_id, v); }
        if (g_spu_user_event_hook && g_spu_user_event_hook(ctx, v)) break;
        spu_channel_write(&ctx->ch_out_intr_mbox, v);
        if (g_spu_out_mbox_hook) g_spu_out_mbox_hook(ctx->spu_group_id, ctx->spu_id, 1, v);
        break;
    case SPU_WrDec:          ctx->decrementer = v;
                             ctx->dec_base_ns = spu_host_ns();              break;
    case SPU_WrEventMask:
        spu_evlog(ctx, "EventMask", v, 0);
                             ctx->event_mask = v;                           break; /* WrEventMask */
    case SPU_WrEventAck:
        /* Under the lock-line lock, because every producer of the LR bit sets
         * it under that lock: the PPU coherent store, and a peer SPU's PUTLLC,
         * PUTLLUC or plain PUT. A bare read-modify-write here can read
         * event_status, have a concurrent |= SPU_EVENT_LR land in between, and
         * write the stale value back. The edge is then gone, and an SPU that
         * has just acknowledged the events it read goes back to sleep believing
         * it still holds a reservation it has already lost, which is the parked
         * SPURS kernel this whole mechanism exists to wake. */
        SPU_LL_HINT(0xFFFFFF80u, 10);
        spu_coh_event_ack(ctx, v);   /* SPU_LL_STRIPE: an atomic AND with stripes */
        break;
    case SPU_WrSRR0:         ctx->srr0 = v;                                 break;
    default:
        /* Unknown / unhandled channel write -- ignore (matches a no-op SPU). */
        break;
    }
}

/* ===========================================================================
 * Channel-stall contract (faithful-adopt, from canersaka's fork).
 *
 * A blocking spu_rdch on an empty read channel parks the SPU host thread on a
 * per-SPU CV until a producer (mailbox/signal write, event raise) calls
 * spu_ch_wake -- never fabricating a value. Under SPU_LOCKSTEP the wait
 * releases the run token first (block_begin) so a peer SPU that must post the
 * awaited data is never blocked on this ctx. A 10 ms re-poll is the missed-wake
 * safety net. Opt-in (env SPU_CH_BLOCK=1), default OFF while the producers
 * (mailbox/signal writes, MFC tag-status event raise) are being wired -- until
 * then blocking a read whose producer is missing would hang, so default stays
 * legacy non-blocking with zero regression, exactly like the lockstep gate.
 * ===========================================================================*/
/* Set by runtime/spu/spu_raw.c once a raw SPU exists. A raw SPU runs on its own
 * host thread against a PPU that pokes its mailboxes by MMIO, so an empty
 * RdInMbox must PARK, not fabricate a zero -- there is no scheduler above it to
 * retry. Off unless a raw SPU is actually created, so no other port changes. */
int g_spu_force_ch_block = 0;

static int yz_ch_block(void)
{
    static int v = -1;
    if (v < 0) v = getenv("SPU_CH_BLOCK") ? 1 : 0;
    return v || g_spu_force_ch_block;
}

/* Would rdch complete right now? Plain reads + the 10 ms re-poll cover cross-
 * thread visibility (x86 TSO + the wait syscall's barrier); the s43 atomics are
 * a later refinement. */
/* MFC_LLR_LOST_EVENT (0x400) -- the only producer for it.
 *
 * The SPU "wait until another processor touches this variable" idiom is: GETLLAR
 * the 128-byte line, set the event mask to Lr, then block in
 * `rdch SPU_RdEventStat` until the reservation is lost. Nothing ever raised that
 * bit, so an SPU using it waited forever -- ps1_netemu's GPU core (SPU 4) parks
 * at pc=0x0A5E8 with evmask=0x400 exactly here while the PPU spins waiting on the
 * SPU. Deadlock, and the reason the PS1 core never starts.
 *
 * Losing a reservation means the reserved line changed, and GETLLAR already
 * snapshots it, so compare against that snapshot rather than hooking every store.
 * ponytail: polled, not store-hooked -- one 128-byte memcmp per 10 ms poll on an
 * already-blocked SPU, and nothing at all on the PPU store path. The ceiling is
 * one poll of wake latency, and an A-B-A write that restores the same bytes
 * between polls is missed; hook the writers if either ever matters.
 */
static void spu_resv_lost_poll(spu_context* ctx)
{
    if (!(ctx->event_mask & 0x400u) || !ctx->resv_valid) return;
    if (!vm_base || ctx->resv_ea == 0) return;
    if (memcmp(vm_base + ctx->resv_ea, ctx->resv_line, 128) != 0) {
        ctx->resv_valid = 0;
        ctx->event_status |= 0x400u;
    }
}

static int spu_ch_ready(spu_context* ctx, uint32_t channel)
{
    if (channel == SPU_RdEventStat) spu_resv_lost_poll(ctx);
    switch (channel) {
    case SPU_RdInMbox:      return ctx->rcv_evt_n != 0 || ctx->ch_in_mbox.count != 0;
    case SPU_RdSigNotify1:  return ctx->ch_sig_notify[0].count != 0;
    case SPU_RdSigNotify2:  return ctx->ch_sig_notify[1].count != 0;
    case SPU_RdEventStat:   return (ctx->event_status & ctx->event_mask) != 0;
    default:                return 1;   /* non-blocking channels: always ready */
    }
}

/* Signal the per-SPU wait CV so a blocked spu_rdch re-checks its predicate.
 * MUST be called by every producer AFTER it makes a read predicate true. */
void spu_ch_wake(spu_context* ctx)
{
    if (!ctx) return;
    WakeAllConditionVariable((CONDITION_VARIABLE*)&ctx->ch_wait_cv);
}

/* Block the calling SPU host thread until `channel` is readable. */
static void spu_ch_wait(spu_context* ctx, uint32_t channel, const char* op)
{
    if (!yz_ch_block() || spu_ch_ready(ctx, channel)) return;

    { static unsigned long bn = 0; unsigned long n = ++bn;
      if (n <= 50 || (n % 512) == 0)
        fprintf(stderr, "[ch-block] spu=%X pc=0x%05X op=%s ch=%u evstat=0x%X evmask=0x%X\n",
                ctx->spu_id, ctx->pc & SPU_LS_MASK, op, channel,
                ctx->event_status, ctx->event_mask); }

    ctx->status = SPU_STATUS_WAITING_CHANNEL;
    yz_lockstep_block_begin(ctx);          /* release the run token before the OS wait */
    {
        unsigned long long start = GetTickCount64(), next_hb = 2000;
        while (!spu_ch_ready(ctx, channel)) {
            AcquireSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
            if (!spu_ch_ready(ctx, channel))
                SleepConditionVariableSRW((CONDITION_VARIABLE*)&ctx->ch_wait_cv,
                                          (SRWLOCK*)&ctx->ch_wait_lock, 10, 0);
            ReleaseSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
            unsigned long long waited = GetTickCount64() - start;
            if (waited >= next_hb) {
                fprintf(stderr, "[ch-wait] spu=%X pc=0x%05X ch=%u waited=%llums evstat=0x%X evmask=0x%X resv[valid=%d ea=0x%08X]\n",
                        ctx->spu_id, ctx->pc & SPU_LS_MASK, channel, waited,
                        ctx->event_status, ctx->event_mask,
                        ctx->resv_valid, ctx->resv_ea);
                fflush(stderr);
                next_hb = ((waited / 2000) + 1) * 2000;
            }
        }
    }
    yz_lockstep_block_end(ctx);            /* rejoin the rotation, reacquire the token */
    ctx->status = SPU_STATUS_RUNNING;
}

/* ===========================================================================
 * Channel read (returns value in the preferred word slot)
 * ===========================================================================*/
SPU_THREAD_LOCAL uint32_t (*t_spu_dec_hook)(spu_context*, uint32_t);

static __attribute__((noinline)) u128 spu_rdch_full(spu_context* ctx, uint32_t channel);
static u128 spu_rdch_real(spu_context* ctx, uint32_t channel);
static uint32_t spu_rdch_real_u32(spu_context* ctx, uint32_t channel)
{
    return spu_rdch_real(ctx, channel)._u32[0];
}
/* SPU_DEC_HIST=1 (diagnostic, off): where decrementer reads come from -- the
 * return address in the caller of spu_rdch (lifted code, a native), counted
 * per site; every 5 s the top 12 with their exe RVA and the image running
 * (tools/maprva.py <map> <log> names them). A site that reads it millions of
 * times a second is a timed wait loop. */
static int s_dec_hist = -1;
static __attribute__((noinline)) void spu_dec_hist_note(const spu_context* ctx, void* ra)
{
    enum { DH = 4096 };
    static void* volatile s_ra[DH];
    static volatile long long s_n[DH];
    static volatile int s_img[DH];
    static volatile unsigned long long s_t0;
    for (unsigned i = (unsigned)(((uintptr_t)ra >> 2) * 2654435761u) % DH, k = 0; k < 64; k++, i = (i + 1) % DH) {
        void* cur = s_ra[i];
        if (cur == ra) { __atomic_add_fetch(&s_n[i], 1, __ATOMIC_RELAXED); break; }
        if (!cur) {
            void* expect = 0;
            if (__atomic_compare_exchange_n(&s_ra[i], &expect, ra, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) || expect == ra) {
                s_img[i] = ctx->image_id;
                __atomic_add_fetch(&s_n[i], 1, __ATOMIC_RELAXED);
                break;
            }
        }
    }
    extern unsigned long long ps3_qpc_us(void);
    const unsigned long long now = ps3_qpc_us();
    unsigned long long t0 = s_t0;
    if (!t0) { s_t0 = now; return; }
    if (now - t0 < 5000000ull || !__atomic_compare_exchange_n(&s_t0, &t0, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    extern char __ImageBase;
    static long long snap[DH];
    long long tot = 0;
    for (int k = 0; k < DH; k++) { snap[k] = __atomic_exchange_n(&s_n[k], 0, __ATOMIC_RELAXED); tot += snap[k]; }
    fprintf(stderr, "[dec-hist] %lld decrementer reads in 5 s, top sites:\n", tot);
    for (int r = 0; r < 12; r++) {
        int best = -1;
        for (int k = 0; k < DH; k++) if (snap[k] > 0 && (best < 0 || snap[k] > snap[best])) best = k;
        if (best < 0) break;
        fprintf(stderr, "[dec-hist] %10lld  img=%d rva=0x%llX\n", snap[best], s_img[best],
                (unsigned long long)((uintptr_t)s_ra[best] - (uintptr_t)&__ImageBase));
        snap[best] = 0;
    }
}
u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    if (__builtin_expect(s_dec_hist != 0, 0) && channel == SPU_RdDec) {
        if (s_dec_hist < 0) s_dec_hist = getenv("SPU_DEC_HIST") ? 1 : 0;
        if (s_dec_hist) spu_dec_hist_note(ctx, __builtin_return_address(0));
    }
    /* A recorded or replayed SPU function run (spu_iojrn.h) sees every channel
     * read but the decrementer's, which has its own hook (t_spu_dec_hook). */
    const spu_iojrn_ops* j = t_spu_iojrn;
    if (__builtin_expect(j != 0, 0) && channel != SPU_RdDec)
        return spu_make_preferred_u32(j->rdch(ctx, channel, spu_rdch_real_u32));
    return spu_rdch_real(ctx, channel);
}
static u128 spu_rdch_real(spu_context* ctx, uint32_t channel)
{
    if (__builtin_expect(spu_ch_fast() > 0, 1)) {   /* SPU_CH_FAST: mfc_channel_read's field loads */
        if (channel == MFC_RdTagStat)    return spu_make_preferred_u32(ctx->mfc_tag_status);
        if (channel == MFC_RdAtomicStat) return spu_make_preferred_u32(ctx->atomic_stat);
        if (channel == MFC_RdTagMask)    return spu_make_preferred_u32(ctx->mfc_tag_mask);
    }
    return spu_rdch_full(ctx, channel);
}
static __attribute__((noinline)) u128 spu_rdch_full(spu_context* ctx, uint32_t channel)
{
    { static int s_c = -1; if (s_c < 0) { const char* e = getenv("SPU_CHHIST"); s_c = e ? (atoi(e) > 0 ? atoi(e) : 2000) : 0; }
      if (s_c) { static unsigned long long r[128]; static unsigned long long n;
          r[channel & 127]++;
          if ((++n % (unsigned long long)s_c) == 0) { fprintf(stderr, "[chr] %llu reads:%c", n, 10);
              for (int i = 0; i < 128; i++) if (r[i])
                  fprintf(stderr, "   rdch ch%-3d %llu%c", i, r[i], 10); } } }
    /* Block (never fabricate) on an empty producer-fed read channel (opt-in
     * SPU_CH_BLOCK). RdEventStat now has a producer (the MFC tag-status event
     * raise above), but only block it when the SPU has actually enabled events
     * (event_mask != 0) -- a masked-off read must return 0 immediately, not
     * park forever. RdInMbox/RdSigNotify park on their PPU producers. */
    if (channel == SPU_RdInMbox || channel == SPU_RdSigNotify1 || channel == SPU_RdSigNotify2)
        spu_ch_wait(ctx, channel, "rdch");
    else if (channel == SPU_RdEventStat && ctx->event_mask != 0)
        spu_ch_wait(ctx, channel, "rdch");

    uint32_t v = 0;

    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_c[10] = {0}; static uint64_t s_tot = 0;
        int b = (channel==SPU_RdInMbox)?0:(channel==SPU_RdSigNotify1)?1:(channel==SPU_RdSigNotify2)?2:
                (channel==SPU_RdDec)?3:(channel==SPU_RdEventStat)?4:(channel==SPU_RdEventMask)?5:
                (channel==MFC_RdTagStat)?6:(channel==MFC_RdAtomicStat)?7:(channel==SPU_RdMachStat)?8:9;
        s_c[b]++;
        if ((++s_tot % 2000000) == 0)
          fprintf(stderr, "[rdch] InMbox=%llu Sig1=%llu Sig2=%llu Dec=%llu EvStat=%llu EvMask=%llu TagStat=%llu AtomStat=%llu MachStat=%llu other=%llu\n",
            (unsigned long long)s_c[0],(unsigned long long)s_c[1],(unsigned long long)s_c[2],(unsigned long long)s_c[3],
            (unsigned long long)s_c[4],(unsigned long long)s_c[5],(unsigned long long)s_c[6],(unsigned long long)s_c[7],
            (unsigned long long)s_c[8],(unsigned long long)s_c[9]); } }

    if (channel_is_mfc(channel)) {
        v = mfc_channel_read(spu_mfc_lazy() ? NULL : mfc_for(ctx), ctx, channel);
        return spu_make_preferred_u32(v);
    }

    switch (channel) {
    case SPU_RdInMbox:
        if (ctx->rcv_evt_n > 0) { v = ctx->rcv_evt[ctx->rcv_evt_i++]; ctx->rcv_evt_n--; }
        else v = spu_channel_read(&ctx->ch_in_mbox);
        break;
    case SPU_RdSigNotify1:  v = spu_channel_read(&ctx->ch_sig_notify[0]); break;
    case SPU_RdSigNotify2:  v = spu_channel_read(&ctx->ch_sig_notify[1]); break;
    /* The decrementer must actually DECREMENT. Returning the latched WrDec
     * value verbatim makes every `rdch $ch8` deadline poll spin forever: the
     * elapsed-time term is always zero, so the deadline never passes. LBP's
     * wwsjob SPURS policy module hangs on exactly that at LS 0x2D68:
     *
     *     2d6c:  lqa   $2, 0x1530     ; deadline
     *     2d74:  rdch  $5, $ch8       ; now  <- always read 0
     *     2d78:  sf    $6, $2, $5     ; now - deadline
     *     2d80:  cgti  $5, $6, 0
     *     2d88:  binz  $2, $0         ; leave once it goes positive
     *
     * It never leaves, never reaches its first DMA, and blocks the workload.
     * Wraparound on subtract is deliberate -- it is what hardware does, and the
     * `cgti > 0` test above is written to tolerate it. */
    case SPU_RdDec: {
        /* Never armed by a WrDec: stamp the base on first read rather than
         * measuring from epoch 0, which would subtract the host's entire
         * uptime and hand back a wild value to code that has every right to
         * read the decrementer before writing it. */
        if (!ctx->dec_base_ns) ctx->dec_base_ns = spu_host_ns();
        uint64_t elapsed = spu_host_ns() - ctx->dec_base_ns;
        /* Split seconds from the remainder before scaling: elapsed_ns * 79.8M
         * overflows 64 bits 231 s after the base stamp, and a context that
         * never writes its decrementer (the SPURS kernel's) keeps the stamp
         * from boot, so every timed loop on that SPU saw the counter jump
         * forward by ~16 s of ticks at 231 s, 462 s, 693 s... */
        const uint64_t secs = elapsed / 1000000000ull, rem = elapsed % 1000000000ull;
        uint32_t ticks   = (uint32_t)(secs * SPU_DECREMENTER_HZ +
                                      (rem * SPU_DECREMENTER_HZ) / 1000000000ull);
        v = ctx->decrementer - ticks;
        /* Recording/replaying a function's run (the title's spu_fnrec): the
         * decrementer values it reads are logged, then fed back, so a replay
         * is deterministic. NULL everywhere else. */
        if (t_spu_dec_hook) v = t_spu_dec_hook(ctx, v);
        break;
    }
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    case SPU_RdEventStat:   spu_resv_lost_poll(ctx); v = ctx->event_status; break;
    /* CBEA 9.8: numeric bit 0 is interrupt-enable, independent of run status.
     * This runtime models nonisolated SPUs, so the isolation bit stays clear. */
    case SPU_RdMachStat:    v = ctx->int_enable & 1u; break;
    case SPU_RdSRR0:        v = ctx->srr0;                              break;
    default:
        v = 0;
        break;
    }
    return spu_make_preferred_u32(v);
}

/* ===========================================================================
 * Channel count (rchcnt) -- how many entries can be read/written right now
 * ===========================================================================*/
static uint32_t spu_rchcnt_real(spu_context* ctx, uint32_t channel);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    const spu_iojrn_ops* j = t_spu_iojrn;
    if (__builtin_expect(j != 0, 0)) return j->rchcnt(ctx, channel, spu_rchcnt_real);
    return spu_rchcnt_real(ctx, channel);
}
static uint32_t spu_rchcnt_real(spu_context* ctx, uint32_t channel)
{
    /* SPU_WHOPOLLS=<n>: rchcnt/park accounting PER SPU, printed every n calls
     * with the pc each SPU is sitting at.
     *
     * SPU_CHHIST sums every SPU into one histogram, which is exactly the wrong
     * shape for a deadlock: it showed rchcnt ch0/ch4 climbing past 1.7 billion
     * while MFC_WrTagUpdate (ch23) stayed frozen, i.e. somebody polls forever
     * and somebody else stopped doing DMA -- without saying whether that is one
     * SPU or two. The freeze under investigation is the PPU-side spin in
     * func_000D2298 waiting for spu4 to advance a counter at 0x002DF008, so
     * which SPU is parked, and on what, is the whole question. */
    { static int s_w = -1;
      if (s_w < 0) { const char* e = getenv("SPU_WHOPOLLS");
        s_w = e ? (atoi(e) > 0 ? atoi(e) : 4000000) : 0; }
      if (s_w) { static unsigned long long c[8][40]; static unsigned long long n;
          static uint32_t lastpc[8], lastst[8], lastmask[8];
          static uint32_t lastsig[8][2], lastmb[8], lastrea[8];
          static int lastrv[8], lastrd[8];
          static uint32_t lastline[8][8], lastls[8][8];
          static uint32_t lastlsa[8], lastr90[8];
          const unsigned sp = (unsigned)(ctx->spu_id & 7u);
          if (channel < 40u) c[sp][channel]++;
          lastpc[sp] = (uint32_t)ctx->pc & SPU_LS_MASK;
          lastst[sp] = ctx->event_status; lastmask[sp] = ctx->event_mask;
          lastsig[sp][0] = ctx->ch_sig_notify[0].count;
          lastsig[sp][1] = ctx->ch_sig_notify[1].count;
          lastmb[sp] = ctx->ch_in_mbox.count;
          lastrv[sp] = ctx->resv_valid; lastrea[sp] = ctx->resv_ea;
          lastlsa[sp] = ctx->mfc_lsa & SPU_LS_MASK;
          lastr90[sp] = ctx->gpr[90]._u32[0];
          lastrd[sp] = (ctx->resv_valid && vm_base && ctx->resv_ea)
                     ? (memcmp(vm_base + ctx->resv_ea, ctx->resv_line, 128) != 0)
                     : -1;
          { static int s_ld2 = -2; static uint32_t s_lo2;
            if (s_ld2 == -2) { const char* e5 = getenv("SPU_LSDUMP");
                               s_ld2 = e5 ? 1 : 0;
                               s_lo2 = e5 ? (uint32_t)strtoul(e5,0,16) : 0u; }
            if (s_ld2 && ctx->ls)
                for (int z = 0; z < 8; z++) {
                    const uint8_t* l8 = ctx->ls + ((s_lo2 + z * 4) & SPU_LS_MASK);
                    lastls[sp][z] = ((uint32_t)l8[0] << 24) | ((uint32_t)l8[1] << 16)
                                  | ((uint32_t)l8[2] << 8) | l8[3];
                } }
          for (int z = 0; z < 8; z++) {
              const uint8_t* b8 = ctx->resv_line + z * 4;
              lastline[sp][z] = ((uint32_t)b8[0] << 24) | ((uint32_t)b8[1] << 16)
                              | ((uint32_t)b8[2] << 8) | b8[3];
          }
          if ((++n % (unsigned long long)s_w) == 0) {
              fprintf(stderr, "[whopolls] %llu rchcnt calls\n", n);
              for (unsigned q = 0; q < 8; q++) {
                  int any = 0;
                  for (unsigned k = 0; k < 40; k++) if (c[q][k]) any = 1;
                  if (!any) continue;
                  /* The channel state each poll is actually testing. Counts
                   * alone cannot say whether a poll returns 0 or 1, and that is
                   * the difference between "the SPU is not being told" and "the
                   * SPU is told and ignores it". */
                  fprintf(stderr, "   spu%u pc=0x%05X ev[st=%08X mask=%08X]"
                                  " sig[%u %u] inmbox=%u", q, lastpc[q],
                          lastst[q], lastmask[q], lastsig[q][0], lastsig[q][1],
                          lastmb[q]);
                  /* The reservation is the other half of an evmask=0x400 wait:
                   * spu_resv_lost_poll returns immediately unless resv_valid,
                   * so a cleared reservation is indistinguishable from "nobody
                   * wrote the line" in event_status alone -- and they are
                   * different bugs. diff says whether the line has actually
                   * changed under the snapshot right now. */
                  /* mfc_lsa and r90 alongside the reservation. GETLLAR
                   * copies the line to &ls[mfc_lsa] and to resv_line from the
                   * same source, so a snapshot that disagrees with the local
                   * store can only mean mfc_lsa was not what the SPU asked for
                   * -- and the SPU computes both that LSA and the address its
                   * compare reads from r90. Printing the two together is what
                   * decides it. */
                  fprintf(stderr, " resv[v=%d ea=0x%08X diff=%d]"
                                  " mfc_lsa=0x%05X r90=0x%08X",
                          lastrv[q], lastrea[q], lastrd[q],
                          lastlsa[q], lastr90[q]);
                  /* And the line itself. "The reservation is intact and the
                   * line has not changed" still does not say what the SPU is
                   * waiting FOR; the words do. Snapshot side, so it is exactly
                   * what the SPU last read. */
                  /* SPU_LSDUMP=<hex LS offset>: 8 words of this SPU's local
                   * store. The reserved line says what the SPU can SEE; its LS
                   * says what it BELIEVES. For the spu4 deadlock those are the
                   * two numbers to compare -- its consumed counter is staged at
                   * LS 0x10888 (the counter PUT's lsa 0x10880, +8). */
                  { static int s_ld = -2; static uint32_t s_lo;
                    if (s_ld == -2) { const char* e4 = getenv("SPU_LSDUMP");
                                      s_ld = e4 ? 1 : 0;
                                      s_lo = e4 ? (uint32_t)strtoul(e4,0,16) : 0u; }
                    if (s_ld) {
                        fprintf(stderr, " ls[0x%05X:", s_lo);
                        for (int z = 0; z < 8; z++)
                            fprintf(stderr, " %08X", lastls[q][z]);
                        fprintf(stderr, "]");
                    } }
                  if (lastrv[q]) {
                      fprintf(stderr, " line[");
                      for (int z = 0; z < 8; z++)
                          fprintf(stderr, "%s%08X", z ? " " : "", lastline[q][z]);
                      fprintf(stderr, "]");
                  }
                  for (unsigned k = 0; k < 40; k++) if (c[q][k])
                      fprintf(stderr, " ch%u=%llu", k, c[q][k]);
                  fprintf(stderr, "\n");
              }
              fflush(stderr);
          } } }
    /* SPU_CHHIST also covers rchcnt. It used to instrument only wrch/rdch,
     * which is the one place a parked persistent worker is guaranteed NOT to
     * appear: park_on_empty_inmbox halts from inside THIS function, so a
     * worker that polls rchcnt and parks produced a completely empty channel
     * histogram and read as 'never touches a channel'. */
    { static int s_c = -1;
      if (s_c < 0) { const char* e = getenv("SPU_CHHIST");
        s_c = e ? (atoi(e) > 0 ? atoi(e) : 2000) : 0; }
      if (s_c) { static unsigned long long c[128]; static unsigned long long n;
          c[channel & 127]++;
          if ((++n % (unsigned long long)s_c) == 0) {
              fprintf(stderr, "[chc] %llu rchcnt:%c", n, 10);
              for (int i = 0; i < 128; i++) if (c[i])
                  fprintf(stderr, "   rchcnt ch%-3d %llu%c", i, c[i], 10);
              fflush(stderr); } } }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_cnt[8] = {0}; static uint64_t s_total = 0;
        int b = (channel==SPU_RdInMbox)?0:(channel==SPU_RdEventStat)?1:(channel==SPU_RdSigNotify1)?2:
                (channel==SPU_RdSigNotify2)?3:(channel==MFC_RdTagStat)?4:(channel==SPU_WrOutMbox)?5:
                (channel==SPU_WrOutIntrMbox)?6:7;
        s_cnt[b]++;
        if ((++s_total % 2000000) == 0)
          fprintf(stderr, "[pollcnt] InMbox=%llu EvStat=%llu Sig1=%llu Sig2=%llu TagStat=%llu OutMbox=%llu OutIntr=%llu other=%llu (ch last=%u)\n",
                  (unsigned long long)s_cnt[0],(unsigned long long)s_cnt[1],(unsigned long long)s_cnt[2],
                  (unsigned long long)s_cnt[3],(unsigned long long)s_cnt[4],(unsigned long long)s_cnt[5],
                  (unsigned long long)s_cnt[6],(unsigned long long)s_cnt[7], channel); } }
    switch (channel) {
    case SPU_RdInMbox:
        /* Synchronous persistent-worker park: after its handshake the SPU polls
         * the inbound mailbox for PPU commands; if empty and parking is armed,
         * halt (longjmp out of spu_run_with_halt) instead of spinning forever. */
        /* A pending sys_spu_thread_receive_event reply counts as readable
         * mailbox words: the worker checks rchcnt to decide whether its reply
         * has arrived, and parking on an "empty" inbox that actually holds the
         * reply strands it. */
        if (ctx->park_on_empty_inmbox && ctx->rcv_evt_n == 0 &&
            ctx->ch_in_mbox.count == 0) {
            extern void spu_halt(spu_context*);
            spu_halt(ctx);
        }
        return (uint32_t)ctx->rcv_evt_n + ctx->ch_in_mbox.count;           /* readable */
    /* Free slots, CLAMPED. The queue behind each channel is SPU_CHANNEL_CAP
     * deep -- deeper than the hardware depth reported here -- so `count` can
     * exceed the depth and a bare subtraction underflows, telling the SPU it
     * has four billion free slots. That silently converts a bounded mailbox
     * into an unbounded one for any title that polls before writing, which is
     * every SPU-heavy one. */
    case SPU_WrOutMbox:
        return ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH
             ? 0u : (uint32_t)(SPU_MBOX_DEPTH - ctx->ch_out_mbox.count);
    case SPU_WrOutIntrMbox:
        return ctx->ch_out_intr_mbox.count >= SPU_INTR_MBOX_DEPTH
             ? 0u : (uint32_t)(SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count);
    case SPU_RdSigNotify1:   return ctx->ch_sig_notify[0].count;
    case SPU_RdSigNotify2:   return ctx->ch_sig_notify[1].count;
    case MFC_Cmd:            return MFC_QUEUE_DEPTH - mfc_for(ctx)->queue_count;
    /* An enabled event pending, or 0 -- NOT the `default: 1` this used to fall
     * through to. The SPU idiom is `rchcnt SPU_RdEventStat; brnz -> rdch`: it
     * asks whether an event is pending and only commits to the BLOCKING read if
     * the answer is yes. Answering 1 unconditionally lured it into a read that
     * then parked forever, because rdch correctly blocks while
     * (event_status & event_mask) == 0 -- rchcnt and rdch disagreed.
     *
     * ps1_netemu's audio SPU does exactly this at LS 0x8934 and parked at
     * 0x0A5E8 waiting for MFC_LLR_LOST_EVENT on a line nothing was ever going
     * to touch; on hardware it would simply have fallen through to 0x893C and
     * kept working. Same condition as spu_ch_ready's case, including the
     * lost-reservation poll, so the two now agree by construction. */
    case SPU_RdEventStat:
        spu_resv_lost_poll(ctx);
        return (ctx->event_status & ctx->event_mask) != 0;
    case MFC_RdTagStat:      return 1;  /* synchronous: status always ready */
    /* Stall-and-notify status is a single-value channel: count is 1 while a
     * newly-stalled tag is pending to be read, else 0. The wwsjob interrupt
     * handler gates on this (rchcnt $ch25; brz IhcExit) -- returning the old
     * default of 1 made it enter on spurious interrupts with an empty stat,
     * running IhcLoop on a zero mask (clz(0) -> bogus tagId/ack). */
    case MFC_RdListStallStat: return ctx->list_stall_stat ? 1 : 0;
    default:                 return 1;  /* default: channel ready */
    }
}

/* ===========================================================================
 * Indirect-branch dispatch + function registry
 * ===========================================================================*/
typedef void (*spu_fn)(spu_context*);

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;   /* which recompiled image this function belongs to */
} spu_reg_entry;

/* One entry per lifted SPU function across EVERY registered image, and a game
 * that lifts its whole SPU workload set has a lot of them: Yakuza: Dead Souls
 * registers ~170k (cri_audio alone is ~35k, gs_task ~9k, the job binaries ~11k).
 * At 65536 the registry silently truncated every image registered past the cap
 * -- and the SPURS job-chain policy, the job binaries and the Edge geometry
 * task (gs_task) register LAST, so their functions were dropped wholesale and
 * every indirect branch into them fell through to a branch-to-0. Size it for
 * the real workload; the overflow is now loud (see spu_register_function). */
#define SPU_FN_REGISTRY_MAX 262144
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;

/* Accessor for the sampling profiler in ppu_loader.cpp. Its address->function
 * map is built from the PPU table alone, which leaves the SPU-lifted bodies as
 * unowned gaps -- and an [entry, next_entry) extent then charges every SPU
 * sample to whichever PPU function happens to precede it. That produced a
 * confident and completely wrong "99% of guest time in one PPU function".
 * Handing the SPU host pointers over lets the map cover both, so the gaps are
 * real entries and the extents mean something. */
uint32_t spu_registry_size(void) { return s_registry_count; }

int spu_registry_entry(uint32_t i, void** host, uint32_t* ls_addr)
{
    if (i >= s_registry_count) return 0;
    if (host)    *host    = (void*)s_registry[i].fn;
    if (ls_addr) *ls_addr = s_registry[i].addr;
    return 1;
}

/* Hash index over the registry. spu_lookup runs on EVERY guest indirect
 * branch -- with the Bink decoder live that is millions of dispatches per
 * second, and the old linear scan (~950 binkspu entries walked per branch)
 * dominated movie playback. Buckets chain in REGISTRATION ORDER (tail
 * append) so the first-registered-match-wins semantics of the linear scan
 * are preserved exactly; the image-id wildcard rules stay in the bucket
 * walk, which is 1-3 entries (same LS addr across overlapping images).
 * Registration is startup-single-threaded; lookups treat the index as
 * read-only. Chain links store index+1 so zero-init means "empty". */
#define SPU_FN_HASH_SIZE 131072   /* power of two, ~= MAX/2 -> load factor ~2 */
static uint32_t s_hash_head[SPU_FN_HASH_SIZE];
static uint32_t s_hash_tail[SPU_FN_HASH_SIZE];
static uint32_t s_hash_next[SPU_FN_REGISTRY_MAX];

static inline uint32_t spu_fn_hash(uint32_t addr)
{
    return ((addr >> 2) * 2654435761u) & (SPU_FN_HASH_SIZE - 1);
}

/* Image currently being registered. SPURS images (kernel/policy/job) overlap in
 * LS, so each registers under a distinct id via spu_begin_image() before calling
 * its (prefixed) spu_recomp_register(). Single-image callers leave it 0. */
static int s_reg_image = 0;
void spu_begin_image(int image_id) { s_reg_image = image_id; }

/* One bit per LS word for each image id: is a lifted function registered at
 * this address? The interpreter asks spu_lifted_probe at every instruction,
 * and almost every answer is "no" -- which a bit test gives without the three
 * hash lookups. 8 KB per image that registers anything. */
#define SPU_FN_BITS_IMAGES 1024
static uint8_t* s_fn_bits[SPU_FN_BITS_IMAGES];
static void spu_fn_bits_set(int img, uint32_t addr)
{
    if (img < 0 || img >= SPU_FN_BITS_IMAGES) return;
    if (!s_fn_bits[img]) s_fn_bits[img] = (uint8_t*)calloc(SPU_LS_SIZE / 32, 1);
    if (!s_fn_bits[img]) return;
    const uint32_t w = (addr & SPU_LS_MASK) >> 2;
    s_fn_bits[img][w >> 3] |= (uint8_t)(1u << (w & 7));
}
static inline int spu_fn_bits_test(int img, uint32_t addr)
{
    if (img < 0 || img >= SPU_FN_BITS_IMAGES || !s_fn_bits[img]) return 0;
    const uint32_t w = (addr & SPU_LS_MASK) >> 2;
    return (s_fn_bits[img][w >> 3] >> (w & 7)) & 1;
}

/* The next lifted entry of image img above addr (SPU_LS_SIZE if none): where
 * the lifted function starting at addr ends (spu_shadow_check). */
uint32_t spu_fn_next_entry(int img, uint32_t addr)
{
    if (img < 0 || img >= SPU_FN_BITS_IMAGES || !s_fn_bits[img]) return SPU_LS_SIZE;
    for (uint32_t a = (addr & SPU_LS_MASK & ~3u) + 4; a < SPU_LS_SIZE; a += 4)
        if (spu_fn_bits_test(img, a)) return a;
    return SPU_LS_SIZE;
}

/* The title's default SPU_NOLIFT list; a title defines its own to override
 * this weak empty one (God of War: Ascension: host/ascension_defaults.cpp). */
__attribute__((weak)) const char* const spu_title_nolift = 0;

void spu_register_function(uint32_t addr, spu_fn fn)
{
    if (s_registry_count >= SPU_FN_REGISTRY_MAX) {
        /* Silent truncation here dropped whole late-registered SPU images and
         * cost a multi-round hunt (the branch-to-0 looked like a lifter/overlay
         * bug). Never again: say so, once, loudly, with the number to raise the
         * cap to. */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr,
                    "[spu] FATAL: SPU function registry full at %u entries "
                    "(SPU_FN_REGISTRY_MAX=%u) -- image %d function 0x%05X and all "
                    "later registrations DROPPED. Raise SPU_FN_REGISTRY_MAX.\n",
                    s_registry_count, (unsigned)SPU_FN_REGISTRY_MAX,
                    s_reg_image, addr);
            fflush(stderr);
        }
        return;
    }
    /* SPU_NOLIFT=<image>:<hex addr>[,...]: leave these lifted functions out
     * (an image of -1 matches any), so that code runs interpreted -- to find
     * which lift of several changes behaviour without rebuilding. */
    { static int s_n = -1; static struct { int img; uint32_t addr; } s_skip[64];
      if (s_n < 0) {
          s_n = 0;
          /* A title may name its own default (spu_title_nolift, a weak symbol
           * it defines); SPU_NOLIFT, even empty, replaces it. */
          const char* e = getenv("SPU_NOLIFT");
          if (!e) e = spu_title_nolift;
          while (e && *e && s_n < 64) {
              char* end;
              s_skip[s_n].img = (int)strtol(e, &end, 10);
              if (*end != ':') break;
              s_skip[s_n].addr = (uint32_t)strtoul(end + 1, &end, 16);
              s_n++;
              e = (*end == ',') ? end + 1 : NULL;
          }
          if (s_n) fprintf(stderr, "[spu] SPU_NOLIFT: %d function(s) left to the interpreter\n", s_n);
      }
      for (int k = 0; k < s_n; k++)
          if (s_skip[k].addr == addr && (s_skip[k].img < 0 || s_skip[k].img == s_reg_image))
              return; }
    spu_fn_bits_set(s_reg_image, addr);
    uint32_t i = s_registry_count;
    s_registry[i].addr = addr;
    s_registry[i].fn = fn;
    s_registry[i].image_id = s_reg_image;
    s_registry_count = i + 1;
    uint32_t h = spu_fn_hash(addr);
    s_hash_next[i] = 0;
    if (s_hash_head[h] == 0)
        s_hash_head[h] = i + 1;
    else
        s_hash_next[s_hash_tail[h] - 1] = i + 1;
    s_hash_tail[h] = i + 1;
}

/* Report a CROSS-IMAGE match: the registry served a function some other image
 * registered at this address, because one side's image_id was the 0 wildcard.
 *
 * This is a real hazard, reported with evidence by @canersaka (#59): SPURS job
 * chains load job binaries into descriptor-assigned LS slots, so the same binary
 * can sit at different addresses on different rounds. If the lift was fixed-base
 * and the exact match misses, the wildcard can serve ANOTHER image's function at
 * that address -- the job then runs the wrong program end to end, returns
 * cleanly, and the only symptom is something three layers downstream that never
 * happens. Their words: one log line would have saved a day.
 *
 * Logging, not refusing. Refusing the wildcard outright was measured and does
 * NOT survive contact: a dormant task image can register spans overlapping the
 * resident kernel's low LS range, so a legitimate service-to-kernel yield gets
 * falsely rejected. Telling resident from dormant needs residency tracking the
 * registry does not have. So this reports and keeps going.
 *
 * Deduped per (addr, from, to): spu_lookup runs millions of times a second, and
 * a substitution that repeats every dispatch would bury the log it is meant to
 * make readable. */
static void spu_report_cross_image(uint32_t addr, int want, int got)
{
    enum { SEEN_MAX = 64 };
    static struct { uint32_t addr; int want, got; } seen[SEEN_MAX];
    static unsigned n_seen = 0;
    for (unsigned i = 0; i < n_seen; i++)
        if (seen[i].addr == addr && seen[i].want == want && seen[i].got == got)
            return;
    if (n_seen < SEEN_MAX) {
        seen[n_seen].addr = addr; seen[n_seen].want = want; seen[n_seen].got = got;
        n_seen++;
    }
    fprintf(stderr,
            "[spu] CROSS-IMAGE dispatch: LS 0x%05X requested by image %d, served "
            "by image %d's function (id-0 wildcard). If this job was loaded at a "
            "runtime-chosen base, it may be running the WRONG program -- see "
            "issue #59.\n", addr, want, got);
}

/* Per host thread, the last answers of spu_lookup by (address, image): every
 * dispatch asks one to three of them, and the chain walk through the 512 KB
 * head table and the registry costs dependent cache misses each time. The
 * registry is filled by static constructors before any SPU runs and is only
 * ever appended to; a different count empties the memo. Answers that come
 * with a cross-image report (a wildcard match) are not kept, so the report
 * fires as before. SPU_LOOKUP_MEMO=0: walk every time. */
#define SPU_LOOKUP_MEMO_SLOTS 512u          /* power of two */
static SPU_TLS struct { uint64_t nkey; spu_fn fn; } t_lookup_memo[SPU_LOOKUP_MEMO_SLOTS];
static SPU_TLS uint32_t t_lookup_memo_count;
static int s_lookup_memo = -1;

/* spu_lookup, also saying whether the answer came with a cross-image report
 * (*exact = 0; untouched otherwise): spu_indirect_branch's dispatch memo
 * below keeps only answers that did not, so the report fires as before. */
static inline __attribute__((always_inline)) spu_fn spu_lookup_x(uint32_t addr, int image_id, int* exact)
{
    if (s_lookup_memo < 0) { const char* e = getenv("SPU_LOOKUP_MEMO"); s_lookup_memo = !(e && e[0] == '0'); }
    /* Keys are stored inverted so a zeroed slot means (0xFFFFFFFF, -1), which
     * no lookup with an image id >= 0 can ask for. */
    const int memo = s_lookup_memo && image_id >= 0;
    const uint64_t nkey = ~(((uint64_t)(uint32_t)image_id << 32) | addr);
    const unsigned slot = (((addr >> 2) * 2654435761u) ^ ((uint32_t)image_id * 0x85EBCA6Bu))
                          >> 23 & (SPU_LOOKUP_MEMO_SLOTS - 1);
    if (memo) {
        if (t_lookup_memo_count != s_registry_count) {
            memset(t_lookup_memo, 0, sizeof t_lookup_memo);
            t_lookup_memo_count = s_registry_count;
        }
        if (t_lookup_memo[slot].nkey == nkey) return t_lookup_memo[slot].fn;
    }
    /* Match the context's active image; image_id 0 (context or entry) matches
     * any, for back-compat with single-image contexts. */
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr == addr &&
            (image_id == 0 || e->image_id == 0 || e->image_id == image_id)) {
            /* One compare on the return path; the report itself is rare. */
            if (e->image_id != image_id) {
                spu_report_cross_image(addr, image_id, e->image_id);
                *exact = 0;
            }
            else if (memo) { t_lookup_memo[slot].nkey = nkey; t_lookup_memo[slot].fn = e->fn; }
            return e->fn;
        }
    }
    if (memo) { t_lookup_memo[slot].nkey = nkey; t_lookup_memo[slot].fn = NULL; }
    return NULL;
}

spu_fn spu_lookup(uint32_t addr, int image_id)   /* exported: clang-built fast-path dispatch (spu_dispatch_mt.c) needs it */
{
    int exact = 1;
    return spu_lookup_x(addr, image_id, &exact);
}

/* Which image registered a function at this LS address, or -1 if none did.
 *
 * Deliberately NOT spu_lookup(addr, 0): that call means "dispatch here in a
 * context with no image", so it reports a cross-image substitution for every
 * hit in a real image. This is a question about the registry, not a dispatch,
 * and it runs once per SPU thread start. First match wins, in registration
 * order, so it agrees with which function spu_lookup would actually serve. */
int spu_image_of_function(uint32_t addr)
{
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr == addr) return e->image_id;
    }
    return -1;
}

/* Does a lifted function exist at this LS address (any image)? Lets the lv2
 * layer decide between real SPU execution and the PPU-fallback paths. */
int spu_have_function(uint32_t addr)
{
    return spu_image_of_function(addr) >= 0;
}

/* The pure interpreter's fast-path "is this LSA already lifted?" probe
 * (spu_interp.c, rejoin path). It IS the registry lookup, so overlay eviction and
 * self-modifying-code invalidation are honored automatically. Lived in
 * spu_fn_registry.c before the registry was consolidated into this TU. */
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa)
{
    return (spu_lifted_fn)spu_lookup(lsa, ctx ? ctx->image_id : 0);
}

/* ---- Swappable code overlays (see spu_context.resident_ovl) --------------
 * A title registers each runtime-streamed overlay's SOURCE content EA with
 * the image id its lifted functions were registered under. The MFC GET path
 * marks that overlay resident in the streaming context; dispatch retries a
 * primary-image miss against the resident overlay's registry. */
typedef struct { uint32_t src_ea; int image_id; uint8_t sig[16]; int has_sig; uint32_t span;
                 uint32_t pic_base; uint32_t first_word; int sig_ready; } spu_ovl_src;
/* 6 FMOD codec/DSP overlays + up to 89 WWS job-code modules (all stream into
 * the same job code buffer at LS 0x4000, dispatched by content signature). */
#define SPU_OVL_SRC_MAX 512
static spu_ovl_src s_ovl_src[SPU_OVL_SRC_MAX];
static int s_ovl_src_count = 0;

void spu_overlay_register_source(uint32_t content_ea, int image_id)
{
    if (s_ovl_src_count < SPU_OVL_SRC_MAX) {
        s_ovl_src[s_ovl_src_count].src_ea = content_ea;
        s_ovl_src[s_ovl_src_count].image_id = image_id;
        s_ovl_src[s_ovl_src_count].has_sig = 0;
        s_ovl_src[s_ovl_src_count].span = 0;
        s_ovl_src[s_ovl_src_count].pic_base = 0;
        s_ovl_src_count++;
    }
}

/* Register a bounded code image that may coexist with other streamed images.
 * Its functions must be translated at the local-store addresses used by the title. */
void spu_overlay_register_region(uint32_t content_ea, uint32_t span, int image_id)
{
    if (!span || span > SPU_LS_SIZE || s_ovl_src_count >= SPU_OVL_SRC_MAX) return;
    spu_overlay_register_source(content_ea, image_id);
    s_ovl_src[s_ovl_src_count - 1].span = span;
}

/* A code fragment a job streams from content_ea into a buffer, at a local-store
 * address that depends on what else the job loads. It was lifted position-
 * independently (spu_lifter --pic) at lift_base: wherever the GET puts it,
 * dispatch looks pc - lsa + lift_base up in its image and runs it with
 * ctx->pic_delta = lsa - lift_base. */
void spu_overlay_register_pic(uint32_t content_ea, uint32_t span, uint32_t lift_base, int image_id)
{
    if (!span || !lift_base || s_ovl_src_count >= SPU_OVL_SRC_MAX) return;
    spu_overlay_register_region(content_ea, span, image_id);
    s_ovl_src[s_ovl_src_count - 1].pic_base = lift_base & SPU_LS_MASK;
}

/* When each resident_code slot was last claimed, per SPU (see note_get). */
static uint32_t s_code_seq[16][SPU_RESIDENT_CODE_SLOTS];
static uint32_t s_code_clock;

/* SPU_PIC_LOG=<n>: the first n claims, evictions and lookups of streamed code
 * spans (resident_code), to see why a fragment ran interpreted. */
static int spu_pic_log(void)
{
    static long s_n = -1;
    if (s_n < 0) { const char* e = getenv("SPU_PIC_LOG"); s_n = e ? atol(e) : 0; }
    if (s_n <= 0) return 0;
    s_n--;
    return 1;
}

/* spu_lookup restricted to one image: a position-independent image's lift
 * addresses are not where it runs, so a wildcard (image 0) entry there means
 * nothing. */
static spu_fn spu_lookup_in_image(uint32_t addr, int image_id)
{
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr == addr && e->image_id == image_id) return e->fn;
    }
    return NULL;
}

/* The streamed code span (resident_code) that holds pc, if any: *owner is its
 * image (0 when pc is in none), the result its function at pc. For a
 * position-independent image this also sets ctx->pic_delta. Shared by the full
 * resolver and the musttail fast path (spu_dispatch_mt.c). */
/* The span [lo, hi) that the context's live streamed-code slots cover, cached
 * per host thread: most lookups are for the module's own code, outside every
 * slot, and the 32-slot walk answered "none" on every trampoline step (~6% of
 * God of War: Ascension's SPU threads in its first level). Only a claim adds a
 * slot, and it bumps the claim generation (spu_rc_gen); evictions only shrink the set, which a stale
 * (larger) span answers correctly, by walking. */
/* SPU_RC_GEN_CTX (default on; =0: one counter for every context, as the
 * global g_rc_gen was): the claim generation kept per context, in 64 buckets
 * of context addresses on cache lines of their own (contexts sharing a bucket
 * only see each other's claims as spurious changes). What the generation
 * guards -- t_rc_span and spu_indirect_branch's memo -- is the context's OWN
 * streamed code slots, and a context's slots change only under its own claims
 * (spu_pic_claim on that context). With one global counter every EDGE job
 * start on any SPU thread (it claims its fragment spans) dropped every other
 * thread's cached span and memo, and the counter's line moved between cores
 * on each claim. 32-bit as g_rc_gen was (a bucket wraps no sooner).
 * The mode is fixed before any SPU runs (a constructor), so every claim and
 * every check use the same buckets; =0 maps every context to bucket 0. */
#define SPU_RC_GEN_BUCKETS 64u
static struct { volatile unsigned gen; char pad[60]; } g_rc_gen_b[SPU_RC_GEN_BUCKETS] __attribute__((aligned(64)));
static unsigned s_rc_gen_mask = SPU_RC_GEN_BUCKETS - 1u;
__attribute__((constructor)) static void spu_rc_gen_mode_init(void)
{
    const char* e = getenv("SPU_RC_GEN_CTX");
    s_rc_gen_mask = (e && e[0] == '0') ? 0u : SPU_RC_GEN_BUCKETS - 1u;
}
static inline unsigned spu_rc_gen_bucket(const spu_context* ctx)
{
    return ((unsigned)(((uintptr_t)ctx >> 6) * 0x9E3779B1u) >> 26) & s_rc_gen_mask;   /* 0..63 */
}
static inline volatile unsigned* spu_rc_gen_slot(const spu_context* ctx)
{
    return &g_rc_gen_b[spu_rc_gen_bucket(ctx)].gen;
}
static inline unsigned spu_rc_gen(const spu_context* ctx)
{
    return __atomic_load_n(spu_rc_gen_slot(ctx), __ATOMIC_ACQUIRE);
}
static inline void spu_rc_gen_bump(const spu_context* ctx)
{
    __atomic_add_fetch(spu_rc_gen_slot(ctx), 1u, __ATOMIC_RELEASE);
}
static SPU_THREAD_LOCAL struct { const spu_context* ctx; unsigned gen; uint32_t lo, hi; } t_rc_span;
static inline int spu_rc_outside(const spu_context* ctx, uint32_t pc)
{
    const unsigned gen = spu_rc_gen(ctx);
    if (t_rc_span.ctx != ctx || t_rc_span.gen != gen) {
        uint32_t lo = SPU_LS_SIZE, hi = 0;
        for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
            if (!ctx->resident_code[slot].image_id) continue;
            const uint32_t a = ctx->resident_code[slot].lsa, e = a + ctx->resident_code[slot].size;
            if (a < lo) lo = a;
            if (e > hi) hi = e;
        }
        t_rc_span.ctx = ctx; t_rc_span.gen = gen; t_rc_span.lo = lo; t_rc_span.hi = hi;
    }
    return pc < t_rc_span.lo || pc >= t_rc_span.hi;
}
/* [lsa, lsa + size) misses the span of every live slot (see above). */
static inline int spu_rc_range_outside(const spu_context* ctx, uint32_t lsa, uint32_t size)
{
    (void)spu_rc_outside(ctx, 0);   /* refreshes t_rc_span for ctx */
    return lsa >= t_rc_span.hi || lsa + size <= t_rc_span.lo;
}

/* spu_resident_code_fn, also naming the slot that answered (*slot_at, -1 for
 * none) and clearing *exact when a lookup came with a cross-image report. */
static inline __attribute__((always_inline))
spu_fn spu_resident_code_fn_x(spu_context* ctx, uint32_t pc, int* owner, int* slot_at, int* exact)
{
    *slot_at = -1;
    if (spu_rc_outside(ctx, pc)) { *owner = 0; return NULL; }
    for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
        const int img = ctx->resident_code[slot].image_id;
        if (!img || pc < ctx->resident_code[slot].lsa ||
            pc - ctx->resident_code[slot].lsa >= ctx->resident_code[slot].size)
            continue;
        *owner = img;
        *slot_at = (int)slot;
        if (!ctx->resident_code[slot].base)
            return spu_lookup_x(pc, img, exact);
        const uint32_t d = ctx->resident_code[slot].lsa - ctx->resident_code[slot].base;
        spu_fn fn = spu_lookup_in_image(pc - d, img);
        if (fn) ctx->pic_delta = d;
        if (spu_pic_log())
            fprintf(stderr, "[spu-pic] lookup pc=0x%05X slot=%u img=%d span=0x%05X+0x%X -> 0x%05X %s\n",
                    pc, slot, img, ctx->resident_code[slot].lsa, ctx->resident_code[slot].size,
                    pc - d, fn ? "lifted" : "MISS");
        return fn;
    }
    *owner = 0;
    return NULL;
}

spu_fn spu_resident_code_fn(spu_context* ctx, uint32_t pc, int* owner)
{
    int slot_at, exact = 1;
    return spu_resident_code_fn_x(ctx, pc, owner, &slot_at, &exact);
}

/* Declared in spu_interp.h. The interpreter asks this at every step, so it
 * must answer what spu_indirect_branch would do: before it only saw the
 * context's own image, and a job that fell to the interpreter at one unlifted
 * address ran the rest of its body interpreted although every function of the
 * resident job module (and of its code fragments) was lifted. */
/* Position-independent fragments found by content, where the interpreter is
 * about to run. The GET-side tracking (spu_overlay_note_get) only sees code
 * that arrives as a fragment-sized GET from its executable address, or as a
 * block that starts with a fragment; a job that brings its code some other
 * way -- God of War: Ascension's module 207 in the main menu takes heap
 * copies and long stretches of the executable that start mid-fragment, and
 * overwrites parts of them with data in between -- left whole jobs
 * interpreted (230 ms each, the menu at 1 fps). So at an interpreter step
 * that no resident span covers with a lifted function, look the bytes at pc
 * up among the lifted fragments' function entries: when they match an entry
 * and the fragment's first quadword sits where that entry implies, the
 * fragment is resident there -- claim it, and the step rejoins lifted code.
 * Position-independent code that is byte-identical behaves identically, and
 * both the entry (64 bytes) and the fragment start are compared. */
static void spu_pic_claim(spu_context* ctx, const spu_ovl_src* o, uint32_t lsa, uint32_t ea, uint32_t size);
static unsigned spu_pic_hash(uint32_t w);
static int spu_pic_sigs_ready(void);
typedef struct { uint32_t w[4]; uint32_t rel; int src; int next; } spu_pic_entry;
static uint64_t s_pic_entry_filter[64];
static spu_pic_entry* s_pic_entries;
static int s_pic_entry_head[4096];
static int spu_pic_entries_build(void);
static int spu_pic_entries_ready(void)
{
    static int s_state;                    /* 0 not built, 1 building, 2 ready, 3 none */
    int st = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
    if (st >= 2) return st == 2;
    int expect = 0;
    if (__atomic_compare_exchange_n(&s_state, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        st = spu_pic_entries_build() ? 2 : 3;
        __atomic_store_n(&s_state, st, __ATOMIC_RELEASE);
        return st == 2;
    }
    while ((st = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE)) == 1) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    return st == 2;
}
static int spu_pic_entries_build(void)
{
    int s_ready = 0;
    if (!spu_pic_sigs_ready()) return 0;
    for (int h = 0; h < 4096; h++) s_pic_entry_head[h] = -1;
    int cap = 0, n = 0;
    for (uint32_t r = 0; r < s_registry_count; r++) {
        const spu_reg_entry* e = &s_registry[r];
        for (int i = 0; i < s_ovl_src_count; i++) {
            const spu_ovl_src* o = &s_ovl_src[i];
            if (!o->sig_ready || o->image_id != e->image_id) continue;
            const uint32_t rel = e->addr - o->pic_base;
            if (rel >= o->span) break;
            if (n == cap) {
                cap = cap ? cap * 2 : 1024;
                spu_pic_entry* grown = (spu_pic_entry*)realloc(s_pic_entries, (size_t)cap * sizeof *grown);
                if (!grown) return n > 0;
                s_pic_entries = grown;
            }
            spu_pic_entry* p = &s_pic_entries[n];
            const uint8_t* src = vm_base + o->src_ea + rel;
            for (int k = 0; k < 4; k++)
                p->w[k] = ((uint32_t)src[4 * k] << 24) | ((uint32_t)src[4 * k + 1] << 16) |
                          ((uint32_t)src[4 * k + 2] << 8) | src[4 * k + 3];
            p->rel = rel; p->src = i;
            const unsigned h = spu_pic_hash(p->w[0]);
            p->next = s_pic_entry_head[h]; s_pic_entry_head[h] = n;
            s_pic_entry_filter[h >> 6] |= 1ull << (h & 63);
            n++;
            break;
        }
    }
    (void)s_ready;
    return n > 0;
}

static int spu_pic_content_claim(spu_context* ctx, uint32_t lsa)
{
    if (lsa + 64 > SPU_LS_SIZE || !spu_pic_entries_ready()) return 0;
    const uint8_t* at = ctx->ls + lsa;
    const uint32_t w0 = ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
    const unsigned h = spu_pic_hash(w0);
    if (!((s_pic_entry_filter[h >> 6] >> (h & 63)) & 1)) return 0;
    {   int owner = 0; const uint32_t pic = ctx->pic_delta;
        spu_fn fn = spu_resident_code_fn(ctx, lsa, &owner);
        ctx->pic_delta = pic;
        if (fn) return 0; }                /* already resident and lifted */
    for (int i = s_pic_entry_head[h]; i >= 0; i = s_pic_entries[i].next) {
        const spu_pic_entry* p = &s_pic_entries[i];
        if (p->w[0] != w0 || p->rel > lsa) continue;
        const spu_ovl_src* o = &s_ovl_src[p->src];
        const uint32_t n = (o->span - p->rel < 64u) ? o->span - p->rel : 64u;
        if (memcmp(at, vm_base + o->src_ea + p->rel, n) != 0) continue;
        const uint32_t lsa0 = lsa - p->rel;
        if (memcmp(ctx->ls + lsa0, o->sig, 16) != 0) continue;
        /* Spans this one now covers are stale: the bytes there are this fragment. */
        for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot)
            if (ctx->resident_code[slot].image_id &&
                ctx->resident_code[slot].lsa < lsa0 + o->span &&
                lsa0 < ctx->resident_code[slot].lsa + ctx->resident_code[slot].size)
                ctx->resident_code[slot].image_id = 0;
        if (spu_pic_log())
            fprintf(stderr, "[spu-pic] content-claim img=%d at lsa=0x%05X (entry +0x%X at pc=0x%05X)\n",
                    o->image_id, lsa0, p->rel, lsa);
        spu_pic_claim(ctx, o, lsa0, o->src_ea, o->span);
        return 1;
    }
    return 0;
}

int spu_lifted_probe(spu_context* ctx, uint32_t lsa)
{
    lsa &= SPU_LS_MASK;
    /* SPU_PROBE=<mask>: what besides the context's own image the interpreter
     * may rejoin -- 1 streamed code spans, 2 the resident taskset task, 4 the
     * resident overlay; 0 is the old answer. Rejoining the resident overlay
     * (4) stalls Ascension at the end of loading, its post-process tasks
     * parked in WAIT_SIGNAL (7 runs of 7 with 4 or 5 in the mask, 0 of 4 with
     * 0 or 1): a job module's lifted function entered from interpreted code
     * returns by host return, past the interpreted caller. Missing job-module
     * entries go into the lift instead (jobmods_extra_funcs.json). Default 1. */
    /* 8: the resident overlay too, but only while interrupts are disabled. The
     * stall above comes from lifted job code running inside a job's interrupt
     * window (module 214 enables them at 0x4414, disables them at 0x447C):
     * interpreted code is never interrupted, lifted code is at every trampoline
     * step, and the runtime's handler emulation does not survive that there.
     * Outside the window the interpreted remainder of the job -- the whole job
     * body after 0x4418, ~25M instructions/s in the cinematic -- can go back
     * to lifted code. Default 9 (1 | 8). */
    static int s_mask = -1;
    if (s_mask < 0) { const char* e = getenv("SPU_PROBE"); s_mask = e && *e ? atoi(e) : 9; }
    /* A lifted fragment recognised by its bytes (spu_pic_content_claim). */
    if ((s_mask & 1) && spu_pic_content_claim(ctx, lsa)) return 1;
    /* Fast "no": no image this context could dispatch to has a function at
     * lsa, and lsa is in no streamed code span (whose lifts sit elsewhere). */
    {   int spans = 0;
        for (unsigned slot = 0; !spu_rc_outside(ctx, lsa) && slot < SPU_RESIDENT_CODE_SLOTS; ++slot)
            if (ctx->resident_code[slot].image_id &&
                lsa - ctx->resident_code[slot].lsa < ctx->resident_code[slot].size) { spans = 1; break; }
        if (!spans && ctx->image_id > 0 &&   /* image 0 matches every image's functions */
            !spu_fn_bits_test(ctx->image_id, lsa) && !spu_fn_bits_test(0, lsa) &&
            !spu_fn_bits_test(ctx->resident_ovl, lsa) && !spu_fn_bits_test(ctx->resident_task, lsa))
            return 0; }
    int owner = 0;
    if (s_mask & 1) {
        const uint32_t pic = ctx->pic_delta;
        spu_fn fn = spu_resident_code_fn(ctx, lsa, &owner);
        ctx->pic_delta = pic;             /* a probe; the dispatch sets it */
        if (owner) return fn != NULL;
    }
    if ((s_mask & 2) && ctx->resident_task && spu_lookup(lsa, ctx->resident_task)) return 1;
    if (((s_mask & 4) || ((s_mask & 8) && !ctx->int_enable)) &&
        ctx->resident_ovl && spu_lookup(lsa, ctx->resident_ovl)) return 1;
    /* SPU_REJOIN=<ovl>:<lo>-<hi>[,...] (hex LS addresses): rejoin the resident
     * overlay's lifted functions in these ranges only, whatever the mask --
     * to find, by halving, which lifted function behaves unlike the
     * interpreter on a path only the interpreter ever ran. */
    { static int s_rn = -1; static struct { int ovl; uint32_t lo, hi; } s_rj[16];
      if (s_rn < 0) {
          s_rn = 0;
          const char* e = getenv("SPU_REJOIN");
          while (e && *e && s_rn < 16) {
              char* end;
              s_rj[s_rn].ovl = (int)strtol(e, &end, 10);
              if (*end != ':') break;
              s_rj[s_rn].lo = (uint32_t)strtoul(end + 1, &end, 16);
              s_rj[s_rn].hi = s_rj[s_rn].lo;
              if (*end == '-') s_rj[s_rn].hi = (uint32_t)strtoul(end + 1, &end, 16);
              s_rn++;
              e = (*end == ',') ? end + 1 : NULL;
          }
      }
      for (int k = 0; k < s_rn; k++)
          if (ctx->resident_ovl == s_rj[k].ovl && lsa >= s_rj[k].lo && lsa <= s_rj[k].hi &&
              spu_lookup(lsa, ctx->resident_ovl))
              return 1; }
    return spu_lifted_lookup(ctx, lsa) != NULL;
}

/* Register one-way runtime entries that replace the guest call stack.
 * Matching the resolved function, not just its LS address, avoids affecting
 * unrelated overlays using the same address. Registration precedes execution. */
static struct { uint32_t entry; int image_id; } s_stack_reset[16];
unsigned g_spu_stack_reset_count;   /* read by spu_task_launch_check before calling in */
#define s_stack_reset_count g_spu_stack_reset_count
void spu_register_stack_reset_entry(uint32_t entry, int image_id)
{
    if (s_stack_reset_count < 16) {
        s_stack_reset[s_stack_reset_count].entry = entry & SPU_LS_MASK;
        s_stack_reset[s_stack_reset_count++].image_id = image_id;
    }
}

/* Also called before direct trampoline transfers: generated calls can enter
 * a stack-switching routine without going through spu_indirect_branch. */
void spu_check_stack_reset(spu_context* ctx, void (*fn)(spu_context*))
{
    if (!ctx->host_depth || !s_spu_halt_armed || ctx->policy_mode) return;
    for (unsigned i = 0; i < s_stack_reset_count; ++i)
        if (ctx->pc == s_stack_reset[i].entry &&
            fn == spu_lookup(ctx->pc, s_stack_reset[i].image_id)) {
            g_spu_trampoline_fn = 0;
            longjmp(s_spu_halt_env, 2);
        }
}

/* SPURS taskset TASK entries (see spu_context.resident_task). A taskset can hold
 * several tasks whose lifts share the SAME LS base -- the co-resident task-code
 * region -- so no LS address identifies which task owns it. The title registers
 * each task's ELF ENTRY point with the image id its functions were registered
 * under; spu_indirect_branch adopts that image the moment the policy branches
 * into the entry from outside the region. This is what lets each such task be
 * registered under its own real image id instead of the id-0 wildcard, so a
 * dormant co-resident task can no longer shadow the one the policy launched. */
#define SPU_TASK_ENTRY_MAX 16
static struct { uint32_t entry; int image_id; } s_task_entry[SPU_TASK_ENTRY_MAX];
static int s_task_entry_count = 0;
void spu_taskset_register_task_entry(uint32_t entry, int image_id)
{
    if (s_task_entry_count < SPU_TASK_ENTRY_MAX) {
        s_task_entry[s_task_entry_count].entry = entry & SPU_LS_MASK;
        s_task_entry[s_task_entry_count].image_id = image_id;
        s_task_entry_count++;
    }
}
static int spu_taskset_task_image(uint32_t entry)
{
    for (int i = 0; i < s_task_entry_count; i++)
        if (s_task_entry[i].entry == entry) return s_task_entry[i].image_id;
    return 0;
}
/* Resume PCs and even fresh entry PCs overlap between task ELFs. The
 * taskset policy's TaskInfo identifies which ELF it has actually loaded. */
static struct { uint32_t elf_ea; int image_id, policy_image_id; }
    s_task_elf[SPU_TASK_ENTRY_MAX];
static int s_task_elf_count;
void spu_taskset_register_task_elf(uint32_t elf_ea, int image_id, int policy_image_id)
{
    if (s_task_elf_count < SPU_TASK_ENTRY_MAX) {
        s_task_elf[s_task_elf_count].elf_ea = elf_ea & ~7u;
        s_task_elf[s_task_elf_count].image_id = image_id;
        s_task_elf[s_task_elf_count++].policy_image_id = policy_image_id;
    }
}
static int spu_taskset_resident_image(const spu_context* ctx)
{
    const uint8_t* p = ctx->ls + 0x2794; /* SpursTasksetContext.taskInfo.elf */
    uint32_t elf = (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                    ((uint32_t)p[2] << 8) | p[3]) & ~7u;
    for (int i = 0; i < s_task_elf_count; ++i)
        if (s_task_elf[i].elf_ea == elf &&
            s_task_elf[i].policy_image_id == ctx->resident_ovl)
            return s_task_elf[i].image_id;
    return 0;
}
/* Lowest LS address of the shared task-code region: the taskset tasks all lift
 * at LS 0x3000 (below it is the SPURS kernel/policy/context, 0x290..0x2FFF). */
#define SPU_TASKSET_TASK_LO 0x3000u

/* Content-signature variant: FMOD COPIES codec overlays to the heap before
 * streaming them into the swap slot, so the source EA is unknowable ahead of
 * time -- match the first 16 bytes of the streamed content instead. */
void spu_overlay_register_sig(const uint8_t sig[16], int image_id)
{
    if (s_ovl_src_count < SPU_OVL_SRC_MAX) {
        s_ovl_src[s_ovl_src_count].src_ea = 0;
        s_ovl_src[s_ovl_src_count].image_id = image_id;
        memcpy(s_ovl_src[s_ovl_src_count].sig, sig, 16);
        s_ovl_src[s_ovl_src_count].has_sig = 1;
        s_ovl_src_count++;
    }
}

/* Called from the MFC GET path after the copy: ls points at the JUST-COPIED
 * bytes. EA match first (exact, cheap), then content signature for sizeable
 * chunks (overlay bodies are >= 0x500 bytes). */
/* SPU_DUMP_OVL=<dir>[,<lsa>]: write out every distinct code body streamed into
 * LS <lsa> (default 0x5000, the job code buffer), so job bodies the title
 * keeps in its own data can be lifted. A body larger than one DMA arrives as
 * consecutive GETs (contiguous EA and LS); those are appended to the same
 * file. One file per source EA: ovl_<ea>.bin. */
static void spu_ovl_dump(spu_context* ctx, uint32_t ea, uint32_t lsa, const uint8_t* ls, uint32_t size)
{
    static int s_on = -1; static char s_dir[400]; static uint32_t s_lsa = 0x5000;
    if (s_on < 0) { const char* e = getenv("SPU_DUMP_OVL"); s_on = e && *e;
        if (s_on) { snprintf(s_dir, sizeof s_dir, "%s", e);
                    char* c = strchr(s_dir, ','); if (c) { *c = 0; s_lsa = (uint32_t)strtoul(c + 1, 0, 0); } } }
    if (!s_on) return;
    static uint32_t s_seen[256]; static unsigned s_n;
    static SPU_TLS uint32_t t_src, t_next_ea, t_next_lsa;
    FILE* f = NULL; char path[512];
    if (lsa == s_lsa && size >= 256) {
        for (unsigned i = 0; i < s_n; i++) if (s_seen[i] == ea) { t_src = 0; return; }
        if (s_n < 256) s_seen[s_n++] = ea;
        t_src = ea;
        snprintf(path, sizeof path, "%s/ovl_%08X.bin", s_dir, ea);
        f = fopen(path, "wb");
        fprintf(stderr, "[spu-ovl-dump] img=%d body ea=0x%08X size=%u -> %s\n", ctx->image_id, ea, size, path);
    } else if (t_src && ea == t_next_ea && lsa == t_next_lsa) {
        snprintf(path, sizeof path, "%s/ovl_%08X.bin", s_dir, t_src);
        f = fopen(path, "ab");
    } else { t_src = 0; return; }
    if (f) { fwrite(ls, 1, size, f); fclose(f); }
    t_next_ea = ea + size; t_next_lsa = lsa + size;
}

/* SPU_LOCKGUARD=<hex ea>: the word at that EA is a ticket lock (serving,next
 * halfwords). Log any SPU write that moves either halfword by anything other
 * than 0 or +1 -- a stale whole-line store rolling the lock back. `src` is the
 * new bytes for [ea, ea+size). */
void spu_lockguard_check(spu_context* ctx, uint32_t ea, const uint8_t* src, uint32_t size, const char* what)
{
    static int64_t s_g = -2;
    if (s_g == -2) { const char* e = getenv("SPU_LOCKGUARD"); s_g = e ? (int64_t)strtoul(e, 0, 16) : -1; }
    if (s_g < 0 || !vm_base) return;
    uint32_t g = (uint32_t)s_g;
    if (g < ea || g + 4 > ea + size) return;
    const uint8_t* o = vm_base + g; const uint8_t* n = src + (g - ea);
    unsigned os = (o[0] << 8) | o[1], on = (o[2] << 8) | o[3];
    unsigned ns = (n[0] << 8) | n[1], nn = (n[2] << 8) | n[3];
    int bad = !(ns == os || ns == ((os + 1) & 0xFFFF)) || !(nn == on || nn == ((on + 1) & 0xFFFF));
    if (bad) { static int c; if (c++ < 32)
        fprintf(stderr, "[lockguard] %s img=%d pc=0x%05X ea=0x%08X size=%u: %04X/%04X -> %04X/%04X\n",
                what, ctx ? ctx->image_id : -1, ctx ? (unsigned)(ctx->pc & SPU_LS_MASK) : 0u,
                ea, size, os, on, ns, nn); }
}

/* Claim a resident_code slot for a position-independent (or fixed) code span
 * that landed at `lsa`, from `ea`, with `size` bytes from there in the GET. */
static void spu_pic_claim(spu_context* ctx, const spu_ovl_src* o, uint32_t lsa, uint32_t ea, uint32_t size)
{
        if (lsa + o->span > SPU_LS_SIZE) return;
        /* SPU_RC_SAME_NOGEN (default on; =0: always bump): a claim that
         * rewrites a slot with exactly what it holds changes no span, so the
         * generation (which drops every thread's cached span) stays. EDGE
         * jobs re-claim the same fragments at the same LS address per job. */
        static int s_same_nogen = -1;
        if (s_same_nogen < 0) { const char* e = getenv("SPU_RC_SAME_NOGEN"); s_same_nogen = !(e && *e == '0'); }
        const uint32_t nsize = (o->pic_base && size < o->span) ? size : o->span;
        int same = 0;
        for (unsigned slot = 0; s_same_nogen && slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
            if (ctx->resident_code[slot].image_id && ctx->resident_code[slot].lsa != lsa) continue;
            same = ctx->resident_code[slot].image_id == o->image_id && ctx->resident_code[slot].size == nsize &&
                   ctx->resident_code[slot].source_ea == ea && ctx->resident_code[slot].base == o->pic_base;
            break;   /* the slot the loop below takes */
        }
        if (!same) spu_rc_gen_bump(ctx);   /* see spu_rc_outside (SPU_RC_GEN_CTX) */
        for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
            if (ctx->resident_code[slot].image_id && ctx->resident_code[slot].lsa != lsa)
                continue;
            ctx->resident_code[slot].lsa = lsa;
            /* A position-independent fragment's span is an upper bound
             * (fragments are packed back to back); the GET that brings it
             * in is the whole fragment, so its size is the exact extent. */
            ctx->resident_code[slot].size = (o->pic_base && size < o->span) ? size : o->span;
            ctx->resident_code[slot].source_ea = ea;
            ctx->resident_code[slot].base = o->pic_base;
            ctx->resident_code[slot].image_id = o->image_id;
            s_code_seq[ctx->spu_id & 15][slot] = ++s_code_clock;
            if (o->pic_base && spu_pic_log())
                fprintf(stderr, "[spu-pic] claim slot=%u img=%d ea=0x%08X lsa=0x%05X size=0x%X spu=%u\n",
                        slot, o->image_id, ea, lsa, ctx->resident_code[slot].size, ctx->spu_id);
            return;
        }
        /* All spans taken: a job's fragments land at new addresses every
         * time and the old spans are only freed when a GET overlaps them.
         * Replace the one claimed longest ago: the running job's own
         * fragments (at most six) are always the newest. A span replaced
         * this way only loses its lift (its bytes fall to the
         * interpreter). The claim order is kept per SPU outside the
         * context; a shared spu_id only makes the choice worse, never
         * wrong. */
        { unsigned slot = 0;
          const uint32_t* q = s_code_seq[ctx->spu_id & 15];
          for (unsigned k = 1; k < SPU_RESIDENT_CODE_SLOTS; ++k)
              if (q[k] < q[slot]) slot = k;
          s_code_seq[ctx->spu_id & 15][slot] = ++s_code_clock;
          if (o->pic_base && spu_pic_log())
              fprintf(stderr, "[spu-pic] replace slot=%u img=%d (was %d) ea=0x%08X lsa=0x%05X\n",
                      slot, o->image_id, ctx->resident_code[slot].image_id, ea, lsa);
          ctx->resident_code[slot].lsa = lsa;
          ctx->resident_code[slot].size = (o->pic_base && size < o->span) ? size : o->span;
          ctx->resident_code[slot].source_ea = ea;
          ctx->resident_code[slot].base = o->pic_base;
          ctx->resident_code[slot].image_id = o->image_id; }
}

/* Fragment signatures for the scan below: each position-independent span's
 * first 16 bytes, read from guest memory once it is loaded (the executable's
 * segments are, before any SPU runs), and a 4096-bit filter on their first words. */
static uint64_t s_pic_filter[64];   /* 4096 bits */
static unsigned spu_pic_hash(uint32_t w) { return (w ^ (w >> 12) ^ (w >> 24)) & 4095u; }
static int spu_pic_sigs_build(void);
static int spu_pic_sigs_ready(void)
{
    static int s_state;                    /* 0 not built, 1 building, 2 ready, 3 none */
    int st = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
    if (st >= 2) return st == 2;
    int expect = 0;
    if (__atomic_compare_exchange_n(&s_state, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        st = spu_pic_sigs_build() ? 2 : 3;
        __atomic_store_n(&s_state, st, __ATOMIC_RELEASE);
        return st == 2;
    }
    while ((st = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE)) == 1) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    return st == 2;
}
/* The same signatures for the fragment scan of spu_overlay_note_get: a
 * 65536-bit filter on a multiplicative hash of the first word, and buckets
 * of the entries by that hash, each in table order. The scan used to walk the
 * whole source table for every quadword that passed the 4096-bit filter --
 * about one data quadword in a hundred, 77 entries each -- on every GET of
 * 512 bytes or more. A bucket holds every entry whose first word hashes like
 * the quadword's, in table order, so its first full match is the entry the
 * walk found. SPU_FRAG_SCAN_OLD=1: the old filter and walk. */
#define SPU_FRAG_FILTER_BITS 65536u
#define SPU_FRAG_BUCKETS     1024u
static uint64_t s_frag_filter[SPU_FRAG_FILTER_BITS / 64];
static int16_t  s_frag_head[SPU_FRAG_BUCKETS];
static int16_t  s_frag_next[SPU_OVL_SRC_MAX];
static inline uint32_t spu_frag_hash(uint32_t w) { return (w * 2654435761u) >> 16; }
/* ---- SPU_FRAG_SCAN8 (default on where the CPU has AVX2; =0: off) ----
 * The fragment scan below tests every quadword's first word against the
 * 65536-bit filter (SPU_FRAG_SCAN4: four at a time). A quadword can only hit
 * when its first word IS the first word of a fragment chained in s_frag_head
 * (SPU_FRAG_TEST compares o->first_word == w before anything else), so a
 * prefilter that keeps every such word may drop any other. Four 256-bit maps
 * of those words, each indexed by one byte of a multiplicative hash of the
 * word, live in registers: a word whose bit is clear in any map is no
 * fragment's first word. Eight quadwords a step (AVX2); a step where any of
 * the eight passes all four maps runs the exact test for each of its eight
 * quadwords in order, so the hits, their order and the slot limit are the
 * same. About 4% of data steps pass (random or float data); ~2x faster on a
 * 16 KB data GET than SPU_FRAG_SCAN4 on a Zen 4. The maps are built with the
 * buckets, once (spu_pic_sigs_build). */
static uint32_t s_frag_cmap[4][8];
static inline void spu_frag_cmap_add(uint32_t w)
{
    const uint32_t p1 = w * 2654435761u, p2 = w * 0x85EBCA77u;
    const uint32_t f[4] = { p1 >> 24, (p1 >> 16) & 255u, p2 >> 24, (p2 >> 16) & 255u };
    for (int m = 0; m < 4; m++) s_frag_cmap[m][f[m] >> 5] |= 1u << (f[m] & 31u);
}
#if defined(__clang__) && (defined(__x86_64__) || defined(_M_X64))
#define SPU_FRAG_HAVE_AVX2_PATH 1
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
static int spu_cpu_has_avx2(void)
{
    int r = 0;
#ifdef _MSC_VER
    int i[4];
    __cpuid(i, 0);
    if (i[0] >= 7) {
        __cpuid(i, 1);
        const int osxsave = (i[2] >> 27) & 1, avx = (i[2] >> 28) & 1;
        if (osxsave && avx && (_xgetbv(0) & 6) == 6) {   /* the OS saves the ymm state */
            __cpuidex(i, 7, 0);
            r = (i[1] >> 5) & 1;                          /* EBX bit 5: AVX2 */
        }
    }
#else
    __builtin_cpu_init();
    r = __builtin_cpu_supports("avx2");
#endif
    return r;
}
/* Nonzero when any of the 8 quadwords at p, p+16, ... p+112 has a first word
 * that passes all four maps. */
__attribute__((target("avx2"))) static inline unsigned spu_frag_cand8_avx2(const uint8_t* p, const __m256i* cm)
{
    /* Each 128-bit lane of a..d is one quadword: a = q0|q1, b = q2|q3, ... */
    const __m256i a = _mm256_loadu_si256((const __m256i*)(p));
    const __m256i b = _mm256_loadu_si256((const __m256i*)(p + 32));
    const __m256i c = _mm256_loadu_si256((const __m256i*)(p + 64));
    const __m256i d = _mm256_loadu_si256((const __m256i*)(p + 96));
    /* First words: lane 0 -> q0 q2 q4 q6, lane 1 -> q1 q3 q5 q7. */
    const __m256i w = _mm256_unpacklo_epi64(_mm256_unpacklo_epi32(a, b), _mm256_unpacklo_epi32(c, d));
    const __m256i bswap = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                           3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m256i be = _mm256_shuffle_epi8(w, bswap);                    /* the word as the SPU reads it */
    const __m256i p1 = _mm256_mullo_epi32(be, _mm256_set1_epi32((int)2654435761u));
    const __m256i p2 = _mm256_mullo_epi32(be, _mm256_set1_epi32((int)0x85EBCA77u));
    const __m256i m31 = _mm256_set1_epi32(31);
    /* Map m, field f: dword f >> 5 (vpermd reads the index's low three bits), bit f & 31. */
    const __m256i t0 = _mm256_srlv_epi32(_mm256_permutevar8x32_epi32(cm[0], _mm256_srli_epi32(p1, 29)),
                                         _mm256_and_si256(_mm256_srli_epi32(p1, 24), m31));
    const __m256i t1 = _mm256_srlv_epi32(_mm256_permutevar8x32_epi32(cm[1], _mm256_srli_epi32(p1, 21)),
                                         _mm256_and_si256(_mm256_srli_epi32(p1, 16), m31));
    const __m256i t2 = _mm256_srlv_epi32(_mm256_permutevar8x32_epi32(cm[2], _mm256_srli_epi32(p2, 29)),
                                         _mm256_and_si256(_mm256_srli_epi32(p2, 24), m31));
    const __m256i t3 = _mm256_srlv_epi32(_mm256_permutevar8x32_epi32(cm[3], _mm256_srli_epi32(p2, 21)),
                                         _mm256_and_si256(_mm256_srli_epi32(p2, 16), m31));
    const __m256i all = _mm256_and_si256(_mm256_and_si256(t0, t1), _mm256_and_si256(t2, t3));
    return (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_slli_epi32(all, 31)));
}
/* From off, in steps of eight quadwords while all eight are inside both
 * limits of the scan, the first step holding a candidate (or where the steps
 * end). */
__attribute__((target("avx2"))) static uint32_t spu_frag_skip8_avx2(const uint8_t* ls, uint32_t off,
                                                                     uint32_t size, uint32_t lsa)
{
    __m256i cm[4];
    for (int m = 0; m < 4; m++) cm[m] = _mm256_loadu_si256((const __m256i*)s_frag_cmap[m]);
    while (off + 128 <= size && lsa + off + 112 < SPU_LS_SIZE && !spu_frag_cand8_avx2(ls + off, cm)) off += 128;
    return off;
}
#endif
static int spu_frag_scan8_on(void)
{
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("SPU_FRAG_SCAN8");
#ifdef SPU_FRAG_HAVE_AVX2_PATH
        s = !(e && *e == '0') && spu_cpu_has_avx2();
#else
        (void)e; s = 0;
#endif
    }
    return s;
}
static int spu_pic_sigs_build(void)
{
    int n = 0;
    for (int i = 0; i < s_ovl_src_count; i++) {
        spu_ovl_src* o = &s_ovl_src[i];
        if (!o->pic_base || o->has_sig || !o->src_ea) continue;
        memcpy(o->sig, vm_base + o->src_ea, 16);
        o->first_word = ((uint32_t)o->sig[0] << 24) | ((uint32_t)o->sig[1] << 16) |
                        ((uint32_t)o->sig[2] << 8) | o->sig[3];
        if (!o->first_word) continue;
        o->sig_ready = 1;
        s_pic_filter[spu_pic_hash(o->first_word) >> 6] |= 1ull << (spu_pic_hash(o->first_word) & 63);
        n++;
    }
    int16_t tail[SPU_FRAG_BUCKETS];
    for (unsigned b = 0; b < SPU_FRAG_BUCKETS; b++) s_frag_head[b] = tail[b] = -1;
    for (int i = 0; i < s_ovl_src_count; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        if (!o->sig_ready) continue;
        const uint32_t h = spu_frag_hash(o->first_word), b = h & (SPU_FRAG_BUCKETS - 1);
        s_frag_filter[h >> 6] |= 1ull << (h & 63);
        spu_frag_cmap_add(o->first_word);   /* SPU_FRAG_SCAN8 */
        s_frag_next[i] = -1;
        if (tail[b] < 0) s_frag_head[b] = (int16_t)i; else s_frag_next[tail[b]] = (int16_t)i;
        tail[b] = (int16_t)i;
    }
    return n > 0;
}

/* Index over s_ovl_src for the first-match lookup in spu_overlay_note_get,
 * which runs on EVERY DMA GET: an entry matched by source EA (no signature)
 * and one matched by its 16-byte content signature. Looking through the whole
 * table per GET (God of War: Ascension registers 77 entries; 34 memcmp for
 * each GET of 512 bytes or more) was ~2.5% of every SPU thread in combat.
 *
 * The loop returned the FIRST entry in table order whose test passes, so the
 * index keeps, per key, the lowest index carrying it, and the lookup takes the
 * lower of the EA hit and the signature hit: the same entry. Entries are only
 * appended (registration), never changed, so the index is rebuilt when the
 * count differs from the one it was built for. */
#define SPU_OVL_IDX_SLOTS 2048u                 /* power of two, > 2 x SPU_OVL_SRC_MAX */
static struct {
    int count;                                 /* s_ovl_src_count it was built for, -1 none */
    int ea0;                                   /* lowest EA entry with src_ea 0, or -1 */
    uint32_t ea_key[SPU_OVL_IDX_SLOTS];
    int16_t  ea_idx[SPU_OVL_IDX_SLOTS];        /* -1 empty */
    uint64_t sig_k0[SPU_OVL_IDX_SLOTS], sig_k1[SPU_OVL_IDX_SLOTS];
    int16_t  sig_idx[SPU_OVL_IDX_SLOTS];       /* -1 empty */
} s_ovl_idx = { -1 };
static volatile long s_ovl_idx_lock;
static unsigned spu_ovl_ea_slot(uint32_t ea) { return (ea * 2654435761u) >> 21; }
static unsigned spu_ovl_sig_slot(uint64_t k0, uint64_t k1)
{
    uint64_t h = (k0 ^ (k1 * 0x9E3779B97F4A7C15ull)) * 0xff51afd7ed558ccdull;
    return (unsigned)(h >> 53);
}
static void spu_ovl_index_build(int count)
{
    for (unsigned s = 0; s < SPU_OVL_IDX_SLOTS; s++) { s_ovl_idx.ea_idx[s] = -1; s_ovl_idx.sig_idx[s] = -1; }
    s_ovl_idx.ea0 = -1;
    for (int i = 0; i < count; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        if (!o->has_sig) {
            if (!o->src_ea) { if (s_ovl_idx.ea0 < 0) s_ovl_idx.ea0 = i; continue; }
            unsigned s = spu_ovl_ea_slot(o->src_ea);
            while (s_ovl_idx.ea_idx[s] >= 0 && s_ovl_idx.ea_key[s] != o->src_ea) s = (s + 1) & (SPU_OVL_IDX_SLOTS - 1);
            if (s_ovl_idx.ea_idx[s] < 0) { s_ovl_idx.ea_key[s] = o->src_ea; s_ovl_idx.ea_idx[s] = (int16_t)i; }
        } else {
            uint64_t k0, k1; memcpy(&k0, o->sig, 8); memcpy(&k1, o->sig + 8, 8);
            unsigned s = spu_ovl_sig_slot(k0, k1);
            while (s_ovl_idx.sig_idx[s] >= 0 && (s_ovl_idx.sig_k0[s] != k0 || s_ovl_idx.sig_k1[s] != k1))
                s = (s + 1) & (SPU_OVL_IDX_SLOTS - 1);
            if (s_ovl_idx.sig_idx[s] < 0) { s_ovl_idx.sig_k0[s] = k0; s_ovl_idx.sig_k1[s] = k1; s_ovl_idx.sig_idx[s] = (int16_t)i; }
        }
    }
}
/* The lowest index whose test passes for this GET, or -1. */
static int spu_ovl_first_match(uint32_t ea, const uint8_t* ls, uint32_t size)
{
    const int count = s_ovl_src_count;
    if (__atomic_load_n(&s_ovl_idx.count, __ATOMIC_ACQUIRE) != count) {
        while (__atomic_exchange_n(&s_ovl_idx_lock, 1, __ATOMIC_ACQUIRE)) {
#ifdef _WIN32
            SwitchToThread();
#else
            sched_yield();
#endif
        }
        if (s_ovl_idx.count != count) {
            __atomic_store_n(&s_ovl_idx.count, -1, __ATOMIC_RELEASE);
            spu_ovl_index_build(count);
            __atomic_store_n(&s_ovl_idx.count, count, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&s_ovl_idx_lock, 0, __ATOMIC_RELEASE);
    }
    int best = -1;
    if (ea) {
        unsigned s = spu_ovl_ea_slot(ea);
        while (s_ovl_idx.ea_idx[s] >= 0) {
            if (s_ovl_idx.ea_key[s] == ea) { best = s_ovl_idx.ea_idx[s]; break; }
            s = (s + 1) & (SPU_OVL_IDX_SLOTS - 1);
        }
    } else {
        best = s_ovl_idx.ea0;
    }
    if (size >= 512) {
        uint64_t k0, k1; memcpy(&k0, ls, 8); memcpy(&k1, ls + 8, 8);
        unsigned s = spu_ovl_sig_slot(k0, k1);
        while (s_ovl_idx.sig_idx[s] >= 0) {
            if (s_ovl_idx.sig_k0[s] == k0 && s_ovl_idx.sig_k1[s] == k1) {
                if (best < 0 || s_ovl_idx.sig_idx[s] < best) best = s_ovl_idx.sig_idx[s];
                break;
            }
            s = (s + 1) & (SPU_OVL_IDX_SLOTS - 1);
        }
    }
    return best;
}

void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size)
{
    uint32_t lsa = (uint32_t)(ls - ctx->ls);
    if (spu_stop0_on()) spu_stop0_note_get(ctx, ea, lsa, size);   /* SPU_STOP0_DIAG */
    spu_ovl_dump(ctx, ea, lsa, ls, size);
    /* SPU_NG_SPAN (default on; =0: always walk): a GET that misses the span
     * [lo, hi) all live streamed-code slots of this context lie in can
     * overlap none of them, so there is nothing to evict (spu_rc_outside's
     * per-thread cache; a stale span is larger, which only walks). Most GETs
     * are data that lands outside every code buffer. */
    static int s_ng_span = -1;
    if (s_ng_span < 0) { const char* e = getenv("SPU_NG_SPAN"); s_ng_span = !(e && *e == '0'); }
    if (!s_ng_span || !spu_rc_range_outside(ctx, lsa, size))
    for (unsigned slot = 0; slot < SPU_RESIDENT_CODE_SLOTS; ++slot) {
        if (!ctx->resident_code[slot].image_id) continue;
        uint32_t base = ctx->resident_code[slot].lsa;
        uint32_t end = base + ctx->resident_code[slot].size;
        if (lsa < end && lsa + size > base &&
            (lsa < base || ea != ctx->resident_code[slot].source_ea + (lsa - base))) {
            if (ctx->resident_code[slot].base && spu_pic_log())
                fprintf(stderr, "[spu-pic] evict slot=%u img=%d span=0x%05X+0x%X by GET lsa=0x%05X ea=0x%08X size=0x%X\n",
                        slot, ctx->resident_code[slot].image_id, base, ctx->resident_code[slot].size, lsa, ea, size);
            ctx->resident_code[slot].image_id = 0;
        }
    }
    /* SPU_OVL_LINEAR=1: the table walk the index replaced (A/B check). */
    static int s_ovl_linear = -1;
    if (s_ovl_linear < 0) s_ovl_linear = getenv("SPU_OVL_LINEAR") ? 1 : 0;
    const int first = s_ovl_linear ? -2 : spu_ovl_first_match(ea, ls, size);
    for (int i = first >= 0 ? first : 0; first != -1 && i < s_ovl_src_count; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        int hit = o->has_sig ? (size >= 512 && memcmp(ls, o->sig, 16) == 0)
                             : (o->src_ea == ea);
        if (hit) {
            if (o->span) {
                spu_pic_claim(ctx, o, lsa, ea, size);
                return;
            }
            if (ctx->resident_ovl != o->image_id) {
                ctx->resident_ovl = o->image_id;
                { static int _n = 0; if (_n < 32 && ++_n)
                    fprintf(stderr, "[spu-ovl] img=%d streamed overlay src=0x%08X "
                            "-> LS 0x%05X size=%u -> resident ovl image %d%s\n",
                            ctx->image_id, ea, (uint32_t)(ls - ctx->ls), size,
                            ctx->resident_ovl, o->has_sig ? " (sig match)" : ""); }
            }
            return;
        }
    }
    /* Fragments inside a bigger GET. Some jobs do not stream their code from
     * the executable: the game first copies a job's header and code fragments
     * into a heap buffer, and the job brings the whole block in with one GET
     * (God of War: Ascension's module 207 in the main menu: 0x29C0 bytes from
     * the heap, a 0x60-byte header, then its fragments). No source EA matches,
     * so look for the fragments by content at every quadword of the block; the
     * filter on their first words keeps the scan to one load per quadword. */
    if (size >= 0x200 && spu_pic_sigs_ready()) {
        /* Find them all first: each one's extent ends where the next begins. */
        const spu_ovl_src* hit_o[SPU_RESIDENT_CODE_SLOTS]; uint32_t hit_off[SPU_RESIDENT_CODE_SLOTS];
        unsigned nhit = 0;
        static int s_frag_old = -1;
        if (s_frag_old < 0) s_frag_old = getenv("SPU_FRAG_SCAN_OLD") ? 1 : 0;
        /* SPU_FRAG_SCAN4 (default on; =0: one quadword a step): four quadwords'
         * filter tests per step, one branch for all four; a step that passes
         * runs the exact per-quadword test below for each of them in order,
         * so the hits, their order and the slot limit are the same. Nearly
         * every GET of 512 bytes or more is data, which fails all four. */
        static int s_scan4 = -1;
        if (s_scan4 < 0) { const char* e = getenv("SPU_FRAG_SCAN4"); s_scan4 = !(e && *e == '0'); }
        if (!s_frag_old) {
            /* The exact test of the quadword at OFF (as before SPU_FRAG_SCAN4). */
#define SPU_FRAG_TEST(OFF) do {                                                              \
                const uint32_t off = (OFF);                                                  \
                const uint32_t w = ((uint32_t)ls[off] << 24) | ((uint32_t)ls[off + 1] << 16) | \
                                   ((uint32_t)ls[off + 2] << 8) | ls[off + 3];               \
                const uint32_t h = spu_frag_hash(w);                                         \
                if (!((s_frag_filter[h >> 6] >> (h & 63)) & 1)) break;                       \
                for (int i = s_frag_head[h & (SPU_FRAG_BUCKETS - 1)]; i >= 0; i = s_frag_next[i]) { \
                    const spu_ovl_src* o = &s_ovl_src[i];                                    \
                    if (o->first_word != w || memcmp(ls + off, o->sig, 16) != 0) continue;   \
                    /* Variants of one kernel share their first quadword (God of             \
                     * War: Ascension's fragments at 0x729A40/0x729B80/0x729CC0/             \
                     * 0x729E00 agree for 0x29 bytes, 0x727700/0x727940/0x727B80             \
                     * too): the first of them in the table claimed all, and a               \
                     * job ran another variant's lift. Compare up to 64 bytes, as            \
                     * spu_pic_content_claim does. */                                        \
                    {   uint32_t n = size - off;                                             \
                        if (n > o->span) n = o->span;                                        \
                        if (n > 64u) n = 64u;                                                \
                        if (memcmp(ls + off, vm_base + o->src_ea, n) != 0) continue; }       \
                    hit_o[nhit] = o; hit_off[nhit] = off; nhit++;                            \
                    break;                                                                   \
                }                                                                            \
            } while (0)
            uint32_t off4 = 0;
#ifdef SPU_FRAG_HAVE_AVX2_PATH
            /* SPU_FRAG_SCAN8: steps of eight; a step with a candidate runs the
             * exact test for each of its eight quadwords in order. */
            if (spu_frag_scan8_on())
                while (nhit < SPU_RESIDENT_CODE_SLOTS) {
                    off4 = spu_frag_skip8_avx2(ls, off4, size, lsa);
                    if (!(off4 + 128 <= size && lsa + off4 + 112 < SPU_LS_SIZE)) break;
                    for (uint32_t k = 0; k < 128 && nhit < SPU_RESIDENT_CODE_SLOTS; k += 16) SPU_FRAG_TEST(off4 + k);
                    off4 += 128;
                }
#endif
            /* Steps of four, while all four offsets are inside both limits. */
            if (s_scan4)
                for (; off4 + 64 <= size && lsa + off4 + 48 < SPU_LS_SIZE && nhit < SPU_RESIDENT_CODE_SLOTS; off4 += 64) {
                    uint32_t w0, w1, w2, w3;
                    memcpy(&w0, ls + off4, 4); memcpy(&w1, ls + off4 + 16, 4);
                    memcpy(&w2, ls + off4 + 32, 4); memcpy(&w3, ls + off4 + 48, 4);
                    const uint32_t h0 = spu_frag_hash(__builtin_bswap32(w0)), h1 = spu_frag_hash(__builtin_bswap32(w1));
                    const uint32_t h2 = spu_frag_hash(__builtin_bswap32(w2)), h3 = spu_frag_hash(__builtin_bswap32(w3));
                    if (!(((s_frag_filter[h0 >> 6] >> (h0 & 63)) | (s_frag_filter[h1 >> 6] >> (h1 & 63)) |
                           (s_frag_filter[h2 >> 6] >> (h2 & 63)) | (s_frag_filter[h3 >> 6] >> (h3 & 63))) & 1)) continue;
                    for (uint32_t k = 0; k < 64 && nhit < SPU_RESIDENT_CODE_SLOTS; k += 16) SPU_FRAG_TEST(off4 + k);
                }
            for (uint32_t o4 = off4; o4 + 16 <= size && lsa + o4 < SPU_LS_SIZE && nhit < SPU_RESIDENT_CODE_SLOTS; o4 += 16)
                SPU_FRAG_TEST(o4);
#undef SPU_FRAG_TEST
        } else
        for (uint32_t off = 0; off + 16 <= size && lsa + off < SPU_LS_SIZE && nhit < SPU_RESIDENT_CODE_SLOTS; off += 16) {
            const uint32_t w = ((uint32_t)ls[off] << 24) | ((uint32_t)ls[off + 1] << 16) |
                               ((uint32_t)ls[off + 2] << 8) | ls[off + 3];
            const unsigned h = spu_pic_hash(w);
            if (!((s_pic_filter[h >> 6] >> (h & 63)) & 1)) continue;
            for (int i = 0; i < s_ovl_src_count; i++) {
                const spu_ovl_src* o = &s_ovl_src[i];
                if (!o->sig_ready || o->first_word != w || memcmp(ls + off, o->sig, 16) != 0) continue;
                hit_o[nhit] = o; hit_off[nhit] = off; nhit++;
                break;
            }
        }
        for (unsigned k = 0; k < nhit; k++) {
            const uint32_t stop = k + 1 < nhit ? hit_off[k + 1] : size;
            spu_pic_claim(ctx, hit_o[k], lsa + hit_off[k], ea + hit_off[k], stop - hit_off[k]);
        }
    }
}

/* Match a 16-byte module header against the registered content signatures.
 * Returns the overlay image id, or 0 if none. Used by the WWS job-code dispatch
 * path: the job manager streams a module into the code buffer via a DMA LIST
 * (per-element GETs the single-transfer note_get hook doesn't see as one >=512
 * chunk), so resident_ovl is set at branch-in time by matching the buffer head. */
int spu_overlay_match_sig(const uint8_t hdr[16])
{
    for (int i = 0; i < s_ovl_src_count; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        if (o->has_sig && memcmp(hdr, o->sig, 16) == 0)
            return o->image_id;
    }
    return 0;
}

/* HLE of the taskset Policy Module's task-syscall entry (LS 0xA70). A SPURS task
 * (e.g. the cri_mpv task, image 22) reads syscallAddr from its SpursTasksetContext
 * (LS 0x27C4) and branches to it to perform a task syscall (EXIT/YIELD/WAIT/POLL).
 * The real kernel has the PM code resident at 0xA70; we don't, so we plant 0xA70 as
 * syscallAddr (in the cri dispatch) and INTERCEPT a branch to it here to HLE the
 * syscall. num = r3&0xF (0x10 bit = the "2" variant), args in r4. Adopted from the
 * JonathanDC64/ps3recomp fork (aaea4158) which uses this to run SPURS tasks clean. */
/* Write the event bits a PPU-side Set owed this wait object into guest memory,
 * at object+0x00 (events) and every object+0x30 slot (pendingRecv).
 *
 * Called from the WAIT_SIGNAL handler immediately before control returns to the
 * guest. Doing it at Set time loses a race: the guest reads object+0x30 the
 * instant it resumes, and a task that took the latched-wake path was never
 * parked for the Set to target. Here the guest demonstrably has not read yet.
 *
 * ponytail: fills all 16 slots rather than the one the guest picked -- the slot
 * index is derived from state we do not model, and a wrong slot delivers
 * nothing. Narrow it if a title ever cares which slot fired. */
static void spu_ef_deliver_owed(spu_context* ctx, uint32_t wobj)
{
    (void)ctx;
    static int s_od = -1;
    if (s_od < 0) s_od = getenv("SPURS_EF_OBJ_DELIVER") ? 1 : 0;
    if (!s_od || !wobj || !vm_base) return;
    extern uint16_t spu_ef_bits_take(uint32_t);
    uint16_t bits = spu_ef_bits_take(wobj);
    if (!bits) return;
    uint8_t* m = vm_base + wobj;
    m[0] = (uint8_t)(bits >> 8); m[1] = (uint8_t)bits;          /* +0x00 events */
    for (uint32_t s = 0; s < 16; s++) {                          /* +0x30 slots */
        m[0x30 + 2*s]     = (uint8_t)(bits >> 8);
        m[0x30 + 2*s + 1] = (uint8_t)bits;
    }
    { static int _n = 0; if (_n < 8 && ++_n)
        fprintf(stderr, "[spu] delivered owed bits=0x%04X to object 0x%08X \n",
                (unsigned)bits, wobj); }
}

void spu_spurs_taskset_syscall(spu_context* ctx)   /* non-static: also called by the pure interpreter (spu_interp.c) */
{
    uint32_t raw = ctx->gpr[3]._u32[0];
    uint32_t num = raw & 0x0F;
    /* SPU_SYSCALL_IMG=<n>: every syscall of image n, uncapped (the 24-line
     * budget is spent by FMOD's WAIT_SIGNAL loop long before a later task). */
    static int s_si = -2;
    if (s_si == -2) { const char* e = getenv("SPU_SYSCALL_IMG"); s_si = e ? atoi(e) : -1; }
    { static int _n = 0; if (_n++ < 24 || ctx->image_id == s_si)
        fprintf(stderr, "[spu] SPURS taskset syscall num=%u (raw=0x%X args=0x%08X) image=%d link/r0=0x%05X wobj@2FDC=0x%02X%02X%02X%02X\n",
                num, raw, ctx->gpr[4]._u32[0], ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                ctx->ls[0x2FDC], ctx->ls[0x2FDD], ctx->ls[0x2FDE], ctx->ls[0x2FDF]); }
    /* The task-API argument is passed in LOCAL STORE at 0x2FD0, not in r4: the
     * caller does shufb(arg,...) -> stqd 0x2FD0 and only then sets r3 = number
     * (see func_00026DE0 / func_000272AC in image 22). Logging r4 shows caller
     * leftovers and hides what the task actually asked for. */
    { static int s_sa = -1; if (s_sa < 0) s_sa = getenv("YDKJ_SYSCALL_ARG") ? 1 : 0;
      if (s_sa) { static int _n = 0; if (_n < 40 && ++_n) {
        const uint8_t* a = &ctx->ls[0x2FD0];
        fprintf(stderr, "[spu] syscall num=%u LS[0x2FD0]=%02X%02X%02X%02X %02X%02X%02X%02X"
                        " %02X%02X%02X%02X %02X%02X%02X%02X img=%d\n",
                num, a[0],a[1],a[2],a[3], a[4],a[5],a[6],a[7],
                a[8],a[9],a[10],a[11], a[12],a[13],a[14],a[15], ctx->image_id);
        fflush(stderr); } } }
    /* NOTE (YDKJ cri_mpv): the cri task's BOOTSTRAP (func_00003040) calls the
     * task-API syscall and EXPECTS IT TO RETURN, then branches to the real task
     * entry (0x3050). Halting on num=0 here kills the task at bootstrap before it
     * runs. So for image 22 we DON'T halt on num=0 -- we return so the bootstrap
     * continues to the decode entry. (A genuine end-of-task EXIT would re-enter and
     * spin; if that happens, gate a real halt after the task has done work.)
     * For non-cri images keep the fork's EXIT=halt semantics. Env YDKJ_CRI_EXIT_HALT
     * forces the old halt behaviour for comparison. */
    /* The cri bootstrap calls EXIT once and EXPECTS IT TO RETURN, then branches to
     * the real task entry -- so image 22 cannot halt on the first one. But a task
     * that has since done work and calls EXIT again means it: returning there makes
     * it re-enter and spin (observed: 6x num=0 from link 0x26E18 once the task is
     * woken). Honour the first call, halt on the rest. Each task runs on its own
     * host thread, so a thread-local count is per task. */
    static _Thread_local int s_exit_seen = 0;
    if (num == 0 && ctx->image_id == 22 && !getenv("YDKJ_CRI_EXIT_HALT")) {
        if (s_exit_seen++ == 0) { ctx->gpr[3]._u32[0] = 0; return; }   /* bootstrap */
        { static int _n = 0; if (_n < 8 && ++_n)
            fprintf(stderr, "[spu] cri task EXIT #%d -- halting (was spinning)\n",
                    s_exit_seen); }
        ctx->status = SPU_STATUS_STOPPED_BY_STOP;
        spu_halt(ctx);
        return;
    }
    if (num == 0 && (ctx->image_id != 22 || getenv("YDKJ_CRI_EXIT_HALT"))) {
        ctx->status = SPU_STATUS_STOPPED_BY_STOP;
        spu_halt(ctx);          /* longjmp out to spu_run_with_halt; post-run writes exit code */
        return;
    }
    /* WAIT_SIGNAL(2): REAL semantics (RPCS3 spursTasksetProcessSyscall) --
     * consume the task's bit in the guest taskset's `signalled` bitset, or
     * SLEEP this task's host thread until _cellSpursSendSignal /
     * cellSpursEventFlagSet delivers one (the FMOD mixer's flag-A wait path:
     * the task registers its wait slot in the flag struct with atomics, then
     * syscalls WAIT_SIGNAL; the PPU-side Set satisfies the slot and signals).
     * Returning "success" here without waiting made the task spin on empty
     * work state forever -- the LBP boot deadlock. taskset/taskId come from
     * the SpursTasksetContext this runtime planted at LS 0x2700. */
    if (num == 2) {
        uint32_t ts  = ((uint32_t)ctx->ls[0x27BC] << 24) | ((uint32_t)ctx->ls[0x27BD] << 16) |
                       ((uint32_t)ctx->ls[0x27BE] << 8)  |  (uint32_t)ctx->ls[0x27BF];
        uint32_t tid = ((uint32_t)ctx->ls[0x27D4] << 24) | ((uint32_t)ctx->ls[0x27D5] << 16) |
                       ((uint32_t)ctx->ls[0x27D6] << 8)  |  (uint32_t)ctx->ls[0x27D7];
        /* The guest names its wait OBJECT in the task-API argument at LS 0x2FD0
         * (low nibble is flags). It reads its received event bits from
         * object+0x30, so this is both who to wake and where to deliver. */
        uint32_t wobj = (((uint32_t)ctx->ls[0x2FDC] << 24) |
                         ((uint32_t)ctx->ls[0x2FDD] << 16) |
                         ((uint32_t)ctx->ls[0x2FDE] << 8)  |
                          (uint32_t)ctx->ls[0x2FDF]) & ~0xFu;
        /* SPURS_EF_SPU_REPLY=1 -- DIAGNOSTIC BISECT, NOT A FIX.
         *
         * The guest task is supposed to set its SPU->PPU flag (object+0x100)
         * when a work cycle completes; it never does, so the PPU blocks on that
         * flag forever and can never enqueue the next unit of work. That is a
         * cycle: we cannot see what the PPU would do next without breaking it.
         *
         * On RE-ENTRY to WAIT_SIGNAL for the same object the previous cycle has
         * finished, so set the flag on the task`s behalf and watch where the PPU
         * goes. This FABRICATES guest state -- the repo`s own history says faked
         * success is the most expensive kind of bug (see the unresolved-NID note
         * in ppu_hle.cpp), so it stays opt-in and must never become the default.
         * If the PPU advances, the answer is what the task must do to earn it. */
        { static int s_rp = -1;
          if (s_rp < 0) { const char* e = getenv("SPURS_EF_SPU_REPLY");
                          s_rp = e ? atoi(e) : 0; }   /* 2 = reply on EVERY entry */
          if (s_rp && wobj) {
              static uint32_t seen[8]; static int seen_n = 0;
              int again = 0;
              for (int i = 0; i < seen_n; i++) if (seen[i] == wobj) { again = 1; break; }
              if (s_rp >= 2) again = 1;   /* force: answer "would the PPU move at all?" */
              if (!again) { if (seen_n < 8) seen[seen_n++] = wobj; }
              else {
                  extern void spurs_ef_set_from_spu(uint32_t, uint16_t);
                  spurs_ef_set_from_spu(wobj + 0x100u, 1);
                  static int _n = 0;
                  if (_n++ < 8)
                      fprintf(stderr, "[spu] SPU-REPLY: set flag 0x%08X for object "
                                      "0x%08X (cycle complete)\n", wobj + 0x100u, wobj);
              }
          } }

        if (ts) {
            extern int spu_taskset_wait_signal(uint32_t, uint32_t);
            /* Park = OS-level wait on this SPU's host thread. Under the lockstep
             * gate the thread HOLDS the global run token here; parking without
             * releasing it starves every other lifted SPU (observed: FMOD task 1
             * parks in WAIT_SIGNAL holding the token -> the mixer never runs
             * again -> the PPU audio pump blocks forever on flag 0x94F600). */
            /* Publish that this task is parked so a PPU-side event-flag Set can
             * reach it even though it registered no wait slot in the flag. */
            extern void spu_taskset_parked_add(uint32_t, uint32_t, uint32_t);
            extern void spu_taskset_parked_del(uint32_t, uint32_t);
            extern int spu_taskset_consume_wake(uint32_t);
            if (spu_taskset_consume_wake(ts)) {   /* a Set beat us to the park */
                spu_ef_deliver_owed(ctx, wobj);
                ctx->gpr[3]._u32[0] = 0;
                return;
            }
            yz_lockstep_block_begin(ctx);
            { extern void spu_taskset_note_wait_link(uint32_t);   /* MLAA_DIAG: where it waits */
              spu_taskset_note_wait_link(ctx->gpr[0]._u32[0] & SPU_LS_MASK); }
            spu_taskset_parked_add(ts, tid, wobj);
            spu_taskset_wait_signal(ts, tid);
            spu_taskset_parked_del(ts, tid);
            yz_lockstep_block_end(ctx);
            spu_ef_deliver_owed(ctx, wobj);
        }
        ctx->gpr[3]._u32[0] = 0;
        return;
    }
    /* EXIT(0, cri bootstrap)/YIELD(1)/POLL(3)/RECV_WKL_FLAG(4):
     * report success and resume (return -> lifted caller continues at its link). */
    ctx->gpr[3]._u32[0] = 0;
}

/* SPU_SMC_PROF=1 (diagnostic, off): what each entry into the SMC path below
 * costs. Per (image, resident overlay, entry pc): calls and TSC ticks spent in
 * spu_smc_microstep (its own steps, the interpreter and whatever runs before
 * it returns), with the first 32 code bytes at the entry as first seen. Every
 * 5 s the top SPU_SMC_PROF_TOP (default 40) by ticks, as `[smc-prof]` lines:
 * which entries a lift would pay for, and the bytes to find them by. */
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define SPU_SMC_PROF_SLOTS 8192u
static struct { int used, img, ovl; uint32_t pc; unsigned long long calls, ticks, all_ticks; uint8_t code[32]; }
    s_smc_prof[SPU_SMC_PROF_SLOTS];
static volatile long s_smc_prof_lock;
static void spu_smc_prof_note(int img, int ovl, uint32_t pc, const uint8_t code[32], unsigned long long ticks)
{
    static int s_top = -1; static unsigned long long s_t0, s_total;
    if (s_top < 0) { const char* t = getenv("SPU_SMC_PROF_TOP"); s_top = t && atoi(t) > 0 ? atoi(t) : 40; }
    while (_InterlockedExchange(&s_smc_prof_lock, 1)) _mm_pause();
    unsigned slot = ((pc >> 2) * 2654435761u ^ (unsigned)ovl * 40503u ^ (unsigned)img) & (SPU_SMC_PROF_SLOTS - 1u);
    for (unsigned probe = 0; probe < SPU_SMC_PROF_SLOTS; probe++, slot = (slot + 1) & (SPU_SMC_PROF_SLOTS - 1u)) {
        if (!s_smc_prof[slot].used) {
            s_smc_prof[slot].used = 1; s_smc_prof[slot].img = img; s_smc_prof[slot].ovl = ovl; s_smc_prof[slot].pc = pc;
            memcpy(s_smc_prof[slot].code, code, 32);
        }
        if (s_smc_prof[slot].pc == pc && s_smc_prof[slot].ovl == ovl && s_smc_prof[slot].img == img) {
            s_smc_prof[slot].calls++; s_smc_prof[slot].ticks += ticks; s_smc_prof[slot].all_ticks += ticks;
            break;
        }
    }
    s_total += ticks;
    extern unsigned long long ps3_ms_now(void);
    const unsigned long long now = ps3_ms_now();
    if (!s_t0) s_t0 = now;
    if (now - s_t0 >= 5000) {
        s_t0 = now;
        fprintf(stderr, "[smc-prof] %.1f Mticks in the SMC path in 5 s; top entries (ticks this 5 s, calls, run total):\n",
                (double)s_total / 1e6);
        s_total = 0;
        for (int r = 0; r < s_top; r++) {
            int best = -1;
            for (unsigned i = 0; i < SPU_SMC_PROF_SLOTS; i++)
                if (s_smc_prof[i].used && s_smc_prof[i].ticks &&
                    (best < 0 || s_smc_prof[i].ticks > s_smc_prof[best].ticks)) best = (int)i;
            if (best < 0) break;
            char hex[80]; int o = 0;
            for (int k = 0; k < 32; k++) o += snprintf(hex + o, sizeof hex - (size_t)o, "%02X", s_smc_prof[best].code[k]);
            fprintf(stderr, "[smc-prof]   img=%d ovl=%d start=0x%05X ticks=%llu calls=%llu total=%llu code=%s\n",
                    s_smc_prof[best].img, s_smc_prof[best].ovl, s_smc_prof[best].pc, s_smc_prof[best].ticks,
                    s_smc_prof[best].calls, s_smc_prof[best].all_ticks, hex);
            s_smc_prof[best].ticks = 0;   /* taken; the rest are cleared below */
        }
        for (unsigned i = 0; i < SPU_SMC_PROF_SLOTS; i++) { s_smc_prof[i].ticks = 0; s_smc_prof[i].calls = 0; }
        fflush(stderr);
    }
    _InterlockedExchange(&s_smc_prof_lock, 0);
}

/* ---------------------------------------------------------------------------
 * Micro-interpreter for RUNTIME-GENERATED stub code (SMC).
 *
 * The WWS jobmanager's interrupt handler GENERATES a register save/restore
 * stub above the static image (LS 0x3FEC0: a chain of stqd/lqd plus a `bi`
 * terminator, written at interrupt entry) and calls it. No lift can exist for
 * bytes that only come into being at runtime, so when the indirect dispatch
 * finds no registered function we interpret the LIVE LS bytes directly for
 * the small instruction set such stubs use, and exit back into lifted code at
 * the first branch (trampoline re-dispatch). Returns 1 if it executed to a
 * branch, 0 on an unknown opcode (caller falls through to BRANCH-TO-0). */
#include "spu_smc_stub.h"   /* SPU_SMC_STUB_FAST */
/* SPU_SMC_RET_LR (default on; =0: off). A call into code that only exists at
 * runtime goes `r0 = link; pc = target; spu_indirect_branch; spu_drain_call(
 * link)` -- the bracket's return point is set only AFTER the callee came back,
 * so while the callee runs here ctx->drain_ret_pc is an outer drain's (or 0).
 * The interpreter then never recognised the callee's `bi $r0` back to the
 * bracket: it went on interpreting the caller's code, and every lifted
 * function it met it ran in a drain of its own (SPU_REJOIN_CALL) -- the rest
 * of the job, the job manager's return into its loop, the next job, whose own
 * runtime-built callback started the same again one level deeper. God of War:
 * Ascension's module 218 (EDGE, callback through LS[r3 + 0x8C] from native
 * 0xD158) did that once per job until the SPU thread's host stack overflowed:
 * the game closed with no message (or [STACKOVERFLOW] spu_smc_microstep ->
 * spu_drain_loop -> Wws PM 0x2CF8/0x32A0 -> job 4A5C ... repeating), and the
 * host frames left behind resumed later on a newer job's local store (module
 * 216 branching into an LS slot whose kernel had moved: "job exit at a stop
 * word" at 0x24AA0, stop 0 on data). Now the link the call was made with is a
 * hand-back point as well: reached after at least one instruction, the
 * transfer goes back to the bracket (a pending trampoline at that pc) instead
 * of on through the caller. */
static int spu_smc_ret_lr_on(void)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("SPU_SMC_RET_LR"); s_on = !(e && e[0] == '0'); }
    return s_on;
}

static int spu_smc_microstep(spu_context* ctx)
{
    /* The link this transfer was made with (SPU_SMC_RET_LR, above). */
    const uint32_t entry_lr = spu_smc_ret_lr_on() ? (ctx->gpr[0]._u32[0] & SPU_LS_MASK & ~3u) : 0u;
    /* SPU_SMC_STUB_FAST (default on): the job manager's register save/restore
     * stub in one block, the same effects as the steps below. */
    if (spu_smc_stub_fast_on() && spu_smc_stub_fast(ctx))
        return 1;
    uint32_t pc = ctx->pc & SPU_LS_MASK & ~3u;
    extern void spu_pcwatch(spu_context*, uint32_t);
    for (int steps = 0; steps < 4096; steps++) {
        spu_pcwatch(ctx, pc);
        uint32_t w = ((uint32_t)ctx->ls[pc] << 24) | ((uint32_t)ctx->ls[pc+1] << 16) |
                     ((uint32_t)ctx->ls[pc+2] << 8) | ctx->ls[pc+3];
        uint32_t op11 = w >> 21, op9 = w >> 23, op8 = w >> 24, op7 = w >> 25;
        uint32_t rt = w & 0x7F, ra = (w >> 7) & 0x7F, rb = (w >> 14) & 0x7F;
        int32_t  i10 = (int32_t)(w << 8) >> 22;               /* bits 14-23 sext */
        int32_t  i16 = (int32_t)(int16_t)((w >> 7) & 0xFFFF);
        (void)rb;

        if (op11 == 0x1A8 || op11 == 0x1A9 || op11 == 0x1AA || op11 == 0x1AB) {
            /* bi / bisl / iret / bisled (+E/D interrupt bits) */
            if (op11 == 0x1AB &&                       /* bisled: only on event */
                (ctx->event_status & ctx->event_mask) == 0) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            uint32_t tgt = (op11 == 0x1AA) ? ctx->srr0
                                           : ctx->gpr[ra]._u32[0];
            if (op11 == 0x1A9 || op11 == 0x1AB)
                ctx->gpr[rt] = spu_splat_u32(pc + 4);
            ctx->pc = tgt & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op11 >= 0x128 && op11 <= 0x12B) {
            /* biz / binz / bihz / bihnz rt,ra (+E/D bits, taken-only) */
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op11) {
            case 0x128: taken = (cv == 0); break;                     /* biz  */
            case 0x129: taken = (cv != 0); break;                     /* binz */
            case 0x12A: taken = ((cv & 0xFFFF) == 0); break;          /* bihz */
            default:    taken = ((cv & 0xFFFF) != 0); break;          /* bihnz*/
            }
            if (!taken) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            ctx->pc = ctx->gpr[ra]._u32[0] & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x064 || op9 == 0x060 || op9 == 0x066 || op9 == 0x062) {
            /* br / bra / brsl / brasl */
            uint32_t tgt = (op9 == 0x060 || op9 == 0x062)
                         ? ((uint32_t)i16 << 2)
                         : (pc + ((uint32_t)i16 << 2));
            if (op9 == 0x066 || op9 == 0x062)
                ctx->gpr[rt] = spu_splat_u32(pc + 4);
            ctx->pc = tgt & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x040 || op9 == 0x042 || op9 == 0x044 || op9 == 0x046) {
            /* brz / brnz / brhz / brhnz rt,label (relative) */
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op9) {
            case 0x040: taken = (cv == 0); break;
            case 0x042: taken = (cv != 0); break;
            case 0x044: taken = ((cv & 0xFFFF) == 0); break;
            default:    taken = ((cv & 0xFFFF) != 0); break;
            }
            if (!taken) { pc += 4; continue; }
            ctx->pc = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op8 == 0x24) {                                    /* stqd rt,i10(ra) */
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            spu_ls_write128(ctx, a, ctx->gpr[rt]);
            pc += 4; continue;
        }
        if (op8 == 0x34) {                                    /* lqd rt,i10(ra) */
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x041 || op9 == 0x061) {                   /* stqa / lqa (abs) */
            /* LSA = I16 << 2 (SPU ISA, RI16 form), as in spu_interp.c and
             * the lifter. */
            uint32_t a = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK & ~15u;
            if (op9 == 0x041) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x047 || op9 == 0x067) {                   /* stqr / lqr (rel) */
            uint32_t a = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~15u;
            if (op9 == 0x047) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op8 == 0x44) {                                    /* xori rt,ra,i10 */
            /* Runtime-generated job stubs toggle their own instruction words.
             * Use the signed RI10 immediate, just like statically lifted xori. */
            ctx->gpr[rt] = spu_xori(ctx->gpr[ra], i10);
            pc += 4; continue;
        }
        if (op8 == 0x1C) {                                    /* ai rt,ra,i10 */
            u128 r = ctx->gpr[ra];
            for (int k = 0; k < 4; k++) r._u32[k] += (uint32_t)i10;
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op11 == 0x040 || op11 == 0x0C0) {                 /* sf / a (word) */
            u128 x = ctx->gpr[ra], y = ctx->gpr[rb], r;
            for (int k = 0; k < 4; k++)
                r._u32[k] = (op11 == 0x040) ? y._u32[k] - x._u32[k]
                                            : x._u32[k] + y._u32[k];
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op9 == 0x081) { ctx->gpr[rt] = spu_splat_u32((uint32_t)i16); pc += 4; continue; }  /* il  */
        if (op9 == 0x082) { ctx->gpr[rt] = spu_splat_u32(((w >> 7) & 0xFFFF) << 16); pc += 4; continue; } /* ilhu */
        if (op9 == 0x0C1) { u128 r = ctx->gpr[rt];            /* iohl */
            for (int k = 0; k < 4; k++) r._u32[k] |= (w >> 7) & 0xFFFF;
            ctx->gpr[rt] = r; pc += 4; continue; }
        if (op7 == 0x21)  { ctx->gpr[rt] = spu_splat_u32((w >> 7) & 0x3FFFF); pc += 4; continue; } /* ila */
        if (op11 == 0x201 || op11 == 0x001) { pc += 4; continue; }  /* nop/lnop */
        if (op11 == 0x002 || op11 == 0x003) { pc += 4; continue; }  /* sync/dsync */
        /* Branch hints, all no-ops for execution: hbr is the 11-bit-opcode RR
         * form (0x1AC), hbra and hbrr are the 7-bit-opcode RI18 form (0x08,
         * 0x09) -- the same op7 group as ila (0x21) above, NOT op9. Testing
         * op9 here never matched either (hbrr 0x12033296 has op9 0x24), so a
         * runtime-generated stub carrying a branch hint decoded to UNKNOWN,
         * the microstep bailed, and the SPU fell into branch-to-0. */
        /* sagemono reached the same fix independently in #166, from a different
         * witness (hbrr 0x1200048C rather than 0x12033296). Same line, same
         * diagnosis, arrived at separately -- which is about as good as
         * corroboration gets for a decode bug. */
        if (op11 == 0x1AC || op7 == 0x08 || op7 == 0x09) { pc += 4; continue; } /* hbr/hbra/hbrr */

        /* Hand off to the real interpreter rather than giving up.
         *
         * This microstepper knows a dozen opcodes -- enough for the
         * runtime-generated stubs it was written for -- and spu_interp.c is a
         * full, selftested SPU interpreter that was already sitting here
         * unused on this path. Bailing on the first ordinary instruction is
         * strictly worse than running it.
         *
         * It matters for overlay code: a SPURS job policy module DMAs its job
         * bodies into local store and branches to them, so there is no lifted
         * body to find and the fallback IS the execution path. Guitar Hero
         * III`s job kernel dies here on an `ilh` at LS 0x6638 -- a perfectly
         * ordinary instruction the interpreter handles -- and with it the
         * decompression job it was running never completes. */
        /* Every DISTINCT entry, once, with the resident overlay: each one is
         * code the lift has no function for (a runtime-built return address,
         * a function pointer), and the list is what a re-lift needs as extra
         * entries. A capped count showed the first 8 and hid the hot one. */
        /* SPU_SMC_SEEN_MAX=<n>: how many distinct entries to log (default 256,
         * at most 4096); a combat run filled the 256 and hid the rest. */
        { static struct { int img, ovl; uint32_t pc; } seen[4096]; static int n_seen = 0;
          static int s_seen_max = -1;
          if (s_seen_max < 0) { const char* e = getenv("SPU_SMC_SEEN_MAX");
              s_seen_max = e && atoi(e) > 0 ? (atoi(e) < 4096 ? atoi(e) : 4096) : 256; }
          int known = 0;
          if (s_seen_max <= 256) {
              for (int i = 0; i < n_seen; i++)
                  if (seen[i].pc == pc && seen[i].img == ctx->image_id &&
                      seen[i].ovl == ctx->resident_ovl) { known = 1; break; }
          } else {
              /* Above 256 a hashed lookup: the scan ran on every entry into
               * this path (inside SPU_SMC_PROF's timing) and grew with n_seen. */
              static short s_seen_hash[8192];   /* seen index + 1; at most half full */
              unsigned h = ((pc >> 2) * 2654435761u ^ (unsigned)ctx->resident_ovl * 40503u ^
                            (unsigned)ctx->image_id) & 8191u;
              for (; s_seen_hash[h]; h = (h + 1) & 8191u) {
                  const int i = s_seen_hash[h] - 1;
                  if (seen[i].pc == pc && seen[i].img == ctx->image_id &&
                      seen[i].ovl == ctx->resident_ovl) { known = 1; break; }
              }
              if (!known && n_seen < s_seen_max) s_seen_hash[h] = (short)(n_seen + 1);
          }
          if (!known && n_seen < s_seen_max) {
              seen[n_seen].img = ctx->image_id; seen[n_seen].ovl = ctx->resident_ovl;
              seen[n_seen++].pc = pc;
              /* start= is where the unresolved branch landed -- the address a
               * lift needs as an entry; pc= is only where the microstep gave
               * up, steps instructions further on. */
              fprintf(stderr, "[spu-smc] microstep img=%d ovl=%d start=0x%05X pc=0x%05X word 0x%08X"
                      " not in the microstep set -- handing to the interpreter (steps=%d)\n",
                      ctx->image_id, ctx->resident_ovl, ctx->pc & SPU_LS_MASK & ~3u, pc, w, steps);
              /* SPU_SMC_LSDUMP=<dir>[,<ovl>]: the whole local store at that
               * entry (first 32), so code a job builds or DMAs into its own
               * buffers can be lifted at the address it runs from. */
              /* SPU_SMC_LSDUMP_MAX=<n>: how many such dumps (default 32). */
              static int s_ld = -1, s_ld_ovl = -1, s_ld_n, s_ld_max = 32; static char s_ld_dir[400];
              if (s_ld < 0) { const char* e = getenv("SPU_SMC_LSDUMP"); s_ld = e && *e;
                  if (s_ld) { snprintf(s_ld_dir, sizeof s_ld_dir, "%s", e);
                              char* c = strchr(s_ld_dir, ','); if (c) { *c = 0; s_ld_ovl = atoi(c + 1); } }
                  const char* m = getenv("SPU_SMC_LSDUMP_MAX"); if (m && atoi(m) > 0) s_ld_max = atoi(m); }
              if (s_ld && s_ld_n < s_ld_max && (s_ld_ovl < 0 || s_ld_ovl == ctx->resident_ovl)) {
                  char path[512];
                  snprintf(path, sizeof path, "%s/ls_ovl%d_%05X.bin", s_ld_dir, ctx->resident_ovl,
                           ctx->pc & SPU_LS_MASK & ~3u);
                  FILE* f = fopen(path, "wb");
                  if (f) { fwrite(ctx->ls, 1, SPU_LS_SIZE, f); fclose(f); s_ld_n++; }
              }
          } }
        ctx->pc = pc;
        /* Stop at the drain's return point too: it sits mid-function, so it is
         * no lifted entry, and a job's return would otherwise run on through
         * the caller's code (GH3's song-freeze; see drain_ret_pc). */
        const uint32_t return_pc = ctx->drain_ret_pc;
        uint32_t result;
    interp_again:
        g_spu_interp_stop2 = entry_lr;
        result = spu_interp_run_until(ctx, pc, return_pc);
        g_spu_interp_stop2 = 0;
        /* Back at the link the call was made with: return to the bracket. */
        if (entry_lr && (ctx->pc & SPU_LS_MASK) == entry_lr && (ctx->status & SPU_STATUS_RUNNING) &&
            (!return_pc || entry_lr != return_pc)) {
            static volatile long s_rl;
            if (__atomic_add_fetch(&s_rl, 1, __ATOMIC_RELAXED) <= 4)
                fprintf(stderr, "[spu-smc] img=%d ovl=%d: interpreted callee from 0x%05X returned to its link 0x%05X "
                                "(SPU_SMC_RET_LR)\n", ctx->image_id, ctx->resident_ovl, pc, entry_lr);
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        /* SPU_INTERP_HIST=1: instructions interpreted per (overlay, entry),
         * the top 16 every 5 s -- where interpreter time actually goes (the
         * microstep line above prints each entry once, not its cost). */
        { static int s_h = -1; if (s_h < 0) s_h = getenv("SPU_INTERP_HIST") ? 1 : 0;
          if (s_h) {
              extern uint64_t g_spu_interp_steps;
              extern unsigned long long ps3_ms_now(void);
              enum { HN = 512 };
              static struct { int ovl; uint32_t pc; unsigned long long steps, calls; } h[HN];
              static int hn; static unsigned long long t0;
              /* Unsynchronised (a diagnostic): counts can be lost between SPU
               * threads, but every index stays below HN. */
              int n = hn < HN ? hn : HN, k = 0;
              for (; k < n; k++) if (h[k].pc == pc && h[k].ovl == ctx->resident_ovl) break;
              if (k == n && n < HN) { h[n].ovl = ctx->resident_ovl; h[n].pc = pc; h[n].steps = h[n].calls = 0; hn = n + 1; }
              if (k < HN) { h[k].steps += g_spu_interp_steps; h[k].calls++; }
              unsigned long long now = ps3_ms_now();
              if (!t0) t0 = now;
              if (now - t0 >= 5000) {
                  t0 = now;
                  unsigned long long tot = 0;
                  if (hn > HN) hn = HN;
                  for (int i = 0; i < hn; i++) tot += h[i].steps;
                  fprintf(stderr, "[interp-hist] %llu steps in 5 s, top entries:\n", tot);
                  for (int r = 0; r < 16; r++) {
                      int best = -1;
                      for (int i = 0; i < hn; i++)
                          if (h[i].steps && (best < 0 || h[i].steps > h[best].steps)) best = i;
                      if (best < 0) break;
                      fprintf(stderr, "[interp-hist]   ovl=%d start=0x%05X steps=%llu calls=%llu\n",
                              h[best].ovl, h[best].pc, h[best].steps, h[best].calls);
                      h[best].steps = 0;
                  }
                  for (int i = 0; i < hn; i++) h[i].steps = 0, h[i].calls = 0;
              } } }
        /* SPU_REJOIN_CALL=1: the lifted code the interpreter rejoins runs in a
         * drain of this interpreter's own (spu_drain_interp), and its first
         * host return -- a `bi $lr` back to an interpreted caller, or out of
         * a function entered mid-body -- resumes interpretation at that pc.
         * As a plain transfer that host return fell to the enclosing lifted
         * frame's drain, whose return point it is not: the drain restarted
         * dispatch from the driver and dropped every host frame above the job
         * (Ascension's level-load job, module 204, froze between the loading
         * movies). Under an interpreter's drain already, the transfer below
         * is right: the host return lands with that interpreter. At
         * return_pc the re-run below stops at once and returns. */
        { static int s_rc = -1; if (s_rc < 0) { const char* e = getenv("SPU_REJOIN_CALL"); s_rc = e && *e == '1'; }
          extern SPU_THREAD_LOCAL int g_spu_drain_anyret;
          extern void spu_drain_interp(spu_context*, uint32_t);
          if (s_rc && g_spu_drain_anyret != (int)ctx->host_depth && ctx->image_id >= 0 && (ctx->status & SPU_STATUS_RUNNING) &&
              (!return_pc || (ctx->pc & SPU_LS_MASK) != return_pc) &&
              spu_lifted_probe(ctx, ctx->pc)) {
              { static int _n = 0; if (_n < 8 && ++_n)
                  fprintf(stderr, "[spu-rejoin] img=%d ovl=%d lifted 0x%05X lr=0x%05X depth=%u\n",
                          ctx->image_id, ctx->resident_ovl, ctx->pc & SPU_LS_MASK,
                          ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->host_depth); }
              const int32_t si = (int32_t)ctx->image_id;
              ctx->host_depth++;
              g_spu_trampoline_fn = spu_indirect_branch;
              spu_drain_interp(ctx, return_pc);
              ctx->host_depth--;
              spu_img_restore(ctx, si);
              ctx->drain_ret_pc = return_pc;   /* the inner drain cleared it */
              if (!(ctx->status & SPU_STATUS_RUNNING)) return 1;
              pc = ctx->pc & SPU_LS_MASK;
              /* The rejoined code came back to the call's link: return to the
               * bracket rather than interpret on (SPU_SMC_RET_LR). */
              if (entry_lr && pc == entry_lr && (!return_pc || entry_lr != return_pc)) {
                  g_spu_trampoline_fn = spu_indirect_branch;
                  return 1;
              }
              goto interp_again;
          } }
        /* A live interpreter rejoin is a transfer, not a host return. Nested
         * drains can repair an empty trampoline, but the top-level driver
         * otherwise ends the run before executing the lifted target.
         * An already-owning drain must retain its own host continuation;
         * stopped states and diagnostic exits keep their existing behavior. */
        if (ctx->image_id >= 0 && (ctx->status & SPU_STATUS_RUNNING) &&
            (!return_pc || (ctx->pc & SPU_LS_MASK) != return_pc) &&
            spu_lifted_probe(ctx, ctx->pc)) {
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        return result ? 1 : 0;
    }
    { static int _n = 0; if (_n < 4 && ++_n)
        fprintf(stderr, "[spu-smc] microstep img=%d runaway (4096 steps from 0x%05X)\n",
                ctx->image_id, ctx->pc & SPU_LS_MASK); }
    return 0;
}

/* ---- spu_indirect_branch's fast path (SPU_IB_FAST, default on; =0: off) ----
 * Every `bi` and every return at host depth 0 comes through here: in God of
 * War: Ascension's geometry jobs millions a second. The full resolver below
 * takes a 0x318-byte frame with a stack cookie, reads a dozen diagnostic
 * gates and makes four or five calls (task image, resident code span, one to
 * three registry lookups, the stack-reset check) to find the same function
 * as last time. The fast path takes the branches the resolver would serve
 * with nothing but its lookup -- no interrupt registers to restore, none of
 * the kernel-service, taskset-syscall, image-23 or job-return addresses, no
 * taskset task registered, none of the diagnostics that read env (below) --
 * and answers the lookup from a per-thread memo of the resolver's own
 * resolution chain. Everything else, and every miss of the chain, goes to
 * the full resolver, which does what it always did.
 *
 * The memo answer depends on: the context, pc, image_id, resident_ovl,
 * resident_task (after the pc < 0x3000 clear), the registry (count; append
 * only), and the streamed code slots. A slot gains coverage only through
 * spu_pic_claim, which bumps the claim generation (spu_rc_gen) first; anything else only clears a
 * slot's image_id. So an answer that no slot covers pc stays true until
 * the generation moves, and an answer from slot k stays true while it holds
 * and slot k still has the image, lsa, size and base it was read with (the
 * slots before it cannot have gained pc). Answers with a cross-image report
 * are not kept, so the report fires every time as before; ctx->pic_delta is
 * set on a hit exactly as the lookup would set it. */
typedef struct {
    const spu_context* ctx;
    spu_fn   fn;
    uint32_t pc, gen, regs;                          /* gen: spu_rc_gen(ctx) it was read at */
    int32_t  img, ovl, task;
    int32_t  slot;                                   /* resident_code slot, -1: none */
    uint32_t s_lsa, s_size, s_base; int32_t s_img;   /* that slot as it was read */
    uint32_t gbucket;                                /* spu_rc_gen_bucket(ctx): the generation it holds for */
} spu_ib_memo_ent;
_Static_assert(sizeof(spu_ib_memo_ent) == 64, "one memo entry per cache line");
#define SPU_IB_MEMO_SLOTS 256u                       /* power of two */
static SPU_TLS spu_ib_memo_ent t_ib_memo[SPU_IB_MEMO_SLOTS];
static int s_ib_fast = -1;
static int s_ib_irq_fast;   /* SPU_IB_IRQ_FAST (read by spu_ib_fast_init) */

static __attribute__((noinline)) int spu_ib_fast_init(void)
{
    /* The gates the full resolver reads from the environment (any value
     * turns each on); with one of them set every branch takes the resolver. */
    const char* e = getenv("SPU_IB_FAST");
    const char* m = getenv("SPU_LOOKUP_MEMO");
    const char* pl = getenv("SPU_PIC_LOG");
    const int on = !(e && e[0] == '0') && !(m && m[0] == '0') && !(pl && atol(pl) > 0) &&
                   !getenv("SPU_IBCOV") && !getenv("SPU_POLLTRACE") && !getenv("YDKJ_POLTRACE") &&
                   !getenv("SPU_JOBTRACE") && !getenv("LBP_HLE_JOBDONE") && !getenv("SPU_JOBEXEC");
    s_ib_fast = on;
    /* SPU_IB_IRQ_FAST=1 (default off; my-changes/spu-cg2, T4): the fast path
     * also takes branches made while an interrupt's registers are saved
     * (irq_saved) with interrupts disabled -- the Wws job manager's handler
     * body, its command-dispatch jump tables. There the full resolver's only
     * extra step, spu_irq_regs_maybe_restore, returns 0 and changes nothing
     * (it acts only with int_enable set), so the answer is the same. An iret
     * (interrupts enabled again) still takes the full resolver. */
    { const char* q = getenv("SPU_IB_IRQ_FAST"); s_ib_irq_fast = q && q[0] == '1'; }
    return on;
}

/* SPU_IB_FAST_AB=1 (rsx_live_draw.c's [draws] block): the fast path off in
 * every other window, for an A/B in one scene. Only when it is on by config;
 * memo entries stay valid across an off window (they are checked on use). */
void spu_ib_fast_ab_set(int off)
{
    static int cfg = -1;
    if (cfg < 0) cfg = s_ib_fast >= 0 ? s_ib_fast : spu_ib_fast_init();
    if (cfg > 0) s_ib_fast = off ? 0 : 1;
}

static int s_ib_memo_reval = -1;
static inline int spu_ib_memo_reval_on(void)
{
    if (__builtin_expect(s_ib_memo_reval < 0, 0)) {
        const char* e = getenv("SPU_IB_MEMO_REVAL");
        s_ib_memo_reval = !(e && e[0] == '0');
    }
    return s_ib_memo_reval;
}

static inline uint32_t spu_ib_memo_index(uint32_t pc, int ovl)
{
    return (((pc >> 2) * 2654435761u) ^ ((uint32_t)ovl * 0x9E3779B1u)) >> 24 & (SPU_IB_MEMO_SLOTS - 1u);
}

/* The full resolver's resolution chain, in its order, for a memo miss. */
static __attribute__((noinline)) spu_fn spu_ib_resolve_fill(spu_context* ctx, uint32_t pc, spu_ib_memo_ent* m,
                                                            unsigned gen, uint32_t regs)
{
    int code_owner = 0, slot_at = -1, exact = 1;
    spu_fn fn = spu_resident_code_fn_x(ctx, pc, &code_owner, &slot_at, &exact);
    if (!code_owner && ctx->resident_task)
        fn = spu_lookup_x(pc, ctx->resident_task, &exact);
    if (!code_owner && !fn && ctx->resident_ovl) fn = spu_lookup_x(pc, ctx->resident_ovl, &exact);
    if (!code_owner && !fn) fn = spu_lookup_x(pc, ctx->image_id, &exact);
    if (exact) {
        m->ctx = ctx; m->fn = fn; m->pc = pc; m->gen = gen; m->regs = regs; m->gbucket = spu_rc_gen_bucket(ctx);
        m->img = ctx->image_id; m->ovl = ctx->resident_ovl; m->task = ctx->resident_task;
        m->slot = slot_at;
        if (slot_at >= 0) {
            m->s_lsa = ctx->resident_code[slot_at].lsa;   m->s_size = ctx->resident_code[slot_at].size;
            m->s_base = ctx->resident_code[slot_at].base; m->s_img = ctx->resident_code[slot_at].image_id;
        }
    } else {
        m->ctx = NULL;
    }
    return fn;
}

/* A memo miss. SPU_IB_MEMO_REVAL (default on; =0: every miss runs the
 * chain, as before): an entry for this context, pc, images and registry whose
 * answer no slot gave (slot -1) is the registry chain's -- a function of pc,
 * the three images and the registry alone -- and it holds while no slot
 * covers pc, which spu_rc_outside answers for the slots as they are now (the
 * resolver's own first test). Then the entry is kept at the new generation
 * and answers as a hit would (no pic_delta: none is set for slot -1).
 * Otherwise the chain fills the entry. The generation and registry count are
 * read here, before any slot is read, as the caller read them before. Out of
 * line, so the hit path keeps its small frame. */
static __attribute__((noinline)) spu_fn spu_ib_memo_miss(spu_context* ctx, uint32_t pc, spu_ib_memo_ent* m)
{
    const unsigned gen = spu_rc_gen(ctx);
    const uint32_t regs = s_registry_count;
    if (m->ctx == ctx && m->pc == pc && m->slot < 0 && m->regs == regs && m->img == ctx->image_id &&
        m->ovl == ctx->resident_ovl && m->task == ctx->resident_task &&
        spu_ib_memo_reval_on() && spu_rc_outside(ctx, pc)) {
        m->gen = gen;
        return m->fn;
    }
    return spu_ib_resolve_fill(ctx, pc, m, gen, regs);
}

static __attribute__((noinline)) void spu_indirect_branch_full(spu_context* ctx);

void spu_indirect_branch(spu_context* ctx)
{
    const uint32_t pc = ctx->pc & SPU_LS_MASK;
    if (__builtin_expect(s_ib_fast > 0 || (s_ib_fast < 0 && spu_ib_fast_init()), 1) &&
        (!ctx->irq_saved || (s_ib_irq_fast && !ctx->int_enable)) &&
        pc != SPURS_TASKSET_PM_SYSCALL_LS && pc != SPU_JOB_RETURN_LS &&
        !(ctx->policy_mode && (pc == SPURS_PM_EXIT_TO_KERNEL_LS || pc == SPURS_PM_SELECT_WORKLOAD_LS)) &&
        ctx->image_id != SPU_RESOLVER_ONLY_IMAGE_ID &&
        s_task_elf_count == 0 && s_task_entry_count == 0) {
        ctx->pc = pc;
        if (pc < SPU_TASKSET_TASK_LO) ctx->resident_task = 0;
        const uint32_t regs = s_registry_count;
        spu_ib_memo_ent* m = &t_ib_memo[spu_ib_memo_index(pc, ctx->resident_ovl)];
        spu_fn fn;
        /* m->gbucket is ctx's generation bucket whenever m->ctx == ctx (SPU_RC_GEN_CTX). */
        if ((m->ctx == ctx && m->pc == pc && m->gen == __atomic_load_n(&g_rc_gen_b[m->gbucket].gen, __ATOMIC_ACQUIRE) &&
             m->regs == regs &&
             m->img == ctx->image_id && m->ovl == ctx->resident_ovl && m->task == ctx->resident_task &&
             (m->slot < 0 ||
              (ctx->resident_code[m->slot].image_id == m->s_img && ctx->resident_code[m->slot].lsa == m->s_lsa &&
               ctx->resident_code[m->slot].size == m->s_size && ctx->resident_code[m->slot].base == m->s_base)))) {
            fn = m->fn;
            if (fn && m->slot >= 0 && m->s_base) ctx->pic_delta = m->s_lsa - m->s_base;
        } else {
            fn = spu_ib_memo_miss(ctx, pc, m);   /* SPU_IB_MEMO_REVAL, else the chain */
        }
        if (fn) {
            if (g_spu_stack_reset_count) spu_check_stack_reset(ctx, fn);
#if defined(__clang__)
            __attribute__((musttail)) return fn(ctx);
#else
            fn(ctx);
            return;
#endif
        }
    }
#if defined(__clang__)
    __attribute__((musttail)) return spu_indirect_branch_full(ctx);
#else
    spu_indirect_branch_full(ctx);
#endif
}

/* cellSpursModulePoll's answer (the selectWorkload kernel service below). */
static void spu_pm_select_answer(spu_context* ctx)
{
            /* The kernel's selection refreshes tempArea, here as on entry. */
            { extern void spurs_kernel_temp_area_refresh(spu_context*);
              spurs_kernel_temp_area_refresh(ctx); }
            /* The kernel's answer is a u64 in the preferred doubleword: the
             * workload it selected in the high word, the poll status in the
             * low one (RPCS3, spursKernel1SelectWorkload). cellSpursModulePoll
             * yields when the selected workload is not wklCurrentId, so "keep
             * running" is the module's own id, read back from LS 0x1DC. This
             * answer 0 -- workload 0 selected -- keeps wid 0 running but
             * tells every other policy module to yield after one poll: God of
             * War: Ascension's Wws job managers (wid 14-18) left each run after
             * a single pipeline step, their last jobs unstored. */
            { const uint8_t* k = ctx->ls + 0x1DC;
              const uint32_t cur = ((uint32_t)k[0] << 24) | ((uint32_t)k[1] << 16)
                                 | ((uint32_t)k[2] << 8) | k[3];
              /* Under SPURS_READY_LEVEL the answer is a real selection
               * (cellSpurs.c). Otherwise it stays 0, as it always was: a
               * policy module that never yields would hold its host worker
               * forever, and the scheduler's idle re-runs count on them
               * leaving. */
              extern int spurs_ready_level(void);
              extern uint32_t spurs_poll_select(uint32_t ea, uint32_t cur, uint32_t spu);
              uint32_t sel = 0;
              if (spurs_ready_level()) {
                  const uint8_t* q = ctx->ls + 0x1C4;   /* spurs EA, low word */
                  const uint8_t* n = ctx->ls + 0x1C8;   /* spuNum */
                  sel = spurs_poll_select(((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3],
                                          cur, ((uint32_t)n[0] << 24) | ((uint32_t)n[1] << 16) | ((uint32_t)n[2] << 8) | n[3]);
              }
              ctx->gpr[3] = spu_make_preferred_u32(0);
              ctx->gpr[3]._u32[0] = sel;     /* selected workload */
              ctx->gpr[3]._u32[1] = 0; }     /* poll status: nothing pending */
}

/* nexec (ascension host/spu/wip/nexec): a native executor thread answers a job's branches into code it
 * does not run (the policy module's job API) itself. NULL on every other thread; tested only here, on
 * the path a lookup miss takes. Returns 1 when it handled the branch. */
SPU_THREAD_LOCAL int (*t_spu_ib_hook)(spu_context*) = 0;
static __attribute__((noinline)) void spu_indirect_branch_full(spu_context* ctx)
{
    if (__builtin_expect(t_spu_ib_hook != 0, 0) && t_spu_ib_hook(ctx)) return;
    /* Real SPU bi/bisl mask the target to the 256 KB local store; the high bits
     * of a computed pointer (e.g. a packed handle like 0x7a028803) are ignored.
     * Without this, any indirect branch through such a value fails the lookup
     * and falls into branch-to-0. All lifted funcs live below SPU_LS_SIZE, so
     * masking is a no-op for already-valid targets. */
    ctx->pc &= SPU_LS_MASK;
    /* Interrupt-return register restore (see spu_drain.c spu_irq_regs_save):
     * the WWS handler's save/restore shim lives at unlifted top-of-LS, so we
     * enforce the preserve-all-registers hardware contract here -- the first
     * interrupts-enabled dispatch at the saved srr0 is the irete. */
    { extern int spu_irq_regs_maybe_restore(spu_context*);
      if (ctx->irq_saved && spu_irq_regs_maybe_restore(ctx)) {
          static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
          static int _r = 0;
          if (s_it && _r++ < 200)
              fprintf(stderr, "[irq] IRET restore at srr0=0x%05X\n", ctx->pc & SPU_LS_MASK);
      } }
    /* SPURS kernel services (policy-module runs only): the HLE kernel plants
     * these two reserved addresses as exitToKernelAddr / selectWorkloadAddr in
     * the SpursKernelContext (spurs_policy.c). */
    if (ctx->policy_mode) {
        extern SPU_THREAD_LOCAL volatile unsigned g_spurs_pm_polls, g_spurs_pm_exited;
        if (ctx->pc == SPURS_PM_EXIT_TO_KERNEL_LS) {
            /* Module exit: the workload returned to the kernel (drained/yield).
             * Print gated: fires once per policy run = thousands/sec. */
            g_spurs_pm_exited = 1;
            { static SPU_THREAD_LOCAL int s_t = -1; if (s_t < 0) s_t = getenv("SPURS_PM_TRACE") ? 1 : 0;
              if (s_t) { static SPU_THREAD_LOCAL unsigned long _n = 0; unsigned long n = ++_n;
                if (n <= 64 || (n & 0xFFF) == 0)
                    fprintf(stderr, "[spurs-pm] exit-to-kernel#%lu (r3=0x%08X polls=%u)\n",
                            n, ctx->gpr[3]._u32[0], g_spurs_pm_polls); } }
            ctx->status = SPU_STATUS_STOPPED_BY_STOP;
            spu_halt(ctx);
            return;
        }
        if (ctx->pc == SPURS_PM_SELECT_WORKLOAD_LS) {
            /* cellSpursModulePoll: report "no contention — keep running".
             * (The host scheduler admits bounded workers; this service does not preempt.) */
            unsigned n = ++g_spurs_pm_polls;
            /* The count restarts with every run, so "poll #1" printed once
             * per policy run: gated like exit-to-kernel above. */
            { static SPU_THREAD_LOCAL int s_t = -1; if (s_t < 0) s_t = getenv("SPURS_PM_TRACE") ? 1 : 0;
              if (s_t && (n <= 4 || (n % 4096) == 0))
                fprintf(stderr, "[spurs-pm] poll #%u (r3=0x%08X) -> continue\n",
                        n, ctx->gpr[3]._u32[0]); }
            /* A recorded or replayed SPU function run (spu_iojrn.h) journals the
             * answer: it comes from main memory (tempArea, the ready count). */
            { const spu_iojrn_ops* j = t_spu_iojrn;
              if (__builtin_expect(j != 0, 0) && j->ksvc) j->ksvc(ctx, SPURS_PM_SELECT_WORKLOAD_LS, spu_pm_select_answer);
              else if (__builtin_expect(t_spu_ksvc_hook != 0, 0)) t_spu_ksvc_hook(ctx, SPURS_PM_SELECT_WORKLOAD_LS, spu_pm_select_answer);
              else spu_pm_select_answer(ctx); }
            ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            return;
        }
    }
    /* Synthetic HLE tasks have no resident taskset policy at 0xA70. Real
     * tasksets use the SAME syscall address, so the API table alone is not
     * evidence that this is an HLE context. LLE tasks must enter Sony's policy
     * to yield/select workloads instead of parking an entire SPU host thread.
     * Image 22 is the legacy standalone HLE CRI task runner. */
    if (ctx->pc == SPURS_TASKSET_PM_SYSCALL_LS) {
        uint32_t sc = ((uint32_t)ctx->ls[0x27C4] << 24) | ((uint32_t)ctx->ls[0x27C5] << 16)
                    | ((uint32_t)ctx->ls[0x27C6] << 8)  | ctx->ls[0x27C7];
        /* A task dispatched standalone (no policy module in this local store)
         * runs on the SpursTasksetContext spu_workload.c plants, whose
         * syscallAddr is this address; nothing is lifted at 0xA70 for it, so
         * the only alternative to the HLE is the synthesised-stop exit below
         * -- which is how LBP's audio tasks (images 6/7) ended on their first
         * syscall and left the loading thread waiting on their event flag. */
        int lifted = spu_lookup(SPURS_TASKSET_PM_SYSCALL_LS, ctx->image_id) ||
            (ctx->resident_task && spu_lookup(SPURS_TASKSET_PM_SYSCALL_LS, ctx->resident_task)) ||
            (ctx->resident_ovl  && spu_lookup(SPURS_TASKSET_PM_SYSCALL_LS, ctx->resident_ovl));
        int standalone = !ctx->policy_mode && sc == SPURS_TASKSET_PM_SYSCALL_LS && !lifted;
        if (ctx->image_id == 22 || standalone ||
            (ctx->policy_mode && sc == SPURS_TASKSET_PM_SYSCALL_LS)) {
            spu_spurs_taskset_syscall(ctx);
            ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            return;
        }
    }
    /* YDKJ_CRI_R4: the taskset policy entry (LS 0xA00, image 23) writes r4 into
     * SpursTasksetContext.taskset @LS 0x27B8 (per RPCS3 cellSpursSpu.cpp). Our
     * kernel->policy handoff doesn't convey the taskset EA, so the policy DMAs
     * the taskset from garbage -> waiting!=0 -> wrong resume path -> savedContextLr=0.
     * Inject r4 = taskset EA (0x0F000000) at the policy entry dispatch (this is the
     * exact point before the entry reads r4, after the kernel's arg setup). */
    if (ctx->image_id == SPU_RESOLVER_ONLY_IMAGE_ID && !getenv("YDKJ_NO_CRI_R4")) {
        static int s_r4 = -1; if (s_r4 < 0) s_r4 = getenv("YDKJ_CRI_CHAIN") ? 1 : 0;
        if (s_r4) {
            /* Force ctxt->taskset @LS 0x27B8 = the REAL game taskset EA on every
             * image-23 branch, so the policy's atomic reads + context-EA computation
             * use the actual taskset (the r4 handoff sets it to garbage 0x0000FFFF via
             * a path we can't intercept). Was hardcoded 0x0F000000, which mismatched
             * the game's real taskset (0x45F1B000) -> policy DMA'd garbage. */
            extern uint32_t g_ydkj_real_taskset_ea;
            uint32_t ts = g_ydkj_real_taskset_ea ? g_ydkj_real_taskset_ea : 0x0F000000u;
            ctx->ls[0x27B8]=0x00; ctx->ls[0x27B9]=0x00; ctx->ls[0x27BA]=0x00; ctx->ls[0x27BB]=0x00;
            ctx->ls[0x27BC]=(uint8_t)(ts>>24); ctx->ls[0x27BD]=(uint8_t)(ts>>16); ctx->ls[0x27BE]=(uint8_t)(ts>>8); ctx->ls[0x27BF]=(uint8_t)ts;
            if (ctx->pc == 0xA00u) { static int _n=0; if (_n < 4 && ++_n)
                fprintf(stderr, "[cri-r4] policy entry pc=0xA00: forced ctxt->taskset LS[0x27B8]=0x0F000000\n"); }
        }
    }
    /* SPU_IBCOV: image-3 (Bink SPU) PC-page coverage. Track which 0x1000-byte LS
     * pages the task's indirect branches land in; dump the set periodically. If
     * coverage stays in the kernel/wait region (~0x13xxx) the decode routine never
     * runs; if it spans a wide high range, decode executes but doesn't output. */
    { static int s_cov = -1; if (s_cov < 0) s_cov = getenv("SPU_IBCOV") ? 1 : 0;
      if (s_cov && ctx->image_id == 3) {
        static uint8_t pages[64] = {0};   /* 64 pages * 0x1000 = 256KB LS */
        static uint64_t hits = 0;
        uint32_t pg = (ctx->pc & 0x3FFFF) >> 12;
        int newp = 0;
        if (pg < 64 && !pages[pg]) { pages[pg] = 1; newp = 1; }
        ++hits;
        if (newp || (hits % 200000) == 0) {
            char line[400]; int p = snprintf(line, sizeof line, "[ibcov3] hits=%llu tgt=0x%05X pages:", (unsigned long long)hits, ctx->pc & 0x3FFFF);
            for (int i = 0; i < 64; i++) if (pages[i]) p += snprintf(line+p, sizeof(line)-p, " 0x%X", i<<12);
            fprintf(stderr, "%s\n", line); }
      } }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_n = 0; static uint32_t s_last = 0; static uint64_t s_run = 0;
        if (ctx->pc == s_last) s_run++; else { s_last = ctx->pc; s_run = 1; }
        if ((++s_n % 2000000) == 0)
          fprintf(stderr, "[ibranch] %llu indirect branches; current target=0x%05X run=%llu\n",
                  (unsigned long long)s_n, ctx->pc, (unsigned long long)s_run); } }
    /* Policy-entry trace: the SPURS policy at LS 0xA00 branches on
     * r8 = word at LS[r6] (must be 32 for the path that sets the dispatch ptr
     * LS[0x780]). Log r3..r6 + the word the kernel handed it, to see why the
     * wrong branch is taken. Env YDKJ_POLTRACE. */
    if (ctx->pc == 0xA00u) {
        static int64_t pt=-2; if (pt==-2){ const char* e=getenv("YDKJ_POLTRACE"); pt=e?1:0; }
        if (pt) { static int _p=0; if (_p < 8 && ++_p) {
            /* re-lifted policy entry uses r80 (kernel-set context base): r44=LS[r80+0xC0] */
            uint32_t r80=ctx->gpr[80]._u32[0] & SPU_LS_MASK;
            const uint8_t* q = ctx->ls + ((r80+0xC0)&SPU_LS_MASK);
            uint32_t ctxw = ((uint32_t)q[0]<<24)|((uint32_t)q[1]<<16)|((uint32_t)q[2]<<8)|q[3];
            fprintf(stderr, "[POLTRACE] policy@0xA00 r3=%08X r4=%08X r80=%08X  LS[r80+0xC0]=%08X\n",
                ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], r80, ctxw);
            fflush(stderr);
        } }
    }
    /* Resolve both fresh launches and mid-function resumes from TaskInfo.
     * A scheduler call temporarily leaves the task region, but its return PC
     * is usually not an ELF entry. Prefer the policy's selected ELF so tasks
     * sharing an entry address cannot shadow one another. Keep the legacy
     * entry-only mapping for runners that have not registered ELF metadata. */
    if (ctx->pc < SPU_TASKSET_TASK_LO) {
        ctx->resident_task = 0;
    } else {
        int ti = spu_taskset_resident_image(ctx);
        if (ti && !ctx->resident_task && ctx->host_depth &&
            s_spu_halt_armed && !ctx->policy_mode) {
            /* The policy restored a task's guest registers and branches to
             * its saved PC. Its host frames still belong to the scheduler or
             * an earlier task invocation; none can satisfy this task's return.
             * Resume at depth zero so SPU_RET follows the restored guest link. */
            ctx->resident_task = ti;
            g_spu_trampoline_fn = 0;
            longjmp(s_spu_halt_env, 2);
        }
        if (!ti && !ctx->resident_task) ti = spu_taskset_task_image(ctx->pc);
        if (ti) ctx->resident_task = ti;
    }
    /* The launched task owns the shared task-code region: resolve it there FIRST
     * so a co-resident task at the same LS base cannot shadow it. (resident_task
     * is 0 outside the region, so this only ever fires for genuine task code.) */
    int code_owner = 0;
    spu_fn fn = spu_resident_code_fn(ctx, ctx->pc, &code_owner);
    if (!code_owner && ctx->resident_task)
        fn = spu_lookup(ctx->pc, ctx->resident_task);
    /* Resident overlay next: streamed code overwrote that LS range, so its lift
     * is the truth there -- the base image's stale bytes at the same addresses
     * may also be registered (historical junk lifts) and must lose. */
    if (!code_owner && !fn && ctx->resident_ovl) fn = spu_lookup(ctx->pc, ctx->resident_ovl);
    if (!code_owner && !fn) fn = spu_lookup(ctx->pc, ctx->image_id);
    /* The job returned through the link register we planted: it is finished.
     * Its outermost frame ends in `bi $r0`, and r0 was 0 -- so without this the
     * return landed on LS 0, which is the job's OWN entry, and it ran a second
     * lap with dead registers: re-reading its parameters through a now-zero
     * context pointer and spinning on a DMA to what were really its own
     * opcodes. The work is already done by the time it returns. */
    if (!ctx->policy_mode && ctx->pc == SPU_JOB_RETURN_LS) {
        static int _n = 0;
        if (_n++ < 4)
            fprintf(stderr, "[spurs-job] img=%d returned to the job manager \n",
                    ctx->image_id);
        spu_halt(ctx);
        return;
    }
    /* Branch into high LS with no lifted code there.
     *
     * This was added believing the jobs called a resident SPURS job-manager
     * kernel at 0x16100 / 0x18160 / 0x1A100. That was WRONG. Those "absolute
     * branches" were the disassembler decoding ASCII as instructions: the
     * bytes behind one of them are 30 2C 20 43, i.e. "0, C" from the string
     * "ch0, F:0:400, 100, Channel 0 level, %". Every job embeds the same
     * parameter-description text, which is why the same phantom target
     * appeared in 9 of 12 unrelated images and looked like a shared ABI.
     *
     * The guard is kept because branching into unlifted high local store is
     * still an error worth stopping at rather than executing whatever is
     * there -- but it is a backstop, not a kernel interface. */
    /* SPU exit: the CRT does not branch to a `stop` in the image -- it BUILDS one
     * in memory and jumps to it. ps1_netemu's GPU core ends with
     *
     *     ori $r80,$r3,0 / andi $r80,$r80,255 / iohl $r80,0x2000
     *     stqd $r80,0x10($r1) / sync / ai $r3,$r1,16 / bi $r3
     *
     * i.e. `stop (0x2000 | status)` assembled onto the stack and executed there.
     * That target is a stack address with no lifted code, so the unlifted-branch
     * guard below called a clean exit "branched into unlifted LS 0x3FFB0 -- ending
     * the job" and reported stop_code 0, which reads as a runaway and sent a long
     * chase after phantom memory corruption. Decode the target word instead: a
     * `stop` is opcode 0 in the top 11 bits, so this is unambiguous. */
    if (!fn) {
        uint32_t p0 = ctx->pc & SPU_LS_MASK;
        if (p0 + 3 < SPU_LS_SIZE) {
            uint32_t w = ((uint32_t)ctx->ls[p0] << 24) | ((uint32_t)ctx->ls[p0+1] << 16) |
                         ((uint32_t)ctx->ls[p0+2] << 8) | ctx->ls[p0+3];
            if ((w >> 21) == 0u) {                 /* stop / stopd */
                ctx->stop_code = w & 0x3FFFu;
                static int _n = 0;
                if (_n++ < 8)
                    fprintf(stderr, "[spu] img=%d exit: synthesised stop 0x%04X at LS 0x%05X\n",
                            ctx->image_id, ctx->stop_code, p0);
                spu_job_synth_stop_diag(ctx, p0);
                spu_halt(ctx);
                return;
            }
        }
    }

    if (!fn && !ctx->policy_mode && ctx->pc >= SPU_JM2_KERNEL_BASE) {
        static uint32_t seen[16]; static int n_seen = 0;
        int known = 0;
        for (int i = 0; i < n_seen; i++) if (seen[i] == ctx->pc) { known = 1; break; }
        if (!known && n_seen < 16) {
            seen[n_seen++] = ctx->pc;
            fprintf(stderr, "[spu] img=%d branched into unlifted LS 0x%05X "
                    "(lr=0x%05X) -- ending the job\n",
                    ctx->image_id, ctx->pc, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
            { fprintf(stderr, "      last dispatched PCs (oldest first):");
              for (unsigned q = 0; q < 8; q++) {
                  unsigned idx = (g_spu_pch_n + q) & 7u;
                  if (g_spu_pch_n > q || g_spu_pch[idx]) fprintf(stderr, " 0x%05X", g_spu_pch[idx]);
              }
              fprintf(stderr, "%c", 10); }
            /* Is there real code at the target, or is the pc garbage? Eight
             * words at the target and at the return address separate "the lift
             * missed a function" from "this branch should never have happened". */
            { uint32_t a[2]; a[0] = ctx->pc & SPU_LS_MASK;
              a[1] = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
              for (int k = 0; k < 2; k++) {
                  fprintf(stderr, "      LS[0x%05X]:", a[k]);
                  for (uint32_t o = 0; o < 32 && a[k] + o + 3 < SPU_LS_SIZE; o += 4)
                      fprintf(stderr, " %02X%02X%02X%02X",
                              ctx->ls[a[k]+o], ctx->ls[a[k]+o+1],
                              ctx->ls[a[k]+o+2], ctx->ls[a[k]+o+3]);
                  fprintf(stderr, "\n");
              } }
            fflush(stderr);
        }
        /* SPU_INTERP_UNLIFTED=1: if real code is present at the target, run it
         * through the interpreter instead of ending the job. No lift can exist
         * for a module the title loads at runtime -- You Don't Know Jack's FMOD
         * mixer relocates a DSP plugin into local store and calls it -- and the
         * interpreter rejoins the compiled path as soon as it reaches a lifted
         * address, which is what the plugin's return does. Opt-in so titles
         * that reach here on a genuinely bad pc keep the loud stop. */
        { uint32_t p = ctx->pc & SPU_LS_MASK;
          uint32_t w0 = ((uint32_t)ctx->ls[p] << 24) | ((uint32_t)ctx->ls[p+1] << 16) |
                        ((uint32_t)ctx->ls[p+2] << 8) | ctx->ls[p+3];
          static int s_iu = -1;
          if (s_iu < 0) { const char* e = getenv("SPU_INTERP_UNLIFTED"); s_iu = e ? 1 : 0; }
          if (s_iu && w0) {
              static int _n = 0;
              if (_n++ < 8)
                  fprintf(stderr, "[spu] img=%d interpreting unlifted LS 0x%05X "
                          "(runtime-loaded code)\n", ctx->image_id, p);
              spu_interp_run(ctx, p);
              return;
          } }
        spu_halt(ctx);
        return;
    }
    /* WWS job-code overlay match: the PM (image 2) streams a job module into the
     * code buffer (base = LS[0x1320]) and branches to base+entryOffset. Its 16-byte
     * ila header identifies which of the 89 lifted job modules it is; match it and
     * mark that overlay resident so the module's lifted functions dispatch. The
     * module arrives via a DMA LIST, so the single-GET note_get hook never fired. */
    if (!fn && ctx->image_id == 2 && ctx->pc >= 0x4000 && ctx->pc < 0x3E000) {
        uint32_t codeBase = ((uint32_t)ctx->ls[0x1320] << 24) | ((uint32_t)ctx->ls[0x1321] << 16) |
                            ((uint32_t)ctx->ls[0x1322] << 8) | ctx->ls[0x1323];
        if (codeBase + 16 <= SPU_LS_SIZE) {
            extern int spu_overlay_match_sig(const uint8_t hdr[16]);
            int img = spu_overlay_match_sig(&ctx->ls[codeBase]);
            if (img) {
                if (ctx->resident_ovl != img) {
                    ctx->resident_ovl = img;
                    static int _n = 0; if (_n < 32 && ++_n)
                        fprintf(stderr, "[spu-ovl] WWS job module resident: codeBase=0x%05X "
                                "pc=0x%05X -> overlay image %d\n", codeBase, ctx->pc & SPU_LS_MASK, img);
                }
                fn = spu_lookup(ctx->pc, img);
            }
        }
    }
    /* WWS job-code fallback: the jobmanager PM (image 2) stages each job's
     * code into an LS buffer and branches into it. The staged blobs are
     * lifted as their own images at their LS residence addresses (image 1 =
     * job_wws_7BD900 at the 0x14400-region buffer); the PM's own table can
     * never contain them. On a policy-module miss inside the job-buffer
     * range, retry against the job image before falling to the interpreter. */
    /* WWS job-code fallback -- but ONLY when real code is staged at the target.
     * A PM job whose code buffer is empty (the {0xA400,0} null-code placeholder)
     * must NOT resolve to image-1's unrelated function that happens to share the
     * file offset: that ran garbage, the job did no work, and its ring slot
     * never advanced -> the loader's job ring deadlocked. An empty buffer falls
     * through to the null-job handling below. */
    if (!fn && ctx->image_id == 2 && ctx->policy_mode &&
        ctx->pc >= 0x4000 && ctx->pc < 0x3E000) {
        uint32_t w0 = ((uint32_t)ctx->ls[ctx->pc] << 24) | ((uint32_t)ctx->ls[ctx->pc+1] << 16) |
                      ((uint32_t)ctx->ls[ctx->pc+2] << 8) | ctx->ls[ctx->pc+3];
        if (w0 != 0)
            fn = spu_lookup(ctx->pc, 1);
    }
    /* Null-code PM job (empty buffer): return control to the dispatcher's own
     * host-call bracket so its post-job continuation runs the completion
     * bookkeeping -- exactly as a real job that did nothing and returned.
     * (Falling to the SMC microstep would log a spurious BRANCH-TO-0 on the
     * 0x00000000 word; this is the same outcome, quiet.) */
    if (!fn && ctx->image_id == 2 && ctx->policy_mode &&
        ctx->pc >= 0x4000 && ctx->pc < 0x3E000 &&
        ((uint32_t)ctx->ls[ctx->pc] | ctx->ls[ctx->pc+1] |
         ctx->ls[ctx->pc+2] | ctx->ls[ctx->pc+3]) == 0) {
        static int _n = 0;
        if (_n++ < 8) {
            /* Ground-truth dump of the WWS code-buffer resolution at dispatch
             * (wwsjob-rosetta offsets). jobEntry = lsaJobCodeBuffer + [buf+0x10];
             * lsaJobCodeBuffer = Gbt_bufferPageNum<<10 from the RunJob command.
             * If the code buffer really is 0x4000 but empty -> the code LOAD is
             * the bug; if lsaJobCodeBuffer should be 0x14400 -> the RunJob
             * buffer-set resolution is. Dump both + the bufferSetArray rows. */
            uint32_t lsaCode = (uint32_t)ctx->ls[0x1330]<<24 | ctx->ls[0x1331]<<16 |
                               ctx->ls[0x1332]<<8 | ctx->ls[0x1333];
            uint32_t runJob  = (uint32_t)ctx->ls[0x12E0]<<24 | ctx->ls[0x12E1]<<16 |
                               ctx->ls[0x12E2]<<8 | ctx->ls[0x12E3];
            const uint8_t* bs = ctx->ls + 0xDF0;   /* g_WwsJob_bufferSetArray */
            fprintf(stderr, "[pm-jobres] pc=0x%05X lsaJobCodeBuffer(0x1330)=0x%08X "
                    "runJobNum(0x12E0)=0x%08X code@14400:%02X%02X%02X%02X\n"
                    "           bufferSetArray[0xDF0]: %02X%02X%02X%02X %02X%02X%02X%02X "
                    "%02X%02X%02X%02X %02X%02X%02X%02X\n",
                    ctx->pc & SPU_LS_MASK, lsaCode, runJob,
                    ctx->ls[0x14400],ctx->ls[0x14401],ctx->ls[0x14402],ctx->ls[0x14403],
                    bs[0],bs[1],bs[2],bs[3], bs[4],bs[5],bs[6],bs[7],
                    bs[8],bs[9],bs[10],bs[11], bs[12],bs[13],bs[14],bs[15]);
        }
        return;   /* enclosing SPU_DRAIN bracket resumes the dispatcher */
    }
    /* JOB-BODY EXECUTION probe: the WWS job manager enters a staged job via
     * `bie pJobCode` (changeloadtorunjob.spu), an INDIRECT branch to
     * lsaJobCodeBuffer + entryOffset. A successful dispatch with the pc inside
     * the job code buffer and a jobmod overlay resident (image ids 200+) is the
     * proof the lifted job module actually runs. Env SPU_JOBEXEC=1. */
    /* SPU_JOBTRACE=1: log EVERY dispatcher entry while a job module (overlay
     * id >= 200) is resident -- the job's cross-function control-flow trail.
     * The pm-flow drain hook can't see a job that performs no channel ops
     * (LBP's frozen job runs as a complete no-op: entry, no DMAs, return);
     * this names its early-out path instead. */
    { static int s_jt = -1;
      if (s_jt < 0) { const char* e = getenv("SPU_JOBTRACE"); s_jt = e ? 1 : 0; }
      if (s_jt && ctx->resident_ovl >= 200) { static int _n = 0;
        uint32_t p = ctx->pc & SPU_LS_MASK;
        /* Full trail, but only start logging once the job body proper begins
         * (pc in the job code buffer >= 0x4000) so the cap isn't spent on the
         * PM code the job calls back into -- and keep logging thereafter. */
        static int _armed = 0;
        if (p >= 0x4000) _armed = 1;
        if (_armed && _n++ < 1500) {
            fprintf(stderr, "[jobtrace] pc=0x%05X lr=0x%05X%s",
                    p, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                    fn ? "" : " (no lift)");
            /* JobApi entries: args r3-r5. Post-job return 0x3308: result r3 +
             * the job's saved GetBufferTag result r80. */
            if (p == 0x2F30 || p == 0x1700 || p == 0x1770 || p == 0x17C8)
                fprintf(stderr, " args r3=%08X.%08X r4=%08X r5=%08X",
                        ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                        ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0]);
            if (p == 0x3308 || p == 0x3258)
                fprintf(stderr, " r3=%08X.%08X.%08X.%08X r80=%08X.%08X",
                        ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                        ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3],
                        ctx->gpr[80]._u32[0], ctx->gpr[80]._u32[1]);
            /* Mid-GetBufferTag: r8 = the set/buf id bytes computed at 0x2F30
             * entry; if wrong here, something clobbered it ACROSS the
             * GetLogicalBuffer call (interrupt delivery?). */
            if (p == 0x2F6C)
                fprintf(stderr, " r8=%08X r10=%08X r13=%08X r15=%08X int=%d",
                        ctx->gpr[8]._u32[0], ctx->gpr[10]._u32[0],
                        ctx->gpr[13]._u32[0], ctx->gpr[15]._u32[0],
                        ctx->int_enable);
            if (p == 0x3030)   /* final BufferTag in r7 (result) */
                fprintf(stderr, " BufferTag r7=%08X.%08X",
                        ctx->gpr[7]._u32[0], ctx->gpr[7]._u32[1]);
            fprintf(stderr, "\n");
        }
      } }
    /* LBP_HLE_JOBDONE=1: HLE the WWS job COMPLETION. Our lifted SPURS PM never
     * reaches the store/DecrementDependency phase (the multi-SPU barrier never
     * converges), so a job's completion word -- passed by the PPU as param[4],
     * an EA in the 0x470Axxxx pool, embedded in the job's command list at LS
     * ~0xC00 -- is never written, and the PPU JobManagerWorker (guest-fn
     * 0x521BD4) spins on it forever (loading freeze). When a job body actually
     * dispatches (overlay >=200, entry 0x4A40), scan its staged command list
     * for the completion EA and write it nonzero, unblocking the worker. This
     * fakes ONLY the done-signal (not the job's data DMA), so it tests how far
     * the boot gets once loading stops deadlocking. */
    if (ctx->pc == 0x4A40 && ctx->resident_ovl >= 200) {
        static int s_hd = -1;
        if (s_hd < 0) { const char* e = getenv("LBP_HLE_JOBDONE"); s_hd = e ? 1 : 0; }
        if (s_hd) {
            extern uint8_t* vm_base;
            int wrote = 0;
            for (uint32_t o = 0xC00; o + 4 <= 0xE80 && wrote < 4; o += 4) {
                uint32_t w = (ctx->ls[o]<<24)|(ctx->ls[o+1]<<16)|(ctx->ls[o+2]<<8)|ctx->ls[o+3];
                if ((w & 0xFFFF0000u) == 0x470A0000u && vm_base) {
                    /* write nonzero to the completion word in main RAM (BE) */
                    if (vm_base[w]==0 && vm_base[w+1]==0 && vm_base[w+2]==0 && vm_base[w+3]==0) {
                        vm_base[w+3] = 1;
                        static int _n = 0; if (_n < 24 && ++_n)
                            fprintf(stderr, "[hle-jobdone] wrote completion 0x%08X=1 (job overlay %d)\n",
                                    w, ctx->resident_ovl);
                        wrote++;
                    }
                }
            }
        }
    }
    if (fn && ctx->pc >= 0x4000 && ctx->resident_ovl >= 200) {
        static int s_je = -1;
        if (s_je < 0) { const char* e = getenv("SPU_JOBEXEC"); s_je = e ? 1 : 0; }
        if (s_je) { static int _n = 0; if (_n < 24 && ++_n)
            fprintf(stderr, "[jobexec] DISPATCH job code pc=0x%05X overlay=%d "
                    "runJobNum=0x%02X%02X%02X%02X\n",
                    ctx->pc & SPU_LS_MASK, ctx->resident_ovl,
                    ctx->ls[0x12E0], ctx->ls[0x12E1], ctx->ls[0x12E2], ctx->ls[0x12E3]);
          /* SPU_JOBEXEC_DUMP=<path>: write the full 256KB LS the first time a job
           * entry (pc==codeBuffer+entryOffset) dispatches, to diff vs the RPCS3
           * oracle SPU4 dump (why our job bails where the oracle's runs). */
          static int s_dumped = 0;
          if (!s_dumped && ctx->pc == 0x4A40) {
              const char* dp = getenv("SPU_JOBEXEC_DUMP");
              if (dp) { FILE* f = fopen(dp, "wb");
                  if (f) { fwrite(ctx->ls, 1, SPU_LS_SIZE, f); fclose(f);
                      fprintf(stderr, "[jobexec] dumped LS at job entry -> %s\n", dp); }
                  s_dumped = 1; } } }
    }
    if (fn) {
        spu_check_stack_reset(ctx, fn);
        /* MUSTTAIL: a guest loop that iterates through an indirect branch (the
         * Bink decoder's per-command dispatch does) must not grow the host
         * stack -- a plain call here leaked a resolver+callee frame per
         * iteration and blew the thread stack ~4k iterations into the first
         * really-decoding movie frame (silent 0x80000001 death). */
#if defined(__clang__)
        __attribute__((musttail)) return fn(ctx);
#else
        fn(ctx);
        return;
#endif
    }
    /* wwsjob JOB-CODE entry probe: the PM stages each job's code into a
     * buffer above its static image (0x3700..) and branches into it. No lift
     * exists at those pcs (the code arrives at runtime), so the branch lands
     * here. Log the entry + leading bytes -- the bytes identify WHICH job
     * blob was staged (match against lifted job images for dispatch). */
    /* Staged-job "return to kernel": a jm2-protocol job binary ends by
     * jumping to LS 0 (the jm2 kernel's home on real hardware). Inside the
     * wwsjob PM's LS, address 0 holds the interrupt VECTOR instead -- letting
     * the jump proceed runs the interrupt handler as the job's continuation
     * and wedges the pipeline (lanes freeze mid-queue). The PM called the job
     * with a return link in r0 (0x31C8 sets 0x3258); route the kernel-return
     * there: the PM resumes its post-job flow, which is exactly what the jm2
     * kernel hand-back does on hardware. */
    if (ctx->image_id == 2 && ctx->policy_mode && ctx->pc == 0 &&
        (ctx->gpr[0]._u32[0] & SPU_LS_MASK) >= 0xA00 &&
        (ctx->gpr[0]._u32[0] & SPU_LS_MASK) < 0x3700) {
        static int _n = 0;
        if (_n++ < 8)
            fprintf(stderr, "[wws-jobret] staged job returned to kernel (LS 0); "
                    "resuming PM at link 0x%05X\n", ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
        spu_indirect_branch(ctx);      /* re-dispatch at the rewritten pc */
        return;
    }

    /* A SPURS *job* that branches back to LS 0 has RETURNED TO THE JOB MANAGER.
     * That is how a jm2 job signals completion: its crt tail-jumps to the
     * resident manager, which on hardware loads the next job and sets up r3/r4
     * afresh. We load the job at LS 0 and have no manager, so the jump lands on
     * the job's OWN entry and it runs a second lap -- with dead registers.
     *
     * Tokyo Jungle showed exactly that: lap one is correct at every step and
     * does the real work, then the crt jumps to 0 and lap two re-reads its
     * parameters through a now-zero context pointer (hence the reads of LS 0x20
     * and the DMA addresses that were really its own opcodes) and spins forever.
     * The work was already finished before the jump; the second lap is noise.
     *
     * The initial entry is called directly, not through here, so any arrival at
     * LS 0 in this path is a re-entry. Treat it as job end. */
    if (!ctx->policy_mode && ctx->pc == 0 && ctx->image_id > 0) {
        static int _n = 0;
        if (_n++ < 8)
            fprintf(stderr, "[spurs-job] img=%d returned to the job manager "
                    "(branch to LS 0) -- job complete\n", ctx->image_id);
        spu_halt(ctx);
        return;
    }
    if (ctx->image_id == 2 &&
        ((ctx->pc >= 0x3700 && ctx->pc < 0x3FE80) ||
         (ctx->pc < 0xA00 && ctx->pc != SPURS_PM_EXIT_TO_KERNEL_LS &&
          ctx->pc != SPURS_PM_SELECT_WORKLOAD_LS))) {
        static int _n = 0;
        if (_n < 12) {
            _n++;
            const uint8_t* p = ctx->ls + (ctx->pc & SPU_LS_MASK);
            fprintf(stderr, "[wws-jobentry] pc=0x%05X lr=0x%05X bytes:"
                    " %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                    ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                    p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                    p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
            /* Companion state: the OTHER load target (the real 0x100-byte
             * loads land at 0x14400 -- possibly the actual job code, with the
             * entry computed from the wrong slot's base) + the job records
             * the entry math reads (0x1320 base block, 0x1440 batch record). */
            { const uint8_t* q = ctx->ls + 0x14400;
              const uint8_t* r1 = ctx->ls + 0x1320;
              const uint8_t* r2 = ctx->ls + 0x1440;
              fprintf(stderr, "[wws-jobentry]   LS14400: %02X%02X%02X%02X %02X%02X%02X%02X"
                      "  LS1320: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X"
                      "  LS1440: %02X%02X%02X%02X %02X%02X%02X%02X\n",
                      q[0],q[1],q[2],q[3], q[4],q[5],q[6],q[7],
                      r1[0],r1[1],r1[2],r1[3], r1[4],r1[5],r1[6],r1[7],
                      r1[8],r1[9],r1[10],r1[11], r1[12],r1[13],r1[14],r1[15],
                      r2[0],r2[1],r2[2],r2[3], r2[4],r2[5],r2[6],r2[7]); }
            fflush(stderr);
        }
    }
    /* No lifted function at this PC: it may be RUNTIME-GENERATED code (the
     * WWS jobmanager writes a save/restore stub above its static
     * image and calls it). Interpret the live LS bytes; on success the next
     * branch re-enters lifted code via the trampoline. */
    { static int s_sp = -1;
      if (s_sp < 0) s_sp = getenv("SPU_SMC_PROF") ? 1 : 0;
      if (s_sp) {   /* SPU_SMC_PROF (diagnostic): see spu_smc_prof_note */
          const uint32_t start = ctx->pc & SPU_LS_MASK & ~3u;
          const int img = ctx->image_id, ovl = ctx->resident_ovl;
          uint8_t code[32];
          for (uint32_t k = 0; k < 32; k++) code[k] = ctx->ls[(start + k) & SPU_LS_MASK];
          const unsigned long long c0 = __rdtsc();
          const int ok = spu_smc_microstep(ctx);
          spu_smc_prof_note(img, ovl, start, code, __rdtsc() - c0);
          if (ok) return;
      } else if (spu_smc_microstep(ctx))
          return; }
    /* Cap the unresolved-branch log PER IMAGE: a global cap let one noisy
     * image (the FMOD mixer's overlay calls) exhaust it and silently hide
     * every other image's misses -- LBP's loading jobs skipped their command
     * handlers for a whole session without a single log line. */
    { enum { BT0_MAX_IMG = 64, BT0_PER_IMG = 12 };
      static int _bt0[BT0_MAX_IMG];
      unsigned img = (ctx->image_id >= 0 && ctx->image_id < BT0_MAX_IMG)
                     ? (unsigned)ctx->image_id : 0;
      if (_bt0[img]++ < BT0_PER_IMG)
        fprintf(stderr, "[SPU] BRANCH-TO-0 unresolved pc=0x%05X image=%d lr=0x%05X\n",
                ctx->pc, ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK); }
    /* One-shot: the FMOD null-handler DSP node carries a PPU descriptor EA at
     * node+0x14 (observed 0x93C3C0). Dump it to identify which plugin/unit
     * type never got its SPU code streamed (env SPU_DSPDESC=<hex ea>). */
    { static int _d = -1; static uint32_t _ea = 0;
      if (_d < 0) { const char* e = getenv("SPU_DSPDESC");
        _ea = e ? (uint32_t)strtoul(e, 0, 16) : 0; _d = _ea ? 1 : 0; }
      if (_d == 1) { _d = 2;
        extern uint8_t* vm_base;
        fprintf(stderr, "[dspdesc] RAM[0x%08X]:", _ea);
        for (int k = 0; k < 0x60; k += 4)
            fprintf(stderr, " %02X%02X%02X%02X", vm_base[_ea+k], vm_base[_ea+k+1],
                    vm_base[_ea+k+2], vm_base[_ea+k+3]);
        fprintf(stderr, "\n"); fflush(stderr); } }
    /* SPU_MISS_DUMP_IMG=<n>: reserve the deep-dump budget for image n's misses
     * (the global 2-shot budget was always consumed by an earlier image's
     * misses, hiding the one under investigation). Unset = old behavior. */
    { static int s_img = -2; static uint32_t s_pcmin = 0;
      if (s_img == -2) { const char* e = getenv("SPU_MISS_DUMP_IMG"); s_img = e ? atoi(e) : -1;
        const char* p = getenv("SPU_MISS_DUMP_PCMIN"); s_pcmin = p ? (uint32_t)strtoul(p,0,0) : 0; }
      static int _n=0;
      uint32_t misspc = ctx->pc & SPU_LS_MASK;
      if ((s_img < 0 || ctx->image_id == s_img) && misspc >= s_pcmin && _n++ < 2) {
        fprintf(stderr, "[SPU] branch-to-0 lr=0x%05X r1=0x%05X\n",
                ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->gpr[1]._u32[0] & SPU_LS_MASK);
#ifdef _WIN32
        void* frames[24]; unsigned short fn = RtlCaptureStackBackTrace(0, 24, frames, NULL);
        char* base = (char*)GetModuleHandleA(NULL);
        fprintf(stderr, "[SPU] host bt RVAs:");
        for (unsigned short i = 0; i < fn; i++)
            fprintf(stderr, " 0x%zX", (size_t)((char*)frames[i] - base));
        fprintf(stderr, "\n");
#endif
        /* State-diff oracle: dump full LS + all GPRs at the branch-to-0 so it can
         * be compared byte-for-byte against the RPCS3 savestate LS of the same
         * (cri_mpv) task. Path from YDKJ_SPU_LSDUMP, else ./recomp_spu_ls.bin. */
        const char* dp = getenv("YDKJ_SPU_LSDUMP");
        if (!dp || !*dp) dp = "recomp_spu_ls.bin";
        FILE* lf = fopen(dp, "wb");
        if (lf) { fwrite(ctx->ls, 1, SPU_LS_SIZE, lf); fclose(lf);
                  fprintf(stderr, "[SPU] dumped 256KB LS -> %s\n", dp); }
        fprintf(stderr, "[SPU] image_id=%d  GPR dump (r0..r127, hi64:lo64 of each quadword, preferred slot = _u32[0]):\n", ctx->image_id);
        for (int g = 0; g < 128; g++) {
            fprintf(stderr, " r%-3d=%08X %08X %08X %08X", g,
                    ctx->gpr[g]._u32[0], ctx->gpr[g]._u32[1],
                    ctx->gpr[g]._u32[2], ctx->gpr[g]._u32[3]);
            if ((g & 1) == 1) fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        /* echo the dispatch chain values the way func_00026DE0 computes them */
        { uint32_t bec0 = ctx->ls[0xBEC0]<<24 | ctx->ls[0xBEC1]<<16 | ctx->ls[0xBEC2]<<8 | ctx->ls[0xBEC3];
          fprintf(stderr, "[SPU] LS[0xBEC0].w0=0x%08X  LS[0x2d4e0:16]=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
            bec0,
            ctx->ls[0x2d4e0],ctx->ls[0x2d4e1],ctx->ls[0x2d4e2],ctx->ls[0x2d4e3],
            ctx->ls[0x2d4e4],ctx->ls[0x2d4e5],ctx->ls[0x2d4e6],ctx->ls[0x2d4e7],
            ctx->ls[0x2d4e8],ctx->ls[0x2d4e9],ctx->ls[0x2d4ea],ctx->ls[0x2d4eb],
            ctx->ls[0x2d4ec],ctx->ls[0x2d4ed],ctx->ls[0x2d4ee],ctx->ls[0x2d4ef]); }
    } }
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

/* ===========================================================================
 * Execution trace (for §3 validation: diff vs RPCS3 SPU interpreter)
 *
 * When the lifter is invoked with --trace, every emitted instruction is
 * surrounded by spu_trace_pc(ctx, PC) before execution and spu_trace_rt(
 * ctx, RT) after, for instructions whose destination is the rt slot. The
 * output is one line per event:
 *
 *     <PC-5hex>                          - PC about to execute
 *       r<rt> <hi-64hex> <lo-64hex>      - register written, post-state
 *
 * Direct to stderr by default; call spu_trace_init(path) once at startup
 * to redirect to a file. The format is intentionally minimal and stable
 * so a small converter can line it up against an RPCS3.log SPU trace.
 * ===========================================================================*/
static FILE* s_trace_fp = NULL;

void spu_trace_init(const char* path)
{
    if (!path || !*path) { s_trace_fp = stderr; return; }
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) s_trace_fp = stderr;
}

/* Bounded by design: a --trace lift emits one call PER INSTRUCTION, so an image
 * that spins (a persistent SPURS policy module polling its job queue never
 * returns) would otherwise write stderr until the disk fills. The cap keeps the
 * useful part -- the entry path plus enough revolutions to show what the loop
 * tests -- and the tail IS the loop. SPU_TRACE_MAX=0 restores unbounded.
 * SPU_TRACE_FILE redirects off stderr so the trace does not interleave with the
 * boot log. */
/* Trace output is OPT-IN at runtime: a --trace lift bakes the calls into the
 * generated C forever, so a trace-lifted image shipped in a title build (LBP's
 * job kernel, kept from the WWS verification) would otherwise spew the
 * per-instruction log into every boot. Enable with SPU_TRACE=1 (stderr) or
 * SPU_TRACE_FILE=<path>. */
static long long s_trace_left = -1;     /* -1 uninit, -2 unbounded, 0 off */
/* Set when spu_trace_pc suppressed its line (gates below): the paired
 * spu_trace_rt call for the same instruction must also stay silent. */
static SPU_THREAD_LOCAL int s_trace_suppress;

void spu_trace_pc(spu_context* ctx, uint32_t pc)
{
    (void)ctx;
    /* SPU_TRACE_AT_WALL=1: hold the trace until the PPU's job-barrier probe
     * arms g_barrier_sync_watch, so the budget covers the post-ticket claim/
     * stage pass instead of boot-time idle polling. */
    { static int s_aw = -1;
      if (s_aw < 0) s_aw = getenv("SPU_TRACE_AT_WALL") ? 1 : 0;
      if (s_aw) { extern uint32_t g_barrier_sync_watch;
                  if (!g_barrier_sync_watch) { s_trace_suppress = 1; return; } } }
    /* SPU_TRACE_FLOWCTX=1: trace ONLY the run the SPURS_PM_FLOW tracer armed
     * (the stalled queue's spu0 dispatch) -- post-wall idle dispatches of the
     * other 13 jobmanager wids otherwise exhaust the budget in under a second. */
    { static int s_fc = -1;
      if (s_fc < 0) s_fc = getenv("SPU_TRACE_FLOWCTX") ? 1 : 0;
      if (s_fc) { extern void* volatile g_pm_flow_ctx;
                  if (g_pm_flow_ctx != (void*)ctx) { s_trace_suppress = 1; return; } } }
    /* SPU_TRACE_IMG=<n>: trace only contexts running image n (e.g. 1 = the
     * WWS job binary, to catch the crt corrupting the job's r3/r4 args). */
    { static int64_t s_ti = -2;
      if (s_ti == -2) { const char* e = getenv("SPU_TRACE_IMG");
                        s_ti = e ? atoll(e) : -1; }
      if (s_ti >= 0 && ctx->image_id != (int)s_ti) { s_trace_suppress = 1; return; } }
    s_trace_suppress = 0;
    if (s_trace_left < 0) {
        if (s_trace_left == -1) {
            const char* p = getenv("SPU_TRACE_FILE");
            if ((!p || !*p) && !getenv("SPU_TRACE")) { s_trace_left = 0; return; }
            if (p && *p && !s_trace_fp) spu_trace_init(p);
            const char* m = getenv("SPU_TRACE_MAX");
            s_trace_left = (m && *m) ? atoll(m) : 200000;
            if (s_trace_left == 0) s_trace_left = -2;   /* -2 = unbounded */
        }
    }
    if (!s_trace_fp) s_trace_fp = stderr;
    if (s_trace_left == 0) return;
    if (s_trace_left > 0 && --s_trace_left == 0) {
        fprintf(s_trace_fp, "-- SPU_TRACE_MAX reached; trace stopped --\n");
        fflush(s_trace_fp);
        return;
    }
    fprintf(s_trace_fp, "%05X\n", pc & SPU_LS_MASK);
}

void spu_trace_rt(spu_context* ctx, uint32_t rt)
{
    if (s_trace_suppress) return;          /* paired _pc line was gated out */
    if (s_trace_left == 0) return;         /* tracing off/stopped (see _pc) */
    if (!s_trace_fp) s_trace_fp = stderr;
    u128 v = ctx->gpr[rt & 0x7F];
    fprintf(s_trace_fp, "  r%-3u %016llX %016llX\n",
            (unsigned)(rt & 0x7F),
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#ifdef __cplusplus
}
#endif