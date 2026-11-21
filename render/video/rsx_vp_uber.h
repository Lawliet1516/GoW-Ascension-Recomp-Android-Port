/* UBER3 VS (2026-11-12): the vertex ubershader -- one vertex shader per INPUT CLASS (which attributes the vertex
 * layout feeds, and which of them arrive as raw words) that runs the NV40 vertex program as data: the program's own
 * 16-byte instructions in a constant buffer (b3), decoded in the shader exactly as rsx_vp_decompiler.c decodes them
 * into text. Same helper functions, same expression per opcode, same store order, same epilogue; registers, outputs,
 * inputs and the position declared precise and compiled -Gis as the IEEE-strict specialised text is.
 * Made by host\uber3\tools\mkvsuber.py -- do not edit by hand. */
#ifndef PS3RECOMP_RSX_VP_UBER_H
#define PS3RECOMP_RSX_VP_UBER_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define RSX_VP_UBER_HDR 5u      /* rows (uint4) before the code: header, 16 input kinds */
#define RSX_VP_UBER_MAX 512u    /* instructions */
static const char s_vpu_head[] =
"struct VSOutput {\n"
"    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
"    float4 fog:FOG;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
"    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
"};\n"
"cbuffer VPConst : register(b0) {\n"
"    float4 vp_c[512];\n"
"    float4 vp_posscale;\n"
"    float4 vp_posoffset;\n"
"};\n"
"cbuffer VPUber : register(b3) {\n"
"    uint4 vu_hdr;\n"
"    uint4 vu_kind[4];\n"
"    uint4 vu_code[512];\n"
"};\n"
"float vp_mul1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n"
"float4 vp_mul(float4 a, float4 b) { return float4(vp_mul1(a.x, b.x), vp_mul1(a.y, b.y), vp_mul1(a.z, b.z), vp_mul1(a.w, b.w)); }\n"
"float vp_dot3(float4 a, float4 b) { float4 m = vp_mul(a, b); return m.x + m.y + m.z; }\n"
"float vp_dot4(float4 a, float4 b) { float4 m = vp_mul(a, b); return m.x + m.y + m.z + m.w; }\n"
"uint vb_sw(uint w) { return (w >> 24) | ((w >> 8) & 0xFF00u) | ((w << 8) & 0xFF0000u) | (w << 24); }\n"
"uint vb_h(uint4 w, uint c) { uint x = w[c >> 1]; return (c & 1u) ? ((((x >> 16) & 0xFFu) << 8) | (x >> 24)) : (((x & 0xFFu) << 8) | ((x >> 8) & 0xFFu)); }\n"
"uint vb_b(uint4 w, uint c) { return (w[c >> 2] >> ((c & 3u) * 8u)) & 0xFFu; }\n"
"float vb_f16(uint h) { uint s = (h >> 15) << 31; uint e = (h >> 10) & 31u; uint m = h & 1023u; return asfloat(e == 0u ? s : (e == 31u ? (s | 0x7F800000u | (m << 13)) : (s | ((e + 112u) << 23) | (m << 13)))); }\n"
"float vb_s16(uint h) { return (float)(asint(h << 16) >> 16); }\n"
"float4 vdec(uint4 w, uint t, uint n) {\n"
"    if (t == 6u) { uint x = vb_sw(w.x);\n"
"        return float4((float)(asint(x << 21) >> 21) / 1023.0, (float)(asint(x << 10) >> 21) / 1023.0,\n"
"                      (float)(asint(x) >> 22) / 511.0, 1.0); }\n"
"    float4 o = float4(0, 0, 0, 1);\n"
"    [unroll] for (uint c = 0u; c < 4u; c++) {\n"
"        if (c < n) {\n"
"            float v = 0.0;\n"
"            if (t == 2u) v = asfloat(vb_sw(w[c]));\n"
"            else if (t == 3u) v = vb_f16(vb_h(w, c));\n"
"            else if (t == 1u) v = vb_s16(vb_h(w, c)) / 32767.0;\n"
"            else if (t == 5u) v = vb_s16(vb_h(w, c));\n"
"            else if (t == 4u) v = (float)vb_b(w, c) / 255.0;\n"
"            else if (t == 7u) v = (float)vb_b(w, c);\n"
"            o[c] = v;\n"
"        }\n"
"    }\n"
"    return o;\n"
"}\n"
"bool vu_cmp(float x, uint cond) {\n"
"    if (cond == 1u) return x < 0.0;\n"
"    if (cond == 2u) return x == 0.0;\n"
"    if (cond == 3u) return x <= 0.0;\n"
"    if (cond == 4u) return x > 0.0;\n"
"    if (cond == 5u) return x != 0.0;\n"
"    if (cond == 6u) return x >= 0.0;\n"
"    return cond == 7u;\n"
"}\n"
"float4 vu_swz(float4 b, uint src, bool ab) {\n"
"    if (ab) b = abs(b);\n"
"    float4 s = float4(b[(src >> 14) & 3u], b[(src >> 12) & 3u], b[(src >> 10) & 3u], b[(src >> 8) & 3u]);\n"
"    if ((src >> 16) & 1u) s = -s;\n"
"    return s;\n"
"}\n"
"uint vu_kd(uint a) { return vu_kind[a >> 2][a & 3u]; }\n"
"#define VU_SRC(S, AB, OUT) { const uint _rt = (S) & 3u; float4 _b; \\\n"
"    if (_rt == 2u) _b = v[input_src]; \\\n"
"    else if (_rt == 3u) _b = vp_c[index_const ? ((const_src + (uint)ar[addr_sel][addr_swz]) & 511u) : (const_src & 511u)]; \\\n"
"    else _b = r[((S) >> 2) & 31u]; \\\n"
"    OUT = vu_swz(_b, (S), AB); }\n"
"VSOutput main(VSInput input) {\n"
"    PRECISE float4 v[16];\n"
"    [unroll] for (int _k=0;_k<16;_k++) v[_k]=float4(0,0,0,1);\n";
static const char s_vpu_body[] =
"    PRECISE float4 r[32]; PRECISE float4 o[16];\n"
"    [unroll] for (int _i=0;_i<32;_i++) r[_i]=(float4)0;\n"
"    [unroll] for (int _j=0;_j<16;_j++) o[_j]=float4(0,0,0,1);\n"
"    int4 ar[2]; ar[0] = (int4)0; ar[1] = (int4)0;\n"
"    PRECISE float4 cc[2]; cc[0] = (float4)0; cc[1] = (float4)0;\n"
"    int pc = 0; int cs[8]; int csp = 0;\n"
"    [unroll] for (int _q=0;_q<8;_q++) cs[_q]=0;\n"
"    const int n_ins = (int)vu_hdr.x;\n"
"    const uint br_base = vu_hdr.y;\n"
"    const bool cc_env = vu_hdr.z != 0u;\n"
"    [loop] for (int it = 0; it < 65536 && pc >= 0 && pc < n_ins; it++) {\n"
"        const uint4 d = vu_code[pc];\n"
"        const uint d0 = d.x, d1 = d.y, d2 = d.z, d3 = d.w;\n"
"        const uint vec_op = (d1 >> 22) & 0x1Fu, sca_op = (d1 >> 27) & 0x1Fu;\n"
"        const uint input_src = (d1 >> 8) & 0xFu, const_src = (d1 >> 12) & 0x3FFu;\n"
"        const bool index_const = ((d3 >> 1) & 1u) != 0u;\n"
"        const uint src0 = ((d2 >> 23) & 0x1FFu) | ((d1 & 0xFFu) << 9);\n"
"        const uint src1 = (d2 >> 6) & 0x1FFFFu;\n"
"        const uint src2 = ((d3 >> 21) & 0x7FFu) | ((d2 & 0x3Fu) << 11);\n"
"        const bool ab0 = ((d0 >> 21) & 1u) != 0u, ab1 = ((d0 >> 22) & 1u) != 0u, ab2 = ((d0 >> 23) & 1u) != 0u;\n"
"        const uint addr_sel = (d0 >> 24) & 1u, addr_swz = d0 & 3u;\n"
"        const bool sat = ((d0 >> 26) & 1u) != 0u;\n"
"        const uint cond = (d0 >> 10) & 7u;\n"
"        const bool test = ((d0 >> 13) & 1u) != 0u && cond != 7u;\n"
"        const uint cc_sel = (d0 >> 25) & 1u;\n"
"        const uint dst_tmp = (d0 >> 15) & 0x3Fu, dst_out = (d3 >> 2) & 0x1Fu, sca_dst_tmp = (d3 >> 7) & 0x3Fu;\n"
"        const bool vec_result = ((d0 >> 30) & 1u) != 0u;\n"
"        const uint vmask = (d3 >> 13) & 15u, smask = (d3 >> 17) & 15u;\n"
"        const bool b14 = ((d0 >> 14) & 1u) != 0u, b29 = ((d0 >> 29) & 1u) != 0u;\n"
"        const bool cc_update = b14 && b29;\n"
"        const bool cc_either = cc_env && (b14 || b29);\n"
"        if (vmask != 0u && vec_op != 0u) {\n"
"            float4 A, B, C;\n"
"            VU_SRC(src0, ab0, A)\n"
"            VU_SRC(src1, ab1, B)\n"
"            VU_SRC(src2, ab2, C)\n"
"            PRECISE float4 _v = (float4)0;\n"
"            bool handled = true;\n"
"            switch (vec_op) {\n"
"            case 1u: _v = A; break;\n"
"            case 2u: _v = vp_mul((A),(B)); break;\n"
"            case 3u: _v = (A)+(C); break;\n"
"            case 4u: _v = vp_mul((A),(B))+(C); break;\n"
"            case 5u: _v = (float4)(vp_dot3((A),(B))); break;\n"
"            case 6u: _v = (float4)((vp_dot3((A),(B))+(B).w)); break;\n"
"            case 7u: _v = (float4)(vp_dot4((A),(B))); break;\n"
"            case 8u: _v = (float4)(float4(1,(A).y*(B).y,(A).z,(B).w)); break;\n"
"            case 9u: _v = (float4)(min((A),(B))); break;\n"
"            case 10u: _v = (float4)(max((A),(B))); break;\n"
"            case 11u: _v = (float4)((float4)((A)<(B))); break;\n"
"            case 12u: _v = (float4)((float4)((A)>=(B))); break;\n"
"            case 14u: _v = (float4)(frac(A)); break;\n"
"            case 15u: _v = (float4)(floor(A)); break;\n"
"            case 16u: _v = (float4)((float4)((A)==(B))); break;\n"
"            case 17u: _v = (float4)((float4)0); break;\n"
"            case 18u: _v = (float4)((float4)((A)>(B))); break;\n"
"            case 19u: _v = (float4)((float4)((A)<=(B))); break;\n"
"            case 20u: _v = (float4)((float4)((A)!=(B))); break;\n"
"            case 21u: _v = (float4)(float4(1,1,1,1)); break;\n"
"            case 22u: _v = (float4)(sign(A)); break;\n"
"            case 25u: _v = (float4)(float4(0,0,0,0)); break;\n"
"            case 13u: {\n"
"                int4 _a = (int4)floor(A);\n"
"                [unroll] for (uint c = 0u; c < 4u; c++) if ((vmask >> (3u - c)) & 1u) ar[addr_sel][c] = _a[c];\n"
"                handled = false;\n"
"                break; }\n"
"            default: handled = false; break;\n"
"            }\n"
"            if (handled) {\n"
"                if (sat) _v = saturate(_v);\n"
"                const bool to_o = vec_result && dst_out != 0x1Fu;\n"
"                const bool to_r = dst_tmp != 0x3Fu;\n"
"                const bool to_cc = cc_update || (cc_either && !to_o && !to_r);\n"
"                [unroll] for (uint c = 0u; c < 4u; c++) if ((vmask >> (3u - c)) & 1u) {\n"
"                    bool w = true;\n"
"                    if (test) w = vu_cmp(cc[cc_sel][(d0 >> (8u - 2u * c)) & 3u], cond);\n"
"                    if (w) {\n"
"                        if (to_o) o[dst_out & 15u][c] = _v[c];\n"
"                        if (to_r) r[dst_tmp & 31u][c] = _v[c];\n"
"                    }\n"
"                }\n"
"                if (to_cc) {\n"
"                    [unroll] for (uint c2 = 0u; c2 < 4u; c2++) if ((vmask >> (3u - c2)) & 1u) {\n"
"                        bool w = true;\n"
"                        if (test) w = vu_cmp(cc[cc_sel][(d0 >> (8u - 2u * c2)) & 3u], cond);\n"
"                        if (w) cc[cc_sel][c2] = _v[c2];\n"
"                    }\n"
"                }\n"
"            }\n"
"        }\n"
"        if (smask != 0u && sca_op != 0u) {\n"
"            float4 C;\n"
"            VU_SRC(src2, ab2, C)\n"
"            PRECISE float4 _v = (float4)0;\n"
"            bool handled = true;\n"
"            switch (sca_op) {\n"
"            case 1u: _v = C; break;\n"
"            case 2u: _v = (float4)((1.0/(C).x)); break;\n"
"            case 3u: _v = (float4)(clamp(1.0/(C).x, 5.42101e-20, 1.884467e19)); break;\n"
"            case 4u: _v = (float4)(rsqrt(max(abs((C).x), 1e-10))); break;\n"
"            case 5u: _v = (float4)(exp2((C).x)); break;\n"
"            case 6u: _v = (float4)(log2(max(abs((C).x), 1e-10))); break;\n"
"            case 13u: _v = (float4)(log2(max(abs((C).x), 1e-10))); break;\n"
"            case 14u: _v = (float4)(exp2((C).x)); break;\n"
"            case 15u: _v = (float4)(sin((C).x)); break;\n"
"            case 16u: _v = (float4)(cos((C).x)); break;\n"
"            case 7u: _v = (float4)(float4(1.0, max((C).x, 0.0), ((C).x > 0.0) ? exp2(clamp((C).w, -127.996, 127.996) * log2(max((C).y, 1e-10))) : 0.0, 1.0)); break;\n"
"            default: handled = false; break;\n"
"            }\n"
"            if (handled) {\n"
"                if (sat) _v = saturate(_v);\n"
"                const bool to_o = !vec_result && dst_out != 0x1Fu;\n"
"                const bool to_r = sca_dst_tmp != 0x3Fu;\n"
"                const bool to_cc = cc_either && !to_o && !to_r;\n"
"                [unroll] for (uint c = 0u; c < 4u; c++) if ((smask >> (3u - c)) & 1u) {\n"
"                    bool w = true;\n"
"                    if (test) w = vu_cmp(cc[cc_sel][(d0 >> (8u - 2u * c)) & 3u], cond);\n"
"                    if (w) {\n"
"                        if (to_o) o[dst_out & 15u][c] = _v[c];\n"
"                        if (to_r) r[sca_dst_tmp & 31u][c] = _v[c];\n"
"                    }\n"
"                }\n"
"                if (to_cc) {\n"
"                    [unroll] for (uint c2 = 0u; c2 < 4u; c2++) if ((smask >> (3u - c2)) & 1u) {\n"
"                        bool w = true;\n"
"                        if (test) w = vu_cmp(cc[cc_sel][(d0 >> (8u - 2u * c2)) & 3u], cond);\n"
"                        if (w) cc[cc_sel][c2] = _v[c2];\n"
"                    }\n"
"                }\n"
"            }\n"
"        }\n"
"        const int next = pc + 1;\n"
"        if (sca_op == 8u || sca_op == 9u || sca_op == 10u || sca_op == 11u || sca_op == 12u || sca_op == 17u || sca_op == 18u) {\n"
"            const uint t = (((d0 >> 23) & 1u) << 9) | ((d2 & 0x3Fu) << 3) | ((d3 >> 29) & 7u);\n"
"            const int tgt = (int)(t >= br_base ? t - br_base : t);\n"
"            bool cnd = cond == 7u;\n"
"            if (cond != 7u && cond != 0u) cnd = vu_cmp(cc[cc_sel][(d0 >> 8) & 3u], cond);\n"
"            if (sca_op == 9u || sca_op == 8u) pc = cnd ? tgt : next;\n"
"            else if (sca_op == 10u || sca_op == 11u) {\n"
"                if (cnd) { cs[csp] = next; csp = min(csp + 1, 7); pc = tgt; } else pc = next;\n"
"            } else if (sca_op == 12u) {\n"
"                if (cnd) { if (csp > 0) { csp--; pc = cs[csp]; } else pc = -1; } else pc = next;\n"
"            } else pc = next;\n"
"        } else pc = next;\n"
"    }\n"
"    VSOutput Out;\n"
"    PRECISE float4 _p = o[0];\n"
"    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
"    Out.col0 = o[1]; Out.col1 = o[2]; Out.fog = o[5].xxxx;\n"
"    Out.t0=o[7];  Out.t1=o[8];  Out.t2=o[9];  Out.t3=o[10];\n"
"    Out.t4=o[11]; Out.t5=o[12]; Out.t6=o[13]; Out.t7=o[14];\n"
"    Out.t8=o[15]; Out.t9=o[6];\n"
"#ifdef VU_NUDGE\n"
"    if ((asuint(Out.pos.z) & 0x7FFFFFFFu) != 0u) Out.pos.z = asfloat(asuint(Out.pos.z) - 1u);\n"
"#endif\n"
"    return Out;\n"
"}\n";
/* The table: row 0 = (instructions, branch base, LD_VP_CC_EITHER, 0), rows 1-4 = raw kind per attribute
 * (type * 8 + size; 0 = a float4 input), then the instructions as they are. Returns the rows, 0 = not taken. */
