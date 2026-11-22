# Staged, resumable outputs for android/lift/prepare.sh (sourced, not run).
#
# A step that makes a directory tree (the PPU lift: header + ~12 chunk files + 6 post-lift hooks; the SPU lift) is built in a sibling
# staging directory, checked, sealed and only then moved into place. "Complete" means: the directory holds a .lifted-from stamp equal to
# the stamp of the current inputs AND a .manifest whose every line verifies (sha1, nothing missing, nothing extra). A file existing is
# never taken as proof: ppu_recomp.h is written before the chunks, and 4 of the hooks rewrite the chunks in place, so an interrupted run
# can leave a header, a truncated chunk or a *.tmp behind.
#
#   <final>                 the finished output (what the build reads)
#   <dir>/.<name>.staging   the run in progress; removed when the next run starts (it was interrupted)
#   <final>.prev            the previous COMPLETE output, kept when a new one replaces it (one generation)
#   <final>.incomplete      a previous output that did not verify, kept for inspection instead of being deleted (one generation)
#
# Order inside stage_seal: manifest first, stamp last. Order of stage_promote: final -> .prev/.incomplete, staging -> final; if the
# process dies between the two renames, stage_recover puts .prev back.

stage_path() { echo "$(dirname "$1")/.$(basename "$1").staging"; }

# stage_manifest <dir>: sha1 of every regular file except the seal files, sorted, paths relative to <dir>
stage_manifest() {
  (cd "$1" && find . -type f ! -name .manifest ! -name .lifted-from -printf '%P\0' | LC_ALL=C sort -z | xargs -0 -r sha1sum)
}

# stage_check <final> <stamp>: 0 = complete for these inputs. Otherwise 1 and the reason on stdout.
stage_check() {
  local d="$1" want="$2"
  [ -d "$d" ] || { echo "absent"; return 1; }
  [ -f "$d/.lifted-from" ] || { echo "no .lifted-from stamp (interrupted, or made by an older prepare.sh)"; return 1; }
  [ -f "$d/.manifest" ] || { echo "no .manifest (made by an older prepare.sh: cannot be verified)"; return 1; }
  local have; have="$(cat "$d/.lifted-from")"
  [ "$have" = "$want" ] || { echo "made from other inputs (stamp $have, now $want)"; return 1; }
  local now; now="$(stage_manifest "$d")" || { echo "cannot read the files"; return 1; }
  if [ "$now" != "$(cat "$d/.manifest")" ]; then
    echo "files differ from the manifest: $(diff <(cat "$d/.manifest") <(echo "$now") | grep -c '^[<>]') line(s) (missing, changed or extra files)"
    return 1
  fi
  return 0
}

# stage_recover <final>: a crash between the two renames of stage_promote leaves no <final> but a .prev: put it back
stage_recover() {
  local d="$1"
  if [ ! -e "$d" ] && [ -d "$d.prev" ]; then
    mv "$d.prev" "$d" && echo "   recovered $(basename "$d") from $(basename "$d").prev (an earlier promotion was interrupted)"
  fi
  return 0
}

# stage_begin <final>: a fresh, empty staging directory (a leftover one is an interrupted run: it is removed); prints its path
stage_begin() {
  local s; s="$(stage_path "$1")"
  if [ -e "$s" ]; then echo "   removing the staging directory of an interrupted run: $s" >&2; rm -rf "$s"; fi
  mkdir -p "$s" && echo "$s"
}

# stage_seal <staging> <stamp>: manifest, then the stamp (last write: its presence means everything before it finished)
stage_seal() {
  local m; m="$(stage_manifest "$1")" || return 1   # computed before any seal file exists, so the manifest never lists itself
  printf '%s\n' "$m" > "$1/.manifest.tmp" && mv "$1/.manifest.tmp" "$1/.manifest" && \
  printf '%s' "$2" > "$1/.lifted-from.tmp" && mv "$1/.lifted-from.tmp" "$1/.lifted-from"
}

# stage_promote <staging> <final>: keep what was there (.prev if it verified with its own manifest, else .incomplete), then move in
stage_promote() {
  local s="$1" d="$2"
  if [ -e "$d" ]; then
    local keep="$d.incomplete"
    if [ -f "$d/.lifted-from" ] && [ -f "$d/.manifest" ] && [ "$(stage_manifest "$d")" = "$(cat "$d/.manifest")" ]; then keep="$d.prev"; fi
    rm -rf "$keep" && mv "$d" "$keep" && echo "   kept the previous output as $(basename "$keep")"
  fi
  mv "$s" "$d"
}

# ---- output checks (run on the staging directory before it is sealed) ------------------------------------------------------------
check_ppu() {  # check_ppu <dir> <lifter log>: the header, exactly the chunks the lifter reported, none empty, no *.tmp left
  local d="$1" n f; n="$(sed -n 's/^Wrote \([0-9][0-9]*\) source chunks.*/\1/p' "$2" | tail -1)"
  [ -n "$n" ] || { echo "the lifter log does not report its chunks"; return 1; }
  [ -s "$d/ppu_recomp.h" ] || { echo "ppu_recomp.h missing or empty"; return 1; }
  [ "$(find "$d" -maxdepth 1 -name 'ppu_recomp_*.cpp' | wc -l)" = "$n" ] || { echo "the lifter wrote $n chunks, the directory has a different count"; return 1; }
  for f in "$d"/ppu_recomp_*.cpp; do [ -s "$f" ] || { echo "empty chunk $f"; return 1; }; done
  [ -z "$(find "$d" -name '*.tmp' | head -1)" ] || { echo "temporary files left: $(find "$d" -name '*.tmp' | head -3)"; return 1; }
  printf 'chunks=%s\nfunctions=%s\n' "$n" "$(sed -n 's/^ *\([0-9][0-9]*\) functions lifted.*/\1/p' "$2" | tail -1)" > "$d/.lift-info"
  echo "ok: $(cat "$d/.lift-info" | tr '\n' ' ')"
}
check_spu() {  # check_spu <dir>: the three outputs the build needs, non-empty, no *.tmp left
  local f; for f in jobmods_register.c pic_register.c spu_workloads.c; do [ -s "$1/$f" ] || { echo "missing or empty: $f"; return 1; }; done
  [ -z "$(find "$1" -name '*.tmp' | head -1)" ] || { echo "temporary files left"; return 1; }
  echo "ok: $(find "$1" -name '*.c' | wc -l) C files"
}
