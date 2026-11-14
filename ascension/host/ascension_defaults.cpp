// Runtime defaults for God of War: Ascension, set before main() runs.
//
// Each is an environment switch ps3recomp reads on first use. A value already
// set in the environment wins, so any of them can still be turned off for a
// run (set it to 0).
//
//   PS3_EDGE_ZLIB_HLE  the Edge Zlib inflate queue is serviced on the host:
//                      the SPU path cannot serve it yet, and without it the
//                      archives never load.
//   RSX_LIVE_DRAW      the live NV4097 draw engine. The default D3D12 path
//                      records none of this title's draws (every present
//                      had draws=0: a black window); the live engine draws
//                      the intro screens correctly.
//   GCM_PARK_WAIT_MS   how long a parked FIFO waits to be patched before the
//                      runtime follows `put` (ps3recomp's default is 50 ms).
//                      This title's SPU jobs fill a frame and patch the park
//                      after `put` has moved; following it early skips the
//                      frame.
//   GCM_SEMA_ACQUIRE   a FIFO SEMAPHORE_ACQUIRE really waits. After each
//                      frame's reports the GPU waits for the label the SPU
//                      post-process tasks write when they finish the frame;
//                      without the wait it runs ahead, RsxSync overwrites the
//                      tasks' descriptor before the slow ones read it, and a
//                      round loses its decrements.
//   GCM_PARK_SPU_HOLD_MS  a park the vblank handler has patched (the next
//                      frame's segment) is held while SPURS tasks are still
//                      running, up to this long. The first frame has no
//                      post-process ACQUIRE: the game relies on the RSX taking
//                      a frame's time, and without the hold frame 2's reports
//                      landed while round 1 still ran, so round 2 never began.
//   GCM_REPORT_SPU_HOLD_MS  a GET_REPORT waits, up to this long, while SPURS
//                      tasks are out of WAIT_SIGNAL or have a signal pending.
//                      RsxSync turns each report into a signal for the
//                      post-process tasks; when the PPU side is quick (job
//                      module 214 lifted) the GPU's next report reached
//                      RsxSync before the tasks had started the round, its
//                      signal was eaten by their barriers, and the game
//                      stopped at the end of loading every time.
//                      2000 ms, not 50 (2026-11-09, host/mlaa/NOTES.md): the
//                      hold lasts only while tasks run, so its length is the
//                      round's. With SPU MLAA on (the patch "Disable SPU MLAA"
//                      unticked) the first round takes 22-26 ms on a 9950X3D,
//                      59-76 ms on 8 of its cores slowed to half speed, and
//                      over 50 ms on a Ryzen 7 7840HS laptop (its logs: the
//                      hold gave up at 46-49 ms); at 50 ms the hold let the
//                      next frame's report signal tasks still in round 1 and
//                      the game hung at its first frames (the walker waiting
//                      for label 0x510 = 2 for good). The same at 10 ms here.
//                      The runtime's GCM_REPORT_SPU_HOLD_RING (on by default
//                      with GCM_ROUND_RING) also holds a frame's first ring
//                      report while the previous frame's round still runs.
//   GCM_ROUND_GATE     with RPCS3's RSX, the placeholder report after each
//                      frame report waits until RsxSync has read every frame
//                      report it was polling and the post-process round in
//                      its descriptor (0x9BAC30) has written its label. A
//                      PS3's RSX takes a frame's time, so a round always ends
//                      before the next one is signalled; a recompiled one
//                      does not, and the next round's signal was lost in the
//                      tasks' barriers (the Mac never reached the loading
//                      screen). It waits on the game's own progress, not on
//                      a clock.
//   GCM_ROUND_RING     RsxSync's ring of 18 frame reports (0x1101ED00): only
//                      those are watched. Another job polls reports at
//                      0x1124xxxx/0x1126xxxx without reading them again, and
//                      the gate waited a second for each. RsxSync reads its
//                      reports in order, so the one after the last it read is
//                      awaited even before it polls it (on Windows the RSX
//                      wrote it while RsxSync still acted on the one before).
//   SPURS_READY_LEVEL  readyCount is a level, as the real kernel treats it.
//                      The Wws job managers zero their own count once their
//                      jobs are stored; consuming it per admission left
//                      RsxSync's round-2 job unrun until a later kick, which
//                      never came (the render thread waits on frame slot
//                      0x99AAC8 for it).
//   SPURS_MAXCONT      up to five SPUs on the main menu's Wws job lists
//                      (wid 3, 16 jobs a frame, and the EDGE lists, wid
//                      15-17, one a frame), which the game gives one SPU
//                      each. Recompiled SPU code runs them several times
//                      slower than a real SPU, and one after the other they
//                      held the menu at 14 fps; shared, it runs at ~33. The
//                      Wws manager keeps a progress lane per SPU, so a list
//                      is built to be shared. In the first level wid 13 ran
//                      on one SPU half the time while the PPU waited; up to
//                      three: ~14.5 -> ~15.4 fps. Six on every list hung it.
//   SPURS_IDLE_BACKOFF_MAX  0: a workload that ran and found nothing is not
//                      skipped for up to 16 kernel passes (~16 ms) after;
//                      work the PPU stores meanwhile waited for it. First
//                      level ~13.4 -> ~14.5 fps.
//   SPU_REJOIN         the interpreter rejoins job module 204's lifted code
//                      (jobmod_31107645db58, the level-load job) between LS
//                      0x9700 and 0x1C000, which it otherwise only does with
//                      interrupts off. Run interpreted (~95M instructions per
//                      5 s) it held the main loop at ~55 ms an iteration: the
//                      loading movies played at 50-75% with stuttering audio.
//   SPU_REJOIN_CALL    that rejoined code runs in a drain of the interpreter's
//                      own, so its host return resumes interpretation at the
//                      right pc instead of unwinding past the interpreted
//                      caller.
//   LD_PERF_OVERLAY    frame rate, frame time, speed, CPU and GPU use in the
//                      top-right corner of the window.
//   PS3_VERBOSE=0      the per-frame log lines (task creation and joins, SPU
//                      dispatch, viewport changes, events) stay quiet: every
//                      guest thread writes them through one FILE lock, and
//                      they cost the main thread several percent of its time.
//                      Set PS3_VERBOSE=1 to get them back when investigating.
//
// The OS environment is used directly, not getenv(): the boot harness routes
// getenv through a cache that also remembers a miss, so asking first would pin
// the variable as unset before it is set.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <stdlib.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string>

