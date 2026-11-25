# M607 - Geometry Marker Controls

Controls for the geometry display's markers, on M602 - 3D Viewer Toolbars' right display bar and in its Display Options popup.
- **Sizes:** a Point Size multiplier on today's automatic world size (0.4% of the bounds diagonal, hard-coded in `Gui/GeoDisplay.cpp` ~:858-862), a Line Size multiplier, Normal Scale, Vector Scale, and Label Size (pixels) for number text.
- **The reference app's core marker set:** vertex markers and numbers, primitive normals, point UV and XYZ coordinates, hulls and backface tint, beside today's point markers, point and primitive numbers, and point normals.
- **Per-marker visibility mode:** Always, Selected Only, or Near Pointer.
- **Display categories:** a "Set Options For" choice of Displayed, Selected, Template or Ghost, so each kind of geometry carries its own marker settings. M363 - Geometry Display Extras' template display builds on this.

Markers stay Hydra guide prims in the viewer-private guide layer (`GeoDisplay::authorGuide`, ~:880), and number and coordinate text stays QPainter labels (`GeoDisplay::paintLabels`), with no second GL pass.

Member of F40 - 3D Viewer Tools.

## Scope

**In:**
- New ViewerInstance knobs beside `pointMarkers`, `normalScale` and the rest (`Engine/ViewerInstance.cpp` ~:1027-1045): `pointSize`, `lineSize`, `vectorScale`, `labelSize`; `vertexMarkers`, `vertexNumbers`, `primitiveNormals`, `pointUVs`, `pointPositions`, `hulls`, `tintBackfaces`; a visibility-mode knob for each marker type.
- Display categories: every marker knob is stored per category (Displayed / Selected / Template / Ghost), and a `markerCategory` choice selects which category the bar and popup edit, plus "Use Displayed Settings" per category. Only Displayed and Selected have geometry until M363 - Geometry Display Extras lands Template display; Ghost has none yet.
- Right display bar buttons for each marker toggle; the Display Options popup holds sizes, visibility modes and the category selector, laid out to fit 1920x1080.
- Label caps per frame are kept (today's cap), applied to vertex numbers and coordinates too.

**Out:**
- Attribute visualisers and template/footprint display (M363 - Geometry Display Extras).
- Point trails, coincident-point markers and UV-overlap markers; no milestone.
- Group highlighting moves to M608 - Geometry Group Picker's Group List.

**Edge cases:**
- Near Pointer needs the pointer position: guide prims are rebuilt for the region near the pointer at most once per frame, and labels use the existing label path.
- Selected Only with no selection shows nothing.
- Vertex markers on seamless meshes are inset towards their primitive centre so they stay pickable, as the reference app's "offset vertex markers" does.
- A 10M-point detail with point markers on stays interactive (sizes change widths only, no re-split).

**Assumptions:**
- Unconfirmed: Near Pointer's radius is a fixed screen distance (about 60 px), not a knob.

## Phase 607.1: Markers

- [ ] M607.P1.T1 — Size multipliers and label size
  - files: `Engine/ViewerInstance.{h,cpp}`, `Gui/GeoDisplay.{h,cpp}` (~:858-910), `Gui/SceneDisplayBar.cpp` (popup), `Tests/gui/m607_marker_controls.py`
  - approach: Add `pointSize`, `lineSize`, `vectorScale` (default 1) and `labelSize` (default today's font size). `markerWidth` and `lineWidth` multiply by them, and the highlighted ×2 rule stays. Changing a size re-authors widths only. Popup spin boxes bind through `editViewerKnobs`.
  - verify: Xvfb script: Point Size 3 triples guide-point widths (read the guide layer's `widths` through a test hook) and label size changes the text height; a 1M-point grid stays interactive while the slider is dragged.
  - size: M
- [ ] M607.P1.T2 — The core marker set
  - files: `Engine/ViewerInstance.{h,cpp}`, `Gui/GeoDisplay.{h,cpp}`, `Gui/SceneDisplayBar.cpp`, `Gui/Resources/Images/`
  - approach: Vertex markers (inset points) and numbers; primitive normals (a line from each primitive centre); point UV and XYZ labels; hulls (polygon outlines, as `basisCurves` guides); backface tint (a guide display-colour override on backfaces, or the renderer's backface option if Storm has one). Each is a guide prim or a label set built like the existing ones, with a bar button.
  - verify: the script turns each on for a GeoPrimitive sphere and checks the guide layer or label list contains it; screenshots of each are taken.
  - size: L
- [ ] M607.P1.T3 — Per-marker visibility mode
  - files: `Engine/ViewerInstance.{h,cpp}`, `Gui/GeoDisplay.{h,cpp}`, `Gui/SceneViewport.cpp` (pointer position to the display), `Gui/SceneDisplayBar.cpp`
  - approach: A `<marker>Visibility` choice (Always / Selected Only / Near Pointer) for each marker type. Selected Only filters by the component selection. Near Pointer filters by projected distance to the pointer, re-evaluated at most once per frame on pointer move.
  - verify: the script with point numbers on Selected Only labels only the selected points; Near Pointer labels only points within the radius of a scripted pointer position.
  - size: M
- [ ] M607.P1.T4 — Display categories
  - files: `Engine/ViewerInstance.{h,cpp}`, `Gui/GeoDisplay.{h,cpp}`, `Gui/SceneDisplayBar.cpp`, `Tests/ViewerMarkerCategories_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Store each marker knob per category, as one knob per category or a multi-dimensional knob, whichever the knob system makes cheaper. Add `markerCategory` and per-category "Use Displayed Settings". `GeoDisplay` resolves settings for each displayed detail by its category: Displayed for the viewed node; Selected for selected components' overrides; Template and Ghost for M363's later use. The bar and popup edit the selected category.
  - verify: gtest: Selected category point size 2 with Displayed 1 gives the selected points twice the width; "Use Displayed Settings" on Template returns the Displayed values.
  - size: M

**Verification gate:** CI green; release gate green with `--gui` running `Tests/gui/m607_marker_controls.py`; `ViewerMarkerCategories_Test` passes; screenshots of the Display Options popup and of a sphere with vertex numbers, hulls and primitive normals are shared one file per screenshot and collected for F40 - 3D Viewer Tools' UAT.

## Decisions
- 2026-11-25 — Marker size is a multiplier on the automatic world size, not pixels: pixel sizing would re-author widths on every camera move (user, grill).
- 2026-11-25 — The core marker set plus display categories; M363 - Geometry Display Extras' template display uses the categories (user, grill).
