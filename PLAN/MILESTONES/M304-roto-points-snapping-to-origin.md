# M304 - Roto Points Snapping To Origin

Follow-up from the user's 2026-11-01 UAT approval of M261 - Roto Keyframes In Dope Sheet And Curve Editor: roto control points sometimes snap to 0,0. M224 - Fluid Roto And RotoPaint Interaction fixed one cause (edits made during a live render were overwritten by the next copy from the internal curve). The remaining causes are shape-key, undo and save paths that read or write the internal (render) curves while a render holds the node, so edits that exist only in the GUI copy are lost or replaced by empty (0,0) values. Write failing regression tests for each, then fix them.

## Scope

**In:**
- Auto-key during a render (`setKeyframeInternal` and its callers), whole-shape key operations (`setKeyframe`, `removeKeyframe`, `moveKeyframe`, `removeAnimation`, track-unlink bake), M261's point edits at a keyframe, undo snapshots of SmoothCusp/RemoveFeather/TrackLink and `removeFeatherAtIndex`, and serialization during a render.

**Out:**
- Python `setPointAtIndex` doing nothing with auto-key off on an unkeyed shape (a separate bug, not a snap).
- Older project formats (no backward compatibility).

**Edge cases:**
- Feather points; the first point against the others; ripple edits; auto-key on and off.
- A half-written read (x keyed, y not yet) returning an unset or 0 y: check it explicitly, since asserts are inert in these builds.

## Phase 304.1: Failing regression tests

- [ ] M304.P1.T1 — Engine gtests for keying during a render
  - files: `Tests/RotoKeyDuringRender_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: use `RenderingFlagSetter` (`Engine/Node.h`) and the keyed-shape helper as `Tests/BezierIncrementalCopy_Test.cpp` does. With auto-key on at an unkeyed frame: insert a point after index 0, move point 3, `setPointAtIndex` on point 5, then `setKeyframe`, `removeKeyframe`, `moveKeyframe`, `removeAnimation` and a track-unlink bake. After the write-back, check both sides against the expected positions, never (0,0).
  - verify: the tests fail on main a16ff2464 for the auto-key, whole-shape key and track-unlink cases.
  - size: M
- [ ] M304.P1.T2 — Engine gtests for undo snapshots and save/load during a render
  - files: `Tests/RotoKeyDuringRender_Test.cpp`
  - approach: add a point during a render, then smooth/cusp, remove-feather or track-link, and undo; check positions. Save during a render, load, and compare positions. Add an explicit check for the half-written read.
  - verify: the tests fail on main a16ff2464 for the undo and save cases.
  - size: M
- [ ] M304.P1.T3 — Python API test and GUI script
  - files: the Python roto test (`Tests/PyRoto*_Test.cpp` or the file the repo uses), `Tests/gui/m304_points_keep_position.py` (new)
  - approach: from Python, `setPointAtIndex` then the keyframe API on an animated shape. In the GUI, while the viewer renders, insert a point, press Add Key, and move a key in the dope sheet; check the points stay put. Use real APIs and a captioned screenshot per step.
  - verify: the gtest fails on main; the GUI script runs under `build.sh --gui` (render timing may make the GUI failure flaky; the engine tests are the proof).
  - size: M

## Phase 304.2: Fixes

- [ ] M304.P2.T1 — Auto-key reads and writes one side
  - files: `Engine/Bezier.cpp`, `Engine/Bezier.h`
  - approach: `setKeyframeInternal(time, useGuiCurve)` reads and writes the same side and marks that time dirty; every auto-key caller passes the side it edits.
  - verify: the auto-key cases of M304.P1.T1 pass.
  - size: M
- [ ] M304.P2.T2 — Whole-shape key operations respect the render
  - files: `Engine/Bezier.cpp`
  - approach: `setKeyframe`, `removeKeyframe`, `moveKeyframe`, `removeAnimation`, `unlinkPoint`, `setPointPositionAtKeyframe` and `getPointPositionForEdit` choose their side with `useGuiCurveForEdit()`; the GUI path marks dirty instead of copying internal to GUI; `moveKeyframe` takes `itemMutex`.
  - verify: the remaining M304.P1.T1 cases and M304.P1.T3 pass; the M261 GUI script still passes.
  - size: L
- [ ] M304.P2.T3 — Undo snapshots keep GUI state
  - files: `Engine/RotoUndoCommand.cpp`, `Engine/Bezier.cpp`
  - approach: SmoothCusp, RemoveFeather and TrackLink snapshot and restore with `cloneGuiState`/`restorePointState`, as `RotoDragUndoCommand` does; `removeFeatherAtIndex` uses `cloneGuiState` and marks dirty.
  - verify: the undo cases of M304.P1.T2 pass.
  - size: M
- [ ] M304.P2.T4 — Saving includes pending GUI edits
  - files: `Engine/RotoSerializationConvert.cpp`
  - approach: `toSerialization` flushes first, or writes the GUI state while a render runs.
  - verify: the save/load case of M304.P1.T2 passes.
  - size: M

**Verification gate:** the Phase 304.1 tests fail on main a16ff2464 and pass after Phase 304.2; existing Roto and Bezier gtests pass (`BezierIncrementalCopy_Test`, `RotoDragUndo_Test` and the rest); the m224, m261 and m304 GUI scripts pass remotely; the user checks a UAT AppImage (insert, Add Key and dope-sheet key move while rendering).

## Decisions

- 2026-11-08 — Elaborated from a read-only investigation of main a16ff2464. Ranked causes: auto-key keys internal values during a render (high); whole-shape key operations ignore the render (high-medium); undo snapshots copy internal state (medium); saving drops pending GUI edits (medium-low); half-written reads (low-medium, unproven); M261 point edits skip the flush (low); track-unlink bake (low). Ruled out: constant interpolation, `Bezier::clone`, dope-sheet state copies, drag and MakeBezier undos, singular transforms.
