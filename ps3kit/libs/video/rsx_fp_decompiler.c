/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) → HLSL decompiler
 *
 * See rsx_fp_decompiler.h and docs/RSX_FRAGMENT_PROGRAM.md.
 *
 * The encoding (4 words / 16 bytes per instruction; CONST sources carry an
 * inline 16-byte constant in the following slot) is taken verbatim from
 * Mesa/nouveau nvfx_shader.h. Bit positions are spelled out as named macros
 * below so this file is self-documenting.
 */

#include <stdlib.h>
#include "rsx_fp_decompiler.h"
#include <stdio.h>
#include <string.h>

/* ---- DWORD 0 (OPDEST) --------------------------------------------------- */
#define FP_END              (1u << 0)
#define FP_OUT_REG_SHIFT    1
#define FP_OUT_REG_MASK     (63u << 1)
#define FP_OUT_HALF         (1u << 7)
#define FP_COND_WRITE       (1u << 8)
#define FP_OUT_MASK_SHIFT   9          /* X@9 Y@10 Z@11 W@12 */
#define FP_INPUT_SRC_SHIFT  13
#define FP_INPUT_SRC_MASK   (15u << 13)
#define FP_TEX_UNIT_SHIFT   17
#define FP_TEX_UNIT_MASK    (15u << 17)
#define FP_OPCODE_SHIFT     24
#define FP_OPCODE_MASK      (0x3Fu << 24)
#define FP_OUT_NONE         (1u << 30)
#define FP_OUT_SAT          (1u << 31)

/* ---- DWORD 1/2/3 (SRC0/1/2) --------------------------------------------- */
#define FP_REG_TYPE_SHIFT   0
#define FP_REG_TYPE_MASK    (3u << 0)
#define   FP_REG_TYPE_TEMP  0
#define   FP_REG_TYPE_INPUT 1
#define   FP_REG_TYPE_CONST 2
#define FP_SRC_REG_SHIFT    2
#define FP_SRC_REG_MASK     (63u << 2)
#define FP_SRC_HALF         (1u << 8)
#define FP_SRC_SWZ_SHIFT    9          /* X@9 Y@11 Z@13 W@15, 2 bits each */
#define FP_SRC_NEGATE       (1u << 17)
#define FP_SRC0_ABS         (1u << 29) /* in DWORD1 */
#define FP_SRC1_ABS         (1u << 18) /* in DWORD2 */
#define FP_SRC2_ABS         (1u << 18) /* in DWORD3 (RPCS3 SRC2.abs) */
#define FP_BRANCH           (1u << 31) /* DWORD2 bit31: instruction is a branch */

/* ---- Execution-condition / condition-code fields (SRC0 = DWORD1) ---------
 * The NV40 fragment ISA predicates each instruction on a condition register
 * (CC0/CC1). Layout verified against RPCS3's RSXFragmentProgram.h SRC0 union
 * (exec_if_lt@18, exec_if_eq@19, exec_if_gr@20, cond_swizzle@21..28,
 * cond_mod_reg_index@30, cond_reg_index@31) and OPDEST.set_cond@8. All three
 * exec bits set (0b111) == unconditional (the default); none set == never.
 * Dropped entirely until now: MEASURED present in 4 of this capture's 43
 * fragment programs (scratch/a010_reports/fragment/, cond_detail_out.txt) —
 * genuine SET_CC→exec_if producer/consumer pairs (incl. conditional TEX). */
#define FP_EXEC_IF_LT       (1u << 18)
#define FP_EXEC_IF_EQ       (1u << 19)
#define FP_EXEC_IF_GR       (1u << 20)
#define FP_COND_SWZ_SHIFT   21          /* X@21 Y@23 Z@25 W@27, 2 bits each */
#define FP_COND_MOD_REG     (1u << 30)  /* set_cond WRITE target: cc0/cc1     */
#define FP_COND_REG         (1u << 31)  /* exec_if READ source:   cc0/cc1     */

/* ---- Opcodes ------------------------------------------------------------ */
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

const char* rsx_fp_opcode_name(u32 op)
{
    switch (op) {
    case OP_NOP: return "NOP"; case OP_MOV: return "MOV"; case OP_MUL: return "MUL";
    case OP_ADD: return "ADD"; case OP_MAD: return "MAD"; case OP_DP3: return "DP3";
    case OP_DP4: return "DP4"; case OP_DST: return "DST"; case OP_MIN: return "MIN";
    case OP_MAX: return "MAX"; case OP_SLT: return "SLT"; case OP_SGE: return "SGE";
    case OP_SLE: return "SLE"; case OP_SGT: return "SGT"; case OP_SNE: return "SNE";
    case OP_SEQ: return "SEQ"; case OP_FRC: return "FRC"; case OP_FLR: return "FLR";
    case OP_KIL: return "KIL"; case OP_DDX: return "DDX"; case OP_DDY: return "DDY";
    case OP_TEX: return "TEX"; case OP_TXP: return "TXP"; case OP_TXD: return "TXD";
    case OP_RCP: return "RCP"; case OP_RSQ: return "RSQ"; case OP_EX2: return "EX2";
    case OP_LG2: return "LG2"; case OP_LIT: return "LIT"; case OP_LRP: return "LRP";
    case OP_STR: return "STR"; case OP_SFL: return "SFL"; case OP_COS: return "COS";
    case OP_SIN: return "SIN"; case OP_POW: return "POW"; case OP_DP2A: return "DP2A";
    case OP_TXL: return "TXL"; case OP_TXB: return "TXB"; case OP_RFL: return "RFL";
    case OP_DIV: return "DIV"; case OP_DP2: return "DP2"; case OP_NRM: return "NRM";
    case OP_DIVSQ: return "DIVSQ"; case OP_LIF: return "LIF";
    case OP_PK4B: return "PK4B"; case OP_UP4B: return "UP4B"; case OP_PK2H: return "PK2H";
    case OP_UP2H: return "UP2H"; case OP_PK4UB: return "PK4UB"; case OP_UP4UB: return "UP4UB";
    case OP_PK2US: return "PK2US"; case OP_UP2US: return "UP2US"; case OP_PKG: return "PKG";
    case OP_UPG: return "UPG";
    case OP_FENCT: return "FENCT"; case OP_FENCB: return "FENCB";
    default:     return "?";
    }
}

/* ------------------------------------------------------------------------- */

u32 rsx_fp_read_word(const u8* p)
{
    u32 be = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    return (be << 16) | (be >> 16);   /* 16-bit half-word swap */
}

u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        /* A CONST source pulls an inline 16-byte constant slot. */
        if (((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
            ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
            ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) {
            if (off + 16 <= max_bytes) off += 16;
        }
        if (w0 & FP_END) return off;
    }
    return 0;
}

static int fp_instruction_has_constant(u32 w1, u32 w2, u32 w3)
{
    return
        ((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST ||
        ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST ||
        ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST;
}

unsigned long long g_rsx_fp_nonfinite_constants = 0;

/* An inline fragment constant is never legitimately non-finite.  A NaN one
 * poisons every pixel the program touches, which on an FP16 HDR target shows
 * up as a saturated white fill rather than as an obviously broken draw.
 * Substitute zero and count it so the underlying bad read stays visible. */
u32 rsx_fp_sanitize_constant_word(u32 w)
{
    float f;
    memcpy(&f, &w, sizeof f);
    if (f == f && f <= 3.0e38f && f >= -3.0e38f)
        return w;
    g_rsx_fp_nonfinite_constants++;
    return 0;
}

int rsx_fp_collect_constants(
    const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out)
{
    if (!ucode || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    u32 off = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        if (fp_instruction_has_constant(w1, w2, w3)) {
            if (off + 16 > max_bytes ||
                out->count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return -1;
            for (u32 component = 0; component < 4; ++component)
                out->values[out->count][component] =
                    rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + component * 4));
            out->count++;
            off += 16;
        }
        if (w0 & FP_END)
            return (int)out->count;
    }
    return -1;
}

/* rsx_fp_collect_constants without clearing the whole block first: only
 * out->count and values[0..count) are written. The block is 32 KB (2048
 * slots), so the clear cost more than the scan and flushed the L1 cache on
 * every draw; a caller that never reads past count gets the same result. */
int rsx_fp_collect_constants_used(
    const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out)
{
    if (!ucode || !out)
        return -1;
    out->count = 0;

    u32 off = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        if (fp_instruction_has_constant(w1, w2, w3)) {
            if (off + 16 > max_bytes ||
                out->count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return -1;
            for (u32 component = 0; component < 4; ++component)
                out->values[out->count][component] =
                    rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + component * 4));
            out->count++;
            off += 16;
        }
        if (w0 & FP_END)
            return (int)out->count;
    }
    return -1;
}

/* Eight bytes a step (xxHash64's round) instead of byte-at-a-time FNV-1a:
 * the structural hash runs on every draw's fragment program. */
static u64 fp_hash_bytes(const void* data, u32 size, u64 hash)
{
    const u8* p = (const u8*)data;
    const u64 P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull;
    hash ^= (u64)size * P3;
    while (size >= 8) {
        u64 w; memcpy(&w, p, 8);
        w *= P2; w = (w << 31) | (w >> 33); w *= P1;
        hash ^= w;
        hash = ((hash << 27) | (hash >> 37)) * P1 + P3;
        p += 8; size -= 8;
    }
    while (size--) {
        hash ^= (u64)(*p++) * P3;
        hash = ((hash << 11) | (hash >> 53)) * P1;
    }
    hash ^= hash >> 33; hash *= P2; hash ^= hash >> 29;
    return hash;
}

u64 rsx_fp_structural_hash(const u8* ucode, u32 max_bytes, u64 seed)
{
    if (!ucode)
        return 0;
    static const u32 structural_tag = 0x31535046u; /* "FPS1" */
    u64 hash = fp_hash_bytes(&structural_tag, sizeof(structural_tag), seed);
    u32 off = 0;
    u32 instruction_count = 0;
    u32 constant_count = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        hash = fp_hash_bytes(ucode + off, 16, hash);
        instruction_count++;
        off += 16;
        if (fp_instruction_has_constant(w1, w2, w3)) {
            if (off + 16 > max_bytes ||
                constant_count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return 0;
            constant_count++;
            off += 16;
        }
        if (w0 & FP_END) {
            hash = fp_hash_bytes(
                &instruction_count, sizeof(instruction_count), hash);
            hash = fp_hash_bytes(
                &constant_count, sizeof(constant_count), hash);
            return hash;
        }
    }
    return 0;
}

int rsx_fp_uses_wpos(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT) == 0 &&
            (((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_INPUT ||
             ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_INPUT ||
             ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_INPUT))
            return 1;
        if (fp_instruction_has_constant(w1, w2, w3)) off += 16;
        if (w0 & FP_END) return 0;
    }
    return 0;
}

int rsx_fp_apply_shader_window(char* hlsl, u32 out_size, u32 shader_window)
{
    return rsx_fp_apply_shader_window_scaled(hlsl, out_size, shader_window, NULL);
}

/* inv_scale (an HLSL expression, or NULL for 1): the render target has that
 * many guest pixels per host pixel, so SV_Position is brought back to guest
 * pixels. A host pixel then reads its guest pixel's coordinate: half-pixel
 * centres pos * k, integer centres (pos - 0.5) * k, both of which floor to
 * the guest pixel; a bottom-left origin counts down from the guest height
 * (H - (pos + 0.5) * k for integer centres, again flooring right). With
 * k = 1 each is the unscaled form. */
int rsx_fp_apply_shader_window_scaled(char* hlsl, u32 out_size, u32 shader_window,
                                      const char* inv_scale)
{
    static const char from[] = "input.position";
    static const char to[] = "_rsx_wpos";
    const u32 height = shader_window & 0xFFFu;
    const int bottom = ((shader_window >> 12) & 0xFu) == 1u;
    const int integer = ((shader_window >> 16) & 0xFu) == 1u;
    if (!bottom && !integer && !inv_scale) return 0;
    char line[320];
    if (!inv_scale) {
        char define[160];
        snprintf(define, sizeof(define),
                 "#define _rsx_wpos float4(input.position.x - %s, %s, input.position.zw)\n",
                 integer ? "0.5" : "0.0",
                 bottom ? (integer ? "%u.0 - input.position.y - 0.5" : "%u.0 - input.position.y")
                        : (integer ? "input.position.y - 0.5" : "input.position.y"));
        snprintf(line, sizeof(line), define, height);
    } else {
        char x[96], y[128];
        /* LD_WPOS_SNAP=1: every host pixel reads its guest pixel's own
         * coordinate (floor(pos * k) + 0.5, or the integer for integer
         * centres), exactly what the shader saw at 1x, instead of a position
         * inside that guest pixel. */
        static int snap = -1;
        if (snap < 0) { const char* e = getenv("LD_WPOS_SNAP"); snap = e && e[0] == '1'; }
        if (snap) {
            snprintf(x, sizeof x, integer ? "floor(input.position.x * %s)" : "(floor(input.position.x * %s) + 0.5)",
                     inv_scale);
            if (bottom)
                snprintf(y, sizeof y, integer ? "%u.0 - 1.0 - floor(input.position.y * %s)"
                                              : "%u.0 - (floor(input.position.y * %s) + 0.5)", height, inv_scale);
            else
                snprintf(y, sizeof y, integer ? "floor(input.position.y * %s)" : "(floor(input.position.y * %s) + 0.5)",
                         inv_scale);
        } else {
            snprintf(x, sizeof x, integer ? "(input.position.x - 0.5) * %s" : "input.position.x * %s", inv_scale);
            if (bottom)
                snprintf(y, sizeof y, integer ? "%u.0 - (input.position.y + 0.5) * %s" : "%u.0 - input.position.y * %s",
                         height, inv_scale);
            else
                snprintf(y, sizeof y, integer ? "(input.position.y - 0.5) * %s" : "input.position.y * %s", inv_scale);
        }
        snprintf(line, sizeof(line), "#define _rsx_wpos float4(%s, %s, input.position.zw)\n", x, y);
    }
    /* count and grow: each read gets shorter, the define is added once */
    u32 n = 0;
    for (char* p = strstr(hlsl, from); p; p = strstr(p + 1, from)) n++;
    if (!n) return 0;
    const size_t len = strlen(hlsl), dl = strlen(line);
    if (len + dl + 1 > out_size) return -1;
    char* w = hlsl;
    for (const char* r = hlsl; *r;) {
        if (!strncmp(r, from, sizeof(from) - 1)) {
            memcpy(w, to, sizeof(to) - 1); w += sizeof(to) - 1; r += sizeof(from) - 1;
        } else *w++ = *r++;
    }
    *w = '\0';
    memmove(hlsl + dl, hlsl, strlen(hlsl) + 1);
    memcpy(hlsl, line, dl);
    return (int)n;
}

u64 rsx_fp_literal_source_hash(
    const u8* ucode, u32 max_bytes, u64 seed)
{
    const u64 structural =
        rsx_fp_structural_hash(ucode, max_bytes, seed);
    if (!structural)
        return 0;
    rsx_fp_constant_block constants;
    if (rsx_fp_collect_constants(ucode, max_bytes, &constants) < 0)
        return 0;
    u64 hash = structural;
    for (u32 slot = 0; slot < constants.count; ++slot) {
        float value[4];
        for (u32 component = 0; component < 4; ++component)
            memcpy(
                &value[component],
                &constants.values[slot][component],
                sizeof(value[component]));
        char literal[128];
        const int length = snprintf(
            literal, sizeof(literal), "float4(%g,%g,%g,%g)",
            value[0], value[1], value[2], value[3]);
        if (length < 0 || (u32)length >= sizeof(literal))
            return 0;
        hash = fp_hash_bytes(literal, (u32)length, hash);
    }
    return hash;
}

