#!/usr/bin/env bash
# Builds libgowa_app.so with the NDK and copies it and libSDL3.so into the Gradle project, then (optionally) the APK.
#   android/app/build_native.sh [apk]      env: NDK, SDL3_ROOT, JAVA_HOME
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NDK="${NDK:-$(ls -d "$HOME"/Android/Sdk/ndk/* | tail -1)}"; SDL3_ROOT="${SDL3_ROOT:-$HOME/toolchain-bt3/sdl3}"
BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
OUT="$HERE/app/src/main/jniLibs/arm64-v8a"; mkdir -p "$OUT"
"$BIN/aarch64-linux-android29-clang" -std=gnu11 -O2 -mcpu=cortex-a78 -fPIC -shared -Wall -Wextra -Wno-unused-parameter \
  -I"$SDL3_ROOT/include" -I"$HERE/native" "$HERE/native/gowa_app.c" -o "$OUT/libgowa_app.so" \
  -L"$SDL3_ROOT/lib" -lSDL3 -lvulkan -llog -landroid -lm
cp "$SDL3_ROOT/lib/libSDL3.so" "$OUT/"
echo "native libs in $OUT"
if [ "${1:-}" = "apk" ]; then
  export JAVA_HOME="${JAVA_HOME:-$HOME/Android/jdk}"; export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
  (cd "$HERE" && ./gradlew --offline -q assembleDebug)
  echo "APK: $HERE/app/build/outputs/apk/debug/app-debug.apk"
fi
