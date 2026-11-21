/* LD_PSO_SAME's change counter (rsx_dispatch.c, g_rsx_dsp_pso_epoch): counts
 * a changed value of any register the pipeline choice can read, a changed
 * transform program word, a seed; not a rewrite of the same value, not the
 * registers listed as never read there. Off until enabled. Both dispatch
 * modes (method-kind table on and off). */
#include <stdio.h>
#include <string.h>
#include "rsx_dispatch.h"

static int fails;
#define EXPECT(cond, what) do { if (!(cond)) { fails++; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static unsigned bump(rsx_dispatch* r, u32 m, u32 a)
{
    const unsigned e0 = g_rsx_dsp_pso_epoch;
    rsx_dispatch_method(r, m, a);
    return g_rsx_dsp_pso_epoch - e0;
}

int main(void)
{
    static rsx_dispatch r;
    for (int table = 0; table < 2; table++) {
        rsx_dispatch_set_table(table);
        rsx_dispatch_init(&r, NULL);
        if (table == 0) {
            /* Off: nothing counted. */
            EXPECT(bump(&r, 0x0208, 0x121) == 0, "off: RT_FORMAT change not counted");
            rsx_dispatch_pso_epoch_enable();
        }
        EXPECT(bump(&r, 0x0208, 0x122) == 1, "RT_FORMAT change");
        EXPECT(bump(&r, 0x0208, 0x122) == 0, "RT_FORMAT same value");
        EXPECT(bump(&r, 0x1A04, 0x12345) == 1, "texture format change");
        EXPECT(g_rsx_dsp_pso_last == (0x1A04u >> 2), "last register");
        EXPECT(bump(&r, 0x08E4, 0x1000) == 1, "fragment program address");
        EXPECT(bump(&r, 0x1D60, 0x40) == 1, "shader control");
        /* Never read by get_pso. */
        EXPECT(bump(&r, 0x1A00, 0x5000) == 0, "texture 0 offset quiet");
        EXPECT(bump(&r, 0x1A00 + 15 * 0x20, 0x7000) == 0, "texture 15 offset quiet");
        EXPECT(bump(&r, 0x1680 + 4 * 3, 0x8000) == 0, "vertex array offset quiet");
        EXPECT(bump(&r, 0x181C, 0x9000) == 0, "index array address quiet");
        EXPECT(bump(&r, 0x1D6C, 0x10) == 0, "semaphore offset quiet");
        EXPECT(bump(&r, 0x1D70, 0x11) == 0, "semaphore release quiet");
        EXPECT(bump(&r, 0x1800, 0x12) == 0, "report quiet");
        EXPECT(bump(&r, 0x1EFC, 7) == 0, "constant slot quiet");
        EXPECT(bump(&r, 0x1F00, 0x3F800000) == 0, "constant upload quiet");
        EXPECT(bump(&r, 0x1F7C, 0x40000000) == 0, "constant upload end quiet");
        EXPECT(bump(&r, 0x1808, 5) == 0 && bump(&r, 0x1808, 0) == 0, "begin/end quiet");
        /* Neighbours of quiet registers are not quiet. */
        EXPECT(bump(&r, 0x1A08, 0x30303) == 1, "texture 0 address word");
        EXPECT(bump(&r, 0x1740, 0x22) == 1, "vertex format");
        EXPECT(bump(&r, 0x1EA0, 3) == 1, "vertex program start");
        /* Transform program words: counted when they change. */
        EXPECT(bump(&r, 0x1E9C, 0) == 0, "upload slot quiet");
        const unsigned e0 = g_rsx_dsp_pso_epoch;
        for (u32 w = 0; w < 8; w++) rsx_dispatch_method(&r, 0x0B80 + w * 4, 0x11110000u + w + 100u * table);
        EXPECT(g_rsx_dsp_pso_epoch - e0 == 8, "8 new program words");
        EXPECT(r.vp[0] == 0x11110000u + 100u * table && r.vp[7] == 0x11110007u + 100u * table, "program stored");
        rsx_dispatch_method(&r, 0x1E9C, 0);
        const unsigned e1 = g_rsx_dsp_pso_epoch;
        for (u32 w = 0; w < 8; w++) rsx_dispatch_method(&r, 0x0B80 + w * 4, 0x11110000u + w + 100u * table);
        EXPECT(g_rsx_dsp_pso_epoch == e1, "same program words again");
        rsx_dispatch_method(&r, 0x1E9C, 1);
        EXPECT(bump(&r, 0x0B84, 0x2222) == 1, "one changed word in slot 1");
        /* Vertex constant (current attribute) writes go through vtx_const_set. */
        EXPECT(bump(&r, 0x1940 + 4 * 2, 0xFF00FF00u) >= 1, "DATA4UB changes the current attribute");
        EXPECT(bump(&r, 0x1940 + 4 * 2, 0xFF00FF00u) == 0, "DATA4UB same value");
        /* Seeds count. */
        { const unsigned s0 = g_rsx_dsp_pso_epoch; u32 regs[4] = { 1, 2, 3, 4 };
          rsx_dispatch_seed_registers(&r, regs, 4);
          EXPECT(g_rsx_dsp_pso_epoch != s0, "seed registers");
          const unsigned s1 = g_rsx_dsp_pso_epoch;
          rsx_dispatch_seed_transform_program(&r, regs, 4);
          EXPECT(g_rsx_dsp_pso_epoch != s1, "seed program"); }
    }
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
