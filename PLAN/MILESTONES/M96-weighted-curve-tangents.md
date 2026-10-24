# M96 - Weighted Curve Tangents

Add weights (handle length) to keyframe tangents, so that dragging a curve-editor handle lengthens or shortens it and changes the curve. Today the model is slope only: `KeyFrame` (`Engine/Curve.h:50-112`) holds two derivatives, and `Interpolation::interpolate/derive/integrate*` (`Engine/Interpolation.h:45-89`) evaluate a Hermite cubic in time. Handle length exists only on screen: `CurveWidgetPrivate::refreshKeyTangents` (`Gui/CurveWidgetPrivate.cpp:1474-1546`) derives it from the zoom (1/3 of the span, clamped to 1/8 of the widget).

This milestone gives each tangent a weight: the handle's time extent as a fraction of the adjacent segment. A segment whose two inner handles are both weighted evaluates as a 2D Bezier in (time, value). Any other segment keeps today's Hermite code path, so it evaluates bit-for-bit as before. On top of that model the milestone builds the curve-editor drag, undo, the M16 YAML codec, the Python API, and the curve-editor HiDPI hit-test fix. Deps: M93 (done) and M16 - Project Format Redesign, because the serialization task targets M16's `Serialization/Yaml/CurveYaml.cpp`, not the XML adapters M16 deletes. M172 - GL Widget Context Loss waits on this milestone and edits the same `Gui/CurveWidget*.cpp`, so M96 leaves `initializeGL` and the `TextRenderer` alone.

## Scope

**In:**
- `KeyFrame` gains a per-key `weighted` flag plus `leftWeight`/`rightWeight`: the fraction of the adjacent segment's time span, default exactly `1/3`, clamped to `[kMinTangentWeight, kMaxTangentWeight]` (0.01 to 0.99, which keeps time monotonic in every segment). Only Free (`F`) and Broken (`X`) keys can be weighted.
- Weighted evaluation for value, derivative, integral and clamped integral (`Engine/Interpolation.*`), wired into `Curve::getValueAt/getDerivativeAt/getIntegrateFromTo` (`Engine/Curve.cpp:946/1032/1094`) and into `CurveSnapshot::valueAt` (`Engine/Nodes/Image/CurveSnapshot.h:93-137`).
- Weights included in `KeyFrame::operator==`, `Curve::appendContentHash` (`Engine/Curve.cpp:383-401`) and the knob key-equality checks (`Engine/KnobImpl.h:1576`, :2926).
- A knob-level tangent setter that carries weights, replacing `moveDerivativesAtTime`/`moveDerivativeAtTime` (`Engine/Knob.h:668-669`) (clean break).
- M16 YAML curve tokens for weighted keys, `docs/project-format.md`, and the clipboard, which goes through the same codec.
- Python: per-key tangent get/set on `AnimatedParam`, and weights on `ParametricParam` control points.
- Curve editor: weighted handles drawn at their true control points; a drag that sets slope and length; unbroken coupling; a "Weighted tangents" toggle action; undo.
- The curve-editor and dope-sheet HiDPI hit-test at `QT_SCALE_FACTOR=2`.

**Out:**
- GPU evaluation of weighted segments in `Engine/Gpu/Kernels/curvesnapshot.slang`. Weighted parametric curves fall back to CPU (follow-up via the inbox).
- Auto-tangent types (Smooth, Catmull-Rom, Cubic, Horizontal) computing their derivatives with weighted neighbours taken into account. `Interpolation::autoComputeDerivatives` stays as it is.
- Numeric weight entry in `EditKeyFrameDialog` (`Gui/CurveWidgetDialogs.cpp:416-560`).
- Roto `BezierCP` handles, a separate model covered in M69.
- Dope-sheet UI changes beyond the HiDPI fix. Weights ride along in `KeyFrame` copies.
- GL context-loss handling in `CurveWidget` (M172).

