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

- [x] M118.P1.T1 — Reconcile the target list with M117's baseline
  - files: this milestone file (`## Decisions`
- 2026-11-08 — Budgets (user): keep M118 open for more work. GPU-host bench (RX 7900 XTX), after build at 3000 nodes mixed/comp: pan 42–46, zoom 22–24, hover 16–17, drag 38–46 ms/event (budget 16; flat scaling, so a fixed per-paint cost); select_all and knob_change_fanout pass on mixed, miss on comp (~100–112 ms); delete_1000 204–233, undo 870–914 (stalls to 919), redo 184–210 ms (budget 100); click-select, rubber-band, set-expression, label edit pass; load/paste/delete slopes pass. The M117 'before' build can't create the bench's CheckerBoard, so the before side is re-run against M118's merge base with main (0553d5c3a, branch bench/m118-base).
), no code
  - approach: read `tools/bench/gui/BASELINE.md` and the host baseline. For each superlinear or over-budget op, name the cause from the list above, or profile it (`tools/bench/profile_run.sh` style eu-stack on the Xvfb run) where none fits. Record the mapping. A hotspot the list doesn't cover gets a new trailing task in the matching phase.
  - verify: every over-budget op on the host baseline maps to at least one task.
  - size: M
- [x] M118.P1.T2 — Stop re-rendering the whole scene for the navigator on pan and zoom
  - files: `Gui/NodeGraph.cpp`, `Gui/NodeGraph20.cpp`, `Gui/NodeGraph30.cpp`, `Gui/NodeGraph35.cpp`, `Gui/NodeGraphPrivate.h`
  - approach: split the navigator into a cached scene thumbnail and a viewport rectangle overlay. Pan, zoom, resize and centre only move the rectangle. Re-render the thumbnail only after content changes (nodes added, removed, moved or resized, or backdrop changes), debounced to 250 ms and run when the event loop is idle. Never re-render it from `paintEvent`. Compute the scene bounding rect incrementally, or cache it until content changes, instead of `calcNodesBoundingRect` per paint. Stop the `removeItem`/`addItem` dance around overlays: hide them with a flag during the render instead.
  - verify: M117's `pan` and `zoom_step` at N=3000 on Xvfb drop well below baseline, and the navigator still matches the graph after an add or move (`m55_node_graph.py` plus a new check in it).
  - size: M
- [x] M118.P1.T3 — Index the scene and make hover hit-testing local
  - files: `Gui/GuiPrivate.cpp`, `Gui/Gui05.cpp`, `Gui/NodeGraph10.cpp`, `Gui/NodeGraph20.cpp`
  - approach: switch node graph scenes to `BspTreeIndex`. Rewrite `hasItemNearbyMouse` as a single `items(smallRect, IntersectsItemBoundingRect)` query, filtered by type, so no full-scene scan runs per mouse move. Bench `drag_move` before and after. If it regresses beyond tolerance, tune `bspTreeDepth` or keep `NoIndex` with a node-rect spatial hash for the hover query (record the choice).
  - verify: Xvfb `hover` at N=3000 is flat against N (slope 1.2 or less from 300 to 3000), and `drag_move` doesn't regress past tolerance.
  - size: M
- [x] M118.P1.T4 — Remove per-paint settings reads and text-size checks
  - files: `Gui/Edge.cpp`, `Gui/NodeGraphTextItem.cpp`, `Gui/NodeGraph.cpp`, `Gui/NodeGraph.h`
  - approach: cache `isNodeGraphAntiAliasingEnabled()` on the `NodeGraph`, refreshed by the settings-changed path, and read the cache in `Edge::paint`/`LinkArrow::paint`. In `NodeGraphTextItem::paint`, decide "too small to draw" from `painter->worldTransform()` scale times a font height cached at `setFont`, with no `mapFromScene` or `QFontMetrics` per paint.
  - verify: an antialiasing preference toggle still repaints correctly (Xvfb shot), text still hides at the same zoom (shot pair), and `zoom_step` improves.
  - size: M
