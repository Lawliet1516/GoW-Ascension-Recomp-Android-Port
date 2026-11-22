#!/usr/bin/env bash
# T6: clang-analyzer (NDK r29 clang-tidy) over every TU of the Android runtime build. Resumable: a TU whose .out exists is skipped.
S=/root/work/colab-station; B=$S/runs/T3b-runtime-android/build; O=$S/runs/T6-static-analysis; mkdir -p $O/tu
export TIDY=$S/toolchain/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin/clang-tidy B O
export CHECKS='-*,clang-analyzer-core.*,clang-analyzer-unix.*,clang-analyzer-cplusplus.*,clang-analyzer-optin.portability.*,bugprone-sizeof-expression'
python3 -c "import json;[print(e['file']) for e in json.load(open('$B/compile_commands.json'))]" | sort -u | \
xargs -P 2 -I{} bash -c 'f="{}"; out="$O/tu/$(echo "$f" | md5sum | cut -c1-12).out"; [ -s "$out.done" ] && exit 0
  /usr/bin/time -f "TIME %e s MAXRSS %M KB" -o "$out.time" "$TIDY" -p "$B" --checks="$CHECKS" "$f" > "$out" 2>&1; echo "$f" > "$out.done"'
echo "TUs analysed: $(ls $O/tu/*.done | wc -l)"
