/* host_pin.c -- topology-aware placement of the host threads that carry the
 * frame, plus the EcoQoS opt-out and MMCSS. Every switch is OFF by default.
 *
 * WHY. The frame is carried by 9 always-runnable threads: 6 SPURS policy
 * workers (cellSpurs.c spurs_pm_worker), the guest main thread (boot_main.cpp
 * ppu_run; ~55% of it is usleep(30) polls that spin on SwitchToThread,
 * sys_timer.c:265-279), the present thread that walks the RSX FIFO and submits
 * D3D12 (boot_main.cpp vblank_ticker), and the SPURS kernel, which spins on
 * SwitchToThread under SPURS_KERNEL_SPIN=1 (cellSpurs.c:2703-2717). On an
 * 8-core/16-thread laptop (7840HS) that is more hot threads than physical
 * cores, so the scheduler doubles two of them up on one core's two SMT
 * threads at any moment, migrates them, and lets bursty threads (4 PSO
 * builders, the NVIDIA driver's threads, 21 guest PPU threads) land beside
 * the walker or an SPU lane. SPURS_PIN / PS3_PIN_PPU are set in
 * ascension_defaults.cpp:289,292 but nothing reads them.
 *
 * WHAT. Read the topology (GetLogicalProcessorInformationEx: cores, SMT
 * siblings, L3 groups; GetSystemCpuSetInformation: CPU set ids and the
 * scheduler's core ranking), then, for 8+ physical cores:
 *
 *   walker      -> the best-ranked core, primary hardware thread; its SMT
 *                  sibling left EMPTY (PS3_PIN_WALKER_SOLO, default 1)
 *   SPU 0..5    -> the next six cores, one each, primary hardware thread
 *                  (SPUs 0-3 carry EDGE, so they get the better cores)
 *   PPU main    -> the core holding logical processor 0 (most interrupts and
 *                  DPCs land there; the PPU is ~55% polling, so it minds least)
 *   SPURS kernel-> the SMT sibling of SPU 5's core (it spins; beside the PPU
 *                  it slowed the main thread: rows 26-27. PS3_PIN_KERNEL_PPU=1
 *                  puts it on the PPU core's sibling as before)
 *   light       -> the process DEFAULT CPU sets: the SPU cores' SMT siblings
 *                  and any spare core. Every thread not pinned here (guest PPU
 *                  threads, audio, PSO builders, the GPU driver's threads,
 *                  the watchdog) runs there.
 *
 * With 10+ cores the kernel gets a core of its own, and spare cores join the
 * light set. With fewer than 8 cores there is no placement at all unless
 * PS3_PIN_SMALL=1 (host_small_cpu: the OS's own spreading is faster there) --
 * except on a uniform 6-core CPU with SMT, where it is on by default
 * (hp_six_default, 2026-11-09: 6 cores and 12 logical processors, all of them
 * the process's, one core type, one L3; PS3_PIN_SMALL=0 turns it off there);
 * with it, the plan below: the walker gets a core (both hardware threads,
 * PS3_PIN_SMALL_WALKER) and the PPU a hardware thread of a core without logical
 * processor 0 (PS3_PIN_SMALL_PPU); the SPU workers and the kernel share the
 * other cores as one set (never fewer than 6 workers: H 0.5b / MASTER-PLAN
 * section 8), and the light threads may run anywhere but on the walker's core
 * and the PPU's hardware thread (PS3_PIN_SMALL_OLD=1: the first plan, light on
 * ONE logical processor).
 * In that plan the light threads share the SPU workers' logical processors, so
 * the SPU workers, the SPURS kernel and the SPURS task threads run at
 * THREAD_PRIORITY_ABOVE_NORMAL (PS3_PIN_SPU_PRIO, hp_role_prio): the guest PPU
 * threads with a guest priority under 256 run there too (sys_ppu_thread.c), and
 * one of them spinning above a ready SPU worker held it off the CPU for ~4 s,
 * the other five SPUs waiting on it, until Windows' starvation boost (a 6-core
 * test run of v0.1.23 with PS3_PIN_SMALL=1, 2026-11-09).
 *
 * CPU sets (SetThreadSelectedCpuSets / SetProcessDefaultCpuSets) rather than
 * hard affinity by default: the assignment is just as strict for our threads,
 * but the OS may still run a thread elsewhere if its CPU set is taken away
 * (Game Mode / "Allocated" sets), so a bad plan degrades instead of
 * deadlocking. PS3_HOST_PIN_HARD=1 uses SetThreadGroupAffinity instead.
 *
 * Switches (all off unless noted):
 *   PS3_HOST_PIN=1           master switch for everything below.
 *     SPURS_PIN=0            ...but leave the SPU workers floating
 *                            (ascension_defaults.cpp sets it to 1 already).
 *     PS3_PIN_PPU=0          ...but leave the PPU main thread floating (ditto).
 *     PS3_PIN_WALKER=0       ...but leave the walker floating.
 *     PS3_PIN_KERNEL=0       ...but leave the SPURS kernel floating.
 *     PS3_PIN_WALKER_SOLO=0  let light threads use the walker's SMT sibling.
 *     PS3_PIN_MAP=walker=7;ppu=0;kernel=0s;spu=1,2,3,4,5,6
 *                            explicit plan, physical core numbers as printed
 *                            in the [pin] line; "Ns" = core N's SMT sibling.
 *     PS3_HOST_PIN_HARD=1    hard thread affinity instead of CPU sets.
 *     PS3_HOST_PIN_AB=<ms>   flip the whole plan on/off every <ms> (5000 lines
 *                            up with the 5 s [draws]/[perf] windows) and print
 *                            "[pin-ab] on|off t=<us>" so abwin.py can tag the
 *                            windows. The in-run A/B the measurement rules ask
 *                            for (HANDOFF 0.12).
 *   PS3_ECOQOS_OFF=1         opt the process (and each pinned thread) out of
 *                            EcoQoS execution-speed throttling, and keep the
 *                            1 ms timer resolution honoured when the window is
 *                            occluded (Windows 11 otherwise ignores
 *                            timeBeginPeriod for a hidden window).
 *   PS3_MMCSS=1              walker and PPU main join the MMCSS "Games" task,
 *                            the audio mixer "Audio". =2 also raises the Games
 *                            threads to AVRT_PRIORITY_HIGH. Only sensible with
 *                            PS3_HOST_PIN=1: a boosted spinning thread on a
 *                            shared logical processor starves its neighbour.
 *   PS3_HOST_PIN_LOG=0       no [pin] lines.
 *
 * HYBRID CPUs (2026-11-09; Intel 12th-14th gen and Core Ultra: fast P-cores and
 * slow E-cores, told apart by EfficiencyClass). The plan above ranks P-cores
 * first but needs 8 hot cores, so it put SPU 4-5 (5 = the game's sound job, wid
 * 8) and, from 10 cores on, the PPU main thread and the SPURS kernel on E-cores
 * while light threads got P-cores; at exactly 8 cores (4P+4E) the 8-core rule
 * turns placement off. On a CPU whose usable cores have more than one
 * EfficiencyClass (PS3_HYBRID=0: everything as before, the uniform rules):
 *   - UPDATE 2026-11-09 night (host\hybrid\NOTES.md sections 9-11, after SHADE's
 *     real A/B on an i5-12500H 4P+8E laptop: the P pool below ran 25.5 vs 40.3
 *     fps -- Windows parks the P-cores' second hardware threads, and a soft set
 *     holding parked processors runs on its unparked ones only, so his 8-lp pool
 *     was 4 lps for 9 hot threads): the hybrid plan is the default on NO CPU
 *     (hp_hybrid_default_on): every hybrid CPU gets 0.1.23's behaviour (as
 *     PS3_HYBRID=0) unless PS3_HOST_PIN_AB or PS3_HYBRID_PLAN asks. The pool on
 *     8 P-cores measured better only in the parked simulation (+16% in one run,
 *     +11% in another, a tie overall in a third) and never on a real CPU. The
 *     hybrid side (opt-in): with 8+ P-cores the pool, below that the A/B flips
 *     three sides, "olds0", "olds" and 0.1.23 (olds0 = 0.1.23's single-processor
 *     plan with the sound job's SPU 5 on a P-core and one more SPU worker on the
 *     lp0 core's first thread; olds = the same without the lp0 core): olds0 tied
 *     0.1.23 in fps in the parked simulation on 4P+8E with fewer frames >= 50 ms,
 *     and a real laptop has to decide (Intel test 2). The rest of this paragraph
 *     is the pool design (PS3_HYBRID_PLAN=pool):
 *   - the hybrid plan (with PS3_HOST_PIN=1, when the P-cores have at least 8
 *     logical processors, one per hot thread): the walker gets a P-core of its
 *     own (both hardware threads, nobody else of ours there) when the other
 *     P-cores are still 7+ cores for the PPU main thread and the six SPU workers
 *     (cores, i.e. first hardware threads: the second ones may be parked), else
 *     it joins them; those share ONE soft set, the "P pool"
 *     (every logical processor of the other P-cores: no single-processor pins, so
 *     a worker that wakes takes whichever P hardware thread is free -- the
 *     8-core stalls came with single-processor exclusive pins); the SPURS kernel
 *     joins the pool too, unless it spins and the pool has fewer cores than
 *     threads (then the E-cores); light threads (the process
 *     default: guest PPU threads, the audio mixer, PSO builders, the GPU driver's
 *     threads) on the E-cores, from the guest's start on (LD_PSO_PRECOMPILE's
 *     pipeline building before it may use every core). Fewer than 8 P-core
 *     logical processors: no placement on the hybrid side (the OS spreads).
 *   - host_small_cpu() counts P-cores: fewer than 8 -> "small" -> the SPURS
 *     kernel's yield loop off (SPURS_KERNEL_SPIN=auto). The 8-core rule (no
 *     placement at exactly 8 cores) is not applied to the hybrid plan: the
 *     single-processor pins it works around are not in it.
 *   - EcoQoS opt-out (as PS3_ECOQOS_OFF=1) by default for the process and the
 *     hot threads; PS3_ECOQOS_OFF=0 keeps it off.
 *   - PS3_HOST_PIN_AB=<ms> flips between the hybrid side and today's behaviour
 *     on this CPU (old plan or none, by today's rules; EcoQoS and the kernel's
 *     auto yield loop as today) every <ms> from the guest's start, printing
 *     "[pin-ab] hybrid|old t=<us>" (host\hybrid\tools\habwin.py splits a log);
 *     below 8 P-cores it flips olds0, olds and old ("[pin-ab] olds0|olds|old",
 *     host\hybrid\tools\simtab10.py splits that).
 *   PS3_HYBRID_SOLO=0/1 forces the walker's own P-core off/on, PS3_HYBRID_KERNEL=p|e
 *   the kernel's set. PS3_PIN_MAP on a hybrid: today's behaviour (the map).
 *   PS3_HYBRID_PLAN=<layout> (pool, prim, olds, spill, split or 9-10 letters, see
 *   hp_layout_t) replaces the hybrid side's layout; PS3_HYBRID_AB_SIDES=<a>,<b>[,<c>]
 *   makes the A/B flip through those layouts ("old" = today's behaviour) in turn.
 *   PS3_FAKE_HYBRID=<P>p<E>e: TEST ONLY, see hp_fake_hybrid (PS3_FAKE_PARK=1 there);
 *   both are ignored unless PS3_TEST_ONLY=1 is set too.
 *   The process affinity mask counts (hp_hybrid_detect): an 8P+8E CPU limited to its
 *   P-cores (Task Manager, Process Lasso) is a uniform 8-core CPU here, as in 0.1.23.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include "host_pin.h"

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

#define HP_MAX_LP    256
#define HP_MAX_CORE  128
#define HP_MAX_REG   64
#define HP_NSPU      8            /* room for SPURS_NSPUS=8; the plan places 6 */

typedef struct {
    WORD  group;
    BYTE  idx;                    /* logical processor number within the group */
    ULONG cpuset;                 /* CPU set id (0 = unknown) */
    int   core;                   /* index into s_core */
    BYTE  sched;                  /* SchedulingClass (higher = preferred), 0 if not exposed */
    BYTE  unusable;               /* parked / allocated to another process */
} hp_lp_t;

typedef struct {
    int   lp[4];                  /* s_lp indices, primary first */
    int   nlp;
    DWORD l3_size;                /* bytes of the L3 this core sits under */
    int   l3_id;
    BYTE  sched;                  /* best SchedulingClass of its threads */
    BYTE  eff;                    /* EfficiencyClass (hybrid CPUs; 0 on Zen 4) */
    int   has_lp0;
} hp_core_t;

typedef struct { short lp[HP_MAX_LP]; int n; } hp_set_t;

static hp_lp_t   s_lp[HP_MAX_LP];
static int       s_nlp;
static hp_core_t s_core[HP_MAX_CORE];
static int       s_ncore;

static hp_set_t  s_set_walker, s_set_ppu, s_set_kernel, s_set_light;
static hp_set_t  s_set_spu[HP_NSPU];
static unsigned char s_lp_hot[HP_MAX_LP];   /* hot-role slots assigned per LP */
static unsigned char s_lp_light[HP_MAX_LP];

static volatile LONG s_init_state;           /* 0 none, 1 running, 2 done */
static int  s_pin_on, s_pin_spu, s_pin_ppu, s_pin_walker, s_pin_kernel;
static int  s_hard, s_log = 1, s_ecoqos_off, s_mmcss;
static volatile LONG s_ab_on = 1;            /* PS3_HOST_PIN_AB: plan currently applied */

static __declspec(thread) int t_exclusive;

typedef struct { HANDLE h; enum host_role role; int index; } hp_reg_t;
static hp_reg_t s_reg[HP_MAX_REG];
static volatile LONG s_nreg;
static SRWLOCK s_reg_lock = SRWLOCK_INIT;

/* Hybrid CPUs (see the header). */
typedef struct {
    hp_set_t walker, pool, kernel, light, all;
    hp_set_t wcore, prest, pall;              /* the first P-core; the other P-cores; every P-core (layouts) */
    unsigned char lp_hot[HP_MAX_LP], lp_light[HP_MAX_LP];
    int walker_solo, kernel_e, kernel_spin, np, ne;
    int pc[HP_MAX_CORE], ec[HP_MAX_CORE];     /* P-cores (best first, the lp0 core last) and E-cores, as cores */
} hp_hyplan_t;
static hp_hyplan_t s_hy;
/* Layouts of the hybrid plan (tuning, 2026-11-09 evening; host\hybrid\NOTES.md
 * section 9). One letter per hot thread -- walker, PPU main, SPURS kernel, SPU
 * 0..5 -- naming its CPU set: w = the walker's P-core of its own (both hardware
 * threads of the first P-core), p = the P pool (the other P-cores; every P-core
 * when no thread has w), e = the E-cores, q = the P pool and the E-cores (soft:
 * the scheduler picks), a = every core. Named: "pool" = the rule above (walker w
 * when the other P-cores keep 7+ logical processors, else p; kernel p, or e
 * when it spins in a small pool; SPUs p), "spill" = wpqqqqqqp (4 P-cores: the
 * walker's own core, PPU + SPU 5 on P, SPU 0-4 + the kernel also on the
 * E-cores), "split" = wppppeeep. SPU 5 carries the sound job (wid 8).
 * More letters (after SHADE's real A/B, NOTES.md section 9): x = a logical
 * processor of its own, the FIRST hardware thread of a P-core (the lp0 core
 * last; none left: an E-core), y = an E-core of its own; both handed out in the
 * order walker, PPU main, SPU 5, SPU 0-4, kernel (x before y), none left: f;
 * f = soft over the first hardware threads of the P-cores nobody owns and the
 * E-cores nobody owns. A 10th letter places the light threads (the process
 * default): e = the E-cores (default), l = every logical processor no hot thread
 * owns, but not the walker's core (its own or its x's sibling), a = every core.
 * Why x/y: on a 4P+8E laptop (SHADE, i5-12500H) Windows parks the P-cores'
 * second hardware threads; a soft set that includes parked processors runs on
 * its unparked ones only (CPU Sets docs, and host\hybrid cstest), so the 8-lp P
 * pool behaved like 4 lps (hybrid side 25.5 vs 40.3 fps, process CPU 28% vs 54%).
 * "prim" = x for the walker, the PPU main, SPU 5 and SPU 0.. while P-cores
 * other than the lp0 core last, y for the rest; more y than E-cores: those f;
 * light l. "olds" = the same with 0.1.23's order but SPU 5 first: walker, SPU
 * 5, SPU 1-4, SPU 0, PPU main (on 4P+8E: 0.1.23's plan with SPU 5 and SPU 0
 * swapped). "olds0" = olds over every P-core: the lp0 core's first thread takes
 * one more SPU worker (4P+8E: walker, SPU 5, SPU 0 on P first threads, SPU 1 on
 * the lp0 core's; PPU, SPU 2-4, kernel on own E-cores). Uniform CPUs never get here.
 * DEFAULTS (NOTES.md sections 9-11): the hybrid plan is on by default on NO CPU (hp_hybrid_default_on): a hybrid CPU
 * gets 0.1.23's behaviour unless PS3_HOST_PIN_AB / PS3_HYBRID_PLAN ask for it. The hybrid side's layout then: pool
 * with 8+ P-cores, olds0 below (hp_layout_default); PS3_HOST_PIN_AB without PS3_HYBRID_PLAN / PS3_HYBRID_AB_SIDES
 * below 8 P-cores flips olds0, olds and old (hp_init_hybrid). "pool" (the plan of hyb3 / Intel test 1) stays
 * available by PS3_HYBRID_PLAN=pool. */