int rsx_fp_constant_ring_plan(
    u32 used, u32 capacity, u32 data_bytes,
    u32* out_offset, u32* out_allocation_bytes)
{
    if (!out_offset || !out_allocation_bytes || data_bytes == 0)
        return -1;
    if (data_bytes > 0xFFFFFF00u)
        return -1;
    const u32 allocation = (data_bytes + 255u) & ~255u;
    if (allocation > capacity)
        return -1;
    if (used > capacity || allocation > capacity - used)
        return 0;
    *out_offset = used;
    *out_allocation_bytes = allocation;
    return 1;
}

int rsx_vertex_constant_ring_plan(
    u32 used, u32 capacity, u32 block_bytes, u32* out_offset)
{
    if (!out_offset || block_bytes == 0 || (block_bytes & 255u) != 0 ||
        block_bytes > capacity)
        return -1;
    if (used > capacity || block_bytes > capacity - used)
        return 0;
    *out_offset = used;
    return 1;
}

/* Bounded string appender. */
typedef struct { char* p; u32 cap; u32 len; int ok; } Out;

static void out_puts(Out* o, const char* s)
{
    if (!o->ok) return;
    u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) { o->ok = 0; return; }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = '\0';
}

/* Decoded view of one source word. */
typedef struct {
    u32 type;       /* FP_REG_TYPE_* */
    u32 index;
    int half;
    int negate;
    int abs;
    char swz[5];    /* e.g. "xyzw" */
} Src;

static void decode_src(u32 w, int abs_bit, Src* s)
{
    static const char comp[4] = {'x','y','z','w'};
    s->type   = (w & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT;
    s->index  = (w & FP_SRC_REG_MASK)  >> FP_SRC_REG_SHIFT;
    s->half   = (w & FP_SRC_HALF) ? 1 : 0;
    s->negate = (w & FP_SRC_NEGATE) ? 1 : 0;
    s->abs    = abs_bit ? 1 : 0;
    for (int i = 0; i < 4; i++)
        s->swz[i] = comp[(w >> (FP_SRC_SWZ_SHIFT + 2 * i)) & 3];
    s->swz[4] = '\0';
}

/* HLSL expression for the interpolated input selected by OPDEST.INPUT_SRC.
 * The backend's passthrough VS forwards COL0/COL1/FOG/TEXCOORD0..7; WPOS maps
 * to SV_POSITION (screen-space — an approximation of the RSX window coord). */
static const char* input_expr(u32 input_src)
{
    static const char* tc[8] = {
        "input.tc0", "input.tc1", "input.tc2", "input.tc3",
        "input.tc4", "input.tc5", "input.tc6", "input.tc7" };
    switch (input_src) {
    case 0x0: return "input.position"; /* WPOS */
    /* RSX clamps fragment color-input reads before swizzle/abs/negate.
     * Texture coordinates and the vertex output registers remain unbounded. */
    case 0x1: return "saturate(input.col0)"; /* COL0 */
    case 0x2: return "saturate(input.col1)"; /* COL1 */
    case 0x3: return "input.fog";      /* FOGC */
    default:
        if (input_src >= 0x4 && input_src <= 0xB) return tc[input_src - 0x4]; /* TC0..7 */
        if (input_src == 0xC) return "input.tc8";
        if (input_src == 0xD) return "input.tc9";
        return "float4(0,0,0,0)";      /* FACING/SSA not plumbed */
    }
}

/* SRC2 bit 31 of the instruction being emitted: its texture-coordinate input
 * is read multiplied by 1/w (RPCS3 FragmentProgramDecompiler, "perspective
 * correct flag", PR #10764, the mode credited with missing effects in God of
 * War III / Ascension). gl_FragCoord.w there is 1/w; SV_Position.w is w. */
static int s_fp_perspective_corr;

/* LD_FP_PREC (default on): the NV40 precision modifiers. Each destination
 * (OPDEST bits 22-23) and each source (SRC1 word bits 19-21, 22-24, 25-27 for
 * sources 0, 1, 2) may name a precision the value is clamped to: 1 = FP16
 * (+-65504), 2 = FX12 (-2..2), 3 = FX9 (-1..1), 4 (sources) = saturate. Cg
 * emits them for half and fixed variables. The translation ignored them, so a
 * value the RSX holds at 2 (a lighting term in fixed precision, say) grew
 * unbounded and a lit surface could burn to white. Rules as RPCS3's
 * FragmentProgramDecompiler (SetDst, GetSRC, ClampValue). LD_FP_PREC=0: none,
 * as before. LD_FP_SRC2ABS (default on): the third source's abs bit (SRC2
 * bit 18) was never read; =0: ignored, as before. */
volatile long long g_rsx_fp_prec_clamps;   /* clamps emitted, for the [draws] line */
static int fp_prec_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_FP_PREC"); on = !(e && e[0] == '0'); }
    return on;
}
static int fp_src2abs_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_FP_SRC2ABS"); on = !(e && e[0] == '0'); }
    return on;
}
/* LD_FP_FACE (default on): input register 0xE is SSA, the facing sign, +1 on
 * a front face and -1 on a back face (RPCS3 FragmentProgramDecompiler, "ssa";
 * VKFragmentProgram: gl_FrontFacing ? 1 : -1). It was read as 0. Two-sided
 * G-buffer materials (203 of the game's cached programs) build the normal as
 * sign * T + B..., so the sign term vanished and the lighting with it. Read
 * from SV_IsFrontFace, which follows the FrontCounterClockwise the live draw
 * takes from the guest front face, so front and back are the guest's.
 * LD_FP_FACE=0: 0, as before.
 * LD_FP_NRM_SAFE (default on): NRM of a zero vector returns the vector instead
 * of NaN (RPCS3 _builtin_normalize, length(x) > 0 ? normalize(x) : x).
 * LD_FP_NRM_SAFE=0: plain normalize, as before. */
static int fp_face_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_FP_FACE"); on = !(e && e[0] == '0'); }
    return on;
}
static int fp_nrm_safe_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_FP_NRM_SAFE"); on = !(e && e[0] == '0'); }
    return on;
}
/* LD_FP_DP3_PRECISE (default on): a DP3 into a full-precision destination
 * rounds as the RSX does, x.x*y.x + (x.y*y.y + x.z*y.z) by fused
 * multiply-adds, not as dot() (RPCS3 PR #19293, hardware-tested: God of
 * War III / Ascension's sky and fog took wrong colours, the Tower of Delphi's
 * sky orange instead of blue). =0: dot(), as before. */
static int fp_dp3_precise_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_FP_DP3_PRECISE"); on = !(e && e[0] == '0'); }
    return on;
}
/* LD_FP_BRANCH_ANY (default on): IFE/LOOP/REP/BRK test the condition on every
 * component of the condition swizzle and branch if any passes (RPCS3 GetCond:
 * any(cc.swizzle OP 0)); only the first component was tested. Same text as
 * before for a broadcast swizzle (.xxxx), where the two agree. =0: the first
 * component, as before.
 * LD_FP_CC_PRED (default on): an instruction with no destination that only
 * writes the condition register writes it under its own execution condition,
 * per component, as a destination write would be; it was written always.
 * =0: always, as before. */
static int fp_branch_any_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_FP_BRANCH_ANY"); on = !(e && e[0] == '0'); }
    return on;
}
static int fp_cc_pred_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_FP_CC_PRED"); on = !(e && e[0] == '0'); }
    return on;
}

/* Limits of precision code 1-4, or 0 when it clamps nothing. */
static int fp_prec_limits(u32 prec, const char** lo, const char** hi)
{
    switch (prec) {
    case 1: *lo = "-65504.0"; *hi = "65504.0"; return 1;
    case 2: *lo = "-2.0"; *hi = "2.0"; return 1;
    case 3: *lo = "-1.0"; *hi = "1.0"; return 1;
    case 4: *lo = "0.0"; *hi = "1.0"; return 1;
    default: return 0;
    }
}

/* Build the swizzled/negated/abs'd HLSL for one source into `buf`; `prec` is
 * its precision modifier (LD_FP_PREC, 0 = none). */
static void emit_src(const Src* s, u32 input_src, const float* k, int has_k,
                     int buffered, u32 constant_slot, u32 prec, char* buf, u32 bufsz)
{
    char base[96];
    if (s->type == FP_REG_TYPE_TEMP) {
        snprintf(base, sizeof(base), "%s[%u]", s->half ? "h" : "r", s->index);
    } else if (s->type == FP_REG_TYPE_INPUT) {
        if (s_fp_perspective_corr && input_src >= 0x4 && input_src <= 0xD)
            snprintf(base, sizeof(base), "(%s / input.position.w)", input_expr(input_src));
        else if (input_src == 0xE && fp_face_on())
            snprintf(base, sizeof(base), "((float4)(_rsx_face ? 1.0 : -1.0))");
        else
            snprintf(base, sizeof(base), "%s", input_expr(input_src));
    } else { /* CONST */
        if (buffered && has_k)
            snprintf(
                base, sizeof(base), "fp_constants[%u]", constant_slot);
        else if (has_k)
            snprintf(base, sizeof(base), "float4(%g,%g,%g,%g)",
                     k[0], k[1], k[2], k[3]);
        else
            snprintf(base, sizeof(base), "float4(0,0,0,0)");
    }

    char swz[160];
    snprintf(swz, sizeof(swz), "(%s).%s", base, s->swz);

    /* LD_FP_PREC: saturate clamps before abs, the others after it; negate
     * comes last (RPCS3 GetSRC: tested on Naruto UNS). */
    const char* lo = NULL;
    const char* hi = NULL;
    if (prec && !fp_prec_limits(prec, &lo, &hi)) prec = 0;
    char inner[256];
    if (prec == 4) {
        snprintf(inner, sizeof(inner), "clamp(%s, %s, %s)", swz, lo, hi);
        g_rsx_fp_prec_clamps++;
    } else
        snprintf(inner, sizeof(inner), "%s", swz);
    char mag[300];
    if (s->abs) snprintf(mag, sizeof(mag), "abs(%s)", inner);
    else        snprintf(mag, sizeof(mag), "%s", inner);
    char outer[360];
    if (prec && prec != 4) {
        snprintf(outer, sizeof(outer), "clamp(%s, %s, %s)", mag, lo, hi);
        g_rsx_fp_prec_clamps++;
    } else
        snprintf(outer, sizeof(outer), "%s", mag);
    if (s->negate) snprintf(buf, bufsz, "-(%s)", outer);
    else           snprintf(buf, bufsz, "%s", outer);
}

/* LD_FP_PREC: the precision modifier source `idx` of an instruction reads
 * with (0 = none), with RPCS3's exceptions: a half temp at FP16, the colour
 * and fog inputs, and type-3 sources are read as they are. */
static u32 fp_src_prec(const Src* s, u32 w2, u32 idx, u32 input_src, int is_branch)
{
    if (!fp_prec_on() || is_branch) return 0;
    const u32 prec = (w2 >> (19u + 3u * idx)) & 7u;
    if (prec == 0 || prec > 4) return 0;
    if (s->type == FP_REG_TYPE_TEMP && s->half && prec == 1) return 0;
    if (s->type == FP_REG_TYPE_INPUT && (input_src >= 1 && input_src <= 3)) return 0;
    if (s->type == 3) return 0;
    return prec;
}

/* Dest write-mask letters from OPDEST bits 9..12 (X..W). */
static void dest_mask(u32 op0, char* m)
{
    int n = 0;
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 0))) m[n++] = 'x';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 1))) m[n++] = 'y';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 2))) m[n++] = 'z';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 3))) m[n++] = 'w';
    if (n == 0) { m[0] = 'x'; m[1] = 'y'; m[2] = 'z'; m[3] = 'w'; n = 4; }
    m[n] = '\0';
}

/* CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS: when set, fragment output is fp32
 * (color = r0); when clear, output is fp16 (color = h0). Matches RPCS3
 * FragmentProgramDecompiler::BuildCode (gcm_enums.h). */
#define FP_CTRL_32BIT_EXPORTS 0x40u

/* The four colour exports live at fixed register indices, one set per export
 * width: r0/r2/r3/r4 with 32-bit exports and h0/h4/h6/h8 with 16-bit ones
 * (rsx_commands.h, SHADER_CONTROL 0x40; RPCS3 FragmentProgramDecompiler's
 * BuildCode table). Everything else in the file is a plain temp. */
#define FP_MAX_COLOR_TARGETS 4
static const u32 fp_export_reg[2][FP_MAX_COLOR_TARGETS] = {
    { 0, 2, 3, 4 },   /* 32-bit exports: r */
    { 0, 4, 6, 8 },   /* 16-bit exports: h */
};

/* LD_FP_EXPORT_GATHER (default on). The half registers alias the full ones:
 * h(2k) is the low half of r(k) (its x and y), h(2k+1) the high half, each
 * full lane holding two halves (RPCS3 RegisterDependencyPass: r.x =
 * packHalf2x16(h.xy)). With 16-bit exports the ROP reads h0/h4/h6/h8; a lane
 * of one that the program last wrote through the full register (r0/r2/r3/r4)
 * reads those bits as two halves. God of War: Ascension's G-buffer programs
 * do that on their second to fourth targets (21 of the first level's 81
 * fragment programs; 12 of them a whole target), and the translation, which
 * keeps the two files apart, exported the stale half register instead. Such
 * lanes now take the unpacked bits. =0: the half register, as before. */
static int fp_export_gather_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_FP_EXPORT_GATHER"); on = !(e && e[0] == '0'); }
    return on;
}
/* Lines setting each lane of export variable `var` (h[reg] under 16-bit
 * exports) that `lw` says was last written through the full register. */
static void fp_export_gather(Out* o, const char* var, u32 reg, const u8* lw, u32 lw_size)
{
    static const char comp[4] = { 'x', 'y', 'z', 'w' };
    for (u32 c = 0; c < 4; c++) {
        if (reg * 4u + c >= lw_size || lw[reg * 4u + c] != 'R') continue;
        char line[128];
        snprintf(line, sizeof line, "    %s.%c = f16tof32((asuint(r[%u].%c) >> %uu) & 0xFFFFu);\n",
                 var, comp[c], reg >> 1, comp[((reg & 1u) << 1) + (c >> 1)], (c & 1u) * 16u);
        out_puts(o, line);
    }
}

/* LD_FP_ALIAS_SYNC (default off; =1 on): the same aliasing inside a program. A read
 * of r(k) whose 16-bit halves were last written through h(2k)/h(2k+1) gets
 * the packed halves, and a read of h(n) last written through r(n/2) gets the
 * unpacked bits, as RPCS3's RegisterDependencyPass injects (packHalf2x16 /
 * unpackHalf2x16, OR16 for a lane written half each way). The register being
 * read is brought up to date just before the instruction, so later reads see
 * it too. Branch-free programs only, and only lanes whose last write is
 * known (a predicated write leaves it unknown). Off, the files stay apart.
 * Off by default: on, Kratos's hands and boots came out bright yellow (user
 * report on the user2 build; scripted A/B 2026-10-23: LD_FP_ALIAS_SYNC=0
 * alone gave normal hands and boots, LD_FP_EXPORT_GATHER=0 alone made it
 * worse). The packing it injects is wrong for some lane or program not yet
 * found. */
static int fp_alias_sync_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_FP_ALIAS_SYNC"); on = e && e[0] == '1'; }
    return on;
}
/* Bring temp register (index, half) up to date from the other file for every
 * lane whose last write went through it; mark those lanes 'B' (both agree).
 * lw: 'R' full, 'H' half, 'B' both, 'U' unknown, 0 never written. */
