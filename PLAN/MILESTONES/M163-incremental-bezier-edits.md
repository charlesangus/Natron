# M163 - Incremental Bezier Edits

Building M16 - Project Format Redesign's heavy benchmark project (30 beziers × 24 points × 200 keys through Python) took 2664 s, because each `Bezier` point edit does work proportional to the whole shape's animation. The main cost is `Bezier::copyInternalPointsToGuiPoints()` (`Engine/Bezier.cpp:1007`). It calls `BezierCP::cloneInternalCurvesToGuiCurves()` (`Engine/BezierCP.cpp:982`) on every control point and feather point, so all six curves of each point get cloned. That runs after `setPointAtIndexInternal` (`:2175`), `movePointByIndexInternal` (`:1723`), `moveBezierPointInternal` (`:2032`), `transformPoint` (`:2270`), `smoothOrCuspPointAtIndex` (`:2355`), `clone` (`:1102`), `addControlPoint` (`:1181`), `addControlPointAfterIndex` (`:1387`), `removeKeyframe` (`:2455`), `removeAnimation` (`:2487`) and `moveKeyframe` (`:2543`). This milestone makes the GUI copy incremental. It also replaces the eager orientation recompute with a per-time cache that is invalidated per key. The GUI-copy guarantee stays: while `canSetInternalPoints()` (`:1022`) is false during a render, edits go to the GUI curves, and `dequeueGuiActions()` (`:977`) writes them back. Filed 2026-10-24 at the user's request (2026-10-22).

## Scope

**In:**
- Per-point and per-time GUI copies for every edit path listed above. Write-back from GUI to internal limited to what changed (dirty tracking).
- Orientation computed lazily per time, with invalidation per key, for both the internal and GUI maps (`isClockwiseOriented` / `guiIsClockwiseOriented`, `Engine/RotoContextPrivate.h:402-406`).
- A scaling microbenchmark, a Python bench for the host AppImage, and an equivalence test that checks the GUI copy against the internal curves.

**Out:**
- Removing the GUI/internal double-buffer, or changing the threading model of roto renders.
- Cost inside `Curve` itself, such as `Curve::addKeyFrame` (`Engine/Curve.cpp:478`) and the double copy in `Curve::clone` (`:339`).
- Cloning of whole shapes by the undo commands (`Engine/RotoUndoCommand.cpp`).

**Edge cases:**
- The original report says every edit recomputes orientation at every key. That needs correcting: on main, edit paths already use the per-time `refreshPolygonOrientation(bool,double)` (`Bezier.cpp:3485`). The per-key loop over every key (`:3499`) runs only from `setCurveFinished` (`:1504`), `removeControlPointByIndex` (`:1542`), project load (`Engine/RotoSerializationConvert.cpp:229`) and `RemovePointUndoCommand::redo` (`Engine/RotoUndoCommand.cpp:555`). Phase 163.3 still matters for those callers and for the eager per-edit O(P log K) cost.
- The orientation reader (`Bezier.cpp:3443`) always reads the internal map, even when `useGuiCurve` is true. `moveKeyframe` stores the key time as the orientation bool (`foundOldIt->first`, `:2539`). Fixing either one can change overlay feather direction. Render reads `useGuiCurve=false` (`Engine/RotoContext.cpp:3325, 3796`), so render output must stay bit-identical.
- `setPointAtIndexInternal` copies to the GUI before `setKeyframe(time)` (`:2172-2178`), and `setKeyframe` (`:2379`) never copies. A per-time copy has to run after the auto-key step.
- Shapes with no keys use `isClockwiseOrientedStatic`. An edit to an unkeyed shape must mark it dirty.
- The cache currently also holds entries at interpolated (non-key) times, from lazy reads. Editing key t must invalidate the cache over (prevKey, nextKey).

**Assumptions:**
- The Python methods `addControlPoint`, `setPointAtIndex` and `setFeatherPointAtIndex` forward one-to-one to the C++ calls (`Engine/PyRoto.cpp:214, 276, 289`). A C++ gtest bench therefore measures the same path.
- `tools/bench/gui/run.sh` is not on `origin/main` (it comes with M117 - GUI Perf Harness And Baseline), so the drag check in P4.T1 is conditional on it.

