# M135 - Pool Sharing Flake Fix

Make `PoolSharing.WideGraphLeavesGetOneThreadEach` (`Tests/PoolSharing_Test.cpp:656`) deterministic. It failed CI `build-and-test` once during M69 - Minor Cleanup's gate ("leaf 63 got two threads instead of one"). It passed on rerun and in both local runs, and M69 touches no pool or scheduler code.

The likely cause is that the test's premise is timing-dependent, not that the scheduler is wrong. The test builds 64 leaves under a pairwise Merge tree on a pool of 4, and expects every leaf's `getNCPUsAvailableForEffect()` to be 1. The budget comes from `RenderScheduler::computeTaskBudget(poolMax, running, ready, cores, perEffect)` (`Engine/RenderScheduler.cpp:812`), computed at pop time (`:1053`): `min(idle, share)`, where `idle = poolMax − running − ready + 1`. Near the end of the leaf wave, few tasks may be running or ready. A late leaf then legitimately gets 2 threads. Whether that happens depends on how fast its siblings finish, since each takes `kDelayMs` = 10 ms.

## Scope

**In:**
- Confirm the cause, then make the test assert only what the scheduler guarantees.

**Out:**
- Changing `computeTaskBudget` or scheduler admission. If the investigation shows a real scheduler bug, record it and stop for the user; don't fix it here.
- Other PoolSharing cases. Fix them only if the same root cause is found there, and record that in `## Decisions`.

**Assumptions:**
- A tail leaf getting more than one thread is correct behaviour (spare cores go to the remaining work), per M63 - Task-Graph Render Scheduler's budget design.

## Phase 135.1: Diagnose and fix

- [ ] M135.P1.T1 — Make WideGraphLeavesGetOneThreadEach assert the scheduler's actual guarantee
  - files: `Tests/PoolSharing_Test.cpp`, `Engine/RenderScheduler.h` (only if a test hook is needed)
  - approach: first reproduce. Run the test in a loop (`--gtest_repeat=500`) while loading the host's cores (for example `stress-ng --cpu $(nproc)`, or a parallel ctest), and record which leaves fail. Read `popLocked` (`Engine/RenderScheduler.cpp:972-1056`) to confirm that a late leaf can see `running + ready < poolMax`. If it can, the scheduler is right and the assertion is too strong. Replace it with a deterministic one. The preferred fix records each leaf's budget **together with** the `running`/`ready` it saw: from the render hook, use the scheduler's existing counter snapshot (the function at `:805` that returns running/ready), or a test-only hook. Then assert that the budget equals `computeTaskBudget` for those inputs, and that every leaf popped while at least `poolMax` tasks were running or ready got exactly 1. A fallback, if snapshotting isn't race-free, is to assert 1 for every leaf except the last `poolMax − 1` in render order (record order in the hook). If instead the cause is a scheduler bug, stop and add a `# Open questions` entry rather than changing the test.
  - verify: the reproduction loop that failed before the change passes 500/500 after it, under the same load. The test still fails if `computeTaskBudget` is temporarily changed to give every task `poolMax` threads, so it still guards the wide-graph behaviour. Full `PoolSharing*` suite green.
  - size: M

**Verification gate:** one build. The stressed repeat loop is 500/500, and full ctest is green apart from known goldens. CI `build-and-test` is green on the PR.
