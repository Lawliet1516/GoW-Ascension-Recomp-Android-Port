# Renderer audit: what the production renderer does, and what a Vulkan/Android renderer must reproduce

Status: **report before any re-implementation** (requested). Branch `android/renderer-audit`. No renderer code was changed.

**Method and confidence.** Static analysis of the source tree plus targeted reading; the numbers come from
`android/tools/audit_renderer.py` (re-run it to reproduce them) and from probing the phone (`android/results/vkprobe_adreno710.txt`).
`rsx_live_draw.c` is 30,772 lines: it was **not** read line by line. Statements are tagged **[scan]** (counted/classified by script),
**[read]** (I read the code) or **[inferred]** (reasoned from names/comments, to be verified). Nothing here was run against the game.

## 1. What is in the tree (three render paths, one of them production)

| path | files | size | state |
|---|---|---|---|
| **Production (Windows)**: the "live draw" engine, NV4097 command stream -> D3D12 | `rsx_live_draw.c` | 30,772 lines / 1.63 MB | D3D12 only; on other platforms every entry point is a **no-op stub** [read, first lines of the file] |
| Old vtable path (rsx_state -> D3D12 backend) | `rsx_d3d12_backend.c`, `rsx_commands.c` | 6,801 lines | "Phase 1: clear, present, vertex-coloured triangle"; not the production draw path [read] |
| **Portable engine + backends** | `rsx_draw_engine.c` (+`rsx_dispatch.c`, `rsx_vertex_compact.c`, `rsx_texture_layout.c`, decompilers) with `rsx_vulkan_backend.c` / `rsx_metal_backend.m` | engine 2,705 lines; Vulkan 3,475; Metal 3,821 | the only path that exists for Vulkan; it was "carried across" from `rsx_live_draw.c` as it was when ps3recomp vendored it |

Upstream comparison **[scan, measured on both trees]**: the ps3recomp v0.17.0 `rsx_live_draw.c` is **10,545 lines (473 KB)**; this fork's is 30,772
(1,632 KB). **About 71 % of the production renderer is Ascension-specific work added after the portable engine was split off**: async pipeline
building, a persistent pipeline library, surface write-back, occlusion queries, a texture cache with dedup/LRU/heaps, draw reuse, constant-buffer
dedup, SMAA/FSR, floor/hole fixes. None of the markers I searched (`LD_PSO_ASYNC`, `LD_SURF_WB`, `OCCL_`, `LD_SMAA`, `LD_TEX_DEDUP`,
`LD_DRAW_REUSE`) exist in the upstream baseline. Upstream's portable engine kept evolving (engine 130 KB at v0.17.0 -> 191 KB at v0.20.0; Metal
188 -> 258 KB; Vulkan backend 182 -> 245 KB, with a CI that runs 12 draw scenes under the validation layer); this fork's copies are older (engine 115 KB,
Vulkan 169 KB).

Where the production code stands relative to D3D12 **[scan]**: of 686 functions, 251 (**64 % of the lines**) mention D3D12/DXGI/D3DCompile or the
engine's device/queue globals, 359 (26 %) are free of any OS/API symbol, 76 (10 %) use only Windows APIs (threads, timers, events).
"D3D12-bound" is an upper bound: such functions also contain portable decision logic that can be lifted out.

## 2. How a frame flows (and the contract any renderer must keep)

`cellGcmSys.c` (the FIFO walker, 5.8k lines, portable) decodes NV4097 methods -> `rsx_dispatch.c` (register file, portable) -> the engine ->
backend. Two sets of calls are woven through `cellGcmSys.c`: `rsx_live_draw_*` (Windows production) and `rsx_draw_engine_*` (portable): 2D blits
(NV3089), flips, display buffers, references. **[read]**

Facts that constrain the design, found in the production code **[read]**:

1. **Guest-visible synchronization points.** The walker calls `rsx_live_draw_vtx_join` "at everything that shows the guest how far the RSX has
   got: labels, semaphores, references, reports, flips, user commands, 2D writes, the end of a pass". A renderer may batch freely *between* these points
   and must have retired everything the guest can observe *at* them.