static void fp_alias_sync(Out* o, u32 index, int half, u8* lw, u32 lw_size)
{
    static const char comp[4] = { 'x', 'y', 'z', 'w' };
    char line[256];
    for (u32 c = 0; c < 4; c++) {
        if (half) {
            const u32 u = index * 4u + c;
            if (u >= lw_size || lw[u] != 'R') continue;
            snprintf(line, sizeof line, "    h[%u].%c = f16tof32((asuint(r[%u].%c) >> %uu) & 0xFFFFu);\n",
                     index, comp[c], index >> 1, comp[((index & 1u) << 1) + (c >> 1)], (c & 1u) * 16u);
            out_puts(o, line);
            lw[u] = 'B';
        } else {
            const u32 u0 = index * 8u + 2u * c, u1 = u0 + 1u;
            if (u1 >= lw_size || (lw[u0] != 'H' && lw[u1] != 'H')) continue;
            if (lw[u0] == 'U' || lw[u1] == 'U') continue;
            const u32 n = 2u * index + (c >> 1);           /* the half register */
            const char lo = comp[(c & 1u) * 2u], hi = comp[(c & 1u) * 2u + 1u];
            char lo_txt[96], hi_txt[96];
            if (lw[u0] == 'H') snprintf(lo_txt, sizeof lo_txt, "f32tof16(h[%u].%c)", n, lo);
            else snprintf(lo_txt, sizeof lo_txt, "(asuint(r[%u].%c) & 0xFFFFu)", index, comp[c]);
            if (lw[u1] == 'H') snprintf(hi_txt, sizeof hi_txt, "(f32tof16(h[%u].%c) << 16)", n, hi);
            else snprintf(hi_txt, sizeof hi_txt, "(asuint(r[%u].%c) & 0xFFFF0000u)", index, comp[c]);
            snprintf(line, sizeof line, "    r[%u].%c = asfloat(%s | %s);\n", index, comp[c], lo_txt, hi_txt);
            out_puts(o, line);
            lw[u0] = lw[u1] = 'B';
        }
    }
}

/* Which colour target a destination register is, or -1 for a plain temp. */
static int fp_export_target(u32 index, int half)
{
    for (int t = 0; t < FP_MAX_COLOR_TARGETS; t++)
        if (fp_export_reg[half][t] == index) return t;
    return -1;
}

int rsx_fp_decompile(const u8* ucode, u32 max_bytes, u32 ctrl, char* out, u32 out_size)
{
    return rsx_fp_decompile_ex(ucode, max_bytes, ctrl, 0u, out, out_size);
}

void rsx_fp_shadow_desc_from_texture(rsx_fp_shadow_desc* out, u32 format,
    u32 dimension, u32 cube, u32 mips, u32 filter, u32 address, u32 control0)
{
    memset(out, 0, sizeof *out);
    const u32 base = format & 0x9Fu, func = (address >> 28) & 15u;
    if (!(control0 & 0x80000000u) || (control0 & 4u) ||
        (base != 0x90 && base != 0x91 && base != 0x92 && base != 0x93) ||
        func == 0 || func == 7) return;
    const u32 minf = (filter >> 16) & 7u, magf = (filter >> 24) & 7u;
    const int minlin = minf == 2 || minf == 4 || minf == 6;
    const int maglin = magf == 2;
    out->enabled = (base == 0x91 || base == 0x93 || func > 7 || dimension != 2 || cube || mips > 1 ||
        minf < 1 || minf > 6 || magf < 1 || magf > 2 || minlin != maglin) ? 2 : 1;
    out->func = (u8)func;
    out->linear = (u8)minlin;
    out->unnormalized = (u8)((format & 0x40u) != 0);
}

/* Units a texture fetch with EXP_TEX (OPDEST bit 21) reads. */
u32 rsx_fp_texture_exp_units(const u8* ucode, u32 max_bytes)
{
    u32 units = 0;
    if (!ucode) return 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u;
        if (!(w2 & FP_BRANCH) && (w0 & (1u << 21)) && (op == OP_TEX || op == OP_TXP ||
            op == OP_TXB || op == OP_TXL || op == OP_TXD || op == OP_TEXBEM || op == OP_TXPBEM))
            units |= 1u << ((w0 >> FP_TEX_UNIT_SHIFT) & 15u);
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2))
            off += 16;
        if (w0 & FP_END) break;
    }
    return units;
}

u32 rsx_fp_texture_units(const u8* ucode, u32 max_bytes)
{
    u32 units = 0;
    if (!ucode) return 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u;
        if (!(w2 & FP_BRANCH) && (op == OP_TEX || op == OP_TXP ||
            op == OP_TXB || op == OP_TXL || op == OP_TXD ||
            op == OP_TEXBEM || op == OP_TXPBEM))
            units |= 1u << ((w0 >> FP_TEX_UNIT_SHIFT) & 15u);
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2))
            off += 16;
        if (w0 & FP_END) break;
    }
    return units;
}

static void fp_shadow_helper(Out* p, u32 unit, u32 cube_mask,
                              const rsx_fp_shadow_desc* d)
{
    char tex[32], text[4096];
    snprintf(tex, sizeof tex, cube_mask ? "rsx_tex%u" : "rsx_tex[%u]", unit);
    const char* cmp[] = { "0.0", "reference < depth", "reference == depth",
        "reference <= depth", "reference > depth", "reference != depth",
        "reference >= depth", "1.0" };
    snprintf(text, sizeof text,
        "float _rsx_shadow_cmp%u(float reference, float depth) { return (float)(%s); }\n"
        "float4 _rsx_shadow%u(float3 coordinate) {\n"
        "    float reference = saturate(coordinate.z);\n",
        unit, cmp[d->func], unit);
    out_puts(p, text);
    if (d->linear || d->unnormalized) {
        snprintf(text, sizeof text, "    uint width, height; %s.GetDimensions(width, height);\n"
            "    float2 size = float2(width, height);\n", tex);
        out_puts(p, text);
    }
    out_puts(p, d->unnormalized ? "    float2 uv = coordinate.xy / size;\n" :
                                     "    float2 uv = coordinate.xy;\n");
    if (!d->linear) {
        snprintf(text, sizeof text,
            "    float depth = %s.SampleLevel(rsx_samp[%u], uv, 0.0).r;\n"
            "    return (float4)_rsx_shadow_cmp%u(reference, depth);\n}\n",
            tex, unit, unit);
        out_puts(p, text);
        return;
    }
    out_puts(p, "    float2 position = uv * size - 0.5;\n"
        "    float2 base = floor(position); float2 fraction = position - base;\n");
    for (u32 tap = 0; tap < 4; tap++) {
        snprintf(text, sizeof text,
            "    float d%u = %s.SampleLevel(rsx_samp[%u], (base + float2(%s, %s)) / size, 0.0).r;\n"
            "    float c%u = _rsx_shadow_cmp%u(reference, d%u);\n",
            tap, tex, unit, (tap & 1) ? "1.5" : "0.5", (tap & 2) ? "1.5" : "0.5", tap, unit, tap);
        out_puts(p, text);
    }
    out_puts(p, "    return (float4)lerp(lerp(c0, c1, fraction.x), lerp(c2, c3, fraction.x), fraction.y);\n}\n");
}

/* EXP_TEX overrides gamma on compatible normalized color formats. Metadata
 * has already excluded formats without gamma support. */
u32 rsx_fp_texture_gamma_units(const u8* ucode, u32 max_bytes,
                                const u8 gamma_masks[16])
{
    if (!ucode || !gamma_masks) return 0;
    u32 units = 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u, unit = (w0 >> FP_TEX_UNIT_SHIFT) & 15u;
        if (!(w2 & FP_BRANCH) && !(w0 & (1u << 21)) && gamma_masks[unit] &&
            (op == OP_TEX || op == OP_TXP || op == OP_TXB || op == OP_TXL ||
             op == OP_TXD || op == OP_TEXBEM || op == OP_TXPBEM))
            units |= 1u << unit;
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2)) off += 16;
        if (w0 & FP_END) return units;
    }
    return 0;
}

static void fp_gamma_helper(Out* out)
{
    out_puts(out,
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
        "                  (mask & 8u) ? decoded.a : v.a);\n}\n");
}

static u32 s_fp_volume_mask;
void rsx_fp_set_volume_mask(u32 mask) { s_fp_volume_mask = mask; }
static u32 s_fp_expand_mask;
void rsx_fp_set_expand_mask(u32 mask) { s_fp_expand_mask = mask; }
/* LD_FP_EXP_FLOAT: units whose format takes no expansion (the float formats
 * and HILO_S8: RPCS3 get_format_features), set per draw by the live draw only
 * for units the program expands, so every other program is unchanged. */
static u32 s_fp_noexpand_mask;
void rsx_fp_set_noexpand_mask(u32 mask) { s_fp_noexpand_mask = mask; }

/* A sample expanded from [0,1] to [-1,1], as the RSX returns it: either the
 * unit asks for it (biased remap, s_fp_expand_mask) or the instruction does.
 * Bit 21 of a texture instruction's OPDEST word is EXP_TEX (RPCS3's
 * dst.exp_tex): the fetch itself is scaled by 2 and biased by -1. God of War:
 * Ascension's motion blur reads its velocity buffer (stored as v*0.5+0.5)
 * that way; without it "at rest" read as 0.5, full speed, and the blur weight
 * was 1 on every pixel. LD_EXP_TEX=0 ignores the instruction bit. */
static void fp_expand_sample(char* rhs, size_t cap, u32 unit, u32 exp_tex)
{
    static int s_exp_on = -1;
    if (s_exp_on < 0) { const char* e = getenv("LD_EXP_TEX"); s_exp_on = !(e && e[0] == '0'); }
    if (!s_exp_on) exp_tex = 0;
    if ((s_fp_noexpand_mask >> unit) & 1u) return;
    if ((!exp_tex && !((s_fp_expand_mask >> unit) & 1u)) || !rhs[0]) return;
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "(%s * 2.0 - 1.0)", rhs);
    snprintf(rhs, cap, "%s", tmp);
}

