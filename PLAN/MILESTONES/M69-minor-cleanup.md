# M69 - Minor Cleanup

A grab-bag of small UX fixes from the user (2026-10-08), shipped together. They are node-graph interaction, the pane menu, the viewer colour picker, roto multi-point feather, curve-editor tangent break/join, and a crash on node deletion. The knob and Keyer items moved to M95 - Knob Cleanup And Keyer Redesign; weighted tangents moved to M96 - Weighted Curve Tangents (see Decisions). "Unconnected inputs stay visible" was done by M55.P1.T1.

Scouted 2026-10-11 on `milestone/m55-node-graph-polish` at `10e0accd5`. Line numbers are from that tree; re-check them after M55 and M44 merge. One build at the end (user rule): implementers do not compile.

## Phase 69.1: Crash fix

- [ ] M69.P1.T1 — Stop the Edit menu holding dangling undo/redo actions after a node's undo stack dies
  - files: `Gui/Gui30.cpp`, `Gui/GuiPrivate.h`, `Gui/GuiPrivate.cpp`
  - approach:
    - Cause: `Gui::removeUndoStack` (`Gui30.cpp:660-680`) removes the stack's actions from `menuEdit` when they are current, but leaves `_imp->_currentUndoAction`/`_currentRedoAction` pointing at them. The actions are parented to the `QUndoStack` (`registerNewUndoStack`, `:649-657`), so they die with it (`NodeGui::removeUndoStack`, `Gui/NodeGui.cpp:936-940`, from `destroyGui` `:2984`). The next `GuiPrivate::setUndoRedoActions` (`GuiPrivate.cpp:564-580`) calls `menuEdit->removeAction` on a freed pointer.
    - Fix: in `removeUndoStack`, null the current pointers when they match, and call `_undoStacksGroup->removeStack(stack)` before the stack goes away. Also hold the two current pointers as `QPointer<QAction>` in `GuiPrivate.h:78-79`, so a stack deleted by any other path cannot leave a dangling pointer. `setUndoRedoActions` then skips null entries.
  - verify: covered by P7.T1's crash case at the gate build. Open a Grade's panel, click a slider so the panel's undo stack becomes active, delete the Grade from the graph, then open a Blur's panel and change a value, then use Edit > Undo. No crash, and the Edit menu shows the Blur's undo text.
  - size: M

## Phase 69.2: Node graph interaction

- [ ] M69.P2.T1 — Make input arrows easier to grab
  - files: `Gui/NodeGraph10.cpp`, `Gui/Edge.cpp`, `Gui/Edge.h`
  - approach:
    - Today: `NodeGraph::hasItemNearbyMouse` (`NodeGraph10.cpp:108-190`) queries `items()` over a fixed `TO_DPIX(10.)` viewport square against each item's shape. `Edge::shape()` (`Edge.cpp:629`) is the 2px line plus a 10px arrowhead (`ARROW_SIZE_DISCONNECTED`). The function then takes `*edges.begin()`, which is pointer order, not the nearest edge. Nodes win at the exact point.
    - Change: pick the edge nearest the cursor in scene space using `dist2ToSegment` (already in `Edge.cpp:653`; expose it as a static helper). Give unconnected input stubs (`!hasSource()`, not output edges) a larger grab radius in **viewport** pixels, so it doesn't shrink when zoomed out: about `TO_DPIX(18)` around the whole stub plus its free tail, against 10 for connected pipes.
    - Keep "a node under the exact cursor wins" and the bend-point test unchanged. Don't change how pipes are drawn.
  - verify: P7.T1 sends presses at 12px and 16px from an unconnected Merge input stub, at 100% and 50% zoom. Each starts an arrow drag (the stub follows the cursor). A press 30px away doesn't. With two pipes crossing near the cursor, the nearer one is taken.
  - size: M

