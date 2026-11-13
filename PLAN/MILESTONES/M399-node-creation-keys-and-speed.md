# M399 - Node Creation Keys And Speed

Four node-graph fixes in one milestone (user, 2026-11-11):

1. **Viewer number keys make duplicate viewers.** Pressing 1–9/0 with a node selected goes
   `Gui::connectInput` (`Gui/Gui30.cpp`) → `NodeGraph::connectCurrentViewerToSelection`
   (`Gui/NodeGraph30.cpp:64`). That function only consults one cached pointer,
   `getLastSelectedViewer()`. When the pointer is null it creates a new Viewer in
   `getGroup()`, even when the group already holds one. The pointer is null when the Viewer
   was never selected or focused (after a project load or creation from a script), when its
   `ViewerTab` was destroyed (`Gui/ViewerTab10.cpp:602` clears it), and when it points into
   another group and no tab in this graph's group is found. `Gui::connectInput` also picks
   `_lastFocusedGraph`, which may not be the graph holding the selection.
2. **The tab menu stops working after a global-mode edit.** Tab opens the menu from
   `NodeGraph::event` (`Gui/NodeGraph25.cpp:188`) only while
   `_imp->_nodeCreationShortcutEnabled` is true. That flag is set by `enterEvent` and cleared
   by `leaveEvent` (`Gui/NodeGraph30.cpp:147,156`). Global mode
   (`Gui/ColorTrackballGlobalMode.cpp`, with `ValueKnobGlobalModeTarget.cpp` and
   `TransformGroupGlobalModeTarget.cpp`) installs an app-wide event filter, `grabMouse`s
   another widget, and sets an override cursor. The likeliest cause, from reading the code
   but not reproduced: the grab sends the graph a Leave, and no Enter follows while the
   pointer is already inside. Other candidates: an exit path that never removes the app-wide
   filter, which swallows every non-pass-through key including Tab, or an override cursor
   that is never restored.
3. **Creating a node is perceptibly slow on a weak host**, from a plugin shortcut ("B" for
   Blur) and from Enter in the tab menu. The path runs `createNodeFromNodesMenu`
   (`Gui/GuiApplicationManager.cpp:1670`) → `AppInstance::createNode` →
   `GuiAppInstance::createNodeGui` (`Gui/GuiAppInstance.cpp:~447-630`). All of it is
   synchronous: node GUI, viewer interface, edges, then `computePreviewImage(currentFrame)`
   inline (`:598`), `triggerAutoSave()`, and `moveNodesForIdealPosition`. Properties-panel
   construction, Python callbacks and the undo push have not been traced yet.
4. **The tab menu is slow to open.** Every open runs a Python string through
   `TabMenuBridge::launch` (`Gui/TabMenuBridge.cpp:~119,300`). In
   `Gui/Resources/TabTabTab/tabtabtab_natron.py`, `launch()` re-finds the main window,
   re-attaches the popup and re-reads Preferences through Python (`:302,358`). `get_items()`
   (`:246`) walks the whole nodes menu and menubar in Python. The core
   (`tabtabtab_nuke_core`) caches the widget, and may skip rebuilding the item list. That
   is unverified.

**Budgets (user, 2026-11-11).** Judged on this 4-core dev host, running the stripped release
AppImage:
- **Node creation ≤ 50 ms**, from the key press (plugin shortcut, or Enter in the tab menu)
  to the node being painted in the graph.
- **Tab-menu open ≤ 33 ms**, from Tab to the populated menu being painted.
- Both budgets hold on an empty graph and on the GUI bench's 300- and 3000-node `mixed`
  graphs, every run, not just on average.

Code scout: `main` at `d07d202ea`, 2026-11-11. Line numbers will have moved after M118 - Large
Graph GUI Responsiveness merges, so re-grep them.

## Scope

**In:**
- The viewer lookup behind the 1–9/0 and Shift+1–9/0 connect keys, and the choice of graph
  those keys act on.
- Tab opening the menu after any global-mode exit: Esc, Enter, right-click, a modal dialog,
  app deactivation or an ungrab.
- Latency on the node-creation path, the tab-menu open path and the tab-menu Enter path.
  Also new GUI bench ops that measure them.

**Out:**
- General graph responsiveness at 3000+ nodes, such as pan, zoom and selection. M118 - Large
  Graph GUI Responsiveness owns that.
- Tab-menu search ranking, its layout and the items it lists.
- Render latency after a node is created. The budget ends when the node is painted, not when
  a render finishes.

**Edge cases:**
- The selected node's group holds several Viewers. Use the last-selected or last-focused one
  if it is in this group and activated. Otherwise use the Viewer whose tab was most recently
  focused, then the first activated Viewer in the group's node order. Never create one.
- A Viewer in a parent or child group does not count. Each group gets its own Viewer, as
  today.
- A deactivated (deleted, in the undo trash) Viewer does not count. Creating a new one is
  correct then.
- A Viewer node whose `ViewerTab` was closed counts as existing. Reuse it and reopen or focus
  its tab the way selecting a Viewer does today.
- Pressing Tab while global mode is still engaged keeps its current behaviour, which is that
  global mode swallows it. Only Tab after an exit is in scope.
- The properties panel may appear after the node, in a later frame. The budget covers the
  node's paint, not the panel's.
- The first tab-menu open of a session, which does a Python bootstrap and builds the widget,
  is measured and reported. It may be done ahead of time at startup, off the critical path,
  so that it meets the budget too.

