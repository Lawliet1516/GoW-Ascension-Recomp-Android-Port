# gl_ClipDistance on the Adreno 710: independent repro (22/11/2026)

`clip_repro.c` (this directory) is a standalone Vulkan program: no RSX, no decompiler, no Ascension code. Hand-written GLSL (`shaders/`) compiled by `glslc --target-env=vulkan1.0`,
64x64 RGBA8 offscreen target cleared to blue, a solid red quad, pixels read back and judged. Build: `build.sh`. Run: as an executable on the phone (no layers) and inside the
debuggable APK (`com.gowa.scenes`, `--es args a:repro`) with `VK_LAYER_KHRONOS_validation` loaded.

Three vertex-shader variants x four cases:
- variants: `clip1` (`float gl_ClipDistance[1]`), `clip4all` (`[4]`, all equal), `clip4rsx` (`[4]` = 0.5, d, 0.5, 0.5: the shape the RSX decompiler emits);
- cases: **A** all distances +1 (everything drawn), **B** all -1 (nothing drawn), **C** d = -x (left half drawn, edge at column 32), **control** quad outside NDC (nothing drawn: proves the harness can see "no triangle").

## Result of the first repro (GLSL only): 12/12 pass in both runs

| evidence | where |
|---|---|
| device, `shaderClipDistance` reported (1), limits (maxClipDistances 8), flag enabled in the struct passed to `vkCreateDevice` (1) | `results/clip_repro/run_executable_no_layer.txt` (first lines) |
| Android 16 (SDK 36), build, board | `results/clip_repro/device.txt` |
| SPIR-V disassembly: `OpCapability ClipDistance`, `BuiltIn ClipDistance` | `results/clip_repro/clip*.vert.dis.txt` |
| values written per vertex (x, y, d) | `WRITTEN` lines in both run files |
| visual result per case (pixel counts, first non-red column) | `PASS/FAIL` lines |
| validation layer: loaded (`Loaded layer VK_LAYER_KHRONOS_validation`), 0 messages | `run_apk_validation_layer*.txt` |

Per the brief, "0 validation messages" is **not** used as evidence that rendering is right: the evidence is the pixels. The layer was shown to report in the previous step
(positive control, `r1_results.md`).

## Interpretation (first repro, superseded by the section "Root cause" below where it differs)

**The Adreno 710 driver (Vulkan 1.1.128, driverVersion 0x80267063) clips correctly with `gl_ClipDistance`**, including the all-negative case and the exact shape the RSX decompiler emits.
The statement in `r1_results.md` that "the driver ignores ClipDistance" was **wrong** and is withdrawn: it was inferred from the scene result without an independent control.
The failure of `--clip` is therefore in the integration (SPIR-V generation, pipeline state, or what the scene itself draws), **not proven to be the driver**.
The `varying + discard` fallback is **not justified by this evidence** and is not implemented.

## Limits
- One device, one driver build. 64x64, 1 sample, no depth, one primitive type (strip). Not tested here: MSAA, depth test, multiple planes with different values, interpolation at edges of a non-axis-aligned plane.
- The repro uses SPIR-V 1.0 from glslang (GLSL); the renderer's SPIR-V comes from glslang HLSL, which can differ (see the next step).

---

# Root cause: `ClipDistance` declared as a standalone output variable (22/11/2026, second round)

## 1. SPIR-V of the renderer versus the repro that works
The renderer's vertex SPIR-V for `--clip` was captured **as handed to `vkCreateShaderModule`** (`PS3RECOMP_VK_SPV_DUMP`, commit 11d3134): 1903 words, hash `095007ee8a1baa92`, and the log says
`pipeline slot 1 = vs 095007ee8a1baa92 + fs d2120ca2acddd762`, so **the module in the pipeline of the scene is the one inspected** (it is also byte-identical to the earlier host-side `clip_vs.spv`).
Disassembly: `results/clip_repro/renderer_scene_clip_vs.dis.txt` (renderer, glslang 11 HLSL front end) versus `clip4rsx.vert.dis.txt` (GLSL repro that clips correctly).

| aspect | repro GLSL (works) | renderer HLSL (fails) | semantic impact |
|---|---|---|---|
| `OpCapability ClipDistance` | yes | yes | none |
| array type | `float[4]` | `float[4]` | none |
| where `BuiltIn ClipDistance` is attached | `OpMemberDecorate %gl_PerVertex 1` (member of a `Block` struct) | `OpDecorate %_entryPointOutput_clipd0 BuiltIn ClipDistance` (the variable itself) | **tested below: this is the cause** |
| `BuiltIn Position` | member of the same Block | standalone variable | tested below: no impact |
| stores | `OpAccessChain %_ %int_1 %int_k` | `OpAccessChain %_entryPointOutput_clipd0 %int_k` via a function-local temp | tested below: no impact |
| values written | (0.5, d, 0.5, 0.5) | same (`float4(0.5, -(o[5].z), 0.5, 0.5)` in the scene) | none |

## 2. Controlled isolation (same logic, only the declaration form changes)
Hand-assembled SPIR-V (`tools/gen_spvasm.py`, `spirv-as`, all `spirv-val --target-env vulkan1.0` valid) plus an HLSL shader compiled by the HLSL front end without any RSX code
(`shaders/hlsl_min.vs.hlsl`, `hlsl_rsx.vs.hlsl`: the same 14-varying output struct and a trailing `float4 SV_ClipDistance0`). 9 variants x 4 cases = 36 cases, same pattern in the executable and in the APK with the layer:

