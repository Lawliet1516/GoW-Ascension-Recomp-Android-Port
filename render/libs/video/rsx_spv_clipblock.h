/* rsx_spv_clipblock.h -- SPIR-V post-pass: put a standalone ClipDistance output into a Block.
 *
 * glslang's HLSL front end declares SV_ClipDistance as a standalone Output variable decorated `BuiltIn ClipDistance`:
 *     OpDecorate %clipd0 BuiltIn ClipDistance        %clipd0 = OpVariable %ptr_Output_arr_float_N Output
 * Valid SPIR-V (spirv-val, Vulkan validation layers accept it), but the Qualcomm Adreno 710 driver (Vulkan 1.1.128) does not clip with it:
 * every distance is ignored. The same module with the array as a member of a Block struct clips correctly
 * (android/render/clip_repro/README_RESULTS.md, "Root cause": 36 controlled cases). This rewrites
 *     OpDecorate %v BuiltIn ClipDistance      ->   OpMemberDecorate %S 0 BuiltIn ClipDistance ; OpDecorate %S Block
 *     %v : ptr(Output, float[N])              ->   %v : ptr(Output, struct S { float[N] })
 *     OpAccessChain %v i...                   ->   OpAccessChain %v %c0 i...        (%c0 = int 0)
 *     OpStore/OpLoad %v ...  (whole array)    ->   OpAccessChain %t %v %c0 ; OpStore/OpLoad %t ...
 * The clipping semantics are unchanged (still fixed-function clipping); only the declaration form differs. Other drivers get an equally valid module.
 *
 * Fail-safe: if the module has a use of the variable this does not know how to rewrite, or already has a Block with BuiltIn members (a second one would be invalid),
 * the module is left UNCHANGED and the function returns 0.
 * Returns 1 when it rewrote the module, 0 when there was nothing to do or it declined. Vertex-stage Output variables only; CullDistance is not touched. */
#ifndef RSX_SPV_CLIPBLOCK_H
#define RSX_SPV_CLIPBLOCK_H

#include <vector>