extern "C" int ascension_patch_on(const char* name);   // host/ascension_patches.cpp

namespace {

void set_default(const char* name, const char* value)
{
#ifdef _WIN32
    if (GetEnvironmentVariableA(name, nullptr, 0)) return;
    SetEnvironmentVariableA(name, value);
    // The CRT's own copy too: the runtime's cached getenv reads the OS
    // environment, but a build without that cache (the instrumented PGO
    // build, PS3_PGO_GEN) uses the CRT's getenv, which never sees
    // SetEnvironmentVariable.
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

struct AscensionDefaults {
    AscensionDefaults()
    {
        // The game advances its clock by a fixed 1/30 s per frame: faster
        // frames are faster gameplay. With the Unlock FPS patch (ported to
        // 1.00, patches-rpcs3\ascension-1.00.yml) switched on (PS3_PATCHES,
        // host/ascension_patches.cpp) the clock follows real time and the
        // limit is 60.
        const bool unlocked = ascension_patch_on("Unlock FPS") != 0;
        set_default("PS3_FPS_LIMIT", unlocked ? "60" : "30");
        set_default("LD_PERF_OVERLAY_FPS", unlocked ? "60" : "30");
        if (unlocked) set_default("LD_PERF_OVERLAY_REALTIME", "1");
        // Prerendered movies are skipped (the game's own skip, CROSS, pressed
        // while one decodes). PS3_SKIP_MOVIES=0 plays them.
        set_default("PS3_SKIP_MOVIES", "1");
        // The FIFO walker's "circling" heuristic (for titles that drive the
        // RSX from a second buffer) mistook this game's heavy combat frames
        // for a loop, skipped to `put` past hundreds of fences, and hung.
        set_default("GCM_FIFO_CIRCLE_PASSES", "0");
        set_default("PS3_EDGE_ZLIB_HLE", "1");
        set_default("RSX_LIVE_DRAW", "1");
        // The previous draw's pipeline when nothing get_pso reads changed,
        // now also with a depth-format unit bound (its zeta's state hashed)
        // and with the depth-bounds range, vertex formats and texcoord
        // controls not counting as changes. Combat 2026-10-23: 71% of draws
        // reuse (19.5% before), get_pso 0.78 -> 0.52 us a draw; verify mode
        // (LD_PSO_SAME_VERIFY=1) found 0 mismatches in ~1.1M checked draws.
        // 2026-10-24: the user's next-stage enemies (pigs, harpy) once drew
        // without their colours; it looked tied to this, but the user then
        // saw it happens only sometimes after an area load (and once on an
        // older build), and LD_PSO_SAME_VERIFY=1 found 0 mismatches while
        // fighting those enemies. Fixed instead as a stuck texture
        // (FS_MARK_WRITTEN, LD_TEX_AUDIT).
        set_default("LD_PSO_SAME", "1");
        // The live draw's per-draw cost cuts (host/video/DRAW-COST-NOTES.md):
        // vertex-constant blocks deduplicated (+ LD_CB_MASK), the pipeline
        // choice kept by the fragment program's content and the cull/winding
        // registers, fragment-program analyses by content, the vertex layout
        // kept per program, render targets / root arguments not set again,
        // 16-bit indices. Off in ps3kit (other titles untested). 2026-11-05,
        // satyr-prison rooftop with OCCL=2: walker -13..-18% a frame; verify
        // (DRAWOPT_VERIFY=16) 0 mismatches there and in the heavy fight.
        // DRAWOPT=0 turns them all off, DRAWOPT_<X>=0 one.
        set_default("DRAWOPT", "1");
        // Occlusion culling with the game's own answers (OCCL=2; host/video/
        // OCCLUSION-NOTES.md "Step 2", MERGE-0117A-NOTES.md): the Wws job
        // manager polls each ZPASS report of the RenderJobs banks while it
        // reads "pending"; the renderer leaves it pending, submits the queries
        // early and writes the true count when it resolves (a watchdog answers
        // "visible" after 8 ms). Hidden objects skip their colour passes:
        // rooftop +7.8/+7.8/+9.5% fps at 4/6/all cores, heavy-fight wall views
        // +6.5/+15.8/+17.1%. The bank addresses are this title's, so it is
        // off in ps3kit. The launcher passes OCCL=2 or OCCL=0 (its "occlusion"
        // setting); OCCL=0 turns it off.
        set_default("OCCL", "2");
        // The depth-EQUAL precision check, sampled (LD_EQ_CHECK=2; host/video/
        // MERGE-0117A-FLOOR-NOTES.md): one frame every 10 s in play draws each
        // depth-EQUAL draw five times more, writing nothing (variants built by
        // one lowest-priority thread, never stored), and at exit the log gets
        // an [eq-pipe] line per pipeline and a one-line verdict against the
        // NVIDIA reference -- so a tester's ordinary log says whether the
        // colour pass finds the depth pass's depth on that GPU (the AMD
        // missing-floor question). The reference table is this title's, so it
        // is off in ps3kit. LD_EQ_CHECK=0 turns it off, =1 every frame (slow).
        set_default("LD_EQ_CHECK", "2");
        // A 1:1 blit the GPU copied between live surfaces into VRAM skips its
        // guest-memory copy (the renderer reads the GPU copy; copies into main
        // memory keep it). Heavy fight 2026-10-30: walker -2.9% a draw, 28.9 vs
        // 28.2 fps (EXPERIMENTS row 50).
        set_default("LD_BLIT_GUEST_SKIP", "1");
        // Native EDGE job front end (host/spu/wip/hle, 2026-10-31): module 218's
        // jobs run as block-merged host code calling the natives directly; exact on
        // 2,840 recorded jobs, heavy fight about -4% frame time (in-run A/B).
        set_default("EDGE_HLE", "1");
        // EDGE geometry jobs on all six SPUs (host/spu/wip/edge6, 2026-10-31): SPUs
        // 4-5 get their own output rings and labels in an extra 8 MB IO region (the
        // title only built them for SPUs 0-3, which froze it on the laptop). Heavy
        // fight +1.2 fps in-run on top of the front end (+3 on 0.1.2); no freeze in
        // 16 boots. EDGE6=0: geometry on SPUs 0-3 as the title set it up.
        set_default("EDGE6", "1");
        // The WWS policy module's in-job interrupt as host code (host/spu/wip/pm_v1,
        // 2026-10-31): exact on 2,840 recorded jobs, EDGE time -1.4% in-run, 4 clean
        // fight runs. EDGE_PM=0: the lifted/interpreted vector as before.
        set_default("EDGE_PM", "1");
        // Lock-line lock as 64 stripes by line (runtime/spu/spu_coherency.c,
        // 2026-10-31): stress test 0 violations, 6 clean soak runs, heavy-fight SPU
        // time -1.5..-2.8% in-run. SPU_LL_STRIPE=0: the single global lock.
        set_default("SPU_LL_STRIPE", "1");
        // The stale-surface check only counts unless set to 2; at 1 it read the
        // write watch over whole surfaces every frame (row 51).
        set_default("LD_SURF_GUEST_NEWER", "0");
        // How long the FIFO walker waits on a self-JUMP park for the SPU job that
        // patches it before it follows `put`. Every such resync in this game skipped
        // the frame's render-slot release and froze it for good (sound still on). At
        // 2000 a tester's 6-core PC froze in 2 runs of 2 (2026-11-01); here, all six
        // SPU threads held 3 s mid-fight (PS3_SPU_STALL_TEST=45,3000) froze at 2000
        // and paused 3.0 s, then ran at 60 fps, at 60000.
        set_default("GCM_PARK_WAIT_MS", "60000");
        set_default("GCM_SEMA_ACQUIRE", "1");
        set_default("GCM_PARK_SPU_HOLD_MS", "1000");
        set_default("GCM_REPORT_SPU_HOLD_MS", "2000");
        set_default("GCM_ROUND_GATE", "9BAC30");
        set_default("GCM_ROUND_RING", "1101ED00,18");
        set_default("SPURS_READY_LEVEL", "1");
        // The game's own max contention for the RenJobSet lists (wids 9-12),
        // GeneralJob 7, RenderJobs 13 and MeshProc 14 (dropped by
        // _cellSpursWorkloadAttributeInitialize). At 1 SPU each the
        // RenJobSet lists finished late and pushed RenderJobs -> MeshProc ->
        // EDGE into the RSX park. Combat 2026-10-23: 43 -> 52 fps, park
        // 12 ms -> 7.2 ms (3 runs, no hangs).
        // wid 0 (Wws_Job CollisL, the game asks 5): its job lists ran on one
        // SPU while the others idled, and the main thread spins on their lane
        // counters (func_00466274) -- 39% of its time in the next stage's
        // heavy area. 2026-10-24 heavy spot: 41 -> 50.8 fps median, SPU slots
        // empty while parked 4150 -> 520 ms per 5 s, no hangs.
        set_default("SPURS_MAXCONT", "0=5,3=5,7=6,9=8,10=8,11=8,12=8,13=6,14=5,15=5,16=5,17=5");
        // Idle workloads back off (up to 2^2 x SPURS_IDLE_BASE_US) and a free
        // SPU goes to a workload with work before an idle re-run. With the
        // spinning kernel, a cap of 0 re-admitted idle workloads ~200,000
        // times a second. Combat A/B (2026-10-22, same scene, 2 runs each):
        // 40.8 -> 45.9 fps on the build with the faster SPU code.
        set_default("SPURS_IDLE_BACKOFF_MAX", "2");
        set_default("SPURS_KICK_FIRST", "1");
        // Priority scheduling, with the RenJobSet lists (wids 9-12) one step
        // ahead of the EDGE lists (7 vs 8). After the main thread hands the
        // RSX its frame (the park's end) it waits in its job graph for
        // RenJobSet 9 -> 10 -> 11 -> 12 -> CollisL before it computes the next
        // frame; the second EDGE list, kicked at the same moment, held SPUs
        // 0-3 and BrBDblBuf SPU 5, so RenJobSet started on one SPU. Combat
        // 2026-10-23 (3 runs each, no hangs): 53.3 -> 57.5 fps, park
        // ~8 -> ~5.7 ms. SPURS_PRIO_SCHED=0 restores the old order (the
        // SPURS_PRIO overrides only act under priority scheduling).
        set_default("SPURS_PRIO_SCHED", "1");
        set_default("SPURS_PRIO", "9=7,10=7,11=7,12=7");
        // A PPU store into a line an SPU has reserved notifies through a
        // packed (line, context) table instead of walking all ~64-128
        // registered SPU contexts, one cache miss each, under the lock-line
        // lock (runtime/spu/spu_coherency.c). The main thread spent ~16% of
        // its frame work there. Combat 2026-10-23 (2 runs each, same exe, no
        // hangs): 53.9 -> 58.1 fps, park 6.0 -> 4.6 ms. SPU_COH_DENSE=0: the
        // list walk.
        set_default("SPU_COH_DENSE", "1");
        // The VRAM manager's copy/move abandon a call whose extent the guest
        // itself rejects (where a PS3 would hit `tw` and stop) instead of
        // walking 4 GB in 16 MB steps every frame (host/ascension_vram_guard.cpp;
        // the hooks are in the lifted code via tools/vram_guard_patch.py --
        // rerun it after a relift). The bad extent comes from a texture block
        // shrunk twice (negative size), seen in ~30% of enemy-area runs of the
        // lean build: CopyLocalMemory/MoveLocalMemory "offset error" floods,
        // fps ~12 or no flips. 2026-10-24: with a forced double shrink
        // (PPU_VRAM_FAULT=300) the flood came back without the guard (3374
        // lines) and not with it (4 runs); 5 classic runs with it on, 0 hangs,
        // 58.3-59.1 fps. PPU_VRAM_GUARD=0: the old behaviour.
        // OFF (2026-10-24 08:00): in the user's real hit (user13, lean) the
        // skipped moves were "past the end of VRAM" compactions of 16 MB of
        // textures; skipping them left the textures where the manager no
        // longer looks -- garbled textures everywhere -- and a guest thread
        // later spun on a null pointer (HOTREAD8 on 0x0, CPU at full load).
        // A guard is no fix; the double shrink itself has to be found.
        set_default("PPU_VRAM_GUARD", "0");
        // Stopgap for the batch-job freeze: a DMA-list element real hardware
        // would fault on (module 216's command-stream lists caught with one
        // stale quadword) is re-read from the main memory the list came from,
        // when that copy is well-formed (spu_dma.h, spu_channels.c).
        set_default("SPU_LIST_REFETCH", "1");
        // Freeze hunt (2026-10-23): SPU_WTRACE=12300000-12340000 records every
        // write into the batch-job buffer (0x1232xxxx in all three freezes
        // seen) so a bad module-216 DMA-list element prints who wrote it
        // (runtime/spu/spu_wtrace.c). Off by default: ~10% fps in combat.
        // The SPURS kernel looks for ready workloads continuously instead of
        // every 1 ms: work an SPU atomic or a PPU store makes ready starts at
        // once. One host core; combat ~28.2 -> ~29.2 fps. "auto": not with
        // fewer than 8 cores, where that core is one the SPU workers need
        // (4 cores 23.8 -> 35.2 fps with placement off too; host_small_cpu).
        set_default("SPURS_KERNEL_SPIN", "auto");
        // 2026-11-06 (host\spu\SCHED-NOTES.md): the spinning kernel yields with Sleep(0) instead of
        // SwitchToThread, so another program's busy threads no longer take its CPU for whole time slices
        // (heavy fight + 16 foreign busy threads: 43-60 fps vs 13-25). SPURS_KERNEL_YIELD=0: the old yield.
        // OFF again 2026-11-07 ~00:40: the integrator's heavy fight with 16 busy foreign threads collapsed to
        // 11-14 fps with user77 (yield 1) vs ~55 with user75 (old yield) -- contradicts SCHED D1; until settled, 0.
        set_default("SPURS_KERNEL_YIELD", "0");
        // 2026-11-09 (host\cpu\NOTES.md 2): the flip-pacing wait (func_00375A50, usleep(30) at lr 0x375A78, polling
        // the RSX ref) blocks until a fence is published or the ref read gate opens instead of yield-spinning: it
        // was 119-356 ms a second of the PPU main thread at the 60 cap. The guest reads the same values in the same
        // order (runtime/syscalls/sys_timer.c, libs/video/cellGcmSys.c cellGcm_ref_wait).
        // OFF by default for 0.1.24 (2026-11-10 ~03:15): under other processes' bursts the blocked PPU main is
        // rescheduled late (ready share 0.163 vs 0.029); in-run A/B (CPU_WAIT_AB, user96, 6 cores, rooftop) frames
        // >= 100 ms 12.0/min on vs 6.9 off, and the cpu helper saw more >= 100 ms frames on the on side in 3 of 4
        // runs. Opt-in: SYS_USLEEP_REFWAIT=375A78 (saves ~300 ms/s of the PPU main at the 60 cap).
        set_default("SYS_USLEEP_REFWAIT", "0");
        // The graphics walker thread at THREAD_PRIORITY_HIGHEST: below 8 cores the SPU workers took its CPU
        // 14% of the time (4 cores, rooftop: +6% in-run, 8/8 pairs). PS3_WALKER_PRIO=0: normal priority.
        // HIGHEST (SCHED-NOTES E, 2026-11-07): +5.3% at 4 cores, +0.8% at 6, +0.2% at 16; 3/3 freeze checks clean.
        set_default("PS3_WALKER_PRIO", "2");
        // The whole game above other programs (2026-11-07, user: "if the cpu is used by other program the game
        // should not drop fps"): ABOVE_NORMAL priority class (host_pin.c), so background programs get what the
        // game leaves. PS3_PROCESS_PRIORITY=0: normal.
        set_default("PS3_PROCESS_PRIORITY", "1");
        // The audio mixer at TIME_CRITICAL (2026-11-07, host\AUDIO-NOTES.md): at NORMAL it sat at the same level as
        // the SPU workers in the ABOVE_NORMAL process (below the HIGHEST walker) and woke 26-88 ms late on a full CPU:
        // the 22 ms WASAPI buffer ran dry = the crackling players heard in fights. 3 cores: 7-38 empty-buffer events
        // per run -> 0 (4 runs), fps unchanged. AUDIO_PRIO=0: normal priority.
        set_default("AUDIO_PRIO", "15");
        // Elastic audio (2026-11-09, host\aud2\NOTES.md; RPCS3's buffering idea): a port block the game's sound job
        // (BrBDblBufWrkld, SPU 5) has not written yet is waited for while the device buffer still has sound, instead of
        // being played as silence; past the floor it fades out/in. Injected 60 ms late every 2 s: holes 360-423 -> 0.
        // AUDIO_BUF_MS=40 (+18 ms output latency; 60 = +38 ms covers later sound but is unchecked for lip sync).
        // AUDIO_ELASTIC=0: the old consume. AUDIO_ELASTIC_TS stays off (real timestamps, untested; never called).
        set_default("AUDIO_ELASTIC", "1");
        set_default("AUDIO_BUF_MS", "40");
        // The SPU worker running the sound job (wid 8) gets HIGHEST priority while it runs it (2026-11-11, host\q4
        // SOUND CHECK SETTLED): weak-laptop sim, 2 alternated plain pairs: concealed sound 4.28% / 1.79% -> 0.00% /
        // 0.00%, frames >= 100 ms 85 / 23 -> 6 / 9 per min, median fps unchanged. SPURS_BOOST_WID=-1: off.
        set_default("SPURS_BOOST_WID", "8=2");
        // A block the sound job still has not written when the device needs it (elastic concealment) repeats the
        // last block (gain 1, 0.6, 0.3; joined over 64 frames) instead of fading to silence (2026-11-10, q4 SOUND):
        // sound dropouts in the weak-laptop sim 12.5 -> 1.1 and 4.0 -> 0.6 per 5 s (two runs, before the quieter
        // repeats got their fade-out). AUDIO_CONCEAL_REPEAT=0: fade.
        set_default("AUDIO_CONCEAL_REPEAT", "1");
        // SPU n on physical core n + 1, alone: its contexts' local stores and
        // lifted code stay in that core's caches. First level ~14.8 -> ~15.6
        // fps, module 218's job 72 -> 65 us.
        set_default("SPURS_PIN", "1");
        // The main PPU thread on core 0, which SPURS_PIN leaves free: it
        // waits on the SPUs and kicks their next jobs. ~15.8 -> ~16.7 fps.
        set_default("PS3_PIN_PPU", "1");
        set_default("SPU_REJOIN", "204:9700-1C000");
        set_default("SPU_REJOIN_CALL", "1");
        set_default("LD_PERF_OVERLAY", "1");
        // Picture (2026-10-23): the live renderer draws at 2x (2560 x 1440;
        // RPCS3-style resolution scaling, rsx_live_draw.c g_ld_rs) with SMAA
        // 1x on the finished frame. First level, 1x vs 2x + SMAA: 52 vs 53 fps
        // median (the GPU is not the limit). The launcher passes its own
        // choice (PS3_RENDER_SCALE 100/150/200/300, LD_SMAA); LD_RES_SCALE
        // overrides both.
        set_default("PS3_RENDER_SCALE", "200");
        set_default("LD_SMAA", "1");
        // RCAS sharpening at the window's size, at the strength the user
        // runs RPCS3 with (its "RCAS Sharpening Strength" 80%).
        set_default("LD_SHARPEN", "80");
        // Colour vibrance + a little contrast in the same final pass (the user
        // asked for colour that pops more); 0 turns it off.
        set_default("LD_VIBRANCE", "50");
        // RPCS3's performance overlay in the launcher's style: its Cinzel and
        // palette (text, panel, gold titles).
        set_default("PS3_OVERLAY_COLORS", "#E2B86AFF,#0B090899,#E2B86AFF,#00000000");
        set_default_overlay_font();
        set_default("PS3_VERBOSE", "0");
    }

    // launcher/Cinzel-Black.ttf beside a build in build/<platform>/ (the
    // launcher passes its own copy).
    static void set_default_overlay_font()
    {
        char exe[4096] = {0};
#ifdef _WIN32
        if (!GetModuleFileNameA(nullptr, exe, sizeof(exe))) return;
#elif defined(__APPLE__)
        uint32_t n = sizeof(exe);
        if (_NSGetExecutablePath(exe, &n)) return;
#else
        if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) <= 0) return;
#endif
        std::string dir = exe;
        for (int up = 0; up < 3; up++) {
            const auto slash = dir.find_last_of("/\\");
            if (slash == std::string::npos) return;
            dir.resize(slash);
        }
        const std::string font = dir + "/launcher/Cinzel-Black.ttf";
        if (FILE* f = fopen(font.c_str(), "rb")) {
            fclose(f);
            set_default("PS3_OVERLAY_FONT", font.c_str());
        }
    }
} s_defaults;

}  // namespace

