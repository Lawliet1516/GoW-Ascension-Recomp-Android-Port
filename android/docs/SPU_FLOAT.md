# SPU single-precision float on ARM64: why it was slow, what x86 does, what we did

Branch: `android/exp-spu-fastfp` (experimental, not merged into the stable branch).
Device: Redmi Note 13 Pro 5G, Snapdragon 7s Gen 2 (4x Cortex-A78 @ 2.4 GHz = cpu4-7, 4x Cortex-A55 @ 1.96 GHz = cpu0-3,
ARMv8.2-A + fp16 + dotprod, no SVE). All numbers below were measured on cpu7 (A78) unless stated.
Raw results and tables: [../RESULTS.md](../RESULTS.md).

## 1. Why the current NEON path emulates through double

The SPU's single-precision arithmetic is not IEEE-754 binary32:

| SPU behaviour | Host default (x86 MXCSR / AArch64 FPCR = 0) |
|---|---|
| results are truncated (round toward zero) | round to nearest even |
| a denormal operand is read as zero, a denormal result is flushed to zero | denormals are kept |
| no Inf / NaN: biased exponent 255 is an ordinary number (range up to ~2^128), overflow saturates | exponent 255 is Inf/NaN |
| never produces -0 | produces -0 |
| `fma` is fused, truncated once | `fmla` is fused, rounded to nearest |

One host instruction in the default mode therefore gives wrong bits. `spu_helpers.h`'s NEON ("nv") path gets the exact result without
touching the FP mode: it widens each pair of lanes to `f64` (a product of two 24-bit significands is exact in `f64`, a sum is exact
when the exponents differ by at most 29), operates in `f64`, clears the 29 low significand bits (that is truncation), narrows (exact),
and then tests every lane with 64-bit integer operations (is it a normal single, a zero, on a 24-bit boundary?). `fma` additionally
needs Knuth's TwoSum (six more `f64` operations) to know whether the 53-bit rounding crossed a 24-bit boundary.

Instruction counts for one `spu_fm` (clang -O2, `-mcpu=cortex-a78`, wrapper function included):

| op | scalar helpers | NEON "nv" (f64 emulation) | FPCR fast path |
|---|---|---|---|
| `fa` | 76 | 90 | **20** |
| `fm` | 76 | 81 | **26** |
| `fma` | 112 | 120 | **32** |

In the NEON `fm`: 6 `fcvtl/fcvtn` widen/narrow instructions, 21 instructions on `.2d` lanes (so every 4-lane op is two half-width
ops) and the rest is the exponent/boundary classification (`ushr`, `cmhi`, `bic`, `and`, `dup`, ...). The arithmetic itself is a small
part of the cost; **the exactness bookkeeping is the cost**. That is why the NEON float path is no faster than plain scalar code and
sometimes slower (skin kernel: NEON 240 ns/vertex, scalar 154 ns).

## 2. How the x86 fast path gets the same bits (what it guarantees)

`spu__xs_*` in `spu_helpers.h` (used when the thread runs SPU code):

1. `spu_run_with_halt` sets **MXCSR = RZ + FTZ + DAZ** for the whole run (thread state, restored on exit). Now a single `addps` /
   `mulps` / `vfmadd` *is* the SPU operation: truncation, denormals read/flushed as zero, one rounding for fma.
2. **A range test per op**: if every lane's operands are ordinary singles and cannot overflow, the hardware result is used.
   `fa/fs`: both biased exponents <= 253. `fm`: both <= 253 and `ex+ey <= 380`. `fma`: all three <= 253 and `ex+ey <= 379`.
   Below 2^127 a sum stays below 2^128 and truncation never rounds up; the product bound keeps the result under 2^128.
3. `pos0f`: `r + 0.0` under RZ turns -0 into +0 and leaves everything else alone (the SPU never produces -0).
4. Any lane failing the test (exponent 254/255 = extended range, a possible overflow) sends the **whole op** to the exact per-lane path, in the
   host's standard mode (`SPU_FP_STD_BEGIN/END` around it). That path is the old exact code, so the result is identical either way.
