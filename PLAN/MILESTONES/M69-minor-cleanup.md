# M69 - Minor Cleanup

A grab-bag of small UX fixes from the user (2026-10-08), shipped together. They are node-graph interaction, the pane menu, the viewer colour picker, roto multi-point feather, curve-editor tangent break/join, a crash on node deletion, and IDistort's UV layer picker. The knob and Keyer items moved to M95 - Knob Cleanup And Keyer Redesign; weighted tangents moved to M96 - Weighted Curve Tangents (see Decisions). "Unconnected inputs stay visible" was done by M55.P1.T1.

Scouted 2026-10-11 on `milestone/m55-node-graph-polish` at `10e0accd5`. **Re-checked 2026-10-12** on `milestone/m69-minor-cleanup` at `6fcf9c4fa` (main with M44, M55 and M84 merged). Line numbers below are from that tree. One build at the end (user rule): implementers do not compile.

**Rebases expected.** M56 - Visual And Menu Polish and M45 - Native Tab Menu merge before M69 and edit some of the same files. Each task that shares a file says so with **REBASE (M56)** or **REBASE (M45)**; line numbers in those files will shift. Neither milestone changes the functions these tasks edit, except where the task says otherwise.

## Phase 69.1: Crash fix

- [x] M69.P1.T1 — Stop the Edit menu holding dangling undo/redo actions after a node's undo stack dies
  - files: `Gui/Gui30.cpp`, `Gui/GuiPrivate.h`, `Gui/GuiPrivate.cpp` (**REBASE (M56)**: `GuiPrivate.cpp` includes and `createPropertiesBinGui`; `setUndoRedoActions` is untouched)
  - approach:
    - Cause: `Gui::removeUndoStack` (`Gui30.cpp:660-681`) removes the stack's actions from `menuEdit` when they are current, but leaves `_imp->_currentUndoAction`/`_currentRedoAction` pointing at them. The actions are parented to the `QUndoStack` (`registerNewUndoStack`, `:649-657`), so they die with it (`NodeGui::removeUndoStack`, `Gui/NodeGui.cpp:935-940`, from `destroyGui` `:2889`, call at `:2898`; M56 shifts these). The next `GuiPrivate::setUndoRedoActions` (`GuiPrivate.cpp:565-580`) calls `menuEdit->removeAction` on a freed pointer.
    - Fix: in `removeUndoStack`, null the current pointers when they match, and call `_undoStacksGroup->removeStack(stack)` before the stack goes away. Also hold the two current pointers as `QPointer<QAction>` in `GuiPrivate.h:78-79`, so a stack deleted by any other path cannot leave a dangling pointer. `setUndoRedoActions` then skips null entries.
  - verify: covered by P7.T1's crash case at the gate build. Open a Grade's panel, click a slider so the panel's undo stack becomes active, delete the Grade from the graph, then open a Blur's panel and change a value, then use Edit > Undo. No crash, and the Edit menu shows the Blur's undo text.
  - size: M

## Phase 69.2: Node graph interaction

- [x] M69.P2.T1 — Make input arrows easier to grab
  - files: `Gui/NodeGraph10.cpp`, `Gui/Edge.cpp`, `Gui/Edge.h`
  - approach:
    - Today: `NodeGraph::hasItemNearbyMouse` (`NodeGraph10.cpp:109-191`) queries `items()` over a fixed `TO_DPIX(10.)` viewport square (`:131`) against each item's shape. `Edge::shape()` (`Edge.cpp:629`) is the 2px line plus a 10px arrowhead (`ARROW_SIZE_DISCONNECTED`, `:61`). The function then takes `*edges.begin()` (`:180`), which is pointer order, not the nearest edge. Nodes win at the exact point.
    - Change: pick the edge nearest the cursor in scene space using `dist2ToSegment` (file-static in `Edge.cpp:653`; expose it as a static helper). Give unconnected input stubs (`!hasSource()`, not output edges) a larger grab radius in **viewport** pixels, so it doesn't shrink when zoomed out: about `TO_DPIX(18)` around the whole stub plus its free tail, against 10 for connected pipes.
    - Keep "a node under the exact cursor wins" and the bend-point test unchanged. Don't change how pipes are drawn.
  - verify: P7.T1 sends presses at 12px and 16px from an unconnected Merge input stub, at 100% and 50% zoom. Each starts an arrow drag (the stub follows the cursor). A press 30px away doesn't. With two pipes crossing near the cursor, the nearer one is taken.
  - size: M

- [x] M69.P2.T2 — Add pick-up connection mode: click an input, it follows the cursor, click a node to connect
  - files: `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph10.cpp`, `Gui/NodeGraph15.cpp`, `Gui/NodeGraph20.cpp`, `Gui/NodeGraph25.cpp` (**REBASE (M45)**: M45 drops `_lastNodeCreatedName` from `NodeGraphPrivate.h`, and in `NodeGraph25.cpp` rewrites `NodeGraph::event`/`popCreateNodeDialog` and removes the create-node branch of `keyPressEvent`; the Escape branch is untouched)
  - approach:
    - A press and release on an input arrow with no movement (`hasMovedOnce` false at the `eEventStateDraggingArrow` release, `NodeGraph15.cpp:141-142`) enters pick-up instead of doing nothing. Store `_pickedUpArrow` (an `Edge*`) plus the original source in `NodeGraphPrivate`, as a flag **separate from `_evtState`**, so the existing pan (middle drag) and zoom (wheel) states keep working while an input is held.
    - While held: every mouse move, and every pan or zoom step, calls `dragSource(mapFromScene(cursor))` (`NodeGraph20.cpp:461-468` is the existing drag path), so the arrow stays under the cursor.
    - Left-click on a node: connect with exactly the `ConnectCommand` and `handleConnectionError` path the drag release uses (`NodeGraph15.cpp:142-191`). Factor that block into one helper shared by drag and pick-up.
    - Right-click or Escape (`NodeGraph25.cpp` `keyPressEvent` `:230`, Escape at `:248`) cancels: restore the original source with `initLine`, push nothing, and suppress the context menu for that right-click.
    - Left-click on empty space also cancels. Picking up an already-connected input shows it detached while held; cancel restores it.
    - Pick-up mode applies to input arrows only, not output edges.
  - verify: P7.T1 runs: click a Blur's input, move, pan with the middle button, zoom with the wheel (the arrow tip stays at the cursor), click a Constant, and Blur's input 0 is the Constant, undone in one Ctrl+Z. A second run is cancelled by Escape and a third by right-click; in both, connections are unchanged, no undo entry is added, and no context menu opens.
  - size: L

