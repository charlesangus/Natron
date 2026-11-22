# M500 - USDProject3D Lens Maps

USDProject3D's projections follow the projector's lens (design: `PLAN/DESIGN/2026-11-14-usd-lens-distortion-design.md`, "Consumers", USDProject3D). Natron bakes an undistort map for the lens internally, at the plate's resolution plus the overscan it needs. It serves the map as a `natron://` live image on a new `lensMap` input of the projection shader. Each network (Storm glslfx, Cycles, MoonRay) gains one lookup: projected st, then `lensMap`, then plate st. The USDWrite projection bake carries `lensMap` like the plate. Member of F34 - Scene Lens Distortion.

## Scope

**In:**
- `lens` ("Lens") beside the camera menu (`USDProject3D.cpp:856-870`); the lens is sampled at the hold frame when Use Frame Hold is on, and resolved on the rendering stage when the camera is (`natron:projectionCamera`, `USDProject3D.h:80`).
- The internal bake (a CPU model evaluation, cached per lens sample and size) served through `LiveTextureStore`.
- `lensMap` in `Engine/Nodes/Scene/shaders/natronProject3D.glslfx`, `CyclesNetwork` (:219) and `MoonRayNetwork` (:425); `authorResolvedProjector` (`USDWrite.cpp:838`).

**Out:**
- Using an authored lens map instead of baking one: M501 - Lens Maps In The Scene.
- A vertex-shader distortion inside Storm: rejected in the design.

**Edge cases:**
- Lens None → no `lensMap` authored, and the networks are exactly as today.
- Map alpha 0 (Newton failed) → the projection is treated as outside the plate.
- An animated lens without frame hold → one bake per frame; each bake replaces the last in the store.

**Assumptions:**
- A `natron:projectionLens` token beside `natron:projectionCamera` is a planner addition the design doc does not name.

## Phase 500.1: Lens and bake

- [ ] M500.P1.T1 — Lens menu and resolution
  - files: `Engine/Nodes/Scene/USDProject3D.{h,cpp}`, `Engine/Nodes/Scene/SceneProjectionCameras.{h,cpp}`, `Tests/Scene/USDProject3D_Test.cpp`
  - approach: add the lens menu, and record the choice next to `natron:projectionCamera` (as a `natron:projectionLens` token) so the rendering-stage path resolves the lens through `SceneLens` on that stage at the hold or current time.
  - verify: gtest: the hold frame picks the held lens sample; rendering-stage resolution finds a lens that only exists downstream.
  - size: M

- [ ] M500.P1.T2 — Internal undistort-map bake as a live image
  - files: `Engine/Nodes/Scene/USDProject3D.cpp`, new `Engine/Nodes/Scene/SceneLensMapBake.{h,cpp}`, `Engine/Nodes/Scene/LiveTextureStore.{h,cpp}` (if a non-graph producer needs an entry point), `Tests/Scene/USDProject3D_Test.cpp`
  - approach: bake an STMap-convention undistort map (RG = normalised distorted plate position, A = valid) at the plate size, with a data window grown to `undistortedBounds`. Serve it under a stable `natron://` URI keyed by the lens-sample hash, and author it on the projection shader's `lensMap` input. Run the bake off the main thread and make it cancellable.
  - verify: gtest: baked texels equal the model's forward map within 1e-5; an unchanged lens reuses the cached entry.
  - size: L

## Phase 500.2: Shader networks

- [ ] M500.P2.T1 — Storm glslfx lensMap lookup
  - files: `Engine/Nodes/Scene/shaders/natronProject3D.glslfx`, `Engine/Nodes/Scene/USDProject3D.cpp`, `Tests/Scene/USDProject3D_Test.cpp`
  - approach: a `lensMap` texture input with a `hasLensMap` flag; when set, the projected st reads lensMap.rg, alpha 0 means outside, and the result becomes the plate st.
  - verify: a Storm render gtest: a projection through a barrel lens matches the plate undistorted by LensDistortion, within tolerance, on a card filling the projector.
  - size: M

- [ ] M500.P2.T2 — Cycles network lensMap lookup
  - files: `Engine/Nodes/Scene/USDProject3D.cpp` (`CyclesNetwork`), `Tests/Scene/USDProject3D_Test.cpp`
  - approach: insert an image-texture node sampling `lensMap` at the projected st, closest filtering, then feed its RG to the plate texture's vector. Alpha drives the outside mix.
  - verify: a Cycles render gtest matches the Storm result of P2.T1 within the delegate parity tolerance.
  - size: M

- [ ] M500.P2.T3 — MoonRay network lensMap lookup
  - files: `Engine/Nodes/Scene/USDProject3D.cpp` (`MoonRayNetwork`), `Tests/Scene/USDProject3D_Test.cpp`
  - approach: the same lookup with MoonRay's ImageMap, respecting the user-data limits noted at :419-422.
  - verify: a MoonRay render gtest matches P2.T1 within the delegate parity tolerance.
  - size: M

## Phase 500.3: Bake and GUI

- [ ] M500.P3.T1 — USDWrite projection bake carries lensMap
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (`authorResolvedProjector`, :838), `Tests/Scene/USDWrite_Test.cpp`
  - approach: resolve `lensMap` like the plate: Export Live Textures writes it as an EXR and rewrites the asset path.
  - verify: gtest: the written file has `lensMap` pointing to an EXR on disk whose texels match the bake; reading it back renders the same projection in Storm.
  - size: M

- [ ] M500.P3.T2 — GUI check and docs
  - files: `Tests/gui/scene_projection_delegates.py`, `Engine/Nodes/Scene/docs/USDProject3D.md`
  - approach: extend the delegates script with a lens on the projector across Storm, Cycles and MoonRay. Document Lens.
  - verify: the script passes locally and in the remote `--gui` gate.
  - size: M

**Verification gate:** full build and ctest green, PR CI green, a remote GUI gate including `Tests/gui/scene_projection_delegates.py` and `Tests/gui/m234_projection.py`. Storm, Cycles and MoonRay projections through a barrel lens shared one file each.
