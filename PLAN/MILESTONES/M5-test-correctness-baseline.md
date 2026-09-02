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

> The "work the existing P0 list" task that stood here is **cancelled**. Its
> eight issue numbers were scraped from `Documentation/source/maintainers/todo.rst`,
> an upstream file predating this fork by six weeks, and read as requirements
> during plan authoring. Triage found most were Windows-only or filed against the
> Qt5/PySide2 2.5.0 bundle — a stack this fork does not have. See
> `DECISIONS/2026-09-02-inherited-docs-are-not-requirements.md`. M5 delivers the
> safety net only; bug-fixing work enters through the user, not through inherited
> docs.

**Verification gate:** the ctest suite (including the new render regression
test) stays green on every CI run.