#define HP_NLAY  9
#define HP_NSIDE 3
typedef struct {
    char name[16], spec[HP_NLAY + 2];     /* 9 hot letters + an optional light letter */
    hp_set_t set[HP_NLAY], p, light;      /* per hot thread; its 'p' set (SPU 6-7 under SPURS_NSPUS); light */
    unsigned char lp_hot[HP_MAX_LP], lp_light[HP_MAX_LP];
    int walker_solo;
    int kspin;                            /* name ends in +k / -k: the kernel's auto yield loop on / off; else -1 */
} hp_layout_t;
static hp_layout_t s_lay[HP_NSIDE];       /* per side of the A/B (side 0 only without one) */
static int  s_nside = 1;                  /* sides the A/B flips through */
static int  s_side_old[HP_NSIDE];         /* side k = today's behaviour on this CPU, not a hybrid layout */
static char s_side_name[HP_NSIDE][16];
static int  s_ab_named;                   /* PS3_HYBRID_AB_SIDES given: tags/lines name the sides */
static int  s_side_spin[HP_NSIDE];        /* the kernel's auto yield loop per side (s_spin_by_side) */
static int  s_spin_by_side;               /* a hybrid layout decides it (+k / -k, or the A/B's sides differ) */
static int  s_hy_on;                         /* usable cores of more than one EfficiencyClass, PS3_HYBRID != 0 */
static int  s_hy_effmax;                     /* the P-cores' EfficiencyClass */
static int  s_hy_np, s_hy_ne, s_hy_npl, s_hy_nel;   /* P-/E-cores and their logical processors */
static int  s_hy_small;                      /* host_small_cpu() there: fewer than 8 P-cores */
static int  s_hy_plan_on;                    /* the hybrid plan places threads */
static int  s_old_on;                        /* today's plan would place threads (the A/B's old side) */
static int  s_hy_ecoqos;                     /* EcoQoS opt-out by the hybrid default (A/B: hybrid side only) */
static int  s_hy_ab, s_hy_ab_spin;           /* hybrid A/B; and the kernel's auto yield loop differs by side */
static volatile LONG s_side;                 /* side in force (index; side 0 = the default layout, 1 = old) */
static volatile LONG s_game_started;         /* the PPU main thread registered (after LD_PSO_PRECOMPILE) */
static SRWLOCK s_side_lock = SRWLOCK_INIT;
static __declspec(thread) int t_excl_side[HP_NSIDE];
static int  s_fake_cores;                    /* PS3_FAKE_HYBRID (TEST ONLY): P+E, else 0 */

static int env_int(const char* name, int dflt)
{
    const char* e = getenv(name);
    return (e && *e) ? atoi(e) : dflt;
}

/* ---------------------------------------------------------------- topology */

/* The physical cores this process may run on (after PS3_CPU_LIMIT): cores with
 * a hardware thread in the process affinity mask. 0 when not known. */
static int hp_process_cores(void)
{
    DWORD_PTR pm = 0, sm = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) || !pm) return 0;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !len) return 0;
    BYTE* buf = (BYTE*)malloc(len);
    if (!buf) return 0;
    int n = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len))
        for (DWORD off = 0; off < len; ) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (e->Relationship == RelationProcessorCore)
                for (WORD g = 0; g < e->Processor.GroupCount; g++)
                    if (e->Processor.GroupMask[g].Group == 0 && (e->Processor.GroupMask[g].Mask & pm)) { n++; break; }
            off += e->Size;
        }
    free(buf);
    return n;
}

static int s_proc_cores;

/* Fewer than 8 physical cores for the process (PS3_SMALL_CPU=0/1 forces the
 * answer). There the six SPU workers need every hardware thread: placement
 * (which gives the walker and the PPU one each and the workers the rest) and
 * the SPURS kernel's yield loop both cost more than they give. Heaviest fight,
 * user51, PS3_CPU_LIMIT, 2026-11-02, both on vs both off: 4 cores 23.8 vs 35.2
 * fps, 6 cores 42.9 vs 46.4; 8 cores 55.1 vs 49.0 (kept on). Valid once
 * host_qos_init has applied PS3_CPU_LIMIT. On a hybrid CPU (s_hy_on) the
 * answer counts P-cores instead (hybrid header); hp_small_old() is today's. */
static volatile LONG s_small = -1;
static int hp_small_old(void)
{
    LONG v = s_small;
    if (v < 0) {
        s_proc_cores = s_fake_cores ? s_fake_cores : hp_process_cores();
        const char* e = getenv("PS3_SMALL_CPU");
        v = (e && *e) ? atoi(e) != 0 : (s_proc_cores > 0 && s_proc_cores < 8);
        s_small = v;
    }
    return (int)v;
}
int host_small_cpu(void)
{
    return s_hy_on ? s_hy_small : hp_small_old();
}

static int hp_find_lp(WORD group, BYTE idx)
{
    for (int i = 0; i < s_nlp; i++)
        if (s_lp[i].group == group && s_lp[i].idx == idx) return i;
    return -1;
}

static int hp_read_topology(void)
{
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, NULL, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !len) return 0;
    BYTE* buf = (BYTE*)malloc(len);
    if (!buf) return 0;
    if (!GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        free(buf); return 0;
    }
    /* Pass 1: cores and their hardware threads, in the order Windows lists them. */
    for (DWORD off = 0; off < len; ) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
        if (e->Relationship == RelationProcessorCore && s_ncore < HP_MAX_CORE) {
            hp_core_t* c = &s_core[s_ncore];
            memset(c, 0, sizeof *c);
            c->l3_id = -1;
            c->eff = e->Processor.EfficiencyClass;
            for (WORD g = 0; g < e->Processor.GroupCount; g++) {
                const GROUP_AFFINITY* ga = &e->Processor.GroupMask[g];
                for (BYTE b = 0; b < 64; b++) {
                    if (!(ga->Mask & ((KAFFINITY)1 << b)) || s_nlp >= HP_MAX_LP) continue;
                    hp_lp_t* l = &s_lp[s_nlp];
                    memset(l, 0, sizeof *l);
                    l->group = ga->Group; l->idx = b; l->core = s_ncore;
                    if (c->nlp < 4) c->lp[c->nlp++] = s_nlp;
                    if (ga->Group == 0 && b == 0) c->has_lp0 = 1;
                    s_nlp++;
                }
            }
            if (c->nlp) s_ncore++;
        }
        off += e->Size;
    }
    /* Pass 2: L3 groups (one per CCX on Zen; the 9950X3D's V-cache CCD has
     * the bigger one). */
    int l3_next = 0;
    for (DWORD off = 0; off < len; ) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
        if (e->Relationship == RelationCache && e->Cache.Level == 3 &&
            (e->Cache.Type == CacheUnified || e->Cache.Type == CacheData)) {
            const GROUP_AFFINITY* ga = &e->Cache.GroupMask;
            const int id = l3_next++;
            for (int c = 0; c < s_ncore; c++) {
                const hp_lp_t* l = &s_lp[s_core[c].lp[0]];
                if (l->group == ga->Group && (ga->Mask & ((KAFFINITY)1 << l->idx))) {
                    s_core[c].l3_id = id; s_core[c].l3_size = e->Cache.CacheSize;
                }
            }
        }
        off += e->Size;
    }
    free(buf);

    /* CPU set ids (needed for SetThreadSelectedCpuSets) and the scheduler's
     * ranking. SchedulingClass is what Windows exposes of the preferred-core
     * order on hybrid parts; whether it reflects AMD CPPC ranks on the 7840HS
     * is UNVERIFIED -- the [pin] line prints it, and PS3_PIN_MAP overrides. */
    ULONG clen = 0;
    GetSystemCpuSetInformation(NULL, 0, &clen, GetCurrentProcess(), 0);
    if (clen) {
        BYTE* cb = (BYTE*)malloc(clen);
        if (cb && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)cb, clen, &clen, GetCurrentProcess(), 0)) {
            for (ULONG off = 0; off < clen; ) {
                PSYSTEM_CPU_SET_INFORMATION e = (PSYSTEM_CPU_SET_INFORMATION)(cb + off);
                if (e->Type == CpuSetInformation) {
                    const int i = hp_find_lp(e->CpuSet.Group, e->CpuSet.LogicalProcessorIndex);
                    if (i >= 0) {
                        s_lp[i].cpuset = e->CpuSet.Id;
                        s_lp[i].sched = e->CpuSet.SchedulingClass;
                        s_lp[i].unusable = (e->CpuSet.Allocated && !e->CpuSet.AllocatedToTargetProcess) ? 1 : 0;
                        hp_core_t* c = &s_core[s_lp[i].core];
                        if (s_lp[i].sched > c->sched) c->sched = s_lp[i].sched;
                    }
                }
                off += e->Size;
            }
        }
        free(cb);
    }
    return s_ncore > 0;
}

/* PS3_FAKE_HYBRID=<P>p<E>e -- TEST ONLY, for a PC without E-cores: the code
 * paths of a hybrid CPU on a uniform one. The first <P> physical cores become
 * P-cores (EfficiencyClass 1, all their hardware threads; core 0 keeps logical
 * processor 0, as on Intel); <E> E-cores (EfficiencyClass 0, one logical
 * processor each, as Intel's have no SMT) are made of the hardware threads of
 * the LAST physical cores, one fake E-core per logical processor, so two fake
 * E-cores share a real core (a busy pair runs at SMT speed: slower than a
 * P-core, but no model of a real E-core). The process may use exactly those
 * logical processors (as PS3_CPU_LIMIT, which is then ignored), and every rule
 * sees <P>+<E> cores. Applied whatever PS3_HYBRID says (PS3_HYBRID=0 + this =
 * today's behaviour on that topology). */
static int hp_fake_hybrid(const char* spec)
{
    int np = 0, ne = 0;
    char cp = 0, ce = 0;
    if (sscanf(spec, "%d%c%d%c", &np, &cp, &ne, &ce) != 4 || (cp != 'p' && cp != 'P') || (ce != 'e' && ce != 'E') ||
        np < 1 || ne < 1 || np >= s_ncore) {
        fprintf(stderr, "[pin] PS3_FAKE_HYBRID=%s: not <P>p<E>e for this CPU's %d cores, ignored\n", spec, s_ncore);
        return 0;
    }
    int efirst = s_ncore, got = 0;
    while (got < ne && efirst > np) { efirst--; got += s_core[efirst].nlp; }
    if (got < ne) {
        fprintf(stderr, "[pin] PS3_FAKE_HYBRID=%s: %d E-cores do not fit beside %d P-cores on %d cores, ignored\n",
                spec, ne, np, s_ncore);
        return 0;
    }
    static hp_core_t nc[HP_MAX_CORE];
    int n = 0, made = 0;
    for (int c = 0; c < np; c++) { nc[n] = s_core[c]; nc[n].eff = 1; n++; }
    for (int c = efirst; c < s_ncore && made < ne; c++)
        for (int k = 0; k < s_core[c].nlp && made < ne && n < HP_MAX_CORE; k++, made++) {
            hp_core_t e = s_core[c];
            const int lp = s_core[c].lp[k];
            e.lp[0] = lp; e.nlp = 1; e.eff = 0;
            e.has_lp0 = s_lp[lp].group == 0 && s_lp[lp].idx == 0;
            e.sched = s_lp[lp].sched;
            nc[n++] = e;
        }
    static unsigned char used[HP_MAX_LP];
    memset(used, 0, sizeof used);
    DWORD_PTR mask = 0;
    for (int c = 0; c < n; c++)
        for (int k = 0; k < nc[c].nlp; k++) {
            const hp_lp_t* l = &s_lp[nc[c].lp[k]];
            if (l->group != 0 || l->idx >= 64) {
                fprintf(stderr, "[pin] PS3_FAKE_HYBRID=%s: needs processor group 0 only, ignored\n", spec);
                return 0;
            }
            used[nc[c].lp[k]] = 1;
            mask |= (DWORD_PTR)1 << l->idx;
        }
    /* PS3_FAKE_PARK=1 (TEST ONLY): the P-cores' second hardware threads stay in the topology (the plans
     * see 2 per P-core) but out of the process mask, as SHADE's parked SMT siblings: a CPU set that holds
     * them runs on its other processors only, one that holds nothing else runs anywhere (cstest). */
    DWORD_PTR amask = mask;
    const int park = env_int("PS3_FAKE_PARK", 0);
    if (park)
        for (int c = 0; c < np; c++)
            for (int k = 1; k < nc[c].nlp; k++) amask &= ~((DWORD_PTR)1 << s_lp[nc[c].lp[k]].idx);
    if (!SetProcessAffinityMask(GetCurrentProcess(), amask)) {
        fprintf(stderr, "[pin] PS3_FAKE_HYBRID=%s: SetProcessAffinityMask failed (%lu), ignored\n", spec, GetLastError());
        return 0;
    }
    if (park)
        fprintf(stderr, "[pin] PS3_FAKE_PARK=1 (TEST ONLY): the P-cores' second hardware threads are parked: process "
                        "affinity 0x%llX (the topology keeps them)\n", (unsigned long long)amask);
    for (int l = 0; l < s_nlp; l++) if (!used[l]) { s_lp[l].unusable = 1; s_lp[l].core = 0; }
    for (int c = 0; c < n; c++) for (int k = 0; k < nc[c].nlp; k++) s_lp[nc[c].lp[k]].core = c;
    memcpy(s_core, nc, (size_t)n * sizeof s_core[0]);
    s_ncore = n;
    s_fake_cores = n;
    fprintf(stderr, "[pin] PS3_FAKE_HYBRID=%s (TEST ONLY): %d P-cores = real cores 0-%d, %d E-cores = one logical "
                    "processor each of real cores %d-%d; process limited to them (mask 0x%llX)\n",
            spec, np, np - 1, ne, efirst, efirst + (ne + 1) / 2 - 1, (unsigned long long)mask);
    return 1;
}

static int hp_cpu_intel(void)
{
    int r[4];
    __cpuid(r, 0);
    return r[1] == 0x756E6547 && r[3] == 0x49656E69 && r[2] == 0x6C65746E;   /* "GenuineIntel" */
}

/* More than one EfficiencyClass among the cores the process may use: hybrid. "May use" = not held by another
 * process's CPU set AND inside the process affinity mask (skeptic, NOTES.md 10/11: 0.1.23's rules count only the
 * cores in the mask, hp_process_cores; an 8P+8E player who limited the game to its P-cores with Task Manager or
 * Process Lasso before it started gets a uniform 8-core CPU, as in 0.1.23, not a hybrid plan with its light threads
 * on E-cores outside the mask). Not with PS3_FAKE_HYBRID (it made its own mask, marked the rest unusable, and
 * PS3_FAKE_PARK's parked siblings are outside it on purpose) nor across processor groups (no Intel hybrid has more
 * than 64 logical processors). The processors outside the mask are marked unusable only when the hybrid path is
 * really taken (hp_hybrid_mask_apply), so 0.1.23's rules see the topology as 0.1.23 did. */
