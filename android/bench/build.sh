#!/usr/bin/env bash
# Builds the on-device tests with the NDK and pushes them to /data/local/tmp (adb).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; PS3KIT="$HERE/../../ps3kit"
NDK="${NDK:-$(ls -d "$HOME"/Android/Sdk/ndk/* | tail -1)}"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang"
OUT="${1:-$HERE/out}"; mkdir -p "$OUT"
ARCH="${ARCH:--march=armv8.2-a+fp16+dotprod}"
"$CC" -std=gnu11 -O2 $ARCH -ffp-contract=off -I"$PS3KIT/runtime" -I"$PS3KIT/runtime/ppu" \
    "$HERE/vmx_neon_test.c" -o "$OUT/vmx_neon_test" -lm
echo "$OUT/vmx_neon_test"
# SPU kernels, three variants (NEON helpers / pure-C helpers / tuned for the big cores)
SPU="$PS3KIT/runtime/spu"
for v in neon scalar a78; do
  case $v in
    neon)   X=(-march=armv8.2-a+fp16+dotprod);;
    scalar) X=(-march=armv8.2-a+fp16+dotprod -DSPU_SCALAR_HELPERS -DSPU_SCALAR_INT);;
    a78)   X=(-mcpu=cortex-a78);;
  esac
  "$CC" -std=gnu11 -O2 "${X[@]}" -ffp-contract=off -I"$SPU" -I"$PS3KIT/include" -I"$PS3KIT/runtime/platform" \
      "$HERE/spu_kernels.c" -o "$OUT/spu_kernels_$v" -lm
  echo "$OUT/spu_kernels_$v"
done
# FPCR float fast path: bit-exactness test and the kernels with the fast path on
"$CC" -std=gnu11 -O2 -march=armv8.2-a+fp16+dotprod -ffp-contract=off -DSPU_A64_FASTFP -I"$SPU" -I"$PS3KIT/include" -I"$PS3KIT/runtime/platform" "$HERE/spu_fp_test.c" -o "$OUT/spu_fp_test" -lm
"$CC" -std=gnu11 -O2 -march=armv8.2-a+fp16+dotprod -ffp-contract=off -DSPU_A64_FASTFP -DKERNELS_FPMODE -I"$SPU" -I"$PS3KIT/include" -I"$PS3KIT/runtime/platform" "$HERE/spu_kernels.c" -o "$OUT/spu_kernels_fastfp" -lm
echo "$OUT/spu_fp_test $OUT/spu_kernels_fastfp"
# per-op latency/throughput, three implementations (compare with compare_ops.py)
for v in scalar neon fastfp; do
  case $v in
    scalar) X=(-DSPU_SCALAR_HELPERS -DSPU_SCALAR_INT);;
    neon)   X=();;
    fastfp) X=(-DSPU_A64_FASTFP);;
  esac
  "$CC" -std=gnu11 -O2 -march=armv8.2-a+fp16+dotprod -mtune=cortex-a78 -ffp-contract=off "${X[@]}" -I"$SPU" -I"$PS3KIT/include" -I"$PS3KIT/runtime/platform" \
      "$HERE/spu_ops_bench.c" -o "$OUT/spu_ops_$v" -lm
done
echo built per-op benches