**Assumptions:**
- If the budgets hold under Xvfb on the dev host, they hold on its real display. Xvfb paints
  in software and is not faster. The user's UAT confirms it.
- M118 - Large Graph GUI Responsiveness's node-GUI construction changes (M118.P6.T12) are on
  `main` before the latency profile runs, which is why it is a dependency.

## Phase 399.1: Viewer number keys

- [x] M399.P1.T1 — Reuse any Viewer already in the selection's group before creating one
  - files: `Gui/NodeGraph30.cpp`, `Gui/NodeGraph.h`, `Gui/Gui30.cpp`
  - approach: In `connectCurrentViewerToSelection`, keep `getLastSelectedViewer()` as the
    first choice when its node is activated and its group's graph is `this`. Otherwise
    search `getGroup()`'s nodes for an activated `InspectorNode` that is a Viewer
    (`PLUGINID_NATRON_VIEWER`), and choose in the edge-case order in `## Scope`: the most
    recently focused tab in `getGui()->getViewersList()`, then the group's node order.
    Create a Viewer only when the search finds none. When a found Viewer has no live
    `ViewerTab`, open or focus its tab the same way selecting the Viewer node does. Then
    record it as the last selected viewer, so the next press is a direct hit. In
    `Gui::connectInput`, act on the graph that holds the current node selection when it
    differs from `_lastFocusedGraph` (the user just clicked a node there). Fall back to
    today's choice when no graph has a selection.
  - verify: builds; under Xvfb, a script-created Viewer that was never selected is reused
    when `1` is pressed with another node selected, and the group's Viewer count stays at one.
  - size: M

- [x] M399.P1.T2 — GUI script test for viewer reuse on number keys
  - files: `Tests/gui/m399_viewer_number_keys.py` (new), registered beside the existing
    `Tests/gui/` scripts the way `Tests/gui/m55_node_graph.py` is
  - approach: Under Xvfb (the `build/deeprepro/run-gui.sh` recipe locally, or
    `build.sh --from-run <id> --gui "<script>"` remotely), with real key events sent to the
    graph widget (not direct calls to `connectCurrentViewerToSelection`), check these cases.
    In each one, press `1` with a non-Viewer node selected and assert that the group's Viewer
    count does not change and that the node is connected to input 0 of the expected Viewer.
    (a) A Viewer created by script and never selected. (b) A Viewer whose tab was closed.
    (c) A project saved and reloaded with one Viewer. (d) Inside a Group node that has its
    own Viewer, while the root graph also has one: the Group's Viewer is used. (e) Inside a
    Group with no Viewer: exactly one is created there, and a second press reuses it.
    (f) The only Viewer is deleted (it sits in the trash): one is created. Keep the owner
    wrappers alive per the PySide-lifetime rule in the GUI-script memory notes.
  - verify: the script passes against M399.P1.T1. With M399.P1.T1 reverted, cases (a)–(c)
    fail.
  - size: M

## Phase 399.2: Tab menu after global mode

- [x] M399.P2.T1 — GUI script that reproduces the dead tab menu after global mode
  - files: `Tests/gui/m399_tab_after_global_mode.py` (new), registered like M399.P1.T2's
  - approach: Under Xvfb, open a node's properties. Engage global mode on a value knob, then
    on the colour trackball and on a transform group (the three targets). Make a stroke edit
    with real mouse events, then exit by each path: Esc, Enter, right-click, and an
    application deactivate. After each exit, move the pointer into the node graph, the way a
    user does after an edit (with and without a fresh Enter event), press Tab with a real key
    event, and assert that the tab-menu popup is visible. Close it with Esc before the next
    case. Also assert that `QApplication::overrideCursor()` is null and that a probe key
    reaches the graph, which shows the app-wide filter is gone. Report which paths fail on
    `main`.
  - verify: the script runs to completion and fails on `main` for at least the path the user
    hit. Record the failing paths in this milestone's `## Decisions`. If nothing fails,
    report that to the PM, because the reproduction must be widened before any fix.
  - size: M

- [x] M399.P2.T2 — Make Tab work after every global-mode exit
  - files: `Gui/NodeGraph30.cpp`, `Gui/NodeGraph25.cpp`, `Gui/NodeGraphPrivate.h`,
    `Gui/ColorTrackballGlobalMode.cpp`; the two `*GlobalModeTarget.cpp` files only if
    M399.P2.T1 shows a leak there
  - approach: Fix the causes M399.P2.T1 recorded. Two changes are expected: (1) Stop gating
    Tab on the Enter/Leave flag. At key time, test whether the pointer is over the graph's
    viewport (`viewport()->rect().contains(viewport()->mapFromGlobal(QCursor::pos()))`), and
    open the menu when it is, or when the graph has keyboard focus. Remove the flag, or keep
    it only where something else still reads it. (2) Make every global-mode exit path,
    including `endStrokeWithoutRelease` on ungrab and app deactivation, end the mode:
    remove the app-wide filter (after the right-click tail disarms), call `releaseMouse`, and
    restore the override cursor, all from one teardown function so no path can skip a step.
  - verify: builds; M399.P2.T1's script passes on every path; the `Tests/gui/` global-mode
    and tab-menu scripts still pass.
  - size: M