- [ ] M69.P2.T2 — Add pick-up connection mode: click an input, it follows the cursor, click a node to connect
  - files: `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph10.cpp`, `Gui/NodeGraph15.cpp`, `Gui/NodeGraph20.cpp`, `Gui/NodeGraph25.cpp`
  - approach:
    - A press and release on an input arrow with no movement (`hasMovedOnce` false at the `eEventStateDraggingArrow` release, `NodeGraph15.cpp:144`) enters pick-up instead of doing nothing. Store `_pickedUpArrow` (an `Edge*`) plus the original source in `NodeGraphPrivate`, as a flag **separate from `_evtState`**, so the existing pan (middle drag) and zoom (wheel) states keep working while an input is held.
    - While held: every mouse move, and every pan or zoom step, calls `dragSource(mapFromScene(cursor))` (`NodeGraph20.cpp:762-768` is the existing drag path), so the arrow stays under the cursor.
    - Left-click on a node: connect with exactly the `ConnectCommand` and `handleConnectionError` path the drag release uses (`NodeGraph15.cpp:144-195`). Factor that block into one helper shared by drag and pick-up.
    - Right-click or Escape (`NodeGraph25.cpp` keyPress) cancels: restore the original source with `initLine`, push nothing, and suppress the context menu for that right-click.
    - Left-click on empty space also cancels (recommended default; see Q6). Picking up an already-connected input shows it detached while held; cancel restores it.
    - Pick-up mode applies to input arrows only, not output edges.
  - verify: P7.T1 runs: click a Blur's input, move, pan with the middle button, zoom with the wheel (the arrow tip stays at the cursor), click a Constant, and Blur's input 0 is the Constant, undone in one Ctrl+Z. A second run is cancelled by Escape and a third by right-click; in both, connections are unchanged, no undo entry is added, and no context menu opens.
  - size: L

- [ ] M69.P2.T3 — Add keyboard shortcuts that connect the selected nodes
  - files: `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp`, `Gui/NodeGraph25.cpp`, `Gui/NodeGraph40.cpp`, `Gui/NodeGraph.h` (**OVERLAP: ActionShortcuts.h / GuiApplicationManager10.cpp** with P6.T2)
  - approach:
    - These are the defaults pending Q5. **Y** connects: each other selected node, in selection order, feeds the next free input of the **last-selected** node, preferred input first (`Node::getPreferredInput`). **Shift+Y** connects in reverse: the last-selected node feeds the preferred input of every other selected node.
    - Register both in `kShortcutGroupNodegraph`. Y and Shift+Y are free in that group; the viewer's Y luminance binding is in a different group.
    - Push all connections as one parent `QUndoCommand` holding `ConnectCommand`s.
    - Skip connections that would make a cycle or that `canConnectInput` rejects, and report them in the status bar. Do nothing with fewer than two selected nodes.
  - verify: P7.T1 selects Constant, then Checkerboard, then Merge, and presses Y: Merge A and B are fed by the two sources, undone in one step. It selects Grade, Blur and Constant (last) and presses Shift+Y: both Grade and Blur take the Constant.
  - size: M

- [ ] M69.P2.T4 — Make pasting onto a selected node connect it
  - files: `Gui/NodeGraph40.cpp`, `Gui/NodeGraphPrivate10.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraphSplice.h`, `Gui/NodeGraphSplice.cpp`
  - approach:
    - In `NodeGraph::pasteNodeClipBoards(const QPointF&)` (`NodeGraph40.cpp:175-198`), capture the selection before `pasteNodesInternal` (`NodeGraphPrivate10.cpp:56-165`) replaces it.
    - If exactly one non-backdrop node was selected, use M55's `planSpliceForSelection` (`Gui/NodeGraphSplice.h`) on the pasted nodes to find the single entry with a free preferred input. Connect that input to the selected node.
    - Then place the pasted set as node creation does: `moveNodesForIdealPosition(entry, selected, true)` (`Gui/NodeGraph05.cpp:54`), which also re-routes the selected node's outputs through the exit. This is the "insert" default from Q4. Move the other pasted nodes by the same offset.
    - Make the `AddMultipleNodesCommand` pushed at `NodeGraphPrivate10.cpp:161` and the connection one undo step (a parent command).
    - Leave Ctrl+V with no selection, duplicate (Alt+C) and clone unchanged. If the pasted set has no valid entry, paste unconnected, as today.
    - **OVERLAP with M93**: the clipboard is read through a boost `xml_iarchive` in this function; rebase onto M93 if it has merged.
  - verify: P7.T1 copies a Blur, selects a Read that feeds a Viewer, and pastes: the result is Read→Blur→Viewer, and one Ctrl+Z removes the Blur and restores Read→Viewer. A pasted Blur→Grade chain enters at the Blur. Pasting with nothing selected leaves the Blur unconnected.
  - size: L

