/*
 * UBER (2026-11-08): the fragment-program ubershader -- see rsx_fp_uber.h.
 *
 * rsx_fp_uber_encode walks the program exactly as rsx_fp_decompile_internal
 * does (same offsets, same constant slots, same block structure) and writes,
 * per instruction that has an effect, one decoded operation of 8 words; the
 * per-draw values the specialised text bakes in go to a header. The shader
 * (rsx_fp_uber_hlsl) executes those operations with the same expressions the
 * decompiler prints, in the same order.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "rsx_fp_uber.h"

/* the decompiler's fields (rsx_fp_decompiler.c) */
#define FP_END              (1u << 0)
#define FP_OUT_REG_SHIFT    1
#define FP_OUT_REG_MASK     (63u << 1)
#define FP_OUT_HALF         (1u << 7)
#define FP_COND_WRITE       (1u << 8)
#define FP_OUT_MASK_SHIFT   9
#define FP_INPUT_SRC_SHIFT  13
#define FP_INPUT_SRC_MASK   (15u << 13)
#define FP_TEX_UNIT_SHIFT   17
#define FP_TEX_UNIT_MASK    (15u << 17)
#define FP_OPCODE_SHIFT     24
#define FP_OPCODE_MASK      (0x3Fu << 24)
#define FP_OUT_NONE         (1u << 30)
#define FP_OUT_SAT          (1u << 31)
#define FP_REG_TYPE_MASK    3u
#define FP_REG_TYPE_TEMP    0
#define FP_REG_TYPE_INPUT   1
#define FP_REG_TYPE_CONST   2
#define FP_SRC_REG_SHIFT    2
#define FP_SRC_REG_MASK     (63u << 2)
#define FP_SRC_HALF         (1u << 8)
#define FP_SRC_SWZ_SHIFT    9
#define FP_SRC_NEGATE       (1u << 17)
#define FP_SRC0_ABS         (1u << 29)
#define FP_SRC1_ABS         (1u << 18)
#define FP_SRC2_ABS         (1u << 18)
#define FP_BRANCH           (1u << 31)
#define FP_EXEC_IF_LT       (1u << 18)
#define FP_EXEC_IF_EQ       (1u << 19)
#define FP_EXEC_IF_GR       (1u << 20)
#define FP_COND_SWZ_SHIFT   21
#define FP_COND_MOD_REG     (1u << 30)
#define FP_COND_REG         (1u << 31)
#define FP_CTRL_32BIT_EXPORTS 0x40u

enum {
    OP_NOP=0x00, OP_MOV=0x01, OP_MUL=0x02, OP_ADD=0x03, OP_MAD=0x04,
    OP_DP3=0x05, OP_DP4=0x06, OP_DST=0x07, OP_MIN=0x08, OP_MAX=0x09,
    OP_SLT=0x0A, OP_SGE=0x0B, OP_SLE=0x0C, OP_SGT=0x0D, OP_SNE=0x0E,
    OP_SEQ=0x0F, OP_FRC=0x10, OP_FLR=0x11, OP_KIL=0x12, OP_PK4B=0x13,
    OP_UP4B=0x14, OP_DDX=0x15, OP_DDY=0x16, OP_TEX=0x17, OP_TXP=0x18,
    OP_TXD=0x19, OP_RCP=0x1A, OP_RSQ=0x1B, OP_EX2=0x1C, OP_LG2=0x1D,
    OP_LIT=0x1E, OP_LRP=0x1F, OP_STR=0x20, OP_SFL=0x21, OP_COS=0x22,
    OP_SIN=0x23, OP_PK2H=0x24, OP_UP2H=0x25, OP_POW=0x26, OP_PK4UB=0x27,
    OP_UP4UB=0x28, OP_PK2US=0x29, OP_UP2US=0x2A, OP_PKG=0x2C, OP_UPG=0x2D, OP_DP2A=0x2E, OP_TXL=0x2F,
    OP_TXB=0x31, OP_TEXBEM=0x33, OP_TXPBEM=0x34, OP_RFL=0x36, OP_DP2=0x38, OP_NRM=0x39, OP_DIV=0x3A,
    OP_DIVSQ=0x3B, OP_LIF=0x3C, OP_FENCT=0x3D, OP_FENCB=0x3E
};

/* the shader's operations (keep in step with the HLSL below) */
enum {
    U_NOP = 0, U_MOV, U_MUL, U_ADD, U_MAD, U_DP3, U_DP3P, U_DP4, U_MIN, U_MAX, U_FRC, U_FLR, U_DDX, U_DDY,
    U_RCP, U_RSQ, U_EX2, U_LG2, U_COS, U_SIN, U_POW, U_DIV, U_DIVSQ, U_DP2, U_NRM, U_LRP, U_LIF,
    U_SLT, U_SGE, U_SLE, U_SGT, U_SNE, U_SEQ,
    U_PK4UB, U_UP4UB, U_PKG, U_UPG, U_PK4B, U_UP4B, U_PK2US, U_UP2US, U_PK2H, U_UP2H,
    U_TEX, U_TXP, U_TXB, U_TXL, U_KIL,
    U_IF = 60, U_ELSE, U_LOOP, U_ENDLOOP, U_BRK
};

const char* rsx_fp_uber_refuse_name(u32 why)
{
    static const char* const n[RSX_FP_UBER_NO_N] = { "ok", "args", "decompiler switch not default", "depth compare layout/fetch",
        "gamma on a TXD/TEXBEM fetch", "register past r15/h31", "too long", "flow control nested too deep",
        "output scale with several targets", "constants", "depth compare with only 3D units" };
    return why < RSX_FP_UBER_NO_N ? n[why] : "?";
}

static u32 rd(const u8* p) { return rsx_fp_read_word(p); }

static int env_is(const char* name, char v)
{
    const char* e = getenv(name);
    return e && e[0] == v;
}
/* 1 when every decompiler switch the shader models is at its default */
static int uber_env_default(void)
{
    static int ok = -1;
    if (ok < 0)
        ok = !env_is("LD_FP_PREC", '0') && !env_is("LD_FP_SRC2ABS", '0') && !env_is("LD_FP_FACE", '0') &&
             !env_is("LD_FP_NRM_SAFE", '0') && !env_is("LD_FP_DP3_PRECISE", '0') && !env_is("LD_FP_BRANCH_ANY", '0') &&
             !env_is("LD_FP_CC_PRED", '0') && !env_is("LD_FP_EXPORT_GATHER", '0') && !env_is("LD_FP_ALIAS_SYNC", '1') &&
             !env_is("LD_EXP_TEX", '0') && !env_is("LD_FP_NO_PERSP_CORR", '1') && !env_is("LD_WPOS_SNAP", '1') &&
             !getenv("RSX_FP_CTRL_AUTO");
    return ok;
}

static int has_const(u32 w1, u32 w2, u32 w3)
{
    return (w1 & 3u) == FP_REG_TYPE_CONST || (w2 & 3u) == FP_REG_TYPE_CONST || (w3 & 3u) == FP_REG_TYPE_CONST;
}

/* one source word -> the shader's source field */
static u32 enc_src(u32 w, int abs_bit, u32 prec, int has_k)
{
    const u32 type = w & 3u;
    const u32 idx = (w & FP_SRC_REG_MASK) >> FP_SRC_REG_SHIFT;
    u32 s = type | (idx << 2) | ((w & FP_SRC_HALF) ? (1u << 8) : 0u) | (((w >> FP_SRC_SWZ_SHIFT) & 0xFFu) << 9);
    if (w & FP_SRC_NEGATE) s |= 1u << 17;
    if (abs_bit) s |= 1u << 18;
    s |= (prec & 7u) << 19;
    if (has_k) s |= 1u << 22;
    return s;
}
/* fp_src_prec */
static u32 src_prec(u32 w, u32 w2, u32 idx, u32 input_src)
{
    const u32 prec = (w2 >> (19u + 3u * idx)) & 7u;
    if (prec == 0 || prec > 4) return 0;
    const u32 type = w & 3u;
    if (type == FP_REG_TYPE_TEMP && (w & FP_SRC_HALF) && prec == 1) return 0;
    if (type == FP_REG_TYPE_INPUT && (input_src >= 1 && input_src <= 3)) return 0;
    if (type == 3) return 0;
    return prec;
}
static const u32 export_reg[2][4] = { { 0, 2, 3, 4 }, { 0, 4, 6, 8 } };
static int export_target(u32 index, int half)
{
    for (int t = 0; t < 4; t++) if (export_reg[half][t] == index) return t;
    return -1;
}

