# M337 - Geometry Spreadsheet

A read-only spreadsheet of per-point, per-vertex, per-primitive and per-detail data, like
Houdini's Geometry Spreadsheet, for both USD prims (points and primvars) and geometry details.
Raised by the user during the scene graph redesign (2026-11-04) as a key part of the native
geometry plan. It sits as a tab in M333 - Prim Inspector's inspector. Member of F15 - Native
Geometry: branches off and merges into `feature/f15-native-geometry`, and the Feature's UAT
covers it.

## Scope

**In:**
- Tabs for points, vertices, primitives and detail, for both `GeoDetail` and USD prims (points and primvars by interpolation).
- One column per tuple component (`P[x]`, `P[y]`, `P[z]`). Array attributes show as a compact cell that expands on hover. Packed primitives show their intrinsics (transform, bounds, kind, source path).
- A virtualised model that reads only the visible rows straight from the pages, so millions of rows cost nothing to show.
- A group-membership filter, a "show selected only" filter, and sorting by column, with the sort index built on a worker.
- Two-way selection sync with the Viewer: rows → `HdSelection` highlighting (geometry) or prim/point highlighting (USD); a Viewer component pick → rows.

**Out:**
- Editing values from the table (none). Edits go through `GeoEdit`, `GeoWrangle` and the USD nodes, as in Houdini.
- A full intrinsics view for every primitive type (area, vertex count and so on) (none).

**Edge cases:**
- A prim inside an unloaded payload has nothing to show; the tab says it's not loaded and doesn't load it.
- Sorting 10M rows: the index builds on a worker while the unsorted table stays usable, and a newer sort or a changed input cancels it.
- Tombstoned details: rows follow dense element numbers, never storage offsets.

