# Android ARM64 port: measurements and findings

Device: **Redmi Note 13 Pro 5G (2312DRA50G)**, Snapdragon 7s Gen 2, Adreno 710, 12 GB. From `/proc/cpuinfo`: cpu0-3 Cortex-A55
(1958 MHz max, capacity 439), cpu4-7 Cortex-A78 (2400 MHz, capacity 1024), ARMv8.2-A + fp16 + dotprod, no SVE.
NDK r29 (clang 21), `-O2 -mcpu=cortex-a78 -ffp-contract=off`. Single-thread runs are pinned to cpu7. Phone on battery (25-30 %), Android
governors untouched (no root). Raw data: [results/](results/). Reproduce with [README.md](README.md).

**Read the numbers with this in mind:** the phone's clocks wander (big cores observed 691-2400 MHz inside one run) and the same binary moved
+-25 % between runs. Ratios *within one run* are reliable; absolute throughput is not. Sustained results below are therefore also
given per GHz.

**Nothing here proves the game runs or is playable.** No lifted PPU code and no game data have been executed (no `EBOOT.elf` yet).

## 1. SPU float: three implementations, per operation (ns; latency = dependent chain, throughput = 8 independent chains)

`scalar` = `-DSPU_SCALAR_HELPERS -DSPU_SCALAR_INT` (pure C helpers, the fallback); `neon` = the helpers as upstream ships them for ARM64
(float ops emulate through f64); `fastfp` = `-DSPU_A64_FASTFP` (FPCR RZ|FZ + range test, see [docs/SPU_FLOAT.md](docs/SPU_FLOAT.md)).
Float operands: a multiplier of exactly 1.0 so dependent chains stay in the ordinary range (a run with `SPU_A64_COUNT` confirms
**0 slow-path calls** for fa/fs/fm/fma/fms/fnms in these loops; an earlier version of this benchmark drifted out of range and measured the
slow path, see the commit history).

```
op       | lat ns: scalar    neon  fastfp | tput ns: scalar    neon  fastfp | best (tput)
a        |           6.30    4.77    4.79 |            1.02    0.26    0.26 | neon
sf       |           6.33    4.76    4.79 |            2.12    0.26    0.26 | neon
ai       |           6.29    4.76    4.78 |            1.11    0.26    0.26 | neon
and      |           4.78    4.76    4.76 |            1.06    0.26    0.26 | neon
or       |           4.76    4.77    4.76 |            1.05    0.26    0.26 | neon
xor      |           4.77    4.76    4.76 |            1.05    0.26    0.26 | neon
andc     |           4.77    4.76    4.76 |            1.06    1.06    1.06 | neon
eqv      |           4.77    4.76    4.76 |            1.06    1.05    1.06 | neon
ah       |           8.31    4.77    4.77 |            4.29    0.26    0.26 | neon
ceq      |           7.57    4.76    4.79 |            3.21    0.26    0.26 | neon
ceqb     |          15.71    4.77    4.76 |           10.59    0.26    0.26 | neon
cgt      |           7.32    4.78    4.76 |            2.74    0.26    0.26 | neon
clgt     |           6.99    4.76    4.75 |            2.44    0.26    0.26 | neon
selb     |           5.82    5.83    5.83 |            1.05    0.53    0.53 | neon
shufb    |           7.96    7.96    7.93 |            1.32    1.40    1.39 | scalar
rotqby   |          13.44    4.76    4.77 |           14.23    0.26    0.26 | neon
rotqbyi  |           7.50    4.76    4.76 |            2.07    0.26    0.26 | neon
rotqbi   |           7.78    7.77    7.77 |            3.03    3.07    3.00 | fastfp
rot      |           6.93    6.36    6.36 |            2.92    1.06    1.05 | fastfp
roti     |           6.90    7.48    7.48 |            2.12    1.29    1.29 | neon
shli     |           5.83    5.84    5.81 |            0.66    0.66    0.66 | neon
rotm     |           7.86    4.76    4.77 |            2.92    0.53    0.53 | neon
mpy      |          11.66    8.49    8.47 |            3.42    1.05    1.05 | neon
mpyu     |           6.82    7.43    7.43 |            2.62    0.66    0.66 | neon
mpyh     |           6.81    8.51    8.50 |            2.61    1.05    1.05 | neon
mpya     |           9.03    8.48    8.47 |            3.73    1.25    1.25 | neon
clz      |           1.06    4.76    4.75 |            2.24    0.26    0.26 | neon
cntb     |          30.01    4.77    4.75 |           29.59    0.26    0.26 | neon
gb       |           7.96    4.78    4.75 |            1.85    1.32    1.32 | fastfp
avgb     |          19.30    4.78    4.77 |           15.06    0.26    0.26 | neon
absdb    |          30.08    4.77    4.77 |           26.03    0.26    0.26 | neon
orx      |           5.85    5.84    5.82 |            1.10    1.08    1.10 | neon
fa       |          31.30   19.74    6.35 |           29.32   12.43    2.90 | fastfp
fs       |          33.46   19.62    6.34 |           29.93   12.41    2.92 | fastfp
fm       |          31.26   16.81    6.34 |           28.58   11.23    2.90 | fastfp
fma      |          24.07   23.34    7.41 |           22.59   17.64    3.41 | fastfp
fms      |          24.29   23.36    7.43 |           22.94   17.63    3.42 | fastfp
fnms     |          52.13   23.62    8.72 |           37.58   18.45    3.78 | fastfp
frest    |          14.21   14.09   14.29 |           10.89   10.95   10.89 | fastfp
frsqest  |          14.52   14.48   14.50 |            8.73    8.63    8.67 | neon
fi       |           9.77    9.82    9.78 |            8.51    8.51    8.52 | scalar
csflt    |          12.91   12.86   12.88 |            6.46    6.49    6.45 | fastfp
cflts    |           8.47    8.46    8.49 |            3.97    3.97    3.98 | neon
fceq     |          10.26   10.27   10.29 |            5.56    5.56    5.57 | scalar
fcgt     |          10.26   10.26   10.25 |            5.57    5.58    5.56 | fastfp
fcmgt    |           9.03    9.23    9.21 |            4.25    4.24    4.25 | neon
```

