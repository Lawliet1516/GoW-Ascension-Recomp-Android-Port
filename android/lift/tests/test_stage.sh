#!/usr/bin/env bash
# Regression tests for the staged, resumable outputs of android/lift/prepare.sh (lift_stage.sh). No game data: a synthetic generator
# writes files the way ppu_lifter.py does (header first, then chunks, each through *.tmp + rename, then hooks that rewrite chunks IN
# PLACE) and is killed with SIGKILL at chosen points, so nothing gets a chance to clean up -- the same as a dropped Colab session.
#   android/lift/tests/test_stage.sh        exit 0 = all passed
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIB="$HERE/../lift_stage.sh"; PREP="$HERE/../prepare.sh"; LIFTER="$HERE/../../../ps3kit/tools/ppu_lifter.py"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "ok   $*"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL $*"; }
expect() { local what="$1"; shift; if "$@"; then ok "$what"; else bad "$what"; fi; }

# The step-3 sequence of prepare.sh, with the generator and hooks replaced. $3 = where to die (none|header|chunk|hooks|promote).
cat > "$T/step3.sh" <<'EOF'
. "$LIB"
FINAL="$1"; STAMP="$2"; DIE="${3:-none}"; LOG="$(dirname "$FINAL")/lift.log"
gen() {  # like ppu_lifter.py: header via tmp+rename, then 3 chunks via tmp+rename, then the summary lines prepare.sh parses
  local d="$1" i
  if [ "$DIE" = header ]; then head -c 5000 /dev/urandom > "$d/ppu_recomp.h.tmp"; kill -9 $$; fi
  printf '/* header %s */\n' "$STAMP" > "$d/ppu_recomp.h.tmp" && mv "$d/ppu_recomp.h.tmp" "$d/ppu_recomp.h"
  for i in 000 001 002; do
    if [ "$DIE" = chunk ] && [ $i = 001 ]; then head -c 70000 /dev/zero | tr '\0' x > "$d/ppu_recomp_$i.cpp.tmp"; kill -9 $$; fi
    { printf '#include "ppu_recomp.h"\n'; for n in $(seq 1 2000); do echo "void f_${i}_$n(void) { /* $STAMP */ }"; done; } > "$d/ppu_recomp_$i.cpp.tmp"
    mv "$d/ppu_recomp_$i.cpp.tmp" "$d/ppu_recomp_$i.cpp"
  done
  echo "Wrote 3 source chunks: ppu_recomp_000.cpp .. ppu_recomp_002.cpp"; echo "  6000 functions lifted"
}
hooks() {  # like zprepass_patch.py & co: rewrite a chunk in place (open "w"); dying here truncates it
  local d="$1"; local body; body="$(cat "$d/ppu_recomp_001.cpp")"
  if [ "$DIE" = hooks ]; then printf '%s' "${body:0:1000}" > "$d/ppu_recomp_001.cpp"; kill -9 $$; fi
  printf '%s\n/* hooked */\n' "$body" > "$d/ppu_recomp_001.cpp"
}
stage_recover "$FINAL"
if why="$(stage_check "$FINAL" "$STAMP")"; then echo "SKIPPED: complete"; exit 0; fi
echo "REASON: $why"
STG="$(stage_begin "$FINAL")"
gen "$STG" > "$LOG" || exit 1
hooks "$STG" || exit 1
check_ppu "$STG" "$LOG" > /dev/null || { echo "CHECK FAILED"; exit 1; }
stage_seal "$STG" "$STAMP"
if [ "$DIE" = promote ]; then mv "$FINAL" "$FINAL.prev" 2>/dev/null; kill -9 $$; fi   # between the two renames of stage_promote
stage_promote "$STG" "$FINAL"
echo "DONE"
EOF
run() { LIB="$LIB" bash "$T/step3.sh" "$@" 2>&1; }
. "$LIB"
R="$T/src/recomp"; mkdir -p "$T/src"

echo "== 1. first run"
out="$(run "$R" A)"; expect "first run finishes" grep -q DONE <<<"$out"
expect "complete for stamp A" stage_check "$R" A
expect "chunks, header, manifest, stamp, info present" test -s "$R/ppu_recomp_002.cpp" -a -s "$R/.manifest" -a -s "$R/.lifted-from" -a -s "$R/.lift-info"
expect ".lift-info records 3 chunks / 6000 functions" grep -qz "chunks=3.functions=6000" "$R/.lift-info"
expect "no staging left" test ! -e "$T/src/.recomp.staging"
ref="$(cat "$R/.manifest")"

echo "== 5. second run, nothing changed"
m0="$(stat -c %Y.%s "$R"/ppu_recomp_*.cpp)"; out="$(run "$R" A)"
expect "skipped" grep -q "SKIPPED: complete" <<<"$out"
expect "files untouched (mtime+size)" test "$m0" = "$(stat -c %Y.%s "$R"/ppu_recomp_*.cpp)"

echo "== 2. killed while writing the header (inputs changed to B: a relift is due)"
out="$(run "$R" B header)"
expect "the complete A output is untouched" stage_check "$R" A
expect "a staging directory is left behind" test -e "$T/src/.recomp.staging/ppu_recomp.h.tmp"
expect "not taken as complete for B" bash -c ". '$LIB'; ! stage_check '$R' B >/dev/null"

