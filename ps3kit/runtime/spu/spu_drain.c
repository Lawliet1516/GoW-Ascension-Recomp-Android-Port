/* SPU_DRAIN trampoline model -- runtime state + stub hooks (faithful-adopt).
 *
 * Milestone 1 lands the execution-model plumbing; the per-transfer hooks
 * (lockstep gate, flight recorder, SPURS task-launch) are inert stubs here and
 * gain real bodies in their own milestones. Keeping them as real out-of-line
 * functions (not macros) means the SPU_DRAIN/SPU_RET call sites are already in
 * place -- later milestones only replace the bodies. */
#include "spu_context.h"
#include "spu_iojrn.h"
#include <setjmp.h>

/* Halt landing pads that the CRT's longjmp leaves without unwinding the host
 * frames in between (spu_channels.c, SPU_HALT_UNWIND). */
extern void spu_jmpbuf_noframe(jmp_buf env);

/* See spu_context.h `irq_frame`. */
typedef struct spu_irq_frame {
    jmp_buf               env;
    struct spu_irq_frame* prev;
    uint32_t              depth;
    int                   image_id;
} spu_irq_frame;

/* Pending cross-function transfer target for this host thread's SPU context. */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;
SPU_THREAD_LOCAL uint32_t g_spu_pch[8];
SPU_THREAD_LOCAL unsigned g_spu_pch_n;

/* SPU_DIRECT=1 (default off): direct transfers (spu_context.h, SPU_XFER).
 * The innermost drain loop's {host_depth, return pc}; depth ~0 = none may go
 * direct. Set by spu_drain_loop, cleared by spu_run_with_halt for the
 * top-level drain and by spu_fn_dispatch around natives and hooks. */
SPU_THREAD_LOCAL spu_direct_state t_spu_direct = { ~0u, 0u };
int g_spu_direct = -1;
int spu_direct_init(void)
{
    const char* e = getenv("SPU_DIRECT");
    g_spu_direct = (e && e[0] == '1') ? 1 : 0;
    return g_spu_direct;
}

/* yz_lockstep_tick now has its real body in spu_lockstep.c (milestone 2). */

/* SPURS task-launch interception at a trampoline hop (milestone: SPURS kernel).
 * No-op until then. */

/* PM flow trace (SPURS_PM_FLOW=1): record every cross-function transfer of ONE
 * policy-module run (the ctx spurs_policy.c arms) so the post-claim decision
 * path can be reconstructed offline. Written by the drain-loop hook below;
 * armed/dumped by spu_run_policy_module. */
uint32_t          g_pm_flow_buf[8192];
volatile unsigned g_pm_flow_n = 0;
void* volatile    g_pm_flow_ctx = 0;

#include <stdlib.h>
#include <stdio.h>

/* Sync-model ring-drain: the wwsjob PM's job ring drains only when jobs mark
 * their record's "done" bit (0x8000 in the record's first halfword @LS 0xDF0).
 * On hardware the async load-completion INTERRUPT sets it; our loads complete
 * synchronously, so a job whose code never got a ring slot stays "pending"
 * forever and the ring deadlocks (scan re-bails at LS 0x2318). When we observe
 * that bail spin, the in-flight loads HAVE in fact completed in our model, so
 * set the done bit on the pending records -- the faithful sync equivalent of
 * the completion interrupt. Env SPU_JOBDRAIN (default off while validating). */
extern void spu_halt(spu_context*);
/* Last 32 drain steps on this host thread (pc), for post-mortems of a run that
 * ended where it should not have (spurs_policy.c, [pm-end]). */
static _Thread_local uint32_t t_recent[32];
static _Thread_local uint32_t t_recent_n;
unsigned spu_recent_pcs(uint32_t* out, unsigned max)
{
    unsigned n = t_recent_n < 32 ? t_recent_n : 32, k = n < max ? n : max;
    for (unsigned i = 0; i < k; i++) out[i] = t_recent[(t_recent_n - k + i) & 31];
    return k;
}

/* SPU_JOB_HIST=3: the same gaps keyed by the lifted function each step ran
 * (and whatever it called before the next step), printed every 5 s as the
 * top 40 with their exe RVA: python tools/maprva.py <map> <log> names them. */