2. **Render targets are read back into guest memory.** Ascension draws the scene's average luminance into a 1x1 ARGB8 target in main memory
   (`1:0xAA0000`) and its auto exposure reads it from there; without write-back "the menu came out washed out". `LD_SURF_WB` (default on) copies small
   targets to a readback buffer at each flush and stores them as the RSX would (A,R,G,B). The ordering rules (`LD_WB_SYNC`, `LD_WB_DEFER`) exist
   because stale values reached the game. The engine's `depth_readback` and `guest_read/guest_wrote` hooks are the portable counterparts.
3. **The game reads/writes surfaces with CPU and 2D engine** (render target -> main memory -> VRAM via NV3089, post-processing on SPUs): the
   engine keeps host surfaces coherent with guest memory around those transfers.
4. **Pipelines are not available when the draw is issued**: the production engine parks or skips draws while a pipeline compiles
   (`LD_PSO_ASYNC`, `LD_FLOOR_DEFER`, `LD_PSO_HOLD_*`), because the first visit of an area otherwise stutters for seconds. Any Vulkan design needs the same policy.

## 3. Feature map by category

Legend: **Reuse** = portable code used as is; **Translate** = same logic, different API calls; **Rewrite** = no usable equivalent, new design.

### 3.1 Render targets and framebuffers

| production behaviour | source | Vulkan/Android | verdict |
|---|---|---|---|
| colour surfaces keyed by (location, offset) in guest memory, created on demand, seeded from guest bytes, evicted by age | `surfaces` section (~800 lines `ld_surface*`), `LD_SURF_EVICT_FRAMES/ALIAS/GROW_ALIGN` [scan] | `VkImage` + view per surface; seed upload by staging (UMA: map and copy) | Reuse the keying/aliasing logic (it is in the portable engine in a baseline form); Translate creation |
| MRT up to 4 targets, all of target A's size/format; secondary attachments must take A's blend/mask | engine contract (`rt_count` in the pipeline key) [read] | multiple colour attachments in one render pass | Translate |
| FP16 HDR target (`SET_SURFACE_FORMAT 0xB`) | `R16G16B16A16_FLOAT` 20 refs [scan] | `R16G16B16A16_SFLOAT` colour attachment (Adreno supports) | Translate |
| "zeta" keep / repack (`LD_ZETA_KEEP`, `LD_ZPACK`, `make_zeta*` ~560 lines) | production only | depth image per zeta address; keep-on-resize copy pass | Translate (needs a fullscreen pass pipeline) |
| display buffers and present | `ld_present*` ~500 lines + DXGI | acquire -> blit/draw -> present (see 3.8) | Rewrite on the new swapchain (done in `android/app`) |
| 1x1 luminance write-back, 2D-transfer coherence | see section 2 | host buffer readback (UMA: no staging copy needed, but needs the barrier + fence) | Translate; **correctness-critical** |

### 3.2 Shaders and pipelines

