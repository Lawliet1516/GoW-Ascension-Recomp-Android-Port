#!/usr/bin/env bash
# Usage: android/build.sh [build-dir]   (env: NDK, SDL3_ROOT, CMAKE)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NDK="${NDK:-$(ls -d "$HOME"/Android/Sdk/ndk/* | tail -1)}"
SDL3_ROOT="${SDL3_ROOT:-$HOME/toolchain-bt3/sdl3}"
CMAKE="${CMAKE:-$(ls -d "$HOME"/Android/Sdk/cmake/*/bin | tail -1)/cmake}"
BUILD="${1:-$HERE/../build-android}"
"$CMAKE" -S "$HERE" -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$(dirname "$CMAKE")/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  -DSDL3_ROOT="$SDL3_ROOT" ${CMAKE_EXTRA:-}
"$CMAKE" --build "$BUILD" -j"$(nproc)"