static void spu_fn_hist_step(void* fn)
{
    extern unsigned long long ps3_qpc_us(void);
    enum { FH = 8192 };
    static void* volatile s_fn[FH];
    static volatile long long s_us[FH];
    static volatile unsigned long long s_t0;
    static _Thread_local unsigned long long t_last;
    static _Thread_local int t_slot = -1;
    const unsigned long long now = ps3_qpc_us();
    if (t_slot >= 0 && now - t_last < 2000)
        __atomic_add_fetch(&s_us[t_slot], (long long)(now - t_last), __ATOMIC_RELAXED);
    t_last = now;
    t_slot = -1;
    for (unsigned i = (unsigned)(((uintptr_t)fn >> 4) * 2654435761u) % FH, n = 0; n < 64; n++, i = (i + 1) % FH) {
        void* cur = __atomic_load_n(&s_fn[i], __ATOMIC_RELAXED);
        if (cur == fn) { t_slot = (int)i; break; }
        if (!cur) {
            void* expect = 0;
            if (__atomic_compare_exchange_n(&s_fn[i], &expect, fn, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) || expect == fn) {
                t_slot = (int)i; break;
            }
        }
    }
    unsigned long long t0 = __atomic_load_n(&s_t0, __ATOMIC_RELAXED);
    if (!t0) { __atomic_store_n(&s_t0, now, __ATOMIC_RELAXED); return; }
    if (now - t0 < 5000000ull || !__atomic_compare_exchange_n(&s_t0, &t0, now, 0,
                                                              __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
#ifdef _WIN32
    extern char __ImageBase;
    const uintptr_t base = (uintptr_t)&__ImageBase;
#else
    const uintptr_t base = 0;
#endif
    long long tot = 0;
    static long long snap[FH];
    for (int k = 0; k < FH; k++) { snap[k] = __atomic_exchange_n(&s_us[k], 0, __ATOMIC_RELAXED); tot += snap[k]; }
    fprintf(stderr, "[fn-hist] %.1f ms/s of SPU steps, top functions:\n", tot / 5000.0);
    for (int r = 0; r < 40; r++) {
        int best = -1;
        for (int k = 0; k < FH; k++) if (snap[k] > 0 && (best < 0 || snap[k] > snap[best])) best = k;
        if (best < 0) break;
        const uintptr_t f = (uintptr_t)s_fn[best];
        if (f & 1)
            fprintf(stderr, "[fn-hist] %7.1f ms/s  indirect to pc=0x%05X ovl=%u\n", snap[best] / 5000.0,
                    (unsigned)((f >> 1) & SPU_LS_MASK), (unsigned)((uint64_t)f >> 32));
        else
            fprintf(stderr, "[fn-hist] %7.1f ms/s  rva=0x%llX\n", snap[best] / 5000.0,
                    (unsigned long long)(f - base));
        snap[best] = 0;
    }
}

/* SPU_JOB_HIST=1: SPU time per resident job module (or per image outside
 * one), from the gaps between trampoline steps on each host thread; gaps over
 * 2 ms are the thread parked and are dropped. Every 5 s, ms per second each. */
static void spu_job_hist_step(const spu_context* ctx, void* fn)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("SPU_JOB_HIST"); s_on = e ? (*e == '2' ? 2 : *e == '3' ? 3 : 1) : 0; }
    if (!s_on) return;
    if (s_on == 3) {
        /* an indirect branch is keyed by where it goes (odd keys: pc, overlay) */
        if (fn == (void*)spu_indirect_branch)
            fn = (void*)(uintptr_t)(1u | ((uint64_t)(ctx->pc & SPU_LS_MASK) << 1) |
                                    ((uint64_t)(uint32_t)ctx->resident_ovl << 32));
        spu_fn_hist_step(fn);
        return;
    }
    extern unsigned long long ps3_qpc_us(void);
    enum { JH = 1200 };
    static volatile long long s_us[JH];
    static volatile unsigned long long s_t0;
    static _Thread_local unsigned long long t_last;
    static _Thread_local int t_key = -1;
    const unsigned long long now = ps3_qpc_us();
    if (t_key >= 0 && now - t_last < 2000)
        __atomic_add_fetch(&s_us[t_key], (long long)(now - t_last), __ATOMIC_RELAXED);
    t_last = now;
    /* SPU_JOB_HIST=2: by the image the step runs in (a code fragment's own
     * image, 300+), not by the resident job module that called it */
    t_key = s_on == 1 && ctx->resident_ovl > 0 && ctx->resident_ovl < 600 ? ctx->resident_ovl
                                                                         : 600 + (ctx->image_id & 0x1FF);
    unsigned long long t0 = __atomic_load_n(&s_t0, __ATOMIC_RELAXED);
    if (!t0) { __atomic_store_n(&s_t0, now, __ATOMIC_RELAXED); return; }
    if (now - t0 < 5000000ull || !__atomic_compare_exchange_n(&s_t0, &t0, now, 0,
                                                              __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    char line[1024]; int o = snprintf(line, sizeof line, "[job-hist] ms/s:");
    for (int k = 0; k < JH && o < (int)sizeof line - 32; k++) {
        const long long us = __atomic_exchange_n(&s_us[k], 0, __ATOMIC_RELAXED);
        if (us >= 5000)   /* >= 1 ms/s */
            o += snprintf(line + o, sizeof line - (size_t)o, k < 600 ? " ovl%d=%.1f" : " img%d=%.1f",
                          k < 600 ? k : k - 600, us / 5000.0);
    }
    fprintf(stderr, "%s\n", line);
}

/* SPU_EXITTRACE=<img>: remember the last PCs this image executed and dump
 * them when the job ends. "Why did it exit here" is a question about the
 * path taken, and a forward trace from the entry never reaches far enough
 * to show it. */
static int64_t s_et = -2;
static uint32_t s_et_ring[64]; static uint32_t s_et_ri;

/* A SPURS job that branched to LS 0 has finished (see spu_task_launch_check):
 * report it and halt. Shared by the full step check and the inline one. */
static void spu_job_end_at_ls0(spu_context* ctx)
{
    static int _n = 0;
    if (_n++ < 8)
        fprintf(stderr, "[spurs-job] img=%d branched to LS 0 -- job complete\n",
                ctx->image_id);
    if (s_et >= 0 && ctx->image_id == s_et) {
        fprintf(stderr, "[spu-exit] img=%d last PCs:", ctx->image_id);
        for (int i = 32; i >= 1; i--)
            fprintf(stderr, " %05X", s_et_ring[(s_et_ri - i) & 63]);
        fputc(10, stderr); fflush(stderr);
    }
    spu_halt(ctx);
}

extern void spu_check_stack_reset(spu_context*, void (*)(spu_context*));
extern unsigned g_spu_stack_reset_count;   /* none registered: nothing to check */

void spu_task_launch_check(spu_context* ctx, void* fn)
{
    /* Runs on every trampoline step (millions a second), so the diagnostics
     * share one gate read once from the environment; with none of them set a
     * step costs the job-end and stack-reset checks below and nothing else. */
    static int s_diag = -1;
    if (s_diag < 0)
        s_diag = (getenv("SPU_JOB_HIST") || getenv("SPU_SHADOW") || getenv("SPU_EXITTRACE") ||
                  getenv("SPU_STEPTRACE") || getenv("SPU_JOBDRAIN")) ? 1 : 0;
    if (s_diag) {
        spu_job_hist_step(ctx, fn);
        extern void spu_shadow_check(spu_context*, void (*)(spu_context*));
        spu_shadow_check(ctx, (void (*)(spu_context*))fn);
    }
    t_recent[t_recent_n++ & 31] = (uint32_t)ctx->pc & SPU_LS_MASK;
    if (g_spu_stack_reset_count) spu_check_stack_reset(ctx, (void (*)(spu_context*))fn);
    /* A SPURS job returning to LS 0 is finished -- its crt tail-jumps to the
     * resident job manager, and with the job loaded at 0 that lands on its own
     * entry. Planting a return address in r0 catches the jobs that get there
     * via `bi $r0`, but some branch to 0 DIRECTLY, which the lifter turns into
     * a plain trampoline that never reaches spu_indirect_branch. The drain sees
     * every step, so catch it here too. Step 0 is the real entry. */
    static int s_no_ls0 = -1;
    if (s_no_ls0 < 0) s_no_ls0 = getenv("SPU_NO_LS0_END") ? 1 : 0;
    if (s_et == -2) { const char* e = getenv("SPU_EXITTRACE");
                      s_et = e ? strtol(e, 0, 0) : -1; }
    if (s_diag && s_et >= 0 && ctx->image_id == s_et)
        s_et_ring[s_et_ri++ & 63] = (uint32_t)ctx->pc & SPU_LS_MASK;

    if (!s_no_ls0 && ctx->steps++ && (ctx->pc & SPU_LS_MASK) == 0 &&
        !ctx->policy_mode && ctx->image_id > 0) {
        spu_job_end_at_ls0(ctx);
        return;
    }
    /* SPU_STEPTRACE=<img>: pc and the argument registers at every trampoline
     * step. This runs on each drain iteration, so it is the finest-grained
     * view of a register file changing under a running job. */
    if (g_pm_flow_ctx == (void*)ctx && g_pm_flow_n < 8192)
        g_pm_flow_buf[g_pm_flow_n++] = ctx->pc;
    if (!s_diag) return;
    { static int64_t s_t = -2;
      if (s_t == -2) { const char* e = getenv("SPU_STEPTRACE"); s_t = e ? strtol(e,0,0) : -1; }
      static int64_t s_from = -2;
      if (s_from == -2) { const char* e = getenv("SPU_STEPTRACE_FROM");
                          s_from = e ? strtol(e,0,16) : -1; }
      static int s_armed = 0;
      if (s_t >= 0 && ctx->image_id == s_t) {
          if (s_from >= 0 && !s_armed &&
              ((uint32_t)ctx->pc & SPU_LS_MASK) == (uint32_t)s_from) s_armed = 1;
          static int n = 0;
          if ((s_from < 0 || s_armed) && n++ < 40) {
              fprintf(stderr, "[step] pc=0x%05X r1=0x%05X r2=0x%08X r3=0x%08X r4=0x%08X\n",
                      (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[1]._u32[0],
                      ctx->gpr[2]._u32[0], ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0]);
              fflush(stderr);
          }
      } }

    static int s_on = -1;
    if (s_on < 0) s_on = getenv("SPU_JOBDRAIN") ? 1 : 0;
    if (!s_on || ctx->image_id != 2 || !ctx->policy_mode) return;

    /* 0x2318 = the type-2 load's ring-full bail (resets scan, retries). */
    if ((ctx->pc & 0x3FFFF) != 0x2318) return;

    static unsigned s_bail = 0;
    if (++s_bail < 4096) return;      /* only after a real spin, not one pass */
    s_bail = 0;

    /* Set bit 31 (the 0x8000 first-halfword done flag) on each pending record
     * the release sweep scans (16 slots, 4-byte stride at LS 0xDF0), for any
     * record that is non-empty and not already done. */
    int marked = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t* r = &ctx->ls[0xDF0 + i * 4];
        uint32_t w = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
                     ((uint32_t)r[2] << 8) | r[3];
        if (w == 0 || (w & 0x80000000u)) continue;
        w |= 0x80000000u;
        r[0] = (uint8_t)(w >> 24); r[1] = (uint8_t)(w >> 16);
        r[2] = (uint8_t)(w >> 8);  r[3] = (uint8_t)w;
        marked++;
    }
    static int _n = 0;
    if (marked && _n++ < 12)
        fprintf(stderr, "[pm-jobdrain] ring-full spin: marked %d pending records done\n", marked);
}

/* spu_task_launch_check inlined into the drain loops below. With none of its
 * diagnostics set (SPU_JOB_HIST, SPU_SHADOW, SPU_EXITTRACE, SPU_STEPTRACE,
 * SPU_JOBDRAIN, SPU_NO_LS0_END) a step only notes its pc, checks for a job's
 * branch to LS 0 and feeds the PM flow trace, and the call around that, with
 * its gate reads, was ~2% of an SPU thread in combat. Same checks, same
 * order. SPU_STEP_INLINE=0: call the full function on every step. */
static int s_step_inline = -1;
static int spu_step_inline_init(void)
{
    const char* e = getenv("SPU_STEP_INLINE");
    if (e && e[0] == '0') return 0;
    return !(getenv("SPU_JOB_HIST") || getenv("SPU_SHADOW") || getenv("SPU_EXITTRACE") ||
             getenv("SPU_STEPTRACE") || getenv("SPU_JOBDRAIN") || getenv("SPU_NO_LS0_END"));
}
int spu_step_inline(void)
{
    if (s_step_inline < 0) s_step_inline = spu_step_inline_init();
    return s_step_inline;
}
/* always_inline: as a plain `static inline` the PGO build kept it out of line
 * (its own symbol, ~2% of an SPU thread in the geometry jobs). */
static inline __attribute__((always_inline)) void spu_step_check(spu_context* ctx, void (*fn)(spu_context*))
{
    if (s_step_inline <= 0 && !spu_step_inline()) { spu_task_launch_check(ctx, (void*)fn); return; }
    t_recent[t_recent_n++ & 31] = (uint32_t)ctx->pc & SPU_LS_MASK;
    if (g_spu_stack_reset_count) spu_check_stack_reset(ctx, fn);
    if (ctx->steps++ && (ctx->pc & SPU_LS_MASK) == 0 &&
        !ctx->policy_mode && ctx->image_id > 0) {
        spu_job_end_at_ls0(ctx);
        return;
    }
    if (g_pm_flow_ctx == (void*)ctx && g_pm_flow_n < 8192)
        g_pm_flow_buf[g_pm_flow_n++] = ctx->pc;
}

/* Restore the active image after a lifted call bracket. The brsl/bisl emission
 * saves image_id in a call-site local and hands it back here so an image
 * adopted inside the callee cannot leak into the caller's continuation. A
 * persistent LS-0xA00 workload module (module_img_a00) is re-applied since the
 * plain restore would undo it. */
void spu_img_restore(spu_context* ctx, int32_t saved_img)
{
    ctx->image_id = ctx->module_img_a00 ? ctx->module_img_a00 : saved_img;
}

/* spu_ch_wake now has its real body in spu_channels.c (milestone 3). */

/* --- SPU interrupt dispatch (drain-loop hook) ------------------------------
 * Called when int_enable && (event_status & event_mask): architectural SPU
 * interrupt. srr0 <- the interrupted continuation (ctx->pc already holds the
 * pending transfer target), interrupts disable, and control vectors through
 * the guest-planted branch instruction at LS 0 (the WWS jobmanager's entry
 * writes `bra 0xA2C` there; its handler drives the DMA-list job pipeline and
 * returns via iret). If LS 0 holds no br/bra, the interrupt is NOT taken --
 * we log once and resume normal flow rather than jump into zeros. */
#include <stdio.h>
#include <stdlib.h>

/* ---- Deferred DMA-list stall-and-notify delivery --------------------------
 * Real MFC hardware processes a queued list command (the WWS job manager's
 * barriered null list, MFC_GETLB) ASYNCHRONOUSLY: the stall-and-notify -- and
 * the interrupt it raises -- land after the SPU has run on past the code that
 * armed it. We execute the list synchronously inside `wrch MFC_Cmd`, so the
 * interrupt was delivered before the job manager had stored
 * g_WwsJob_loadJobState = kReadCommands (LS 0x12A0).
 *
 * Its interrupt handler gates the Load->Run advance on BOTH the ch25 stall mask
 * carrying tag 0 (kLoadJob_readCommands) AND loadJobState == kReadCommands. With
 * the state still kNone it correctly declined to advance -- and because
 * MFC_RdListStallStat is "accumulative, clear on read" that single stall was
 * consumed, so no later interrupt could ever advance it: the job never ran and
 * LBP's loading thread deadlocked on a completion that never came.
 *
 * Holding the interrupt off for N drain ticks was tried and DISPROVEN: a probe
 * at the stall site shows loadJobState is ALREADY 0 when the GETLB is issued, so
 * the arming store never ran on this path at all -- it is not an interrupt-timing
 * race. (The pm takes trychangefreetoloadjob's early-exit `ceqhi jobHeader,1 /
 * brhz cond1,Exit` to LS 0x2D30, skipping the Load setup.) Default OFF; kept
 * env-gated for experiments: SPU_SN_DEFER=<n> ticks (0 = immediate, faithful
 * to the current synchronous list execution). */
void* volatile   g_sn_defer_ctx = 0;
volatile unsigned g_sn_defer     = 0;

unsigned spu_sn_defer_ticks(void)
{
    static int s_n = -1;
    if (s_n < 0) { const char* e = getenv("SPU_SN_DEFER"); s_n = e ? atoi(e) : 0; }
    return (unsigned)(s_n < 0 ? 0 : s_n);
}

/* ---- Interrupt register preservation --------------------------------------
 * Hardware contract: an SPU interrupt handler preserves every register (the
 * WWS jobmanager guarantees it with a save/restore shim it installs at the
 * TOP of LS, ~0x3FEC0 -- interrupthandlerasm.spu.s). That shim lies OUTSIDE
 * the PM image our lifter processed, so the lifted handler runs WITHOUT the
 * save/restore and eats the interrupted code's live registers. Measured
 * mid-GetBufferTag: the fsmbi mask registers (r13/r15) arrived zeroed at the
 * packing selbs, the BufferTag's lanes collapsed, the job saw a NULL buffer
 * and bailed -- LBP's loading freeze.
 *
 * Enforce the contract host-side: snapshot the full GPR file when the
 * interrupt is taken, restore it when the handler irets (detected in
 * spu_indirect_branch: pc == saved srr0 with interrupts re-enabled -- during
 * the handler int_enable stays 0, so the first enabled dispatch at srr0 IS
 * the irete). State the handler legitimately publishes lives in LS/channels
 * and is untouched. Per-ctx slots; no nesting (interrupts stay disabled
 * until iret). */
/* The Wws job manager's interrupt handler (pm 0xA2C..0xAAC) has two ways out:
 * irete to srr0, or -- when its body put a target at LS 0x3FEA0 -- a branch
 * there with interrupts still off: it switched away and will not return to
 * the interrupted code. The registers saved at the take and the frame that
 * took it then stay armed: the handler's host frames pile up under the
 * redirected code (God of War: Ascension: depth 2, 3, 9, 26, then a host
 * stack overflow), and the next time execution passes the old srr0 with
 * interrupts on, the save is restored over live registers (module 216's
 * command interpreter then reads a bad command or a zero buffer and stops).
 * A dispatch with interrupts on at another pc while the save is armed is that
 * exit. [irq] lines (always): "handler left without iret" when it is seen,
 * "STALE restore" when such a save is put back later.
 * SPU_IRQ_REDIRECT_FIX=1: at that exit drop the save and the frame (unwinding
 * to the taking frame, as an iret does). Changes nothing when no handler
 * leaves that way (fnrec replays take no interrupts). */
static SPU_THREAD_LOCAL const spu_context* t_irq_left;   /* its handler left without iret */
static volatile long long g_irq_left_n, g_irq_stale_n;
static int spu_irq_redirect_fix(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPU_IRQ_REDIRECT_FIX"); on = e && e[0] == '1'; }
    return on;
}

static void spu_irq_regs_save(spu_context* ctx)
{
    if (t_irq_left == ctx) t_irq_left = NULL;   /* a new interrupt: its own save */
    ctx->irq_saved = 1;
    ctx->irq_resume_pc = ctx->srr0;
    memcpy(ctx->irq_gpr, ctx->gpr, sizeof ctx->irq_gpr);
}

/* Called from spu_indirect_branch on every dispatch. Restores + clears when
 * the iret lands. Returns 1 if a restore happened (diagnostic). */
/* Drop any saved register file belonging to this context. The save slots are
 * keyed by RAW POINTER, and an spu_context is typically a stack local -- so a
 * later, unrelated run lands on the same address and inherits the earlier
 * context's registers wholesale. A fresh run must never do that. */
void spu_irq_regs_forget(spu_context* ctx)
{
    if (t_irq_left == ctx) t_irq_left = NULL;
    ctx->irq_saved = 0;
}

int spu_irq_regs_maybe_restore(spu_context* ctx)
{
    {
        if (ctx->irq_saved) {
            if ((ctx->pc & SPU_LS_MASK) == (ctx->irq_resume_pc & SPU_LS_MASK) &&
                ctx->int_enable) {
                if (t_irq_left == ctx) {   /* its handler had left without iret */
                    t_irq_left = NULL;
                    const long long n = __atomic_add_fetch(&g_irq_stale_n, 1, __ATOMIC_RELAXED);
                    if (n <= 16 || !(n & (n - 1)))
                        fprintf(stderr, "[irq] STALE restore #%lld: the registers saved at an interrupt whose handler "
                                        "left without iret put back at srr0=0x%05X img=%d ovl=%d depth=%u\n",
                                n, ctx->pc & SPU_LS_MASK, ctx->image_id, (int)ctx->resident_ovl, (unsigned)ctx->host_depth);
                }
                memcpy(ctx->gpr, ctx->irq_gpr, sizeof ctx->irq_gpr);
                ctx->irq_saved = 0;
                /* The iret completed below the frame that took the interrupt:
                 * abandon the handler's host frames and resume there. */
                spu_irq_frame* f = (spu_irq_frame*)ctx->irq_frame;
                if (f && ctx->host_depth <= f->depth)
                    ctx->irq_frame = f->prev;      /* returned to its loop the ordinary way */
                if (f && ctx->host_depth > f->depth) {
                    { static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
                      static int _n = 0;
                      if (s_it && _n++ < 200)
                          fprintf(stderr, "[irq] IRET at depth %u unwinds to taking frame depth %u (srr0=0x%05X)\n",
                                  ctx->host_depth, f->depth, ctx->pc & SPU_LS_MASK); }
                    longjmp(f->env, 1);
                }
                if (!f && ctx->host_depth > 0) {
                    /* Taken by the top-level driver loop (depth 0), which
                     * keeps no frame: the driver's own restart re-enters at
                     * srr0 with the host stack empty, which is that frame. */
                    { static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
                      static int _n = 0;
                      if (s_it && _n++ < 200)
                          fprintf(stderr, "[irq] IRET at depth %u unwinds to the driver (srr0=0x%05X)\n",
                                  ctx->host_depth, ctx->pc & SPU_LS_MASK); }
                    extern void spu_restart_dispatch(spu_context*);
                    spu_restart_dispatch(ctx);
                }
                return 1;
            }
            if (ctx->int_enable && t_irq_left != ctx) {
                /* Interrupts on again, not at srr0: the handler left without iret. */
                spu_irq_frame* f = (spu_irq_frame*)ctx->irq_frame;
                const int fix = spu_irq_redirect_fix();
                const long long n = __atomic_add_fetch(&g_irq_left_n, 1, __ATOMIC_RELAXED);
                if (n <= 16 || !(n & (n - 1)))
                    fprintf(stderr, "[irq] handler left without iret #%lld: img=%d ovl=%d srr0=0x%05X now pc=0x%05X "
                                    "depth=%u taking frame depth=%d fix=%s\n",
                            n, ctx->image_id, (int)ctx->resident_ovl, ctx->irq_resume_pc & SPU_LS_MASK,
                            ctx->pc & SPU_LS_MASK, (unsigned)ctx->host_depth, f ? (int)f->depth : -1, fix ? "on" : "off");
                if (!fix) { t_irq_left = ctx; return 0; }
                ctx->irq_saved = 0;                        /* SPU_IRQ_REDIRECT_FIX */
                if (f && ctx->host_depth > f->depth)
                    longjmp(f->env, 1);                    /* the drain pops it, goes on at pc */
                if (f) ctx->irq_frame = f->prev;
                else if (ctx->host_depth > 0) {
                    extern void spu_restart_dispatch(spu_context*);
                    spu_restart_dispatch(ctx);
                }
            }
            return 0;
        }
    }
    return 0;
}

typedef void (*(*spu_irq_vector_fn)(spu_context*))(spu_context*);
spu_irq_vector_fn volatile g_spu_irq_vector_hook;   /* see the end of spu_take_interrupt */
void (*spu_take_interrupt(spu_context* ctx,
                          void (*tf)(spu_context*)))(spu_context*)
{
    /* SPU_IRQ_DELIVER=0: never take an SPU interrupt. God of War: Ascension's
     * Wws job manager polls its DMA-list stalls when it is not interrupted --
     * a run whose job code is interpreted takes none at all and plays -- and
     * taking them inside lifted job code (module 214's 0x4414-0x447C window)
     * stops the game at the end of loading. */
    { static int s_deliver = -1;
      if (s_deliver < 0) { const char* e = getenv("SPU_IRQ_DELIVER"); s_deliver = !(e && *e == '0'); }
      if (!s_deliver) return tf; }
    /* SPU_NOIRQ=<ovl>:<lo>-<hi>[,...] (hex LS): defer an interrupt while that
     * resident overlay's lifted code runs in [lo, hi] -- the way the
     * interpreter, which is never interrupted, runs it. It is taken at the
     * first transfer outside. For job code the interpreter can only leave by
     * SPU_REJOIN (Ascension's level-load job, module 204, during the loading
     * movies): lifted and interrupted there, the load never finished. */
    { static int s_n = -1; static struct { int ovl; uint32_t lo, hi; } s_r[8];
      if (s_n < 0) {
          s_n = 0;
          const char* e = getenv("SPU_NOIRQ");
          while (e && *e && s_n < 8) {
              char* end; s_r[s_n].ovl = (int)strtol(e, &end, 10);
              if (*end != ':') break;
              s_r[s_n].lo = (uint32_t)strtoul(end + 1, &end, 16);
              s_r[s_n].hi = *end == '-' ? (uint32_t)strtoul(end + 1, &end, 16) : s_r[s_n].lo;
              s_n++;
              e = *end == ',' ? end + 1 : NULL;
          }
      }
      const uint32_t pc = ctx->pc & SPU_LS_MASK;
      for (int k = 0; k < s_n; k++)
          if (ctx->resident_ovl == s_r[k].ovl && pc >= s_r[k].lo && pc <= s_r[k].hi) return tf; }
    /* Stall-and-notify still settling: let the SPU run on (see above). */
    if (g_sn_defer && g_sn_defer_ctx == (void*)ctx) {
        if (--g_sn_defer == 0) g_sn_defer_ctx = 0;
        return tf;
    }
    const uint8_t* v = ctx->ls;      /* vector word at LS 0, big-endian */
    uint32_t w = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
                 ((uint32_t)v[2] << 8) | v[3];
    uint32_t op9 = w >> 23;          /* top 9 opcode bits */
    uint32_t target;
    if (op9 == 0x060) {              /* bra i16 (absolute) */
        target = ((w >> 7) & 0xFFFF) << 2;
    } else if (op9 == 0x064) {       /* br i16 (relative to LS 0) */
        target = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK;
    } else {
        static int _w = 0;
        if (_w++ < 4)
            fprintf(stderr, "[spu-int] pending (st=0x%X mask=0x%X) but LS0 word "
                    "0x%08X is no branch -- not taken\n",
                    ctx->event_status, ctx->event_mask, w);
        return tf;
    }
    ctx->srr0 = ctx->pc;             /* resume point for iret */
    ctx->int_enable = 0;
    /* A recorded or replayed SPU function run (spu_iojrn.h) notes it. */
    { const spu_iojrn_ops* j = t_spu_iojrn;
      if (__builtin_expect(j != 0, 0)) j->irq(ctx, ctx->srr0 & SPU_LS_MASK, target & SPU_LS_MASK); }
    { extern void spu_evlog(const spu_context*, const char*, uint32_t, uint32_t);
      spu_evlog(ctx, "INT-take srr0/target", ctx->srr0 & SPU_LS_MASK, target & SPU_LS_MASK); }
    spu_irq_regs_save(ctx);          /* hardware contract: handler preserves regs */
    { static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
      if (s_it) { static int _n = 0; if (_n++ < 200)
        fprintf(stderr, "[irq] TAKE srr0=0x%05X depth=%d r13=%08X r15=%08X\n",
                ctx->srr0 & SPU_LS_MASK, ctx->host_depth,
                ctx->gpr[13]._u32[0], ctx->gpr[15]._u32[0]); } }
    ctx->pc = target & SPU_LS_MASK;
    { static int _n = 0;
      if (_n++ < 16) {
          /* WWS Load->Run diagnosis: the job-manager interrupt handler advances
           * g_WwsJob_loadJobState kReadCommands(1)->kExecuteCommands(2) only if
           * (a) the ch25 stall mask has tag 0 (kLoadJob_readCommands) AND
           * (b) loadJobState's HALFWORDS compare equal to 1 (ceqhi).
           * Log both at interrupt entry so we can see which test fails. */
          const uint8_t* js = ctx->ls + 0x12A0;   /* g_WwsJob_loadJobState quadword */
          fprintf(stderr, "[spu-int] TAKEN img=%d events=0x%X&0x%X vector->0x%05X "
                  "(srr0=0x%05X) stallstat=0x%X parked=0x%X loadJobState@12A0=%02X%02X%02X%02X "
                  "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n", ctx->image_id,
                  ctx->event_status, ctx->event_mask, ctx->pc, ctx->srr0,
                  ctx->list_stall_stat, ctx->list_stall_mask,
                  js[0],js[1],js[2],js[3], js[4],js[5],js[6],js[7],
                  js[8],js[9],js[10],js[11], js[12],js[13],js[14],js[15]); } }
    /* A title's native interrupt vector (NULL: none): asked once the interrupt
     * is taken, with pc at the vector target; a non-NULL answer runs in place
     * of the dispatch to the lifted vector code (the same frame, the same
     * drain loop). God of War: Ascension: the Wws job manager's handler under
     * EDGE jobs (host/spu/wip/pm, EDGE_PM=1). */
    { spu_irq_vector_fn h = g_spu_irq_vector_hook;
      if (h) { void (*nf)(spu_context*) = h(ctx); if (nf) return nf; } }
    return spu_indirect_branch;
}

/* The host_depth at which spu_depth_guard halts (SPU_HOST_DEPTH_MAX). */
/* Exported (SPU_CALL_END in spu_context.h reads it). */
#define s_depth_max g_spu_depth_max
uint32_t s_depth_max = 0;
static __attribute__((noinline)) uint32_t spu_depth_max_init(void)
{
    const char* e = getenv("SPU_HOST_DEPTH_MAX");
    s_depth_max = e ? (uint32_t)strtoul(e, 0, 0) : 2000u;
    return s_depth_max;
}
static inline __attribute__((always_inline)) uint32_t spu_depth_max(void)
{
    return s_depth_max ? s_depth_max : spu_depth_max_init();
}

/* SPU_STACK_RESCUE (default on; =0: off). A host stack overflow on an SPU
 * thread ends the process (the [STACKOVERFLOW] reporter exits; without the
 * stack guarantee it closes with no message at all), and lifted-call nesting
 * can reach that long before SPU_HOST_DEPTH_MAX: interpreter frames are
 * large, and a job manager that keeps re-entering its loop from inside its
 * jobs never returns. When a drain starts with less than a third of the
 * thread's stack left (the SPU workers have 1 MB), drop the lifted host
 * frames and resume the guest at its
 * current pc from the execution driver (the one-way stack reset path,
 * spu_run_with_halt case 2), with one line the first times it happens. The
 * dropped frames' continuations are lost, which is what the guest had done to
 * them already in every case seen (God of War: Ascension, module 218 under the
 * Wws job manager: SPU_SMC_RET_LR in spu_channels.c). */
/* Per host thread, everything spu_depth_guard keeps: the stack margin
 * (SPU_STACK_RESCUE) and the ring of recent drain sites for the depth
 * report. One block, so a guard finds all of it from one thread pointer. */
static SPU_THREAD_LOCAL struct {
    uintptr_t stack_low, stack_thr;
    uint32_t  n;
    uint32_t  ring[24];
} t_dg;
#ifdef _WIN32
#include <windows.h>
static int s_stack_rescue = -1;
static __attribute__((noinline)) void spu_stack_rescue(spu_context* ctx, uintptr_t left)
{
    static volatile long s_n;
    const long n = __atomic_add_fetch(&s_n, 1, __ATOMIC_RELAXED);
    if (n <= 8 || !(n & (n - 1))) {
        fprintf(stderr, "[spu] host stack nearly exhausted (%u KB left) at drain depth %u: img=%d ovl=%d pc=0x%05X "
                        "-- dropping the lifted host frames, resuming at pc from the driver (SPU_STACK_RESCUE, #%ld)\n",
                (unsigned)(left >> 10), (unsigned)ctx->host_depth, ctx->image_id, (int)ctx->resident_ovl,
                (unsigned)(ctx->pc & SPU_LS_MASK), n);
        fflush(stderr);
    }
    ctx->irq_frame = 0;   /* the interrupt frames live in the frames being dropped */
    extern void spu_restart_dispatch(spu_context*);
    spu_restart_dispatch(ctx);
}
#endif

#ifdef _WIN32
/* The guard's first call on a thread (and while SPU_STACK_RESCUE is unread). */
static __attribute__((noinline)) void spu_depth_guard_init(void)
{
    if (s_stack_rescue < 0) { const char* e = getenv("SPU_STACK_RESCUE"); s_stack_rescue = !(e && e[0] == '0'); }
    if (!t_dg.stack_low) {   /* once per thread: a third of its stack is the margin */
        ULONG_PTR lo = 0, hi = 0; GetCurrentThreadStackLimits(&lo, &hi);
        t_dg.stack_low = lo ? lo : 1; t_dg.stack_thr = hi > lo ? (hi - lo) / 3u : 0;
    }
}
#endif

static __attribute__((noinline)) void spu_depth_guard_trip(spu_context* ctx);

/* spu_depth_guard, inlined into the drain loops below (it ran once per drain,
 * out of line with a stack cookie and three thread-pointer lookups: ~1% of
 * an SPU thread in the geometry jobs). The stack check measures this frame,
 * which is the drain's own once inlined. */
static inline __attribute__((always_inline)) void spu_depth_guard_inl(spu_context* ctx)
{
#ifdef _WIN32
    if (__builtin_expect(s_stack_rescue != 0, 1)) {
        if (__builtin_expect(s_stack_rescue < 0 || !t_dg.stack_low, 0)) spu_depth_guard_init();
        const uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
        if (s_stack_rescue > 0 && t_dg.stack_thr && sp - t_dg.stack_low < t_dg.stack_thr)
            spu_stack_rescue(ctx, sp - t_dg.stack_low);
    }
#endif
    const uint32_t s_max = spu_depth_max();

    /* Ring of recent drain sites. When the depth trips, the useful question is
     * not "where are we" but "what cycle got us here" -- one pc names a point,
     * a ring names the loop. Per host thread: shared, every drain on six SPU
     * threads wrote the same cache line, and the ring of interleaved threads
     * named no loop anyway. */
    t_dg.ring[t_dg.n % 24u] = ((uint32_t)ctx->pc & SPU_LS_MASK);
    t_dg.n++;

    if (ctx->host_depth < s_max) return;
    spu_depth_guard_trip(ctx);
}

/* See the declaration in spu_context.h. */
void spu_depth_guard(spu_context* ctx)
{
    spu_depth_guard_inl(ctx);
}

static __attribute__((noinline)) void spu_depth_guard_trip(spu_context* ctx)
{
    enum { RING = 24 };
    const uint32_t* s_ring = t_dg.ring;
    const uint32_t s_n = t_dg.n;
    static int s_reported = 0;
    if (s_reported < 1) {   /* one report: five SPU threads all trip together */
        s_reported++;
        fprintf(stderr, "[spu-depth] img=%d pc=0x%05X lr=0x%05X host_depth=%u"
                        " -- lifted call recursion, halting the SPU\n",
                ctx->image_id, (uint32_t)ctx->pc & SPU_LS_MASK,
                ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->host_depth);
        fprintf(stderr, "[spu-depth] recent drain sites (oldest first):");
        uint32_t start = (s_n > RING) ? (s_n - RING) : 0;
        for (uint32_t k = start; k < s_n; k++)
            fprintf(stderr, " %05X", s_ring[k % RING]);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    { extern void spu_halt(spu_context*); spu_halt(ctx); }
}

/* See spu_context.h. Env-gated: SPU_TAILRET=1. */
int spu_tailret_enabled(void)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("SPU_TAILRET"); s_on = (e && e[0] == 0x31) ? 1 : 0; }
    return s_on;
}