static DWORD_PTR s_hy_pm;   /* the process affinity mask the detection used; 0 = none */
static int hp_lp_in_mask(int l) { return !s_hy_pm || (s_lp[l].idx < 64 && ((s_hy_pm >> s_lp[l].idx) & 1)); }
static void hp_hybrid_detect(void)
{
    { DWORD_PTR pm = 0, sm = 0;
      int one_group = 1;
      for (int l = 0; l < s_nlp; l++) one_group &= s_lp[l].group == 0;
      s_hy_pm = (!s_fake_cores && one_group && GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) ? pm : 0; }
    int emax = -1, emin = 256;
    for (int c = 0; c < s_ncore; c++) {
        int usable = 0;
        for (int k = 0; k < s_core[c].nlp; k++) usable += !s_lp[s_core[c].lp[k]].unusable && hp_lp_in_mask(s_core[c].lp[k]);
        if (!usable) continue;
        if (s_core[c].eff > emax) emax = s_core[c].eff;
        if (s_core[c].eff < emin) emin = s_core[c].eff;
    }
    if (emax < 0 || emax == emin) return;
    s_hy_effmax = emax;
    for (int c = 0; c < s_ncore; c++) {
        int usable = 0;
        for (int k = 0; k < s_core[c].nlp; k++) usable += !s_lp[s_core[c].lp[k]].unusable && hp_lp_in_mask(s_core[c].lp[k]);
        if (!usable) continue;
        if (s_core[c].eff == emax) { s_hy_np++; s_hy_npl += usable; }
        else                        { s_hy_ne++; s_hy_nel += usable; }
    }
    s_hy_on = 1;
}
/* The hybrid path is taken: its plans (and the A/B's old side) use the processors inside the mask only. */
static void hp_hybrid_mask_apply(void)
{
    int n = 0;
    if (s_hy_pm)
        for (int l = 0; l < s_nlp; l++)
            if (!s_lp[l].unusable && !hp_lp_in_mask(l)) { s_lp[l].unusable = 1; n++; }
    if (n && s_log)
        fprintf(stderr, "[pin] hybrid: %d logical processor(s) outside the process affinity mask 0x%llX left out\n", n,
                (unsigned long long)s_hy_pm);
}

/* ------------------------------------------------------------------- plan */

static void set_add(hp_set_t* s, int lp)
{
    if (lp < 0 || lp >= s_nlp) return;
    for (int i = 0; i < s->n; i++) if (s->lp[i] == lp) return;
    if (s->n < HP_MAX_LP) s->lp[s->n++] = (short)lp;
}

/* PS3_CPU_LIMIT: plan on the last n cores only, the ones the process may use;
 * the others' hardware threads are never handed out. */
static void hp_keep_last_cores(int n)
{
    const int first = s_ncore - n;
    for (int c = 0; c < s_ncore; c++)
        for (int k = 0; k < s_core[c].nlp; k++) {
            hp_lp_t* l = &s_lp[s_core[c].lp[k]];
            if (c < first) l->unusable = 1;
            else l->core = c - first;
        }
    memmove(s_core, s_core + first, (size_t)n * sizeof s_core[0]);
    s_ncore = n;
}

/* PS3_CPU_FIRST (TEST ONLY): plan on the first n cores only (logical processor 0 included). */
static void hp_keep_first_cores(int n)
{
    for (int c = n; c < s_ncore; c++)
        for (int k = 0; k < s_core[c].nlp; k++) s_lp[s_core[c].lp[k]].unusable = 1;
    s_ncore = n;
}

static int core_primary(int c) { return s_core[c].lp[0]; }
static int core_sibling(int c) { return s_core[c].nlp > 1 ? s_core[c].lp[1] : -1; }

/* Cores best-first: bigger L3, then higher SchedulingClass, then the order
 * Windows lists them; the core with logical processor 0 last (interrupts). */
static void hp_core_order(int* order)
{
    for (int i = 0; i < s_ncore; i++) order[i] = i;
    for (int i = 1; i < s_ncore; i++) {
        const int v = order[i];
        int j = i - 1;
        for (; j >= 0; j--) {
            const hp_core_t* a = &s_core[order[j]];
            const hp_core_t* b = &s_core[v];
            int a_before_b;
            if (a->has_lp0 != b->has_lp0)      a_before_b = !a->has_lp0;
            else if (a->eff != b->eff)         a_before_b = a->eff > b->eff;      /* P-cores first */
            else if (a->l3_size != b->l3_size) a_before_b = a->l3_size > b->l3_size;
            else if (a->sched != b->sched)     a_before_b = a->sched > b->sched;
            else                               a_before_b = order[j] < v;
            if (a_before_b) break;
            order[j + 1] = order[j];
        }
        order[j + 1] = v;
    }
}

/* PS3_PIN_MAP=walker=7;ppu=0;kernel=0s;spu=1,2,3,4,5,6 */
static int hp_parse_core_ref(const char** p)
{
    char* end;
    const long c = strtol(*p, &end, 10);
    if (end == *p || c < 0 || c >= s_ncore) return -1;
    int lp = core_primary((int)c);
    if (*end == 's') { lp = core_sibling((int)c); end++; }
    *p = end;
    return lp;
}

static int hp_plan_from_map(const char* m)
{
    const char* p = m;
    while (*p) {
        while (*p == ';' || *p == ' ') p++;
        if (!*p) break;
        hp_set_t* single = NULL;
        int spu = 0;
        if      (!strncmp(p, "walker=", 7)) { single = &s_set_walker; p += 7; }
        else if (!strncmp(p, "ppu=", 4))    { single = &s_set_ppu;    p += 4; }
        else if (!strncmp(p, "kernel=", 7)) { single = &s_set_kernel; p += 7; }
        else if (!strncmp(p, "spu=", 4))    { spu = 1;                 p += 4; }
        else return 0;
        if (single) { const int lp = hp_parse_core_ref(&p); if (lp < 0) return 0; set_add(single, lp); }
        else {
            for (int i = 0; spu && i < HP_NSPU; i++) {
                const int lp = hp_parse_core_ref(&p); if (lp < 0) return 0;
                set_add(&s_set_spu[i], lp);
                if (*p != ',') break;
                p++;
            }
        }
    }
    return 1;
}

/* Two-CCD X3D CPUs (7950X3D, 9950X3D: one CCD with the 3D V-Cache, 96 MB L3, one without, 32 MB): today's 10+-core
 * plan puts the walker and the six SPU workers on the V-cache CCD's seven cores without logical processor 0 and the
 * PPU main thread on the other CCD. Returns the core with logical processor 0 when (a) the cores carry two or more L3
 * sizes, (b) that core is usable and sits under the biggest L3, (c) order[0..6] (walker + SPUs) sit under that same L3
 * with the same EfficiencyClass and (d) there are 10+ cores; else -1 (today's plan). Unchanged: a one-CCD X3D
 * (9800X3D: 8 cores, placement off anyway), a 6+6 X3D (7900X3D / 9900X3D: (c) fails), CCDs with equal L3 (9950X3D2,
 * 9950X, 7950X), a process limited to the non-V-cache CCD (no lp-0 core), hybrid CPUs (their own plan) and
 * PS3_PIN_MAP. PS3_PIN_X3D=0 turns it off. host\x3d\NOTES.md, 2026-11-12. */
static int hp_vcache_lp0_core(const int* order)
{
    if (s_ncore < 10 || !env_int("PS3_PIN_X3D", 1)) return -1;
    DWORD lo = 0, hi = 0; int c0 = -1;
    for (int c = 0; c < s_ncore; c++) {
        const DWORD l3 = s_core[c].l3_size;
        if (!lo || l3 < lo) lo = l3;
        if (l3 > hi) hi = l3;
        if (s_core[c].has_lp0) c0 = c;
    }
    if (c0 < 0 || !lo || lo == hi || s_core[c0].l3_size != hi || s_lp[core_primary(c0)].unusable) return -1;
    for (int i = 0; i < 7; i++)
        if (s_core[order[i]].l3_id != s_core[c0].l3_id || s_core[order[i]].eff != s_core[c0].eff) return -1;
    return c0;
}

static int s_small_plan;   /* the 3-7-core plan was made (its light set includes the SPU set) */
static int s_small_ppu = 1, s_small_walker = 1;   /* PS3_PIN_SMALL_PPU / PS3_PIN_SMALL_WALKER (host_qos_init) */
static void hp_ab_spu_side(int on);   /* the SPU-side parity on / off (PS3_HOST_PIN_AB, PS3_PIN_SPU_PRIO_AB) */

static void hp_plan(void)
{
    int order[HP_MAX_CORE];
    hp_core_order(order);
    const int solo = env_int("PS3_PIN_WALKER_SOLO", 1);
    const char* map = getenv("PS3_PIN_MAP");
    int walker_core = -1;

    if (map && *map && hp_plan_from_map(map)) {
        if (s_set_walker.n) walker_core = s_lp[s_set_walker.lp[0]].core;
    } else if (s_ncore >= 8) {
        walker_core = order[0];
        set_add(&s_set_walker, core_primary(order[0]));
        const int vc_lp0 = hp_vcache_lp0_core(order);
        if (vc_lp0 >= 0) {
            /* Two-CCD X3D (hp_vcache_lp0_core): the PPU main thread joins the walker and the six SPU workers on the
             * V-cache CCD (order[1]); SPU 4 (no EDGE) takes that CCD's logical-processor-0 core instead of a core on
             * the other CCD; the kernel stays on the other CCD (order[8]). Measured on a 9950X3D, all 16 cores, cap
             * off, interleaved (host\x3d\NOTES.md, layout F vs today's A): heavy fight 111.6 vs 94.9 fps (+17%; 1% low
             * 80.6 vs 67.6), rooftop 69.5 vs 68.7 (tie). PS3_PIN_X3D=0: today's plan. */
            static const int k[6] = { 2, 3, 4, 5, -1, 6 };
            for (int i = 0; i < 6; i++) set_add(&s_set_spu[i], core_primary(k[i] < 0 ? vc_lp0 : order[k[i]]));
            set_add(&s_set_ppu, core_primary(order[1]));
        } else {
            for (int i = 0; i < 6; i++) set_add(&s_set_spu[i], core_primary(order[1 + i]));
            set_add(&s_set_ppu, core_primary(order[7]));
        }
        /* The kernel spins (SwitchToThread). Beside the PPU main thread it took
         * that thread's core, and the PPU carries ~20 ms of real work per heavy
         * frame: put it beside the last SPU worker instead. Measured on the
         * 7840HS (EXPERIMENTS rows 26-27): 30.7 vs 27.3 fps in heavy fights.
         * PS3_PIN_KERNEL_PPU=1 restores the old place. */
        const int kernel_by_ppu = env_int("PS3_PIN_KERNEL_PPU", 0);
        if (s_ncore >= 10)                 set_add(&s_set_kernel, core_primary(order[8]));
        else if (!kernel_by_ppu && core_sibling(order[6]) >= 0) set_add(&s_set_kernel, core_sibling(order[6]));
        else if (core_sibling(order[7]) >= 0) set_add(&s_set_kernel, core_sibling(order[7]));
        else                               set_add(&s_set_kernel, core_primary(order[7]));
    } else if (s_ncore >= 3 && env_int("PS3_PIN_SMALL_OLD", 0)) {
        /* The first 6-core plan, for A/B: as below, but the kernel on the PPU's
         * sibling and the light threads on the walker's sibling ALONE -- on a
         * 6-core CPU every guest thread, the audio mixer, the PSO builders and
         * the GPU driver's threads shared one logical processor (a tester's
         * i5 6/12, 2026-11-01: the jump-park freeze in 2 runs of 2). */
        const int ppu_core = order[s_ncore - 1];
        walker_core = order[0];
        set_add(&s_set_walker, core_primary(walker_core));
        set_add(&s_set_ppu, core_primary(ppu_core));
        set_add(&s_set_kernel, core_sibling(ppu_core) >= 0 ? core_sibling(ppu_core) : core_primary(ppu_core));
        for (int i = 0; i < 6; i++)
            for (int k = 1; k < s_ncore - 1; k++)
                for (int t = 0; t < s_core[order[k]].nlp; t++) set_add(&s_set_spu[i], s_core[order[k]].lp[t]);
    } else if (s_ncore >= 3) {
        /* 3-7 cores (6-core class): the walker the best core and the PPU main
         * thread a hardware thread; the six SPU workers and the SPURS kernel
         * share the other cores (every hardware thread: the kernel spins, and
         * beside the PPU it took that thread's core, rows 26-27); the light
         * threads may use every hardware thread but the walker's and the PPU's.
         * PS3_PIN_SMALL_PPU=1 (default; 0 = before 2026-11-09): when the core
         * with logical processor 0 (where most interrupts and DPCs land) is one
         * of ours, the PPU takes the next core and the lp-0 core joins the SPU
         * set, whose six workers can move around a busy processor; at 6 cores
         * the PPU main thread runs ~99% of the time and carries the opening
         * (6 cores of a 9950X3D with lp 0, in-run A/B: the PPU READY 1.8% of the
         * time on lp 0 vs 0.1% off it; with 100 ms priority-15 bursts on lp 0
         * every 2 s, 23.6 vs 0 frames of 100 ms or more per minute).
         * PS3_PIN_SMALL_WALKER=1 (default; 0 = before): the walker may use both
         * hardware threads of its core and the light threads leave its sibling,
         * so a burst of another program's or the driver's work on one of them
         * no longer holds it off the CPU (same bursts on the walker's processor:
         * 32.7 vs 0 frames of 100 ms or more per minute; without bursts the
         * walker is 13% faster per draw with its core to itself, 2026-11-09). */
        int ppu_core = order[s_ncore - 1];
        if (s_small_ppu && s_ncore >= 4 && s_core[ppu_core].has_lp0) ppu_core = order[s_ncore - 2];
        walker_core = order[0];
        const int wsib = s_small_walker ? core_sibling(walker_core) : -1;
        set_add(&s_set_walker, core_primary(walker_core));
        if (wsib >= 0) set_add(&s_set_walker, wsib);
        set_add(&s_set_ppu, core_primary(ppu_core));
        for (int k = 1; k < s_ncore; k++) {
            const int c = order[k];
            if (c == ppu_core) continue;
            for (int t = 0; t < s_core[c].nlp; t++) {
                const int lp = s_core[c].lp[t];
                for (int i = 0; i < 6; i++) set_add(&s_set_spu[i], lp);
                set_add(&s_set_kernel, lp);
            }
        }
        for (int l = 0; l < s_nlp; l++)
            if (!s_lp[l].unusable && l != core_primary(walker_core) && l != wsib && l != core_primary(ppu_core))
                set_add(&s_set_light, l);
        s_small_plan = 1;   /* light threads share the SPU set: hp_role_prio's parity */
    } else {
        return;   /* 1-2 cores: nothing sensible to pin */
    }

    /* Hot slots per logical processor, for host_pin_self_exclusive(). */
    hp_set_t* hot[3 + HP_NSPU] = { &s_set_walker, &s_set_ppu, &s_set_kernel };
    for (int i = 0; i < HP_NSPU; i++) hot[3 + i] = &s_set_spu[i];
    for (int h = 0; h < 3 + HP_NSPU; h++)
        for (int i = 0; i < hot[h]->n; i++) s_lp_hot[hot[h]->lp[i]]++;

    /* Light: every logical processor no hot thread holds, minus the walker's
     * sibling when it runs solo (8+ cores only: on 6 the core is too dear). */
    const int walker_sib = (walker_core >= 0 && solo && s_ncore >= 8) ? core_sibling(walker_core) : -1;
    if (!s_set_light.n)   /* (the small-CPU plan has made its own) */
        for (int l = 0; l < s_nlp; l++)
            if (!s_lp_hot[l] && l != walker_sib && !s_lp[l].unusable) set_add(&s_set_light, l);
    /* No SMT and no spare core: the light threads share the SPU cores, not
     * the walker's or the PPU's. */
    if (!s_set_light.n)
        for (int i = 0; i < HP_NSPU; i++)
            for (int k = 0; k < s_set_spu[i].n; k++) set_add(&s_set_light, s_set_spu[i].lp[k]);
    for (int i = 0; i < s_set_light.n; i++) s_lp_light[s_set_light.lp[i]] = 1;
}

static void set_add_core(hp_set_t* s, int c)
{
    for (int k = 0; k < s_core[c].nlp; k++)
        if (!s_lp[s_core[c].lp[k]].unusable) set_add(s, s_core[c].lp[k]);
}