- 2026-11-12 — **Round 3 bench** (8489ce70f, run 20261112T231959Z, Xvfb, relative only). The harness now counts trailing repaints (T22), so wall and CPU per event are not comparable with earlier rounds: per-event CPU at 3000 is about 220 ms for create and tab create, where the old window cut off the second repaint. T17 holds: `tab_menu_create` CPU at 3000 is 226 ms against `create_by_shortcut` 222. T22 holds. Still missing: T18, where individual `panelBuildPass` runs have a median of 9.4 ms, p90 15.4, p99 25 and max 36, and 8% exceed 16 ms (p90 target 10). T19, where positioning at 3000 is 9.3 ms for shortcut, 9.7 for tab and 4.6 for create_then_key (target 2). T20, where createNodeGui at 3000 is 13.6, 14.3 and 7.7 ms (target 5). At 0 and 300, engine plus GUI plus positioning comes to 5-9 ms; the rest of each latency is Xvfb compositing. Tab open at 0/300/3000 is 5.5-6 ms for launch and show, and 24-29 ms end to end. T21 has not been measured.
- 2026-11-12 — T18-T20 round 4 (bddb09f01, build run 43331868915). Profile, sampled sparsely with eu-stack: panel pass overshoot is Qt's per-widget `setVisible` (restyle, ancestor relayout, X round trip) inside single knobs; the pass now checks the budget before each knob against the last knob's cost, and creates tabs in a pass of its own (`panelBuildPages`). Synchronous builds log as `panelBuildSync`. About 9 ms of createNodeGui's 13.6 ms at 3000 is the nested positioning; its own cost is about 3 ms. A fan-out insert reconnects its outputs in one `InputEditBatch`. New sub-stages `positioningMove`/`positioningConnect`/`nodeGraphItem` will show where positioning goes. Open: heavy single knobs stay over 10 ms unless rows are built under a hidden container (a design change; not done).
- 2026-11-12 — **Round 4 bench** (bddb09f01, run 20261113T011941Z, Xvfb, relative only). Per-pass `panelBuildPass` median 6.7 ms, p90 12.6, p99 21, max 34 (round 3: 9.4/15.4/25/36); tabs pass `panelBuildPages` median 1.9 ms. Positioning at 3000 is all `positioningConnect`: median 7-8 ms (`positioningMove` ~0). `nodeGraphItem` is 2.4-3 ms at 3000, so createNodeGui's own cost meets T20's 5 ms target once nested positioning is excluded; T20 done, and the remainder is filed as P4.T23. Tab open launch and show are 3.6-4.2 ms; end to end 21-23 ms median, max 41 at 3000. The settle window now runs through the incremental panel build's repaints, so headline latencies include it. T18 short of p90 10 (single heavy knobs: Qt show/restyle). The rest waits on the user's `--host` run on a real display (UAT step 13).

## Phase 399.3: Measure creation and tab-menu latency

- [x] M399.P3.T1 — GUI bench ops for node creation and tab-menu latency
  - files: `tools/bench/gui/gui_bench.py`, `tools/bench/gui/README.md`
  - approach: Add three ops. `create_by_shortcut` sends the Blur shortcut key event to the
    graph with a node selected. `tab_menu_open` sends Tab. `tab_menu_create` types `blur` in
    the open menu and sends Enter. Each op times from just before the key event is posted to
    the end of the first graph (or popup) paint that shows the result. Hook the paint with a
    one-shot event filter on the viewport or popup for `QEvent::Paint`, and pump events until
    it fires, with a 2 s timeout that fails the run. Run each op 20 times per graph (empty,
    300 and 3000 `mixed` from `gen_graph.py`), undoing each creation between runs. Record
    min, median, p95 and max in the existing JSONL format, plus the first-ever
    `tab_menu_open` of the session as its own sample. Add an optional per-stage split, gated
    on an env var as the bench's existing timing split is (see M118.P6.T1): engine
    `createNode`, `createNodeGui`, panel build, preview, autosave, positioning, Python
    callbacks, and for the tab menu the Python `launch`, `get_items` and `show`.
  - verify: `tools/bench/gui/run.sh --xvfb` runs the new ops on the dev host against a
    release build and writes their rows. `compare.py` accepts the file.
  - size: M

- [x] M399.P3.T2 — Profile both paths on the dev host and rank the misses
  - files: none (measurement); results go in this milestone's `## Decisions`
  - approach: On this 4-core dev host, with the stripped release AppImage under Xvfb, run
    M399.P3.T1's ops with the stage split on. Where a stage is opaque, use the profiling
    recipe in the container profiling notes (`eu-stack` sampling of the main thread). Record
    each op's p95 and max at each graph size against its budget, the stage split, and a
    ranked list of what to remove from each path. Confirm or rule out each suspect named in
    the intro: inline preview, autosave, panel build, Python on every tab open, the menu
    walk, and `singleShot(0)` hops.
  - verify: the ranked list and the numbers are in `## Decisions`. Every M399.P4 task names
    the stage it removes, and a miss with no task gets a new trailing task.
  - size: M

## Phase 399.4: Make both paths instant