**Edge cases:**
- Unweighted segment: the Hermite path runs unchanged, with exact equality to today's values. A segment where only one side is weighted takes the Bezier path, with `1/3` on the unweighted side.
- Constant and Linear segments, and extrapolation before the first key or after the last (`interp == eKeyframeTypeNone`): weights are ignored. The outer handle of the first and last key is never weighted; a drag on it sets slope only.
- Periodic curves: the wrap segment uses last.right and first.left (`Gui/CurveGui.cpp:150-178` and the Curve equivalent).
- Changing a key to a non-F/X interpolation clears `weighted` and resets both weights to `1/3`; undo restores them. Turning on "Weighted tangents" for a key with an auto type first converts it to Free, keeping its current slopes.
- Moving or scaling keys in time (curve editor, dope sheet, retime): fractions keep the segment's shape, scaled. Inserting or removing a key changes the span that a neighbour's fraction refers to; the fraction is kept and the shape between the keys changes, as it does today for slopes.
- With weights at the maximum on both sides, time stays strictly monotonic (the Bernstein coefficients of `x'(s)` stay positive when a, b < 1), so the derivative is finite.
- NaN or out-of-range weights from Python or from a file are clamped, or rejected with an error naming the key. Nothing asserts.
- Weighted F/X keys on roto and tracker curves stay inline in M16's codec, because the blob tabulation already refuses keys with stored derivatives (`Serialization/Yaml/RotoYaml.cpp:268-294` on M16).
- A parametric curve with any weighted segment makes `CurveSnapshot::packGpu` return false, so ColorLookup and ColorCorrect render on the CPU (`Engine/Nodes/Color/ColorLookup.cpp:385`, `ColorCorrect.cpp:299`).

**Assumptions:**
- Qt 6 `QOpenGLWidget::resizeGL` receives logical sizes, as `ViewerGL::resizeGL` asserts (`Gui/ViewerGL.cpp:198`). `CurveWidget::resizeGL` divides them by the DPR again (`Gui/CurveWidget.cpp:604-612`), which would give the zoom context half the widget at 2× and cause the reported miss. Not verified; P3.T1 confirms it first. `DopeSheetView.cpp:3249-3256` has the same pattern.
- M16's codec and `KeyFrameSerialization` (`Serialization/CurveSerialization.h:38-56`) are unchanged at merge from branch HEAD `0d58f66ef`.
- `Interpolation::solveCubic` (`Engine/Interpolation.cpp:199`) plus one or two Newton steps is accurate to about 1e-12 in time over [0,1]. If not, bracketed Newton replaces it.

## Phase 96.1: Engine model and evaluation