static int rsx_fp_decompile_internal(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    int buffered, const rsx_fp_shadow_desc* shadow, const u8 gamma_masks[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    if (!ucode || !out || out_size == 0) return -1;
    /* Volume units sample like cube units (three coordinates, declared one
     * by one); per_unit says whether the bank is declared per unit at all. */
    const u32 vol_mask = s_fp_volume_mask & ~tex_cube_mask;
    const u32 xyz_mask = tex_cube_mask | vol_mask;
    const u32 per_unit = xyz_mask;
    const u32 gamma_units = rsx_fp_texture_gamma_units(ucode, max_bytes, gamma_masks);

    const u32 sampled_units = rsx_fp_texture_units(ucode, max_bytes);
    if (shadow) for (u32 u = 0; u < 16; u++)
        if ((sampled_units & (1u << u)) && shadow[u].enabled &&
            (shadow[u].enabled != 1 || shadow[u].func > 7 ||
             shadow[u].linear > 1 || shadow[u].unnormalized > 1 ||
             (tex_cube_mask & (1u << u)))) {
            static u32 n_layout_errors;
            if (n_layout_errors++ < 32)
                fprintf(stderr, "[RSX fp] unsupported depth comparison layout on texture unit %u\n", u);
            return -1;
        }
    u32 buffered_constant_count = 0;
    if (buffered) {
        rsx_fp_constant_block constants;
        const int result =
            rsx_fp_collect_constants(ucode, max_bytes, &constants);
        if (result < 0)
            return -1;
        buffered_constant_count = constants.count;
    }
    if (out_constant_count)
        *out_constant_count = buffered_constant_count;

    /* The BODY is emitted first and the preamble is inserted in front of it
     * afterwards. main's signature names the return type, and the return type
     * is a struct as soon as the program turns out to write a second colour
     * export -- which is only known once every instruction has been decoded.
     * Emitting the preamble here would mean guessing it. */
    Out o = { out, out_size, 0, 1 };
    out[0] = '\0';
    int uses_pack = 0;   /* a pack or unpack opcode was decoded: the preamble gets its helpers */
    int uses_nrm = 0;    /* LD_FP_NRM_SAFE: an NRM was decoded, the preamble gets _rsx_nrm */
    int uses_dp3p = 0;   /* LD_FP_DP3_PRECISE: a full-precision DP3, the preamble gets _rsx_dp3p */
    out_puts(&o,
        "    float4 r[48]; float4 h[48];\n"
        /* Fully initialise both register files: RSX programs routinely read a
         * register lane before writing it (the hardware reads undefined), but
         * HLSL rejects use-before-init (error X4000). Zero everything. */
        "    [unroll] for (int _i = 0; _i < 48; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }\n"
        /* Condition-code registers for exec_if predication / set_cond. Reset
         * to 0 (NV40 CC power-up state). Dead in non-predicated programs. */
        "    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n");

    /* Which colour export each register file received a write to, indexed
     * [half][target]. The whole file is tracked, not just target 0, because
     * the highest one written decides how many SV_TARGETs come out. */
    int wrote[2][FP_MAX_COLOR_TARGETS];
    memset(wrote, 0, sizeof wrote);
    /* LD_FP_EXPORT_GATHER: the last writer of each 16-bit unit of the
     * register file ('R' full, 'H' half): r[k].c is units k*8 + 2c and +1,
     * h[n].c is unit n*4 + c. */
    u8 last_w[64 * 8];
    memset(last_w, 0, sizeof last_w);
    int any_branch = 0;   /* flow control: the last writer is not static, no gather */
    /* LD_FP_ALIAS_SYNC: decided up front (a branch later would make the
     * syncs emitted before it path-dependent). */
    int alias_sync = fp_alias_sync_on() && ctrl != RSX_FP_CTRL_AUTO;
    for (u32 so = 0; alias_sync && so + 16 <= max_bytes;) {
        const u32 sw0 = rsx_fp_read_word(ucode + so), sw1 = rsx_fp_read_word(ucode + so + 4);
        const u32 sw2 = rsx_fp_read_word(ucode + so + 8), sw3 = rsx_fp_read_word(ucode + so + 12);
        if (sw2 & FP_BRANCH) alias_sync = 0;
        so += 16;
        if (!(sw2 & FP_BRANCH) && fp_instruction_has_constant(sw1, sw2, sw3)) so += 16;
        if (sw0 & FP_END) break;
    }
    int count = 0;
    u32 off = 0;
    u32 constant_slot = 0;

    /* Open IFE/LOOP/REP blocks: the byte offset each one's else branch starts
     * at (0 = none) and where it ends. See the branch case below. */
    enum { FP_MAX_BLOCKS = 32 };
    u32 blk_else[FP_MAX_BLOCKS], blk_end[FP_MAX_BLOCKS];
    int blk_loop[FP_MAX_BLOCKS];
    int n_blk = 0, n_loop = 0, loops_open = 0;

    while (off + 16 <= max_bytes) {
        /* Close blocks ending here, innermost first, then open an else. Ends
         * before elses, as RPCS3's FragmentProgramDecompiler::Decompile does. */
        while (n_blk > 0 && blk_end[n_blk - 1] == off) {
            out_puts(&o, "    }\n");
            loops_open -= blk_loop[--n_blk];
        }
        if (n_blk > 0 && blk_else[n_blk - 1] == off) {
            out_puts(&o, "    } else {\n");
            blk_else[n_blk - 1] = 0;
        }
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        count++;

        u32 opcode    = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
        u32 input_src = (w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
        u32 tex_unit  = (w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT;
        if (!(w2 & FP_BRANCH) && shadow && shadow[tex_unit].enabled &&
            (opcode == OP_TXB || opcode == OP_TXL || opcode == OP_TXD ||
             opcode == OP_TEXBEM || opcode == OP_TXPBEM)) {
            static u32 n_fetch_errors;
            if (n_fetch_errors++ < 32)
                fprintf(stderr, "[RSX fp] unsupported depth comparison fetch on texture unit %u\n", tex_unit);
            return -1;
        }
        int is_branch = (w2 & FP_BRANCH) ? 1 : 0;

        /* Execution condition (exec_if) + condition-code write (set_cond).
         * Faithful to RPCS3 FragmentProgramDecompiler {GetRawCond, AddCodeCond,
         * SetDst}: all-set == unconditional, none-set == never, otherwise a
         * per-component select of the result vs the destination, gated by a
         * sign test of the cond-swizzled CC register. */
        int exec_lt   = (w1 & FP_EXEC_IF_LT) ? 1 : 0;
        int exec_eq   = (w1 & FP_EXEC_IF_EQ) ? 1 : 0;
        int exec_gr   = (w1 & FP_EXEC_IF_GR) ? 1 : 0;
        int is_uncond = (exec_lt && exec_eq && exec_gr);
        int is_never  = (!exec_lt && !exec_eq && !exec_gr);
        int set_cond  = (w0 & FP_COND_WRITE) ? 1 : 0;
        u32 cc_read   = (w1 & FP_COND_REG)     ? 1u : 0u;
        u32 cc_write  = (w1 & FP_COND_MOD_REG) ? 1u : 0u;
        static const char comp_c[4] = {'x','y','z','w'};
        char cswz[5];
        for (int ci = 0; ci < 4; ci++)
            cswz[ci] = comp_c[(w1 >> (FP_COND_SWZ_SHIFT + 2 * ci)) & 3];
        cswz[4] = '\0';
        /* HLSL comparison operator for the exec_if sign mask (matches the
         * oracle's SGE/SLE/SNE/SGT/SLT/SEQ selection in GetRawCond). */
        const char* cmp_op = "==";
        if      (exec_gr && exec_eq) cmp_op = ">=";
        else if (exec_lt && exec_eq) cmp_op = "<=";
        else if (exec_gr && exec_lt) cmp_op = "!=";
        else if (exec_gr)            cmp_op = ">";
        else if (exec_lt)            cmp_op = "<";
        else                         cmp_op = "==";  /* exec_eq only */

        Src s0, s1, s2;
        decode_src(w1, w1 & FP_SRC0_ABS, &s0);
        decode_src(w2, w2 & FP_SRC1_ABS, &s1);
        decode_src(w3, fp_src2abs_on() && !(w2 & FP_BRANCH) ? (w3 & FP_SRC2_ABS) : 0, &s2);

        /* Inline constant: any CONST source pulls the next 16 bytes as a
         * float4 literal and advances past it. */
        float k[4] = {0,0,0,0};
        int has_k = 0;
        if (s0.type == FP_REG_TYPE_CONST || s1.type == FP_REG_TYPE_CONST ||
            s2.type == FP_REG_TYPE_CONST) {
            if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + i * 4));
                    memcpy(&k[i], &cw, 4);
                }
                off += 16;
            }
            has_k = 1;
        }

        char a[200], b[200], c[200];
        { static int off_pc = -1;   /* LD_FP_NO_PERSP_CORR=1: ignore the flag (old behaviour) */
          if (off_pc < 0) { const char* e = getenv("LD_FP_NO_PERSP_CORR"); off_pc = e && e[0] == '1'; }
          s_fp_perspective_corr = !off_pc && !is_branch && (w3 & 0x80000000u) != 0; }
        emit_src(
            &s0, input_src, k, has_k, buffered, constant_slot,
            fp_src_prec(&s0, w2, 0, input_src, is_branch), a, sizeof(a));
        emit_src(
            &s1, input_src, k, has_k, buffered, constant_slot,
            fp_src_prec(&s1, w2, 1, input_src, is_branch), b, sizeof(b));
        emit_src(
            &s2, input_src, k, has_k, buffered, constant_slot,
            fp_src_prec(&s2, w2, 2, input_src, is_branch), c, sizeof(c));
        if (has_k)
            constant_slot++;
        if (alias_sync && !is_branch) {   /* LD_FP_ALIAS_SYNC: the temps this instruction reads */
            const Src* srcs[3] = { &s0, &s1, &s2 };
            for (u32 si = 0; si < 3; si++)
                if (srcs[si]->type == FP_REG_TYPE_TEMP &&
                    (si == 0 || srcs[si]->index != srcs[0]->index || srcs[si]->half != srcs[0]->half))
                    fp_alias_sync(&o, srcs[si]->index, srcs[si]->half, last_w, sizeof last_w);
        }

        /* Flow control: SRC1.branch set, and OPDEST's opcode field selects
         * BRK/CAL/IFE/LOOP/REP/RET (RPCS3 RSXFragmentProgram.h, 0x40..0x45).
         * IFE carries the else and end addresses in SRC1/SRC2 [0:30] as word
         * offsets; LOOP/REP carry end/init/increment counters in SRC1 [2:9],
         * [10:17], [19:26] and the end address in SRC2. The condition is the
         * exec_if test on the first component of the condition swizzle.
         * Skipping these ran both sides of every if/else: Crazy Taxi's uber
         * material shader then overwrote its lit colour with a COLOR0 the
         * vertex program never writes, and the whole city drew black. */
        if (is_branch) {
            any_branch = 1;   /* LD_FP_EXPORT_GATHER */
            char cond[64], line[160];
            if (is_uncond)
                snprintf(cond, sizeof cond, "true");
            else if (is_never)
                snprintf(cond, sizeof cond, "false");
            else if (fp_branch_any_on() &&
                     (cswz[1] != cswz[0] || cswz[2] != cswz[0] || cswz[3] != cswz[0]))
                snprintf(cond, sizeof cond, "any(cc%u.%s %s (float4)0.0)", cc_read, cswz, cmp_op);
            else
                snprintf(cond, sizeof cond, "cc%u.%c %s 0.0", cc_read, cswz[0], cmp_op);
            const u32 end_at = (w3 & 0x7FFFFFFFu) << 2;
            switch (opcode) {
            case 0x02: /* IFE */
            case 0x03: /* LOOP */
            case 0x04: /* REP */
                if (n_blk == FP_MAX_BLOCKS || end_at <= off) {
                    out_puts(&o, "    /* TODO: malformed flow-control op skipped */\n");
                    break;
                }
                if (opcode == 0x02) {
                    const u32 else_at = (w2 & 0x7FFFFFFFu) << 2;
                    blk_else[n_blk] = else_at != end_at ? else_at : 0;
                    blk_loop[n_blk] = 0;
                    snprintf(line, sizeof line, "    if (%s) {\n", cond);
                } else {
                    const u32 end_n = (w2 >> 2) & 0xFFu, init = (w2 >> 10) & 0xFFu;
                    const u32 inc = (w2 >> 19) & 0xFFu;
                    blk_else[n_blk] = 0;
                    blk_loop[n_blk] = 1;
                    snprintf(line, sizeof line,
                             "    if (%s) [loop] for (int _l%d = %u; _l%d < %u; _l%d += %u) {\n",
                             cond, n_loop, init, n_loop, end_n, n_loop, inc ? inc : 1u);
                    n_loop++;
                    loops_open++;
                }
                blk_end[n_blk++] = end_at;
                out_puts(&o, line);
                break;
            case 0x00: /* BRK */
                snprintf(line, sizeof line, "    if (%s) break;\n", cond);
                if (loops_open) out_puts(&o, line);
                break;
            default:   /* CAL / RET: no subroutine support yet */
                out_puts(&o, "    /* TODO: CAL/RET flow-control op skipped */\n");
                break;
            }
            if (w0 & FP_END) break;
            continue;
        }

        if (gamma_masks && gamma_masks[tex_unit] && !(w0 & (1u << 21)) &&
            (opcode == OP_TXD || opcode == OP_TEXBEM || opcode == OP_TXPBEM)) {
            static u32 reported;
            if (reported++ < 32)
                fprintf(stderr, "[RSX fp] gamma fetch opcode %u unsupported on unit %u\n", opcode, tex_unit);
            return -1;
        }

        /* Build the RHS expression for this opcode. */
        char rhs[640];
        int handled = 1;
        switch (opcode) {
        case OP_NOP: rhs[0] = '\0'; handled = 0; break;
        case OP_MOV: snprintf(rhs, sizeof(rhs), "%s", a); break;
        case OP_MUL: snprintf(rhs, sizeof(rhs), "(%s) * (%s)", a, b); break;
        case OP_ADD: snprintf(rhs, sizeof(rhs), "(%s) + (%s)", a, b); break;
        case OP_MAD: snprintf(rhs, sizeof(rhs), "(%s) * (%s) + (%s)", a, b, c); break;
        case OP_DP3:
            if (fp_dp3_precise_on() && ((w0 >> 22) & 3u) == 0) {   /* destination precision: full */
                uses_dp3p = 1;
                snprintf(rhs, sizeof(rhs), "_rsx_dp3p((%s).xyz, (%s).xyz)", a, b);
            } else
                snprintf(rhs, sizeof(rhs), "dot((%s).xyz, (%s).xyz)", a, b);
            break;
        case OP_DP4: snprintf(rhs, sizeof(rhs), "dot((%s), (%s))", a, b); break;
        case OP_MIN: snprintf(rhs, sizeof(rhs), "min((%s), (%s))", a, b); break;
        case OP_MAX: snprintf(rhs, sizeof(rhs), "max((%s), (%s))", a, b); break;
        case OP_FRC: snprintf(rhs, sizeof(rhs), "frac(%s)", a); break;
        case OP_FLR: snprintf(rhs, sizeof(rhs), "floor(%s)", a); break;
        /* Screen-space derivatives. God of War: Ascension's distance-field
         * font takes its edge width from max(|ddx|, |ddy|); left unhandled, the
         * width was 0 and every glyph drew bloated, its counters filled. */
        case OP_DDX: snprintf(rhs, sizeof(rhs), "ddx(%s)", a); break;
        case OP_DDY: snprintf(rhs, sizeof(rhs), "ddy(%s)", a); break;
        case OP_RCP: snprintf(rhs, sizeof(rhs), "(1.0 / (%s).x)", a); break;
        /* Pack/unpack (RPCS3 FragmentProgramDecompiler PK/UP cases and the pkg/upg
         * builtins): the packed word is a scalar that replicates. */
        case OP_PK4UB: uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk4ub(%s))", a); break;
        case OP_UP4UB: uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up4ub((%s).x)", a); break;
        case OP_PKG:   uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk4ub(_rsx_lin2srgb(%s)))", a); break;
        case OP_UPG:   uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_srgb2lin(_rsx_up4ub((%s).x))", a); break;
        case OP_PK4B:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk4b(%s))", a); break;
        case OP_UP4B:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up4b((%s).x)", a); break;
        case OP_PK2US: uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk2us(%s))", a); break;
        case OP_UP2US: uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up2us((%s).x)", a); break;
        case OP_PK2H:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk2h(%s))", a); break;
        case OP_UP2H:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up2h((%s).x)", a); break;
        /* Real RSX RSQ ignores the input's sign — 1/sqrt(|x|), not 1/sqrt(x)
         * (oracle: RPCS3 FragmentProgramDecompiler.cpp, "RSQ ignores the sign
         * of the inputs (Metro Last Light, GTA4)"). The strict form NaN'd the
         * normal-map Z-reconstruction (1-x²-y² measurably negative for most
         * pixels, median -0.0078) and poisoned the whole lighting chain — the
         * s25-s27 black/blue character class (scratch/s26_fp_bisect.md s27
         * part 4; two independent shader pairs confirmed). */
        case OP_RSQ: snprintf(rhs, sizeof(rhs), "rsqrt(abs((%s).x))", a); break;
        case OP_EX2: snprintf(rhs, sizeof(rhs), "exp2((%s).x)", a); break;
        case OP_LG2: snprintf(rhs, sizeof(rhs), "log2((%s).x)", a); break;
        case OP_COS: snprintf(rhs, sizeof(rhs), "cos((%s).x)", a); break;
        case OP_SIN: snprintf(rhs, sizeof(rhs), "sin((%s).x)", a); break;
        case OP_POW: snprintf(rhs, sizeof(rhs), "pow((%s).x, (%s).x)", a, b); break;
        case OP_DIV: snprintf(rhs, sizeof(rhs), "(%s) / (%s).x", a, b); break;
        /* Real RSX DIVSQ shares RSQ's sign-ignoring sqrt on the denominator
         * (oracle: RPCS3 FragmentProgramDecompiler.cpp's _builtin_sqrt macro,
         * reused by _builtin_divsq) AND forces the result to exactly 0,
         * component-wise, wherever the NUMERATOR component is 0 -- even if
         * the denominator is also 0 (oracle: same file, "DIVSQ is not
         * compliant. Result is 0 if numerator is 0 regardless of
         * denominator", FragmentProgramDecompiler.cpp:1076). Using a true
         * per-component select (HLSL vector `?:`, not lerp/mix) avoids NaN
         * contamination from the 0/0 branch, matching the oracle's own note
         * on why it can't use a blend here. */
        case OP_DIVSQ:
            snprintf(rhs, sizeof(rhs),
                     "(abs(%s) > 0.0 ? (%s) / sqrt(abs((%s).x)) : (float4)0.0)",
                     a, a, b);
            break;
        case OP_DP2: snprintf(rhs, sizeof(rhs), "dot((%s).xy, (%s).xy)", a, b); break;
        case OP_NRM:
            if (fp_nrm_safe_on()) {
                uses_nrm = 1;
                snprintf(rhs, sizeof(rhs), "float4(_rsx_nrm((%s).xyz), 1.0)", a);
            } else
                snprintf(rhs, sizeof(rhs), "float4(normalize((%s).xyz), 1.0)", a);
            break;
        case OP_LRP: snprintf(rhs, sizeof(rhs), "lerp((%s), (%s), (%s))", c, b, a); break;
        /* Lighting coefficients: (1, y, y > 0 ? 2^w : 0, 1) (RPCS3
         * FragmentProgramDecompiler, RSX_FP_OPCODE_LIF). */
        case OP_LIF:
            snprintf(rhs, sizeof(rhs),
                     "float4(1.0, (%s).y, (%s).y > 0.0 ? exp2((%s).w) : 0.0, 1.0)", a, a, a);
            break;
        /* Texture/branch fences: ordering hints with no result -- no-op. */
        case OP_FENCT: case OP_FENCB: rhs[0] = '\0'; handled = 0; break;
        case OP_SLT: snprintf(rhs, sizeof(rhs), "(float4)((%s) <  (%s))", a, b); break;
        case OP_SGE: snprintf(rhs, sizeof(rhs), "(float4)((%s) >= (%s))", a, b); break;
        case OP_SLE: snprintf(rhs, sizeof(rhs), "(float4)((%s) <= (%s))", a, b); break;
        case OP_SGT: snprintf(rhs, sizeof(rhs), "(float4)((%s) >  (%s))", a, b); break;
        case OP_SNE: snprintf(rhs, sizeof(rhs), "(float4)((%s) != (%s))", a, b); break;
        case OP_SEQ: snprintf(rhs, sizeof(rhs), "(float4)((%s) == (%s))", a, b); break;
        case OP_TEX:
            if (shadow && shadow[tex_unit].enabled) {
                snprintf(rhs, sizeof(rhs), "_rsx_shadow%u((%s).xyz)", tex_unit, a);
                break;
            }
            if ((xyz_mask >> tex_unit) & 1u)
                /* Cubemap or volume: sample with three coordinates. */
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz)", tex_unit, tex_unit, a);
            else if (per_unit)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xy)", tex_unit, tex_unit, a);
            else
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex[%u].Sample(rsx_samp[%u], (%s).xy)", tex_unit, tex_unit, a);
            fp_expand_sample(rhs, sizeof(rhs), tex_unit, (w0 >> 21) & 1u);
            break;
        case OP_TXP:
            if (shadow && shadow[tex_unit].enabled) {
                snprintf(rhs, sizeof(rhs), "_rsx_shadow%u((%s).xyz / (%s).w)", tex_unit, a, a);
                break;
            }
            if ((tex_cube_mask >> tex_unit) & 1u)
                /* Projective divide is meaningless for a cube lookup; sample
                 * the direction directly. */
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz)", tex_unit, tex_unit, a);
            else if ((vol_mask >> tex_unit) & 1u)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz / (%s).w)", tex_unit, tex_unit, a, a);
            else if (per_unit)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tex_unit, tex_unit, a, a);
            else
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex[%u].Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tex_unit, tex_unit, a, a);
            fp_expand_sample(rhs, sizeof(rhs), tex_unit, (w0 >> 21) & 1u);
            break;
        case OP_TXB: case OP_TXL: {
            /* Biased / explicit-LOD sample; bias or LOD is src1.x (RPCS3
             * TEXTURE_SAMPLE2D_BIAS / _LOD). GH3's fret buttons and fret lines
             * are TXB: unhandled, they sampled nothing and output alpha 0. */
            const int cube = (xyz_mask >> tex_unit) & 1u;
            char tn[32];
            snprintf(tn, sizeof(tn), per_unit ? "rsx_tex%u" : "rsx_tex[%u]", tex_unit);
            snprintf(rhs, sizeof(rhs), "%s.%s(rsx_samp[%u], (%s).%s, (%s).x)", tn,
                     opcode == OP_TXB ? "SampleBias" : "SampleLevel", tex_unit, a,
                     cube ? "xyz" : "xy", b);
            fp_expand_sample(rhs, sizeof(rhs), tex_unit, (w0 >> 21) & 1u);
            break;
        }
        case OP_KIL:
            /* Fragment kill. Predicated by the same exec_if condition as any
             * other instruction (RPCS3 FragmentProgramDecompiler case
             * RSX_FP_OPCODE_KIL: AddFlowOp("discard") under GetCond());
             * unconditional encodings discard outright. Dropping this (the
             * old TODO) rendered every alpha-cutout material as a solid
             * quad. */
            if (is_never) {
                rhs[0] = '\0';
            } else if (is_uncond) {
                out_puts(&o, "    discard;\n");
            } else {
                char kil[128];
                snprintf(kil, sizeof(kil),
                         "    if (any(cc%u.%s %s (float4)0)) discard;\n",
                         cc_read, cswz, cmp_op);
                out_puts(&o, kil);
            }
            handled = 0;   /* no destination write */
            break;
        default: {
            /* The number and the raw words, so an opcode outside the table
             * can be identified from the dumped shader alone. */
            char todo[160];
            snprintf(todo, sizeof todo,
                     "    /* TODO: unhandled FP opcode 0x%02X (%s) words %08X %08X %08X %08X */\n",
                     opcode, rsx_fp_opcode_name(opcode), w0, w1, w2, w3);
            out_puts(&o, todo);
            handled = 0;
            break; }
            break;
        }

        if (gamma_masks && gamma_masks[tex_unit] && !(w0 & (1u << 21)) &&
            (opcode == OP_TEX || opcode == OP_TXP || opcode == OP_TXB || opcode == OP_TXL)) {
            char sample[sizeof rhs]; memcpy(sample, rhs, sizeof sample);
            const int length = snprintf(rhs, sizeof rhs, "_rsx_gamma_input(%s, %uu)",
                                        sample, gamma_masks[tex_unit] & 15u);
            if (length < 0 || (size_t)length >= sizeof rhs) return -1;
        }

        int has_dest = !(w0 & FP_OUT_NONE);
        if (handled && (has_dest || set_cond)) {
            u32 dst_idx = (w0 & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            int dst_half = (w0 & FP_OUT_HALF) ? 1 : 0;
            const char* reg = dst_half ? "h" : "r";
            char m[5];
            dest_mask(w0, m);

            const char* sat = (w0 & FP_OUT_SAT) ? " _v = saturate(_v);" : "";
            /* LD_FP_PREC: the destination's precision clamp when it does not
             * saturate (RPCS3 SetDst), except for the opcodes whose result
             * is already bounded or must not be, a half-to-half temp move,
             * and an FP16 modifier on a full-precision register. */
            char prec_txt[64];
            if (!(w0 & FP_OUT_SAT) && fp_prec_on() && !is_branch) {
                const u32 dprec = (w0 >> 22) & 3u;
                int skip = dprec == 0 ||
                    opcode == OP_NRM || opcode == OP_MAX || opcode == OP_MIN ||
                    opcode == OP_COS || opcode == OP_SIN || opcode == OP_RFL ||
                    opcode == OP_FRC || opcode == OP_LIT || opcode == OP_LIF ||
                    opcode == OP_LG2 ||
                    (opcode == OP_MOV && dst_half && s0.half && s0.type == FP_REG_TYPE_TEMP) ||
                    (dprec == 1 && !dst_half);
                const char* lo = NULL;
                const char* hi = NULL;
                if (!skip && fp_prec_limits(dprec, &lo, &hi)) {
                    snprintf(prec_txt, sizeof prec_txt, " _v = clamp(_v, %s, %s);", lo, hi);
                    sat = prec_txt;
                    g_rsx_fp_prec_clamps++;
                }
            }
            /* NV40 per-instruction result-scale modifier (SRC1 word bits
             * 28-30): 1/2/3 = *2/*4/*8, 5/6/7 = /2//4//8; applied to the
             * result BEFORE saturate (RPCS3 FragmentProgramDecompiler.cpp
             * SetDst, RSXFragmentProgram.h SRC1.scale:3). Dropped entirely
             * until s32: draw 1625's two-tap box filter (÷2 on the final
             * ADD) summed unaveraged -- the flat 2x composite gain behind
             * the whiteout/overbright class (scratch/s32_gain_hunt.md). */
            u32 res_scale = (w2 >> 28) & 0x7u;
            static const char* scale_txt[8] = { "", " * 2.0", " * 4.0", " * 8.0",
                                                "", " / 2.0", " / 4.0", " / 8.0" };

            /* Broadcast the result to float4 first so the write-mask picks the
             * CORRESPONDING components: `dst.w = rhs` would HLSL-truncate a
             * vector rhs to its .x, but NV40 writes rhs.w to dst.w. Scalar
             * results (DPx/RCP/...) replicate, which is also the hardware
             * behavior. */
            if (has_dest && is_uncond && !set_cond) {
                /* Common unconditional path — kept byte-identical so the 39
                 * non-predicated programs in this capture emit unchanged. */
                char line[800];
                snprintf(line, sizeof(line),
                         "    { float4 _v = (float4)(%s)%s;%s %s[%u].%s = _v.%s; }\n",
                         rhs, scale_txt[res_scale], sat, reg, dst_idx, m, m);
                out_puts(&o, line);
            } else {
                /* Predicated and/or CC-writing instruction. */
                char buf[1200];
                snprintf(buf, sizeof(buf),
                         "    { float4 _v = (float4)(%s)%s;%s\n",
                         rhs, scale_txt[res_scale], sat);
                out_puts(&o, buf);
                if (has_dest) {
                    if (is_uncond) {
                        snprintf(buf, sizeof(buf),
                                 "      %s[%u].%s = _v.%s;\n", reg, dst_idx, m, m);
                    } else if (is_never) {
                        snprintf(buf, sizeof(buf),
                                 "      /* exec_if=none: write suppressed */\n");
                    } else {
                        /* Per-component select gated by the CC sign test.
                         * A real select, not lerp(dst, _v, mask): lerp is
                         * arithmetic (dst + mask * (_v - dst)), so an Inf or
                         * NaN in the unselected operand poisons the result
                         * (0 * Inf = NaN), and a register holding raw bits
                         * (a packed word) loses its denormal and NaN
                         * patterns on the way through the ALU. */
                        snprintf(buf, sizeof(buf),
                                 "      float4 _p = (float4)((cc%u).%s %s (float4)0.0);\n"
                                 "      %s[%u].%s = (_p.%s != 0.0) ? _v.%s : %s[%u].%s;\n",
                                 cc_read, cswz, cmp_op,
                                 reg, dst_idx, m, m, m, reg, dst_idx, m);
                    }
                    out_puts(&o, buf);
                }
                if (set_cond) {
                    /* CC receives the destination value (post-select), masked;
                     * for no-dest set_cond it receives the raw result. */
                    if (has_dest)
                        snprintf(buf, sizeof(buf),
                                 "      cc%u.%s = %s[%u].%s;\n", cc_write, m, reg, dst_idx, m);
                    else if (fp_cc_pred_on() && is_never)
                        snprintf(buf, sizeof(buf),
                                 "      /* exec_if=none: condition write suppressed */\n");
                    else if (fp_cc_pred_on() && !is_uncond)
                        snprintf(buf, sizeof(buf),
                                 "      float4 _q = (float4)((cc%u).%s %s (float4)0.0);\n"
                                 "      cc%u.%s = (_q.%s != 0.0) ? _v.%s : cc%u.%s;\n",
                                 cc_read, cswz, cmp_op, cc_write, m, m, m, cc_write, m);
                    else
                        snprintf(buf, sizeof(buf),
                                 "      cc%u.%s = _v.%s;\n", cc_write, m, m);
                    out_puts(&o, buf);
                }
                out_puts(&o, "    }\n");
            }

            if (has_dest) {
                const int target = fp_export_target(dst_idx, dst_half);
                if (target >= 0) wrote[dst_half][target] = 1;
                const u32 dmask = (w0 >> FP_OUT_MASK_SHIFT) & 0xFu ? (w0 >> FP_OUT_MASK_SHIFT) & 0xFu : 0xFu;
                const u8 wr_h = is_uncond || !alias_sync ? 'H' : 'U', wr_r = is_uncond || !alias_sync ? 'R' : 'U';
                for (u32 c = 0; c < 4; c++) {
                    if (!((dmask >> c) & 1u)) continue;
                    if (dst_half) { if (dst_idx * 4u + c < sizeof last_w) last_w[dst_idx * 4u + c] = wr_h; }
                    else if (dst_idx * 8u + 2u * c + 1u < sizeof last_w)
                        last_w[dst_idx * 8u + 2u * c] = last_w[dst_idx * 8u + 2u * c + 1u] = wr_r;
                }
            }
        }

        if (w0 & FP_END) break;
    }
    /* A block whose end address is at or past END never saw it: close it. */
    while (n_blk-- > 0)
        out_puts(&o, "    }\n");

    /* Fragment color output register selection. The NV40 hardware picks the
     * output from the SHADER_CONTROL word's 32_BITS_EXPORTS bit, not from
     * whichever temp happened to be written: fp32-export programs output r0,
     * fp16-export programs (the default) output h0 (RPCS3
     * FragmentProgramDecompiler::BuildCode). Many real material/lighting
     * shaders accumulate into fp32 temps (r2/r3/r4) but write their FINAL
     * color to h0 — the old "wrote_r0 => return r0" heuristic returned a stale
     * intermediate, producing flat/constant surfaces. */
    int half_exports;
    if (ctrl == RSX_FP_CTRL_AUTO) {
        /* No control word (standalone tests): legacy heuristic. */
        half_exports = (wrote[0][0] || !wrote[1][0]) ? 0 : 1;
    } else {
        half_exports = (ctrl & FP_CTRL_32BIT_EXPORTS) ? 0 : 1;
    }
    const char* export_file = half_exports ? "h" : "r";

    /* How many colour targets the program feeds: one past the highest export
     * register of the selected file it wrote. A program that writes only the
     * first keeps the single-value return it has always had, byte for byte;
     * only a program that reaches the second, third or fourth grows a struct.
     * Writes to the OTHER file's export registers do not count -- r2/r3/r4 are
     * ordinary temps under 16-bit exports, and accumulating into them while
     * the colour goes to h0 is what most material shaders do. */
    u32 targets = 1;
    for (u32 t = FP_MAX_COLOR_TARGETS; t-- > 1; )
        if (wrote[half_exports][t]) { targets = t + 1; break; }

    /* Depth export. SHADER_CONTROL bits 1-3 (0xE,
     * CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT) make the program's depth the
     * fragment depth: always the fp32 register r1.z, whatever the colour
     * export width, clamped to [0,1]. Without SV_Depth the fragment keeps the
     * interpolated depth of the primitive, so a full-screen pass that rebuilds
     * a depth buffer writes the quad's constant depth instead. The output
     * becomes a struct even for one colour target. */
    const int depth_export = ctrl != RSX_FP_CTRL_AUTO && (ctrl & 0xEu) != 0u;
    const int gather = half_exports && ctrl != RSX_FP_CTRL_AUTO && !any_branch && fp_export_gather_on();
    if (targets == 1 && !depth_export) {
        char tail[96];
        snprintf(tail, sizeof(tail), "    float4 _o = %s[0];\n", export_file);
        out_puts(&o, tail);
        if (gather) fp_export_gather(&o, "_o", 0, last_w, sizeof last_w);
        out_puts(&o, "    return (_o == _o) ? _o : (float4)0;\n}\n");
    } else {
        /* Multiple render targets. The NaN guard is per target: a poisoned
         * G-buffer plane is as fatal as a poisoned colour one, and the planes
         * are computed independently. */
        out_puts(&o, "    PSOutput _out;\n");
        for (u32 t = 0; t < targets; t++) {
            char line[192];
            snprintf(line, sizeof(line), "    float4 _o%u = %s[%u];\n",
                     t, export_file, fp_export_reg[half_exports][t]);
            out_puts(&o, line);
            if (gather) {
                char var[8];
                snprintf(var, sizeof var, "_o%u", t);
                fp_export_gather(&o, var, fp_export_reg[half_exports][t], last_w, sizeof last_w);
            }
            snprintf(line, sizeof(line), "    _out.t%u = (_o%u == _o%u) ? _o%u : (float4)0;\n",
                     t, t, t, t);
            out_puts(&o, line);
        }
        if (depth_export)
            out_puts(&o, "    float _d = r[1].z;\n"
                         "    _out.depth = (_d == _d) ? saturate(_d) : 0.0;\n");
        out_puts(&o, "    return _out;\n}\n");
    }

    /* Preamble: PSInput matches the backend's placeholder layout; temp/half
     * register files; texture+sampler banks for TEX. Built here, in front of
     * the body, now that the return type is known. */
    char preamble[32768]; /* bounded room for all 16 comparison helpers */
    Out p = { preamble, sizeof(preamble), 0, 1 };
    preamble[0] = '\0';
    out_puts(&p,
        "struct PSInput {\n"
        "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
        "    float4 fog : FOG;\n"
        "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
        "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
        "    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n"
        "};\n");
    /* Texture bank. With no cube units (the default) emit the exact legacy
     * array declaration so 2D-only programs are byte-identical. When any unit
     * is a cubemap, declare each unit individually at its t-register so the
     * cube units can be TextureCube while the rest stay Texture2D. */
    if (per_unit == 0) {
        out_puts(&p, "Texture2D    rsx_tex[16] : register(t0);\n");
    } else {
        char decl[64];
        for (u32 u = 0; u < 16; u++) {
            snprintf(decl, sizeof(decl), "%s rsx_tex%u : register(t%u);\n",
                     ((tex_cube_mask >> u) & 1u) ? "TextureCube" :
                     ((vol_mask >> u) & 1u) ? "Texture3D  " : "Texture2D  ", u, u);
            out_puts(&p, decl);
        }
    }
    out_puts(&p,
        "SamplerState rsx_samp[16] : register(s0);\n"
    );
    if (buffered) {
        char constants_decl[192];
        snprintf(
            constants_decl, sizeof(constants_decl),
            "cbuffer PSConstants : register(b1) {\n"
            "    float4 fp_constants[%u];\n"
            "    float4 fp_alpha;\n"
            "};\n",
            buffered_constant_count ? buffered_constant_count : 1u);
        out_puts(&p, constants_decl);
    }
    if (shadow) for (u32 u = 0; u < 16; u++)
        if ((sampled_units & (1u << u)) && shadow[u].enabled)
            fp_shadow_helper(&p, u, tex_cube_mask, &shadow[u]);
    if (gamma_units) fp_gamma_helper(&p);
    /* NV40 pack/unpack. A packed word travels on in a float's bits, so the
     * packs bitcast and the unpacks bitcast back; the sRGB forms follow
     * RPCS3's _builtin_pkg/_builtin_upg (linear_to_srgb, srgb_to_linear).
     * These were the opcodes inFAMOUS 2's character materials used and the
     * decompiler skipped, leaving the destination with whatever it held.
     * Only for a program that uses one, so every other program's text is
     * unchanged. */
    if (uses_pack) out_puts(&p,
        "uint   _rsx_pk4ub(float4 v) { uint4 b = (uint4)round(saturate(v) * 255.0); return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
        "float4 _rsx_up4ub(float v) { uint u = asuint(v); return float4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u) / 255.0; }\n"
        "uint   _rsx_pk4b(float4 v) { uint4 b = (uint4)((int4)round(clamp(v, -1.0, 1.0) * 127.0)) & 255u; return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
        "float4 _rsx_up4b(float v) { uint u = asuint(v); int4 b = int4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u); b = (b << 24) >> 24; return clamp((float4)b / 127.0, -1.0, 1.0); }\n"
        "uint   _rsx_pk2us(float4 v) { uint2 b = (uint2)round(saturate(v.xy) * 65535.0); return b.x | (b.y << 16); }\n"
        "float4 _rsx_up2us(float v) { uint u = asuint(v); return float4(u & 65535u, u >> 16, u & 65535u, u >> 16) / 65535.0; }\n"
        "uint   _rsx_pk2h(float4 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }\n"
        "float4 _rsx_up2h(float v) { uint u = asuint(v); return float4(f16tof32(u & 65535u), f16tof32(u >> 16), f16tof32(u & 65535u), f16tof32(u >> 16)); }\n"
        "float4 _rsx_lin2srgb(float4 cl) { cl = saturate(cl); float4 low = cl * 12.92; float4 high = 1.055 * pow(cl, 1.0 / 2.4) - 0.055; return saturate((cl < 0.0031308) ? low : high); }\n"
        "float4 _rsx_srgb2lin(float4 cs) { float4 a = cs / 12.92; float4 b = pow(max(cs + 0.055, 0.0) / 1.055, 2.4); return (cs > 0.04045) ? b : a; }\n");
    if (uses_nrm) out_puts(&p,
        "float3 _rsx_nrm(float3 v) { return dot(v, v) > 0.0 ? normalize(v) : v; }\n");
    if (uses_dp3p) out_puts(&p,
        "float _rsx_dp3p(float3 a, float3 b) { return mad(a.x, b.x, mad(a.y, b.y, a.z * b.z)); }\n");
    /* LD_FP_FACE: only a program that reads SSA takes the facing input, so
     * every other program's signature is unchanged. SV_IsFrontFace comes from
     * the rasterizer, not the vertex shader, so PSInput is untouched. */
    const int uses_face = o.ok && strstr(out, "_rsx_face") != NULL;
    const char* face_param = uses_face ? ", bool _rsx_face : SV_IsFrontFace" : "";
    if (targets == 1 && !depth_export) {
        char sig[128];
        snprintf(sig, sizeof sig, "float4 main(PSInput input%s) : SV_TARGET {\n", face_param);
        out_puts(&p, sig);
    } else {
        out_puts(&p, "struct PSOutput {\n");
        for (u32 t = 0; t < targets; t++) {
            char member[64];
            snprintf(member, sizeof(member),
                     "    float4 t%u : SV_TARGET%u;\n", t, t);
            out_puts(&p, member);
        }
        if (depth_export)
            out_puts(&p, "    float depth : SV_Depth;\n");
        char sig[128];
        snprintf(sig, sizeof sig, "};\nPSOutput main(PSInput input%s) {\n", face_param);
        out_puts(&p, sig);
    }

    if (!o.ok || !p.ok) return -1;
    if (o.len + p.len + 1 > out_size) return -1;
    memmove(out + p.len, out, o.len + 1);
    memcpy(out, preamble, p.len);
    return count;
}