| production behaviour | source | Vulkan/Android | verdict |
|---|---|---|---|
| RSX vertex/fragment programs -> HLSL by decompilers | `rsx_vp_decompiler.c` 937, `rsx_fp_decompiler.c` 2,597, `rsx_fp_uber.c` 2,182 lines [scan] | unchanged (portable) | **Reuse** |
| HLSL -> bytecode with FXC (`D3DCompile`, SM 5.0; 2 refs to 6.0 for DXC) | 18 call sites; 4 `vs_5_0`, 8 `ps_5_0`, 3 `cs_5_0` [scan] | HLSL -> SPIR-V with glslang (`rsx_shader_spirv.cpp`, 140 lines, VS+FS only, Vulkan 1.0 target) | Translate; compute stages (`cs_5_0`: post-processing) not covered |
| HLSL features emitted: `Sample/SampleLevel/SampleBias/Load/GetDimensions`, `ddx/ddy`, `clip/discard`, `f16tof32/f32tof16`, `asfloat/asuint`, `SV_Position/Target/Depth/VertexID/IsFrontFace`, `cbuffer`, Texture2D/3D/Cube | token scan of the decompilers [scan] | all are standard SM5 and are accepted by glslang's HLSL front end (upstream CI compiles every scene on lavapipe) | feasible; **cost on a phone CPU unmeasured** |
| pipeline key = VP + FP + structural render state + vertex layout + RT format/count; alpha reference and constants excluded so per-draw constant changes do not multiply pipelines | `ld_hash_structural_render_state`, mirrored in `rsx_draw_engine_hash_render_state` [read] | same key -> `VkPipeline` | Reuse the key; Translate creation |
| async pipeline build with thread pools, "urgent lane", split compile, hold/skip policy, one-compile-per-text, memoisation (**51 switches**, ~1,500 lines `ld_pso*`) | production only [scan] | worker threads + `VK_EXT_pipeline_creation_cache_control` (Adreno has it) + same hold/skip policy | Translate the policy, Rewrite the mechanism |
| pipeline library persisted between runs; 13,425 recorded pipelines shipped for the Windows release | `CreatePipelineLibrary/StorePipeline` [scan], README | `VkPipelineCache` saved to the app's files dir, validated by header (vendor/device/driver UUID); the *recipes* (what to precompile) can be recorded on the phone, **shaders derive from the game and cannot be shipped** | Rewrite |
| shader disk cache (HLSL text -> bytecode) | `shader_disk*` ~350 lines | SPIR-V cache beside the pipeline cache | Translate |
| root signature (8 `CreateRootSignature`) | | descriptor set layout + push constants (Adreno 710: 64 KB UBO, 7 sets, push descriptors available) | Translate |

### 3.3 Textures and formats

| production behaviour | Vulkan/Android | verdict |
|---|---|---|
| decoded to host rows by the portable `rsx_texture_decode` / `rsx_texture_layout.c` | unchanged | **Reuse** |
| formats in the D3D12 code [scan]: R8G8B8A8_UNORM (69 refs), RGBA16F (20), RGBA32F (19), R32F (14), R8 (8), BC1/BC2/BC3 (8/7/7), R16G16F, R8G8, R16G16B16A16, D24S8, D32S8X24 | Adreno 710 **samples BC1/BC3/BC7 natively** (probe), RGBA16F/32F, D24S8, D32S8 all yes; BC2 not probed (add) | Translate; no software transcoding needed |
| texture cache: keyed by content hash, intra-frame refresh, LRU (1,024 entries in the engine; **41 switches**, `LD_TEX_*`, `LD_SMP_*`), write-watch to avoid re-hashing, sub-allocation from 64 MB heaps | Vulkan memory suballocation + the same cache logic | Reuse the key/LRU logic; Rewrite the heap (placed resources -> `vkAllocateMemory` + offset) |
| sampler tables that persist, rotate and recycle (`LD_SMP_*`) | `VkSampler` cache (probe: `maxPerStageDescriptorSamplers` 524,288; the allocation-count limit was not probed) | Translate |
| anisotropy, LOD bias in 4.8 fixed point, border colour, shadow-compare scale, volume and cube fixes (`LD_TEX_ANISO`, `LD_TEX_LODBIAS`, `LD_TEX_BORDER`, `LD_VOLUME`, `LD_CUBE_STRIDE`, `LD_SHADOW*`) | `samplerAnisotropy`, `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER` (custom border needs an extension: check), compare samplers | Translate; each is a **correctness fix with a visible symptom** (see the ledger) |
| fp16 "half2" copies for R16G16F, fp16 texture decode | RG16F images | Translate |

### 3.4 Depth and stencil

| behaviour | Vulkan/Android |
|---|---|
| D24S8 and D32S8X24 targets; typeless sampling (`R24_UNORM_X8_TYPELESS` / `R32_FLOAT_X8X24_TYPELESS` views) | D24S8 is supported; sampling depth needs a depth-aspect view in `SHADER_READ_ONLY_OPTIMAL` (cannot alias like D3D12 typeless): the engine's `depth_snapshot` (copy/resolve to a sampleable image) is the portable answer |
| depth bounds (`OMSetDepthBounds`, `LD_DEPTH_BOUNDS`), two-sided stencil, depth bias, `LD_ZCLIP` (clip vs clamp) | `depthBounds` feature **not in the probe list: query it**; the rest is core Vulkan |
| depth readback into guest memory for textures that read depth bytes (`LD_ZREAD_RECENT`, `depth_readback`) | readback buffer; expensive on a tile GPU, keep it rare |

