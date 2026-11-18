# M433 - Overlay Point Maps

An engine API that maps single points through a node, in both directions, on the CPU at draft cost. It is what lets overlays follow the image through non-linear warps:
- **forward** (input → output) places an upstream node's overlay in a downstream view;
- **inverse** (output → input) maps pen events back.

Every native distorting node implements it, and a chain helper composes a path's maps. Nothing draws through it yet: M435 - Overlays Deform Through Warps wires it into the viewer, and M436 - Overlays On Projected Geometry and M437 - CardProject25D add their own maps.

This is deliberately not M153 - Distortion Concatenation's render-side windowed inverse map. That milestone is designed later and may reuse the per-node math written here (user, 2026-11-14).

Scouted 2026-11-14 on `origin/main`:
- Only a 3x3 `getTransform` exists (`Engine/EffectInstance.h:1815/1829`). It is implemented by Transform (`Engine/Nodes/Transform/Transform.cpp:452`), Resample-type `StagedImageEffect`s (`Engine/Nodes/Image/StagedImageEffect.cpp:701`) and CornerPin (`CornerPin.cpp:699`).
- Lens models already map points both ways: `LensModels.h` `Model::map(Direction, x, y, Sample*)`, `distort`, `undistort` and `jacobian`.
- SplineWarp has `SplineWarpMesh::evaluate` and `SplineWarpTps::evaluate`, both output → source.
- STMap (`STMap.h:107`) and IDistort (`IDistort.h:107`) warp by image data and have no point API.
- IWarp and ITransform are the Map-driven variable warps from M185 - Variable Warps.

## Scope

**In:**
- `EffectInstance::mapOverlayPoint(time, view, inputNb, direction, Point in, Point* out)`. It works in canonical coordinates and returns `eMapped`, `eIdentity`, `eUnmappable` (outside the domain, or no convergence) or `ePending` (data not ready).
- `overlayMapKind()`, returning none, matrix or non-linear, so a resolver can keep the cheap matrix path when nothing non-linear sits on it.
- The default implementation uses `getTransform` (draft) when it answers. Otherwise it returns identity, so every non-warping node passes points through unchanged.
- Implementations for every native distorting node:
  - LensDistortion and LensCorrect, through the models;
  - SplineWarp, through the mesh/TPS, which is the inverse direction directly, with forward by Newton;
  - STMap and IDistort, through an asynchronous low-resolution map fetch;
  - IWarp and ITransform, through the same async fetch of their Map input.
- The matrix nodes (Transform, CornerPin, ICornerPin, Reformat, Crop, Mirror, Position) use the default. Each is tested to confirm it.
- A shared numeric inverter: Newton with an analytic or finite-difference Jacobian, seeded from a coarse grid. It covers a node with only one direction.
- `OverlayMapChain`: built from a path of (node, inputNb, direction) legs, it maps points and polylines. It subdivides a segment adaptively when its mapped midpoint strays more than a viewport-pixel tolerance from the chord.

**Out:**
- The render-side windowed inverse map, Jacobians for filtering, and GPU maps: M153 - Distortion Concatenation.
- Viewer drawing, hit-testing and the per-vertex overlay pipeline: M435 - Overlays Deform Through Warps.
- Maps through 3D projection and rendering: M436 - Overlays On Projected Geometry.
- CardProject25D's map: M437 - CardProject25D.
- OFX plug-ins' non-linear warps: OFX has no point-map action. They pass through as identity, or as their `getTransform` matrix.
- A Python API for point maps: none.

**Edge cases:**
- STMap, IDistort, IWarp or ITransform map data not yet fetched: `ePending`, and the fetch starts. When it arrives the node emits an overlay-redraw request. The fetch never blocks the GUI thread.
- The map input is disconnected or errors: identity (the node passes its source through) or `eUnmappable`, whichever matches what the node renders in that state.
- Folding warps (SplineWarp, IDistort) with several preimages: the inverter returns the root nearest the seed. The chain seeds each point from its neighbour's result, so a polyline stays continuous.
- A point outside a lens model's domain: `eUnmappable`. The chain drops that vertex and breaks the polyline there.
- Motion-blurred warps: draft mode, centre of shutter, the same as today's matrix overlays.
- The map fetch scale is a proxy, at most 256 px on the long side. Point accuracy is bilinear at that scale. Unconfirmed: that is accurate enough for handles, with the scale raised in P2.T3 if a test shows visible drift.