- [ ] ~~M69.P2.T5 — Open the Tab node menu at the cursor with no jump~~ (cancelled 2026-10-11: M45.P2.T1 takes this over)
  - files: `Gui/NodeCreationDialog.cpp`, `Gui/NodeCreationDialog.h`
  - approach:
    - Cause: the `NodeCreationDialog` constructor calls `textEdit->setText(initialFilterName)` (`:445-447`) after `textChanged` is connected to `filterText` (`:106`). So `filterText` shows the `Qt::ToolTip` list view at `mapToGlobal(QPoint(0, height()))` (`:186-198`) before the dialog has been shown or moved. That position is near the screen's top-left. `QTimer::singleShot(20, showCompleter)` (`:459`) then moves it.
    - Fix: `filterText` updates the model but only shows or moves the list while `window()->isVisible()`. Override `NodeCreationDialog::showEvent` to place the dialog at the cursor (the current centring maths, `:450-454`, but using the final `sizeHint()` after the line edit is in the layout), then call `showCompleter()` directly. Drop the 20ms timer.
    - This is not the same bug as the pane menu in P3.T1.
  - verify: P7.T1 presses Tab over the graph and, in the first event-loop turn after the dialog appears, records the global geometry of the dialog and its list view: both lie within 300px of the cursor, and the list's top-left never reads `(0,0)`. Shots before and after.
  - size: M

## Phase 69.3: Panels

- [ ] M69.P3.T1 — Right-click the empty header space for the pane menu, at the cursor
  - files: `Gui/TabWidget.h`, `Gui/TabWidget.cpp`
  - approach:
    - The pane menu is `TabWidget::createMenu()` (`TabWidget.cpp:383-470`): split vertical/horizontal, float, close, hide toolbar/tabs header, the "... here" entries, set as anchor. It is connected to `leftCornerButton`'s `clicked()` (`:314`) and opens with `menu.exec(leftCornerButton->mapToGlobal(QPoint(0, 0)))`. The empty space right of the tabs is the stretch in the header row, owned by `TabWidgetHeader` (`TabWidget.h:121-146`), a bare `QWidget` that only handles `leaveEvent`.
    - Give `TabWidgetHeader` a `contextMenuEvent` that emits `paneMenuRequested(QPoint globalPos)` only when `childAt(e->pos())` is null, so the stretch area is the only target. Right-clicks on `TabBar` and the float/close buttons keep their own behaviour.
    - Change `createMenu()` to `createMenu(const QPoint& globalPos)`. The `leftCornerButton` slot passes `leftCornerButton->mapToGlobal(QPoint(0,0))`, unchanged; the header passes the cursor position.
  - verify: P7.T1 right-clicks the empty header of the graph pane: a `QMenu` opens with the same action texts as the corner icon's menu, its top-left at the cursor. Right-clicking a tab doesn't open it. The corner icon still opens its menu at the icon.
  - size: M

## Phase 69.4: Viewer colour picker

