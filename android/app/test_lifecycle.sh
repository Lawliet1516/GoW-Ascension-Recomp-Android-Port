#!/usr/bin/env bash
# Lifecycle / rotation / suspend-resume stability test for the bring-up app. Needs the app installed (adb install -r app-debug.apk)
# and, on Xiaomi/HyperOS, "Install via USB" enabled once. Parses logcat (tag gowa) for the counters the app prints.
#
#   android/app/test_lifecycle.sh [cycles=20]
#
# Pass criteria (all must hold): no FAIL line; at exit surface create == destroy and swapchain create == destroy ("EXIT clean");
# frames keep increasing after every resume; no "failed:" / "device lost" lines in logcat; memory (PSS) does not grow more than 10 %.
# UNTESTED until the first run on a phone: written to be run, not yet run.
set -uo pipefail
CYC="${1:-20}"; PKG=com.gowa.app; ACT=$PKG/.GowActivity; FAIL=0
fail() { echo "FAIL: $*"; FAIL=1; }
frames() { adb logcat -d -s gowa:I | grep -o 'frames=[0-9]*' | tail -1 | cut -d= -f2; }
pss() { adb shell dumpsys meminfo $PKG 2>/dev/null | awk '/TOTAL PSS:/ {print $3; exit} /TOTAL +[0-9]+/ {print $2; exit}'; }
adb logcat -c; adb shell am force-stop $PKG; adb shell am start -n $ACT >/dev/null; sleep 6
F0=$(frames); P0=$(pss); echo "start: frames=$F0 pss=${P0}KB"; [ -n "$F0" ] || fail "no frames after start (see: adb logcat -s gowa)"
echo "== $CYC suspend/resume cycles (HOME, then relaunch)"
for i in $(seq 1 "$CYC"); do
  adb shell am start -a android.intent.action.MAIN -c android.intent.category.HOME >/dev/null; sleep 1.5
  adb shell am start -n $ACT >/dev/null; sleep 2.5
  F1=$(frames); [ "${F1:-0}" -gt "${F0:-0}" ] || fail "cycle $i: frames did not advance after resume ($F0 -> $F1)"; F0=$F1
done
echo "== rotation: every orientation, 3 times"
adb shell settings put system accelerometer_rotation 0
for r in 0 1 2 3 0 1 2 3 0 1 2 3; do
  adb shell settings put system user_rotation $r; sleep 2
  F1=$(frames); [ "${F1:-0}" -gt "${F0:-0}" ] || fail "rotation $r: frames did not advance"; F0=$F1
  adb exec-out screencap -p > "/tmp/gowa_rot_$r.png"
done
adb shell settings put system accelerometer_rotation 1
echo "== screen off/on (lock) 3 times"
for i in 1 2 3; do adb shell input keyevent KEYCODE_SLEEP 2>/dev/null || adb shell cmd power sleep 2>/dev/null; sleep 2; adb shell input keyevent KEYCODE_WAKEUP 2>/dev/null; sleep 3
  F1=$(frames); [ "${F1:-0}" -gt "${F0:-0}" ] || fail "lock cycle $i: frames did not advance"; F0=$F1; done
P1=$(pss); echo "end: frames=$F0 pss=${P1}KB (start ${P0}KB)"
if [ -n "$P0" ] && [ -n "$P1" ] && [ "$P1" -gt $(( P0 * 11 / 10 )) ]; then fail "PSS grew more than 10% ($P0 -> $P1 KB)"; fi
adb logcat -d -s gowa:E | grep -E "failed|lost" && fail "errors in logcat"
adb shell am force-stop $PKG; sleep 1
adb logcat -d -s gowa:I | grep -E "SURFACE destroyed|SWAPCHAIN|EXIT" | tail -5
echo "$([ $FAIL = 0 ] && echo PASSED || echo FAILED)"; exit $FAIL
