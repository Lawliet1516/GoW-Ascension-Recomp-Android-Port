#!/usr/bin/env bash
# R1: the 30 upstream scene runs inside the debuggable APK (com.gowa.scenes) with the Khronos validation layer ON.
#   r1_scenes_apk.sh [outdir]      env: ANDROID_SERIAL
# Same scene list and pass criteria as r0_scenes.sh, plus: the layer's messages are read from logcat (tag VALIDATION), because in an app
# process they do not go to stdout. A run passes if exit 0, no "MISMATCH", no VALIDATION "Error"/"Warning" lines. debug.vulkan.layers is global:
# it is reset on exit (trap), always.
set -uo pipefail
OUT="${1:-/tmp/r1}"; mkdir -p "$OUT"; PKG=com.gowa.scenes
trap 'adb shell "setprop debug.vulkan.layers \"\""' EXIT
adb shell "setprop debug.vulkan.layers VK_LAYER_KHRONOS_validation"
echo "| pass | scene | exit | engine path | VALIDATION errors | VALIDATION warnings | MISMATCH |"; echo "|---|---|---|---|---|---|---|"
for pass in fixed guest; do
  if [ $pass = fixed ]; then SCENES=("" "--draw" "--draw --frames=60" "--depth" "--tex" "--tex-io" "--quads" "--threads" "--split-present"); ENV="X=1"
  else SCENES=("" "--draw" "--depth" "--quads" "--shader" "--tex" "--tex-io" "--mip" "--rtt" "--depthtex" "--mrt" "--mrt-a" "--cube" "--depthbounds" "--split-present" "--feedback" "--clip" "--clip-keep" "--float-rt" "--float-rt-r" "--unbound"); ENV="PS3RECOMP_VK_GUEST_PROGRAMS=1"; fi
  for s in "${SCENES[@]}"; do
    name="${s:-default}"; tag="$pass$(echo "$name" | tr ' =-' '___')"
    adb logcat -c; adb shell run-as $PKG rm -f files/$tag.rc files/$tag.log; adb shell am force-stop $PKG
    adb shell am start -n $PKG/.MainActivity --es args "a:${s// /,}" --es env "$ENV" --es tag "$tag" >/dev/null
    rc=""; for i in $(seq 150); do rc=$(adb shell run-as $PKG cat files/$tag.rc 2>/dev/null); [ -n "$rc" ] && break; sleep 1; done
    adb shell run-as $PKG cat files/$tag.log > "$OUT/$tag.log" 2>&1; adb logcat -d -s VALIDATION:I > "$OUT/$tag.vvl.txt"
    eng=$(grep -q "register-file draw engine path" "$OUT/$tag.log" && echo yes || echo no)
    ve=$(grep -c "Validation Error" "$OUT/$tag.vvl.txt"); vw=$(grep -c "Validation Warning" "$OUT/$tag.vvl.txt"); mm=$(grep -c MISMATCH "$OUT/$tag.log")
    printf "| %s | %s | %s | %s | %s | %s | %s |\n" "$pass" "$name" "${rc:-timeout}" "$eng" "$ve" "$vw" "$mm"
  done
done