echo "== 3. killed in the middle of the chunks"
out="$(run "$R" B chunk)"
expect "the complete A output is still untouched" stage_check "$R" A
expect "staging holds a header but only part of the chunks" test -s "$T/src/.recomp.staging/ppu_recomp.h" -a ! -e "$T/src/.recomp.staging/ppu_recomp_002.cpp"

echo "== 3b. killed inside the hooks (a chunk truncated in place)"
out="$(run "$R" B hooks)"
expect "the complete A output is still untouched" stage_check "$R" A
expect "staging chunk is truncated (what the old prepare.sh would have accepted)" test "$(stat -c %s "$T/src/.recomp.staging/ppu_recomp_001.cpp")" -le 1000

echo "== 4. resume after the interruptions"
out="$(run "$R" B)"; expect "resume finishes" grep -q DONE <<<"$out"
expect "the leftover staging was reported" grep -q "staging directory of an interrupted run" <<<"$out"
expect "complete for B" stage_check "$R" B
expect "previous complete A kept as recomp.prev and still verifies" stage_check "$R.prev" A
clean="$T/clean/src/recomp"; mkdir -p "$T/clean/src"; run "$clean" B > /dev/null
expect "resumed result is byte-identical to an uninterrupted run" test "$(cat "$R/.manifest")" = "$(cat "$clean/.manifest")"
expect "no *.tmp in the result" test -z "$(find "$R" -name '*.tmp')"
out="$(run "$R" B)"; expect "idempotent: a further run skips" grep -q "SKIPPED" <<<"$out"

echo "== 6. incomplete or inconsistent trees are detected"
cp -a "$R" "$T/x"
truncate -s 100 "$T/x/ppu_recomp_002.cpp"; expect "truncated chunk" bash -c ". '$LIB'; why=\$(stage_check '$T/x' B); [ \$? = 1 ] && grep -q 'differ from the manifest' <<<\"\$why\""
rm -rf "$T/x"; cp -a "$R" "$T/x"; rm "$T/x/ppu_recomp_001.cpp"; expect "missing chunk" bash -c ". '$LIB'; ! stage_check '$T/x' B >/dev/null"
rm -rf "$T/x"; cp -a "$R" "$T/x"; echo junk > "$T/x/ppu_recomp_003.cpp.tmp"; expect "stray *.tmp" bash -c ". '$LIB'; ! stage_check '$T/x' B >/dev/null"
rm -rf "$T/x"; cp -a "$R" "$T/x"; rm "$T/x/.lifted-from"; expect "no stamp" bash -c ". '$LIB'; stage_check '$T/x' B | grep -q 'no .lifted-from'"
rm -rf "$T/x"; cp -a "$R" "$T/x"; rm "$T/x/.manifest"; expect "old-style tree (stamp, no manifest) is not trusted" bash -c ". '$LIB'; stage_check '$T/x' B | grep -q 'no .manifest'"
expect "other inputs (stamp A vs B)" bash -c ". '$LIB'; stage_check '$R' A | grep -q 'other inputs'"
rm -rf "$T/x"; mkdir -p "$T/x"; echo h > "$T/x/ppu_recomp.h"; expect "header only: check_ppu refuses" bash -c ". '$LIB'; printf 'Wrote 3 source chunks: a\n' > '$T/l'; ! check_ppu '$T/x' '$T/l' >/dev/null"
expect "check_ppu refuses a log without the chunk report (lifter died)" bash -c ". '$LIB'; : > '$T/l2'; ! check_ppu '$R' '$T/l2' >/dev/null"

echo "== 7. killed between the two renames of the promotion"
out="$(run "$R" C promote)"
expect "final missing, .prev present (the crash state)" test ! -e "$R" -a -d "$R.prev"
out="$(run "$R" C)"; expect "next run recovers .prev, then relifts for C" grep -q "recovered recomp from recomp.prev" <<<"$out"
expect "complete for C" stage_check "$R" C

echo "== 8. an incomplete final tree is kept as .incomplete, not deleted, and never as .prev"
rm "$R/.lifted-from"; out="$(run "$R" C)"
expect "kept as recomp.incomplete" test -d "$R.incomplete"
expect "complete for C again" stage_check "$R" C

echo "== 9. prepare.sh and the lifter still match what these tests assume"
expect "lifter prints 'Wrote N source chunks'" grep -q 'print(f"Wrote {len(paths)} source chunks' "$LIFTER"
expect "lifter prints 'N functions lifted'" grep -q 'functions lifted")' "$LIFTER"
seq3="$(grep -o -E 'stage_recover "\$RECOMP"|stage_check "\$RECOMP"|stage_begin "\$RECOMP"|check_ppu "\$STG"|stage_seal "\$STG" "\$PPU_STAMP"|stage_promote "\$STG" "\$RECOMP"' "$PREP" | tr '\n' '|')"
expect "prepare.sh step 3 runs recover|check|begin|check_ppu|seal|promote in this order" test "$seq3" = 'stage_recover "$RECOMP"|stage_check "$RECOMP"|stage_begin "$RECOMP"|check_ppu "$STG"|stage_seal "$STG" "$PPU_STAMP"|stage_promote "$STG" "$RECOMP"|'
expect "prepare.sh no longer gates on ppu_recomp.h alone" bash -c "! grep -q '! -f \"\$RECOMP/ppu_recomp.h\"' '$PREP'"

echo "passed $PASS, failed $FAIL"; [ $FAIL = 0 ]
