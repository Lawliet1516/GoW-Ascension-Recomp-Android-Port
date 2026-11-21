# render/: the Android renderer tree

Layout mirrors ps3kit so upstream files compile **unmodified** (their relative includes `../../runtime/...` and `../../include/ps3emu/...` resolve):
`render/libs/video/` (the migrated video layer), `render/include/` (upstream's newer headers; searched before `ps3kit/include`),
`render/runtime -> ../ps3kit/runtime` (symlink: the runtime is the fork's, with the Ascension fixes).

`render/libs/video/` starts as a copy of `ps3kit/libs/video/` (this fork's state, commit recorded in the first commit of this directory) **without the
Windows/Apple-only pieces** (`rsx_live_draw.*`, the D3D12 and Metal backends, FSR3/SMAA, ffx). It is then migrated toward the pinned upstream
(ps3recomp v0.20.0, see `third_party/ps3recomp-upstream/UPSTREAM.md`) in separate commits, one concern each, each naming the upstream blobs it takes.

`ps3kit/libs/video/` is **not touched**: the Windows production renderer keeps building byte for byte as before. The Android build
(`GOWA_RENDER_UPSTREAM=ON`) compiles `render/libs/video` instead of `ps3kit/libs/video`. When the migration is validated, convergence of the two copies is a separate,
reviewable step.

Layering (the separation asked for): `rsx_draw_engine.c` is the rendering logic (RSX state -> draws, targets, textures, pipelines keys) and talks to the GPU only through
the `rsx_draw_backend` vtable (`rsx_draw_engine.h`); `rsx_vulkan_backend.c` is the Vulkan implementation of that vtable. Vendor-specific behaviour (Adreno, Mali)
belongs in the backend behind capability flags, never in the engine. Ascension-specific behaviour is re-applied in the engine as separately switchable policies (see
`android/render/MERGE_LOG.md`), not inside the backend.
