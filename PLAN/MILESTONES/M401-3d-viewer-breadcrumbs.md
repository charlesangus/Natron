# M401 - 3D Viewer Breadcrumbs

A clickable breadcrumb bar above the Viewer's 3D view replaces the selected-path HUD text
(`Gui/SceneViewport.cpp` paint code, `" | /path"`). Each segment selects that ancestor; a
chevron after a segment lists its children and a right-click lists its siblings; segments drag
onto path and pattern knobs; a "+N" badge opens every selected path, grouped by level; Alt+arrow
keys walk the selection. Member of F4 - Heavy Scene Viewer, so it builds on M328 - Deferred
Viewer Stage's lazy loading and M332 - Scene Graph Points' multiple panels.

## Scope

**In:**
- A breadcrumb bar widget above `SceneViewport`, shown whenever the 3D view is, fed by `SceneViewport::selectionChanged`. The path text leaves the HUD; renderer, time and samples stay.
- Click a segment: the selection becomes that prim. Shift-click adds it. Click `/`: the selection clears.
- A chevron after each segment (and after `/`) opens that prim's children; right-click a segment opens its siblings. Choosing an entry selects it.
- Context menu on a segment: Siblings, Copy Path, Copy Selected Paths, Frame, Show In Scene Graph. Double-click selects and frames.
- Segment tooltips: type, kind, loaded state.
- Dragging a segment carries its path, and dragging the "+N" badge carries every selected path, as `application/x-natron-prim-paths` (`SceneGraphModel::primPathsMimeType()`), so existing knob drop targets work unchanged.
- The "+N" badge opens a dropdown with one breadcrumb row per selected prim and a Level spinner (default off) that collapses rows to their ancestor at that depth with a count.
- Pickwalk shortcuts, rebindable in the shortcut editor: Alt+Left parent, Alt+Right child, Alt+Up / Alt+Down previous / next sibling.

**Out:**
- Knob-side prim tools (selection menu, picker popup, armed pick): M334 - Knob Prim Tools.
- Draw-state editing from the bar: M329 - Viewer Draw States.
- Editing prims (rename, delete, reparent) from the bar: none.
- A breadcrumb for the 2D viewer: none.

**Edge cases:**
- Several prims selected: the bar shows the current (last) path plus "+N"; clicking a segment replaces the selection with that one prim.
- Selected path no longer exists after a graph edit: every segment stays visible, missing ones are greyed and not clickable, and Copy Path still works.
- Path too long for the bar at 1920x1080: the middle segments collapse into a "…" button that lists them, and the first and last segments stay visible.
- Empty selection: the bar stays visible with only the `/` crumb, whose chevron lists top-level prims. The layout doesn't jump.
- Chevron on a prim whose payload isn't loaded: the popup shows a "Not loaded" row with a Load button (`SceneViewport::requestLoad(paths, false)`) and fills in when the load lands.
- A prim with a very large number of children: the popup is a lazy, filterable list, with no cap on the number of entries and no main-thread stall from listing them.
- Children and siblings are listed with the Scene Graph panel's `childPredicate()` (active, defined, non-abstract, instance proxies included), so the bar and panel always agree.
- No stage yet, or a sync in progress: the bar shows the path text, and popups open once `readStage` yields a stage.
- Pickwalk with several prims selected: every prim walks and duplicates merge. Alt+Right returns to the child last walked up from, else the first child. Siblings wrap at the ends. Parent of a top-level prim and child of a leaf leave that prim where it is.
- Show In Scene Graph reveals the prim in a panel following this viewer. If every panel is pinned elsewhere, it opens a new panel following this viewer and never changes a pin. If M332 - Scene Graph Points ends up without multiple panels, it instead asks whether to unpin the panel, and does nothing on No.

