# Colab station, wave 1 — commit dc38f15 (android/lift-pipeline == colab/lift-preflight), snapshot via git archive
Machine: 2 vCPU Xeon 2.2 GHz, 12.67 GB RAM, no swap. Nothing in the repository working tree was changed.
## Measurements
```
id                   start                      wall_s  user_s  sys_s  cpu   maxrss_MB  peak_sys_MB  exit
smoke                2026-11-22T18:50:58+00:00  2       0.00    0.00   0%    1          0            0
T1-ps3kit-pytests    2026-11-22T18:51:18+00:00  96.69   75.44   7.83   86%   165        172          0
T2-conformance       2026-11-22T19:03:23+00:00  29.63   26.55   1.72   95%   182        165          0
T3a-sdl3             2026-11-22T19:04:03+00:00  79.5    87.09   13.74  126%  127        89           0
T3b-runtime-android  2026-11-22T19:05:27+00:00  51.02   60.10   5.08   127%  101        124          0
T4a-vmx-build        2026-11-22T19:07:08+00:00  0.46    0.43    0.03   99%   64         0            0
T4b-vmx-run-qemu     2026-11-22T19:07:09+00:00  34.37   32.46   0.03   94%   9          53           0
T6-static-analysis   2026-11-22T19:08:49+00:00  446.04  644.10  15.52  147%  154        147          0
```
## T1 ps3kit Python suite (65 scripts): 33 PASS, 2 PASS(skip, macOS-only), 30 FAIL — classification:
```
      2 fork-lifter-preamble
      1 fork-mpnet-symbols
      3 fork-new-runtime-symbols
      8 MSVC-only-code
     16 OTHER
```
## T2 PPU/VMX conformance: 1533/1533 checks pass in x86 gcc O1, clang O1/O2, arm64 NDK r29 O1/O2 under qemu cortex-a76; selftest.sh exit 0
## T3 SDL3 3.4.12 + libps3kit_android.a: built; whole-archive link: 17 undefined without bring-up stubs, 0 undefined + 16 duplicate psnr_* with port_stubs.c
## T4 vmx_neon_test: bit-exact, 0 mismatches over 3,000,000 vectors/op (qemu; ns/op not meaningful)
## T6 clang-analyzer (NDK r29) on 167 TUs: 37 findings (runs/T6-static-analysis/findings.txt), not triaged into confirmed bugs
## T7 lift preflight: prepare.sh --dry-run passes every check up to the missing EBOOT; build_lifted.sh guard works
