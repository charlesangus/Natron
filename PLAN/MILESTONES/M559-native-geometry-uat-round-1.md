# M559 - Native Geometry UAT Round 1

Fixes from the user's first UAT pass over the F15 - Native Geometry AppImage (2026-11-23), all needed before F15 merges. Member of F15 - Native Geometry.

## Scope

**In:**
- Geometry edge colour in the node graph matches the geometry node colour.
- Dragging a knob no longer greys out the viewer.
- A disabled node on DisplaceGeo's image input passes through instead of erroring "<node> could not be rendered".
- GeoUVProject's `uv` shows in the geometry spreadsheet.
- Reliable point selection: box select picks every point inside the box, through primitives, whatever pscale or normals; no foreground-only filtering.
- The geometry spreadsheet becomes its own panel, out of the Scene Graph panel.
- USDToGeo appears in the node menus' USD-input filter when a USD node or USD viewer selection is current.
- The F15 gate's `geometry_toolbar_menu` GUI timeout.

**Out:**
- USDMerge opinion order (M560 - USDMerge Opinion Order).

## Phase 559.1: Fixes

- [x] M559.P1.T1 — Geometry pipes take the geometry node colour (2704eb91c..83c53985e)
  - files: the node-graph edge drawing (`Gui/Edge.cpp`, the per-kind pipe colour) and the Native 3D colour preference
  - approach: edges carrying geometry use the Native 3D node colour (#803399 preference), as USD pipes use the USD colour
  - verify: GUI script checks a geo→geo edge's pen colour equals the preference
  - size: M
- [x] M559.P1.T2 — Knob drags don't grey out the viewer (2704eb91c..83c53985e)
  - files: viewer stale cue (M451 - Viewer Stale Cue Experiments), geometry viewer refresh
  - approach: find why a knob drag on a geometry chain shows the stale/grey state, and keep the last image or geometry shown while the new one computes, as the 2D viewer does
  - verify: GUI script drags a geo knob and checks the viewer is not in the stale cue state between updates
  - size: M
- [x] M559.P1.T3 — Disabled node on DisplaceGeo's image input (2704eb91c..83c53985e)
  - files: DisplaceGeo's image fetch (`Engine/Nodes/Geometry/`), how USD nodes handle a disabled image source (fixed there earlier)
  - approach: a disabled image source renders through its pass-through input like any disabled node; with no pass-through, treat the input as disconnected. Reuse the USD-side fix
  - verify: ctest or GUI test with Card→DisplaceGeo with a disabled Blur upstream: no error, result matches the enabled-bypass image
  - size: M
- [x] M559.P1.T4 — GeoUVProject's uv in the spreadsheet (2704eb91c..83c53985e)
  - files: GeoUVProject, the geometry spreadsheet model (M337 - Geometry Spreadsheet)
  - approach: Card→GeoTransform→GeoUVProject shows no `uv` column, though GeoScatter with inherit uv does; find whether the attribute lands on another class (vertex vs point) the spreadsheet doesn't show, or the spreadsheet caches the attribute list
  - verify: test that the spreadsheet model lists `uv` after GeoUVProject
  - size: M
- [x] M559.P1.T5 — Box and click point selection selects through (2704eb91c..83c53985e)
  - files: geometry point picking in the viewer (M361 - Geometry Point Editing)
  - approach: box select is a projection test of every point in the box, no depth/occlusion or normal filtering (user 2026-11-23: selecting through primitives is fine, as in other SOP apps); click picking uses screen distance to the point centre, not pscale-sized splats. Fix the high-pscale case and the box-size-dependent subsets
  - verify: GUI script on Card→GeoScatter at high pscale: boxes of different sizes select exactly the points projecting inside them
  - size: L
- [x] M559.P1.T6 — Geometry spreadsheet in its own panel (2704eb91c..83c53985e)
  - files: Scene Graph panel (`Gui/SceneGraphPanel.cpp`), geometry spreadsheet widget, panel registration and pane menu
  - approach: the Scene Graph panel is the USD system's; the geometry spreadsheet moves to its own registered panel (Geometry Spreadsheet) following the viewed geometry node
  - verify: GUI script opens the Geometry Spreadsheet panel and finds no spreadsheet in the Scene Graph panel
  - size: M
- [x] M559.P1.T7 — USDToGeo in the USD-input filter (2704eb91c..83c53985e)
  - files: the node menu / tab-menu USD-input filtering
  - approach: USDToGeo takes a USD input, so it is listed when the filter for USD-input nodes is active (USD node or USD viewer selection current)
  - verify: extend the menu GUI test
  - size: S
- [x] M559.P1.T8 — geometry_toolbar_menu GUI timeout (2704eb91c..83c53985e)
  - files: `Tests/gui/geometry_toolbar_menu.py`
  - approach: run 45247271588 timed out after the colour check (step at script line ~139); find the hang
  - verify: passes at the gate
  - size: M

## Phase 559.2: Gate (PM runs)

- [x] M559.P2.T1 — Remote gate with tests, AppImage and the geometry GUI scripts; new F15 AppImage for UAT round 2 (m559-gate5 green; PR #361)

## Decisions
- 2026-11-23 — Box selection selects through primitives; no foreground-only picking (user).
- 2026-11-23 — Geometry spreadsheet is its own panel, not part of the Scene Graph (user).
- 2026-11-23 — The Geometry Spreadsheet panel shares the Scene Graph's follow viewer / follow selection / pin behaviour; the Scene Graph keeps its own Spreadsheet tab for USD prims (user).
- 2026-11-23 — The missing uv was the Points view. GeoUVProject keeps writing vertex uv by default and gets an option to write point uv instead; no spreadsheet change (user).
