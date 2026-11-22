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
. "$HERE/lift_stage.sh"   # staged outputs: a step is done only when its sealed output verifies (see that file)

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
MD5="$(md5sum "$PRISTINE" | cut -d' ' -f1)"; echo "EBOOT.ELF md5 $MD5"
# --dry-run changes nothing: no directory, no file, no temporary is made from here on (step() only prints in that mode)
[ $DRY = 1 ] || mkdir -p "$LOGS" "$PATCHDIR" "$META/spu"
sum_of() { cat "$@" | sha1sum | cut -c1-12; }   # what a stamp records about the tools and data a step reads

# ---- 1. the SPU job modules ----------------------------------------------------------------------------------------------
if [ $FORCE = 1 ] || [ ! -f "$JM/jobmod_7868e9b847d1_e6D98.bin" ]; then
  step "carve the SPU job modules" "$LOGS/prepare-jobmods.log" "$PY" -I "$TOOLS/jobmods_carve.py" "$PRISTINE" "$TOOLS/jobmods_extra_funcs.json" "$JM" trailer+0 --write
  [ $DRY = 1 ] || [ -f "$JM/jobmod_7868e9b847d1_e6D98.bin" ] || { echo "the job modules do not match the known 1.00 disc EBOOT (is it BCES01741 1.00, decrypted?)"; exit 1; }
fi

# ---- 2. the game patches as run-time switches ------------------------------------------------------------------------------
PATCHES=("$ASC/patches-rpcs3/ascension-1.00.yml"); [ -f "$PATCHDIR/patch.yml" ] && PATCHES=("$PATCHDIR/patch.yml" "${PATCHES[@]}")
step "patch table" "$LOGS/prepare-patches.log" "$PY" -I "$TOOLS/rpcs3_patch.py" runtime --elf "$PRISTINE" --table "$TABLE" --sites "$SITES" --out "$ALLON" --patches "${PATCHES[@]}"

# ---- 3. the PPU code -> src/recomp, then the post-lift hooks -----------------------------------------------------------------
# Built in a staging directory and moved into place only after the lifter, all six hooks and the output check succeed (lift_stage.sh).
# Complete = stamp of the current inputs + every file matching the manifest; ppu_recomp.h existing proves nothing (it is written first).
hooks() {  # hooks <dir>: 4 of these rewrite the chunks in place, one more reason never to run them on the final tree
  for t in vram_guard_patch zprepass_patch texstream_patch gamelog_patch; do "$PY" "$TOOLS/$t.py" "$1" || return 1; done
  "$PY" "$TOOLS/vram_plan_patch.py" apply "$1" || return 1
  "$PY" -I "$TOOLS/mksrc.py" "$1"
}
[ $DRY = 1 ] || stage_recover "$RECOMP"
if [ -f "$SITES" ]; then
  PPU_STAMP="runtime:$MD5:$(sha1sum "$SITES" | cut -d' ' -f1):tools:$(sum_of "$KIT/ppu_lifter.py" "$KIT/ppu_disasm.py" "$KIT/elf_parser.py" \
             "$TOOLS"/{vram_guard_patch,zprepass_patch,texstream_patch,gamelog_patch,vram_plan_patch,mksrc}.py "$META/functions.json" "$META/EBOOT.imports.json")"
else PPU_STAMP="(not known before step 2 runs)"; fi
if [ $FORCE = 0 ] && why="$(stage_check "$RECOMP" "$PPU_STAMP")"; then
  echo "== translate the PPU code: complete for these inputs ($(tr '\n' ' ' < "$RECOMP/.lift-info")), skipped"
