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

- [ ] M251.P1.T1 — Beauty and depth from Cycles
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/CyclesAovs.cpp`, `Engine/Nodes/Scene/CyclesAovs.h`
  - approach: with `Renderer = Cycles`, create the Cycles engine through M248's path, request `color` and `depth`, render to the delegate's converged state for the default sample count, read the buffers via `MappedAov`, convert depth to camera-space distance like Storm, write `rgba` and `depth.Z`.
  - verify: a unit test renders a unit cube with a fixed seed headless and gets non-black `rgba`, `depth.Z` equal to the camera distance at the centre pixel (within tolerance).
  - size: L

- [ ] M251.P1.T2 — primId and normal layers
  - files: `Engine/Nodes/Scene/CyclesAovs.cpp`, `Engine/LayerRegistry.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`
  - approach: request `primId` and `normal` AOVs; reuse Storm's path-hash float and hash → path metadata map for `primId.id`; register a `normal` layer with its channel names. If the delegate lacks `primId`, derive it as noted in Assumptions.
  - verify: a two-cube scene gives two distinct `primId.id` values whose metadata map names the right prims; `normal` at the centre pixel is the face normal in camera space (within tolerance).
  - size: M

- [ ] M251.P1.T3 — Cancel and thread budget
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/CyclesAovs.cpp`
  - approach: poll the Natron abort flag from the render loop and call the engine's stop; set the delegate's thread count from the scheduler's budget for the render's worker (see the task-graph scheduler decision). Verify the call names against the delegate's settings list.
  - verify: aborting a long render returns within a second and leaves no Cycles threads (checked via /proc thread count); two concurrent USDRender renders do not oversubscribe cores beyond the budget.
  - size: M

- [ ] M251.P1.T4 — Reference render test
  - files: `Tests/Scene/USDRenderCycles_Test.cpp`, a committed reference scene and image under `Tests/`
  - approach: fixed seed and samples; compares within a documented tolerance, not bit-exact (Cycles' sampling differs across CPUs); records the tolerance rationale in the test.
  - verify: passes on the strict runner and in CI release; rerun produces identical output on the same host.
  - size: M

**Verification gate:** CI green including the Cycles test; a Cycles USDRender comp renders headless with `NatronRenderer -w`; a rendered frame and its `depth.Z`, `normal` and `primId.id` views are shared with the user; Storm references unchanged.

## Decisions

- 2026-10-28: Cycles rendered empty: on USD 26.08 the engine runs on scene indices and UsdGeom Cube/Sphere/Cone/Cylinder/Capsule/Plane reach the delegate as implicit surfaces; Cycles 5.2 accepts only mesh/basisCurves/points/volume and only Storm registers ImplicitSurfaceSceneIndexPlugin. CyclesAovs::registerSceneIndices() registers HdsiImplicitSurfaceSceneIndex for Cycles, called before every UsdImagingGLEngine (hdsi linked). MoonRay (M252) likely needs the same. The fixture's metersPerUnit 0.01 is irrelevant (stageMetersPerUnit scales camera and geometry together).
- 2026-10-28: Implemented (build pending; reference image to record in the published image). Each engine sets stageMetersPerUnit 1 so Cycles depth is camera z in scene units (+inf on no hit), like Storm. Headlight is Storm-only. Normal AOV in camera space, offered only by non-Storm delegates; `normal` (X,Y,Z) registered in the layer registry and saved in projects. Cycles instance id read as stored+1. Cycles has no stop call: abort resets (deletes) the engine. Thread budget = the arena the engine is built in (fixed for its life, shared by concurrent Cycles sessions). Golden tolerances 0.08/channel, ≤1% pixels, mean ≤0.005 (per-machine CPU kernels with fast math). Not implemented: one warning per unsupported prim kind (in the edge cases but no task). Open for the user: no-light Cycles shows 0.5 grey background, not black; layer-menu refresh on renderer change needs a GUI check.
- 2026-10-27 — **Full AOV set wanted, mechanism deferred (user):** v1 delivers rgba, depth.Z, primId.id and normal; the user chose to spike delegate-discovered AOV selection against USD RenderSettings-driven selection in M255 - Delegate Output Spikes before building the rest.
