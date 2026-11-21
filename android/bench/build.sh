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