Integer ops: NEON wins on throughput by 4-100x over the scalar helpers on most (cntb 29.6 -> 0.26 ns, absdb 26.0 -> 0.26, avgb 15.1 -> 0.26,
rotqby 14.2 -> 0.26, ceqb 10.6 -> 0.26, a 1.02 -> 0.26); a few lose on *latency* (clz: 1.06 ns scalar vs 4.76 NEON, because the NEON form
round-trips through a vector register) while winning throughput, which is what independent lifted code is bound by. Where the columns tie within ~1 ns the generated code is the same and the difference is
codegen noise (checked: `rotqbi`, `shli` have no NEON path at all and still differ by that much between variants).
Per-operation dispatch decisions taken from this data:
- float fa/fs/fm/fma/fms/fnms: `fastfp` (throughput 3.9-5.2x over `neon`, 6.6-10x over `scalar`; latency 2.6-3.2x over `neon`), opt-in until validated;
- `orx`: scalar by default (NEON vext chain was 8.3 ns latency / 1.22 ns throughput vs 5.4 / 1.02 scalar in three repeated runs; the
  `neon` column above already uses the scalar default); `-DSPU_A64_ORX_NEON` restores the NEON form;
- everything else keeps the existing per-op NEON/scalar choice of the helpers; no op was forced to NEON where it lost;
- `frest/frsqest/fi/csflt/cflts` and the comparisons are unchanged between variants (they are LUT/integer code; +-2 ns differences between
  binaries are code-layout noise, not understood further).

## 2. Representative kernels (one core, ns per iteration of a 4096-element pass, min..max of 3 runs)

| kernel | what it is | scalar | neon | fastfp | fastfp vs neon / vs scalar |
|---|---|---|---|---|---|
| skin | vec4 transform: 3 shufb splats + fm + 2 fma + fa, quadword load/store | 154.8..180.4 | 239.4..239.7 | 89.1..104.2 | x2.69 / x1.74 |
| recip | dot + frest/fi + Newton step | 187.3..188.6 | 189.2..189.5 | 49.3..57.7 | x3.84 / x3.80 |
| bits | and/rotqby/cgt/selb/shufb/ceq chain | 19.5..22.8 | 8.3..9.7 | 8.3..9.7 | x1.00 / x2.34 |
| copy | lqd/stqd through the big-endian LS accessors | 1.7..2.0 | 1.7..2.0 | 1.7..2.0 | x1.00 / x1.03 |

The upstream NEON float path is *slower than plain scalar* on the float-heavy kernels (skin 239 vs 155-180 ns).