- [x] M118.P1.T5 — Low-zoom level of detail
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`, `Gui/NodeGraph30.cpp`, `Gui/Edge.cpp`
  - approach: when the view scale drops below the threshold where text already hides, switch every NodeGui to a simplified mode: the coloured body rect and edges only, with icons, indicators, preset icon, persistent message and disabled cross hidden. Switch once per threshold crossing in the zoom handler, not per paint. Edges skip arrowheads and labels in that mode.
  - verify: Xvfb shots at three zoom levels go to the user, who signs them off. `zoom_step` and `fit_all` at N=3000 improve.
  - size: M
- [x] M118.P1.T6 — Collapse per-node indicator items into painted state
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`, (`NodeGuiIndicator` lives in `Gui/NodeGui.cpp`/`.h`, ~3141)
  - approach: the four indicators and the two disabled-cross lines are separate `QGraphicsItem`s per node. Paint them from the node's body item using flags instead, keeping their tooltips via the body's `hoverMoveEvent` hit regions. This cuts the item count per node by about a third.
  - verify: shots of every indicator state (expression, disabled, cache, link, persistent message) match before and after. Tooltips still show (`m55_node_graph.py` hover check). Scene item count at N=3000 is logged and has dropped.
  - size: M

## Phase 118.2: Interaction walks

- [x] M118.P2.T1 — Make node stacking after a move local
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGraph20.cpp`
  - approach: in `refreshPositionEnd`, replace the all-nodes `intersects()` loop with a scene `items(rect)` query for the node's rect. Skip the stacking pass entirely during project load and bulk operations, doing one pass at the end (P4.T1/P4.T2). `moveSelectedNodesBy` runs stacking once per drag event for the whole selection, not once per node.
  - verify: Xvfb `drag_move` with 50 selected at N=3000 is flat against N. Overlapping nodes still stack the same way (`m55_node_graph.py`).
  - size: M
- [x] M118.P2.T2 — Set-based selection and an incremental rubber band
  - files: `Gui/NodeGraph30.cpp`, `Gui/NodeGraph25.cpp`, `Gui/NodeGraphPrivate.cpp`, `Gui/NodeGraphPrivate.h`
  - approach: keep `_selection` as a vector for order plus an `unordered_set` for membership. `selectAllNodes` sets state in one pass, without per-node `refreshEdgesVisility` storms (refresh edges once at the end). The rubber band computes the items inside the rect through the scene index, and applies only the diff against the previous rect: newly inside are selected, newly outside deselected. It doesn't `resetSelection` per mouse move.
  - verify: `select_all` and `rubber_band` at N=3000 are within budget on Xvfb relative to baseline. Selection semantics (Shift, Ctrl) are unchanged (`m55_node_graph.py`).
  - size: M
- [x] M118.P2.T3 — Event-driven render-state indicators instead of a 300 ms whole-graph poll
  - files: `Gui/NodeGraph35.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGui.cpp`, `Engine/Node.cpp`, `Engine/Node.h`
  - approach: have the node signal (queued) when its rendering counter crosses zero or its persistent message changes. NodeGui adds itself to a dirty set, and the timer runs only while the set is non-empty, refreshing only those nodes. With no render running and no message changes, the timer never fires.
  - verify: Xvfb `idle` CPU at N=3000 drops to near the N=300 figure. The rendering indicator still lights during a render (`m55_node_graph.py` or a new check).
  - size: M
- [x] M118.P2.T4 — Cheap label no-op and linear link-arrow diff
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
  - approach: `setNameItemHtml` compares its inputs against the last-set inputs cached on the NodeGui before building any HTML or calling `toHtml()`. It calls `adjustSize`/`resize` only when the text changed. `onKnobsLinksChanged` diffs old and new links through a map keyed by (master node, link type), instead of nested finds.
  - verify: `edit_label` and `knob_change_fanout` improve on Xvfb. Labels and link arrows look unchanged (shots). A gtest-free check: a no-op label set doesn't call `adjustSize` (counter logged in a debug build).
  - size: M
- [x] M118.P2.T5 — Constant-time trash and bounded trash growth
  - files: `Gui/NodeGraph35.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: `moveToTrash`/`restoreFromTrash` use an index (a hash map from `NodeGui*` to position, or swap-and-pop). Destroy trashed NodeGuis when the undo command that owns them is destroyed (stack limit or clean), instead of keeping them until the graph dies.
  - verify: `delete_1000` and undo/redo at N=3000 are within baseline tolerance or better. Deleting, then running enough commands to drop the undo, frees the trashed items (count logged).
  - size: M

