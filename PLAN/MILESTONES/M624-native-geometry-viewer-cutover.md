# M624 - Native Geometry Viewer Cutover

Member of F41 - Native Geometry Scene Index. Wire GeoSceneIndex (M622, M623) into the Storm viewer engine, make it the only way Storm draws native geometry, keep picking working, and delete the staged Storm path. Cycles and MoonRay viewers keep serving the `natrongeo` layer. Background: `PLAN/DESIGN/2026-11-26-m617-research/bypass-feasibility.md` §0-§2.

## Scope

**In:**
- The scene index is merged into the Storm engine's chain inside `SceneDrawFilter::appendFilter`, the first of our renderer plugins. The order among our InsertionOrderAtStart plugins is undocumented (`SceneDrawFilter.cpp` ~259-262), so everything downstream (draw filter, selection and id tasks, Storm filters) sees the prims.
- ViewerInstance and GeoViewerLayer serve the detail (with its GeoUsdConvert) to Storm and the `natrongeo` layer to Cycles and MoonRay. Switching delegate rebuilds the scene once.
- Prim, marquee and component picking, and the pick → GeoDetail mapping.
- Removal of the staged Storm path for native geometry. GeoSdfFileFormat and GeoLayers stay for GeoToUSD, export and the non-Storm delegates.

**Out:**
- Framing, stand-in boxes, display highlight, prim handles and guides: M625 - Native Geometry Viewer Stage Readers.
- The cook-ordering and failed-cook bugs: M627 - Native Geometry Viewer Cook Fixes.
- Native prims in the Scene Graph outliner, prim inspector, prim picker and match search: none. Native geometry is not USD until it is converted, and the Geometry Spreadsheet is its inspector.

**Edge cases:**
- A viewer switching Storm → Cycles → Storm mid-drag ends on the latest detail in both.
- A view holding both native geometry and a USD scene (two inputs or wipe) draws both, with native geometry from the scene index and USD from the stage.

**Assumptions:**
- Whole-prim `TestIntersection` resolves native prims through the primOrigin set in M622.P1.T1. If it doesn't in 26.08, prim picks go through the pick task directly, as `SelectionEngine::testComponentIntersection` already does.

## Phase 624.1: Wiring

- [ ] M624.P1.T1 — Merge GeoSceneIndex into the Storm engine chain
  - files: `Engine/Nodes/Scene/SceneDrawFilter.cpp/.h`, `Gui/SceneViewport.cpp` (engine construction and arming, ~2059-2112), `Engine/Nodes/Geometry/GeoSceneIndex.h`
  - approach: add a thread-local arming scope like `StormIdTasks::Arming` (`StormIdPass.cpp` ~446-466) that carries the view's GeoSceneIndex into the engine constructor. Inside `SceneDrawFilter::appendFilter`, merge it with `HdMergingSceneIndex` under the engine's `_sceneDelegateId` prefix, so the prims are inside the render and selection collection roots. Arm it only for Storm engines. Updates take the same stage lock that Hydra sync takes.
  - verify: a ctest that constructs a Storm engine with an armed GeoSceneIndex and a Box detail, renders one frame offscreen under Xvfb and finds the box's prim in the render index (`GetRprimIds`), plus the M622 tests.
  - size: L
- [ ] M624.P1.T2 — Serve details to Storm and layers to Cycles and MoonRay
  - files: `Engine/ViewerInstance.cpp` (`evaluateGeometryInput` ~4041-4074), `Engine/Nodes/Geometry/GeoViewerLayer.cpp/.h`, `Gui/SceneViewport.cpp` (~3999-4005, 4129), `Engine/Nodes/Geometry/GeoEdit.cpp`, `Engine/Nodes/Geometry/GeoScatter.cpp`
  - approach: the scene payload for native geometry carries the detail and its GeoUsdConvert. The viewer calls `GeoSceneIndex::update` for Storm, and builds the `natrongeo` layer only while the viewer's delegate is Cycles or MoonRay (the ProgressiveRender worker snapshot path, `HydraRenderImpl.cpp` ~3793, is unchanged). The `refreshLayers` and `findLayers` callers in GeoEdit and GeoScatter feed the scene index on Storm.
  - verify: `GeoViewerLayer_Test` is updated for the new payload. A GUI script under Xvfb shows a GeoScatter on Storm, switches to Cycles and back, and checks pixels show the points in both.
  - size: L
- [ ] M624.P1.T3 — Picking on scene-index prims
  - files: `Gui/SceneViewport.cpp` (prim and marquee pick ~6491-6676), `Gui/SelectionEngine.cpp`, `Engine/Nodes/Geometry/GeoViewerLayer.cpp` (pick mapping ~361-635), `Tests/gui/m361_geo_pick.py`, `Tests/gui/m361_geo_selection.py`, `Tests/gui/m559_geo_point_select.py`
  - approach: check in the 26.08 `usdImagingGL/engine.cpp` how `TestIntersection` maps hits in scene-index mode. Use primOrigin if that resolves native prims, else route prim and marquee picks through the pick task directly. Component picks and the GeoDetail mapping keep their render-index paths unchanged.
  - verify: the m361 and m559 pick and selection GUI scripts pass on Storm.
  - size: M

## Phase 624.2: Remove the staged Storm path

- [ ] M624.P2.T1 — Delete staged native geometry from the Storm viewer
  - files: `Engine/ViewerInstance.cpp`, `Engine/Nodes/Geometry/GeoViewerLayer.cpp/.h`, `Gui/SceneViewport.cpp`, `Tests/gui/m559_geo_drag_keeps_frame.py`
  - approach: remove every branch where the Storm viewer sublayers a `natrongeo` layer for native geometry, plus their dead helpers. Keep GeoSdfFileFormat and GeoLayers for GeoToUSD, export and the delegate path. Add no fallback switch.
  - verify: grep shows no Storm-viewer use of the `natrongeo` layer. ctest and the m361 and m559 GUI scripts pass. A Storm GeoTransform drag on a Box logs zero USD resyncs (the viewer's resync counter).
  - size: M

**Verification gate:** ctest, the m361 and m559 GUI scripts and the M624.P1.T2 delegate-switch script pass under Xvfb on Storm. PR CI is green against the F41 - Native Geometry Scene Index branch.

## Decisions
- 2026-11-26 — Remove the staged Storm path, keep a test-only parity oracle (user): one Storm path, with parity checked by `GeoSceneIndexParity_Test`. An env-var fallback was rejected.
- 2026-11-26 — Cycles and MoonRay viewers keep the staged layer (user): their render time dwarfs USD composition, and a thread-safe native snapshot for the worker engine (about 1-1.5 weeks) buys little. Both paths are built from GeoUsdConvert.
- 2026-11-26 — No native prims in the USD panels (user): "native geo is native geo, usd inspectors don't make sense over native geo until it's converted." The Geometry Spreadsheet is the inspector.
