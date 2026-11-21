/*
 * ps3recomp - NV4097 method dispatcher (Track B / LAYER 2)
 *
 * See rsx_dispatch.h for the design and the clean-room source list.
 *
 * Register/method facts used here (envytools rnndb nv30-40_3d + Mesa nv30
 * + psdevwiki; RPCS3 consulted as a read-only semantics oracle):
 *
 *   0x0194 DMA_COLOR0, 0x018C DMA_COLOR1, 0x01B4/0x01B8 DMA_COLOR2/3,
 *   0x0198 DMA_ZETA                DMA handle 0xFEED0000 = RSX local memory,
 *                                  0xFEED0001 = main (IO-mapped) memory
 *   0x0200 RT_HORIZ  x[0:15] w[16:31]      0x0204 RT_VERT y/h
 *   0x0208 RT_FORMAT color[0:4] depth[5:7] type[8:11] aa[12:15]
 *   0x020C/021C/0280/0284 COLOR0-3_PITCH   0x0210/0218/0288/028C COLOR0-3_OFFSET
 *   0x0214 ZETA_OFFSET   0x022C ZETA_PITCH   0x0220 RT_ENABLE
 *   0x0A00 VIEWPORT_HORIZ x/w   0x0A04 VIEWPORT_VERT y/h
 *   0x0A20..0x0A2C VIEWPORT_TRANSLATE[4]  0x0A30..0x0A3C VIEWPORT_SCALE[4]
 *   0x0B80..0x0BFC VP_UPLOAD_INST window (load ptr auto-advances per vec4)
 *   0x1680+i*4 VTXBUF_OFFSET[i]  bit31 = location, [0:30] = offset
 *   0x1840+i*4 TEX_SIZE1[i] (control3) pitch[0:19] depth[20:31]
 *   0x1A00+i*0x20 fragment texture unit i (16 units, 8 words each):
 *     +0x00 TEX_OFFSET   +0x04 TEX_FORMAT dma[0:1] (1 = local, 2 = main)
 *                          cube[2] border[3] dims[4:7] fmt[8:15] mips[16:19]
 *     +0x08 TEX_WRAP     +0x0C TEX_ENABLE (control0, enable = bit 31)
 *     +0x10 TEX_SWIZZLE (component remap)  +0x14 TEX_FILTER
 *     +0x18 TEX_NPOT_SIZE height[0:15] width[16:31]  +0x1C TEX_BORDER_COLOR
 *   0x1738 VERTEX_DATA_BASE_OFFSET  0x173C VB_ELEMENT_BASE
 *   0x1740+i*4 VTXFMT[i] type[0:3] size[4:7] stride[8:15] frequency[16:31]
 *   0x1808 VERTEX_BEGIN_END  arg = primitive, 0 = end
 *   0x1814 VB_VERTEX_BATCH   first[0:23] (count-1)[24:31]
 *   0x1818 INLINE_ARRAY     vertex data pushed through the FIFO itself
 *   0x181C IDXBUF_OFFSET     0x1820 IDXBUF_FORMAT location[0:3] type[4:11]
 *   0x1824 VB_INDEX_BATCH    first[0:23] (count-1)[24:31]
 *   0x1D8C CLEAR_DEPTH_VALUE  0x1D90 CLEAR_COLOR_VALUE (A8R8G8B8)
 *   0x1D94 CLEAR_BUFFERS     Z=0x01 S=0x02 R=0x10 G=0x20 B=0x40 A=0x80
 *   0x1E9C VP_UPLOAD_FROM_ID (transform program load pointer)
 *   0x1EFC VP_UPLOAD_CONST_ID (transform constant load pointer; NOT
 *          auto-advancing — every burst re-reads it)
 *   0x1F00..0x1F7C VP_UPLOAD_CONST window: word i -> slot load + i/4, lane i%4
 *   0xE944 gcm driver-queue flip (established empirically from this title)
 */

#include "rsx_dispatch.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define M_DMA_COLOR1            0x018C
#define M_DMA_COLOR0            0x0194
#define M_DMA_ZETA              0x0198
#define M_DMA_COLOR2            0x01B4
#define M_DMA_COLOR3            0x01B8
#define M_RT_HORIZ              0x0200
#define M_RT_VERT               0x0204
#define M_RT_FORMAT             0x0208
#define M_COLOR0_PITCH          0x020C
#define M_COLOR0_OFFSET         0x0210
#define M_ZETA_OFFSET           0x0214
#define M_COLOR1_OFFSET         0x0218
#define M_COLOR1_PITCH          0x021C
#define M_RT_ENABLE             0x0220
#define M_ZETA_PITCH            0x022C
#define M_COLOR2_PITCH          0x0280
#define M_COLOR3_PITCH          0x0284
#define M_COLOR2_OFFSET         0x0288
#define M_COLOR3_OFFSET         0x028C
#define M_VIEWPORT_HORIZ        0x0A00
#define M_VIEWPORT_VERT         0x0A04
#define M_VIEWPORT_TRANSLATE    0x0A20
#define M_VIEWPORT_SCALE        0x0A30
#define M_VP_UPLOAD_INST        0x0B80
#define M_VTXBUF_OFFSET         0x1680
#define M_VERTEX_DATA_BASE      0x1738
#define M_VB_ELEMENT_BASE       0x173C
#define M_VTXFMT                0x1740
#define M_VERTEX_BEGIN_END      0x1808
#define M_VB_VERTEX_BATCH       0x1814
#define M_INLINE_ARRAY          0x1818
#define M_IDXBUF_OFFSET         0x181C
#define M_IDXBUF_FORMAT         0x1820
#define M_VB_INDEX_BATCH        0x1824
#define M_TEX_SIZE1             0x1840  /* + unit * 4  (control3)           */
#define M_TEX_OFFSET            0x1A00  /* + unit * 0x20 (8-word unit block) */
#define M_VERTEX_TEXTURE        0x0900  /* + unit * 0x20 (8-word unit block) */
#define M_FP_ACTIVE_PROGRAM     0x08E4  /* offset | dma-context [1:0]        */
#define M_FP_CONTROL            0x1D60
#define M_VP_START_FROM_ID      0x1EA0
#define M_VTX_ATTR_4F           0x1C00  /* + attr * 0x10, 4 float words      */
#define M_CLEAR_DEPTH_VALUE     0x1D8C
#define M_CLEAR_COLOR_VALUE     0x1D90
#define M_CLEAR_BUFFERS         0x1D94
#define M_VP_UPLOAD_FROM_ID     0x1E9C
#define M_VP_UPLOAD_CONST_ID    0x1EFC
#define M_VP_UPLOAD_CONST       0x1F00
#define M_GCM_FLIP_HEAD_0       0xE920
#define M_GCM_FLIP_HEAD_1       0xE924
#define M_GCM_DRIVER_FLIP       0xE944
/* Primitive restart: a sentinel index value that breaks a TRIANGLE_STRIP/
 * FAN mid-draw with no connecting triangle (RPCS3 oracle: gcm_enums.h
 * NV4097_SET_RESTART_INDEX_ENABLE/NV4097_SET_RESTART_INDEX; semantics in
 * rsx_methods.h restart_index_enabled()/restart_index() -- enabled only
 * when the enable bit is set AND, for 16-bit index buffers, the configured
 * value fits in 16 bits). */
#define M_RESTART_INDEX_ENABLE  0x1DAC
#define M_RESTART_INDEX         0x1DB0

/* Render state consumed directly by the replay harness's PSO builder
 * (rsx_dsp_reg reads straight out of the register file, so these already
 * took effect before being classified here; this fixes the coverage report
 * to match reality instead of showing them as TODO). Addresses from
 * tools/nv40_methods.py (envytools rnndb). */