- [ ] M96.P1.T1 — Add weighted Bezier segment evaluation to `Interpolation`
  - files: `Engine/Interpolation.h`, `Engine/Interpolation.cpp`, `Tests/WeightedInterpolation_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Add `kDefaultTangentWeight = 1./3`, `kMinTangentWeight` and `kMaxTangentWeight`. Give `interpolate`, `derive`, `derive_clamp`, `integrate` and `integrate_clamp` two extra parameters, `vcurWeightRight` and `vnextWeightLeft`. When both equal `kDefaultTangentWeight`, or the segment is Constant or None, run today's Hermite code untouched. Otherwise use control points in u = (t-tcur)/span: X = {0, a, 1-b, 1} and Y = {P0, P0+slopeR·a·span, P3−slopeL·b·span, P3}. Solve X(s)=u with `solveCubic`, keeping the root in [0,1] and polishing it with Newton. Value is Y(s). Derivative is Y'(s)/(X'(s)·span). The integral is span·∫Y(s)X'(s)ds, a degree-5 polynomial integrated exactly. The clamped integral splits at the s where Y crosses vmin/vmax (roots of a cubic), and uses clamp·ΔX on the clamped pieces.
  - verify: five gtests. (1) Weights 1/3 forced through the Bezier path match the Hermite path within 1e-12 for random segments. (2) Unweighted results are bitwise equal to a reference copy of today's Hermite formula pasted into the test, for every `KeyframeTypeEnum` pair. (3) Value and integral are continuous at segment ends. (4) The derivative matches a central finite difference. (5) At a=b=0.99 time is monotonic and the derivative is finite. Compiled at the phase build.
  - size: L

- [ ] M96.P1.T2 — Add weights to `KeyFrame` and wire them through `Curve`
  - files: `Engine/Curve.h`, `Engine/Curve.cpp`, `Engine/CurvePrivate.h`, `Tests/Curve_Test.cpp`
  - approach: Add `_weighted`, `_leftWeight` and `_rightWeight` with accessors, clamping and NaN rejection, and include them in `operator==`, copy and assignment. Pass each key's weights (or 1/3 when unweighted) at every `Interpolation::` call in `getValueAt`/`getDerivativeAt`/`getIntegrateFromTo` (:998-1194). Add `Curve::setKeyFrameTangents(left, right, leftWeight, rightWeight, weighted, index, newIndex)`, replacing `setKeyFrameDerivatives`. `setKeyframeInterpolation_internal` (:1598) resets weights when the type leaves F/X. Hash the weights and the flag in `appendContentHash`. `to/fromSerialization` stays as it is until P2.T1.
  - verify: new `Curve_Test` cases: a weighted Free key changes mid-segment values while values at the keys stay put; switching it to Smooth resets the weights; `clone` and `operator=` keep the weights; content hashes differ only through the weight. Compiled at the phase build.
  - size: M

- [ ] M96.P1.T3 — Make `CurveSnapshot` honour weights and fall back from GPU on weighted segments
  - files: `Engine/Nodes/Image/CurveSnapshot.h`, `Tests/Native/NativeColorLookup_Test.cpp`, `Tests/Native/NativeColorLookupGpu_Test.cpp`
  - approach: `valueAt` (:93-137) passes weights exactly as `Curve::getValueAt` does. `packSegment` (:237+) returns false for any segment that would take the Bezier path, so `packGpu` fails and the callers clear `gpuCurves`. Update the class comment.
  - verify: CPU test: a ColorLookup with one weighted key renders equal to `Curve::getValueAt` sampled per pixel. GPU test: the same curve packs false and the render still matches the CPU. Unweighted GPU tests unchanged. Compiled at the phase build.
  - size: M

- [ ] M96.P1.T4 — Replace the knob derivative setters with a tangent setter that carries weights
  - files: `Engine/Knob.h`, `Engine/Knob.cpp`, `Engine/KnobImpl.h`, `Engine/KnobTypes.h`, `Engine/KnobTypes.cpp`
  - approach: Add `struct KeyTangents { double left, right, leftWeight, rightWeight; bool weighted; }` and `KnobI::setTangentsAtTime(reason, view, dim, time, const KeyTangents&, KeyFrame* newKey)`, replacing `moveDerivativesAtTime`/`moveDerivativeAtTime` (`Knob.h:668-669`, `Knob.cpp:1518-1575`) and fixing every caller. Weights go into the key-equality checks at `KnobImpl.h:1576` and :2926. `KnobParametric::addControlPoint`/`get/setNthControlPoint` (`KnobTypes.cpp:2089-2253`) gain weight overloads.
  - verify: gtest: a Double knob with three keys gets a weighted right tangent through `setTangentsAtTime`; the value at the segment midpoint changes, `curveAnimationChanged` fires, and the node hash changes. Phase build `tools/ci/remote/build.sh debug --tests`; all of ctest green.
  - size: M

## Phase 96.2: Serialization and Python (rebase on main after M16 merges)

- [ ] M96.P2.T1 — Serialize weights in M16's YAML curve codec
  - files: `Serialization/CurveSerialization.h`, `Engine/Curve.cpp`, `Serialization/Yaml/CurveYaml.cpp`, `docs/project-format.md`, `Tests/Serialization/CurveYaml_Test.cpp`
  - approach: `KeyFrameSerialization` gains `weighted`, `leftWeight` and `rightWeight`, defaults false, 1/3 and 1/3. Wire them in `Curve::to/fromSerialization` (:350-381). Add two weighted tokens: `FW` writes time, value, slope, then rightWeight and leftWeight; `XW` writes time, value, right, left, then rightWeight and leftWeight. Teach the decoder states and `expectedAt` about them. Unweighted keys write exactly today's bytes. `canonicalCurveForYaml` resets the weights of unweighted or non-F/X keys. Out-of-range or NaN weights are a decode error naming the element. Document the tokens under `## Curves`. formatVersion stays 1 (see Decisions).
  - verify: `CurveYaml_Test` round trip is bit-exact for FW/XW/mixed curves; an unweighted curve encodes byte-identically to the pre-change encoder (golden string); truncated FW/XW input gives a "curve ends where it expects a ... weight" error. M16's project round-trip and clipboard tests stay green with a weighted key added to their graph. Compiled at the phase build.
  - size: M

