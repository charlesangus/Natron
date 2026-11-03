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

- [x] M234.P1.T1 — `USDProjectUV` node
  - files: `Engine/Nodes/Scene/USDProjectUV.{h,cpp,reg.inc}`, `Engine/AppManager.cpp`, CMake source list, `Tests/Scene/USDProjectUV_Test.cpp`
  - approach: for each matched mesh at each needed frame, transform points to world, then through the camera's view/projection (`GfCamera`, conformed per the fit-mode knob) to NDC, and map to 0–1. Author `primvars:<name>` as `faceVarying` (indexed by faceVertexIndices) as time samples when the mesh or the camera is animated. Reads use `SceneQueryPool`. The layer key covers the matched paths, the camera's values and the mesh's point hash.
  - verify: gtest — a camera-facing USDCard filling the frame gets UVs (0,0)…(1,1) at its corners; moving the camera changes them; an animated camera gives per-frame samples; a missing camera errors with the path.
  - size: M

## Phase 234.2: USDProject3D

- [x] M234.P2.T1 — Spike: glslfx projection surface in Storm
  - files: a throwaway `tools/spikes/storm-projection/` program and `.glslfx`, the result recorded in this file's `## Decisions`
  - approach: hand-author a `.usda` with a card, a camera, and a material whose glslfx surface takes a `GfMatrix4d` world-to-camera-NDC parameter and a texture, projecting the fragment world position. Render it with `UsdImagingGLEngine` under the strict llvmpipe runner and on AMD/NVIDIA, with the texture from a `natron://` URI through M233's resolver. Record whether parameters animate (time samples), whether world position is available in the surface stage, and the GLSL entry points used. If Storm rejects it, stop and put the fallback (projection in USDRender's own pass) to the user.
  - verify: recorded screenshots per driver show the texture projected perspective-correctly onto a tilted card.
  - size: M

