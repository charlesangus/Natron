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
- ~~Time-bucketed tracing on both backends~~ (Phase 2, replaced by Phase 3 after UAT round 1): camera samples are stratified into `Shutter Segments` time buckets; per bucket the tracer refits deforming BLAS, rebuilds the TLAS at that time, and traces that bucket's rays.
- Continuous-time motion blur (Phase 3): the shutter is cut into `Shutter Segments` segments (default 1) whose end-points are the motion samples (with one segment: shutter open and close, i.e. towards the previous and next frame for a centred shutter). Every ray carries its own time, drawn continuously and stratified across the shutter; transforms and points are interpolated linearly between the two end-points around it. Segments raises the end-point count for curved motion. Same model on CPU and GPU.
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
- Shutter Segments greater than the samples count: clamped to the sample count (Phase 2 only; Phase 3 never clamps, segments only add end-points).
- Phase 3: a prim with fewer authored samples than end-points is evaluated at each end-point (decomposed transforms, linear points), so every prim has exactly segments + 1 end-points.

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

- [x] M283.P2.T1 — Shutter in the request and settings
  - files: `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}`, `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.cpp`, `Tests/Scene/USDRender_Test.cpp`
  - approach: add the shutter interval to `HydraRender::Request` (from the camera, overridden by the delegate settings), pass it to the engine so Hydra serves interval samples, hash it into the render key. Register Shutter, Shutter Offset, Shutter Segments and the three toggles.
  - verify: gtest — changing the camera's shutter or the override changes the node hash and the interval the delegate receives (logged by a test hook).
  - size: M

- [x] M283.P2.T2 — Motion samples in scene sync
  - files: `Engine/Nodes/Scene/Utility/UtilityMesh.cpp`, `Engine/Nodes/Scene/Utility/UtilityInstancer.cpp`, `Engine/Nodes/Scene/Utility/UtilityScene.{h,cpp}`, `Tests/Scene/NatronUtilityMotion_Test.cpp`
  - approach: store per-mesh transform samples and point samples (or velocities/accelerations) over the interval; `UtilityScene::evaluateAt(t)` produces the transforms and, for deforming meshes, point arrays at a bucket time. Topology changes within the interval mark the mesh static and record a warning.
  - verify: gtest — a translating cube evaluates to the interpolated position at mid-shutter; a velocities-only mesh moves by v·t; a topology-changing mesh is static and named in the warning.
  - size: L

- [x] M283.P2.T3 — Time-bucketed tracing on both backends
  - files: `Engine/Nodes/Scene/Utility/UtilityRenderPass.cpp`, `Engine/Nodes/Scene/Utility/EmbreeTracer.cpp`, `Engine/Nodes/Scene/Utility/GpuTracer.cpp`, `Engine/Nodes/Scene/Utility/Kernels/RayGen.slang`, `Tests/Scene/NatronUtilityMotion_Test.cpp`
  - approach: assign each camera sample to a time bucket (stratified across the shutter); per bucket, refit deforming geometry and rebuild instances at the bucket time, then trace that bucket's ray batch. Camera blur moves the ray origins/frustum per bucket. Record per-bucket timings.
  - verify: gtest — a cube moving one pixel-width per frame yields a box-filtered smear of the analytic width on both backends; segments = 1 equals the unblurred render; a bench on a 1M-triangle deforming mesh at 8 buckets is recorded in the report.
  - size: L

**Verification gate:** full build and ctest green on both backends; the viewer refines progressively under Xvfb (GUI script); a motion-blurred render is shared with the user.

## Phase 283.3: Continuous-time motion blur (UAT round 1)