- [ ] M96.P2.T2 — Expose tangent weights in the Python API
  - files: `Engine/PyParameter.h`, `Engine/PyParameter.cpp`, `Engine/typesystem_engine.xml`, `Tests/PyKeyTangents_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Add `AnimatedParam.getKeyTangents(time, dimension=0)` returning `(interpolation, left, right, leftWeight, rightWeight, weighted)`, and `setKeyTangents(time, left, right, leftWeight=1/3, rightWeight=1/3, weighted=False, dimension=0)`, routed through `KnobI::setTangentsAtTime` so undo and signals behave. `ParametricParam.getNthControlPoint` returns weights too (tuple changed in place, clean break); check the bundled PyPlugs (`Gui/Resources/PyPlugs/{ZMask,LightWrap,Glow}.py`) still work. `setNthControlPoint` and `addControlPoint` take optional weights. Bad times raise `ValueError` rather than returning null.
  - verify: gtest driving Python as `Tests/PyPlugInstantiate_Test.cpp` does: set a weighted tangent, read it back, check `getValueAt` mid-segment, `ValueError` for a missing key. `PyPlugInstantiate_Test` passes for ZMask, LightWrap and Glow. Phase build `tools/ci/remote/build.sh debug --tests`; all green.
  - size: M

## Phase 96.3: Curve editor

- [ ] M96.P3.T1 — Fix the curve-editor and dope-sheet hit-test at device pixel ratio 2
  - files: `Gui/CurveWidget.cpp`, `Gui/CurveWidgetPrivate.cpp`, `Gui/DopeSheetView.cpp`
  - approach: Confirm the assumption first: log `resizeGL`'s w/h against `width()`/`height()` at `QT_SCALE_FACTOR=2`. If they are logical, stop dividing by the DPR when setting `zoomCtx.setScreenSize` (:604-612). Keep `glViewport` and the projection in device pixels (`w*dpr`), and audit `isNearbyCurve/KeyFrame/Tangent` (:654-867) and the `e->pos()`/`e->position()` mapping (:1340) so all of them use one logical space. Same fix in `DopeSheetView.cpp:3249-3256`. Do not touch `initializeGL` (M172).
  - verify: P3.T5's `hidpi-pick` section passes at `QT_SCALE_FACTOR=2` and at 1×; m56's 2× curve-editor shot unchanged.
  - size: M

- [ ] M96.P3.T2 — Draw weighted handles at their control points, and sample weighted segments correctly
  - files: `Gui/CurveWidgetPrivate.cpp`, `Gui/CurveGui.cpp`, `Gui/CurveGui.h`
  - approach: In `refreshKeyTangents`, place a weighted key's handles at (t ± w·span, v ± slope·w·span), with no zoom clamp. Unweighted keys and outer handles keep today's clamped drawing. In `CurveGui`'s adaptive step (:150-215) the Hermite second-derivative estimate is wrong for weighted segments: bound the step from the Bezier's control polygon, or use a fixed 2-px step, and evaluate through `Curve::getValueAt`.
  - verify: P3.T5's `weighted-draw` section: the drawn curve passes within 1 px of `getValueAt` samples, and the handle tip lies on the true control point.
  - size: M

- [ ] M96.P3.T3 — Make a handle drag set weight and slope, with unbroken coupling and undo
  - files: `Gui/CurveEditorUndoRedo.h`, `Gui/CurveEditorUndoRedo.cpp`, `Gui/CurveWidgetPrivate.cpp`, `Tests/CurveWeightedTangent_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: `moveSelectedTangent` (:1455) passes the cursor point. On a weighted key, `MoveTangentCommand` (:666-870) computes slope = dy/dx and weight = |dx|/span, clamped. Free (unbroken): the other side takes the same slope, and its weight scales by the same factor as the dragged one. Broken or Ctrl-drag: the other side is untouched. On an unweighted key the drag sets slope only, as today. Store and restore the old weights and flag; `mergeWith` keeps the first old state. Both the knob and the parametric branch go through the P1 setters. `JoinTangentsCommand` (:876+) keeps the weights.
  - verify: five gtests in the style of `CurveTangentJoin_Test`: a weighted Free drag doubles the right weight and the left weight doubles too; Broken changes one side only; undo restores the bits; merged drags undo in one step; an unweighted drag leaves the weights at exactly 1/3.
  - size: L