/* A return may use any register, not only r0. Running that branch target
 * inside this drain executes the caller's continuation twice (once here,
 * once when its real host frame resumes), including its stack adjustment.
 *
 * One kind of "wrong" pc is safe to run here, and must be: a lifted ENTRY
 * that has no host frame of its own. Every WWS job module starts with
 *
 *     ila $r0, entry+0xAC ; a $r0, $r0, $r126 ; br body
 *
 * so the body ends `bi $r0` at a point the entry function tail-jumped to and
 * whose host frame is therefore gone: the continuation exists only as the
 * lifted function seeded at that address. Unwinding instead (the restart
 * below) leaves the manager unable to resume at ITS return point, which is a
 * plain r0 drain with no entry, and the job ends on a garbage r0. So when the
 * callee comes back on a lifted entry, run it in this drain until the pc
 * reaches return_pc; only a pc with no lifted code behind it is unwound. */
typedef void (*spu_drain_fn)(spu_context*);
extern spu_drain_fn spu_lookup(uint32_t addr, int image_id);

/* The host_depth of the interpreter's drain (spu_drain_interp) running on
 * this thread, 0 if the innermost drain is not one. A rejoin at that depth is
 * a trampoline of that drain, where a host return just means "the guest goes
 * to pc next" and the interpreter owning it continues there
 * (spu_smc_microstep); a call bracket raises host_depth before it drains. */
