/* Shims for runtime functions introduced upstream AFTER v0.15.0 (the base of this fork's ps3kit/runtime) that the pure-upstream video layer in
 * render/libs/video calls. They exist so the scene runner links; each one says exactly what it replaces. They are not implementations of the game's behaviour.
 *
 *   ps3_system_time_us      upstream runtime/syscalls/sys_timer.c (POSIX branch, copied: the host monotonic clock in microseconds)
 *   spu_taskset_round_busy  upstream runtime/spu/spu_workload.c: how many SPURS "round" tasks are busy, so the RSX can hold its next report
 *                           (GCM_REPORT_SPU_HOLD_MS). Returns 0 here: the scene runner has no SPU tasksets. For the real game this must be the real
 *                           function (a runtime migration, see android/render/MERGE_LOG.md): the stub below announces itself if it is ever asked.
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

uint64_t ps3_system_time_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

int spu_taskset_round_busy(uint32_t* taskset_ea, uint32_t* taskId)
{
    static int once; if (!once) { once = 1; fprintf(stderr, "[RENDER SHIM] spu_taskset_round_busy() called: returns 0 (no SPU tasksets); a game run needs the real runtime function\n"); }
    (void)taskset_ea; (void)taskId; return 0;
}
