# M261 - Roto Keyframes In Dope Sheet And Curve Editor

Filed from M163 - Incremental Bezier Edits UAT (user, 2026-10-27), a pre-existing issue. The dope sheet never shows curve hosts: `DopeSheet::addNode` skips Roto/RotoPaint (Gui/DopeSheet.cpp:382, upstream since 2016), and its model only knows knobs that have a `KnobGui`; Bezier shape keys live on the control points (`Bezier::getKeyframeTimes`, Engine/Bezier.cpp:3453), not on knobs. The curve editor shows a host's shapes but drops the host's own knob curves (Gui/CurveEditor.cpp:347-354 builds either a `NodeCurveEditorContext` or a `RotoCurveEditorContext`, never both), and plots each shape's "Animation" track as a ramp of keyframe indices (`BezierCPCurveGui`, Gui/CurveGui.cpp:766-823). M146 only renamed accessors here. This milestone gives both editors one row per shape holding its keys, selectable and movable, plus the node's own curves.

Deps: M163 - Incremental Bezier Edits.

## Scope

**In:**
- Dope sheet: every node with a curve context (Roto, RotoPaint, any geometry host) gets a node row with its own animatable knobs (excluding the per-item proxy knobs). Under it, one row per Bezier (shape keys) and child rows for that item's animated knobs.
- Dope sheet shape keys: select, move, delete and change interpolation, all with undo.
- Curve editor: the host node's own knob curves come back next to its items. The shape track is shown as keys on a flat line with no fake values, named "Shape" and hidden while the shape has no keys (pending the user's OK, see P3.T2).
- GUI test script that screenshots both editors for user sign-off.

**Out:**
- Paint strokes as keyed rows (strokes have no shape keys; their item knobs still get rows).
- Copy/paste of shape keys between shapes or onto knob rows.
- Layer grouping in either editor (items stay flat in render order).
- Per-control-point curves in the curve editor.

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
  - size: M
- [ ] M261.P2.T2 — Move, delete and interpolation commands for shape keys
  - files: Gui/DopeSheetEditorUndoRedo.cpp/.h
  - approach: `DSMoveKeysAndNodesCommand`, `DSTransformKeysCommand`, `DSRemoveKeysCommand` and `DSSetSelectedKeysInterpolationCommand` branch on `key->bezier`. They use `Bezier::moveKeyframe`/`setKeyframe`/`removeKeyframe`/`setKeyFrameInterpolation`, with the collision refusal from Scope. Paste skips shape keys.
  - size: M

## Phase 261.3: Curve editor

- [ ] M261.P3.T1 — Keep the host node's own knob curves
  - files: Gui/CurveEditor.cpp, Gui/CurveEditor.h
  - approach: `addNode` creates the `NodeCurveEditorContext` for curve hosts too, filtering out `isItemProxyKnob`, and adds the `RotoCurveEditorContext`. `removeNode`, `getSelectedCurves`/`findKnob` handle both contexts.
  - size: M
- [ ] M261.P3.T2 — Shape track as keys on a flat line (user's OK pending)
  - files: Gui/CurveGui.cpp/.h, Gui/CurveWidget.cpp, Gui/CurveWidgetPrivate.cpp, Gui/CurveEditorUndoRedo.cpp, Gui/CurveEditor.cpp
  - approach: `BezierCPCurveGui::evaluate` and `getKeyFrames` return value 0, and `getCurveYRange` returns [0,0]. Ignore the shape track when framing on Y. Drags move it in time only, with the same collision rule. Rename "Animation" to "Shape" and hide it while `getKeyframesCount()==0`. Alternative if the user prefers: drop the shape track from the curve editor and leave shape timing to the dope sheet.
  - size: M

## Phase 261.4: Tests

- [ ] M261.P4.T1 — GUI test with screenshots of both editors
  - files: Tests/gui/m261_roto_keys_editors.py (model on Tests/gui/m146_curve_editor_roto.py)
  - approach: build Roto and RotoPaint, each with two Beziers keyed at 1/10/20, one item opacity key and one node-knob key. Dope sheet: check the node, Shape and knob rows exist and key counts match; select all, delete, undo, checking the Bezier keys from Python. Curve editor: check the node-knob rows exist and the Shape row is flat. Save both screenshots for user sign-off.
  - verify: run-gui-test.sh in natron-dev under Xvfb.
  - size: M

**Verification gate:** one build of all phases, ctest green, m261_roto_keys_editors.py passes under Xvfb, and the user approves the dope sheet and curve editor screenshots from a release AppImage.

## Decisions