int rsx_fp_decompile_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                        u32 tex_cube_mask, char* out, u32 out_size)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 0, NULL, NULL, out, out_size, NULL);
}

int rsx_fp_decompile_buffered_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 1, NULL, NULL, out, out_size,
        out_constant_count);
}

int rsx_fp_decompile_buffered_shadow_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask,
        1, shadow, NULL, out, out_size, out_constant_count);
}

int rsx_fp_decompile_buffered_shadow_color_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask,
        1, shadow, gamma_masks, out, out_size, out_constant_count);
}

static float fp_half_to_float(u16 h)
{
    const u32 sign = (u32)(h & 0x8000u) << 16;
    u32 exponent = (h >> 10) & 0x1Fu;
    u32 mantissa = h & 0x03FFu;
    u32 bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127u - 15u + 1u;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x03FFu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
    }
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

float rsx_fp_alpha_ref(u32 raw, u32 surface_color_format)
{
    /* NV4097 surface color formats: 14=W16Z16Y16X16,
     * 15=W32Z32Y32X32, 16=X32. */
    if (surface_color_format == 14u)
        return fp_half_to_float((u16)(raw & 0xFFFFu));
    if (surface_color_format == 15u || surface_color_format == 16u) {
        float value;
        memcpy(&value, &raw, sizeof(value));
        return value;
    }
    return (float)(raw & 0xFFu) / 255.0f;
}