**Assumptions:**
- An effect can request a render of one of its inputs at a reduced render scale from a background thread, through the existing tree-render or cache path, without a viewer. P2.T3 confirms this first and reports if it can't.

## Phase 433.1: API and chain

- [x] M433.P1.T1 — `mapOverlayPoint` API and default
  - files: `Engine/EffectInstance.{h,cpp}`, new `Engine/OverlayPointMap.{h,cpp}` (result enum, direction, `Point`), CMake source list, `Tests/OverlayPointMap_Test.cpp`
  - approach:
    - Add the virtual `mapOverlayPoint` and `overlayMapKind` with the default: call `getTransform_public` in draft mode at render scale 1. Convert pixel to canonical with the input's PAR as `ViewerTabPrivate::getOverlayTransform` does (`Gui/ViewerTabPrivate.cpp:163`). Apply the matrix for forward and its inverse for inverse.
    - A disabled node is identity.
    - Add an `overlayMapChanged` notification (a signal or a node-level callback the GUI can connect to) for `ePending` completion.
  - verify: gtest — Transform translate/rotate/scale, CornerPin and Reformat map a point forward and back to within 1e-9; Grade is identity; a disabled Transform is identity.
  - size: M

- [x] M433.P1.T2 — Numeric inverter and `OverlayMapChain`
  - files: `Engine/OverlayPointMap.{h,cpp}`, `Tests/OverlayPointMap_Test.cpp`
  - approach:
    - `invertNumerically(fn, target, seed)`: damped Newton with a finite-difference Jacobian. Max 20 iterations, tolerance 1e-6 canonical. Fall back to a coarse-grid seed search when the seed fails. Returns `eUnmappable` on failure.
    - `OverlayMapChain` holds legs. `mapPoint` folds the legs. `ePending` from any leg makes the whole result pending. `eUnmappable` ends it.
    - `mapPolyline(points, tolerancePx, viewportScale)` adaptively subdivides each segment, depth ≤ 8, when the mapped midpoint deviates by more than the tolerance. It splits the output at unmappable vertices and seeds each point from its predecessor.
  - verify: gtest — inverting an analytic barrel function matches its closed-form inverse within 1e-6; a chain Transform → analytic warp → Transform round-trips; a straight line through a barrel warp subdivides into a curve within tolerance; an unmappable midpoint splits the polyline.
  - size: M

## Phase 433.2: Node implementations

- [x] M433.P2.T1 — LensDistortion and LensCorrect point maps
  - files: `Engine/Nodes/Transform/LensDistortion.{h,cpp}`, `Engine/Nodes/Transform/LensCorrect.{h,cpp}`, `Tests/OverlayPointMap_Test.cpp` or the nodes' existing test files
  - approach: build the node's `Model` at the time, exactly as render does, and map through `Model::map` in the right direction. Respect the node's undistort/redistort mode and its format normalisation. Map an invalid model or a failed map to `eUnmappable`.
  - verify: gtest — a known k1/k2 LensDistortion maps the frame corners to the positions its render puts a single bright pixel at (render a one-pixel dot, compare to within 0.5 px); forward ∘ inverse is identity within 1e-6.
  - size: M