/* The hybrid plan (header). P-cores in the order above (the lp0 core last). */
static void hp_plan_hybrid(void)
{
    int order[HP_MAX_CORE], P[HP_MAX_CORE], np = 0;
    hp_core_order(order);
    for (int i = 0; i < s_ncore; i++) {
        const int c = order[i];
        int usable = 0;   /* (a core whose threads another process holds: skipped) */
        for (int k = 0; k < s_core[c].nlp; k++) usable += !s_lp[s_core[c].lp[k]].unusable;
        if (!usable) continue;
        if (s_core[c].eff == s_hy_effmax) P[np++] = c;
        else { set_add_core(&s_hy.light, c); s_hy.ec[s_hy.ne++] = c; }
        set_add_core(&s_hy.all, c);
    }
    if (!np) return;
    hp_set_t w;
    memset(&w, 0, sizeof w);
    set_add_core(&w, P[0]);
    s_hy.wcore = w; s_hy.np = np;
    memcpy(s_hy.pc, P, (size_t)np * sizeof P[0]);
    for (int i = 0; i < np; i++) { set_add_core(&s_hy.pall, P[i]); if (i) set_add_core(&s_hy.prest, P[i]); }
    /* The walker alone on its P-core when the other P-cores keep a CORE for
     * each of the PPU main thread and the six SPU workers. Cores, not logical
     * processors (skeptic, NOTES.md 10/11): Windows may park the P-cores'
     * second hardware threads (SHADE's 4P+8E laptop, NOTES.md 9.1), and a soft
     * set then runs on the first ones only. */
    const int solo = env_int("PS3_HYBRID_SOLO", -1);
    s_hy.walker_solo = np >= 2 && (solo >= 0 ? solo != 0 : np - 1 >= 7);
    for (int i = s_hy.walker_solo ? 1 : 0; i < np; i++) set_add_core(&s_hy.pool, P[i]);
    s_hy.walker = s_hy.walker_solo ? w : s_hy.pool;
    /* The kernel: in the pool, unless it spins (SPURS_KERNEL_SPIN=1, or =auto
     * with 8+ P-cores) and the pool has fewer cores than the threads that
     * would share it -- a spinning thread there would take a first hardware
     * thread the workers need; then the E-cores, with the light threads. */
    const char* ks = getenv("SPURS_KERNEL_SPIN");
    s_hy.kernel_spin = (ks && !strcmp(ks, "auto")) ? !s_hy_small : (ks && *ks == '1');
    const int sharing = 8 + (s_hy.walker_solo ? 0 : 1);   /* PPU + 6 SPU + kernel (+ walker) */
    const int pool_cores = np - (s_hy.walker_solo ? 1 : 0);
    const char* km = getenv("PS3_HYBRID_KERNEL");
    s_hy.kernel_e = s_hy.light.n && ((km && *km) ? (*km == 'e' || *km == 'E')
                                                 : (s_hy.kernel_spin && pool_cores < sharing));
    s_hy.kernel = s_hy.kernel_e ? s_hy.light : s_hy.pool;
    const hp_set_t* hot[4] = { &s_hy.walker, &s_hy.kernel, &s_hy.pool /* PPU */, &s_hy.pool /* SPUs */ };
    for (int h = 0; h < 4; h++)
        for (int i = 0; i < hot[h]->n; i++) s_hy.lp_hot[hot[h]->lp[i]] += (unsigned char)(h == 3 ? 6 : 1);
    for (int i = 0; i < s_hy.light.n; i++) s_hy.lp_light[s_hy.light.lp[i]] = 1;
}

/* Hot threads in the order own logical processors are handed out (layout roles: walker 0, PPU main 1, kernel 2,
 * SPU 0-5 = 3-8): walker, PPU main, SPU 5 (the sound job), SPU 0-4, kernel. */
static const int hp_lay_prio[HP_NLAY] = { 0, 1, 8, 3, 4, 5, 6, 7, 2 };

/* A layout's letters (hp_layout_t): a name, or 9 letters of w p e q a x y f + an optional light letter e l a. */
static int hp_layout_spec(const char* name, char* spec)
{
    if (!strcmp(name, "pool")) {
        snprintf(spec, HP_NLAY + 1, "%cp%cpppppp", s_hy.walker_solo ? 'w' : 'p', s_hy.kernel_e ? 'e' : 'p');
        return 1;
    }
    if (!strcmp(name, "spill")) { strcpy(spec, "wpqqqqqqp"); return 1; }
    if (!strcmp(name, "split")) { strcpy(spec, "wppppeeep"); return 1; }
    /* prim: x for walker, PPU main, SPU 5, SPU 0-4 while P-cores other than the lp0 core last; olds: 0.1.23's
     * order (walker, SPU 0-5, PPU main) with SPU 5 (the sound job) moved to the front: walker, SPU 5, SPU 1-4,
     * SPU 0, PPU main. The rest y (the kernel always). */
    static const int prim_x[HP_NLAY - 1] = { 0, 1, 8, 3, 4, 5, 6, 7 };
    static const int olds_x[HP_NLAY - 1] = { 0, 8, 4, 5, 6, 7, 3, 1 };
    /* olds0: walker, SPU 5, SPU 0-4, PPU main over EVERY P-core (the lp0 core's first thread too, handed out last) */
    static const int olds0_x[HP_NLAY - 1] = { 0, 8, 3, 4, 5, 6, 7, 1 };
    const int lp0_too = !strcmp(name, "olds0");
    const int* xo = !strcmp(name, "prim") ? prim_x : !strcmp(name, "olds") ? olds_x : lp0_too ? olds0_x : NULL;
    if (xo) {
        int nfree = 0, ny = 0;
        for (int j = 0; j < s_hy.np; j++) nfree += lp0_too || !s_core[s_hy.pc[j]].has_lp0;
        memset(spec, 'y', HP_NLAY);
        for (int i = 0; i < HP_NLAY - 1 && i < nfree; i++) spec[xo[i]] = 'x';   /* the kernel: never */
        for (int r = 0; r < HP_NLAY; r++) ny += spec[r] == 'y';
        if (ny > s_hy.ne) for (int r = 0; r < HP_NLAY; r++) if (spec[r] == 'y') spec[r] = 'f';
        spec[HP_NLAY] = 'l'; spec[HP_NLAY + 1] = 0;
        return 1;
    }
    const size_t n = strlen(name);
    if ((n != HP_NLAY && n != HP_NLAY + 1) || strspn(name, "wpeqaxyf") < HP_NLAY) return 0;
    if (n == HP_NLAY + 1 && !strchr("ela", name[HP_NLAY])) return 0;
    strcpy(spec, name);
    return 1;
}

static int core_first_usable(int c)
{
    for (int k = 0; k < s_core[c].nlp; k++) if (!s_lp[s_core[c].lp[k]].unusable) return s_core[c].lp[k];
    return -1;
}

static int hp_layout_build(hp_layout_t* L, const char* name)
{
    memset(L, 0, sizeof *L);
    L->kspin = -1;
    char base[16];
    snprintf(base, sizeof base, "%s", name);
    const size_t bl = strlen(base);
    if (bl > 2 && base[bl - 1] == 'k' && (base[bl - 2] == '+' || base[bl - 2] == '-')) {
        L->kspin = base[bl - 2] == '+';
        base[bl - 2] = 0;
    }
    if (!hp_layout_spec(base, L->spec)) return 0;
    snprintf(L->name, sizeof L->name, "%s", name);
    /* w needs a second P-core for the pool; with one P-core it means p */
    int has_w = 0;
    for (int r = 0; r < HP_NLAY; r++) has_w |= L->spec[r] == 'w';
    has_w = has_w && s_hy.np >= 2;
    L->walker_solo = has_w && L->spec[0] == 'w';
    /* own logical processors: x before y, in hp_lay_prio's order */
    unsigned char pu[HP_MAX_CORE], eu[HP_MAX_CORE];
    memset(pu, 0, sizeof pu); memset(eu, 0, sizeof eu);
    if (has_w) pu[0] = 1;                 /* wcore = the first P-core */
    int own[HP_NLAY];
    for (int r = 0; r < HP_NLAY; r++) own[r] = -1;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < HP_NLAY; i++) {
            const int r = hp_lay_prio[i];
            if (L->spec[r] != (pass ? 'y' : 'x')) continue;
            int lp = -1;
            if (!pass) {
                for (int j = 0; j < s_hy.np && lp < 0; j++)   /* the lp0 core last */
                    if (!pu[j] && !s_core[s_hy.pc[j]].has_lp0) { pu[j] = 1; lp = core_first_usable(s_hy.pc[j]); }
                for (int j = 0; j < s_hy.np && lp < 0; j++)
                    if (!pu[j]) { pu[j] = 1; lp = core_first_usable(s_hy.pc[j]); }
            }
            for (int j = 0; j < s_hy.ne && lp < 0; j++)
                if (!eu[j]) { eu[j] = 1; lp = core_first_usable(s_hy.ec[j]); }
            own[r] = lp;
        }
    /* p: the P-cores nobody owns (both threads); f: their first threads + the E-cores nobody owns */
    hp_set_t fset;
    memset(&fset, 0, sizeof fset);
    for (int j = 0; j < s_hy.np; j++)
        if (!pu[j]) { set_add_core(&L->p, s_hy.pc[j]); set_add(&fset, core_first_usable(s_hy.pc[j])); }
    for (int j = 0; j < s_hy.ne; j++) if (!eu[j]) set_add(&fset, core_first_usable(s_hy.ec[j]));
    if (!L->p.n) L->p = s_hy.pall;
    if (!fset.n) fset = s_hy.light.n ? s_hy.light : s_hy.pall;
    for (int r = 0; r < HP_NLAY; r++) {
        hp_set_t* s = &L->set[r];
        switch (L->spec[r]) {
        case 'w': *s = has_w ? s_hy.wcore : s_hy.pall; break;
        case 'p': *s = L->p; break;
        case 'e': *s = s_hy.light.n ? s_hy.light : L->p; break;
        case 'q': *s = L->p; for (int i = 0; i < s_hy.light.n; i++) set_add(s, s_hy.light.lp[i]); break;
        case 'x': case 'y': if (own[r] >= 0) set_add(s, own[r]); else *s = fset; break;
        case 'f': *s = fset; break;
        default:  *s = s_hy.all; break;
        }
        for (int i = 0; i < s->n; i++) L->lp_hot[s->lp[i]]++;
    }
    /* a kernel with a logical processor of its own runs its auto yield loop (0.1.23: on from 8 cores, the kernel
     * on a core of its own from 10); +k / -k decide otherwise */
    if (L->kspin < 0 && (L->spec[2] == 'x' || L->spec[2] == 'y') && own[2] >= 0) L->kspin = 1;
    /* the light threads (the process default on this side) */
    const char lc = L->spec[HP_NLAY] ? L->spec[HP_NLAY] : 'e';
    if (lc == 'a') L->light = s_hy.all;
    else if (lc == 'l') {
        unsigned char no[HP_MAX_LP];
        memset(no, 0, sizeof no);
        for (int r = 0; r < HP_NLAY; r++) if (own[r] >= 0) no[own[r]] = 1;
        if (has_w) for (int i = 0; i < s_hy.wcore.n; i++) no[s_hy.wcore.lp[i]] = 1;
        if (L->spec[0] == 'x' && own[0] >= 0) {
            const int wc = s_lp[own[0]].core;
            for (int k = 0; k < s_core[wc].nlp; k++) no[s_core[wc].lp[k]] = 1;
        }
        for (int i = 0; i < s_hy.all.n; i++) if (!no[s_hy.all.lp[i]]) set_add(&L->light, s_hy.all.lp[i]);
        if (!L->light.n) L->light = s_hy.light.n ? s_hy.light : s_hy.all;
    } else L->light = s_hy.light.n ? s_hy.light : s_hy.all;
    for (int i = 0; i < L->light.n; i++) L->lp_light[L->light.lp[i]] = 1;
    return 1;
}

/* The hybrid side's layout (opt-in): PS3_HYBRID_PLAN (a name or 9-10 letters), else "pool" with 8+ P-cores and
 * "olds0" below (NOTES.md section 9: with SHADE's parked P-core siblings the 4P pool collapsed -- 25.5 vs 40.3 fps on
 * his 4P+8E laptop, 9-10 vs 21-30 in the parked simulation -- while one logical processor per hot thread, as 0.1.23
 * does, is immune to parking; with 8 P-cores the pool keeps a core for each of its threads once the kernel is on
 * the E-cores, and measured +16% / +11% / a tie against 0.1.23 in three parked simulation runs). */
static const char* hp_layout_default(void)
{
    const char* e = getenv("PS3_HYBRID_PLAN");
    if (e && *e) return e;
    return s_hy_np >= 8 ? "pool" : "olds0";
}

/* The hybrid plan by default: on NO CPU in 0.1.24 (skeptic + SHADE's real A/B, NOTES.md 10/11). The only real
 * hybrid A/B (4P+8E) went against the plan it tested, and the 8-P-core gain is a simulation result that did not
 * repeat in every run. Every hybrid CPU gets 0.1.23's behaviour (as PS3_HYBRID=0) unless PS3_HOST_PIN_AB or
 * PS3_HYBRID_PLAN asks for the hybrid side. To make a class the default, a real tester's PS3_HOST_PIN_AB=30000 log
 * of that class must show (host\hybrid\MERGE-0124.md): fps median >= old, frames >= 50 AND >= 100 ms per minute
 * and [hitch] >= 100 ms no higher, concealment and lead <= 1 no worse, no freeze sign, in two sessions. */
static int hp_hybrid_default_on(void)
{
    return 0;
}

/* -------------------------------------------------------------- applying */

static int set_to_cpusets(const hp_set_t* s, ULONG* ids)
{
    int n = 0;
    for (int i = 0; i < s->n; i++) if (s_lp[s->lp[i]].cpuset) ids[n++] = s_lp[s->lp[i]].cpuset;
    return n;
}

static BOOL apply_thread(HANDLE h, const hp_set_t* s, int on)
{
    if (s_hard) {
        GROUP_AFFINITY ga;
        memset(&ga, 0, sizeof ga);
        if (on && s->n) {
            ga.Group = s_lp[s->lp[0]].group;
            for (int i = 0; i < s->n; i++)
                if (s_lp[s->lp[i]].group == ga.Group) ga.Mask |= (KAFFINITY)1 << s_lp[s->lp[i]].idx;
        } else {
            /* off: every logical processor of the thread's current group */
            GROUP_AFFINITY cur;
            if (!GetThreadGroupAffinity(h, &cur)) return FALSE;
            ga.Group = cur.Group;
            for (int l = 0; l < s_nlp; l++)
                if (s_lp[l].group == ga.Group) ga.Mask |= (KAFFINITY)1 << s_lp[l].idx;
        }
        return SetThreadGroupAffinity(h, &ga, NULL);
    }
    ULONG ids[HP_MAX_LP];
    const int n = on ? set_to_cpusets(s, ids) : 0;
    return SetThreadSelectedCpuSets(h, n ? ids : NULL, (ULONG)n);
}

static void apply_process_default(int on)
{
    ULONG ids[HP_MAX_LP];
    const int n = on ? set_to_cpusets(&s_set_light, ids) : 0;
    SetProcessDefaultCpuSets(GetCurrentProcess(), n ? ids : NULL, (ULONG)n);
}

static const hp_set_t* role_set(enum host_role role, int index)
{
    switch (role) {
    case HOST_ROLE_PPU_MAIN:     return s_pin_ppu    ? &s_set_ppu    : NULL;
    case HOST_ROLE_WALKER:       return s_pin_walker ? &s_set_walker : NULL;
    case HOST_ROLE_SPURS_KERNEL: return s_pin_kernel ? &s_set_kernel : NULL;
    case HOST_ROLE_SPU:
        if (!s_pin_spu || index < 0 || index >= HP_NSPU || !s_set_spu[index].n) return NULL;
        return &s_set_spu[index];
    default: return NULL;   /* light / audio: the process default */
    }
}

/* A hybrid layout's set for a role. A hot role whose switch is off floats on
 * every core (not the process default: that is the E-cores). */
static const hp_set_t* role_set_hy(const hp_layout_t* L, enum host_role role, int index)
{
    switch (role) {
    case HOST_ROLE_WALKER:       return s_pin_walker ? &L->set[0] : &s_hy.all;
    case HOST_ROLE_PPU_MAIN:     return s_pin_ppu    ? &L->set[1] : &s_hy.all;
    case HOST_ROLE_SPURS_KERNEL: return s_pin_kernel ? &L->set[2] : &s_hy.all;
    case HOST_ROLE_SPU:
        if (index < 0 || index >= HP_NSPU) return NULL;
        return s_pin_spu ? (index < 6 ? &L->set[3 + index] : &L->p) : &s_hy.all;
    default: return NULL;
    }
}

/* A role's set on side k of the hybrid A/B (NULL: no placement). */
static const hp_set_t* hp_side_set(enum host_role role, int index, int k)
{
    if (!s_side_old[k]) return s_hy_plan_on ? role_set_hy(&s_lay[k], role, index) : NULL;
    return s_old_on ? role_set(role, index) : NULL;
}

static void apply_default_set(const hp_set_t* s)
{
    ULONG ids[HP_MAX_LP];
    const int n = s ? set_to_cpusets(s, ids) : 0;
    SetProcessDefaultCpuSets(GetCurrentProcess(), n ? ids : NULL, (ULONG)n);
}