- [ ] M96.P3.T4 — Add an undoable "Weighted tangents" toggle to the curve editor
  - files: `Gui/CurveWidgetPrivate.cpp`, `Gui/CurveWidget.h`, `Gui/CurveWidget.cpp`, `Gui/ActionShortcuts.h`, `Gui/CurveEditorUndoRedo.cpp`
  - approach: A checkable action in the interpolation menu next to Free and Join (:256-264), shortcut ID `weighted`, handled in `keyPressEvent` (:1635). A `SetKeysWeightedCommand` turns the flag on (converting auto types to Free and keeping slopes) or off (resetting to 1/3), for all selected keys, undoable.
  - verify: P3.T5's `toggle` section; the P3.T3 gtest extended with a toggle-and-undo case.
  - size: M

- [ ] M96.P3.T5 — Add a GUI script for weighted tangents
  - files: `Tests/gui/m96_weighted_tangents.py` (new)
  - approach: Follow `Tests/gui/m69_cleanup.py`'s `curve-join` section (:1383-1510), `guitest` helpers, sections selectable with `M96_SECTIONS`. Graph: a Grade with three keys on `white.r`. Sections: `toggle` (flag on/off with undo); `weighted-drag` (dragging a weighted Free handle outward changes the knob value at the segment midpoint, read through Python); `unbroken` (opposite handle's on-screen length scales); `broken` (opposite handle unchanged); `release` (handle extent the same while held and after release, equal to the cursor distance); `unweighted` (a drag changes slope only and the m69 length check still holds); `save-load` (save as `.ntpz`, reload, values and weights equal); `undo`; `weighted-draw`; `hidpi-pick` (with `QT_SCALE_FACTOR=2`, a click on the curve selects it). Shots `m96-<section>.png`.
  - verify: phase build `tools/ci/remote/build.sh fast --tests`, then `Tests/gui/run-gui-test.sh Tests/gui/m96_weighted_tangents.py <AppImage>` at 1× and at `QT_SCALE_FACTOR=2`; every section passes, and `m69_cleanup.py` `curve-join` passes.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests` is green, including the new Interpolation, Curve, CurveSnapshot, Python and command tests. On a packaged AppImage, `Tests/gui/m96_weighted_tangents.py` passes every section at 1× and at `QT_SCALE_FACTOR=2`, and `m69_cleanup.py` (`curve-join`) and `m56_visual.py`'s 2× curve-editor shot still pass. The GUI bench runs because `Engine/Knob*` is touched (`tools/bench/gui/run.sh --xvfb` + `compare.py`), with the result recorded below. The user has approved the screenshots.

## Decisions

- 2026-10-24 — **Elaborated from the stub (PM, opus consultant); PM calls on the consultant's open points.** Weight unit: a fraction of the adjacent span, clamped 0.01–0.99 (survives retimes and key moves, keeps time monotonic). Unbroken coupling: the other handle scales by the same factor, keeping the ratio. Format: stay at formatVersion 1, since an old reader refuses `FW`/`XW` rather than misreading them, which M16's versioning rule allows. Parametric curves with weighted segments fall back to CPU; GPU evaluation is a follow-up. The dope sheet's identical DPR division is fixed in P3.T1. Deps become M93, M16: P2 targets M16's YAML codec, so the lane rebases onto main after M16 merges and before P2.
- 2026-10-13 — Handle length belongs here, not in M69 - Minor Cleanup: the user decided at the M69 UAT (2026-10-12) that M69 drops its visual-only follow-the-cursor handle drawing instead of implementing length.
- 2026-10-13 — The curve-editor HiDPI hit-test bug lives here (user, from an inbox report): this milestone already reworks curve-editor picking and handles.
