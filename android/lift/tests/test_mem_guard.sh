#!/usr/bin/env bash
# Tests for android/lift/mem_guard.sh and the memory handling of build_lifted.sh.
#   android/lift/tests/test_mem_guard.sh      env: NDK (for the build_lifted.sh part; without it that part is reported SKIPPED, not passed)
# The build_lifted.sh part compiles a SYNTHETIC tree of two trivial files (not game code): it exercises the script's mechanics
# (job planning, watchdog, completion marker, resume), and says nothing about the memory or time of the real lifted code.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIFT="$HERE/.."; . "$LIFT/mem_guard.sh"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
PASS=0; FAIL=0; SKIP=0
ok() { PASS=$((PASS+1)); echo "ok   $*"; }; bad() { FAIL=$((FAIL+1)); echo "FAIL $*"; }
expect() { local w="$1"; shift; if "$@"; then ok "$w"; else bad "$w"; fi; }
plan() { mem_plan_jobs "$@" 2>/dev/null; }

echo "== job planning (mem_plan_jobs requested peak avail reserve)"
expect "1 job, 8000 MB free -> 1"                          test "$(plan 1 '' 8000 1024)" = 1
expect "1 job, 4000 MB free -> refused (4096+1024 needed)"  bash -c ". '$LIFT/mem_guard.sh'; ! mem_plan_jobs 1 '' 4000 1024 2>/dev/null"
expect "2 jobs without a measurement -> refused"            bash -c ". '$LIFT/mem_guard.sh'; ! mem_plan_jobs 2 '' 12000 1024 2>/dev/null"
expect "2 jobs, peak 5000 -> 13524 > 12000 -> refused"      bash -c ". '$LIFT/mem_guard.sh'; ! mem_plan_jobs 2 5000 12000 1024 2>/dev/null"
expect "2 jobs, peak 3000 -> 8524 <= 12000 -> 2"            test "$(plan 2 3000 12000 1024)" = 2
expect "3 jobs, peak 3000 -> 12274 > 12000 -> refused"      bash -c ". '$LIFT/mem_guard.sh'; ! mem_plan_jobs 3 3000 12000 1024 2>/dev/null"
expect "LIFT_JOBS=abc -> refused"                           bash -c ". '$LIFT/mem_guard.sh'; ! mem_plan_jobs abc '' 12000 1024 2>/dev/null"
expect "LIFT_JOBS=0 -> refused"                             bash -c ". '$LIFT/mem_guard.sh'; ! mem_plan_jobs 0 '' 12000 1024 2>/dev/null"

echo "== watchdog (mem_guard_run)"
mem_guard_run 1 "$T/a.tsv" bash -c 'exit 0'; expect "command succeeds -> 0" test $? = 0
mem_guard_run 1 "$T/b.tsv" bash -c 'exit 7'; expect "command fails -> its exit code (7)" test $? = 7
expect "samples logged with header" grep -q "^t_s.avail_mb" "$T/a.tsv"
left() { ps -eo stat=,comm=,args= | awk -v a="$1" '$1 !~ /^Z/ && $2 == "sleep" && $4 == a' | wc -l; }   # by comm, never matches this script
t0=$SECONDS; mem_guard_run 99999999 "$T/c.tsv" bash -c 'sleep 307 & sleep 307; wait' 2>"$T/c.err"; rc=$?
expect "below the floor -> stopped, exit 75" test $rc = 75
expect "stopped within 5 s (no waiting on the zombie)" test $((SECONDS - t0)) -le 5
expect "the reason is printed" grep -q "MEMORY GUARD" "$T/c.err"
expect "no process of the stopped tree is left (incl. background grandchildren)" test "$(left 307)" = 0
mem_guard_run 99999999 "$T/e.tsv" bash -c 'setsid sleep 308 & bash -c "sleep 308" & wait' 2>/dev/null
expect "children in other process groups / sessions are stopped too" test "$(left 308)" = 0
expect "peak 0 (nothing sampled) is not a measurement: 2 jobs refused" bash -c ". '$LIFT/mem_guard.sh'; ! mem_plan_jobs 2 0 12000 1024 2>/dev/null"
CXX="$(command -v clang++ || true)"
if [ -n "$CXX" ]; then
  printf '#include <map>\n#include <string>\n#include <vector>\n#include <regex>\n%s\nint main(){return 0;}\n' "$(for i in $(seq 1 60); do echo "std::map<std::string,std::vector<std::regex>> m$i;"; done)" > "$T/heavy.cpp"
  mem_guard_run 1 "$T/d.tsv" "$CXX" -O2 -c "$T/heavy.cpp" -o "$T/heavy.o"; rc=$?
  expect "real clang compile under the guard succeeds" test $rc = 0
  expect "its RSS was measured (> 50 MB)" test "$(cat "$T/d.tsv.peak")" -gt 50
  echo "     measured clang peak: $(cat "$T/d.tsv.peak") MB"
