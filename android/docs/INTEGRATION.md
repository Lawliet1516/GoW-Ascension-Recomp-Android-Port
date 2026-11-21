# Integrating the FPCR float fast path into the stable branch: criteria and plan

State: `SPU_A64_FASTFP` lives only on `android/exp-spu-fastfp`. Stable branches (`android/m1-cmake-sdl3`, `android/m2-link`) have the
CMake option `GOWA_SPU_FASTFP` **absent/OFF**, so they build and behave exactly like upstream's ARM64 helpers; the scalar helpers
(`-DSPU_SCALAR_HELPERS`) and upstream's NEON helpers stay available everywhere. Nothing is merged into `main` and upstream
(`bc160201983/GoW-Ascension-Recomp`, `sp00nznet/ps3recomp`) is not touched.

## Already satisfied (see ../RESULTS.md, sections 4-5)

- bit-exact against the exact reference on 4.3 M structured records x 6 ops with mixed lanes; reference checked against an independent model;
- the test is able to fail (4 mutants caught, negative control);
- FPCR restored on every exit path that could be exercised: normal return, `stop` halt, odd caller FPCR, nested, new threads, 4 threads x 20,000 runs;
- misuse is detectable (`SPU_A64_CHECK_MODE`);
- measured gain on representative kernels and under sustained load, at lower temperature.

## Must still hold before merging into a stable branch

1. **Game SPU jobs.** Run the real lifted SPU code of the game (needs the user's `EBOOT.elf`) in a `SPU_A64_CHECK_MODE` build: zero
   `[spu-fp] FATAL` over a full session. This is the only way to find an SPU entry point that runs outside `spu_run_with_halt`.
2. **Differential on recorded jobs.** The Windows port has `host/spu/spu_fnrec` (record/replay of SPU functions). Replay recorded jobs with
   `fastfp` and with the exact path (`-DSPU_SCALAR_HELPERS`) and compare output memory byte for byte.
3. **Exercise halt case 2** (stack reset / indirect re-entry) and a signal during a run with a lifted program (today: reviewed only).
4. **No regression elsewhere**: the stable build's tests (`vmx_neon_test`, `spu_fp_test` NOMODE control, `spu_run_test`) pass on both device clusters (pin to cpu0 too: the A55 cores must give the same bits).
5. Re-run the sustained benchmark with the phone plugged in and the battery above 50 % to separate governor effects from thermal ones.

## How it would be merged

- Cherry-pick the three commits (helpers + runtime, validation tooling, bench) onto a new branch from the stable tip; keep `GOWA_SPU_FASTFP` default **OFF**
  until 1-2 above pass on the game, then switch the default for the release build only. `SPU_FAST_FP=0`-style runtime fallback does not exist in a fastfp build, so
  the fallback is a build flag: a bad result in the field means shipping the scalar/NEON build, which is the same source with the option off.
- Review focus: `spu_helpers.h` (the `spu__a64_*` block and `SPU_FP_STD_*`), `spu_channels.c` (`SPU_REASSERT_FP_MODE`, enter/leave), `win32_compat.c` (thread FPCR reset).

## Next engineering steps (independent of the EBOOT unless noted)

| step | status |
|---|---|
| runtime library builds for arm64 with SDL3 + Vulkan backend | done (`android/build.sh`) |
| lifted SPU code runs through the real runtime on the phone | done for a hand-assembled program |
| Vulkan backend initialises on the Adreno 710 (headless) | done (`vk_smoke`) |
| window + swapchain on Android (SDL3 Activity, `VK_KHR_android_surface`) and a frame on screen | next |
| replace per-draw submit-and-wait and permanent `GENERAL` layouts by render passes with load/store ops (tile-based GPU) | design needed; the D3D12 engine's game fixes are the real scope |
| lift the PPU/SPU of the game and cross-compile it for arm64 (needs `EBOOT.elf`, a 16 GB+ host or CI) | blocked on the dump |
| run PPU + SPU + Vulkan together, measure frame time | blocked on the dump |
