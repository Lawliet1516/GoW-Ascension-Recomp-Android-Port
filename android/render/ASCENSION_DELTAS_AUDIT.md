# Audit of the Ascension deltas (Phase 2, audit only: nothing reapplied)

Branch `android/renderer-upstream`. Written 22/11/2026. Complements `MIGRATION_ANALYSIS.md` and `MERGE_LOG.md` (kept as they are; where this report disagrees with them it says so in section 9).
Measurements are reproducible: `python3 android/render/audit_deltas.py` (read-only, writes `results/deltas_audit/`).

**What is evidence and what is not.** Every classification below cites its basis: **[code]** I read the code (fork side, upstream v0.20.0 side, or both); **[measured]** a script count;
**[fork comment]** the fork's own comment or `ascension/host/ascension_defaults.cpp` says it (a claim by the authors, not re-tested here); **[hypothesis]** reasoning, to be verified.
**Nothing here was run against the game, and no patch was applied or built.** The upstream unit tests were not built or run in this audit.

## 1. Inventory of the 12 patches

`ascension-deltas/<file>.patch` = fork change relative to ps3recomp **v0.15.0** (the base the fork was cut from), per file. Counted from the patch files: **5,645 changed lines** (+5,398 / -247), 12 files.

| patch | changed lines | hunks | what it is (from the code) |
|---|---|---|---|
| `cellGcmSys.c` | 2,279 | 73 | the FIFO walker and libgcm layer: sync points, park/report holds, flip pacing, IO mapping, blits, diagnostics |
| `rsx_fp_decompiler.c` | 1,090 | 29 | fragment-program -> HLSL: correctness fixes, WPOS handling, statistics |
| `rsx_vertex_compact.c` | 1,223 | 9 | vertex fetch: x86 SIMD (SSE4.1/AVX2+FMA), single-pass plan, shared per-attribute fetch |
| `rsx_dispatch.c` | 446 | 8 | register store: dispatch table, transform-constant windows, PSO-epoch / constant-generation counters |
| `rsx_vp_decompiler.c` | 284 | 13 | vertex-program -> HLSL: TEX8/TEX9, condition-register fix, raw-input path, statistics |
| `rsx_commands.c` | 133 | 8 | the old `rsx_state` vtable path (not the production draw path): transform program/constant methods |
| `rsx_vertex_compact.h` | 69 | 4 | declarations for the above |
| `rsx_fp_decompiler.h` | 58 | 3 | declarations for the above |
| `rsx_dispatch.h` | 41 | 1 | declarations for the above |
| `rsx_vp_decompiler.h` | 14 | 1 | declarations for the above |
| `rsx_vulkan_backend.c` | 6 | 2 | an SDL3 shim and the point-size macros |
| `rsx_draw_engine.c` | 2 | 2 | `t8`/`t9` varyings in the fixed vertex shader |

Origin: the fork's production copy `ps3kit/libs/video/` (untouched by the migration, so the Windows build is unchanged). Upstream equivalents: `render/libs/video/` (pure v0.20.0).
Per-hunk and per-function numbers: `results/deltas_audit/overlap_by_hunk.tsv`, `by_function.tsv`, `overlap_summary.txt`.

**Verbatim overlap [measured].** Of 3,972 significant added lines (>= 20 characters) only **246 (6 %)** appear line for line in upstream v0.20.0; per file: `rsx_draw_engine.c` 100 %, `rsx_commands.c` 50 %, `rsx_dispatch.c` 10 %,
`rsx_fp_decompiler.c` 8 %, `rsx_vertex_compact.c` 8 %, `rsx_vp_decompiler.c` 6 %, `cellGcmSys.c` 2 %. This is a lower bound for "already incorporated" (upstream may implement the same behaviour with other code),
so every group below was decided by reading the code, not by this number.

## 2. Classification

Classes (as requested): **1** already incorporated by upstream; **2** needed and compatible; **3** needed, needs adaptation; **4** obsolete or replaced; **5** incompatible with the current architecture; **6** must be rewritten; **7** uncertain.
A patch file mixes features, so the unit of classification is the **feature group**; the per-patch row summarises it. Group ids are used in the later sections.