## Phase 118.3: Expressions and naming

- [x] M118.P3.T1 — Compile each expression once
  - files: `Engine/Knob.cpp`, `Engine/Knob.h`, `Engine/KnobImpl.h`, `Tests/Knob_Test.cpp`
  - approach: when an expression is set, compile the generated function once and keep the code or function object per (dimension, view) on the knob, under the GIL. `executeExpression` then calls it with `(time, view)` through `PyObject_Vectorcall`, with no string building or `PyRun_String`. Drop it on expression change, on rename of a referenced node, and on project close. The existing `_exprRes` per-time cache stays.
  - verify: the existing expression tests are green. A new gtest counts compiles: setting once and evaluating 1000 times compiles once. Xvfb `frame_step` and `knob_change_fanout` improve.
  - size: L
- [x] M118.P3.T2 — Two Python runs per setExpression
  - files: `Engine/Knob.cpp`, `Engine/Knob.h`
  - approach: fold `validateExpression`'s define and call into one script run, reusing P3.T1's compiled object. `parseListenersFromExpression` stays a separate run after `setExpressionInternal` stores the expression: `addListener`'s metadata cycle check and `onKnobSlaved` read that stored state, and validating with it cleared stops an expression that reads its own knob recursing.
  - verify: the expression tests are green, including invalid-expression and dependency tests. Xvfb `set_expression` improves.
  - size: M
- [x] M118.P3.T3 — Recheck only the invalid expressions that reference what changed
  - files: `Engine/AppInstance.cpp`, `Engine/AppInstance.h`, `Engine/NodeInputs.cpp`, `Engine/Node.cpp`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: when an expression fails, record the node and script names it references (from the text, or from the listener parse when available). Index invalid expressions by those names. On connect, activate, create or rename, recheck only the entries indexed under the affected node's names. Entries with no extractable names stay in an "always recheck" bucket.
  - verify: a gtest with 200 broken expressions referencing a missing node: creating that node fixes all of them, and connecting an unrelated node rechecks none (counter). Xvfb `connect` improves.
  - size: M
- [x] M118.P3.T4 — Visited set in listener refresh
  - files: `Engine/Knob.cpp`, `Tests/Knob_Test.cpp`
  - approach: `refreshListenersAfterValueChange` carries a visited set through its recursion, so each listener refreshes once per change even on diamond-shaped expression graphs.
  - verify: a gtest with a 20-level diamond of expressions finishes in milliseconds and refreshes each knob exactly once (counter). The existing tests are green.
  - size: M
- [x] M118.P3.T5 — Linear naming and link restore on paste, clone and undo
  - files: `Engine/NodeGroup.cpp`, `Engine/NodeGroup.h`, `Gui/NodeGraphPrivate10.cpp`, `Engine/Knob.cpp`
  - approach: build one name set (script names and labels) per paste, clone or undo batch, and use it for `checkIfNodeLabelExists` and the retry loop. `findMaster` resolves through a fully qualified name to node map built once per restore batch, without a per-candidate `qDebug`.
  - verify: a gtest pastes 1000 nodes into a 3000-node group and gets unique names with time linear in the pasted count. Xvfb `paste_1000` and `duplicate_100` improve.
  - size: M

## Phase 118.4: Bulk load, paste and delete

