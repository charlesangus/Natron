# M605 - GeoEdit Component Transforms

GeoEdit's viewer edits go beyond today's single-axis point drag (`writeComponentDrag`, `Gui/SceneViewport.cpp` ~:13530, calling `GeoEdit::translate`). Selected points, edges, primitives and packed primitives get translate, rotate, scale and the M603 - Combined Transform Handle gizmo. Transform spaces come from the reference app's Edit node: Global; Local Frame by Component, by Normal, or by Oriented Bounding Box; and Local Frame by Connectivity, which edits each island in its own frame. There is also a soft radius with Linear/Quadratic/Cubic/Metaball falloff, and Peak along normals. As now, the edits bake into GeoEdit's point offsets and packed transforms (`GeoEditData`, `Engine/Nodes/Geometry/GeoEdit.h` ~:56), so the node stays cumulative and not animatable.

Member of F40 - 3D Viewer Tools.

## Scope

**In:**
- Rotate, scale and combined drags on component selections, baked into `GeoEditData` offsets (points) and left-multiplied transforms (packed primitives).
- Transform Space: Global / Local Frame / Local Frame by Connectivity, with Local Frame Method: Component / Normal / Oriented Bounding Box. The handle snaps to the most recently selected island in By Connectivity mode.
- Soft Radius with Falloff: Linear / Quadratic / Cubic / Metaball, previewed by colouring the affected points during the drag.
- Peak: move along point normals (N, or computed normals when N is absent), as a drag on a dedicated handle part.
- Each option is a GeoEdit knob, so it is saved, and is shown in M602 - 3D Viewer Toolbars' tool-options area.

**Out:**
- Slide on Surface and the reference-geometry input; no milestone.
- Animated component edits; GeoEdit stays cumulative; no milestone.
- Snapping (M606 - Viewer Snapping).

**Edge cases:**
- Edge and primitive selections move their points; a point shared by two islands in By Connectivity mode belongs to one island, since connectivity groups points.
- The soft radius reaches points outside the selection with a falloff weight; a radius of 0 means selection only.
- The detail's counts change upstream after edits: existing behaviour (edits not applied, `GeoEditData` ~:54) is kept and the node message says why.
- Degenerate local frames (a single point with no normal) fall back to Global.

**Assumptions:**
- Unconfirmed: soft-falloff distance is Euclidean, not along the surface, and matches the reference default.

## Phase 605.1: Transforms

- [ ] M605.P1.T1 — Rotate and scale component edits in GeoEdit's data model
  - files: `Engine/Nodes/Geometry/GeoEdit.{h,cpp}`, `Tests/Geometry/GeoEdit_Test.cpp`
  - approach: Add `GeoEdit::transform(selection, matrix, pivot frame, weights)` next to `translate`. It folds a full 4x4 into point offsets (P' = M·P − P, accumulated) and into packed-primitive transforms. Keep the encode/decode format readable and exact (`GeoEditData::encode`), with no old-format import.
  - verify: gtest: a 90° rotate about a pivot moves the selected points exactly and round-trips through encode/decode; a weighted transform scales the offsets by the weights.
  - size: M
- [ ] M605.P1.T2 — Transform spaces and local frames, including by connectivity
  - files: `Engine/Nodes/Geometry/GeoEdit.{h,cpp}`, `Engine/Geo/GeoOpUtils.{h,cpp}` (connectivity helper if none exists), `Tests/Geometry/GeoEdit_Test.cpp`
  - approach: Knobs `transformSpace` (Global/Local Frame/Local Frame by Connectivity) and `localFrameMethod` (Component/Normal/Oriented Bounding Box). Compute one frame for the selection, or one per connected island. In By Connectivity mode a drag applies the same local-space change in each island's frame. Reuse an existing connectivity routine if `Engine/Geo` has one; otherwise add a union-find over primitive vertices.
  - verify: gtest: two separate cubes selected, By Connectivity, scale 2 → each scales about its own centre; Oriented Bounding Box on a rotated box gives its axes.
  - size: M
- [ ] M605.P1.T3 — Soft radius, falloff and Peak
  - files: `Engine/Nodes/Geometry/GeoEdit.{h,cpp}`, `Gui/GeoDisplay.cpp` (falloff colouring), `Tests/Geometry/GeoEdit_Test.cpp`
  - approach: Knobs `softRadius` and `falloff` (Linear/Quadratic/Cubic/Metaball). Weights by Euclidean distance to the nearest selected point; use a spatial grid for large details. Peak is a translate along each point's normal by the drag amount, weighted the same way. During a drag, the viewer colours weighted points through the guide layer.
  - verify: gtest: radius 1, Linear, a point at distance 0.5 moves half as far; Peak moves points along N.
  - size: M
- [ ] M605.P1.T4 — Viewer drags for every tool on components
  - files: `Gui/SceneViewport.cpp` (`componentBase`/`componentTargets`/`findComponentEdit` ~:8196-8311, `writeComponentDrag` ~:13530), `Gui/SceneTransformWidget.cpp`, `Tests/gui/m605_geoedit_transforms.py`
  - approach: Replace the single-axis path with the full gizmo for components. The frame comes from T2's space settings and drags call `GeoEdit::transform` with T3's weights. Register GeoEdit's new knobs as viewer-context knobs.
  - verify: Xvfb script: select points on a grid, rotate with R, scale with E, and check the GeoEdit `edits` change and the displayed points; By Connectivity with two islands scales both about their own centres.
  - size: M

**Verification gate:** CI green; release gate green with `--gui` running `Tests/gui/m605_geoedit_transforms.py` and the M361 - Geometry Point Editing scripts; `GeoEdit_Test` passes; screenshots of a soft-falloff drag and a By Connectivity scale are shared one file per screenshot and collected for F40 - 3D Viewer Tools' UAT.

## Decisions
- 2026-11-25 — Takes the reference Edit node's transform spaces, soft falloff and Peak; Slide on Surface and the reference input stay out (user, grill).
- 2026-11-25 — Rotate/scale bake into GeoEdit's offsets, matching the reference node's cumulative, non-animatable model (code finding: `GeoEditData` already stores baked offsets).