#define M_ALPHA_FUNC_ENABLE     0x0304
#define M_ALPHA_FUNC_FUNC       0x0308
#define M_ALPHA_FUNC_REF        0x030C
#define M_BLEND_FUNC_ENABLE     0x0310
#define M_BLEND_FUNC_SRC        0x0314
#define M_BLEND_FUNC_DST        0x0318
#define M_COLOR_MASK            0x0324
#define M_MRT_COLOR_MASK        0x0370
#define M_DEPTH_FUNC            0x0A6C
#define M_DEPTH_WRITE_ENABLE    0x0A70
#define M_DEPTH_TEST_ENABLE     0x0A74
#define M_CULL_FACE             0x1830
#define M_FRONT_FACE            0x1834
#define M_CULL_FACE_ENABLE      0x183C
#define M_VTX_CACHE_INVALIDATE  0x1714
#define M_NV4097_NOP            0x0100
#define M_VP_ATTRIB_EN          0x1FF0
#define M_VP_RESULT_EN          0x1FF4

#define DMA_HANDLE_LOCAL        0xFEED0000u
#define DMA_HANDLE_MAIN         0xFEED0001u

static u32 dma_to_location(u32 handle)
{
    return handle == DMA_HANDLE_MAIN ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
}

static void mark_class(rsx_dispatch* rsx, u32 method, u32 count, u8 klass)
{
    for (u32 i = 0; i < count; i++)
        rsx->klass[(method >> 2) + i] = klass;
}

void rsx_dispatch_init(rsx_dispatch* rsx, const rsx_dispatch_sink* sink)
{
    memset(rsx, 0, sizeof(*rsx));
    g_rsx_dsp_pso_epoch++;   /* LD_PSO_SAME */ g_rsx_dsp_bind_epoch++;   /* LD_DRAW_REUSE */
    g_rsx_dsp_vp_gen++; g_rsx_dsp_pso_stable++;   /* DRAW-COST */
    if (sink)
        rsx->sink = *sink;

    /* s31 (scratch/s31_blue_emitter.md): the nv40 RESET DEFAULT for
     * COLOR_MASK is all-channels-on (RPCS3 rsx_methods.cpp:345,983 seeds
     * registers[NV4097_SET_COLOR_MASK]=0x1010101). Seeding it here makes a
     * raw register read trustworthy for every consumer: a game-written 0 is
     * a REAL "write no color channels" (the character shadow-mask
     * depth-prime pass), distinguishable from never-initialized. Without
     * the seed, consumers substituted mask=ALL for 0 and force-wrote the
     * depth-only pass's garbage FP output (the blue-character class). */
    rsx->regs[M_COLOR_MASK >> 2] = 0x01010101u;

    /* NV40 reset state is counter-clockwise front winding.  Leaving this
     * register zero made a live stream that relied on the hardware default
     * cull the exterior of orphanage meshes, while captured replay was correct
     * because its seeded register image already contained 0x901. */
    rsx->regs[M_FRONT_FACE >> 2] = 0x0901u;

    /* NV40 reset ZSTENCIL_CLEAR_VALUE (Z24S8: depth24<<8 | stencil8) is
     * 0xFFFFFF00 = depth 1.0, stencil 0 (RPCS3 rsx_methods.cpp reset image
     * as numbering oracle). Seeded so a raw register read is trustworthy for
     * clear consumers, mirroring COLOR_MASK/FRONT_FACE above. */
    rsx->regs[0x1D8C >> 2] = 0xFFFFFF00u;

    /* Execution methods */
    mark_class(rsx, M_VERTEX_BEGIN_END,  1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VB_VERTEX_BATCH,   1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_INLINE_ARRAY,      1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VB_INDEX_BATCH,    1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_CLEAR_BUFFERS,     1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VP_UPLOAD_INST,   32, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VP_UPLOAD_CONST,  32, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_GCM_FLIP_HEAD_0,   1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_GCM_FLIP_HEAD_1,   1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_GCM_DRIVER_FLIP,   1, RSX_DSP_CLASS_EXEC);

    /* State the getters decode */
    mark_class(rsx, M_DMA_COLOR1,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_DMA_COLOR0,        2, RSX_DSP_CLASS_STATE); /* +ZETA */
    mark_class(rsx, M_DMA_COLOR2,        2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_RT_HORIZ,         12, RSX_DSP_CLASS_STATE); /* 0x200..0x22C */
    mark_class(rsx, M_COLOR2_PITCH,      4, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VIEWPORT_HORIZ,    2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VIEWPORT_TRANSLATE, 8, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VTXBUF_OFFSET,    16, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VERTEX_DATA_BASE,  2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VTXFMT,           16, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_IDXBUF_OFFSET,     2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_TEX_SIZE1,        16, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_TEX_OFFSET, RSX_DSP_NUM_TEXTURES * 8, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_CLEAR_DEPTH_VALUE, 2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_UPLOAD_FROM_ID, 1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_RESTART_INDEX_ENABLE, 2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_UPLOAD_CONST_ID, 1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_FP_ACTIVE_PROGRAM, 1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_FP_CONTROL,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_START_FROM_ID,  1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VTX_ATTR_4F,      64, RSX_DSP_CLASS_STATE);

    /* Alpha test / blend / color-mask / depth / cull: rsx_dsp_reg reads these
     * straight from the register file (replay_main.c's decode_render_state,
     * color-mask application), so they were already live; this only corrects
     * the coverage report. */
    mark_class(rsx, M_ALPHA_FUNC_ENABLE, 3, RSX_DSP_CLASS_STATE); /* ..FUNC_REF */
    mark_class(rsx, M_BLEND_FUNC_ENABLE, 5, RSX_DSP_CLASS_STATE); /* ..BLEND_EQUATION */
    mark_class(rsx, M_COLOR_MASK,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_MRT_COLOR_MASK,    1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_DEPTH_FUNC,        3, RSX_DSP_CLASS_STATE); /* ..TEST_ENABLE */
    mark_class(rsx, M_CULL_FACE,         1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_FRONT_FACE,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_CULL_FACE_ENABLE,  1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_ATTRIB_EN,      1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_RESULT_EN,      1, RSX_DSP_CLASS_STATE);

    /* Explicit no-ops: the vertex cache invalidate hint and the reserved
     * NOP method carry no state the replay harness needs (there is no CPU
     * vertex cache to invalidate; every draw re-fetches from guest memory
     * each time). Classifying them EXEC removes them from the TODO count
     * without pretending they do anything. */
    mark_class(rsx, M_VTX_CACHE_INVALIDATE, 1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_NV4097_NOP,           1, RSX_DSP_CLASS_EXEC);
}

void rsx_dispatch_seed_registers(rsx_dispatch* rsx, const u32* regs, u32 count)
{
    if (count > RSX_DSP_NUM_REGS)
        count = RSX_DSP_NUM_REGS;
    memcpy(rsx->regs, regs, count * sizeof(u32));
    g_rsx_dsp_pso_epoch++;   /* LD_PSO_SAME */ g_rsx_dsp_bind_epoch++;   /* LD_DRAW_REUSE */
    g_rsx_dsp_vp_gen++; g_rsx_dsp_pso_stable++;   /* DRAW-COST */
}

void rsx_dispatch_seed_transform_program(rsx_dispatch* rsx, const u32* words, u32 count)
{
    if (count > RSX_DSP_VP_WORDS)
        count = RSX_DSP_VP_WORDS;
    memcpy(rsx->vp, words, count * sizeof(u32));
    g_rsx_dsp_pso_epoch++;   /* LD_PSO_SAME */ g_rsx_dsp_bind_epoch++;   /* LD_DRAW_REUSE */
    g_rsx_dsp_vp_gen++; g_rsx_dsp_pso_stable++;   /* DRAW-COST */
}

