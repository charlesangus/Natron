# M55 - Node Graph Polish

Full title: Polish — node graph interaction

Independent, low-risk node-graph interaction fixes, shipped together:
- Every input is always shown. The "Auto hide masks inputs" preference is removed, and edge refresh is made self-healing.
- Always-on splice-into-pipe for unconnected nodes and chains, with an insertion marker.
- A render dialog with a frame-range prompt and a modal foreground render.

The design notes, the current behaviour re-checked against `a50bef552`, and the reasoning behind the design calls are in `PLAN/DESIGN/2026-10-10-m55-elaboration.md`. The `## Decisions` below override it. One build at the end (user rule): implementers do not compile.

## Phase 55.1: Input pipes always drawn

- [x] M55.P1.T1 — Remove the "Auto hide masks inputs" preference; always show every input
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`, `Engine/AppManager.h`, `Engine/AppManager.cpp`, `Gui/GuiAppInstance.h`, `Gui/GuiAppInstance.cpp`, `Gui/NodeGraph.h`, `Gui/NodeGraph13.cpp`, `Gui/NodeGraph25.cpp`, `Gui/NodeGraph35.cpp`, `Gui/NodeGraph45.cpp`, `Gui/ActionShortcuts.h`, `Gui/ActionShortcuts.cpp`, `Gui/GuiApplicationManager10.cpp`, `Gui/Edge.h`, `Gui/Edge.cpp`. Deliberately **not** touched: `Gui/Gui.h`, `Gui/Gui50.cpp`, `Gui/Gui.cpp`, `Gui/GuiPrivate.h` (M44-owned) and `Engine/AppInstance.h` (M76-owned) — see approach.
  - approach:
    - This is finding (f) above: the preference's tooltip says "mask input", but `Edge::_imp->optional = _imp->isMask || effect->isInputOptional(inputNb)` (`Edge.cpp:373`) means it hides any plugin-optional input, mask or not, plus every input of a Reader outright. Remove it rather than retarget it.
    - Delete the knob: `Settings::_hideOptionalInputsAutomatically` (`Settings.h:577`), its two accessor declarations (`Settings.h:311-312`), its creation/tooltip/`addKnob` (`Settings.cpp:1251-1256`), its default (`:1616`), the `else if (k == _hideOptionalInputsAutomatically.get() ...)` branch in `onKnobValueChanged` (`:2361-2362`), and the setter/getter (`:3362-3370`).
    - Delete the now-dead call chain down to, but not including, the files the M44/M76 ground rules protect: `AppManager::toggleAutoHideGraphInputs` (`AppManager.h:682`, `AppManager.cpp:3305-3317`) and `GuiAppInstance::toggleAutoHideGraphInputs` (`GuiAppInstance.h:166`, `GuiAppInstance.cpp:922-926`). Leave `AppInstance::toggleAutoHideGraphInputs` (`Engine/AppInstance.h:314`) exactly as it is — it is already an empty inline virtual, so losing its only override is harmless. Leave `Gui::toggleAutoHideGraphInputs` (`Gui.h:449`, `Gui50.cpp:852-855`) exactly as it is too; once `GuiAppInstance`'s override is gone, nothing calls it, and it stays as unreachable dead code rather than an edit to an M44 file.
    - `Gui::toggleAutoHideGraphInputs`'s one call, `_imp->_nodeGraphArea->toggleAutoHideInputs(false)`, still needs to compile, so trim `NodeGraph::toggleAutoHideInputs`'s body (`NodeGraph45.cpp:73-86`) down to just the two `refreshEdgesVisility()` loops, deleting the `if (setSettings) { ... }` block (`:76-79`) that touches the now-gone Settings methods. Leave the declaration (`NodeGraph.h:220`) alone; it is still called with that signature.
    - Remove the user-facing toggle entirely: the View menu's "Auto-Hide Optional Inputs" checkable action (`NodeGraph35.cpp:433-438`), its keybind dispatch (`NodeGraph25.cpp:380-381`), and its registration (`ActionShortcuts.h:440-441`, `ActionShortcuts.cpp:151`, `GuiApplicationManager10.cpp:935`).
    - Remove the now-unused read path: `Edge::areOptionalInputsAutoHidden` (`Edge.h:100`, `Edge.cpp:288-293`) and `NodeGraph::areOptionalInputsAutoHidden` (`NodeGraph.h:137`, `NodeGraph13.cpp:90-93`).
    - In `Edge::computeVisibility` (`Edge.cpp:313-358`), delete the whole `autoHide`/`isReader`/`isViewer` arm (`:339-357`, the `else` under `if (_imp->isOutputEdge)`) and replace it with a plain `return true;`. The function then only hides an input edge for `!isInputVisible()`, for `isRotoMask`, or for the per-node "Hide inputs" knob while deselected — all kept, per (e). `hovered` may become unused inside `computeVisibility` once this is done; if so, simplify the parameter there without changing `Edge::refreshState`'s own use of `hovered` (`:362-378`) for anything else.
    - Do not touch (e): `isInputVisible` and the per-node "Hide inputs" knob stay exactly as they are.
    - This closes the M69 - Minor Cleanup item "Unconnected inputs stay visible when the node is deselected (they currently disappear)" — same preference, same removed branch.
    - `Settings::restoreKnobsFromSettings` (`Settings.cpp:2063-2113`) only looks up, in the saved `QSettings`, the keys of knobs it is handed; it never iterates the saved file's own keys. So a settings file carrying the old `autoHideInputs` key from a pre-M55 build loads unchanged once the knob is gone — that key is simply never looked up again, with no warning and no error.
  - verify: this task has no build of its own (per the ground rules, only P1.T2 runs against a standing binary). At the gate build:
    - the Preferences panel's Nodegraph tab no longer offers "Auto hide masks inputs", and the graph's View menu no longer offers "Auto-Hide Optional Inputs" or its shortcut;
    - P1.T2's new case (below) passes: an unconnected optional/mask input, and every input of a Reader, stay visible while the node is deselected and unhovered;
    - loading a settings file saved by a pre-M55 build with `autoHideInputs=true` produces no warning and no change in behaviour.
  - size: S

- [x] M55.P1.T2 — Build a GUI test that fails when a connected input has no drawn pipe, or an unconnected one hides on deselection, from synthetic cases
  - files: `Tests/gui/m55_input_pipes.py` (new)
  - approach:
    - Model the script on `build/m24-shots/m24_border.py` (watchdog, `gt.check`/`report`, crop to the NodeGraph). Create every node with `CreateNodeArgsPropSettingsOpened=True`.
    - The probe: for each connected input of each node, look for a pipe in the owning NodeGraph's scene (`gt.widgets_of("NodeGraph")` → `.scene().items()`). `Edge` does not override `type()`, so PySide wraps it as a `QGraphicsLineItem` at `zValue() == 15`. Its `line()` is in root-item coordinates, which are the same coordinates as `Effect.getPosition()`/`getSize()`. A match is a visible line item whose ends lie within a few pixels of the destination's box and the source's box.
    - If the items don't come through as `QGraphicsLineItem`, fall back to sampling pixels along the expected segment in a crop of the graph. Report which probe was used.
    - Cases. Each is a separate `check`, and each also checks again after hovering away and after nudging a node, so a pipe that only "heals on touch" is recorded:
      - (a1) Group with one Input, connected from a Constant. Then add a second Input inside the group (`app.createNode("fr.inria.built-in.Input", -1, group)`). Check the group's first pipe.
      - (a2) JoinViews. Connect two views, then add a project view. Check the pipes.
      - (b1) A Group holding Constant→Blur→Output. Save, close, then load. Open the group graph and check Constant→Blur and Blur→Output.
      - (b2) Copy a connected chain and paste it into a group graph.
      - (c1) Delete a node that has both an input and an output connected, then undo. Check both pipes.
      - (c2) Same as (c1) inside a group.
      - (d) Move the source onto the destination so they overlap, then pull it back out. The pipe must be visible again with its arrow.
      - (e) A node with an unconnected optional/mask input, and a Reader, each deselected and unhovered: both must keep their input pipe/stub visible. This is the item moved in from M69 and the case P1.T1 fixes; it is expected to **fail** on the pre-fix binary and to pass at the gate build.
      - Controls that must report **hidden**: "Hide inputs" on, and `setInputVisible(false)`-style hiding through the Python API if it is exposed. These separate intended behaviour from the bug, and must stay hidden both before and after P1.T1.
    - Write a shot for every failing case.
  - verify:
    - Run `NATRON_GUI_TEST_OUT=build/m55-shots/pipes Tests/gui/run-gui-test.sh Tests/gui/m55_input_pipes.py build/fast/App/Natron` against the **existing** pre-fix binary. Don't rebuild; if the binary is older than `a50bef552`, note that in the report.
    - Record which cases fail in M55's `## Decisions`. At least (e) is expected to fail against the pre-fix binary; (b1) and (c1) are the strongest suspects among the rest from the code reading.
  - size: M