Requires M54 - Native Geometry Data Model's page API (`GeoAttribute`, `GeoPagedColumn` in `Engine/Geo/GeoPage.h`, `GeoIndexMap` in `Engine/Geo/GeoIndexMap.h`), packed intrinsics (`Engine/Geo/GeoPacked.h`), groups (`Engine/Geo/GeoGroup.h`) and group patterns (`geoSelectElements`, `Engine/Geo/GeoOpUtils.h`). It also needs M333 - Prim Inspector's tab interface (`ScenePrimInspectorTab` and `ScenePrimInspectorRead` in `Gui/ScenePrimInspector.h`: reads run on the inspector's worker and take the stage lock in short calls), and M376 - Geometry Display's viewer geometry layer (`GeoViewerLayer::getDetail()`/`getServedFrame()`, `Engine/Nodes/Geometry/GeoViewerLayer.h`). Selection sync (Phase 337.4) uses M361 - Geometry Point Editing's pick modes (M361.P1.T1) and live viewer selection (M361.P4.T1), which are not in code yet. USD reads go through `SceneOps` (`Engine/Nodes/Scene/SceneOps.h`), as the Attributes tab's `readAttributes` does. GUI checks are `Tests/gui/m337_*.py`, run remotely with an explicit `--gui`. Linux x86-64 only.

## Phase 337.1: Element tables (engine)

- [ ] M337.P1.T1 — Spreadsheet source interface and the `GeoDetail` source
  - files: `Engine/Geo/GeoSpreadsheet.h`/`.cpp` (new), `Engine/CMakeLists.txt`, `Tests/GeoSpreadsheet_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: a pxr-free `GeoSpreadsheetSource`, an immutable snapshot that is safe to read from any thread. Per class (point, vertex, primitive, detail) it gives a row count and a column list, and two cell reads. `readCells(cls, firstRow, rowCount, columns, out)` returns display text. `sortKey(cls, column, row)` returns a double, a string or an array length. The `GeoDetail` source holds a `GeoDetailConstPtr`. Rows are dense element numbers (row count = `indexMap(cls).size()`). A visible row block is turned into offsets with `forEachLiveRunInPage`/`offsetFromIndex`, never by scanning the column. Columns: one per tuple component, named `x y z w` for vector, point, normal and quaternion tuples up to 4, `r g b a` for colour, and `[i]` otherwise (`P[x]`, `Cd[r]`, `xform[15]`). Strings come through the string table. Array attributes give one compact cell (`(n) [a, b, c, …]`), with the whole value available for the tooltip and the full-value window. The vertex class leads with a `Point` column (`vertexPoint`, then the point number). When packed primitives exist, the primitive class shows `intrinsic:kind`, `intrinsic:transform` (compact cell), `intrinsic:bounds` and `intrinsic:sourcePath` from the `geoPacked*` getters, in place of the raw `packed*` attribute columns. The detail class has one row.
  - verify: unit tests. A grid with `P`, `Cd`, a string and an int array attribute gives the expected column names and cell text. After `deletePoints` on rows 2–4, row 2 shows the old point 5 (dense numbers). Float64 `P` reads at full precision. The vertex `Point` column matches the topology. A `geoAppendPackedGeometry` and a `geoAppendPackedUSD` primitive show kind, transform, bounds and source path. On a 10M-point detail, `readCells` for 50 rows near the end touches only those rows' pages (checked with a page-counting test hook).
  - size: M

- [ ] M337.P1.T2 — USD prim element source, without loading payloads
  - files: `Engine/Nodes/Scene/ScenePrimElements.h`/`.cpp` (new), `Engine/Nodes/Scene/SceneOps.h`/`.cpp`, `Engine/CMakeLists.txt`, `Tests/ScenePrimElements_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: `SceneOps::readPrimElements(stage, path, frame, fps, cancelled)` builds a `GeoSpreadsheetSource` from a prim at the time code `readAttribute` uses. It takes one `Get` per attribute, so the caller can take the stage lock once per attribute. The `VtArray`s are kept by reference count and no values are copied out. If `!prim.IsLoaded()`, or the prim isn't on the stage, it returns a NotLoaded or Missing status and never calls `Load`/`LoadAndUnload`. Class mapping — points: `UsdGeomPointBased` `points`, `normals`/`velocities`/`accelerations`/`widths` with vertex interpolation, and vertex or varying primvars; a `UsdGeomPointInstancer` gives positions, orientations, scales, `protoIndices`, `ids` and velocities. Vertices: faceVarying primvars, plus `Point` from `faceVertexIndices`. Primitives: faces of a mesh or curves of `BasisCurves`, with uniform primvars. Detail: constant primvars. Primvars come from `UsdGeomPrimvarsAPI::GetPrimvars()` by `GetInterpolation()`. An indexed primvar is resolved per visible row (`values[indices[row]]`), never `ComputeFlattened`. If `elementSize` > 1, the cell shows as an array. A primvar whose length doesn't match its class count gets a flagged column with empty cells and a header tooltip saying why. Groups are `UsdGeomSubset`s: face subsets for primitives, point subsets for points.
  - verify: unit tests on an in-memory stage: a mesh with vertex, faceVarying, uniform, constant, indexed and `elementSize` 2 primvars lands each in its class, with the right cells; time-sampled `points` read at two frames differ; a payload prim on a stage opened with `UsdStage::LoadNone` returns NotLoaded, and the stage's load set is unchanged afterwards; a mismatched-length primvar is flagged; face and point subsets list under their classes.
  - size: L

## Phase 337.2: The spreadsheet tab

- [ ] M337.P2.T1 — Virtualised table model
  - files: `Gui/GeoSpreadsheetModel.h`/`.cpp` (new), `Gui/CMakeLists.txt`, `Tests/GeoSpreadsheetModel_Test.cpp`, `Tests/CMakeLists.txt` (`GuiTests`)
  - approach: a `QAbstractTableModel` over one source snapshot, one class and an optional row order (a `shared_ptr<const std::vector<GeoIndex>>`, filled in by Phase 337.3). It doesn't use `QSortFilterProxyModel`, whose mappings are O(n) on the main thread. `data()` reads only the rows asked for, through a small cache of the last visible row block (`readCells` per block). The vertical header shows the element number. `ToolTipRole` shows an array or intrinsic cell's full value. The view uses fixed row heights, and column widths come from a sample of the visible rows only (never `resizeColumnsToContents`). Swapping the snapshot is one model reset. The model counts its cell reads for tests.
  - verify: GuiTests. A 10M-row source shows `rowCount()` 10M, and `data()` on the last row returns the expected text. Painting a 40-row viewport reads at most 40 × columns cells (read counter). A row order maps view row to element number. A tooltip on an array cell holds every element.
  - size: M

- [ ] M337.P2.T2 — Spreadsheet tab in the inspector for USD prims
  - files: `Gui/GeoSpreadsheet.h`/`.cpp` (new), `Gui/ScenePrimInspector.cpp` (`addTab`), `Gui/CMakeLists.txt`, `Tests/gui/m337_spreadsheet_usd.py`
  - approach: a `ScenePrimInspectorTab` titled "Spreadsheet" (object name `sceneGraphSpreadsheet`). It has a class bar ("Points", "Vertices", "Primitives", "Detail", each with its count; object names `sceneGraphSpreadsheetPoints` etc.) over a `QTableView` on M337.P2.T1's model. `read()` runs M337.P1.T2 under `ScenePrimInspectorRead::readStage`, one short call per attribute, and checks `isCancelled()` between calls. It returns a closure that swaps the snapshot in. `readsAtFrame()` is true. NotLoaded shows "Not loaded — load the payload from the scene graph to inspect it" and loads nothing. `setStale` greys the table. Double-clicking a cell opens the inspector's existing full-value window (`sceneGraphFullValue`). The tab mirrors `natronGeoSpreadsheetRows`, `natronGeoSpreadsheetClass` and `natronGeoSpreadsheetCellReads` into properties for scripts. The toolbar fits the inspector at its default width in the 1920x1080 default layout.
  - verify: GUI script. Selecting a `USDRead` mesh prim lists its points with `points[x]`… and its primvars under the right class tabs. A frame change updates time-sampled points. Selecting a prim under an unloaded payload shows the not-loaded text, and the scene graph still reports the payload as unloaded. Hovering an `elementSize` 2 cell shows the full tuple.
  - size: L

- [ ] M337.P2.T3 — Geometry context: the viewed node's `GeoDetail`
  - files: `Engine/ViewerInstance.h`/`.cpp` (`getShownGeometry`), `Gui/GeoSpreadsheet.cpp`, `Gui/SceneGraphPanel.cpp` (hand the tab its `ViewerTab`), `Tests/gui/m337_spreadsheet_geo.py`
  - approach: add `ViewerInstance::getShownGeometry(int textureIndex, GeoDetailConstPtr* detail, int* frame) const`, which returns `geometryLayers[i]->getDetail()` and its served frame under `lastSceneMutex`. When `read.getNode()`'s effect `producesGeometryData()`, the tab shows the whole detail and doesn't need a prim (`readsWithoutPrim()` true in this mode). If the node is the viewer's `getSceneViewedNode()`, it uses the viewer's shown detail, which keeps element numbers identical to what the viewport picks. Otherwise (a pinned node) it calls `evaluateGeometry` on the inspector worker at the read's frame (it supports calls outside a render, `Engine/EffectInstanceRenderGeometry.cpp:107`). A selected geometry-layer prim doesn't narrow the rows.
  - verify: GUI script. Viewing a `GeoPrimitive` grid shows the point, vertex, primitive and detail counts matching the detail. After a `GeoDelete`, rows are numbered densely. A packed `USDToGeo` output shows `intrinsic:*` columns, and hovering `intrinsic:transform` shows all 16 values. Scrubbing the frame on an animated `GeoTransform` updates `P[y]`.
  - size: M

## Phase 337.3: Filters and sort

- [ ] M337.P3.T1 — Cancellable row-order builder
  - files: `Engine/Geo/GeoSpreadsheet.h`/`.cpp`, `Tests/GeoSpreadsheet_Test.cpp`
  - approach: `geoSpreadsheetBuildOrder(source, cls, mask, sortColumn, descending, const std::atomic<bool>& cancelled)` builds the row order. It filters by an element mask (`GeoElementMask` over dense numbers, or none), extracts sort keys with `geoParallelFor`, sorts 64k-row chunks in parallel and merges them. It checks `cancelled` between chunks and merge passes and returns nullopt when cancelled. Equal keys keep element order. Descending reverses the comparison, not the result.
  - verify: unit tests. Ascending and descending orders on float, int, string and array-length columns are correct and stable. A mask leaves only its elements. On 10M rows, setting `cancelled` mid-sort returns within 100 ms (generous; it checks that cancellation is honoured, not speed).
  - size: M

- [ ] M337.P3.T2 — Group filter and column sort in the tab
  - files: `Gui/GeoSpreadsheet.cpp`/`.h`, `Gui/GeoSpreadsheetModel.cpp`, `Tests/gui/m337_spreadsheet_sort.py`
  - approach: a "Group" editable combo box listing the current class's groups (`groupNames(cls)`, or the USD subsets). For a `GeoDetail` it accepts any `geoSelectElements` pattern, and shows a malformed pattern's error inline. A header click sorts, and a second click reverses. Both run on the tab's own worker thread through M337.P3.T1. A generation counter makes a newer sort, a new filter, a new snapshot or a new prim cancel the build under way. The table stays unsorted and usable until the order arrives, with a "Sorting…" label in the toolbar. A new snapshot keeps the sort column and filter and rebuilds against the new data. The tab exposes `natronGeoSpreadsheetSortBusy`, `natronGeoSpreadsheetSortsStarted` and `natronGeoSpreadsheetSortsCancelled`.
  - verify: GUI script on a 3000×3000 grid. Sorting `P[y]` descending puts the top row's value at the maximum. Clicking another header during the build cancels the first (the cancelled count rises). Scrolling works while `SortBusy` is true. The group `0-9 14` shows 11 rows, and a bad pattern shows its error. Changing the upstream `GeoTransform` while a sort builds cancels it and re-sorts the new data.
  - size: M

## Phase 337.4: Selection sync with the Viewer

Interface assumed from M361 - Geometry Point Editing, as its brief states it: M361.P4.T1 gives one accessor on the 3D view for the live geometry selection — a class (points, edges or primitives) and an element set (global point numbers, primitive numbers, or `GeoEdge` point pairs). M361.P1.T1 gives pick modes points, edges and primitives, highlighted through an `HdSelection` in the engine's selection tracker, in the geometry context only. M337 also needs a setter that takes a class and a set and switches the pick mode without converting the set, plus a change notification (a Qt signal). If M361 ships only the getter, M337.P4.T1 adds the setter and signal next to it in `Gui/SceneViewport.h`.

- [ ] M337.P4.T1 — Rows → live viewer selection (geometry)
  - files: `Gui/GeoSpreadsheet.cpp`, `Gui/SceneViewport.h`/`.cpp` (setter and signal, if M361 lacks them), `Tests/gui/m337_selection_rows_to_viewer.py`
  - approach: assumes the M361.P4.T1 accessor above. Selecting rows in the Points or Primitives tab writes that class and set into the live selection, so M361's Tab-create and reselect consume it. The row ranges of the `QItemSelection` are mapped through the row order into a set. The code walks ranges, never indexes, so Ctrl+A on 10M rows is one range. Sets over 1M rows are built on the tab worker. Vertex rows select their points. Detail rows select nothing. An update coming back from the viewer is ignored by a reentrancy guard.
  - verify: GUI script. Selecting points rows 0–9 highlights those ten points, and the M361 accessor returns `{0..9}` in point mode. Selecting all rows of a 4M-point grid keeps the main-thread heartbeat gap under 500 ms. Primitive rows give primitive mode.
  - size: M

- [ ] M337.P4.T2 — Viewer pick → rows
  - files: `Gui/GeoSpreadsheet.cpp`, `Gui/GeoSpreadsheetModel.cpp` (inverse row order), `Tests/gui/m337_selection_viewer_to_rows.py`
  - approach: assumes M361.P1.T1's pick modes and the change signal above. On a selection change, the tab switches to the selection's class: points and primitives directly, and edges show their endpoints in Points (M361.P3.T1 Promote). It selects the matching rows as coalesced ranges and scrolls to the first. With a sort active, it maps elements through an inverse order built with the order. Elements filtered out by the group are reported in the toolbar ("12 selected, 3 hidden by filter").
  - verify: GUI script. Clicking a cube corner in point mode selects and scrolls to that point's row. A marquee of primitives selects their rows in the Primitives tab. An edge pick selects its two endpoint rows. Under a descending sort the right rows are selected.
  - size: M

- [ ] M337.P4.T3 — "Selected Only" filter
  - files: `Gui/GeoSpreadsheet.cpp`, `Tests/gui/m337_selection_viewer_to_rows.py`
  - approach: a "Selected Only" toggle (object name `sceneGraphSpreadsheetSelectedOnly`). In the geometry context it intersects the group mask with the live viewer selection, converted to the tab's class. For a USD prim it keeps the rows highlighted by M337.P4.T4. Selection changes rebuild through M337.P3.T1's cancellable worker.
  - verify: GUI script. Picking three points with Selected Only on shows three rows. Combined with group `0-1`, it shows the intersection. Turning the toggle off restores every row.
  - size: M

- [ ] M337.P4.T4 — USD rows → point and face highlighting in the scene context
  - files: `Gui/GeoSpreadsheet.cpp`, `Gui/SceneViewport.h`/`.cpp`, `Tests/gui/m337_selection_usd.py`
  - approach: assumes M361.P1.T1's `HdSelection` path through the engine's selection tracker. It adds a display-only `SceneViewport::setComponentHighlight(primPath, class, indices)`: `HdSelection::AddPoints` for point rows and `AddElements` for face rows. The highlight is cleared on a prim change. There is no viewer → rows for USD, because scene-context picking stays at prim level (M361's Out list, M232 - Scene Selection To Nodes).
  - verify: GUI script. Selecting 5 point rows of a `USDRead` mesh highlights them (pixel check under Xvfb), and selecting another prim clears it.
  - size: M

**Verification gate:** CI green; unit tests green (M337.P1.T1, M337.P1.T2 and M337.P3.T1 engine tests, and the M337.P2.T1 GuiTests); the remote GUI run, with an explicit `--gui`, passes `m337_spreadsheet_usd.py`, `m337_spreadsheet_geo.py`, `m337_spreadsheet_sort.py`, `m337_selection_rows_to_viewer.py`, `m337_selection_viewer_to_rows.py` and `m337_selection_usd.py`; on a mesh with millions of points (a 3000×3000 grid, viewed both as `GeoDetail` and through `GeoToUSD` as a USD mesh) switching class tabs, scrolling to the last row, sorting `P[y]` and select-all keep a QTimer heartbeat gap under 500 ms and the cell-read counter stays at the visible rows, and a prim under an unloaded payload stays unloaded; Xvfb screenshots (one file each) of the Points, Primitives (packed intrinsics) and Detail tabs, a sort in progress and a row ↔ viewer selection are shared with the user before sign-off, and the toolbar fits the 1920x1080 default layout; the pre-existing ctest suite is green.

## Decisions

- 2026-11-08 — **Read-only, as in Houdini (user, /cat-plan grill).** This settles the earlier Out item about editing.
- 2026-11-08 — **Houdini's layout, virtualised (user):** class tabs, a column per component, expandable array cells, packed intrinsics, group filter and worker-built sort.
- 2026-11-08 — **Selection syncs both ways (user);** this adds M361 - Geometry Point Editing as a dependency.
- 2026-11-08 — **Joined F15 - Native Geometry (user);** stays a stub until its blockers land (user).
- 2026-11-16 — **Worked ahead of unmet Deps (user):** started before F4 - Heavy Scene Viewer lands, stacked on M376 - Geometry Display's branch (F4 and F15 merged in) via M361 - Geometry Point Editing's branch. The PR into F15 opens only after F4 lands, M376 merges into F15, and this branch is rebased onto F15.