## 3. Sustained load: 4 threads on the 4 A78 cores, 240 s, all four kernels round-robin

Column units: throughput = million kernel iterations/s summed over threads; temperature = hottest cpu thermal zone; clocks sampled from cpu4/cpu7.
`thr/GHz` divides by the mean big-core clock to remove the governor's wandering.

```
sus_scalar.txt   n=48 thr mean= 25.34 sd= 1.73 min= 19.10 max= 28.72 | q1= 26.04 q4= 25.74 ( -1.1%) | temp mean=55.7 max=70.9 | big MHz mean= 1396 min= 1344 max= 2400 | thr/GHz= 18.3
sus_neon.txt     n=48 thr mean= 29.99 sd= 4.41 min= 14.65 max= 35.90 | q1= 34.56 q4= 24.12 (-30.2%) | temp mean=61.9 max=74.0 | big MHz mean= 1869 min= 1344 max= 2400 | thr/GHz= 16.3
sus_neon2.txt    n=48 thr mean= 22.97 sd= 4.75 min= 14.73 max= 33.61 | q1= 25.62 q4= 24.84 ( -3.0%) | temp mean=52.2 max=72.6 | big MHz mean= 1530 min=  691 max= 2304 | thr/GHz= 16.2
sus_fastfp.txt   n=48 thr mean= 60.48 sd= 3.01 min= 52.38 max= 65.68 | q1= 60.35 q4= 59.36 ( -1.6%) | temp mean=54.0 max=57.6 | big MHz mean= 1477 min= 1344 max= 1900 | thr/GHz= 41.1
sus_fastfp2.txt  n=48 thr mean= 84.85 sd=10.75 min= 46.36 max= 92.49 | q1= 74.70 q4= 87.37 (+17.0%) | temp mean=55.5 max=63.1 | big MHz mean= 2041 min=  691 max= 2400 | thr/GHz= 42.0
```

Runs were done in two orders (scalar, neon, fastfp, then neon2, fastfp2) because the first neon run started from a different thermal
state. Findings:
- **fastfp is ~2.5x neon per clock (41-42 vs 16.2-16.3 thr/GHz) and ~2.3x scalar (18.3)**, and stable: -1.6 % first to last quarter, sd 3 (5 % of the mean) in run 1.
- It also ran **cooler** (mean 54 C, max 57.6 vs 62-74 C for neon) at lower clocks in run 1: fewer instructions per result.
- neon run 1 lost 30 % between the first and last quarter (big-core clock samples spanning 1344-2400 MHz, hottest zone up to 74 C); run 2 did not
  (-3 %) at a lower starting temperature, and run 2 sat at 691 MHz for some samples. Throttling is real and run-to-run dependent on this phone; it
  was not possible to isolate whether it was thermal or the battery governor (battery 25-30 %, unplugged). fastfp showed no loss in run 1
  and +17 % (warm-up of the governor, not a gain of the code) in run 2.
- Every 240 s run is a single sample per configuration order; treat the ratios as indicative, the absolute values as not reproducible.

## 4. Precision validation (the fast path against everything we can compare it to)

| check | result |
|---|---|
| Independent exact-rational Python model of the SPU float semantics vs the upstream C exact reference (`spu_*_ref`), 6 ops, structured + random records (strides 40 and 23: 108,316 + 188,376 records) | **all equal** |
| Fast path vs exact reference, 4,332,637 records x 6 ops, **four different records per vector** (lanes of different exponent classes meet in one op): every exponent 0-255 against every exponent class, 12x12x12 exponent triples at 0/1/2/3/126-129/252-255 with 8 mantissa classes and all sign combinations, product bound ex+ey in 376-384, fma bound 379, cancellation +-1 ulp, truncation ties, 300k random bit patterns | **0 differences** |
| Same, 4M random + special vectors per op (`spu_fp_test` part 1) | **0 mismatches**; negative control (run without entering the mode) 8.9 M mismatches |
| Every other float helper (frest, frsqest, fi, frds, fesd, cflts, cfltu, csflt, cuflt, dfma/dfms/dfnma/dfnms, fceq/fcgt/fcmeq/fcmgt) identical in standard and SPU float mode, 400k vectors each | **0 mismatches** |
| **Mutation check** (the test must be able to fail): fa limit 253->254; fm product bound 380->382; fma bound 379->383; `pos0` removed | **all four caught** (9,005 / 18,708 / 71,866 / 335,441 differences) |
| Real runtime running a lifted, hand-assembled SPU program through `spu_run_with_halt` (`spu_run_test`): 1+2=3, 3*2=6, fma, fs, **1 + 0.75 ulp -> 1.0 (truncation; round-to-nearest gives 0x3F800001)**, **denormal*2 -> 0 (IEEE gives 0x00800000)** | correct; the same program run outside the mode gives the IEEE results (negative control) |