- [x] M399.P4.T1 — Take preview, autosave and panel build off node creation's critical path
  - files: `Gui/GuiAppInstance.cpp`, `Gui/NodeGui.cpp`, `Gui/NodeSettingsPanel.cpp`,
    `Gui/DockablePanel*.cpp` as the profile directs
  - approach: Interactive single-node creation (not project load, paste or Python batch
    creation, which keep their current paths) should paint the node first and defer the
    rest. Queue `computePreviewImage` and `triggerAutoSave` to run after the next paint, for
    example by a zero-timer posted once the graph has painted, coalesced per node. Build the
    properties panel's knob widgets after the node's first paint. Build only the visible
    page eagerly and the other pages on first show, if the profile puts the panel cost over
    budget. Keep undo, naming, auto-connect and positioning synchronous, so the node appears
    where and how it does today.
  - verify: builds; ctest and `Tests/gui/` pass; M399.P3.T1's `create_by_shortcut` and
    `tab_menu_create` show the deferred stages gone from the press-to-paint span.
  - size: L

- [x] M399.P4.T2 — Open the tab menu without per-open Python work
  - files: `Gui/TabMenuBridge.cpp`, `Gui/TabMenuBridge.h`,
    `Gui/Resources/TabTabTab/tabtabtab_natron.py`,
    `Gui/Resources/TabTabTab/tabtabtab_nuke_core.py`
  - approach: Cache everything that does not change between opens: the main window, the
    popup's parent, the Preferences values (refreshed on the settings-changed signal), and
    the item list. Rebuild the item list only when the nodes menu, the menubar, a plugin
    registration or a shortcut changes. Connect those signals to one dirty flag, and use the
    context `_accepts` filter on the cached list instead of re-walking the menu. Replace the
    per-open interpreted Python string in `TabMenuBridge::launch` with a call through a
    cached callable on the bootstrapped module. Warm the widget and the item list once
    after startup, idle and off the critical path, so the first open also meets the budget.
    On Enter, create the node directly rather than through an extra `singleShot(0)` hop,
    where the profile shows that hop costs a frame. The search and ranking behaviour stays
    as it is.
  - verify: builds; the tab-menu `Tests/gui/` scripts pass, including the nodes-menu test;
    a plugin registered or a shortcut changed after startup shows up in the next open;
    `tab_menu_open` meets 33 ms in M399.P3.T1's bench.
  - size: M

- [x] M399.P4.T4 — Scope data-kind invalidation and make positioning O(1)
  - files: `Engine/Node.cpp` (~:3120 `invalidateEffectiveOutputDataKindCaches`), `Gui/NodeGraph05.cpp` (~:242-256 `moveNodesForIdealPosition`)
  - approach: A connect from creation's positioning walks the whole connected component both ways to invalidate data-kind caches. Invalidate downstream only, or lazily by a generation counter checked on read. Move the nodes below the new one without the recursive walk.
  - verify: builds; ctest passes; the stage log's positioning median is under 2 ms at 3000 nodes.
  - size: M

- [x] M399.P4.T5 — Update the item grid and navigator incrementally after a creation
  - files: `Gui/NodeGraph30.cpp` (~:565, ~:634), `Gui/NodeGraph10.cpp` (~:181), `Gui/NodeGraphLowDetailLayer.cpp` as needed
  - approach: Creation sets `itemGridDirty` through `invalidateNavigator`, so the next paint rebuilds the whole grid, and `areAllNodesVisible` recomputes every node's bounds on the navigator timer. Insert or update only the new node and its edges in the grid, and cache the nodes' bounding rect, updated per change.
  - verify: builds; ctest and `m55_node_graph.py` pass; the post-create loop pass at 3000 nodes is within 10% of its value on an empty graph.
  - size: M

- [x] M399.P4.T6 — Remove the smaller creation costs
  - files: `Gui/TabMenuBridge.cpp` (~:200-237), `Engine/Project.cpp` (~:2782), `Gui/NodeGraphUndoRedo.cpp` (~:296)
  - approach: `triggerInContext` diffs the node list before and after (O(N²)); use the created node it returns. Make `Project::refreshLayersKnob` incremental instead of walking every node. Drop the duplicate autosave in `AddMultipleNodesCommand::redo`.
  - verify: builds; ctest and the tab-menu `Tests/gui/` scripts pass; none of the three shows in a creation sample at 3000 nodes.
  - size: M

- [x] M399.P4.T7 — Bound the graph walks each connect runs
  - files: `Engine/Node*.cpp`, `Engine/NodeInputs.cpp`, `Gui/NodeGraph05.cpp` as the callers direct
  - approach: P4.T4's agent found positioning's remaining cost is not `moveBelowPositionRecursively` but four full-graph walks per connect: two `hasViewersConnected`, `abortViewerRendersForEdit` → `getDownstreamOutputs`, and the cycle check (`isNodeUpstream` → `hashDependsOn`). Bound each to the nodes the edit affects, with a visited set, and drop duplicate walks per connect. Behaviour stays the same.
  - verify: builds; ctest passes, with visit-count tests on a 3000-node graph; the stage log's positioning median is under 2 ms at 3000 nodes.
  - size: M

- [x] M399.P4.T8 — Bound the low-detail repaint after a creation
  - files: `Gui/NodeGraphLowDetailLayer.cpp` (~:466-579), `Gui/NodeGraph05.cpp`
  - approach: dirty only the new node, its edges and the nodes that actually moved; stop more than 32 dirty rects collapsing into one union (~:473).
  - verify: `create_by_shortcut` graph-paint samples at 3000 within 2× of the 0-node count; `m55_node_graph.py` passes.
  - size: M