- [ ] M69.P4.T1 — Always pick scene-referred values; remove "Linear color pickers"
  - files: `Gui/ViewerGL.cpp`, `Gui/ViewerGL.h`, `Engine/Settings.h`, `Engine/Settings.cpp`, `Gui/KnobGuiColor.cpp`
  - approach:
    - Finding: the picker does **not** read the display texture. `getColorAt`/`getColorAtRect` (`ViewerGL.cpp:4279-4517`) sample `lastRenderedTiles`, which holds `params->colorImage`. That is the viewed node's rendered image (`Engine/ViewerInstance.cpp:2941` → `endTransferBufferFromRAMToGPU` → `ViewerGL.cpp:1505`), before gain, gamma and the display transform. The display processor is applied only when `forceLinear` is false, which is the `linearPickers` setting (default **true**).
    - Remove the `forceLinear` parameter and the `applyViewerDisplayTransform` branches from both functions, and their callers at `:2626`, `:3734` and `:3910`.
    - Delete `_linearPickers` and `getColorPickerLinear()` (`Settings.h:118,508`; `Settings.cpp:659-664,1564,2524-2527`). An old settings file's `linearPickers` key is never looked up again, as M55 found for `autoHideInputs`.
    - Rewrite the colour knob's picker tooltip (`KnobGuiColor.cpp`, the "converts to linear" sentence, at `:843` on the M44 branch): picked values are the image's scene-referred values before any viewer transform.
  - verify: P7.T1 shows a Constant (0.18, 2.0, 0.5, 1) through viewer gain +2, gamma 2.2 and the sRGB display. Ctrl-click reads back exactly (0.18, 2.0, 0.5, 1) in the info bar and in a picking Grade's colour knob. The rect pick returns the same values. The Preferences UI page no longer lists "Linear color pickers".
  - size: M

- [ ] M69.P4.T2 — Add an Engine helper that samples a node's image over a pixel rect
  - files: `Engine/NodeColorSampler.h` (new), `Engine/NodeColorSampler.cpp` (new), `Tests/NodeColorSampler_Test.cpp` (new), `Tests/CMakeLists.txt` (**OVERLAP: Tests/CMakeLists.txt**)
  - approach:
    - `bool sampleNodeColor(const NodePtr& node, double time, ViewIdx view, unsigned int mipmapLevel, const RectI& pixelRect, const ImageLayerDesc& layer, float rgba[4])`. It averages the rect. It renders the RoI synchronously the way `TrackMarker::getMarkerImage` does (`Engine/TrackMarker.cpp:1162-1220`: `ParallelRenderArgsSetter` + `renderRoI`), on the same scale and layer the viewer used, so the normal case is a cache hit on the image the viewer render just pulled through.
    - It returns false for a null, disabled or deep node. Treat a missing layer channel as 0 and a missing alpha as 1.
  - verify: `ctest -R NodeColorSampler`: for Constant(0.25)→Grade(gain 2), sampling the Grade gives 0.5 and its input (the Constant) gives 0.25. A 4×4 rect over a CheckerBoard square boundary gives the mean. A disconnected node returns false.
  - size: M

- [ ] M69.P4.T3 — Add the viewer's Input/Output pick mode, defaulting to Input
  - files: `Gui/ViewerGL.cpp`, `Gui/ViewerGL.h`, `Gui/ViewerTab.cpp`, `Gui/ViewerTabPrivate.h`, `Gui/InfoViewerWidget.cpp`
  - approach:
    - Add a checkable "Pick: In / Out" button next to `pickerButton` (`ViewerTab.cpp:551`), per viewer, default **In** (Q7). Hold the mode in `ViewerGL`.
    - Mode Out keeps today's path. In mode In, the point pick (`pickColorInternal`, `:3720`, finally using its dead `pickInput` argument), the rect pick (`updateRectangleColorPickerInternal`, `:3906`) and the hover readout (`updateColorPicker`, `~:2600`) find texture *i*'s viewed node (`ViewerInstance::getActiveInputs` → `getInput`). They take that node's `getPreferredInputNode()` (Q8) and call P4.T2's sampler at the same time, view, mipmap and layer.
    - If the viewed node has no connected preferred input (a Read, a generator), fall back to Out and show "out" in the info bar.
    - The info bar shows "in"/"out" beside the RGBA. The picked value flows unchanged to `setColorPickersColor`, the parametric picker and the histogram cursor.
    - Delete the commented-out `kShortcutIDMousePickInputColor` registration (`GuiApplicationManager10.cpp:899`) and `#define` (`ActionShortcuts.h:334-335`); the toolbar mode replaces it.
  - verify: P7.T1 views Constant(0.25)→Grade(gain 2): In reads 0.25 and Out reads 0.5. On a Read it reads the Read's output, marked "out". Hovering in In mode across a 2K plate under a heavy Blur stays interactive (UAT). Shots of the button in both states.
  - size: L

## Phase 69.5: Roto multi-point feather

