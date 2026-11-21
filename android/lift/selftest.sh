#!/usr/bin/env bash
# Checks the lifting toolchain WITHOUT the game: the lifter's own host tests, then the PPU instruction conformance suite (1,202 instruction cases
# generated from the lifter's actual emissions) cross-compiled for arm64 and, if a phone is connected through adb, run on it.
#   android/lift/selftest.sh        env: PYTHON, NDK
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/../.." && pwd)"; KIT="$REPO/ps3kit"
PY="${PYTHON:-python3}"; NDK="${NDK:-$(ls -d "$HOME"/Android/Sdk/ndk/* | tail -1)}"
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++"; OUT="${OUT:-$REPO/build-lifted/selftest}"; mkdir -p "$OUT"
cd "$KIT"
for t in test_ppu_lifter_chunks test_ppu_branches test_ppu_jumptables test_ppu_fallthrough test_ppu_vmx_lanes test_ppu_vcmp_record_cr6; do
  echo "== $t"; "$PY" "tools/$t.py" | tail -2
done
echo "== PPU instruction conformance: generate + cross-compile for arm64"
"$PY" tools/test_ppu_lift.py --emit | tail -2
# the generated driver expects the game's ppu_recomp.h to define PPU_LEAN_CONST; for a test it is empty
"$CXX" -std=c++17 -O1 -w -ffp-contract=off -mcpu=cortex-a78 -D_GNU_SOURCE "-DPPU_LEAN_CONST=" -Iruntime -Iruntime/ppu -Iruntime/platform -Iinclude \
  scratch/ppu_conformance.cpp -o "$OUT/ppu_conformance"
if command -v adb >/dev/null && [ -n "$(adb devices | sed 1d | grep -w device || true)" ]; then
  adb push "$OUT/ppu_conformance" /data/local/tmp/ >/dev/null; echo "== on the device:"; adb shell /data/local/tmp/ppu_conformance | tail -3
else echo "(no adb device: the arm64 binary is $OUT/ppu_conformance)"; fi
