# M514 - Utility Raster Mode

A raster render mode in Natron Utility that is faster on simple setups, mostly projections with motion blur (F7 - Natron Utility Core UAT round 1, item 6).

**How Utility renders today.** It draws each frame one sample per pixel per pass (`wavefrontDrawSamples`, `Engine/Nodes/Scene/Utility/Wavefront.h:303-322`, `Wavefront.cpp:1526`).
- On the GPU, each pass runs RayGen, then `GpuTracer::recordIntersect` for the camera rays (`WavefrontGpu.cpp:1242-1246`), then ResolveHits, Shade and the bounces (`:1162`, `:1418`, `:1478`).
- Each ray carries its own shutter time, and moving geometry is traced through swept-bounds structures (M283 - Utility Sampling And Motion Blur, Phase 3).
- Device fallback lives in `UtilityRenderPass.cpp:666-739`. The settings are in `Engine/Nodes/Scene/UtilitySettings.cpp` (`pixelSamples` :56, `shutterSegments` :185).

**What the GPU layer offers.** M472 - GPU Raster Pipelines gives `GpuRecordContext::createRaster/beginRaster/draw/drawIndexedIndirect` (`Engine/Gpu/GpuRecord.h:52-101`).
- It supports only R32F/RGBA32F attachments, with no depth (`GpuRaster.cpp:44`, `:130`).
- On NVIDIA and AMD the compute queue is an async family (`GpuDevice.cpp:383-415`), and a raster context on another family refuses textures (`GpuRecord.h:46-54`).

**What raster mode does.** It works one pass at a time. Each pass picks one shutter time and one sub-pixel jitter for the whole frame. It rasterizes a visibility buffer of instance and primitive ids, then rebuilds exact hits (t, u, v) for the jittered camera rays in compute. Everything from ResolveHits onwards stays the same code.

## Scope
**In:**
- New settings: `renderMode` (Ray/Raster) and `rasterSamples`.
- A depth attachment and uint colour attachments in the GPU raster layer.
- A raster visibility pass, with motion evaluated at each pass's time.
- Hit reconstruction into the existing hit layout.
- Ray-path fallback with a node warning, per-pass cancel, and progressive passes.

**Out:**
- Storm oversampling and blur: M515 - Storm Oversample And Motion Blur.
- A CPU rasterizer.
- Velocity post-blur.
- MSAA.
- Raster for the Spherical, Cylindrical and UV Unwrap projections, which render with Ray.
- Deep output: M286 - Utility Renderer Deep Output, on the ray path, used by both modes.

**Edge cases:**
- Depth and id AOVs take the heaviest sample, as the film does today (M283 decision, 2026-11-14). They are never averaged.
- Cutouts, Project3D discards and transmission go on through ResolveHits and the tracer.
- These render with Ray and warn: Device=CPU with Raster, a device without the raster capability, and a device without ray queries.
- Auto out-of-memory fallback goes to CPU Ray and warns.
- Frames over the VRAM budget or over `maxImageDimension2D` rasterize in bands or tiles, and shade in the executor's existing row batches.
- A closed shutter gives one time for every pass.
- A low `rasterSamples` steps visibly; the tooltip says so.
- A camera that moves or changes projection uses `wavefrontFrameAt` (`Wavefront.h:290`) at each pass's time. Lights stay at shutter centre.

**Assumptions:**
- Unconfirmed: D32_SFLOAT depth and R32G32_UINT attachments work on RADV, NVIDIA, ANV and llvmpipe. P1.T1 logs it.
- Unconfirmed: raster beats Ray mode on projection scenes. P3.T2 benches this; it is not gated.
- Unconfirmed: GpuTracer can share its device end-point buffers with raster without copying. P1.T2 assumes it.