int rsx_fp_uber_encode(const u8* ucode, u32 max_bytes, const rsx_fp_uber_knobs* k,
                       u32 (*out)[4], u32 cap_vec4, rsx_fp_uber_info* info)
{
    rsx_fp_uber_info dummy;
    if (!info) info = &dummy;
    memset(info, 0, sizeof *info);
#define REFUSE(r) do { info->refuse = (r); return -1; } while (0)
    if (!ucode || !k || !out || cap_vec4 < RSX_FP_UBER_HEADER_VEC4 + 2u) REFUSE(RSX_FP_UBER_NO_ARGS);
    if (!uber_env_default() || k->ctrl == RSX_FP_CTRL_AUTO) REFUSE(RSX_FP_UBER_NO_ENV);
    if (max_bytes > RSX_FP_UBER_MAX_BYTES) REFUSE(RSX_FP_UBER_NO_SIZE);
    const u32 cube_mask = k->cube_mask;
    const u32 vol_mask = k->volume_mask & ~cube_mask;
    const u32 xyz_mask = cube_mask | vol_mask;
    const u32 sampled = rsx_fp_texture_units(ucode, max_bytes);
    const rsx_fp_shadow_desc* shadow = k->shadow;
    for (u32 u = 0; u < 16; u++)
        if ((sampled & (1u << u)) && shadow[u].enabled &&
            (shadow[u].enabled != 1 || shadow[u].func > 7 || shadow[u].linear > 1 || shadow[u].unnormalized > 1 ||
             (cube_mask & (1u << u))))
            REFUSE(RSX_FP_UBER_NO_SHADOW);
    u32 shadow_used = 0;
    for (u32 u = 0; u < 16; u++) if ((sampled & (1u << u)) && shadow[u].enabled) shadow_used |= 1u << u;
    /* fp_shadow_helper names the texture rsx_tex[u] unless a CUBE unit exists; with 3D units only, the
     * declarations are per unit and the text does not compile */
    if (shadow_used && !cube_mask && vol_mask) REFUSE(RSX_FP_UBER_NO_SHADOW_MIX);
    if (shadow_used && !k->shadow_cb) REFUSE(RSX_FP_UBER_NO_SHADOW);
    rsx_fp_constant_block* cb = (rsx_fp_constant_block*)malloc(sizeof *cb);
    if (!cb) REFUSE(RSX_FP_UBER_NO_ARGS);
    const int ncons = rsx_fp_collect_constants_used(ucode, max_bytes, cb);
    free(cb);
    if (ncons < 0) REFUSE(RSX_FP_UBER_NO_CONST);

    memset(out, 0, (size_t)RSX_FP_UBER_HEADER_VEC4 * 16u);
    u32 n = 0;   /* uops */
    const u32 max_uops = (cap_vec4 - RSX_FP_UBER_HEADER_VEC4) / 2u < RSX_FP_UBER_MAX_UOPS
                       ? (cap_vec4 - RSX_FP_UBER_HEADER_VEC4) / 2u : RSX_FP_UBER_MAX_UOPS;
#define UOP(i) (&out[RSX_FP_UBER_HEADER_VEC4 + 2u * (i)][0])   /* 8 words */
#define NEWUOP() (n < max_uops ? (memset(UOP(n), 0, 32), UOP(n++)) : NULL)

    int wrote[2][4];
    memset(wrote, 0, sizeof wrote);
    u8 last_w[64 * 8];
    memset(last_w, 0, sizeof last_w);
    int any_branch = 0;
    u32 off = 0, constant_slot = 0;
    enum { MAXB = 32 };
    u32 blk_else[MAXB], blk_end[MAXB];
    int blk_loop[MAXB];
    u32 blk_uop[MAXB];      /* the IF / LOOP uop of the block */
    u32 blk_elseuop[MAXB];  /* the ELSE uop (jump to end), or ~0 */
    int n_blk = 0, loops_open = 0, loop_depth_max = 0;
    u32 brk_fix[256]; u32 n_brk = 0;   /* BRK uops: exit patched at their loop's close */
    u32 brk_blk[256];

    while (off + 16 <= max_bytes) {
        while (n_blk > 0 && blk_end[n_blk - 1] == off) {
            const int b = --n_blk;
            if (blk_loop[b]) {
                u32* e = NEWUOP();
                if (!e) REFUSE(RSX_FP_UBER_NO_SIZE);
                e[0] = U_ENDLOOP;
                e[5] = blk_uop[b] + 1u;               /* the body's first uop */
                UOP(blk_uop[b])[5] = n;               /* LOOP: exit = after the ENDLOOP */
                for (u32 i = 0; i < n_brk; i++) if (brk_blk[i] == (u32)b && brk_fix[i] != ~0u) {
                    UOP(brk_fix[i])[5] = n; brk_fix[i] = ~0u; }
                loops_open--;
            } else {
                u32* f = UOP(blk_uop[b]);
                if (blk_elseuop[b] == ~0u) f[5] = n;  /* no else: false -> end */
                else UOP(blk_elseuop[b])[5] = n;      /* the then-branch's end jumps here */
                f[6] = n;
            }
        }
        if (n_blk > 0 && blk_else[n_blk - 1] == off) {
            u32* e = NEWUOP();
            if (!e) REFUSE(RSX_FP_UBER_NO_SIZE);
            e[0] = U_ELSE;
            blk_elseuop[n_blk - 1] = n - 1u;
            UOP(blk_uop[n_blk - 1])[5] = n;           /* false -> the else branch */
            blk_else[n_blk - 1] = 0;
        }
        const u32 w0 = rd(ucode + off + 0), w1 = rd(ucode + off + 4), w2 = rd(ucode + off + 8), w3 = rd(ucode + off + 12);
        off += 16;
        const u32 opcode = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
        const u32 input_src = (w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
        const u32 tex_unit = (w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT;
        if (!(w2 & FP_BRANCH) && (shadow_used >> tex_unit & 1u) &&
            (opcode == OP_TXB || opcode == OP_TXL || opcode == OP_TXD || opcode == OP_TEXBEM || opcode == OP_TXPBEM))
            REFUSE(RSX_FP_UBER_NO_SHADOW);
        /* (the decompiler tests shadow[tex_unit].enabled for any unit here, sampled or not) */
        if (!(w2 & FP_BRANCH) && shadow[tex_unit].enabled &&
            (opcode == OP_TXB || opcode == OP_TXL || opcode == OP_TXD || opcode == OP_TEXBEM || opcode == OP_TXPBEM))
            REFUSE(RSX_FP_UBER_NO_SHADOW);
        const int is_branch = (w2 & FP_BRANCH) ? 1 : 0;
        const int lt = (w1 & FP_EXEC_IF_LT) != 0, eq = (w1 & FP_EXEC_IF_EQ) != 0, gr = (w1 & FP_EXEC_IF_GR) != 0;
        const int is_uncond = lt && eq && gr, is_never = !lt && !eq && !gr;
        const int set_cond = (w0 & FP_COND_WRITE) != 0;
        const u32 cc_read = (w1 & FP_COND_REG) ? 1u : 0u, cc_write = (w1 & FP_COND_MOD_REG) ? 1u : 0u;
        const u32 cswz = (w1 >> FP_COND_SWZ_SHIFT) & 0xFFu;
        u32 cmp;   /* 0 ==, 1 >=, 2 <=, 3 !=, 4 >, 5 < */
        if (gr && eq) cmp = 1; else if (lt && eq) cmp = 2; else if (gr && lt) cmp = 3;
        else if (gr) cmp = 4; else if (lt) cmp = 5; else cmp = 0;
        const u32 exec = is_uncond ? 0u : is_never ? 1u : 2u;
        const u32 w1x = exec | (cmp << 2) | (cswz << 5) | (input_src << 13) | (tex_unit << 17);
        const u32 s0t = w1 & 3u, s1t = w2 & 3u, s2t = w3 & 3u;
        int has_k = 0;
        if (s0t == FP_REG_TYPE_CONST || s1t == FP_REG_TYPE_CONST || s2t == FP_REG_TYPE_CONST) {
            if (off + 16 <= max_bytes) off += 16;
            has_k = 1;
        }
        const u32 slot = constant_slot;
        if (has_k) constant_slot++;
        if (slot > 2047u) REFUSE(RSX_FP_UBER_NO_CONST);

        if (is_branch) {
            any_branch = 1;
            const u32 end_at = (w3 & 0x7FFFFFFFu) << 2;
            switch (opcode) {
            case 0x02: case 0x03: case 0x04: {
                if (n_blk == MAXB || end_at <= off) break;   /* malformed: skipped, as the decompiler does */
                u32* e = NEWUOP();
                if (!e) REFUSE(RSX_FP_UBER_NO_SIZE);
                e[1] = w1x;
                e[0] = (opcode == 0x02 ? U_IF : U_LOOP) | (cc_read << 28);
                if (opcode == 0x02) {
                    const u32 else_at = (w2 & 0x7FFFFFFFu) << 2;
                    blk_else[n_blk] = else_at != end_at ? else_at : 0;
                    blk_loop[n_blk] = 0;
                } else {
                    const u32 end_n = (w2 >> 2) & 0xFFu, init = (w2 >> 10) & 0xFFu;
                    u32 inc = (w2 >> 19) & 0xFFu;
                    if (!inc) inc = 1;
                    e[6] = init < end_n ? (end_n - init + inc - 1u) / inc : 0u;   /* iterations */
                    blk_else[n_blk] = 0;
                    blk_loop[n_blk] = 1;
                    loops_open++;
                    if (loops_open > loop_depth_max) loop_depth_max = loops_open;
                    if (loops_open > 4) REFUSE(RSX_FP_UBER_NO_NEST);
                }
                blk_uop[n_blk] = n - 1u;
                blk_elseuop[n_blk] = ~0u;
                blk_end[n_blk++] = end_at;
                break; }
            case 0x00:   /* BRK: only inside a loop */
                if (loops_open) {
                    int b = n_blk - 1;
                    while (b >= 0 && !blk_loop[b]) b--;
                    if (b < 0 || n_brk >= 256) REFUSE(RSX_FP_UBER_NO_NEST);
                    u32* e = NEWUOP();
                    if (!e) REFUSE(RSX_FP_UBER_NO_SIZE);
                    e[0] = U_BRK | (cc_read << 28);
                    e[1] = w1x;
                    brk_fix[n_brk] = n - 1u; brk_blk[n_brk++] = (u32)b;
                }
                break;
            default: break;   /* CAL / RET: skipped */
            }
            if (w0 & FP_END) break;
            continue;
        }
        const u32 g = k->gamma_masks[tex_unit] & 15u;
        const int exp_tex = (w0 >> 21) & 1u;
        if (g && !exp_tex && (opcode == OP_TXD || opcode == OP_TEXBEM || opcode == OP_TXPBEM))
            REFUSE(RSX_FP_UBER_NO_GAMMA_OP);
        u32 uop = U_NOP;
        int handled = 1;
        switch (opcode) {
        case OP_MOV: uop = U_MOV; break;
        case OP_MUL: uop = U_MUL; break;
        case OP_ADD: uop = U_ADD; break;
        case OP_MAD: uop = U_MAD; break;
        case OP_DP3: uop = ((w0 >> 22) & 3u) == 0 ? U_DP3P : U_DP3; break;
        case OP_DP4: uop = U_DP4; break;
        case OP_MIN: uop = U_MIN; break;
        case OP_MAX: uop = U_MAX; break;
        case OP_FRC: uop = U_FRC; break;
        case OP_FLR: uop = U_FLR; break;
        case OP_DDX: uop = U_DDX; break;
        case OP_DDY: uop = U_DDY; break;
        case OP_RCP: uop = U_RCP; break;
        case OP_PK4UB: uop = U_PK4UB; break;
        case OP_UP4UB: uop = U_UP4UB; break;
        case OP_PKG: uop = U_PKG; break;
        case OP_UPG: uop = U_UPG; break;
        case OP_PK4B: uop = U_PK4B; break;
        case OP_UP4B: uop = U_UP4B; break;
        case OP_PK2US: uop = U_PK2US; break;
        case OP_UP2US: uop = U_UP2US; break;
        case OP_PK2H: uop = U_PK2H; break;
        case OP_UP2H: uop = U_UP2H; break;
        case OP_RSQ: uop = U_RSQ; break;
        case OP_EX2: uop = U_EX2; break;
        case OP_LG2: uop = U_LG2; break;
        case OP_COS: uop = U_COS; break;
        case OP_SIN: uop = U_SIN; break;
        case OP_POW: uop = U_POW; break;
        case OP_DIV: uop = U_DIV; break;
        case OP_DIVSQ: uop = U_DIVSQ; break;
        case OP_DP2: uop = U_DP2; break;
        case OP_NRM: uop = U_NRM; break;
        case OP_LRP: uop = U_LRP; break;
        case OP_LIF: uop = U_LIF; break;
        case OP_SLT: uop = U_SLT; break;
        case OP_SGE: uop = U_SGE; break;
        case OP_SLE: uop = U_SLE; break;
        case OP_SGT: uop = U_SGT; break;
        case OP_SNE: uop = U_SNE; break;
        case OP_SEQ: uop = U_SEQ; break;
        case OP_TEX: uop = U_TEX; break;
        case OP_TXP: uop = U_TXP; break;
        case OP_TXB: uop = U_TXB; break;
        case OP_TXL: uop = U_TXL; break;
        case OP_KIL: uop = U_KIL; handled = 0; break;
        default: handled = 0; break;   /* NOP, FENCT/FENCB and the unhandled opcodes: no effect */
        }
        const int has_dest = !(w0 & FP_OUT_NONE);
        if (uop == U_KIL) {
            if (!is_never) {
                u32* e = NEWUOP();
                if (!e) REFUSE(RSX_FP_UBER_NO_SIZE);
                e[0] = U_KIL | (cc_read << 28);
                e[1] = w1x;
            }
        } else if (handled && (has_dest || set_cond)) {
            const u32 dst_idx = (w0 & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            const int dst_half = (w0 & FP_OUT_HALF) ? 1 : 0;
            u32 dm = (w0 >> FP_OUT_MASK_SHIFT) & 0xFu;
            if (!dm) dm = 0xFu;
            /* sources: their registers must exist (r[48] / h[48]: a larger index does not compile) */
            const u32 ws[3] = { w1, w2, w3 };
            /* the sources the operation reads (the others never reach the text) */
            const u32 nsrc = (uop == U_MAD || uop == U_LRP) ? 3u :
                (uop == U_MUL || uop == U_ADD || uop == U_DP3 || uop == U_DP3P || uop == U_DP4 || uop == U_MIN ||
                 uop == U_MAX || uop == U_POW || uop == U_DIV || uop == U_DIVSQ || uop == U_DP2 ||
                 (uop >= U_SLT && uop <= U_SEQ) || uop == U_TXB || uop == U_TXL) ? 2u : 1u;
            for (u32 si = 0; si < nsrc; si++)
                if ((ws[si] & 3u) == FP_REG_TYPE_TEMP &&
                    ((ws[si] & FP_SRC_REG_MASK) >> FP_SRC_REG_SHIFT) >= ((ws[si] & FP_SRC_HALF) ? 32u : 16u))
                    REFUSE(RSX_FP_UBER_NO_REG);
            /* the shader keeps r0-r15 and h0-h31 (the game's highest: r12, h25 over all 32,990 programs) */
            if (has_dest && dst_idx >= (dst_half ? 32u : 16u)) REFUSE(RSX_FP_UBER_NO_REG);
            const int persp = (w3 & 0x80000000u) != 0;
            u32 dclamp = 0;
            const int sat = (w0 & FP_OUT_SAT) != 0;
            if (!sat) {
                const u32 dprec = (w0 >> 22) & 3u;
                const int skip = dprec == 0 ||
                    opcode == OP_NRM || opcode == OP_MAX || opcode == OP_MIN ||
                    opcode == OP_COS || opcode == OP_SIN || opcode == OP_RFL ||
                    opcode == OP_FRC || opcode == OP_LIT || opcode == OP_LIF || opcode == OP_LG2 ||
                    (opcode == OP_MOV && dst_half && (w1 & FP_SRC_HALF) && (w1 & 3u) == FP_REG_TYPE_TEMP) ||
                    (dprec == 1 && !dst_half);
                if (!skip) dclamp = dprec;   /* 1 fp16, 2 fx12, 3 fx9 */
            }
            const u32 scale = (w2 >> 28) & 7u;
            u32* e = NEWUOP();
            if (!e) REFUSE(RSX_FP_UBER_NO_SIZE);
            e[0] = uop | (dst_idx << 8) | ((u32)dst_half << 14) | (dm << 15) | ((u32)has_dest << 19) | ((u32)sat << 20) |
                   (dclamp << 21) | (scale << 23) | ((u32)set_cond << 26) | (cc_write << 27) | (cc_read << 28) |
                   ((u32)exp_tex << 29) | ((u32)persp << 30);
            e[1] = w1x | (slot << 21);
            e[2] = enc_src(w1, (w1 & FP_SRC0_ABS) != 0, src_prec(w1, w2, 0, input_src), has_k);
            e[3] = enc_src(w2, (w2 & FP_SRC1_ABS) != 0, src_prec(w2, w2, 1, input_src), has_k);
            e[4] = enc_src(w3, (w3 & FP_SRC2_ABS) != 0, src_prec(w3, w2, 2, input_src), has_k);
            if (has_dest) {
                const int t = export_target(dst_idx, dst_half);
                if (t >= 0) wrote[dst_half][t] = 1;
                for (u32 c = 0; c < 4; c++) {
                    if (!((dm >> c) & 1u)) continue;
                    if (dst_half) { if (dst_idx * 4u + c < sizeof last_w) last_w[dst_idx * 4u + c] = 'H'; }
                    else if (dst_idx * 8u + 2u * c + 1u < sizeof last_w)
                        last_w[dst_idx * 8u + 2u * c] = last_w[dst_idx * 8u + 2u * c + 1u] = 'R';
                }
            }
        }
        if (w0 & FP_END) break;
    }
    /* blocks never closed: closed at the end (the decompiler's trailing braces) */
    while (n_blk > 0) {
        const int b = --n_blk;
        if (blk_loop[b]) {
            u32* e = NEWUOP();
            if (!e) REFUSE(RSX_FP_UBER_NO_SIZE);
            e[0] = U_ENDLOOP; e[5] = blk_uop[b] + 1u;
            UOP(blk_uop[b])[5] = n;
            for (u32 i = 0; i < n_brk; i++) if (brk_blk[i] == (u32)b && brk_fix[i] != ~0u) { UOP(brk_fix[i])[5] = n; brk_fix[i] = ~0u; }
        } else {
            u32* f = UOP(blk_uop[b]);
            if (blk_elseuop[b] == ~0u) f[5] = n; else UOP(blk_elseuop[b])[5] = n;
            f[6] = n;
        }
    }

    const int half_exports = !(k->ctrl & FP_CTRL_32BIT_EXPORTS);
    u32 targets = 1;
    for (u32 t = 4; t-- > 1; ) if (wrote[half_exports][t]) { targets = t + 1; break; }
    const int depth_export = (k->ctrl & 0xEu) != 0u;
    const int gather = half_exports && !any_branch;
    if (k->out_scale_on && (targets > 1 || depth_export)) REFUSE(RSX_FP_UBER_NO_OUTSCALE);

    /* header */
    u32 flags = 0;
    if (half_exports) flags |= 1u;
    if (depth_export) flags |= 2u;
    if (k->srgb_output) flags |= 4u;
    u32 acmp = 7u;
    if (k->alpha_test) {
        const u32 f = (u32)k->alpha_func;
        acmp = f >= 0x0200u && f <= 0x0207u ? f - 0x0200u : 7u;
        if (acmp != 7u) flags |= 8u;
    }
    if (k->out_scale_on) {
        flags |= 16u;
        if (k->out_scale_sat) flags |= 32u;
        if (k->out_scale_dyn) flags |= 64u;
    }
    const u32 sw = k->shader_window;
    if (sw) {
        const int bottom = ((sw >> 12) & 0xFu) == 1u, integer = ((sw >> 16) & 0xFu) == 1u;
        if (bottom || integer || k->res_scaled) {
            flags |= 128u;
            if (bottom) flags |= 256u;
            if (integer) flags |= 512u;
            if (k->res_scaled) flags |= 1024u;
        }
    }
    if (gather) flags |= 2048u;
    out[0][0] = n; out[0][1] = flags; out[0][2] = targets; out[0][3] = acmp;
    out[1][0] = ncons ? (u32)ncons : 1u;
    { const float hgt = (float)(sw & 0xFFFu); memcpy(&out[1][1], &hgt, 4); }
    out[1][2] = k->shadow_cb ? 1u : 0u;
    if (k->out_scale_on && !k->out_scale_dyn) memcpy(out[2], k->out_scale, 12);
    for (u32 t = 0; gather && t < targets; t++) {
        const u32 reg = export_reg[1][t];
        for (u32 c = 0; c < 4; c++) {
            if (reg * 4u + c >= sizeof last_w || last_w[reg * 4u + c] != 'R') continue;
            out[3 + t][c] = (1u << 15) | (reg >> 1) | ((((reg & 1u) << 1) + (c >> 1)) << 8) | ((c & 1u) << 10);
        }
    }
    for (u32 u = 0; u < 16; u++) {
        u32 f = (cube_mask >> u & 1u) ? 1u : (vol_mask >> u & 1u) ? 2u : 0u;
        if (shadow_used >> u & 1u) {
            const rsx_fp_shadow_desc* d = &shadow[u];
            f |= 4u | ((u32)(d->func & 7u) << 3) | ((u32)(d->linear & 1u) << 6) | ((u32)(d->unnormalized & 1u) << 7);
            const u32 mode = d->unnormalized ? (k->res_scaled ? 3u : 2u) : 1u;
            f |= mode << 16;
            if (k->shadow_edge && d->linear && !d->unnormalized) f |= 1u << 18;
        }
        if (k->expand_mask >> u & 1u) f |= 1u << 8;
        if (k->noexpand_mask >> u & 1u) f |= 1u << 9;
        f |= (u32)(k->gamma_masks[u] & 15u) << 10;
        const u32 un = k->unnorm_mask & ~(cube_mask | vol_mask);
        if ((un >> u & 1u) && k->unnorm_dim[u][0] && k->unnorm_dim[u][1]) {
            f |= 1u << 14;
            /* the text's literal: %.9g of 1.0 / size, read back as a float */
            const float sx = (float)(1.0 / (double)k->unnorm_dim[u][0]), sy = (float)(1.0 / (double)k->unnorm_dim[u][1]);
            memcpy(&out[7 + u][1], &sx, 4); memcpy(&out[7 + u][2], &sy, 4);
        }
        if ((k->surf_units >> u & 1u) && !(xyz_mask >> u & 1u) && !(shadow_used >> u & 1u)) f |= 1u << 15;
        out[7 + u][0] = f;
    }
    info->uops = n;
    info->vec4s = RSX_FP_UBER_HEADER_VEC4 + 2u * n;
    info->targets = targets;
    info->depth_export = depth_export;
    (void)loop_depth_max;
    return (int)info->vec4s;
#undef REFUSE
#undef UOP
#undef NEWUOP
}

/* ------------------------------------------------------------------------- */
/* The shader. Expressions are the decompiler's (rsx_fp_decompiler.c), spelled
 * the same way, so the compiler lowers them the same way. */
static const char s_uber_body[] =
"struct PSInput {\n"
"    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
"    float4 fog : FOG;\n"
"    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
"    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
"    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n"
"};\n"
"Texture2D    UT2[16] : register(t0);\n"
"TextureCube  UTC[16] : register(t0, space1);\n"
"Texture3D    UT3[16] : register(t0, space2);\n"
"SamplerState US[16] : register(s0);\n"
"cbuffer PSConstants : register(b1) { float4 C[4096]; };\n"
"cbuffer UberProgram : register(b2) { uint4 P[4096]; };\n"
"struct PSOutput {\n"
"    float4 t0 : SV_TARGET0; float4 t1 : SV_TARGET1; float4 t2 : SV_TARGET2; float4 t3 : SV_TARGET3;\n"
"#ifdef UBER_DEPTH\n"
"    float depth : SV_Depth;\n"
"#endif\n"
"};\n"
"static float4 R[48];   /* r0-r15, then h0-h31: the game's programs reach r12 and h25 */\n"
"static float4 IN[16];\n"
"static float4 CC[2];\n"
"float4 _rsx_gamma_input(float4 v, uint mask) {\n"
"    float4 lo = v / 12.92;\n"
"    float4 hi = pow(max((v + 0.055) / 1.055, (float4)0), (float4)2.4);\n"
"    float4 decoded = float4(v.r > 0.04045 ? hi.r : lo.r,\n"
"                            v.g > 0.04045 ? hi.g : lo.g,\n"
"                            v.b > 0.04045 ? hi.b : lo.b,\n"
"                            v.a > 0.04045 ? hi.a : lo.a);\n"
"    return float4((mask & 1u) ? decoded.r : v.r,\n"
"                  (mask & 2u) ? decoded.g : v.g,\n"
"                  (mask & 4u) ? decoded.b : v.b,\n"
"                  (mask & 8u) ? decoded.a : v.a);\n}\n"
"uint   _rsx_pk4ub(float4 v) { uint4 b = (uint4)round(saturate(v) * 255.0); return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
"float4 _rsx_up4ub(float v) { uint u = asuint(v); return float4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u) / 255.0; }\n"
"uint   _rsx_pk4b(float4 v) { uint4 b = (uint4)((int4)round(clamp(v, -1.0, 1.0) * 127.0)) & 255u; return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
"float4 _rsx_up4b(float v) { uint u = asuint(v); int4 b = int4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u); b = (b << 24) >> 24; return clamp((float4)b / 127.0, -1.0, 1.0); }\n"
"uint   _rsx_pk2us(float4 v) { uint2 b = (uint2)round(saturate(v.xy) * 65535.0); return b.x | (b.y << 16); }\n"
"float4 _rsx_up2us(float v) { uint u = asuint(v); return float4(u & 65535u, u >> 16, u & 65535u, u >> 16) / 65535.0; }\n"
"uint   _rsx_pk2h(float4 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }\n"
"float4 _rsx_up2h(float v) { uint u = asuint(v); return float4(f16tof32(u & 65535u), f16tof32(u >> 16), f16tof32(u & 65535u), f16tof32(u >> 16)); }\n"
"float4 _rsx_lin2srgb(float4 cl) { cl = saturate(cl); float4 low = cl * 12.92; float4 high = 1.055 * pow(cl, 1.0 / 2.4) - 0.055; return saturate((cl < 0.0031308) ? low : high); }\n"
"float4 _rsx_srgb2lin(float4 cs) { float4 a = cs / 12.92; float4 b = pow(max(cs + 0.055, 0.0) / 1.055, 2.4); return (cs > 0.04045) ? b : a; }\n"
"float3 _rsx_nrm(float3 v) { return dot(v, v) > 0.0 ? normalize(v) : v; }\n"
"float _rsx_dp3p(float3 a, float3 b) { return mad(a.x, b.x, mad(a.y, b.y, a.z * b.z)); }\n"
"float4 _rsx_srgb_output(float4 v) {\n"
"    float3 lo = v.rgb * 12.92;\n"
"    float3 hi = 1.055 * pow(max(v.rgb, (float3)0), (float3)(1.0 / 2.4)) - 0.055;\n"
"    float3 rgb = float3(v.r < 0.0031308 ? lo.r : hi.r,\n"
"                         v.g < 0.0031308 ? lo.g : hi.g,\n"
"                         v.b < 0.0031308 ? lo.b : hi.b);\n"
"    return float4(saturate(rgb), v.a);\n}\n"
"float4 uswz(float4 v, uint s) { return float4(v[s & 3u], v[(s >> 2) & 3u], v[(s >> 4) & 3u], v[(s >> 6) & 3u]); }\n"
"bool4 ucmp(float4 v, uint c) {\n"
"    if (c == 1u) return v >= (float4)0.0; if (c == 2u) return v <= (float4)0.0; if (c == 3u) return v != (float4)0.0;\n"
"    if (c == 4u) return v > (float4)0.0; if (c == 5u) return v < (float4)0.0; return v == (float4)0.0;\n}\n"
/* a source: fetch, swizzle, precision (saturate before abs, the rest after), abs, negate */
"float4 usrc(uint s, uint insrc, bool persp, uint slot, float4 wpos_w) {\n"
"    uint type = s & 3u; float4 v;\n"
"    if (type == 0u) v = ((s >> 8) & 1u) ? R[16u + min((s >> 2) & 63u, 31u)] : R[min((s >> 2) & 63u, 15u)];\n"
"    else if (type == 1u) { v = IN[insrc]; if (persp && insrc >= 4u && insrc <= 13u) v = v / wpos_w.w; }\n"
"    else v = ((s >> 22) & 1u) ? C[slot] : (float4)0;\n"
"    v = uswz(v, (s >> 9) & 255u);\n"
"    uint prec = (s >> 19) & 7u;\n"
"    if (prec == 4u) v = clamp(v, 0.0, 1.0);\n"
"    if ((s >> 18) & 1u) v = abs(v);\n"
"    if (prec == 1u) v = clamp(v, -65504.0, 65504.0); else if (prec == 2u) v = clamp(v, -2.0, 2.0); else if (prec == 3u) v = clamp(v, -1.0, 1.0);\n"
"    if ((s >> 17) & 1u) v = -(v);\n"
"    return v;\n}\n"
"float _rsx_shcmp(uint f, float reference, float depth) {\n"
"    if (f == 1u) return (float)(reference < depth); if (f == 2u) return (float)(reference == depth);\n"
"    if (f == 3u) return (float)(reference <= depth); if (f == 4u) return (float)(reference > depth);\n"
"    if (f == 5u) return (float)(reference != depth); if (f == 6u) return (float)(reference >= depth);\n"
"    if (f == 7u) return (float)(1.0); return (float)(0.0);\n}\n"
"float _rsx_shtap(uint u, uint f, uint sc, float reference, float2 p, float2 size) {\n"
"    float4 e = C[sc + u];\n"
"    float2 t = p - 0.5;\n"
"    float2 c = clamp(t, 0.0, floor(e.xy * size + 0.5) - 1.0);\n"
"    uint m = (uint)e.z;\n"
"    if (((m & 4u) != 0u && c.x != t.x) || ((m & 8u) != 0u && c.y != t.y))\n"
"        return _rsx_shcmp(f, reference, e.w);\n"
"    float2 q = float2((m & 1u) != 0u ? c.x : t.x, (m & 2u) != 0u ? c.y : t.y);\n"
"    return _rsx_shcmp(f, reference, UT2[u].SampleLevel(US[u], (q + 0.5) / size, 0.0).r);\n}\n"
"float4 _rsx_shadow(uint u, uint uf, uint sc, float3 coordinate) {\n"
"    uint f = (uf >> 3) & 7u;\n"
"    float reference = saturate(coordinate.z);\n"
"    float2 size = (float2)0;\n"
"    if ((uf >> 6) & 3u) { uint width, height; UT2[u].GetDimensions(width, height); size = float2(width, height); }\n"
"    uint mode = (uf >> 16) & 3u; float2 uv;\n"
"    if (mode == 1u) uv = coordinate.xy * C[sc + u].xy;\n"
"    else if (mode == 2u) uv = coordinate.xy / size;\n"
"    else if (mode == 3u) uv = coordinate.xy * C[sc + u].xy / size;\n"
"    else uv = coordinate.xy;\n"
"    if (!((uf >> 6) & 1u)) {\n"
"        float depth = UT2[u].SampleLevel(US[u], uv, 0.0).r;\n"
"        return (float4)_rsx_shcmp(f, reference, depth);\n"
"    }\n"
"    float2 position = uv * size - 0.5;\n"
"    float2 base = floor(position); float2 fraction = position - base;\n"
"    float c0, c1, c2, c3;\n"
"    if ((uf >> 18) & 1u) {\n"
"        c0 = _rsx_shtap(u, f, sc, reference, base + float2(0.5, 0.5), size);\n"
"        c1 = _rsx_shtap(u, f, sc, reference, base + float2(1.5, 0.5), size);\n"
"        c2 = _rsx_shtap(u, f, sc, reference, base + float2(0.5, 1.5), size);\n"
"        c3 = _rsx_shtap(u, f, sc, reference, base + float2(1.5, 1.5), size);\n"
"    } else {\n"
"        float d0 = UT2[u].SampleLevel(US[u], (base + float2(0.5, 0.5)) / size, 0.0).r;\n"
"        c0 = _rsx_shcmp(f, reference, d0);\n"
"        float d1 = UT2[u].SampleLevel(US[u], (base + float2(1.5, 0.5)) / size, 0.0).r;\n"
"        c1 = _rsx_shcmp(f, reference, d1);\n"
"        float d2 = UT2[u].SampleLevel(US[u], (base + float2(0.5, 1.5)) / size, 0.0).r;\n"
"        c2 = _rsx_shcmp(f, reference, d2);\n"
"        float d3 = UT2[u].SampleLevel(US[u], (base + float2(1.5, 1.5)) / size, 0.0).r;\n"
"        c3 = _rsx_shcmp(f, reference, d3);\n"
"    }\n"
"    return (float4)lerp(lerp(c0, c1, fraction.x), lerp(c2, c3, fraction.x), fraction.y);\n}\n"
/* a texture fetch (TEX, TXP, TXB, TXL) */
"float4 _rsx_fetch(uint op, uint u, bool exp_tex, float4 a, float4 b, uint sc) {\n"
"    uint4 U = P[7u + u]; uint uf = U.x; uint ty = uf & 3u; float4 rhs;\n"
"    if ((uf >> 2) & 1u) rhs = _rsx_shadow(u, uf, sc, op == 44u ? (a).xyz / (a).w : (a).xyz);\n"
"    else {\n"
"        if (ty == 1u) {\n"
"            if (op == 45u) rhs = UTC[u].SampleBias(US[u], (a).xyz, (b).x);\n"
"            else if (op == 46u) rhs = UTC[u].SampleLevel(US[u], (a).xyz, (b).x);\n"
"            else rhs = UTC[u].Sample(US[u], (a).xyz);\n"
"        } else if (ty == 2u) {\n"
"            if (op == 45u) rhs = UT3[u].SampleBias(US[u], (a).xyz, (b).x);\n"
"            else if (op == 46u) rhs = UT3[u].SampleLevel(US[u], (a).xyz, (b).x);\n"
"            else if (op == 44u) rhs = UT3[u].Sample(US[u], (a).xyz / (a).w);\n"
"            else rhs = UT3[u].Sample(US[u], (a).xyz);\n"
"        } else {\n"
"            float2 c = op == 44u ? (a).xy / (a).w : (a).xy;\n"
"            if ((uf >> 15) & 1u) c = c * C[sc + u].xy;\n"
"            if (((uf >> 14) & 1u) && op <= 44u) c = c * asfloat(U.yz);\n"
"            if (op == 45u) rhs = UT2[u].SampleBias(US[u], c, (b).x);\n"
"            else if (op == 46u) rhs = UT2[u].SampleLevel(US[u], c, (b).x);\n"
"            else rhs = UT2[u].Sample(US[u], c);\n"
"        }\n"
"        if (!((uf >> 9) & 1u) && (exp_tex || ((uf >> 8) & 1u))) rhs = (rhs * 2.0 - 1.0);\n"
"    }\n"
"    uint g = (uf >> 10) & 15u;\n"
"    if (g && !exp_tex) rhs = _rsx_gamma_input(rhs, g);\n"
"    return rhs;\n}\n"
"PSOutput main(PSInput input, bool _rsx_face : SV_IsFrontFace) {\n"
"    uint4 H0 = P[0]; uint4 H1 = P[1];\n"
"    uint flags = H0.y;\n"
"    uint vs = H1.x;            /* fp_alpha is C[vs], fp_shadow_scale[u] is C[vs + 1 + u] */\n"
"    uint sc = vs + 1u;\n"
"    float4 wpos = input.position;\n"
"    if (flags & 128u) {\n"
"        float hgt = asfloat(H1.y);\n"
"        if (flags & 1024u) {\n"
"            float k = C[vs].y;\n"
"            float x = (flags & 512u) ? (input.position.x - 0.5) * k : input.position.x * k;\n"
"            float y = (flags & 256u) ? ((flags & 512u) ? hgt - (input.position.y + 0.5) * k : hgt - input.position.y * k)\n"
"                                     : ((flags & 512u) ? (input.position.y - 0.5) * k : input.position.y * k);\n"
"            wpos = float4(x, y, input.position.zw);\n"
"        } else {\n"
"            float x = input.position.x - ((flags & 512u) ? 0.5 : 0.0);\n"
"            float y = (flags & 256u) ? ((flags & 512u) ? hgt - input.position.y - 0.5 : hgt - input.position.y)\n"
"                                     : ((flags & 512u) ? input.position.y - 0.5 : input.position.y);\n"
"            wpos = float4(x, y, input.position.zw);\n"
"        }\n"
"    }\n"
"    IN[0] = wpos; IN[1] = saturate(input.col0); IN[2] = saturate(input.col1); IN[3] = input.fog;\n"
"    IN[4] = input.tc0; IN[5] = input.tc1; IN[6] = input.tc2; IN[7] = input.tc3;\n"
"    IN[8] = input.tc4; IN[9] = input.tc5; IN[10] = input.tc6; IN[11] = input.tc7;\n"
"    IN[12] = input.tc8; IN[13] = input.tc9; IN[14] = (float4)(_rsx_face ? 1.0 : -1.0); IN[15] = (float4)0;\n"
"    [unroll] for (uint _i = 0; _i < 48u; _i++) R[_i] = (float4)0;\n"
"    CC[0] = (float4)0; CC[1] = (float4)0;\n"
"    uint lc[4]; lc[0] = 0; lc[1] = 0; lc[2] = 0; lc[3] = 0; int ld = -1;\n"
"    uint n = H0.x; uint pc = 0;\n"
"    [loop] while (pc < n) {\n"
"        uint4 q0 = P[32u + 2u * pc]; uint4 q1 = P[33u + 2u * pc];\n"
"        pc++;\n"
"        uint w = q0.x; uint op = w & 255u; uint x = q0.y;\n"
"        uint ex = x & 3u;\n"
"        bool4 cnd = bool4(true, true, true, true);\n"
"        if (ex == 2u) cnd = ucmp(uswz(CC[(w >> 28) & 1u], (x >> 5) & 255u), (x >> 2) & 7u);\n"
"        else if (ex == 1u) cnd = bool4(false, false, false, false);\n"
"        if (op >= 60u) {\n"
"            bool any_c = any(cnd);\n"
"            if (op == 60u) { if (!any_c) pc = q1.y; }\n"
"            else if (op == 61u) { pc = q1.y; }\n"
"            else if (op == 62u) { if (!any_c || q1.z == 0u) pc = q1.y; else { ld++; lc[ld] = q1.z; } }\n"
"            else if (op == 63u) { lc[ld] = lc[ld] - 1u; if (lc[ld] > 0u) pc = q1.y; else ld--; }\n"
"            else if (op == 64u) { if (any_c) { pc = q1.y; ld--; } }\n"
"            continue;\n"
"        }\n"
"        if (op == 47u) { if (any(cnd)) discard; continue; }\n"
"        uint insrc = (x >> 13) & 15u; uint slot = x >> 21; bool persp = ((w >> 30) & 1u) != 0u;\n"
"        float4 a = usrc(q0.z, insrc, persp, slot, input.position);\n"
"        float4 b = usrc(q0.w, insrc, persp, slot, input.position);\n"
"        float4 c = usrc(q1.x, insrc, persp, slot, input.position);\n"
"        float4 _v;\n"
"        switch (op) {\n"
"        case 1u: _v = (float4)(a); break;\n"
"        case 2u: _v = (float4)((a) * (b)); break;\n"
"        case 3u: _v = (float4)((a) + (b)); break;\n"
"        case 4u: _v = (float4)((a) * (b) + (c)); break;\n"
"        case 5u: _v = (float4)(dot((a).xyz, (b).xyz)); break;\n"
"        case 6u: _v = (float4)(_rsx_dp3p((a).xyz, (b).xyz)); break;\n"
"        case 7u: _v = (float4)(dot((a), (b))); break;\n"
"        case 8u: _v = (float4)(min((a), (b))); break;\n"
"        case 9u: _v = (float4)(max((a), (b))); break;\n"
"        case 10u: _v = (float4)(frac(a)); break;\n"
"        case 11u: _v = (float4)(floor(a)); break;\n"
"        case 12u: _v = (float4)(ddx(a)); break;\n"
"        case 13u: _v = (float4)(ddy(a)); break;\n"
"        case 14u: _v = (float4)((1.0 / (a).x)); break;\n"
"        case 15u: _v = (float4)(rsqrt(abs((a).x))); break;\n"
"        case 16u: _v = (float4)(exp2((a).x)); break;\n"
"        case 17u: _v = (float4)(log2((a).x)); break;\n"
"        case 18u: _v = (float4)(cos((a).x)); break;\n"
"        case 19u: _v = (float4)(sin((a).x)); break;\n"
"        case 20u: _v = (float4)(pow((a).x, (b).x)); break;\n"
"        case 21u: _v = (float4)((a) / (b).x); break;\n"
"        case 22u: _v = (float4)((abs(a) > 0.0 ? (a) / sqrt(abs((b).x)) : (float4)0.0)); break;\n"
"        case 23u: _v = (float4)(dot((a).xy, (b).xy)); break;\n"
"        case 24u: _v = (float4)(float4(_rsx_nrm((a).xyz), 1.0)); break;\n"
"        case 25u: _v = (float4)(lerp((c), (b), (a))); break;\n"
"        case 26u: _v = (float4)(float4(1.0, (a).y, (a).y > 0.0 ? exp2((a).w) : 0.0, 1.0)); break;\n"
"        case 27u: _v = (float4)((float4)((a) <  (b))); break;\n"
"        case 28u: _v = (float4)((float4)((a) >= (b))); break;\n"
"        case 29u: _v = (float4)((float4)((a) <= (b))); break;\n"
"        case 30u: _v = (float4)((float4)((a) >  (b))); break;\n"
"        case 31u: _v = (float4)((float4)((a) != (b))); break;\n"
"        case 32u: _v = (float4)((float4)((a) == (b))); break;\n"
"        case 33u: _v = (float4)(asfloat(_rsx_pk4ub(a))); break;\n"
"        case 34u: _v = (float4)(_rsx_up4ub((a).x)); break;\n"
"        case 35u: _v = (float4)(asfloat(_rsx_pk4ub(_rsx_lin2srgb(a)))); break;\n"
"        case 36u: _v = (float4)(_rsx_srgb2lin(_rsx_up4ub((a).x))); break;\n"
"        case 37u: _v = (float4)(asfloat(_rsx_pk4b(a))); break;\n"
"        case 38u: _v = (float4)(_rsx_up4b((a).x)); break;\n"
"        case 39u: _v = (float4)(asfloat(_rsx_pk2us(a))); break;\n"
"        case 40u: _v = (float4)(_rsx_up2us((a).x)); break;\n"
"        case 41u: _v = (float4)(asfloat(_rsx_pk2h(a))); break;\n"
"        case 42u: _v = (float4)(_rsx_up2h((a).x)); break;\n"
"        default: _v = (float4)(_rsx_fetch(op, (x >> 17) & 15u, ((w >> 29) & 1u) != 0u, a, b, sc)); break;\n"
"        }\n"
"        uint scl = (w >> 23) & 7u;\n"
"        if (scl == 1u) _v = _v * 2.0; else if (scl == 2u) _v = _v * 4.0; else if (scl == 3u) _v = _v * 8.0;\n"
"        else if (scl == 5u) _v = _v / 2.0; else if (scl == 6u) _v = _v / 4.0; else if (scl == 7u) _v = _v / 8.0;\n"
"        if ((w >> 20) & 1u) _v = saturate(_v);\n"
"        else { uint dc = (w >> 21) & 3u;\n"
"            if (dc == 1u) _v = clamp(_v, -65504.0, 65504.0); else if (dc == 2u) _v = clamp(_v, -2.0, 2.0); else if (dc == 3u) _v = clamp(_v, -1.0, 1.0); }\n"
"        uint dm = (w >> 15) & 15u;\n"
"        bool4 m = bool4(dm & 1u, dm & 2u, dm & 4u, dm & 8u);\n"
"        uint d = ((w >> 14) & 1u) ? 16u + ((w >> 8) & 31u) : ((w >> 8) & 15u);\n"
"        if ((w >> 19) & 1u) {\n"
"            float4 old = R[d];\n"
"            R[d] = (m && cnd) ? _v : old;\n"
"        }\n"
"        if ((w >> 26) & 1u) {\n"
"            uint cw = (w >> 27) & 1u;\n"
"            float4 src = ((w >> 19) & 1u) ? R[d] : _v;\n"
"            bool4 cm = ((w >> 19) & 1u) ? m : (m && cnd);\n"
"            float4 oldc = CC[cw];\n"
"            CC[cw] = cm ? src : oldc;\n"
"        }\n"
"    }\n"
"    float4 o[4]; o[0] = (float4)0; o[1] = (float4)0; o[2] = (float4)0; o[3] = (float4)0;\n"
"    uint nt = H0.z;\n"
"    bool hx = (flags & 1u) != 0u;\n"
"    [unroll] for (uint t = 0; t < 4u; t++) {\n"
"        if (t < nt) {\n"
"            uint reg = hx ? (t == 0u ? 16u : t == 1u ? 20u : t == 2u ? 22u : 24u) : (t == 0u ? 0u : t + 1u);\n"
"            float4 v = R[reg];\n"
"            uint4 G = P[3u + t];\n"
"            if (flags & 2048u) {\n"
"                [unroll] for (uint l = 0; l < 4u; l++) {\n"
"                    uint gl = G[l];\n"
"                    if (gl & 32768u) v[l] = f16tof32((asuint(R[gl & 127u][(gl >> 8) & 3u]) >> (((gl >> 10) & 1u) * 16u)) & 0xFFFFu);\n"
"                }\n"
"            }\n"
"            o[t] = (v == v) ? v : (float4)0;\n"
"            if (flags & 4u) o[t] = _rsx_srgb_output(o[t]);\n"
"        }\n"
"    }\n"
"    if (flags & 16u) {\n"
"        float3 s3 = (flags & 64u) ? C[vs].zzz : asfloat(P[2].xyz);\n"
"        o[0].rgb = ((flags & 32u) ? saturate(o[0].rgb) : (o[0].rgb)) * (s3);\n"
"    }\n"
"    if (flags & 8u) {\n"
"        float _rsx_alpha_ref = C[vs].x; uint ac = H0.w; bool pass;\n"
"        if (ac == 1u) pass = o[0].a < _rsx_alpha_ref; else if (ac == 2u) pass = o[0].a == _rsx_alpha_ref;\n"
"        else if (ac == 3u) pass = o[0].a <= _rsx_alpha_ref; else if (ac == 4u) pass = o[0].a > _rsx_alpha_ref;\n"
"        else if (ac == 5u) pass = o[0].a != _rsx_alpha_ref; else if (ac == 6u) pass = o[0].a >= _rsx_alpha_ref;\n"
"        else pass = false;\n"
"        if (!(pass)) discard;\n"
"    }\n"
"    PSOutput _out;\n"
"    _out.t0 = o[0]; _out.t1 = o[1]; _out.t2 = o[2]; _out.t3 = o[3];\n"
"#ifdef UBER_DEPTH\n"
"    float _d = R[1].z;\n"
"    _out.depth = (_d == _d) ? saturate(_d) : 0.0;\n"
"#endif\n"
"    return _out;\n}\n";

const char* rsx_fp_uber_hlsl(int depth)
{
    static char* with_depth;
    if (!depth) return s_uber_body;
    if (!with_depth) {
        static const char def[] = "#define UBER_DEPTH 1\n";
        char* t = (char*)malloc(sizeof def + sizeof s_uber_body);
        if (!t) return NULL;
        memcpy(t, def, sizeof def - 1);
        memcpy(t + sizeof def - 1, s_uber_body, sizeof s_uber_body);
        with_depth = t;
    }
    return with_depth;
}

/* UBER3 FAST (2026-11-12): the fast-path ubershader.
 *
 * Measured (host\uber3\NOTES.md section 2) on NVIDIA: the full ubershader's GPU time is not its arithmetic. It is
 * (1) the STATE the shader keeps alive per pixel -- every value alive across the interpreter loop slows every
 * iteration (the 48-register file + the 16-entry input array alone are ~83 % of the time of a short program), an
 * array the loop writes costs ~2 us per Mpx per operation per scalar lane whatever its size -- and (2) the number of
 * tests an iteration executes (~0.2 us per Mpx each).
 * So: rsx_fp_uber_pack renames the registers of an encoded program so that only as many are kept as are alive at
 * the same time (a liveness pass per component + a linear scan), loads each input once before the loop, and marks
 * per operation what it does NOT need. The fast shader (rsx_fp_uber_fast_hlsl) is the same interpreter, the same
 * expression per operation (so the same bits), with a small register file of real variables (no array, not
 * cleared), built in a few sizes x 2 kinds (programs of plain operations only; programs that also need IF/ELSE,
 * KIL, conditional execution or condition codes); the program runs on the smallest that fits. A program the pass
 * cannot take (loops, too many registers alive at once, depth export) stays on the full ubershader: coverage unchanged. */
static const u32 s_fast_nreg[RSX_FP_UBER_FAST_SIZES] = { RSX_FP_UBER_FAST_NREGS };

/* class = register-file size index + RSX_FP_UBER_FAST_SIZES * (1: the shader with the general path) */
u32 rsx_fp_uber_fast_nreg(int cls)
{
    return cls >= 0 && cls < RSX_FP_UBER_FAST_CLASSES ? s_fast_nreg[cls % RSX_FP_UBER_FAST_SIZES] : 0u;
}
int rsx_fp_uber_fast_class(u32 nreg, int general)
{
    for (int c = 0; c < RSX_FP_UBER_FAST_SIZES; c++)
        if (nreg <= s_fast_nreg[c]) return c + (general ? RSX_FP_UBER_FAST_SIZES : 0);
    return -1;
}

#define U_LOADIN 48u   /* the pack pass's own operation: register = input (perspective-corrected when bit 30 is set) */
static u32 fast_nsrc(u32 uop)
{
    return (uop == U_MAD || uop == U_LRP) ? 3u :
        (uop == U_MUL || uop == U_ADD || uop == U_DP3 || uop == U_DP3P || uop == U_DP4 || uop == U_MIN ||
         uop == U_MAX || uop == U_POW || uop == U_DIV || uop == U_DIVSQ || uop == U_DP2 ||
         (uop >= U_SLT && uop <= U_SEQ) || uop == U_TXB || uop == U_TXL) ? 2u : 1u;
}
/* the result lanes of source si the operation reads (a superset is always safe) */
static u32 fast_lanes(u32 uop, u32 si, u32 dm, int plain)
{
    if (!plain) return 15u;
    switch (uop) {
    case U_MOV: case U_MUL: case U_ADD: case U_MAD: case U_MIN: case U_MAX: case U_FRC: case U_FLR:
    case U_SLT: case U_SGE: case U_SLE: case U_SGT: case U_SNE: case U_SEQ: case U_LRP:
        return dm;
    case U_DP3: case U_DP3P: case U_NRM: return 7u;
    case U_DP2: return 3u;
    case U_RCP: case U_RSQ: case U_EX2: case U_LG2: case U_COS: case U_SIN: case U_POW:
    case U_UP4UB: case U_UPG: case U_UP4B: case U_UP2US: case U_UP2H:
        return 1u;
    case U_DIV: case U_DIVSQ: return si ? 1u : dm;
    case U_TXB: case U_TXL: return si ? 1u : 15u;
    default: return 15u;   /* DP4, DDX/DDY (kept whole), LIF, the packs, TEX/TXP */
    }
}

/* In place: t = a table rsx_fp_uber_encode made (room for cap_vec4 slots). Returns the table's new size in uint4
 * slots, the registers needed (*nreg) and whether the program needs the general path (*general); or -1 with the
 * table untouched (the program stays on the full ubershader). opts: none yet (0).
 * What changes: registers renamed (source index / destination fields); an input source's index field is 32 + the
 * input (48 + n: texcoord n, perspective-corrected); a constant source's is 62 (63: no constant). (Tried and
 * dropped: inputs loaded into registers before the loop -- the game's programs read 9-13 inputs, so they then need
 * 17-18 registers instead of 11; the code for it below is unused: n_in is always 0.)
 * Marks per operation, read by the fast shader: source bit 23 "not plain" (a swizzle or a modifier), bit 24 a
 * modifier, bit 25 a swizzle (after the lanes the operation does not read took the identity selector), bit 26 that
 * swizzle is one component for every lane read; q1.w bits 0-1 the sources read, bit 2 result modifiers, bit 3 a full unconditional write, bit 4
 * "general path" (IF/ELSE, KIL, conditional execution, a condition-code write, no destination).
 * Header slot 23: x registers to clear at the start (read before written), y the export registers (a byte per
 * target), z the depth register, w registers | LOADINs << 8. */
int rsx_fp_uber_pack(u32 (*t)[4], u32 cap_vec4, u32 opts, u32* nreg_out, int* general_out)
{
    enum { NV = 80, MAXBLK = 256, MAXNEST = 32 };   /* r0-r15, h0-h31, then the inputs: 48 + input + 16 * (perspective) */
    typedef struct { u8 rv[3], rc[3], nr, wv, wm, kill; } uinfo;
    typedef struct { u32 a, els, end; } blk;        /* IF at a; ELSE at els (0: none); the block ends before end */
    const u32 n = t[0][0], flags = t[0][1], targets = t[0][2];
    const int in_direct = 1;   /* inputs stay sources (index 32 + input, 48 + texcoord: perspective-corrected) */
    (void)opts;
    if (n > RSX_FP_UBER_MAX_UOPS || targets > 4u || targets == 0u) return -1;
#define FUOP(i) (&t[RSX_FP_UBER_HEADER_VEC4 + 2u * (i)][0])
    uinfo* ui = (uinfo*)calloc((size_t)n + 1u, sizeof *ui);
    u8* live = (u8*)calloc(((size_t)n + 1u) * NV, 1);      /* live[i][v]: the lanes of v needed when operation i starts */
    u8* phys = (u8*)malloc(((size_t)n + 1u) * NV);         /* phys[i][v]: its register while operation i runs */
    blk* bl = (blk*)malloc(MAXBLK * sizeof *bl);
    u8 (*stk)[2][NV] = (u8 (*)[2][NV])malloc(MAXNEST * sizeof *stk);
    int ret = -1;
    u32 nb = 0;
    if (!ui || !live || !phys || !bl || !stk) goto done;
#define LIVE(i, v) live[(size_t)(i) * NV + (v)]
#define PHYS(i, v) phys[(size_t)(i) * NV + (v)]
    /* 1. what each operation reads and writes; the IF blocks */
    for (u32 i = 0; i < n; i++) {
        const u32* e = FUOP(i);
        const u32 w = e[0], op = w & 255u;
        uinfo* u = &ui[i];
        u->wv = 0xFF;
        if (op == U_LOOP || op == U_ENDLOOP || op == U_BRK) goto done;
        if (op == U_IF) {
            if (nb == MAXBLK || e[5] <= i || e[5] > e[6] || e[6] > n) goto done;
            bl[nb].a = i; bl[nb].end = e[6]; bl[nb].els = 0;
            if (e[5] != e[6]) {
                if ((FUOP(e[5] - 1u)[0] & 255u) != U_ELSE || e[5] - 1u <= i) goto done;
                bl[nb].els = e[5] - 1u;
            }
            nb++;
            continue;
        }
        if (op == U_ELSE || op == U_KIL || op == U_NOP) continue;
        if (op > U_TXL) goto done;
        const u32 ns = fast_nsrc(op), dm = (w >> 15) & 15u;
        const int has_dest = (w >> 19) & 1u, uncond = (e[1] & 3u) == 0u, set_cond = (w >> 26) & 1u;
        const int plain = has_dest && uncond && !set_cond;
        for (u32 si = 0; si < ns; si++) {
            const u32 s = e[2 + si];
            u32 v;
            if ((s & 3u) == 1u) {
                if (in_direct) continue;
                const u32 insrc = (e[1] >> 13) & 15u;
                v = 48u + insrc + (((w >> 30) & 1u) && insrc >= 4u && insrc <= 13u ? 16u : 0u);
            } else if ((s & 3u) == 0u) {
                const u32 idx = (s >> 2) & 63u;
                if (((s >> 8) & 1u) ? idx >= 32u : idx >= 16u) goto done;
                v = ((s >> 8) & 1u) ? 16u + idx : idx;
            } else continue;
            const u32 z = (s >> 9) & 255u, ln = fast_lanes(op, si, dm, plain);
            u32 c = 0;
            for (u32 l = 0; l < 4; l++) if ((ln >> l) & 1u) c |= 1u << ((z >> (2u * l)) & 3u);
            u->rv[u->nr] = (u8)v; u->rc[u->nr] = (u8)c; u->nr++;
        }
        if (has_dest) {
            const u32 idx = (w >> 8) & 63u & (((w >> 14) & 1u) ? 31u : 15u);
            u->wv = (u8)(((w >> 14) & 1u) ? 16u + idx : idx);
            u->wm = (u8)dm; u->kill = (u8)uncond;
        }
    }
    /* 2. liveness, backwards; an IF joins its two ways */
    static const u8 xr[2][4] = { { 0, 2, 3, 4 }, { 16, 20, 22, 24 } };
    const int hx = (flags & 1u) != 0u;
    {
        u8 cur[NV];
        memset(cur, 0, sizeof cur);
        for (u32 tg = 0; tg < targets; tg++) {
            cur[xr[hx][tg]] = 15u;
            if (flags & 2048u)
                for (u32 l = 0; l < 4; l++) {
                    const u32 gl = t[3 + tg][l];
                    if (!(gl & 32768u)) continue;
                    if ((gl & 127u) >= 16u) goto done;
                    cur[gl & 127u] |= (u8)(1u << ((gl >> 8) & 3u));
                }
        }
        if (flags & 2u) cur[1] |= 4u;
        memcpy(&LIVE(n, 0), cur, NV);
        u32 sp = 0; u32 sblk[MAXNEST];
        for (u32 i = n; i-- > 0; ) {
            for (u32 k = 0; k < nb; k++)   /* blocks that end here, the outer one first */
                if (bl[k].end == i + 1u) {
                    if (sp == MAXNEST) goto done;
                    memcpy(stk[sp][0], cur, NV); sblk[sp] = k; sp++;
                }
            const u32 op = FUOP(i)[0] & 255u;
            if (op == U_ELSE) {
                if (!sp || bl[sblk[sp - 1]].els != i) goto done;
                memcpy(stk[sp - 1][1], cur, NV);          /* needed entering the else part */
                memcpy(cur, stk[sp - 1][0], NV);          /* the then part runs into what follows the block */
            } else if (op == U_IF) {
                if (!sp || bl[sblk[sp - 1]].a != i) goto done;
                sp--;
                const u8* other = bl[sblk[sp]].els ? stk[sp][1] : stk[sp][0];
                for (u32 v = 0; v < NV; v++) cur[v] |= other[v];
            } else {
                const uinfo* u = &ui[i];
                if (u->wv != 0xFF && u->kill) cur[u->wv] &= (u8)~u->wm;
                for (u32 r = 0; r < u->nr; r++) cur[u->rv[r]] |= u->rc[r];
            }
            memcpy(&LIVE(i, 0), cur, NV);
        }
        if (sp) goto done;
    }
    /* 3. where each register must have a home: where it is needed or written; across a whole IF block when it is
     * needed entering or after it (both ways must leave it in the same place) */
    u32 n_in = 0, clear_v[48], n_clear = 0;
    for (u32 v = 0; v < NV; v++) {
        if (!LIVE(0, v)) continue;
        if (v >= 48u) n_in++; else clear_v[n_clear++] = v;   /* read before written: starts as zero */
    }
    for (u32 i = 0; i <= n; i++)
        for (u32 v = 0; v < NV; v++) PHYS(i, v) = LIVE(i, v) ? 1 : 0;
    for (u32 i = 0; i < n; i++) if (ui[i].wv != 0xFF) PHYS(i, ui[i].wv) = 1;
    for (u32 k = 0; k < nb; k++)
        for (u32 v = 0; v < NV; v++)
            if (LIVE(bl[k].a, v) || LIVE(bl[k].end, v))
                for (u32 i = bl[k].a; i <= bl[k].end; i++) PHYS(i, v) = 1;
    /* 4. linear scan over the positions: a register is taken where its home starts, given back after it ends */
    u32 nreg = 0, clear_mask = 0;
    {
        u8 at[NV], busy[32];
        memset(at, 0xFF, sizeof at); memset(busy, 0, sizeof busy);
        for (u32 i = 0; i <= n; i++) {
            for (u32 v = 0; v < NV; v++) {
                if (!PHYS(i, v)) { PHYS(i, v) = 0xFF; continue; }
                if (at[v] == 0xFF) {
                    u32 p = 0;
                    while (p < 32u && busy[p]) p++;
                    if (p == 32u) goto done;
                    busy[p] = 1; at[v] = (u8)p;
                    if (p + 1u > nreg) nreg = p + 1u;
                }
                PHYS(i, v) = at[v];
            }
            if (i < n)
                for (u32 v = 0; v < NV; v++)
                    if (at[v] != 0xFF && !PHYS(i + 1u, v)) { busy[at[v]] = 0; at[v] = 0xFF; }
        }
    }
    if (!nreg) nreg = 1;
    if (rsx_fp_uber_fast_class(nreg, 0) < 0) goto done;
    if (RSX_FP_UBER_HEADER_VEC4 + 2u * (n + n_in) > cap_vec4 || n + n_in > RSX_FP_UBER_MAX_UOPS) goto done;
    for (u32 k = 0; k < n_clear; k++) clear_mask |= 1u << PHYS(0, clear_v[k]);
    /* 5. rewrite (nothing can fail from here) */
    int general = 0;
    for (u32 i = 0; i < n; i++) {
        u32* e = FUOP(i);
        const u32 w = e[0], op = w & 255u;
        if (op >= U_KIL || op == U_NOP) { e[7] = 16u; general = 1; continue; }
        const u32 ns = fast_nsrc(op);
        const int plain_op = ((w >> 19) & 1u) && (e[1] & 3u) == 0u && !((w >> 26) & 1u);
        for (u32 si = 0; si < 3; si++) {
            u32 s = e[2 + si];
            if (si >= ns) { e[2 + si] = 0; continue; }
            {   /* the lanes the operation does not read take the identity selector: more sources need no swizzle */
                const u32 ln = fast_lanes(op, si, (w >> 15) & 15u, plain_op);
                u32 z = (s >> 9) & 255u;
                u32 same = 4u;   /* 0..3: every lane read takes that one component (a broadcast); 5: not */
                for (u32 l = 0; l < 4; l++) {
                    if (!((ln >> l) & 1u)) continue;
                    const u32 c = (z >> (2u * l)) & 3u;
                    same = same == 4u ? c : same == c ? same : 5u;
                }
                for (u32 l = 0; l < 4; l++) if (!((ln >> l) & 1u)) z = (z & ~(3u << (2u * l))) | (l << (2u * l));
                s &= ~(1u << 26);
                if (z != 0xE4u && same < 4u) { z = same * 0x55u; s |= 1u << 26; }   /* bit 26: a broadcast of component z & 3 */
                s = (s & ~(255u << 9)) | (z << 9);
            }
            if ((s & 3u) == 1u && in_direct) {
                const u32 insrc = (e[1] >> 13) & 15u;
                const u32 code = insrc == 15u ? 63u : (((w >> 30) & 1u) && insrc >= 4u && insrc <= 13u) ? 44u + insrc : 32u + insrc;
                s = (s & ~0x1FCu) | (code << 2);
                s &= 0x7FFFFFu | (1u << 26);
                if (((s >> 19) & 7u) || ((s >> 17) & 3u)) s |= 3u << 23;
                if (((s >> 9) & 255u) != 0xE4u) s |= (1u << 23) | (1u << 25);
                e[2 + si] = s;
                continue;
            }
            if ((s & 3u) == 1u) {
                const u32 insrc = (e[1] >> 13) & 15u;
                const u32 v = 48u + insrc + (((w >> 30) & 1u) && insrc >= 4u && insrc <= 13u ? 16u : 0u);
                s = (s & ~0x1FFu) | ((u32)PHYS(i, v) << 2);   /* now a register */
            } else if ((s & 3u) == 0u) {
                const u32 idx = (s >> 2) & 63u;
                const u32 v = ((s >> 8) & 1u) ? 16u + idx : idx;
                s = (s & ~0x1FCu) | ((u32)PHYS(i, v) << 2);
            }
            s &= 0x7FFFFFu | (1u << 26);
            if ((s & 3u) != 0u) s = (s & ~0xFCu) | ((((s >> 22) & 1u) ? 62u : 63u) << 2);   /* a constant (or none) */
            if (((s >> 19) & 7u) || ((s >> 17) & 3u)) s |= 3u << 23;
            if (((s >> 9) & 255u) != 0xE4u) s |= (1u << 23) | (1u << 25);
            e[2 + si] = s;
        }
        u32 x = ns;
        if (((w >> 23) & 7u) || ((w >> 20) & 1u) || ((w >> 21) & 3u)) x |= 4u;
        u32 nw = w & ~0x7F00u;
        if ((w >> 19) & 1u) {
            nw |= (u32)PHYS(i, ui[i].wv) << 8;
            if (((w >> 15) & 15u) == 15u && (e[1] & 3u) == 0u) x |= 8u;
        }
        if (!((w >> 19) & 1u) || (e[1] & 3u) != 0u || ((w >> 26) & 1u)) { x |= 16u; general = 1; }
        e[0] = nw;
        e[7] = x;
    }
    {
        u32 xb = 0;
        int any_gather = 0;
        for (u32 tg = 0; tg < targets; tg++) {
            xb |= (u32)PHYS(n, xr[hx][tg]) << (8u * tg);
            if (flags & 2048u)
                for (u32 l = 0; l < 4; l++) {
                    const u32 gl = t[3 + tg][l];
                    if (!(gl & 32768u)) continue;
                    t[3 + tg][l] = (gl & ~127u) | PHYS(n, gl & 127u);
                    any_gather = 1;
                }
        }
        if (!any_gather) t[0][1] &= ~2048u;
        t[23][0] = clear_mask; t[23][1] = xb; t[23][2] = (flags & 2u) ? PHYS(n, 1) : 0u;
    }
    if (n_in) {
        /* the LOADINs are the table's first operations (the shader runs them in its own loop, before the program,
         * so no interpolant stays alive in the main loop); jump targets move by their count */
        memmove(FUOP(n_in), FUOP(0), (size_t)n * 32u);
        u32 o = 0;
        for (u32 v = 48; v < NV; v++) {
            if (!LIVE(0, v)) continue;
            u32* e = FUOP(o); o++;
            memset(e, 0, 32);
            const u32 key = v - 48u;
            e[0] = U_LOADIN | ((u32)PHYS(0, v) << 8) | (15u << 15) | (1u << 19) | ((key & 16u) ? (1u << 30) : 0u);
            e[1] = (key & 15u) << 13;
            e[7] = 8u | 16u;
        }
        for (u32 i = n_in; i < n + n_in; i++) {
            u32* e = FUOP(i);
            const u32 op = e[0] & 255u;
            if (op == U_IF || op == U_ELSE) { e[5] += n_in; if (op == U_IF) e[6] += n_in; }
        }
        t[0][0] = n + n_in;
    }
    t[23][3] = nreg | (n_in << 8);
    if (nreg_out) *nreg_out = nreg;
    if (general_out) *general_out = general;
    ret = (int)(RSX_FP_UBER_HEADER_VEC4 + 2u * t[0][0]);
done:
    free(ui); free(live); free(phys); free(bl); free(stk);
    return ret;
#undef FUOP
#undef LIVE
#undef PHYS
}

static const char s_uber_fast_body[] =
"#define F_ONESW 1   /* a source is ONE switch (registers, inputs, the constant): measured ~2 % faster than a split fetch */\n"
"struct PSInput {\n"
"    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
"    float4 fog : FOG;\n"
"    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
"    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
"    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n"
"};\n"
"Texture2D    UT2[16] : register(t0);\n"
"TextureCube  UTC[16] : register(t0, space1);\n"
"Texture3D    UT3[16] : register(t0, space2);\n"
"SamplerState US[16] : register(s0);\n"
"cbuffer PSConstants : register(b1) { float4 C[4096]; };\n"
"cbuffer UberProgram : register(b2) { uint4 P[4096]; };\n"
"struct PSOutput {\n"
"    float4 t0 : SV_TARGET0; float4 t1 : SV_TARGET1; float4 t2 : SV_TARGET2; float4 t3 : SV_TARGET3;\n"
"#ifdef UBER_DEPTH\n"
"    float depth : SV_Depth;\n"
"#endif\n"
"};\n"
"#ifndef NREG\n"
"#define NREG 8u\n"
"#endif\n"
"static float4 g0, g1, g2, g3, g4, g5, g6, g7, g8, g9, g10, g11, g12, g13, g14, g15, g16, g17, g18, g19, g20, g21, g22, g23, g24, g25, g26, g27, g28, g29, g30, g31;   /* the renamed registers (rsx_fp_uber_pack): real variables, no array */\n"
"float4 rget(uint i) {\n"
"    switch (i) {\n"
"#if NREG > 1u\n"
"    case 1u: return g1;\n"
"#endif\n"
"#if NREG > 2u\n"
"    case 2u: return g2;\n"
"#endif\n"
"#if NREG > 3u\n"
"    case 3u: return g3;\n"
"#endif\n"
"#if NREG > 4u\n"
"    case 4u: return g4;\n"
"#endif\n"
"#if NREG > 5u\n"
"    case 5u: return g5;\n"
"#endif\n"
"#if NREG > 6u\n"
"    case 6u: return g6;\n"
"#endif\n"
"#if NREG > 7u\n"
"    case 7u: return g7;\n"
"#endif\n"
"#if NREG > 8u\n"
"    case 8u: return g8;\n"
"#endif\n"
"#if NREG > 9u\n"
"    case 9u: return g9;\n"
"#endif\n"
"#if NREG > 10u\n"
"    case 10u: return g10;\n"
"#endif\n"
"#if NREG > 11u\n"
"    case 11u: return g11;\n"
"#endif\n"
"#if NREG > 12u\n"
"    case 12u: return g12;\n"
"#endif\n"
"#if NREG > 13u\n"
"    case 13u: return g13;\n"
"#endif\n"
"#if NREG > 14u\n"
"    case 14u: return g14;\n"
"#endif\n"
"#if NREG > 15u\n"
"    case 15u: return g15;\n"
"#endif\n"
"#if NREG > 16u\n"
"    case 16u: return g16;\n"
"#endif\n"
"#if NREG > 17u\n"
"    case 17u: return g17;\n"
"#endif\n"
"#if NREG > 18u\n"
"    case 18u: return g18;\n"
"#endif\n"
"#if NREG > 19u\n"
"    case 19u: return g19;\n"
"#endif\n"
"#if NREG > 20u\n"
"    case 20u: return g20;\n"
"#endif\n"
"#if NREG > 21u\n"
"    case 21u: return g21;\n"
"#endif\n"
"#if NREG > 22u\n"
"    case 22u: return g22;\n"
"#endif\n"
"#if NREG > 23u\n"
"    case 23u: return g23;\n"
"#endif\n"
"#if NREG > 24u\n"
"    case 24u: return g24;\n"
"#endif\n"
"#if NREG > 25u\n"
"    case 25u: return g25;\n"
"#endif\n"
"#if NREG > 26u\n"
"    case 26u: return g26;\n"
"#endif\n"
"#if NREG > 27u\n"
"    case 27u: return g27;\n"
"#endif\n"
"#if NREG > 28u\n"
"    case 28u: return g28;\n"
"#endif\n"
"#if NREG > 29u\n"
"    case 29u: return g29;\n"
"#endif\n"
"#if NREG > 30u\n"
"    case 30u: return g30;\n"
"#endif\n"
"#if NREG > 31u\n"
"    case 31u: return g31;\n"
"#endif\n"
"    default: return g0;\n"
"    }\n"
"}\n"
"void rput(uint d, float4 v) {\n"
"    switch (d) {\n"
"#if NREG > 1u\n"
"    case 1u: g1 = v; break;\n"
"#endif\n"
"#if NREG > 2u\n"
"    case 2u: g2 = v; break;\n"
"#endif\n"
"#if NREG > 3u\n"
"    case 3u: g3 = v; break;\n"
"#endif\n"
"#if NREG > 4u\n"
"    case 4u: g4 = v; break;\n"
"#endif\n"
"#if NREG > 5u\n"
"    case 5u: g5 = v; break;\n"
"#endif\n"
"#if NREG > 6u\n"
"    case 6u: g6 = v; break;\n"
"#endif\n"
"#if NREG > 7u\n"
"    case 7u: g7 = v; break;\n"
"#endif\n"
"#if NREG > 8u\n"
"    case 8u: g8 = v; break;\n"
"#endif\n"
"#if NREG > 9u\n"
"    case 9u: g9 = v; break;\n"
"#endif\n"
"#if NREG > 10u\n"
"    case 10u: g10 = v; break;\n"
"#endif\n"
"#if NREG > 11u\n"
"    case 11u: g11 = v; break;\n"
"#endif\n"
"#if NREG > 12u\n"
"    case 12u: g12 = v; break;\n"
"#endif\n"
"#if NREG > 13u\n"
"    case 13u: g13 = v; break;\n"
"#endif\n"
"#if NREG > 14u\n"
"    case 14u: g14 = v; break;\n"
"#endif\n"
"#if NREG > 15u\n"
"    case 15u: g15 = v; break;\n"
"#endif\n"
"#if NREG > 16u\n"
"    case 16u: g16 = v; break;\n"
"#endif\n"
"#if NREG > 17u\n"
"    case 17u: g17 = v; break;\n"
"#endif\n"
"#if NREG > 18u\n"
"    case 18u: g18 = v; break;\n"
"#endif\n"
"#if NREG > 19u\n"
"    case 19u: g19 = v; break;\n"
"#endif\n"
"#if NREG > 20u\n"
"    case 20u: g20 = v; break;\n"
"#endif\n"
"#if NREG > 21u\n"
"    case 21u: g21 = v; break;\n"
"#endif\n"
"#if NREG > 22u\n"
"    case 22u: g22 = v; break;\n"
"#endif\n"
"#if NREG > 23u\n"
"    case 23u: g23 = v; break;\n"
"#endif\n"
"#if NREG > 24u\n"
"    case 24u: g24 = v; break;\n"
"#endif\n"
"#if NREG > 25u\n"
"    case 25u: g25 = v; break;\n"
"#endif\n"
"#if NREG > 26u\n"
"    case 26u: g26 = v; break;\n"
"#endif\n"
"#if NREG > 27u\n"
"    case 27u: g27 = v; break;\n"
"#endif\n"
"#if NREG > 28u\n"
"    case 28u: g28 = v; break;\n"
"#endif\n"
"#if NREG > 29u\n"
"    case 29u: g29 = v; break;\n"
"#endif\n"
"#if NREG > 30u\n"
"    case 30u: g30 = v; break;\n"
"#endif\n"
"#if NREG > 31u\n"
"    case 31u: g31 = v; break;\n"
"#endif\n"
"    default: g0 = v; break;\n"
"    }\n"
"}\n"
"static float4 CC0; static float4 CC1;\n"
"static PSInput gin; static float4 gwpos; static float gface; static float gpw;\n"
"float usel(float4 v, uint i) { return i == 0u ? v.x : i == 1u ? v.y : i == 2u ? v.z : v.w; }\n"
"float4 uswzf(float4 v, uint z) { return float4(usel(v, z & 3u), usel(v, (z >> 2) & 3u), usel(v, (z >> 4) & 3u), usel(v, (z >> 6) & 3u)); }\n"
"float4 _rsx_gamma_input(float4 v, uint mask) {\n"
"    float4 lo = v / 12.92;\n"
"    float4 hi = pow(max((v + 0.055) / 1.055, (float4)0), (float4)2.4);\n"
"    float4 decoded = float4(v.r > 0.04045 ? hi.r : lo.r,\n"
"                            v.g > 0.04045 ? hi.g : lo.g,\n"
"                            v.b > 0.04045 ? hi.b : lo.b,\n"
"                            v.a > 0.04045 ? hi.a : lo.a);\n"
"    return float4((mask & 1u) ? decoded.r : v.r,\n"
"                  (mask & 2u) ? decoded.g : v.g,\n"
"                  (mask & 4u) ? decoded.b : v.b,\n"
"                  (mask & 8u) ? decoded.a : v.a);\n"
"}\n"
"uint   _rsx_pk4ub(float4 v) { uint4 b = (uint4)round(saturate(v) * 255.0); return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
"float4 _rsx_up4ub(float v) { uint u = asuint(v); return float4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u) / 255.0; }\n"
"uint   _rsx_pk4b(float4 v) { uint4 b = (uint4)((int4)round(clamp(v, -1.0, 1.0) * 127.0)) & 255u; return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
"float4 _rsx_up4b(float v) { uint u = asuint(v); int4 b = int4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u); b = (b << 24) >> 24; return clamp((float4)b / 127.0, -1.0, 1.0); }\n"
"uint   _rsx_pk2us(float4 v) { uint2 b = (uint2)round(saturate(v.xy) * 65535.0); return b.x | (b.y << 16); }\n"
"float4 _rsx_up2us(float v) { uint u = asuint(v); return float4(u & 65535u, u >> 16, u & 65535u, u >> 16) / 65535.0; }\n"
"uint   _rsx_pk2h(float4 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }\n"
"float4 _rsx_up2h(float v) { uint u = asuint(v); return float4(f16tof32(u & 65535u), f16tof32(u >> 16), f16tof32(u & 65535u), f16tof32(u >> 16)); }\n"
"float4 _rsx_lin2srgb(float4 cl) { cl = saturate(cl); float4 low = cl * 12.92; float4 high = 1.055 * pow(cl, 1.0 / 2.4) - 0.055; return saturate((cl < 0.0031308) ? low : high); }\n"
"float4 _rsx_srgb2lin(float4 cs) { float4 a = cs / 12.92; float4 b = pow(max(cs + 0.055, 0.0) / 1.055, 2.4); return (cs > 0.04045) ? b : a; }\n"
"float3 _rsx_nrm(float3 v) { return dot(v, v) > 0.0 ? normalize(v) : v; }\n"
"float _rsx_dp3p(float3 a, float3 b) { return mad(a.x, b.x, mad(a.y, b.y, a.z * b.z)); }\n"
"float4 _rsx_srgb_output(float4 v) {\n"
"    float3 lo = v.rgb * 12.92;\n"
"    float3 hi = 1.055 * pow(max(v.rgb, (float3)0), (float3)(1.0 / 2.4)) - 0.055;\n"
"    float3 rgb = float3(v.r < 0.0031308 ? lo.r : hi.r,\n"
"                         v.g < 0.0031308 ? lo.g : hi.g,\n"
"                         v.b < 0.0031308 ? lo.b : hi.b);\n"
"    return float4(saturate(rgb), v.a);\n"
"}\n"
"float4 uswz(float4 v, uint s) { return float4(v[s & 3u], v[(s >> 2) & 3u], v[(s >> 4) & 3u], v[(s >> 6) & 3u]); }\n"
"bool4 ucmp(float4 v, uint c) {\n"
"    if (c == 1u) return v >= (float4)0.0; if (c == 2u) return v <= (float4)0.0; if (c == 3u) return v != (float4)0.0;\n"
"    if (c == 4u) return v > (float4)0.0; if (c == 5u) return v < (float4)0.0; return v == (float4)0.0;\n"
"}\n"
"float4 usrcf(uint s, uint slot) {\n"
"    uint i = (s >> 2) & 63u; float4 v;\n"
"#ifdef F_ONESW\n"
"    switch (i) {\n"
"#if NREG > 1u\n"
"    case 1u: v = g1; break;\n"
"#endif\n"
"#if NREG > 2u\n"
"    case 2u: v = g2; break;\n"
"#endif\n"
"#if NREG > 3u\n"
"    case 3u: v = g3; break;\n"
"#endif\n"
"#if NREG > 4u\n"
"    case 4u: v = g4; break;\n"
"#endif\n"
"#if NREG > 5u\n"
"    case 5u: v = g5; break;\n"
"#endif\n"
"#if NREG > 6u\n"
"    case 6u: v = g6; break;\n"
"#endif\n"
"#if NREG > 7u\n"
"    case 7u: v = g7; break;\n"
"#endif\n"
"#if NREG > 8u\n"
"    case 8u: v = g8; break;\n"
"#endif\n"
"#if NREG > 9u\n"
"    case 9u: v = g9; break;\n"
"#endif\n"
"#if NREG > 10u\n"
"    case 10u: v = g10; break;\n"
"#endif\n"
"#if NREG > 11u\n"
"    case 11u: v = g11; break;\n"
"#endif\n"
"#if NREG > 12u\n"
"    case 12u: v = g12; break;\n"
"#endif\n"
"#if NREG > 13u\n"
"    case 13u: v = g13; break;\n"
"#endif\n"
"#if NREG > 14u\n"
"    case 14u: v = g14; break;\n"
"#endif\n"
"#if NREG > 15u\n"
"    case 15u: v = g15; break;\n"
"#endif\n"
"#if NREG > 16u\n"
"    case 16u: v = g16; break;\n"
"#endif\n"
"#if NREG > 17u\n"
"    case 17u: v = g17; break;\n"
"#endif\n"
"#if NREG > 18u\n"
"    case 18u: v = g18; break;\n"
"#endif\n"
"#if NREG > 19u\n"
"    case 19u: v = g19; break;\n"
"#endif\n"
"#if NREG > 20u\n"
"    case 20u: v = g20; break;\n"
"#endif\n"
"#if NREG > 21u\n"
"    case 21u: v = g21; break;\n"
"#endif\n"
"#if NREG > 22u\n"
"    case 22u: v = g22; break;\n"
"#endif\n"
"#if NREG > 23u\n"
"    case 23u: v = g23; break;\n"
"#endif\n"
"#if NREG > 24u\n"
"    case 24u: v = g24; break;\n"
"#endif\n"
"#if NREG > 25u\n"
"    case 25u: v = g25; break;\n"
"#endif\n"
"#if NREG > 26u\n"
"    case 26u: v = g26; break;\n"
"#endif\n"
"#if NREG > 27u\n"
"    case 27u: v = g27; break;\n"
"#endif\n"
"#if NREG > 28u\n"
"    case 28u: v = g28; break;\n"
"#endif\n"
"#if NREG > 29u\n"
"    case 29u: v = g29; break;\n"
"#endif\n"
"#if NREG > 30u\n"
"    case 30u: v = g30; break;\n"
"#endif\n"
"#if NREG > 31u\n"
"    case 31u: v = g31; break;\n"
"#endif\n"
"        case 32u: v = gwpos; break; case 33u: v = saturate(gin.col0); break; case 34u: v = saturate(gin.col1); break; case 35u: v = gin.fog; break;\n"
"        case 36u: v = gin.tc0; break; case 37u: v = gin.tc1; break; case 38u: v = gin.tc2; break; case 39u: v = gin.tc3; break;\n"
"        case 40u: v = gin.tc4; break; case 41u: v = gin.tc5; break; case 42u: v = gin.tc6; break; case 43u: v = gin.tc7; break;\n"
"        case 44u: v = gin.tc8; break; case 45u: v = gin.tc9; break; case 46u: v = (float4)gface; break;\n"
"        case 48u: v = gin.tc0 / gpw; break; case 49u: v = gin.tc1 / gpw; break; case 50u: v = gin.tc2 / gpw; break; case 51u: v = gin.tc3 / gpw; break;\n"
"        case 52u: v = gin.tc4 / gpw; break; case 53u: v = gin.tc5 / gpw; break; case 54u: v = gin.tc6 / gpw; break; case 55u: v = gin.tc7 / gpw; break;\n"
"        case 56u: v = gin.tc8 / gpw; break; case 57u: v = gin.tc9 / gpw; break;\n"
"    case 62u: v = C[slot]; break;\n"
"    case 63u: v = (float4)0; break;\n"
"    default: v = g0; break;\n"
"    }\n"
"#else\n"
"    if (i < NREG) {\n"
"        switch (i) {\n"
"#if NREG > 1u\n"
"        case 1u: v = g1; break;\n"
"#endif\n"
"#if NREG > 2u\n"
"        case 2u: v = g2; break;\n"
"#endif\n"
"#if NREG > 3u\n"
"        case 3u: v = g3; break;\n"
"#endif\n"
"#if NREG > 4u\n"
"        case 4u: v = g4; break;\n"
"#endif\n"
"#if NREG > 5u\n"
"        case 5u: v = g5; break;\n"
"#endif\n"
"#if NREG > 6u\n"
"        case 6u: v = g6; break;\n"
"#endif\n"
"#if NREG > 7u\n"
"        case 7u: v = g7; break;\n"
"#endif\n"
"#if NREG > 8u\n"
"        case 8u: v = g8; break;\n"
"#endif\n"
"#if NREG > 9u\n"
"        case 9u: v = g9; break;\n"
"#endif\n"
"#if NREG > 10u\n"
"        case 10u: v = g10; break;\n"
"#endif\n"
"#if NREG > 11u\n"
"        case 11u: v = g11; break;\n"
"#endif\n"
"#if NREG > 12u\n"
"        case 12u: v = g12; break;\n"
"#endif\n"
"#if NREG > 13u\n"
"        case 13u: v = g13; break;\n"
"#endif\n"
"#if NREG > 14u\n"
"        case 14u: v = g14; break;\n"
"#endif\n"
"#if NREG > 15u\n"
"        case 15u: v = g15; break;\n"
"#endif\n"
"#if NREG > 16u\n"
"        case 16u: v = g16; break;\n"
"#endif\n"
"#if NREG > 17u\n"
"        case 17u: v = g17; break;\n"
"#endif\n"
"#if NREG > 18u\n"
"        case 18u: v = g18; break;\n"
"#endif\n"
"#if NREG > 19u\n"
"        case 19u: v = g19; break;\n"
"#endif\n"
"#if NREG > 20u\n"
"        case 20u: v = g20; break;\n"
"#endif\n"
"#if NREG > 21u\n"
"        case 21u: v = g21; break;\n"
"#endif\n"
"#if NREG > 22u\n"
"        case 22u: v = g22; break;\n"
"#endif\n"
"#if NREG > 23u\n"
"        case 23u: v = g23; break;\n"
"#endif\n"
"#if NREG > 24u\n"
"        case 24u: v = g24; break;\n"
"#endif\n"
"#if NREG > 25u\n"
"        case 25u: v = g25; break;\n"
"#endif\n"
"#if NREG > 26u\n"
"        case 26u: v = g26; break;\n"
"#endif\n"
"#if NREG > 27u\n"
"        case 27u: v = g27; break;\n"
"#endif\n"
"#if NREG > 28u\n"
"        case 28u: v = g28; break;\n"
"#endif\n"
"#if NREG > 29u\n"
"        case 29u: v = g29; break;\n"
"#endif\n"
"#if NREG > 30u\n"
"        case 30u: v = g30; break;\n"
"#endif\n"
"#if NREG > 31u\n"
"        case 31u: v = g31; break;\n"
"#endif\n"
"        default: v = g0; break;\n"
"        }\n"
"    } else if (i >= 62u) {\n"
"        if (i == 62u) v = C[slot]; else v = (float4)0;      /* the constant; none */\n"
"    } else {\n"
"        switch (i) {                                         /* an input; 48..57: a texcoord, perspective-corrected */\n"
"        case 32u: v = gwpos; break; case 33u: v = saturate(gin.col0); break; case 34u: v = saturate(gin.col1); break; case 35u: v = gin.fog; break;\n"
"        case 36u: v = gin.tc0; break; case 37u: v = gin.tc1; break; case 38u: v = gin.tc2; break; case 39u: v = gin.tc3; break;\n"
"        case 40u: v = gin.tc4; break; case 41u: v = gin.tc5; break; case 42u: v = gin.tc6; break; case 43u: v = gin.tc7; break;\n"
"        case 44u: v = gin.tc8; break; case 45u: v = gin.tc9; break; case 46u: v = (float4)gface; break;\n"
"        case 48u: v = gin.tc0 / gpw; break; case 49u: v = gin.tc1 / gpw; break; case 50u: v = gin.tc2 / gpw; break; case 51u: v = gin.tc3 / gpw; break;\n"
"        case 52u: v = gin.tc4 / gpw; break; case 53u: v = gin.tc5 / gpw; break; case 54u: v = gin.tc6 / gpw; break; case 55u: v = gin.tc7 / gpw; break;\n"
"        case 56u: v = gin.tc8 / gpw; break; case 57u: v = gin.tc9 / gpw; break;\n"
"        default: v = (float4)0; break;\n"
"        }\n"
"    }\n"
"#endif\n"
"    if ((s >> 23) & 1u) {                                    /* a swizzle and / or modifiers */\n"
"        if ((s >> 25) & 1u) { if ((s >> 26) & 1u) v = (float4)usel(v, (s >> 9) & 3u); else v = uswzf(v, (s >> 9) & 255u); }\n"
"        if ((s >> 24) & 1u) {\n"
"            uint prec = (s >> 19) & 7u; bool ab = ((s >> 18) & 1u) != 0u;\n"
"            if (prec == 0u) { if (ab) v = abs(v); }\n"
"            else if (prec == 4u) { v = clamp(v, 0.0, 1.0); if (ab) v = abs(v); }\n"
"            else {\n"
"                if (ab) v = abs(v);\n"
"                if (prec == 1u) v = clamp(v, -65504.0, 65504.0); else if (prec == 2u) v = clamp(v, -2.0, 2.0); else if (prec == 3u) v = clamp(v, -1.0, 1.0);\n"
"            }\n"
"            if ((s >> 17) & 1u) v = -(v);\n"
"        }\n"
"    }\n"
"    return v;\n"
"}\n"
"void rputm(uint d, float4 v, bool4 m) {\n"
"    switch (d) {\n"
"#if NREG > 1u\n"
"    case 1u: g1 = m ? v : g1; break;\n"
"#endif\n"
"#if NREG > 2u\n"
"    case 2u: g2 = m ? v : g2; break;\n"
"#endif\n"
"#if NREG > 3u\n"
"    case 3u: g3 = m ? v : g3; break;\n"
"#endif\n"
"#if NREG > 4u\n"
"    case 4u: g4 = m ? v : g4; break;\n"
"#endif\n"
"#if NREG > 5u\n"
"    case 5u: g5 = m ? v : g5; break;\n"
"#endif\n"
"#if NREG > 6u\n"
"    case 6u: g6 = m ? v : g6; break;\n"
"#endif\n"
"#if NREG > 7u\n"
"    case 7u: g7 = m ? v : g7; break;\n"
"#endif\n"
"#if NREG > 8u\n"
"    case 8u: g8 = m ? v : g8; break;\n"
"#endif\n"
"#if NREG > 9u\n"
"    case 9u: g9 = m ? v : g9; break;\n"
"#endif\n"
"#if NREG > 10u\n"
"    case 10u: g10 = m ? v : g10; break;\n"
"#endif\n"
"#if NREG > 11u\n"
"    case 11u: g11 = m ? v : g11; break;\n"
"#endif\n"
"#if NREG > 12u\n"
"    case 12u: g12 = m ? v : g12; break;\n"
"#endif\n"
"#if NREG > 13u\n"
"    case 13u: g13 = m ? v : g13; break;\n"
"#endif\n"
"#if NREG > 14u\n"
"    case 14u: g14 = m ? v : g14; break;\n"
"#endif\n"
"#if NREG > 15u\n"
"    case 15u: g15 = m ? v : g15; break;\n"
"#endif\n"
"#if NREG > 16u\n"
"    case 16u: g16 = m ? v : g16; break;\n"
"#endif\n"
"#if NREG > 17u\n"
"    case 17u: g17 = m ? v : g17; break;\n"
"#endif\n"
"#if NREG > 18u\n"
"    case 18u: g18 = m ? v : g18; break;\n"
"#endif\n"
"#if NREG > 19u\n"
"    case 19u: g19 = m ? v : g19; break;\n"
"#endif\n"
"#if NREG > 20u\n"
"    case 20u: g20 = m ? v : g20; break;\n"
"#endif\n"
"#if NREG > 21u\n"
"    case 21u: g21 = m ? v : g21; break;\n"
"#endif\n"
"#if NREG > 22u\n"
"    case 22u: g22 = m ? v : g22; break;\n"
"#endif\n"
"#if NREG > 23u\n"
"    case 23u: g23 = m ? v : g23; break;\n"
"#endif\n"
"#if NREG > 24u\n"
"    case 24u: g24 = m ? v : g24; break;\n"
"#endif\n"
"#if NREG > 25u\n"
"    case 25u: g25 = m ? v : g25; break;\n"
"#endif\n"
"#if NREG > 26u\n"
"    case 26u: g26 = m ? v : g26; break;\n"
"#endif\n"
"#if NREG > 27u\n"
"    case 27u: g27 = m ? v : g27; break;\n"
"#endif\n"
"#if NREG > 28u\n"
"    case 28u: g28 = m ? v : g28; break;\n"
"#endif\n"
"#if NREG > 29u\n"
"    case 29u: g29 = m ? v : g29; break;\n"
"#endif\n"
"#if NREG > 30u\n"
"    case 30u: g30 = m ? v : g30; break;\n"
"#endif\n"
"#if NREG > 31u\n"
"    case 31u: g31 = m ? v : g31; break;\n"
"#endif\n"
"    default: g0 = m ? v : g0; break;\n"
"    }\n"
"}\n"
"float _rsx_shcmp(uint f, float reference, float depth) {\n"
"    if (f == 1u) return (float)(reference < depth); if (f == 2u) return (float)(reference == depth);\n"
"    if (f == 3u) return (float)(reference <= depth); if (f == 4u) return (float)(reference > depth);\n"
"    if (f == 5u) return (float)(reference != depth); if (f == 6u) return (float)(reference >= depth);\n"
"    if (f == 7u) return (float)(1.0); return (float)(0.0);\n"
"}\n"
"float _rsx_shtap(uint u, uint f, uint sc, float reference, float2 p, float2 size) {\n"
"    float4 e = C[sc + u];\n"
"    float2 t = p - 0.5;\n"
"    float2 c = clamp(t, 0.0, floor(e.xy * size + 0.5) - 1.0);\n"
"    uint m = (uint)e.z;\n"
"    if (((m & 4u) != 0u && c.x != t.x) || ((m & 8u) != 0u && c.y != t.y))\n"
"        return _rsx_shcmp(f, reference, e.w);\n"
"    float2 q = float2((m & 1u) != 0u ? c.x : t.x, (m & 2u) != 0u ? c.y : t.y);\n"
"    return _rsx_shcmp(f, reference, UT2[u].SampleLevel(US[u], (q + 0.5) / size, 0.0).r);\n"
"}\n"
"float4 _rsx_shadow(uint u, uint uf, uint sc, float3 coordinate) {\n"
"    uint f = (uf >> 3) & 7u;\n"
"    float reference = saturate(coordinate.z);\n"
"    float2 size = (float2)0;\n"
"    if ((uf >> 6) & 3u) { uint width, height; UT2[u].GetDimensions(width, height); size = float2(width, height); }\n"
"    uint mode = (uf >> 16) & 3u; float2 uv;\n"
"    if (mode == 1u) uv = coordinate.xy * C[sc + u].xy;\n"
"    else if (mode == 2u) uv = coordinate.xy / size;\n"
"    else if (mode == 3u) uv = coordinate.xy * C[sc + u].xy / size;\n"
"    else uv = coordinate.xy;\n"
"    if (!((uf >> 6) & 1u)) {\n"
"        float depth = UT2[u].SampleLevel(US[u], uv, 0.0).r;\n"
"        return (float4)_rsx_shcmp(f, reference, depth);\n"
"    }\n"
"    float2 position = uv * size - 0.5;\n"
"    float2 base = floor(position); float2 fraction = position - base;\n"
"    float c0, c1, c2, c3;\n"
"    if ((uf >> 18) & 1u) {\n"
"        c0 = _rsx_shtap(u, f, sc, reference, base + float2(0.5, 0.5), size);\n"
"        c1 = _rsx_shtap(u, f, sc, reference, base + float2(1.5, 0.5), size);\n"
"        c2 = _rsx_shtap(u, f, sc, reference, base + float2(0.5, 1.5), size);\n"
"        c3 = _rsx_shtap(u, f, sc, reference, base + float2(1.5, 1.5), size);\n"
"    } else {\n"
"        float d0 = UT2[u].SampleLevel(US[u], (base + float2(0.5, 0.5)) / size, 0.0).r;\n"
"        c0 = _rsx_shcmp(f, reference, d0);\n"
"        float d1 = UT2[u].SampleLevel(US[u], (base + float2(1.5, 0.5)) / size, 0.0).r;\n"
"        c1 = _rsx_shcmp(f, reference, d1);\n"
"        float d2 = UT2[u].SampleLevel(US[u], (base + float2(0.5, 1.5)) / size, 0.0).r;\n"
"        c2 = _rsx_shcmp(f, reference, d2);\n"
"        float d3 = UT2[u].SampleLevel(US[u], (base + float2(1.5, 1.5)) / size, 0.0).r;\n"
"        c3 = _rsx_shcmp(f, reference, d3);\n"
"    }\n"
"    return (float4)lerp(lerp(c0, c1, fraction.x), lerp(c2, c3, fraction.x), fraction.y);\n"
"}\n"
"float4 _rsx_fetch(uint op, uint u, bool exp_tex, float4 a, float4 b, uint sc) {\n"
"    uint4 U = P[7u + u]; uint uf = U.x; uint ty = uf & 3u; float4 rhs;\n"
"    if ((uf >> 2) & 1u) rhs = _rsx_shadow(u, uf, sc, op == 44u ? (a).xyz / (a).w : (a).xyz);\n"
"    else {\n"
"        if (ty == 1u) {\n"
"            if (op == 45u) rhs = UTC[u].SampleBias(US[u], (a).xyz, (b).x);\n"
"            else if (op == 46u) rhs = UTC[u].SampleLevel(US[u], (a).xyz, (b).x);\n"
"            else rhs = UTC[u].Sample(US[u], (a).xyz);\n"
"        } else if (ty == 2u) {\n"
"            if (op == 45u) rhs = UT3[u].SampleBias(US[u], (a).xyz, (b).x);\n"
"            else if (op == 46u) rhs = UT3[u].SampleLevel(US[u], (a).xyz, (b).x);\n"
"            else if (op == 44u) rhs = UT3[u].Sample(US[u], (a).xyz / (a).w);\n"
"            else rhs = UT3[u].Sample(US[u], (a).xyz);\n"
"        } else {\n"
"            float2 c = op == 44u ? (a).xy / (a).w : (a).xy;\n"
"            if ((uf >> 15) & 1u) c = c * C[sc + u].xy;\n"
"            if (((uf >> 14) & 1u) && op <= 44u) c = c * asfloat(U.yz);\n"
"            if (op == 45u) rhs = UT2[u].SampleBias(US[u], c, (b).x);\n"
"            else if (op == 46u) rhs = UT2[u].SampleLevel(US[u], c, (b).x);\n"
"            else rhs = UT2[u].Sample(US[u], c);\n"
"        }\n"
"        if (!((uf >> 9) & 1u) && (exp_tex || ((uf >> 8) & 1u))) rhs = (rhs * 2.0 - 1.0);\n"
"    }\n"
"    uint g = (uf >> 10) & 15u;\n"
"    if (g && !exp_tex) rhs = _rsx_gamma_input(rhs, g);\n"
"    return rhs;\n"
"}\n"
"PSOutput main(PSInput input, bool _rsx_face : SV_IsFrontFace) {\n"
"    uint4 H0 = P[0]; uint4 H1 = P[1];\n"
"    uint flags = H0.y;\n"
"    uint vs = H1.x;            /* fp_alpha is C[vs], fp_shadow_scale[u] is C[vs + 1 + u] */\n"
"    uint sc = vs + 1u;\n"
"    float4 wpos = input.position;\n"
"    if (flags & 128u) {\n"
"        float hgt = asfloat(H1.y);\n"
"        if (flags & 1024u) {\n"
"            float k = C[vs].y;\n"
"            float x = (flags & 512u) ? (input.position.x - 0.5) * k : input.position.x * k;\n"
"            float y = (flags & 256u) ? ((flags & 512u) ? hgt - (input.position.y + 0.5) * k : hgt - input.position.y * k)\n"
"                                     : ((flags & 512u) ? (input.position.y - 0.5) * k : input.position.y * k);\n"
"            wpos = float4(x, y, input.position.zw);\n"
"        } else {\n"
"            float x = input.position.x - ((flags & 512u) ? 0.5 : 0.0);\n"
"            float y = (flags & 256u) ? ((flags & 512u) ? hgt - input.position.y - 0.5 : hgt - input.position.y)\n"
"                                     : ((flags & 512u) ? input.position.y - 0.5 : input.position.y);\n"
"            wpos = float4(x, y, input.position.zw);\n"
"        }\n"
"    }\n"
"    gin = input; gwpos = wpos; gface = _rsx_face ? 1.0 : -1.0; gpw = input.position.w;\n"
"    uint4 HF = P[23];          /* x: registers to clear, y: export registers (a byte each), z: the depth register */\n"
"    if (HF.x) {\n"
"#if NREG > 0u\n"
"        if ((HF.x >> 0u) & 1u) g0 = (float4)0;\n"
"#endif\n"
"#if NREG > 1u\n"
"        if ((HF.x >> 1u) & 1u) g1 = (float4)0;\n"
"#endif\n"
"#if NREG > 2u\n"
"        if ((HF.x >> 2u) & 1u) g2 = (float4)0;\n"
"#endif\n"
"#if NREG > 3u\n"
"        if ((HF.x >> 3u) & 1u) g3 = (float4)0;\n"
"#endif\n"
"#if NREG > 4u\n"
"        if ((HF.x >> 4u) & 1u) g4 = (float4)0;\n"
"#endif\n"
"#if NREG > 5u\n"
"        if ((HF.x >> 5u) & 1u) g5 = (float4)0;\n"
"#endif\n"
"#if NREG > 6u\n"
"        if ((HF.x >> 6u) & 1u) g6 = (float4)0;\n"
"#endif\n"
"#if NREG > 7u\n"
"        if ((HF.x >> 7u) & 1u) g7 = (float4)0;\n"
"#endif\n"
"#if NREG > 8u\n"
"        if ((HF.x >> 8u) & 1u) g8 = (float4)0;\n"
"#endif\n"
"#if NREG > 9u\n"
"        if ((HF.x >> 9u) & 1u) g9 = (float4)0;\n"
"#endif\n"
"#if NREG > 10u\n"
"        if ((HF.x >> 10u) & 1u) g10 = (float4)0;\n"
"#endif\n"
"#if NREG > 11u\n"
"        if ((HF.x >> 11u) & 1u) g11 = (float4)0;\n"
"#endif\n"
"#if NREG > 12u\n"
"        if ((HF.x >> 12u) & 1u) g12 = (float4)0;\n"
"#endif\n"
"#if NREG > 13u\n"
"        if ((HF.x >> 13u) & 1u) g13 = (float4)0;\n"
"#endif\n"
"#if NREG > 14u\n"
"        if ((HF.x >> 14u) & 1u) g14 = (float4)0;\n"
"#endif\n"
"#if NREG > 15u\n"
"        if ((HF.x >> 15u) & 1u) g15 = (float4)0;\n"
"#endif\n"
"#if NREG > 16u\n"
"        if ((HF.x >> 16u) & 1u) g16 = (float4)0;\n"
"#endif\n"
"#if NREG > 17u\n"
"        if ((HF.x >> 17u) & 1u) g17 = (float4)0;\n"
"#endif\n"
"#if NREG > 18u\n"
"        if ((HF.x >> 18u) & 1u) g18 = (float4)0;\n"
"#endif\n"
"#if NREG > 19u\n"
"        if ((HF.x >> 19u) & 1u) g19 = (float4)0;\n"
"#endif\n"
"#if NREG > 20u\n"
"        if ((HF.x >> 20u) & 1u) g20 = (float4)0;\n"
"#endif\n"
"#if NREG > 21u\n"
"        if ((HF.x >> 21u) & 1u) g21 = (float4)0;\n"
"#endif\n"
"#if NREG > 22u\n"
"        if ((HF.x >> 22u) & 1u) g22 = (float4)0;\n"
"#endif\n"
"#if NREG > 23u\n"
"        if ((HF.x >> 23u) & 1u) g23 = (float4)0;\n"
"#endif\n"
"#if NREG > 24u\n"
"        if ((HF.x >> 24u) & 1u) g24 = (float4)0;\n"
"#endif\n"
"#if NREG > 25u\n"
"        if ((HF.x >> 25u) & 1u) g25 = (float4)0;\n"
"#endif\n"
"#if NREG > 26u\n"
"        if ((HF.x >> 26u) & 1u) g26 = (float4)0;\n"
"#endif\n"
"#if NREG > 27u\n"
"        if ((HF.x >> 27u) & 1u) g27 = (float4)0;\n"
"#endif\n"
"#if NREG > 28u\n"
"        if ((HF.x >> 28u) & 1u) g28 = (float4)0;\n"
"#endif\n"
"#if NREG > 29u\n"
"        if ((HF.x >> 29u) & 1u) g29 = (float4)0;\n"
"#endif\n"
"#if NREG > 30u\n"
"        if ((HF.x >> 30u) & 1u) g30 = (float4)0;\n"
"#endif\n"
"#if NREG > 31u\n"
"        if ((HF.x >> 31u) & 1u) g31 = (float4)0;\n"
"#endif\n"
"    }\n"
"    CC0 = (float4)0; CC1 = (float4)0;\n"
"    uint n = H0.x; uint pc = 0;\n"
"    [loop] while (pc < n) {\n"
"        uint4 q0 = P[32u + 2u * pc]; uint4 q1 = P[33u + 2u * pc];\n"
"        pc++;\n"
"        uint w = q0.x; uint op = w & 255u; uint x = q0.y;\n"
"#ifdef F_PLAIN\n"
"        {\n"
"#else\n"
"        if (!((q1.w >> 4) & 1u)) {\n"
"#endif\n"
"            uint fslot = x >> 21; uint fns = q1.w & 3u;\n"
"            float4 a = usrcf(q0.z, fslot); float4 b = (float4)0; float4 c = (float4)0;\n"
"            if (fns > 1u) { b = usrcf(q0.w, fslot); if (fns > 2u) c = usrcf(q1.x, fslot); }\n"
"            float4 _v;\n"
"            if (op <= 4u) {\n"
"                if (op <= 2u) { if (op == 1u) _v = (float4)(a); else _v = (float4)((a) * (b)); }\n"
"                else { if (op == 3u) _v = (float4)((a) + (b)); else _v = (float4)((a) * (b) + (c)); }\n"
"            } else\n"
"            if (op >= 43u) _v = (float4)(_rsx_fetch(op, (x >> 17) & 15u, ((w >> 29) & 1u) != 0u, a, b, sc));\n"
"            else\n"
"            switch (op) {\n"
"            case 1u: _v = (float4)(a); break;\n"
"            case 2u: _v = (float4)((a) * (b)); break;\n"
"            case 3u: _v = (float4)((a) + (b)); break;\n"
"            case 4u: _v = (float4)((a) * (b) + (c)); break;\n"
"            case 5u: _v = (float4)(dot((a).xyz, (b).xyz)); break;\n"
"            case 6u: _v = (float4)(_rsx_dp3p((a).xyz, (b).xyz)); break;\n"
"            case 7u: _v = (float4)(dot((a), (b))); break;\n"
"            case 8u: _v = (float4)(min((a), (b))); break;\n"
"            case 9u: _v = (float4)(max((a), (b))); break;\n"
"            case 10u: _v = (float4)(frac(a)); break;\n"
"            case 11u: _v = (float4)(floor(a)); break;\n"
"            case 12u: _v = (float4)(ddx(a)); break;\n"
"            case 13u: _v = (float4)(ddy(a)); break;\n"
"            case 14u: _v = (float4)((1.0 / (a).x)); break;\n"
"            case 15u: _v = (float4)(rsqrt(abs((a).x))); break;\n"
"            case 16u: _v = (float4)(exp2((a).x)); break;\n"
"            case 17u: _v = (float4)(log2((a).x)); break;\n"
"            case 18u: _v = (float4)(cos((a).x)); break;\n"
"            case 19u: _v = (float4)(sin((a).x)); break;\n"
"            case 20u: _v = (float4)(pow((a).x, (b).x)); break;\n"
"            case 21u: _v = (float4)((a) / (b).x); break;\n"
"            case 22u: _v = (float4)((abs(a) > 0.0 ? (a) / sqrt(abs((b).x)) : (float4)0.0)); break;\n"
"            case 23u: _v = (float4)(dot((a).xy, (b).xy)); break;\n"
"            case 24u: _v = (float4)(float4(_rsx_nrm((a).xyz), 1.0)); break;\n"
"            case 25u: _v = (float4)(lerp((c), (b), (a))); break;\n"
"            case 26u: _v = (float4)(float4(1.0, (a).y, (a).y > 0.0 ? exp2((a).w) : 0.0, 1.0)); break;\n"
"            case 27u: _v = (float4)((float4)((a) <  (b))); break;\n"
"            case 28u: _v = (float4)((float4)((a) >= (b))); break;\n"
"            case 29u: _v = (float4)((float4)((a) <= (b))); break;\n"
"            case 30u: _v = (float4)((float4)((a) >  (b))); break;\n"
"            case 31u: _v = (float4)((float4)((a) != (b))); break;\n"
"            case 32u: _v = (float4)((float4)((a) == (b))); break;\n"
"            case 33u: _v = (float4)(asfloat(_rsx_pk4ub(a))); break;\n"
"            case 34u: _v = (float4)(_rsx_up4ub((a).x)); break;\n"
"            case 35u: _v = (float4)(asfloat(_rsx_pk4ub(_rsx_lin2srgb(a)))); break;\n"
"            case 36u: _v = (float4)(_rsx_srgb2lin(_rsx_up4ub((a).x))); break;\n"
"            case 37u: _v = (float4)(asfloat(_rsx_pk4b(a))); break;\n"
"            case 38u: _v = (float4)(_rsx_up4b((a).x)); break;\n"
"            case 39u: _v = (float4)(asfloat(_rsx_pk2us(a))); break;\n"
"            case 40u: _v = (float4)(_rsx_up2us((a).x)); break;\n"
"            case 41u: _v = (float4)(asfloat(_rsx_pk2h(a))); break;\n"
"            case 42u: _v = (float4)(_rsx_up2h((a).x)); break;\n"
"            default: _v = (float4)(_rsx_fetch(op, (x >> 17) & 15u, ((w >> 29) & 1u) != 0u, a, b, sc)); break;\n"
"            }\n"
"            if ((q1.w >> 2) & 1u) {\n"
"            uint scl = (w >> 23) & 7u;\n"
"            if (scl != 0u) { if (scl == 1u) _v = _v * 2.0; else if (scl == 2u) _v = _v * 4.0; else if (scl == 3u) _v = _v * 8.0;\n"
"            else if (scl == 5u) _v = _v / 2.0; else if (scl == 6u) _v = _v / 4.0; else if (scl == 7u) _v = _v / 8.0; }\n"
"            if ((w >> 20) & 1u) _v = saturate(_v);\n"
"            else { uint dc = (w >> 21) & 3u;\n"
"                if (dc != 0u) { if (dc == 1u) _v = clamp(_v, -65504.0, 65504.0); else if (dc == 2u) _v = clamp(_v, -2.0, 2.0); else if (dc == 3u) _v = clamp(_v, -1.0, 1.0); } }\n"
"            }\n"
"            uint fdm = (w >> 15) & 15u;\n"
"            rputm((w >> 8) & 63u, _v, bool4(fdm & 1u, fdm & 2u, fdm & 4u, fdm & 8u));\n"
"            continue;\n"
"        }\n"
"#ifndef F_PLAIN\n"
"        uint ex = x & 3u;\n"
"        bool4 cnd = bool4(true, true, true, true);\n"
"        if (ex == 2u) { float4 cv = ((w >> 28) & 1u) ? CC1 : CC0; uint cz = (x >> 5) & 255u;\n"
"            if (cz != 0xE4u) cv = uswzf(cv, cz);\n"
"            cnd = ucmp(cv, (x >> 2) & 7u); }\n"
"        else if (ex == 1u) cnd = bool4(false, false, false, false);\n"
"        if (op >= 60u) {\n"
"            bool any_c = any(cnd);\n"
"            if (op == 60u) { if (!any_c) pc = q1.y; }\n"
"            else if (op == 61u) { pc = q1.y; }\n"
"            continue;\n"
"        }\n"
"        if (op == 47u) { if (any(cnd)) discard; continue; }\n"
"        uint insrc = (x >> 13) & 15u; uint slot = x >> 21; bool persp = ((w >> 30) & 1u) != 0u;\n"
"        uint ns = q1.w & 3u;\n"
"        float4 a = usrcf(q0.z, slot);\n"
"        float4 b = (float4)0; float4 c = (float4)0;\n"
"        if (ns > 1u) b = usrcf(q0.w, slot);\n"
"        if (ns > 2u) c = usrcf(q1.x, slot);\n"
"        float4 _v;\n"
"#ifdef F_DISP\n"
"        if (op <= 4u) {\n"
"            if (op <= 2u) { if (op == 1u) _v = (float4)(a); else _v = (float4)((a) * (b)); }\n"
"            else { if (op == 3u) _v = (float4)((a) + (b)); else _v = (float4)((a) * (b) + (c)); }\n"
"        } else\n"
"#endif\n"
"        switch (op) {\n"
"        case 1u: _v = (float4)(a); break;\n"
"        case 2u: _v = (float4)((a) * (b)); break;\n"
"        case 3u: _v = (float4)((a) + (b)); break;\n"
"        case 4u: _v = (float4)((a) * (b) + (c)); break;\n"
"        case 5u: _v = (float4)(dot((a).xyz, (b).xyz)); break;\n"
"        case 6u: _v = (float4)(_rsx_dp3p((a).xyz, (b).xyz)); break;\n"
"        case 7u: _v = (float4)(dot((a), (b))); break;\n"
"        case 8u: _v = (float4)(min((a), (b))); break;\n"
"        case 9u: _v = (float4)(max((a), (b))); break;\n"
"        case 10u: _v = (float4)(frac(a)); break;\n"
"        case 11u: _v = (float4)(floor(a)); break;\n"
"        case 12u: _v = (float4)(ddx(a)); break;\n"
"        case 13u: _v = (float4)(ddy(a)); break;\n"
"        case 14u: _v = (float4)((1.0 / (a).x)); break;\n"
"        case 15u: _v = (float4)(rsqrt(abs((a).x))); break;\n"
"        case 16u: _v = (float4)(exp2((a).x)); break;\n"
"        case 17u: _v = (float4)(log2((a).x)); break;\n"
"        case 18u: _v = (float4)(cos((a).x)); break;\n"
"        case 19u: _v = (float4)(sin((a).x)); break;\n"
"        case 20u: _v = (float4)(pow((a).x, (b).x)); break;\n"
"        case 21u: _v = (float4)((a) / (b).x); break;\n"
"        case 22u: _v = (float4)((abs(a) > 0.0 ? (a) / sqrt(abs((b).x)) : (float4)0.0)); break;\n"
"        case 23u: _v = (float4)(dot((a).xy, (b).xy)); break;\n"
"        case 24u: _v = (float4)(float4(_rsx_nrm((a).xyz), 1.0)); break;\n"
"        case 25u: _v = (float4)(lerp((c), (b), (a))); break;\n"
"        case 26u: _v = (float4)(float4(1.0, (a).y, (a).y > 0.0 ? exp2((a).w) : 0.0, 1.0)); break;\n"
"        case 27u: _v = (float4)((float4)((a) <  (b))); break;\n"
"        case 28u: _v = (float4)((float4)((a) >= (b))); break;\n"
"        case 29u: _v = (float4)((float4)((a) <= (b))); break;\n"
"        case 30u: _v = (float4)((float4)((a) >  (b))); break;\n"
"        case 31u: _v = (float4)((float4)((a) != (b))); break;\n"
"        case 32u: _v = (float4)((float4)((a) == (b))); break;\n"
"        case 33u: _v = (float4)(asfloat(_rsx_pk4ub(a))); break;\n"
"        case 34u: _v = (float4)(_rsx_up4ub((a).x)); break;\n"
"        case 35u: _v = (float4)(asfloat(_rsx_pk4ub(_rsx_lin2srgb(a)))); break;\n"
"        case 36u: _v = (float4)(_rsx_srgb2lin(_rsx_up4ub((a).x))); break;\n"
"        case 37u: _v = (float4)(asfloat(_rsx_pk4b(a))); break;\n"
"        case 38u: _v = (float4)(_rsx_up4b((a).x)); break;\n"
"        case 39u: _v = (float4)(asfloat(_rsx_pk2us(a))); break;\n"
"        case 40u: _v = (float4)(_rsx_up2us((a).x)); break;\n"
"        case 41u: _v = (float4)(asfloat(_rsx_pk2h(a))); break;\n"
"        case 42u: _v = (float4)(_rsx_up2h((a).x)); break;\n"
"        default: _v = (float4)(_rsx_fetch(op, (x >> 17) & 15u, ((w >> 29) & 1u) != 0u, a, b, sc)); break;\n"
"        }\n"
"        if ((q1.w >> 2) & 1u) {\n"
"        uint scl = (w >> 23) & 7u;\n"
"        if (scl != 0u) { if (scl == 1u) _v = _v * 2.0; else if (scl == 2u) _v = _v * 4.0; else if (scl == 3u) _v = _v * 8.0;\n"
"        else if (scl == 5u) _v = _v / 2.0; else if (scl == 6u) _v = _v / 4.0; else if (scl == 7u) _v = _v / 8.0; }\n"
"        if ((w >> 20) & 1u) _v = saturate(_v);\n"
"        else { uint dc = (w >> 21) & 3u;\n"
"            if (dc != 0u) { if (dc == 1u) _v = clamp(_v, -65504.0, 65504.0); else if (dc == 2u) _v = clamp(_v, -2.0, 2.0); else if (dc == 3u) _v = clamp(_v, -1.0, 1.0); } }\n"
"        }\n"
"        uint dm = (w >> 15) & 15u;\n"
"        bool4 m = bool4(dm & 1u, dm & 2u, dm & 4u, dm & 8u);\n"
"        uint d = (w >> 8) & 63u;\n"
"        float4 nd = _v;            /* the destination register's value after the write */\n"
"        if ((w >> 19) & 1u) {\n"
"            if (!((q1.w >> 3) & 1u)) {\n"
"                float4 old = rget(d);\n"
"                nd = (m && cnd) ? _v : old;\n"
"            }\n"
"            rput(d, nd);\n"
"        }\n"
"        if ((w >> 26) & 1u) {\n"
"            uint cw = (w >> 27) & 1u;\n"
"            float4 src = ((w >> 19) & 1u) ? nd : _v;\n"
"            bool4 cm = ((w >> 19) & 1u) ? m : (m && cnd);\n"
"            if (cw) CC1 = cm ? src : CC1; else CC0 = cm ? src : CC0;\n"
"        }\n"
"#endif\n"
"    }\n"
"    float4 o[4]; o[0] = (float4)0; o[1] = (float4)0; o[2] = (float4)0; o[3] = (float4)0;\n"
"    uint nt = H0.z;\n"
"    bool hx = (flags & 1u) != 0u;\n"
"    [unroll] for (uint t = 0; t < 4u; t++) {\n"
"        if (t < nt) {\n"
"            float4 v = rget((HF.y >> (8u * t)) & 255u);\n"
"            uint4 G = P[3u + t];\n"
"            if (flags & 2048u) {\n"
"                [unroll] for (uint l = 0; l < 4u; l++) {\n"
"                    uint gl = G[l];\n"
"                    if (gl & 32768u) {\n"
"                        float gv = f16tof32((asuint(usel(rget(gl & 127u), (gl >> 8) & 3u)) >> (((gl >> 10) & 1u) * 16u)) & 0xFFFFu);\n"
"                        if (l == 0u) v.x = gv; else if (l == 1u) v.y = gv; else if (l == 2u) v.z = gv; else v.w = gv;\n"
"                    }\n"
"                }\n"
"            }\n"
"            o[t] = (v == v) ? v : (float4)0;\n"
"            if (flags & 4u) o[t] = _rsx_srgb_output(o[t]);\n"
"        }\n"
"    }\n"
"    if (flags & 16u) {\n"
"        float3 s3 = (flags & 64u) ? C[vs].zzz : asfloat(P[2].xyz);\n"
"        o[0].rgb = ((flags & 32u) ? saturate(o[0].rgb) : (o[0].rgb)) * (s3);\n"
"    }\n"
"    if (flags & 8u) {\n"
"        float _rsx_alpha_ref = C[vs].x; uint ac = H0.w; bool pass;\n"
"        if (ac == 1u) pass = o[0].a < _rsx_alpha_ref; else if (ac == 2u) pass = o[0].a == _rsx_alpha_ref;\n"
"        else if (ac == 3u) pass = o[0].a <= _rsx_alpha_ref; else if (ac == 4u) pass = o[0].a > _rsx_alpha_ref;\n"
"        else if (ac == 5u) pass = o[0].a != _rsx_alpha_ref; else if (ac == 6u) pass = o[0].a >= _rsx_alpha_ref;\n"
"        else pass = false;\n"
"        if (!(pass)) discard;\n"
"    }\n"
"    PSOutput _out;\n"
"    _out.t0 = o[0]; _out.t1 = o[1]; _out.t2 = o[2]; _out.t3 = o[3];\n"
"#ifdef UBER_DEPTH\n"
"    float _d = rget(HF.z & 255u).z;\n"
"    _out.depth = (_d == _d) ? saturate(_d) : 0.0;\n"
"#endif\n"
"    return _out;\n"
"}\n"
"\n";

const char* rsx_fp_uber_fast_hlsl(int cls)
{
    static char* text[RSX_FP_UBER_FAST_CLASSES];
    if (cls < 0 || cls >= RSX_FP_UBER_FAST_CLASSES) return NULL;
    if (!text[cls]) {
        char* t = (char*)malloc(sizeof s_uber_fast_body + 96);
        if (!t) return NULL;
        const int k = snprintf(t, 96, "#define NREG %uu\n%s", s_fast_nreg[cls % RSX_FP_UBER_FAST_SIZES],
                               cls < RSX_FP_UBER_FAST_SIZES ? "#define F_PLAIN 1\n" : "");
        memcpy(t + k, s_uber_fast_body, sizeof s_uber_fast_body);
        text[cls] = t;
    }
    return text[cls];
}
