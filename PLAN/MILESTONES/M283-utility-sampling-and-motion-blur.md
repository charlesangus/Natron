# M283 - Utility Sampling And Motion Blur

Turn Natron Utility from one ray per pixel into a sampled renderer: stratified camera samples,
a reconstruction filter, progressive convergence for the viewer, and true motion blur — camera,
transform and deformation — over a shutter that comes from the USD camera and can be overridden
in render settings. Motion vectors themselves are output by M285 - Utility Renderer AOVs; this
milestone provides the time sampling they are measured from.

## Scope

**In:**
- Settings (as `HdRenderSettingDescriptor`s, knobs via M253 - Delegate Render Settings): `Samples` (N×N stratified grid, default 3), `Pixel Filter` (box, triangle, Gaussian, Blackman-Harris; default Blackman-Harris) and `Filter Width`, `Seed`.
- Progressive convergence: the render pass accumulates sample passes and reports `IsConverged` when the sample count is reached, so M254 - Progressive Viewer Render refines in the viewer; USDRender waits for convergence.
- Shutter: the camera's `shutter:open`/`shutter:close` by default; settings override `Shutter` (length in frames), `Shutter Offset` (centred / start / end / custom) and `Shutter Segments`. Per-type toggles: camera, transform, deformation.
- Motion sampling through Hydra's interval APIs: `SampleTransform` for xforms and instancers, `SamplePrimvar` for points, plus `velocities`/`accelerations` (with `nonlinearSampleCount`/`blurScale`) when points are not time-sampled.
- Time-bucketed tracing on both backends: camera samples are stratified into `Shutter Segments` time buckets; per bucket the tracer refits deforming BLAS, rebuilds the TLAS at that time, and traces that bucket's rays. Same algorithm on CPU and GPU, for parity.
- `HydraRender::Request` carries the shutter interval so the engine gives the delegate motion samples.

**Out:**
- Motion vector AOVs — M285 - Utility Renderer AOVs.
- Adaptive sampling, denoising — none.
- Light motion blur (moving lights) — none; lights are evaluated at shutter centre.
- Lens depth of field — none (USD camera f-stop is ignored; documented).

**Edge cases:**
- A prim with fewer time samples than buckets: transforms are interpolated (matrix decomposition, slerp for rotation), points linearly.
- Topology changing within the shutter: no deformation blur for that mesh (static at shutter centre) and a node warning names it.
- Shutter of zero length, or all blur toggles off: one bucket at the frame time; output identical to the unblurred render.
- `velocities` present and points time-sampled: time-sampled points win (USD rule).
- Shutter Segments greater than the samples count: clamped to the sample count.

**Assumptions:**
- `UsdImagingGLEngine` passes a shutter interval through to the delegate's `SampleTransform`/`SamplePrimvar` when the camera and render settings declare one (and the Request gains the fields to set it); verify in the pinned headers and usdview.
- Rebuilding the TLAS and refitting deforming BLAS per bucket is fast enough at 8–16 buckets for typical scenes; measure in M283.P2.T3.

## Phase 283.1: Pixel sampling