- [x] M399.P4.T9 — Give the node-graph scene a fixed sceneRect
  - files: `Gui/NodeGraph.cpp` (~:144)
  - approach: Qt rescans every item for `itemsBoundingRect` on each add because the scene has no sceneRect; set one on the scene, kept in step with the view's.
  - verify: no `itemsBoundingRect` in a 3000-node creation sample; `m55_node_graph.py` passes.
  - size: S

- [x] M399.P4.T10 — Bound connect-time upstream work
  - files: `Engine/Node.cpp` (`onInputChanged`, `ParallelRenderArgsSetter`), `Engine/NodeInputs.cpp`
  - approach: a GUI connect runs `buildArgsMap`, `getContentHash` and a recursive `getTimeInvariantMetadata` upstream; skip them or make them lazy until a render needs them.
  - verify: positioning median under 2 ms at 3000; ctest passes.
  - size: M

- [x] M399.P4.T11 — Tab-menu Enter invokes before closing and re-filters later
  - files: `Gui/Resources/TabTabTab/tabtabtab_nuke_core.py` (~:1076-1101)
  - approach: invoke, then close, for actions that open no window; apply `prev_string` on the next show instead of `setText` on the critical path.
  - verify: `tab_menu_create` within 10% of `create_by_shortcut`; `m45_tab_menu.py` passes.
  - size: M

- [x] M399.P4.T12 — Dope-sheet insert without the downstream walk
  - files: `Gui/DopeSheet.cpp` (~:321-380), `Gui/DopeSheetHierarchyView.cpp` (~:969)
  - approach: `getNearestTimeFromOutputs_recursive` does a `std::list` find per visited node; use a hash set, or defer the insert until the dope sheet is shown.
  - verify: the walk is absent from panel-build samples at 3000; ctest passes.
  - size: S

- [x] M399.P4.T13 — Build only the visible panel page
  - files: `Gui/KnobGuiContainerHelper.cpp`, `Gui/NodeGui.cpp` (~:711)
  - approach: the deferred panel build (117-195 ms, one stall the next key waits on) builds every page's knob widgets; build the visible page, the rest on first show, and split the build across loop passes.
  - verify: the new `create_then_key` op (P4.T16) meets 50 ms; GUI scripts pass.
  - size: L

- [x] M399.P4.T14 — Make the layers refresh and navigator bounds incremental
  - files: `Engine/Project.cpp` (~:2782), `Engine/Node.cpp` (~:9188), `Gui/NodeGraphPrivate.cpp` (~:250-310)
  - approach: cache each node's layer ids and diff them; handle removals in the pending bounds instead of marking all bounds dirty.
  - verify: neither shows in a 3000-node creation sample.
  - size: M

- [x] M399.P4.T15 — Warm the popup's native window at startup
  - files: `Gui/Resources/TabTabTab/tabtabtab_natron.py` (~:687 warm-up)
  - approach: create the window (`winId`), polish and lay it out during warm-up.
  - verify: the session's first `tab_menu_open` ≤ 33 ms.
  - size: S

- [x] M399.P4.T16 — Bench validity
  - files: `tools/bench/gui/gui_bench.py`, `tools/bench/gui/README.md`
  - approach: put the paint filter on the popup only, not the whole application; drain the previous rep's destroy before t0; add a `create_then_key` op (create, then Tab as soon as the node paints, timed to the menu's paint) so the deferred panel stall is counted.
  - verify: the bench runs the new op; `compare.py` accepts the file.
  - size: M

- [x] M399.P4.T17 — Remove the second whole-widget graph repaint after a creation
  - files: `Gui/NodeGraph30.cpp`, `Gui/NodeGraph10.cpp`, `Gui/NodeGraphLowDetailLayer.cpp`, `Gui/NodeGui.cpp`
  - approach: every creation queues a second, whole-widget graph repaint (frame included) about 60 ms after the first, which refills the low-detail cache; find what queues it (focus, activation or palette change, or `invalidateNavigator(rebuildGrid)` at `NodeGraph30.cpp:594,629`) and keep the cache valid, invalidating only dirty rects.
  - verify: one graph paint per creation; `tab_menu_create` CPU at 3000 within 20 ms of `create_by_shortcut`.
  - size: M

- [ ] M399.P4.T18 — Make the panel build passes hold their budget
  - files: `Gui/KnobGuiContainerHelper.cpp` (~:478, :488-574, :1142), `Gui/NodeGui.cpp` (~:731-860)
  - approach: move the first pass out of `startPanelBuild`, split `createPanel`, check the budget inside vector-knob init, and on undo or deactivate drop an unbuilt panel instead of building it.
  - verify: `panelBuildPass` p90 ≤ 10 ms; no `ensureKnobGuisCreated` in undo samples.
  - size: M

- [ ] M399.P4.T19 — Positioning and connect cost at 3000
  - files: `Gui/GuiAppInstance.cpp` (~:631-653), the Engine `replaceInput` path
  - approach: skip the viewer abort and the upstream refresh when no viewer depends on the node; bound `moveNodesForIdealPosition`.
  - verify: positioning median ≤ 2 ms at 3000.
  - size: M

- [x] M399.P4.T20 — createNodeGui growth from 2 to 20 ms
  - files: `Gui/GuiAppInstance.cpp` (~:537-614), `Gui/NodeGui.cpp`
  - approach: find what scans all nodes during node GUI creation and bound it.
  - verify: createNodeGui ≤ 5 ms at 3000.
  - size: M