- [x] M118.P4.T1 — Defer per-node layout work during project load
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGraph.cpp`, `Gui/ProjectGui.cpp`
  - approach: while the project is loading, `NodeGui::resize` and `refreshPosition` skip stacking, the edge refresh and the navigator invalidation. On load end, the graph does one stacking pass, one edge refresh and one navigator render.
  - verify: Xvfb `load` slope is 1.2 or less from 300 to 3000. A loaded project looks the same as before (shots of the `mixed` fixture).
  - size: M
- [x] M118.P4.T2 — Batch node creation, paste and delete in the GUI
  - files: `Gui/NodeGraph.cpp`, `Gui/NodeGraph40.cpp`, `Gui/NodeGraphPrivate10.cpp`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: paste, duplicate, clone and delete of many nodes push one undo command for the batch. They select once at the end instead of per-node `selectNode`/`clearSelection`, and refresh edges and stacking once.
  - verify: `paste_1000`, `duplicate_100` and `delete_1000` slopes are 1.2 or less. Undo restores the whole batch in one step (`m55_node_graph.py` plus a new check).
  - size: M
- [x] M118.P4.T3 — One timeline connection per graph
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

## Phase 118.6: Budget closure

Added 2026-11-08 after the GPU-host bench (user: keep M118 open for budget work). Causes below are read from code, not yet profiled; T1 confirms or corrects them. Note: interaction cost is not flat in N (comp pan 15.8→45.8 ms/event from 300 to 3000); the N≤1000 figures sit on a ~16.7 ms frame floor, and the harness's per-event time includes a forced full repaint plus the event-loop pass after each event.

- [x] M118.P6.T1 — Split the bench timing and profile the misses on the dev host
  - files: `tools/bench/gui/gui_bench.py`, `tools/bench/gui/compare.py`, `tools/bench/gui/README.md`
  - approach: per event, record the step's wall and main-thread CPU (`time.thread_time`) and the loop pass after it separately; add `--no-forced-repaint` (process posted events in the step instead). Under Xvfb, release build, comp, N=3000, reps 1, one op per run (pan, hover, zoom_step, drag_move_1, select_all, knob_change_fanout, undo, redo) with eu-stack sampling (`tools/bench/sample_stacks.sh`, `-u root --privileged`), each run under 10 min. Record the top frames per op in Decisions and correct T2–T8 where they disagree.
  - verify: every failing op has a profile-backed cause in Decisions; the frame-floor finding goes to the user as a budget question (one frame vs main-thread CPU per event).
  - size: M
- [ ] M118.P6.T2 — Batch Python (de)declaration for bulk activate/deactivate
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Gui/NodeGraphUndoRedo.cpp`, `Gui/CurveEditor.cpp`, `Gui/DopeSheet.cpp`, `Tests/GraphScalingGuiEdits_Test.cpp`
  - approach: undo of a 1000-node delete runs two Python scripts per node (`declareAllPythonAttributes`, Node.cpp ~9933–10068) plus expression restore and recheck per node; delete/redo run a `del` script per node and O(k·N) CurveEditor/DopeSheet removals. Batch: one script for all nodes declared/deleted in an undo/redo/delete (no per-node Script Editor echo, one expression-generation bump); expression restore and recheck once per batch; indexed CurveEditor/DopeSheet removal, `centerOn` once.
  - verify: gtest — undoing a 1000-node delete runs ≤ 2 scripts (counter), expressions and `.expression0` survive; Xvfb `undo`/`redo`/`delete_1000` at 3000 drop sharply; `m55_node_graph.py` 0 failures.
  - size: M
- [ ] M118.P6.T3 — Drag: no O(N) work per move
  - files: `Gui/NodeGraph20.cpp`, `Gui/NodeGui.cpp`, `Gui/NodeGraph30.cpp`, `Gui/NodeGraphPrivate.cpp`
  - approach: splice-hint candidates from the selection rect plus tolerance only, rect-tested before any shape test; during a drag gesture grow the node bounds incrementally and render the navigator thumbnail once on release; invalidate the moved rects, not the whole viewport.
  - verify: Xvfb `drag_move_1`/`_50` at 3000 drop, no stalls; splice hint and navigator checks in `m55_node_graph.py` pass.
  - size: M
- [x] M118.P6.T4 — Pan by scrolling the view, not moving the root item
  - files: `Gui/NodeGraph.cpp`, `Gui/NodeGraph15.cpp`, `Gui/NodeGraph20.cpp`, `Gui/NodeGraph40.cpp`, `tools/bench/gui/gui_bench.py`
  - approach: a large fixed sceneRect with hidden scrollbars so pan, autoscroll and centre go through `scrollContentsBy` (viewport blit, only the exposed strip painted); overlays positioned on view change, never in `paintEvent`; update the harness's `root_offset` and `to_view`.
  - verify: Xvfb `pan` at 3000 is near the N=300 figure; Xvfb shots before/after pan and fit match.
  - size: L
