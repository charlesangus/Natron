# M625 - Native Geometry Viewer Stage Readers

Member of F41 - Native Geometry Scene Index. Several viewer features find native geometry by reading the UsdStage. Once M624 - Native Geometry Viewer Cutover takes native geometry off the Storm stage, they would see nothing. Give each one a native source, read from the shown GeoDetail and its GeoUsdConvert, and move the GeoDisplay guides off their Sdf layer. Background: `PLAN/DESIGN/2026-11-26-m617-research/bypass-feasibility.md` §1 (the rows marked **S**).

## Scope

**In:**
- Framing (F, frame selection) and stand-in boxes from native bounds.
- The display highlight of selected points and faces.
- Prim-level transform handles and their drag preview on native prims, as an xform override in GeoSceneIndex.
- The GeoDisplay guides (markers, normals, packed bounds, group highlight) served from an `HdRetainedSceneIndex` and rebuilt only when their inputs change.

**Out:**
- Outliner, prim inspector, prim picker and match search for native prims: none (user decision recorded in M624 - Native Geometry Viewer Cutover).
- Camera and light guides: native geometry has none.

**Edge cases:**
- A view showing both a USD scene and native geometry frames the union of both.
- Guides on an empty detail draw nothing and send no notices.

## Phase 625.1: Native readers

- [ ] M625.P1.T1 — Framing and stand-in boxes from native bounds
  - files: `Gui/SceneViewport.cpp` (framing ~5437-5488), `Gui/SceneDrawBoxes.cpp` (~115-158, 642-687), `Engine/Nodes/Geometry/GeoPacked.*` / `geoDetailBounds`
  - approach: when the view shows native geometry (`getShownGeometry`), take bounds from the detail and its packed bounds, and union them with the stage's `UsdGeomBBoxCache` result for any USD in the view. Stand-in boxes use the same native bounds per partition. Padding points by width comes from M627 - Native Geometry Viewer Cook Fixes.
  - verify: a GUI script under Xvfb frames a GeoScatter (F) and a Box, and checks the camera fits their bounds. A forced over-budget frame shows native stand-in boxes.
  - size: M
- [ ] M625.P1.T2 — Display highlight from the detail
  - files: `Gui/SceneViewport.cpp` (~3393-3480), `Gui/GeoDisplay.cpp` / `Engine/Nodes/Geometry/GeoViewerLayer.cpp` (component highlights)
  - approach: resolve selected points and faces to world positions from the GeoDetail and GeoUsdConvert's element numbering, instead of `UsdGeomPointBased` and `UsdGeomXformCache`.
  - verify: the m559 point-select GUI script and a face-select check draw the highlight at the selected elements.
  - size: M
- [ ] M625.P1.T3 — Prim handles and drag preview through a scene-index xform override
  - files: `Engine/Nodes/Geometry/GeoSceneIndex.h/.cpp`, `Engine/Nodes/Scene/SceneStage.cpp` (`setPreviewTransforms` ~428-518), `Gui/SceneTransformWidget.cpp`, `Gui/SceneGizmo.cpp` (~266-297), `Gui/SceneViewport.cpp` (~2961-3020, 5860-5899)
  - approach: add `GeoSceneIndex::setPreviewTransform(path, matrix)`, which overrides `xform` and dirties only that locator. Handles take a native prim's frame from the detail. `setPreviewTransforms` keeps authoring on the root layer for USD prims and calls the override for native ones.
  - verify: a GeoSceneIndex_Test case checks that the override dirties only `xform`. A GUI script drags a prim handle on a native prim and checks the preview moves and the committed value lands.
  - size: M
- [ ] M625.P1.T4 — GeoDisplay guides from a retained scene index
  - files: `Gui/GeoDisplay.cpp` (guide build ~160-505, 631-655, 862-900), `Gui/SceneSelectionTasks.cpp` (merge pattern ~336-346)
  - approach: build markers, normals, packed bounds and group highlights as `HdRetainedSceneIndex` prims with guide purpose, merged like `SceneSelectionTasks::append`. Rebuild a guide only when its inputs (its attribute or the display options) change, not on every new detail. Drop the anonymous guide Sdf layer and its TfNotice resync counter.
  - verify: the existing GeoDisplay GUI checks pass. A test shows that a pscale-only change rebuilds no guide whose input was not pscale.
  - size: L

**Verification gate:** ctest and the framing, highlight, handle and GeoDisplay GUI scripts pass under Xvfb on Storm. PR CI is green against the F41 - Native Geometry Scene Index branch.

## Decisions
