# M497 - Lens Schema And Camera Authoring

A camera's parametric lens distortion lives in the USD scene as a codeless `NatronLens` prim. It is a child of the camera by default, and the camera's ordered `natron:lenses` relationship selects it (design: `PLAN/DESIGN/2026-11-14-usd-lens-distortion-design.md`, "Schema sketch" and "Lookup rule"). This milestone builds the schema and the `SceneLens` helpers that every consumer (M498–M501) uses to build a `LensModels::Model` at a time. It also adds the USDCamera Lens Distortion group and a parametric USDLensDistortion node, and proves the USDWrite/USDRead round trip. Member of F34 - Scene Lens Distortion.

## Scope

**In:**
- A codeless typed `NatronLens` schema (`plugInfo.json` + `generatedSchema.usda`), registered at start-up the way `natronLiveTexture` is (`Engine/CMakeLists.txt:251-254`, `LiveTextureResolver.cpp:209-225`). It holds `model`, `parameters:<LensDistortion script name>`, `useCameraIntrinsics`, `referenceSize` and `pixelAspectRatio`.
- One model-knob table shared by LensDistortion, USDCamera and USDLensDistortion, so the three cannot drift.
- `SceneLens`: candidates, a Lens menu, lookup by choice, a read at a time (3DE intrinsics from the camera), undistorted bounds of a format, and authoring.
- The USDCamera Lens Distortion group (off by default) and the USDLensDistortion in-out node (parametric).
- USDWrite (Layered and Flattened) and USDRead round trip.

**Out:**
- The map attributes (`mapType`, `undistortMap`, `redistortMap`, `idistortScale`) and USDLensDistortion's image inputs: M501 - Lens Maps In The Scene.
- LensDistortion's Scene input and Copy From LensDistortion: M498 - LensDistortion From Scene.
- Any consumer applying a lens: M499 - Scene Lens In Projection Nodes, M500 - USDProject3D Lens Maps.
- USDRender applying a lens, and Natron Utility distorted rays: none for now (user, 2026-11-22).
- Importing or exporting Hydra's `lensDistortion:*` camera attributes: none.
- Old project compatibility: none (no backward compat in the fork).

**Edge cases:**
- `natron:lenses` authored → its `NatronLens` targets in order; otherwise `NatronLens` children in namespace order.
- A dangling target or a named lens that is gone → a warning and no distortion. An invalid `model` token or `Model::valid == false` → an error.
- `referenceSize` × `pixelAspectRatio` off the aperture aspect by more than 0.1% → a warning, and the lens fits horizontally.
- `pfFile` is not stored, and unauthored parameters take LensDistortion's defaults.

**Assumptions:**
- The pinned OpenUSD supports codeless schemas (`skipCodeGeneration` in `generatedSchema.usda`). T2 confirms this in the pxr headers and docs before writing anything.
- The `NatronLens` and `natron:` prefixes stay until M385 - OpenNucleon Code Rename renames them along with everything else.

## Phase 497.1: Schema and helpers

- [ ] M497.P1.T1 — Shared lens model-knob table
  - files: new `Engine/Nodes/Transform/LensModelParams.{h,cpp}`, `Engine/Nodes/Transform/LensDistortion.{h,cpp}` (`ParamDef`/`kParamDefs` at :80-159, `buildModel` at :698), `Tests/Native/LensModelParams_Test.cpp`
  - approach: move `ParamDef`, `kParamDefs`, the model-bit masks and the param→`LensModels::Model` switch out of LensDistortion.cpp unchanged (names, labels, hints and defaults as they are); expose `buildModel(ModelType, const Frame&, std::function<double(ModelParam,int)>)` and a token↔ModelType map using LensDistortion's Model ids. LensDistortion keeps its knobs and calls the shared function.
  - verify: the existing `NativeLensDistortion_Test`, `NativeLensDistortionGpu_Test` and `LensModels*_Test` pass unchanged. A new gtest builds each of the 8 models through the shared function and through a LensDistortion node with the same values, and the outputs are bit-identical.
  - size: M

- [ ] M497.P1.T2 — Codeless NatronLens schema, registered at start-up
  - files: new `Engine/Nodes/Scene/lens/plugInfo.json`, `Engine/Nodes/Scene/lens/generatedSchema.usda`, `Engine/CMakeLists.txt` (copy to `Resources/usd/natronLens/`), the AppImage staging script, `Engine/Nodes/Scene/LiveTextureResolver.cpp` or a new `Engine/Nodes/Scene/SceneSchemas.cpp` (`PlugRegistry::RegisterPlugins`), `Tests/Scene/SceneLens_Test.cpp`
  - approach: check the pxr headers and the `.local/docs` USD tree for the codeless-schema form first. Declare `NatronLens` (typed, concrete, IsA) with the attributes in the design sketch (parametric only), plus the `natron:lenses` relationship as a camera-side custom property (no API schema). Register before any stage opens, and report a missing file the way the live-texture path does.
  - verify: a gtest finds `NatronLens` in `UsdSchemaRegistry` with its fallbacks, and a defined prim reports `IsA` the type. A missing plugInfo produces the error text. The AppImage file list includes `Resources/usd/natronLens/plugInfo.json`.
  - size: M

