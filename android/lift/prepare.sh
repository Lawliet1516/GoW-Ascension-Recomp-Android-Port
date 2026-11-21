#!/usr/bin/env bash
# Linux port of ascension/scripts/windows/prepare.ps1: makes the game-specific sources from YOUR OWN decrypted EBOOT.ELF.
# The lifter and every tool it calls are Python and platform independent; only the PowerShell orchestration is replaced.
#
# NOTHING made here may be shared: it is translated game code. It goes to <repo>/ascension/src, ascension/meta/spu and WORK_DIR, all git-ignored.
#
#   android/lift/prepare.sh [--dry-run] [--force] [-j N]
#
# Environment:  WORK_DIR  (default <repo>/work)   must contain pristine/EBOOT.ELF (decrypted BCES01741 1.00; see README)
#               PYTHON    (default python3; needs ps3kit/requirements.txt)
# --dry-run: checks the tools, the inputs and the EBOOT's md5 and prints what would run; changes nothing.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/../.." && pwd)"
ASC="$REPO/ascension"; KIT="$REPO/ps3kit/tools"; TOOLS="$ASC/tools"; META="$ASC/meta"
RECOMP="$ASC/src/recomp"; SPU="$ASC/src/spu"; LOGS="$ASC/build/logs"
WORK="${WORK_DIR:-$REPO/work}"; PRISTINE="$WORK/pristine/EBOOT.ELF"; PATCHDIR="$WORK/patches"; ALLON="$PATCHDIR/all-on.ELF"
SITES="$META/patch_sites.json"; TABLE="$ASC/host/ascension_patch_table.inc"; JM="$WORK/jobmods"
PY="${PYTHON:-python3}"; FORCE=0; DRY=0; JOBS="$(nproc)"
while [ $# -gt 0 ]; do case "$1" in --force) FORCE=1;; --dry-run) DRY=1;; -j) JOBS="$2"; shift;; *) echo "unknown option $1"; exit 2;; esac; shift; done

step() {  # step "name" logfile command...
  local name="$1" log="$2"; shift 2; echo "== $name (log: $log)"
  if [ $DRY = 1 ]; then echo "   would run: $*"; return 0; fi
  local t0=$SECONDS
  if ! "$@" > "$log" 2>&1; then echo "FAILED: $name. Last lines of $log:"; tail -15 "$log"; exit 1; fi
  echo "   done in $((SECONDS - t0)) s"
}
need() { [ -e "$1" ] || { echo "missing required file: $1"; MISSING=1; }; }

# ---- 0. inputs and tools ------------------------------------------------------------------------------------------------
MISSING=0
for f in "$KIT/ppu_lifter.py" "$KIT/lift_jobmods.py" "$KIT/lift_spu_pic.py" "$KIT/build_spu_workloads.py" "$KIT/extract_spu_images.py" \
         "$TOOLS/jobmods_carve.py" "$TOOLS/jobmods_extra_funcs.json" "$TOOLS/rpcs3_patch.py" "$TOOLS/extract_spu_pm.py" "$TOOLS/job_fragments.json" \
         "$TOOLS/vram_guard_patch.py" "$TOOLS/vram_plan_patch.py" "$TOOLS/zprepass_patch.py" "$TOOLS/texstream_patch.py" "$TOOLS/gamelog_patch.py" "$TOOLS/mksrc.py" \
         "$META/functions.json" "$META/EBOOT.imports.json" "$ASC/patches-rpcs3/ascension-1.00.yml"; do need "$f"; done
[ $MISSING = 0 ] || { echo "the repository is incomplete"; exit 1; }
"$PY" - <<'PYEOF' || { echo "Python 3.10+ with the packages of ps3kit/requirements.txt is required (pip install -r ps3kit/requirements.txt)"; exit 1; }
import sys
assert sys.version_info >= (3, 10), sys.version
import capstone, Crypto, construct, tabulate, tqdm
PYEOF
if [ ! -f "$PRISTINE" ]; then
  echo "missing $PRISTINE"
  echo "Decrypt your own disc's PS3_GAME/USRDIR/EBOOT.BIN to EBOOT.ELF (RPCS3: Utilities > Decrypt PS3 Binaries, or ascension/tools/decrypt_self.py with"
  echo "your own key file) and put it there. Region/version supported by this script: Europe BCES01741 1.00."
  [ $DRY = 1 ] && { echo "(dry run: everything else checked and present)"; exit 0; }
  exit 1
fi
echo "EBOOT.ELF md5 $(md5sum "$PRISTINE" | cut -d' ' -f1)"
mkdir -p "$LOGS" "$PATCHDIR" "$META/spu"

# ---- 1. the SPU job modules ----------------------------------------------------------------------------------------------
if [ $FORCE = 1 ] || [ ! -f "$JM/jobmod_7868e9b847d1_e6D98.bin" ]; then
  step "carve the SPU job modules" "$LOGS/prepare-jobmods.log" "$PY" -I "$TOOLS/jobmods_carve.py" "$PRISTINE" "$TOOLS/jobmods_extra_funcs.json" "$JM" trailer+0 --write
  [ $DRY = 1 ] || [ -f "$JM/jobmod_7868e9b847d1_e6D98.bin" ] || { echo "the job modules do not match the known 1.00 disc EBOOT (is it BCES01741 1.00, decrypted?)"; exit 1; }
fi

# ---- 2. the game patches as run-time switches ------------------------------------------------------------------------------
PATCHES=("$ASC/patches-rpcs3/ascension-1.00.yml"); [ -f "$PATCHDIR/patch.yml" ] && PATCHES=("$PATCHDIR/patch.yml" "${PATCHES[@]}")
step "patch table" "$LOGS/prepare-patches.log" "$PY" -I "$TOOLS/rpcs3_patch.py" runtime --elf "$PRISTINE" --table "$TABLE" --sites "$SITES" --out "$ALLON" --patches "${PATCHES[@]}"

# ---- 3. the PPU code -> src/recomp, then the post-lift hooks -----------------------------------------------------------------
if [ $FORCE = 1 ] || [ ! -f "$RECOMP/ppu_recomp.h" ]; then
  step "translate the PPU code" "$LOGS/prepare-ppu.log" "$PY" "$KIT/ppu_lifter.py" "$PRISTINE" --functions "$META/functions.json" \
       --hle-stubs "$META/EBOOT.imports.json" --toc 0x81EF60 --code-end 0x62C4CC --chunk-lines 150000 -j "$JOBS" --patch-sites "$SITES" -o "$RECOMP"
  hooks() {
    for t in vram_guard_patch zprepass_patch texstream_patch gamelog_patch; do "$PY" "$TOOLS/$t.py" "$RECOMP" || return 1; done
    "$PY" "$TOOLS/vram_plan_patch.py" apply "$RECOMP" || return 1
    "$PY" -I "$TOOLS/mksrc.py" "$RECOMP"
  }
  step "post-lift hooks" "$LOGS/prepare-hooks.log" hooks
  [ $DRY = 1 ] || echo "runtime:$(md5sum "$PRISTINE" | cut -d' ' -f1):$(sha1sum "$SITES" | cut -d' ' -f1)" > "$RECOMP/.lifted-from"
fi

# ---- 4. the SPU programs embedded in the EBOOT -> meta/spu ---------------------------------------------------------------------
TMPON="$(mktemp -d)"; TMPORIG="$(mktemp -d)"; trap 'rm -rf "$TMPON" "$TMPORIG"' EXIT
extract() {
  "$PY" "$KIT/extract_spu_images.py" "$ALLON" -o "$TMPON" || return 1
  "$PY" "$KIT/extract_spu_images.py" "$PRISTINE" -o "$TMPORIG" || return 1
  "$PY" "$TOOLS/extract_spu_pm.py" "$PRISTINE" "$META/spu/spu_wws_pm_at_0077CA80.elf" --address 0x77CA80 --size 12288 --fnv 0xC8D774963A96DCF2
}
step "extract the SPU programs" "$LOGS/prepare-spu-extract.log" extract
if [ $DRY = 0 ]; then
  for f in "$TMPON"/*.elf; do
    b="$(basename "$f")"; cp -f "$f" "$META/spu/$b"
    if [ -f "$TMPORIG/$b" ] && ! cmp -s "$TMPORIG/$b" "$f"; then cp -f "$TMPORIG/$b" "$META/spu/spu_xorig_${b:4}"; fi
  done
  echo "   $(ls "$META/spu"/*.elf | wc -l) SPU programs in meta/spu"
fi

# ---- 5. the SPU programs -> src/spu --------------------------------------------------------------------------------------------
if [ $FORCE = 1 ] || [ ! -f "$SPU/spu_workloads.c" ]; then
  mkdir -p "$SPU"
  export SPU_LIFT_XREGS=1
  LIFT_JOBMODS_MERGE=1 step "translate the SPU job modules" "$LOGS/prepare-spu-jobmods.log" "$PY" "$KIT/lift_jobmods.py" --jobmods "$JM" --out "$SPU" \
       --register "$SPU/jobmods_register.c" --symbol-prefix ascension --extra-funcs-json "$TOOLS/jobmods_extra_funcs.json"
  step "translate the SPU code fragments" "$LOGS/prepare-spu-fragments.log" "$PY" "$KIT/lift_spu_pic.py" --elf "$PRISTINE" --fragments "$TOOLS/job_fragments.json" \
       --out "$SPU" --register "$SPU/pic_register.c" --symbol-prefix ascension
  step "translate the SPU programs" "$LOGS/prepare-spu-images.log" "$PY" "$KIT/build_spu_workloads.py" --images "$META/spu" --lifted "$SPU" \
       --out "$SPU/spu_workloads.c" --register-fn ascension_spu_register_all --constructor --title ascension --relift \
       --extra-funcs "spu_wws_pm_at_0077CA80=0xA2C,0xA68,0x3368,0x3378"
fi
echo "prepared. Next: android/lift/build_lifted.sh"