## Phase 163.1: Baseline and safety net

- [x] M163.P1.T1 — Add a Bezier edit-scaling microbenchmark
  - files: `Tests/BezierEditScaling_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Use the fixture pattern from `Tests/RotoContentHash_Test.cpp:69-91`. Build a finished, feathered 24-point bezier and key it at K = 25, 50, 100, 200 with `setPointAtIndex(false/true, …)` per point per key. Use `std::chrono::steady_clock` and print the total build time per K, the median per-edit `setPointAtIndex` cost at the last K, and the log-log slope, in the style of `HashingManyStrokesTiming` (`:255`). Print only, with no timing asserts yet.
  - verify: ctest runs the suite and the release log shows four rows plus a slope. Record the baseline slope (expected about 2 for total time) in the task notes.
  - size: M

- [x] M163.P1.T2 — Add a GUI/internal equivalence and golden test covering every edit path
  - files: `Tests/BezierGuiCopyEquivalence_Test.cpp` (new), `Tests/CMakeLists.txt`, `Tests/fixtures/bezier-edit-golden.txt` (new)
  - approach: Write a deterministic script over a keyed, feathered shape. It calls every edit path listed in the intro, plus ripple edit, auto-key on and off, `setKeyframe`, `removeKeyframe`, `moveKeyframe`, `removeControlPointByIndex` and `setCurveFinished`. After each step, compare `get{Position,Left,RightBezierPoint}AtTime(true,…)` with `(false,…)` on every point at each key time and at midpoints between keys. Also check `isFeatherPolygonClockwiseOriented(false,t)` against a fresh shape built by `clone`. Write a golden fixture containing the serialized roto bytes and a mask render checksum at three frames (render through a Write node, as `Tests/WriteAllLayers_Test.cpp` does). Generate the fixture from main's current code.
  - verify: The test passes on unmodified main and the golden fixture is committed.
  - size: L

- [x] M163.P1.T3 — Add a Python bezier-build bench for the host AppImage and record the baseline
  - files: `tools/bench/bezier_build_bench.py` (new), `tools/bench/README.md`, `tools/bench/BASELINE.md`
  - approach: The script runs under NatronRenderer. It builds N beziers × 24 points × K keys (defaults 1×24×{50,100,200}; `--heavy` gives 30×24×200) and prints the wall time per K and the per-edit cost. Run it on the user's host with a stripped `release` AppImage built from main, and record the numbers in BASELINE.md.
  - verify: BASELINE.md has a dated "M163 before" table from the host, not from a GitHub runner.
  - size: M

## Phase 163.2: Incremental GUI copy

- [x] M163.P2.T1 — Add per-time and per-curve copy primitives to BezierCP
  - files: `Engine/BezierCP.h`, `Engine/BezierCP.cpp`, `Engine/Curve.h`, `Engine/Curve.cpp`
  - approach: Add `cloneInternalCurvesToGuiCurvesAtTime(double t)` and the matching `cloneGuiCurvesToInternalCurvesAtTime(double t)`, next to `:982` and `:1003`. Each one mirrors the key at t on all six curves, or removes it if the source has none, and also copies the static positions. If `Curve` has no helper for copying a single keyframe with its derivatives and interpolation, add `Curve::copyKeyFrameFrom(const Curve&, double)`. Keep the full clones for structural cases.
  - verify: A new unit case in `Tests/BezierGuiCopyEquivalence_Test.cpp` shows that a per-time copy matches a full clone, value for value and derivative for derivative.
  - size: M

- [x] M163.P2.T2 — Switch the single-time and single-point edit paths to incremental copies
  - files: `Engine/Bezier.cpp`, `Engine/Bezier.h`
  - approach: Add `copyInternalPointsToGuiAtTime(t)` (all points at one time) and `copyInternalPointToGui(cp, fp)` (one point, all times). Use them in `setPointAtIndexInternal`, `movePointByIndexInternal`, `moveBezierPointInternal`, `transformPoint` and `smoothOrCuspPointAtIndex`. Ripple-edit branches use the per-point variant. Run the copy after `setKeyframe(time)`. Copy only the orientation entry at t.
  - verify: The P1.T2 equivalence test and golden pass, and the P1.T1 per-edit cost no longer grows with points × keys.
  - size: L

- [x] M163.P2.T3 — Make keyframe and structural ops and the render write-back incremental
  - files: `Engine/Bezier.cpp`, `Engine/RotoContextPrivate.h`
  - approach: `addControlPoint` and `addControlPointAfterIndex` copy only the new point or points, because the point lists are shared. `removeKeyframe` and `moveKeyframe` copy only the affected times. `clone`, `removeAnimation` and load keep the full copy. Replace the `mustCopyGui` bool (`RotoContextPrivate.h:411`) with a dirty set of (point, time) pairs or "all", so `dequeueGuiActions` (`Bezier.cpp:977`) writes back only what changed.
  - verify: The P1.T2 test passes, plus a new case that edits while `canSetInternalPoints()` is forced false and then dequeues, showing internal equals GUI.
  - size: M

## Phase 163.3: Lazy per-time orientation

- [x] M163.P3.T1 — Replace eager orientation recompute with per-key invalidation
  - files: `Engine/Bezier.cpp`, `Engine/Bezier.h`, `Engine/RotoContextPrivate.h`, `Engine/RotoUndoCommand.cpp`, `Engine/RotoSerializationConvert.cpp`
  - approach: An edit at t erases the cached entries in [prevKey, nextKey] in the map for the edited side. Unkeyed shapes set a static-dirty flag. The every-key `refreshPolygonOrientation(bool)` (`:3499`) becomes "clear the cache". The reader (`:3440`) computes on a miss, under `itemMutex`, from the map that matches `useGuiCurve`. `moveKeyframe` erases both times. Keep `setAutoOrientationComputation` working.
  - verify: The P1.T2 orientation checks and golden render pass, and the `RemovePointUndoCommand` path takes O(1) per removal.
  - size: L

- [x] M163.P3.T2 — Turn the bench into a scaling regression guard
  - files: `Tests/BezierEditScaling_Test.cpp`
  - approach: Assert on ratios, never on absolute times, using the median of 5 runs. Total build time from K=100 to K=200 must stay at or below 2.6×. Per-edit `setPointAtIndex` cost at K=200 must stay at or below 2× the cost at K=25. Skip the asserts when `NDEBUG` is unset.
  - verify: It passes on the phase build, and fails locally if the full copy at `:1007` is temporarily put back.
  - size: M

## Phase 163.4: Host measurement

- [ ] M163.P4.T1 — Measure after on the host and check drag responsiveness
  - files: `tools/bench/BASELINE.md`
  - approach: Rerun `tools/bench/bezier_build_bench.py` (including `--heavy`) on a stripped `release` AppImage on the user's host and record the "after" table next to the baseline. If `tools/bench/gui/run.sh` exists on main, run a point drag on a 24×200 feathered shape, plus one with a tracker-linked bezier, and record frame times. Otherwise record the drag check as a manual run on the AppImage.
  - verify: The table shows time roughly linear in K and a large drop for the heavy build. The drag numbers (or the manual note) are recorded and approved by the user.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh release --tests` is green for each phase. `BezierGuiCopyEquivalence` matches the golden fixture byte for byte (serialized roto and mask checksums), and `BezierEditScaling`, `RotoContentHash`, `RotoLayer`, `RotoFeatherDrag` and `RotoXmlEquivalence` pass. The host AppImage bench shows a 24×200 build roughly linear in keys with flat per-edit cost. The drag check passes, through the GUI bench if M117 - GUI Perf Harness And Baseline has landed.

2026-10-27: Release gate 547/547; bench 73.5 s → 0.20 s at 200 keys (BASELINE.md); draft PR #134; UAT pending (drag check).