static void print_set(char* out, size_t cap, const hp_set_t* s)
{
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < s->n && o + 12 < cap; i++) {
        const hp_lp_t* l = &s_lp[s->lp[i]];
        o += (size_t)snprintf(out + o, cap - o, "%s%u:%u", i ? "," : "", (unsigned)l->group, (unsigned)l->idx);
    }
}

static void hp_log_cores(void)
{
    int smt = 1;
    for (int c = 0; c < s_ncore; c++) if (s_core[c].nlp > smt) smt = s_core[c].nlp;
    fprintf(stderr, "[pin] %d cores, %d logical, SMT %d, %s\n", s_ncore, s_nlp, smt,
            s_hard ? "hard affinity" : "CPU sets");
    for (int c = 0; c < s_ncore; c++)
        fprintf(stderr, "[pin]   core %d: lp %u:%u%s%s L3#%d %lu KB sched=%u eff=%u%s\n", c,
                (unsigned)s_lp[s_core[c].lp[0]].group, (unsigned)s_lp[s_core[c].lp[0]].idx,
                s_core[c].nlp > 1 ? "+sib" : "", s_core[c].has_lp0 ? " (lp0)" : "",
                s_core[c].l3_id, (unsigned long)(s_core[c].l3_size / 1024), (unsigned)s_core[c].sched,
                (unsigned)s_core[c].eff, s_lp[s_core[c].lp[0]].unusable ? " UNUSABLE" : "");
}

/* The old plan's role lines; pfx "" = exactly the lines before the hybrid work. */
static void hp_log_roles(const char* pfx)
{
    char b[1024];
    print_set(b, sizeof b, &s_set_walker); fprintf(stderr, "[pin] %swalker %s%s\n", pfx, b, s_pin_walker ? "" : " (off)");
    print_set(b, sizeof b, &s_set_ppu);    fprintf(stderr, "[pin] %sppu    %s%s\n", pfx, b, s_pin_ppu ? "" : " (off)");
    print_set(b, sizeof b, &s_set_kernel); fprintf(stderr, "[pin] %skernel %s%s\n", pfx, b, s_pin_kernel ? "" : " (off)");
    for (int i = 0; i < 6; i++) {
        print_set(b, sizeof b, &s_set_spu[i]); fprintf(stderr, "[pin] %sspu%d   %s%s\n", pfx, i, b, s_pin_spu ? "" : " (off)");
    }
    print_set(b, sizeof b, &s_set_light);  fprintf(stderr, "[pin] %slight  %s (process default)\n", pfx, b);
}

static void hp_log_plan(void)
{
    if (!s_log) return;
    hp_log_cores();
    hp_log_roles("");
}

/* Which E-core / P-core numbers (as in the core table) a set covers, e.g. "P2,P3,P0". */
static void print_cores(char* out, size_t cap, const hp_set_t* s)
{
    size_t o = 0;
    out[0] = 0;
    for (int c = 0; c < s_ncore && o + 12 < cap; c++)
        for (int k = 0; k < s_core[c].nlp; k++) {
            int in = 0;
            for (int i = 0; i < s->n && !in; i++) in = s->lp[i] == s_core[c].lp[k];
            if (in) {
                o += (size_t)snprintf(out + o, cap - o, "%s%c%d", o ? "," : "", s_core[c].eff == s_hy_effmax ? 'P' : 'E', c);
                break;
            }
        }
}

/* One layout, a line per hot thread: "[pin] hybrid[spill] spu2   -> P pool + E-cores P1,..,E4,.. (lp ...)". */
static void hp_log_layout(const hp_layout_t* L)
{
    static const char* const rn[HP_NLAY] = { "walker", "ppu", "kernel", "spu0", "spu1", "spu2", "spu3", "spu4", "spu5" };
    char b[1024], c[512];
    const int has_w = L->walker_solo || (strchr(L->spec, 'w') && s_hy.np >= 2);
    for (int r = 0; r < HP_NLAY; r++) {
        const char* kind;
        const int one = L->set[r].n == 1;
        const int on_p = one && s_core[s_lp[L->set[r].lp[0]].core].eff == s_hy_effmax;
        switch (L->spec[r]) {
        case 'w': kind = has_w ? "own P-core" : "P pool"; break;
        case 'p': kind = "P pool"; break;
        case 'e': kind = s_hy.light.n ? "E-cores" : "P pool"; break;
        case 'q': kind = "P pool + E-cores"; break;
        case 'x': case 'y': kind = !one ? "shared (none left)" : on_p ? "own P thread" : "own E-core"; break;
        case 'f': kind = "free P first threads + E-cores"; break;
        default:  kind = "every core"; break;
        }
        print_set(b, sizeof b, &L->set[r]); print_cores(c, sizeof c, &L->set[r]);
        fprintf(stderr, "[pin] hybrid[%s] %-6s -> %s %s (lp %s, %d logical)%s\n", L->name, rn[r], kind, c, b, L->set[r].n,
                r == 8 ? ", the sound job" : "");
    }
    print_set(b, sizeof b, &L->light); print_cores(c, sizeof c, &L->light);
    fprintf(stderr, "[pin] hybrid[%s] light  -> %s (lp %s, %d logical): process default from the guest's start "
                    "(every core until then, for LD_PSO_PRECOMPILE)\n", L->name, c, b, L->light.n);
    fprintf(stderr, "[pin] hybrid[%s] layout %s (walker, ppu, kernel, spu0-5[, light]: w own P-core, p P pool, e "
                    "E-cores, q P pool + E-cores, a every core, x own P thread, y own E-core, f free P threads + "
                    "E-cores; light e E-cores, l the rest, a every core)\n", L->name, L->spec);
}

static void hp_log_hybrid(void)
{
    if (!s_log) return;
    char b[1024], c[512];
    if (!s_hy_plan_on) return;
    if (s_ab_named || strcmp(s_lay[0].name, "pool")) {
        for (int k = 0; k < s_nside; k++) if (!s_side_old[k]) hp_log_layout(&s_lay[k]);
        return;
    }
    print_set(b, sizeof b, &s_hy.walker); print_cores(c, sizeof c, &s_hy.walker);
    fprintf(stderr, "[pin] hybrid walker -> %s (lp %s)%s%s\n", c, b,
            s_hy.walker_solo ? ": a P-core of its own, nothing else of ours on it" : ": the P pool, shared",
            s_pin_walker ? "" : " [PS3_PIN_WALKER=0: floats on every core]");
    print_set(b, sizeof b, &s_hy.pool); print_cores(c, sizeof c, &s_hy.pool);
    fprintf(stderr, "[pin] hybrid ppu    -> P pool %s (lp %s, %d logical, shared)%s\n", c, b, s_hy.pool.n,
            s_pin_ppu ? "" : " [PS3_PIN_PPU=0: floats on every core]");
    fprintf(stderr, "[pin] hybrid spu0-5 -> P pool (the same set, shared by all six; SPU 5 runs the sound job)%s\n",
            s_pin_spu ? "" : " [SPURS_PIN=0: float on every core]");
    print_set(b, sizeof b, &s_hy.kernel); print_cores(c, sizeof c, &s_hy.kernel);
    fprintf(stderr, "[pin] hybrid kernel -> %s %s (lp %s), yield loop %s%s\n", s_hy.kernel_e ? "E-cores" : "P pool", c, b,
            s_hy.kernel_spin ? "on" : "off", s_pin_kernel ? "" : " [PS3_PIN_KERNEL=0: floats on every core]");
    print_set(b, sizeof b, &s_hy.light); print_cores(c, sizeof c, &s_hy.light);
    fprintf(stderr, "[pin] hybrid light  -> E-cores %s (lp %s): process default from the guest's start (every core "
                    "until then, for LD_PSO_PRECOMPILE)\n", c, b);
}

/* PS3_HOST_PIN_AB=<ms>: the whole plan on, then off, then on... */
static DWORD WINAPI hp_ab_thread(LPVOID arg)
{
    const DWORD ms = (DWORD)(uintptr_t)arg;
    extern unsigned long long ps3_qpc_us(void);
    for (;;) {
        Sleep(ms);
        const LONG on = !InterlockedCompareExchange(&s_ab_on, 0, 0);
        InterlockedExchange(&s_ab_on, on);
        apply_process_default(on);
        AcquireSRWLockShared(&s_reg_lock);
        for (LONG i = 0; i < s_nreg; i++) {
            const hp_set_t* s = role_set(s_reg[i].role, s_reg[i].index);
            if (s) apply_thread(s_reg[i].h, s, on);
        }
        ReleaseSRWLockShared(&s_reg_lock);
        hp_ab_spu_side(on);   /* six: the 6-core plan's "off" = placement off by default (v0.1.23) */
        fprintf(stderr, "[pin-ab] %s t=%lluus\n", on ? "on" : "off", ps3_qpc_us());
    }
    return 0;
}

/* PS3_SPU_STALL_TEST=<after s>,<ms>[,<every s>] (testing; needs PS3_HOST_PIN=1,
 * which registers the threads): suspend all six SPU worker threads for <ms>,
 * as a starved CPU would stall them. A tester's 6-core PC froze for good when
 * an SPU job patched its FIFO park later than GCM_PARK_WAIT_MS (2026-11-01). */
static DWORD WINAPI hp_stall_thread(LPVOID arg)
{
    const char* e = (const char*)arg;
    unsigned after = 0, ms = 0, every = 0;
    if (sscanf(e, "%u,%u,%u", &after, &ms, &every) < 2 || !ms) return 0;
    extern unsigned long long ps3_qpc_us(void);
    Sleep(after * 1000u);
    for (;;) {
        HANDLE held[HP_NSPU]; int n = 0;
        AcquireSRWLockShared(&s_reg_lock);
        for (LONG i = 0; i < s_nreg && n < HP_NSPU; i++)
            if (s_reg[i].role == HOST_ROLE_SPU && SuspendThread(s_reg[i].h) != (DWORD)-1) held[n++] = s_reg[i].h;
        ReleaseSRWLockShared(&s_reg_lock);
        fprintf(stderr, "[stall-test] %d SPU thread(s) held for %u ms t=%lluus\n", n, ms, ps3_qpc_us());
        Sleep(ms);
        for (int i = 0; i < n; i++) ResumeThread(held[i]);
        fprintf(stderr, "[stall-test] released t=%lluus\n", ps3_qpc_us());
        if (!every) break;
        Sleep(every * 1000u);
    }
    return 0;
}

/* ---------------------------------------------------------- QoS / MMCSS */

static void hp_ecoqos_process(void)
{
    PROCESS_POWER_THROTTLING_STATE st;
    memset(&st, 0, sizeof st);
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    st.StateMask = 0;   /* controlled, and NOT throttled */
    const BOOL ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
    if (!ok) {   /* older Windows 10: the timer bit is unknown, retry without it */
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
    }
    if (s_log) fprintf(stderr, "[pin] EcoQoS opt-out (process)%s\n", ok ? "" : " without the timer bit");
}

static void hp_ecoqos_thread(void)
{
    THREAD_POWER_THROTTLING_STATE st;
    memset(&st, 0, sizeof st);
    st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &st, sizeof st);
}

/* The hybrid A/B's switches: opt out (1) or hand EcoQoS back to the system (0,
 * as when nothing was ever set). */
static void hp_ecoqos_process_set(int optout)
{
    PROCESS_POWER_THROTTLING_STATE st;
    memset(&st, 0, sizeof st);
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = optout ? (PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION) : 0;
    st.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st) && optout) {
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
    }
}

static void hp_ecoqos_thread_set(HANDLE h, int optout)
{
    THREAD_POWER_THROTTLING_STATE st;
    memset(&st, 0, sizeof st);
    st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = optout ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    st.StateMask = 0;
    SetThreadInformation(h, ThreadPowerThrottling, &st, sizeof st);
}

static int hp_role_hot(enum host_role r) { return r != HOST_ROLE_LIGHT && r != HOST_ROLE_AUDIO; }

/* Put one side of the hybrid A/B in force: the process default, every
 * registered thread's set, and (when it came from the hybrid default) EcoQoS.
 * The kernel's auto yield loop reads s_side itself (host_kernel_spin_auto). */
static void hp_apply_side(int k)
{
    const int hy = !s_side_old[k];
    AcquireSRWLockExclusive(&s_side_lock);
    InterlockedExchange(&s_side, k);
    if (InterlockedCompareExchange(&s_game_started, 0, 0))
        apply_default_set(hy ? (s_hy_plan_on ? &s_lay[k].light : NULL) : (s_old_on ? &s_set_light : NULL));
    if (s_hy_ecoqos) hp_ecoqos_process_set(hy);
    AcquireSRWLockShared(&s_reg_lock);
    for (LONG i = 0; i < s_nreg; i++) {
        const hp_set_t* s = hp_side_set(s_reg[i].role, s_reg[i].index, k);
        apply_thread(s_reg[i].h, s, s && s->n);
        if (s_hy_ecoqos && hp_role_hot(s_reg[i].role)) hp_ecoqos_thread_set(s_reg[i].h, hy);
    }
    ReleaseSRWLockShared(&s_reg_lock);
    ReleaseSRWLockExclusive(&s_side_lock);
}

/* The guest starts (the PPU main thread registers, after LD_PSO_PRECOMPILE):
 * the light threads move to the E-cores. */
static void hp_game_start(void)
{
    AcquireSRWLockExclusive(&s_side_lock);
    const LONG was = InterlockedExchange(&s_game_started, 1);
    const int k = (int)InterlockedCompareExchange(&s_side, 0, 0);
    if (!was && s_hy_plan_on && !s_side_old[k]) {
        apply_default_set(&s_lay[k].light);
        if (s_log) fprintf(stderr, "[pin] hybrid: the guest starts: light threads (process default) on %s now\n",
                           (s_lay[k].spec[HP_NLAY] == 'l' || s_lay[k].spec[HP_NLAY] == 'a') ? "the layout's light set"
                                                                                           : "the E-cores");
    }
    ReleaseSRWLockExclusive(&s_side_lock);
}

/* PS3_HOST_PIN_AB=<ms> on a hybrid: hybrid side, then today's behaviour, then
 * the hybrid side... every <ms> from the guest's start. */
static DWORD WINAPI hp_hab_thread(LPVOID arg)
{
    const DWORD ms = (DWORD)(uintptr_t)arg;
    extern unsigned long long ps3_qpc_us(void);
    while (!InterlockedCompareExchange(&s_game_started, 0, 0)) Sleep(50);
    if (!s_ab_named)
        fprintf(stderr, "[pin-ab] hybrid t=%lluus (the guest starts; hybrid and old flip every %lu ms)\n", ps3_qpc_us(),
                (unsigned long)ms);
    else
        fprintf(stderr, "[pin-ab] %s t=%lluus (the guest starts; %d sides in turn every %lu ms: %s, %s%s%s)\n",
                s_side_name[0], ps3_qpc_us(), s_nside, (unsigned long)ms, s_side_name[0], s_side_name[1],
                s_nside > 2 ? ", " : "", s_nside > 2 ? s_side_name[2] : "");
    for (;;) {
        Sleep(ms);
        const int k = (int)((InterlockedCompareExchange(&s_side, 0, 0) + 1) % s_nside);
        hp_apply_side(k);
        fprintf(stderr, "[pin-ab] %s t=%lluus\n", s_side_name[k], ps3_qpc_us());
    }
    return 0;
}

