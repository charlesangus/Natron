# M279 - Utility Renderer Foundation

Stand up **Natron Utility**, Natron's own Hydra render delegate, as a CPU wavefront ray tracer
on Embree 4. It renders meshes (subdivision cages unrefined), PointInstancers and native
instances through any USD camera, with flat unlit `displayColor`, plus `depth` and `primId`.
It appears in USDRender's Renderer menu and the 3D viewer's renderer dropdown through M248 -
Render Delegate Selection's registry. The milestone fixes the architecture every later E19 -
Utility Renderer milestone builds on: the wavefront stages, the intersection backend interface
and the shared Slang shading kernels compiled to C++. Project-wide design:
`PLAN/DECISIONS/2026-11-01-natron-utility-renderer.md`.

## Scope

**In:**
- Delegate classes: renderer plug-in, render delegate, render pass, render buffer, mesh rprim, instancer, camera sprim (fallback sprims/bprims as Hydra requires).
- In-process registration: display name "Natron Utility", plug-in id `HdNatronUtilityRendererPlugin`, plugInfo priority above 0 so Hydra's own default stays Storm.
- Scene sync from Hydra dirty bits: per-mesh geometry rebuilt only when dirty; instance transforms refit; the top-level structure rebuilt when membership or transforms change.
- Wavefront pipeline on CPU: ray generation (Slang, C++ target) → intersection (Embree) → shading (Slang, C++ target) → film write.
- AOVs `color` (flat unlit displayColor × displayOpacity, premultiplied), `depth` (USDRender's existing `depth.Z` convention), `primId` / `instanceId` (USDRender's existing primId hash mapping).
- Embree runs inside the scheduler's thread budget, not a private pool.
- Selectable in the 3D viewer from this milestone on (user, 2026-11-01).
- USD purpose: the render pass draws only rprims whose render tag (`geometry`, `render`, `proxy`, `guide`) is in the pass's render tags, so the viewer's M229 purpose toggles work with Natron Utility. USDRender gains `Purposes` toggles (render on, proxy off, guide off by default — USD's final-frame convention) that set the engine's purpose params for every delegate (user, 2026-11-01).

**Out:**
- GPU path and device selection — M280 - Utility Renderer GPU Path.
- Materials, textures, lights — M281 - Utility Renderer Materials And Lights.
- Pixel samples, filters, motion blur — M283 - Utility Renderer Sampling And Motion Blur (this milestone shoots one centred ray per pixel).
- Subdivision refinement — M284 - Utility Renderer Subdivision.
- BasisCurves, Points, volumes — none (a later curves/points milestone, user 2026-11-01).

**Edge cases:**
- Empty scene or no camera: transparent black, with the same camera error USDRender already gives other delegates.
- Degenerate faces, n-gons, holes (`holeIndices`), left-handed orientation: triangulated through `HdMeshUtil`; degenerate triangles dropped; orientation honoured for the geometric normal.
- Mesh with no `displayColor`: mid-grey (0.18), matching Storm's fallback.
- Invisible prims (`visibility` invisible) are not built; toggling visibility only flips instance membership.
- Render and proxy both enabled on a model with both: both draw (USD semantics; the user asked for it).
- Purpose toggled: only render-tag membership changes; no geometry rebuild.
- Edit during a render: Hydra's sync happens before the next render; the in-flight render is aborted through the existing request abort.

**Assumptions:**
- A renderer plug-in whose types are compiled into a Natron library can be registered with a plugInfo.json found through `RenderDelegates::registerPluginDirectory` (M233's `Engine/Nodes/Scene/plugInfo.json` already registers in-process ArResolver/HioImage types this way). Verify against the pinned USD's `PlugRegistry` source and the M233 registration.
- Embree 4 is installed in the natron-dev and published images (M249's `tools/ci/local/build-cycles.sh` checks `libembree4`) and can be staged into the AppImage.
- `UsdImagingGLEngine` with `gpuEnabled = false` (`HydraRenderImpl.cpp`) drives a third-party delegate whose `Map()`ped buffers are CPU memory — as for Cycles and MoonRay.

## Phase 279.1: Delegate skeleton

- [x] M279.P1.T1 — Build wiring for the delegate and Embree
  - files: `Engine/Nodes/Scene/Utility/CMakeLists.txt` (new), `Engine/CMakeLists.txt`, `cmake/NatronUsd.cmake`, the AppImage staging script, `Engine/Nodes/Scene/plugInfo.json`
  - approach: new source directory `Engine/Nodes/Scene/Utility/` compiled into the Engine library (pxr types stay inside `Engine/Nodes/Scene/`). `find_package(embree 4 REQUIRED)` and link. Add the renderer plug-in type entry (`HdRendererPlugin` base, displayName "Natron Utility", priority 50) to the existing plugInfo.json, read the pinned USD's `plug/` source for how an in-process library entry is declared, and stage `libembree4` into the AppImage. Cite the headers used.
  - verify: the tree configures and builds with an empty plug-in class; `ldd` on the built Engine library shows `libembree4`; the AppImage staging step lists it.
  - size: M

- [x] M279.P1.T2 — Renderer plug-in, render delegate and render buffer
  - files: `Engine/Nodes/Scene/Utility/UtilityRendererPlugin.{h,cpp}`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.{h,cpp}`, `Engine/Nodes/Scene/Utility/UtilityRenderBuffer.{h,cpp}`, `Tests/Scene/NatronUtility_Test.cpp`
  - approach: implement the pure virtuals of `HdRendererPlugin`, `HdRenderDelegate` (supported Rprim `mesh`, Sprim `camera` plus whatever fallbacks Hydra requires, Bprim `renderBuffer`) and `HdRenderBuffer` (float/int formats as host memory, `Map`/`Unmap`/`Resolve`/`IsConverged`). Model the lifecycle on hdEmbree's delegate from the pinned USD source and cite it. `GetDefaultAovDescriptor` serves `color`, `depth`, `primId`, `instanceId`.
  - verify: gtest — `RenderDelegates::list()` contains "Natron Utility" as loadable; `HdRendererPluginRegistry::GetDefaultPluginId()` is still Storm; creating and destroying the delegate leaks nothing under the test's allocation counters.
  - size: L

- [x] M279.P1.T3 — Mesh rprim, instancer and camera sync
  - files: `Engine/Nodes/Scene/Utility/UtilityMesh.{h,cpp}`, `Engine/Nodes/Scene/Utility/UtilityInstancer.{h,cpp}`, `Engine/Nodes/Scene/Utility/UtilityScene.{h,cpp}`, `Tests/Scene/NatronUtility_Test.cpp`
  - approach: `UtilityMesh::Sync` triangulates with `HdMeshUtil` (holes, orientation, n-gons), pulls `points`, `displayColor`, `displayOpacity` and the transform, and rebuilds only on dirty points/topology. `UtilityInstancer` flattens nested PointInstancer and native-instance transforms per prototype. `UtilityScene` owns the per-mesh geometry records and instance list and exposes a version that bumps on any change; it knows nothing about Embree.
  - verify: gtest on hand-authored `.usda` — a PointInstancer of 3 cubes yields 3 instances of one geometry record; editing one point dirties only that mesh; hiding a prim drops its instance and nothing is rebuilt.
  - size: L

## Phase 279.2: Wavefront core on CPU

- [x] M279.P2.T1 — Intersection backend interface and Embree implementation
  - files: `Engine/Nodes/Scene/Utility/Tracer.h`, `Engine/Nodes/Scene/Utility/EmbreeTracer.{h,cpp}`, `Tests/Scene/NatronUtilityTracer_Test.cpp`
  - approach: `Tracer` takes a `UtilityScene` version and builds or refits its structures (per-geometry BLAS, instance-level TLAS), then intersects a ray batch (SoA origin/dir/tmin/tmax) into a hit batch (t, geometry id, instance id, primitive id, barycentrics) and answers occlusion batches. Embree implementation: one `RTCScene` per geometry plus an instance scene; `rtcIntersect16`/streams over the batch; refit on dirty points only. The arena runs on the scheduler's TBB pool with its thread budget.
  - verify: gtest — rays at a unit quad hit with the expected t and barycentrics; an instance offset moves the hit; editing points refits without rebuilding the instance scene (counter).
  - size: L

- [x] M279.P2.T2 — Shared Slang kernels for ray generation and flat shading
  - files: `Engine/Nodes/Scene/Utility/Kernels/RayGen.slang`, `Engine/Nodes/Scene/Utility/Kernels/ShadeFlat.slang`, `Engine/Nodes/Scene/Utility/Kernels/UtilityTypes.slang`, `Engine/Nodes/Scene/Utility/CMakeLists.txt`
  - approach: declare both through `slang_add_kernel(... CPP)` (`cmake/SlangKernels.cmake`) so each compiles to SPIR-V and C++. RayGen builds primary rays from the camera frustum USDRender already passes (centred sample per pixel). ShadeFlat reads a hit batch and the scene's per-geometry colour buffers and writes color (premultiplied displayColor × displayOpacity), depth (USDRender's `depth.Z` convention, read from `USDRender.cpp`) and primId/instanceId. No intersection inside Slang.
  - verify: both kernels compile to SPIR-V and C++ in the build; a gtest drives the C++ RayGen for a 4×4 frame and checks corner ray directions against the camera frustum.
  - size: M

- [x] M279.P2.T3 — Render pass driving the wavefront on CPU
  - files: `Engine/Nodes/Scene/Utility/UtilityRenderPass.{h,cpp}`, `Engine/Nodes/Scene/Utility/WavefrontCpu.{h,cpp}`, `Tests/Scene/NatronUtility_Test.cpp`
  - approach: `_Execute` resolves the camera and AOV bindings, then runs RayGen (C++) → `EmbreeTracer` → ShadeFlat (C++) over tiles in parallel and writes the bound render buffers. Honour the abort flag between tiles. Keep the stage loop backend-agnostic so M280 - Utility Renderer GPU Path swaps the executor.
  - verify: gtest renders a red unit cube through the delegate directly (no Natron node) and reads `color`, `depth`, `primId` from the buffers: the centre pixel is red with depth equal to the analytic distance, corners transparent.
  - size: L

## Phase 279.3: Host wiring

- [ ] M279.P3.T1 — USDRender and viewer through Natron Utility
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/RenderDelegates.cpp`, `Tests/Scene/USDRender_Test.cpp`, `Engine/Nodes/Scene/docs/USDRender.md`
  - approach: confirm the M248 non-Storm path picks the delegate up unchanged; fix only what it needs (AOV names, primId mapping through `primPathHash`). Document Natron Utility in the USDRender doc's renderer list.
  - verify: gtest — USDRender with Renderer = Natron Utility on `Tests/fixtures/scene-reference.usda` produces `rgba`, `depth.Z`, `primId.id`; `depth.Z` and `primId.id` match the Storm render (`USDRenderStorm_Test`'s reference) on interior pixels away from silhouettes, within 1e-4 relative depth.
  - size: M

- [ ] M279.P3.T3 — Purpose through render tags and USDRender toggles
  - files: `Engine/Nodes/Scene/Utility/UtilityRenderPass.cpp`, `Engine/Nodes/Scene/Utility/UtilityScene.{h,cpp}`, `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}`, `Engine/Nodes/Scene/USDRender.cpp`, `Tests/Scene/NatronUtilityPurpose_Test.cpp`
  - approach: the render pass reads its render tags and builds the instance list only from rprims whose `GetRenderTag()` is included (cite how hdEmbree / HdSt filter by render tags in the pinned USD); changing tags only rebuilds the instance list. Add `HydraRender::Request` purpose flags passed to the engine's render params (`showRender`/`showProxy`/`showGuides`), hashed into the render key. USDRender gets `renderPurpose` ("Render Purpose", on), `proxyPurpose` ("Proxy Purpose", off), `guidePurpose` ("Guide Purpose", off). Read how M229 sets the viewer's purposes and reuse it.
  - verify: gtest on a `.usda` with a default, a render, a proxy and a guide prim — USDRender with Natron Utility at defaults shows default + render only; toggling proxy shows the proxy; the same toggles change Storm's output too; toggling a purpose leaves the geometry rebuild counter unchanged.
  - size: L

- [ ] M279.P3.T2 — Golden and viewer GUI check
  - files: `Tests/Scene/NatronUtilityGolden_Test.cpp`, `Tests/fixtures/native-golden/NatronUtility/` (≤256 px half-float crops), `Tests/gui/natron_utility_viewer.py`
  - approach: record a golden of the reference scene's color/depth/primId through the CPU path (crop per the LFS rule). A GUI script selects Natron Utility in the 3D viewer's dropdown, renders, and screenshots.
  - verify: golden test passes; the GUI script passes under the strict Xvfb runner and its screenshot shows the scene, and toggling the viewer's render purpose shows a render-purpose prim; screenshot shared with the user.
  - size: M

**Verification gate:** full build and ctest green (including the new NatronUtility tests and the unchanged Storm tests); the GUI script passes under Xvfb; the user has seen the viewer screenshot.

## Decisions

- 2026-11-01 — **Meshes plus instancing first (user):** PointInstancer and native instances are in; BasisCurves and Points come in a later milestone.
- 2026-11-01 — **Name "Natron Utility" (user).**
- 2026-11-01 — **In the viewer from the start (user):** selectable in the 3D viewer's dropdown from this milestone.
- 2026-11-01 — **Flat unlit stopgap:** the foundation shades flat displayColor; M281 - Utility Renderer Materials And Lights replaces it with lit shading, which is black without lights (user, 2026-11-01).
- 2026-11-01 — **The renderer handles purpose (user):** render tags honoured in the pass; USDRender gets purpose toggles defaulting to render-purpose only.
- 2026-11-01 — **Incremental sync:** only dirty meshes rebuild their structures, per the long-lived-scene principle (2026-10-25 review).