// Lifted SPU functions left to the interpreter unless SPU_NOLIFT says
// otherwise. None now: job module 214 (jobmod_e9f3f00d551e) from 0x4418 used
// to be kept interpreted because lifting it stopped the game at the end of
// loading; that was the report race GCM_REPORT_SPU_HOLD_MS closes (the lift
// itself checked out under SPU_SHADOW). Read by the runtime's registration,
// which runs in static constructors, so it is a constant and not an
// environment default.
extern "C" const char* const spu_title_nolift = nullptr;

// Before booting (boot_main.cpp, ASCENSION_PRE_MAIN): SPU_FN_REPLAY=<dir>
// replays recorded SPU function runs (host/spu/spu_fnrec.c) and exits.
extern "C" int spu_fnrec_replay_main(void);
extern "C" int ascension_patch_list_main(void);   // PS3_PATCH_LIST=1 (host/ascension_patches.cpp)
extern "C" uint32_t ppu_hle_inject_base;   // ps3kit libs/video/cellGcmSys.c (vm.h VM_HLE_INJECT_BASE)
extern "C" int ascension_pre_main(void)
{
    // The runtime's GCM label / control / offset-table window (0x8000 bytes) sat at 0x20000000, right above the
    // title's mmapper heap (0x11000000 up): a tester's session mapped 0x1E200000..0x20100000 after ~12 min, the
    // title's own data landed on the labels and the walker waited forever on label 0x10 (picture frozen, sound on;
    // "iooffset == -1" just before; tester log 2026-11-11). Moved past the EDGE6 rings (0x2E000000, 8 MB) and below
    // the raw-SPU windows (0x30000000). PS3_INJECT_BASE=<hex>: another place (20000000 = the old one).
    { const char* e = getenv("PS3_INJECT_BASE");
      const uint32_t b = e && *e ? (uint32_t)strtoul(e, nullptr, 16) & ~0xFFFFu : 0x2F000000u;
      if (b) ppu_hle_inject_base = b;
      fprintf(stderr, "[gcm] label window at 0x%08X\n", ppu_hle_inject_base); }
    const int rc = ascension_patch_list_main();
    if (rc >= 0) return rc;
    return spu_fnrec_replay_main();
}