namespace rsx_spv {

namespace detail {
enum : unsigned {
    OpName = 5, OpEntryPoint = 15, OpTypeInt = 21, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpFunctionCall = 57, OpVariable = 59,
    OpLoad = 61, OpStore = 62, OpCopyMemory = 63, OpCopyMemorySized = 64, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpPtrAccessChain = 67,
    OpDecorate = 71, OpMemberDecorate = 72, OpCopyObject = 83, OpFunction = 54
};
enum : unsigned { DecBlock = 2, DecBuiltIn = 11, BuiltInClipDistance = 3, ScOutput = 3 };
}

/* Rewrites `spv` in place. */
inline int put_clipdistance_in_block(std::vector<unsigned int>& spv)
{
    using namespace detail;
    if (spv.size() < 6 || spv[0] != 0x07230203u) return 0;

    /* ---- pass 1: find the variable and the types around it ---------------------------------------------------------------------------------- */
    unsigned var = 0;                                   /* the standalone ClipDistance variable (Output) */
    for (size_t i = 5; i < spv.size();) {
        const unsigned wc = spv[i] >> 16, op = spv[i] & 0xFFFFu;
        if (!wc || i + wc > spv.size()) return 0;
        /* a module already has a Block with BuiltIn members (Position in gl_PerVertex): a second one is invalid (at most one per entry point) -> decline */
        if (op == OpMemberDecorate && wc >= 5 && spv[i + 3] == DecBuiltIn) return 0;
        if (op == OpDecorate && wc == 4 && spv[i + 2] == DecBuiltIn && spv[i + 3] == BuiltInClipDistance) {
            if (var) return 0;                          /* two standalone ClipDistance decorations: not a shape this knows */
            var = spv[i + 1];
        }
        i += wc;
    }
    if (!var) return 0;                                 /* nothing standalone (already a Block member, or no clip distances) */

    unsigned ptr_ty = 0, var_at = 0;                    /* the variable's pointer type; index of its OpVariable */
    for (size_t i = 5; i < spv.size();) {
        const unsigned wc = spv[i] >> 16, op = spv[i] & 0xFFFFu;
        if (op == OpVariable && wc >= 4 && spv[i + 2] == var) {
            if (spv[i + 3] != ScOutput || wc != 4) return 0;   /* Input, or has an initializer: leave it */
            ptr_ty = spv[i + 1]; var_at = (unsigned)i;
        }
        i += wc;
    }
    if (!ptr_ty) return 0;
    unsigned arr_ty = 0;
    for (size_t i = 5; i < spv.size();) {
        const unsigned wc = spv[i] >> 16, op = spv[i] & 0xFFFFu;
        if (op == OpTypePointer && wc == 4 && spv[i + 1] == ptr_ty && spv[i + 2] == ScOutput) arr_ty = spv[i + 3];
        i += wc;
    }
    if (!arr_ty) return 0;

    /* an int32 type and a constant 0 of it (used as the struct-member index); created below when the module has none */
    unsigned int_ty = 0, c0 = 0;
    for (size_t i = 5; i < spv.size();) {
        const unsigned wc = spv[i] >> 16, op = spv[i] & 0xFFFFu;
        if (op == OpTypeInt && wc == 4 && spv[i + 2] == 32 && !int_ty) int_ty = spv[i + 1];
        i += wc;
    }
    if (int_ty)
        for (size_t i = 5; i < spv.size();) {
            const unsigned wc = spv[i] >> 16, op = spv[i] & 0xFFFFu;
            if (op == OpConstant && wc == 4 && spv[i + 1] == int_ty && spv[i + 3] == 0 && !c0) c0 = spv[i + 2];
            i += wc;
        }

    /* ---- pass 2: every use must be one this can rewrite ------------------------------------------------------------------------------------- */
    for (size_t i = 5; i < spv.size();) {
        const unsigned wc = spv[i] >> 16, op = spv[i] & 0xFFFFu;
        switch (op) {
        case OpFunctionCall: for (unsigned k = 4; k < wc; k++) if (spv[i + k] == var) return 0; break;
        case OpCopyMemory: case OpCopyMemorySized: if (spv[i + 1] == var || spv[i + 2] == var) return 0; break;
        case OpPtrAccessChain: if (wc > 3 && spv[i + 3] == var) return 0; break;
        case OpCopyObject: if (wc > 3 && spv[i + 3] == var) return 0; break;
        case OpAccessChain: case OpInBoundsAccessChain:
            for (unsigned k = 4; k < wc; k++) if (spv[i + k] == var) return 0;   /* as an index: nonsense; decline */
            break;
        default: break;
        }
        i += wc;
    }

    /* ---- build the rewritten module --------------------------------------------------------------------------------------------------------- */
    unsigned bound = spv[3];
    const unsigned s_ty = bound++, ps_ty = bound++;
    unsigned new_int = 0, new_c0 = 0;
    if (!int_ty) { new_int = bound++; int_ty = new_int; }
    if (!c0)     { new_c0 = bound++; c0 = new_c0; }

    std::vector<unsigned int> out;
    out.reserve(spv.size() + 64);
    out.insert(out.end(), spv.begin(), spv.begin() + 5);
    for (size_t i = 5; i < spv.size();) {
        const unsigned wc = spv[i] >> 16, op = spv[i] & 0xFFFFu;
        const unsigned int* w = &spv[i];
        if (op == OpDecorate && wc == 4 && w[1] == var && w[2] == DecBuiltIn && w[3] == BuiltInClipDistance) {
            out.push_back((5u << 16) | OpMemberDecorate); out.push_back(s_ty); out.push_back(0); out.push_back(DecBuiltIn); out.push_back(BuiltInClipDistance);
            out.push_back((3u << 16) | OpDecorate); out.push_back(s_ty); out.push_back(DecBlock);
        } else if (i == var_at) {
            /* the new types go right before the variable: arr_ty (defined earlier) is visible, and so is every type the module defines before its variables */
            if (new_int) { out.push_back((4u << 16) | OpTypeInt); out.push_back(new_int); out.push_back(32); out.push_back(1); }
            if (new_c0)  { out.push_back((4u << 16) | OpConstant); out.push_back(int_ty); out.push_back(new_c0); out.push_back(0); }
            out.push_back((3u << 16) | OpTypeStruct); out.push_back(s_ty); out.push_back(arr_ty);
            out.push_back((4u << 16) | OpTypePointer); out.push_back(ps_ty); out.push_back(ScOutput); out.push_back(s_ty);
            out.push_back((4u << 16) | OpVariable); out.push_back(ps_ty); out.push_back(var); out.push_back(ScOutput);
        } else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc >= 5 && w[3] == var) {
            out.push_back(((wc + 1u) << 16) | op); out.push_back(w[1]); out.push_back(w[2]); out.push_back(var); out.push_back(c0);
            for (unsigned k = 4; k < wc; k++) out.push_back(w[k]);
        } else if (op == OpStore && wc >= 3 && w[1] == var) {          /* whole-array store: go through a member access chain */
            const unsigned t = bound++;
            out.push_back((5u << 16) | OpAccessChain); out.push_back(ptr_ty); out.push_back(t); out.push_back(var); out.push_back(c0);
            out.push_back(((unsigned)wc << 16) | OpStore); out.push_back(t);
            for (unsigned k = 2; k < wc; k++) out.push_back(w[k]);
        } else if (op == OpLoad && wc >= 4 && w[3] == var) {           /* whole-array load */
            const unsigned t = bound++;
            out.push_back((5u << 16) | OpAccessChain); out.push_back(ptr_ty); out.push_back(t); out.push_back(var); out.push_back(c0);
            out.push_back(((unsigned)wc << 16) | OpLoad); out.push_back(w[1]); out.push_back(w[2]); out.push_back(t);
            for (unsigned k = 4; k < wc; k++) out.push_back(w[k]);
        } else {
            out.insert(out.end(), w, w + wc);
        }
        i += wc;
    }
    out[3] = bound;
    spv.swap(out);
    return 1;
}

} // namespace rsx_spv

#endif