- [x] M283.P3.T1 — End-points in the scene and a time per ray
  - files: `Engine/Nodes/Scene/Utility/UtilityScene.{h,cpp}`, `Engine/Nodes/Scene/Utility/Wavefront.{h,cpp}`, `Engine/Nodes/Scene/Utility/Kernels/UtilityTypes.slang`, `Engine/Nodes/Scene/Utility/Kernels/RayGen.slang`, `Engine/Nodes/Scene/Utility/Kernels/ShadeSurface.slang`, `Engine/Nodes/Scene/Utility/Kernels/ShadeLights.slang`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.{h,cpp}`, `Tests/Scene/NatronUtilityMotion_Test.cpp`
  - approach: `UtilityScene::Snapshot` carries `segments + 1` end-point transforms per instance and end-point point arrays per deforming geometry (static ones keep one), replacing `evaluateAt`'s per-bucket snapshots. The ray arrays gain a ninth, `time` (shutter-relative 0..1), stratified per pixel sample with the existing golden-fraction order; bounce, shadow and light rays inherit it. Camera rays interpolate the camera's end-points at their time. Shading interpolates the hit's points and normals at the ray's time. Shutter Segments defaults to 1 and is not clamped; wavefrontDrawSamples draws one pass per sample, not per bucket.
  - verify: gtest — a quad translating one pixel per frame yields the analytic box smear within 1/N² at segments 1 on both backends; a rotating quad at segments 1 vs 8 differs only in the curve's chord error; a closed shutter equals the unblurred render.
  - size: L
- [x] M283.P3.T2 — Embree motion
  - files: `Engine/Nodes/Scene/Utility/EmbreeTracer.cpp`, `Tests/Scene/NatronUtilityTracer_Test.cpp`
  - approach: `rtcSetGeometryTimeStepCount(segments + 1)` with one vertex buffer per end-point for deforming meshes; instances with `rtcSetGeometryTimeStepCount` and `rtcSetGeometryTransformQuaternion` per end-point (linear translation and scale, slerped rotation, matching Phase 2's decomposition); `RTCRay::time` from the ray's time; scene flags keep `RTC_BUILD_QUALITY_MEDIUM` for motion BVHs.
  - verify: gtest — a ray at t = 0.25 hits a translating quad at the interpolated position; static geometry gives identical hits with and without motion on.
  - size: M
- [x] M283.P3.T3 — Vulkan motion through swept bounds
  - files: `Engine/Nodes/Scene/Utility/GpuTracer.cpp`, `Engine/Nodes/Scene/Utility/Kernels/Intersect.slang`, `Engine/Gpu/GpuAccel.{h,cpp}`, `Tests/Scene/NatronUtilityTracer_Test.cpp`
  - approach: static instances and geometry stay hardware triangles. Per segment, a deforming mesh gets an AABB BLAS of each triangle's swept bounds over the segment, and the candidate shader intersects the triangle interpolated at the ray's time; a moving instance becomes an AABB instance of its swept world bounds over the segment, whose candidate transforms the ray by the instance's interpolated inverse at the ray's time and traces a one-instance top-level structure over the geometry's BLAS. A ray picks its segment's structures (one structure set per segment, each split into structures of at most 2^24 instances like the static scene).
  - verify: gtest — GPU hits match Embree's within float tolerance on the translating, rotating and deforming quads at random ray times; a static scene builds no AABB structures.
  - size: L

## Decisions

- 2026-11-01 — **True motion blur and antialiasing (user):** shutter sampling, multi-segment transforms and deformation, configurable pixel filter.
- 2026-11-01 — **Shutter from the camera, overridable (user).**
- 2026-11-01 — **Time-bucketed tracing on both backends:** Vulkan's KHR acceleration structures have no motion blur, so both backends rebuild per time bucket for identical results (Nuke's ScanlineRender also renders per time sample).
- 2026-11-14 — Claimed by bosley-native-nodes (user request).
- 2026-11-14 — Phase 1 implemented (03c49bfda). Only colour is filtered; depth and ids take the heaviest sample. The settings key is `pixelSamples`, so it can't be read as the viewer's cap. The viewer shows each completed sample pass. PM calls: no hard cap on Pixel Samples (no-hand-holding rule), only a soft slider maximum. Exact-match tests (Storm match, cancel, purpose) pin `pixelSamples=1`; goldens are re-recorded.
- 2026-11-14 — Phase 2 implemented (a4159f88b), F7 merged in. The shutter interval is in the engine settings (hashed; re-syncs prims), while camera motion goes per render so an animated camera doesn't rebuild the engine; the camera gets 9 samples across the shutter. Meshes and instancers sample over the shutter (linear points, decomposed transforms with shortest-arc rotation, shear ignored); a topology change inside the shutter keeps the mesh still and warns. A velocity scene index is registered for Utility. PM calls: Override Shutter and a Shutter Offset menu (Centred/Start/End/Custom + custom value) are accepted; Shutter Segments defaults to 8, clamped to the pixel's sample count; the viewer sets no shutter, so only node renders blur. Unverified: the velocity index's frame rate on non-24 fps stages. The 1M-triangle 8-bucket bench (`NATRON_UTILITY_MOTION_BENCH`) runs on the GPU host. Gating.
- 2026-11-14 — Gate on a4159f88b: 7 suites failed. Product: the sample pattern was never shuffled, so the 3x3 Blackman-Harris read ~0.11 low at edges (Kensler per-pixel shuffle in RayGen.slang); time buckets each covered half a pixel (the order now steps the grid by golden fractions). Tests: GPU film fixtures lacked a runtime hold; the cancel, golden and single-sample fixtures now pin `pixelSamples=1`; two statistical checks were tighter than any unbiased 9-sample estimate. PM call: filter importance sampling (equal weights, positions from the filter — worst edge error 0.09 vs 0.5 at 9 samples in a replica) is a follow-up, since it changes Phase 1's id rule. Re-gating.
- 2026-11-14 — Gate green on c1b14d0df (ctest both legs; natron_utility_viewer). PR #286 into F7 opened; Codex review running.
- 2026-11-14 — PR #286 review round 1 (Codex): 5 findings, all fixed (c8f694871). Readers get a per-pass published copy of the film (CPU copy-and-swap, GPU device copy), so the viewer never reads a pass mid-write; topology variation (counts, indices, holes) across the shutter installs the mesh static at mid-shutter with the warning; one segment draws at the frame time; Pixel Samples has no hard cap (refused only when the square overflows). PM call: a topology-varying mesh with blur on and one segment still sits at mid-shutter (the mesh doesn't know the segment count); recorded as a known edge. Re-gating; one round.
- 2026-11-14 — PR CI on ba098b912 timed out NatronUtilityCancel in both debug legs: its two hand-made engines rendered the new 3×3 default sample grid on a 4096 frame (stops still under 500 ms). They now pin one pixel sample (9b8bd45d6); PR CI rerunning.
- 2026-11-14 — PR CI on 9b8bd45d6: NatronUtilityCancel passes; task-graph leg failed only GraphScalingConnectTest (timeout, fixed by #284) and a CacheStressTest SEGFAULT at Python finalisation after all 16 cases passed (legacy leg clean). Re-run after #284 reaches F7; diagnose if it repeats.
- 2026-11-14 — Main (with #284) merged into F7 (9daa71dca; HydraRenderImpl convergence loop keeps F7's cancel-aware loop with main's three-argument RestartWatch::after) and F7 into this branch (75811c8e8); PR CI rerunning.
- 2026-11-21 — **UAT round 1 (user): continuous-time motion blur.** Interpolate between temporal samples by shutter segments; default 1 segment (previous frame to this one and this one to the next for a centred shutter); rays distributed continuously in time; transforms and deformation linear between segment end-points; a segments knob raises it; CPU and GPU. Supersedes the 2026-11-01 time-bucket decision and Shutter Segments' default of 8 and its clamp. Phase 3 added. Open for the user: (a) GPU route — swept-bounds AABB structures with in-shader interpolation (portable, exact, chosen) vs `VK_NV_ray_tracing_motion_blur` (native on NVIDIA only, AMD would still need the portable path) vs keeping time buckets on the GPU (cheap, but stepped and differs from the CPU); (b) "linear" transforms read as linear translation/scale with slerped rotation, since a linear matrix blend shrinks spinning objects; (c) lights stay at shutter centre.
- 2026-11-21 — **Reopened by F7 - Natron Utility Core's UAT (user):** interpolated shutter segments, default 1. **GPU route: swept-bounds structures with interpolation in the shader (user)**, portable to AMD and NVIDIA and matching the CPU. **Rotation: decompose and slerp (user)**: linear translation and scale, slerped rotation; lights stay at the shutter centre. Phase 283.3 is implemented on feature/f7-natron-utility-core.
- 2026-11-21 — Phase 283.3 in 6d081804a, gate pending. **PM calls:**
  - Bakes (UV unwrap) trace at the shutter centre, with no blur.
  - Segments are not capped by the sample count.
  - A camera projection change within the shutter is a linear frustum blend.
  - GPU motion structures rebuild on any instance change, with no refit.

  **Follow-ups:**
  - Deforming points read from host-visible memory: move them to device-local, with the F7 performance list's geometry-upload item.
  - Nested ray queries are unrun on real AMD or NVIDIA drivers: GPU-host check at UAT.