/* The comparison the alpha test has to pass. The alpha it tests is colour
 * target 0's, which for a single-target program is the returned value itself
 * and for an MRT one is the returned struct's first member -- the NV40 alpha
 * test has one input however many targets the program feeds. `cmp` is the
 * CELL_GCM function minus 0x200, with NEVER (0) spelled out and ALWAYS (7)
 * already handled by the caller. */
static void fp_alpha_pass_expr(u32 cmp, const char* alpha, char* out, u32 out_size)
{
    static const char* op[8] = { NULL, "<", "==", "<=", ">", "!=", ">=", NULL };
    if (cmp == 0 || cmp >= 7 || !op[cmp])
        snprintf(out, out_size, "false");
    else
        snprintf(out, out_size, "%s.a %s _rsx_alpha_ref", alpha, op[cmp]);
}

/* Where that alpha lives, and what type the value being returned has. */
static int fp_returns_struct(const char* hlsl)
{
    return strstr(hlsl, "struct PSOutput") != NULL;
}

int rsx_fp_apply_alpha_test(char* hlsl, u32 out_size, u32 func, float ref)
{
    if (!hlsl || out_size == 0)
        return -1;

    const u32 cmp = func >= 0x0200u && func <= 0x0207u ? func - 0x0200u : 7u;
    if (cmp == 7u)
        return 0;

    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker)
        return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression ||
        (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';

    const int mrt = fp_returns_struct(hlsl);
    char pass[96];
    fp_alpha_pass_expr(cmp, mrt ? "_rsx_out.t0" : "_rsx_out", pass, sizeof(pass));
    char suffix[512];
    const int n = snprintf(suffix, sizeof(suffix),
        "    %s _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = %.9g;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        mrt ? "PSOutput" : "float4", output, (double)ref, pass);
    if (n < 0 || (u32)n >= sizeof(suffix))
        return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > out_size)
        return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

int rsx_fp_apply_alpha_test_buffered(
    char* hlsl, u32 out_size, u32 func)
{
    if (!hlsl || out_size == 0)
        return -1;

    const u32 cmp =
        func >= 0x0200u && func <= 0x0207u ? func - 0x0200u : 7u;
    if (cmp == 7u)
        return 0;

    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker)
        return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression ||
        (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';

    const int mrt = fp_returns_struct(hlsl);
    char pass[96];
    fp_alpha_pass_expr(cmp, mrt ? "_rsx_out.t0" : "_rsx_out", pass, sizeof(pass));
    char suffix[512];
    const int n = snprintf(
        suffix, sizeof(suffix),
        "    %s _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = fp_alpha.x;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        mrt ? "PSOutput" : "float4", output, pass);
    if (n < 0 || (u32)n >= sizeof(suffix))
        return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > out_size)
        return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

/* --- kept from this tree when adopting the newer decompiler ---------------
 * rsx_d3d12_backend.c calls both of these: extract_consts feeds the per-draw
 * fp_k[] slots, code_hash keys the PSO cache on the ucode rather than the
 * constants. The live-draw engine does not need them, so they are absent
 * upstream in caner's fork. */

/* Walk a program the same way the decompiler does and copy out its inline
 * constants in the SAME order the decompiler assigns fp_k[] indices. The backend
 * calls this per draw, so re-patched constants take effect without recompiling. */
int rsx_fp_extract_consts(const u8* ucode, u32 max_bytes, float* out, int max_out)
{
    if (!ucode || !out) return 0;
    u32 off = 0; int n = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        int is_branch = (w2 & FP_BRANCH) != 0;
        int has_k = !is_branch &&
            ((((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST));
        if (has_k) {
            if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + i * 4));
                    if (n < max_out) memcpy(&out[n * 4 + i], &cw, 4);
                }
                off += 16;
            }
            if (n < max_out) n++;
        }
        if (w0 & FP_END) break;
    }
    { static int dbg = -1;
      if (dbg < 0) { const char* e = getenv("FP_KDBG"); dbg = e ? atoi(e) : 0; }
      if (dbg) { static int m = 0; if (m < 3 && ++m) {
        fprintf(stderr, "[FPK] %d consts%c", n, 10);
        for (int _q = 0; _q < n; _q++)
            fprintf(stderr, "   k[%2d] = (%g %g %g %g)%c", _q, out[_q*4+0],
                    out[_q*4+1], out[_q*4+2], out[_q*4+3], 10); } }
    }
    return n;
}

/* FNV-1a over a program's INSTRUCTION words only, skipping the inline constant
 * slots. With constants hoisted into fp_k[] the compiled shader does not depend
 * on their values, so including them in the pipeline key made it change every
 * time the title re-patched a constant -- which is exactly what thrashed the
 * cache. */
u32 rsx_fp_code_hash(const u8* ucode, u32 max_bytes)
{
    u32 h = 2166136261u;
    if (!ucode) return h;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        for (u32 i = 0; i < 16; i++) { h ^= ucode[off + i]; h *= 16777619u; }
        off += 16;
        int is_branch = (w2 & FP_BRANCH) != 0;
        int has_k = !is_branch &&
            ((((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST));
        if (has_k) off += 16;            /* skip the constant, do not hash it */
        if (w0 & FP_END) break;
    }
    return h;
}

/* Scale texel-space coordinates to the 0..1 range HLSL sampling expects.
 *
 * An RSX texture whose format carries RSX_TEX_FMT_UNNORM (0x40) is addressed in
 * TEXELS, not normalised units. ps1_netemu composites the PS1 framebuffer with
 * exactly that: format 0xE2 (A1R5G5B5 | UNNORM | LINEAR) over a 1024x512
 * texture, and a quad whose texture coordinates run 1..638 by 2..474 --
 *
 *     [uvdbg] unit=0 ps1fb 1024x512 pitch=2048 fmt=0xE2
 *     [uvdbg]   v0 pos(-1.0 -1.0) tc(1.0000 2.0000)
 *     [uvdbg]   v2 pos(0.8 0.8)   tc(638.0000 474.0000)
 *
 * The bit was only ever masked OUT of the base format so the decoder could
 * recognise A1R5G5B5, and never honoured for addressing. So the sampler was
 * handed coordinates hundreds of times out of range, which under clamp lands on
 * one edge texel for the whole quad -- a flat, uniform result. That is the
 * black screen: the PS1 renders, the composite pass runs, and it samples one
 * corner texel.
 *
 * D3D12 has no unnormalised addressing mode, so the division has to happen in
 * the shader. Doing it as a post-pass over the generated HLSL rather than
 * inside the emitter keeps it beside rsx_fp_apply_alpha_test, which already
 * works this way, and leaves every non-UNNORM program byte-identical.
 *
 * Cube units are skipped: their coordinate is a float3 direction, and
 * RSX_TEX_FMT_UNNORM has no meaning for one.
 */
int rsx_fp_apply_unnorm_scale(char* hlsl, u32 hlsl_size, u32 unnorm_mask,
                              const u32 dim[][2], u32 cube_mask)
{
    if (!hlsl || !unnorm_mask) return 0;
    int patched = 0;

    for (u32 u = 0; u < 16; u++) {
        if (!((unnorm_mask >> u) & 1u)) continue;
        if ((cube_mask >> u) & 1u) continue;
        const u32 w = dim[u][0], h = dim[u][1];
        if (!w || !h) continue;

        /* Both spellings the emitter produces for a 2D unit. */
        char needles[2][40];
        snprintf(needles[0], sizeof needles[0], "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
        snprintf(needles[1], sizeof needles[1], "rsx_tex%u.Sample(rsx_samp[%u],", u, u);

        for (int nidx = 0; nidx < 2; nidx++) {
            const char* needle = needles[nidx];
            const u32 nlen = (u32)strlen(needle);
            for (;;) {
                char* at = strstr(hlsl, needle);
                if (!at) break;
                /* The '(' of Sample(, then its matching ')'. */
                char* open = at + nlen - 1;   /* the ',' ... step back to '(' */
                while (open > at && *open != '(') open--;
                if (*open != '(') break;
                int depth = 0;
                char* p = open;
                for (; *p; p++) {
                    if (*p == '(') depth++;
                    else if (*p == ')') { depth--; if (!depth) break; }
                }
                if (*p != ')') break;

                char scale[80];
                snprintf(scale, sizeof scale, " * float2(%.9g, %.9g)",
                         1.0 / (double)w, 1.0 / (double)h);
                const u32 slen = (u32)strlen(scale);
                const u32 used = (u32)strlen(hlsl);
                if (used + slen + 1 > hlsl_size) return -1;

                memmove(p + slen, p, used - (u32)(p - hlsl) + 1);
                memcpy(p, scale, slen);
                patched++;

                /* Mark this call site done so the next strstr moves past it:
                 * the inserted text is inside the call, so an unmarked scan
                 * would find the same site forever. */
                memcpy(at, "rsx_TEX", 7);
            }
        }
        /* Restore the marker. */
        for (char* q = hlsl; (q = strstr(q, "rsx_TEX")) != NULL; )
            memcpy(q, "rsx_tex", 7);
    }
    return patched;
}

/* NV4097_SET_SHADER_PACKER accepts any nonzero value. Floating surfaces and
 * 32-bit exports bypass the integer framebuffer's RGB transfer function. */
int rsx_fp_srgb_output_enabled(u32 packer, u32 ctrl, u32 color_format)
{
    return packer != 0 && !(ctrl & FP_CTRL_32BIT_EXPORTS) &&
           color_format != 0x0Bu && color_format != 0x0Cu && color_format != 0x0Du;
}

int rsx_fp_apply_srgb_output(char* hlsl, u32 capacity)
{
    static const char helper[] =
        "float4 _rsx_srgb_output(float4 v) {\n"
        "    float3 lo = v.rgb * 12.92;\n"
        "    float3 hi = 1.055 * pow(max(v.rgb, (float3)0), (float3)(1.0 / 2.4)) - 0.055;\n"
        "    float3 rgb = float3(v.r < 0.0031308 ? lo.r : hi.r,\n"
        "                         v.g < 0.0031308 ? lo.g : hi.g,\n"
        "                         v.b < 0.0031308 ? lo.b : hi.b);\n"
        "    return float4(saturate(rgb), v.a);\n}\n";
    if (!hlsl || !capacity) return -1;
    size_t len = 0;
    while (len < capacity && hlsl[len]) ++len;
    if (len == capacity || strstr(hlsl, "_rsx_srgb_output")) return -1;
    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return ")) marker = p;
    if (!marker) return -1;
    const char* expression = marker + strlen("    return ");
    const char* end = strstr(expression, ";\n}\n");
    if (!end || end == expression || (size_t)(end - expression) >= 96 || end[4])
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(end - expression));
    output[end - expression] = 0;
    const int mrt = strstr(hlsl, "struct PSOutput") != NULL;
    char suffix[1024];
    int n = snprintf(suffix, sizeof suffix, "    %s _rsx_color_out = %s;\n",
                     mrt ? "PSOutput" : "float4", output);
    if (n < 0 || (size_t)n >= sizeof suffix) return -1;
    if (mrt) {
        unsigned targets = 0;
        for (unsigned t = 0; t < 4; ++t) {
            char field[64];
            snprintf(field, sizeof field, "    float4 t%u : SV_TARGET%u;", t, t);
            if (!strstr(hlsl, field)) continue;
            const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                "    _rsx_color_out.t%u = _rsx_srgb_output(_rsx_color_out.t%u);\n", t, t);
            if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
            n += added; ++targets;
        }
        if (!targets) return -1;
    } else {
        const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
            "    _rsx_color_out = _rsx_srgb_output(_rsx_color_out);\n");
        if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
        n += added;
    }
    const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                              "    return _rsx_color_out;\n}\n");
    if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
    n += added;
    const size_t prefix = (size_t)(marker - hlsl);
    const size_t helper_len = sizeof helper - 1;
    if (helper_len + prefix + (size_t)n + 1 > capacity) return -1;
    /* All parsing/capacity checks precede the first mutation. */
    memcpy(marker, suffix, (size_t)n + 1);
    memmove(hlsl + helper_len, hlsl, prefix + (size_t)n + 1);
    memcpy(hlsl, helper, helper_len);
    return 1;
}