SPU_THREAD_LOCAL int g_spu_drain_anyret;

/* SPU_DRAIN_LIVELOCK (default on; =0: off): one line, once a run, when a
 * thread resumes the same (pc, return_pc) at a lifted entry more than a
 * million times in a row within 10 s -- the drain-resume freeze, where a
 * job's continuation runs again and again without reaching its return point
 * (rc4diag-1: module 216, entry 0xDB58, return_pc 0x7930, ~86 M a second).
 * The state is what the guest is spinning on. Only a print: nothing changes. */
static void spu_drain_livelock_note(spu_context* ctx, uint32_t return_pc, uint32_t pc)
{
    static SPU_THREAD_LOCAL uint32_t t_pc = ~0u, t_ret = ~0u, t_n;
    static SPU_THREAD_LOCAL unsigned long long t_ms;
    static int s_on = -1, s_said;
    extern unsigned long long ps3_ms_now(void);
    const uint32_t ret = return_pc & SPU_LS_MASK;
    if (pc != t_pc || ret != t_ret) { t_pc = pc; t_ret = ret; t_n = 1; t_ms = ps3_ms_now(); return; }
    if (++t_n != 1000000u) return;
    if (s_on < 0) { const char* e = getenv("SPU_DRAIN_LIVELOCK"); s_on = !(e && e[0] == '0'); }
    if (!s_on || ps3_ms_now() - t_ms > 10000ull || __atomic_exchange_n(&s_said, 1, __ATOMIC_ACQ_REL)) return;
    const uint8_t* q = ctx->ls + (pc & 0x3FFFCu);
    const uint8_t* m = ctx->ls + 0x4000;
    fprintf(stderr, "[spu] drain LIVELOCK: the same (pc=0x%05X, return_pc=0x%05X) resumed >1M times in a row in %llu ms: "
                    "img=%d ovl=%d depth=%u job module %02X%02X%02X%02X word@pc %02X%02X%02X%02X int_enable=%d "
                    "events=0x%X mask=0x%X srr0=0x%05X status=0x%X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X\n",
            pc, ret, ps3_ms_now() - t_ms, ctx->image_id, (int)ctx->resident_ovl, (unsigned)ctx->host_depth,
            m[0], m[1], m[2], m[3], q[0], q[1], q[2], q[3], (int)ctx->int_enable,
            ctx->event_status, ctx->event_mask, ctx->srr0 & SPU_LS_MASK, ctx->status,
            ctx->gpr[0]._u32[0], ctx->gpr[1]._u32[0], ctx->gpr[2]._u32[0], ctx->gpr[3]._u32[0],
            ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0]);
    fflush(stderr);
}