void rsx_dispatch_seed_transform_constants(rsx_dispatch* rsx, const u32* words, u32 count)
{
    const u32 max_words = RSX_DSP_NUM_CONSTANTS * 4;
    if (count > max_words)
        count = max_words;
    memcpy(rsx->constants, words, count * sizeof(u32));
    g_rsx_dsp_const_gen++;   /* LD_CB_GEN */
}

/* Every NV4097_SET_VERTEX_DATA* variant writes the same constant ("current")
 * attribute register a disabled array feeds. Only the 4F form was decoded, so a
 * colour set with SetVertexData4ub read as the (0,0,0,1) default: Tornado
 * Outbreak's UI draws its vertex colour that way, alpha 0 failed the alpha test
 * on every glyph, and 58 draws a frame reached an all-black screen. Fold each
 * variant into the 4F slots rsx_dsp_vertex_default reads. Offsets and packing
 * from the SDK (gcm_implementation_sub.h; x is the LOW byte/half). */
/* LD_PSO_SAME's change counter (rsx_dispatch.h). s_pso_quiet[i]: register i
 * is never read by the live draw's pipeline choice (get_pso), so changing it
 * does not count. Only what is certain is listed: */
int rsx_dsp_pso_epoch_on;
unsigned g_rsx_dsp_pso_epoch = 1;
unsigned g_rsx_dsp_const_gen = 1;   /* every transform-constant write (LD_CB_GEN) */
unsigned g_rsx_dsp_pso_last;     /* the register (index) whose change counted last */
/* LD_DRAW_REUSE: every change of a register value except the ones that only
 * say where this draw's vertices and indices are, start it, upload program
 * words or constants (counted by g_rsx_dsp_const_gen and the program check),
 * or are the front end's references, reports, semaphores and clear values.
 * Texture offsets count here (a different texture is a different binding),
 * unlike for g_rsx_dsp_pso_epoch. */
unsigned g_rsx_dsp_bind_epoch = 1;
unsigned g_rsx_dsp_bind_last;   /* the register (index) whose change counted last */
/* Cull mode, winding and polygon offset: read only by the pipeline choice
 * and the depth offset in the vertex constants (LD_ZBIAS_VS), which the reuse
 * works out again when this counter moved (God of War: Ascension flips
 * FRONT_FACE between EDGE segments of mirrored meshes and toggles the offset
 * for decals: ~25% of its draws). */
unsigned g_rsx_dsp_raster_epoch = 1;
/* DRAW-COST (DRAWOPT_HEAD, rsx_live_draw.c): moves with every change of a transform-program word or of its start
 * (and at any seed or quiet-list change), while g_rsx_dsp_pso_epoch is counted. */
unsigned g_rsx_dsp_vp_gen = 1;
/* DRAW-COST (DRAWOPT_PSO, rsx_live_draw.c's pipeline-choice memo): moves with
 * g_rsx_dsp_pso_epoch, except for the registers made volatile
 * (rsx_dispatch_pso_volatile), whose values the memo compares itself. */
unsigned g_rsx_dsp_pso_stable = 1;
static u8 s_pso_vol[RSX_DSP_NUM_REGS];
static u8 s_pso_quiet[RSX_DSP_NUM_REGS];
static u8 s_bind_quiet[RSX_DSP_NUM_REGS];   /* 1: never counts, 2: counts in g_rsx_dsp_raster_epoch */
static void dsp_bind_quiet_init(void)
{
    for (u32 m = 0x0000; m < 0x0100; m += 4) s_bind_quiet[m >> 2] = 1;   /* NV406E: ref, semaphores */
    s_bind_quiet[0x0100 >> 2] = s_bind_quiet[0x0104 >> 2] = s_bind_quiet[0x0110 >> 2] = 1;   /* NOP, NOTIFY, WAIT_FOR_IDLE */
    for (u32 m = 0x0B80; m < 0x0C00; m += 4) s_bind_quiet[m >> 2] = 1;   /* program upload port */
    s_bind_quiet[0x1E9C >> 2] = 1;                                         /* program upload slot */
    for (u32 m = 0x1EFC; m < 0x1F80; m += 4) s_bind_quiet[m >> 2] = 1;   /* constant upload slot + window */
    for (u32 m = 0x1680; m < 0x16C0; m += 4) s_bind_quiet[m >> 2] = 1;   /* vertex array offsets */
    s_bind_quiet[0x1738 >> 2] = s_bind_quiet[0x173C >> 2] = 1;             /* VERTEX_DATA_BASE_OFFSET / _INDEX */
    s_bind_quiet[0x17C8 >> 2] = s_bind_quiet[0x1800 >> 2] = 1;             /* CLEAR_REPORT_VALUE, GET_REPORT */
    s_bind_quiet[0x1808 >> 2] = s_bind_quiet[0x1814 >> 2] = s_bind_quiet[0x1818 >> 2] = 1;   /* BEGIN_END, DRAW_ARRAYS, INLINE_ARRAY */
    s_bind_quiet[0x181C >> 2] = s_bind_quiet[0x1820 >> 2] = s_bind_quiet[0x1824 >> 2] = 1;   /* INDEX_ARRAY_*, DRAW_INDEX_ARRAY */
    s_bind_quiet[0x1D6C >> 2] = s_bind_quiet[0x1D70 >> 2] = s_bind_quiet[0x1D74 >> 2] = 1;   /* back-end semaphores */
    s_bind_quiet[0x1D8C >> 2] = s_bind_quiet[0x1D90 >> 2] = s_bind_quiet[0x1D94 >> 2] = 1;   /* clear values, CLEAR_SURFACE */
    s_bind_quiet[0x0354 >> 2] = 1;   /* BACK_STENCIL_FUNC_REF: D3D12 has the front reference only */
    s_bind_quiet[0x0334 >> 2] = 1;   /* STENCIL_FUNC_REF: dynamic state, set on every draw */
    s_bind_quiet[0x1830 >> 2] = s_bind_quiet[0x1834 >> 2] = s_bind_quiet[0x183C >> 2] = 2;   /* CULL_FACE, FRONT_FACE, CULL_FACE_ENABLE */
    s_bind_quiet[0x0A68 >> 2] = s_bind_quiet[0x0A78 >> 2] = s_bind_quiet[0x0A7C >> 2] = 2;   /* POLY_OFFSET_FILL_ENABLE, its slope and bias */
}
void rsx_dispatch_bind_quiet(u32 lo, u32 hi)
{
    for (u32 m = lo & ~3u; m <= hi && (m >> 2) < RSX_DSP_NUM_REGS; m += 4) s_bind_quiet[m >> 2] = 1;
    g_rsx_dsp_vp_gen++;   /* DRAW-COST */
}
void rsx_dispatch_pso_epoch_enable(void)
{
    if (rsx_dsp_pso_epoch_on) return;
    dsp_bind_quiet_init();
    for (u32 m = 0x0000; m < 0x0100; m += 4) s_pso_quiet[m >> 2] = 1;   /* NV406E: ref, semaphores */
    s_pso_quiet[0x0100 >> 2] = 1;                       /* NOP */
    s_pso_quiet[0x0104 >> 2] = 1;                       /* NOTIFY */
    s_pso_quiet[0x0110 >> 2] = 1;                       /* WAIT_FOR_IDLE */
    for (u32 m = 0x0B80; m < 0x0C00; m += 4) s_pso_quiet[m >> 2] = 1;   /* program upload port: the words count (below) */
    s_pso_quiet[0x1E9C >> 2] = 1;                       /* program upload slot */
    for (u32 m = 0x1EFC; m < 0x1F80; m += 4) s_pso_quiet[m >> 2] = 1;   /* constant upload slot + window */
    for (u32 m = 0x1680; m < 0x16C0; m += 4) s_pso_quiet[m >> 2] = 1;   /* vertex array offsets */
    s_pso_quiet[0x1738 >> 2] = 1;                       /* VERTEX_DATA_BASE_OFFSET */
    s_pso_quiet[0x173C >> 2] = 1;                       /* VERTEX_DATA_BASE_INDEX */
    s_pso_quiet[0x17C8 >> 2] = 1;                       /* CLEAR_REPORT_VALUE */
    s_pso_quiet[0x1800 >> 2] = 1;                       /* GET_REPORT */
    s_pso_quiet[0x1808 >> 2] = 1;                       /* BEGIN_END */
    s_pso_quiet[0x1814 >> 2] = 1;                       /* DRAW_ARRAYS */
    s_pso_quiet[0x1818 >> 2] = 1;                       /* INLINE_ARRAY */
    s_pso_quiet[0x181C >> 2] = 1;                       /* INDEX_ARRAY_ADDRESS */
    s_pso_quiet[0x1820 >> 2] = 1;                       /* INDEX_ARRAY_DMA */
    s_pso_quiet[0x1824 >> 2] = 1;                       /* DRAW_INDEX_ARRAY */
    s_pso_quiet[0x1D6C >> 2] = 1;                       /* SEMAPHORE_OFFSET */
    s_pso_quiet[0x1D70 >> 2] = 1;                       /* BACK_END_WRITE_SEMAPHORE_RELEASE */
    s_pso_quiet[0x1D74 >> 2] = 1;                       /* TEXTURE_READ_SEMAPHORE_RELEASE */
    s_pso_quiet[0x1D8C >> 2] = 1;                       /* clear depth value */
    s_pso_quiet[0x1D90 >> 2] = 1;                       /* clear colour value */
    s_pso_quiet[0x1D94 >> 2] = 1;                       /* CLEAR_SURFACE */
    /* Texture image offsets: get_pso reads a unit's offset only to match it
     * against depth snapshots (LD_PSO_SAME never reuses with a depth-format
     * unit enabled) and surfaces (ld_surf_scale_units, recomputed on reuse). */
    for (u32 u = 0; u < 16; u++) s_pso_quiet[(0x1A00 + u * 0x20) >> 2] = 1;
    g_rsx_dsp_pso_epoch++;
    g_rsx_dsp_vp_gen++; g_rsx_dsp_pso_stable++;   /* DRAW-COST */
    __atomic_store_n(&rsx_dsp_pso_epoch_on, 1, __ATOMIC_RELEASE);
}
/* DRAW-COST (DRAWOPT_PSO, rsx_live_draw.c): the registers the pipeline choice
 * reads (s_pso_quiet clear) whose changes do not end a reuse of the bindings
 * (s_bind_quiet set) -- the only registers get_pso reads that can differ
 * between two draws of one LD_DRAW_REUSE run (today the cull mode, winding and
 * polygon offset). Their indices into idx[] (at most max); returns how many
 * there are. Valid once rsx_dispatch_pso_epoch_enable() has run. */