/* Multiply the colour output's RGB by `scale` (the blend's source factor
 * moved into the shader, rsx_live_draw.c ld_blend_fold); saturate first for a
 * fixed-point target, which clamps the output before blending. One target
 * only; alpha and a depth export are kept. Apply after the sRGB output and
 * before alpha-test wrapping. Returns 1, or -1 without mutation. */
int rsx_fp_apply_output_rgb_scale(char* hlsl, u32 capacity, const char* scale, int saturate_first)
{
    if (!hlsl || !capacity || !scale || strlen(scale) > 120) return -1;
    size_t len = 0;
    while (len < capacity && hlsl[len]) ++len;
    if (len == capacity || strstr(hlsl, "_rsx_blend_out") || strstr(hlsl, "struct PSOutput")) return -1;
    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return ")) marker = p;
    if (!marker) return -1;
    const char* expression = marker + strlen("    return ");
    const char* end = strstr(expression, ";\n}\n");
    if (!end || end == expression || (size_t)(end - expression) >= 96 || end[4])
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(end - expression));
    output[end - expression] = 0;
    char suffix[512];
    const int n = snprintf(suffix, sizeof suffix,
        "    float4 _rsx_blend_out = %s;\n"
        "    _rsx_blend_out.rgb = %s(_rsx_blend_out.rgb) * (%s);\n"
        "    return _rsx_blend_out;\n}\n",
        output, saturate_first ? "saturate" : "", scale);
    if (n < 0 || (size_t)n >= sizeof suffix) return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > capacity) return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

int rsx_fp_decompile_buffered_color_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const u8 gamma_masks[16], char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask,
        1, NULL, gamma_masks, out, out_size, out_constant_count);
}

/* ---- LD_SHADER_STATS: which opcodes and modifiers the title's programs use --
 * rsx_fp_stats_note() is called by the live draw once per pipeline it builds
 * (only when LD_SHADER_STATS is set). The ucode counts are made once per
 * distinct program (structural hash), so they read "occurrences/programs";
 * the checks against register state (texcoord 2D mode, expansion on float
 * formats, ...) are made per pipeline. Nothing here touches the generated
 * text. rsx_fp_stats_print() writes the table to stderr. */
enum {
    FPS_SAT, FPS_SCALE, FPS_DPREC1, FPS_DPREC2, FPS_DPREC3,
    FPS_SPREC1, FPS_SPREC2, FPS_SPREC3, FPS_SPREC4, FPS_SPREC5,
    FPS_SETCC, FPS_SETCC_NODEST, FPS_SETCC_NODEST_PRED, FPS_PRED, FPS_NEVER,
    FPS_CCREAD1, FPS_CCWRITE1, FPS_KIL_PRED,
    FPS_ABS, FPS_NEG, FPS_SRC2ABS, FPS_TYPE3, FPS_PERSP, FPS_USEINDEX, FPS_EXPTEX,
    FPS_DST_HALF, FPS_SRC_HALF, FPS_NODEST, FPS_DST_OOR,
    FPS_IFE, FPS_IFE_SWZ, FPS_LOOP, FPS_BRK, FPS_CALRET,
    FPS_TC_ZW, FPS_SSA, FPS_WPOS_W, FPS_ALIAS_R_AFTER_H, FPS_ALIAS_H_AFTER_R, FPS_EXPORT_ALIAS,
    FPS_DEPTH_EXPORT, FPS_EXPORT32, FPS_MRT2, FPS_MRT3, FPS_MRT4,
    /* export_alias detail: only alpha lanes aliased / a colour lane / on an
     * MRT target (t >= 1) / every lane of the target / the depth export */
    FPS_EXPORT_ALIAS_ALPHA_ONLY, FPS_EXPORT_ALIAS_RGB, FPS_EXPORT_ALIAS_MRT, FPS_EXPORT_ALIAS_ALL,
    FPS_EXPORT_ALIAS_DEPTH,
    FPS_NFLAGS
};
static const char* const fps_flag_names[FPS_NFLAGS] = {
    "sat", "scale", "dprec_fp16", "dprec_fx12", "dprec_fx9",
    "sprec_fp16", "sprec_fx12", "sprec_fx9", "sprec_sat", "sprec_5to7",
    "set_cc", "set_cc_nodest", "set_cc_nodest_pred", "predicated", "never",
    "cc_read_cc1", "cc_write_cc1", "kil_pred",
    "abs", "neg", "src2_abs", "src_type3", "persp_corr", "use_index", "exp_tex",
    "dst_half", "src_half", "no_dest", "dst_reg_ge48",
    "ife", "ife_cond_not_broadcast", "loop_rep", "brk", "cal_ret",
    "tc_zw_read", "ssa_read", "wpos_w_read", "read_r_after_h", "read_h_after_r", "export_alias",
    "depth_export", "export32", "mrt2", "mrt3", "mrt4",
    "export_alias_alpha_only", "export_alias_rgb", "export_alias_mrt", "export_alias_all_lanes",
    "export_alias_depth"
};
enum {
    FPR_TC2D_ZW, FPR_CENTROID, FPR_EXP_FLOAT, FPR_EXP_ZERO_REMAP, FPR_MRT_SHORT,
    FPR_OUTMASK_READ, FPR_VP_UNWRITTEN_READ, FPR_TWOSIDE_COLOR, FPR_SSA,
    FPR_N
};
static const char* const fps_run_names[FPR_N] = {
    "tc2d_zw_read", "centroid_tc", "exp_tex_float_fmt", "exp_tex_zero_remap", "fewer_targets_than_bound",
    "read_output_masked_off", "read_vp_unwritten", "two_side_color", "ssa"
};
#define FPS_OPS 72   /* 0x00..0x3F plus BRK..RET at 0x40..0x45 */
#define FPS_SEEN 16384
static struct {
    u32 programs, psos, instructions, seen_full;
    unsigned long long op_occ[FPS_OPS]; u32 op_prog[FPS_OPS];
    unsigned long long fl_occ[FPS_NFLAGS]; u32 fl_prog[FPS_NFLAGS];
    unsigned long long in_occ[16]; u32 in_prog[16];
    u32 run[FPR_N];
    u32 run_bits[FPR_N];   /* which units / texcoords / outputs, OR'd */
    u64 seen[FPS_SEEN];
} s_fps;

/* Components (bit per x..w) of source `idx` an opcode consumes before the
 * source swizzle, given the destination mask; 0 = the source is not read. */
static u32 fps_src_comps(u32 op, u32 idx, u32 dm, int xyz)
{
    const u32 tc = xyz ? 0x7u : 0x3u;
    switch (op) {
    case OP_NOP: case OP_KIL: case OP_FENCT: case OP_FENCB: case OP_STR: case OP_SFL:
        return 0;
    case OP_MOV: case OP_FRC: case OP_FLR: case OP_DDX: case OP_DDY:
        return idx == 0 ? dm : 0;
    case OP_RCP: case OP_RSQ: case OP_EX2: case OP_LG2: case OP_COS: case OP_SIN:
    case OP_UP4B: case OP_UP4UB: case OP_UP2H: case OP_UP2US: case OP_UPG:
        return idx == 0 ? 0x1u : 0;
    case OP_POW: return idx < 2 ? 0x1u : 0;
    case OP_DIV: case OP_DIVSQ: return idx == 0 ? dm : idx == 1 ? 0x1u : 0;
    case OP_DP2: return idx < 2 ? 0x3u : 0;
    case OP_DP2A: return idx < 2 ? 0x3u : 0x1u;
    case OP_DP3: return idx < 2 ? 0x7u : 0;
    case OP_DP4: return idx < 2 ? 0xFu : 0;
    case OP_NRM: return idx == 0 ? 0x7u : 0;
    case OP_DST: return idx == 0 ? 0x6u : idx == 1 ? 0xAu : 0;
    case OP_LIF: case OP_LIT: return idx == 0 ? 0xBu : 0;
    case OP_PK2H: case OP_PK2US: return idx == 0 ? 0x3u : 0;
    case OP_PK4B: case OP_PK4UB: case OP_PKG: return idx == 0 ? 0xFu : 0;
    case OP_TEX: case OP_TEXBEM: return idx == 0 ? tc : 0;
    case OP_TXP: case OP_TXPBEM: return idx == 0 ? (tc | 0x8u) : 0;
    case OP_TXB: case OP_TXL: return idx == 0 ? tc : idx == 1 ? 0x1u : 0;
    case OP_TXD: return tc;
    case OP_MAD: case OP_LRP: return dm;
    default: return idx < 2 ? dm : 0;   /* two-source element-wise */
    }
}

static u32 fps_swz_lanes(u32 w, u32 comps)
{
    u32 lanes = 0;
    for (u32 i = 0; i < 4; i++)
        if (comps & (1u << i)) lanes |= 1u << ((w >> (FP_SRC_SWZ_SHIFT + 2 * i)) & 3u);
    return lanes;
}

/* Bit of SET_VERTEX_ATTRIB_OUTPUT_MASK (0x1FF4) and the VP output register
 * that feed fragment input `in`, or ~0u when it has none (WPOS, SSA). */
static u32 fps_input_outmask_bit(u32 in)
{
    if (in == 1) return 0x5u;          /* front | back diffuse */
    if (in == 2) return 0xAu;          /* front | back specular */
    if (in == 3) return 0x10u;         /* fog */
    if (in >= 4 && in <= 11) return 1u << (14 + in - 4);
    if (in == 12) return 1u << 12;     /* TEX8 */
    if (in == 13) return 1u << 13;     /* TEX9 */
    return ~0u;
}
static u32 fps_input_vp_reg(u32 in)
{
    switch (in) {
    case 1: return 1;
    case 2: return 2;
    case 3: return 5;
    case 12: return 15;
    case 13: return 6;
    default: return (in >= 4 && in <= 11) ? 7 + in - 4 : ~0u;
    }
}