**Assumptions:**
- M332 - Scene Graph Points gives several Scene Graph panels, each following the viewer, the node selection or a pin, with a way to open a new one that follows the viewer. If it doesn't, the ask-to-unpin fallback applies (see Edge cases).
- `SceneViewport::requestLoad` (on F4 - Heavy Scene Viewer's branch, from M328 - Deferred Viewer Stage) keeps its `(paths, withDescendants)` shape.

## Decisions

- 2026-11-12 — Bar above the 3D view, replacing the HUD path text: it's a real widget, so clicks, drag and menus are straightforward (user).
- 2026-11-12 — Chevron for children, right-click for siblings, in the file-browser style (user).
- 2026-11-12 — Multi-selection shows the current path plus a "+N" dropdown of rows with a Level grouping spinner. Clicking selects that one prim (user).
- 2026-11-12 — Extras: Show In Scene Graph, pickwalk, Copy Path, tooltips, double-click to frame (user).
- 2026-11-12 — Joins F4 - Heavy Scene Viewer and depends on M332 - Scene Graph Points. Show In Scene Graph never touches a pin, and main has only one panel until M332 lands (user; this reverses an earlier "ship on its own").
- 2026-11-12 — If M332 - Scene Graph Points' multiple panels don't pan out, Show In Scene Graph may ask to unpin the single panel (user).
- 2026-11-12 — Pickwalk keys follow the bar's left-to-right layout: Left/Right for parent/child, Up/Down for siblings. All selected prims walk, Down/Right remembers the last child, siblings wrap (user).
- 2026-11-12 — Unloaded payloads show "Not loaded" plus Load in the chevron popup (user).
- 2026-11-12 — Missing segments are greyed, not trimmed; long paths elide the middle; an empty selection keeps the bar with a `/` crumb, and clicking `/` clears the selection (user).
- 2026-11-12 — A segment drags one path, the badge drags all, a dropdown row drags its row (user).
- 2026-11-12 — Plain Left/Right and Up/Down arrows are already bound to Player and NodeGraph shortcuts (`Gui/GuiApplicationManager10.cpp:909-930`), and Alt+arrows are free, so pickwalk uses Alt (code finding).

## Phase 401.1: The bar

- [ ] M401.P1.T1 — Breadcrumb bar above the 3D view
  - files: new `Gui/SceneBreadcrumbBar.{h,cpp}`, `Gui/CMakeLists.txt`, `Gui/ViewerTab.cpp` (`viewerSubContainerLayout`, near the `SceneViewport` insertion), `Gui/SceneViewport.cpp` (HUD path text), new `Tests/gui/scene_breadcrumbs.py`
  - approach: a `QWidget` strip inserted above the `SceneViewport` and shown and hidden with it. It listens to `selectionChanged` and renders the current (last) path as `/` plus one flat button per segment, with a "+N" badge when several prims are selected (the badge does nothing until M401.P2.T1). Click calls `select({path}, Replace)`, Shift-click calls `Add`, and `/` clears. Each refresh checks which segments exist through `readStage` and greys missing ones. When the bar is too narrow, the middle segments collapse into a "…" button whose menu lists them. Remove only the path part of the HUD string (`SceneViewport.cpp` ~5186-5194). Keep a test hook that returns the segment texts and states.
  - verify: GUI script under Xvfb — select `/geo/group_1/a_thing/mesh` (build the hierarchy with the existing scene fixtures), check that the segments read `geo, group_1, a_thing, mesh`; click `group_1` and the viewport selection is `/geo/group_1`; Shift-click adds; `/` clears; prune the prim and check the trailing segments are greyed; narrow the viewer to 1920x1080 split width and check the "…" elision appears; the HUD no longer contains the path.
  - size: M

- [ ] M401.P1.T2 — Chevron children and sibling popups
  - files: `Gui/SceneBreadcrumbBar.cpp`, new `Gui/ScenePrimListPopup.{h,cpp}`, `Gui/SceneGraphModel.{h,cpp}` (export `childPredicate()` and the type-icon lookup), `Tests/gui/scene_breadcrumbs.py`
  - approach: a chevron after each segment (and after `/`) opens a popup with a filter field over a lazy `QAbstractListModel` that fetches child names in batches through `readStage`, using `GetFilteredChildrenNames(childPredicate())`, with the panel's type icons. Right-click on a segment opens the same popup over that prim's parent, with the segment's prim highlighted (siblings). Picking a row selects it. A prim with `HasAuthoredPayloads() && !IsLoaded()` gets a "Not loaded" row whose Load button calls `SceneViewport::requestLoad({path}, false)`, and the list refreshes when the stage's edit count changes.
  - verify: GUI script — the chevron on `group_1` lists its children; right-click lists `group_1, group_2`, and choosing `group_2` selects `/geo/group_2`; the filter narrows a 10k-child fixture without the main thread stalling past the viewer stall budget; on an unloaded payload, the "Not loaded" row's Load fills the list.
  - size: M

- [ ] M401.P1.T3 — Segment menu, tooltips and double-click frame
  - files: `Gui/SceneBreadcrumbBar.cpp`, `Tests/gui/scene_breadcrumbs.py`
  - approach: the segment context menu holds Siblings (opens M401.P1.T2's popup), Copy Path (that segment's path), Copy Selected Paths (newline-joined) and Frame (select, then `SceneViewport::frameSelection()`). Double-click on a segment does the same as Frame. The tooltip reads type name, kind and loaded state from the prim at hover time through `readStage`, and on a missing segment says "Not in this scene".
  - verify: GUI script — Copy Path puts `/geo/group_1` on the clipboard; Copy Selected Paths with two selected gives both lines; double-click selects and changes the camera (framing happened); a tooltip's text names the prim's type.
  - size: M

- [ ] M401.P1.T4 — Drag paths onto knobs
  - files: `Gui/SceneBreadcrumbBar.cpp`, `Tests/gui/scene_breadcrumbs_drag.py`
  - approach: start a drag from a segment past the drag threshold carrying that path, and from the "+N" badge carrying every selected path. Build the `QMimeData` exactly as `SceneGraphModel::mimeData` does: `primPathsMimeType()` and newline-joined `setText`. Expose the mime builder to the test hook. Knob drop targets (`KnobLineEdit::dropEvent` → `KnobGuiString::dropPrimPaths`) stay unchanged.
  - verify: GUI script after `Tests/gui/scene_graph_drag.py` — the bar's mime for the `group_1` segment, dropped on a USDPrune pattern knob, writes `/geo/group_1`; the badge's mime with two selected writes both to the pattern knob, and a path knob takes one, the same as a Scene Graph drag.
  - size: M

- [ ] M401.P1.T5 — Show In Scene Graph
  - files: `Gui/SceneBreadcrumbBar.cpp`, `Gui/SceneGraphPanel.{h,cpp}`, `Gui/Gui40.cpp` (panel lookup and creation), `Tests/gui/scene_breadcrumbs_show_in_graph.py`
  - approach: add `SceneGraphPanel::revealPaths(paths)`, which expands the ancestors (lazy, and without forcing payload loads), selects and scrolls to the prim. Add Show In Scene Graph to the segment menu. It picks the first panel following this viewer, otherwise opens a new panel following this viewer through M332 - Scene Graph Points' panel creation. It raises the panel and calls `revealPaths`. It never changes a pin silently: if multiple panels aren't available, it asks "Unpin the Scene Graph from <node> to show this prim?" and unpins only on Yes.
  - verify: GUI script — with the panel closed, the action opens a panel showing the prim selected and visible; with the only panel pinned to another node, a second viewer-following panel opens and the pinned one keeps its pin and node (or, without multiple panels, the unpin question appears and No leaves the pin unchanged).
  - size: M

## Phase 401.2: Multi-selection and pickwalk

- [ ] M401.P2.T1 — "+N" dropdown with Level grouping
  - files: `Gui/SceneBreadcrumbBar.cpp`, new `Gui/SceneBreadcrumbListPopup.{h,cpp}`, `Tests/gui/scene_breadcrumbs_multi.py`
  - approach: the badge toggles a popup under the bar: a `QListView` over a model of selected paths, with a delegate that paints each row as breadcrumbs and hit-tests segments, so there is no widget per row and huge selections stay fast. The Level spinner (0 = off, full paths) groups rows by their ancestor at that depth, shown as `/geo/group_1 (14)`, computed incrementally off the selection list with no stage read. Clicking a segment in a row selects that prim. Dragging a row carries its path (a grouped row carries its ancestor) with the same mime as M401.P1.T4.
  - verify: GUI script — with 20 prims across two groups selected, the dropdown lists 20 rows; Level 2 shows `/geo/group_1 (14)` and `/geo/group_2 (6)`; clicking `group_2` in a row selects only `/geo/group_2`; 100k selected paths open the popup within the stall budget.
  - size: L

- [ ] M401.P2.T2 — Pickwalk shortcuts
  - files: `Gui/GuiApplicationManager10.cpp` (`registerKeybind` in a 3D-viewer group), `Gui/ActionShortcuts.h` (IDs and descriptions), `Gui/SceneViewport.{h,cpp}`, `Tests/gui/scene_pickwalk.py`
  - approach: register Pickwalk Parent (Alt+Left), Child (Alt+Right), Previous Sibling (Alt+Up) and Next Sibling (Alt+Down), active while the 3D view or the bar has focus. Each walks every selected prim through `readStage` with `childPredicate()`, merges duplicates, and keeps the walked current prim last. Child goes to the remembered last child for that parent (a small per-viewport map filled on Parent walks), else the first. Siblings wrap. Ends that have nowhere to go leave the prim in place. Check the registry has no conflicting binding in the groups active over the viewer.
  - verify: GUI script — from `/geo/group_1/a_thing/mesh`, Alt+Left twice gives `/geo/group_1`, Alt+Right gives back `a_thing`, Alt+Down from `group_1` gives `group_2` and wraps from the last; with two prims selected, Alt+Left merges siblings into one parent; the keys appear in the shortcut editor and can be rebound.
  - size: M

**Verification gate:** release gate green; every `Tests/gui/scene_breadcrumbs*.py` and `scene_pickwalk.py` script passes under Xvfb; on the heavy-scene city fixture, the chevron popup, sibling popup and "+N" dropdown keep the main thread within the viewer stall budget. Collected for F4 - Heavy Scene Viewer's UAT: screenshots of the bar (single, elided, greyed), the chevron popup with "Not loaded", the context menu and the Level dropdown.
- 2026-11-12 — P1.T1 implemented (215f7700f): the bar reads the stage with `tryReadStage` and a 50 ms retry so the GUI thread never blocks on an edit; the HUD text is published as `natronSceneHud` and `SceneViewport::selectPaths` is a script slot for the tests. GUI script runs with the phase build.
- 2026-11-12 — P1.T2 implemented (cc2563e00): the list model reads 256 names per non-blocking stage read and resumes from the last name; the "Not loaded" row is a widget above the list; with no stage the popup shows "Waiting for the scene" and fills on the next sync.
- 2026-11-12 — P1.T3–T5 implemented (96cb6d026, 6ef0434db). Missing segments are greyed by palette, not disabled, so they still get the menu and drag. The mime builder moved to `SceneGraphModel::mimeDataOfPaths`, shared with the panel. `Gui::showPrimsInSceneGraph` lives in `Gui/Gui20.cpp`, where panel creation is. Phase 401.1 build running.
- 2026-11-12 — P2.T1 implemented (0e126172f): one-pass grouping over `QStringView`s into the stored paths; the popup hides during a row drag and closes after; `selectPaths` accepts paths the stage lacks, so the 100k case uses synthetic paths.
- 2026-11-13 — Phase 2 implemented (0e126172f, 83ed9a2a5). The gate's binding-count test was updated for the four pickwalk keys (bd9bc2cbd). Gate run 43382042162: ctest green, GUI 14 of 160 checks failed. Fixes in 3431bda7c: the bar and child popup waited on `isSceneSynced()`, which includes the engine's frame, and now gate on a new `isStageSynced()`; Show In Scene Graph docked its panel over the viewer it came from. Harness fixes: click menu entries, frame distinct prims, rebind the primary field, and focus via a real click. **Show In Scene Graph places a closed or new panel in the first pane that isn't the viewer's (PM call); an open panel stays put.** A screenshot goes to F4's UAT.
- 2026-11-13 — Gate run 43417013167 on 0682e5a6e: `scene_breadcrumbs.py` failed because a deactivated (pruned) prim still returns a valid `UsdPrim`; the bar now uses the Scene Graph panel's on-stage rule (6e397b84b). A `USDRenderMoonRayTest` segfault in the task-graph leg passed on its rerun. Re-gating (run 43441206714).
