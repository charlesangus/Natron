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
points themselves are not duplicated), `guiFinished`, `guiIsOpenBezier`, and a `guiDirtyList`
flag next to the other dirty marks.

- **Side rule.** The list side always matches the curve side: `useGuiCurve` reads iterate the GUI
  list, internal reads iterate the internal list, through `pointsFor(bool useGuiCurve)`,
  `featherPointsFor(bool)`, `finishedFor(bool)` and `openFor(bool)`, so a reader can never pair the GUI list with
  internal curves (the empty-curve (0,0) case). Edit paths always walk the GUI lists: under a
  hold they equal the internal ones.
- **Writes.** Structural edits take `EditSide` and edit the GUI lists. Under a hold they then copy
  them to the internal lists (`copyGuiListsToRenderLists`). During a render they set
  `guiDirtyList` instead, and mark each inserted point with `markGuiDirtyPoint` so its internal
  curves are filled at write-back. Both lists stay under `itemMutex`; no new lock. A GUI-only
  structural edit leaves the content hash alone, since renders do not see it until write-back.
- **Write-back.** `dequeueGuiActions()` first, if `guiDirtyList`, assigns the internal lists,
  `finished` and `isOpenBezier` from the GUI ones and erases the internal orientation cache, then runs its existing per-point/per-time copies over the new list. Deleted points are
  simply no longer referenced; a render still holding the old list kept them alive through its
  `shared_ptr`s. Several edits during one render are already applied in order to the GUI list, so
  the swap writes back their combined result in order.
- **Accessors.** `getControlPoints()`, `getFeatherPoints()`, `getControlPointAtIndex()` and the
  other main-thread accessors return the GUI list. `getControlPoints_mt_safe(bool)`,
  `getFeatherPoints_mt_safe(bool)`, `getControlPointsCount(bool)`, `isCurveFinished(bool)` and
  `isOpenBezier(bool)` default to the GUI side; the render callers pass `false`: the mask and
  feather paths in `RotoContext.cpp`, `rasterizeCoverage`, the SplineWarp cookie and Retouch's
  shape collection, and `SplineWarpCurveShape::fromBezier` (which passes its own `useGuiCurves`
  through; the SplineWarp overlay asks for the GUI side).
  `Retouch.cpp` only reads `finished` on the main thread and keeps the GUI default. This is the
  same render-path code that is being reworked for render performance; whichever change lands
  second rebases onto the other. `appendGeometryContentHash` hashes the internal lists and
  `finished`, as it hashes the internal curves. Track-link bookkeeping walks the union of both
  lists, so a point deleted during a render keeps its link watched until write-back. The union
  lists the render lists first, in their order, then GUI-only points, so the live content hash of
  the linked points does not change when a linked point is deleted from the GUI list alone.
- **Bounding boxes.** `RotoDrawableItem::getBoundingBox(time, useGuiCurves)` and
  `RotoCurveContext::getItemsRegionOfDefinition(..., useGuiCurves, ...)` take the side
  explicitly. The transform-centre resets (the item's and the context's, including the one
  `setCurveFinished` runs) pass `true`: a shape drawn entirely during a render has an empty
  render list, so reading it would give a NaN centre. The mask region of definition, the mask
  renders, the SplineWarp cookie and Retouch pass `false`.
- **Main-thread readers** that are not edits read the GUI side throughout: AutoStraighten's drawn
  lines and line orientation, the Python group export and the curve editor's control-point
  curves. Reading the GUI list with internal curves would give (0,0) for a point inserted during a
  render.
- **Deleting every point.** `clearAllPoints` during a render empties the GUI lists only; the
  render keeps drawing the old shape and the empty shape lands at write-back.
- **`finished`.** `setCurveFinished` writes `guiFinished`, and `finished` too under a hold;
  during a render it marks the list dirty, and the write-back copies it with the lists.
- **`isOpenBezier`** is set at creation, on load and by `clone`. Undo and redo clone into a live
  shape, so it has a GUI side too: `clone` writes `guiIsOpenBezier`, and the render side follows
  under a hold or at write-back.

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

Every current `Bezier::clone` caller runs on the main thread. `makeDrawableCopy` (RotoPanel,
SplineWarp undo and Retouch) and `RotoLayer::clone` clone into a freshly built item not yet in
any layer, but the undo commands clone a saved copy back into the live shape, which a render
may be reading. The rule:

- **Source side:** always the source's GUI list, `guiFinished`, `guiIsOpenBezier` and GUI point
  state, on either thread. The GUI side is what the user sees and is never older than the render
  side, so a duplicate made during a render matches the shape on screen. Read under the source's
  `itemMutex`, which also guards the GUI list. The new points are built there with both of their
  sides written, which is safe because they are in no list yet, and are swapped into the target
  under its own lock, so the two locks are never held together.
- **Target side, main thread:** open an `EditSide` on the target. With no render it writes both
  sides; during a render (an undo into a live shape) it writes the GUI side and marks all and the
  list dirty, under the target's `itemMutex`, like any edit.
- **Off the main thread** (no caller today; `EditSide` cannot be used there because the hold is
  main-thread only): the new points are built on the calling thread, and the swap into the
  target's lists is queued to the main thread, where it takes the main-thread path above.
  Main-thread readers iterate the GUI lists without a lock between calls, so no other thread may
  swap them. This is a runtime branch, not an assert, since asserts are inert in these builds.
  The target's point lists change only once the main thread runs the queued swap. If the
  target's context or node is gone by then, the swap is dropped: the shape is being torn down,
  and `EditSide` needs the node.
- **Knobs off the main thread** are cloned on the calling thread, before the swap is queued.
  `Knob::clone` reads the live source knob, and a detached snapshot taken through serialization
  would lose its expressions and links. So knob values land before the points, and a main-thread
  edit to the target's points made between the clone and the queued swap loses to the swap.

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
2. `finished` and `isOpenBezier` are isolated with the list.
3. `clone` copies the source's GUI side on both threads.
4. An off-main-thread `clone` queues its list swap to the main thread.
5. Reorder is a delete plus an insert.
