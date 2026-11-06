# M251 - Cycles Rendering

USDRender renders a scene through Cycles' Hydra delegate and produces `rgba`, `depth.Z`,
`primId.id` and a `normal` layer as a normal 2D image, CPU only. Builds on M248 - Render
Delegate Selection (the knob and the engine) and M249 - Bundle Cycles Delegate (the
delegate). Wider AOV sets are decided after M255 - Delegate Output Spikes; settings knobs are
M253 - Delegate Render Settings.

## Scope

**In:**
- `Renderer = Cycles` on USDRender renders the composed stage from the chosen camera at the project format, to a fixed sample count taken from the delegate's default for now.
- Output layers `rgba`, `depth.Z`, `primId.id` (with the same hash → path metadata as Storm) and `normal` (user, 2026-10-27: full AOV set wanted; mechanism is M256 - Delegate AOV Selection).
- Scene-linear output; no tone mapping on the node.
- Render cancel on abort, with Cycles' threads released promptly.
- Cycles uses a thread count taken from Natron's scheduler budget, not all cores unconditionally.

**Out:**
- Other AOVs, light-path expressions — M256 - Delegate AOV Selection.
- Settings knobs — M253 - Delegate Render Settings.
- Progressive viewer — M254 - Progressive Viewer Render.
- Deep — M257 - Cycles Deep Output.
- GPU devices — none.

**Edge cases:**
- A prim type or material the delegate cannot render: one persistent node warning per kind, the rest renders.
- No camera on the stage: node error, as for Storm.
- Zero lights: renders black plus the dome/background; no injected headlight (Storm's headlight is Storm's).
- A stochastic render is not bit-exact; tests use a fixed seed and sample count and compare within a tolerance.
- Multi-view renders: one render per view, as Storm.

**Assumptions:**
- Unconfirmed: the Cycles delegate exposes `color`, `depth`, `primId` and `normal` render vars. Read the delegate source for its AOV list; if `primId` is absent, derive the layer from Cycles' object-ID pass and record it as a decision.
- Cycles' colour output is scene-linear Rec.709 (Cycles' working space); confirm against the delegate before wiring colour space metadata.

## Phase 251.1: Render path

- [x] M251.P1.T1 — Beauty and depth from Cycles
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/CyclesAovs.cpp`, `Engine/Nodes/Scene/CyclesAovs.h`
  - approach: with `Renderer = Cycles`, create the Cycles engine through M248's path, request `color` and `depth`, render to the delegate's converged state for the default sample count, read the buffers via `MappedAov`, convert depth to camera-space distance like Storm, write `rgba` and `depth.Z`.
  - verify: a unit test renders a unit cube with a fixed seed headless and gets non-black `rgba`, `depth.Z` equal to the camera distance at the centre pixel (within tolerance).
  - size: L

- [x] M251.P1.T2 — primId and normal layers
  - files: `Engine/Nodes/Scene/CyclesAovs.cpp`, `Engine/LayerRegistry.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`
  - approach: request `primId` and `normal` AOVs; reuse Storm's path-hash float and hash → path metadata map for `primId.id`; register a `normal` layer with its channel names. If the delegate lacks `primId`, derive it as noted in Assumptions.
  - verify: a two-cube scene gives two distinct `primId.id` values whose metadata map names the right prims; `normal` at the centre pixel is the face normal in camera space (within tolerance).
  - size: M

- [x] M251.P1.T3 — Cancel and thread budget
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/CyclesAovs.cpp`
  - approach: poll the Natron abort flag from the render loop and call the engine's stop; set the delegate's thread count from the scheduler's budget for the render's worker (see the task-graph scheduler decision). Verify the call names against the delegate's settings list.
  - verify: aborting a long render returns within a second and leaves no Cycles threads (checked via /proc thread count); two concurrent USDRender renders do not oversubscribe cores beyond the budget.
  - size: M

