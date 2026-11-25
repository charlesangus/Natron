# M604 - GeoTransform Viewer Handles

Bring the 3D transform tools to native geometry. A **node handle API** lets a geometry node declare a transform frame bound to its knobs, the way the reference app's node handles work. The viewer draws the M603 - Combined Transform Handle gizmo (or a single-mode one) on that frame, and drags write the knobs. GeoTransform, GeoUVProject (its projection frame) and GeoPrimitive (size via scale boxes) are the first users. Which node a drag edits follows the existing `Settings::Viewer3DTransformTargetEnum` (`Engine/Settings.h` ~:97). Owner edits the viewed or nearest upstream GeoTransform. Open Edit edits the GeoTransform whose panel is open. Always Edit creates a GeoTransform after the viewed node, with `group`/`groupType` filled from the current component selection. Today GeoTransform has no handles at all (`Engine/Nodes/Geometry/GeoTransform.cpp` ~:532-619 is knobs only).

Member of F40 - 3D Viewer Tools.

## Scope

**In:**
- Node handle API: a geometry node returns zero or more handle frames (origin, orientation, which handle kinds are allowed, and the knob bound to each part), evaluated from its knobs at the current time and its input's bounds. A drag maps back to knob values with undo and auto-key.
- GeoTransform: translate/rotate/scale/uniformScale/pivot bound to the gizmo, frame at `pivot` (or the group centroid when `centrePivot` is on), with `rotateOrder` honoured and `invertTransform` showing the inverted frame.
- GeoUVProject: `translate`/`rotate`/`scale` of its projection frame, drawn with the projection shape (plane, box, cylinder or sphere outline per `projection`).
- GeoPrimitive: the scale boxes drive the size knob of the current `type` (`cardSize`, `gridSize`, `cubeSize`, `sphereRadius`, `cylinderRadius`/`cylinderHeight`), with no translate or rotate parts, since it has no such knobs.
- Target resolution for geometry, mirroring the USD setting (Owner / Open Edit / Always Edit).
- Each node's viewer-context options (for example GeoTransform's Group and Centre Pivot) show in M602 - 3D Viewer Toolbars' tool-options area.

**Out:**
- Handles for every other geometry node (M613 - Geometry Node Handles Rollout).
- Component-level edits (M605 - GeoEdit Component Transforms).

**Edge cases:**
- Owner mode with a chain of several GeoTransforms upstream: the nearest upstream GeoTransform whose `group` matches the current selection (or that has an empty group) is the owner. With none, nothing is editable and a status message says so.
- Always Edit with no component selection creates a GeoTransform with an empty group (the whole detail).
- An animated knob auto-keys; an expression-driven knob is read-only and the handle part draws greyed out.
- Empty geometry: GeoTransform's frame falls back to its `pivot` knob.

**Assumptions:**
- Unconfirmed: the geometry view's handles reuse `SceneTransformDrag`/`computeDragChange` unchanged, with only the commit path differing (knobs, not prim xforms).

## Phase 604.1: Node handle API

- [ ] M604.P1.T1 — Node handle API and its viewer plumbing
  - files: new `Engine/Nodes/Geometry/GeoNodeHandles.{h,cpp}`, `Engine/Nodes/NodeSpec.h` (~:135, next to `geoGroupKnob`), `Gui/SceneViewport.cpp` (`refreshTransformHandles` ~:12595, `commitOpHandleDrag` ~:13500), `Gui/SceneTransformTarget.{h,cpp}`, `Engine/CMakeLists.txt`
  - approach: Define a `GeoNodeHandle` (world frame, allowed kinds, per-part knob binding with dimension and scale) and a virtual on the geometry node base that returns them. In the geometry view, `refreshTransformHandles` asks the targeted node for handles and draws the gizmo on each. A drag runs the existing `computeDragChange` and then writes the bound knobs in one undo step, auto-keying animated ones. Keep the USD path untouched.
  - verify: gtest with a test geometry node declaring one frame: a simulated translate change writes the bound knob; an animated knob gets a key; the USD-prim `SceneTransform` tests still pass.
  - size: L
