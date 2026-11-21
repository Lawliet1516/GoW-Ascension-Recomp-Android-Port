# Session summary (21/11/2026): Android native port of God of War: Ascension

Fork: https://github.com/Lawliet1516/GoW-Ascension-Recomp-Android-Port (base: bc160201983/GoW-Ascension-Recomp by BilalReloaded, MIT; ps3kit from sp00nznet/ps3recomp).
Test device: Redmi Note 13 Pro 5G (Snapdragon 7s Gen 2: cpu4-7 Cortex-A78, cpu0-3 A55, ARMv8.2, no SVE; Adreno 710, Vulkan 1.1.128).
Rules in force: own branches only, `main` protected (PR only), upstream never modified, nothing merged without validation, no EBOOT/lifted code to shared CI.

## Branches (all pushed)
| branch | content | state |
|---|---|---|
| `main` | upstream + README with credits and the title video | merged by PR #1 |
| `android/m0-audit` | runtime compiles for aarch64 (NEON/ucontext/timer shims) | done |
| `android/m1-cmake-sdl3` | SDL3 via `ps3kit/include/ps3_sdl.h`, Android CMake (`android/CMakeLists.txt`), VMX NEON, CPU = Cortex-A78 | done |
| `android/m2-link` | recursive libs glob, opt-in noisy bring-up stubs | done (stub fix for psnr/vm_base is only on renderer-upstream: cherry-pick `android/host/port_stubs.c`) |
| `android/exp-spu-fastfp` | **experimental** SPU float fast path (FPCR RZ\|FZ), benchmarks, RESULTS.md, docs/SPU_FLOAT.md, INTEGRATION.md | validated, NOT merged |
| `android/m3-app-swapchain` | Gradle+SDL3 app, Vulkan swapchain with lifecycle handling, `test_lifecycle.sh` | ran on device |
| `android/renderer-audit` | RENDERER_AUDIT.md, switch ledger (417 switches), audit script | report done |
| `android/lift-pipeline` | `android/lift/` prepare.sh (Linux port of prepare.ps1), build_lifted.sh, selftest.sh | ready, needs EBOOT |
| `android/renderer-upstream` | renderer migration to ps3recomp v0.20.0 in `render/` | in progress (latest) |

## Results so far (details: android/RESULTS.md, docs/*.md, android/render/*.md)
- **SPU float**: the NEON path was slow because of exactness bookkeeping (f64 emulation), not NEON. FPCR fast path (same design as the x86 one): 4-5x throughput over NEON, ~2.5x per GHz sustained, cooler.
  Bit-exact vs exact reference on 4.3M structured records x 6 ops (mixed lanes), reference vs independent exact-rational Python model, 4 mutants caught, real runtime ran a lifted SPU program on the phone, FPCR restored on stop/nested/threads.
  Facts: bionic longjmp does NOT restore FPCR; threads and signal handlers inherit it. Still experimental: needs game SPU jobs in a CHECK_MODE build before merging (see INTEGRATION.md).
- **PPU**: lifter emits portable C (no x86 intrinsics): no binary translation needed. NEON for 9 VMX helpers (bit-exact). PPU lifter conformance suite (1,202 cases / 1,533 checks) cross-compiled and passing on the A78. Guest barriers already lower to dmb.
- **Renderer audit**: production renderer = rsx_live_draw.c (D3D12, Windows only, 30.7k lines, ~71% Ascension-specific over upstream's 10.5k baseline). Portable engine + Vulkan backend exist upstream; the fork's copies were v0.15-era.
  Production engine batches draws and flushes at 12 reasons; the fork's Vulkan backend does submit+wait per draw (not viable on tile GPUs). Adreno 710: BC textures sampled natively, UMA memory, timeline semaphores, MAILBOX+FIFO, 60/90/120 Hz.
- **Migration** (`render/`, ps3kit untouched): pinned upstream v0.20.0 d81c393e (base v0.15.0 0fe7dbf5), refs under `refs/upstream/ps3recomp/*` (read-only). 12 shared files taken as pure upstream; Ascension deltas kept as `android/render/ascension-deltas/*.patch` (4.6k lines) to be reapplied feature by feature.
  The tree built from `render/` reproduces the pristine-upstream scene baseline exactly: 29 of 30 runs pass on the Adreno 710; `--clip` fails (SPIR-V valid, shaderClipDistance=1, cause unknown).
- **App**: first frame on screen at 60 fps; 13 suspend/resume cycles balanced; no memory growth. Rotation/lock/debug-key tests SKIPPED (HyperOS needs "USB debugging (security settings)").
- **Validation layers**: load in a debuggable APK (jniLibs + `debug.vulkan.layers`), not in native executables. ALWAYS reset the property afterwards: `adb shell "setprop debug.vulkan.layers ''"`. Not yet used for the scene runs.

## Next steps (in order)
1. Package the upstream scene runner in the APK and run the 30 scene runs with the validation layer; diagnose `--clip` (fallback idea: distances as varying + `discard`).
2. Reapply Ascension behaviour in `render/` one feature per commit, each with a test and a ledger link: walker guest-visible points (labels, semaphores, references `cellGcm_ref_on_poll`, flips, SPU PUT blocks), render-target write-back + 1x1 luminance (rewrite in the portable engine), decompiler fixes (TEX8 etc.), texture cache/sampler behaviour. Migrate the real `spu_taskset_round_busy` (currently a warning shim).
3. Backend restructuring with before/after measurements (the upstream already has present thread, VK_DRAW_BATCH, fence wait: measure first).
4. With the EBOOT (Europe BCES01741 1.00, decrypted, in `work/pristine/EBOOT.ELF`): `android/lift/selftest.sh`, `prepare.sh`, `build_lifted.sh` on a 16 GB+ machine (never on shared CI), then link PPU+SPU+renderer and measure.
5. When validated: decide merge of fastfp (INTEGRATION.md criteria) and convergence of `render/` with `ps3kit/libs/video`.

## Open items / caveats
- Nothing about the game has run: no lifted PPU/SPU of the game, no Ascension rendering. All performance numbers are microbenchmarks/scenes.
- Throttling: clocks on the phone wander 691-2400 MHz (+-25% run to run); use ratios per GHz.
- Phone is also the user's remote-control screen for this session: avoid taking it over; screenshots only when the app is in front.
- The old fork (MaChInEgUn3 based) is NOT deleted (authorised only after confirming the new one).
