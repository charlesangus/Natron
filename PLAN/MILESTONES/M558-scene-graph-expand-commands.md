# M558 - Scene Graph Expand Commands

Expand and collapse whole branches of the Scene Graph panel: Expand All Below, Expand To <kind> (ancestors open so every prim of that kind is a visible, collapsed row) and Collapse All Below, from the row menu, Shift-click on a branch arrow, the `*` key, Expand/Collapse buttons in M556 - Scene Graph Bulk Commands's toolbar row (using its scope combo), and editable shortcuts. Bulk expansion never loads a payload: it stops at unloaded rows. Member of F37 - Scene Graph Bulk Ops.

Today (origin/main): the model is lazy (`canFetchMore`/`fetchMore`, `Gui/SceneGraphModel.cpp` ~:2638/~:2653, batches of `kFetchBatchSize` = 1000); unloaded payload prims report children; expanding an unloaded row auto-loads it (`onExpanded`, `Gui/SceneGraphPanel.cpp` ~:2680); expansion is remembered across resets in `expanded` (~:309); `expandAll()` runs only on the changed-here tree, and a comment (~:133) warns it lays out every row on the main thread. Qt's default `*` key calls `expandRecursively`, which would auto-load every payload it reaches.

## Scope

**In:**
- A chunked, cancellable expander that walks model rows a slice per event-loop tick, fetching lazily as it goes, and never triggers `onExpanded`'s auto-load.
- Expand All Below, Expand To <kind> (built-in and registered custom kinds, via M556's kind levels), Collapse All Below.
- Triggers: row menu; Shift-click a branch arrow (expand all below; Shift-click an expanded arrow collapses all below); `*` rerouted to the expander; Expand and Collapse buttons in M556's toolbar row driven by its scope combo (Below = all below, a kind scope = To <kind>); editable shortcuts, unbound by default.

**Out:**
- Loading payloads during expansion (user): loading stays explicit (M556 - Scene Graph Bulk Commands).
- Expansion of the filter results list and the changed-here tree.
- A row cap: no limit on how many rows expand; responsiveness comes from chunking and cancellation.

**Edge cases:**
- Unloaded or failed payload rows reached by the walk stay collapsed, with their badge; the status line counts them ("Expanded 4,210 rows; 37 unloaded payloads not expanded").
- Expand To <kind>: the walk uses M556's stop rule; rows of the kind are made visible and left collapsed, pass-through ancestors expanded; branches with no match below stay as they were.
- Esc, a new expand/collapse command, a model reset or a stage change cancels a running expansion; rows already expanded stay expanded and are recorded in `expanded`.
- Rows of not-yet-fetched children: the expander calls `fetchMore` on the parent until its children are present before descending.
- A plain click on an arrow behaves as today, including auto-load of an unloaded row.

**Assumptions:**
- M556 - Scene Graph Bulk Commands's `sceneKindLevel` and stop rule apply unchanged to model rows (the model's Kind column reads the same authored kind).

## Phase 558.1: Expander

- [ ] M558.P1.T1 — A chunked, non-loading branch expander
  - files: `Gui/SceneGraphPanel.cpp` (near `onExpanded` ~:2680 and the `expanded` set ~:309); a new `Gui/SceneGraphExpander.{h,cpp}` if it reads cleaner (register in Gui's CMake)
  - approach: a breadth-first queue of model indexes processed in slices on a zero-interval `QTimer` (slice sized by a time budget, as the model's batch fetch is), `fetchMore` before descending, `setExpanded` with a guard flag that makes `onExpanded` skip its auto-load, unloaded/failed rows (`ePayloadStateRole`) not expanded and counted. Modes: all below, to kind (stop predicate from `sceneKindLevel` on the row's kind), collapse below (collapse descendants deepest-first from the `expanded` set, then the roots). Cancellation per Edge cases. Updates `expanded` and writes a summary to the status label.
  - verify: a gtest (or the P2.T2 GUI test) on a fixture with ~10k prims expands all below without loading any payload, and the event loop keeps processing events between slices (a timer posted during the expansion fires before it ends)
  - size: L

## Phase 558.2: Commands

- [ ] M558.P2.T1 — Expand/collapse triggers: menu, Shift-click, `*`, toolbar, shortcuts
  - files: `Gui/SceneGraphPanel.cpp` (`showRowMenu` ~:2305, the tree's event filter / mouse handling, M556's toolbar row), `Gui/ActionShortcuts.{h,cpp}` (Viewer3D group)
  - approach: row menu entries Expand All Below, Expand To Assembly / Component / Subcomponent / each custom kind, Collapse All Below (objectNames `sceneGraphExpandBelow`, `sceneGraphExpandTo_<kind>`, `sceneGraphCollapseBelow`); Shift+click on the branch indicator (hit-test with `visualRect` and the indentation) runs expand all below, or collapse all below when expanded; intercept `*` in the tree's key handling and run expand all below on the current row; add Expand and Collapse buttons to M556's toolbar row reading its scope combo (Selected acts like Below for these); register unbound editable shortcuts for each command.
  - verify: the P2.T2 GUI test drives each trigger
  - size: M
- [ ] M558.P2.T2 — GUI test for expand commands
  - files: new `Tests/gui/scene_graph_expand.py`, fixtures under `Tests/fixtures/` (reuse M556's kind fixture plus a wide one)
  - approach: Expand To Component on an assembly row shows every component row collapsed and expands no component; Expand All Below leaves unloaded payload rows collapsed and the stage's load set unchanged; `*` and Shift-click do the same and never load; Collapse All Below closes everything under the row; Esc during a long expansion stops it. Follow the PySide lifetime rules (never `parentItem()`; keep the window active).
  - verify: passes under Xvfb locally and in the remote `--gui` gate
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests --gui "scene_graph_expand.py scene_graph_bulk.py scene_graph_panel.py scene_graph_payloads.py"` green; Expand To Component on Island's `isBayCedarA1` responsive (no visible stall) collected for F37 - Scene Graph Bulk Ops's UAT.

## Decisions
- 2026-11-23 — Bulk expansion stops at unloaded payloads and never loads (user).
- 2026-11-23 — Expand To <kind> leaves the kind's rows visible and collapsed (user), matching Houdini's Expand To Components.
- 2026-11-23 — Triggers: row menu, Shift-click arrow, `*`, toolbar buttons on M556's scope combo, editable shortcuts (user).