- [ ] M69.P5.T1 — Add a pure helper for the multi-point feather drag, with unit tests
  - files: `Engine/RotoFeatherDrag.h` (new), `Engine/RotoFeatherDrag.cpp` (new), `Tests/RotoFeatherDrag_Test.cpp` (new), `Tests/CMakeLists.txt` (**OVERLAP: Tests/CMakeLists.txt**)
  - approach:
    - Extract the per-point direction maths from `MoveFeatherBarUndoCommand::redo` (`Engine/RotoUndoCommand.cpp:1012-~1100`): the unit feather direction, or the normal from left/right derivatives when the feather sits on the control point.
    - Add `double featherDragDistance(dir, dx, dy)`, the projection of the drag onto the grabbed point's direction, and `Point applyFeatherDistance(featherPt, dir, s)`.
    - Rule (Q9, recommended): every selected point moves by the same signed distance `s` along its own direction.
  - verify: `ctest -R RotoFeatherDrag`: a drag along a direction gives that distance. Three points with different normals each move by `s` along their own normal. The feather-on-point case uses the derivative normal.
  - size: M

- [ ] M69.P5.T2 — Drag the feather of every selected point together, as one undo step
  - files: `Engine/RotoPaint.cpp`, `Engine/RotoUndoCommand.h`, `Engine/RotoUndoCommand.cpp`
  - approach:
    - In the press handler, when the grabbed feather bar's control point is already selected, skip `clearCPSSelection()` (`RotoPaint.cpp:2317` and `:2353`); otherwise select it alone, as today.
    - Change `MoveFeatherBarUndoCommand` (`RotoUndoCommand.h:320-348`) to hold a list of (control point, feather point) pairs with their old positions, built from `selectedCps`. It computes `s` once from the grabbed point and applies it to every pair through P5.T1, at `_time`, with ripple edit honoured per point. Keep `mergeWith` so a drag is one undo step.
    - Call it from the move handler (`:2852-2853`).
  - verify: `ctest -R RotoLayer` still passes. P7.T1: a Bezier with three of five points selected, one feather bar dragged outward by 20px, moves exactly those three feathers by the same distance along their normals, and one Ctrl+Z restores all three. Shots before and after.
  - size: L

## Phase 69.6: Curve editor tangents (break and join)

- [ ] M69.P6.T1 — Add a "join tangents" command and its slope helper, with unit tests
  - files: `Gui/CurveEditorUndoRedo.h`, `Gui/CurveEditorUndoRedo.cpp`, `Tests/CurveTangentJoin_Test.cpp` (new), `Tests/CMakeLists.txt` (**OVERLAP: Tests/CMakeLists.txt**)
  - approach:
    - Pure `double joinedDerivative(double left, double right)`: the slope of the mean of the two tangent angles, `tan((atan(l)+atan(r))/2)` (Q10).
    - Add `JoinTangentsCommand(widget, keys)`: for each `eKeyframeTypeBroken` key, set both derivatives to the joined value and set the interpolation to `eKeyframeTypeFree`, through the same knob path `MoveTangentCommand::setNewDerivatives` uses (`moveDerivativesAtTime` + `setInterpolationAtTime`, `CurveEditorUndoRedo.cpp:~740-790`, including the `KnobParametric` branch). Undo restores both derivatives and the interpolation.
  - verify: `ctest -R CurveTangentJoin`: (1, 1) gives 1; (0, ∞-like 1e6) gives about 1; (−1, 1) gives 0. A command round-trip on a `KnobDouble` curve restores the broken state on undo.
  - size: M