## Phase 514.1: Visibility raster
- [x] M514.P1.T1 — Depth attachments and uint colour attachments in the raster layer
  - files: `Engine/Gpu/GpuRaster.{h,cpp}`, `Engine/Gpu/GpuTexture.{h,cpp}`, `Engine/Gpu/GpuRecord.{h,cpp}`, `Engine/Gpu/GpuDevice.{h,cpp}`, `Tests/Gpu/GpuRaster_Test.cpp`
  - approach:
    - Add a `GpuRasterDepth { None, Less }` pipeline option using a D32_SFLOAT attachment. `beginRaster` takes an optional depth texture and clear value.
    - Allow R32G32_UINT and R32G32B32A32_UINT colour attachments, Replace only, with no blend.
    - Add a `rasterVisibility` capability, queried through format features.
    - The pipeline cache key includes the depth mode.
  - verify: gtest on llvmpipe.
    - With depth, two overlapping triangles keep the nearer one's uint ids exactly, in both draw orders.
    - The capability is logged.
    - The existing raster tests are unchanged.
  - size: L
- [x] M514.P1.T2 — Device tables and visibility kernel for raster scenes
  - files: `Engine/Nodes/Scene/Utility/GpuTracer.{h,cpp}`, `Engine/Nodes/Scene/Utility/Kernels/RasterVisibility.slang` (new), `RasterVisibility.cmake` (new), `UtilityKernels.{h,cpp}`
  - approach:
    - GpuTracer exposes its device end-point point arrays, indices and per-instance end-point transforms, without copying.
    - Vertex shader: pulls vertices and interpolates at the pass's time. Points are linear; translation and scale are linear and rotation is slerped, matching M283 Phase 3. It then applies the jittered view-projection.
    - Fragment shader: writes (instance slot, primitive id), with depth Less.
    - Draws: one `drawIndexedIndirect` per geometry, with instances from a per-geometry instance list. Check for abort between draws.
  - verify: gtest.
    - In a static 3-cube scene, the visibility ids equal `recordIntersect`'s for the same un-jittered rays on every pixel except silhouette edges, which use the top-left rule.
    - A translating quad at time 0.5 sits at its interpolated position.
  - size: L

## Phase 514.2: Hits and passes
- [x] M514.P2.T1 — Rebuild hits from the visibility buffer
  - files: `Kernels/RasterHits.slang` (new), `.cmake` (new), `WavefrontGpu.cpp`
  - approach:
    - For each batch pixel, generate the jittered camera ray at the pass time, as RayGen does, into the same ray arrays, including its time.
    - Intersect the identified triangle, interpolated at that time. Write `hitValues` and `hitIds` with `recordIntersect`'s layout. An empty texel is a miss.
    - In the raster context, draw, then run RasterHits.
    - Release the hit and ray buffers to the compute family (`releaseToOtherQueue`). The compute context acquires them and continues from ResolveHits. When both queues are the same family, no ownership transfer is needed.
  - verify: gtest. For a static textured card and cube scene at 1 sample with jitter off:
    - colour, depth and the id AOVs from raster match Ray mode within 1e-4;
    - the ids match exactly.
  - size: L
- [x] M514.P2.T2 — Pass schedule: one time and one jitter per pass
  - files: `Wavefront.{h,cpp}` (`WavefrontFrame` gains `raster` and the pass fields), `WavefrontGpu.cpp`
  - approach:
    - Pass i of N gets a jitter from scrambled Halton (2, 3) under `seed`.
    - Its time is (perm(i) + 0.5)/N over the shutter, where perm is a seed-driven permutation that decorrelates time from jitter.
    - The film splats with the existing pixel filter, and `passDrawn` fires after each pass.
    - Rasterize in bands sized to the pool budget and the device's image limits.
  - verify: gtest.
    - A quad moving one pixel per frame gives a box smear, within a 1/N step tolerance.
    - A closed shutter equals the static render.
    - A frame larger than a forced small band limit equals the single-band render.
  - size: M
