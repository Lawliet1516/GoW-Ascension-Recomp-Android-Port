/*
 * UBER (2026-11-08): the fragment-program ubershader.
 *
 * One pixel shader, compiled once, runs any RSX fragment program as DATA: the
 * CPU turns the program (and every per-draw value the specialised text would
 * bake in -- texture kinds, gamma masks, texel scales, sRGB output, alpha
 * test, window position, ...) into a small table of decoded instructions
 * (rsx_fp_uber_encode), and the shader interprets that table. A draw whose
 * specialised pipeline is still being built can so be drawn at once with a
 * pipeline that only needs the driver's ~0.5-6 ms create (the shader's
 * bytecode is the same every time) instead of DXC + create (~45 ms).
 *
 * The table reproduces rsx_fp_decompile_internal and the live draw's text
 * post-passes (shadow scale/edge patches, surface scale, sRGB output, output
 * RGB scale, alpha test, texel scale, shader window) operation by operation,
 * so the result is the specialised text's up to floating-point rounding.
 * Anything the decompiler refuses (or would make a text that cannot compile)
 * is refused here too: such a draw waits for its pipeline as before.
 */
#ifndef PS3RECOMP_RSX_FP_UBER_H
#define PS3RECOMP_RSX_FP_UBER_H

#include "ps3emu/ps3types.h"
#include "rsx_fp_decompiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_FP_UBER_HEADER_VEC4  32u     /* uint4 slots before the instructions */
#define RSX_FP_UBER_MAX_UOPS     1980u   /* (4096 - 32) / 2: one 64 KB constant buffer */
#define RSX_FP_UBER_MAX_BYTES    (4096u * 16u)

/* Everything the live draw decides per draw that the specialised pixel text
 * depends on (rsx_live_draw.c get_pso_full, the rsx_fp_decompile_* call and
 * the passes after it). Buffered pixel constants only. */
typedef struct rsx_fp_uber_knobs {
    u32 ctrl;                     /* SHADER_CONTROL (never RSX_FP_CTRL_AUTO) */
    u32 cube_mask, volume_mask;   /* rsx_fp_set_volume_mask's value */
    u32 expand_mask, noexpand_mask;
    rsx_fp_shadow_desc shadow[16];   /* func already mirrored (LD_SHADOW_SWAP) */
    u32 shadow_mask;              /* units with shadow[u].enabled */
    int shadow_cb;                /* g_ld_cur_shadow_cb: fp_shadow_scale[16] follows fp_alpha */
    int res_scaled;               /* g_ld_rs > 1 */
    int shadow_edge;              /* ld_shadow_edge_on() */
    u32 surf_units;               /* g_ld_cur_surf_units */
    u8 gamma_masks[16];           /* already limited to the program's gamma units */
    int gamma_on;                 /* the colour decompile variant was used (masks passed) */
    u32 srgb_output;
    int out_scale_on, out_scale_dyn, out_scale_sat;
    float out_scale[3];
    int alpha_test, alpha_func;   /* rs.alpha_test_enable, rs.alpha_func */
    u32 unnorm_mask;              /* after the shadow units were taken out */
    u32 unnorm_dim[16][2];
    u32 shader_window;            /* the key's value: 0 = no rewrite */
} rsx_fp_uber_knobs;

typedef struct rsx_fp_uber_info {
    u32 uops, vec4s;              /* instructions; uint4 slots used in total */
    u32 targets;                  /* colour targets the program feeds (1..4) */
    int depth_export;             /* needs the SV_Depth variant */
    u32 refuse;                   /* 0, or the RSX_FP_UBER_NO_* reason */
} rsx_fp_uber_info;

enum {
    RSX_FP_UBER_OK = 0,
    RSX_FP_UBER_NO_ARGS, RSX_FP_UBER_NO_ENV, RSX_FP_UBER_NO_SHADOW, RSX_FP_UBER_NO_GAMMA_OP,
    RSX_FP_UBER_NO_REG, RSX_FP_UBER_NO_SIZE, RSX_FP_UBER_NO_NEST, RSX_FP_UBER_NO_OUTSCALE,
    RSX_FP_UBER_NO_CONST, RSX_FP_UBER_NO_SHADOW_MIX, RSX_FP_UBER_NO_N
};
const char* rsx_fp_uber_refuse_name(u32 why);

/* Encode the program into out (uint4 slots, at most cap_vec4). Returns the
 * number of uint4 slots written (> 0), or -1 (info->refuse says why). */
int rsx_fp_uber_encode(const u8* ucode, u32 size, const rsx_fp_uber_knobs* k,
                       u32 (*out)[4], u32 cap_vec4, rsx_fp_uber_info* info);

/* The ubershader's HLSL (ps_6_0, entry main). depth: the SV_Depth variant. */
const char* rsx_fp_uber_hlsl(int depth);










/* UBER3 FAST BEGIN */
/* The fast path (2026-11-12): rsx_fp_uber_pack renames an encoded table's registers (in place) so the program runs
 * on a small register file; rsx_fp_uber_fast_class picks the smallest fast shader that holds *nreg registers (-1:
 * none, use the full ubershader with the UNPACKED table). */
#define RSX_FP_UBER_FAST_SIZES   5
#define RSX_FP_UBER_FAST_CLASSES (2 * RSX_FP_UBER_FAST_SIZES)   /* plain programs, then programs that need the general path */
#define RSX_FP_UBER_FAST_NREGS   4u, 8u, 12u, 16u, 24u
int rsx_fp_uber_pack(u32 (*table)[4], u32 cap_vec4, u32 opts, u32* nreg, int* general);
int rsx_fp_uber_fast_class(u32 nreg, int general);
u32 rsx_fp_uber_fast_nreg(int cls);
const char* rsx_fp_uber_fast_hlsl(int cls);   /* no SV_Depth variant: a depth-export program stays on the full ubershader */
/* UBER3 FAST END */

#ifdef __cplusplus
}
#endif
#endif