- [ ] M604.P1.T2 — Geometry target resolution for the three target modes
  - files: `Gui/SceneTransformTarget.{h,cpp}` (`findSceneTransformOwner` ~:636), `Gui/SceneSelectionActions.cpp` (~:824-940, group filling), `Engine/Nodes/Geometry/GeoTransform.h`
  - approach: Add `findGeoHandleOwner(viewed, selection)` for the geometry kind. Owner walks upstream to the nearest node with handles whose group matches the selection. Open Edit takes the open panel's node if it has handles. Always Edit creates a GeoTransform after the viewed node and fills `group`/`groupType` from the component selection, reusing `SceneSelectionActions`' group-writing code. The USD resolver stays as is.
  - verify: gtest on a GeoPrimitive → GeoTransform(group a) → GeoTransform(group b) chain: Owner with a selection in b finds the second node; Always Edit creates a node with `group` set from the selection.
  - size: M

## Phase 604.2: First users

- [ ] M604.P2.T1 — GeoTransform handles
  - files: `Engine/Nodes/Geometry/GeoTransform.{h,cpp}`, `Tests/Geometry/GeoTransformHandles_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Return one frame at `pivot`, or at the group centroid when `centrePivot` is on (reuse the centroid code GeoTransform already uses to cook), oriented by `rotate` in `rotateOrder`. Bind translate, rotate, scale and uniformScale, with pivot used by pivot-move mode. Register `group` and `centrePivot` as viewer-context knobs.
  - verify: gtest: a rotate change about Y by 90° writes `rotate.y` = 90 with the frame at the centroid; a pivot move writes `pivot`.
  - size: M
- [ ] M604.P2.T2 — GeoUVProject and GeoPrimitive handles
  - files: `Engine/Nodes/Geometry/GeoUVProject.{h,cpp}`, `Engine/Nodes/Geometry/GeoPrimitive.{h,cpp}`, `Gui/SceneGizmo.cpp` (projection outline), `Tests/Geometry/GeoTransformHandles_Test.cpp`
  - approach: GeoUVProject returns its projection frame (`translate`/`rotate`/`scale`), drawn with an outline per `projection`; it is skipped when a camera path drives the projection. GeoPrimitive returns a scale-only frame at the origin, whose scale boxes map to the current type's size knob (one box per dimension the knob has).
  - verify: gtest: scaling GeoPrimitive's X box by 2 doubles `cubeSize.x`; a GeoUVProject translate writes its `translate`.
  - size: M
- [ ] M604.P2.T3 — GUI check and node docs
  - files: `Tests/gui/m604_geo_handles.py`, `Engine/Nodes/Geometry/docs/GeoTransform.md`, `Engine/Nodes/Geometry/docs/GeoUVProject.md`, `Engine/Nodes/Geometry/docs/GeoPrimitive.md`
  - approach: An Xvfb script builds GeoPrimitive → GeoTransform, views it in 3D and drives T/R/E/Y drags through the viewer's test hooks in each target mode. Document viewer handles in each node's doc, without naming other applications.
  - verify: the script passes under Xvfb at 1920x1080.
  - size: M

**Verification gate:** CI green; release gate green with `--gui` running `Tests/gui/m604_geo_handles.py` and the M603 script; `GeoTransformHandles_Test` passes; screenshots of a GeoTransform handle at a group centroid and of GeoUVProject's projection frame are shared one file per screenshot and collected for F40 - 3D Viewer Tools' UAT.

## Decisions
- 2026-11-25 — Geometry drags follow the existing Viewer 3D Transform Target setting (Owner / Open Edit / Always Edit), with Always Edit filling the group from the selection (user, grill).
- 2026-11-25 — A generic node handle API plus a first batch (GeoTransform, GeoPrimitive, GeoUVProject); the rest follow in M613 - Geometry Node Handles Rollout (user, grill).
- 2026-11-25 — GeoPrimitive has no translate/rotate knobs (`GeoPrimitive.h` ~:40-57), so its handle is scale-only, bound to the size knobs (code finding).
