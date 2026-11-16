# M454 - Roto Insert Point Drag

Inserting a point on a Roto or SplineWarp curve becomes a click-and-drag gesture: Ctrl+Alt+click (and the Add Points tool's click) on a segment inserts the point and, while the button is held, drags it, with the new point's handles re-smoothed on every move. Today `RotoCurveInteract`'s pen-down (`Engine/RotoCurveInteract.cpp`, ~4066-4150, three `AddPointUndoCommand` sites) inserts the point and sets no drag state, so the user has to click again to move it. SplineWarp's `SplineWarpInteract` derives from `RotoCurveInteract`, so both nodes get the behaviour from one change.

## Scope

**In:**
- Ctrl+Alt+click on a curve and a plain click with the Add Points tool insert the point, then drag it while held.
- During the drag, the new point's handles re-smooth continuously (`Bezier::smoothPointAtIndex` → `BezierCP::smoothPoint`).
- The point moves at the current frame exactly as a normal control-point drag does (autokey, feather point following, multi-view rules).
- One undo step removes the inserted point, wherever it was dragged to.

**Out:**
- The neighbours' handles: they keep what the de Casteljau split gave them — none.
- Auto-smooth persisting after release: the point is ordinary once released — none.
- Any change to the insert maths (`Bezier::addControlPointAfterIndex`) — none.

**Edge cases:**
- Click without moving: identical to today (point inserted at the split position with split handles, no smoothing).
- Animated shape: the insert adds the point at every keyframe as today; the drag moves it only at the current frame, and only keys it when autokey would key a normal drag.
- Escape or a cancelled drag mid-gesture: the point stays where the last move put it; undo removes it.
- Feathered curve: the feather point follows the drag like a normal control-point drag.

## Phase 454.1: Gesture

- [ ] M454.P1.T1 — Insert-then-drag state with live smoothing
  - files: `Engine/RotoCurveInteract.cpp`, `Engine/RotoCurveInteract.h`, `Engine/RotoUndoCommand.cpp`, `Engine/RotoUndoCommand.h`
  - approach: after each `AddPointUndoCommand` site (Ctrl+Alt in the select-points and select-curves cases, and the Add Points tool), select the new control point and enter a new state (e.g. `eEventStateDraggingInsertedPoint`) that moves it through the same code path as `eEventStateDraggingSelectedControlPoints` (autokey, feather), then calls `smoothPointAtIndex` on that point only, at the current time, on each move. Make the gesture one undo step: have the add command record the drag's final position (merge the move into it, e.g. a `mergeWith` on the add command, or a macro), so undo removes the point and redo re-inserts and re-moves it.
  - verify: syntax-check; P1.T2's tests.
  - size: M

- [ ] M454.P1.T2 — Tests for the gesture
  - files: `Tests/RotoInsertPointDrag_Test.cpp` (new, or the existing Roto interact test file if one exists), `Tests/CMakeLists.txt`, `Tests/gui/m454_insert_point_drag.py` (new)
  - approach: gtest at the `Bezier` level: insert at t, move the new point, smooth it — its handles are collinear through it and the neighbours' handles equal the split result. GUI script driving real mouse events on a Roto shape and on a SplineWarp curve: Ctrl+Alt+press on a segment, move, release → the point is at the release position with smooth handles; one undo removes it; plain click with no move matches today's result; the Add Points tool behaves the same.
  - verify: the tests pass in the remote gate and the GUI script fails on main.
  - size: M

**Verification gate:** build and ctest green; the GUI script passes under the remote gate on both Roto and SplineWarp; UAT AppImage with steps for the user.

## Decisions

- 2026-11-16 — Neighbour handles keep the split result (user): predictable, doesn't overwrite tuned handles.
- 2026-11-16 — The Add Points tool gets the same click-and-drag (user).
- 2026-11-16 — Smoothing is live only during the insert drag; afterwards the point is ordinary (user): no new saved per-point flag.
- 2026-11-16 — One undo step for insert plus drag (user).
- 2026-11-16 — SplineWarp is covered by the same change: `SplineWarpInteract` derives from `RotoCurveInteract` (code finding).
