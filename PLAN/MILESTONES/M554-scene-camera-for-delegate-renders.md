# M554 - Scene Camera For Delegate Renders

Natron never hands a Hydra delegate the scene camera. `HydraRender::Request` carries only a frustum (`Engine/Nodes/Scene/HydraRenderImpl.h:118`), and every render site calls `engine->SetCameraState(view, projection)` (`Engine/Nodes/Scene/HydraRenderImpl.cpp` ~:1567, ~:1682, ~:3699; `Gui/SceneViewport.cpp` ~:2812). Cycles and MoonRay therefore render through a free camera with no shutter and no depth-of-field values, so motion blur from a camera's `shutter:open`/`shutter:close` can't work in any Natron render. Pass the camera path through and call `UsdImagingGLEngine::SetCameraPath` plus `SetFraming`/`SetOverrideWindowPolicy` for camera renders, keeping `SetCameraState` for free-camera viewer navigation. Also support velocity motion blur on volumes (the VDB `velocity` grid, user 2026-11-23), and re-enable `VolumeRender{Cycles,MoonRay}.VelocityBlursTheVolume`, which M345 - VDB Rendering Fixes disabled for this reason (`Tests/Scene/VolumeRender_Test.cpp` on the F1 branch). Found by M345's gate (run 45155927523: blurred and still renders bit-identical on both renderers). Member of F1 - VDB Volumes.

## Scope

**In:**
- A camera path on `HydraRender::Request`, set by `USDRender` (`Engine/Nodes/Scene/USDRender.cpp` ~:1122, where `cameraFrustum` is computed from `cameraPath`), and by the viewer when it looks through a scene camera.
- `SetCameraPath` + framing + window policy for camera renders, at every site that renders through a camera; engine cache keys that include the camera path.
- Camera shutter motion blur in Cycles (`cycles:integrator:motion_blur` on) and MoonRay.
- Velocity-grid motion blur on volumes in Cycles and MoonRay.
- Re-enabling both `VelocityBlursTheVolume` tests.

**Out:**
- Free-camera viewer navigation: keeps `SetCameraState`.
- Storm motion blur and oversampling (M515 - Storm Oversample And Motion Blur).
- New camera knobs or lens authoring (M497 - Lens Schema And Camera Authoring).

**Edge cases:**
- A camera path that doesn't resolve to a `UsdGeomCamera` at render time: the render fails with an error naming the path, as a missing camera does today; no silent fallback to the frustum.
- The image framing must not move: a Storm render through the camera path matches the old frustum render pixel for pixel at every aperture fit and aspect `cameraFrustum` supports.

**Assumptions:**
- hdCycles and hdMoonray read shutter and DoF from the camera sprim when given a camera path (unverified; P2.T1 checks).
- At least one of hdCycles/hdMoonray already turns a volume's `velocity` field into motion, or can with a field binding authored on the volume prim (unverified; P2.T1 checks).

## Phase 554.1: Camera Path

- [ ] M554.P1.T1 — Carry the scene camera path on the render request
  - files: `Engine/Nodes/Scene/HydraRenderImpl.h` (`Request`), `Engine/Nodes/Scene/HydraRenderImpl.cpp` (engine cache entry / reuse key), `Engine/Nodes/Scene/USDRender.cpp` (~:1122)
  - approach: add `SdfPath camera` (empty = free camera) and the aperture fit that `cameraFrustum` used to `Request`, with comments in the style of the neighbouring fields. `USDRender` sets both next to the frustum it already computes. Add the camera path to the "same render" reuse test and to whatever decides whether an engine's view moved, so a camera switch never reuses a render. No behaviour change yet: render sites still call `SetCameraState`.
  - verify: the build passes; existing `ctest -R 'USDRender|HydraRender'` stays green
  - size: M
