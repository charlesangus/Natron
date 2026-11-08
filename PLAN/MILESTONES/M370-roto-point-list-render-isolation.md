# M370 - Roto Point List Render Isolation

M304 - Roto Points Snapping To Origin keeps a GUI side for point positions, keys, interpolation
and track links while a render holds a Bezier's internal curves, but the point list itself is
still shared: deleting points (or otherwise changing which points are in the list, around the
point removal in `Engine/Bezier.cpp` ~1745) during a render changes the list the render
iterates. Also still shared: each point's `broken` flag, and `Bezier::clone` picks its side
without the render-start hold because it can run off the main thread. This milestone gives the
point list, the `broken` flag and `clone` the same isolation, so renders never see a change
mid-render.

Line numbers read 2026-11-08; re-check against M304's merged code before starting.

## Scope

**In:**
- A GUI-side point list (or a copy-on-write snapshot) that a running render never sees change,
  written back with the other GUI edits when the render releases.
- The `broken` flag isolated the same way.
- `Bezier::clone` choosing its side correctly off the main thread.

**Edge cases:**
- Insert, delete and reorder of points, and deleting every point, while a render runs.
- Several edits during one render are all written back, in order.

**Assumptions:**
- Render starts stay responsive: no main-thread wait on workers (UI responsiveness rule).

## Phase 370.1: Design

- [ ] M370.P1.T1 — Design note for point-list, broken-flag and clone isolation
  - files: Engine/Bezier.cpp, Engine/Bezier.h, Engine/BezierCP.cpp, Engine/RotoDrawableItem.cpp (read), a new note under Engine/docs/
  - approach: Read M304's GUI-side mechanism and write a short note: GUI-side list vs copy-on-write snapshot (recommend whichever reuses M304's hold and write-back with the fewest new locks), how `broken` joins it, how `clone` finds the right side without the render-start hold, and the cost on large shapes. Send it to the user and park as `response` until approved.
  - verify: The note exists and the user's approval is recorded in this file's `## Decisions`.
  - size: L

## Phase 370.2: Implementation

- [ ] M370.P2.T1 — Isolate the point list from running renders
  - files: Engine/Bezier.cpp, Engine/Bezier.h, Engine/BezierPrivate.h (if present), Tests/ (M304's render-isolation test file)
  - approach: Implement the approved list design: renders iterate a list that structural edits (insert, delete, reorder) never mutate during the render; GUI edits go to the GUI side and are written back with M304's other GUI edits. Extend M304's `RenderThreadReadsNoEditMadeDuringItsRender` pattern with a concurrent-reader test that deletes, inserts and reorders points mid-render.
  - verify: The new concurrent tests pass repeatedly (`--gtest_repeat=50`), and M304's existing render-isolation tests still pass.
  - size: L

- [ ] M370.P2.T2 — Isolate the broken flag and make clone pick its side off the main thread
  - files: Engine/Bezier.cpp, Engine/BezierCP.cpp, Engine/BezierCP.h, Tests/ (M304's render-isolation test file)
  - approach: Move `broken` onto the same GUI/render sides as positions per the note, and make `Bezier::clone` select its source side by the approved rule rather than by the render-start hold. Add tests: toggling `broken` mid-render is invisible to the render; `clone` from a worker thread during a render copies the render side and from the main thread copies the GUI side (or whatever the note specifies).
  - verify: The new tests pass repeatedly (`--gtest_repeat=50`); ctest for Roto and Bezier suites green.
  - size: L

**Verification gate:** full build and ctest green, with the new concurrent tests passing under `--gtest_repeat=50`; the m224, m261 and m304 GUI scripts pass under Xvfb.

## Decisions

- 2026-11-08 — Render start must not wait on workers from the main thread (user): keep render starts responsive.
