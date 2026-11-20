# Roto point list, broken flag and clone: render isolation

Status: approved.

## What already exists

Each `BezierCP` carries two sides: internal curves, static positions, track link and offsets
(read by renders) and their `gui*` copies (read and written by the GUI). A main-thread edit opens
a `Bezier::EditSide`:

- no render holds the node: `Node::holdRenderStartsForEdit()` succeeds, pending GUI edits are
  flushed, the edit writes the internal side and mirrors it to the GUI side, and render starts on
  other threads wait until the edit ends (the main thread never waits on a worker);
- a render holds the node: the edit writes the GUI side only and records what changed
  (`guiDirtyAll`, `guiDirtyTimes`, `guiDirtyPoints`, under `guiCopyMutex`). When the last render
  releases the node, `Node` dequeues on the main thread and `Bezier::dequeueGuiActions()` copies
  the dirty GUI state to the internal side under `itemMutex`.

Three things are outside that scheme:

1. `BezierPrivate::points` / `featherPoints` were one list pair, and `finished` one flag. `addControlPoint`,
   `addControlPointAfterIndex`, `removeControlPointByIndex` and `clearAllPoints` mutate it under
   `itemMutex` even during a render. Each render read locks `itemMutex`, so a single read is
   consistent, but a render makes many reads (count, bbox, feather quads, per-sample
   evaluation), and an insert between them adds a point whose internal curves are still empty:
   the render sees it at (0,0). A delete shifts indices between reads, and closing or opening
   the curve changes whether the render fills it.
2. `BezierCPPrivate::broken` was a single atomic. `getLeft/RightBezierPointAtTime` realigns
   tangents on it for both sides, so toggling it mid-render changes the tangents a render reads.
3. `Bezier::clone` picked its side with `canSetInternalPoints()` and no hold, so a render could
   start between that check and the internal-side write.

There is no reorder operation on a Bezier; "reorder" is a delete plus an insert, which the list
design covers.

## A GUI-side point list

`BezierPrivate` has `guiPoints` / `guiFeatherPoints` (lists of the same `BezierCPPtr`s; the
points themselves are not duplicated), `guiFinished`, and a `guiDirtyList` flag next to the other
dirty marks.

- **Side rule.** The list side always matches the curve side: `useGuiCurve` reads iterate the GUI
  list, internal reads iterate the internal list, through `pointsFor(bool useGuiCurve)`,
  `featherPointsFor(bool)` and `finishedFor(bool)`, so a reader can never pair the GUI list with
  internal curves (the empty-curve (0,0) case). Edit paths always walk the GUI lists: under a
  hold they equal the internal ones.
- **Writes.** Structural edits take `EditSide` and edit the GUI lists. Under a hold they then copy
  them to the internal lists (`copyGuiListsToRenderLists`). During a render they set
  `guiDirtyList` instead, and mark each inserted point with `markGuiDirtyPoint` so its internal
  curves are filled at write-back. Both lists stay under `itemMutex`; no new lock. A GUI-only
  structural edit leaves the content hash alone, since renders do not see it until write-back.
- **Write-back.** `dequeueGuiActions()` first, if `guiDirtyList`, assigns the internal lists and
  `finished` from the GUI ones and erases the internal orientation cache, then runs its existing per-point/per-time copies over the new list. Deleted points are
  simply no longer referenced; a render still holding the old list kept them alive through its
  `shared_ptr`s. Several edits during one render are already applied in order to the GUI list, so
  the swap writes back their combined result in order.
- **Accessors.** `getControlPoints()`, `getFeatherPoints()`, `getControlPointAtIndex()` and the
  other main-thread accessors return the GUI list. `getControlPoints_mt_safe(bool)`,
  `getFeatherPoints_mt_safe(bool)`, `getControlPointsCount(bool)` and `isCurveFinished(bool)`
  default to the GUI side; the render callers pass `false`: the mask and feather paths in
  `RotoContext.cpp`, the shape count in `RotoCurveContext.cpp`, and `SplineWarpCurveShape::fromBezier`
  (which passes its own `useGuiCurves` through; the SplineWarp overlay asks for the GUI side).
  `Retouch.cpp` only reads `finished` on the main thread and keeps the GUI default. This is the
  same render-path code that is being reworked for render performance; whichever change lands
  second rebases onto the other. `appendGeometryContentHash` hashes the internal lists and
  `finished`, as it hashes the internal curves. Track-link bookkeeping walks the union of both
  lists, so a point deleted during a render keeps its link watched until write-back.