- [x] M514.P2.T3 — Render Mode and Raster Samples settings, with fallback
  - files: `Engine/Nodes/Scene/UtilitySettings.cpp`, `UtilityRenderDelegate.h`, `UtilityRenderPass.cpp`, `Tests/Scene/NatronUtilityRaster_Test.cpp` (new)
  - approach:
    - New knobs, both hashed:
      - `renderMode`, "Render Mode": Ray/Raster, default Ray.
      - `rasterSamples`, "Raster Samples": default 32, shown only in Raster.
    - Where Raster can't run (CPU device, no `rasterVisibility` or `rasterFloatBlend`, a non-Camera projection), render with Ray and report through `setFrameReport` with a warning naming the reason.
  - verify: gtest.
    - Each fallback gives the Ray image plus its warning.
    - Switching modes changes the hash.
  - size: M

## Phase 514.3: Hardening
- [x] M514.P3.T1 — Cancel and secondary-effect tests
  - files: `Tests/Scene/NatronUtilityRaster_Test.cpp`, `Tests/Scene/NatronUtilityCancel_Test.cpp`
  - approach:
    - Cancel mid-pass and mid-draw on a 4096-wide frame; it must stop within the existing cancel budget.
    - Shadows, a mirror reflection and a refracting sphere appear in Raster mode, loosely matching Ray (0.05 mean).
    - The bundled gtest is old: no GTEST_SKIP, and `INSTANTIATE_TEST_CASE_P` only.
  - verify: pass on llvmpipe in ctest.
  - size: M
- [x] M514.P3.T2 — Bench and node docs
  - files: `Engine/Nodes/Scene/docs/` (the Utility renderer page), `tools/bench/` (an env-gated bench, like `NATRON_UTILITY_MOTION_BENCH`)
  - approach:
    - Document Render Mode, Raster Samples, the fallbacks and the stepping.
    - Bench a projected-card shot with blur in Ray mode against Raster mode, and record the result in Decisions.
  - verify: the docs build; the numbers are recorded.
  - size: M

## Phase 514.4: Interactive Raster speed

Raster replaces only raygen and intersect; each pass still runs the shared wavefront pipeline (~95 ms/pass at 1080p on an RTX 3060: 8 batches with two blocking waits each, ~40 MB AOV readback, single-threaded CPU `AovFilm::writeTile`, full-frame `publishPass`, Shade/Resolve/bounce dispatches) plus ~0.7 s fixed per frame (incl. a BVH build Raster never uses). The same per-pass overhead hits Ray on every sample (~97 ms/sample), so T1-T3 and T5 apply to both modes and must speed up Ray too; T4 is Raster-only (Ray could still reuse an unchanged BVH), T6 is Raster-only. Diagnosis 2026-11-27: `WavefrontGpu.cpp:163, 1311, 1380, 1462-1747, 1914-1927, 1959-2015`, `Wavefront.cpp:1690, 1707-1727`, `UtilityRenderPass.cpp:147-175`, `UtilityRenderBuffer.cpp:938-1001, 1112-1185`.

- [ ] M514.P4.T1 — Per-stage profile and a fixed-vs-per-pass bench
  - files: `Engine/Nodes/Scene/Utility/` (an env-gated `NATRON_UTILITY_PROFILE` timer around tracer update, table upload, and per pass: raster, shade, readback, writeTile, publishPass), `tools/bench/utility_raster_bench.py`/`.sh`
  - approach: profile records go to a JSONL path from the env var; the bench renders N consecutive frames per session, fits time against pass counts (1, 4, 16, 64) to separate fixed and per-pass cost, and times the render without the 1080p PNG encode (EXR or no Write).
  - verify: a local llvmpipe run of the bench prints fixed and per-pass figures per mode and a per-stage table; no cost when the env var is unset.
  - size: M