typedef HANDLE (WINAPI *pfn_AvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
typedef BOOL   (WINAPI *pfn_AvSetMmThreadPriority)(HANDLE, int);

static void hp_mmcss(const wchar_t* task, int high)
{
    static HMODULE s_avrt;
    static pfn_AvSetMmThreadCharacteristicsW s_set;
    static pfn_AvSetMmThreadPriority s_prio;
    if (!s_avrt) {
        s_avrt = LoadLibraryExW(L"avrt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!s_avrt) return;
        s_set  = (pfn_AvSetMmThreadCharacteristicsW)(void*)GetProcAddress(s_avrt, "AvSetMmThreadCharacteristicsW");
        s_prio = (pfn_AvSetMmThreadPriority)(void*)GetProcAddress(s_avrt, "AvSetMmThreadPriority");
    }
    if (!s_set) return;
    DWORD idx = 0;
    HANDLE h = s_set(task, &idx);   /* kept for the thread's life; never reverted */
    if (h && high && s_prio) s_prio(h, 1 /* AVRT_PRIORITY_HIGH */);
    if (s_log) fprintf(stderr, "[pin] MMCSS \"%ls\"%s for tid %lu: %s\n", task, high ? " high" : "",
                       GetCurrentThreadId(), h ? "ok" : "failed");
}

/* ------------------------------------------------------- thread priority */

/* PS3_WALKER_PRIO=<n> / PS3_HOT_PRIO=<n> (SCHED-NOTES.md, 2026-11-06; both off by
 * default): a THREAD_PRIORITY_* level (1 = above normal, 2 = highest) for the
 * walker, and for the SPU workers + SPURS kernel (and the walker too when
 * PS3_WALKER_PRIO is not set). Independent of PS3_HOST_PIN, so it also works
 * below 8 cores, where placement is off. Why: at 4 cores (PS3_CPU_LIMIT=4) the
 * 9 hot threads share 8 logical processors and the walker -- the bottleneck --
 * was ready-but-not-running 14% of the time; set from outside the process to
 * HIGHEST it walked 17% faster per draw (+9..+10% fps, every paired block). At
 * all cores, other programs' threads at normal priority took the CPUs of the
 * pinned threads (each SPU worker and the kernel may run on one logical
 * processor only). The PPU main thread is left alone: it spins, and raised it
 * starves the SPURS kernel below 8 cores.
 *   PS3_PRIO_AB=<ms>: the levels on, then normal, then on... every <ms>, with
 *   "[prio-ab] on|off t=<us>" lines (an in-run A/B, as PS3_HOST_PIN_AB). */
static int s_prio_walker, s_prio_hot, s_prio_ab_ms;
static volatile LONG s_prio_on = 1;
static hp_reg_t s_preg[HP_MAX_REG];
static volatile LONG s_npreg;
static SRWLOCK s_preg_lock = SRWLOCK_INIT;

/* six (2026-11-09) PS3_PIN_SPU_PRIO=<n> (default 1 = THREAD_PRIORITY_ABOVE_NORMAL; 0 = the old behaviour: normal).
 * With the 3-7-core plan only (s_small_plan): the level of the SPU workers, the SPURS kernel and the SPURS task
 * threads (spu_workload.c spu_async_thread, via host_spu_side_prio) unless PS3_HOT_PRIO sets one. That plan lets the
 * light threads run on the SPU set's logical processors, and the guest PPU threads with a guest priority under 256
 * run ABOVE_NORMAL (sys_ppu_thread.c; in this game fios x6, brbDriver, brbClient, snd_stream x2, EdgeWatchdog,
 * voice). A 6-core test run of v0.1.23 with PS3_PIN_SMALL=1: brbDriver (11, 1.0 s/s CPU, 67 switches/s) and
 * EdgeWatchdog (11) ran in the SPU set while SPU worker 5 (10) sat READY with no CPU for ~4 s, the other five SPUs
 * waiting on it in RenJobSet, until Windows' starvation boost (15) let it run; spinners at 11 placed on two of the SPU
 * set's processors repeat it at will (4-5 s frames), and not at equal levels. At equal levels a ready worker gets
 * its CPU at the spinner's quantum end; the SPURS task threads (SPU programs too) get the same level, so neither
 * side of an SPU-to-SPU wait outranks the other. host_spu_side_prio() also turns the inline render jobs' bounded
 * lock-line waits on (host/comb nrinline.c NRENDER_WAIT_US: only where this parity is in force).
 * PS3_PIN_SPU_PRIO_AB=<ms> (TEST ONLY): the parity (SPU workers and kernel; task threads that start meanwhile) and
 * with it the bounded waits on, then off, then on... every <ms>, "[pin-ab] on|off" lines (abwin6.py splits them).
 * PS3_HOST_PIN_AB's "off" side drops them too (hp_ab_spu_side): placement off means no parity, as by default. */
static int s_spu_prio = 1;
static volatile LONG s_spu_ab_on = 1;
static int hp_spu_parity(void) { return s_small_plan && s_spu_prio > 0 && s_spu_ab_on ? s_spu_prio : 0; }

static int hp_role_prio(enum host_role r)
{
    if (r == HOST_ROLE_WALKER) return s_prio_walker ? s_prio_walker : s_prio_hot;
    if (r == HOST_ROLE_SPU || r == HOST_ROLE_SPURS_KERNEL) return s_prio_hot ? s_prio_hot : hp_spu_parity();
    return 0;
}

/* The THREAD_PRIORITY_* level for the SPURS task threads (0 = leave them at normal): the SPU workers' level while
 * the PS3_PIN_SPU_PRIO parity is in force. */
int host_spu_side_prio(void)
{
    if (s_init_state != 2) return 0;   /* (a plain read: also asked once per inline render job) */
    return hp_spu_parity() ? hp_role_prio(HOST_ROLE_SPU) : 0;
}

/* The logical processors the process may use (group 0 affinity mask; after PS3_CPU_LIMIT). */
static int hp_process_lps(void)
{
    DWORD_PTR pm = 0, sm = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) return 0;
    int n = 0;
    for (; pm; pm &= pm - 1) n++;
    return n;
}

#include <intrin.h>
/* An AMD 6-core part that mixes full and compact ("c") cores in one L3 and may report one EfficiencyClass for both
 * (unverified): Phoenix 2 (Ryzen 5 8500G / 8500GE, 7545U, 8540U, 220, Ryzen Z1) and Krackan (Ryzen AI 5 340). */
static int hp_brand_has(const char* b, const char* k)
{
    for (const char* q = strstr(b, k); q; q = strstr(q + 1, k)) {
        const char n = q[strlen(k)];
        if (!((n >= '0' && n <= '9') || (n >= 'A' && n <= 'Z') || (n >= 'a' && n <= 'z'))) return 1;
    }
    return 0;
}
static int hp_amd_mixed_6c(void)
{
    int r[4];
    char b[49];
    memset(b, 0, sizeof b);
    __cpuid(r, 0);
    if (!(r[1] == 0x68747541 && r[3] == 0x69746E65 && r[2] == 0x444D4163)) return 0;   /* "AuthenticAMD" */
    __cpuid(r, (int)0x80000000);
    if ((unsigned)r[0] < 0x80000004u) return 0;
    for (int i = 0; i < 3; i++) { __cpuid(r, (int)(0x80000002u + (unsigned)i)); memcpy(b + 16 * i, r, 16); }
    static const char* const k[] = { "8500G", "8500GE", "7545U", "8540U", "Ryzen 5 220", "Ryzen 5 PRO 220",
                                     "AI 5 340", "AI 5 PRO 340", NULL };
    for (int i = 0; k[i]; i++) if (hp_brand_has(b, k[i])) return 1;
    return hp_brand_has(b, "Ryzen Z1") && !strstr(b, "Z1 Extreme");
}

/* six (2026-11-09): is the 6-core plan on by default here (PS3_PIN_SMALL unset; s_proc_cores < 8)? Only on the CPU
 * shape it was measured on: 6 physical cores with SMT (12 logical processors), the whole of what the plan sees --
 * the plan works on the topology, so a bigger CPU limited to 6 cores by the user (Task Manager, Process Lasso, an
 * X3D's V-cache-only setting) would get the 16-core plan with pins outside the process (measured worse: 55.5 vs
 * 58.1 fps, frames of 50 ms or more 9.0 vs 0.6 per minute) -- no logical processor reserved for another process,
 * one EfficiencyClass, one L3 (two-CCX parts such as Ryzen 5 1600 / 2600 / 3600 / 4600G and full+compact core
 * mixes are untested), and not a known AMD full+compact mix. *why: the reason when not (for the [pin] line). */
static int hp_six_default(int lps, char* why, size_t cap)
{
    if (s_proc_cores < 6) { snprintf(why, cap, "faster below 6 cores without it; PS3_PIN_SMALL=1 keeps it"); return 0; }
    if (s_proc_cores > 6) {
        snprintf(why, cap, "on by default below 8 cores only on a uniform 6-core CPU with SMT; PS3_PIN_SMALL=1 keeps it");
        return 0;
    }
    if (lps != 12) {
        snprintf(why, cap, "%s: the 6-core plan is on by default with SMT only, untested without; PS3_PIN_SMALL=1 "
                           "keeps it", lps == 6 ? "no SMT" : "not 12 logical processors");
        return 0;
    }
    if (s_ncore != 6) {
        if (s_ncore > 6) snprintf(why, cap, "the process may use 6 of this CPU's %d cores (an affinity set outside the "
                                            "game): the plan is made for the whole CPU", s_ncore);
        else snprintf(why, cap, "the CPU topology was not read");
        return 0;
    }
    int usable = 0, emin = 256, emax = -1, l3a = -2, l3two = 0;
    for (int c = 0; c < s_ncore; c++) {
        for (int k = 0; k < s_core[c].nlp; k++) usable += !s_lp[s_core[c].lp[k]].unusable;
        if (s_core[c].eff < emin) emin = s_core[c].eff;
        if (s_core[c].eff > emax) emax = s_core[c].eff;
        if (l3a == -2) l3a = s_core[c].l3_id; else if (s_core[c].l3_id != l3a) l3two = 1;
    }
    if (usable != 12) {
        snprintf(why, cap, "%d of its 12 logical processors are reserved for another process", 12 - usable);
        return 0;
    }
    if (emin != emax) {
        snprintf(why, cap, "cores of more than one type (EfficiencyClass %d-%d): the 6-core plan is untested there; "
                           "PS3_PIN_SMALL=1 keeps it", emin, emax);
        return 0;
    }
    if (l3two) {
        snprintf(why, cap, "more than one L3 cache (two CCXs or dies): the 6-core plan is untested there; "
                           "PS3_PIN_SMALL=1 keeps it");
        return 0;
    }
    if (hp_amd_mixed_6c()) {
        snprintf(why, cap, "an AMD CPU with full and compact cores: the 6-core plan is untested there; PS3_PIN_SMALL=1 "
                           "keeps it");
        return 0;
    }
    return 1;
}

/* PS3_PIN_VAR_AB=<ms> (TEST ONLY, small-CPU plan): plan B (as configured) and plan A (PS3_PIN_SMALL_PPU=0 and
 * PS3_PIN_SMALL_WALKER=0: the plan before 2026-11-09) flip every <ms>: "[pin-ab] on" = B, "off" = A. A thread that
 * registers during a flip may keep the other plan's set until the next one. */
typedef struct {
    hp_set_t walker, ppu, kernel, light, spu[HP_NSPU];
    unsigned char lp_hot[HP_MAX_LP], lp_light[HP_MAX_LP];
} hp_snap_t;
static hp_snap_t s_snap[2];   /* [0] plan A, [1] plan B */
static void hp_snap_save(hp_snap_t* p)
{
    p->walker = s_set_walker; p->ppu = s_set_ppu; p->kernel = s_set_kernel; p->light = s_set_light;
    memcpy(p->spu, s_set_spu, sizeof p->spu);
    memcpy(p->lp_hot, s_lp_hot, sizeof p->lp_hot); memcpy(p->lp_light, s_lp_light, sizeof p->lp_light);
}
static void hp_snap_load(const hp_snap_t* p)
{
    s_set_walker = p->walker; s_set_ppu = p->ppu; s_set_kernel = p->kernel; s_set_light = p->light;
    memcpy(s_set_spu, p->spu, sizeof p->spu);
    memcpy(s_lp_hot, p->lp_hot, sizeof s_lp_hot); memcpy(s_lp_light, p->lp_light, sizeof s_lp_light);
}
static int hp_var_ab_setup(void)
{
    hp_snap_save(&s_snap[1]);
    const int ppu = s_small_ppu, walker = s_small_walker;
    static const hp_snap_t zero;
    hp_snap_load(&zero);
    s_small_ppu = s_small_walker = 0;
    hp_plan();
    hp_snap_save(&s_snap[0]);
    s_small_ppu = ppu; s_small_walker = walker;
    if (s_log) { fprintf(stderr, "[pin] PS3_PIN_VAR_AB plan A (\"off\"):\n"); hp_log_plan(); }
    hp_snap_load(&s_snap[1]);
    if (s_log) fprintf(stderr, "[pin] PS3_PIN_VAR_AB plan B (\"on\") = the plan above it (PS3_PIN_SMALL_PPU=%d, "
                               "PS3_PIN_SMALL_WALKER=%d); B first\n", ppu, walker);
    return 1;
}
static DWORD WINAPI hp_var_ab_thread(LPVOID arg)
{
    const DWORD ms = (DWORD)(uintptr_t)arg;
    extern unsigned long long ps3_qpc_us(void);
    int b = 1;
    for (;;) {
        Sleep(ms);
        b = !b;
        AcquireSRWLockExclusive(&s_reg_lock);
        hp_snap_load(&s_snap[b]);
        apply_process_default(1);
        for (LONG i = 0; i < s_nreg; i++) {
            const hp_set_t* s = role_set(s_reg[i].role, s_reg[i].index);
            if (s && s->n) apply_thread(s_reg[i].h, s, 1);
        }
        ReleaseSRWLockExclusive(&s_reg_lock);
        fprintf(stderr, "[pin-ab] %s t=%lluus (PS3_PIN_VAR_AB: plan %s)\n", b ? "on" : "off", ps3_qpc_us(), b ? "B" : "A");
    }
    return 0;
}

/* The SPU-side parity on / off at run time (the in-run A/Bs only): the SPU workers' and the kernel's level now, the
 * SPURS task threads' for those that start after, the bounded lock-line waits at the next inline job. Small-CPU plan
 * with its parity only (not under PS3_HOT_PRIO, which sets the level itself). */
static void hp_ab_spu_side(int on)
{
    if (!s_small_plan || s_spu_prio <= 0 || s_prio_hot) return;
    InterlockedExchange(&s_spu_ab_on, on);
    AcquireSRWLockShared(&s_preg_lock);
    for (LONG i = 0; i < s_npreg; i++)
        if (s_preg[i].role == HOST_ROLE_SPU || s_preg[i].role == HOST_ROLE_SPURS_KERNEL)
            SetThreadPriority(s_preg[i].h, on ? hp_role_prio(s_preg[i].role) : THREAD_PRIORITY_NORMAL);
    ReleaseSRWLockShared(&s_preg_lock);
}

static DWORD WINAPI hp_spu_prio_ab_thread(LPVOID arg)
{
    const DWORD ms = (DWORD)(uintptr_t)arg;
    extern unsigned long long ps3_qpc_us(void);
    for (;;) {
        Sleep(ms);
        const LONG on = !InterlockedCompareExchange(&s_spu_ab_on, 0, 0);
        hp_ab_spu_side(on);
        fprintf(stderr, "[pin-ab] %s t=%lluus (PS3_PIN_SPU_PRIO_AB: SPU-side priority %s, NRENDER_WAIT %s)\n",
                on ? "on" : "off", ps3_qpc_us(), on ? "raised" : "normal", on ? "on" : "off");
    }
    return 0;
}

static DWORD WINAPI hp_prio_ab_thread(LPVOID arg)
{
    const DWORD ms = (DWORD)(uintptr_t)arg;
    extern unsigned long long ps3_qpc_us(void);
    for (;;) {
        Sleep(ms);
        const LONG on = !InterlockedCompareExchange(&s_prio_on, 0, 0);
        InterlockedExchange(&s_prio_on, on);
        AcquireSRWLockShared(&s_preg_lock);
        for (LONG i = 0; i < s_npreg; i++)
            SetThreadPriority(s_preg[i].h, on ? hp_role_prio(s_preg[i].role) : THREAD_PRIORITY_NORMAL);
        ReleaseSRWLockShared(&s_preg_lock);
        fprintf(stderr, "[prio-ab] %s t=%lluus\n", on ? "on" : "off", ps3_qpc_us());
    }
    return 0;
}

static void hp_prio_thread(enum host_role role, int index)
{
    const int p = hp_role_prio(role);
    if (!p) return;
    HANDLE dup = NULL;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup,
                        THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION |
                        THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        AcquireSRWLockExclusive(&s_preg_lock);
        if (s_npreg < HP_MAX_REG) { s_preg[s_npreg].h = dup; s_preg[s_npreg].role = role; s_preg[s_npreg].index = index; s_npreg++; dup = NULL; }
        ReleaseSRWLockExclusive(&s_preg_lock);
        if (dup) CloseHandle(dup);
    }
    const BOOL ok = SetThreadPriority(GetCurrentThread(),
                                      InterlockedCompareExchange(&s_prio_on, 0, 0) ? p : THREAD_PRIORITY_NORMAL);
    if (s_log) fprintf(stderr, "[prio] tid %lu role %d.%d priority %d%s\n", GetCurrentThreadId(), (int)role, index, p,
                       ok ? "" : " FAILED");
}

/* ------------------------------------------------------------------ API */

/* host_qos_init on a hybrid CPU, after today's rules have decided s_pin_on
 * (= the old side: today's placement). pin_env: PS3_HOST_PIN. */
