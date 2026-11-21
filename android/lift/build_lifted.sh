#!/usr/bin/env bash
# android/lift/build_lifted.sh [build-dir]   env: NDK, LIFT_JOBS (default: RAM(GB)/4, at least 1), LIFT_ROOT, extra cmake args in CMAKE_EXTRA
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/../.." && pwd)"
NDK="${NDK:-$(ls -d "$HOME"/Android/Sdk/ndk/* | tail -1)}"
CMAKE="${CMAKE:-$(ls -d "$HOME"/Android/Sdk/cmake/*/bin 2>/dev/null | tail -1)/cmake}"; [ -x "$CMAKE" ] || CMAKE=cmake
NINJA="$(dirname "$CMAKE")/ninja"; [ -x "$NINJA" ] || NINJA=ninja
RAM_GB=$(awk '/MemTotal/ {printf "%d", $2/1048576}' /proc/meminfo)
JOBS="${LIFT_JOBS:-$(( RAM_GB / 4 > 0 ? RAM_GB / 4 : 1 ))}"
BUILD="${1:-$REPO/build-lifted}"
echo "RAM ${RAM_GB} GB -> ${JOBS} parallel lifted-file compiles (each needs 2-4 GB; if a compile is killed, lower LIFT_JOBS)"
[ "$RAM_GB" -ge 8 ] || echo "WARNING: under 8 GB of RAM the lifted PPU files will not compile reliably. Use a bigger machine (android/lift/README.md)."
"$CMAKE" -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release -DLIFT_JOBS="$JOBS" ${LIFT_ROOT:+-DLIFT_ROOT="$LIFT_ROOT"} ${CMAKE_EXTRA:-}
"$CMAKE" --build "$BUILD"
echo "built: $BUILD/libgow_ppu.a $BUILD/libgow_spu.a"
