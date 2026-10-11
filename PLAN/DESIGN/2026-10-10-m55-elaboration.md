# M55 - Node Graph Polish: elaboration

Consultant elaboration, 2026-10-10, written against `main` at `a50bef552` (M24 merged). It replaces the M55 section of `2026-10-10-m44-m55-m56-scoping.md`. Every file:line below was re-checked against `a50bef552`, and several of the scout's references had moved after M24. The stub's `## Decisions` apply: M46 stays separate; splice is always on while dragging, a modifier suppresses it, it works for multi-node selections and it draws an insertion marker; a foreground render is a modal progress dialog with cancel, and "background" keeps today's in-process render with the Progress panel.

## Ground rules for this milestone

- **One build, at the end.** Implementers don't compile. Each `verify:` line describes what the gate build checks: the targeted ctest cases, the GUI scripts and CI. The one exception is P1.T1, which runs a script against the **existing** `build/fast/App/Natron` binary (built at `a50bef552`, before the fix). That is not a build.
- **Parallel lane (M44).** No task edits an M44 file except `Tests/CMakeLists.txt`, where three tasks each add one line to a source list (flagged `OVERLAP`). The render work stays out of `Gui/Gui.cpp`, `Gui/Gui.h`, `Gui/Gui50.cpp` and `Gui/GuiPrivate.h`, because the dialog is reached through a free function in the new `Gui/RenderDialog.h`. `Gui/KnobGuiButton.cpp` is not an M44 file, since M44 owns `KnobGui.*` and not `KnobGuiButton.*`. It includes `Gui/KnobUndoCommand.h` but does not change it.
- **Other host (M76 - Native Write, in progress).** M55 does not touch `Engine/AppInstance.*`, `Engine/Node.cpp`, `Engine/WriteNode.cpp` or `Engine/Nodes/IO/*`. M76 lists `Gui/Gui40.cpp:874` (Render Selected's `createWriter`) as a call site it does not change. Leave that call as it is.
- **Test placement.** `GuiTests` (`Tests/CMakeLists.txt:146-155`) has a `QApplication` but no `Gui` and no `NodeGraph`, so node-graph behaviour is tested by Python scripts under Xvfb (`Tests/gui/run-gui-test.sh`). Pure logic is split into small helpers that gtest can reach. The scripts create nodes with `CreateNodeArgsPropSettingsOpened=True`, to match what the toolbar and Tab menu do (see `build/m24-shots/m24_border.py:39-41,90`).

## What the code does today (re-checked)

**Input pipes.** The candidate causes the scout listed, with current line numbers and three new findings:
- (a) `NodeGui::refreshEdges` (`Gui/NodeGui.cpp:1405`, size check at `:1413`) and `NodeGui::connectEdge` (`:2082`, check at `:2090`) return silently when `_inputEdges.size() != getGuiInputs().size()`. The edges are only rebuilt when the `inputsInitialized` signal reaches `NodeGui::initializeInputs` (`:1648`). That signal is emitted by `Node::initializeInputs` (`Engine/NodeInputs.cpp:416-477`), which runs when a Group gains a GroupInput (`Engine/NodeGroup.cpp:1388`) or when JoinViews sees a change in the project's views (`Engine/JoinViewsNode.cpp:151`).
- (b) `refreshEdges` skips (`continue`) any input whose NodeGui doesn't exist yet (`:1424-1426`). `connectEdge` goes further and **sets the edge's source to null** in that case (`:2094-2101`), so the pipe becomes a dangling stub. `initializeInputs` has `assert(gui_i)` at `:1680`, which means a release build quietly sets a null source. Nothing retries any of these.
- (b′) **New:** after a project loads, `ProjectGui` refreshes edges only for `getVisibleNodes()` (`Gui/ProjectGui.cpp:423-427`). That resolves to `Gui::getVisibleNodes` → `_nodeGraphArea->getAllActiveNodes()` (`Gui/Gui40.cpp:413-416`), which covers the root graph only. Nodes inside Group sub-graphs never get this pass.
- (c) `initializeInputs` creates the edges hidden and inactive when the NodeGui is `!isVisible()` (`:1672-1675`). **New:** `NodeGui::showGui` (`:2213-2240`) puts the edges back in the scene and calls `refreshEdges`, but never `refreshEdgesVisility`. A hidden edge therefore stays hidden until something such as a hover recomputes visibility. That fits "it shows up after I touch the node".
- (d) When the two nodes overlap, `Edge::initLine` (`Gui/Edge.cpp:438`) finds no intersection with the destination box. The line then runs between the two centres, under the nodes.
- (e) Intended behaviour that looks like the bug: `isInputVisible` and the "Hide inputs" knob (`Edge::computeVisibility`, `Gui/Edge.cpp:313-358`). Also, the auto-hide-optional-inputs setting hides **every** input of an `isReader()` node, connected or not, when that node isn't hovered or selected (`:351-353`).

**Splice.** Most of it exists, behind Ctrl:
- `NodeGraph::checkForHints` (`Gui/NodeGraph20.cpp:56`) is turned on by `doConnectionHints = controlDown` (`:63`). Ctrl+Shift gives merge hints instead. It turns off when the dragged node is fully connected (`:74-80`) and ignores backdrops (`:66-70`).
- `moveSelectedNodesBy` gives up unless exactly one node is selected (`:369-371`).
- Pipe detection is `NodeGui::hasEdgeNearbyRect` (`Gui/NodeGui.cpp:2152`). It tests every input edge against the dragged node's `boundingRectWithEdges`, so dangling input and output stubs count too.
- The highlight is `Edge::setUseHighlight` (`Gui/Edge.cpp:397`), painted plain green over the M24 kind tint (`:865`). The ghost edges are `_hintInputEdge`/`_hintOutputEdge`, created at `Gui/NodeGraph.cpp:152-158`.
- On release, `MoveMultipleNodesCommand` and then `InsertNodeCommand` are pushed as **two** undo steps (`Gui/NodeGraph15.cpp:232-261`). `InsertNodeCommand` is at `Gui/NodeGraphUndoRedo.cpp:532-628`.
- Modifiers already in use in the graph:
  - Alt+press pans (`NodeGraph10.cpp:216`).
  - Ctrl+drag on a backdrop moves the backdrop without its contents (`NodeGraph20.cpp:313`, `NodeGraph15.cpp:214`).
  - Ctrl+click without moving opens the panel (`NodeGraph15.cpp:143,197`).
  - Holding Ctrl shows bend points (`NodeGraph25.cpp:286`).
  - Shift at press adds to the selection.
  - Shift held **during** a drag currently does nothing on its own.

**Roto feather.**
- Grabbing a feather bar calls `clearCPSSelection()` first (`Engine/RotoPaint.cpp:2317` and `:2353`), so a multi-point selection is lost.
- The move handler pushes one `MoveFeatherBarUndoCommand` for the single grabbed pair (`:2852-2853`).
- The command (`Engine/RotoUndoCommand.h:320-348`, `.cpp:969-1127`) projects the drag onto that point's feather direction. When the feather point sits on the control point, it uses the derivative normal instead (`.cpp:1012-1108`).
- The overlay highlights only the grabbed bar (`RotoPaint.cpp:1888,1931`).
- Selected pairs may be stored as (cp, fp) or (fp, cp) (`RotoPaintInteract.cpp:892-922`).

**Render.**
- Render Selected (`Gui/Gui40.cpp:827-894`), Render All (`:816-824`) and the Write node's Render button (`Engine/Node.cpp:6146-6160`; the button is created at `:3233` with `setAsRenderButton()`) all send `INT_MIN`/`INT_MAX`.
- `AppInstancePrivate::validateRenderOptions` (`Engine/AppInstance.cpp:1854`) resolves that to the writer's `getFrameRange_public`, falling back to the project range.
- `doBlockingRender` runs `blockingMap` on the calling thread (`:1793-1801`), which would freeze the GUI.
- The Progress panel already watches every render. For an in-process render it connects to `RenderEngine::frameRendered`/`renderFinished` and wires `taskCanceled` to `abortRendering_non_blocking` (`Gui/ProgressPanel.cpp:492-496`). For a separate-process render it connects to the `ProcessHandler` signals instead (`:382-388`). It looks tasks up per node through the private `findTask(node)` (`:116`).

## Design calls made here

- **Suppress modifier: Shift.**
  - With no modifier, a drag splices into connected pipes.
  - Shift held at the last move and at release means no splice and no hint.
  - Ctrl+Shift keeps the merge hint.
  - Ctrl keeps today's extra hints onto dangling input and output stubs.

  Why Shift:
  - Alt is already pan-on-press, and Linux window managers grab Alt+drag.
  - Using Ctrl to suppress would invert what Ctrl means in Natron today (it *enables* hints), and Ctrl already has three other meanings in the graph.
  - Shift held during a drag has no meaning on its own today.
  - The one interaction is harmless: someone who Shift-clicks to build a selection and keeps Shift held through the drag gets no splice, which is a conservative outcome.
- **Always-on applies to connected pipes only.** Only pipes with both a source and a destination are candidates without Ctrl. A dangling stub only matches with Ctrl, as it does today. This keeps ordinary rearranging from snapping onto every loose arrow.
- **Eligibility (see Q3).** A selection splices only when all of these hold:
  - It has exactly one *entry*: a selected node whose preferred input is free.
  - It has exactly one *exit*: a selected node with no outputs inside the selection, and no selected node has outputs outside the selection.
  - The entry reaches the exit through connections inside the selection.
  - It contains no backdrop.
  - No selected node is a Reader or has zero inputs. Dropping a generator onto a pipe today silently drops the upstream connection (`InsertNodeCommand::redo` with `prefInput == -1`, `.cpp:603-611`).

  Pipes that start or end on a selected node are excluded. The existing `canConnectInput` checks are kept for both ends.
- **Render frame range.**
  - The dialog offers a range preset: *Write node range* (the default, resolved per writer the way `validateRenderOptions` does), *Project range*, or *Custom* (first, last, step).
  - The step defaults to the writer's `frameIncr` (`Node::getFrameStepKnobValue`).
  - With several writers, *Write node range* resolves separately for each one.
  - The dialog never writes the choice back into the Write node's knobs.
  - The last preset and the background checkbox are remembered for the session.
  - Python `app.render` and `-w` are unchanged.
- **Foreground progress follows the Progress panel's tasks, not the render engine.** That way one code path covers in-process renders, "Render in a separate process" and queued renders, and M55 never edits `AppInstance`. Cancelling calls the task's own cancel. For a pausable (image-sequence) task that leaves the task *Paused* in the panel, where it can be restarted, which matches the panel's own button.

## Phase 55.1: Input pipes always drawn

- [ ] M55.P1.T1 — Build a GUI test that fails when a connected input has no drawn pipe, from synthetic cases
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
      - Controls that must report **hidden**: "Hide inputs" on, and `setInputVisible(false)`-style hiding through the Python API if it is exposed. These separate intended behaviour from the bug.
    - Write a shot for every failing case.
  - verify:
    - Run `NATRON_GUI_TEST_OUT=build/m55-shots/pipes Tests/gui/run-gui-test.sh Tests/gui/m55_input_pipes.py build/fast/App/Natron` against the **existing** pre-fix binary. Don't rebuild; if the binary is older than `a50bef552`, note that in the report.
    - Record which cases fail in M55's `## Decisions`. At least one case is expected to fail (b1 and c1 are the strongest suspects from the code reading).
    - If none fail, P1.T2 still lands the defensive fixes, and the item is closed by the user's UAT or repro (Q1).
  - size: M

- [ ] M55.P1.T2 — Fix the causes P1.T1 exposes, and make edge refresh self-healing
  - files: `Gui/NodeGui.cpp`, `Gui/ProjectGui.cpp`, `Gui/GuiAppInstance.cpp`, `Gui/Edge.cpp`
  - approach:
    - Fix whatever P1.T1 shows, with these as the expected fixes:
      - (a) In `refreshEdges` and `connectEdge`, when the sizes differ, call `initializeInputs()` and continue, instead of returning.
      - (b) Never write a null source for an input that exists. In `connectEdge` and `initializeInputs`, if the input's NodeGui is missing, leave the edge alone. In `GuiAppInstance::createNodeGui` (`Gui/GuiAppInstance.cpp:518-520`), after `initializeInputs()`, call `doRefreshEdgesGUI()` on the new node's existing GUI outputs, as `showGui` does at `NodeGui.cpp:2235-2240`, so that a late GUI repairs the pipes downstream of it. Replace `assert(gui_i)` (`:1680`) with that same tolerant path.
      - (b′) After a project load, refresh edges and links for every NodeGui in every graph, including Group sub-graphs, not just `getVisibleNodes()` (`ProjectGui.cpp:423-427`). Walk the project's nodes recursively through `Node::getNodeGui()`.
      - (c) Call `refreshEdgesVisility()` at the end of `showGui` and after `initializeInputs`. Stop hard-hiding edges for `!isVisible()` in `initializeInputs`, and leave visibility to `computeVisibility`. Roto-brush inputs stay hidden.
      - (d) Only if P1.T1's (d) case fails: in `Edge::initLine`, when the boxes overlap, draw nothing visible rather than a centre-to-centre line under the nodes. Then check that separating the nodes restores the pipe.
    - Don't touch (e). It is intended behaviour.
  - verify: at the gate build, `m55_input_pipes.py` passes every case, including the controls. `m24_border.py` still reports 0 failures, which guards M24's ring and edge re-attach.
  - size: L

## Phase 55.2: Splice into pipe

- [ ] M55.P2.T1 — Add a pure splice-plan helper that picks a selection's entry and exit, with unit tests
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

- [ ] M55.P2.T2 — Make splice always on, suppressed by Shift, for single- and multi-node selections
  - files: `Gui/NodeGraph20.cpp`, `Gui/NodeGraph15.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph.h`
  - approach:
    - Restructure `checkForHints` into three modes:
      - **splice**, with no modifier: connected pipes only, planned by P2.T1;
      - **dangling**, with Ctrl: today's stub hints, single node only;
      - **merge**, with Ctrl+Shift: unchanged.
    - Shift alone clears every hint.
    - Remove the `size() != 1` early return (`NodeGraph20.cpp:369-371`) for splice mode.
    - Candidate pipes are input edges of visible nodes, other than those nodes' own edges, that intersect the `boundingRectWithEdges` of any selected node (padded by the current 10px tolerance). Skip any pipe whose source or destination is selected. Take the pipe closest to the selection's centre. Keep the `canConnectInput` checks: the pipe's destination must accept the exit, and the entry's preferred input must accept the pipe's source.
    - Store the plan (pipe, entry, entry input, exit) in `NodeGraphPrivate` next to `_highLightedEdge`. Point the ghost edges at source→entry and exit→destination.
    - On release (`NodeGraph15.cpp:237-261`), skip the splice if Shift is held in the release event. Clear the highlight, the ghosts and the plan on cancel, on Escape and when the selection changes.
    - Keep `checkForHints` cheap. It runs on every mouse move, so build the plan once per press and only re-run the pipe search on each move.
  - verify: covered by P5.T1's script at the gate build:
    - a Blur dragged over a Read→Viewer pipe highlights it with no modifier, and dropping it gives Read→Blur→Viewer;
    - with Shift held there is no highlight, and the drop leaves the graph unchanged;
    - a selected Blur→Grade chain splices with the Blur as the entry and the Grade as the exit;
    - an already-connected node dragged across another pipe does not splice;
    - Ctrl still offers the dangling-stub hint, and Ctrl+Shift still offers the merge hint.
  - size: L

- [ ] M55.P2.T3 — Draw an insertion marker at the splice point
  - files: `Gui/NodeGraph.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph20.cpp`, `Gui/NodeGraph15.cpp`
  - approach:
    - Create one `QGraphicsPathItem` marker under `_nodeRoot` beside the hint edges (`NodeGraph.cpp:152-158`), and delete it with them (`:204-209`).
    - The shape is a filled circle on the pipe with a short bar across the pipe's direction, in the highlight green, with a dark outline so it reads against M24's kind tints and on a light background.
    - Its z-value is above edges (15) and below node bodies.
    - Place it at the point on the pipe's visible segment (`edge->line()` mapped to scene coordinates) closest to the selection's centre. Update it on every move while a plan exists. Hide it with the hints.
    - Leave `Edge::paint` alone. The existing green highlight stays.
  - verify: P5.T1 takes shots of the marker over a vertical pipe and a diagonal pipe, with the bar visibly across the pipe. The marker is gone after the drop, after Shift and after Escape.
  - size: M

- [ ] M55.P2.T4 — Make a splice one undo step, moves included
  - files: `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`, `Gui/NodeGraph15.cpp`
  - approach:
    - Generalise `InsertNodeCommand` to `(graph, edge, exitNode, entryNode = exitNode, entryInput = preferred)`:
      - redo connects the pipe's destination to the exit, and the entry's input to the old source;
      - undo reverses both;
      - the single-node behaviour and the "already connected" check (`NodeGraphUndoRedo.cpp:582-602`) stay the same.
    - On release, push one parent `QUndoCommand` with the text "Insert nodes", holding `MoveMultipleNodesCommand` and then `InsertNodeCommand` as children. The move's first-redo skip (`.cpp:92-98`) already suits a child command.
    - The plain-move and merge-hint paths are unchanged.
  - verify: at the gate build, P5.T1 checks that after a chain splice one Ctrl+Z restores both the connections and both node positions, and that one Ctrl+Shift+Z (redo) re-applies them. A single-node splice also undoes in one step.
  - size: M

## Phase 55.3: Roto multi-point feather

- [ ] M55.P3.T1 — Extract the feather-bar maths into a helper that moves every selected point by one distance change, with tests
  - files: `Engine/RotoFeatherBarDrag.h` (new), `Engine/RotoFeatherBarDrag.cpp` (new), `Tests/RotoFeatherBarDrag_Test.cpp` (new), `Tests/CMakeLists.txt` (**OVERLAP: Tests/CMakeLists.txt**; one line in `Tests_SOURCES`)
  - approach:
    - Move the direction maths out of `MoveFeatherBarUndoCommand::redo` (`Engine/RotoUndoCommand.cpp:1012-1095`), both the feather-vector branch and the derivative-normal branch, into `featherBarDirection(cp, fp, time) -> Point` (a unit vector, or zero).
    - Add `moveFeatherBars(points, dragged, dx, dy, time, autoKey)`:
      - normalise each pair to (cp, fp) with `isFeatherPoint()`;
      - compute `s = dot((dx, dy), featherBarDirection(dragged))`;
      - for each pair, move the feather point by `s * featherBarDirection(pair)` through `moveFeatherByIndex`, only when auto-key is on or the point is on a keyframe (as today).
    - This is Q2's rule (a). Don't clamp at zero: one point today can already cross through its curve, and the multi-point case should behave the same way.
  - verify: `ctest -R RotoFeatherBarDrag` covers these cases:
    - a Roto `makeSquare` with three of four points selected and mixed feather offsets: every selected point's |fp−cp| changes by the same `s`, and the unselected point is unchanged;
    - a point whose feather sits on the control point moves along the derivative normal;
    - a single-point drag gives the same position the old formula gave (a regression check);
    - with no keyframe at `time` and auto-key off, nothing moves.
  - size: M

- [ ] M55.P3.T2 — Keep the selection when a selected point's feather bar is grabbed, and drag all selected feathers in one undo command
  - files: `Engine/RotoPaint.cpp`, `Engine/RotoUndoCommand.h`, `Engine/RotoUndoCommand.cpp`
  - approach:
    - At `RotoPaint.cpp:2317` and `:2353`, check whether the grabbed bar's pair is already in `selectedCps`, matching either member. If it is, keep the selection and set `featherBarBeingDragged`; otherwise keep today's clear-and-select behaviour.
    - `MoveFeatherBarUndoCommand` takes the selected pairs plus the dragged pair. For each pair it stores (curve, live pair, cloned old pair). Redo calls P3.T1's `moveFeatherBars`. Undo restores every clone and bumps each curve's age once. Both redo and undo restore the whole selection through `setSelection(beziers, cps)`.
    - `mergeWith` requires the same dragged pair, the same set of pairs, the same time and the same ripple flag. Each drag is then one undo step.
    - While dragging, the overlay highlights every selected feather bar, not just the grabbed one (`:1888`, `:1931`).
  - verify: at the gate build, P5.T1 takes shots of a square with three points selected before and after a feather-bar drag, and all three feathers move. One Ctrl+Z restores all three. Grabbing an unselected point's bar still selects only that point.
  - size: M

## Phase 55.4: Render dialog

- [ ] M55.P4.T1 — Add `RenderDialog` (writers, frame-range preset, background checkbox) and a pure range resolver with tests
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

- [ ] M55.P4.T2 — Route Render Selected, Render All and the Write panel's Render button through the dialog
  - files: `Gui/Gui40.cpp`, `Gui/KnobGuiButton.cpp`, `Gui/RenderDialog.cpp`
  - approach:
    - `Gui::renderSelectedNode` (`Gui40.cpp:827-894`) collects the selected writers, creating one for a single non-writer as it does now (don't change the `createWriter` call). It then calls `showRenderDialog` instead of `startWritersRendering`.
    - `Gui::renderAllWriters` (`:816-824`) collects every active, enabled writer in the project, recursively, and does the same.
    - In `KnobGuiButton::emitValueChanged` (`Gui/KnobGuiButton.cpp:177-191`), when `k->isRenderButton()`, the holder is a writer effect and `getGui()` is non-null, open the dialog for that node instead of calling `trigger()`. Python, `-w` and `app.render` still go through `Node.cpp:6146` unchanged.
    - If the dialog is cancelled, nothing renders. If background is ticked, call `startWritersRendering(false, works)`, which is exactly today's path.
  - verify: P5.T1 at the gate build:
    - each of the three entry points opens the dialog;
    - Cancel leaves no Progress panel task;
    - background with *Custom* 3–5 writes exactly frames 3, 4 and 5 and shows a panel task;
    - `app.render(writer, 1, 2)` from the Script Editor still renders without a dialog.
  - size: M

- [ ] M55.P4.T3 — Add a modal foreground render with progress and cancel, driven by the Progress panel's tasks
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
  - verify: P5.T1 at the gate build:
    - a 10-frame Write in the foreground shows the dialog, the graph doesn't take clicks while it is up, the dialog closes by itself, and 10 files exist;
    - a 60-frame render with a heavy Blur, cancelled after the first frame, stops early (fewer than 60 files), the dialog closes and the panel shows Paused or Canceled;
    - the same 10-frame test passes with "Render in a separate process" on.
  - size: L

## Phase 55.5: Evidence and UAT

- [ ] M55.P5.T1 — Write the M55 screenshot and interaction script and run it against the milestone build
  - files: `Tests/gui/m55_node_graph.py` (new)
  - approach:
    - Model it on `build/m24-shots/m24_border.py` (watchdog, NodeGraph crop, `gt.check`), creating nodes with `CreateNodeArgsPropSettingsOpened=True`.
    - Drive drags with synthetic `QMouseEvent`s sent to the NodeGraph viewport: press, at least three moves, release, with modifiers set on the events. Map positions through the view's `mapFromScene`.
    - Viewer drags for roto go to the viewer's GL widget in the same way.
    - Sections and shots:
      - Splice: hover with the marker (vertical and diagonal pipes), after the drop, Shift-suppressed, chain splice, one-step undo, a connected node not splicing, and the Ctrl dangling and Ctrl+Shift merge hints.
      - Roto: three points before and after a feather drag, and after undo.
      - Render: the dialog from all three entry points, background *Custom* 3–5, foreground mid-render, foreground cancel, and foreground with a separate process.
    - Also run P1.T1's `m55_input_pipes.py` and `m24_border.py`.
  - verify: `NATRON_GUI_TEST_OUT=build/m55-shots/out GUI_TEST_TIMEOUT=300 Tests/gui/run-gui-test.sh Tests/gui/m55_node_graph.py build/fast/App/Natron` reports 0 failures, as do the other two scripts. Show the shots to the user, who approves them.
  - size: L

- [ ] M55.P5.T2 — Package the AppImage for the user checkpoint
  - files: `build/appimages/M55-<sha>.AppImage`, `build/appimages/M55-uat.md`
  - approach: package with `tools/ci/local/package.sh`, as M24 did (the `--no-appstream` wrapper note in M24's Decisions applies). The UAT script walks through:
    - splicing a Blur and a two-node chain into a pipe and watching the marker;
    - Shift to suppress, and one Ctrl+Z;
    - a connected node not splicing;
    - a multi-point roto feather drag and its undo;
    - Render from the Write panel, Render Selected and Render All, each with the dialog;
    - a foreground render and its cancel, and a background render in the Progress panel;
    - the user's own project from Q1, if one is given, checking that every pipe is drawn after load, after deleting and undoing, and inside groups.
  - verify: the user signs off on the AppImage.
  - size: M

**Verification gate:**
- One `fast` build at the end, with CI `format`, `lint-ci` and `build-and-test` green.
- ctest green, including the new `NodeGraphSplice`, `RotoFeatherBarDrag` and `RenderDialogRange` cases.
- Under `Tests/gui/run-gui-test.sh` against the milestone build, `m55_input_pipes.py` passes every case (after failing on the pre-fix binary for the cases recorded in P1.T1), and `m55_node_graph.py` and `m24_border.py` report 0 failures.
- The user approves the screenshots: splice marker and drop, chain splice and undo, roto multi-feather before and after, render dialog, and foreground progress and cancel.
- The user signs off on `build/appimages/M55-<sha>.AppImage` through the UAT script.

## Open questions

1. **Input pipe repro.** The code reading turned up several real gaps: edges inside groups are never refreshed after a load, `connectEdge` nulls the source, and `showGui` skips the visibility refresh. Do you have a project where a connected pipe is missing?
   - (a) Send an `.ntp` file plus a screenshot, and say whether the pipe comes back after hovering or moving a node, whether the node is a Group, Merge or JoinViews or sits inside a group, and whether "Hide inputs" is on. It becomes a P1.T1 case.
   - (b) **Recommended:** no repro needed up front. M55 ships the synthetic-case fixes, and the item closes if your UAT on your usual projects doesn't show it again. A later sighting reopens it with a repro.
2. **Multi-point feather rule.**
   - (a) **Recommended:** every selected point's feather distance changes by the same amount, each along its own normal. The amount is the drag projected onto the grabbed bar.
   - (b) The raw drag vector is projected onto each point's own normal, so points facing away from the drag move less or move inward.
   - (c) Every feather distance is scaled by the same factor, which keeps their ratios.

   Also, should a feather be stopped at zero distance instead of crossing through the curve? **Recommended:** no, to match single-point behaviour today.
3. **Which drags may splice, given that it's always on.**
   - (a) **Recommended:** only a selection with a free entry input and no outputs leaving it (an unconnected node or chain). Rearranging a wired graph never rewires anything.
   - (b) Also let an already-connected node splice. It would be pulled out of its current pipe and put into the new one, which Natron cannot do today and which needs a "yank out" command.
   - (c) As (a), but also let a connected node splice while Ctrl is held.
