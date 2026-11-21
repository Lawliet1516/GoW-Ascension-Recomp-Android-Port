# R0: unmodified upstream v0.20.0 on the Adreno 710 (21/11/2026)

Build: pristine ps3recomp v0.20.0 (`d81c393e`) + `android-portability/upstream-v0.20.0-android.patch` (32 lines: bionic has no `ucontext`, no `pthread_setaffinity_np`),
cross-compiled for arm64 with NDK r29, SDL2 2.32.10 and glslang 15.4.0 (HLSL front end) built for arm64. Device: Redmi Note 13 Pro 5G, Adreno (TM) 710, Vulkan 1.1.128.
Runner: `android/render/r0_scenes.sh` (the scenes and pass criteria of upstream's `.github/workflows/linux-vulkan.yml`). **Validation layers: not active in this run** (see below).

Table: `results/r0_table.md`. Summary: 9 fixed-function runs, 21 guest-program runs (HLSL -> SPIR-V with glslang **on the phone**, engine path):
**29 pass, 1 fails: `--clip` (exit 3)**. The `--depthbounds` scene really ran (the device has `depthBounds`); `shaderClipDistance`, `imageCubeArray`, `logicOp` are all supported.

## The failure: user clip planes (`--clip`)

- The scene draws the `--shader` triangle with a vertex program that writes user-clip distances so the whole triangle must be clipped. Expected the clear colour
  `0xFF101830`; the Adreno drew the triangle (`0xFFFF0000`). `--clip-keep` (the same scene with the plane keeping the triangle) passes, which is also what "clip distances ignored" would give.
- The vertex SPIR-V is **valid** (`spirv-val --target-env vulkan1.0`), declares `OpCapability ClipDistance`, and decorates the `SV_ClipDistance0` output `BuiltIn ClipDistance`
  (`results/clip_vs.spv`, source HLSL `results/clip_vs.hlsl`: `Out.clipd0 = float4(0.5, -(o[5].z), 0.5, 0.5)`). The backend enables `shaderClipDistance` (the device reports it).
- So it is either a driver behaviour (this is an old driver, Vulkan 1.1.128) or an API-usage problem that only the validation layer would show. **Not diagnosed yet.**
  Next steps in order: validation layer run (needs the in-APK runner), a minimal standalone repro, then (if the driver ignores it) the portable workaround: pass the distances as a
  varying and `discard` in the fragment shader instead of `SV_ClipDistance`.
- Why it matters for Ascension: user clip planes are RSX state the game can set; whether Ascension uses them is not known yet (to be checked with the real command stream).

## What this baseline is and is not

It is the reference the migrated tree must reproduce: the same binary of scenes must pass/fail identically when built from `render/` with the same sources. It does not
test any Ascension behaviour, and a passing scene is not proof of correctness (scenes check a pixel or a few).
