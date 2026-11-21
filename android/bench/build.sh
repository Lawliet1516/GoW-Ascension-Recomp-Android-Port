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
# integrated tests: the real runtime library (built with SPU_A64_FASTFP) + a lifted, hand-assembled SPU program
ROOT="$HERE/../.."; LIBDIR="${LIBDIR:-$ROOT/build-android-fastfp}"
SDL3_ROOT="${SDL3_ROOT:-$HOME/toolchain-bt3/sdl3}"
if [ ! -f "$LIBDIR/libps3kit_android.a" ]; then
  CMAKE_EXTRA="-DGOWA_SPU_FASTFP=ON" "$HERE/../build.sh" "$LIBDIR"
fi
LIFT="$OUT/spuprog_lift"; python3 "$HERE/spuprog/make_prog.py" >/dev/null
PYTHONPATH="$PS3KIT/tools" python3 "$PS3KIT/tools/spu_lifter.py" --auto-functions "$HERE/spuprog/prog.elf" --output "$LIFT" >/dev/null
CF=(-std=gnu17 -O2 -fwrapv -mcpu=cortex-a78 -ffp-contract=off -w -D_GNU_SOURCE -DSPU_A64_FASTFP -I"$SPU" -I"$PS3KIT/include" -I"$PS3KIT/runtime" -I"$PS3KIT/runtime/platform" -I"$PS3KIT/runtime/ppu" -I"$LIFT")
LINK=("$LIBDIR/libgowa_port_stubs.a" "$LIBDIR/libgowa_host_stubs.a" "$LIBDIR/libps3kit_android.a" "$LIBDIR/libgowa_port_stubs.a" "$LIBDIR/libgowa_host_stubs.a" -L"$SDL3_ROOT/lib" -lSDL3 -llog -landroid -ldl -lm)
CXX="${CC%clang}clang++"
for v in "" "_checked"; do
  X=(); [ -n "$v" ] && X=(-DSPU_A64_CHECK_MODE)
  "$CC" "${CF[@]}" "${X[@]}" -c "$HERE/spu_run_test.c" -o "$OUT/spu_run_test$v.o"
  "$CC" "${CF[@]}" "${X[@]}" -c "$LIFT/spu_recomp.c" -o "$OUT/spu_recomp$v.o"
  "$CXX" -o "$OUT/spu_run_test$v" "$OUT/spu_run_test$v.o" "$OUT/spu_recomp$v.o" "${LINK[@]}"
done
"$CC" -O2 -w "$HERE/vkprobe.c" -o "$OUT/vkprobe" -ldl
"$CC" -O2 -w -c "$HERE/vk_smoke.c" -o "$OUT/vk_smoke.o"
"$CXX" -o "$OUT/vk_smoke" "$OUT/vk_smoke.o" "${LINK[@]}"
"$CC" -O1 "$HERE/fpcr_probe.c" -o "$OUT/fpcr_probe"
echo "run on the phone (push libSDL3.so too, LD_LIBRARY_PATH=.): spu_run_test spu_run_test_checked vk_smoke vkprobe fpcr_probe"
