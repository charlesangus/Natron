# M344 - Camera Projection In Delegates

Make M234 - Camera Projection Nodes' `USDProject3D` render in Cycles and MoonRay, matching Storm. The user called this "a core part of comp workflows" (2026-11-05). Today the node authors a Storm-only glslfx surface (`Engine/Nodes/Scene/shaders/natronProject3D.glslfx`), so both delegates fall back. The node will author each renderer's native network beside the glslfx one, on `outputs:cycles:surface` and `outputs:moonray:surface`, driven by the same stored parameters. The plate arrives through M343 - Live Textures In Delegates.

## Scope

**In:**
- All `USDProject3D` behaviour on both delegates: the projection through `worldToProjector` and `plateTransform`, World and Geometry projection space, Project On, Crop, near and far clip, and the camera and plate following the frame or frame hold.
- The same output as Storm: unlit, emissive, with the plate's alpha showing what is behind it.
- Agreement with Storm within tolerance on M234's test scenes, on interior pixels.

**Out:**
- Occlusion, and combining a projection with an existing material: as in M234 - Camera Projection Nodes.
- OSL: none (user, 2026-11-05).
- Natron Utility: M281 - Utility Materials And Lights evaluates the projection natively.
- MaterialX projection: none, MaterialX is set aside (user, 2026-11-05).
- Per-placement and deforming-geometry projection: M341 - Per-Placement Geometry Projection and M342 - Sticky Projections On Deforming Geometry extend all three renderers when they land.

**Edge cases:**
- Points behind the camera or outside the crop: the outside mode, as in Storm.
- Motion blur: the projection is evaluated at the shutter's frame time for v1, so a moving camera doesn't smear the plate differently per sample. T1 records whether a delegate forces otherwise.
- USDWrite: its export bake still replaces the material with a `UsdPreviewSurface`, so the native networks don't reach exported files.
- Plate missing: M343's fallback and node warning.

**Assumptions:**
- Cycles nodes can express the projection. `texture_coordinate` takes only an affine `ob_tfm`, so the perspective divide needs `geometry` Position, vector maths and `image_texture`, or a dedicated node in the charlesangus fork. T1 decides.
- MoonRay's `ProjectCameraMap`, reached through hdMoonray's coordSys binding (`Material.cc:105`), or a small map DSO can match Storm's parameters. T1 decides.
- M325 - MoonRay Fork carries the `GetMaterialRenderContexts` override that lets hdMoonray read `outputs:moonray:surface`.

## Phase 344.1: Route

- [ ] M344.P1.T1 — Hand-author both projection networks
  - files: scratch only; findings recorded in this file's `## Decisions`
  - approach: hand-author `.usda` materials for Cycles and MoonRay that reproduce `natronProject3D.glslfx`'s parameters on M234's test scene, with a file plate. Read `ProjectCameraMap`'s attributes first. Compare each against Storm. If a stock network can't match, specify the smallest fork node or DSO that does.
  - verify: each renderer's frame matches Storm within tolerance on interior pixels; the chosen network, or the required new node, is recorded with source citations.
  - size: L

## Phase 344.2: Authoring

- [ ] M344.P2.T1 — `USDProject3D` authors the Cycles network
  - files: `Engine/Nodes/Scene/USDProject3D.cpp`, `Engine/Nodes/Scene/SceneProjectionCameras.{h,cpp}`, `Tests/Scene/USDRenderCycles_Test.cpp`, one reference crop (≤256 px, half-float, zip)
  - approach: author T1's network on `outputs:cycles:surface` from the values already stored for glslfx, per frame or held, so the two can't drift. Any fork node goes in first, with `NATRON_CYCLES_COMMIT` bumped.
  - verify: gtest — a plate projected onto a card from the render camera reproduces the plate; Crop and Project On change the expected pixels; an animated camera differs at two frames.
  - size: L

- [ ] M344.P2.T2 — `USDProject3D` authors the MoonRay network
  - files: `Engine/Nodes/Scene/USDProject3D.cpp`, `Tests/Scene/USDRenderMoonRay_Test.cpp`, one reference crop (≤256 px, half-float, zip)
  - approach: as M344.P2.T1, on `outputs:moonray:surface`, using T1's map or DSO. A new DSO ships through M325's fork and passes `build-moonray.sh`'s count checks.
  - verify: gtest — the same three cases as M344.P2.T1 pass through MoonRay.
  - size: L

- [ ] M344.P2.T3 — Export and progressive viewer check
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (only if needed), `Tests/Scene/USDWrite_Test.cpp`, `Tests/gui/scene_projection_delegates.py`
  - approach: confirm the export bake drops the native outputs, and fix it if not. A GUI script selects each delegate, projects a Constant-driven plate onto a card, moves the camera and edits the plate.
  - verify: gtest — an exported file has no `cycles:` or `moonray:` outputs and renders the same in `usdview`; the Xvfb script at 1920x1080 passes and its screenshots go to the user, one file each.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; the delegate render tests pass; `scene_projection_delegates.py` passes under Xvfb; the user OKs the screenshots and tests a UAT AppImage projecting a plate sequence in both delegates.

## Decisions

- 2026-11-05 — **Projection is in scope for delegates (user):** "absolutely, this is a core part of comp workflows".
- 2026-11-05 — **Author native networks in the node, not a translation filter (cat-plan):** the parameters are already known at authoring time, and the result is plain USD a user can inspect.