- [x] M118.P6.T5 — Cheap low-detail paint
  - files: `Gui/NodeGui.cpp`, `Gui/Edge.cpp`, `Gui/NodeGraph.cpp`
  - approach: measure NoCache vs DeviceCoordinateCache in low detail; in low detail draw node bodies and edges as batched non-AA primitives from one layer item instead of per-item paint.
  - verify: Xvfb `zoom_step` and `pan` at 3000 drop; three-zoom shots signed off by the user again.
  - size: M
- [ ] M118.P6.T6 — Grid index for hit tests
  - files: `Gui/NodeGraph10.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraphPrivate.cpp`, `Gui/NodeGui.cpp`, `Gui/Edge.cpp`
  - approach: a uniform grid of node and edge bounds in `_nodeRoot` coordinates, updated on move and edge geometry change; hover does one tolerance query; splice hint, rubber band and tooltips use it too.
  - verify: Xvfb `hover` flat from 300 to 3000; `m55_node_graph.py` hover and tooltip checks pass.
  - size: M
- [x] M118.P6.T7 — One downstream walk per value-change cascade
  - files: `Engine/EffectInstance.cpp`, `Engine/Knob.cpp`, `Engine/Node.cpp`, `Tests/Knob_Test.cpp`
  - approach: collect the nodes reached by an outermost set/endChanges; at its end, one multi-source `refreshShownDownstreamInputRelatedData`, one `getViewersDownstreamOf`, previews refreshed once.
  - verify: gtest — 50 dependents on a 3000-chain each visit a node once (counter); Xvfb `knob_change_fanout` on comp near wide.
  - size: M
- [ ] M118.P6.T8 — Bulk selection without per-node redraw storms
  - files: `Gui/NodeGraphPrivate.cpp`, `Gui/NodeGui.cpp`, `Gui/Edge.cpp`
  - approach: `setUserSelected` only acts on change; `replaceSelection` invalidates the navigator once and skips per-node edge/indicator updates whose state didn't change.
  - verify: Xvfb `select_all` at 3000 improves and stays linear 1000→3000; selection shots match.
  - size: S
- [ ] M118.P6.T10 — Edges and link arrows out of `_nodeRoot`; z bands instead of `stackBefore`
  - files: `Gui/NodeGui.cpp`, `Gui/Edge.cpp`, `Gui/NodeGraph35.cpp`, `Gui/NodeGraph.cpp`, `Gui/NodeGraphPrivate.*`
  - approach: Qt's sibling-order work under `_nodeRoot` (~9000 children) is O(siblings) per `stackBefore`/`setParentItem`. Edges and LinkArrows go in their own container items. Node stacking uses z bands, with no per-node `stackBefore`; a restack only happens on drag release.
  - verify: Xvfb gate `drag_move_50` and `undo` at 3000 drop sharply; node stacking and overlap shots match; `m55_node_graph.py` passes.
  - size: M
- [ ] M118.P6.T11 — Cached low-detail layer render with partial invalidation
  - files: `Gui/NodeGraphLowDetailLayer.{h,cpp}`, `Gui/NodeGraph10.cpp`, `Gui/NodeGraph40.cpp`
  - approach: the layers render to a cached pixmap invalidated by changed rects. Item changes coalesce into one layer update per event-loop pass. `getGridItems` never refiles the grid inside paint. `centerOnAllNodes` doesn't queue a second full paint.
  - verify: Xvfb gate `select_all`, `fit_all` loop pass and `click_select` p95 at 3000 drop; the three-zoom shots match.
  - size: M
- [ ] M118.P6.T12 — Node GUI construction does each refresh once per node in bulk paste
  - files: `Gui/NodeGui.cpp`, `Gui/Edge.cpp`
  - approach: `setNameItemHtml` runs once per created node, not three times. During a paste batch, edge and data-kind pen refreshes are deferred to the batch end. `Edge::initLine` caches label widths.
  - verify: Xvfb gate `paste_100`, `duplicate_100` and `paste_1000` drop; the paste and duplicate checks in `m55_node_graph.py` pass.
  - size: M

- [ ] M118.P6.T9 — GPU-host bench rerun against the merge base
  - files: `tools/bench/gui/BASELINE.md`, this milestone file
  - approach: one release AppImage of this branch and one of `bench/m118-base` (0553d5c3a); one sitting on the GPU host, before and after back to back, all topologies 300–3000; report per-event CPU and the loop pass separately.
  - verify: every budget holds, or the user waives the op by name; the table goes in BASELINE.md.
  - size: S