### 2.1 Per patch (summary)

| patch | summary |
|---|---|
| `rsx_draw_engine.c` | **1**: the two lines are already in upstream (100 % verbatim) |
| `rsx_vulkan_backend.c` | **4**: SDL3 shim / macros; this tree uses upstream's SDL2 path |
| `rsx_commands.c` | **1** (functional) + a method fast path (**3**, low priority). Old vtable path, not production [code, `RENDERER_AUDIT.md` s.1] |
| `rsx_dispatch.c/.h` | mixed: windows **1**; dispatch table **2** (performance); PSO-epoch / constant-generation counters **5** (their consumer is `rsx_live_draw.c`, D3D12 only) |
| `rsx_vp_decompiler.c/.h` | mixed: TEX8/TEX9 **1**; `LD_VP_CC_EITHER` **2**; raw-input path and statistics **4** |
| `rsx_fp_decompiler.c/.h` | mixed: about half the correctness fixes **1** (upstream implemented them); the rest **2/3**; consumers tied to the D3D12 live draw **5**; statistics **4**; WPOS **7** |
| `rsx_vertex_compact.c/.h` | mixed: x86 SIMD **5** (does not compile on ARM64, falls back to scalar: correct, slower); plan/fetch restructuring **1/7**; NEON would be **6**, only if profiling asks |
| `cellGcmSys.c` | mixed, the large one; see 2.2 |

### 2.2 `cellGcmSys.c`: feature groups (the walker)

