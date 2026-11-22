# M498 - LensDistortion From Scene

LensDistortion gains an optional Scene input with Camera and Lens dropdowns and a Model Source choice (Knobs / Scene), so plate undistortion uses the same lens the 3D nodes use. USDLensDistortion gains Copy From LensDistortion, which copies a solve's knobs and keys into the scene (design: `PLAN/DESIGN/2026-11-14-usd-lens-distortion-design.md`, "Authoring"). Member of F34 - Scene Lens Distortion.

## Scope

**In:**
- LensDistortion: `scene` input, `camera` ("Camera") and `lens` ("Lens") menus, `modelSource` ("Model Source": Knobs / Scene). With Scene, the model comes from `SceneLens` at each frame, and the model knobs are disabled and show the scene values.
- USDLensDistortion: `sourceNode` ("Source Node") and `copyFromLensDistortion` ("Copy From LensDistortion"), which copy values and keys.

**Out:**
- Map-only lenses driving LensDistortion: M501 - Lens Maps In The Scene (LensDistortion reports an error for a map-only lens until then).
- Projection consumers: M499 - Scene Lens In Projection Nodes, M500 - USDProject3D Lens Maps.

**Edge cases:**
- Scene with no camera or lens = None → passthrough with a warning; a dangling lens → a warning and passthrough; a bad model → an error.
- The frame is LensDistortion's own reference (its input format). Pixel-unit PanoTools parameters scale by the lens's `referenceSize`.
- Copy with `pfFile` set copies the C3/C5/centre/squeeze samples, never the file.

## Phase 498.1: Scene model source

- [ ] M498.P1.T1 — Scene input, menus and Model Source in the render path
  - files: `Engine/Nodes/Transform/LensDistortion.{h,cpp}`, `Tests/Native/NativeLensDistortionScene_Test.cpp`
  - approach: add the optional scene input and the menus, filled on the main thread with a queued refresh as `USDProject3D::refreshCameraMenu` does (`USDProject3D.cpp:856-870`, `HydraRender::cameraPaths`, `SceneLens::menuEntries`). With Scene, `buildModel` comes from `SceneLens::resolve` and `buildModel` at the render time, on CPU and GPU alike (the GPU uses the same `Model` through `LensModelsGpu.h`).
  - verify: gtest: a stage lens with K1 = -0.05 gives output bit-identical to Knobs with K1 = -0.05 on CPU and on GPU; an animated scene K1 changes the output between frames; the None and dangling cases pass through and warn.
  - size: M

- [ ] M498.P1.T2 — Read-only knob display of the scene lens
  - files: `Engine/Nodes/Transform/LensDistortion.cpp`, `Tests/Native/NativeLensDistortionScene_Test.cpp`
  - approach: with Scene, the model knobs are disabled. On a time change or a scene change (main thread, queued), they are set to the resolved values with no undo and no render trigger. Switching back to Knobs re-enables them and keeps the shown values.
  - verify: gtest calls the refresh directly and checks the values. The m498 GUI script (P2.T2) checks the knobs update when the timeline moves.
  - size: M

## Phase 498.2: Copy and GUI

- [ ] M498.P2.T1 — Copy From LensDistortion on USDLensDistortion
  - files: `Engine/Nodes/Scene/USDLensDistortion.{h,cpp}`, `Engine/Nodes/Scene/docs/USDLensDistortion.md`, `Tests/Scene/USDLensDistortion_Test.cpp`
  - approach: a node-name menu of the LensDistortion nodes in the group, and a button that copies `model` and every row of the shared table (value and all keys, both dimensions) through the knob API in one undo step.
  - verify: gtest: copying a LensDistortion with keyed K1 and centre yields identical knob curves and an identical resolved lens at in-between times.
  - size: M

- [ ] M498.P2.T2 — GUI check and docs
  - files: new `Tests/gui/m498_lens_from_scene.py`, `Engine/Nodes/Transform/docs/LensDistortion.md`
  - approach: Xvfb script: USDCamera (lens on) → LensDistortion Scene input; set Model Source to Scene; move the timeline and check the read-only knobs follow; press Copy on a USDLensDistortion. The docs describe the new controls.
  - verify: the script passes locally and in the remote `--gui` gate.
  - size: M

**Verification gate:** full build and ctest green, PR CI green, a remote GUI gate including `Tests/gui/m498_lens_from_scene.py` and `Tests/gui/m132_lens_distortion.py`, a screenshot of the Scene controls before sign-off.
