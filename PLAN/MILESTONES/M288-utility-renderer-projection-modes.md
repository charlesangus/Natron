# M288 - Utility Renderer Projection Modes

Add ScanlineRender's non-camera projections to Natron Utility: spherical (lat-long),
cylindrical, and a UV-unwrap mode that bakes one prim's fully shaded result — lighting,
shadows, reflections and AOVs — into its UV space; plus overscan in pixels for every mode.
M53 - Projected Texture Painting's `SceneRaster` stays the geometry and projection bake; this
is the shaded bake.

## Scope

**In:**
- `Projection` render setting: `Camera` (default — perspective or orthographic from the USD camera), `Spherical` (360×180 lat-long around the camera position, camera orientation), `Cylindrical` (360° horizontal, vertical FOV from the camera), `UV Unwrap`.
- UV Unwrap: settings `UV Prim` (path), `UV Primvar` (default `st`), `UDIM Tile` (default 1001); each texel launches a ray from just above the surface point back along the normal (so shading sees the surface exactly as a camera hit), and all AOVs are available.
- `Overscan` (pixels) widens the frame on every side for Camera, Spherical and Cylindrical; USDRender's output RoD grows to match.
- Motion blur and subdivision apply in every mode.

**Out:**
- Geometry/projection bakes (position, normal, coverage, plate projection with occlusion) — M53 - Projected Texture Painting.
- Stereo and VR stereo projections — none.
- Fisheye and lens-distortion projections — none.

**Edge cases:**
- UV Unwrap on a prim without the primvar: node error naming prim and primvar.
- Overlapping UV islands: later triangles overwrite (documented, as M53's bake does).
- UV Unwrap edge padding: texels outside the layout stay empty; dilation is left to 2D nodes (an `Edge Dilation` setting is a possible follow-up, not in scope).
- Spherical at the poles: rays computed per texel centre; no pinching artefacts beyond the lat-long map's own.
- Overscan with deep output: deep covers the overscanned area too.

**Assumptions:**
- USDRender's output region can grow beyond the project format by the overscan amount with its existing RoD logic.

## Phase 288.1: Projections

- [ ] M288.P1.T1 — Spherical and cylindrical ray generation with overscan
  - files: `Engine/Nodes/Scene/Utility/Kernels/RayGen.slang`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.cpp`, `Engine/Nodes/Scene/USDRender.cpp`, `Tests/Scene/NatronUtilityProjection_Test.cpp`
  - approach: RayGen switches on the Projection setting; spherical maps pixel → (longitude, latitude) in camera space, cylindrical → (angle, height); overscan offsets pixel coordinates and USDRender enlarges its RoD by the overscan. Register Projection and Overscan.
  - verify: gtest — a camera inside a cube with six coloured faces renders the expected face colours at the lat-long's analytic positions; cylindrical wraps 360°; 16 px overscan grows the output by 32 px in each dimension with the centre unchanged.
  - size: M

- [ ] M288.P1.T2 — UV Unwrap ray generation
  - files: `Engine/Nodes/Scene/Utility/UtilityUvUnwrap.{h,cpp}`, `Engine/Nodes/Scene/Utility/Kernels/RayGenUv.slang`, `Engine/Nodes/Scene/Utility/UtilityRenderPass.cpp`, `Tests/Scene/NatronUtilityProjection_Test.cpp`
  - approach: rasterise the prim's triangles in UV space (CPU, reuse `HdMeshUtil` face-varying remap) into a texel → (triangle, barycentrics) map for the chosen tile; RayGenUv emits for each covered texel a ray starting just off the surface along the normal and aimed back at it, so the existing shading and AOVs run unchanged.
  - verify: gtest — a lit quad with full 0–1 UVs bakes the same shading as a head-on camera render; a quad in tile 1002 bakes empty at 1001 and full at 1002; a missing primvar is a node error.
  - size: L

**Verification gate:** full build and ctest green on both backends; a lat-long environment render and a UV bake are shared with the user.

## Decisions

- 2026-11-01 — **Spherical, cylindrical and UV unwrap (user);** M53's SceneRaster keeps the geometry/projection bake.