u32 rsx_dispatch_pso_unpinned(u32* idx, u32 max)
{
    u32 n = 0;
    if (!rsx_dsp_pso_epoch_on) return 0;
    for (u32 i = 0; i < RSX_DSP_NUM_REGS; i++)
        if (!s_pso_quiet[i] && s_bind_quiet[i]) { if (n < max) idx[n] = i; n++; }
    return n;
}
void rsx_dispatch_pso_quiet(u32 lo, u32 hi)
{
    for (u32 m = lo & ~3u; m <= hi && (m >> 2) < RSX_DSP_NUM_REGS; m += 4) s_pso_quiet[m >> 2] = 1;
    g_rsx_dsp_vp_gen++; g_rsx_dsp_pso_stable++;   /* DRAW-COST */
}
/* DRAW-COST (DRAWOPT_PSO): registers [lo, hi] still count in
 * g_rsx_dsp_pso_epoch but not in g_rsx_dsp_pso_stable. */
void rsx_dispatch_pso_volatile(u32 lo, u32 hi)
{
    for (u32 m = lo & ~3u; m <= hi && (m >> 2) < RSX_DSP_NUM_REGS; m += 4) s_pso_vol[m >> 2] = 1;
    g_rsx_dsp_pso_stable++;
}
static inline void dsp_pso_note(const rsx_dispatch* rsx, u32 idx, u32 arg)
{
    if (rsx->regs[idx] != arg) {
        if (!s_pso_quiet[idx]) { g_rsx_dsp_pso_epoch++; g_rsx_dsp_pso_last = idx; g_rsx_dsp_pso_stable += !s_pso_vol[idx]; }
        if (idx == (M_VP_START_FROM_ID >> 2)) g_rsx_dsp_vp_gen++;   /* DRAW-COST: DRAWOPT_HEAD */
        if (!s_bind_quiet[idx]) { g_rsx_dsp_bind_epoch++; g_rsx_dsp_bind_last = idx; }   /* LD_DRAW_REUSE */
        else if (s_bind_quiet[idx] == 2) g_rsx_dsp_raster_epoch++;
    }
}

static void vtx_const_set(rsx_dispatch* rsx, u32 attr, u32 c, float v)
{
    u32 w; memcpy(&w, &v, 4);
    const u32 idx = (M_VTX_ATTR_4F + attr * 0x10 + c * 4) >> 2;
    if (__builtin_expect(rsx_dsp_pso_epoch_on, 0)) dsp_pso_note(rsx, idx, w);
    rsx->regs[idx] = w;
    /* Folded packed constants also have an explicit canonical value, even
     * when every component is zero. Immediate snapshots need that presence. */
    rsx->seen[idx]++;
}

static void vtx_const_fold(rsx_dispatch* rsx, u32 m, u32 arg)
{
    float f;
    u32 attr, word;
    if (m >= 0x1E40 && m < 0x1E80) {                  /* DATA1F: (x,0,0,1) */
        attr = (m - 0x1E40) >> 2; memcpy(&f, &arg, 4);
        vtx_const_set(rsx, attr, 0, f); vtx_const_set(rsx, attr, 1, 0.0f);
        vtx_const_set(rsx, attr, 2, 0.0f); vtx_const_set(rsx, attr, 3, 1.0f);
    } else if (m >= 0x1880 && m < 0x1900) {           /* DATA2F: (x,y,0,1) */
        attr = (m - 0x1880) >> 3; word = ((m - 0x1880) >> 2) & 1; memcpy(&f, &arg, 4);
        vtx_const_set(rsx, attr, word, f);
        if (word == 0) { vtx_const_set(rsx, attr, 2, 0.0f); vtx_const_set(rsx, attr, 3, 1.0f); }
    } else if (m >= 0x1500 && m < 0x1600) {           /* DATA3F: (x,y,z,1) */
        attr = (m - 0x1500) >> 4; word = ((m - 0x1500) >> 2) & 3; memcpy(&f, &arg, 4);
        if (word < 3) vtx_const_set(rsx, attr, word, f);
        if (word == 0) vtx_const_set(rsx, attr, 3, 1.0f);
    } else if (m >= 0x1900 && m < 0x1940) {           /* DATA2S: (x,y,0,1), unnormalised */
        attr = (m - 0x1900) >> 2;
        vtx_const_set(rsx, attr, 0, (float)(int16_t)(arg & 0xFFFF));
        vtx_const_set(rsx, attr, 1, (float)(int16_t)(arg >> 16));
        vtx_const_set(rsx, attr, 2, 0.0f); vtx_const_set(rsx, attr, 3, 1.0f);
    } else if (m >= 0x1940 && m < 0x1980) {           /* DATA4UB: normalised bytes */
        attr = (m - 0x1940) >> 2;
        for (u32 c = 0; c < 4; c++)
            vtx_const_set(rsx, attr, c, (float)((arg >> (8 * c)) & 0xFF) / 255.0f);
    } else if ((m >= 0x1980 && m < 0x1A00) ||         /* DATA4S: unnormalised */
               (m >= 0x0A80 && m < 0x0B00)) {         /* DATA_SCALED4S: normalised */
        const u32 base = m >= 0x1980 ? 0x1980u : 0x0A80u;
        const float k = m >= 0x1980 ? 1.0f : 1.0f / 32767.0f;
        attr = (m - base) >> 3; word = ((m - base) >> 2) & 1;
        vtx_const_set(rsx, attr, word * 2 + 0, (float)(int16_t)(arg & 0xFFFF) * k);
        vtx_const_set(rsx, attr, word * 2 + 1, (float)(int16_t)(arg >> 16) * k);
    }
}

