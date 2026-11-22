#!/usr/bin/env bash
# T2: PPU/VMX conformance suite (1,202 cases generated from the lifter's own emissions), without the game.
set -uo pipefail
S=/root/work/colab-station; SNAP=$S/snap/dc38f15; OUT=$S/runs/T2-conformance; mkdir -p $OUT
export NDK=$S/toolchain/android-ndk-r29 PYTHON=$S/toolchain/venv/bin/python
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
FLAGS="-std=c++17 -w -ffp-contract=off -D_GNU_SOURCE -DPPU_LEAN_CONST= -Iruntime -Iruntime/ppu -Iruntime/platform -Iinclude"
echo "=== A. android/lift/selftest.sh as committed (host lifter tests + NDK arm64 build, OUT outside the repo)"
OUT=$OUT/selftest bash $SNAP/android/lift/selftest.sh; echo "selftest.sh exit=$?"
cd $SNAP/ps3kit; SRC=scratch/ppu_conformance.cpp; cp $SRC $OUT/ppu_conformance.cpp; md5sum $SRC
run() { # name compile-cmd... ; then runs $OUT/name (with RUNNER prefix)
  local n=$1; shift; local t0=$SECONDS
  if "$@" -o $OUT/$n > $OUT/$n.compile.log 2>&1; then
    echo "--- $n: compiled in $((SECONDS-t0)) s, $(stat -c %s $OUT/$n) bytes"; t0=$SECONDS
    ${RUNNER:-} $OUT/$n > $OUT/$n.run.log 2>&1; local rc=$?
    echo "    ran in $((SECONDS-t0)) s, exit=$rc, FAIL lines=$(grep -c '^FAIL' $OUT/$n.run.log)"; grep -E "ppu-conformance|^FAIL" $OUT/$n.run.log | head -5
  else echo "--- $n: COMPILE FAILED"; tail -5 $OUT/$n.compile.log; fi
}
echo "=== B. x86-64 native (validates lifted semantics on the host)"
RUNNER= run x86_gcc_O1   g++     -O1 $FLAGS $SRC
RUNNER= run x86_clang_O1 clang++ -O1 $FLAGS $SRC
RUNNER= run x86_clang_O2 clang++ -O2 $FLAGS $SRC
echo "=== C. arm64 NDK r29 clang, static (bionic), run under qemu-aarch64 -cpu cortex-a76 (closest qemu model to the A78: ARMv8.2, no SVE)"
RUNNER="qemu-aarch64 -cpu cortex-a76" run a64_O1_static $BIN/aarch64-linux-android29-clang++ -static -O1 -mcpu=cortex-a78 $FLAGS $SRC
RUNNER="qemu-aarch64 -cpu cortex-a76" run a64_O2_static $BIN/aarch64-linux-android29-clang++ -static -O2 -mcpu=cortex-a78 $FLAGS $SRC
echo "=== D. the dynamic binary selftest.sh produced, under qemu (needs Android's linker64: expected not to start)"
file $OUT/selftest/ppu_conformance 2>/dev/null