static void spu_drain_loop(spu_context* ctx, uint32_t return_pc, int anyret)
{
    spu_depth_guard_inl(ctx);
    const int outer_anyret = g_spu_drain_anyret;
    /* SPU_DIRECT: this loop's steps may go direct unless it is an
     * interpreter's (anyret) drain, which must see every return. The outer
     * loop's state comes back on every way out (SPU_DRAIN_POP_IRQ). */
    const int dir_on = spu_direct_on();
    const int dir_steps = dir_on && !anyret;
    spu_direct_state outer_direct = { ~0u, 0u };
    if (dir_on) { outer_direct = t_spu_direct; if (anyret) t_spu_direct.depth = ~0u; }
    /* The frame an interrupt taken in THIS loop returns to. It stays armed
     * after the handler's first host function returns -- the handler keeps
     * running as trampolines of this loop -- until its iret fires or the
     * loop exits, whichever first. */
    spu_irq_frame f;
    f.prev = 0; f.depth = 0; f.image_id = 0;
#define SPU_DRAIN_POP_IRQ() do { if (ctx->irq_frame == &f) ctx->irq_frame = f.prev; ctx->drain_ret_pc = 0; g_spu_drain_anyret = outer_anyret; \
                                  if (dir_on) t_spu_direct = outer_direct; } while (0)
    for (;;) {
        while (g_spu_trampoline_fn) {
            if (g_spu_trampoline_fn == spu_indirect_branch && (return_pc || !anyret) &&
                (ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) {
                g_spu_trampoline_fn = 0;
                SPU_DRAIN_POP_IRQ();
                return;
            }
            void (*fn)(spu_context*) = g_spu_trampoline_fn;
            g_spu_trampoline_fn = 0;
            ctx->drain_ret_pc = return_pc & SPU_LS_MASK;   /* re-set: a nested drain changed it */
            g_spu_drain_anyret = anyret ? (int)ctx->host_depth : 0;
            { extern volatile int g_yz_lockstep_on;   /* -1 (undecided) takes the call */
              if (g_yz_lockstep_on) yz_lockstep_tick(ctx); }
            spu_step_check(ctx, fn);
            { const spu_iojrn_ops* j = t_spu_iojrn;   /* spu_iojrn.h: a traced run */
              if (__builtin_expect(j != 0, 0) && j->step) j->step(ctx, fn);
              if (dir_steps) {
                  /* SPU_DIRECT: the transfers made while this step runs may skip
                   * the loop when they would find nothing else to do here. */
                  extern volatile int g_yz_lockstep_on;
                  t_spu_direct.depth = (!g_yz_lockstep_on && s_step_inline > 0 && !g_spu_stack_reset_count &&
                                        g_pm_flow_ctx != (void*)ctx && !(j && j->step)) ? ctx->host_depth : ~0u;
                  t_spu_direct.ret = return_pc & SPU_LS_MASK;
              } }
            if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) {
                void (*vf)(spu_context*) = spu_take_interrupt(ctx, fn);
                if (!ctx->int_enable) {   /* taken (a take clears the enable; a deferral leaves it) */
                    /* Interrupt taken here: this loop is the frame the iret
                     * comes back to, however deep the handler leaves from. */
                    if (ctx->irq_frame != &f) {
                        f.prev = (spu_irq_frame*)ctx->irq_frame;
                        f.depth = ctx->host_depth;
                        f.image_id = ctx->image_id;
                        ctx->irq_frame = &f;
                    }
                    if (setjmp(f.env) == 0) {
                        spu_jmpbuf_noframe(f.env);
                        vf(ctx);
                    } else {
                        /* iret fired deeper: registers restored, pc = srr0 */
                        ctx->host_depth = f.depth;
                        ctx->image_id = f.image_id;
                        g_spu_drain_anyret = anyret ? (int)ctx->host_depth : 0;
                        g_spu_trampoline_fn = spu_indirect_branch;
                        SPU_DRAIN_POP_IRQ();
                    }
                    continue;
                }
            }
            fn(ctx);
        }
        /* Not while an interrupt this loop took is still unreturned: its
         * frame must stay armed for the iret (drain-resume below keeps it). */
        if ((anyret && ctx->irq_frame != &f) || (ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) {
            SPU_DRAIN_POP_IRQ();
            return;
        }
        {
            uint32_t pc = ctx->pc & SPU_LS_MASK;
            spu_drain_fn fn = ctx->resident_ovl ? spu_lookup(pc, ctx->resident_ovl) : 0;
            if (!fn) fn = spu_lookup(pc, ctx->image_id);
            if (fn) {
                /* The count saturates: it went up on every resume, so a livelock
                 * wrapped it past INT_MAX in ~25 s and then printed every pass
                 * (rc4diag-1: 1.28 M lines, the soak's flood guard). */
                { static int _n = 0; if (_n < 8) { _n++;
                    fprintf(stderr, "[spu] drain-resume return_pc=0x%05X at lifted entry 0x%05X img=%d depth=%d ovl=%d\n",
                            (unsigned)(return_pc & SPU_LS_MASK), pc, ctx->image_id, ctx->host_depth,
                            (int)ctx->resident_ovl); } }
                spu_drain_livelock_note(ctx, return_pc, pc);   /* SPU_DRAIN_LIVELOCK */
                g_spu_trampoline_fn = spu_indirect_branch;
                continue;
            }
        }
        {
            extern void spu_restart_dispatch(spu_context*);
            SPU_DRAIN_POP_IRQ();
            spu_restart_dispatch(ctx);
            return;
        }
    }
#undef SPU_DRAIN_POP_IRQ
}