- [x] M433.P2.T2 — SplineWarp point map
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.{h,cpp}`, `SplineWarpMesh.h`/`SplineWarpTps.h` (read-only use), the SplineWarp test file
  - approach:
    - Inverse (output → source) uses the cached mesh/TPS `evaluate` for the time, built the same way the render builds it and reusing `SplineWarpCache`.
    - Forward uses `invertNumerically` seeded by the point itself.
    - Respect the mix knob (lerp to identity) and the node's distort/undistort direction.
  - verify: gtest — a one-pair SplineWarp moving a source curve by (+30,0) maps a point on the source curve to its dst within 1 px, inverse round-trips, and a mix of 0 is identity.
  - size: M

- [x] M433.P2.T3 — Async map fetch for image-driven warps
  - files: new `Engine/OverlayMapFetch.{h,cpp}`, `Engine/Nodes/Transform/STMap.{h,cpp}`, `IDistort.{h,cpp}`, `IWarp.{h,cpp}`, `ITransform.{h,cpp}`, CMake list, `Tests/OverlayPointMap_Test.cpp`
  - approach:
    - First confirm how to render an input at reduced scale from a worker thread (the tree-render entry point used by thumbnails or `Tests` renders). Stop and report if it isn't possible.
    - `OverlayMapFetch` caches, per (node, input hash, time, view), a ≤256 px float image of the map input. A worker fills it, and completion emits `overlayMapChanged`.
    - STMap's inverse is a bilinear UV lookup → canonical. Its forward is a coarse inverse grid: scatter each sample's source position and fill holes by nearest neighbour, then a Newton polish on the bilinear lookup.
    - IDistort: the same with the vector field (inverse = p + v(p)).
    - IWarp and ITransform: their Map-driven per-pixel parameters, sampled the same way, through their existing per-pixel maths.
    - Return `ePending` until the fetch is ready.
  - verify: gtest — an STMap fed a generated shift map (+10 px) maps points forward and back within 0.5 px after the fetch resolves; the first call returns `ePending`; an IDistort constant vector field maps exactly; the fetch runs off the calling thread (assert the thread id).
  - size: L

- [x] M433.P2.T4 — Matrix nodes confirmed by test
  - files: `Tests/OverlayPointMap_Test.cpp`
  - approach: for Transform, CornerPin, ICornerPin, Reformat, Crop, Mirror and Position, assert `overlayMapKind() == matrix` and that a point maps to where the node's render moves a one-pixel dot. Fix any node whose `getTransform` is missing or wrong in its own file, and name it in the report.
  - verify: the gtest passes.
  - size: M

Note: M435 - Overlays Deform Through Warps benchmarks how the chain is applied to overlay geometry (per vertex, a composed grid, or on the GPU) before building its pipeline. Keep `OverlayMapChain` able to sample itself onto a grid (a `sampleGrid(region, step)` helper is cheap to add in P1.T2), so that spike can use it.

**Verification gate:** `OverlayPointMap_Test` and the nodes' existing test suites pass, and the full ctest suite is green on a remote release build. This milestone needs no UAT, since nothing user-visible changes: it ships inside F25 - Overlay Flow.

## Decisions

- 2026-11-14 — Own point-map API now, separate from M153 - Distortion Concatenation (user, grill, after first asking for M153 first and then re-asking): overlays don't wait on the larger render-side change, and M153 may reuse the per-node math.
- 2026-11-14 — Image-driven warps map through an async low-res map fetch, unmapped with a cue until it arrives (user, grill): exact enough for handles and never stalls the viewer.
- 2026-11-17 — All six tasks' code is committed in the lane; the gate is running (OverlayPointMap_Test plus the node suites).
  - Deviations: LensCorrect round-trips within 1e-3, since lensfun works in single precision. SplineWarp's Mix scales the displacement. ICornerPin and ITransform report a matrix with Map disconnected and nonlinear with it connected. A Transform with mix < 1 or a mask maps as identity. Numeric inverses take an optional seed. The background map fetch counts itself finished only after the node is notified.
  - PM call: STMap's point map follows wrap and UV Alpha (no source where alpha is 0), as a follow-up in this lane after the gate.
- 2026-11-17 — Gate green (run 44212850995, 34f7ed8e9) after test fixes (IDistort and Reformat fixtures needed a format-sized source) and a production fix: offset-form resample ops (Crop, Position) report their pixel translation from getTransform, so overlays follow them (a visible change; correct). F25's branch fast-forwarded to main; PR #311 into F25, in review.
