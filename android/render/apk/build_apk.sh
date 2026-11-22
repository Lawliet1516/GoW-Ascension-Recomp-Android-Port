#!/usr/bin/env bash
# Builds the debuggable scene-runner APK (no Gradle): android/render/apk/build_apk.sh <libgowa_scenes.so> <out.apk> [layer.so]   (env EXTRA_SO="a.so b.so")
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; SO="$1"; OUT="$2"; LAYER="${3:-}"
SDK="$HOME/Android/Sdk"; BT="$(ls -d $SDK/build-tools/* | tail -1)"; PLAT="$(ls -d $SDK/platforms/android-* | tail -1)/android.jar"
export JAVA_HOME="${JAVA_HOME:-$HOME/Android/jdk}"; J="$JAVA_HOME/bin"
W="$(mktemp -d)"; mkdir -p "$W/classes" "$W/apk/lib/arm64-v8a"
"$J/javac" -source 8 -target 8 -bootclasspath "$PLAT" -d "$W/classes" "$HERE"/java/com/gowa/scenes/*.java 2>&1 | grep -v warning || true
"$BT/d8" --lib "$PLAT" --output "$W/apk" "$W/classes"/com/gowa/scenes/*.class
"$BT/aapt2" link -o "$W/base.apk" -I "$PLAT" --manifest "$HERE/AndroidManifest.xml" --min-sdk-version 29 --target-sdk-version 29
cp "$SO" "$W/apk/lib/arm64-v8a/libgowa_scenes.so"; [ -n "$LAYER" ] && cp "$LAYER" "$W/apk/lib/arm64-v8a/libVkLayer_khronos_validation.so"
for x in ${EXTRA_SO:-}; do cp "$x" "$W/apk/lib/arm64-v8a/"; done   # e.g. EXTRA_SO=libclip_repro.so
(cd "$W/apk" && zip -qr ../base.apk classes.dex lib)
"$BT/zipalign" -f -p 4 "$W/base.apk" "$W/aligned.apk"
KS="$HOME/.android/debug.keystore"; [ -f "$KS" ] || { mkdir -p "$HOME/.android"; "$J/keytool" -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000 >/dev/null; }
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$OUT" "$W/aligned.apk"
echo "APK: $OUT"; rm -rf "$W"
