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

- [ ] M399.P1.T1 — Reuse any Viewer already in the selection's group before creating one
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

- [ ] M399.P1.T2 — GUI script test for viewer reuse on number keys
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

- [ ] M399.P2.T1 — GUI script that reproduces the dead tab menu after global mode
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

- [ ] M399.P2.T2 — Make Tab work after every global-mode exit
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

## Phase 399.3: Measure creation and tab-menu latency

- [ ] M399.P3.T1 — GUI bench ops for node creation and tab-menu latency
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

- [ ] M399.P3.T2 — Profile both paths on the dev host and rank the misses
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

- [ ] M399.P4.T1 — Take preview, autosave and panel build off node creation's critical path
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

- [ ] M399.P4.T2 — Open the tab menu without per-open Python work
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