### 3.5 Blending

Core states only (`BLEND_SRC1` 0 refs, no alpha-to-coverage, no conservative raster) **[scan]**. The production engine folds some blends into shader maths
(`LD_BLEND_FOLD`, `LD_BLEND_SCALE`, `LD_BLEND_COLOR`) because D3D12 lacks `ALPHA_FACTOR`-style constants on some GPUs; Vulkan has
`blendConstants` and independent blend (Adreno: both yes). **Translate**; the folding switches can stay off initially. Logic ops: 2 refs, supported.

### 3.5b Synchronization

| D3D12 production | Vulkan |
|---|---|
| one queue, one fence (`Signal`/`GetCompletedValue`/`SetEventOnCompletion`: 8/21/10 sites), command allocator rings, "simple synchronous model" with explicit flush reasons | one queue family (graphics+compute, 3 queues on Adreno) + a **timeline semaphore** (extension present) for CPU waits; ring of per-frame pools |
| resource state tracking: 304 `D3D12_RESOURCE_STATE_` refs, 23 `ResourceBarrier` sites | image layout + access tracking per image; `VK_KHR_synchronization2` present on the device |
| early-submit allocators and "pairs" for occlusion (`OCCL=2`) | optional, perf only |

**Important:** the production engine itself is *not* per-draw synchronous: it records many draws into one command list and waits only at
flushes **[read, enum `ld_flush_reason`]**: PRESENT, GUEST_REFERENCE, VERTEX_RING, VERTEX_CONSTANT_RING, PIXEL_CONSTANT_RING, DESCRIPTOR_RING,
UPLOAD_RING, RETIRE_QUEUE, READBACK, MOVIE, MOVIE_PRESENT, SHUTDOWN. Six of the twelve are *ring-full* flushes: ring sizing decides how often the GPU
is drained, a design lever for Vulkan (bigger rings in UMA cost nothing but address space). **The Vulkan backend in this tree is**: each `vk_eng_draw` does `vkBeginCommandBuffer`,
a full render pass for that single draw, `vkQueueSubmit` and a fence wait **[read, `rsx_vulkan_backend.c:3100-3185`]**.

### 3.6 Command buffers and draw data

| production | Vulkan |
|---|---|
| vertices are converted on the CPU (`rsx_vertex_compact.c`, optionally on 1-4 worker threads `LD_VTX_ASYNC`) into a ring (`LD_ASYNC_RINGS`) | UMA: write straight into a persistently mapped host-visible, coherent+cached device-local buffer (types 4/6 in the probe); ring with per-frame offsets |
| constant buffers with change detection (`LD_CB_GEN`, `LD_CB_DEDUP`) | uniform ring + dynamic offsets (64 B alignment) |
| draw reuse / bind memoisation (44 switches) | perf only, after correctness |

### 3.7 Caches (summary)

Shader text, pipeline library, texture content cache, sampler tables, bind memo, constant-buffer generations, surface cache. All have a portable
*policy*; their *storage* is API-specific. Everything persists in `%LOCALAPPDATA%` on Windows; on Android `getFilesDir()` (cache is
per install and rebuilt on first run).

### 3.8 Memory management and presentation

D3D12: 53 `CreateCommittedResource`, 1 `CreatePlacedResource`, 1 `CreateHeap` (texture heaps), upload/readback/default heaps **[scan]**. Android:
unified memory (one 11.3 GB device-local heap with host-visible+coherent types): no staging for uploads or readbacks, but **non-coherent
cached memory needs explicit flush/invalidate** and every host-visible allocation counts against the same pool. A small suballocator (not a
per-resource `vkAllocateMemory`: drivers limit allocation counts) is a prerequisite.
Present: the DXGI swapchain, FSR3 (`rsx_fsr3.h`, 1,026 lines, compute) and SMAA passes are **optional**; the new swapchain in `android/app` covers plain presentation.

### 3.9 D3D12-specific constructs with no direct Vulkan twin