- [ ] M514.P4.T2 — Keep every AOV on the device and publish once per frame
  - files: `WavefrontGpu.cpp` (depth/primId/instanceId accumulation like colour's `recordDeviceBatch`), `UtilityRenderPass.cpp`, `UtilityRenderBuffer.cpp`, `HydraRenderImpl.cpp` AOV mapping if needed
  - approach: accumulate depth, primId and instanceId on the device as colour already is; read back once per frame; in offline renders publish at frame end (progressive viewer keeps a timer-paced publish).
  - verify: Raster and Ray AOVs unchanged (NatronUtilityRaster, Golden, Motion tests); profile shows no per-pass readback/writeTile.
  - size: M
- [ ] M514.P4.T3 — One submission per Raster pass and one raster context per frame
  - files: `WavefrontGpu.cpp` (batch loop, raster context creation, submitAndWait points)
  - approach: render a Raster pass as one batch (or several passes per submission) and reuse one raster context per frame; drop the per-batch CPU waits.
  - verify: tests unchanged; profile shows one submit per pass.
  - size: M
- [ ] M514.P4.T4 — Skip the BVH build when Raster needs no secondary rays
  - files: `WavefrontGpu.cpp:1914` (`tracer.update`), the Raster setup
  - approach: when Render Mode is Raster and the frame needs no secondary rays (no bounces, shadows, holes needing resolve), skip `tracer.update`; build lazily if a later pass needs it.
  - verify: NatronUtilityRaster tests pass, incl. shadowed/bounce cases still tracing correctly; profile shows no BVH time in the emissive case.
  - size: M
- [ ] M514.P4.T5 — Cache per-frame setup across unchanged frames
  - files: `Wavefront.cpp:1690` (shutter snapshot), `WavefrontGpu.cpp:1922-1927` (shade tables and uploads)
  - approach: reuse the shutter snapshot and tables when the stage version and render settings are unchanged; check whether a Seed change forces a Hydra resync or BVH rebuild and avoid it if so.
  - verify: tests unchanged; profile shows the fixed cost drop on a second identical-stage frame.
  - size: M
- [ ] M514.P4.T6 — Emissive fast path: shade in the fragment shader into a float accumulation attachment
  - files: the Raster pipeline (`GpuRaster*`, the raster shaders), `WavefrontGpu.cpp` Raster path
  - approach: when the bounce limit is 0 and there are no shadow-casting lights or holes, the fragment shader shades directly (emissive/flat, projections) and additively blends into an RGBA32F accumulation attachment (`rasterFloatBlend` is already required); skip RasterHits/Shade and readback per pass. Other cases keep the shared pipeline.
  - verify: fast-path images match the shared-pipeline Raster render within noise (unit test); bench shows the per-pass cost at 1080p in single-digit ms on the vast RTX 3060.
  - size: L

**Verification gate:**
- Build and ctest green on both legs.
- GpuRaster, NatronUtilityRaster, NatronUtilityCancel, Motion and Golden pass on llvmpipe.
- GPU parity suites and the GPU bench unchanged.
- Xvfb check of the Render Mode and Raster Samples knobs.
- RADV and NVIDIA capability results recorded in Decisions.
- Interactive Raster: on the vast RTX 3060 bench (1080p blurred projected card), Raster per-pass cost in single-digit ms and Raster at least 5x faster than Ray at 32 samples, with the per-stage profile recorded in Decisions.
- Ray sheds the shared overhead too: Ray per-sample cost on the same bench at most a third of the 2026-11-27 figure (~97 ms/sample), recorded before/after.
- UAT AppImage with a steps .md.

## Decisions

- 2026-11-22 — A raster mode inside Natron Utility, not a new delegate (user, grilling): one renderer to maintain, sharing scene translation, AOVs and settings. Storm also gets oversampling and shutter accumulation, in M515 - Storm Oversample And Motion Blur.
- 2026-11-22 — Motion blur by accumulating time samples along the shutter segments (user, grilling): exact, and cheap per sample on raster.
- 2026-11-22 — Raster primary, ray secondary (user, grilling): the speed of raster with the ray look.
- 2026-11-22 — Without a usable GPU, render through the Ray path and warn (user, grilling): no CPU rasterizer to maintain.
- 2026-11-22 — Anti-aliasing by sub-pixel jitter on each accumulation sample. Raster mode does not have to match Ray mode; at most a loose parity check (user, grilling).
- 2026-11-26 — Visibility buffer from a depth attachment plus uint id attachments added to M472's raster layer (user). Deep output stays on the ray path in M286 - Utility Renderer Deep Output and covers both modes. A raster A-buffer for deep is a possible later follow-up reusing M286's compaction library.
- 2026-11-26 — A separate Raster Samples knob, default 32, shown only in Raster mode (user).
- 2026-11-26 — Secondary rays keep M283's swept motion structures; no per-pass rebuild (user).
- 2026-11-26 (PM) — P1.T1/T2 deviations: rasterVisibility also needs the geometryShader feature (SV_PrimitiveID), enabled when present; uint attachments open through beginRasterUint(); GpuPipelineCache carries the depth mode in its key; drawIndexedIndirect takes firstCommand for one draw per geometry; RasterVisibility is registered in Engine/Nodes/Scene/Utility/CMakeLists.txt (no per-kernel .cmake there); UtilityKernels.cpp unchanged (CPU raster is out of scope); NatronUtilityRaster_Test.cpp created in P1 rather than P2.T3.
- 2026-11-27 (PM) — First build of P1–P3 (m514-gate3) failed 5 raster tests, all test bugs (coverage edge, a texel-centre tie, an unreachable texel count, mirror/glass rendered with 0 bounces); the raster path was unchanged. m514-gate4 (4d113c5262): GpuRaster/NatronUtilityRaster/Cancel/USDRenderSettings 11/11. Effects: mirror 0.262 (Raster−Ray 0.0000), 0.267 (0.0018), 0.116 (0.0482 — close to the 0.05 bound, watch it). P3.T2's bench numbers need a real GPU: batched with M299's roto bench on one cloud GPU session.
- 2026-11-27 (PM) — First vast run of utility_raster_bench (RTX 3060, run cloud-gpu-20261127T232121Z) is invalid: the card renders without any motion blur and each timed render wrote the whole 1-250 frame range, so Ray ≈ Raster (1.60 vs 1.56 s at 9 spp, 4.66 vs 4.66 at 36). Bench script sent back for fixing (blur must show, one frame per render, fail without blur).
- 2026-11-27 (PM) — Bench fixed (blur shows, one frame per render, blur gate). It found a Raster-mode quality bug: with the default Blackman-Harris filter each Raster pass gets its single jitter offset's filter weight for the whole frame, so the pass nearest the pixel centre dominates and blur collapses to one or two steps (9 samples: Raster plate-edge smear 2.0 px vs Ray 5.6; Box filter 12.0 vs 9.6). PM call: fix the per-pass weighting in the renderer rather than bench with Box.
- 2026-11-27 (PM) — Ray vs Raster bench (vast RTX 3060, cloud-gpu-20261128T024440Z, gate7 build with the per-pass weighting fix): blurred projected card at 1920x1080 — 9 spp: Ray 2.00 s vs Raster 1.56 s median (Raster 1.28x faster; p95 1.27x); 36 spp: Ray 4.62 vs Raster 4.13 s (1.12x; p95 1.23x). Both modes pass the blur gate (plate-edge smear 9 spp Ray 22.7 / Raster 33.0 px; 36 spp 38.0 / 39.2 px).
- 2026-11-27 (PM) — Capability: NVIDIA (RTX 3060, driver 580.95.05) has rasterVisibility — the vast bench rendered Raster on the GPU with no fallback warning. RADV result still to record (needs an AMD GPU, e.g. the GPU host).
- 2026-11-27 — **Interactive Raster in M514 (user):** Raster as first built was only 1.12-1.28x faster than Ray because it shares the whole per-pass pipeline; the speed work goes into M514 as Phase 514.4 before it ships. Targets (PM default, reversible): single-digit-ms per pass at 1080p and ≥5x faster than Ray at 32 samples on the vast RTX 3060 bench.
- 2026-11-27 — **Ray gets the shared fixes (user):** the per-pass pipeline overhead also slows Ray on every sample; Phase 514.4's shared fixes (profiler, device-side AOVs + once-per-frame publish, one submission per pass, cached per-frame setup) must speed up Ray as well, and the gate checks Ray's per-sample cost.