- [ ] M554.P1.T2 — Render through the camera path when the request names one
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (the three `SetCameraState` sites), `Tests/Scene/` (new or existing HydraRender test)
  - approach: when `request.camera` is set, call `engine->SetCameraPath(camera)`, `SetFraming` with a framing whose display window and data window match the current width/height, and `SetOverrideWindowPolicy` with the `CameraUtilConformWindowPolicy` matching the request's aperture fit, so the delegate conforms exactly as `cameraFrustum` did. Free-camera requests keep `SetCameraState`. Switching between the two on one engine must clear the other (`SetCameraPath(SdfPath())` before `SetCameraState`). Add a gtest rendering a small fixture camera in Storm both ways (frustum vs path) at each supported fit and two aspects, comparing pixels exactly.
  - verify: the new parity test passes; full `ctest -R Scene` green
  - size: L
- [ ] M554.P1.T3 — Viewer looks through a scene camera by path
  - files: `Gui/SceneViewport.cpp` (~:2812 and the look-through-camera code path)
  - approach: when the viewer is looking through a scene camera and renders with a delegate, pass the camera path as P1.T2 does; free navigation keeps `SetCameraState`. If the viewer has no look-through-camera mode, record that as a milestone decision and skip.
  - verify: a GUI script (or an extension of an existing `Tests/gui/` viewer camera script) looking through a camera in Storm matches the free-camera image at that camera's frustum
  - size: M

## Phase 554.2: Motion Blur

- [ ] M554.P2.T1 — Check how hdCycles and hdMoonray take shutter and volume velocity
  - files: read-only: the bundled hdCycles and hdMoonray sources (camera sprim sync, volume/field prims), `Tests/Scene/VolumeRender_Test.cpp` on `origin/feature/f1-vdb-volumes`
  - approach: consultant (opus) confirms both delegates read `shutter:open/close` from the camera sprim given `SetCameraPath`, which render settings turn on motion blur (`cycles:integrator:motion_blur`, MoonRay's equivalent), and how each turns a volume's VDB `velocity` grid into motion (field name, field binding on the `UsdVolVolume`, any scale/units setting). Name what Natron must author or set for each.
  - verify: a written answer with file:line in the delegate sources, recorded as a milestone decision
  - size: L
- [ ] M554.P2.T2 — Camera shutter motion blur in Cycles and MoonRay
  - files: `Engine/Nodes/Scene/USDRender.cpp` (delegate settings), `Tests/Scene/` (new motion-blur test)
  - approach: per P2.T1, turn the delegates' motion blur on when the camera's shutter interval is non-zero. Test: a cube translating over the shutter interval, rendered through a camera with an open shutter vs a zero shutter, in Cycles and in MoonRay at low samples; the two must differ along the motion direction. Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: the new test passes on both delegates in a remote debug build
  - size: M
- [ ] M554.P2.T3 — Velocity motion blur on volumes; re-enable the volume blur tests
  - files: per P2.T1 — likely the volume authoring in the F1 volume nodes and `Engine/Nodes/Scene/USDRender.cpp`; `Tests/Scene/VolumeRender_Test.cpp`
  - approach: author or set what P2.T1 named so a VDB's `velocity` grid blurs the volume in both delegates. Re-enable `VolumeRenderCycles.VelocityBlursTheVolume` and `VolumeRenderMoonRay.VelocityBlursTheVolume`, with `cycles:integrator:motion_blur` on for Cycles.
  - verify: both re-enabled tests pass (blurred and still renders differ)
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests` green, including the P1.T2 framing parity test, the P2.T2 shutter test and both `VelocityBlursTheVolume` tests on Cycles and MoonRay; PR CI into the F1 branch green or the PM's gate standing in for it (§9b).

## Decisions
- 2026-11-23 — Joins F1 - VDB Volumes (user): it re-enables tests that live on the F1 branch until F1 lands.
- 2026-11-23 — Volume velocity motion blur is in scope alongside the camera shutter fix (user): velocity blur on volumes is a standard way to blur them and must be supported.
- 2026-11-23 — Claimed ahead of its Deps at the user's request; works on top of the dependency's branch.