/* A lifted call whose callee came back by host return, to the call's own
 * return pc, has nothing to drain: the loop would only check the depth, find
 * no transfer pending and the pc at return_pc, and clear drain_ret_pc. Every
 * brsl/bisl bracket ends here, so do just that inline (the drain-site ring
 * of the depth report is the one thing skipped). An interrupt frame still
 * armed takes the loop, whose exit pops it if it is this one.
 * SPU_DRAIN_FAST=0: always run the loop. */
static int s_drain_fast = -1;
/* SPU_DRAIN_SPLIT (default on with SPU_DRAIN_FAST; =0: one function as
 * before): the fast case first, in a function that needs no frame, and the
 * rest -- the first calls' getenvs, the depth limit's first read, the loop --
 * behind one tail call to the function as it was. The fast test is the same
 * one: s_drain_split > 0 only once SPU_DRAIN_FAST is read and on, and a
 * nonzero s_depth_max is what spu_depth_max() returns. */
#define s_drain_split g_spu_drain_split   /* exported: SPU_CALL_END reads it */
int s_drain_split = -1;
static __attribute__((noinline)) void spu_drain_call_slow(spu_context* ctx, uint32_t return_pc)
{
    if (s_drain_fast < 0) { const char* e = getenv("SPU_DRAIN_FAST"); s_drain_fast = !(e && e[0] == '0'); }
    if (s_drain_split < 0) { const char* e = getenv("SPU_DRAIN_SPLIT"); s_drain_split = s_drain_fast && !(e && e[0] == '0'); }
    if (s_drain_fast && !g_spu_trampoline_fn && !ctx->irq_frame &&
        (((uint32_t)ctx->pc ^ return_pc) & SPU_LS_MASK) == 0 &&
        ctx->host_depth < spu_depth_max()) {
        ctx->drain_ret_pc = 0;
        return;
    }
    spu_drain_loop(ctx, return_pc, 0);
}
void spu_drain_call(spu_context* ctx, uint32_t return_pc)
{
    if (__builtin_expect(s_drain_split > 0, 1) && !g_spu_trampoline_fn && !ctx->irq_frame &&
        (((uint32_t)ctx->pc ^ return_pc) & SPU_LS_MASK) == 0 &&
        s_depth_max && ctx->host_depth < s_depth_max) {
        ctx->drain_ret_pc = 0;
        return;
    }
    spu_drain_call_slow(ctx, return_pc);
}