## Decisions

- 2026-11-07 — Release gate green with the node graph and tab menu GUI scripts at 4851c9775, including the single bulk-delete prompt. UAT AppImage refreshed. P5.T1 and the budget run wait on the GPU host bench session.

- 2026-11-07 — GPU host bench session (user): one sitting covers M118 (against M117 as before), M300 - Unified Tiered Cache admission policies, M329 - Viewer Draw States draw-state budget, and the M328 - Deferred Viewer Stage heavy scene gate. The M130 - Native Edge-Preserving And Directional Filters hot-pixel check (GPU compute Off against Auto) rides along.

- 2026-11-07 — Bulk delete prompt written (code on milestone/m118-large-graph-gui-responsiveness): one summary, capped at 12 link lines with an "and N more" line; none when nothing breaks.

- 2026-11-07 — Bulk delete (user): deleting several nodes whose knobs other nodes link to shows one summary prompt listing what breaks, not one prompt per node.

- 2026-11-07 — Bench move (user): the host rebooted during the Xvfb bench (cause unproven, no kernel log visible; the partial chain-only run is discarded). All benchmarking runs at one time on the GPU host, before and after in the same sitting so the numbers are fair and comparable. This replaces P5.T1's three Xvfb runs on this host; the comparison uses a before build run there, not the old Xvfb baseline.

- 2026-11-07 — Remote release gate green with M118's GUI scripts at 32fa7e605.

- 2026-11-06 — Bench `delete_1000`'s stall was the per-node broken-link modal, then a real runaway in the untimed undo: `deactivate`/`activate` replace Param wrappers, losing `.expression0`, so reads fail repeatedly (147,649 failed evaluations in 300 s at n=60) and each failure wrote content and invalidated the hash. Baseline never had `delete_1000`, `undo` or `redo`. Fixed, not yet built. Hover has no regression: main-thread CPU per event matches a pre-merge control, and the 41 s figure was host load.
- 2026-11-05 — M118.P3.T2 reduced from one run to two: listener registration has to follow storing the expression, while the call has to precede it, so a true single run would change cycle detection, slaving and self-reading expressions. The Xvfb bench decides whether more is needed.
- 2026-11-02 — Audit: P1–P4 are committed and pushed (97eeb6867 P1.T2–T6, 5197e1999 P2, 96535f12b P3, 67f11e035 P4); P1.T1's map is the 2026-10-28 note below. P3.T2 is left open: the define, validate and listener-parse runs were not merged (see that note). P1.T3 deviates: no scene index (panning moves the root item), hit tests walk top-level items. P2.T5 is an indexed trash; trash growth was already bounded by the undo history. No build of P1–P4 is recorded. The branch stacks on M117 - GUI Perf Harness And Baseline and is 29 commits behind main.
- 2026-10-28: Phases 118.2–118.3 implemented (builds pending). P2: batched local restacking (NodeStackingBatch), indexed node/selection lists, render-state indicators driven by Node rendering signals (300 ms throttle), label no-op cache, map-based link arrows; rubber band already did no per-move work; trash already trimmed by undo history. P3: expressions compiled once (PyObject_Call, one function per dimension; failing evaluations lose the outer <module> traceback line); define/validate/listener-parse runs not merged; invalid expressions rechecked by referenced name (dynamic lookups always rechecked; full recheck at load and user-knob changes); listeners refreshed once in topological pre-order; NodeCollection::NamingBatch for paste/duplicate/restore. Overlaps with the other host's expression milestones: P3.T1 ↔ M263.P1.T1 (this keeps integer frames as ints), P3.T2 ↔ M263.P2.T2, P3.T5 ↔ M263.P2.T1.
- 2026-10-28: Phase 118.1 implemented (build pending; bench at the gate). Stall-to-cause map (P1.T1): pan/zoom/drag/resize/centre → navigator re-render + per-paint walk; hover → unindexed scans + forced repaint; drag_move → P2.T1 + navigator; click/select-all/rubber-band → P2.T2; edit_label → P2.T4; set_expression and knob fan-out → P3.T1/T2/T4; frame_step → P4.T3; load → P4.T1 + naming; paste/duplicate → P3.T5, P4.T2; delete/undo hang → double navigator update (fixed in T2), activate/deactivate and P2.T5 (confirm with eu-stack). Navigator: bounds-only thumbnail, re-rendered at most every 250 ms, never in paint. Hit tests walk top-level items (no spatial index: panning moves the root item). Antialias setting cached, refreshed on Preferences save. Low-detail decided per paint; items skipped in paint, not hidden. One badge item and one cross item per node (12 → 2 items); tooltips via NodeGraph::viewportEvent.
- 2026-10-13 — Budgets apply to the real host the user tests on, and only Xvfb regressions are checked in the container (user). The milestone does not name the machine.
- 2026-10-13 — Expression work covers GUI-thread costs only (user). GIL contention between render threads and the GUI is measured by the stall monitor, not fixed.
- 2026-10-13 — Low-zoom level of detail is allowed, gated by the user signing off the screenshots (user).
- 2026-10-13 — Long operations get no progress or cancel UI; they must scale linearly (user).
- 2026-10-13 — Deps are M117 - GUI Perf Harness And Baseline (the baseline comes before any fix) and M56 - Visual And Menu Polish (it reworks node text layout in `NodeGui`). Epic E10 - UI Polish (user).