else SKIP=$((SKIP+1)); echo "SKIP clang RSS measurement (no clang++ on PATH)"; fi

echo "== build_lifted.sh"
if [ -z "${NDK:-}" ]; then SKIP=$((SKIP+1)); echo "SKIP build_lifted.sh integration (set NDK)"; else
  out="$(NDK=/nonexistent bash "$LIFT/build_lifted.sh" "$T/b0" 2>&1)"; rc=$?
  expect "bad NDK -> clear error, exit 2" bash -c "[ $rc = 2 ] && grep -q 'NDK not found' <<<'$out'"
  REPO="$(cd "$LIFT/../.." && pwd)"; d="$REPO/android/lift/not-ignored-build-$$"
  out="$(bash "$LIFT/build_lifted.sh" "$d" 2>&1)"; rc=$?; rm -rf "$d"
  expect "build dir inside the repo and not ignored -> refused, exit 2" bash -c "[ $rc = 2 ] && grep -q 'NOT git-ignored' <<<'$out'"
  SRC="$T/synthetic-src"; mkdir -p "$SRC/recomp" "$SRC/spu"
  echo '/* SYNTHETIC test tree: not game code */' > "$SRC/recomp/ppu_recomp.h"
  printf '#include "ppu_recomp.h"\nint synthetic_ppu_%s(int x) { return x * %s; }\n' a 3 > "$SRC/recomp/ppu_recomp_000.cpp"
  printf 'int synthetic_spu(int x) { return x + 1; }\n' > "$SRC/spu/spu_workloads.c"
  out="$(LIFT_JOBS=2 LIFT_ROOT="$SRC" bash "$LIFT/build_lifted.sh" "$T/b1" 2>&1)"; rc=$?
  expect "LIFT_JOBS=2 in a fresh build dir (no measurement) -> refused, exit 3" bash -c "[ $rc = 3 ] && grep -q 'no measured per-compile peak' <<<'$out'"
  out="$(LIFT_ROOT="$SRC" bash "$LIFT/build_lifted.sh" "$T/b1" 2>&1)"; rc=$?
  expect "default build: 1 job" grep -q -- "-> 1 parallel" <<<"$out"
  expect "default build succeeds" test $rc = 0
  expect "libraries made" test -s "$T/b1/libgow_ppu.a" -a -s "$T/b1/libgow_spu.a"
  expect ".build-complete written" test -f "$T/b1/.build-complete"
  expect "per-build line + memory samples recorded" test -s "$T/b1/lift-builds.tsv" -a -s "$T/b1/lift-mem.tsv"
  pk="$(cat "$T/b1/.mem-peak-mb" 2>/dev/null || echo none)"
  expect "peak recorded only if a compile was actually sampled (got: $pk)" bash -c "[ '$pk' = none ] || [ '$pk' -gt 0 ]"
  touch "$SRC/recomp/ppu_recomp_000.cpp"
  out="$(LIFT_MEM_FLOOR_MB=99999999 LIFT_ROOT="$SRC" bash "$LIFT/build_lifted.sh" "$T/b1" 2>&1)"; rc=$?
  expect "floor above available memory -> stopped by the guard, exit 75" test $rc = 75
  expect "after the stop, .build-complete is gone (old libraries not presented as valid)" test ! -f "$T/b1/.build-complete"
  expect "the stop is explained" grep -q "STOPPED by the memory guard" <<<"$out"
  out="$(LIFT_ROOT="$SRC" bash "$LIFT/build_lifted.sh" "$T/b1" 2>&1)"; rc=$?
  expect "rerun resumes and completes" bash -c "[ $rc = 0 ] && [ -f '$T/b1/.build-complete' ]"
  expect "both runs logged (rc=75 then rc=0)" bash -c "grep -q 'rc=75' '$T/b1/lift-builds.tsv' && tail -1 '$T/b1/lift-builds.tsv' | grep -q 'rc=0'"
fi
echo "passed $PASS, failed $FAIL, skipped $SKIP"; [ $FAIL = 0 ]