`CreateCommittedResource` (committed model), descriptor heaps + root descriptor tables, resource states (vs layouts), `D3DCompile`/FXC, DXGI swapchain with
tearing flags, `ID3D12PipelineLibrary`, `OMSetDepthBounds`, `LD_GPU_WARP` (Microsoft's software rasterizer, a test switch [read]), the debug layer. Each has a Vulkan mapping above; none is blocking.

## 4. Reuse / translate / rewrite, in lines (estimates, upper bounds)

| bucket | content | size |
|---|---|---|
| **Reuse** (portable already) | `cellGcmSys.c`, `rsx_dispatch.c`, vertex/texture/restart-cut math, VP/FP decompilers and the uber shader, state-key hashing, the engine's orchestration and caches in their baseline form | ~15 k lines + 8,036 portable lines of `rsx_live_draw.c` |
| **Translate** (same logic, other API) | surfaces, depth, textures, samplers, blend/stencil state, queries, readbacks, write-back, 2D coherence | most of the 19,616 D3D12-bound lines, but much of that is the *same logic as the portable engine* (the baseline, which is already ported to Metal and Vulkan): the real incremental work is the **~20,000 lines Ascension added** over upstream's 10,545 |
| **Rewrite** | submission/synchronization, resource allocator, pipeline cache + async compile, descriptor management, present, compute post-passes (optional) | new code; the pieces the production engine does with D3D12 idioms |

The effort is therefore **not** "port 30 k lines of D3D12 calls". It is: (a) make the portable engine+Vulkan backend a *correct and fast* implementation of
its own contract (rewrite of the backend internals), then (b) re-apply the Ascension behaviours that change *what is drawn* (the 84 default-on switches in
`android/docs/ld_switch_ledger.csv`) into the portable engine one at a time, each with a regression test, guided by the game.

## 5. Backend restructuring: from submit-and-wait per draw to a pass-based recorder

Today (`rsx_vulkan_backend.c`): per draw one render pass + one submit + one wait; images always `GENERAL`; Vulkan 1.0 entry points; vertex data
through a single buffer reused after each wait. On a tile-based GPU a render pass is the unit of work: each begin/end forces tile load/store of
every attachment, and a wait per draw serializes CPU and GPU. This design is not viable at any resolution; it is a correctness prototype.

Target design (to be implemented in order, see section 6):

1. **Pass recorder.** The engine already tells the backend when targets change (`bind_targets`), when clears happen and when guest-visible points are hit
   (`submit_and_wait(reason)`). Keep one command buffer and one render pass open across consecutive draws to the same target set; end the pass only on
   target change, depth-snapshot, readback, 2D transfer, present or flush. `loadOp = CLEAR` when the RSX clear precedes the first draw, `DONT_CARE` for
   targets that are fully overwritten, `storeOp = DONT_CARE` for depth that nobody samples (the engine knows when `depth_snapshot` is requested).
2. **Submission.** Frames in flight (2-3), one timeline semaphore; CPU waits only at the guest-visible points above (and for the 1x1 write-back
   value with the deferral the production engine uses). No `vkQueueWaitIdle`/`vkDeviceWaitIdle` outside teardown.
3. **Image states.** `UNDEFINED` -> `COLOR_ATTACHMENT_OPTIMAL` -> `SHADER_READ_ONLY_OPTIMAL` -> `TRANSFER_*` tracked per image with
   `VK_KHR_synchronization2` barriers (device extension is present). **No permanent `GENERAL`**: it disables framebuffer compression on both Adreno (UBWC) and Mali
   (AFBC). Do not request `STORAGE` usage on colour targets unless a compute pass needs it (same reason).
4. **Memory.** One host-visible+coherent+cached ring (UMA) for vertices, indices, uniforms; suballocated device-local images; explicit flush/invalidate for
   any non-coherent type.
5. **Pipelines.** Persistent `VkPipelineCache` + async creation workers with the production engine's hold/skip policy; precompile recipes recorded on the phone.
6. **Descriptors.** Per-frame descriptor pools or `VK_KHR_push_descriptor` for the per-draw texture set (present on the device); samplers cached.
7. **GPU-specific.** Adreno 710 and Mali (Valhall): prefer **fewer, larger render passes**, `LOAD_OP_CLEAR/DONT_CARE`, transient depth where the engine allows,
   avoid mid-pass copies (they force a pass break), pre-rotate for the display transform (`VK_QCOM_render_pass_transform` on Adreno) instead of compositor
   rotation, keep MRT G-buffer passes together (candidate for subpass inputs later), expect `maxUniformBufferRange = 64 KB` (fits the RSX constant files).
   Things to **measure** on the device rather than assume: cost of `LOAD_OP_LOAD` for the large HDR targets, UBWC behaviour with the formats used,
   glslang HLSL compile time per shader on the A78, pipeline creation time and cache hit behaviour.

## 6. Implementation order (each step ends in a test that can fail)

| step | work | exit criterion |
|---|---|---|
| R0 | run upstream's scene suite (`ps3recomp_host` scenes) through the Vulkan backend **on the phone**; build glslang for arm64 (the NDK ships its sources: `sources/third_party/shaderc/third_party/glslang`) | all scenes pass on the Adreno 710 with the current (slow) backend: establishes a **correctness baseline** before any restructuring |
| R1 | per-pass recorder + timeline submission + barriers/layout tracking in the backend (no new features) | same scenes pass, byte-identical readbacks; draws/s measured against R0 |
| R2 | UMA ring buffers + suballocator; `VkPipelineCache` persistence | scenes unchanged; allocation count and upload copies reduced (counted) |
| R3 | async pipeline creation with hold/skip policy | no frame is shown with a missing draw (the `LD_FLOOR_ZERO` rule), measured first-visit stall |
| R4 | guest write-back/readback + 2D-transfer coherence on the new recorder | the 1x1 luminance target reads back the value the draw produced, ordering test (stale value must not appear) |
| R5 | re-apply the default-on correctness switches to the portable engine, one by one (ledger), each with a small replay test | each fix demonstrated by its own scene (e.g. cube stride, shadow scale, zclip) |
| R6 | with the user's `EBOOT.elf`: run the real game's first scenes, compare frames with the Windows port's frame dumps | visual equivalence on a recorded sequence |
| R7 | optional passes: SMAA, FSR3/compute, occlusion culling | only if frame time needs them |

Switching R0 first matters: without a baseline that passes on the Adreno, every later change (and every driver quirk) is indistinguishable
from a regression.

## 7. Risks and open questions

1. **HLSL front-end cost on the phone.** Every pipeline starts as HLSL text built by the decompilers and compiled by glslang on the phone's CPU at first
   use; the Windows release ships 13,425 recorded pipelines (not distributable here: they come from the game). First-run times may be minutes; unmeasured.
2. **Engine drift.** The portable engine in this tree is older than upstream's (v0.20.0: +46 %). Decision needed: vendor upstream's newer engine/Vulkan backend
   (MIT, read-only use of upstream, as requested nothing is pushed there) or keep this fork's and port selectively. Recommendation: **vendor upstream's newer
   portable engine and Vulkan backend into a branch**, because its CI-validated scenes are the cheapest correctness oracle available, then re-apply Ascension behaviour on top.
3. **Resolution/target.** The game renders at 720p on the PS3; the Windows port scales up. The Android target (native 720p, 30 fps?) should be agreed
   before tuning anything.
4. **The correctness-critical behaviours are in the ledger, not in a spec.** 84 behaviours are on by default (`ld_switch_ledger.csv`; the classification of
   defaults is heuristic: 142 of 417 switches are listed as unclassified) and they interact. Re-applying them needs the game to verify each.
5. **Features absent from the Android probe:** `depthBounds`, `imageCubeArray`, `BC2`, custom border colours were not queried; do it before deciding what the engine may use.
6. **Compute post-passes** (`cs_5_0`: SMAA/FSR-class) are outside `rsx_hlsl_to_spirv` (VS/FS only).

## 8. What this audit does not say

It does not estimate frame time. It does not claim the Vulkan path can match the Windows renderer's behaviour; it says what that would take. The one-frame
test pattern in `android/app` proves presentation only.