/* Transform constant upload window: word i of the burst writes
 * constants[VP_UPLOAD_CONST_ID + i/4][i%4]; the load pointer does NOT
 * advance. */
static inline void dsp_vp_const(rsx_dispatch* rsx, u32 method, u32 arg)
{
    const u32 word = (method - M_VP_UPLOAD_CONST) >> 2;
    const u32 slot = rsx->regs[M_VP_UPLOAD_CONST_ID >> 2] + (word >> 2);
    if (slot < RSX_DSP_NUM_CONSTANTS) {
        rsx->constants[slot][word & 3] = arg;
        g_rsx_dsp_const_gen++;   /* LD_CB_GEN */
    }
    /* RSX_CONST_LOG=<lo>,<hi>: every transform-constant upload into that
     * slot range (first 400) -- what a shader's constants really were. */
    { static int lo = -2, hi = 0, n = 0;
      if (lo == -2) { const char* e = getenv("RSX_CONST_LOG"); lo = -1;
                      if (e) sscanf(e, "%d,%d", &lo, &hi); }
      if (lo >= 0 && (int)slot >= lo && (int)slot <= hi && n < 400) { n++;
          fprintf(stderr, "[const-load] c[%u].%c = 0x%08X (method 0x%04X id=%u)\n", slot,
                  "xyzw"[word & 3], arg, method, rsx->regs[M_VP_UPLOAD_CONST_ID >> 2]); } }
}

/* Everything rsx_dispatch_method does after storing the register. */
static void dsp_method_effects(rsx_dispatch* rsx, u32 method, u32 arg)
{
    if ((method >= 0x1500 && method < 0x1A00) || (method >= 0x1E40 && method < 0x1E80) ||
        (method >= 0x0A80 && method < 0x0B00))
        vtx_const_fold(rsx, method, arg);

    /* Completing a floating-point position emits a register-based vertex.
     * Other attributes update the current values copied with that position.
     * Keep these writes as ordinary constant state outside BEGIN/END. */
    if (rsx->in_begin_end && (method == 0x1508 || method == 0x1C0C) &&
        rsx->sink.immediate_vertex)
        rsx->sink.immediate_vertex(rsx->sink.user, rsx);

    /* Transform program upload window: word goes to instruction slot
     * VP_UPLOAD_FROM_ID; the load pointer advances after every completed
     * vec4 (hardware auto-increment, confirmed against the RPCS3 oracle). */
    if (method >= M_VP_UPLOAD_INST && method < M_VP_UPLOAD_INST + 32 * 4) {
        const u32 lane = ((method - M_VP_UPLOAD_INST) >> 2) & 3;
        u32* load = &rsx->regs[M_VP_UPLOAD_FROM_ID >> 2];
        const u32 pos = *load * 4 + lane;
        if (pos < RSX_DSP_VP_WORDS) {
            if (__builtin_expect(rsx_dsp_pso_epoch_on, 0) && rsx->vp[pos] != arg) {
                g_rsx_dsp_pso_epoch++;   /* LD_PSO_SAME: the program changed */
                g_rsx_dsp_vp_gen++; g_rsx_dsp_pso_stable++;   /* DRAW-COST: DRAWOPT_HEAD, DRAWOPT_PSO */
                g_rsx_dsp_pso_last = M_VP_UPLOAD_INST >> 2;
                g_rsx_dsp_bind_epoch++;  /* LD_DRAW_REUSE */
            }
            rsx->vp[pos] = arg;
        }
        if (lane == 3)
            (*load)++;
        return;
    }

    /* Transform constant upload window (dsp_vp_const). */
    if (method >= M_VP_UPLOAD_CONST && method < M_VP_UPLOAD_CONST + 32 * 4) {
        dsp_vp_const(rsx, method, arg);
        return;
    }

    switch (method) {
    case M_CLEAR_BUFFERS:
        if (rsx->sink.clear)
            rsx->sink.clear(rsx->sink.user, rsx, arg);
        break;

    case M_VERTEX_BEGIN_END:
        if (arg) {
            rsx->in_begin_end = 1;
            rsx->current_primitive = arg;
            rsx->inline_len = 0;
            if (rsx->sink.begin)
                rsx->sink.begin(rsx->sink.user, rsx, arg);
        } else {
            rsx->in_begin_end = 0;
            /* Hand an inline stream over BEFORE end: the sink reads end as
             * "draw what you have", so a batch delivered after it would be
             * drawn one primitive late, or not at all. */
            if (rsx->inline_len && rsx->sink.inline_array)
                rsx->sink.inline_array(rsx->sink.user, rsx,
                                       rsx->inline_data, rsx->inline_len);
            rsx->inline_len = 0;
            if (rsx->sink.end)
                rsx->sink.end(rsx->sink.user, rsx);
        }
        break;

    /* INLINE_ARRAY: the vertex stream travels IN the pushbuffer rather than
     * being fetched from a vertex array, so there is no (first, count) --
     * every word between BEGIN_END(prim) and BEGIN_END(0) is vertex data.
     * The layout is still the ordinary VTXFMT declaration: enabled
     * attributes packed in ascending index order, one vertex every stride
     * bytes. Store the words back in guest byte order so the consumer
     * decodes them with the same big-endian readers a vertex array uses;
     * the dispatcher never interprets them.
     *
     * A title that builds geometry this way (Guitar Hero III draws its
     * entire 2D layer as inline QUADS) previously issued draws that arrived
     * carrying no vertices at all: the backend saw BEGIN/END with nothing
     * between them and counted an empty group. */
    case M_INLINE_ARRAY:
        if (rsx->inline_len + 4u <= RSX_DSP_INLINE_MAX_BYTES) {
            rsx->inline_data[rsx->inline_len + 0] = (u8)(arg >> 24);
            rsx->inline_data[rsx->inline_len + 1] = (u8)(arg >> 16);
            rsx->inline_data[rsx->inline_len + 2] = (u8)(arg >> 8);
            rsx->inline_data[rsx->inline_len + 3] = (u8)arg;
            rsx->inline_len += 4;
        } else if (rsx->inline_len) {
            /* Truncating leaves a torn final vertex; drop the whole stream
             * and count it instead of drawing something half-read. */
            rsx->inline_len = 0;
            rsx->inline_dropped++;
        }
        break;

    case M_VB_VERTEX_BATCH:
        if (rsx->sink.draw_arrays)
            rsx->sink.draw_arrays(rsx->sink.user, rsx,
                                  arg & 0xFFFFFF, (arg >> 24) + 1);
        break;

    case M_VB_INDEX_BATCH:
        if (rsx->sink.draw_index_array)
            rsx->sink.draw_index_array(rsx->sink.user, rsx,
                                       arg & 0xFFFFFF, (arg >> 24) + 1);
        break;

    case M_GCM_FLIP_HEAD_0:
    case M_GCM_FLIP_HEAD_1:
    case M_GCM_DRIVER_FLIP:
        if (rsx->sink.flip)
            rsx->sink.flip(rsx->sink.user, rsx, arg);
        break;

    default:
        break;
    }
}