- [ ] M399.P4.T21 — Per-item cost in the graph paint at 3000
  - files: `Gui/NodeGraph*.cpp`
  - approach: cull to the exposed rect and avoid `isVisible` walks in `getGridItems`.
  - verify: a full graph paint ≤ 15 ms CPU at 3000.
  - size: M

- [x] M399.P4.T22 — Bench counts trailing paints and drains undo teardown
  - files: `tools/bench/gui/gui_bench.py`
  - approach: drain until idle after undo before t0, and extend each step to the next idle so trailing repaints count.
  - verify: the shortcut step includes the trailing paint; no `~NodeSettingsPanel` samples in the tab window.
  - size: S

- [ ] M399.P4.T23 — Chain insert refreshes everything below once
  - files: Engine `replaceInput`/refresh path, `Gui/NodeGraph05.cpp`
  - approach: `positioningConnect` is all of positioning at 3000 (median 7-8 ms, p90 8.5, max 27; `positioningMove` ~0). Inserting mid-chain refreshes every downstream node (the draft flag every render reads). Bound or defer that walk without changing what renders read. Consultant design (2026-11-12): in `refreshShownDownstreamInputRelatedData`, (a) a draft-flag worklist that stops where a node's flag is unchanged (exact: the flag is OR of the inputs' flags plus `supportsRenderQuality`); (b) dirty state from an edit-generation stamp checked upstream, memoized per generation, instead of eager marks; (c) refresh visible-panel and channel-message nodes from registries. Hashes untouched. Fallback: (a)+(c) with the eager dirty marks coalesced to the next idle, flushed before panel reads.
  - verify: positioning median ≤ 2 ms at 3000.
  - size: L

- [ ] M399.P4.T3 — Re-measure on the dev host and close the remaining misses
  - files: `tools/bench/gui/baselines/xvfb.jsonl`; code as the misses direct
  - approach: Re-run M399.P3.T1's ops on the dev host's release AppImage. For any op over
    budget at any graph size, profile it as in M399.P3.T2, and file the fix as a new trailing
    task in this phase rather than folding it in here. Once all ops meet budget, run the
    standard GUI bench and `compare.py` per the GUI bench rule, and re-baseline the new ops.
  - verify: every new op's max is within budget at 0, 300 and 3000 nodes, and the GUI bench
    shows no regression. Both results are recorded in `## Decisions`.
  - size: M

**Verification gate:** ctest and every `Tests/gui/` script pass, including M399.P1.T2's and
M399.P2.T1's. On the 4-core dev host's stripped release AppImage under Xvfb, M399.P3.T1's
ops meet the budgets on the empty, 300- and 3000-node graphs: `create_by_shortcut` and
`tab_menu_create` max ≤ 50 ms, `tab_menu_open` max ≤ 33 ms, the first open of the session
included. The standard GUI bench shows no regression against `baselines/xvfb.jsonl`, recorded
in `## Decisions`. The user accepts a UAT AppImage with its steps `.md`, covering the number
keys reusing an existing Viewer (including after a reload and inside a Group), Tab after each
global-mode exit, and creation and tab-menu feel on this host.

## Decisions

