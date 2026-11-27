# M622 - Native Geometry Scene Index Core

First member of F41 - Native Geometry Scene Index. Build a Hydra scene index (`HdSceneIndexBase`) that serves native geometry straight from GeoUsdConvert's partitions, so the Storm viewer no longer goes native geometry → Sdf layer → UsdStage → UsdImaging. This milestone builds the scene index and its change diffing, and tests them in isolation. It is not wired into the viewer yet; M624 - Native Geometry Viewer Cutover does that.

Background: `PLAN/DESIGN/2026-11-26-m617-research/` (bypass-feasibility.md §0-§3 has the consumer inventory and the plumbing plan; blender-omniverse.md, houdini.md and gaffer-katana.md cover how Blender, Omniverse, Houdini, Gaffer and Katana avoid staging native data as USD). Line numbers below are as of 11e3a1b115 (F15/F18 code) and will have moved by the time F15 - Native Geometry lands.

## Scope

**In:**
- `GeoSceneIndex`: an `HdSceneIndexBase` built from a GeoUsdConvert. It covers mesh, basisCurves and points prims, flattened `xform`, `visibility`, `purpose`, `extent`, primvars with their interpolation (points, normals, uvs, displayColor from Cd, widths from pscale, and every other attribute GeoUsdConvert maps), geomSubsets for primitive groups, and an `HdPrimOriginSchema` scenePath on every prim.
- Diffing that emits PrimsAdded, PrimsRemoved and PrimsDirtied with exact locators: points-only → `primvars/points`, one primvar → that primvar, topology → `mesh/topology` (or the curves/points equivalent), path or type churn → remove and add.
- A test-only parity oracle that compares the scene index with UsdImaging's stage scene index over the `natrongeo` layer for the same detail.

**Out:**
- Instancers, native instances and packed prims: M623 - Native Geometry Scene Index Instancing.
- Viewer wiring, picking and removing the staged Storm path: M624 - Native Geometry Viewer Cutover.
- Stage-reading GUI features (framing, boxes, highlight, handles, guides): M625 - Native Geometry Viewer Stage Readers.
- Final render, USDWrite, GeoToUSD and native geometry composed onto USD prims, which all stay on GeoUsdConvert's USD path (viewer only).

**Edge cases:**
- An empty detail gives an empty scene index, with no prims under the root, rather than an error.
- A detail whose partitions reorder keeps prim paths stable wherever GeoUsdConvert's names are unchanged. Paths come from GeoUsdConvert, never from the order of a list.

**Assumptions:**
- GeoUsdConvert's `partitionValues()` VtArrays can be handed to Hydra data sources without a copy.

## Phase 622.1: Scene index

- [x] M622.P1.T1 — Build GeoSceneIndex prims and data sources from GeoUsdConvert
  - files: new `Engine/Nodes/Geometry/GeoSceneIndex.h/.cpp`, `Engine/Nodes/Geometry/GeoUsdConvert.h` (expose partition iteration and values if not already public), the Engine CMake source list, new `Tests/Scene/GeoSceneIndex_Test.cpp`
  - approach: follow `GeoUsdConvert`'s partitioning, paths and attribute→primvar mapping (`GeoUsdConvert.cpp` ~1170-1530 and 1237-1246 for displayColor and widths). Serve the values copy-free from `partitionValues()`. Flatten `xformOp:transform` into `HdXformSchema` with `resetXformStack`. Author `purpose`, `visibility`, `extent` and `HdPrimOriginSchema` (scenePath = own path). Prims sit under a caller-supplied prefix. Mesh, basisCurves and points only: instancing partitions are skipped, because M623 owns them. Build data sources with the USD 26.08 Hd schema builders (`HdMeshSchema`, `HdPrimvarsSchema`, …; headers are in the dev image), not hand-rolled containers.
  - verify: GeoSceneIndex_Test builds the scene index from a Box, a GeoScatter-style points detail, a curves detail and a detail with primitive groups. It checks prim types, paths, points, topology, primvar interpolation, xform, extent, subsets and primOrigin through `GetPrim`. The test is registered in ctest.
  - size: L
- [x] M622.P1.T2 — Diff successive details into exact Hydra notices
  - files: `Engine/Nodes/Geometry/GeoSceneIndex.h/.cpp`, `Tests/Scene/GeoSceneIndex_Test.cpp`
  - approach: `update(convert)` compares against the previous convert using `adoptUnchangedPartitions` and `partitionSignature` (the logic `GeoLayers::refresh` uses today, `GeoSdfFileFormat.cpp` ~437-455 and 591-735). It sends PrimsAdded, PrimsRemoved and PrimsDirtied with the narrowest locator. Swap in the new data atomically, so a Hydra sync never sees half an update. Hydra syncs under the stage lock, as `SceneDrawFilter.h` ~95-100 documents for the draw filter.
  - verify: a recording observer in GeoSceneIndex_Test sees only `primvars/points` dirtied for a transform-only change, only `primvars/widths` for a pscale change, a topology locator for a point-count change, and remove+add only for a path change. No notices are sent for an identical detail.
  - size: M
- [x] M622.P1.T3 — Parity oracle against UsdImaging over the natrongeo layer
  - files: new `Tests/Scene/GeoSceneIndexParity_Test.cpp`, ctest registration
  - approach: for each fixture detail, open the `natrongeo` layer in a UsdStage and build UsdImaging's stage scene index over it (`UsdImagingCreateSceneIndices` or `UsdImagingStageSceneIndex`, whichever 26.08 exposes). Walk both trees and compare prim by prim: type, path, points, topology, primvars with interpolation, world xform, extent and subsets. The comparison helper is reused by M623 for instancing fixtures.
  - verify: the parity test passes on the M622.P1.T1 fixtures. A deliberately mismatched primvar (temporary local edit, not committed) makes it fail with the prim path and field in the message.
  - size: M

**Verification gate:** `GeoSceneIndex_Test`, `GeoSceneIndexParity_Test` and the existing `GeoUsdConvert_Test` and `GeoViewerLayer_Test` pass in ctest. PR CI is green against the F41 - Native Geometry Scene Index branch.

## Decisions
- 2026-11-26 — Built from GeoUsdConvert (user): the viewer and export share one interpretation of the geometry, so parity risk stays low. Building straight from GeoDetail was rejected.
- 2026-11-26 — Viewer only (user): final render, USDWrite and GeoToUSD keep authoring USD through GeoUsdConvert. Native geometry composed onto USD prims needs composition and stays staged.