static void hp_init_hybrid(int topo, int pin_env)
{
    const int old_on = s_pin_on && topo;
    { const char* e = getenv("PS3_SMALL_CPU"); s_hy_small = (e && *e) ? atoi(e) != 0 : s_hy_np < 8; }
    const int ab = (pin_env && topo) ? env_int("PS3_HOST_PIN_AB", 0) : 0;
    s_old_on     = old_on;
    s_hy_plan_on = pin_env && topo && s_hy_npl >= 8;
    s_hy_ab      = ab > 0;
    /* PS3_HYBRID_AB_SIDES=<a>,<b>[,<c>] (tuning, NOTES.md section 9): the sides the A/B flips through in turn, each
     * a layout (pool, spill, split or 9 letters), "hybrid" (the default layout) or "old" (today's behaviour);
     * the [pin-ab] tags then name them. Default: hybrid,old. */
    strcpy(s_side_name[0], "hybrid"); strcpy(s_side_name[1], "old"); s_side_old[1] = 1;
    s_nside = s_hy_ab ? 2 : 1;
    int has_old = s_hy_ab;
    { const char* sd = s_hy_ab ? getenv("PS3_HYBRID_AB_SIDES") : NULL;
      if (sd && *sd) {
          int n = 0, old = 0;
          for (const char* p = sd; *p && n < HP_NSIDE; ) {
              const size_t len = strcspn(p, ",");
              if (len && len < sizeof s_side_name[0]) {
                  memcpy(s_side_name[n], p, len); s_side_name[n][len] = 0;
                  s_side_old[n] = !strcmp(s_side_name[n], "old"); old |= s_side_old[n]; n++;
              }
              p += len; if (*p) p++;
          }
          if (n >= 2) { s_nside = n; s_ab_named = 1; has_old = old; }
          else {
              strcpy(s_side_name[0], "hybrid"); strcpy(s_side_name[1], "old");
              s_side_old[0] = 0; s_side_old[1] = 1; s_side_old[2] = 0;
              fprintf(stderr, "[pin] PS3_HYBRID_AB_SIDES=%s: needs 2-3 names, ignored (hybrid,old)\n", sd);
          }
      } }
    /* PS3_HOST_PIN_AB below 8 P-cores, without PS3_HYBRID_PLAN / PS3_HYBRID_AB_SIDES (Intel test 2, NOTES.md 11):
     * three sides in turn, olds0, olds and old. olds = olds0 without the SPU worker on the lp0 core's first thread,
     * so one session also tells what that lp0 pin costs on a real laptop (interrupts / DPCs land there; skeptic). */
    { const char* plan = getenv("PS3_HYBRID_PLAN");
      if (s_hy_ab && !s_ab_named && s_hy_plan_on && s_hy_np < 8 && !(plan && *plan)) {
          strcpy(s_side_name[0], "olds0"); strcpy(s_side_name[1], "olds"); strcpy(s_side_name[2], "old");
          s_side_old[0] = 0; s_side_old[1] = 0; s_side_old[2] = 1;
          s_nside = 3; s_ab_named = 1; has_old = 1;
      } }
    s_hy_ab_spin = s_hy_ab && has_old && s_hy_small != hp_small_old();
    if (s_log)
        fprintf(stderr, "[pin] hybrid: %d P-cores (%d logical) + %d E-cores (%d logical)%s; PS3_HYBRID=0: today's "
                        "behaviour\n", s_hy_np, s_hy_npl, s_hy_ne, s_hy_nel,
                s_fake_cores ? " [PS3_FAKE_HYBRID: TEST ONLY, no real E-cores]" : "");
    /* EcoQoS: Windows 11 may run threads it rates low-importance on E-cores at
     * efficient clocks; the hot threads must never be. */
    { const char* eco = getenv("PS3_ECOQOS_OFF");
      if (!(eco && *eco)) {
          s_ecoqos_off = 1; s_hy_ecoqos = 1;
          hp_ecoqos_process();
          if (s_log) fprintf(stderr, "[pin] hybrid: EcoQoS opt-out on for the process and the hot threads "
                                     "(PS3_ECOQOS_OFF=0: off)%s\n", s_hy_ab ? ", hybrid side only" : "");
      } }
    if (s_log)
        fprintf(stderr, "[pin] hybrid: %s CPU by its %d P-cores (PS3_SMALL_CPU forces it): SPURS kernel yield loop %s "
                        "with SPURS_KERNEL_SPIN=auto%s\n", s_hy_small ? "a small" : "not a small", s_hy_np,
                s_hy_small ? "off" : "on", s_hy_ab_spin ? (s_hy_small ? " (old side: on)" : " (old side: off)") : "");
    if (!(pin_env && topo)) {
        s_pin_on = 0;
        if (s_log) fprintf(stderr, "[pin] hybrid: no thread placement (PS3_HOST_PIN=0)\n");
        return;
    }
    s_pin_spu    = env_int("SPURS_PIN", 1);
    s_pin_ppu    = env_int("PS3_PIN_PPU", 1);
    s_pin_walker = env_int("PS3_PIN_WALKER", 1);
    s_pin_kernel = env_int("PS3_PIN_KERNEL", 1);
    s_hard       = env_int("PS3_HOST_PIN_HARD", 0);
    if (s_old_on && has_old) hp_plan();
    if (s_hy_plan_on) {
        hp_plan_hybrid();
        for (int k = 0; k < s_nside; k++) {
            if (s_side_old[k]) continue;
            const char* nm = strcmp(s_side_name[k], "hybrid") ? s_side_name[k] : hp_layout_default();
            if (!hp_layout_build(&s_lay[k], nm)) {
                fprintf(stderr, "[pin] hybrid: layout '%s' unknown (pool, spill, split or 9 letters of wpeqa), pool "
                                "instead\n", nm);
                hp_layout_build(&s_lay[k], "pool");
            }
        }
        /* the kernel's auto yield loop per side: old = today's rule, a layout = its +k/-k, else the hybrid rule */
        int any = 0, differ = 0;
        for (int k = 0; k < s_nside; k++) {
            s_side_spin[k] = s_side_old[k] ? !hp_small_old() : s_lay[k].kspin >= 0 ? s_lay[k].kspin : !s_hy_small;
            any |= !s_side_old[k] && s_lay[k].kspin >= 0;
            differ |= s_side_spin[k] != s_side_spin[0];
        }
        s_spin_by_side = any || (s_hy_ab && differ);
        s_hy_ab_spin = s_hy_ab && differ;
        if (s_log && any)
            for (int k = 0; k < s_nside; k++)
                if (!s_side_old[k] && s_lay[k].kspin >= 0)
                    fprintf(stderr, "[pin] hybrid[%s]: SPURS kernel yield loop %s with SPURS_KERNEL_SPIN=auto (a "
                                    "logical processor of its own, or +k/-k)\n",
                            s_lay[k].name, s_lay[k].kspin ? "on" : "off");
    }
    if (s_log) {
        hp_log_cores();
        if (s_hy_plan_on) hp_log_hybrid();
        else fprintf(stderr, "[pin] hybrid: only %d P-core logical processors for the 8 hot threads: no placement on "
                             "the hybrid side (the OS spreads them)\n", s_hy_npl);
        if (s_hy_ab && has_old) {
            if (s_old_on) hp_log_roles("old side: ");
            else fprintf(stderr, "[pin] old side: no thread placement (today's rules, line above)\n");
        }
        if (s_hy_ab && !s_ab_named)
            fprintf(stderr, "[pin] hybrid A/B: PS3_HOST_PIN_AB=%d flips hybrid and old from the guest's start "
                            "([pin-ab] hybrid|old lines)%s%s\n", ab,
                    s_hy_ecoqos ? "; EcoQoS opt-out on the hybrid side only" : "",
                    s_hy_ab_spin ? "; the kernel's auto yield loop follows the side" : "");
        else if (s_hy_ab)
            fprintf(stderr, "[pin] hybrid A/B: PS3_HOST_PIN_AB=%d flips %s, %s%s%s in turn from the guest's start "
                            "([pin-ab] <side> lines; PS3_HYBRID_AB_SIDES)%s%s\n", ab, s_side_name[0], s_side_name[1],
                    s_nside > 2 ? ", " : "", s_nside > 2 ? s_side_name[2] : "",
                    s_hy_ecoqos && has_old ? "; EcoQoS opt-out on the hybrid sides only" : "",
                    s_hy_ab_spin ? "; the kernel's auto yield loop follows the side" : "");
    }
    s_pin_on = s_hy_plan_on || s_hy_ab;
    /* No process default yet: LD_PSO_PRECOMPILE may build on every core until
     * the guest starts (hp_game_start). */
    if (s_hy_ab) CloseHandle(CreateThread(NULL, 0, hp_hab_thread, (LPVOID)(uintptr_t)ab, 0, NULL));
    const char* stall = getenv("PS3_SPU_STALL_TEST");
    if (s_pin_on && stall && *stall) CloseHandle(CreateThread(NULL, 0, hp_stall_thread, (LPVOID)stall, 0, NULL));
}

