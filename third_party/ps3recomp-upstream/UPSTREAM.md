# Upstream provenance: ps3recomp

The renderer migration (branch `android/renderer-upstream`) takes the portable draw engine and the Vulkan backend from **ps3recomp**
(https://github.com/sp00nznet/ps3recomp, MIT). Nothing is ever pushed to that repository.

| role | ref in this repo | upstream tag | commit |
|---|---|---|---|
| **pinned target** | `refs/upstream/ps3recomp/v0.20.0` | v0.20.0 (annotated tag object 9811524d) | `d81c393ee72db80ef89aa4464b23c466edda44b0` (2026-11-21) |
| base the fork was cut from | `refs/upstream/ps3recomp/v0.15.0` | v0.15.0 | `0fe7dbf5db9a467aca268865fb26170ea113c541` |

How the base was identified: `rsx_draw_engine.h`, `rsx_texture_layout.c` and `rsx_shader_spirv.cpp` in `ps3kit/libs/video` are byte-identical (same git blob) to
v0.15.0; the other files differ from it by the Ascension changes. (`android/render/migrate.py` redoes the classification.)

Reproduce / compare (no network after the fetch):

```sh
android/render/fetch_upstream.sh                      # fetches both tags read-only, verifies the pinned commit
python3 android/render/migrate.py                     # writes android/render/MIGRATION_ANALYSIS.md
git diff refs/upstream/ps3recomp/v0.20.0 -- libs/video/rsx_vulkan_backend.c   # (paths differ: use `git show <ref>:<path>` against ps3kit/libs/video/...)
```

Every file taken from upstream is committed in its own commit whose message names the upstream commit and the blob ids, so `git log` answers "where did this line come from"
and any step can be reverted alone. Files the fork modified (the Ascension code) are never overwritten wholesale: they are merged hunk by hunk, with the decision recorded in
`android/render/MERGE_LOG.md`.
