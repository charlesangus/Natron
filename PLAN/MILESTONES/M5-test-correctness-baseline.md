# Milestone 5: Test & correctness baseline

ongoing after M4 · low risk. The foundation is "clean" once it's green *and*
hard to silently break again. The existing suite is real but narrow.

## Phase 5.1: Regression coverage

- [ ] M5.P1.T1 — Keep the existing 8-file `ctest` suite passing throughout
  - files: `Curve`, `Hash64`, `Image`, `KnobFile`, `Lut`, `FileSystemModel`,
    `OSGLContext`, `Tracker` test targets
  - approach: this is the regression gate for the whole migration — treat any
    failure introduced by M2/M3 work as a blocker, not a follow-up.
  - verify: `ctest` runs all 8 suites green on every M4 CI run.
  - size: S

- [ ] M5.P1.T2 — Add one headless render regression test
  - files: new test fixture (a known `.ntp` project) + `NatronRenderer`
    invocation wired into `ctest`
  - approach: render a known `.ntp` project via `NatronRenderer` and diff
    against a golden image. Covers the render pipeline, cache, and
    serialization — exactly the subsystems the P0 bugs live in, and the gap
    the maintainer guide's own TODO flags as highest-value.
  - verify: the new `ctest` target renders the fixture and diffs cleanly
    against the golden image in CI.
  - size: M

- [ ] M5.P1.T3 — Work the existing P0 list once CI can catch regressions
  - files: varies per bug — cache disk-limit crash (#192), silent render
    stall (#248), startup/teardown crashes (#845/#1008/#795/#1029/#1057), CLI
    zero-frame bug (#864)
  - approach: same bugs regardless of fork — now actually safe to fix without
    a manual full-GUI check each time, since M4's CI and M5.P1.T1/T2 catch
    regressions.
  - verify: each fixed bug gets a regression test (extending M5.P1.T1/T2's
    suites) that fails before the fix and passes after.
  - size: L

**Verification gate:** the ctest suite (including the new render regression
test) stays green on every CI run, and each P0 bug fixed carries a
regression test that would have caught it.
