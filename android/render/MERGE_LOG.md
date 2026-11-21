# Renderer migration log (branch `android/renderer-upstream`)

Pinned upstream: ps3recomp **v0.20.0** = `d81c393ee72db80ef89aa4464b23c466edda44b0`; the fork was cut from **v0.15.0** = `0fe7dbf5db9a467aca268865fb26170ea113c541`.
Everything lives in `render/` (ps3kit/libs/video is untouched, so the Windows production build is byte for byte what it was).

## Steps taken (each is its own commit; `git log -- render/` shows them)

| # | commit subject | what | reversible by |
|---|---|---|---|
| 1 | provenance | pinned refs `refs/upstream/ps3recomp/*` (read-only remote), analysis script | `git revert` |
| 2 | render/video: copy of ps3kit/libs/video | the fork's state, minus Windows/Apple-only files: the baseline every later diff is against | revert |
| 3 | take upstream for 20 untouched files | files the fork never changed vs v0.15.0 (engine header, texture layout, shader_spirv, vulkan header, ...) | revert |
| 4 | add 42 files new in v0.20.0 | present thread, fence/gpu wait, host surface, tiling, report clock, tests | revert |
| 5 | pure upstream for the 12 files both sides changed | see "Why wholesale" | revert; deltas kept in `ascension-deltas/` |
| 6 | render/include + upstream rsx_live_draw (stubs) | 7 new headers + 2 updated, searched before ps3kit/include | revert |
| 7 | mirror ps3kit layout | `render/libs/video`, `render/include`, `render/runtime -> ../ps3kit/runtime` | revert |
| 8 | render/host + CMake option | upstream's scene runner, `GOWA_RENDER_UPSTREAM` | revert |

**Why wholesale for the 12 shared files.** A three-way text merge was attempted first (`merge_file.py`): cellGcmSys.c 39 conflict hunks (up to 812 fork lines),
fp_decompiler 16, commands 7, vertex_compact 6, vp_decompiler 4 (resolved, then dropped for purity), dispatch 1, engine 2, vulkan 2. The conflicts are not adjacent edits: fork and upstream
rewrote the same code independently (vertex-fetch batching, register store bounds checks, decompiler statistics), so a merge compiles and silently mixes two designs.
The state after step 5 is exactly upstream, which makes the Android build checkable against the unmodified upstream reference (R0 below). The fork's changes are reapplied afterwards.

## Reapplication of the Ascension behaviour (not started; this is the inventory)

`android/render/ascension-deltas/<file>.patch` = upstream v0.15.0 -> this fork, per file (4,6k changed lines in total). Where the lines are:

| file | fork delta | what it contains (from the hunks' function names) | ledger / audit link |
|---|---|---|---|
| `cellGcmSys.c` | 2,279 | **50 hunks in `gcm_rsx_process_fifo_unlocked`** (the FIFO walker core), 10 in `nv3089_blit` (2D blits/resolves), 7 in `cellGcm_ref_on_poll` (references), `gcm_ref_push_at`/`gcm_ref_publish_one`, flip commands (`_cellGcmSetFlipCommand`, `gcm_flip_into_fifo`, `gcm_flip_request`), IO/EA mapping and offset tables, SPU PUT blocks (`gcm_note_spu_put`, `gcm_spu_block_for_put`), `ppu_gcm_pump`, FIFO resync diagnostics | audit section 2 (guest-visible sync points), `LD_WALKCUT*`, `W2_*` |
| `rsx_fp_decompiler.c/.h` | 1,148 | GoW fragment-shader correctness fixes (to be classified hunk by hunk) | `LD_FP_*`, `LD_SHADOW*`, `LD_HALF2*` |
| `rsx_vertex_compact.c/.h` | 1,292 | batch/SIMD/AVX2 vertex fetch, stable-span cache, stats | `LD_VTX_*` (performance; x86 only); upstream has its own batching |
| `rsx_dispatch.c/.h` | 487 | constant-generation counters (`g_rsx_dsp_const_gen`), register-store changes | `LD_CB_GEN`, `LD_CB_DEDUP` |
| `rsx_vp_decompiler.c/.h` | 298 | TEX8/TEX9 plumbing for the amber/terrain materials (a visible symptom), raw-input compaction, shader statistics | `LD_VTX_RAW`, `LD_SHADER_STATS` |
| `rsx_commands.c` | 133 | | |
| `rsx_draw_engine.c`, `rsx_vulkan_backend.c` | 2 / 6 | nothing but the SDL3 shim and the `rsx_vp_pointsize` macros | |

**Order of reapplication** (each: one commit, one test, tied to the ledger switch): (1) the walker's guest-visible points: labels/semaphores, references (`cellGcm_ref_on_poll`),
flips, SPU PUT blocks; (2) render-target write-back and the 1x1 luminance target (`LD_SURF_WB`, `LD_WB_DEFER`, `LD_WB_SYNC`: these live in `rsx_live_draw.c`, i.e. they
must be *written in the portable engine*, not copied); (3) vp/fp decompiler correctness fixes; (4) texture-cache/sampler behaviours the ledger marks default-on; (5) performance-only items (vertex SIMD, cb dedup).
A feature is never dropped because upstream's default is off: each ledger entry is looked up before the decision and the decision is written here.

## R0: the unmodified upstream baseline on the Adreno 710

`android/render/r0_scenes.sh` runs upstream's own scene suite (same list and pass criteria as its `linux-vulkan.yml`). Built from pristine upstream v0.20.0 plus the 32-line
Android portability patch `android-portability/upstream-v0.20.0-android.patch` (bionic has no `ucontext` and no `pthread_setaffinity_np`).
Result (21/11/2026, Adreno (TM) 710, Vulkan 1.1.128, no validation layer yet): **29 of 30 runs pass; `--clip` fails** (see `r0_results.md`).

## S3: the migrated tree reproduces the unmodified upstream (21/11/2026)

`render/libs/video` (pure upstream v0.20.0, steps 1-8) built with `android/CMakeLists.txt -DGOWA_RENDER_UPSTREAM=ON` (fork runtime, SDL2 static, glslang for arm64)
and run through the same 30 scene runs: **identical table to the pristine-upstream baseline** (`results/migrated_tree_table.md` vs `results/r0_table.md`): the same 29 pass and the same `--clip` failure.
All 18 upstream video-layer files compile against the fork's runtime headers once `render/include` (7 new + 2 updated upstream headers) is searched first.

**Runtime drift found while linking** (the video layer of v0.20.0 calls runtime functions that the fork's v0.15-based `ps3kit/runtime` does not have):
- `ps3_system_time_us` (upstream `runtime/syscalls/sys_timer.c`): trivial, shimmed in `android/host/render_runtime_shims.c` (a copy of the POSIX branch).
- `spu_taskset_round_busy` (upstream `runtime/spu/spu_workload.c`): lets the RSX hold its next report while SPURS "round" tasks are busy (`GCM_REPORT_SPU_HOLD_MS`);
  shimmed to 0 for the scene runner, which has no SPU tasksets. **For the real game this is a runtime migration**, not a shim: it is exactly the kind of RSX/SPURS
  interplay the Ascension walker depends on. The shim prints a warning if it is ever asked.
No other runtime symbol was missing. The bring-up stubs file no longer defines `psnr_*` (real code in the library) and `vm_base` is weak.

## Validation layers (state)

The Khronos validation layer **does load inside a debuggable APK** on this phone: `libVkLayer_khronos_validation.so` (Vulkan-ValidationLayers 1.4.363.1 android binaries, arm64) in the
app's `jniLibs` plus `setprop debug.vulkan.layers VK_LAYER_KHRONOS_validation` (the library is mapped in the app process, checked in `/proc/<pid>/maps`). A native executable cannot use it
(`vkCreateInstance` returns VK_ERROR_LAYER_NOT_PRESENT), so the scene runner must run inside the APK; that packaging is the next step. **Caution:** the property is global; it must be reset
afterwards (`setprop debug.vulkan.layers ''`, the empty string, with the quoting shown) or every native Vulkan executable on the phone fails. The scene results above are therefore *without* validation.
