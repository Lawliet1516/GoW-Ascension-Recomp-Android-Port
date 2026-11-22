#!/usr/bin/env bash
# android/lift/build_lifted.sh [build-dir]
#   env: NDK, CMAKE, LIFT_ROOT, CMAKE_EXTRA (extra cmake args)
#        LIFT_JOBS            parallel lifted-file compiles. Default 1. More only with a measured peak that fits (mem_guard.sh).
#        LIFT_MEM_RESERVE_MB  memory kept free for everything else (default 1024)
#        LIFT_MEM_FLOOR_MB    the watchdog stops the build when MemAvailable falls below this (default 768)
#
# Writes <build-dir>/.build-complete ONLY when the build finished; it is removed when a build starts, so libraries left by a build that
# failed or was stopped are never taken as valid. Memory samples: <build-dir>/lift-mem.tsv; one line per build: lift-builds.tsv.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/../.." && pwd)"
. "$HERE/mem_guard.sh"
if [ -z "${NDK:-}" ]; then NDK="$(ls -d "$HOME"/Android/Sdk/ndk/* 2>/dev/null | tail -1 || true)"; fi
[ -n "$NDK" ] && [ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "Android NDK not found: set NDK=<ndk dir> (r29: 29.0.14206865)"; exit 2; }
CMAKE="${CMAKE:-$(ls -d "$HOME"/Android/Sdk/cmake/*/bin 2>/dev/null | tail -1 || true)/cmake}"; [ -x "$CMAKE" ] || CMAKE=cmake
NINJA="$(dirname "$(command -v "$CMAKE")")/ninja"; [ -x "$NINJA" ] || NINJA="$(command -v ninja || true)"
[ -n "$NINJA" ] || { echo "ninja not found"; exit 2; }
BUILD="${1:-$REPO/build-lifted}"; mkdir -p "$BUILD"; BUILD="$(cd "$BUILD" && pwd)"

# A build directory inside the repository must be git-ignored: it will hold objects made from YOUR game code.
case "$BUILD/" in "$REPO"/*)
  git -C "$REPO" check-ignore -q "$BUILD/probe.o" 2>/dev/null || { echo "refusing: $BUILD is inside the repository and NOT git-ignored (objects of the game must never be committed)"; exit 2; } ;;
esac

RESERVE="${LIFT_MEM_RESERVE_MB:-1024}"; FLOOR="${LIFT_MEM_FLOOR_MB:-768}"; AVAIL="$(mem_avail_mb)"
PEAK=""; [ -s "$BUILD/.mem-peak-mb" ] && [ -f "$BUILD/.build-complete" ] && PEAK="$(cat "$BUILD/.mem-peak-mb")"
echo "memory: ${AVAIL} MB available, reserve ${RESERVE} MB, watchdog floor ${FLOOR} MB, measured per-compile peak: ${PEAK:-none yet}"
JOBS="$(mem_plan_jobs "${LIFT_JOBS:-1}" "$PEAK" "$AVAIL" "$RESERVE")" || exit 3
echo "-> $JOBS parallel lifted-file compile(s)"

rm -f "$BUILD/.build-complete"
"$CMAKE" -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release -DLIFT_JOBS="$JOBS" ${LIFT_ROOT:+-DLIFT_ROOT="$LIFT_ROOT"} ${CMAKE_EXTRA:-}

t0=$SECONDS; set +e
mem_guard_run "$FLOOR" "$BUILD/lift-mem.tsv" "$CMAKE" --build "$BUILD"; rc=$?
set -e
peak="$(cat "$BUILD/lift-mem.tsv.peak" 2>/dev/null || echo 0)"; minavail="$(awk 'NR>1 {if (m == "" || $2 < m) m = $2} END {print m + 0}' "$BUILD/lift-mem.tsv")"
printf '%s\tjobs=%s\trc=%s\twall_s=%s\tmax_compile_rss_mb=%s\tmin_avail_mb=%s\n' "$(date -Is)" "$JOBS" "$rc" $((SECONDS - t0)) "$peak" "$minavail" >> "$BUILD/lift-builds.tsv"
echo "build: exit $rc in $((SECONDS - t0)) s; largest compile process ${peak} MB; lowest available ${minavail} MB (samples: $BUILD/lift-mem.tsv)"
if [ $rc != 0 ]; then
  [ $rc = 75 ] && echo "STOPPED by the memory guard. Nothing here is a valid build. Free memory, keep LIFT_JOBS=1, or use a machine with more RAM; rerun to resume (ninja keeps finished objects)."
  exit $rc
fi
# the recorded peak only grows: it is what allows LIFT_JOBS>1 next time (the first build of a directory compiles every file)
old="$(cat "$BUILD/.mem-peak-mb" 2>/dev/null || echo 0)"; [ "$peak" -gt "$old" ] && echo "$peak" > "$BUILD/.mem-peak-mb"
{ for d in recomp spu; do f="${LIFT_ROOT:-$REPO/ascension/src}/$d/.lifted-from"; echo "$d: $(cat "$f" 2>/dev/null || echo 'no stamp')"; done; } > "$BUILD/.build-complete"
echo "built: $BUILD/libgow_ppu.a $BUILD/libgow_spu.a (complete: $BUILD/.build-complete)"