void host_qos_init(void)
{
    if (InterlockedCompareExchange(&s_init_state, 1, 0) != 0) {
        while (InterlockedCompareExchange(&s_init_state, 2, 2) != 2) SwitchToThread();
        return;
    }
    s_log        = env_int("PS3_HOST_PIN_LOG", 1);
    s_ecoqos_off = env_int("PS3_ECOQOS_OFF", 0);
    s_mmcss      = env_int("PS3_MMCSS", 0);
    s_pin_on     = env_int("PS3_HOST_PIN", 0);
    s_prio_walker = env_int("PS3_WALKER_PRIO", 0);
    s_prio_hot    = env_int("PS3_HOT_PRIO", 0);
    s_prio_ab_ms  = env_int("PS3_PRIO_AB", 0);
    s_spu_prio    = env_int("PS3_PIN_SPU_PRIO", 1);
    s_small_ppu   = env_int("PS3_PIN_SMALL_PPU", 1);
    s_small_walker = env_int("PS3_PIN_SMALL_WALKER", 1);
    if (s_prio_walker || s_prio_hot) {
        fprintf(stderr, "[prio] walker %d, SPU workers + SPURS kernel %d (THREAD_PRIORITY_*)%s\n",
                s_prio_walker ? s_prio_walker : s_prio_hot, s_prio_hot, s_prio_ab_ms > 0 ? ", A/B" : "");
        if (s_prio_ab_ms > 0) CloseHandle(CreateThread(NULL, 0, hp_prio_ab_thread, (LPVOID)(uintptr_t)s_prio_ab_ms, 0, NULL));
    }
    if (s_ecoqos_off) hp_ecoqos_process();
    /* PS3_PROCESS_PRIORITY=<0|1|2> (2026-11-07): the process's priority class. 1 = ABOVE_NORMAL: Windows runs the
     * game's threads before other programs' normal-priority threads, so a background program (a video render, a
     * compiler) gets the CPU the game leaves and no longer starves the SPU workers / SPURS kernel / walker (the
     * game's spin-waits yield their slices to ready threads of equal priority). 2 = HIGH. 0 = leave it (normal). */
    { const int pc = env_int("PS3_PROCESS_PRIORITY", 0);
      if (pc > 0) {
          const DWORD cls = pc >= 2 ? HIGH_PRIORITY_CLASS : ABOVE_NORMAL_PRIORITY_CLASS;
          const BOOL ok = SetPriorityClass(GetCurrentProcess(), cls);
          fprintf(stderr, "[prio] process priority class %s%s (PS3_PROCESS_PRIORITY=%d)\n",
                  pc >= 2 ? "HIGH" : "ABOVE_NORMAL", ok ? "" : " FAILED", pc);
      } }
    /* PS3_CPU_LIMIT=<n> (testing: how a CPU with fewer cores runs the game):
     * the process may only use the LAST n physical cores (both SMT threads),
     * i.e. on a two-die CPU the die without the extra cache. Thread placement
     * then plans on those cores alone, as it would on a CPU of that size. */
    const int lim = env_int("PS3_CPU_LIMIT", 0);
    /* Hybrid CPUs (header): the topology is read even without PS3_HOST_PIN, for
     * the EcoQoS and small-CPU decisions; nothing else uses it then. */
    /* PS3_HYBRID: 1 (default) Intel only -- AMD's Zen 4c/5c mixes may report an
     * EfficiencyClass too (unverified) and stay on today's behaviour; 2 any
     * vendor; 0 off. PS3_FAKE_HYBRID implies "any vendor". */
    const char* fake = getenv("PS3_FAKE_HYBRID");
    if (fake && !*fake) fake = NULL;
    /* PS3_FAKE_HYBRID / PS3_FAKE_PARK change the process affinity: never from a stray variable in a player's
     * environment or extra settings alone (skeptic, NOTES.md 10/11). */
    if (fake && !env_int("PS3_TEST_ONLY", 0)) {
        fprintf(stderr, "[pin] PS3_FAKE_HYBRID=%s ignored: a TEST ONLY switch, it needs PS3_TEST_ONLY=1 too\n", fake);
        fake = NULL;
    }
    int hy_env = env_int("PS3_HYBRID", 1);
    if (hy_env == 1 && !fake && !hp_cpu_intel()) hy_env = 0;
    const int topo = (lim > 0 || s_pin_on || hy_env || fake) && hp_read_topology();
    const int faked = fake && topo && hp_fake_hybrid(fake);
    if (faked && lim > 0) fprintf(stderr, "[pin] PS3_CPU_LIMIT=%d ignored: PS3_FAKE_HYBRID chose the cores\n", lim);
    if (!faked && lim > 0 && topo && lim < s_ncore) {
        DWORD_PTR mask = 0;
        for (int c = s_ncore - lim; c < s_ncore; c++)
            for (int k = 0; k < s_core[c].nlp; k++) {
                const hp_lp_t* l = &s_lp[s_core[c].lp[k]];
                if (l->group == 0 && l->idx < 64) mask |= (DWORD_PTR)1 << l->idx;
            }
        if (mask && SetProcessAffinityMask(GetCurrentProcess(), mask)) {
            hp_keep_last_cores(lim);
            fprintf(stderr, "[pin] PS3_CPU_LIMIT=%d: process limited to %d cores (mask 0x%llX)%s\n",
                    lim, lim, (unsigned long long)mask, s_pin_on ? ", placement planned on them" : "");
        } else s_pin_on = 0;
    }
    /* PS3_CPU_FIRST=<n> (TEST ONLY, six 2026-11-09): as PS3_CPU_LIMIT, but the FIRST n cores, logical processor 0
     * included as on a real CPU of that size (PS3_CPU_LIMIT keeps the last ones, without it). Not with PS3_CPU_LIMIT. */
    { const int first = lim > 0 ? 0 : env_int("PS3_CPU_FIRST", 0);
      if (first > 0 && topo && first < s_ncore) {
          DWORD_PTR mask = 0;
          for (int c = 0; c < first; c++)
              for (int k = 0; k < s_core[c].nlp; k++) {
                  const hp_lp_t* l = &s_lp[s_core[c].lp[k]];
                  if (l->group == 0 && l->idx < 64) mask |= (DWORD_PTR)1 << l->idx;
              }
          if (mask && SetProcessAffinityMask(GetCurrentProcess(), mask)) {
              hp_keep_first_cores(first);
              fprintf(stderr, "[pin] PS3_CPU_FIRST=%d (TEST ONLY): process limited to its first %d cores, logical "
                              "processor 0 included (mask 0x%llX)\n", first, first, (unsigned long long)mask);
          } else s_pin_on = 0;
      } }
    /* Hybrid? (PS3_PIN_MAP: the map, as written, under today's rules.) */
    { const char* map = getenv("PS3_PIN_MAP");
      if (topo && hy_env && !(map && *map)) hp_hybrid_detect(); }
    /* NOTES.md sections 9-11: 0.1.23's behaviour on every hybrid CPU unless the A/B or a layout is asked for
     * (hp_hybrid_default_on: no class by default in 0.1.24). */
    { const char* plan = getenv("PS3_HYBRID_PLAN");
      if (s_hy_on && !(s_pin_on && (env_int("PS3_HOST_PIN_AB", 0) > 0 || (plan && *plan) || hp_hybrid_default_on()))) {
          s_hy_on = 0;
          if (s_log)
              fprintf(stderr, "[pin] hybrid: %d P-cores (%d logical) + %d E-cores: 0.1.23's behaviour here (the hybrid "
                              "plan is not a default yet; with PS3_HOST_PIN=1, PS3_HOST_PIN_AB=30000 or "
                              "PS3_HYBRID_PLAN=<layout> tries it)\n", s_hy_np, s_hy_npl, s_hy_ne);
          /* 2026-11-12, tester i7-13620H (6P+4E, 0.1.27): 0.1.23's plan (hp_plan, 8+ cores) gives the walker, the
           * six SPU workers and the PPU main thread order[0..7] -- the cores without logical processor 0, P-cores
           * first -- so with fewer than 8 such P-cores the PPU main thread and SPU 4-5 (5 = sound) were pinned
           * alone on E-cores: STUCK SPU inline runs of 1-6 s, 3 freezes of 2.5-5.7 s with silent audio in 100 s.
           * Placement off (the OS spreads): 185 s, no STUCK, longest hitch 108 ms, flat 30 fps. Every Intel hybrid
           * has at most 8 P-cores, so 0.1.23's plan is off on every one of them; PS3_PIN_HYBRID_OLD=1 keeps it. */
          if (s_pin_on && !env_int("PS3_PIN_HYBRID_OLD", 0)) {
              int np_free = 0;
              for (int c = 0; c < s_ncore; c++) np_free += s_core[c].eff == s_hy_effmax && !s_core[c].has_lp0;
              if (np_free < 8) {
                  s_pin_on = 0;
                  if (s_log)
                      fprintf(stderr, "[pin] hybrid: thread placement off (0.1.23's plan would pin the PPU main "
                                      "thread or an SPU worker alone on an E-core: %d P-cores without logical "
                                      "processor 0, it needs 8; PS3_PIN_HYBRID_OLD=1 keeps it)\n", np_free);
              }
          }
      } else if (s_hy_on) hp_hybrid_mask_apply(); }
    /* Today's rules (on a hybrid: the A/B's old side; printed only there). */
    const int hy_quiet = s_hy_on && env_int("PS3_HOST_PIN_AB", 0) <= 0;
    const char* rule_pfx = s_hy_on ? "[pin] old side (today's rules): " : "[pin] ";
    const int pin_env = s_pin_on;
    /* Fewer than 8 cores: no placement (host_small_cpu), except on a uniform 6-core CPU with SMT (hp_six_default,
     * 2026-11-09): the 6-core plan with its starvation fix (PS3_PIN_SPU_PRIO; NRENDER_WAIT_US in host/comb).
     * Measured on 6 cores of a 9950X3D (in-run A/B against placement off): fast rooftop +6% (-17% walker time per
     * draw) on its first 6 cores and +20% on the last 6, fast opening +7%, slow-core simulation +12% / +27% (opening /
     * rooftop; on the first 6 both sides hold the 30 cap, walker -25% per frame). Not on 6 cores without SMT (untested;
     * a 6c/6t is CPU-bound with 6 logical processors), nor below 6 (4 cores: 23.8 with vs 35.2 fps without,
     * 2026-11-02), nor at 7. PS3_PIN_SMALL=1 keeps placement at any of these sizes, PS3_PIN_SMALL=0 turns it off on
     * 6 cores too. (On a hybrid CPU this is the A/B's old side; the hybrid plan decides by P-cores.) */
    if (s_pin_on && hp_small_old()) {
        const int force = env_int("PS3_PIN_SMALL", -1), lps = hp_process_lps();
        char why[256] = "";
        const int six = force < 0 && hp_six_default(lps, why, sizeof why);
        if (force == 0 || (force < 0 && !six)) {
            s_pin_on = 0;
            if (hy_quiet) {}
            else if (force == 0)
                fprintf(stderr, "%s%d cores: thread placement off (PS3_PIN_SMALL=0)\n", rule_pfx, s_proc_cores);
            else
                fprintf(stderr, "%s%d cores, %d logical: thread placement off (%s)\n", rule_pfx, s_proc_cores, lps, why);
        } else if (!hy_quiet)
            fprintf(stderr, "%s%d cores, %d logical: thread placement on (%s; PS3_PIN_SMALL=0 turns it off)\n",
                    rule_pfx, s_proc_cores, lps, force > 0 ? "PS3_PIN_SMALL=1"
                                                           : "6 cores with SMT, one core type, one L3: the 6-core plan");
    }
    /* Exactly 8 cores: no placement either (2026-11-08). Tester Ati, Ryzen 7
     * 9800X3D 8c/16t + RX 9070 XT, v0.1.21-0.1.22: every run with placement
     * on had 4.3-5.1 frames of 100 ms or more per minute of play (~190 ms
     * stalls on a ~91-frame grid, 'park' or in-pass), the run without it 0.03;
     * heavy scenes matched 4.21 vs 0.04/min. Occlusion was ruled out (its
     * deferral had switched itself off). On 8 cores the plan puts the PPU on
     * lp0, the kernel beside SPU 5 and every other thread on the six SMT
     * siblings; 16 cores with placement on stay at 0.26-0.43/min, so 9+ cores
     * keep it. Only placement changes here: host_small_cpu (the SPURS kernel's
     * spin, cellSpurs.c) still answers "not small" on 8 cores.
     * PS3_PIN_8=1 (or PS3_PIN_SMALL=1) keeps placement on 8 cores. */
    if (s_pin_on && !hp_small_old() && s_proc_cores == 8 &&
        !env_int("PS3_PIN_8", 0) && !env_int("PS3_PIN_SMALL", 0)) {
        s_pin_on = 0;
        if (!hy_quiet)
        fprintf(stderr, "%s8 cores: thread placement off (on 8 cores it caused ~200 ms stalls every few "
                        "seconds; PS3_PIN_8=1 keeps it)\n", rule_pfx);
    }
    if (s_hy_on) {
        hp_init_hybrid(topo, pin_env);
        InterlockedExchange(&s_init_state, 2);
        return;
    }
    if (s_pin_on && topo) {
        s_pin_spu    = env_int("SPURS_PIN", 1);
        s_pin_ppu    = env_int("PS3_PIN_PPU", 1);
        s_pin_walker = env_int("PS3_PIN_WALKER", 1);
        s_pin_kernel = env_int("PS3_PIN_KERNEL", 1);
        s_hard       = env_int("PS3_HOST_PIN_HARD", 0);
        hp_plan();
        hp_log_plan();
        if (s_small_plan && s_log) {
            fprintf(stderr, "[pin] small-CPU plan: walker %s (PS3_PIN_SMALL_WALKER=%d), PPU %s (PS3_PIN_SMALL_PPU=%d)\n",
                    s_set_walker.n > 1 ? "on both hardware threads of its core, light threads off its sibling"
                                       : "on one hardware thread", s_small_walker,
                    s_lp[s_set_ppu.lp[0]].group == 0 && s_lp[s_set_ppu.lp[0]].idx == 0 ? "on logical processor 0"
                                                                                     : "off logical processor 0",
                    s_small_ppu);
            if (!hp_spu_parity())
                fprintf(stderr, "[prio] small-CPU plan: SPU workers, SPURS kernel and SPURS tasks at their old level "
                                "(PS3_PIN_SPU_PRIO=0)\n");
            else
                fprintf(stderr, "[prio] small-CPU plan: SPU workers, SPURS kernel and SPURS task threads at "
                                "THREAD_PRIORITY %d%s, not below the guest threads (ABOVE_NORMAL = 1) that share "
                                "their cores (PS3_PIN_SPU_PRIO=0: the old level)\n", hp_role_prio(HOST_ROLE_SPU),
                        s_prio_hot ? " (PS3_HOT_PRIO)" : "");
        }
        const int ab = env_int("PS3_HOST_PIN_AB", 0);
        const int var_ab = env_int("PS3_PIN_VAR_AB", 0), spu_ab = env_int("PS3_PIN_SPU_PRIO_AB", 0);
        const int var_on = var_ab > 0 && s_small_plan && ab <= 0 && hp_var_ab_setup();
        const int spu_on = spu_ab > 0 && hp_spu_parity() && !s_prio_hot && ab <= 0 && !var_on;
        if ((var_ab > 0 && !var_on) || (spu_ab > 0 && !spu_on))
            fprintf(stderr, "[pin] PS3_PIN_VAR_AB / PS3_PIN_SPU_PRIO_AB ignored (small-CPU plan only, one A/B at a "
                            "time, not with PS3_HOST_PIN_AB or PS3_HOT_PRIO)\n");
        if (ab > 0 && hp_spu_parity() && !s_prio_hot && s_log)
            fprintf(stderr, "[pin] PS3_HOST_PIN_AB: the \"off\" side also runs the SPU side at normal priority without "
                            "the bounded lock-line waits (= placement off, PS3_PIN_SMALL=0)\n");
        apply_process_default(1);
        if (ab > 0) CloseHandle(CreateThread(NULL, 0, hp_ab_thread, (LPVOID)(uintptr_t)ab, 0, NULL));
        if (var_on) CloseHandle(CreateThread(NULL, 0, hp_var_ab_thread, (LPVOID)(uintptr_t)var_ab, 0, NULL));
        if (spu_on) CloseHandle(CreateThread(NULL, 0, hp_spu_prio_ab_thread, (LPVOID)(uintptr_t)spu_ab, 0, NULL));
        const char* stall = getenv("PS3_SPU_STALL_TEST");
        if (stall && *stall) CloseHandle(CreateThread(NULL, 0, hp_stall_thread, (LPVOID)stall, 0, NULL));
    } else s_pin_on = 0;
    InterlockedExchange(&s_init_state, 2);
}

static volatile DWORD s_ppu_main_tid;
DWORD host_ppu_main_thread_id(void) { return s_ppu_main_tid; }

static const char* const s_role_names[HOST_ROLE_COUNT] = { "light", "ppu", "walker", "spu", "kernel", "audio" };

/* host_pin_thread on a hybrid CPU: the set of the side in force; every hot
 * thread is registered (for the A/B), whether or not its side places it. */
static void hp_pin_thread_hybrid(enum host_role role, int index)
{
    const int hot = hp_role_hot(role);
    AcquireSRWLockShared(&s_side_lock);
    const int k = (int)InterlockedCompareExchange(&s_side, 0, 0);
    const int hy = !s_side_old[k];
    if (s_hy_ecoqos && hot) hp_ecoqos_thread_set(GetCurrentThread(), hy);
    if (!s_pin_on || !hot) {
        ReleaseSRWLockShared(&s_side_lock);
        if (role == HOST_ROLE_PPU_MAIN) hp_game_start();
        return;
    }
    const hp_set_t* ss[HP_NSIDE] = { NULL, NULL, NULL };
    for (int j = 0; j < s_nside; j++) {
        const hp_set_t* x = ss[j] = hp_side_set(role, index, j);
        t_excl_side[j] = x && x->n == 1 && (s_side_old[j] ? s_lp_hot[x->lp[0]] == 1 && !s_lp_light[x->lp[0]]
                                                          : s_lay[j].lp_hot[x->lp[0]] == 1 && !s_lay[j].lp_light[x->lp[0]]);
    }
    const hp_set_t* s = ss[k];
    const BOOL ok = (s && s->n) ? apply_thread(GetCurrentThread(), s, 1) : TRUE;
    /* without the A/B: today's set too, only for the log line's shape (as before) */
    const hp_set_t* hs = ss[0];
    const hp_set_t* os = s_nside > 1 ? ss[1] : NULL;
    HANDLE dup = NULL;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup,
                        THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME |
                        THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        AcquireSRWLockExclusive(&s_reg_lock);
        if (s_nreg < HP_MAX_REG) { s_reg[s_nreg].h = dup; s_reg[s_nreg].role = role; s_reg[s_nreg].index = index; s_nreg++; dup = NULL; }
        ReleaseSRWLockExclusive(&s_reg_lock);
        if (dup) CloseHandle(dup);
    }
    ReleaseSRWLockShared(&s_side_lock);
    if (s_log && !s_ab_named) {
        char b[256], o[256], c[256];
        if (hs && hs->n) { print_set(b, sizeof b, hs); print_cores(c, sizeof c, hs); }
        else { strcpy(b, "-"); strcpy(c, "no placement"); }
        if (os && os->n) print_set(o, sizeof o, os); else strcpy(o, "no placement");
        fprintf(stderr, "[pin] tid %lu %s%d -> hybrid %s (lp %s)%s%s%s%s%s\n", GetCurrentThreadId(), s_role_names[role],
                index, c, b, t_excl_side[0] ? " exclusive" : "", s_hy_ab ? "; old side " : "", s_hy_ab ? o : "",
                s_hy_ab ? (hy ? "; now hybrid" : "; now old") : "", ok ? "" : " FAILED");
    } else if (s_log) {
        char line[1024], b[256];
        size_t n = (size_t)snprintf(line, sizeof line, "[pin] tid %lu %s%d ->", GetCurrentThreadId(), s_role_names[role], index);
        for (int j = 0; j < s_nside && n < sizeof line; j++) {
            if (ss[j] && ss[j]->n) print_set(b, sizeof b, ss[j]); else strcpy(b, "no placement");
            n += (size_t)snprintf(line + n, sizeof line - n, "%s %s %s", j ? ";" : "", s_side_name[j], b);
        }
        fprintf(stderr, "%s; now %s%s\n", line, s_side_name[k], ok ? "" : " FAILED");
    }
    if (role == HOST_ROLE_PPU_MAIN) hp_game_start();
}

void host_pin_thread(enum host_role role, int index)
{
    if (role == HOST_ROLE_PPU_MAIN) s_ppu_main_tid = GetCurrentThreadId();
    if (InterlockedCompareExchange(&s_init_state, 2, 2) != 2) host_qos_init();
    const int hot = role != HOST_ROLE_LIGHT && role != HOST_ROLE_AUDIO;
    if (s_ecoqos_off && hot && !s_hy_ecoqos) hp_ecoqos_thread();
    if (s_mmcss) {
        if (role == HOST_ROLE_WALKER || role == HOST_ROLE_PPU_MAIN) hp_mmcss(L"Games", s_mmcss >= 2);
        else if (role == HOST_ROLE_AUDIO) hp_mmcss(L"Audio", 0);
    }
    hp_prio_thread(role, index);   /* PS3_WALKER_PRIO / PS3_HOT_PRIO */
    if (s_hy_on) { hp_pin_thread_hybrid(role, index); return; }
    if (!s_pin_on) return;
    const hp_set_t* s = role_set(role, index);
    if (!s || !s->n) return;
    const BOOL ok = apply_thread(GetCurrentThread(), s, InterlockedCompareExchange(&s_ab_on, 0, 0) != 0);
    t_exclusive = ok && s->n == 1 && s_lp_hot[s->lp[0]] == 1 && !s_lp_light[s->lp[0]];
    HANDLE dup = NULL;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup,
                        THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME |
                        THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        AcquireSRWLockExclusive(&s_reg_lock);
        if (s_nreg < HP_MAX_REG) { s_reg[s_nreg].h = dup; s_reg[s_nreg].role = role; s_reg[s_nreg].index = index; s_nreg++; dup = NULL; }
        ReleaseSRWLockExclusive(&s_reg_lock);
        if (dup) CloseHandle(dup);
    }
    if (s_log) {
        static const char* names[HOST_ROLE_COUNT] = { "light", "ppu", "walker", "spu", "kernel", "audio" };
        char b[256]; print_set(b, sizeof b, s);
        fprintf(stderr, "[pin] tid %lu %s%d -> %s%s%s\n", GetCurrentThreadId(), names[role], index, b,
                t_exclusive ? " exclusive" : "", ok ? "" : " FAILED");
    }
}

int host_pin_self_exclusive(void)
{
    /* Hybrid: the side in force (its pool sets are never exclusive). */
    if (s_hy_on) return t_excl_side[InterlockedCompareExchange(&s_side, 0, 0)];
    /* Under PS3_HOST_PIN_AB the plan is off half the time: no exclusivity then. */
    return t_exclusive && InterlockedCompareExchange(&s_ab_on, 0, 0) != 0;
}

int host_kernel_spin_auto(void)
{
    if (s_spin_by_side) return s_side_spin[InterlockedCompareExchange(&s_side, 0, 0)];
    /* the A/B without a hybrid layout (fewer than 8 P-core logical processors): the rule per side, as hyb3 */
    if (s_hy_ab_spin) return s_side_old[InterlockedCompareExchange(&s_side, 0, 0)] ? !hp_small_old() : !s_hy_small;
    return !host_small_cpu();
}

int host_kernel_spin_auto_varies(void) { return s_hy_ab_spin; }

int host_cpu_hybrid(void) { return s_hy_on; }

#else  /* !_WIN32: the POSIX hosts keep their scheduler's placement */
#include "host_pin.h"
void host_qos_init(void) {}
void host_pin_thread(enum host_role role, int index) { (void)role; (void)index; }
int  host_pin_self_exclusive(void) { return 0; }
int  host_small_cpu(void) { return 0; }
int  host_kernel_spin_auto(void) { return !host_small_cpu(); }
int  host_kernel_spin_auto_varies(void) { return 0; }
int  host_cpu_hybrid(void) { return 0; }
#endif
