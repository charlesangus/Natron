# M261 - Roto Keyframes In Dope Sheet And Curve Editor

Filed from M163 - Incremental Bezier Edits UAT (user, 2026-10-27), a pre-existing issue. The dope sheet never shows curve hosts: `DopeSheet::addNode` skips Roto/RotoPaint (Gui/DopeSheet.cpp:382, upstream since 2016), and its model only knows knobs that have a `KnobGui`; Bezier shape keys live on the control points (`Bezier::getKeyframeTimes`, Engine/Bezier.cpp:3453), not on knobs. The curve editor shows a host's shapes but drops the host's own knob curves (Gui/CurveEditor.cpp:347-354 builds either a `NodeCurveEditorContext` or a `RotoCurveEditorContext`, never both), and plots each shape's "Animation" track as a ramp of keyframe indices (`BezierCPCurveGui`, Gui/CurveGui.cpp:766-823). M146 only renamed accessors here. This milestone gives both editors one row per shape holding its keys, selectable and movable, plus the node's own curves.

Deps: M163 - Incremental Bezier Edits.

## Scope

**In:**
- Dope sheet: every node with a curve context (Roto, RotoPaint, any geometry host) gets a node row with its own animatable knobs (excluding the per-item proxy knobs). Under it, one row per Bezier (shape keys) and child rows for that item's animated knobs.
- Dope sheet shape keys: select, move, delete and change interpolation, all with undo.
- Curve editor: the host node's own knob curves come back next to its items. Shape keys don't appear in the curve editor (user); instead individual control points can be added to it on request, showing their x/y over time.
- GUI test script that screenshots both editors for user sign-off.

**Out:**
- Paint strokes as keyed rows (strokes have no shape keys; their item knobs still get rows).
- Copy/paste of shape keys between shapes or onto knob rows.
- Layer grouping in either editor (items stay flat in render order).

**Edge cases:**
- Moving a shape key onto an existing shape key: refuse the move for that shape. `Bezier::moveKeyframe` would silently overwrite it.
- Items added, removed or renamed while the editors are open: rows follow `itemInserted`/`itemRemoved`/`itemLabelChanged`.
- Undo after the row was rebuilt: the key is addressed by `BezierWPtr` + time, not by row pointer (same rule as `DopeSheetKey::knob`).
- A shape with no keys: the row stays hidden until it gets one.
- RotoPaint's internal render-tree nodes stay excluded (they have no group, DopeSheet.cpp:356).

**Assumptions:**
- The node knobs held in `RotoCurveContextPrivate::knobs` (cloned from or slaved to the selected item in `selectInternal`, Engine/RotoCurveContext.cpp:945-1016) are exactly the proxies to hide. P1.T2 checks this first.
- Python can read a Bezier's key times, for the test (check the binding first).
- `Bezier` emits `keyframeSet`/`keyframeRemoved` including from `moveKeyframe` (Bezier.cpp:2674-2675), so live refresh hooks those.

## Phase 261.1: Dope sheet model for curve hosts

- [ ] M261.P1.T1 — Rows and keys backed by a bare knob or a Bezier
  - files: Gui/DopeSheet.h, Gui/DopeSheet.cpp, Global/Enums.h
  - approach: let `DSKnob` hold a `KnobIPtr` without a `KnobGui`, or a `BezierPtr`. Add `eDopeSheetItemTypeRotoItem` and `eDopeSheetItemTypeShape`. Give `DSKnob` `getKeyFrames()`/`getInternalKnob()`/`getBezier()` and move the `getKnobGui()` call sites in Gui/DopeSheet*.cpp onto them. Add `BezierWPtr bezier` to `DopeSheetKey`, used in `operator==`.
  - verify: syntax-check; ordinary knob rows behave as before.
  - size: M
- [ ] M261.P1.T2 — Admit curve hosts and build item rows
  - files: Gui/DopeSheet.cpp, Engine/RotoCurveContext.h/.cpp
  - approach: remove the skip at DopeSheet.cpp:382. Add `RotoCurveContext::isItemProxyKnob(const KnobIPtr&)` and skip those knobs in the `DSNode` constructor. When `getCurveContext()` is set, add one item row per drawable, from `getCurvesByRenderOrder`. Each Bezier gets a Shape row plus rows for its animated item knobs. Connect context `itemInserted`/`itemRemoved`/`itemLabelChanged` and Bezier `keyframeSet`/`keyframeRemoved`/`cloned` to rebuild the rows or `onKeyframeSetOrRemoved`.
  - verify: syntax-check; build check in the phase gate.
  - size: M

## Phase 261.2: Dope sheet drawing and edits

- [ ] M261.P2.T1 — Draw, hit-test and select shape and item-knob keys
  - files: Gui/DopeSheetView.cpp, Gui/DopeSheetHierarchyView.cpp
  - approach: `drawKeyframes`/`drawKnobRow` and the selection rectangle read through the T1 accessors. Shape keys use `getKeyframeTimesAndInterpolation` for the texture type. Hide shape rows with no keys and item rows with no animation.
  - verify: Shape and item-knob keys draw at their actual times and can be hit-tested and selected across zoom levels.
  - size: M
