#!/usr/bin/env bash
# R0: upstream's scene suite (the same scenes and pass criteria as .github/workflows/linux-vulkan.yml) on the phone.
#   android/render/r0_scenes.sh <path-to-ps3recomp_host> [outdir]
# Pass for a guest-program scene = exit 0, no "VUID", the run says "register-file draw engine path", no "nothing to check".
# Validation layers: only when the layer is installed in the process (see android/render/README.md); without them "VUID" can never appear,
# and the table says so ("layers: off").
set -uo pipefail
BIN="${1:?ps3recomp_host binary}"; OUT="${2:-/tmp/r0}"; mkdir -p "$OUT"
adb push "$BIN" /data/local/tmp/ps3recomp_host >/dev/null
LAYERS="${LAYERS:-off}"
run() { adb shell "cd /data/local/tmp && $1 timeout 120 ./ps3recomp_host $2 2>&1; echo EXIT=\$?"; }
echo "| pass | scene | exit | engine path | VUID | nothing-to-check | layers |"; echo "|---|---|---|---|---|---|---|"
for pass in fixed guest; do
  if [ $pass = fixed ]; then SCENES=("" "--draw" "--draw --frames=60" "--depth" "--tex" "--tex-io" "--quads" "--threads" "--split-present"); ENV=""
  else SCENES=("" "--draw" "--depth" "--quads" "--shader" "--tex" "--tex-io" "--mip" "--rtt" "--depthtex" "--mrt" "--mrt-a" "--cube" "--depthbounds" "--split-present" "--feedback" "--clip" "--clip-keep" "--float-rt" "--float-rt-r" "--unbound"); ENV="PS3RECOMP_VK_GUEST_PROGRAMS=1"; fi
  for s in "${SCENES[@]}"; do
    name="${s:-default}"; f="$OUT/$pass-$(echo "$name" | tr ' =-' '___').log"
    run "$ENV" "$s" > "$f"
    rc=$(grep -o 'EXIT=[0-9]*' "$f" | tail -1 | cut -d= -f2)
    eng=$(grep -q "register-file draw engine path" "$f" && echo yes || echo no)
    vu=$(grep -c "VUID" "$f"); skip=$(grep -c "nothing to check" "$f")
    printf "| %s | %s | %s | %s | %s | %s | %s |\n" "$pass" "$name" "$rc" "$eng" "$vu" "$skip" "$LAYERS"
  done
done
