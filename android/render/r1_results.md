> **CORRECTION (22/11/2026):** the "`--clip`: what is established" section below concluded that the driver ignores `ClipDistance`. An independent repro (`clip_repro/README_RESULTS.md`) shows the
> driver clips correctly (12/12), so that conclusion is **withdrawn**; the cause is **a SPIR-V declaration form** the driver mishandles (standalone `BuiltIn ClipDistance` variable), isolated in `clip_repro/README_RESULTS.md` ("Root cause"). **Fixed** (commit 4dbfe97): `--clip` now passes and the 30 runs are 30/30 with the validation layer (`results/r2_table.md`, "The fix and its verification" in `clip_repro/README_RESULTS.md`).

# R1: the 30 scene runs inside the APK with the Khronos validation layer (22/11/2026)

Adreno (TM) 710, Vulkan 1.1.128, `libVkLayer_khronos_validation.so` 1.4.363.1 (arm64), runner = `render/host` built as `libgowa_scenes.so`
(`gowa_scenes` CMake target, `-Dmain=gowa_host_main`) called through JNI from the debuggable APK `com.gowa.scenes` (`apk/`, built by `apk/build_apk.sh`, no Gradle).
Driver script: `r1_scenes_apk.sh` (resets `debug.vulkan.layers` on exit). Layer messages are read from logcat, tag `VALIDATION`.

**Positive control** (the layer really reports): `GOWA_VVL_CONTROL=1` makes the app call `vkCreateInstance` with a wrong `sType`; logcat shows
`Validation Error: [ VUID-VkInstanceCreateInfo-sType-sType ]`. Without it, "0 errors" would not mean anything.
(Mistakes met on the way, for the next person: the APK must contain the layer .so, else `vkCreateInstance` returns -6; the SDL2 `JNI_OnLoad` must not be exported;
`adb shell` drops empty arguments and `am` parses leading `--`, hence the `a:` + comma encoding of the scene arguments.)

## Result

**30 runs: 29 pass, `--clip` fails as in R0, and the layer reports 0 errors and 0 warnings in all 30** (table below).

| pass | scene | exit | engine path | VALIDATION errors | VALIDATION warnings | MISMATCH |
|---|---|---|---|---|---|---|
| fixed | default | 0 | no | 0 | 0 | 0 |
| fixed | --draw | 0 | no | 0 | 0 | 0 |
| fixed | --draw --frames=60 | 0 | no | 0 | 0 | 0 |
| fixed | --depth | 0 | no | 0 | 0 | 0 |
| fixed | --tex | 0 | no | 0 | 0 | 0 |
| fixed | --tex-io | 0 | no | 0 | 0 | 0 |
| fixed | --quads | 0 | no | 0 | 0 | 0 |
| fixed | --threads | 0 | no | 0 | 0 | 0 |
| fixed | --split-present | 0 | no | 0 | 0 | 0 |
| guest | default | 0 | yes | 0 | 0 | 0 |
| guest | --draw | 0 | yes | 0 | 0 | 0 |
| guest | --depth | 0 | yes | 0 | 0 | 0 |
| guest | --quads | 0 | yes | 0 | 0 | 0 |
| guest | --shader | 0 | yes | 0 | 0 | 0 |
| guest | --tex | 0 | yes | 0 | 0 | 0 |
| guest | --tex-io | 0 | yes | 0 | 0 | 0 |
| guest | --mip | 0 | yes | 0 | 0 | 0 |
| guest | --rtt | 0 | yes | 0 | 0 | 0 |
| guest | --depthtex | 0 | yes | 0 | 0 | 0 |
| guest | --mrt | 0 | yes | 0 | 0 | 0 |
| guest | --mrt-a | 0 | yes | 0 | 0 | 0 |
| guest | --cube | 0 | yes | 0 | 0 | 0 |
| guest | --depthbounds | 0 | yes | 0 | 0 | 0 |
| guest | --split-present | 0 | yes | 0 | 0 | 0 |
| guest | --feedback | 0 | yes | 0 | 0 | 0 |
| guest | --clip | 3 | yes | 0 | 0 | 1 |
| guest | --clip-keep | 0 | yes | 0 | 0 | 0 |
| guest | --float-rt | 0 | yes | 0 | 0 | 0 |
| guest | --float-rt-r | 0 | yes | 0 | 0 | 0 |
| guest | --unbound | 0 | yes | 0 | 0 | 0 |

("engine path = no" in the `fixed` pass is expected: those scenes do not use the register-file draw engine.)

## `--clip`: what is established

- The vertex SPIR-V is valid, `ClipDistance` is `float[4]` Output with `BuiltIn ClipDistance`, the feature `shaderClipDistance` is enabled, and the layer says nothing: **the API usage is not the cause**.
- Experiment (temporary change to `rsx_vp_apply_user_clip`, reverted, not committed): with every distance forced to `-1.0` (`Out.clipd0 = float4(-1,-1,-1,-1)`, confirmed in the dumped HLSL),
  and with a scalar `float clipd0` instead of `float4`, the triangle is still drawn (`0xFFFF0000`, same as R0). A correct implementation must clip everything in that case.
- ~~Conclusion: the Adreno 710 driver ignores `ClipDistance`~~ **withdrawn**: see the correction at the top. What remains true: the scene draws the triangle even with all distances forced to -1.
- Next: find why the renderer's pipeline does not clip although the driver can (SPIR-V/pipeline-state/scene differences versus `clip_repro`). The fallback is on hold.
