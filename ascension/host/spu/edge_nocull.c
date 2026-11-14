/* EDGE_NOCULL_AB=1 (experiment, EXPERIMENTS row 54): in every other [draws]
 * window module 218's EDGE jobs take their no-culling path. At the entry of
 * 51D0 the job's segment flag word (LS[r15 + 12], low 24 bits) gets bit
 * 0x800; 5280 loads that word into r86 and 53B8 skips the cull when the bit
 * is set (the test gpu_edge_shell.c's predictor copies). About a quarter of
 * the jobs already run that path. The GPU then gets every triangle and does
 * the back-face rejection itself; the SPU skips the cull and the index
 * compaction. EDGE_NOCULL=1: in every window.
 *
 * It uses the GPU-EDGE dispatch hook (g_gpu_edge_hook), so it is off when
 * EDGE_GPU is set. */
#include "spu_fnrec.h"
#include <stdio.h>
#include <stdlib.h>

typedef void (*edge_run_fn)(spu_fn_desc*, spu_context*, int);
extern void (*g_gpu_edge_hook)(spu_fn_desc* d, spu_context* ctx, int m, edge_run_fn run);
extern volatile int g_ld_nocull_on;     /* rsx_live_draw.c: toggled per [draws] window */

static volatile long long s_forced, s_seen;

static void nocull_hook(spu_fn_desc* d, spu_context* ctx, int m, edge_run_fn run)
{
    if (d->addr == 0x51D0u && g_ld_nocull_on) {
        const uint32_t r15 = ctx->gpr[15]._u32[0] & SPU_LS_MASK;
        uint8_t* q = ctx->ls + ((r15 + 12u) & SPU_LS_MASK);
        s_seen++;
        if (!(q[2] & 0x08u)) {
            q[2] |= 0x08u;
            if ((++s_forced & 0xFFFF) == 0)
                fprintf(stderr, "[edge-nocull] %lld jobs forced to the no-cull path (%lld seen in ON windows)\n",
                        s_forced, s_seen);
        }
    }
    run(d, ctx, m);
}

__attribute__((constructor)) static void edge_nocull_register(void)
{
    const char* ab = getenv("EDGE_NOCULL_AB");
    const char* al = getenv("EDGE_NOCULL");
    const int always = al && *al == '1';
    if (!(ab && *ab == '1') && !always) return;
    const char* eg = getenv("EDGE_GPU");
    if (eg && *eg && *eg != '0') { fprintf(stderr, "[edge-nocull] off: EDGE_GPU owns the hook\n"); return; }
    if (always) g_ld_nocull_on = 1;
    g_gpu_edge_hook = nocull_hook;
    fprintf(stderr, "[edge-nocull] on (%s)\n", always ? "always" : "A/B per [draws] window");
}