5. Everything not in the fast set (reciprocal estimates, int/float conversions, doubles) keeps its own implementation, written for
   the standard mode; the STD scope switches back for the ones that need round-to-nearest.

Guarantee: bit-identical to the exact reference for every input, because the fast path is only taken where it is provably equal to it,
and the checks are conservative (they send some valid inputs to the slow path; they never send an invalid one to the fast path).
The port also has an opt-in `SPU_XFLOAT_APPROX` (RPCS3's "approximate xfloat": no range test); we do **not** use it.

## 3. FPCR on AArch64: what the hardware and Android actually do (measured: `android/bench/fpcr_probe.c`)

| Question | Result on this device (kernel + bionic) |
|---|---|
| Rounding/flush control | `FPCR.RMode` bits 23:22 (0b11 = RZ), `FPCR.FZ` bit 24. Both honoured by the A78 (`spu_fp_test` passes). FZ flushes inputs **and** outputs for single precision, like FTZ+DAZ together. |
| Default FPCR | 0x0 (RN, FZ off, DN off, AH off) |
| FP exception traps | trap-enable bits are RAZ/WI: writes read back 0. No FP exception can ever be delivered; FPSR flags are cumulative only and nothing reads them. |
| Does `longjmp` restore FPCR? | **No** (bionic). x86's MXCSR is restored by the code's own comment ("longjmp restores the mode setjmp saw") but FPCR is not. The halt landing pads (`case 1/2` after `setjmp`) now set the mode again by hand. |
| Signal handlers | **inherit** the interrupted FPCR (a handler that runs float code while an SPU thread is in RZ|FZ would compute in that mode). |
| New threads | **inherit** the creator's FPCR. A thread must never be created from inside an SPU run, and each SPU worker enters the mode itself. |

Controlled use of the global state (what the code does):
- FPCR is *per-thread*. It is changed only on an SPU worker thread, inside `spu_run_with_halt`, saved on entry and written back on every exit path.
- `SPU_FP_STD_BEGIN/END` (cold paths, double-precision SPU ops) read FPCR and write it only if the SPU bits are set, and put it back.
- Nothing else in the process (PPU threads, the Vulkan driver, SDL, the audio thread) runs with the mode on.
- Because the helpers assume the mode (a per-op `mrs fpcr` would cost most of the gain), a build with `SPU_A64_FASTFP` has no
  runtime `SPU_FAST_FP=0` switch; the stable build (flag off) keeps the old scalar/NEON helpers as the fallback.

## 4. The implementation

`spu__a64_*` in `ps3kit/runtime/spu/spu_helpers.h`, enabled with `-DSPU_A64_FASTFP` (CMake option `GOWA_SPU_FASTFP`):
`fa fs fm fma fms fnms` as `vaddq_f32 / vmulq_f32 / vfmaq_f32` + the exponent test above + `pos0`; slow path = the existing exact per-lane
code (`spu__fa_any` ...) in the standard FP mode, out of line and `cold`. No conversion to double on the fast path.
The other helpers are untouched (their implementations are mode-independent, verified, see below).

Dispatch per operation is by measurement ([../RESULTS.md](../RESULTS.md)): integer ops keep NEON where it wins (most), `orx` is scalar
by default (8.3 ns vs 5.4 ns latency, reproducible over three runs), and the scalar helpers stay available with `-DSPU_SCALAR_HELPERS`.

## 5. Verification

`android/bench/spu_fp_test.c`, run on the phone:
1. `fa fs fm fma fms fnms` in the SPU mode against the exact references `spu_*_ref`, 4,000,000 random + special vectors per op
   (zeros, denormals, exponents 1-4, 248-255, exactly 253/254/255, +-1, few-bit mantissas for exact ties): **0 mismatches**.
   Negative control (`NOMODE=1`, run without entering the mode): 8.9 M mismatches, so the test can fail.
2. Every other float helper (`frest frsqest fi frds fesd cflts cfltu csflt cuflt dfma dfms dfnma dfnms fceq fcgt fcmeq fcmgt`) returns the
   same bits in the standard mode and in the SPU mode: **0 mismatches**.

What this does **not** prove: that the game behaves correctly. No lifted game code has been run yet (no `EBOOT.elf`).