- 2026-10-28 (from M117's baseline): delete of ~30+ nodes and its undo/redo hang (not just slow) from ~60 nodes; load, duplicate_100, paste_100 superlinear (slopes up to 1.8) and time out at N≥2000; hover times out at 2000–3000. Data: M117's tools/bench/gui/BASELINE.md.

- 2026-11-09 — GPU-host trimmed bench (`results-20261109T162629Z`, comp, 300/3000, 3 reps, 17 min). Base 0553d5c3a wedged in knob_change_fanout at n=300 (>300 s), so before/after compares at 300 only: undo −98%, redo −93%, delete_1000 −92%, paste/duplicate −73%, drag_move_1 −47%. click_select +33% is one extra 16.7 ms frame (cpu 9.3→2.6 ms), not a real regression. After build at 3000: every scaling slope ≤1.09 (budget 1.2). Over budget at 3000: per-event max for pan 28, zoom 50, hover 65, drag_move_1 76, drag_move_50 64 ms (budget 16); undo 795 ms with a 897 ms stall, redo 117, delete_1000 137 ms (budget 100). Within budget: click_select 57, select_all 73, knob_change_fanout 94, set_expression 6, edit_label 3 ms. P6 covers the remaining misses.
- 2026-11-10 — Resume audit: P6.T1's harness split (d56ce1b97) and profiler tooling are in, with no profile causes recorded yet; 3733ada06 holds most of P6.T2, P6.T3, P6.T6 and P6.T8 (grid index, batched Python declaration, deferred navigator, bulk-selection redraws), unverified because HEAD was never built. P6.T4, P6.T5 and P6.T7 are not started, and P6.T9 needs the GPU host. The 2026-11-09 GPU-host bench most likely measured 3753ed99, from before those fixes. Main merged in at b31cf1716 and the release gate with the m55/m45 GUI scripts is running.
- 2026-11-10 — P6.T5: low detail draws from two batched layers (edges at z 15, nodes at z 20; one item can't sit at both depths). Nodes use NoCache in low detail and DeviceCoordinateCache otherwise. Children the batches can't draw, such as labels and the navigator's badges, paint themselves in stacking order, so the approved look keeps its labels. Overlapping nodes interleave by layer. PM call: the two-layer split and the overlap order are accepted. The user re-signs the three-zoom shots (`lod-*.png`) at UAT.
- 2026-11-10 — The fan-out cascade test exposed a cubic cost in connecting chains, seen in stack samples. Each connect clears the connected component's output data kinds, and every node's re-resolution walked everything downstream. Each node now keeps its downstream requirement per starting constraint until the next invalidation (28bb9091f). Node naming in the test is also batched (431b7be24).
- 2026-11-10 — Release gate green at 28bb9091f (run 42812526469), including the cascade and grid gtests and `m55_node_graph.py` (229/229); the earlier gate passed all five node-graph GUI scripts with the scroll pan and low-detail layers. The AppImage is staged for the GPU-host session (`BENCHES="m116shots m117 m118"`) for P6.T9 and P5. P6.T2, T3, T6 and T8 stay unchecked until their Xvfb or GPU-host bench numbers are in.
- 2026-11-10 — GPU-host bench (`results-20261110T142238Z`, RX 9070 XT, after build 28bb9091f, comp, n=300 and 3000). The base again wedged in `knob_change_fanout`; the after side's 47 ops are all ok and no op is superlinear (highest slope: load 1.11). Per-event max at 3000, 11-09 → 11-10:
  - Within budget: hover 64.6 → 15.1, zoom_step 49.7 → 15.6, knob_change_fanout 94 → 8.7.
  - At budget: pan 27.9 → 16.1, with stall 59 → 0.
  - Still over: drag_move_1 76 → 81, drag_move_50 64 → 45, undo 795 → 648, redo 117 → 102, delete_1000 137 → 124, paste_100 216 → 189, duplicate_100 244 → 215, paste_1000 2328 → 2073.
  - At 300: click_select 50 and drag_move_1 50.
  - fit_all is flagged REGRESSED on loop-pass time (about 0.05 → 16 ms; per-event unchanged).
  - P6.T3 (drag) and P6.T2 (batched undo/redo/delete/paste) are not yet effective. P6.T9's rerun is this run.
- 2026-11-11 — GPU-host gate bench (`results-20261111T082111Z`, RX 7900 XTX, 7800X3D; after build 42dc24668 against base 0553d5c3a; comp, n=300 and 3000). The whole session took 7 min. Step wall p50/p95 at 3000, with the loop pass recorded separately:
  - Interaction ops pass: hover 0.08/0.65, pan 0.27/0.41, zoom_step 0.17/0.23, rubber_band 0.09/0.14, drag_move_1 0.29/4.3.
  - Edit ops pass: knob_change_fanout 7.6, set_expression 5.2, connect 25, create_one 8, edit_label 1.5.
  - Still over: select_all 79/103 (budget 16), drag_move_50 44/49 (16), undo 328/389, redo 107, delete_1000 133, duplicate_100 214, paste_100 210, paste_1000 2362 (100 each).
  - Minor: fit_all p95 30.5, with its loop pass at 20 ms against 0.05 on the base. click_select p95 30.
  - Against the base, everything improved except fit_all and click_select. The base wedged on duplicate_100 at 3000 and on undo at 300.
  - P6.T9's rerun is this run.
  - P6.T1 profiling of the remaining misses is running on the dev host with the gate harness.
- 2026-11-11 — P6.T1 profile (dev-host N100, Xvfb gate comp-3000, eu-stack and gdb; evidence in `.worktrees/m117/build/m118-prof/`). Two costs dominate. First, Qt sibling-order work under `_nodeRoot`: `stackBefore` and `setParentItem` → `ensureSequentialSiblingIndex`, O(siblings) per call. That covers the drag restack, and undo's LinkArrow creation through `onKnobSlaved` → `onKnobsLinksChanged`. Second, full O(visible N) low-detail repaints per item change, with `refreshItemGrid` run inside paint (select_all, drag, fit_all's loop pass). Other findings:
  - Undo runs 558 Python scripts per 1000-node undo, from the invalid-expression recheck (`setExpressionInternal` parses and validates per dimension). The Python object restore itself is cheap.
  - The paste declare batching works (one script). paste_1000 has no single hotspot: `setNameItemHtml` runs three times per node, and connect does data-kind pen refreshes.
  - Delete and redo do per-node deactivate work: edges, links, data-kind invalidation and downstream walks.
  - Mapped to tasks: P6.T8 is the selection indicator (only act on change, one union repaint); P6.T2 gains the recheck and per-link signal coalescing; P6.T3 gains the label-width cache, now in T12. New tasks: T10 (edges out of `_nodeRoot`, z bands), T11 (cached low-detail render), T12 (paste construction).
  - paste_1000 at 100 ms per 1000 nodes needs about 24× less work on the GPU host. Even after T12 it likely needs lazy NodeGui construction or a separate bulk-paste budget; that is for the user to decide when the numbers are in.
