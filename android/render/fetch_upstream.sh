#!/usr/bin/env bash
# Fetches the pinned upstream releases (read-only) into this repository's object store, so every comparison and every
# "which upstream line did this come from" question can be answered with plain git. The remote can never be pushed to.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
git remote get-url ps3recomp-upstream >/dev/null 2>&1 || git remote add ps3recomp-upstream https://github.com/sp00nznet/ps3recomp.git
git remote set-url --push ps3recomp-upstream DISABLED-read-only-upstream
# BASE: the release this fork was cut from (draw engine header, texture layout and shader_spirv are byte-identical to it); THEIRS: the migration target.
git fetch -q --depth=1 ps3recomp-upstream refs/tags/v0.15.0:refs/upstream/ps3recomp/v0.15.0
git fetch -q --depth=1 ps3recomp-upstream refs/tags/v0.20.0:refs/upstream/ps3recomp/v0.20.0
test "$(git rev-parse 'refs/upstream/ps3recomp/v0.20.0^{commit}')" = d81c393ee72db80ef89aa4464b23c466edda44b0 || { echo "v0.20.0 does not resolve to the pinned commit"; exit 1; }
git for-each-ref refs/upstream
