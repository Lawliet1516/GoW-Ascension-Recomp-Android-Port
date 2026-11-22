# gl_ClipDistance on the Adreno 710: independent repro (22/11/2026)

`clip_repro.c` (this directory) is a standalone Vulkan program: no RSX, no decompiler, no Ascension code. Hand-written GLSL (`shaders/`) compiled by `glslc --target-env=vulkan1.0`,
64x64 RGBA8 offscreen target cleared to blue, a solid red quad, pixels read back and judged. Build: `build.sh`. Run: as an executable on the phone (no layers) and inside the
debuggable APK (`com.gowa.scenes`, `--es args a:repro`) with `VK_LAYER_KHRONOS_validation` loaded.

Three vertex-shader variants x four cases:
- variants: `clip1` (`float gl_ClipDistance[1]`), `clip4all` (`[4]`, all equal), `clip4rsx` (`[4]` = 0.5, d, 0.5, 0.5: the shape the RSX decompiler emits);
- cases: **A** all distances +1 (everything drawn), **B** all -1 (nothing drawn), **C** d = -x (left half drawn, edge at column 32), **control** quad outside NDC (nothing drawn: proves the harness can see "no triangle").

## Result: 12/12 pass in both runs

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

## Interpretation

**The Adreno 710 driver (Vulkan 1.1.128, driverVersion 0x80267063) clips correctly with `gl_ClipDistance`**, including the all-negative case and the exact shape the RSX decompiler emits.
The statement in `r1_results.md` that "the driver ignores ClipDistance" was **wrong** and is withdrawn: it was inferred from the scene result without an independent control.
The failure of `--clip` is therefore in the integration (SPIR-V generation, pipeline state, or what the scene itself draws), **not proven to be the driver**.
The `varying + discard` fallback is **not justified by this evidence** and is not implemented.

## Limits
- One device, one driver build. 64x64, 1 sample, no depth, one primitive type (strip). Not tested here: MSAA, depth test, multiple planes with different values, interpolation at edges of a non-axis-aligned plane.
- The repro uses SPIR-V 1.0 from glslang (GLSL); the renderer's SPIR-V comes from glslang HLSL, which can differ (see the next step).
