#!/usr/bin/env bash
# dryrun_state_test.sh <repo-root> : runs android/lift/prepare.sh --dry-run with a placeholder file at the EBOOT path and diffs the state
R="$1"; W="$(mktemp -d)"; mkdir -p "$W/pristine"; echo "placeholder: NOT an EBOOT (only makes the existence check true; --dry-run never parses it)" > "$W/pristine/EBOOT.ELF"
snap() { (cd "$R" && find . -path ./.git -prune -o -printf '%p %T@ %s\n' | LC_ALL=C sort); (cd "$W" && find . -printf 'W:%p %T@ %s\n' | LC_ALL=C sort); find /tmp -maxdepth 1 -newer "$W/pristine/EBOOT.ELF" -printf 'TMP:%p\n' 2>/dev/null | grep -v claude | LC_ALL=C sort; }
sleep 1; before="$(snap)"
(cd "$R" && PYTHON=/root/work/colab-station/toolchain/venv/bin/python WORK_DIR="$W" bash android/lift/prepare.sh --dry-run) > "$W.out" 2>&1; rc=$?
after="$(snap)"
echo "prepare.sh --dry-run exit=$rc; state changes:"; diff <(echo "$before") <(echo "$after") | grep '^[<>]' | sed 's/ [0-9.]* [0-9]*$//' || echo "   (none)"
echo "--- output:"; cat "$W.out"; git -C "$R" status --porcelain --untracked-files=all | sed 's/^/git: /'
rm -rf "$W" "$W.out"