- [ ] M261.P2.T2 — Move, delete and interpolation commands for shape keys
  - files: Gui/DopeSheetEditorUndoRedo.cpp/.h
  - approach: `DSMoveKeysAndNodesCommand`, `DSTransformKeysCommand`, `DSRemoveKeysCommand` and `DSSetSelectedKeysInterpolationCommand` branch on `key->bezier`. They use `Bezier::moveKeyframe`/`setKeyframe`/`removeKeyframe`/`setKeyFrameInterpolation`, with the collision refusal from Scope. Paste skips shape keys.
  - verify: Move, delete and interpolation edits change the rendered shape and round-trip through undo/redo.
  - size: M

## Phase 261.3: Curve editor

- [ ] M261.P3.T1 — Keep the host node's own knob curves
  - files: Gui/CurveEditor.cpp, Gui/CurveEditor.h
  - approach: `addNode` creates the `NodeCurveEditorContext` for curve hosts too, filtering out `isItemProxyKnob`, and adds the `RotoCurveEditorContext`. `removeNode`, `getSelectedCurves`/`findKnob` handle both contexts.
  - verify: A RotoPaint host shows its ordinary animated knobs and point curves together without duplicate proxy rows.
  - size: M
- [ ] M261.P3.T2 — No shape track in the curve editor; per-point curves on request
  - files: Gui/CurveEditor.cpp/.h, Gui/CurveGui.cpp/.h, Gui/RotoGui*/viewer roto interact, Engine/Bezier.h/.cpp as needed
  - approach: remove the shape "Animation" track (`BezierCPCurveGui` rows) from the curve editor. Add a "Show in Curve Editor" action for selected control points (viewer right-click and/or roto panel) that adds that point's x and y over time as curves under its shape; editing a key value there moves the point at that key (with undo). Points can be removed from the curve editor again.
  - verify: Adding a point shows editable x/y curves, removing it clears them, and curve edits move the point with undo.
  - size: L

- [ ] M261.P2.T3 — Frame the dope sheet over every clip range
  - files: Gui/DopeSheetView.cpp
  - approach: `DopeSheetView::getKeyframeRange` (around lines 2644-2657) writes each Reader/time node's range into `dimFirstKeys[0]`/`dimLastKeys[0]`, overwriting the previous one, so framing (F / centerOnSelection) fits only the last node's range. Append each range instead. Found by M124's dope-sheet GUI check (2026-10-27); predates M124.
  - verify: framing with two Reads of different ranges shows both bars whole.
  - size: S

## Phase 261.4: Tests

- [ ] M261.P4.T1 — GUI test with screenshots of both editors
  - files: Tests/gui/m261_roto_keys_editors.py (model on Tests/gui/m146_curve_editor_roto.py)
  - approach: build Roto and RotoPaint, each with two Beziers keyed at 1/10/20, one item opacity key and one node-knob key. Dope sheet: check the node, Shape and knob rows exist and key counts match; select all, delete, undo, checking the Bezier keys from Python. Curve editor: check the node-knob rows exist and the Shape row is flat. Save both screenshots for user sign-off.
  - verify: run-gui-test.sh in natron-dev under Xvfb.
  - size: M

**Prior verification gate:** one build of all phases, ctest green, m261_roto_keys_editors.py passes under Xvfb, and the user approves the dope sheet and curve editor screenshots from a release AppImage.

## Decisions

## Decisions

- 2026-10-27 — **Curve editor (user):** overall shape keys don't belong in the curve editor; offer adding individual control points to it instead.

## Phase 261.5: UAT editor correctness

- [ ] M261.P5.T1 — Keep viewer timeline indicators synchronized
  - files: Roto/RotoPaint keyframe notifications, Viewer timeline marks, dope-sheet GUI tests
  - approach: Emit the same keyframe-change signal for shape and mask creation, selection-independent dope-sheet edits and brush keys so blue indicators update immediately.
  - verify: Add and move shape/mask keys with no Roto selection; Viewer indicators update without reopening or reselection.
  - size: M

- [ ] M261.P5.T2 — Make brush-opacity key edits change the value
  - files: RotoPaint brush knob keyframe adapter, dope-sheet model/commands, tests
  - approach: Bind the displayed opacity key to the actual brush opacity curve and preserve value/time through move, undo and redo.
  - verify: Moving and editing a brush-opacity key changes rendered opacity at the expected frames.
  - size: M

- [ ] M261.P5.T3 — Keep overlay handles consistent with interpolation
  - files: Bezier evaluation/cache invalidation, viewer overlay, dope-sheet interpolation commands
  - approach: Invalidate and reevaluate viewer control-point positions whenever interpolation changes or a key is inserted between changed keys.
  - verify: Handles and rendered shape match before and after interpolation changes and inserting an intermediate key.
  - size: M

- [ ] M261.P5.T4 — Correct control-point curves, drawing and hit testing
  - files: `Gui/CurveGui*`, Roto curve adapter, curve-editor GUI tests
  - approach: Map each point coordinate to its real x/y curve, draw correct pre/post-key extrapolation, and use the same transformed coordinates for drawing and clickable control points.
  - verify: Known two-dimensional point motion produces accurate x/y curves; drawn and clickable points coincide across zoom levels.
  - size: L

**Verification gate:** dope-sheet, curve-editor and viewer GUI tests pass; indicators, brush opacity, interpolation and control-point curves are accepted from the replacement AppImage.
