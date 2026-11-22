#!/usr/bin/env bash
# runs every Python test of a snapshot's ps3kit sequentially, one result line per test (PASS/FAIL/SKIP/TIMEOUT), per-test log + time
SNAP="$1"; OUT="$2"; PY=/root/work/colab-station/toolchain/venv/bin/python; mkdir -p "$OUT"; cd "$SNAP/ps3kit"
: > "$OUT/summary.tsv"
for t in tools/test_*.py tests/test_*.py tools/compare/selftest.py; do
  n=$(echo "$t" | tr '/' '_'); t0=$(date +%s.%N)
  timeout 900 "$PY" "$t" > "$OUT/$n.log" 2>&1; rc=$?
  dt=$(echo "$(date +%s.%N) - $t0" | bc)
  if [ $rc = 124 ]; then st=TIMEOUT; elif [ $rc != 0 ]; then st=FAIL; elif grep -q -i -E "^SKIP|skipped|requires (macOS|Windows)|not run" "$OUT/$n.log"; then st=PASS?SKIP; else st=PASS; fi
  printf '%s\t%s\t%.1f\t%s\n' "$t" "$st" "$dt" "$rc" | tee -a "$OUT/summary.tsv"
done
