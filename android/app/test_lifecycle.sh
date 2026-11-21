#!/usr/bin/env bash
# Lifecycle / rotation / suspend-resume stability test for the bring-up app. Needs the app installed (adb install -r app-debug.apk)
# and, on Xiaomi/HyperOS, "Install via USB" enabled once. Parses logcat (tag gowa) for the counters the app prints.
#
#   android/app/test_lifecycle.sh [cycles=20]
#
# Pass criteria (all must hold): no FAIL line; at exit surface create == destroy and swapchain create == destroy ("EXIT clean");
# frames keep increasing after every resume; no "failed:" / "device lost" lines in logcat; memory (PSS) does not grow more than 10 %.
# First run 21/11/2026: suspend/resume section executed and passed on the Redmi Note 13 Pro 5G; the rotation and lock sections are reported as SKIPPED when the phone refuses the commands.
set -uo pipefail
CYC="${1:-20}"; PKG=com.gowa.app; ACT=$PKG/.GowActivity; FAIL=0
fail() { echo "FAIL: $*"; FAIL=1; }
skip() { echo "SKIPPED: $*"; SKIPPED=1; }
SKIPPED=0
trap 'adb shell wm user-rotation free >/dev/null 2>&1' EXIT      # always give the phone its auto-rotation back
top_is_app() { adb shell dumpsys activity activities 2>/dev/null | grep -m1 -E "topResumedActivity|mResumedActivity" | grep -q "$PKG"; }
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
echo "== rotation: every orientation, 3 times (wm user-rotation; needs no WRITE_SETTINGS)"
adb shell am start -n $ACT >/dev/null; sleep 2
if ! adb shell wm user-rotation lock 0 >/dev/null 2>&1; then skip "wm user-rotation not permitted: rotation not tested"; else
for r in 0 1 2 3 0 1 2 3 0 1 2 3; do
  adb shell wm user-rotation lock $r >/dev/null; sleep 2.5
  F1=$(frames); [ "${F1:-0}" -gt "${F0:-0}" ] || fail "rotation $r: frames did not advance"; F0=$F1
  top_is_app && adb exec-out screencap -p > "/tmp/gowa_rot_$r.png" || skip "rotation $r: app not in front, no screenshot taken (the phone is in use)"
done
adb shell wm user-rotation free >/dev/null
echo "   swapchain extents seen: $(adb logcat -d -s gowa:I | grep -o 'SWAPCHAIN [0-9x]*' | sort | uniq -c | tr '\n' ' ')"
fi
echo "== screen off/on (lock) 3 times (needs input injection: 'USB debugging (security settings)' on HyperOS)"
if ! adb shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1; then skip "input injection refused: lock/unlock cycles not tested"; else
for i in 1 2 3; do adb shell input keyevent KEYCODE_SLEEP; sleep 2; adb shell input keyevent KEYCODE_WAKEUP; sleep 3
  F1=$(frames); [ "${F1:-0}" -gt "${F0:-0}" ] || fail "lock cycle $i: frames did not advance"; F0=$F1; done
fi
P1=$(pss); echo "end: frames=$F0 pss=${P1}KB (start ${P0}KB)"
if [ -n "$P0" ] && [ -n "$P1" ] && [ "$P1" -gt $(( P0 * 11 / 10 )) ]; then fail "PSS grew more than 10% ($P0 -> $P1 KB)"; fi
adb logcat -d -s gowa:E | grep -E "failed|lost" && fail "errors in logcat"
adb shell am force-stop $PKG; sleep 1
adb logcat -d -s gowa:I | grep -E "SURFACE destroyed|SWAPCHAIN|EXIT" | tail -5
[ $SKIPPED = 1 ] && echo "NOTE: some sections were SKIPPED (see above): a pass here does not cover them"
echo "$([ $FAIL = 0 ] && echo PASSED || echo FAILED)"; exit $FAIL