- [x] M283.P1.T1 — Stratified samples and pixel filter
  - files: `Engine/Nodes/Scene/Utility/Kernels/RayGen.slang`, `Engine/Nodes/Scene/Utility/Kernels/Film.slang`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.cpp`, `Tests/Scene/NatronUtilitySampling_Test.cpp`
  - approach: RayGen draws stratified jittered positions from a per-pixel hashed seed; Film splats each sample's AOVs with the chosen filter weight into accumulation buffers (weighted sum plus weight), resolved on `Resolve()`. Register the four settings.
  - verify: gtest — a half-plane edge renders the analytic coverage within 1/(N²) per pixel; a 1-pixel feature is reconstructed by each filter to the analytic filter response; CPU and GPU agree within tolerance.
  - size: L

- [x] M283.P1.T2 — Progressive passes and convergence
  - files: `Engine/Nodes/Scene/Utility/UtilityRenderPass.{h,cpp}`, `Engine/Nodes/Scene/Utility/UtilityRenderBuffer.cpp`, `Tests/Scene/NatronUtilitySampling_Test.cpp`
  - approach: render in passes of one sample per pixel, publish after each pass, reset accumulation on scene/camera change, report `IsConverged` at the target count. USDRender's path waits for convergence; the viewer (M254) shows intermediate passes.
  - verify: gtest — after k passes the buffer equals the mean of k independent single-sample renders; an edit mid-way resets the count; USDRender returns only the converged frame.
  - size: M

## Phase 283.2: Motion blur

- [ ] M283.P2.T1 — Shutter in the request and settings
  - files: `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}`, `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.cpp`, `Tests/Scene/USDRender_Test.cpp`
  - approach: add the shutter interval to `HydraRender::Request` (from the camera, overridden by the delegate settings), pass it to the engine so Hydra serves interval samples, hash it into the render key. Register Shutter, Shutter Offset, Shutter Segments and the three toggles.
  - verify: gtest — changing the camera's shutter or the override changes the node hash and the interval the delegate receives (logged by a test hook).
  - size: M

- [ ] M283.P2.T2 — Motion samples in scene sync
  - files: `Engine/Nodes/Scene/Utility/UtilityMesh.cpp`, `Engine/Nodes/Scene/Utility/UtilityInstancer.cpp`, `Engine/Nodes/Scene/Utility/UtilityScene.{h,cpp}`, `Tests/Scene/NatronUtilityMotion_Test.cpp`
  - approach: store per-mesh transform samples and point samples (or velocities/accelerations) over the interval; `UtilityScene::evaluateAt(t)` produces the transforms and, for deforming meshes, point arrays at a bucket time. Topology changes within the interval mark the mesh static and record a warning.
  - verify: gtest — a translating cube evaluates to the interpolated position at mid-shutter; a velocities-only mesh moves by v·t; a topology-changing mesh is static and named in the warning.
  - size: L

- [ ] M283.P2.T3 — Time-bucketed tracing on both backends
  - files: `Engine/Nodes/Scene/Utility/UtilityRenderPass.cpp`, `Engine/Nodes/Scene/Utility/EmbreeTracer.cpp`, `Engine/Nodes/Scene/Utility/GpuTracer.cpp`, `Engine/Nodes/Scene/Utility/Kernels/RayGen.slang`, `Tests/Scene/NatronUtilityMotion_Test.cpp`
  - approach: assign each camera sample to a time bucket (stratified across the shutter); per bucket, refit deforming geometry and rebuild instances at the bucket time, then trace that bucket's ray batch. Camera blur moves the ray origins/frustum per bucket. Record per-bucket timings.
  - verify: gtest — a cube moving one pixel-width per frame yields a box-filtered smear of the analytic width on both backends; segments = 1 equals the unblurred render; a bench on a 1M-triangle deforming mesh at 8 buckets is recorded in the report.
  - size: L

**Verification gate:** full build and ctest green on both backends; the viewer refines progressively under Xvfb (GUI script); a motion-blurred render is shared with the user.

## Decisions

- 2026-11-01 — **True motion blur and antialiasing (user):** shutter sampling, multi-segment transforms and deformation, configurable pixel filter.
- 2026-11-01 — **Shutter from the camera, overridable (user).**
- 2026-11-01 — **Time-bucketed tracing on both backends:** Vulkan's KHR acceleration structures have no motion blur, so both backends rebuild per time bucket for identical results (Nuke's ScanlineRender also renders per time sample).
- 2026-11-14 — Claimed by bosley-native-nodes (user request).
- 2026-11-14 — Phase 1 implemented (03c49bfda). Only colour is filtered; depth and ids take the heaviest sample. The settings key is `pixelSamples`, so it can't be read as the viewer's cap. The viewer shows each completed sample pass. PM calls: no hard cap on Pixel Samples (no-hand-holding rule), only a soft slider maximum. Exact-match tests (Storm match, cancel, purpose) pin `pixelSamples=1`; goldens are re-recorded.