- **Deleting every point.** `clearAllPoints` during a render empties the GUI lists only; the
  render keeps drawing the old shape and the empty shape lands at write-back.
- **`finished`.** `setCurveFinished` writes `guiFinished`, and `finished` too under a hold;
  during a render it marks the list dirty, and the write-back copies it with the lists.
  `isOpenBezier` is set only at creation and by `clone` into a new item, so it stays one flag.

Why not copy-on-write: a COW list (`shared_ptr<const BezierCPs>`) only isolates a render that
captures the snapshot once and passes it through all its reads. Today each render read goes back
to the `Bezier` and locks `itemMutex`, so COW would need a render-scoped capture threaded through
the mask, feather, SplineWarp and Retouch paths, which is the render-path code being reworked for
performance. The GUI list reuses the existing hold, dirty marks and write-back unchanged and adds
no lock. If lock-free render reads are wanted later, the internal list can become a
`shared_ptr<const BezierCPs>` swapped at write-back without changing this design.

## The broken flag

`BezierCPPrivate` gains `guiBroken` (atomic, like `broken`, because the mesh and Coons paths read
it without a lock).

- `setBroken(bool useGuiCurves, bool broken)` writes the GUI flag, and the internal one too when
  `!useGuiCurves`; the caller marks the point dirty as for its curves. `getBroken(bool
  useGuiCurves = true)` reads the matching side. Every caller has an `EditSide` or a
  `useGuiCurve` in scope (`moveBezierPointInternal`, `setTangentsBroken`, `collapseRightHandle`,
  `cuspPoint`, `smoothPoint`); the MakeBezier undo goes through `setTangentsBroken`.
- The tangent realign in `getLeft/RightBezierPointAtTime` reads the side it is evaluating.
- `cloneGuiCurvesToInternalCurves[AtTime]` copy `guiBroken` to `broken`;
  `cloneInternalCurvesToGuiCurves[AtTime]` copy back; `cloneGuiState` copies `guiBroken`
  (and `broken` when `alsoInternal`). Copying it in the at-time variants is one store and covers a
  toggle whose point was only time-dirty.
- Serialization writes `guiBroken` (the GUI side is never behind); load sets both.

## Clone

Every current `Bezier::clone` caller runs on the main thread (`makeDrawableCopy` from RotoPanel,
SplineWarp undo and Retouch; `RotoLayer::clone`), and every target is a freshly built item not
yet in any layer, so no render reads it. The rule:

- **Source side:** always the source's GUI list, `guiFinished` and GUI point state, on either
  thread. The GUI side is what the user sees and is never older than the render side, so a
  duplicate made during a render matches the shape on screen. Read under the source's
  `itemMutex`, which also guards the GUI list; the new points are built there and swapped into the
  target under its own lock, so the two locks are never held together.
- **Target side, main thread:** open an `EditSide` on the target. Unattached it never finds a
  render and writes both sides; if a future caller clones into an attached item during a render,
  it writes the GUI side and marks all dirty, like any edit.
- **Off the main thread** (no caller today; `EditSide` cannot be used there because the hold is
  main-thread only): write the target's GUI side only, mark all and the list dirty, and request a
  dequeue on the main thread through the node's existing `mustDequeueActions` signal. This is a runtime branch, not an assert, since
  asserts are inert in these builds.

## Cost on large shapes

- Memory: one extra list node per control and feather point (about 48 bytes on x86-64), and one
  extra bool per point. A 10 000-point shape with feather costs about 1 MB more.
- Idle edits: each structural edit updates two lists instead of one (O(n) for the index walk, as
  today).
- Write-back: only when `guiDirtyList` is set, one O(n) list assignment of `shared_ptr`s plus the
  existing copies for dirty points; inserted points copy their own curves only. No per-render cost
  when no structural edit happened.
- Renders: no change in work; they iterate the same internal list as today.

## Decisions

1. GUI-side list, not a copy-on-write snapshot.
2. `finished` is isolated with the list.
3. `clone` copies the source's GUI side on both threads.
4. An off-main-thread `clone` takes the runtime fallback above.
5. Reorder is a delete plus an insert.
