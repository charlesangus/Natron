# M501 - Lens Maps In The Scene

Lenses with no model travel in the scene as maps (design: `PLAN/DESIGN/2026-11-14-usd-lens-distortion-design.md`, "Schema sketch", map form; "Map conventions"). `NatronLens` gains `mapType`, `undistortMap`, `redistortMap` and `idistortScale`. USDLensDistortion gains image inputs and file maps, a single map is inverted for the other direction, and CardProject25D, USDProjectUV, USDProject3D and LensDistortion load maps for map-only lenses. Member of F34 - Scene Lens Distortion.

## Scope

**In:**
- The schema attributes; `SceneLens` reads maps (asset samples held between frames).
- USDLensDistortion: `undistortMapInput`/`redistortMapInput` image inputs (authored as `natron://` live images), `undistortMapFile`/`redistortMapFile` ("Undistort Map File" and "Redistort Map File"), `mapType` ("Map Type": STMap / IDistort), `idistortScale` ("IDistort Scale").
- A shared async, cancellable map loader (`natron://` through a graph render, files through OIIO) and single-map inversion through M433's inverter.
- Map-only lenses in M498's LensDistortion and in M499's and M500's consumers.

**Out:**
- Maps on the USDCamera group: none (maps are authored through USDLensDistortion).
- USDRender applying a lens (model or map): none for now (user, 2026-11-22).

**Edge cases:**
- Both a model and maps → consumers use the model; the maps are informational.
- Only one map → the other direction is its numerical inverse at map resolution.
- Alpha 0 → outside. A missing map file or URI → an error.
- IDistort values are reference pixels × `idistortScale`; overscan comes from the map's data window.

**Assumptions:**
- `OverlayPointMap::invertNumerically` (`Engine/OverlayPointMap.h:103`, currently on `feature/f25-overlay-flow`) is on main when this starts; F25 - Overlay Flow lands first (M499 already waits for it).

## Phase 501.1: Schema and loading

- [ ] M501.P1.T1 — Map attributes and SceneLens map read
  - files: `Engine/Nodes/Scene/lens/generatedSchema.usda`, `Engine/Nodes/Scene/SceneLens.{h,cpp}`, `Tests/Scene/SceneLens_Test.cpp`
  - approach: add the four attributes. `LensSample` gains map paths resolved at the time (assets are held, not interpolated) plus the type and scale; `model = none` is valid when a map exists. `authorLens` writes maps.
  - verify: gtest: a time-sampled `undistortMap` resolves frame 1001's value at 1001.5; map-only with `model` none is valid; model and maps together report the model as active.
  - size: M

- [ ] M501.P1.T2 — Shared lens map loader and inversion
  - files: new `Engine/Nodes/Scene/SceneLensMaps.{h,cpp}`, `Tests/Scene/SceneLensMaps_Test.cpp`
  - approach: load the needed direction as an STMap-convention float image. IDistort is converted to positions; a missing direction is built by the M433 inverter at map resolution. Loading is async, cancels quickly on abort, and is cached by asset path and sample. It returns errors, never empty images.
  - verify: gtest: an IDistort map and its STMap equivalent load to equal images; the inverse of a synthetic barrel undistort map matches the analytic redistort within one texel; an abort mid-load returns promptly.
  - size: L

## Phase 501.2: Authoring

- [ ] M501.P2.T1 — USDLensDistortion map inputs and file maps
  - files: `Engine/Nodes/Scene/USDLensDistortion.{h,cpp}`, `Engine/Nodes/Scene/docs/USDLensDistortion.md`, `Tests/Scene/USDLensDistortion_Test.cpp`
  - approach: optional image inputs author `natron://` live images (per frame when the input is animated, otherwise once) through `LiveTextureStore`. The file knobs author asset paths. `model` may be set to None for map-only.
  - verify: gtest: a connected STMap input authors a resolvable URI and USDWrite with Export Live Textures writes the EXRs; a file map round-trips through USDWrite/USDRead with a rewritten path.
  - size: L

## Phase 501.3: Consumers

- [ ] M501.P3.T1 — CardProject25D and LensDistortion map-only lenses
  - files: `Engine/Nodes/Transform/CardProject25D.cpp`, `Engine/Nodes/Transform/LensDistortion.cpp`, `Tests/CardProject25D_Test.cpp`, `Tests/Native/NativeLensDistortionScene_Test.cpp`
  - approach: CardProject25D feeds the loaded maps to M437's existing map stages, CPU and GPU. LensDistortion with Model Source Scene and a map-only lens warps through the map with STMap's sampler, CPU and GPU.
  - verify: gtest: map-only output equals the same maps connected as inputs (CardProject25D) or through STMap (LensDistortion), CPU and GPU parity included.
  - size: M

- [ ] M501.P3.T2 — USDProjectUV map-only lenses
  - files: `Engine/Nodes/Scene/USDProjectUV.cpp`, `Tests/Scene/USDProjectUV_Test.cpp`
  - approach: sample the undistort map bilinearly at each vertex's aperture position.
  - verify: gtest: for a map baked from a barrel model, vertex UVs match the model path within one texel.
  - size: M

- [ ] M501.P3.T3 — USDProject3D uses the lens's own map
  - files: `Engine/Nodes/Scene/USDProject3D.cpp`, `Engine/Nodes/Scene/SceneLensMapBake.cpp`, `Tests/Scene/USDProject3D_Test.cpp`
  - approach: for a map-only lens, author its undistort map (or the loader's inverse of its redistort map) on `lensMap` instead of baking one.
  - verify: gtest: a projection with a map-only lens matches the model-baked projection in Storm within tolerance.
  - size: M

- [ ] M501.P3.T4 — GUI check
  - files: new `Tests/gui/m501_lens_maps.py`
  - approach: Xvfb script: LensDistortion STMap output → USDLensDistortion map input, model None → CardProject25D and USDProject3D; render, then abort and re-render.
  - verify: the script passes locally and in the remote `--gui` gate.
  - size: M

**Verification gate:** full build and ctest green (CPU and GPU legs), PR CI green, a remote GUI gate including `Tests/gui/m501_lens_maps.py` and `Tests/gui/scene_projection_delegates.py`. A map-only versus model projection pair shared one file each.