- [ ] M69.P6.T2 — Ctrl-drag a handle to break it, add a Join action, and let a dragged handle follow the cursor
  - files: `Gui/CurveWidget.cpp`, `Gui/CurveWidgetPrivate.cpp`, `Gui/CurveWidgetPrivate.h`, `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp` (**OVERLAP: ActionShortcuts.h / GuiApplicationManager10.cpp** with P2.T3)
  - approach:
    - Ctrl+drag on a tangent of an unbroken key forces the one-sided path: give `MoveTangentCommand` a `forceBreak` flag that sets `_setBoth=false` and `eKeyframeTypeBroken` (constructor `:633-708`). Plain drag keeps today's both-sides behaviour.
    - Add "Join" next to "Break" in the right-click menu (`CurveWidgetPrivate::createMenu`, `:127-288`), with shortcut `kShortcutIDActionCurveEditorJoin` (Shift+X by default; check it is free in `kShortcutGroupCurveEditor`). It pushes P6.T1's command on the selected keys.
    - While a tangent is dragged, draw that handle at the cursor rather than at the zoom-derived length from `refreshKeyTangents` (`:1462`). It snaps back on release. This is visual only; true length is M96.
  - verify: P7.T1 runs: a smooth key, Ctrl-drag its right handle, and only the right slope changes (key reads Broken). Join gives equal slopes and Free. A plain drag still moves both slopes. Shots of a broken and a joined key.
  - size: M

## Phase 69.8: Legacy-project compatibility removal

- [ ] M69.P8.T1 — Remove the legacy-project compatibility machinery
  - files: `Tests/ProjectSerialization_Test.cpp`, `Tests/fixtures/m65-legacy-color.ntp`, `Engine/KnobSerialization.cpp`, plus any other old-plugin-ID/option remapping the scout finds (list them in the task report)
  - approach: added by the user on 2026-10-10 (no backward compatibility in this fork). First scout for every path that only serves old projects and list them. Then delete `ProjectSerialization_Test`'s `LegacyColourChoiceOptionFallsBackWithoutThrowing` (its remaining Grade `maskChannel` check after M76 stripped the Write part), the `Tests/fixtures/m65-legacy-color.ntp` fixture, `filterKnobChoiceOptionCompat` and the choice-option compat filters in `KnobSerialization.cpp` (~:612-650, including the writer entries M76 left). **Overlap:** M93 - Serialization Boundary reshapes serialization; rebase onto it if it has merged, and re-check with its golden fixtures.
  - verify: at the gate build, ctest is green; `grep -rn "Compat\|legacy" Engine/KnobSerialization.cpp` shows no option remapping; the fixture is gone.
  - size: M

## Phase 69.7: Evidence and UAT

- [ ] M69.P7.T1 — Write the M69 GUI script and run it against the milestone build
  - files: `Tests/gui/m69_cleanup.py` (new)
  - approach: model it on `Tests/gui/m55_node_graph.py` (watchdog, `gt.check`, synthetic `QMouseEvent`/`QKeyEvent` on the NodeGraph, viewer and curve-widget viewports, nodes created with `CreateNodeArgsPropSettingsOpened=True`). Add one section per task verify above (P1.T1 through P6.T2), and a shot for every visual result.
  - verify: `NATRON_GUI_TEST_OUT=build/m69-shots GUI_TEST_TIMEOUT=300 Tests/gui/run-gui-test.sh Tests/gui/m69_cleanup.py build/fast/App/Natron` reports 0 failures. `m55_node_graph.py` and `m55_input_pipes.py` still report 0. The user approves the shots.
  - size: L

- [ ] M69.P7.T2 — Package the AppImage and run the user checkpoint
  - files: `build/appimages/M69-<sha>.AppImage`, `build/appimages/M69-uat.md`
  - approach: `tools/ci/local/package.sh fast`. The UAT script covers deleting a node with its panel focused, then undo in another panel; grabbing stubs zoomed out; pick-up connect with pan, zoom and cancel; Y/Shift+Y; paste-attach; the pane header right-click; In/Out picking with gain/gamma set; the multi-feather drag; Ctrl-drag break and Join.
  - verify: the user signs off on the AppImage.
  - size: M

**Verification gate:**
- One `fast` build (`tools/ci/local/build.sh fast`), with CI `format`, `lint-ci` and `build-and-test` green.
- ctest green, including the new `NodeColorSampler`, `RotoFeatherDrag` and `CurveTangentJoin` cases and the existing `RotoLayer`, `NodeGraphSplice` and `RenderDialogRange`.
- `m69_cleanup.py`, `m55_node_graph.py` and `m55_input_pipes.py` report 0 failures under `Tests/gui/run-gui-test.sh`. The user approves the m69 shots.
- The user signs off on `build/appimages/M69-<sha>.AppImage` through the UAT script.

