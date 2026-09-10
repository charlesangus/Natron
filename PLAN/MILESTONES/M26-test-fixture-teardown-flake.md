# Milestone 26: Fix the shared test-fixture teardown flake

The ctest suite aborts in teardown roughly half the time. The gtest body prints
`[  OK  ]` and `[  PASSED  ]`, and the process then dies with `QThread: Destroyed
while thread is still running`, so ctest reports "Subprocess aborted" for a case
that actually passed. It began in `BaseTest` (M17) and now hits anything built on
the app fixture, including M18's `DeepRenderPipelineTest`.

Measured 2026-09-10: three consecutive `ctest -R DeepRenderPipelineTest` runs
gave 2/4, 0/4 and 2/4 failures; one full-suite run failed 5 of 121 — `BaseTest`
cases `DotFedByImageSourceResolvesToImage`,
`DotChainContradictionRejectedFromDownstreamSide`,
`DotResolvesFromItsDownstreamConsumerAlone`, plus
`DeepRenderPipelineTest.AbortDuringUpstreamRenderLeavesNothingCached` and
`.TwoPassHelperMatchesSerialReference`. Which cases are hit varies per run; that
the body passes before the abort does not.

This blocks M18: every one of its remaining tasks verifies through ctest, and a
suite that fails half the time for unrelated reasons cannot verify anything.
Rationale for doing it now and as its own milestone:
`PLAN/DECISIONS/2026-09-10-fix-test-fixture-teardown-flake.md`.

## Phase 26.1: Diagnose and fix

- [ ] M26.P1.T1 — Find which thread outlives teardown
  - files: `Tests/BaseTest.cpp`, `Tests/BaseTest.h`, plus whatever
    `AppInstance`/`AppManager` teardown path the investigation implicates
  - approach: reproduce under the container (`xvfb-run --auto-servernum ctest -R
    BaseTest --output-on-failure`, repeat until it aborts) and identify the
    `QThread` still running when its owner is destroyed. `--gdb` via
    `tools/ci/local/test.sh` only applies to `smoke`, so get a backtrace by
    running the `Tests` binary directly under gdb with a `--gtest_filter`. The
    likely shape is a render/thread-pool worker or a `QThread`-derived helper
    whose owner is destroyed in `BaseTest::TearDown()` without a
    `quit()`/`wait()`, but confirm rather than assume — report the actual thread
    and the actual destruction order before changing anything.
  - verify: a written account naming the thread, its owner, and the exact
    teardown ordering that lets the owner die first; reproduced under gdb with a
    backtrace, not inferred from reading.
  - size: L

- [ ] M26.P1.T2 — Make teardown join the thread it destroys
  - files: as implicated by M26.P1.T1
  - approach: shut the thread down deterministically before its owner is
    destroyed — `quit()` + `wait()` on the owning object's teardown path, or the
    equivalent for a `QThreadPool`. Fix the ordering; do not paper over it by
    leaking the owner, suppressing the warning, or adding a sleep. If the
    offending thread belongs to production code rather than the fixture, fix it
    there — a thread outliving its owner is a real defect that happens to be
    surfacing in tests.
  - verify: `ctest -R BaseTest` and `ctest -R DeepRenderPipelineTest` each run
    **20 consecutive times with zero aborts** (a single green run proves nothing
    at a 50% flake rate); full ctest suite green; the fix demonstrably addresses
    the ordering found in T1 rather than hiding the symptom.
  - size: M

**Verification gate:** the full ctest suite green, and `ctest -R "BaseTest|DeepRenderPipelineTest"` run 20 consecutive times with zero "Subprocess aborted" results. CI's `build-and-test` job green on the PR.
