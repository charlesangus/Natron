# M499 - Scene Lens In Projection Nodes

CardProject25D and USDProjectUV take lens distortion from the scene (design: `PLAN/DESIGN/2026-11-14-usd-lens-distortion-design.md`, "Consumers"). In CardProject25D, the Projection camera's lens undistorts the plate and the Render camera's lens redistorts the result, analytically on CPU and GPU. Connected Undistort/Redistort inputs still win. USDProjectUV sends each vertex's undistorted aperture position through the lens to an exact distorted plate UV. Member of F34 - Scene Lens Distortion.

## Scope

**In:**
- CardProject25D: `projectionLens` ("Projection Lens") and `renderLens` ("Render Lens") menus beside the camera menus, applied when the matching map input is unconnected. CPU and GPU. `getTransform` concatenation is off while a lens is active.
- USDProjectUV: `lens` ("Lens") beside its camera menu; distorted per-vertex UVs; the tooltip says curvature between vertices needs dense meshes.

**Out:**
- Map-only lenses in these nodes: M501 - Lens Maps In The Scene (they report an error until then).
- Changing CardProject25D's map inputs: M437 - CardProject25D (unchanged).

**Edge cases:**
- A map input connected and a scene lens chosen → the map input wins, silently.
- Without a Projection input, the Projection camera is the Render camera held at the projection frame; its lens is resolved at that held time.
- Newton fails at a pixel → that pixel is outside (the Outside mode), as in LensDistortion.

**Assumptions:**
- M437 - CardProject25D lands as planned: `Engine/Nodes/Transform/CardProject25D.{h,cpp}`, `Engine/Gpu/Kernels/cardproject.slang`, and lens-map stages before and after the homography. M437 is a member of F25 - Overlay Flow, so this milestone waits for F25 to land. §5a re-checks the paths at claim time.

## Phase 499.1: CardProject25D

- [ ] M499.P1.T1 — Scene lens menus and CPU path
  - files: `Engine/Nodes/Transform/CardProject25D.{h,cpp}`, `Tests/CardProject25D_Test.cpp`
  - approach: fill the lens menus with the camera menus (`SceneLens::menuEntries`, main thread, queued). In the render, when the Undistort input is unconnected and a projection lens resolves, undistort the plate lookup through the model's undistort direction in aperture space conformed by the Fit Mode. Do the same with the Redistort direction for the render lens. Lenses are resolved at the held or current time, and at the shutter times when motion blur is on. Return no `getTransform` while either lens is active.
  - verify: gtest: a scene lens gives output equal, within filter tolerance, to the same distortion as STMap inputs rendered by LensDistortion; a connected map overrides the lens; `getTransform` is absent with a lens.
  - size: L

- [ ] M499.P1.T2 — GPU path
  - files: `Engine/Gpu/Kernels/cardproject.slang`, `Engine/Nodes/Transform/CardProject25D.cpp`, `Tests/CardProject25D_Test.cpp`
  - approach: pass both models' parameter blocks through `LensModelsGpu.h` and evaluate them with `lensmodels.slang` in the kernel at the same stage as the CPU path.
  - verify: a GPU/CPU parity gtest with a 3DE anamorphic lens on both cameras, within the existing CardProject25D parity tolerance.
  - size: L

## Phase 499.2: USDProjectUV

- [ ] M499.P2.T1 — Per-vertex lens UVs
  - files: `Engine/Nodes/Scene/USDProjectUV.{h,cpp}` (menu at :702-718, camera at :870), `Engine/Nodes/Scene/docs/USDProjectUV.md`, `Tests/Scene/USDProjectUV_Test.cpp`
  - approach: add the lens menu. For each vertex, after the camera projection gives an undistorted aperture position, apply the distort direction and write the distorted plate UV. The lens is resolved at the node's frame (and at its hold frame when held). The tooltip carries the dense-mesh note.
  - verify: gtest: for a barrel lens, a grid's vertex UVs equal the model's forward map within 1e-6; lens None gives today's UVs.
  - size: M

- [ ] M499.P2.T2 — GUI check and docs
  - files: new `Tests/gui/m499_projection_lens.py`, `Engine/Nodes/Transform/docs/CardProject25D.md`
  - approach: Xvfb script: USDCamera (lens on) into CardProject25D and USDProjectUV; pick lenses; render a frame; check no errors and that the menus list the lens names.
  - verify: the script passes locally and in the remote `--gui` gate.
  - size: M

**Verification gate:** full build and ctest green (CPU and GPU legs), PR CI green, a remote GUI gate including `Tests/gui/m499_projection_lens.py` and M437's `Tests/gui/m437_cardproject25d.py`. Before/after CardProject25D renders with a scene lens shared one file each.

## Decisions

- 2026-11-22 — A connected Undistort/Redistort map input wins over the scene lens, silently (user, grilling): connecting a map is an explicit override.