- 2026-11-12 — Claimed at the user's request while M118 (PR #259) is in review. The branch starts from M118's tip (acbf9300d), so the node-graph code it edits matches what lands on main. Phases 1 and 2 start now. Phases 3 and 4 measure after M118 is on main, and main is merged in before the PR.
- 2026-11-12 — Phases 1 and 2 implemented (2879f2e56), build pending. Reproduction on the pre-fix AppImage: number keys case (a), a remembered viewer lost, and (b), a closed tab, fail; (c), a project reload, passes because loading sets the remembered viewer, so the user's reload case may be a new process or a recent-files open, still unreproduced. Global mode: only application deactivation (X focus loss) leaves the mode engaged, for all three targets, with the filter eating Tab and the crosshair cursor stuck. Esc, Enter and right-click exits already work. Ctrl+E does not engage the mode under Xvfb, so the script triggers the action. Extra fix: `Gui::removeViewerTab`'s shadowed `collection` cleared the remembered viewer even when another existed.
- 2026-11-12 — First gate (run 43160524173) failed only `m45_tab_menu.py`: the script drove a node graph hidden behind another tab with a synthetic Enter. PM call: fix the script (9204c6ce4: bring the tab forward, activate, focus, cursor over the viewport), not restore the entered flag. That exposed a pre-existing bug: a create-node key rebound to one a global action uses (F2 is Enable Render Statistics) fired the global action. Fixed by the focused graph claiming the key on ShortcutOverride (5612d9331), as ViewerTab does. Gate re-running (run 43182441048). Open: the shortcut editor checks conflicts only within a group.
- 2026-11-12 — Release gate green at 5612d9331 with all six GUI scripts (run 43182441048). Early UAT AppImage for phases 1-2 is out. P3.T1 dispatched.
- 2026-11-12 — P3.T1 landed at 1ac1e1d02 (gate run 43197694625). Bench results are under `build/bench/gui/20261112T113121Z-*` and `115847Z-*` (3000 only, eu-stack sampled) in the M399 worktree; the stage split is `node-creation-stages.jsonl`.
- 2026-11-12 — **P3.T2 profile** (dev host, stripped release AppImage under Xvfb). The host was loaded (load 26-40 from an outside compile), so wall times are inflated about 2-2.5×; main-thread CPU is given beside them, and a clean-load re-run is owed in P4.T3. The 3000 run repeated within 5%.
  - p95/max ms at 0/300/3000: `create_by_shortcut` 594/681, 671/765, 1054/1158 (CPU median 165/203/539); `tab_menu_create` 556/652, 575/637, 1097/1142 (CPU 151/191/582); `tab_menu_open` 127/157, 93/106, 99/125 (CPU 39-40; the first open of the session 206-296). Budgets 50 / 50 / 33.
  - Create stage medians at 0/300/3000: createNodeGui 257/242/304, panelBuild 254/214/154, positioning 0.4/17/138, engineCreateNode 4.6/4.6/7.5, pythonCallbacks 0.2/0.3/1.7, preview and autosave about 0.005. Unattributed: about 150 ms at 0 and 420 at 3000, the paint pass after creation (loop pass 83/96/250).
  - Samples at 3000: ensurePanelCreated 60% (initializeKnobs 37%), moveNodesForIdealPosition 32%, `Node::invalidateEffectiveOutputDataKindCaches` 22% (a whole-component walk from connectInput, `Engine/Node.cpp:3120`), NodeGui init and edges about 12%. After creation: a full item-grid rebuild (`NodeGraph10.cpp:181`, set dirty by `invalidateNavigator` at `NodeGraph30.cpp:565`), low-detail `paintCached`, `areAllNodesVisible` → calcNodesBoundingRect (`NodeGraph30.cpp:634`), `Project::refreshLayersKnob` over every node (`Project.cpp:2782`).
  - Tab open: launch 8, show 7, refreshAfterShow 26-34 (getItems 23-28 inside), then a second getItems of 23-27 after the paint; about 25 ms unattributed.
  - Suspects: inline preview and autosave ruled out (about 0). Confirmed: panel build (the largest fixed cost); Python on every open (`TabMenuBridge.cpp:106-125`, `_read_preferences` at `tabtabtab_natron.py:358`); the menu walk, twice per open with no cache (`tabtabtab_nuke_core.py:967`, `:1033`), weights loaded on show and saved on close; `singleShot(0)` hops, 2 per open and 1 per create. `preload` is never called, so the first open costs about 200 ms more. `triggerInContext` diffs nodes in O(N²) (`TabMenuBridge.cpp:200-237`). The growth with N is positioning, the data-kind walk, and the post-create grid and paint work.
  - Ranked: (1) defer the panel build, 150-250 ms, P4.T1; (2) scoped data-kind invalidation and O(1) positioning, about 140 ms at 3000, new P4.T4; (3) incremental grid and navigator, about 170 ms at 3000, new P4.T5; (4) the tab menu's cached items, preload, compiled callable, no hops, cached prefs and weights, 30-60 ms per open and about 200 ms on the first, P4.T2; (5) `triggerInContext`, `refreshLayersKnob` and the duplicate autosave, 10-40 ms, new P4.T6; (6) NodeGui::initialize's name HTML and edge labels, 20-40 ms, folded into P4.T1. If P4.T3 still misses, check Shiboken lazy class loading (about 15 samples, probably the bench's own Python) before product code.
- 2026-11-12 — P4.T1, T2, T4-T7 landed (63325826e, 6c1316059: the tab-menu walk cache kept submenu wrappers PySide invalidates; the m158 test lacked Loop and Reverse Flow Variable); gate green (run 43242648575). Create bench on it (host loaded, load ~13 on 4 cores), median/max ms at 0/300/3000: `create_by_shortcut` 117/182, 154/222, 345/454; `tab_menu_open` 22/49, 23/59, 26/76; `tab_menu_create` 200/367, 245/421, 565/720. All still miss at max; P4.T3 profiling what is left.
- 2026-11-12 — **P4.T3 measurement** (host loaded, load 10-13 on 4 cores; main-thread CPU agrees within 5% across runs). p95/max ms at 0/300/3000: `create_by_shortcut` 165/182, 214/222, 416/454 (CPU median/max 33/39, 50/70, 225/285); `tab_menu_open` 43/49, 52/59, 49/76 (CPU 17-20 median, max 50-70; first open 59-90); `tab_menu_create` 265/367, 332/421, 679/720 (CPU 47/60, 80/105, 365/420). P4.T4/T7's 2 ms positioning target missed (25-30 ms at 3000, in connect-time `buildArgsMap`, `getContentHash` and recursive metadata) and P4.T5's loop-pass target missed (229-255 ms at 3000 vs 75-79 at 0). At 0 nodes, 12 of 14 samples wait in `glXSwapBuffers` — Qt 6 composites the whole window through GL because of the viewer's QOpenGLWidget, which on Xvfb is llvmpipe. At 3000: low-detail `paintCached` re-render, `QGraphicsScene::itemsBoundingRect` (no sceneRect), navigator bounds, identity hash, layers refresh. Tab create's extra cost is one more paint and flush (popup closes before creating) and a `setText` re-filter. The deferred panel build is a 117-195 ms stall the next key waits on, which the bench never measures (it pumps events before t0); at 3000, 14 of its 62 samples are in `DopeSheetPrivate::getNearestTimeFromOutputs_recursive`. Tasks P4.T8-T16 filed from this. Samples: `build/m399-prof/p4-*.txt` in the worktree.
- 2026-11-12 — **Budgets are judged on a real display (user).** Under Xvfb the viewer's QOpenGLWidget makes Qt 6 composite the whole window through llvmpipe each frame, a floor of roughly 50-100 ms that a real GPU display doesn't have. Xvfb runs stay as relative measurements; the 50 ms and 33 ms budgets are judged on the dev host's real display or the GPU host. No move to QOpenGLWindow.
- 2026-11-12 — P4.T8-T16 landed (345369cac), gate green (run 43273477412). Xvfb create bench (load ~13-15, relative only), median/max ms and main-thread CPU median at 0/300/3000: `create_by_shortcut` 109/165 (33), 124/156 (42), 235/278 (112); `tab_menu_open` 19/26, 20/49, 19/37 (CPU 17; first open 37, 30, 39); `tab_menu_create` 200/267 (45), 238/316 (69), 413/505 (232); `create_then_key` 233/309 (73), 255/520 (98), 498/579 (275). At 0 and 300 the CPU is within the create budget, so the Xvfb wall time is mostly the window composite (user: judge on a real display); at 3000, creation still costs ~110 ms of CPU and tab create ~230. Round 2 UAT AppImage out for the real-display judgement.
- 2026-11-12 — **Profile at 3000 after P4.T8-T16** (runs 203258Z, 204545Z sampled, 205718Z with an event probe; load 12-16; CPU figures repeat). Stages grow 0→3000 by about 34 ms (createNodeGui 2→20, positioning 0.3→11, engine 2→6.5); the rest of the 33→112 ms CPU is graph painting. Every creation queues a second whole-widget graph repaint about 60 ms after the first, refilling the low-detail cache; the shortcut step ends before it, the tab Enter step (which waits for the popup to hide) and \`create_then_key\` pay it, which is the tab path's extra ~120 ms CPU. Deferred panel passes run 11-12 ms median, p90 35-45, max 87-120 (budget checked only between knobs), and undo forces a full synchronous build. The composite wait is 40-45% of latency at 3000, excluded under the real-display decision. Tasks P4.T17-T22 filed. Data in \`build/m399-prof/p5*\`, \`p6-probe.jsonl\`.
- 2026-11-12 — P4.T17-T22 implemented (8489ce70f), build run 43310327389. T17: node removal (undo, delete) refiled all 3000 nodes and dropped both low-detail caches; now it repaints only the node's area (`noteNodeRemoved`); `onIdentityStateChanged` no longer calls a whole-widget `update()`. T21 partial: each paint pass validates only its own item kind; Qt's walk over ~9000 items (about 30% of paint samples) is unresolved. T18: panel steps all scheduled, budget checked per knob (7 ms), unbuilt panels dropped on undo and re-queued on redo. T19: viewer walks return at once with no connected viewer; a mid-chain insert still refreshes everything below it once (the draft flag every render reads), so the 2 ms target is not met for inserts. T20 partial: edges deleted newest first (Qt sibling renumbering); open: `new NodeGui(_nodeRoot)` re-sorts all nodes by z on each add — fixing it changes stacking, a NodeGraph design call. T22: undo flush and trailing-paint settle in the bench.
- 2026-11-12 — P4.T23 implemented in 0f2195215 (full design: an input edit stamps the edited node and dirty state is read upstream, the draft flag propagates only while it changes, and visible panels and channel-message nodes refresh from registries; five new GraphScalingConnect gtests). Release gate green at 0f2195215 (run 43348063624: both scheduler legs and ten GUI scripts). The AppImage at that commit replaces the round-2 build in `build/appimages/`. GUI bench gate preset running; P4.T3 re-measure and the budget checks for T18/T19/T21/T23 follow from it, with the budgets judged on a real display (user, 2026-11-12).
- 2026-11-12 — **GUI bench gate (Xvfb) regressed (GUI bench rule):** against the M118 AppImage in the same sitting (`--rerun pan,zoom_step,rubber_band,select_all`), 0f2195215 takes rubber_band from 0.37 to 62.8 ms per event (300 nodes) and zoom_step from 1.4 to 50.5 ms. pan goes over budget at 300 and 3000 nodes, and select_all at 300 nodes goes from 43 to 73 ms. Per-event wall time sits at the Xvfb composite floor while loop time collapses, which points to a synchronous full repaint per input event. Diagnosis and fix in progress; this blocks the gate.
- 2026-11-12 — **Bench regression root cause and fix (808e50f42):** the tab menu warm-up (`tabtabtab_natron.py` `_warm_up`, from 345369cac) called `popup.winId()` on the hidden top-level popup. That made five of the main window's children native windows, the node graph's container among them. Qt's repaint throttle checks the top-level window's last composite, so a native child is never throttled, and every `repaint()` painted and composited at once (~34 ms under llvmpipe; 0 native children on M118). The warm-up now calls the unbound `QWidget.create(popup)`, which builds the window and backing store without making it native, so the warm first open is kept. A probe showed forced repaint at 33.7 ms median before the fix and 0.03 ms after. The rebuild is running, and the bench rerun against M118 follows. Real displays would also have composited each native child separately.
- 2026-11-13 — **GUI bench rule result (Xvfb):** with the fix at 808e50f42 (release gate green, run 43366305359), the rerun of pan, zoom_step, rubber_band and select_all against the M118 AppImage in the same sitting shows 0 regressions. Per-event wall time matches M118 (rubber_band 0.29 against 0.28 ms, zoom_step 0.94 against 0.97 ms at 300 nodes), and the other gate ops were already within tolerance. Creation budgets are judged on a real display at UAT (user, 2026-11-12).
