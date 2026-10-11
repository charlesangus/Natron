# M55 - Node Graph Polish: elaboration

Consultant elaboration, 2026-10-10, written against `main` at `a50bef552` (M24 merged). It replaces the M55 section of `2026-10-10-m44-m55-m56-scoping.md`. Every file:line below was re-checked against `a50bef552`, and several of the scout's references had moved after M24. The stub's `## Decisions` apply: M46 stays separate; splice is always on while dragging for an eligible (unconnected) selection, it works for multi-node selections and it draws an insertion marker; a foreground render is a modal progress dialog with cancel, and "background" keeps today's in-process render with the Progress panel. Two further decisions were settled by the user's answers during this elaboration: the "Auto hide masks inputs" preference (`autoHideInputs`) is removed outright, not fixed, since it is the confirmed cause of inputs that disappear or never draw; and multi-point feather dragging is out of scope for M55, moved to M69 - Minor Cleanup.

## Ground rules for this milestone

- **One build, at the end.** Implementers don't compile. Each `verify:` line describes what the gate build checks: the targeted ctest cases, the GUI scripts and CI. The one exception is P1.T2, which runs a script against the **existing** `build/fast/App/Natron` binary (built at `a50bef552`, before the fix). That is not a build.
- **Parallel lane (M44).** No task edits an M44 file except `Tests/CMakeLists.txt`, where two tasks each add one line to a source list (flagged `OVERLAP`). The render work stays out of `Gui/Gui.cpp`, `Gui/Gui.h`, `Gui/Gui50.cpp` and `Gui/GuiPrivate.h`, because the dialog is reached through a free function in the new `Gui/RenderDialog.h`. `Gui/KnobGuiButton.cpp` is not an M44 file, since M44 owns `KnobGui.*` and not `KnobGuiButton.*`. It includes `Gui/KnobUndoCommand.h` but does not change it. The same four files stay untouched by M55.P1.T1's preference removal too: `Gui::toggleAutoHideGraphInputs` (`Gui/Gui50.cpp:852-855`) is left in place as unreachable dead code rather than deleted; see that task.
- **Other host (M76 - Native Write, in progress).** M55 does not touch `Engine/AppInstance.*`, `Engine/Node.cpp`, `Engine/WriteNode.cpp` or `Engine/Nodes/IO/*`. M76 lists `Gui/Gui40.cpp:874` (Render Selected's `createWriter`) as a call site it does not change. Leave that call as it is. M55.P1.T1 likewise leaves `AppInstance::toggleAutoHideGraphInputs` (`Engine/AppInstance.h:314`, already an empty inline no-op) untouched for the same reason.
- **Test placement.** `GuiTests` (`Tests/CMakeLists.txt:146-155`) has a `QApplication` but no `Gui` and no `NodeGraph`, so node-graph behaviour is tested by Python scripts under Xvfb (`Tests/gui/run-gui-test.sh`). Pure logic is split into small helpers that gtest can reach. The scripts create nodes with `CreateNodeArgsPropSettingsOpened=True`, to match what the toolbar and Tab menu do (see `build/m24-shots/m24_border.py:39-41,90`).

## What the code does today (re-checked)

**Input pipes.** The candidate causes the scout listed, with current line numbers and four new findings:
- (a) `NodeGui::refreshEdges` (`Gui/NodeGui.cpp:1405`, size check at `:1413`) and `NodeGui::connectEdge` (`:2082`, check at `:2090`) return silently when `_inputEdges.size() != getGuiInputs().size()`. The edges are only rebuilt when the `inputsInitialized` signal reaches `NodeGui::initializeInputs` (`:1648`). That signal is emitted by `Node::initializeInputs` (`Engine/NodeInputs.cpp:416-477`), which runs when a Group gains a GroupInput (`Engine/NodeGroup.cpp:1388`) or when JoinViews sees a change in the project's views (`Engine/JoinViewsNode.cpp:151`).
- (b) `refreshEdges` skips (`continue`) any input whose NodeGui doesn't exist yet (`:1424-1426`). `connectEdge` goes further and **sets the edge's source to null** in that case (`:2094-2101`), so the pipe becomes a dangling stub. `initializeInputs` has `assert(gui_i)` at `:1680`, which means a release build quietly sets a null source. Nothing retries any of these.
- (b′) **New:** after a project loads, `ProjectGui` refreshes edges only for `getVisibleNodes()` (`Gui/ProjectGui.cpp:423-427`). That resolves to `Gui::getVisibleNodes` → `_nodeGraphArea->getAllActiveNodes()` (`Gui/Gui40.cpp:413-416`), which covers the root graph only. Nodes inside Group sub-graphs never get this pass.
- (c) `initializeInputs` creates the edges hidden and inactive when the NodeGui is `!isVisible()` (`:1672-1675`). **New:** `NodeGui::showGui` (`:2213-2240`) puts the edges back in the scene and calls `refreshEdges`, but never `refreshEdgesVisility`. A hidden edge therefore stays hidden until something such as a hover recomputes visibility. That fits "it shows up after I touch the node".
- (d) When the two nodes overlap, `Edge::initLine` (`Gui/Edge.cpp:438`) finds no intersection with the destination box. The line then runs between the two centres, under the nodes.
- (e) Intended behaviour that stays as-is: `isInputVisible` and the per-node "Hide inputs" knob, both inside `Edge::computeVisibility` (`Gui/Edge.cpp:313-358`, the `isRotoMask || (hideInputsKnobValue && !isSelected)` branch at `:334-335`). The per-node knob (`Node::getHideInputsKnobValue`/`setHideInputsKnobValue`, `Engine/Node.h:1640-1641`, `Engine/Node.cpp:6026-6038`) is an explicit, per-node toggle that a user switches on deliberately, through the graph's View menu (`Gui/NodeGraph35.cpp:440-448`) or `NodeGraph::toggleHideInputs` (`NodeGraph45.cpp:88-103`). It is a different feature from (f) below, and is kept.
- (f) **New (user-confirmed root cause of the missing/vanishing pipes):** the "Auto hide masks inputs" preference (`Settings::_hideOptionalInputsAutomatically`, `Engine/Settings.cpp:1251-1256`, knob name `autoHideInputs`, on by default since `:1616`) is read by `Edge::areOptionalInputsAutoHidden` (`Gui/Edge.cpp:288-293`) and applied in the `autoHide` branch of `computeVisibility` (`:339-357`). Despite its tooltip ("any disconnected **mask** input"), it hides any input the plugin merely marks optional — `Edge::_imp->optional = _imp->isMask || effect->isInputOptional(inputNb)` at `:373`, so non-mask optional inputs are included too — and every input of an `isReader()` node outright, whenever that node isn't hovered or selected. This is also the cause of the item moved here from M69: an unconnected input disappears the moment its node is deselected. Phase 55.1 removes the preference outright rather than retargeting it; see M55.P1.T1.

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
  - Shift held **during** a drag currently does nothing on its own, and this milestone gives it no new meaning there (see Design calls).

**Render.**
- Render Selected (`Gui/Gui40.cpp:827-894`), Render All (`:816-824`) and the Write node's Render button (`Engine/Node.cpp:6146-6160`; the button is created at `:3233` with `setAsRenderButton()`) all send `INT_MIN`/`INT_MAX`.
- `AppInstancePrivate::validateRenderOptions` (`Engine/AppInstance.cpp:1854`) resolves that to the writer's `getFrameRange_public`, falling back to the project range.
- `doBlockingRender` runs `blockingMap` on the calling thread (`:1793-1801`), which would freeze the GUI.
- The Progress panel already watches every render. For an in-process render it connects to `RenderEngine::frameRendered`/`renderFinished` and wires `taskCanceled` to `abortRendering_non_blocking` (`Gui/ProgressPanel.cpp:492-496`). For a separate-process render it connects to the `ProcessHandler` signals instead (`:382-388`). It looks tasks up per node through the private `findTask(node)` (`:116`).

## Design calls made here

- **Auto-hide-optional-inputs preference: removed, not fixed.** The preference's own logic was the bug (see finding (f) above), so M55.P1.T1 deletes the Settings knob (and with it, its auto-generated Preferences-panel entry), the View-menu "Auto-Hide Optional Inputs" action and its keybind, and the `Edge::computeVisibility` branch that read it. After the fix, an input edge is shown whenever `isInputVisible()` allows it internally and the per-node "Hide inputs" knob (kept, see (e)) doesn't hide it — connected or not, mask or not, Reader or not, selected/hovered or not. `Gui/Gui.h`, `Gui/Gui50.cpp`, `Gui/Gui.cpp`, `Gui/GuiPrivate.h` and `Engine/AppInstance.h` are not touched, because M44 and M76 own them respectively; the now-unused `toggleAutoHideGraphInputs` methods those files already contain are left in place as harmless dead code, and `NodeGraph::toggleAutoHideInputs` is reduced to a refresh-only no-op so `Gui/Gui50.cpp`'s one remaining (now unreachable) call site still compiles unchanged.
- **Always-on applies to connected pipes only.** Only pipes with both a source and a destination are candidates without Ctrl. A dangling stub only matches with Ctrl, as it does today. This keeps ordinary rearranging from snapping onto every loose arrow.
- **Eligibility.** A selection splices only when all of these hold:
  - It has exactly one *entry*: a selected node whose preferred input is free.
  - It has exactly one *exit*: a selected node with no outputs inside the selection, and no selected node has outputs outside the selection.
  - The entry reaches the exit through connections inside the selection.
  - It contains no backdrop.
  - No selected node is a Reader or has zero inputs. Dropping a generator onto a pipe today silently drops the upstream connection (`InsertNodeCommand::redo` with `prefInput == -1`, `.cpp:603-611`).

  Pipes that start or end on a selected node are excluded. The existing `canConnectInput` checks are kept for both ends. Splice is simply always on for any drag of an eligible (unconnected) selection; there is no suppress modifier. Ctrl keeps today's extra hints onto dangling input and output stubs, and Ctrl+Shift keeps the merge hint, both unchanged.
- **Render frame range.**
  - The dialog offers a range preset: *Write node range* (the default, resolved per writer the way `validateRenderOptions` does), *Project range*, or *Custom* (first, last, step).
  - The step defaults to the writer's `frameIncr` (`Node::getFrameStepKnobValue`).
  - With several writers, *Write node range* resolves separately for each one.
  - The dialog never writes the choice back into the Write node's knobs.
  - The last preset and the background checkbox are remembered for the session.
  - Python `app.render` and `-w` are unchanged.
- **Foreground progress follows the Progress panel's tasks, not the render engine.** That way one code path covers in-process renders, "Render in a separate process" and queued renders, and M55 never edits `AppInstance`. Cancelling calls the task's own cancel. For a pausable (image-sequence) task that leaves the task *Paused* in the panel, where it can be restarted, which matches the panel's own button.

## Phase 55.1: Input pipes always drawn

- [ ] M55.P1.T1 — Remove the "Auto hide masks inputs" preference; always show every input
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

- [ ] M55.P1.T2 — Build a GUI test that fails when a connected input has no drawn pipe, or an unconnected one hides on deselection, from synthetic cases
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

- [ ] M55.P2.T2 — Make splice always on for single- and multi-node selections
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

- [ ] M55.P2.T3 — Draw an insertion marker at the splice point
  - files: `Gui/NodeGraph.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph20.cpp`, `Gui/NodeGraph15.cpp`
  - approach:
    - Create one `QGraphicsPathItem` marker under `_nodeRoot` beside the hint edges (`NodeGraph.cpp:152-158`), and delete it with them (`:204-209`).
    - The shape is a filled circle on the pipe with a short bar across the pipe's direction, in the highlight green, with a dark outline so it reads against M24's kind tints and on a light background.
    - Its z-value is above edges (15) and below node bodies.
    - Place it at the point on the pipe's visible segment (`edge->line()` mapped to scene coordinates) closest to the selection's centre. Update it on every move while a plan exists. Hide it with the hints.
    - Leave `Edge::paint` alone. The existing green highlight stays.
  - verify: P4.T1 takes shots of the marker over a vertical pipe and a diagonal pipe, with the bar visibly across the pipe. The marker is gone after the drop and after Escape.
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
  - verify: at the gate build, P4.T1 checks that after a chain splice one Ctrl+Z restores both the connections and both node positions, and that one Ctrl+Shift+Z (redo) re-applies them. A single-node splice also undoes in one step.
  - size: M

## Phase 55.3: Render dialog

- [ ] M55.P3.T1 — Add `RenderDialog` (writers, frame-range preset, background checkbox) and a pure range resolver with tests
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

- [ ] M55.P3.T2 — Route Render Selected, Render All and the Write panel's Render button through the dialog
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

- [ ] M55.P3.T3 — Add a modal foreground render with progress and cancel, driven by the Progress panel's tasks
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

## Open questions

None.