What this covers: every bit pattern class the model and the generator know. What it does not: all 2^96 triples (not testable), the exact
reference's agreement with a real SPU (only with our independent model, which follows the documented semantics), and the game.

## 5. FPCR: paths reviewed and tested

Facts measured on the device (`results/fpcr_probe.txt`): default FPCR 0; trap-enable bits RAZ/WI (no FP traps); bionic `longjmp` does **not**
restore FPCR; signal handlers and new threads **inherit** FPCR.

| exit / event | handling | how it is checked |
|---|---|---|
| normal return of `spu_run_with_halt` | `spu_a64_fp_leave(saved)` writes back exactly the entry value | `spu_run_test` 1, 2, 3 |
| SPU `stop` (halt: `longjmp` to the run loop, case 1) | the landing pad re-sets the mode (longjmp keeps the current value, re-set is a safety net); the leave below runs | `spu_run_test` 1 (stop code 0), 7 |
| other halts (`longjmp` case 2: stack reset / indirect re-entry, 3 call sites) | same re-set at the landing pad | **code review only, not executed** |
| caller already has other FPCR bits (round-up) | restored exactly | test 2 (0x400000 -> 0x400000) |
| caller already in the SPU mode (nested use) | restored to the mode, not to zero | test 3 |
| thread created while the creator is in the mode | the thread trampoline clears FPCR | test 6 (child starts at 0) |
| 4 threads x 20,000 runs each, with halts | no drift on any thread, main thread untouched | test 7 |
| a fast op executed outside the mode | `-DSPU_A64_CHECK_MODE` builds abort with a diagnostic (one `mrs` per op: validation only) | checked build: the child process aborts as required |
| signal handlers | inherit the interrupted mode; main-thread handler outside a run sees 0 | test 5 (informational) |
| thread exit inside a run | FPCR is per-thread, a dead thread's value is never inherited (new threads reset) | review |
| cold slow path (`SPU_FP_STD_BEGIN/END`) | reads FPCR, clears the bits, restores the saved value; no halt can occur inside | review |
| runtime `SPU_FAST_FP=0` switch | **not available** in a `SPU_A64_FASTFP` build (helpers assume the mode); the scalar/NEON build is the fallback | by design |

## 6. Link and first real execution on the device

- `android/build.sh` builds `libps3kit_android.a` (all of `ps3kit/libs/**` + the runtime, SDL3, Vulkan backend) with 0 errors.
- A full link of lifted SPU code + the runtime library + SDL3 needs **~35 symbols that the Windows port supplies** from `ascension/host`
  and the per-game PPU loader sources; each is listed and explained in `android/host/port_stubs.c`. With those stand-ins it links and **runs on
  the phone** (`spu_run_test`: the real SPU channel/drain/policy runtime executing lifted code).
- The project's Vulkan backend (`vk_smoke`) **initialises on the Adreno 710 headless**: device pick, offscreen 1280x720 target, depth format,
  a present and a readback (centre pixel 0xff000000, a cleared frame). No window/swapchain, no guest draw, no shader from the game.
  "It initialised" is the only claim.

## 7. GPU: Adreno 710 vs what the renderer needs

Probe (`results/vkprobe_adreno710.txt`): Vulkan **1.1.128**, BC1/BC3/BC7 **sampled-image support yes** (PS3 DXT textures can be used
directly), ETC2/ASTC yes, D24S8 yes, anisotropy 16, geometry + tessellation shaders, dual-source blend, independent blend, occlusion query
precise, multiDrawIndirect, timeline semaphores, synchronization2, push descriptors, descriptor indexing, 16-bit storage, float16/int8,
`QCOM_render_pass_transform`, `EXT_fragment_density_map`, **no** `KHR_dynamic_rendering`, no `shaderFloat64`. Memory is **unified**: one 11.3 GB
device-local heap whose types are also host-visible (coherent+cached): staging copies for textures/buffers are avoidable. Queues: family 0 = 3x
graphics+compute, family 1 = 1x transfer. `maxUniformBufferRange` is 64 KB.

