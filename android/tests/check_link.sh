#!/usr/bin/env bash
# Full-library link check for the Android runtime: links EVERY object of libps3kit_android.a + libgowa_host_stubs.a (--whole-archive,
# so nothing is left out because no test happens to call it) into a shared object with --no-undefined, and reports undefined and
# duplicate symbols by name. Exit 0 only if both lists are empty.
#   android/tests/check_link.sh <build-dir> [extra objects or archives...]     env: NDK, SDL3_ROOT
set -uo pipefail
B="${1:?build dir}"; shift
NDK="${NDK:?set NDK}"; SDL3_ROOT="${SDL3_ROOT:?set SDL3_ROOT}"
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang++"
OUT="$B/check_link"; mkdir -p "$OUT"
"$CXX" -shared -o "$OUT/linkcheck.so" -Wl,--no-undefined -Wl,--error-limit=0 \
  -Wl,--whole-archive "$B/libps3kit_android.a" "$B/libgowa_host_stubs.a" "$@" -Wl,--no-whole-archive \
  -L"$SDL3_ROOT/lib" -lSDL3 -llog -landroid -ldl -lm -static-libstdc++ > "$OUT/link.log" 2>&1
rc=$?
grep -o "undefined symbol: [^ ]*" "$OUT/link.log" | sed 's/undefined symbol: //' | sort -u > "$OUT/undefined.txt"
grep -o "duplicate symbol: [^ ]*" "$OUT/link.log" | sed 's/duplicate symbol: //' | sort -u > "$OUT/duplicate.txt"
echo "link exit=$rc  undefined=$(wc -l < "$OUT/undefined.txt")  duplicate=$(wc -l < "$OUT/duplicate.txt")  (log: $OUT/link.log)"
[ -s "$OUT/undefined.txt" ] && echo "  undefined: $(tr '\n' ' ' < "$OUT/undefined.txt")"
[ -s "$OUT/duplicate.txt" ] && echo "  duplicate: $(tr '\n' ' ' < "$OUT/duplicate.txt")"
[ $rc = 0 ] && [ ! -s "$OUT/undefined.txt" ] && [ ! -s "$OUT/duplicate.txt" ]