/* RSX_DSP_TABLE (default 1): every word the FIFO walker hands the live
 * engine comes through here, ~150,000 a frame in God of War: Ascension's
 * combat, and most of them only set a register. After storing it, each word
 * used to run the same chain of tests -- three vertex-constant ranges, the
 * immediate-vertex methods, the two upload windows, then the switch -- to
 * find nothing to do. One byte per method, worked out once from those same
 * tests, says which words have something to do: none (return), a transform
 * constant upload (straight to it), or anything else (the chain as before).
 * Same effects in the same order for every method. RSX_DSP_TABLE=0: the chain
 * for every word. rsx_dispatch_table: -1 until the first word reads the
 * environment; tests call rsx_dispatch_set_table(0/1). */
int rsx_dispatch_table = -1;
enum { DSP_K_PLAIN = 0, DSP_K_CONST = 1, DSP_K_CHAIN = 2 };
static u8 s_dsp_kind[RSX_DSP_NUM_REGS];
static volatile int s_dsp_kind_ready;
/* WALK2 (ascension host/walk/walk2, 2026-11-08): s_dsp_kind2 = the same table, but 0x1600-0x187F (vertex array
 * offsets/formats, FRONT_FACE, cull, index array registers, ...) are PLAIN except the four switch cases there
 * (BEGIN_END, DRAW_ARRAYS, INLINE_ARRAY, DRAW_INDEX_ARRAY): vtx_const_fold, the immediate-vertex test and the two
 * upload windows do nothing for those methods, so the effects are the same, without the test chain. The walker
 * uses s_dsp_kind_p = s_dsp_kind2 while W2_FIFO is active (rsx_dispatch_w2_set). */
static u8 s_dsp_kind2[RSX_DSP_NUM_REGS];
static const u8* s_dsp_kind_p = s_dsp_kind;

/* Does dsp_method_effects do anything for this (masked) method? Mirrors its
 * tests one for one. */
static u8 dsp_kind_of(u32 method)
{
    if ((method >= 0x1500 && method < 0x1A00) || (method >= 0x1E40 && method < 0x1E80) ||
        (method >= 0x0A80 && method < 0x0B00))
        return DSP_K_CHAIN;                         /* vtx_const_fold */
    if (method == 0x1508 || method == 0x1C0C)
        return DSP_K_CHAIN;                         /* immediate vertex */
    if (method >= M_VP_UPLOAD_INST && method < M_VP_UPLOAD_INST + 32 * 4)
        return DSP_K_CHAIN;                         /* program upload */
    if (method >= M_VP_UPLOAD_CONST && method < M_VP_UPLOAD_CONST + 32 * 4)
        return DSP_K_CONST;                         /* constant upload */
    switch (method) {                               /* the switch's cases */
    case M_CLEAR_BUFFERS: case M_VERTEX_BEGIN_END: case M_INLINE_ARRAY:
    case M_VB_VERTEX_BATCH: case M_VB_INDEX_BATCH:
    case M_GCM_FLIP_HEAD_0: case M_GCM_FLIP_HEAD_1: case M_GCM_DRIVER_FLIP:
        return DSP_K_CHAIN;
    default:
        return DSP_K_PLAIN;
    }
}

static u8 dsp_kind2_of(u32 method)
{
    if (method >= 0x1600 && method < 0x1880) {
        switch (method) {
        case M_VERTEX_BEGIN_END: case M_VB_VERTEX_BATCH: case M_INLINE_ARRAY: case M_VB_INDEX_BATCH:
            return DSP_K_CHAIN;
        default:
            return DSP_K_PLAIN;
        }
    }
    return dsp_kind_of(method);
}