Where the project's renderers stand (file sizes and API counts from the source):

| | D3D12 live-draw engine (Windows production) | Vulkan backend (what exists) |
|---|---|---|
| code | `rsx_live_draw.c` 30,772 lines + `rsx_d3d12_backend.c` 6,801 (1,874 + 821 D3D12 API references) | `rsx_vulkan_backend.c` 3,475 lines (202 Vulkan calls) over the shared `rsx_draw_engine.c` 2,705 |
| off Windows | `rsx_live_draw.c` compiles to **no-op stubs** (its first lines: "Non-Windows: the whole path is a no-op") | this is the only path |
| shader model | FP "uber" shader and decompilers (379 references), async pipeline building (135), pipeline/shader cache with 13,425 recorded pipelines shipped | none of: uber shaders, async compile, pipeline cache |
| occlusion culling, SMAA/FSR/anisotropy tuning, compute passes, texture packs | yes (307 / 66+88 / 32 / 34 / 22 references) | **none (0 references)** |
| frame structure | fences, double buffering | "Deliberately synchronous: every clear, draw and present is one one-shot command buffer, submitted and waited on", images always `VK_IMAGE_LAYOUT_GENERAL`, Vulkan 1.0 entry points only |

So the Vulkan path is a **correctness prototype**, not a renderer: per-draw submit-and-wait and permanent GENERAL layouts are the opposite of
what a tile-based mobile GPU wants (render passes with load/store ops, transient attachments, no mid-frame waits). The game's measured
GPU/CPU split on Windows ("the PS3's SPU work and the main thread, not the graphics card, set the frame rate") means the CPU side comes first,
but a playable Android build needs a real port of the engine's game-specific rendering fixes onto Vulkan, which is on the order of the size of the
D3D12 engine, not a tweak of the 3.5 k-line backend. This is the largest piece of work in the project that is not about CPU speed.

## 8. PPU on ARM64 (what the source shows, no lifted code executed yet)

- The lifter (`tools/ppu_lifter.py`) emits **portable C/C++** and no x86 intrinsics at all (no `_mm_*`, no `__builtin_ia32_*`, no inline asm).
  The "AVX2" in the Windows build is a compiler flag (`/arch:AVX2 -mfma`, option `ASCENSION_PPU_AVX2`) plus the few helpers in
  `ppu_vmx.h`. **No binary translation or dynamic recompilation is needed**: clang compiles the lifted code natively for AArch64. `fmaf`
  lowers to a single `fmadd` there.
- The x86-only part was the nine four-lane float helpers of `ppu_vmx.h` (SSE4.1+FMA); on ARM they fell back to byte-at-a-time loops. They now
  have NEON forms: **bit-identical to the scalar references on 3M random+special vectors per op, NJ on and off** (on the phone). Speed: `vrefp`
  x5.3 faster than the loop; for vaddfp/vmaddfp/vcmpgtfp the compiler already vectorised the reference loop to the same code (x1.00): the
  explicit NEON is correct and no slower, but not a gain there.
- **Weak memory ordering**: the lifter already lowers sync/lwsync/eieio/isync to C11 fences (one `dmb` each on arm64) and its header says why
  (x86's total store order forgave missing barriers). Guest loads/stores are relaxed atomics, as on x86.
- Risks that remain and cannot be measured without the EBOOT: size/compile time of the lifted PPU (190 MB of C++ in 5 files on Windows; the
  build needs 8 GB RAM per compiler process, so it must be cross-compiled on a bigger host or CI), the resulting `.so` size, and the actual speed of
  the lifted PPU on an A78. The Windows numbers say the main thread is a bottleneck even on a Ryzen 9950X3D.

## 9. Not proven / open

- No game code has run. The 17-19 fps (RTX 3080) / 50-100 fps (Ryzen 9 9950X3D) figures belong to the Windows ports on desktop CPUs.
- `fastfp` is validated against references on 4.3 M structured vectors and in a real-runtime run of a small program; it has not seen game SPU
  jobs. It stays **experimental** (branch `android/exp-spu-fastfp`).
- Halt `longjmp` case 2 and signal-during-run behaviour are reviewed, not exercised.
- The sustained numbers are single 240 s samples on a phone whose governor is not controlled.
- The `fi` and `orx` rows had to be re-measured once because of benchmark artifacts (documented above).