| id | feature | class | evidence |
|---|---|---|---|
| W1 | IO space allocator: first fit over free IO pages, taken back on unmap, `GCM_IO_BUMP=1` for the old one; IO lock (`__atomic`, portable) | **2** | upstream still has the bump allocator `gcm_io_alloc(u32 size)` that never takes memory back [code]. Fork comment: Ascension maps and unmaps ~70 MB at every area load; offsets run past the 256 MB aperture, its unmaps fail ("UnmapMainMemory (0x00700000) failed"), `iooffset == -1` ~80,000 times, garbled scenery until restart (2026-11-03) [fork comment] |
| W2 | offset-table write-through once the title has asked for the table (`offset_table_write_through`) | **2** | upstream refreshes the table copy only on `cellGcmGetOffsetTable` [code]; fork comment: a title that took the pointer at boot read 0xFFFF for later pages [fork comment] |
| W3 | `g_gcm_io_remap_gen`: counter bumped when IO pages are mapped again | **3** | the producer is trivial; the consumer is the texture cache of `rsx_live_draw.c` (D3D12). The Vulkan backend's cache would have to read it [code]. Whether upstream's backend needs it is unknown [hypothesis] |
| W4 | EDGE6: extra fixed IO window (`gcm_map_extra`, `g_gcm_init_hook`) | **7** | callers live in `edge6.c`, which is **not in this repository, nor in any branch or in the history** [measured: `git ls-tree` on all branches]. `EDGE6=1` is a title default [fork comment] |
| W5 | vblank handler: one call per vblank (backlog capped at 8) instead of one per pump | **2** | upstream calls the handler once per pump [code]. Fork comment: the movie clock ran at ~75 % and the cinematic fell behind its audio |
| W6 | park grace (`GCM_PARK_WAIT_MS`), park-episode reset, no resync while parked / waiting on an ACQUIRE / holding a report, `GCM_SEMA_ACQUIRE` | **1** | upstream has all four: `GCM_PARK_GRACE_MS` (same role, other name), `park_off = 0` when the walker leaves the park (line ~2678), the `s_sema_blocked_pass/s_park_pass/s_report_hold_pass` exemption from the "stuck" resync, `GCM_SEMA_ACQUIRE` [code]. The title's values are environment defaults (`GCM_PARK_WAIT_MS=60000`, `GCM_SEMA_ACQUIRE=1`) |
| W7 | `GCM_PARK_SPU_HOLD_MS` (a patched park is held while SPURS tasks run); `GCM_PARK_HOLD_US` / `GCM_PARK_KICK` | **3** / **4** | SPU_HOLD is a **boot-critical title default** (`1000`): without it frame 2's reports landed while round 1 still ran and round 2 never began [fork comment, `ascension_defaults.cpp`]. Absent upstream [code]. It needs `spu_taskset_round_busy`, which this runtime does not define (only a warning shim in `android/host/render_runtime_shims.c`) [code]. HOLD_US / KICK are off by default and `KICK=1` froze 3 of 6 runs [fork comment]: do not port |
| W8 | report hold: upstream's `GCM_REPORT_SPU_HOLD_MS` base; the fork's refinements (hold only the report types a signal can hang on, `GCM_REPORT_SPU_HOLD_RING`, `GCM_ROUND_GATE`, `GCM_ROUND_RING`) | base **1**, refinements **3** | base exists upstream [code]. Refinements are title defaults (`GCM_REPORT_SPU_HOLD_MS=2000`, `GCM_ROUND_GATE=9BAC30`, `GCM_ROUND_RING=1101ED00,18`): "the game stopped at the end of loading every time" without them [fork comment]. They use SPURS task state and the title's addresses |
| W9 | `GCM_REPORT_VALIDATE`: a GET_REPORT with an invalid type or unaligned offset is not written | **2** | absent upstream [code]. Fork comment: a placeholder word read before it was patched sent a 16-byte report into the SPU job builder's staging buffer, and the next job list carried a timestamp as a DMA element (the in-game freeze). Small, portable |
| W10 | SPU PUT coupling: `gcm_note_spu_put`, `gcm_spu_block_for_put`, `cellGcm_fifo_kick`, park/job hooks (`cellGcm_park_job_end`, `cellGcm_park_kick_flush`, `GCM_PUT_ORDER`) | base **1**, hooks **7** | the base exists upstream with its own test `test_gcm_spu_put_kick.c` [code]. The fork's hooks are called from `n218_hle.c` / `spurs_pm_worker`, **absent from the repository** [measured] |
| W11a | flip into the FIFO with a title-managed context (marker only needs its own room) | **1** | upstream uses `gcm_ctx_room(ctx, 8)`, which has the same intent [code]; verify with `test_gcm_flip_wrap.c` |
| W11b | `cellGcmSetFlipCommandWithWaitLabel` queues SEMAPHORE_OFFSET/ACQUIRE then the flip, in the stream | **1** | upstream queues the same words [code]; room accounting differs, verify with `test_gcm_flip_stream.c` |
| W11c | `present_at_flip` (a host hook from `boot_main`) | **4** | replaced by upstream's present thread (`rsx_present_thread.h`, `test_rsx_present_thread.c`) [code]; the fork comment says ~25 % of frames never reached the screen with the ticker design, which is a Windows host detail |
| W11d | frame pacing: `PS3_FPS_LIMIT`, walker/direct/both modes, `GCM_PACE_PRECISE` (high-resolution waitable timer) | **3** (+**6** for the timer part) | Ascension steps its clock by 1/30 s per frame, so unpaced flips run the game fast; the title default sets `PS3_FPS_LIMIT=30` or 60 [fork comment]. Upstream has `PS3_FPS_LIMIT` only in `rsx_host_window.h` for window refresh [code]. The timer code is `#ifdef _WIN32`, the POSIX branch exists but would need Android clocks and a test |
| W12a | NV3089 blit guard against a local-memory extent past VRAM (`gcm_local_extent_bad`, `GCM_BLIT_GUARD`) | **2** | absent upstream [code]. Fork comment: VRAM compaction (`func_00374A70`) with a wrapped negative size blitted through 4 GB until "bctr to NULL". A separate PPU-side guard exists in `ascension/host/ascension_vram_guard.cpp` but its default is **off** (`PPU_VRAM_GUARD=0`) [code] |
| W12b | NV3089 unscaled-row copy fast path | **1** | upstream has it (the comment at `cellGcmSys.c` ~1515 describes the same per-pixel cost) [code] |
| W12c | `LD_BLIT_GUEST_SKIP` (skip the guest copy for a blit between live GPU surfaces) | **6** | title default `1`; depends on the renderer knowing which surfaces are live on the GPU, which is `rsx_live_draw.c` state [code]; it must be written in the portable engine |
| W13 | walker performance: `GCM_FIFO_BATCH`, `GCM_WALK_LEAN`, gate caching, `W2_FIFO` | **3**, low priority | pure performance; upstream rewrote parts of the same loop (+1,500 lines, 39 conflict hunks in the merge attempt) [`MERGE_LOG.md`]. Decide after the Phase 3 baseline (upstream has `bench_gcm_walk.c`) |
| W14 | `LD_VTX_ASYNC` joins in the walker, OCCL hooks (`gcm_occl_cas_value`, banks), ZPASS accumulation | **5** | all feed `rsx_live_draw.c` features (asynchronous vertex conversion, occlusion culling with the game's own answers; `OCCL=2` is a title default for performance) that the portable engine does not have [code]. Revisit when the engine has equivalents (**6**) |
| W15 | diagnostics: resync dump, branch trace, walk stats, MLAA diag, `OCCL_RDIAG`, `GCM_METHOD_HASH`, `PPU_DELAY_AB`, `VDEC_TIMELOG` | **7** | off by default, not needed for function; the resync dump and park log are worth having for the first boot (deliberate, small port) [hypothesis] |
| W16 | `SYS_USLEEP_REFWAIT` / `cellGcm_ref_wait` | **4** | the title default is `0` (off) and it needs a change in `ps3kit/runtime/syscalls/sys_timer.c` [code]: out of scope while ps3kit stays untouched |
| W17 | loading-screen turbo (`ps3_load_turbo_hz`, `LD_TURBO_LOAD`) | **4** | PC convenience; driven by `rsx_live_draw.c` |
| W18 | trace removal and small housekeeping (`ydkj_restore_handler_opd`, GetTiledPitchSize prints) | **4** | no behaviour |

### 2.3 Decompilers, dispatch, vertex, commands

| id | feature | class | evidence |
|---|---|---|---|
| F1 | TC8/TC9 inputs, facing input (`SV_IsFrontFace`), `perspective_corr` (SRC2 bit 31), precision-modifier clamps, `EXP_TEX` (OPDEST bit 21), DDX/DDY, volume units, shadow descriptors, sRGB output, structural hash | **1** | upstream implements each of them with its own code [code: `input_expr` 0xC/0xD/0xE, the `range` clamp in `emit_src`, line ~841 for SRC2 bit 31, `OP_DDX` at ~951, `rsx_fp_texture_exp_units`, `vol_mask`]. Equivalence with the fork's output is **not shown** (text differs); verify with `test_fp_decompiler.c` / `validate_fp_shaders.c` before relying on it |
| F2 | `LD_FP_SRC2ABS`, `LD_FP_NRM_SAFE`, `LD_FP_DP3_PRECISE`, `LD_FP_BRANCH_ANY`, `LD_FP_CC_PRED` | **2/3** | absent upstream (`dot()` and plain `normalize()` remain at `OP_DP3`/`OP_NRM`; no SRC2 abs) [code]. Each is gated, default on, with an RPCS3 reference and a Ascension symptom [fork comment]: DP3 precision changed the sky/fog colours, NRM of a zero vector gave NaN. API plumbing needs adapting to upstream's decompile entry point |
| F3 | `LD_FP_EXPORT_GATHER` (16-bit export registers read through the full register file) | **2/3** | absent upstream (no `packHalf2x16`) [code]; fork comment: 21 of the first level's 81 fragment programs, 12 of them a whole target (G-buffer second to fourth targets) |
| F4 | `LD_FP_ALIAS_SYNC` | **4** | default **off**: it made Kratos's hands and boots bright yellow and its packing is known wrong for some lane [fork comment]. Do not port |
| F5 | WPOS: `rsx_fp_uses_wpos`, `rsx_fp_apply_shader_window`, `LD_WPOS_SNAP` | **7** | upstream has its own `rsx_fp_apply_wpos_grid` [code]; the fork's handles render-scale (title default `PS3_RENDER_SCALE=200`) with sub-pixel snapping. Whether upstream's grid is equivalent is unknown |
| F6 | `rsx_fp_apply_output_rgb_scale` (blend source factor folded into the shader) | **5** | its only caller is `ld_blend_fold` in `rsx_live_draw.c` [fork comment] |
| F7 | `rsx_fp_collect_constants_used`, 8-byte hash, `rsx_fp_same_instructions`, `rsx_fp_alias_hazards`, `LD_SHADER_STATS` | **4** | performance memos and statistics for the live draw; upstream has its own structural hash |
| V1 | TEX8/TEX9 plumbing in the vertex program | **1** | upstream has `Out.t8=o[15]; Out.t9=o[6]` and a comment on TEX9 sharing the point-size register [code]; the 100 %-verbatim hunk is the declaration |
| V2 | `LD_VP_CC_EITHER` (condition-register update when either update bit is set) | **2** | absent upstream [code]; default on; fork comment: 3 of the 39 condition-only writes in Ascension's shader statistics use bit 14 alone, so later branches read a stale condition. Small |
| V3 | `rsx_vp_decompile_compact_raw_ex` (`LD_VTX_RAW`) | **4** | `LD_VTX_RAW` is off by default in the ledger |
| D1 | transform-constant / program upload windows | **1** | 60-80 % verbatim in upstream; `rsx_commands.c` equivalents 78 % [measured] |
| D2 | `RSX_DSP_TABLE`: one byte per method says what a register write needs to do | **2**, low priority | portable; the fork has `test_rsx_dispatch_table.c`; performance only (~150,000 words a frame in combat [fork comment]) |
| D3 | PSO-epoch / quiet-register / constant-generation counters (`LD_PSO_SAME`, `LD_DRAW_REUSE`, `LD_CB_GEN`) | **5** | consumed only by `rsx_live_draw.c`; no consumer in the portable engine [code]. A consumer would be a new design (**6**) |
| X1 | SSE4.1 / AVX2+FMA vertex conversion loops (`LD_VTX_SIMD`, `LD_VTX_FAST`) | **5** | x86 intrinsics under `__SSE4_1__`/`RSX_VTX_AVX2`; on aarch64 they compile out and the scalar loop runs [code]. A NEON version is **6** and only worth doing if a profile shows vertex conversion is a cost |
| X2 | single pass over the references for all attributes; shared `rsx_vertex_fetch_attr_one` | **7** | upstream has a similar structure (47-76 % verbatim on these hunks, and its own `rsx_vertex_fetch.c`) [measured]; equivalence unverified (`test_vertex_fetch_plan.c`) |
| X3 | inline-draw attributes the stream does not supply take their default instead of the last mesh's array | **7** | a correctness fix by its comment; upstream has `plan->inline_data` handling [code] but whether it has this rule was not established |
| X4 | element decode specialised for common formats | **3**, low | performance |

## 3. Dependencies and conflicts

Dependencies found [code unless noted]:
- **W7, W8** need the real `spu_taskset_round_busy` and SPURS task state. In this runtime the symbol is **only referenced, never defined** (`cellGcmSys.c`, upstream tests, and a shim that warns): a **runtime migration**, not a patch. `MERGE_LOG.md` already notes it.
- **W3, F6, D3, W12c, W14** depend on `rsx_live_draw.c` behaviours (texture cache, blend fold, PSO reuse, live surfaces, async vertices, occlusion) that exist only in the D3D12 path (30.7k lines; on other platforms its entry points are stubs). Each needs a design in the portable engine, not a copy.
- **W4, W10 (hooks)** need `edge6.c`, `n218_hle.c`, `spurs_pm_worker`: not in the repository.
- **W8 refinements** are tied to this title's memory addresses (`GCM_ROUND_*`), supplied as defaults by `ascension/host/ascension_defaults.cpp`; they are data, and wiring them needs the Android host to set the same environment.
- Walker features share state: the park/report/sema pass flags (`s_park_pass`, `s_sema_blocked_pass`, `s_report_hold_pass`) feed the single "stuck" resync decision that upstream already has; W7/W8 must set them the same way.

Conflicts with the current renderer:
- `cellGcmSys.c` and the decompilers were rewritten independently on both sides (39 / 16 conflict hunks in the earlier merge attempt, `MERGE_LOG.md`); a textual re-application will conflict, so each group is **re-written onto upstream's structure**, using the fork text as the specification.
- Nothing in the 12 patches touches the validated paths of the 30 scenes except through the decompilers (F2/F3/V2, which change generated HLSL for the programs that use those features) and the IO allocator (W1/W2, exercised by the `--tex-io` and IO-mapped scenes).

Platform notes: the fork's walker uses `SRWLOCK`, `QueryPerformanceCounter` and `LARGE_INTEGER`; `ps3kit/runtime/platform/win32_compat.h` provides POSIX versions (`pthread_rwlock`), and upstream's `cellGcmSys.c` compiles with them on Android. Windows-only code is limited to `CreateWaitableTimer` (already `#ifdef _WIN32`) and the `boot_main` hooks.

## 4. Ascension functionality that upstream v0.20.0 does not have

**Boot-critical according to the title's own defaults** (the game hangs or freezes without them, per the fork's comments): `GCM_PARK_SPU_HOLD_MS` (W7), the report-hold refinements and round gate/ring (W8), IO allocation reuse (W1, affects every area load), report validation (W9, an in-game freeze), vblank coalescing (W5, cinematic timing), frame pacing (W11d, game speed).
**Visual correctness**: F2, F3, V2 (programs using those features), W12c.
**Robustness**: W12a (VRAM extent guard), W2.
**Performance only** (defer): W13, D2, X1, X4, F7.
**Needs new design in the portable engine** (no upstream equivalent and no portable source): render-target write-back and the 1x1 luminance target (`LD_SURF_WB`, `LD_WB_DEFER`, `LD_WB_SYNC`: **not in the 12 patches**, they live in `rsx_live_draw.c`; Ascension's auto exposure reads that target from main memory) [`RENDERER_AUDIT.md` s.2], occlusion, async vertex conversion, texture cache dedup, SMAA/FSR.
Also outside the 12 patches: **33 fork-added files** already copied into `render/libs/video` as baseline (uber shaders, texpack, texture decode, FSR3 headers, SMAA, ...) which were not audited here.

## 5. Recommended order of reapplication

Criteria: need for running Ascension, technical risk, dependencies, ease of isolated validation, regression exposure. Smallest-first was not used.

| step | group | why here | validation |
|---|---|---|---|
| 0 | **a host unit-test harness for the relevant upstream tests** (not a patch) | every later step needs a repeatable check; the Android CMake excludes `tests/` and it is **unknown whether the `test_gcm_*` / `test_fp_decompiler` / `validate_*` tests build on this machine** | build and run the existing upstream tests as a baseline before any change |
| 1 | **W1 + W2** (IO allocator, offset-table write-through) | real, documented impact (every area load); portable and self-contained (tables + an `__atomic` lock); no dependency on SPURS, live draw or missing host code; testable in isolation; `GCM_IO_BUMP=1` gives an A/B in one binary | new unit test of map/unmap sequences (the exact failure: reuse after unmap, offsets inside 256 MB, table seen by a pointer taken at boot); `--tex-io` and the 30 scenes as regression |
| 2 | **W9 + W12a** (report validation, blit extent guard) | tiny, absent upstream, each prevents a documented freeze/crash | synthetic FIFO tests in the style of `test_gcm_*` (placeholder type 0xFF; a wrapped negative blit size) |
| 3 | **W5** (vblank coalescing) | small; fixes a measured timing error | test with a counting vblank handler and a delayed pump |
| 4 | **V2, F2, F3** one at a time | visual correctness; portable HLSL text; each default-on and gated | HLSL-text tests in `test_fp_decompiler.c` style (before/after text), scene runs, and a pixel check where a scene exists |
| 5 | **W11d** (frame pacing for Android clocks) | needed for the title's fixed 1/30 s step; rewrite of the timer part | pacing test (`test_gcm_flip_pacing.c` exists upstream) plus a measurement on the device |
| 6 | **W7, W8 refinements** | boot-critical, but cannot be validated without SPURS/the game | blocked (section 7); implement after the runtime provides the real `spu_taskset_round_busy` |
| 7 | surface write-back / 1x1 luminance (in the portable engine) | needed for correct exposure; a design task, not in the 12 patches | a scene that writes a 1x1 target and reads it from guest memory |
| 8 | performance (W13, D2, X1/NEON, X4) | only with Phase 3 baselines | before/after medians and p95/p99 |

### First recommended patch: step 1 (W1 + W2)
It is the first group that has value for the game, a documented failure it prevents, and no dependency on anything we cannot run or do not have; it can be integrated, switched off with one environment variable, and tested by itself.
The walker's sync points, which `MERGE_LOG.md` listed first, turned out to be mostly in upstream already (W6, W10 base, W11a/b) or boot-critical but unvalidatable without the game (W7, W8).
Step 0 must precede it.

## 6. Tests

Existing and relevant (in `render/libs/video/tests/`, **not built or run in this audit**): `test_gcm_park_reuse`, `test_gcm_spu_put_kick`, `test_gcm_resync_labels`, `test_gcm_report_clock`, `test_gcm_flip_pacing`, `test_gcm_flip_stream`, `test_gcm_flip_wrap`, `test_gcm_nop_runs`, `test_gcm_walker_prof`,
`test_fp_decompiler`, `validate_fp_shaders`, `validate_vp_decode`, `validate_vp_shaders`, `test_rsx_vp_run`, `test_rsx_transform_constants`, `test_vertex_fetch_plan`, `test_rsx_draw_engine`; fork-added: `test_rsx_dispatch_table`, `test_rsx_pso_epoch`, `test_rsx_vertex_simd`, `bench_gcm_walk`.
Already passing on the device: the 30 scenes with the validation layer (`results/r2_table.md`), which exercise the decompilers and the engine but **not** IO remapping across unmaps, reports with bad types, wrapped blit sizes or vblank backlogs.
To be created: IO allocator sequence test (W1/W2), report-validation and blit-guard tests (W9/W12a), vblank backlog test (W5), HLSL-text tests per decompiler fix (F2/F3/V2), a 1x1 write-back scene.

## 7. Blockers that depend on the EBOOT, the game host code or another machine

| item | blocked by |
|---|---|
| W4 EDGE6, W10 hooks | `edge6.c`, `n218_hle.c`, `spurs_pm_worker` are not in the repository, any branch or the history |
| W7, W8 refinements | the real `spu_taskset_round_busy` (runtime migration) and a running game/SPURS to see the round handshake |
| W8 data (`GCM_ROUND_*`) | addresses are title data; validating them needs the game |
| W11d final tuning | needs the real game clock |
| F2/F3 fidelity | visual check needs real frames (the fork's evidence is from Windows runs) |
| the EBOOT itself | `work/pristine/EBOOT.ELF` is not on this machine (5 GB RAM; the lift needs 16 GB or more): Phases 4-6 |

## 8. What was not done or not established

Not run: the upstream unit tests; any patch; anything on the device (this phase is source analysis). Not audited: the 33 fork-added files, `rsx_live_draw.c` (30.7k lines; its relevant switches are in `android/docs/ld_switch_ledger.csv`), `ps3kit/runtime`.
Not established: equivalence of upstream's reimplementations (F1, W6, W11a/b, X2, X3) with the fork's behaviour; the cost of vertex conversion on the A78 (no profile exists); whether `test_*` build on this host.
The fork's reasons (symptoms, run counts, dates) are quoted from its own comments and defaults file; they were not reproduced.

## 9. Corrections to earlier documents (not edited, to keep history)

- `MERGE_LOG.md` says the deltas are "4,6k changed lines"; the patch files total **5,645** (+5,398 / -247). Its per-file figures (2,279 / 1,148 / 1,292 / 487 / 298 / 133 / 8) sum to 5,645 as well.
- `MERGE_LOG.md` lists "the walker's guest-visible points" as the first thing to reapply; this audit finds that part largely already in upstream (W6, W10 base, W11a/b), so the first useful reapplication is the IO mapping fidelity (W1 + W2).
- `SESSION_SUMMARY.md` says the first job is "walker guest-visible points: labels, semaphores, references `cellGcm_ref_on_poll`, flips, SPU PUT blocks": `cellGcm_ref_on_poll` is where the fork put ~800 lines of new walker machinery (pacing, report holds, diagnostics) after it; the function itself changed by only a few lines (W16 is the part that touches it).