- [x] M251.P1.T4 — Reference render test (03271dd7d: polygonal meshes without smooth normals render faceted, as in USD and Storm; reference recorded; release gate green)
  - files: `Tests/Scene/USDRenderCycles_Test.cpp`, a committed reference scene and image under `Tests/`
  - approach: fixed seed and samples; compares within a documented tolerance, not bit-exact (Cycles' sampling differs across CPUs); records the tolerance rationale in the test.
  - verify: passes on the strict runner and in CI release; rerun produces identical output on the same host.
  - size: M

## Phase 251.2: UsdPreviewSurface translation (Lobby UAT, 2026-11-05)

- [ ] M251.P2.T1 — Apply UsdUVTexture scale and bias, and route normals through a normal-map node
  - files: charlesangus/cycles natron/deep-5.2 src/hydra/ (node translation), tools/ci/local/build-cycles.sh (NATRON_CYCLES_COMMIT), Tests/ fixture and reference
  - approach: `scale` and `bias` have no mapping (358 warnings each), so a normal map's 0..1 values are never remapped to -1..1, and `UsdPreviewSurface.normal` goes straight to the principled normal. With the 92 `normal` connections cut, the black vase, reception desk and wall sculpture light correctly.
  - verify: a normal-mapped fixture matches a recorded reference; the Lobby's vase, desk and sculpture are lit correctly with normal maps connected.
  - size: M
- [ ] M251.P2.T2 — Restore emission
  - files: charlesangus/cycles natron/deep-5.2 src/hydra/ (node translation), Tests/ fixture and reference
  - approach: `emissiveColor` warns `Could not find parameter` on all 151 Lobby surfaces: the mapping names `emission`, which is not a socket on the node. Map it to the actual emission sockets.
  - verify: an emissive fixture renders lit with no parameter warning.
  - size: M
- [ ] M251.P2.T3 — Implement or silence the remaining dropped UsdPreviewSurface inputs
  - files: charlesangus/cycles natron/deep-5.2 src/hydra/ (node translation), Tests/
  - approach: `opacityThreshold`, `useSpecularWorkflow`, `specularColor`, `occlusion`, `displacement`, `clearcoat` and `fallback` are dropped; with the above that is about 2,000 warnings per Lobby render. Decide per input, record the choice in `## Decisions`.
  - verify: a Lobby render logs no warning for an input that is handled or deliberately ignored; a fixture covers each implemented input.
  - size: M
- [ ] M251.P2.T4 — Report a fallback material on the node
  - files: Engine USDRender and Cycles delegate warning path, a test
  - approach: After M249.P3.T1 a material with no usable network falls back. Raise a node warning naming the material, as MoonRay's prim warning does.
  - verify: rendering the M249.P3.T1 fixture shows a warning on the node naming the material.
  - size: M

**Verification gate:** CI green including the Cycles test; a Cycles USDRender comp renders headless with `NatronRenderer -w`; a rendered frame and its `depth.Z`, `normal` and `primId.id` views are shared with the user; Storm references unchanged.

## Decisions

- 2026-11-05 — M251.P2.T1 written: Cycles fork commit 161c2f848 in /tmp/m251-cycles, unpushed; pin bump and the CyclesNormalMapReference golden follow when Phase 251.2 is complete. Box stays open until then.

- 2026-10-28: Cycles rendered empty: on USD 26.08 the engine runs on scene indices and UsdGeom Cube/Sphere/Cone/Cylinder/Capsule/Plane reach the delegate as implicit surfaces; Cycles 5.2 accepts only mesh/basisCurves/points/volume and only Storm registers ImplicitSurfaceSceneIndexPlugin. CyclesAovs::registerSceneIndices() registers HdsiImplicitSurfaceSceneIndex for Cycles, called before every UsdImagingGLEngine (hdsi linked). MoonRay (M252) likely needs the same. The fixture's metersPerUnit 0.01 is irrelevant (stageMetersPerUnit scales camera and geometry together).
- 2026-10-28: Implemented (build pending; reference image to record in the published image). Each engine sets stageMetersPerUnit 1 so Cycles depth is camera z in scene units (+inf on no hit), like Storm. Headlight is Storm-only. Normal AOV in camera space, offered only by non-Storm delegates; `normal` (X,Y,Z) registered in the layer registry and saved in projects. Cycles instance id read as stored+1. Cycles has no stop call: abort resets (deletes) the engine. Thread budget = the arena the engine is built in (fixed for its life, shared by concurrent Cycles sessions). Golden tolerances 0.08/channel, ≤1% pixels, mean ≤0.005 (per-machine CPU kernels with fast math). Not implemented: one warning per unsupported prim kind (in the edge cases but no task). Open for the user: no-light Cycles shows 0.5 grey background, not black; layer-menu refresh on renderer change needs a GUI check.
- 2026-10-27 — **Full AOV set wanted, mechanism deferred (user):** v1 delivers rgba, depth.Z, primId.id and normal; the user chose to spike delegate-discovered AOV selection against USD RenderSettings-driven selection in M255 - Delegate Output Spikes before building the rest.
- 2026-11-02 — Audit: T1–T3 at d635073e1 (plus 0fe1c8ae9 implicit-surface scene index, 52ca3da1b). T4 not done: USDRenderCycles_Test's MatchesReferenceRender is written but `Tests/fixtures/native-golden/USDRender/CyclesReference*.f32` is not committed, so the test fails until it is recorded in the published image. The one-warning-per-unsupported-prim-kind edge case is still unimplemented. Pushed, clean, no PR, 23 behind origin/main; carries M248's pre-squash commits and M249 (audit).
- 2026-11-03 — **Cycles from the deep-capable fork (user):** see M249; rendering here must work with that build.
- 2026-11-03 — PR #183 review fixes (6dc317908): a live Cycles session keeps its first thread count (TaskScheduler::init is refcounted, first caller wins), so Cycles engines are rebuilt when the scheduler's budget changes; follow-up option: a live thread-lowering setting in our cycles fork to avoid rebuilds. Face-varying normals reach Cycles as flat shading, vertex normals or an unwelded mesh (hdCycles ignores face-varying normals). DistantLight with Normalize off is spec-correct dim in Cycles (solid-angle scaled); documented in UAT.