static u32 rsx_vp_uber_table(const u8* uc, u32 instrs, u32 base, const u8* raw_kind16, u32 (*out)[4])
{
    static int either = -1;
    if (either < 0) { const char* e = getenv("LD_VP_CC_EITHER"); either = !(e && e[0] == '0'); }
    if (!uc || !instrs || instrs > RSX_VP_UBER_MAX) return 0;
    out[0][0] = instrs; out[0][1] = base; out[0][2] = (u32)either; out[0][3] = 0;
    for (u32 a = 0; a < 16; a++) out[1u + (a >> 2)][a & 3u] = raw_kind16 ? raw_kind16[a] : 0u;
    memcpy(out[RSX_VP_UBER_HDR], uc, (size_t)instrs * 16u);   /* little-endian words, as the decompiler reads them */
    return RSX_VP_UBER_HDR + instrs;
}
/* The shader text of one input class. compact 0: all 16 attributes as float4 (the decompiler's plain form);
 * 1: only the attributes in mask (none: SV_VertexID, as the decompiler declares), raw_mask = those that arrive as
 * uint4 words. precise 0 = TEST ONLY (nothing declared precise); 2 = TEST ONLY (the negative control: the clip z one
 * float step nearer). Returns the length, 0 = no room. */
static u32 rsx_vp_uber_hlsl(int compact, u32 mask, u32 raw_mask, int precise, char* out, u32 size)
{
    u32 n = 0;
#define VPU_PUT(...) do { if (n + 160u >= size) return 0; n += (u32)snprintf(out + n, size - n, __VA_ARGS__); } while (0)
    if (!compact) { mask = 0xFFFFu; raw_mask = 0; }
    VPU_PUT("#define PRECISE %s\n", precise ? "precise" : "");
    if (precise == 2) VPU_PUT("#define VU_NUDGE 1\n");
    VPU_PUT("struct VSInput {\n");
    if (compact && !(mask & 0xFFFFu)) VPU_PUT("    uint vertex_id:SV_VertexID;\n");
    for (u32 a = 0; a < 16; a++)
        if (mask & (1u << a)) VPU_PUT((raw_mask >> a) & 1u ? "    uint4 a%u:ATTR%u;\n" : "    float4 a%u:ATTR%u;\n", a, a);
    VPU_PUT("};\n");
    if (n + sizeof s_vpu_head >= size) return 0;
    memcpy(out + n, s_vpu_head, sizeof s_vpu_head - 1u); n += (u32)sizeof s_vpu_head - 1u;
    for (u32 a = 0; a < 16; a++) {
        if (!(mask & (1u << a))) continue;
        if ((raw_mask >> a) & 1u) VPU_PUT("    v[%u]=vdec(input.a%u, vu_kd(%uu) >> 3, vu_kd(%uu) & 7u);\n", a, a, a, a);
        else VPU_PUT("    v[%u]=input.a%u;\n", a, a);
    }
    if (n + sizeof s_vpu_body >= size) return 0;
    memcpy(out + n, s_vpu_body, sizeof s_vpu_body); n += (u32)sizeof s_vpu_body - 1u;
#undef VPU_PUT
    return n;
}
#endif