void rsx_dispatch_set_table(int on)
{
    if (on && !s_dsp_kind_ready) {
        for (u32 i = 0; i < RSX_DSP_NUM_REGS; i++) {
            s_dsp_kind[i] = dsp_kind_of(i << 2);
            s_dsp_kind2[i] = dsp_kind2_of(i << 2);
        }
        __atomic_store_n(&s_dsp_kind_ready, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&rsx_dispatch_table, on ? 1 : 0, __ATOMIC_RELEASE);
}

static int dsp_table_decide(void)
{
    const char* e = getenv("RSX_DSP_TABLE");
    rsx_dispatch_set_table(!(e && e[0] == '0'));
    return rsx_dispatch_table;
}

void rsx_dispatch_method(rsx_dispatch* rsx, u32 method, u32 arg)
{
    method &= 0xFFFFC;
    const u32 idx = method >> 2;
    /* regs[] covers methods 0..0xFFFC (subchannel bits included). A larger
     * value is not a method -- and indexing with it wrote past regs[] into
     * vp[] and constants[]: Tornado Outbreak's c[461] held two copies of a
     * "load constant 461" command header, i.e. FIFO words stored as shader
     * constants. Drop it, and say what it was. */
    if (idx >= RSX_DSP_NUM_REGS) {
        static int n = 0;
        if (n++ < 8)
            fprintf(stderr, "[rsx-dispatch] method 0x%05X (arg 0x%08X) out of range -- dropped\n",
                    method, arg);
        return;
    }
    rsx->seen[idx]++;
    if (__builtin_expect(rsx_dsp_pso_epoch_on, 0)) dsp_pso_note(rsx, idx, arg);   /* LD_PSO_SAME */
    rsx->regs[idx] = arg;
    int table = rsx_dispatch_table;
    if (__builtin_expect(table < 0, 0))
        table = dsp_table_decide();
    if (__builtin_expect(table, 1)) {
        const u8 k = s_dsp_kind_p[idx];
        if (k == DSP_K_PLAIN)
            return;
        if (k == DSP_K_CONST) {
            dsp_vp_const(rsx, method, arg);
            return;
        }
    }
    dsp_method_effects(rsx, method, arg);
}

/* WALK2 (ascension host/walk/walk2, 2026-11-08): FIFO packet fast paths, called by rsx_live_draw.c under
 * W2_FIFO for a whole packet. Each has exactly the effects the per-word rsx_dispatch_method calls would have on
 * regs[], seen[], vp[], constants[] and the epochs (g_rsx_dsp_pso_epoch, _last, _stable, _vp_gen, _bind_epoch,
 * _bind_last, _raster_epoch), word by word in order, with one exception: g_rsx_dsp_const_gen moves once for a
 * constant packet that changed a constant and not at all for one that re-sent the values the constants already
 * held (the per-word path moves it for every constant word; its readers only ask "unchanged since?"). They return
 * 0 with nothing done when the packet is not of their form or the table is not in use; the caller then walks it
 * word by word. */
static int s_w2_quiet_ok = -1;   /* NOP, the constant window + slot, the program window: pso- and bind-quiet */
static int s_w2_constlog = -1;
static int w2_quiet_ok(void)
{
    if (!rsx_dsp_pso_epoch_on) return 1;   /* dsp_pso_note not called at all */
    if (s_w2_quiet_ok < 0) {
        int ok = s_pso_quiet[0x0100 >> 2] && s_bind_quiet[0x0100 >> 2] == 1;
        for (u32 m = 0x1EFC; m < 0x1F80; m += 4) ok &= s_pso_quiet[m >> 2] && s_bind_quiet[m >> 2] == 1;
        for (u32 m = 0x0B80; m < 0x0C00; m += 4) ok &= s_pso_quiet[m >> 2] && s_bind_quiet[m >> 2] == 1;
        s_w2_quiet_ok = ok;
    }
    return s_w2_quiet_ok;
}
void rsx_dispatch_w2_set(int on)
{
    if (on && rsx_dispatch_table == 1 && s_dsp_kind_ready) s_dsp_kind_p = s_dsp_kind2;
    else s_dsp_kind_p = s_dsp_kind;
}
int rsx_dispatch_w2_nop_run(rsx_dispatch* rsx, u32 count, u32 last_arg)
{
    if (rsx_dispatch_table != 1 || !count || !w2_quiet_ok()) return 0;
    rsx->seen[0x0100 >> 2] += count;
    rsx->regs[0x0100 >> 2] = last_arg;
    return 1;
}
int rsx_dispatch_w2_const_run(rsx_dispatch* rsx, u32 method, u32 count, const u32* args)
{
    if (s_w2_constlog < 0) s_w2_constlog = getenv("RSX_CONST_LOG") != NULL;
    if (rsx_dispatch_table != 1 || s_w2_constlog || !count || !w2_quiet_ok()) return 0;
    if (method < M_VP_UPLOAD_CONST_ID || method + (count - 1u) * 4u >= M_VP_UPLOAD_CONST + 32 * 4) return 0;
    int changed = 0;
    u32 m = method;
    for (u32 i = 0; i < count; i++, m += 4) {
        const u32 idx = m >> 2, arg = args[i];
        rsx->seen[idx]++;
        rsx->regs[idx] = arg;
        if (m >= M_VP_UPLOAD_CONST) {
            const u32 word = (m - M_VP_UPLOAD_CONST) >> 2;
            const u32 slot = rsx->regs[M_VP_UPLOAD_CONST_ID >> 2] + (word >> 2);
            if (slot < RSX_DSP_NUM_CONSTANTS) {
                u32* c = &rsx->constants[slot][word & 3];
                changed |= *c != arg;
                *c = arg;
            }
        }
    }
    if (changed) g_rsx_dsp_const_gen++;   /* LD_CB_GEN */
    return 1;
}
int rsx_dispatch_w2_prog_run(rsx_dispatch* rsx, u32 type, u32 method, u32 count, const u32* args)
{
    if (rsx_dispatch_table != 1 || !count || !w2_quiet_ok()) return 0;
    const u32 step = type == 0 ? 4u : 0u;
    if (method < M_VP_UPLOAD_INST || method + (count - 1u) * step >= M_VP_UPLOAD_INST + 32 * 4) return 0;
    u32* load = &rsx->regs[M_VP_UPLOAD_FROM_ID >> 2];
    u32 m = method;
    for (u32 i = 0; i < count; i++, m += step) {
        const u32 idx = m >> 2, arg = args[i];
        rsx->seen[idx]++;
        rsx->regs[idx] = arg;
        const u32 lane = ((m - M_VP_UPLOAD_INST) >> 2) & 3;
        const u32 pos = *load * 4 + lane;
        if (pos < RSX_DSP_VP_WORDS) {
            if (__builtin_expect(rsx_dsp_pso_epoch_on, 0) && rsx->vp[pos] != arg) {
                g_rsx_dsp_pso_epoch++;   /* LD_PSO_SAME: the program changed */
                g_rsx_dsp_vp_gen++; g_rsx_dsp_pso_stable++;   /* DRAW-COST: DRAWOPT_HEAD, DRAWOPT_PSO */
                g_rsx_dsp_pso_last = M_VP_UPLOAD_INST >> 2;
                g_rsx_dsp_bind_epoch++;  /* LD_DRAW_REUSE */
            }
            rsx->vp[pos] = arg;
        }
        if (lane == 3)
            (*load)++;
    }
    return 1;
}

/* WALK2 cut 3b (W2_LEAN, ascension host/walk/walk2): rsx_dispatch_method with its rare work out of line. The
 * compiled rsx_dispatch_method inlines dsp_method_effects (vertex-constant folding with float code), dsp_vp_const
 * and the out-of-range report, so EVERY word pays their prologue: seven pushes, an xmm6 save and the matching
 * pops, though ~3 words in 4 are PLAIN and only store. Here the same statements in the same order, the
 * rare ones in noinline functions: the plain word is a leaf. rsx_live_draw_method calls this one under W2_LEAN. */
__attribute__((noinline)) static void dsp_w2_bad(u32 method, u32 arg)
{
    static int n = 0;
    if (n++ < 8)
        fprintf(stderr, "[rsx-dispatch] method 0x%05X (arg 0x%08X) out of range -- dropped\n", method, arg);
}
__attribute__((noinline)) static void dsp_w2_changed(rsx_dispatch* rsx, u32 idx, u32 arg)
{
    dsp_pso_note(rsx, idx, arg);   /* regs[idx] != arg here: its counters move as in the inline test */
}
__attribute__((noinline)) static void dsp_w2_const(rsx_dispatch* rsx, u32 method, u32 arg)
{
    dsp_vp_const(rsx, method, arg);
}
__attribute__((noinline)) static void dsp_w2_effects(rsx_dispatch* rsx, u32 method, u32 arg)
{
    dsp_method_effects(rsx, method, arg);
}
void rsx_dispatch_method_w2(rsx_dispatch* rsx, u32 method, u32 arg)
{
    method &= 0xFFFFC;
    const u32 idx = method >> 2;
    if (__builtin_expect(idx >= RSX_DSP_NUM_REGS, 0)) { dsp_w2_bad(method, arg); return; }
    rsx->seen[idx]++;
    if (__builtin_expect(rsx_dsp_pso_epoch_on, 0) && rsx->regs[idx] != arg) dsp_w2_changed(rsx, idx, arg);   /* LD_PSO_SAME */
    rsx->regs[idx] = arg;
    int table = rsx_dispatch_table;
    if (__builtin_expect(table < 0, 0))
        table = dsp_table_decide();
    if (__builtin_expect(table, 1)) {
        const u8 k = s_dsp_kind_p[idx];
        if (k == DSP_K_PLAIN)
            return;
        if (k == DSP_K_CONST) {
            dsp_w2_const(rsx, method, arg);
            return;
        }
    }
    dsp_w2_effects(rsx, method, arg);
}

/* ---- getters ----------------------------------------------------------- */

u32 rsx_dsp_clear_color(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_CLEAR_COLOR_VALUE);
}

u32 rsx_dsp_clear_zstencil(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_CLEAR_DEPTH_VALUE);
}

void rsx_dsp_get_surface(const rsx_dispatch* rsx, rsx_dsp_surface* out)
{
    const u32 fmt = rsx_dsp_reg(rsx, M_RT_FORMAT);
    out->color_format = fmt & 0x1F;
    out->depth_format = (fmt >> 5) & 7;
    out->raster_type  = (fmt >> 8) & 0xF;

    const u32 h = rsx_dsp_reg(rsx, M_RT_HORIZ);
    const u32 v = rsx_dsp_reg(rsx, M_RT_VERT);
    out->clip_x = h & 0xFFFF;
    out->clip_w = h >> 16;
    out->clip_y = v & 0xFFFF;
    out->clip_h = v >> 16;
    /* Antialiased surfaces at their sample resolution (rsx_dispatch.h). A
     * texture that reads an AA surface's memory sees the sample grid: a
     * 320x180 SQUARE_ROTATED_4 target is a 640x360 image. */
    out->antialias = (fmt >> 12) & 0xF;
    out->aa_sx = (out->antialias >= 3 && out->antialias <= 5) ? 2u : 1u;
    out->aa_sy = (out->antialias == 4 || out->antialias == 5) ? 2u : 1u;
    out->clip_x *= out->aa_sx; out->clip_w *= out->aa_sx;
    out->clip_y *= out->aa_sy; out->clip_h *= out->aa_sy;

    out->color_offset[0] = rsx_dsp_reg(rsx, M_COLOR0_OFFSET);
    out->color_offset[1] = rsx_dsp_reg(rsx, M_COLOR1_OFFSET);
    out->color_offset[2] = rsx_dsp_reg(rsx, M_COLOR2_OFFSET);
    out->color_offset[3] = rsx_dsp_reg(rsx, M_COLOR3_OFFSET);
    out->color_pitch[0]  = rsx_dsp_reg(rsx, M_COLOR0_PITCH);
    out->color_pitch[1]  = rsx_dsp_reg(rsx, M_COLOR1_PITCH);
    out->color_pitch[2]  = rsx_dsp_reg(rsx, M_COLOR2_PITCH);
    out->color_pitch[3]  = rsx_dsp_reg(rsx, M_COLOR3_PITCH);
    out->color_location[0] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR0));
    out->color_location[1] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR1));
    out->color_location[2] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR2));
    out->color_location[3] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR3));
    out->color_target  = rsx_dsp_reg(rsx, M_RT_ENABLE);
    out->zeta_offset   = rsx_dsp_reg(rsx, M_ZETA_OFFSET);
    out->zeta_pitch    = rsx_dsp_reg(rsx, M_ZETA_PITCH);
    out->zeta_location = dma_to_location(rsx_dsp_reg(rsx, M_DMA_ZETA));
}