- [x] M69.P2.T3 — Add keyboard shortcuts that connect the selected nodes
  - files: `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp`, `Gui/NodeGraph25.cpp`, `Gui/NodeGraph40.cpp`, `Gui/NodeGraph.h` (**OVERLAP: ActionShortcuts.h / GuiApplicationManager10.cpp** with P4.T3 and P6.T2; **REBASE (M56)**: both shortcut files gain the curve editor's "Free" entry; **REBASE (M45)**: `GuiApplicationManager10.cpp` gains an include and `TabMenuBridge::bootstrap()`, and `NodeGraph25.cpp`/`NodeGraph.h` as in P2.T2)
  - approach:
    - **Y** connects: each other selected node, in selection order, feeds the next free input of the **last-selected** node, preferred input first (`Node::getPreferredInputForConnection`). **Shift+Y** connects in reverse: the last-selected node feeds the preferred input of every other selected node.
    - Register both in `kShortcutGroupNodegraph`. Y and Shift+Y are free in that group; the viewer's Y luminance bindings (`GuiApplicationManager10.cpp:842,849`) are in a different group.
    - Push all connections as one parent `QUndoCommand` holding `ConnectCommand`s.
    - Skip connections that would make a cycle or that `canConnectInput` rejects, and report them in the status bar. Do nothing with fewer than two selected nodes.
  - verify: P7.T1 selects Constant, then Checkerboard, then Merge, and presses Y: Merge A and B are fed by the two sources, undone in one step. It selects Grade, Blur and Constant (last) and presses Shift+Y: both Grade and Blur take the Constant.
  - size: M

- [x] M69.P2.T4 — Make pasting onto a selected node connect it
  - files: `Gui/NodeGraph40.cpp`, `Gui/NodeGraphPrivate10.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraphSplice.h`, `Gui/NodeGraphSplice.cpp` (**REBASE (M45)**: `NodeGraphPrivate.h`, as in P2.T2)
  - approach:
    - In `NodeGraph::pasteNodeClipBoards(const QPointF&)` (`NodeGraph40.cpp:174-198`; Ctrl+V reaches it through the no-argument overload at `:201` from `NodeGraph25.cpp:266`), capture the selection before `pasteNodesInternal` (`NodeGraphPrivate10.cpp:56-164`) replaces it.
    - If exactly one non-backdrop node was selected, use M55's `planSpliceForSelection` (`Gui/NodeGraphSplice.h:73`) on the pasted nodes to find the single entry with a free preferred input. Connect that input to the selected node.
    - Then place the pasted set as node creation does: `moveNodesForIdealPosition(entry, selected, true)` (`Gui/NodeGraph05.cpp:54`), which also re-routes the selected node's outputs through the exit. Move the other pasted nodes by the same offset.
    - Make the `AddMultipleNodesCommand` pushed at `NodeGraphPrivate10.cpp:161` and the connection one undo step (a parent command).
    - Leave Ctrl+V with no selection, duplicate (Alt+C) and clone unchanged. If the pasted set has no valid entry, paste unconnected, as today.
    - **OVERLAP with M93**: the clipboard is read through a boost `xml_iarchive` in this function (`NodeGraph40.cpp:189`); rebase onto M93 if it has merged.
  - verify: P7.T1 copies a Blur, selects a Read that feeds a Viewer, and pastes: the result is Read→Blur→Viewer, and one Ctrl+Z removes the Blur and restores Read→Viewer. A pasted Blur→Grade chain enters at the Blur. Pasting with nothing selected leaves the Blur unconnected.
  - size: L

- [ ] ~~M69.P2.T5 — Open the Tab node menu at the cursor with no jump~~ (cancelled 2026-10-11: M45.P2.T1 takes this over; M45 also deletes `NodeCreationDialog`)
  - files: `Gui/NodeCreationDialog.cpp`, `Gui/NodeCreationDialog.h`
  - approach:
    - Cause: the `NodeCreationDialog` constructor calls `textEdit->setText(initialFilterName)` (`:445-447`) after `textChanged` is connected to `filterText` (`:106`). So `filterText` shows the `Qt::ToolTip` list view at `mapToGlobal(QPoint(0, height()))` (`:186-198`) before the dialog has been shown or moved. That position is near the screen's top-left. `QTimer::singleShot(20, showCompleter)` (`:459`) then moves it.
    - Fix: `filterText` updates the model but only shows or moves the list while `window()->isVisible()`. Override `NodeCreationDialog::showEvent` to place the dialog at the cursor (the current centring maths, `:450-454`, but using the final `sizeHint()` after the line edit is in the layout), then call `showCompleter()` directly. Drop the 20ms timer.
    - This is not the same bug as the pane menu in P3.T1.
  - verify: P7.T1 presses Tab over the graph and, in the first event-loop turn after the dialog appears, records the global geometry of the dialog and its list view: both lie within 300px of the cursor, and the list's top-left never reads `(0,0)`. Shots before and after.
  - size: M

## Phase 69.3: Panels

- [x] M69.P3.T1 — Right-click the empty header space for the pane menu, at the cursor
  - files: `Gui/TabWidget.h`, `Gui/TabWidget.cpp` (**REBASE (M56)**: `TabWidget.cpp` includes and the constructor's header margins/spacing, `:257-286`)
  - approach:
    - The pane menu is `TabWidget::createMenu()` (`TabWidget.cpp:383-470`): split vertical/horizontal, float, close, hide toolbar/tabs header, the "... here" entries, set as anchor. It is connected to `leftCornerButton`'s `clicked()` (`:314`) and opens with `menu.exec(leftCornerButton->mapToGlobal(QPoint(0, 0)))` (`:469`). The empty space right of the tabs is the stretch in the header row, owned by `TabWidgetHeader` (`TabWidget.h:121-145`), a bare `QWidget` that only handles `leaveEvent`.
    - Give `TabWidgetHeader` a `contextMenuEvent` that emits `paneMenuRequested(QPoint globalPos)` only when `childAt(e->pos())` is null, so the stretch area is the only target. Right-clicks on `TabBar` and the float/close buttons keep their own behaviour.
    - Change `createMenu()` to `createMenu(const QPoint& globalPos)`. The `leftCornerButton` slot passes `leftCornerButton->mapToGlobal(QPoint(0,0))`, unchanged; the header passes the cursor position.
  - verify: P7.T1 right-clicks the empty header of the graph pane: a `QMenu` opens with the same action texts as the corner icon's menu, its top-left at the cursor. Right-clicking a tab doesn't open it. The corner icon still opens its menu at the icon.
  - size: M

## Phase 69.4: Viewer colour picker

- [x] M69.P4.T1 — Always pick scene-referred values; remove "Linear color pickers"
  - files: `Gui/ViewerGL.cpp`, `Gui/ViewerGL.h`, `Engine/Settings.h`, `Engine/Settings.cpp`, `Gui/KnobGuiColor.cpp` (**REBASE (M56)**: `Settings.*` loses `_useBWIcons`, and `KnobGuiColor.cpp`'s icon code at `:886-910` changes; **REBASE (M45)**: `Settings.*` gains the Tab-menu knobs)
  - approach:
    - Finding: the picker does **not** read the display texture. `getColorAt`/`getColorAtRect` (`ViewerGL.cpp:4279-4517`) sample `lastRenderedTiles`, which holds `params->colorImage`. That is the viewed node's rendered image (`Engine/ViewerInstance.cpp:2961` → `endTransferBufferFromRAMToGPU` → `ViewerGL.cpp:1505`), before gain, gamma and the display transform. The display processor is applied only when `forceLinear` is false (`:4328-4332`, `:4458-4496`), which is the `linearPickers` setting (default **true**).
    - Remove the `forceLinear` parameter and the `applyViewerDisplayTransform` branches from both functions, and their callers at `:2626`, `:3734` and `:3910`.
    - Delete `_linearPickers` and `getColorPickerLinear()` (`Settings.h:119,516`; `Settings.cpp:691-696,1599,2577-2580`). An old settings file's `linearPickers` key is never looked up again, as M55 found for `autoHideInputs`.
    - Rewrite the colour knob's picker tooltip (`KnobGuiColor.cpp:866-867`, the "converts to linear" sentence): picked values are the image's scene-referred values before any viewer transform.
  - verify: P7.T1 shows a Constant (0.18, 2.0, 0.5, 1) through viewer gain +2, gamma 2.2 and the sRGB display. Ctrl+Alt-click (output, P4.T3) reads back exactly (0.18, 2.0, 0.5, 1) in the info bar and in a picking Grade's colour knob. The rect pick returns the same values. The Preferences UI page no longer lists "Linear color pickers".
  - size: M

- [x] M69.P4.T2 — Add an Engine helper that samples a node's image over a pixel rect
  - files: `Engine/NodeColorSampler.h` (new), `Engine/NodeColorSampler.cpp` (new), `Tests/NodeColorSampler_Test.cpp` (new), `Tests/CMakeLists.txt` (**OVERLAP: Tests/CMakeLists.txt** with P5.T1, P6.T1; **REBASE (M56, M45)**: both add test sources)
  - approach:
    - `bool sampleNodeColor(const NodePtr& node, double time, ViewIdx view, unsigned int mipmapLevel, const RectI& pixelRect, const ImageLayerDesc& layer, float rgba[4])`. It averages the rect. It renders the RoI synchronously the way `TrackMarker::getMarkerImage` does (`Engine/TrackMarker.cpp:1162-1220`: `ParallelRenderArgsSetter` at `:1186` + `renderRoI` at `:1212`), on the same scale and layer the viewer used, so the normal case is a cache hit on the image the viewer render just pulled through.
    - It returns false for a null, disabled or deep node. Treat a missing layer channel as 0 and a missing alpha as 1.
  - verify: `ctest -R NodeColorSampler`: for Constant(0.25)→Grade(gain 2), sampling the Grade gives 0.5 and its input (the Constant) gives 0.25. A 4×4 rect over a CheckerBoard square boundary gives the mean. A disconnected node returns false.
  - size: M

- [x] M69.P4.T3 — Make the picker read the viewed node's input by default, and its output with Ctrl+Alt
  - files: `Gui/ViewerGL.cpp`, `Gui/ViewerGL.h`, `Gui/ViewerGLPrivate.h`, `Gui/InfoViewerWidget.h`, `Gui/InfoViewerWidget.cpp`, `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp` (seven files, but the last four are one-line or few-line edits; **OVERLAP: ActionShortcuts.h / GuiApplicationManager10.cpp** with P2.T3 and P6.T2; **REBASE (M56, M45)**: as in P2.T3, in other parts of those two files. This task no longer touches `ViewerTab.cpp`, which M56 rewrites heavily.)
  - approach:
    - This is a modifier, not a toolbar button (user decision 2026-10-11). Plain **Ctrl-click** (`kShortcutIDMousePickColor`), the **Ctrl+Shift rect pick** (`kShortcutIDMouseRectanglePick`) and the **hover readout** (`updateColorPicker`, `ViewerGL.cpp:2562-2670`) read the viewed node's **input**. **Ctrl+Alt-click** reads its **output**, which is today's path.
    - The plumbing already exists, inverted and half-dead. `mousePressEvent` already has a second branch on `kShortcutIDMousePickInputColor` that calls `pickColor(x, y, true)` and enters `eMouseStatePickingInputColor` (`:1889-1899`). The mouse-move handler re-picks per state (`:2469-2477`). `pickColorInternal(x, y, /*pickInput*/)` (`:3720`) ignores the flag. Only the registration is commented out (`GuiApplicationManager10.cpp:900`, Ctrl+Alt+Left). Its `#define` is live (`ActionShortcuts.h:337-338`).
    - Invert it. Rename the shortcut to `kShortcutIDMousePickOutputColor` `"pickOutput"`, described as "Pick a Color from the viewed node's output". Register it with `Qt::ControlModifier | Qt::AltModifier`, `Qt::LeftButton`. No saved shortcut file can name `"pickInput"`, because it was never registered. Re-describe `kShortcutIDMousePickColor` as "Pick a Color from the viewed node's input". Rename `eMouseStatePickingInputColor` to `eMouseStatePickingOutputColor` (`ViewerGLPrivate.h:81`). Make the `bool` passed through `pickColor`/`pickColorInternal` a `pickOutput` that is true only on that path. Store the last point pick's mode in `ViewerGLPrivate`, so a later re-pick at `lastPickerPos` keeps it.
    - Input path, for each texture *i*: get the viewed node from `ViewerInstance::getActiveInputs` (`ViewerGL.cpp:588` shows the call) → `getInput`. Take its `getPreferredInputNode()` (`Engine/NodeInputs.cpp:1534`). For connected inputs that is "Source", else **B** before A (`getPreferredInputInternal(true)` ignores the Merge-A preference), else the first non-optional input. Sample it with P4.T2's `sampleNodeColor` at the same time, view, mipmap level and layer as the tile `getColorAt`/`getColorAtRect` would read (`lastRenderedTiles[mipmapLevel]`'s components). The pick is one pixel for a point/hover and the canonical rect converted to pixels for a rect pick.
    - Fallback to the output path, marked "out": the viewed node has no inputs or no connected preferred input (a Read, a generator), or the sampler returns false.
    - Info bar: add `InfoViewerWidget::setColorSource(bool fromInput)`, a small "in"/"out" label beside the RGBA values, set on every `setColor` call in `updateColorPicker`, `pickColorInternal` and `updateRectangleColorPickerInternal`. The picked value flows unchanged to `setColorPickersColor`, the parametric picker and the histogram cursor.
    - Ctrl+Alt+Left does not collide in the viewer: zoom is Alt+**Meta**+Left (`ViewerGL.cpp:1827-1834`), and pan excludes Control (`:1819-1825`). Plugin overlays still see the press first, as for Ctrl-click.
  - verify: P7.T1 views Constant(0.25)→Grade(gain 2). Ctrl-click reads 0.25 marked "in"; Ctrl+Alt-click reads 0.5 marked "out"; the Ctrl+Shift rect pick and the hover readout read 0.25 "in". With a Merge viewed (B = Constant 0.25, A = Constant 0.75), Ctrl-click reads 0.25. On a Read, Ctrl-click reads the Read's output, marked "out". Shots of the info bar in both states. UAT: hovering across a 2K plate under a heavy Blur stays interactive, and Ctrl+Alt-drag is not taken by the window manager.
  - size: L

## Phase 69.5: Roto multi-point feather

- [x] M69.P5.T1 — Add a pure helper for the multi-point feather drag, with unit tests
  - files: `Engine/RotoFeatherDrag.h` (new), `Engine/RotoFeatherDrag.cpp` (new), `Tests/RotoFeatherDrag_Test.cpp` (new), `Tests/CMakeLists.txt` (**OVERLAP: Tests/CMakeLists.txt**; **REBASE (M56, M45)**)
  - approach:
    - Extract the per-point direction maths from `MoveFeatherBarUndoCommand::redo` (`Engine/RotoUndoCommand.cpp:1012-1110`): the unit feather direction, or the normal from left/right derivatives when the feather sits on the control point.
    - Add `double featherDragDistance(dir, dx, dy)`, the projection of the drag onto the grabbed point's direction, and `Point applyFeatherDistance(featherPt, dir, s)`.
    - Rule (user decision): every selected point moves by the same signed distance `s` along its own direction.
  - verify: `ctest -R RotoFeatherDrag`: a drag along a direction gives that distance. Three points with different normals each move by `s` along their own normal. The feather-on-point case uses the derivative normal.
  - size: M

- [x] M69.P5.T2 — Drag the feather of every selected point together, as one undo step
  - files: `Engine/RotoPaint.cpp`, `Engine/RotoUndoCommand.h`, `Engine/RotoUndoCommand.cpp` (**REBASE (M56)**: `RotoPaint.cpp`'s `initializeKnobs` icon paths change, so the line numbers below drop by about 15; the press and move handlers are untouched)
  - approach:
    - In the press handler, when the grabbed feather bar's control point is already selected, skip `clearCPSSelection()` (`RotoPaint.cpp:2317` and `:2353`); otherwise select it alone, as today.
    - Change `MoveFeatherBarUndoCommand` (`RotoUndoCommand.h:320-348`) to hold a list of (control point, feather point) pairs with their old positions, built from `selectedCps`. It computes `s` once from the grabbed point and applies it to every pair through P5.T1, at `_time`, with ripple edit honoured per point. Keep `mergeWith` (`RotoUndoCommand.cpp:1111`) so a drag is one undo step.
    - Call it from the move handler (`:2852-2853`).
  - verify: `ctest -R RotoLayer` still passes. P7.T1: a Bezier with three of five points selected, one feather bar dragged outward by 20px, moves exactly those three feathers by the same distance along their normals, and one Ctrl+Z restores all three. Shots before and after.
  - size: L

## Phase 69.6: Curve editor tangents (break and join)

- [x] M69.P6.T1 — Add a "join tangents" command and its slope helper, with unit tests
  - files: `Gui/CurveEditorUndoRedo.h`, `Gui/CurveEditorUndoRedo.cpp`, `Tests/CurveTangentJoin_Test.cpp` (new), `Tests/CMakeLists.txt` (**OVERLAP: Tests/CMakeLists.txt**; **REBASE (M56, M45)**)
  - approach:
    - Pure `double joinedDerivative(double left, double right)`: the slope of the mean of the two tangent angles, `tan((atan(l)+atan(r))/2)` (user decision).
    - Add `JoinTangentsCommand(widget, keys)`: for each `eKeyframeTypeBroken` key, set both derivatives to the joined value and set the interpolation to `eKeyframeTypeFree`, through the same knob path `MoveTangentCommand::setNewDerivatives` uses (`CurveEditorUndoRedo.cpp:754-~800`: `moveDerivativesAtTime` + `setInterpolationAtTime`, including the `KnobParametric` branch at `:761`). Undo restores both derivatives and the interpolation.
  - verify: `ctest -R CurveTangentJoin`: (1, 1) gives 1; (0, ∞-like 1e6) gives about 1; (−1, 1) gives 0. A command round-trip on a `KnobDouble` curve restores the broken state on undo.
  - size: M

- [x] M69.P6.T2 — Ctrl-drag a handle to break it, add a Join action, and let a dragged handle follow the cursor
  - files: `Gui/CurveWidget.cpp`, `Gui/CurveWidgetPrivate.cpp`, `Gui/CurveWidgetPrivate.h`, `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp` (**OVERLAP: ActionShortcuts.h / GuiApplicationManager10.cpp** with P2.T3 and P4.T3; **REBASE (M56)**: M56 adds a "Free Interpolation" action (`kShortcutIDActionCurveEditorFree`, no default key) right after Break, in the same five places this task edits: the `#define`s, both `registerKeybind` groups, `CurveWidgetPrivate::createMenu`, `CurveWidget::keyPressEvent`, plus a `freeInterpForSelectedKeyFrames` slot. Put Join after Free.)
  - approach:
    - Ctrl+drag on a tangent of an unbroken key forces the one-sided path: give `MoveTangentCommand` a `forceBreak` flag that sets `_setBoth=false` and `eKeyframeTypeBroken` (constructor `CurveEditorUndoRedo.cpp:633-708`; `_setBoth` is decided at `:678`). Plain drag keeps today's both-sides behaviour.
    - Add "Join" next to "Break" in the right-click menu (`CurveWidgetPrivate::createMenu`, `:127-288`; Break at `:250-254`), with shortcut `kShortcutIDActionCurveEditorJoin`, Shift+X by default. Shift+X is free in `kShortcutGroupCurveEditor` and `kShortcutGroupDopeSheetEditor`; the only Shift+X is the node graph's Switch Inputs. Handle it in `CurveWidget::keyPressEvent` beside Break. It pushes P6.T1's command on the selected keys.
    - While a tangent is dragged, draw that handle at the cursor rather than at the zoom-derived length from `refreshKeyTangents` (`CurveWidgetPrivate.cpp:1462`). It snaps back on release. This is visual only; true length is M96.
  - verify: P7.T1 runs: a smooth key, Ctrl-drag its right handle, and only the right slope changes (key reads Broken). Join gives equal slopes and Free. A plain drag still moves both slopes. Shots of a broken and a joined key.
  - size: M

## Phase 69.8: Legacy-project compatibility removal

- [x] M69.P8.T1 — Remove the knob-name and choice-option compatibility filters
  - files: `Engine/KnobSerialization.cpp`, `Engine/KnobSerialization.h`, `Engine/Node.cpp`, `Engine/PyParameter.cpp`, `Tests/ProjectSerialization_Test.cpp` (**REBASE (M45)**: `Node.cpp` gains about 24 lines elsewhere; **OVERLAP with M76 - Native Write**, on another host, and **M93 - Serialization Boundary**; see approach)
  - approach: the user added this on 2026-10-10 (no backward compatibility in this fork). The re-check of 2026-10-12 found both still on `main`:
    - The filter tables are the whole anonymous namespace `KnobSerialization.cpp:301-659`: `KnobNameFilter`/`KnobChoiceOptionFilter`, `KnobNameFilters` `:445-566`, `KnobChoiceOptionFilters` `:568-657` with the WriteOIIO `outputChannels` motion/disparity entries and the frame-range/bit-depth entries, and the two statics `:658-659`. The functions are `filterKnobNameCompat`/`filterKnobChoiceOptionCompat` at `:740-780`, declared at `KnobSerialization.h:240-262`. Delete all of it.
    - Live callers: `Node.cpp:1762`, `:1780` and `:1816`, and `PyParameter.cpp:1686`. Each keeps the serialized name or option as-is. Delete the commented-out calls at `NodeMain.cpp:670`, `WriteNode.cpp:397` and `ProjectPrivate.cpp:190`, and the `choiceMatch` TODO at `KnobTypes.cpp:1000-1002` that names the function. If `Node.cpp:1751`'s `vMajor == 2 && vMinor < 3` branch exists only to feed these filters, delete it too; otherwise list it in the task report.
    - Delete `ProjectSerialization_Test`'s `LegacyColourChoiceOptionFallsBackWithoutThrowing` (`:413-442`). On `main` it still has its Write1/`outputChannels` half, because M76 has not merged. If M76 merges first, it will already have stripped that half and some writer entries; rebase onto it and delete what is left.
    - The `m65-legacy-color.ntp` fixture and the other `LegacyColorProjectTest` cases are P8.T2.
    - If M93 has merged, rebase onto it and re-check with its golden fixtures.
  - verify: at the gate build, ctest is green; `grep -rn "filterKnob\(Name\|ChoiceOption\)Compat\|KnobChoiceOptionFilter\|KnobNameFilter" Engine Gui Tests` finds nothing.
  - size: M

- [x] M69.P8.T2 — Remove the legacy colour-layer reset and the pre-v17 channel-set gate, with both legacy fixtures
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/NodePrivate.h`, `Engine/NodeMain.cpp`, `Engine/KnobChannelSet.h`/`.cpp`, `Tests/ProjectSerialization_Test.cpp`, `Tests/DefaultChannelSet_Test.cpp`, `Tests/fixtures/m65-legacy-color.ntp`, `Tests/fixtures/channel-set-legacy-defaults.ntp` (**REBASE (M45)**: `Node.cpp`/`Node.h`; pending Q3)
  - approach: split from P8.T1 by the 2026-10-12 scout. Deleting the `m65-legacy-color.ntp` fixture, as P8.T1 first asked, removes every `LegacyColorProjectTest` case (`ProjectSerialization_Test.cpp:312-~530`: `ColorLayerValuesResetToRgbaKeepingChannelsWithAWarning`, `LegacyColorWarningSurvivesARender` and `ResavingWritesRgbaNeverTheStorageID`, beside P8.T1's case). Those tests are the only coverage of M65's load-time machinery. Delete the machinery with them:
    - the legacy colour-layer reset and warning: `rewriteLegacyColorRows`/`rewriteLegacyColorValue`/`Node::resetLegacyColorLayerKnobs`/`postPendingLegacyColorLayerWarning` (`Node.cpp:1450-1600`), the call at `:1672-1676` and `NodeMain.cpp:227`, and `legacyColorLayerWarningPending` (`NodePrivate.h:274,534`);
    - the pre-v17 channel-set default gate: `legacyChannelSetDefault` (`NodePrivate.h:273,532`; `Node.cpp:1640-1650,2903,2963-2972`), `KnobChannelSet::legacyColorDefaultRows` (`KnobChannelSet.h:199`, `.cpp:113`), `DefaultChannelSet_Test`'s `LegacyProjectResetsUnsavedColorToRgbaWithAWarning` and `LegacyColorResetToRgbaSurvivesResaving` (`:485-~560`), and its `channel-set-legacy-defaults.ntp` fixture.
    - Leave `NodeGui.cpp`'s `getLegacyCategoryColor` alone: it is node tile colour, not project compatibility.
    - Run after P8.T1, in the same files' rebase.
  - verify: at the gate build, ctest is green; `grep -rn "legacyColor\|LegacyColor\|legacyChannelSetDefault" Engine Gui Tests` finds nothing; `ls Tests/fixtures/*legacy*` finds nothing.
  - size: L

## Phase 69.9: IDistort UV layer

- [ ] ~~M69.P9.T1 — Give IDistort and STMap a UV layer dropdown that feeds their U, V and Alpha pickers~~ (cancelled 2026-10-12 by the user: IDistort and STMap are rewritten as native nodes in M113 - Native Node Rewrites Continued)
  - files: `Engine/OfxEffectInstance.cpp`, `Engine/Node.h`, `Engine/Node.cpp`, `Engine/NodePrivate.h`, `Tests/LayerKnobsRender_Test.cpp` (**REBASE (M56)**: M56 edits `takeOverMultiplaneChoices`, see approach; **REBASE (M45)**: `Node.cpp`/`Node.h`; pending Q1, Q2)
  - approach:
    - Why it looks RGBA-only. IDistort's `channelU`/`channelV`/`channelA` (openfx-misc `Distortion.cpp:3636-3656`, `describeInContextAddPlaneChannelChoice` over the single `UV` clip) are recognised as channel choices by `OfxMultiplaneChoice::isMultiplaneChannelChoice`. `takeOverMultiplaneChoices` (`OfxEffectInstance.cpp:827-982`) twins each of them as a flat `KnobChannelSelect` (`hostChannelU`/`V`/`A`), not as a layer dropdown; only `kPlaneChoiceTakeOvers` (`:724-727`, Premult/Unpremult `inputPlane`) get a `KnobLayerSelect`. Each twin is input-bound to the UV clip (resolved from the option hints, `:872-884`), and its combo lists `layer.channel` for every layer that input carries. `LayerKnobsRender_Test.cpp:800` shows `diffuse.R` is listed when the UV input carries diffuse. An **unconnected** UV input lists only the colour views (`Node.cpp` `appendInputStreamLayers`, "an unconnected input still lists" Color), which is what M56's shot showed. So nothing is broken, but there is no layer dropdown, and a long flat channel list is hard to use.
    - Why M56 matters. M56 (`edd6372ba`) adds `kChannelChoiceLabelOverrides`. It also makes every channel twin `setAddNewLine(true)` and forces the hidden plugin choice to end its row, so U, V and Alpha each sit on their own row. The new dropdown must be inserted with the same row rules, so write this on top of M56's version of the function.
    - Add one host knob per plugin, `hostUVLayer`, labelled "UV Layer": a `KnobLayerSelect` with no channel quad and no None/All entries, default `rgba`, inserted just before `hostChannelU` on its own row. Declare it input-bound to the same UV input (`node->declareLayerKnob(knob, inputNb, eRoleInputBound)`). Create it inside `takeOverMultiplaneChoices` when the plugin ID is `net.sf.openfx.IDistort` or `net.sf.openfx.STMap` and the `channelU` twin has been made. Use a small table like `kPlaneChoiceTakeOvers` (plugin ID, the twins it filters), not an `if` on names in the loop.
    - Filter. Add `KnobIWPtr layerFilter` to `LayerKnobSource` (`NodePrivate.h:79-101`) and `Node::setLayerKnobFilter(const KnobIPtr& knob, const KnobIPtr& layerSelect)`. In `Node::listLayersForKnob`, after `appendInputStreamLayers`, keep only the layer the filter selects when a filter is set, matching a colour view to the colour storage layer. Set it on `hostChannelU` and `hostChannelV`, and on `hostChannelA` only if Q1 says so. The channel-select GUI relists through `layerListRefreshed` with no GUI change.
    - Changing the layer (user edits only, never on project load, so saved U/V values survive): set `hostChannelU` to the layer's first channel and `hostChannelV` to its second. Set `hostChannelA` to the layer's alpha channel if it has one, else leave it. Then call `node->relistLayerKnobs()`. Hook it where `onMultiplaneTwinChanged` (`OfxEffectInstance.cpp:985`) is reached from `Node.cpp:6288`. The U/V/A twins then push to the plugin through the existing `pushMultiplaneTwin`, unchanged.
    - Load: `hostUVLayer` is persistent. A project without it (every one saved before M69) derives it from `hostChannelU`'s layer in `syncMultiplaneTwinsAfterLoad`.
  - verify: a new `LayerKnobsRender_Test` case: UV reader carrying `rgba` and `diffuse`, IDistort's `hostUVLayer` set to `diffuse`. `listLayersForKnob(hostChannelU)` then lists only diffuse; U is `diffuse.R` and V is `diffuse.G`; the plugin's `channelU` active entry is `diffuse.R`. A save/load round trip keeps `hostUVLayer`, U and V, and a project with no `hostUVLayer` loads it from U. The existing `IDistortChannelTwinReadsTheLayerFromTheUVInput`, `NoneChannelTwinsPushTheNeutralPluginConstant` and `ColorViewsRender_Test`'s IDistort case still pass. P7.T1 takes a shot of the IDistort panel with a multi-layer UV input, showing the UV Layer dropdown open, and checks that `m56_visual.py`'s IDistort/STMap rows still line up.
  - size: L

## Phase 69.7: Evidence and UAT

- [x] M69.P7.T1 — Write the M69 GUI script and run it against the milestone build
  - files: `Tests/gui/m69_cleanup.py` (new)
  - approach: model it on `Tests/gui/m55_node_graph.py` (watchdog, `gt.check`, synthetic `QMouseEvent`/`QKeyEvent` on the NodeGraph, viewer and curve-widget viewports, nodes created with `CreateNodeArgsPropSettingsOpened=True`). Add one section per task verify above (P1.T1 through P6.T2, and P9.T1), and a shot for every visual result.
  - verify: `NATRON_GUI_TEST_OUT=build/m69-shots GUI_TEST_TIMEOUT=300 Tests/gui/run-gui-test.sh Tests/gui/m69_cleanup.py build/fast/App/Natron` reports 0 failures. `m55_node_graph.py`, `m55_input_pipes.py` and (after the M56 rebase) `m56_visual.py` still report 0. The user approves the shots.
  - size: L

- [x] M69.P7.T2 — Package the AppImage and run the user checkpoint
  - files: `build/appimages/M69-<sha>.AppImage`, `build/appimages/M69-uat.md`
  - approach: `tools/ci/local/package.sh fast`. The UAT script covers:
    - deleting a node with its panel focused, then undo in another panel;
    - grabbing stubs zoomed out;
    - pick-up connect with pan, zoom and cancel;
    - Y/Shift+Y;
    - paste-attach;
    - the pane header right-click;
    - Ctrl-click (input) against Ctrl+Alt-click (output) with gain/gamma set, and hover on a heavy comp;
    - the multi-feather drag;
    - Ctrl-drag break and Join;
    - IDistort's UV Layer dropdown on a multi-layer EXR.
  - verify: the user signs off on the AppImage.
  - size: M

## Phase 69.10: UAT round 2 fixes (2026-10-12)

Line numbers are from `milestone/m69-minor-cleanup` at `a2e90be96`. They will shift once M45 - Native Tab Menu and M56 - Visual And Menu Polish merge and this branch rebases.

- [ ] M69.P10.T1 — With an input held, a left-click on anything that isn't a node disconnects that input, as one undo step
  - files: `Gui/NodeGraphPickUp.h`, `Gui/NodeGraphPickUp.cpp`, `Gui/NodeGraph15.cpp`, `Tests/NodeGraphPickUp_Test.cpp`, `Tests/gui/m69_cleanup.py`
  - approach:
    - Add `ePickUpPressDisconnect` to `PickUpPressActionEnum` (`NodeGraphPickUp.h:43-48`).
    - `decidePickUpPress` (`NodeGraphPickUp.cpp:38-62`) returns it for a left click on `ePickUpClickTargetOther`: empty space, a pipe or a backdrop (user decision). A left click on the holding node, a right-click and Escape still cancel.
    - `NodeGraph::handlePickUpPress` (`NodeGraph15.cpp:202-253`) gets a new case. It clears `_pickedUpArrowDest`/`_pickedUpArrowInput` the way the Connect case does. If `arrow->getSource()` is set, it pushes `new ConnectCommand(this, arrow, arrow->getSource(), NodeGuiPtr())`, as the drag-release disconnect does (`:307-310`). It then calls `refreshEdges()` and `initLine()`.
    - An unconnected stub just drops, with no undo entry.
  - verify:
    - gtest `NodeGraphPickUp`: `LeftClickOnEmptySpaceCancels` (`:61`) becomes a Disconnect expectation. The holding-node and right-click cases stay Cancel.
    - The GUI `pick-up` section: picking up a connected input and clicking empty space leaves it unconnected, and one undo restores it.
  - size: M
- [ ] M69.P10.T2 — Re-anchor Y and Shift+Y on selection order
  - files: `Gui/NodeGraphConnect.cpp`, `Gui/NodeGraphConnect.h`, `Gui/ActionShortcuts.h`, `Gui/NodeGraph40.cpp`, `Tests/gui/m69_cleanup.py`
  - approach:
    - `_selection` is kept in click order. Both wrappers in `NodeGraphConnect.cpp` take `selection.back()` as the anchor today (`:95`, `:148`).
    - **Y** (user): the first-selected node is the one whose inputs change; the later nodes, in order, feed its free inputs (preferred input first, then input order).
    - **Shift+Y** (user): the same connection with the order reversed. The sources come first, in order, and the last-selected node is the one whose inputs change.
    - The planners (`planFeedLastSelected` `:38`, `planFeedSelectionFromLast` `:69`) are id-based. Rename them and their parameters to describe their new use.
    - Update the descriptions (`ActionShortcuts.h:512-516`) and the undo texts (`NodeGraph40.cpp:701,709`). Keep the shortcut IDs.
  - verify:
    - `NodeGraphConnect_Test` passes, with its planner cases updated to the new semantics.
    - The GUI `y-shift-y` section (`m69_cleanup.py:726-760`) uses `setSelection([target, s1, s2])` + Y and `setSelection([s1, s2, target])` + Shift+Y, and checks that s1 lands on the preferred input (B), so order is tested.
  - size: M
- [ ] M69.P10.T3 — Add Nuke-style U and Shift+U
  - files: `Gui/NodeGraphConnect.cpp`, `Gui/NodeGraphConnect.h`, `Gui/ActionShortcuts.h`, `Gui/NodeGraph40.cpp`, `Tests/gui/m69_cleanup.py` (plus the gtest file `NodeGraphConnect_Test`)
  - approach: two new rebindable shortcuts, registered and dispatched the way P2.T3 registered Y and Shift+Y. Each is one undo step.
    - **U** (user): the first-selected node's output goes into the first free input of every later-selected node.
    - **Shift+U** (user): the last-selected node's output goes into the first free input of every other selected node.
    - "First free input" means the lowest-index unconnected non-mask input. A node with no free input is skipped.
    - Add a pure id-based planner beside the Y planners, with gtest cases.
  - verify: the `NodeGraphConnect` gtest cases for both planners pass, and a GUI `u-shift-u` section checks the connections and a one-step undo.
  - size: M
- [ ] M69.P10.T4 — Reproduce the copy/paste error in a GUI test and capture its text
  - files: `Tests/gui/m69_cleanup.py`
  - approach:
    - Add a new `paste-after-create` section that follows the UI flow. Create Blur1 with `CreateNodeArgsPropSettingsOpened` and `CreateNodeArgsPropAutoConnect`, then press Ctrl+C through `graph_key`. Create Blur2 with Blur1 selected, then press Ctrl+V. Repeat with Grade and Constant.
    - A `ModalGuard`, like `PopupGuard` (`:281`), polls `QApplication.activeModalWidget()` and records each `QMessageBox`'s title, text and informativeText, then accepts it.
    - Also record node persistent messages and viewer messages as `NOTE` lines, then check that none appeared.
    - The user is pasting the real error text into `.local/uat-feedback-2026-10-12.md`. Use it to sharpen the check if it has arrived.
  - verify: the section fails on the current build, and its NOTE lines give the error.
  - size: M
- [ ] M69.P10.T5 — Fix the copy/paste error
  - files: TBD from the error text (suspects: P2.T4's splice in `NodeGraph40.cpp:191-283` / `NodeGraph05.cpp:54`, which connects outside the undo commands; P8's `Node::loadKnob` filter removal; `NodeGraphPrivate10.cpp:118-131`)
  - approach: TBD once P10.T4 or the user's pasted text gives the error. Plan this task's brief then.
  - verify: P10.T4's section passes.
  - size: M
- [ ] M69.P10.T6 — Open the pane menu from a right-click anywhere in the tab header
  - files: `Gui/TabWidget.h`, `Gui/TabWidget.cpp`, `Tests/gui/m69_cleanup.py`
  - approach:
    - This is a requirement change from the user. A right-click on a tab, on the empty header space, on the left corner button or on the float/close buttons all open the pane menu at the cursor.
    - In `TabWidgetHeader::contextMenuEvent` (`TabWidget.h:149-157`), drop the `childAt` filter and always emit `paneMenuRequested(globalPos)`.
    - Remove the corner button's `setContextMenuPolicy(Qt::CustomContextMenu)` (`TabWidget.cpp:314`), which swallows the event.
    - A right-click on a tab makes that tab current first, because "Float pane" acts on the current widget.
  - verify: the GUI `pane-header-menu` section (`:865-920`) checks that a right-click on a tab (it also becomes current), on the corner button and on the float button each opens the pane menu at the cursor.
  - size: M
- [ ] M69.P10.T7 — Keep a broken roto tangent broken until Smooth is applied
  - files: `Engine/RotoUndoCommand.cpp`, `Engine/BezierCP.cpp`, `Engine/Bezier.cpp`, `Tests/gui/m69_cleanup.py`
  - approach:
    - In `dragTangent` (`RotoUndoCommand.cpp:838-866`), use `const bool broken = breakTangents || cp.getBroken();` for both `getDeltaForPoint` calls and `movePointLeftAndRightIndex`. Then `Bezier::moveBezierPointInternal`'s `setBroken(...)` (`Bezier.cpp:1900-1996`) no longer un-breaks on a plain drag.
    - Copy `_imp->broken` in `BezierCP::clone` (`BezierCP.cpp:899-931`) so undo restores the flag.
    - The explicit un-break is the existing Smooth action (user decision): `Bezier::smoothOrCuspPointAtIndex` (`Bezier.cpp:2279-2335`) already calls `setBroken(false)` when smoothing.
  - verify: a GUI roto section that uses `getControlPointPosition` (`PyRoto.h:134`):
    - Ctrl-drag a right tangent, then plain-drag it. The left tangent doesn't move.
    - Smooth, then a plain drag moves both tangents.
    - Undo of the Ctrl-drag, then a plain drag moves both tangents.
  - size: M
- [ ] M69.P10.T8 — Draw a dragged curve tangent at its normal length; drop the follow-the-cursor stand-in
  - files: `Gui/CurveWidgetPrivate.cpp`, `Gui/CurveWidgetPrivate.h`, `Gui/CurveWidget.cpp`, `Tests/gui/m69_cleanup.py`
  - approach:
    - Delete `_tangentDragCursorPos`: the member (`CurveWidgetPrivate.h:178`), its initializer (`.cpp:98`), its assignment in `moveSelectedTangent` (`.cpp:1465-1468`), the override block in `refreshKeyTangents` (`.cpp:1547-1557`) and the snap-back in `CurveWidget::mouseReleaseEvent` (`CurveWidget.cpp:1246-1251`).
    - Keep `_tangentDragForceBreak`, the `forceBreak` argument and Join.
    - True handle length is M96 - Weighted Curve Tangents (user decision).
    - **REBASE (M56):** M56.P9.T2 edits the colour code in the same files.
  - verify: the GUI `curve-join` (`:924`) slope checks pass. During a long Ctrl-drag, nothing is drawn near the cursor beyond the handle's length.
  - size: M
- [ ] M69.P10.T9 — Show one curve for a folded multi-component knob, and refresh on fold/expand
  - files: `Gui/KnobGui.h`, `Gui/KnobGuiValue.cpp`, `Gui/CurveEditor.cpp`, `Gui/CurveEditor.h`, `Tests/gui/m69_cleanup.py`
  - approach:
    - `createElementsForKnob` (`CurveEditor.cpp:400-497`) makes one curve per dimension.
    - Add the signal `KnobGui::dimensionsFoldedChanged(bool)` and emit it from `KnobGuiValue::foldAllDimensions`/`expandAllDimensions` (`:946-990`, including the auto-fold at `:1166-1170`).
    - While the knob is folded (`getAllDimensionsVisible()`, `KnobGuiValue.cpp:856`), the knob's curve-editor context hides dimensions 1..n and labels dimension 0 with the knob's label.
    - A "hidden by fold" flag on `NodeCurveEditorElement` must be respected by `setVisible`/`checkVisibleState` (`:599-681`), so a later `keyFrameSet` doesn't re-show those dimensions.
    - If the fold button doesn't exist before the panel opens, fall back to "auto-fold enabled and every dimension's curve equal".
  - verify: in the GUI, key a folded Grade `gain`, and the curve-editor tree shows one curve. Expand it and four appear; fold it and one shows again.
  - size: M
- [ ] M69.P10.T10 — Make edits on a folded curve apply to every dimension, as one undo step
  - files: `Gui/CurveWidget.cpp`, `Gui/CurveEditorUndoRedo.h`, `Gui/CurveEditorUndoRedo.cpp`, `Tests/gui/m69_cleanup.py`
  - approach:
    - Wrap the command in `CurveWidget::pushUndoCommand` when its curve is a folded dimension 0. The wrapper is a `FoldedCurveEditCommand(inner, knob)`:
      - redo runs the inner command, then `knob->cloneCurve(view, d, *curve0)` for d = 1..n;
      - undo restores the snapshots of dimensions 1..n, then runs the inner undo;
      - `id()`/`mergeWith` pass through to the inner command, so a drag stays one step.
    - User decision: overwrite every channel with the edited curve. Referencing instead of copying is a follow-up the user raised; it is out of this milestone.
  - verify: in the GUI, with `gain` folded and keyed, drag a key, set Broken and Join. Every dimension then equals dimension 0 at each key, and one undo reverts all four.
  - size: M
- [ ] M69.P10.T11 — Rebase, end build, scripts and the round-3 UAT AppImage
  - files: `build/appimages/M69-<sha>.AppImage`, `build/appimages/M69-uat.md`
  - approach:
    - Rebase onto `main` after M45 and M56 merge.
    - Run one `tools/ci/local/build.sh fast`, then ctest and the gate's GUI scripts.
    - Run `tools/ci/local/package.sh fast`. The UAT script covers every Phase 69.10 item.
  - verify: the gate is green, and the user signs off on the AppImage.
  - size: L

**Verification gate:**
- One `fast` build (`tools/ci/local/build.sh fast`), with CI `format`, `lint-ci` and `build-and-test` green.
- ctest green, including the new `NodeColorSampler`, `RotoFeatherDrag` and `CurveTangentJoin` cases and the new `LayerKnobsRender` UV-layer case. The existing `RotoLayer`, `NodeGraphSplice`, `RenderDialogRange`, `LayerKnobsRender` and `ColorViewsRender` must also pass.
- `m69_cleanup.py`, `m55_node_graph.py`, `m55_input_pipes.py` and `m56_visual.py` report 0 failures under `Tests/gui/run-gui-test.sh`. The user approves the m69 shots.
- The user signs off on `build/appimages/M69-<sha>.AppImage` through the UAT script.

Acceptance sketch:
- Deleting a node whose panel was focused, then undoing in another panel, does not crash.
- Pasting onto a selected node connects it; unconnected inputs are easy to grab.
- Pick-up mode connects an input with a click, pans/zooms while held, and cancels on right-click/Escape.
- Right-clicking the empty space beside a pane's tabs opens the same menu as the panel management icon, at the cursor.
- The colour picker returns scene-referred values. Ctrl-click reads the viewed node's input; Ctrl+Alt-click reads its output.
- Several selected roto points' feathers move together when one feather bar is dragged.
- Curve-editor handles can be broken and re-joined.
- IDistort and STMap pick their UV layer from a dropdown, and U/V/Alpha follow it.
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
- 2026-10-12 — **Freshness check, P4.T3 rewrite, IDistort task, P8 split (planning consultant):** the M69 branch was re-checked at `6fcf9c4fa` (main + M84). M84 shares only `Engine/Settings.cpp` and `Tests/CMakeLists.txt` with M69, in other places.
  - **Line references:** these were stale and are fixed: `NodeGraph20.cpp` drag path (`:762` → `:461`), `NodeGraph15.cpp` release block, the `Settings.*` linear-picker lines, `KnobGuiColor.cpp`'s tooltip (`:843` → `:866`), `ViewerInstance.cpp:2961`, `NodeGui.cpp` `destroyGui` (`:2984` → `:2898`), and P8's `KnobSerialization.cpp` ranges (`~:612-650` → `:301-659,740-780`). Every other file and line still holds. Every task that shares a file with M56 or M45 is tagged **REBASE**.
  - **Overlaps beyond the "started early" note:**
    - M56 also edits `GuiPrivate.cpp` (P1.T1), `TabWidget.cpp` (P3.T1) and `KnobGuiColor.cpp` (P4.T1). It edits `RotoPaint.cpp` (P5.T2), but only icons.
    - M56 also edits `CurveWidget.cpp`, `CurveWidgetPrivate.cpp`, `ActionShortcuts.h` and `GuiApplicationManager10.cpp`. It adds a "Free Interpolation" action beside Break, exactly where P6.T2 adds Join.
    - M45 also edits `NodeGraphPrivate.h` (P2.T2, P2.T4), `GuiApplicationManager10.cpp`, `Settings.*` and `Node.*` (P8, P9).
    - Both add to `Tests/CMakeLists.txt`.
    - The P4.T3 rewrite removes the `ViewerTab.cpp` overlap.
  - **P4.T3:** rewritten to the user's modifier design (no toolbar button, no `ViewerTab.cpp` edit). The scout found the Ctrl+Alt branch (`eMouseStatePickingInputColor`, `pickColor(..., true)`) and the `#define` already live in `ViewerGL.cpp`/`ActionShortcuts.h`. Only the registration was commented out, so the task renames and inverts the existing plumbing rather than adding a mode. `getPreferredInput()` already prefers B on a connected Merge (the A preference applies only to auto-connect), which matches the user's "B on Merge".
  - **P9.T1 (new):** the IDistort/STMap pickers are flat `KnobChannelSelect` twins bound to the UV input. They already list every UV layer when one is connected (covered by `LayerKnobsRender_Test`). M56's shot used an unconnected node, so it showed only the colour views. The fix adds the missing layer dropdown as a host `KnobLayerSelect` that filters the U/V pickers. It is written on top of M56's row-layout changes to `takeOverMultiplaneChoices`.
  - **P8.T1 split:** deleting the `m65-legacy-color.ntp` fixture, as P8.T1 asked, deletes all four `LegacyColorProjectTest` cases. Those cases are the only coverage of M65's legacy colour-layer reset, and the pre-v17 channel-set gate has its own legacy fixture. P8.T1 keeps the compat filters (M). The new P8.T2 removes that machinery and both fixtures (L, pending Q3). On `main` the Write half of the legacy test is still present, because M76 has not merged.
  - **Open questions:**
    - **Q1:** should the UV Layer dropdown filter Alpha too? Recommended: no. It filters U and V only, and Alpha keeps every layer of the UV input, because the UV alpha often comes from `rgba.A`.
    - **Q2:** should STMap get the same dropdown? Recommended: yes. It has the same params from the same openfx-misc source.
    - **Q3:** should P8.T2 remove the legacy colour-layer reset/warning and the pre-v17 channel-set gate? Recommended: yes, under the no-backward-compatibility rule.
    - **Q4:** should Ctrl+Shift+Alt rect-pick read the output? Recommended: no. Rect and hover always read the input, as decided.
- 2026-10-12 — **User answers to the freshness-check questions:**
  - **IDistort/STMap UV layer:** cancelled (P9.T1). The user dropped the alpha-channel feature and wants both rewritten as native nodes in the new **M113 - Native Node Rewrites Continued**.
  - **Legacy removal:** remove both the M65 colour-layer reset and the pre-v17 channel-set gate, with their tests and fixtures (P8.T1 and P8.T2 as written).
  - **Rect pick output:** yes. Ctrl+Shift+Alt drag reads the output; plain Ctrl+Shift drag reads the input. P4.T3 includes this. Hover stays input-only.
- 2026-10-12 — **P2.T4:** the paste splices only when exactly one non-backdrop node is selected, and leaves it unconnected when the target is a Viewer or Write or the pasted entry is a generator. For a chain, outputs are re-routed to the pasted *exit*, because `moveNodesForIdealPosition` routes to the entry. Undo diffs the recorded inputs through a new `ReconnectInputsCommand`. The Python `app.pasteNodes()` path splices too.
- 2026-10-12 — **P8:** removed 1,932 lines. Beyond the brief, `markPersistentMessageFromProjectLoad` also went, because only the deleted warning used it. Still in place: the pre-2.2.3 `"<name>Choice"` string-param branch in `Node::loadKnob` (with only its filter call removed), and the flags `NODE_SERIALIZATION_ALWAYS_SAVES_CHANNEL_SET` and `KNOB_SERIALIZATION_CHANGE_LAYERS_SERIALIZATION`, which other definitions still use. Under the no-backward-compatibility rule, the pre-2.2.3 branch is removed in the P7.T1 end build.
- 2026-10-12 — **P7.T1 (end build):** a cold build hit one compile error (missing `<QContextMenuEvent>` include). ctest 1944/1944. `m69_cleanup.py` 151 PASS / 0 FAIL across 10 sections; `m55_node_graph.py` and `m55_input_pipes.py` 0 FAIL. Product bugs fixed: the stub grab broad-phase reached only ±9 px, and the curve editor did not repaint after Join. The pre-2.2.3 Choice branch was removed. Shots are in `.worktrees/m69/build/m69-shots/out/`.
- 2026-10-12 — **Screenshots approved (user)** (claude.ai artifact `L7vbjS2AiRqXC1aDrqnqnR`). The UAT AppImage is packaged once the build slot frees.
- 2026-10-12 — **UAT AppImage:** `build/appimages/M69-a2e90be96.AppImage` (includes the bundled Vulkan loader), script `build/appimages/M69-uat.md`.
- 2026-10-12 — **UAT round 2 (user, `.local/uat-feedback-2026-10-12.md`), answers and decisions:**
  - **Pick-up:** a click on anything that isn't a node disconnects the held input.
  - **Y and Shift+Y:** Y anchors on the first-selected node, whose inputs change. Shift+Y makes the same connection with the selection order reversed.
  - **U and Shift+U:** Nuke-style shortcuts added. U pipes the first-selected node's output into the first free input of each later node. Shift+U pipes the last-selected node's output into the others.
  - **Pane menu:** right-clicking anywhere in the tab header opens it. This is a requirement change.
  - **Roto:** a broken tangent stays broken until Smooth.
  - **Curve handle length:** deferred to M96 - Weighted Curve Tangents, through an inbox entry. M69 drops its follow-the-cursor stand-in.
  - **Folded knob:** a folded multi-component knob shows one curve, and editing it overwrites every channel. Referencing instead of copying is a follow-up, through an inbox entry.
  - These became Phase 69.10.
