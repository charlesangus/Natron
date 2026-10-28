# M235 - USDCamera From File

`USDCamera` can take its camera from a file: a tracked camera in an `.abc`/`.usd`, with its
values exposed on the node's knobs for edits. This is the comp hand-off from a tracker,
like Nuke's Camera "read from file" and GeoEditCamera. USDRead already loads cameras from
those formats. This milestone makes the camera a first-class, editable `USDCamera` without
dragging in the whole file. It stays a pure source node per M226 - Scene Hierarchy From
Topology, with no scene input. Survey: `PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`.

## Scope

**In:**
- A "Read From File" toggle, a File knob (`.usd`/`.usda`/`.usdc`/`.usdz`/`.abc`), a camera-prim chooser listing the file's `UsdGeomCamera`s (default the first), a frame offset (USDRead's time mapping), and Reload.
- When on, the node references the chosen camera prim from the file at its own Prim Path. The knobs show the file's values at the current frame, read-only by default.
- An "Edit" toggle per knob group (transform, lens, clipping). It authors overrides over the referenced values, Nuke GeoEditCamera-style, and a reset clears them.

**Out:**
- FBX and `.chan` import — none (user: USD/ABC covers modern trackers).
- Baking the file camera into keyframes on the knobs — none.

**Edge cases:**
- The file or camera prim is missing: node error with USD's diagnostic, as USDRead does.
- The camera's parent xforms in the file: the world transform is preserved by referencing the camera with its ancestors flattened into one xformOp (`ComputeLocalToWorldTransform` sampled per frame), recorded in `## Decisions` if another way proves better.
- File units or up axis differ from the project's: a node warning, as USDRead gives (M226.P3.T1). No conversion.
- Reload: shares USDRead's write barrier (M19.P3.T4).

**Assumptions:**
- Unconfirmed: flattening the camera's world transform into the node's own prim (rather than referencing the camera's whole ancestor chain) is what artists expect when they re-parent the camera under a USDXformPrim.

## Phase 235.1: File-backed camera

- [ ] M235.P1.T1 — `USDCamera` reads a camera prim from a file
  - files: `Engine/Nodes/Scene/USDCamera.{h,cpp}`, `Engine/Nodes/Scene/USDRead.cpp` (share its file-stamp, time-mapping and Reload helpers; factor them out if they are private), `Tests/Scene/USDCamera_Test.cpp`, a small committed `.abc` or `.usda` with an animated camera under a transformed parent
  - approach: with Read From File on, author at the node's Prim Path a `UsdGeomCamera` whose lens and clipping attributes come from the file camera (time samples over the project range, mapped through the frame offset) and whose transform is the file camera's world transform as one `xformOp:transform` per sample. The file layer is opened through the same stamp registry and barrier as USDRead. The camera chooser is populated from the file.
  - verify: gtest — the committed animated camera's world matrix and focal length at frames 1 and 10 match `UsdGeomCamera`/`ComputeLocalToWorldTransform` read directly from the file; a missing prim errors; Reload after rewriting the file changes the values.
  - size: M

- [ ] M235.P1.T2 — Knob display and per-group edit overrides
  - files: `Engine/Nodes/Scene/USDCamera.cpp`, `Tests/Scene/USDCamera_Test.cpp`, `Tests/gui/scene_camera_from_file.py`
  - approach: with a file camera, the knobs refresh to the file's values at the current frame (display-only, disabled). An Edit toggle per group (transform, lens, clipping) enables that group's knobs. Their values then author over the file's samples: the transform group as an appended offset op, lens and clipping as replacement values. Turning Edit off restores the file values.
  - verify: gtest — enabling the lens group and setting focal 35 overrides only focalLength while the transform still follows the file; Xvfb GUI script — the knobs show the file's focal at frame 10 and update on scrub.
  - size: M

**Verification gate:** release gate green; a tracked `.abc` camera loads through USDCamera, looks through correctly in the 3D view, and drives USDRender to match the same camera through USDRead; screenshots shared before sign-off.

## Decisions

- 2026-10-28: implemented (T1, T2; build pending). File camera's ancestors flattened into one xformOp:transform (default value only when static); file values copied raw (units/up-axis only warn, as USDRead); Camera Prim id = prim path, a vanished camera errors rather than switching; file-driven knobs are hash-exempt while showing file values; knobs follow the timeline only while the panel is open; transform knobs show the decomposed world matrix and become a local offset under Edit Transform; whole-frame sampling (sub-frame samples lost); shutter/exposure/stereo not copied. USDRead's file handling moved into a shared SceneFileSource. Open: user to confirm flattened world transform is what artists expect.
