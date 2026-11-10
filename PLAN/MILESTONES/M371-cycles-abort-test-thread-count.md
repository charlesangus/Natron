# M371 - Cycles Abort Test Thread Count

`USDRenderCyclesTest.AbortStopsTheRenderPromptly` (`Tests/Scene/USDRenderCycles_Test.cpp` ~571)
is flaky: its thread-count check (second run ≤ first run) failed 19 vs 18 on M360 - Geometry
Kernel Language's release gate (run 42513356809), on a branch that doesn't touch USD or Cycles.
The process thread count picks up unrelated pool threads.

## Scope

**In:**
- The check counts only Cycles/Hydra-owned threads, or allows a settle and retry, so unrelated
  pool threads can't fail it.

**Out:**
- Changes to Cycles abort behaviour itself.

## Phase 371.1: Fix the check

- [x] M371.P1.T1 — Make the abort test's thread check ignore unrelated threads
  - files: Tests/Scene/USDRenderCycles_Test.cpp
  - approach: Prefer counting only Cycles/Hydra-owned threads (e.g. by `/proc/self/task/*/comm` names Cycles/TBB give its workers, if they are distinguishable). If they aren't, wait for the global thread pool to settle and retry the comparison a few times with a short delay before failing. Keep the check able to catch a real thread leak (a growing count over repeated aborts).
  - verify: The test passes with `--gtest_repeat=30` while another test binary runs alongside; a deliberate leak (temporarily skip the render's thread join) still fails it.
  - size: M

**Verification gate:** ctest green; `USDRenderCyclesTest.AbortStopsTheRenderPromptly` passes `--gtest_repeat=30` under load.

## Decisions

- 2026-11-10 — The check takes its reference count after two warm-up runs, then polls each later run for up to 2 s, so a lasting unrelated pool thread is ignored and a per-abort leak still accumulates. The "Subprocess aborted" in run 42757353797 was a different failure: heap corruption in `EmissionMatchesReferenceRender`. It is passed to M378 - Scene Render Test Stability.
- 2026-11-10 — Gate: `USDRenderCyclesTest` passed 30 of 30 under load on both legs (release run 42832073303, at 5c40f8625). PR #250 is reviewed and green, and waits on PR #248 (M377 - Readable CI Names) merging so its checks report under the new required names.
