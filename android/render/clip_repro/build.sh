#!/usr/bin/env bash
# Standalone Vulkan repro for gl_ClipDistance (no RSX code). Builds: out/clip_repro (executable, no layers) and out/libclip_repro.so (JNI, for the APK + validation layer)
#   android/render/clip_repro/build.sh [outdir]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="${1:-$HERE/out}"; mkdir -p "$OUT/spv"
NDK="${NDK:-$(ls -d "$HOME"/Android/Sdk/ndk/* | tail -1)}"; ST="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"; SH="$NDK/shader-tools/linux-x86_64"
CC="$ST/aarch64-linux-android29-clang"
for s in clip1.vert clip4all.vert clip4rsx.vert solid.frag; do
  "$SH/glslc" --target-env=vulkan1.0 -O0 "$HERE/shaders/$s" -o "$OUT/spv/$s.spv"
  "$SH/spirv-val" --target-env vulkan1.0 "$OUT/spv/$s.spv"
  "$SH/spirv-dis" "$OUT/spv/$s.spv" > "$OUT/spv/$s.dis.txt"
  "$SH/glslc" --target-env=vulkan1.0 -O0 -mfmt=c "$HERE/shaders/$s" -o "$OUT/spv/$s.inc"
done
CFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -I$OUT/spv"
"$CC" $CFLAGS "$HERE/clip_repro.c" -o "$OUT/clip_repro" -lvulkan -llog
"$CC" $CFLAGS -fPIC -shared -DCLIP_REPRO_JNI "$HERE/clip_repro.c" -o "$OUT/libclip_repro.so" -lvulkan -llog
echo "built in $OUT"