void rsx_dsp_get_viewport(const rsx_dispatch* rsx, rsx_dsp_viewport* out)
{
    const u32 h = rsx_dsp_reg(rsx, M_VIEWPORT_HORIZ);
    const u32 v = rsx_dsp_reg(rsx, M_VIEWPORT_VERT);
    out->x = h & 0xFFFF;
    out->w = h >> 16;
    out->y = v & 0xFFFF;
    out->h = v >> 16;
    for (u32 i = 0; i < 4; i++) {
        const u32 t = rsx_dsp_reg(rsx, M_VIEWPORT_TRANSLATE + i * 4);
        const u32 s = rsx_dsp_reg(rsx, M_VIEWPORT_SCALE + i * 4);
        memcpy(&out->translate[i], &t, 4);
        memcpy(&out->scale[i], &s, 4);
    }
}

void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out)
{
    memset(out, 0, sizeof(*out));
    if (index >= RSX_DSP_NUM_VERTEX_ATTR)
        return;
    const u32 fmt = rsx_dsp_reg(rsx, M_VTXFMT + index * 4);
    const u32 off = rsx_dsp_reg(rsx, M_VTXBUF_OFFSET + index * 4);
    out->type      = fmt & 7;
    out->size      = (fmt >> 4) & 0xF;
    out->stride    = (fmt >> 8) & 0xFF;
    out->frequency = fmt >> 16;
    out->offset    = off & 0x7FFFFFFF;
    out->location  = off >> 31;
}

void rsx_dsp_get_texture(const rsx_dispatch* rsx, u32 unit, rsx_dsp_texture* out)
{
    memset(out, 0, sizeof(*out));
    if (unit >= RSX_DSP_NUM_TEXTURES)
        return;
    const u32 base = M_TEX_OFFSET + unit * 0x20;
    const u32 fmt  = rsx_dsp_reg(rsx, base + 0x04);
    out->offset    = rsx_dsp_reg(rsx, base);
    out->location  = (fmt & 3) == 2 ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    out->cubemap   = (fmt >> 2) & 1;
    out->dimension = (fmt >> 4) & 0xF;
    out->format    = (fmt >> 8) & 0xFF;
    out->mipmaps   = fmt >> 16;
    out->wrap      = rsx_dsp_reg(rsx, base + 0x08);
    out->control0  = rsx_dsp_reg(rsx, base + 0x0C);
    out->enabled   = out->control0 >> 31;
    out->remap     = rsx_dsp_reg(rsx, base + 0x10);
    out->filter    = rsx_dsp_reg(rsx, base + 0x14);
    const u32 rect = rsx_dsp_reg(rsx, base + 0x18);
    out->width     = rect >> 16;
    out->height    = rect & 0xFFFF;
    out->border_color = rsx_dsp_reg(rsx, base + 0x1C);
    const u32 sz1  = rsx_dsp_reg(rsx, M_TEX_SIZE1 + unit * 4);
    out->pitch     = sz1 & 0xFFFFF;
    out->depth     = sz1 >> 20;
}

void rsx_dsp_get_vertex_texture(const rsx_dispatch* rsx, u32 unit,
                                rsx_dsp_vertex_texture* out)
{
    memset(out, 0, sizeof(*out));
    if (unit >= RSX_DSP_NUM_VERTEX_TEXTURES)
        return;
    const u32 base = M_VERTEX_TEXTURE + unit * 0x20;
    const u32 fmt  = rsx_dsp_reg(rsx, base + 0x04);
    out->offset    = rsx_dsp_reg(rsx, base + 0x00);
    out->location  = (fmt & 3) == 2 ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    out->cubemap   = (fmt >> 2) & 1;
    out->dimension = (fmt >> 4) & 0xF;
    out->format    = (fmt >> 8) & 0xFF;
    out->mipmaps   = fmt >> 16;
    out->wrap      = rsx_dsp_reg(rsx, base + 0x08);
    out->control0  = rsx_dsp_reg(rsx, base + 0x0C);
    out->enabled   = out->control0 >> 31;
    const u32 ctl3 = rsx_dsp_reg(rsx, base + 0x10);
    out->pitch     = ctl3 & 0xFFFFF;
    out->depth     = ctl3 >> 20;
    out->filter    = rsx_dsp_reg(rsx, base + 0x14);
    const u32 rect = rsx_dsp_reg(rsx, base + 0x18);
    out->width     = rect >> 16;
    out->height    = rect & 0xFFFF;
    out->border_color = rsx_dsp_reg(rsx, base + 0x1C);
}

u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_VERTEX_DATA_BASE);
}

u32 rsx_dsp_vertex_data_base_index(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_VB_ELEMENT_BASE);
}

u32 rsx_dsp_fragment_program(const rsx_dispatch* rsx, u32* location)
{
    const u32 v = rsx_dsp_reg(rsx, M_FP_ACTIVE_PROGRAM);
    if (location)
        *location = (v & 3) == 2 ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    return v & ~3u;
}

u32 rsx_dsp_shader_control(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_FP_CONTROL);
}

u32 rsx_dsp_vp_start(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_VP_START_FROM_ID);
}

int rsx_dsp_restart_index_enabled(const rsx_dispatch* rsx, int index_is_u32)
{
    if (!rsx_dsp_reg(rsx, M_RESTART_INDEX_ENABLE))
        return 0;
    /* RPCS3 oracle (rsx_methods.h restart_index_enabled()): a 16-bit index
     * buffer disables restart entirely if the configured sentinel doesn't
     * fit in 16 bits, rather than truncating it. */
    if (!index_is_u32 && rsx_dsp_reg(rsx, M_RESTART_INDEX) > 0xFFFFu)
        return 0;
    return 1;
}

u32 rsx_dsp_restart_index(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_RESTART_INDEX);
}

void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 index, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (index >= RSX_DSP_NUM_VERTEX_ATTR)
        return;
    u32 w[4];
    u32 any = 0;
    for (u32 i = 0; i < 4; i++) {
        w[i] = rsx_dsp_reg(rsx, M_VTX_ATTR_4F + index * 0x10 + i * 4);
        any |= w[i];
    }
    if (!any)
        return;   /* never written: hardware default (0,0,0,1) */
    memcpy(out, w, 16);
}

void rsx_dsp_get_index_array(const rsx_dispatch* rsx, rsx_dsp_index_array* out)
{
    out->offset   = rsx_dsp_reg(rsx, M_IDXBUF_OFFSET);
    const u32 fmt = rsx_dsp_reg(rsx, M_IDXBUF_FORMAT);
    out->location = fmt & 0xF;               /* location index directly     */
    out->is_u32   = ((fmt >> 4) & 0xFF) == 0; /* type 0 = u32, 1 = u16      */
}
