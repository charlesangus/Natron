# M369 - Curve Slopes Independent Of Edit Order

With Smooth keys, a curve's in-between values depend on the order its keys were edited.
`Curve::evaluateCurveChanged` (`Engine/Curve.cpp` ~1927-1985) and `removeKeyFrame` (~648-717)
recompute only the edited key and its two neighbours, but the end keys' slopes read the next
key's derivative (`Engine/Interpolation.cpp` ~1342; `Curve.cpp` ~1849, ~1883), which depends on
keys two away, so they go stale. Projects don't store Smooth slopes
(`Serialization/Yaml/CurveYaml.cpp` ~68) and `Curve::fromSerialization` (~421-424) recomputes
them in time order, so a saved and reloaded curve can differ between keys: about 0.1 px in
M304 - Roto Points Snapping To Origin's
`RotoKeyDuringRenderTest.SaveDuringARenderKeepsAnInsertedPoint` at frame 3.5, after
`Bezier::addControlPointAfterIndex` replaced keys 1, 11, 21, 31 in turn.

Line numbers read 2026-11-08; re-check before starting.

## Scope

**In:**
- Every key edit and removal refreshes two keys either side, so the same keys give the same
  curve whatever the edit order.
- `Curve::copyKeyFrameFrom` copies the same span, so Bezier's GUI and internal copies stay equal.
- A Curve regression test, M304's save test restored to compare between keys, and re-recorded
  expected values wherever the first and last segments change.

**Out:**
- Storing every slope in the project file. Rejected: it reverses the M16 format choice and
  leaves the order dependence.
- Comparing only at keys. Rejected: it hides the bug.

**Edge cases:**
- Curves with 1 or 2 keys: the widened span clamps to the curve's ends.
- The first and last segments of most Smooth curves with 3+ keys change value. That is the fix,
  not a regression; expected values are re-recorded.

## Phase 369.1: Order-independent slopes

- [ ] M369.P1.T1 — Refresh two keys either side on every key edit and removal, and re-record what moves
  - files: Engine/Curve.cpp, Engine/Curve.h, Tests/Curve_Test.cpp, Tests/BezierIncrementalCopy_Test.cpp, knob tests under Tests/ that assert Smooth in-between values, animated render goldens under Tests/
  - approach: In `evaluateCurveChanged` and `removeKeyFrame`, recompute derivatives for keys index-2..index+2 (clamped), in time order, instead of index-1..index+1. Widen `copyKeyFrameFrom` to copy the same span. Add `CurveTest.SameKeysSameCurveWhateverEditOrder`: build one set of 5+ Smooth keys by inserting/editing in several orders (including the 1, 11, 21, 31 replacement sequence) and by `fromSerialization`, and compare values at many in-between times (tight tolerance). Then run ctest and re-record the expected values that changed in Curve_Test, knob tests, BezierIncrementalCopy_Test and animated render goldens; list each re-recorded test in the commit message. Ulp-level golden diffs are re-recorded, not investigated.
  - verify: The new test passes; ctest is green with the re-recorded values; the commit lists every re-recorded test.
  - size: L

- [ ] M369.P1.T2 — Restore M304's save test to compare between keys
  - files: Tests/ (the RotoKeyDuringRenderTest source added by M304 - Roto Points Snapping To Origin)
  - approach: Put back the in-between-keys comparison (frame 3.5 and the other in-between frames) in `RotoKeyDuringRenderTest.SaveDuringARenderKeepsAnInsertedPoint` that M304 relaxed to compare only at keys.
  - verify: The test passes, and fails if T1's widened span is reverted locally.
  - size: M

**Verification gate:** full build and ctest green, including `CurveTest.SameKeysSameCurveWhateverEditOrder` and the restored `RotoKeyDuringRenderTest.SaveDuringARenderKeepsAnInsertedPoint`.

## Decisions

- 2026-11-08 — Refresh two keys either side rather than store slopes or compare only at keys (user): storing slopes reverses the M16 format choice and keeps the order dependence; comparing at keys hides the bug.
