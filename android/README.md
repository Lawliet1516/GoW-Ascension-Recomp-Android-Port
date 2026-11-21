# Android ARM64 port: build and test instructions

Status: research / bring-up. **No APK yet, nothing from the game has run.** See [RESULTS.md](RESULTS.md) for what has been measured and
[docs/SPU_FLOAT.md](docs/SPU_FLOAT.md) for the SPU float investigation.

## Requirements (host: Linux)

- Android NDK r29 (`~/Android/Sdk/ndk/<ver>`), the SDK's CMake 3.31 + Ninja (`~/Android/Sdk/cmake/<ver>/bin`)
- SDL3 built for `arm64-v8a` (prefix with `include/SDL3` and `lib/libSDL3.so`; `SDL3_ROOT`)
- `adb` and a phone with USB/Wi-Fi debugging for the on-device tests
- The test device: Redmi Note 13 Pro 5G (Snapdragon 7s Gen 2: cpu0-3 Cortex-A55, cpu4-7 Cortex-A78, ARMv8.2-A, no SVE)

## Build the runtime library (`libps3kit_android.a`)

```sh
android/build.sh            # NDK, SDL3_ROOT and CMake default to the paths above; build dir: build-android/
```

`android/CMakeLists.txt` is independent of `ps3kit/CMakeLists.txt` (which targets Windows/clang-cl) and mirrors its source lists.
It builds the runtime + HLE libraries + the Vulkan RSX backend against SDL3 (`PS3_SDL3`, see `ps3kit/include/ps3_sdl.h`).
Not built: the Direct3D 12 and Metal backends, `runtime/ppu/*` (need the generated `ppu_recomp.h` from your own lifted EBOOT).
A successful build is **not** a working port: nothing links or runs yet.

## On-device tests and benchmarks (no game needed)

```sh
android/bench/build.sh /tmp/out        # builds every test/bench for the phone with the NDK
adb push /tmp/out/<name> /data/local/tmp/ && adb shell /data/local/tmp/<name>
```

| program | what it checks |
|---|---|
| `vmx_neon_test` | PPU VMX NEON helpers vs the scalar `_ref` forms, bit for bit, then speed |
| `spu_fp_test` (built with `-DSPU_A64_FASTFP`) | SPU float fast path vs exact references; mode-independence of the other float helpers; `NOMODE=1` is the negative control |
| `spu_ops_<scalar\|neon\|fastfp>` | per-operation latency/throughput of 45 SPU helpers; join with `compare_ops.py` |
| `spu_kernels_<variant>` | representative kernels (skin, recip, bits, copy); `--sustain S --threads N --cores 4,5,6,7` for long runs with temperature and clocks |
| `fpcr_probe` | how FPCR behaves on this kernel/bionic (longjmp, signals, threads, traps) |

Run single-thread benchmarks on a big core (`--cores 7` / argument `7`); the little cores are 2-5x slower and the scheduler moves threads
between clusters if you do not pin.

## Branches

| branch | content |
|---|---|
| `main` | upstream + README (credits to the original author) |
| `android/m0-audit` | runtime compiles for aarch64 (shims) |
| `android/m1-cmake-sdl3` | + SDL3 shim, Android CMake project, VMX NEON; stable work |
| `android/exp-spu-fastfp` | experimental: FPCR float fast path, SPU benchmarks |
