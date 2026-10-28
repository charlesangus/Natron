# M118 - Large Graph GUI Responsiveness

Make the node graph fast and hang-free at 3000+ nodes with heavy expressions and labels, judged by M117 - GUI Perf Harness And Baseline's bench. A code scout on 2026-10-13 found the likely causes (`main` at `338d9324a`; M62 - Render Scaling Hotspots touched no `Gui/` file):

- **Navigator.** `NodeGraph::updateNavigator` (`Gui/NodeGraph30.cpp:457`) calls `getFullSceneScreenShot` (`Gui/NodeGraph35.cpp:61`). That re-renders the whole scene and copies it pixel by pixel. It runs from `paintEvent` whenever `_refreshOverlays` is set, which happens on every wheel zoom, middle-drag pan, resize, centre and undo. This is the likeliest hang.
- **No scene index.** `setItemIndexMethod(NoIndex)` (`Gui/GuiPrivate.cpp:394`, `Gui/Gui05.cpp:233`). At about 15–20 child items per node, 3000 nodes is 60k–100k items scanned linearly. `NodeGraph::hasItemNearbyMouse` (`Gui/NodeGraph10.cpp:109`) does two scans on every idle mouse move.
- **Per-paint work.** `Edge::paint`/`LinkArrow::paint` (`Gui/Edge.cpp:807, 993`) read the antialiasing setting knob per paint. `NodeGraphTextItem::paint` (`Gui/NodeGraphTextItem.cpp:66`) does two `mapFromScene` calls and a `QFontMetrics` per item per paint.
- **Moves.** `NodeGui::refreshPositionEnd` (`Gui/NodeGui.cpp:1170`) loops over every node, and it's called per moved node and from `resize()`. That makes drag-move O(k·N), and load and label edits O(N) per node.
- **Selection.** `selectNode` (`Gui/NodeGraph30.cpp:380`) does `std::find` on a vector, so selecting k nodes is O(k²). The rubber band (`Gui/NodeGraphPrivate.cpp:114`) is O(N) per mouse move.
- **Idle timer.** `onRefreshNodesRenderStateTimerTimeout` (`Gui/NodeGraph35.cpp:273`, 300 ms) touches every node, takes mutexes, and calls `update()` per node, even when idle.
- **Labels and links.** `setNameItemHtml` (`Gui/NodeGui.cpp:3262`) calls `toHtml()` before its no-change early-out. `onKnobsLinksChanged` (`Gui/NodeGui.cpp:2763`) is O(L²).
- **Expressions.** `executeExpression` (`Engine/Knob.cpp:3081`) runs `PyRun_String` on every cache miss, re-parsing the source each time. `setExpressionInternal` (`Knob.cpp:2830`) runs Python three times. `AppInstance::recheckInvalidExpressions` (`Engine/AppInstance.cpp:2282`) re-runs every invalid expression on every connect and activate. `refreshListenersAfterValueChange` (`Knob.cpp:4022`) recurses with no visited set.
- **Naming and paste.** `checkIfNodeLabelExists` (`Engine/NodeGroup.cpp:923`) does a linear scan inside a retry loop per pasted node. `findMaster` (`Knob.cpp:160`) does an O(N) scan with a `qDebug` per candidate, per restored link.
- **Odds and ends.** Every `NodeGui` connects to `TimeLine::frameChanged`. `moveToTrash` does a linear find, and `_nodesTrash` is never trimmed.

M117's superlinear list (`tools/bench/gui/BASELINE.md`) is authoritative. Where it disagrees with the list above, it wins. P1.T1 reconciles them before any fix.