| variant | A all +1 | B all -1 | C d=-x | control |
|---|---|---|---|---|
| GLSL `clip1`, `clip4all`, `clip4rsx` (Block) | pass | pass | pass | pass |
| asm `block` (Position + ClipDistance in one Block) | pass | pass | pass | pass |
| asm `pos_solo` (ClipDistance in a Block, Position standalone) | pass | pass | pass | pass |
| asm `clip_solo` (Position in a Block, **ClipDistance standalone**) | pass | **FAIL** (all 4096 px drawn) | **FAIL** | pass |
| asm `both_solo` (both standalone, direct stores) | pass | **FAIL** | **FAIL** | pass |
| HLSL minimal / HLSL rsx-shaped (both standalone, from the HLSL front end) | pass | **FAIL** | **FAIL** | pass |

**Cause (reproducible, 28/36 pass, 8 fail, all 8 with the same single property):** on the Adreno 710 (Vulkan 1.1.128, driverVersion 0x80267063, Android 16) a vertex shader whose
`ClipDistance` output is a *standalone* `Output` variable decorated `BuiltIn ClipDistance` is **not clipped** (the distances are ignored); the identical shader with `ClipDistance`
as a member of a `gl_PerVertex`-style `Block` clips correctly. The form of `Position`, and the way the stores are written, make no difference.

What is established and what is not:
- Established: the pass/fail split follows exactly that declaration form, in 36 controlled cases, on this device and driver build, with and without the validation layer.
- The failing modules are accepted by `spirv-val --target-env vulkan1.0` and produce no validation message; this is not used as proof of anything (the evidence is the pixels).
- Not established: whether other Adreno drivers behave the same, and whether the Vulkan specification requires the Block form (the standalone form is valid SPIR-V and is what glslang's HLSL front end emits;
  the spec text was not re-read for this report). Calling it a driver defect is therefore a reading of the evidence, not a ruling.

## 3. Consequence for the fix
The `varying + discard` fallback is **not needed**: the driver does clip, provided the module declares `ClipDistance` inside a `Block`. The fix is on the SPIR-V that the renderer produces
(rewrite the standalone `ClipDistance` variable into a Block member), which preserves the clipping semantics exactly (still hardware clipping, no extra fragment work, no interpolation differences).

---

# The fix and its verification (22/11/2026)

**Fix** (commit 4dbfe97): `render/libs/video/rsx_spv_clipblock.h`, called from `translate()` in `rsx_shader_spirv.cpp` for vertex shaders, right after glslang produced the SPIR-V (so the result is also what
the translation cache stores). It rewrites the standalone `BuiltIn ClipDistance` output variable into a one-member `Block` struct (`OpMemberDecorate`, `OpDecorate Block`, a new struct and pointer type, `%int_0` as the
first index of every access chain, and an access chain in front of a whole-array `OpStore`/`OpLoad`). Clipping stays fixed-function; only the declaration form changes. It is fail-safe: a module with an unknown use of the variable,
a second `ClipDistance` decoration, or an existing Block with `BuiltIn` members (a second Block is invalid SPIR-V) is returned unchanged. `PS3RECOMP_SPV_CLIPBLOCK=0` disables it (used for the A/B below).

| check | result | evidence |
|---|---|---|
| host: the module of the scene (`095007ee8a1baa92`), both HLSL shaders, both hand-assembled standalone shaders and a six-distance shader are rewritten and `spirv-val --target-env vulkan1.0` valid; modules that are already Block-form, have no clip, or are fragment shaders come back byte-identical | pass | `results/clip_repro/post_pass_host_test.txt` |
| device, independent harness, executable and APK+layer (same pattern): standalone variants fail (12 cases), **every FIXED variant clips 4/4**, including the whole-array-store form and the six-distance shader with only plane 5 tested | 52/64 pass; the 12 failures are exactly the unfixed standalone variants | `run_executable_no_layer.txt`, `run_apk_validation_layer.txt` |
| device, renderer scene, A/B in the same APK: `--clip` with the post-pass off `0xFFFF0000` MISMATCH, on `0xFF101830` OK; `--clip-keep` OK in both | pass | `scene_ab_clipblock.txt` |
| device, the 30 scene runs with the validation layer (positive control in `r1_results.md`) | **30/30 pass**, 0 validation errors, 0 warnings, `debug.vulkan.layers` reset | `results/r2_table.md` |

## What this does not show
- The scene checks one pixel, on one primitive. **Not tested**: partial clipping through the renderer (the repro does test it, case C, 2048/4096 px with the edge at column 32), several planes with different values in the renderer,
  interpolation at plane edges for an oblique plane, depth, MSAA. The post-pass does not touch the position, interpolants, depth or sample state, but that is an argument, not a test.
- One device, one driver build (Adreno 710, Vulkan 1.1.128, driverVersion 0x80267063, Android 16). Other Adreno drivers may or may not need this; the Block form is valid everywhere.
- The post-pass applies to the `--clip` module shape the HLSL front end emits (glslang 11 here). A different glslang version that changes the form falls into the fail-safe (unchanged), and the A/B would show it.
- The failing form is accepted by `spirv-val` and the layer; whether the specification requires the Block form was not re-read. "Driver defect" remains a reading of the evidence.