- [ ] M497.P1.T3 — SceneLens lookup and read at a time
  - files: new `Engine/Nodes/Scene/SceneLens.{h,cpp}`, `Tests/Scene/SceneLens_Test.cpp`
  - approach: `candidates(camera)` and `menuEntries(stage, camera)` (Default, None, names) follow the lookup rule. `resolve(stage, camera, choice, time)` returns a `LensSample` (model, params, flags, reference size, PAR) or a warning or error per the rule. `buildModel(sample, camera, time, frame)` goes through T1's function. With `useCameraIntrinsics`, the 3DE focal length, filmback, lens-centre offset and focus distance come from the camera at the same time, converted through `UsdGeomGetStageMetersPerUnit` (tenths of a unit → cm). It applies the 0.1% aspect check. All of this reads the composed stage only. Menus are filled on the main thread by the caller.
  - verify: gtest: children-only order, relationship order, Default/None/name, dangling target → warning, bad token → error, a time-sampled coefficient interpolates at t=1.5, a zoom on the camera changes a 3DE model when the flag is on and leaves it alone when off, and the aspect mismatch warns.
  - size: L

- [ ] M497.P1.T4 — SceneLens undistorted bounds
  - files: `Engine/Nodes/Scene/SceneLens.{h,cpp}`, `Tests/Scene/SceneLens_Test.cpp`
  - approach: `undistortedBounds(model, format)` maps every edge pixel of the format through the undistort direction and returns the enclosing rect in aperture space. A non-finite or failed edge sample returns an error. There is no cap. M500 - USDProject3D Lens Maps and M501 - Lens Maps In The Scene use it.
  - verify: gtest: identity → the format, barrel → bounds grow, pincushion → bounds shrink, a model that diverges at the edge → an error.
  - size: M

- [ ] M497.P1.T5 — SceneLens authoring
  - files: `Engine/Nodes/Scene/SceneLens.{h,cpp}`, `Tests/Scene/SceneLens_Test.cpp`
  - approach: `authorLens(stage, camera, name, LensValues, AddMode)` defines `<camera>/<name>` as `NatronLens` and writes `model` and the uniforms. It writes only the parameters the model uses, as default values or time samples from a knob-key list. AddMode Append adds the lens to `natron:lenses` if absent, and Replace sets it to the lens alone.
  - verify: gtest: author, then `resolve`, gives the same `LensSample` at keyed and in-between times; Append twice keeps one entry; Replace drops the others.
  - size: M

## Phase 497.2: Authoring nodes

- [ ] M497.P2.T1 — USDCamera Lens Distortion group
  - files: `Engine/Nodes/Scene/USDCamera.{h,cpp}`, `Engine/Nodes/Scene/docs/USDCamera.md`, `Tests/Scene/USDCamera_Test.cpp`
  - approach: add a "Lens Distortion" group with `lensDistortion` ("Lens Distortion", default off), `lensName` ("Lens Name", default `lens`), `model`, every row of T1's table (LensDistortion's names, labels and defaults), `useCameraIntrinsics`, `referenceSize` and `pixelAspectRatio`. Model visibility uses the same mask logic as `LensDistortion::updateModelVisibility`. When on, the camera's scene contribution calls `authorLens` (Replace). Animated knobs author time samples.
  - verify: gtest: the toggle off authors no lens; on with an animated K1 resolves through `SceneLens` at two times; changing the model hides and shows the right knobs.
  - size: M

- [ ] M497.P2.T2 — USDLensDistortion node (parametric)
  - files: new `Engine/Nodes/Scene/USDLensDistortion.{h,cpp,reg.inc}`, new `Engine/Nodes/Scene/docs/USDLensDistortion.md`, `Tests/Scene/USDLensDistortion_Test.cpp`
  - approach: an in-out scene node like `USDEditPrim`/`USDPrune`, using their prim-pattern knob. It has the same lens knobs as P2.T1 (shared builder from T1's table) and `addToLenses` ("Add To Lenses": Append / Replace), and it authors on every matched `UsdGeomCamera`. Non-camera matches are skipped silently.
  - verify: gtest: on a USDRead-style stage with two cameras, a pattern matching both authors a lens on each; Append keeps an existing lens first; a pattern that matches nothing passes the stage through unchanged.
  - size: L

## Phase 497.3: Round trip and GUI

- [ ] M497.P3.T1 — USDWrite/USDRead round trip
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDWrite_Test.cpp`, `Tests/Scene/USDRead_Test.cpp`
  - approach: lens prims and the relationship travel like any other data. Add a warning when a `natron:lenses` target lies outside the written stage.
  - verify: gtest: write in Layered and in Flattened mode, read back, and `SceneLens::resolve` matches the source at keyed times; an out-of-stage target warns; a file with no lens has no distortion.
  - size: M

- [ ] M497.P3.T2 — GUI check
  - files: new `Tests/gui/m497_scene_lens.py`
  - approach: under Xvfb (recipe `build/deeprepro/run-gui.sh`, 1920x1080), build USDCamera with the group on, switch models, then USDRead → USDLensDistortion; check that the scene graph panel shows the lens prim and that the knob labels match their script names.
  - verify: the script passes locally under Xvfb and in the remote `--gui` gate.
  - size: M

**Verification gate:** full build and ctest green, PR CI green, a remote GUI gate (`--gui`) running `Tests/gui/m497_scene_lens.py` and `Tests/gui/m132_lens_distortion.py` (P1.T1 touches LensDistortion). Screenshots of the USDCamera group and USDLensDistortion shared one file each before sign-off.

## Decisions

- 2026-11-22 — The "Nuke" model entry and "Nuke:" tooltip prefixes stay as they are (user).
- 2026-11-22 — No lens in USDRender for now (user, grilling): the render-side redistort and Natron Utility distorted rays from the design's sketch (D, G) are not filed; the design doc keeps them on record.
- 2026-11-22 — The lens milestones ship as one Feature, F34 - Scene Lens Distortion (user, grilling): a lens on a camera does nothing visible until a consumer uses it, so one UAT covers the set.