**Budgets (real host, stripped release AppImage, the user's test machine, 3000-node `mixed` and `comp`):**
- Pan, zoom, hover and drag-move: 16 ms or less per event, including paint.
- Click-select, select-all, rubber-band (per event), delete-1000, undo, redo, set-expression, label edit and knob change with 50 dependents: 100 ms or less.
- No main-thread stall over 250 ms, outside load and pasting or duplicating 1000 nodes.
- Load, paste-1000 and delete-1000 have a scaling slope of 1.2 or less from N=300 to 3000.

The Xvfb runs only have to show no op regressing against M117's Xvfb baseline. They are expected to beat it.

## Scope

**In:**
- The node graph's paint, hit-testing, navigator, selection, move, idle-timer, label and link costs.
- A low-zoom level of detail.
- GUI-thread expression costs: compile once, a single Python run per set, an indexed invalid-expression recheck, and a visited set in listener refresh.
- Naming and link restore on paste, clone and undo.
- Bulk load, paste and delete scaling.
- Re-baselining the Xvfb file after the fixes.

**Out:**
- GIL contention between render threads and GUI-thread Python. It is measured through the stall monitor, not fixed. No milestone owns it yet.
- Progress or cancel UI for long operations. They must scale linearly instead.
- Curve editor, dope sheet and properties-bin performance.
- Engine render performance (E6 - Render Performance).
- Labelmaker annotations (M46 - Labelmaker Annotations, which builds on this milestone and must pass its bench).

**Edge cases:**
- The level of detail changes how a far-zoomed graph looks. It is gated by the user signing off the screenshots.
- BSP indexing makes item moves cost more. If P1.T2's bench shows drag-move regressing, use a tuned `bspTreeDepth` or a custom index over node rects instead of `BspTreeIndex`.
- Expression code objects must be dropped whenever the expression text, the referenced node names (renames), or the Python environment (project reload) changes.
- A pasted selection that collides with existing names still gets unique names. Only the cost changes.

**Assumptions:**
- M56 - Visual And Menu Polish's node text wrap (Phase 56.1) has landed before this starts. `setNameItemHtml` and `resize()` are rebased on top of it.

## Phase 118.1: Reconcile and navigator/paint

- [ ] M118.P1.T1 — Reconcile the target list with M117's baseline
  - files: this milestone file (`## Decisions`), no code
  - approach: read `tools/bench/gui/BASELINE.md` and the host baseline. For each superlinear or over-budget op, name the cause from the list above, or profile it (`tools/bench/profile_run.sh` style eu-stack on the Xvfb run) where none fits. Record the mapping. A hotspot the list doesn't cover gets a new trailing task in the matching phase.
  - verify: every over-budget op on the host baseline maps to at least one task.
  - size: M
- [ ] M118.P1.T2 — Stop re-rendering the whole scene for the navigator on pan and zoom
  - files: `Gui/NodeGraph.cpp`, `Gui/NodeGraph20.cpp`, `Gui/NodeGraph30.cpp`, `Gui/NodeGraph35.cpp`, `Gui/NodeGraphPrivate.h`
  - approach: split the navigator into a cached scene thumbnail and a viewport rectangle overlay. Pan, zoom, resize and centre only move the rectangle. Re-render the thumbnail only after content changes (nodes added, removed, moved or resized, or backdrop changes), debounced to 250 ms and run when the event loop is idle. Never re-render it from `paintEvent`. Compute the scene bounding rect incrementally, or cache it until content changes, instead of `calcNodesBoundingRect` per paint. Stop the `removeItem`/`addItem` dance around overlays: hide them with a flag during the render instead.
  - verify: M117's `pan` and `zoom_step` at N=3000 on Xvfb drop well below baseline, and the navigator still matches the graph after an add or move (`m55_node_graph.py` plus a new check in it).
  - size: M
- [ ] M118.P1.T3 — Index the scene and make hover hit-testing local
  - files: `Gui/GuiPrivate.cpp`, `Gui/Gui05.cpp`, `Gui/NodeGraph10.cpp`, `Gui/NodeGraph20.cpp`
  - approach: switch node graph scenes to `BspTreeIndex`. Rewrite `hasItemNearbyMouse` as a single `items(smallRect, IntersectsItemBoundingRect)` query, filtered by type, so no full-scene scan runs per mouse move. Bench `drag_move` before and after. If it regresses beyond tolerance, tune `bspTreeDepth` or keep `NoIndex` with a node-rect spatial hash for the hover query (record the choice).
  - verify: Xvfb `hover` at N=3000 is flat against N (slope 1.2 or less from 300 to 3000), and `drag_move` doesn't regress past tolerance.
  - size: M
- [ ] M118.P1.T4 — Remove per-paint settings reads and text-size checks
  - files: `Gui/Edge.cpp`, `Gui/NodeGraphTextItem.cpp`, `Gui/NodeGraph.cpp`, `Gui/NodeGraph.h`
  - approach: cache `isNodeGraphAntiAliasingEnabled()` on the `NodeGraph`, refreshed by the settings-changed path, and read the cache in `Edge::paint`/`LinkArrow::paint`. In `NodeGraphTextItem::paint`, decide "too small to draw" from `painter->worldTransform()` scale times a font height cached at `setFont`, with no `mapFromScene` or `QFontMetrics` per paint.
  - verify: an antialiasing preference toggle still repaints correctly (Xvfb shot), text still hides at the same zoom (shot pair), and `zoom_step` improves.
  - size: M
- [ ] M118.P1.T5 — Low-zoom level of detail
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`, `Gui/NodeGraph30.cpp`, `Gui/Edge.cpp`
  - approach: when the view scale drops below the threshold where text already hides, switch every NodeGui to a simplified mode: the coloured body rect and edges only, with icons, indicators, preset icon, persistent message and disabled cross hidden. Switch once per threshold crossing in the zoom handler, not per paint. Edges skip arrowheads and labels in that mode.
  - verify: Xvfb shots at three zoom levels go to the user, who signs them off. `zoom_step` and `fit_all` at N=3000 improve.
  - size: M
- [ ] M118.P1.T6 — Collapse per-node indicator items into painted state
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`, (`NodeGuiIndicator` lives in `Gui/NodeGui.cpp`/`.h`, ~3141)
  - approach: the four indicators and the two disabled-cross lines are separate `QGraphicsItem`s per node. Paint them from the node's body item using flags instead, keeping their tooltips via the body's `hoverMoveEvent` hit regions. This cuts the item count per node by about a third.
  - verify: shots of every indicator state (expression, disabled, cache, link, persistent message) match before and after. Tooltips still show (`m55_node_graph.py` hover check). Scene item count at N=3000 is logged and has dropped.
  - size: M

## Phase 118.2: Interaction walks

- [ ] M118.P2.T1 — Make node stacking after a move local
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGraph20.cpp`
  - approach: in `refreshPositionEnd`, replace the all-nodes `intersects()` loop with a scene `items(rect)` query for the node's rect. Skip the stacking pass entirely during project load and bulk operations, doing one pass at the end (P4.T1/P4.T2). `moveSelectedNodesBy` runs stacking once per drag event for the whole selection, not once per node.
  - verify: Xvfb `drag_move` with 50 selected at N=3000 is flat against N. Overlapping nodes still stack the same way (`m55_node_graph.py`).
  - size: M
- [ ] M118.P2.T2 — Set-based selection and an incremental rubber band
  - files: `Gui/NodeGraph30.cpp`, `Gui/NodeGraph25.cpp`, `Gui/NodeGraphPrivate.cpp`, `Gui/NodeGraphPrivate.h`
  - approach: keep `_selection` as a vector for order plus an `unordered_set` for membership. `selectAllNodes` sets state in one pass, without per-node `refreshEdgesVisility` storms (refresh edges once at the end). The rubber band computes the items inside the rect through the scene index, and applies only the diff against the previous rect: newly inside are selected, newly outside deselected. It doesn't `resetSelection` per mouse move.
  - verify: `select_all` and `rubber_band` at N=3000 are within budget on Xvfb relative to baseline. Selection semantics (Shift, Ctrl) are unchanged (`m55_node_graph.py`).
  - size: M
- [ ] M118.P2.T3 — Event-driven render-state indicators instead of a 300 ms whole-graph poll
  - files: `Gui/NodeGraph35.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGui.cpp`, `Engine/Node.cpp`, `Engine/Node.h`
  - approach: have the node signal (queued) when its rendering counter crosses zero or its persistent message changes. NodeGui adds itself to a dirty set, and the timer runs only while the set is non-empty, refreshing only those nodes. With no render running and no message changes, the timer never fires.
  - verify: Xvfb `idle` CPU at N=3000 drops to near the N=300 figure. The rendering indicator still lights during a render (`m55_node_graph.py` or a new check).
  - size: M
- [ ] M118.P2.T4 — Cheap label no-op and linear link-arrow diff
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
  - approach: `setNameItemHtml` compares its inputs against the last-set inputs cached on the NodeGui before building any HTML or calling `toHtml()`. It calls `adjustSize`/`resize` only when the text changed. `onKnobsLinksChanged` diffs old and new links through a map keyed by (master node, link type), instead of nested finds.
  - verify: `edit_label` and `knob_change_fanout` improve on Xvfb. Labels and link arrows look unchanged (shots). A gtest-free check: a no-op label set doesn't call `adjustSize` (counter logged in a debug build).
  - size: M
- [ ] M118.P2.T5 — Constant-time trash and bounded trash growth
  - files: `Gui/NodeGraph35.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: `moveToTrash`/`restoreFromTrash` use an index (a hash map from `NodeGui*` to position, or swap-and-pop). Destroy trashed NodeGuis when the undo command that owns them is destroyed (stack limit or clean), instead of keeping them until the graph dies.
  - verify: `delete_1000` and undo/redo at N=3000 are within baseline tolerance or better. Deleting, then running enough commands to drop the undo, frees the trashed items (count logged).
  - size: M

## Phase 118.3: Expressions and naming

- [ ] M118.P3.T1 — Compile each expression once
  - files: `Engine/Knob.cpp`, `Engine/Knob.h`, `Engine/KnobImpl.h`, `Tests/Knob_Test.cpp`
  - approach: when an expression is set, compile the generated function once and keep the code or function object per (dimension, view) on the knob, under the GIL. `executeExpression` then calls it with `(time, view)` through `PyObject_Vectorcall`, with no string building or `PyRun_String`. Drop it on expression change, on rename of a referenced node, and on project close. The existing `_exprRes` per-time cache stays.
  - verify: the existing expression tests are green. A new gtest counts compiles: setting once and evaluating 1000 times compiles once. Xvfb `frame_step` and `knob_change_fanout` improve.
  - size: L
- [ ] M118.P3.T2 — One Python run per setExpression
  - files: `Engine/Knob.cpp`, `Engine/Knob.h`
  - approach: fold `validateExpression`'s define-and-call and `parseListenersFromExpression` into one script run, which defines the function, evaluates it once, and returns the listener list. It reuses P3.T1's compiled object instead of compiling again.
  - verify: the expression tests are green, including invalid-expression and dependency tests. Xvfb `set_expression` improves.
  - size: M
- [ ] M118.P3.T3 — Recheck only the invalid expressions that reference what changed
  - files: `Engine/AppInstance.cpp`, `Engine/AppInstance.h`, `Engine/NodeInputs.cpp`, `Engine/Node.cpp`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: when an expression fails, record the node and script names it references (from the text, or from the listener parse when available). Index invalid expressions by those names. On connect, activate, create or rename, recheck only the entries indexed under the affected node's names. Entries with no extractable names stay in an "always recheck" bucket.
  - verify: a gtest with 200 broken expressions referencing a missing node: creating that node fixes all of them, and connecting an unrelated node rechecks none (counter). Xvfb `connect` improves.
  - size: M
- [ ] M118.P3.T4 — Visited set in listener refresh
  - files: `Engine/Knob.cpp`, `Tests/Knob_Test.cpp`
  - approach: `refreshListenersAfterValueChange` carries a visited set through its recursion, so each listener refreshes once per change even on diamond-shaped expression graphs.
  - verify: a gtest with a 20-level diamond of expressions finishes in milliseconds and refreshes each knob exactly once (counter). The existing tests are green.
  - size: M
- [ ] M118.P3.T5 — Linear naming and link restore on paste, clone and undo
  - files: `Engine/NodeGroup.cpp`, `Engine/NodeGroup.h`, `Gui/NodeGraphPrivate10.cpp`, `Engine/Knob.cpp`
  - approach: build one name set (script names and labels) per paste, clone or undo batch, and use it for `checkIfNodeLabelExists` and the retry loop. `findMaster` resolves through a fully qualified name to node map built once per restore batch, without a per-candidate `qDebug`.
  - verify: a gtest pastes 1000 nodes into a 3000-node group and gets unique names with time linear in the pasted count. Xvfb `paste_1000` and `duplicate_100` improve.
  - size: M

## Phase 118.4: Bulk load, paste and delete

- [ ] M118.P4.T1 — Defer per-node layout work during project load
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGraph.cpp`, `Gui/ProjectGui.cpp`
  - approach: while the project is loading, `NodeGui::resize` and `refreshPosition` skip stacking, the edge refresh and the navigator invalidation. On load end, the graph does one stacking pass, one edge refresh and one navigator render.
  - verify: Xvfb `load` slope is 1.2 or less from 300 to 3000. A loaded project looks the same as before (shots of the `mixed` fixture).
  - size: M
- [ ] M118.P4.T2 — Batch node creation, paste and delete in the GUI
  - files: `Gui/NodeGraph.cpp`, `Gui/NodeGraph40.cpp`, `Gui/NodeGraphPrivate10.cpp`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: paste, duplicate, clone and delete of many nodes push one undo command for the batch. They select once at the end instead of per-node `selectNode`/`clearSelection`, and refresh edges and stacking once.
  - verify: `paste_1000`, `duplicate_100` and `delete_1000` slopes are 1.2 or less. Undo restores the whole batch in one step (`m55_node_graph.py` plus a new check).
  - size: M
- [ ] M118.P4.T3 — One timeline connection per graph
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`, `Gui/NodeGraph.cpp`, `Gui/NodeGraph.h`
  - approach: replace each NodeGui's connection to `TimeLine::frameChanged` with one connection on the `NodeGraph`. It forwards only to nodes that registered as frame-dependent (animated indicator, frame-varying label).
  - verify: Xvfb `frame_step` at N=3000 is near the N=300 figure. Animated-knob indicators still update when scrubbing (shot).
  - size: M

## Phase 118.5: Measure, re-baseline and UAT

- [ ] M118.P5.T1 — Xvfb bench against M117's baseline, then re-baseline
  - files: `tools/bench/gui/baselines/xvfb.jsonl`, `tools/bench/gui/BASELINE.md`
  - approach: one release build. Run the full Xvfb bench three times, and run `compare.py` against M117's baseline. Every op must be at or under baseline, and any op still over is profiled and gets a new task in the matching phase. Then commit the new Xvfb baseline with a before/after table and the new `--scaling` table in `BASELINE.md`.
  - verify: `compare.py` against the old baseline exits 0, and every op on the old superlinear list now has a slope of 1.2 or less.
  - size: M
- [ ] M118.P5.T2 — Real-host budget run, shots and AppImage UAT
  - files: `tools/bench/gui/baselines/host-<hostname>.jsonl`, `tools/bench/gui/BASELINE.md`
  - approach: package the stripped release AppImage. Send the user the level-of-detail and indicator shots. The user runs `run.sh --host`. Record the results against the budgets in `BASELINE.md`. The UAT checklist: pan, zoom, hover, drag and rubber-band a 3000-node graph; type an expression; edit a label; paste and delete 1000 nodes; leave it idle and check CPU.
  - verify: every budget holds on the user's host, or the user waives a named op. The user signs off the shots and the UAT. `m55_node_graph.py`, `m55_input_pipes.py` and `m45_tab_menu.py` report 0 failures on the gate build.
  - size: S

**Verification gate:**
- One build. Full ctest is green in both scheduler modes, including the new expression and naming tests, and CI is green.
- The Xvfb bench shows no op regressing against M117's baseline, and the new baseline is committed.
- The real-host run meets every budget, unless the user waived an op.
- The user signs off the shots and the AppImage UAT.

## Decisions

- 2026-10-28: Phase 118.1 implemented (build pending; bench at the gate). Stall-to-cause map (P1.T1): pan/zoom/drag/resize/centre → navigator re-render + per-paint walk; hover → unindexed scans + forced repaint; drag_move → P2.T1 + navigator; click/select-all/rubber-band → P2.T2; edit_label → P2.T4; set_expression and knob fan-out → P3.T1/T2/T4; frame_step → P4.T3; load → P4.T1 + naming; paste/duplicate → P3.T5, P4.T2; delete/undo hang → double navigator update (fixed in T2), activate/deactivate and P2.T5 (confirm with eu-stack). Navigator: bounds-only thumbnail, re-rendered at most every 250 ms, never in paint. Hit tests walk top-level items (no spatial index: panning moves the root item). Antialias setting cached, refreshed on Preferences save. Low-detail decided per paint; items skipped in paint, not hidden. One badge item and one cross item per node (12 → 2 items); tooltips via NodeGraph::viewportEvent.
- 2026-10-13 — Budgets apply to the real host the user tests on, and only Xvfb regressions are checked in the container (user). The milestone does not name the machine.
- 2026-10-13 — Expression work covers GUI-thread costs only (user). GIL contention between render threads and the GUI is measured by the stall monitor, not fixed.
- 2026-10-13 — Low-zoom level of detail is allowed, gated by the user signing off the screenshots (user).
- 2026-10-13 — Long operations get no progress or cancel UI; they must scale linearly (user).
- 2026-10-13 — Deps are M117 - GUI Perf Harness And Baseline (the baseline comes before any fix) and M56 - Visual And Menu Polish (it reworks node text layout in `NodeGui`). Epic E10 - UI Polish (user).

- 2026-10-28 (from M117's baseline): delete of ~30+ nodes and its undo/redo hang (not just slow) from ~60 nodes; load, duplicate_100, paste_100 superlinear (slopes up to 1.8) and time out at N≥2000; hover times out at 2000–3000. Data: M117's tools/bench/gui/BASELINE.md.