void rsx_fp_stats_note(const u8* ucode, u32 max_bytes, u32 ctrl, const rsx_fp_stats_ctx* ctx)
{
    if (!ucode) return;
    s_fps.psos++;
    const u64 h = rsx_fp_structural_hash(ucode, max_bytes, 0x5354415453ull);
    int fresh = 0;
    if (h) {
        u32 slot = (u32)(h ^ (h >> 29)) & (FPS_SEEN - 1u), n = 0;
        while (s_fps.seen[slot] && s_fps.seen[slot] != h && n < FPS_SEEN) {
            slot = (slot + 1u) & (FPS_SEEN - 1u); n++;
        }
        if (n >= FPS_SEEN) s_fps.seen_full++;
        else if (!s_fps.seen[slot]) { s_fps.seen[slot] = h; fresh = 1; }
    }
    u32 op_seen[(FPS_OPS + 31) / 32] = { 0 };
    u32 fl_seen[(FPS_NFLAGS + 31) / 32] = { 0 };
    u32 in_seen = 0;
#define FPS_FLAG(f) do { if (fresh) { s_fps.fl_occ[f]++; fl_seen[(f) >> 5] |= 1u << ((f) & 31); } } while (0)
    /* Storage in 16-bit units: r[k].c is units k*8 + 2c, +1; h[n].c is unit
     * n*4 + c. Holds 'R' or 'H' for the width that wrote it last. */
    u8 unit_w[64 * 8];
    memset(unit_w, 0, sizeof unit_w);
    u32 in_lanes[16] = { 0 };
    u32 exp_units = 0;
    int wrote_t[2][FP_MAX_COLOR_TARGETS] = { { 0 } };
    const u32 xyz = ctx ? ctx->xyz_units : 0u;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        if (fresh) s_fps.instructions++;
        const int is_branch = (w2 & FP_BRANCH) != 0;
        const u32 op6 = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
        const u32 op = is_branch ? 0x40u + (op6 & 7u) : op6;
        if (fresh && op < FPS_OPS) { s_fps.op_occ[op]++; op_seen[op >> 5] |= 1u << (op & 31); }
        const int lt = (w1 & FP_EXEC_IF_LT) != 0, eq = (w1 & FP_EXEC_IF_EQ) != 0, gr = (w1 & FP_EXEC_IF_GR) != 0;
        const int uncond = lt && eq && gr, never = !lt && !eq && !gr;
        if (!uncond && !never) FPS_FLAG(FPS_PRED);
        if (never) FPS_FLAG(FPS_NEVER);
        if (!uncond && (w1 & FP_COND_REG)) FPS_FLAG(FPS_CCREAD1);
        if (is_branch) {
            if (op6 == 0x02) {
                FPS_FLAG(FPS_IFE);
                const u32 cs = (w1 >> FP_COND_SWZ_SHIFT) & 0xFFu;
                if (cs != (cs & 3u) * 0x55u) FPS_FLAG(FPS_IFE_SWZ);
            } else if (op6 == 0x03 || op6 == 0x04) FPS_FLAG(FPS_LOOP);
            else if (op6 == 0x00) FPS_FLAG(FPS_BRK);
            else FPS_FLAG(FPS_CALRET);
            if (w0 & FP_END) break;
            continue;
        }
        const int has_k = fp_instruction_has_constant(w1, w2, w3);
        const u32 dm = (w0 >> FP_OUT_MASK_SHIFT) & 0xFu ? (w0 >> FP_OUT_MASK_SHIFT) & 0xFu : 0xFu;
        const int no_dest = (w0 & FP_OUT_NONE) != 0, set_cc = (w0 & FP_COND_WRITE) != 0;
        const int dst_half = (w0 & FP_OUT_HALF) != 0;
        const u32 dst = (w0 & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
        const u32 in = (w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
        const u32 unit = (w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT;
        if (w0 & FP_OUT_SAT) FPS_FLAG(FPS_SAT);
        if ((w2 >> 28) & 7u) FPS_FLAG(FPS_SCALE);
        { const u32 dp = (w0 >> 22) & 3u;
          if (dp && !no_dest) FPS_FLAG(FPS_DPREC1 + dp - 1u); }
        if (set_cc) {
            FPS_FLAG(FPS_SETCC);
            if (no_dest) { FPS_FLAG(FPS_SETCC_NODEST); if (!uncond) FPS_FLAG(FPS_SETCC_NODEST_PRED); }
            if (w1 & FP_COND_MOD_REG) FPS_FLAG(FPS_CCWRITE1);
        }
        if (op == OP_KIL && !uncond) FPS_FLAG(FPS_KIL_PRED);
        if (no_dest && !set_cc && op != OP_KIL && op != OP_NOP && op != OP_FENCT && op != OP_FENCB)
            FPS_FLAG(FPS_NODEST);
        if (!no_dest && dst_half) FPS_FLAG(FPS_DST_HALF);
        if (!no_dest && !dst_half && dst >= 48) FPS_FLAG(FPS_DST_OOR);
        if ((w0 >> 21) & 1u) {
            if (op == OP_TEX || op == OP_TXP || op == OP_TXB || op == OP_TXL || op == OP_TXD ||
                op == OP_TEXBEM || op == OP_TXPBEM) { FPS_FLAG(FPS_EXPTEX); exp_units |= 1u << unit; }
        }
        if (w3 & 0x40000000u) FPS_FLAG(FPS_USEINDEX);
        const u32 ws[3] = { w1, w2, w3 };
        int persp_noted = 0;
        for (u32 si = 0; si < 3; si++) {
            const u32 comps = fps_src_comps(op, si, dm, (xyz >> unit) & 1u);
            if (!comps) continue;
            const u32 w = ws[si];
            const u32 type = w & FP_REG_TYPE_MASK;
            const u32 lanes = fps_swz_lanes(w, comps);
            const int abs = si == 0 ? (w1 & FP_SRC0_ABS) != 0 : si == 1 ? (w2 & FP_SRC1_ABS) != 0
                                    : (w3 & FP_SRC2_ABS) != 0;
            if (abs) FPS_FLAG(si == 2 ? FPS_SRC2ABS : FPS_ABS);
            if (w & FP_SRC_NEGATE) FPS_FLAG(FPS_NEG);
            { const u32 sp = (w2 >> (19u + 3u * si)) & 7u;
              if (sp) FPS_FLAG(sp >= 5 ? FPS_SPREC5 : FPS_SPREC1 + sp - 1u); }
            if (type == 3) FPS_FLAG(FPS_TYPE3);
            if (type == FP_REG_TYPE_INPUT) {
                in_seen |= 1u << in;
                in_lanes[in] |= lanes;
                if (fresh) s_fps.in_occ[in]++;
                if (in >= 4 && in <= 13 && (lanes & 0xCu)) FPS_FLAG(FPS_TC_ZW);
                if (in == 0xE) FPS_FLAG(FPS_SSA);
                if (in == 0 && (lanes & 0x8u)) FPS_FLAG(FPS_WPOS_W);
                if ((w3 & 0x80000000u) && in >= 4 && in <= 13 && !persp_noted) {
                    FPS_FLAG(FPS_PERSP); persp_noted = 1;
                }
            } else if (type == FP_REG_TYPE_TEMP) {
                const u32 r = (w & FP_SRC_REG_MASK) >> FP_SRC_REG_SHIFT;
                if (w & FP_SRC_HALF) {
                    FPS_FLAG(FPS_SRC_HALF);
                    for (u32 c = 0; c < 4; c++)
                        if ((lanes >> c) & 1u && r * 4u + c < sizeof unit_w &&
                            unit_w[r * 4u + c] == 'R') { FPS_FLAG(FPS_ALIAS_H_AFTER_R); break; }
                } else {
                    for (u32 c = 0; c < 4; c++)
                        if ((lanes >> c) & 1u && r * 8u + 2u * c + 1u < sizeof unit_w &&
                            (unit_w[r * 8u + 2u * c] == 'H' || unit_w[r * 8u + 2u * c + 1u] == 'H')) {
                            FPS_FLAG(FPS_ALIAS_R_AFTER_H); break;
                        }
                }
            }
        }
        if (!no_dest && op != OP_KIL && op != OP_NOP && op != OP_FENCT && op != OP_FENCB) {
            for (u32 c = 0; c < 4; c++) {
                if (!((dm >> c) & 1u)) continue;
                if (dst_half) { if (dst * 4u + c < sizeof unit_w) unit_w[dst * 4u + c] = 'H'; }
                else if (dst * 8u + 2u * c + 1u < sizeof unit_w)
                    unit_w[dst * 8u + 2u * c] = unit_w[dst * 8u + 2u * c + 1u] = 'R';
            }
            const int t = fp_export_target(dst, dst_half);
            if (t >= 0) wrote_t[dst_half][t] = 1;
        }
        if (has_k) off += 16;
        if (w0 & FP_END) break;
    }
    /* What the ROP reads: the export registers of the selected width, and
     * r1.z for a depth export; flag any last written through the other one. */
    const int half_exports = !(ctrl & FP_CTRL_32BIT_EXPORTS);
    u32 targets = 1;
    for (u32 t = FP_MAX_COLOR_TARGETS; t-- > 1; )
        if (wrote_t[half_exports][t]) { targets = t + 1; break; }
    if (!half_exports) FPS_FLAG(FPS_EXPORT32);
    if (targets >= 2) FPS_FLAG(FPS_MRT2 + targets - 2u);
    {
        u32 lanes_any = 0;   /* aliased lanes over all targets, bit per x..w */
        int any = 0, mrt = 0, all = 0;
        for (u32 t = 0; t < targets; t++) {
            const u32 reg = fp_export_reg[half_exports][t];
            u32 lanes = 0;
            for (u32 c = 0; c < 4; c++)
                if (half_exports ? unit_w[reg * 4u + c] == 'R'
                                 : unit_w[reg * 8u + 2u * c] == 'H')
                    lanes |= 1u << c;
            if (lanes) { any = 1; lanes_any |= lanes; if (t) mrt = 1; if (lanes == 0xFu) all = 1; }
        }
        if (any) {
            FPS_FLAG(FPS_EXPORT_ALIAS);
            FPS_FLAG(lanes_any == 0x8u ? FPS_EXPORT_ALIAS_ALPHA_ONLY : FPS_EXPORT_ALIAS_RGB);
            if (mrt) FPS_FLAG(FPS_EXPORT_ALIAS_MRT);
            if (all) FPS_FLAG(FPS_EXPORT_ALIAS_ALL);
        }
    }
    if ((ctrl & 0xEu) != 0u) {
        FPS_FLAG(FPS_DEPTH_EXPORT);
        if (unit_w[1 * 8 + 4] == 'H') { FPS_FLAG(FPS_EXPORT_ALIAS); FPS_FLAG(FPS_EXPORT_ALIAS_DEPTH); }
    }
#undef FPS_FLAG
    if (fresh) {
        s_fps.programs++;
        for (u32 i = 0; i < FPS_OPS; i++) if ((op_seen[i >> 5] >> (i & 31)) & 1u) s_fps.op_prog[i]++;
        for (u32 i = 0; i < FPS_NFLAGS; i++) if ((fl_seen[i >> 5] >> (i & 31)) & 1u) s_fps.fl_prog[i]++;
        for (u32 i = 0; i < 16; i++) if ((in_seen >> i) & 1u) s_fps.in_prog[i]++;
    }
    if (!ctx) return;
    /* Per pipeline: the program against the state it runs with. */
#define FPS_RUN(f, bits) do { s_fps.run[f]++; s_fps.run_bits[f] |= (bits); } while (0)
    u32 tc2d = 0, centroid = 0, masked = 0, unwritten = 0;
    for (u32 i = 4; i <= 13; i++) {
        if (!(in_seen & (1u << i))) continue;
        if (((ctx->tc2d_mask >> (i - 4)) & 1u) && (in_lanes[i] & 0xCu)) tc2d |= 1u << (i - 4);
        if ((ctx->centroid_mask >> (i - 4)) & 1u) centroid |= 1u << (i - 4);
    }
    for (u32 i = 1; i <= 13; i++) {
        if (!(in_seen & (1u << i))) continue;
        const u32 mb = fps_input_outmask_bit(i), vr = fps_input_vp_reg(i);
        if (mb != ~0u && !(ctx->vp_out_mask & mb)) masked |= 1u << i;
        if (vr != ~0u && !((ctx->vp_written >> vr) & 1u)) unwritten |= 1u << i;
    }
    if (tc2d) FPS_RUN(FPR_TC2D_ZW, tc2d);
    if (centroid) FPS_RUN(FPR_CENTROID, centroid);
    if (exp_units & ctx->float_units) FPS_RUN(FPR_EXP_FLOAT, exp_units & ctx->float_units);
    if (exp_units & ctx->zero_remap_units) FPS_RUN(FPR_EXP_ZERO_REMAP, exp_units & ctx->zero_remap_units);
    if (ctx->rt_count > targets) FPS_RUN(FPR_MRT_SHORT, 1u << ctx->rt_count);
    if (masked) FPS_RUN(FPR_OUTMASK_READ, masked);
    if (unwritten) FPS_RUN(FPR_VP_UNWRITTEN_READ, unwritten);
    if (ctx->two_side && (in_seen & 0x6u)) FPS_RUN(FPR_TWOSIDE_COLOR, in_seen & 0x6u);
    if (in_seen & (1u << 0xE)) FPS_RUN(FPR_SSA, 1u);
#undef FPS_RUN
}

void rsx_fp_stats_print(void)
{
    FILE* f = stderr;
    static const char* const in_names[16] = { "WPOS", "COL0", "COL1", "FOGC", "TC0", "TC1", "TC2",
        "TC3", "TC4", "TC5", "TC6", "TC7", "TC8", "TC9", "SSA", "IN15" };
    static const char* const br_names[6] = { "BRK", "CAL", "IFE", "LOOP", "REP", "RET" };
    fprintf(f, "[shader-stats] fp programs=%u instructions=%u pipelines=%u seen_full=%u"
               " (counts: occurrences/programs)\n",
            s_fps.programs, s_fps.instructions, s_fps.psos, s_fps.seen_full);
    char line[1024]; int n = 0;
    line[0] = '\0';
    for (u32 i = 0; i < FPS_OPS; i++) {
        if (!s_fps.op_occ[i]) continue;
        char name[16];
        if (i >= 0x40) snprintf(name, sizeof name, "%s", i - 0x40 < 6 ? br_names[i - 0x40] : "BR?");
        else if (rsx_fp_opcode_name(i)[0] == '?') snprintf(name, sizeof name, "op%02X", i);
        else snprintf(name, sizeof name, "%s", rsx_fp_opcode_name(i));
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%llu/%u", name, s_fps.op_occ[i], s_fps.op_prog[i]);
        if (n > 900) { fprintf(f, "[shader-stats] fp ops:%s\n", line); n = 0; line[0] = '\0'; }
    }
    if (n) fprintf(f, "[shader-stats] fp ops:%s\n", line);
    n = 0; line[0] = '\0';
    for (u32 i = 0; i < FPS_NFLAGS; i++) {
        if (!s_fps.fl_occ[i]) continue;
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%llu/%u", fps_flag_names[i], s_fps.fl_occ[i], s_fps.fl_prog[i]);
        if (n > 900) { fprintf(f, "[shader-stats] fp flags:%s\n", line); n = 0; line[0] = '\0'; }
    }
    if (n) fprintf(f, "[shader-stats] fp flags:%s\n", line);
    n = 0; line[0] = '\0';
    for (u32 i = 0; i < 16; i++)
        if (s_fps.in_occ[i])
            n += snprintf(line + n, sizeof line - (size_t)n, " %s=%llu/%u", in_names[i], s_fps.in_occ[i], s_fps.in_prog[i]);
    fprintf(f, "[shader-stats] fp inputs:%s\n", line);
    n = 0; line[0] = '\0';
    for (u32 i = 0; i < FPR_N; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%u(0x%X)", fps_run_names[i], s_fps.run[i], s_fps.run_bits[i]);
    fprintf(f, "[shader-stats] fp per-pipeline checks:%s\n", line);
}

/* How many register lanes LD_FP_EXPORT_GATHER and LD_FP_ALIAS_SYNC act on in
 * a program (16-bit exports assumed; 0 for a program with flow control, which
 * neither touches): reads of a temp lane last written through the other
 * register file, plus export lanes of h0/h4/h6/h8 last written through
 * r0/r2/r3/r4. For the live draw's per-draw count ([draws] "alias-fixed"). */
/* Are two fragment programs of `size` bytes the same apart from the values
 * of their inline constants? Every instruction slot is compared; only a slot
 * every walker here takes for an inline constant (after an instruction with a
 * CONST source that is not a branch) is skipped. A branch with a CONST-typed
 * source, after which the walkers here disagree on the next slot, counts as
 * different. Everything derived from the instructions alone (texture units,
 * WPOS, alias hazards, gamma units, the structural hash) is then the same
 * for both. */
int rsx_fp_same_instructions(const u8* a, const u8* b, u32 size)
{
    if (!a || !b) return 0;
    for (u32 off = 0; off + 16 <= size;) {
        if (memcmp(a + off, b + off, 16) != 0) return 0;
        const u32 w0 = rsx_fp_read_word(a + off), w1 = rsx_fp_read_word(a + off + 4);
        const u32 w2 = rsx_fp_read_word(a + off + 8), w3 = rsx_fp_read_word(a + off + 12);
        off += 16;
        if (fp_instruction_has_constant(w1, w2, w3)) {
            if (w2 & FP_BRANCH) return 0;
            off += 16;   /* the inline constant: may differ */
        }
        if (w0 & FP_END) return off == size;
    }
    return 0;
}

u32 rsx_fp_alias_hazards(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u8 lw[64 * 8];
    memset(lw, 0, sizeof lw);
    u32 n = 0;
    int wrote_t[FP_MAX_COLOR_TARGETS] = { 0 };
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off), w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8), w3 = rsx_fp_read_word(ucode + off + 12);
        if (w2 & FP_BRANCH) return 0;
        off += 16;
        const u32 ws[3] = { w1, w2, w3 };
        for (u32 si = 0; si < 3; si++) {
            if ((ws[si] & FP_REG_TYPE_MASK) != FP_REG_TYPE_TEMP) continue;
            const u32 r = (ws[si] & FP_SRC_REG_MASK) >> FP_SRC_REG_SHIFT;
            for (u32 c = 0; c < 4; c++) {
                if (ws[si] & FP_SRC_HALF) {
                    if (r * 4u + c < sizeof lw && lw[r * 4u + c] == 'R') { n++; lw[r * 4u + c] = 'B'; }
                } else if (r * 8u + 2u * c + 1u < sizeof lw &&
                           (lw[r * 8u + 2u * c] == 'H' || lw[r * 8u + 2u * c + 1u] == 'H')) {
                    n++; lw[r * 8u + 2u * c] = lw[r * 8u + 2u * c + 1u] = 'B';
                }
            }
        }
        if (!(w0 & FP_OUT_NONE)) {
            const u32 d = (w0 & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            const int h = (w0 & FP_OUT_HALF) != 0;
            const u32 dm = (w0 >> FP_OUT_MASK_SHIFT) & 0xFu ? (w0 >> FP_OUT_MASK_SHIFT) & 0xFu : 0xFu;
            for (u32 c = 0; c < 4; c++) {
                if (!((dm >> c) & 1u)) continue;
                if (h) { if (d * 4u + c < sizeof lw) lw[d * 4u + c] = 'H'; }
                else if (d * 8u + 2u * c + 1u < sizeof lw) lw[d * 8u + 2u * c] = lw[d * 8u + 2u * c + 1u] = 'R';
            }
            const int t = fp_export_target(d, h);
            if (t >= 0 && h) wrote_t[t] = 1;
        }
        if (fp_instruction_has_constant(w1, w2, w3)) off += 16;
        if (w0 & FP_END) break;
    }
    for (u32 t = 0; t < FP_MAX_COLOR_TARGETS; t++) {
        if (t && !wrote_t[t]) continue;
        const u32 reg = fp_export_reg[1][t];
        for (u32 c = 0; c < 4; c++)
            if (reg * 4u + c < sizeof lw && lw[reg * 4u + c] == 'R') n++;
    }
    return n;
}
