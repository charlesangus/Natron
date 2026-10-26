# M234 - Camera Projection Nodes

Two projection nodes, at the user's request (2026-10-26 grill: "1 and 2, separate nodes"):
- **`USDProjectUV`** writes an `st` primvar by projecting mesh points through a camera. It
  is portable to any renderer, since the result is plain UVs with a `UsdPreviewSurface`
  live texture. It is per-vertex, so it warps on coarse geometry, as Nuke's UVProject does.
- **`USDProject3D`** is a live, per-fragment, perspective-correct camera projection of a
  node-graph image through a custom Storm glslfx material: Nuke's Project3D. It shows live
  in the Viewer's 3D view and in USDRender. Since glslfx is Storm-only, USDWrite exports it
  baked.

This replaces the 2026-09-18 roadmap's rule that projection is bake-only. Both nodes take
their images through M233 - Live Node Textures. Survey:
`PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`.

## Scope

**In:**
- `USDProjectUV`: scene input, camera path knob (a stage dropdown, as USDRender has), prim pattern, primvar name (default `st`), and a frame-hold knob for the camera. Authors `faceVarying` UVs.
- `USDProject3D`: scene input plus an Image input (the plate), camera path, prim pattern, frame hold, near/far clip on/off, back-face cull on/off, and a crop/outside mode (black or transparent). It authors a material whose glslfx surface samples the live texture at the fragment's projection through the camera, and binds it to the matched prims.
- An animated camera and an animated plate both follow the frame, or the frame hold.
- USDWrite bakes `USDProject3D` for export. It writes `USDProjectUV`-style UVs plus the live texture when the target is a card-like mesh. Otherwise it leaves the material Storm-only with a warning until M53 - Projected Texture Painting's UV bake lands.

**Out:**
- Occlusion (projection onto hidden faces) — none in v1 (Nuke's Project3D also projects through by default). M53 - Projected Texture Painting's bake has occlusion.
- Combining a projection with an existing material (Nuke's MergeMat) — none.
- MaterialX projection for other delegates — none.

**Edge cases:**
- The camera path doesn't resolve: node error naming the path, as USDRender does.
- Points behind the camera (`USDProjectUV`): their UVs are clamped and a node warning gives the count. `USDProject3D` with clipping on shows the outside mode there.
- The plate aspect differs from the camera aperture: map through the camera's aperture with USDRender's fit modes (a fit-mode knob, default fill).
- Matched prims without a mesh (xforms): no-op for UVProject; Project3D binds the material, and only gprims show it.
- `USDProjectUV` on a non-mesh gprim: a node warning names it.

**Assumptions:**
- Storm accepts a `UsdShadeShader` with a glslfx `sourceAsset` (`info:implementationSource = sourceAsset`, `info:glslfx:sourceAsset`) as a surface, can read world position in its fragment code, takes matrix/float inputs as material parameters, and samples a texture input that M233's resolver serves. M234.P2.T1 proves it before the node is built.

## Phase 234.1: USDProjectUV

- [ ] M234.P1.T1 — `USDProjectUV` node
  - files: `Engine/Nodes/Scene/USDProjectUV.{h,cpp,reg.inc}`, `Engine/AppManager.cpp`, CMake source list, `Tests/Scene/USDProjectUV_Test.cpp`
  - approach: for each matched mesh at each needed frame, transform points to world, then through the camera's view/projection (`GfCamera`, conformed per the fit-mode knob) to NDC, and map to 0–1. Author `primvars:<name>` as `faceVarying` (indexed by faceVertexIndices) as time samples when the mesh or the camera is animated. Reads use `SceneQueryPool`. The layer key covers the matched paths, the camera's values and the mesh's point hash.
  - verify: gtest — a camera-facing USDCard filling the frame gets UVs (0,0)…(1,1) at its corners; moving the camera changes them; an animated camera gives per-frame samples; a missing camera errors with the path.
  - size: M

## Phase 234.2: USDProject3D

- [ ] M234.P2.T1 — Spike: glslfx projection surface in Storm
  - files: a throwaway `tools/spikes/storm-projection/` program and `.glslfx`, the result recorded in this file's `## Decisions`
  - approach: hand-author a `.usda` with a card, a camera, and a material whose glslfx surface takes a `GfMatrix4d` world-to-camera-NDC parameter and a texture, projecting the fragment world position. Render it with `UsdImagingGLEngine` under the strict llvmpipe runner and on AMD/NVIDIA, with the texture from a `natron://` URI through M233's resolver. Record whether parameters animate (time samples), whether world position is available in the surface stage, and the GLSL entry points used. If Storm rejects it, stop and put the fallback (projection in USDRender's own pass) to the user.
  - verify: recorded screenshots per driver show the texture projected perspective-correctly onto a tilted card.
  - size: M

- [ ] M234.P2.T2 — `USDProject3D` node and its glslfx shader
  - files: `Engine/Nodes/Scene/USDProject3D.{h,cpp,reg.inc}`, `Engine/Nodes/Scene/shaders/natronProject3D.glslfx` (installed beside `plugInfo.json`), `Engine/AppManager.cpp`, CMake install rules and bundle staging, `Tests/Scene/USDProject3D_Test.cpp`
  - approach: author a `UsdShadeMaterial` with the glslfx shader per the spike. Parameters: camera matrix (time-sampled per frame, or held), clip on/off with near/far, cull on/off, outside mode, fit mode. The texture input is the plate as a `natron://` live URI (M233.P2.T1's authoring helper, reused). Bind it to the matched prims. Register the plate URIs so M233.P2.T2's pre-render covers them.
  - verify: gtest — the material, shader asset path, bindings and camera samples compose as expected; strict-GL test — a ColorBars plate projected through a camera onto a tilted card renders matching a committed reference; with cull on, a back-facing card is empty.
  - size: L

- [ ] M234.P2.T3 — USDWrite bakes projections for export
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: on export, replace each USDProject3D material with a `UsdPreviewSurface` whose diffuse is the exported plate (M233.P2.T3) and author projected UVs on the bound meshes with `USDProjectUV`'s math, as time samples. Warn when the bound mesh is too coarse (fewer than N faces across the projected area) that the result warps.
  - verify: gtest — an exported card projection opened through USDRead and rendered with USDRender matches the live USDProject3D render within tolerance on a finely subdivided card.
  - size: M

**Verification gate:** release gate green; strict-GL projection tests pass; on real hardware a tracked camera from an `.abc` projects a plate sequence onto cards and a sphere live in the 3D view and in USDRender; screenshots shared before sign-off.

## Decisions

- 2026-10-26 — **Both projection styles, separate nodes (user, M19/M20 UAT grill):** a live Storm projection shader (USDProject3D) and portable projected UVs (USDProjectUV). Supersedes the 2026-09-18 roadmap's "projection is a bake, never a live shader".