- [x] M234.P2.T2 — `USDProject3D` node and its glslfx shader
  - files: `Engine/Nodes/Scene/USDProject3D.{h,cpp,reg.inc}`, `Engine/Nodes/Scene/shaders/natronProject3D.glslfx` (installed beside `plugInfo.json`), `Engine/AppManager.cpp`, CMake install rules and bundle staging, `Tests/Scene/USDProject3D_Test.cpp`
  - approach: author a `UsdShadeMaterial` with the glslfx shader per the spike. Parameters: camera matrix (time-sampled per frame, or held), clip on/off with near/far, cull on/off, outside mode, fit mode. The texture input is the plate as a `natron://` live URI (M233.P2.T1's authoring helper, reused). Bind it to the matched prims. Register the plate URIs so M233.P2.T2's pre-render covers them.
  - verify: gtest — the material, shader asset path, bindings and camera samples compose as expected; strict-GL test — a ColorBars plate projected through a camera onto a tilted card renders matching a committed reference; with cull on, a back-facing card is empty.
  - size: L

- [x] M234.P2.T3 — USDWrite bakes projections for export
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: on export, replace each USDProject3D material with a `UsdPreviewSurface` whose diffuse is the exported plate (M233.P2.T3) and author projected UVs on the bound meshes with `USDProjectUV`'s math, as time samples. Warn when the bound mesh is too coarse (fewer than N faces across the projected area) that the result warps.
  - verify: gtest — an exported card projection opened through USDRead and rendered with USDRender matches the live USDProject3D render within tolerance on a finely subdivided card.
  - size: M

**Prior verification gate:** release gate green; strict-GL projection tests pass; on real hardware a tracked camera from an `.abc` projects a plate sequence onto cards and a sphere live in the 3D view and in USDRender; screenshots shared before sign-off.

## Phase 234.7: UAT round 3 fixes (user, 2026-11-02)

- [ ] M234.P7.T1 — Crop outside the frame is transparent black
  - files: USDProject3D / projection shader and its crop path, projection tests
  - approach: the "crop" setting must give alpha 0 (and black RGB) outside the projected frame; today alpha is 1 there.
  - verify: a test projects with crop on and reads alpha 0 outside the frame, 1 inside.
  - size: M
- [ ] M234.P7.T2 — Camera Path resolves lazily on the composed stage
  - files: USDProject3D camera lookup, the material/shader binding that consumes it, projection tests
  - approach: per the spec, the camera path is resolved when the stage the viewer/render sees is composed, not in the projection node's own upstream scene. Texture → USDProject3D (Camera Path /USDCamera1) → USDCard, merged in USDMerge with USDCamera1, viewed downstream: must project cleanly with no "(missing)", no node warning. Only warn when the final composed stage lacks the prim.
  - verify: a test builds exactly that chain and checks the projection renders the texture (not black/white-alpha) and the node carries no warning.
  - size: L
- [ ] M234.P7.T3 — A disabled node feeding a live texture still projects
  - files: live-texture source resolution (shared with M233), projection tests
  - approach: when the node directly above USDProject3D (or any live-texture consumer) is disabled, follow the disabled node's pass-through input as the render does instead of failing.
  - verify: a test disables the node above the projection and checks the projected texture equals its input's image.
  - size: M
- [ ] M234.P7.T4 — No edge stretching outside the data window
  - files: live-texture upload (data window / format handling), projection shader sampling, tests
  - approach: checkerboard → Crop (black outside) → Transform y 250 (black outside) → USDProject3D shows a black border at the bottom in 2D, but in 3D the checker edge stretches: the texture is clamp-to-edge sampled or the data window is dropped. Upload/sample so pixels outside the data window are black (clamp-to-border transparent), consistent with the 2D viewer.
  - verify: a test renders that chain in 3D and reads black in the band the 2D viewer shows black.
  - size: M

## Decisions

- 2026-10-28: Projection Space Geometry: the shader takes the fragment to object space (ApplyInstanceTransformInverse) then applies a geometryFrame matrix the geometry node writes on its grafted material copy (held frame with Use Frame Hold, recorded as natron:frameHold; time-sampled otherwise). Overscan: a plateTransform (scale, offset) maps the camera frame onto the format inside the RoD-sized live texture; Crop on shows Outside beyond the frame, Crop off samples overscan then extends edges. USDWrite bakes both; with Crop on and overscan the export shows the overscan and warns. USDProjectUV unchanged (no image input). Needs a CI GL run to confirm Storm exposes the transform-inverse helpers in surface shaders.
- 2026-10-28: P2.T3 implemented (build pending). USDWrite bakes before asset paths are rewritten: each mesh bound to a USDProject3D material (found by its glslfx sourceAsset) gets indexed faceVarying `st` through the stored worldToProjector (per frame when the camera or mesh varies), as overs in the root written layer. The material's surface becomes an **unlit** UsdPreviewSurface (diffuse black, emissive ← the existing PlateTexture's rgb, so the plate exports through M233), opacity ← a with threshold 0.5 when Outside is Transparent; the glslfx shader is deactivated. Coarseness warning by perspective error, not face count: a face whose projective depth varies >1% across it warns that the bake warps. A material also bound to a non-mesh gprim stays Storm-only with a warning. Open: emissive vs the spec's "diffuse" (picked emissive to match the unlit live look); the "renders like the live projection through USDRender" gtest needs Storm on CI and is not written.
- 2026-10-28: P2.T2 implemented (build pending). Inputs Image (0) and Camera (1), output a material for a geometry node's Material input. Knobs: prim path, Camera (dropdown of the Camera input's cameras), Project On (Both/Front/Back), Crop (on), Clip Near Far (off), Outside (Black/Transparent), Fit Mode (Fill), Use Frame Hold, Frame Hold. Unlit surface. Plate = UsdUVTexture (wrap black when Crop, else clamp) feeding a float4 glslfx parameter; matrix and origin time-sampled per project frame when the camera varies, else one value. Shader `natronProject3D.glslfx` ships in Resources/usd/natronLiveTexture beside plugInfo.json (CMake copy + install; check-startup.sh checks it) and is found via that plug-in's FindPluginResource. Open to user: (1) Projection Space "Geometry" not implemented (World only); (2) live textures cover the plate's RoD, not its format, so overscan plates misalign; (3) plate alpha does not blend (opaque material); (4) keep the Camera dropdown alongside the input, as P1.
- 2026-10-28: P2.T1 spike passed on llvmpipe (USD 0.26.8, `tools/spikes/storm-projection/`, screenshots in build/m234-spike/). A glslfx `sourceAsset` surface works in Storm: per-fragment perspective-correct projection; world position via `GetWorldToViewInverseMatrix() * Peye`; matrix/float4/int parameters animate by time samples; Front/Back by `gl_FrontFacing != IsFlipped()` with a dFdx/dFdy normal, independent of doubleSided; transparent = discard. A texture parameter must be a connected UsdUVTexture (Storm then generates `HdGet_plate(vec2)`), whose st must be connected even though unused. Entry points: `surfaceShader` + `displacementShader`, `ApplyColorOverrides`. Not done: AMD/NVIDIA screenshots and a `natron://` plate in the spike (the node uses M233's URIs through the same UsdUVTexture file input) — CI/hardware.
- 2026-10-28: P1.T1 implemented (build pending). Camera source = optional Camera input plus a Camera dropdown (that input's cameras, else the Scene input's); Nuke UVProject controls ported except viewer-only display/selectable/render; Format + Fit Mode for Perspective; Use Frame Hold holds the camera only; no animatable knobs; points behind the camera use |w| and clamp; Planar/Spherical/Cylindrical without Nuke's 0.25 factor; out-of-scope vertices keep their old primvar or (0,0); output primvar indexed faceVarying. Open to user: keep the dropdown alongside the Camera input (recommended) or input only.
- 2026-10-26 — **Comp-first projection (user):** USDProject3D takes a Camera input (a scene, read where it is wired, as classic Nuke) and an Image input, in place of the camera path knob and prim pattern; its output is a material that feeds a geometry node's Material input (M226.P2.T4). The camera is stored as values (time-sampled matrices), never as a relationship, so grafting needs no repair. A **Projection Space** choice: World (default; the projection stays fixed and moving geometry slides through it) or Geometry (the camera is stored relative to the geometry where the projection is set up and the shader projects object-space positions, so downstream transforms carry the projection with the geometry). USDProjectUV keeps its scene input and pattern but also takes a Camera input.
- 2026-10-26 — **Both projection styles, separate nodes (user, M19/M20 UAT grill):** a live Storm projection shader (USDProject3D) and portable projected UVs (USDProjectUV). Supersedes the 2026-09-18 roadmap's "projection is a bake, never a live shader".

## Phase 234.3: UAT projection policy

- [x] M234.P3.T1 — Finish unlit alpha-blended projection output
  - files: USDProject3D glslfx/material authoring, USDWrite projection bake, projection tests
  - approach: Keep projection emissive/unlit, propagate plate alpha through live and baked paths, and blend transparent regions instead of using the prior opaque material.
  - verify: A semi-transparent plate matches in the Viewer and USDRender with and without authored lights, and exported output matches within tolerance.
  - size: L

- [x] M234.P3.T2 — Keep camera selection flexible and remove the warp warning
  - files: USDProjectUV/USDProject3D knobs and validation, node docs/tests
  - approach: Keep the Camera dropdown alongside the Camera input, allow a typed camera path that is currently absent but may resolve downstream/later, and remove the coarse-geometry warp warning from the user-facing path.
  - verify: A future camera path round-trips without an immediate hard error and resolves when that camera appears; the dropdown remains available.
  - size: M

- [ ] M234.P3.T3 — Support projected materials through material assignment
  - files: USDProject3D output contract, Assign Material node compatibility, scene/material tests
  - approach: Make the projection output a normal assignable material that can be connected through the material-assignment workflow as well as directly to a geometry node.
  - verify: One projected material is assigned to selected prims through Assign Material and renders identically to the direct connection.
  - size: M

**Verification gate:** strict-GL and hardware projection tests pass; emissive alpha, future camera paths and material assignment are accepted in the replacement AppImage.
- 2026-11-01 — P3 at 970cc4920: projection shader translucent (premultiplied plate), export bake unlit+blended; cameraPath knob decides the camera, dropdown fills it, a missing path is allowed with a warning; warp warning removed. P3.T3 partial: binding through M236-style collection binding tested; the real Assign Material render waits for M236. Risk: blended surfaces write no prim IDs, so picking projected surfaces may break; Project3D goldens may need re-recording (implementer).
- 2026-11-01 — Rebased onto M233's format-only textures (a13baa781): projections request a definition-covering variant (`natron://...&cover=definition`, keyed apart in the store), so overscan still projects with Crop off; materials keep format-only textures; USDWrite exports the variant as <node>_<layer>_definition.exr (PM decision, implementer).
- 2026-11-01 — The 3D viewport never applied colour correction (no renderer AOV, so HdxColorCorrectionTask skipped): now wired to the viewer's OCIO display/view (in progress). Storm doesn't highlight translucent surfaces: accepted for this milestone, filed as M278 - Translucent Selection Highlight (PM).

## Phase 234.6: Viewer gain and gamma in 3D

- [x] M234.P6.T1 — The 3D viewport follows the viewer's gain and gamma
  - files: `Gui/SceneViewport.*`, `Gui/ViewerTab*`, `Tests/SceneViewport_Test.cpp`
  - approach: User, 2026-11-02: the 3D viewport "should follow gain and gamma too". Apply the viewer's gain (exposure, before the display transform) and gamma (after it) in the 3D viewport exactly as the 2D viewer does, including the enable toggles, refreshing when they change. Fold them into the same baked colour path as the OCIO display transform where possible.
  - verify: A test sets gain and gamma on the viewer and checks the 3D card's pixel matches the 2D viewer's maths for the same input; toggling them off restores the plain transform.
  - size: M

- [ ] M234.P6.T2 — Gain and gamma on rendered pixels only, if cheap
  - files: `Gui/SceneViewport.*`
  - approach: User, 2026-11-02: applying gain and gamma to the 3D viewport's background "is not ideal, if it can be applied only to rendered items that would be preferred but it's not a huge deal". If Hydra's colour correction can be limited to rendered pixels (for example by keeping the background out of the corrected AOV and compositing it after, or by drawing the background after the colour-correction task) without a second full-screen pass that slows the viewport, do it; otherwise leave it and record why.
  - verify: With gain 2 the background pixel is unchanged and the card is doubled; viewport frame time is unchanged.
  - size: M
- 2026-11-02 — The 3D viewport ignores auto-contrast; the feature itself is to be removed (user) — M307 - Remove Viewer Auto-Contrast.
- 2026-11-02 — Audit: P6.T1 at 7f7d5b98b (gain before, gamma after the display transform, plus test fixes 5b76d198c, 26cf94ed0). P6.T2 not done: the viewport background is still graded with gain and gamma (setDisplayTransform runs it through the same processor), and no reason is recorded for leaving it. P3.T3 waits on M236 - Shared Materials And Assignment merging. Branch pushed, clean, no PR, 23 behind origin/main. It carries M233 only up to 300b0b546, so it lacks M233's P4/P5; per M233's 2026-11-02 note the projection must move to USDEmissiveMaterial once M233 is merged in (audit).