/* SPU_DRAIN (spu_context.h) for the top-level drain of spu_run_with_halt,
 * with the inlined step check. Same hooks in the same order. */
void spu_drain_top(spu_context* ctx)
{
    spu_depth_guard_inl(ctx);
    const int tailret = spu_tailret_enabled();
    while (g_spu_trampoline_fn) {
        if (tailret && ctx->host_depth > 0 &&
            (((uint32_t)ctx->pc & SPU_LS_MASK) == (ctx->gpr[0]._u32[0] & SPU_LS_MASK))) {
            g_spu_trampoline_fn = 0;
            break;
        }
        void (*tf)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        { extern volatile int g_yz_lockstep_on;   /* 0: the tick returns at once; -1 (undecided) takes the call */
          if (g_yz_lockstep_on) yz_lockstep_tick(ctx); }
        spu_step_check(ctx, tf);
        if (ctx->int_enable && (ctx->event_status & ctx->event_mask))
            tf = spu_take_interrupt(ctx, tf);
        spu_pchist_tick(ctx);
        g_spu_pch[g_spu_pch_n++ & 7u] = (uint32_t)(ctx->pc & SPU_LS_MASK);
        tf(ctx);
    }
}

/* The drain of lifted code the interpreter rejoined: it ends at the first
 * host return, whatever the pc (or at return_pc, the enclosing drain's own
 * return point, when nonzero). */
void spu_drain_interp(spu_context* ctx, uint32_t return_pc)
{
    spu_drain_loop(ctx, return_pc, 1);
}