else
  echo "== PPU code: $([ $FORCE = 1 ] && echo "--force" || echo "$why") -> translate"
  if [ $DRY = 1 ]; then STG="$(stage_path "$RECOMP")"; else STG="$(stage_begin "$RECOMP")"; fi
  step "translate the PPU code" "$LOGS/prepare-ppu.log" "$PY" "$KIT/ppu_lifter.py" "$PRISTINE" --functions "$META/functions.json" \
       --hle-stubs "$META/EBOOT.imports.json" --toc 0x81EF60 --code-end 0x62C4CC --chunk-lines 150000 -j "$JOBS" --patch-sites "$SITES" -o "$STG"
  step "post-lift hooks" "$LOGS/prepare-hooks.log" hooks "$STG"
  step "check the PPU output" "$LOGS/prepare-ppu-check.log" check_ppu "$STG" "$LOGS/prepare-ppu.log"
  if [ $DRY = 0 ]; then stage_seal "$STG" "$PPU_STAMP"; stage_promote "$STG" "$RECOMP"; echo "   $(cat "$LOGS/prepare-ppu-check.log")"; fi
fi

# ---- 4. the SPU programs embedded in the EBOOT -> meta/spu ---------------------------------------------------------------------
if [ $DRY = 0 ]; then TMPON="$(mktemp -d)"; TMPORIG="$(mktemp -d)"; trap 'rm -rf "$TMPON" "$TMPORIG"' EXIT; else TMPON="<tmp>"; TMPORIG="<tmp>"; fi
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
# Three tools write into one directory; the old gate (spu_workloads.c exists) could not tell a run cut short. Staged like step 3.
[ $DRY = 1 ] || stage_recover "$SPU"
if [ $DRY = 0 ]; then
  SPU_STAMP="spu:$MD5:inputs:$(cd "$META/spu" && find . -name '*.elf' -printf '%P\n' | LC_ALL=C sort | xargs sha1sum | sha1sum | cut -c1-12)$(cd "$JM" && find . -type f -printf '%P\n' | LC_ALL=C sort | xargs sha1sum | sha1sum | cut -c1-12):tools:$(sum_of \
             "$KIT"/{lift_jobmods,lift_spu_pic,build_spu_workloads,wrap_spu_elf,spu_program_pipeline,spu_lifter,spu_disasm,spu_elf}.py "$TOOLS/jobmods_extra_funcs.json" "$TOOLS/job_fragments.json")"
else SPU_STAMP="(not known before step 4 runs)"; fi
if [ $FORCE = 0 ] && why="$(stage_check "$SPU" "$SPU_STAMP")"; then
  echo "== translate the SPU code: complete for these inputs, skipped"
else
  echo "== SPU code: $([ $FORCE = 1 ] && echo "--force" || echo "$why") -> translate"
  if [ $DRY = 1 ]; then STG="$(stage_path "$SPU")"; else STG="$(stage_begin "$SPU")"; fi
  export SPU_LIFT_XREGS=1
  LIFT_JOBMODS_MERGE=1 step "translate the SPU job modules" "$LOGS/prepare-spu-jobmods.log" "$PY" "$KIT/lift_jobmods.py" --jobmods "$JM" --out "$STG" \
       --register "$STG/jobmods_register.c" --symbol-prefix ascension --extra-funcs-json "$TOOLS/jobmods_extra_funcs.json"
  step "translate the SPU code fragments" "$LOGS/prepare-spu-fragments.log" "$PY" "$KIT/lift_spu_pic.py" --elf "$PRISTINE" --fragments "$TOOLS/job_fragments.json" \
       --out "$STG" --register "$STG/pic_register.c" --symbol-prefix ascension
  step "translate the SPU programs" "$LOGS/prepare-spu-images.log" "$PY" "$KIT/build_spu_workloads.py" --images "$META/spu" --lifted "$STG" \
       --out "$STG/spu_workloads.c" --register-fn ascension_spu_register_all --constructor --title ascension --relift \
       --extra-funcs "spu_wws_pm_at_0077CA80=0xA2C,0xA68,0x3368,0x3378"
  step "check the SPU output" "$LOGS/prepare-spu-check.log" check_spu "$STG"
  if [ $DRY = 0 ]; then stage_seal "$STG" "$SPU_STAMP"; stage_promote "$STG" "$SPU"; echo "   $(cat "$LOGS/prepare-spu-check.log")"; fi
fi
echo "prepared. Next: android/lift/build_lifted.sh"
