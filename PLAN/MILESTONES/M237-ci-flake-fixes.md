# M237 - CI Flake Fixes

Priority (user, 2026-10-26). Two tests fail PR CI on unrelated branches and block merges.

## Phase 237.1: Fixes

- [x] M237.P1.T1 — `Comps/CacheEquivalenceBenchCompTest` well under its timeout
  - files: the test (`Tests/Native/CacheEquivalence_Test.cpp` or the Comps suite), its comp fixture
  - approach: in the debug ctest legs it takes 372–434 s against a 450 s timeout and times out under load (190 s alone). Shrink the comp, its resolution or its frame count so it runs in a small fraction of the timeout, keeping what it proves (cache on and off render the same).
  - verify: the test's run time in the debug PR CI legs; it still fails if the cache returns a wrong result (a deliberately broken key in a scratch run, not committed).
  - size: M

- [x] M237.P1.T2 — `GpuResidencyTest.PipelinedStripsCalibrateLikeSerial` robust to load
  - files: `Tests/GpuResidency_Test.cpp`
  - approach: it compares timings and fails under CI load. Make the check robust (repeat and take the median or minimum, compare with a tolerance justified by measured spread, or assert the structural property it means rather than wall time), keeping it able to catch a real calibration regression.
  - verify: passes in repeated runs under load (e.g. ctest --repeat until-fail:20 with a parallel load), and still fails on a deliberately broken calibration in a scratch run.
  - size: M

- [x] M237.P1.T3 — `RenderIsolation` hang in the legacy scheduler
  - files: `Tests/RenderIsolation*`, the render code the hang points to
  - approach: on PR #118 (run 40382716700, 2026-10-26) RenderIsolation hit the 300 s timeout in the legacy-scheduler leg, where it normally takes ~1–3 s; the retry passed. A hang, not slowness: find the deadlock (run it repeatedly under load with a short timeout and grab thread stacks with eu-stack), and fix the cause, not the test.
  - verify: `ctest -R RenderIsolation --repeat until-fail:200` in both scheduler modes under parallel load passes.
  - size: L

**Verification gate:** release and debug gates green; both tests stable across repeated runs.

PR #123 (T1, T2) merged 2026-10-26 as ca46ae1df: debug CI ran the bench in 50/72 s (was ~230 s) and GpuResidencyTest in 40/45 s. T3 is PR #124.
Shipped 2026-10-26: T3 as PR #124 (squash 2152326be). RenderIsolation.* ran 320 times (8 parallel workers, legacy scheduler, debug CI tree) with no failure.