Acceptance sketch:
- Deleting a node whose panel was focused, then undoing in another panel, does not crash.
- Pasting onto a selected node connects it; unconnected inputs are easy to grab.
- Pick-up mode connects an input with a click, pans/zooms while held, and cancels on right-click/Escape.
- Right-clicking the empty space beside a pane's tabs opens the same menu as the panel management icon, at the cursor.
- The colour picker returns scene-referred values and can read a node's input (default) or output.
- Several selected roto points' feathers move together when one feather bar is dragged.
- Curve-editor handles can be broken and re-joined.

- No legacy-project compatibility code or legacy fixtures remain.

## Decisions

- 2026-10-11 — **Split (consultant proposal, pending user):** knob removals, the global interactive toggle, the footer reorder and the Keyer redesign move to M95 - Knob Cleanup And Keyer Redesign. Weighted (lengthenable) tangents move to M96 - Weighted Curve Tangents, which waits for M93 because it adds keyframe fields to the archive. Drafts: `PLAN/DESIGN/2026-10-11-m69-split-drafts.md`; open questions: `PLAN/DESIGN/2026-10-11-m69-questions.md`.
- 2026-10-11 — **Scouting:** the picker already samples pre-viewer-transform data (`lastRenderedTiles` = `colorImage`). Only the "Linear color pickers" off-path applied a display transform, so the fix is to remove that path and setting. The Tab-menu jump is the completer list shown before the dialog is placed; it is unrelated to the pane menu's position. The node-deletion crash is `removeUndoStack` leaving `_currentUndoAction` dangling.
- 2026-10-11 — **M56 overlap:** M56's "unpremult-by picker has no label" is the openfx-misc Premult/Unpremult plugin's own control, not the host `hostUnPremultBy` selector. It stays in M56. The footer move is M95.P3.T1.
- 2026-10-11 — **User answers:** the split into M69 / M95 - Knob Cleanup And Keyer Redesign / M96 - Weighted Curve Tangents is approved. The Tab-menu jump moves to M45 - Native Tab Menu (P2.T1), so M69.P2.T5 is cancelled. Deps `M44, M55` confirmed. Design questions: `PLAN/DESIGN/2026-10-11-m69-questions.md`, being walked through with the user.
- 2026-10-11 — **User answers to the design questions** (`PLAN/DESIGN/2026-10-11-m69-questions.md`):
  - **Paste onto a selected node:** insert like node creation (re-route the selected node's outputs through the pasted node), one undo step (P2.T4 as written).
  - **Connection shortcuts:** Y / Shift+Y as in Nuke (P2.T3 as written).
  - **Pick-up mode:** left-click on empty space cancels (P2.T2 as written).
  - **Picker Input/Output mode is a modifier, not a toolbar button:** plain Ctrl-click (and the rect/hover picks) read the viewed node's **input**; Ctrl+Alt reads its **output**, re-enabling the old commented-out `kShortcutIDMousePickInputColor` slot inverted. **P4.T3 changes accordingly:** no toolbar button; keep the info-bar "in"/"out" marker.
  - **Several inputs:** the main/preferred input (B on Merge), falling back to Output when there is none.
  - **Roto multi-feather:** same signed distance along each point's own normal.
  - **Tangent join:** mean of the two angles.
- 2026-10-12 — **Added (user, from the M56 shot review):** IDistort's UV picker offers only RGBA. It needs the layer dropdown that other multiplane nodes have, so the U/V/Alpha channels can come from any layer. To be planned as a task when M69 is promoted.
- 2026-10-12 — **Started early (user)** in `.worktrees/m69` on `milestone/m69-minor-cleanup`, branched from `main` at `6fcf9c4fa` (includes M55 and M84). This is a third lane, beyond the usual two, at the user's request while M56 waits on UAT and M45 runs its gate. M69 overlaps M56 (`ViewerTab.cpp`, `Settings.*`, `OfxEffectInstance.cpp`) and M45 (`NodeGraph25.cpp`, `NodeGraph.h`), so it rebases after each of those merges.