- [ ] M55.P1.T3 — Fix the causes P1.T2 exposes, and make edge refresh self-healing
  - files: `Gui/NodeGui.cpp`, `Gui/ProjectGui.cpp`, `Gui/GuiAppInstance.cpp`, `Gui/Edge.cpp`
  - approach:
    - Fix whatever P1.T2 shows, with these as the expected fixes:
      - (a) In `refreshEdges` and `connectEdge`, when the sizes differ, call `initializeInputs()` and continue, instead of returning.
      - (b) Never write a null source for an input that exists. In `connectEdge` and `initializeInputs`, if the input's NodeGui is missing, leave the edge alone. In `GuiAppInstance::createNodeGui` (`Gui/GuiAppInstance.cpp:518-520`), after `initializeInputs()`, call `doRefreshEdgesGUI()` on the new node's existing GUI outputs, as `showGui` does at `NodeGui.cpp:2235-2240`, so that a late GUI repairs the pipes downstream of it. Replace `assert(gui_i)` (`:1680`) with that same tolerant path.
      - (b′) After a project load, refresh edges and links for every NodeGui in every graph, including Group sub-graphs, not just `getVisibleNodes()` (`ProjectGui.cpp:423-427`). Walk the project's nodes recursively through `Node::getNodeGui()`.
      - (c) Call `refreshEdgesVisility()` at the end of `showGui` and after `initializeInputs`. Stop hard-hiding edges for `!isVisible()` in `initializeInputs`, and leave visibility to `computeVisibility`. Roto-brush inputs stay hidden. (By the time this task lands, P1.T1 will already have removed `computeVisibility`'s auto-hide branch; that removal and this refresh-triggering fix are independent and both still needed.)
      - (d) Only if P1.T2's (d) case fails: in `Edge::initLine`, when the boxes overlap, draw nothing visible rather than a centre-to-centre line under the nodes. Then check that separating the nodes restores the pipe.
    - Don't touch the `isInputVisible`/"Hide inputs"-knob behaviour in (e) above. The auto-hide preference in (f) is removed by P1.T1, not here.
  - verify: at the gate build, `m55_input_pipes.py` passes every case, including (e) and the controls. `m24_border.py` still reports 0 failures, which guards M24's ring and edge re-attach.
  - size: L

## Phase 55.2: Splice into pipe

- [x] M55.P2.T1 — Add a pure splice-plan helper that picks a selection's entry and exit, with unit tests
  - files: `Gui/NodeGraphSplice.h` (new), `Gui/NodeGraphSplice.cpp` (new), `Tests/NodeGraphSplice_Test.cpp` (new), `Tests/CMakeLists.txt` (**OVERLAP: Tests/CMakeLists.txt**; one line in `GuiTests_SOURCES`)
  - approach:
    - Write `SplicePlan planSplice(const std::vector<SpliceNodeInfo>&)`. Each `SpliceNodeInfo` holds an id, its input ids (`-1` for free), its preferred input, its output ids, `isBackdrop` and `hasNoInputs` (Reader, or zero inputs).
    - It returns `{valid, entryId, entryInput, exitId}` using the eligibility rule above.
    - It takes no Natron types, so it can be tested without a graph. A thin adapter, `planSpliceForSelection(const NodesGuiList&)`, builds the infos from `getGuiInputs`/`getGuiOutputs`/`getPreferredInputForConnection`.
  - verify: `ctest -R NodeGraphSplice` covers these cases:
    - one free node (valid);
    - one node with its preferred input connected (invalid);
    - a two-node chain (entry = top, exit = bottom);
    - two disconnected nodes (invalid);
    - a diamond with one entry and one exit (valid);
    - an exit with an output outside the selection (invalid);
    - a Reader (invalid);
    - a backdrop in the selection (invalid);
    - two free entries (invalid).
  - size: M

- [x] M55.P2.T2 — Make splice always on for single- and multi-node selections
  - files: `Gui/NodeGraph20.cpp`, `Gui/NodeGraph15.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph.h`
  - approach:
    - Restructure `checkForHints` into three modes:
      - **splice**, with no modifier: connected pipes only, planned by P2.T1, always on for an eligible selection;
      - **dangling**, with Ctrl: today's stub hints, single node only;
      - **merge**, with Ctrl+Shift: unchanged.
    - Remove the `size() != 1` early return (`NodeGraph20.cpp:369-371`) for splice mode.
    - Candidate pipes are input edges of visible nodes, other than those nodes' own edges, that intersect the `boundingRectWithEdges` of any selected node (padded by the current 10px tolerance). Skip any pipe whose source or destination is selected. Take the pipe closest to the selection's centre. Keep the `canConnectInput` checks: the pipe's destination must accept the exit, and the entry's preferred input must accept the pipe's source.
    - Store the plan (pipe, entry, entry input, exit) in `NodeGraphPrivate` next to `_highLightedEdge`. Point the ghost edges at source→entry and exit→destination.
    - On release (`NodeGraph15.cpp:237-261`), apply the splice unconditionally when a valid plan exists; there is no suppress modifier to check. Clear the highlight, the ghosts and the plan on cancel, on Escape and when the selection changes.
    - Keep `checkForHints` cheap. It runs on every mouse move, so build the plan once per press and only re-run the pipe search on each move.
  - verify: covered by P4.T1's script at the gate build:
    - a Blur dragged over a Read→Viewer pipe highlights it with no modifier, and dropping it gives Read→Blur→Viewer;
    - a selected Blur→Grade chain splices with the Blur as the entry and the Grade as the exit;
    - an already-connected node dragged across another pipe does not splice;
    - Ctrl still offers the dangling-stub hint, and Ctrl+Shift still offers the merge hint, both unchanged.
  - size: L

- [x] M55.P2.T3 — Draw an insertion marker at the splice point
  - files: `Gui/NodeGraph.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph20.cpp`, `Gui/NodeGraph15.cpp`
  - approach:
    - Create one `QGraphicsPathItem` marker under `_nodeRoot` beside the hint edges (`NodeGraph.cpp:152-158`), and delete it with them (`:204-209`).
    - The shape is a filled circle on the pipe with a short bar across the pipe's direction, in the highlight green, with a dark outline so it reads against M24's kind tints and on a light background.
    - Its z-value is above edges (15) and below node bodies.
    - Place it at the point on the pipe's visible segment (`edge->line()` mapped to scene coordinates) closest to the selection's centre. Update it on every move while a plan exists. Hide it with the hints.
    - Leave `Edge::paint` alone. The existing green highlight stays.
  - verify: P4.T1 takes shots of the marker over a vertical pipe and a diagonal pipe, with the bar visibly across the pipe. The marker is gone after the drop and after Escape.
  - size: M

- [x] M55.P2.T4 — Make a splice one undo step, moves included
  - files: `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`, `Gui/NodeGraph15.cpp`
  - approach:
    - Generalise `InsertNodeCommand` to `(graph, edge, exitNode, entryNode = exitNode, entryInput = preferred)`:
      - redo connects the pipe's destination to the exit, and the entry's input to the old source;
      - undo reverses both;
      - the single-node behaviour and the "already connected" check (`NodeGraphUndoRedo.cpp:582-602`) stay the same.
    - On release, push one parent `QUndoCommand` with the text "Insert nodes", holding `MoveMultipleNodesCommand` and then `InsertNodeCommand` as children. The move's first-redo skip (`.cpp:92-98`) already suits a child command.
    - The plain-move and merge-hint paths are unchanged.
  - verify: at the gate build, P4.T1 checks that after a chain splice one Ctrl+Z restores both the connections and both node positions, and that one Ctrl+Shift+Z (redo) re-applies them. A single-node splice also undoes in one step.
  - size: M

## Phase 55.3: Render dialog

- [x] M55.P3.T1 — Add `RenderDialog` (writers, frame-range preset, background checkbox) and a pure range resolver with tests
  - files: `Gui/RenderDialog.h` (new), `Gui/RenderDialog.cpp` (new), `Tests/RenderDialogRange_Test.cpp` (new), `Tests/CMakeLists.txt` (**OVERLAP: Tests/CMakeLists.txt**; one line in `GuiTests_SOURCES`)
  - approach:
    - Pure `RenderRange resolveRenderRange(preset, writerFirst, writerLast, writerStep, projectFirst, projectLast, custom)`. Writer bounds of `INT_MIN`/`INT_MAX` fall back to the project range, as `validateRenderOptions` does. It returns `{first, last, step, error}`, with an error when first > last or step < 1.
    - The dialog lists each writer with its label, output file and resolved Write range (`getFrameRange_public(hash, …, true)`). It offers the preset combo (*Write node range* by default, then *Project range*, then *Custom*) and first/last/step spin boxes, which are enabled for *Custom* and prefilled from the first writer.
    - It has a "Render in background (Progress panel)" checkbox, off by default.
    - OK is disabled while the range is invalid, and the error shows inline.
    - The last preset, custom values and checkbox are remembered for the session.
    - Its output is `std::list<AppInstance::RenderWork>` plus `bool background`. `useRenderStats` comes from `isRenderStatsActionChecked()`.
    - `RenderDialog.h` exposes `void showRenderDialog(Gui*, const std::list<NodePtr>& writers)`. Callers include only that header, so `Gui.h` doesn't change. `Gui/CMakeLists.txt` globs `*.cpp`, so it needs no edit.
  - verify: `ctest -R RenderDialogRange` covers each preset; a writer range of `INT_MIN`/`INT_MAX` falling back to the project range; *Custom* 5–10 step 2; first > last giving an error; and step 0 giving an error.
  - size: M

- [x] M55.P3.T2 — Route Render Selected, Render All and the Write panel's Render button through the dialog
  - files: `Gui/Gui40.cpp`, `Gui/KnobGuiButton.cpp`, `Gui/RenderDialog.cpp`
  - approach:
    - `Gui::renderSelectedNode` (`Gui40.cpp:827-894`) collects the selected writers, creating one for a single non-writer as it does now (don't change the `createWriter` call). It then calls `showRenderDialog` instead of `startWritersRendering`.
    - `Gui::renderAllWriters` (`:816-824`) collects every active, enabled writer in the project, recursively, and does the same.
    - In `KnobGuiButton::emitValueChanged` (`Gui/KnobGuiButton.cpp:177-191`), when `k->isRenderButton()`, the holder is a writer effect and `getGui()` is non-null, open the dialog for that node instead of calling `trigger()`. Python, `-w` and `app.render` still go through `Node.cpp:6146` unchanged.
    - If the dialog is cancelled, nothing renders. If background is ticked, call `startWritersRendering(false, works)`, which is exactly today's path.
  - verify: P4.T1 at the gate build:
    - each of the three entry points opens the dialog;
    - Cancel leaves no Progress panel task;
    - background with *Custom* 3–5 writes exactly frames 3, 4 and 5 and shows a panel task;
    - `app.render(writer, 1, 2)` from the Script Editor still renders without a dialog.
  - size: M

- [x] M55.P3.T3 — Add a modal foreground render with progress and cancel, driven by the Progress panel's tasks
  - files: `Gui/RenderDialog.cpp`, `Gui/ProgressTaskInfo.h`, `Gui/ProgressTaskInfo.cpp`, `Gui/ProgressPanel.h`, `Gui/ProgressPanel.cpp`
  - approach:
    - Give `ProgressTaskInfo` two new signals: `progressChanged(double)`, emitted from `onRenderEngineFrameComputed`, and `taskFinished(int retCode)`, emitted once from `onRenderEngineStopped` and from `cancelTask`.
    - Add a public `ProgressTaskInfoPtr ProgressPanel::getTaskForNode(const NodePtr&) const` that wraps the private `findTask` (`ProgressPanel.cpp:116`).
    - For a foreground render, call `startWritersRendering(false, works)`, then look up each writer's task. A writer with no task (it failed validation) counts as done.
    - Show a `QProgressDialog` with `Qt::ApplicationModal`, opened with `open()` rather than `exec()` to avoid a nested loop, and minimum duration 0.
      - Its label reads "Rendering <writer> (k of n)", and its value is the average progress.
      - Cancel calls `cancelTask(false, 0)` on every unfinished task. That aborts running engines and processes, and removes queued items through `removeRenderFromQueue`.
      - The dialog closes when every task has finished. On a failure it leaves the Progress panel visible.
    - This covers in-process, separate-process and queued renders. It never uses `doBlockingRender` and doesn't touch `AppInstance`.
  - verify: P4.T1 at the gate build:
    - a 10-frame Write in the foreground shows the dialog, the graph doesn't take clicks while it is up, the dialog closes by itself, and 10 files exist;
    - a 60-frame render with a heavy Blur, cancelled after the first frame, stops early (fewer than 60 files), the dialog closes and the panel shows Paused or Canceled;
    - the same 10-frame test passes with "Render in a separate process" on.
  - size: L

## Phase 55.4: Evidence and UAT

- [ ] M55.P4.T1 — Write the M55 screenshot and interaction script and run it against the milestone build
  - files: `Tests/gui/m55_node_graph.py` (new)
  - approach:
    - Model it on `build/m24-shots/m24_border.py` (watchdog, NodeGraph crop, `gt.check`), creating nodes with `CreateNodeArgsPropSettingsOpened=True`.
    - Drive drags with synthetic `QMouseEvent`s sent to the NodeGraph viewport: press, at least three moves, release, with modifiers set on the events. Map positions through the view's `mapFromScene`.
    - Sections and shots:
      - Splice: hover with the marker (vertical and diagonal pipes), after the drop, chain splice, one-step undo, a connected node not splicing, and the Ctrl dangling and Ctrl+Shift merge hints.
      - Render: the dialog from all three entry points, background *Custom* 3–5, foreground mid-render, foreground cancel, and foreground with a separate process.
    - Also run P1.T2's `m55_input_pipes.py` and `m24_border.py`.
  - verify: `NATRON_GUI_TEST_OUT=build/m55-shots/out GUI_TEST_TIMEOUT=300 Tests/gui/run-gui-test.sh Tests/gui/m55_node_graph.py build/fast/App/Natron` reports 0 failures, as do the other two scripts. Show the shots to the user, who approves them.
  - size: L

- [ ] M55.P4.T2 — Package the AppImage for the user checkpoint
  - files: `build/appimages/M55-<sha>.AppImage`, `build/appimages/M55-uat.md`
  - approach: package with `tools/ci/local/package.sh`, as M24 did (the `--no-appstream` wrapper note in M24's Decisions applies). The UAT script walks through:
    - splicing a Blur and a two-node chain into a pipe and watching the marker;
    - one Ctrl+Z to undo the splice;
    - a connected node not splicing;
    - Render from the Write panel, Render Selected and Render All, each with the dialog;
    - a foreground render and its cancel, and a background render in the Progress panel;
    - the user's own project, if one is given, checking that every pipe is drawn after load, after deleting and undoing, and inside groups, and that no input disappears just because its node is deselected.
  - verify: the user signs off on the AppImage.
  - size: M

**Verification gate:**
- One `fast` build at the end, with CI `format`, `lint-ci` and `build-and-test` green.
- ctest green, including the new `NodeGraphSplice` and `RenderDialogRange` cases.
- Under `Tests/gui/run-gui-test.sh` against the milestone build, `m55_input_pipes.py` passes every case (after failing on the pre-fix binary for the cases recorded in P1.T2), and `m55_node_graph.py` and `m24_border.py` report 0 failures.
- The user approves the screenshots: splice marker and drop, chain splice and undo, render dialog, and foreground progress and cancel.
- The user signs off on `build/appimages/M55-<sha>.AppImage` through the UAT script.

## Decisions

- 2026-10-10 — **M46 stays a separate milestone (user):** M55's items are unrelated to Labelmaker, whose real overlap is M56's node-text wrap.
- 2026-10-10 — **Splice is always on while dragging; a modifier suppresses it (user).** It also works for multi-node selections and draws an insertion marker.
- 2026-10-10 — **Foreground render is a modal progress dialog with cancel (user).** The UI is blocked but still repaints; "background" stays today's in-process render with the Progress panel.
- 2026-10-10 — **Elaborated** (12 tasks) from `PLAN/DESIGN/2026-10-10-m55-elaboration.md`.
- 2026-10-10 — **User answers:**
  - **Missing pipes:** the cause is the "hide unused mask inputs" preference targeting non-mask inputs. Remove it and always show every input, up to the existing maximum. This absorbs M69's "unconnected inputs stay visible when deselected". The edge-refresh defects the scout found (groups after load, nulled sources, late GUI creation, showGui visibility) are fixed too.
  - **Roto multi-point feather:** this isn't node-graph work, so it moves to M69 - Minor Cleanup.
  - **Splice:** only unconnected nodes and chains can splice, with **no suppress modifier**. Ctrl's dangling-stub hint and Ctrl+Shift's merge hint are unchanged.
- 2026-10-10 — **Lane:** M55 runs in the project root on `milestone/m55-node-graph-polish`, alongside M44 in `.worktrees/m44`. Tasks avoid M44's files except for additive `Tests/CMakeLists.txt` lines. They also avoid `Engine/AppInstance.*`, which M76 owns on the other host.

- 2026-10-10 — **P2.T2:**
  - Releasing Ctrl mid-drag now drops the Ctrl hint and switches to splice; the old code kept a stale Ctrl hint.
  - Chain splices take two undo steps until P2.T4.
  - Escape and focus-out don't cancel a node drag today. P2.T3 adds a `resetSplicePlan()` call in `NodeGraph25.cpp`'s `keyPressEvent` (Escape) and `NodeGraph45.cpp`'s `focusOutEvent`.
- 2026-10-10 — **P3.T3:** the foreground dialog is application-modal through `show()` rather than `open()`, which would force window-modal and leave floating panels usable.
  - Cancel uses `cancelTask(false, 1)`, the same as the panel's Pause.
  - A separate-process render that fails to start shows ProcessHandler's error dialog but never finishes its task, so the foreground dialog stays open until Cancel.
- 2026-10-10 — **P1.T2:** `Tests/gui/m55_input_pipes.py` written and run against the existing pre-fix `build/fast/App/Natron` (built at `a50bef552`, before P1.T1). Result: only case **(e)** (an unconnected optional/mask input, deselected and unhovered) fails — exactly the one the task flagged as the expected failure. Cases (a1), (a2), (b1), (b2), (c1), (c2), (d) and the "Hide inputs" control all **pass already** on the pre-fix binary: the size-mismatch/null-source/post-load-refresh/showGui-visibility defects the elaboration doc worried about ((a), (b), (b′), (c)) did not reproduce for the synthetic graphs this script builds. (b1) and (c1), the elaboration's "strongest suspects" besides (e), are included and pass.
  - Two environment findings, unrelated to M55's own code, drove the probe design: `NodeGraph.scene()` raises `RuntimeError: Internal C++ object (QGraphicsScene) already deleted` on every call in this binary (reproduces on a bare two-node graph), so the script samples viewport pixels instead of scene items, per the design doc's documented fallback. And in the pre-fix tree, nothing ever calls `NodeGui::refreshEdgesVisility()` with `hovered=true` — hover alone never reveals a hidden input; only selecting the node does (confirmed by reading every call site). Case (e) checks the deselected state directly rather than diffing a hover toggle.
  - A group's subgraph `NodeGraph` is unparented from any tab whenever nothing has real OS focus (true throughout a synthetic script), so group-related cases make it paintable with `resize()`/`move()`/`show()`/`raise_()` directly on the widget rather than the focus-dependent Shift+double-click gesture.
  - Not exercised: `setInputVisible(false)`-style hiding through the Python API — `Engine/PyNode.h`'s `Effect` class does not expose `isInputVisible`/`setInputVisible` to Python. The native Read has 0 inputs on this build, so the Reader half of case (e) has no input edge to check; a NOTE records this rather than a skipped assertion.
