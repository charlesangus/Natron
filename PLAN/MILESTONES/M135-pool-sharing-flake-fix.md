# M135 - Flaky Test Fixes

Originally scoped to the PoolSharing flake. The user widened it on 2026-10-21 to every flaky test found so far; see Phase 135.2.

Make `PoolSharing.WideGraphLeavesGetOneThreadEach` (`Tests/PoolSharing_Test.cpp:656`) deterministic. It failed CI `build-and-test` once during M69 - Minor Cleanup's gate ("leaf 63 got two threads instead of one"). It passed on rerun and in both local runs, and M69 touches no pool or scheduler code.

The likely cause is that the test's premise is timing-dependent, not that the scheduler is wrong. The test builds 64 leaves under a pairwise Merge tree on a pool of 4, and expects every leaf's `getNCPUsAvailableForEffect()` to be 1. The budget comes from `RenderScheduler::computeTaskBudget(poolMax, running, ready, cores, perEffect)` (`Engine/RenderScheduler.cpp:812`), computed at pop time (`:1053`): `min(idle, share)`, where `idle = poolMax − running − ready + 1`. Near the end of the leaf wave, few tasks may be running or ready. A late leaf then legitimately gets 2 threads. Whether that happens depends on how fast its siblings finish, since each takes `kDelayMs` = 10 ms.

## Scope

**In:**
- Confirm the cause, then make the test assert only what the scheduler guarantees.
- `DeepLayersGraphTest.UnchangedLayersPassThroughRatherThanBeingProduced` (`Tests/DeepLayers_Test.cpp:522`): a segfault seen once under parallel debug ctest. Find the root cause and fix it where it lives, Engine included, since a segfault is not a test-premise issue.

**Out:**
- Changing `computeTaskBudget` or scheduler admission. If the investigation shows a real scheduler bug, record it and stop for the user; don't fix it here.
- Other PoolSharing cases. Fix them only if the same root cause is found there, and record that in `## Decisions`.

**Assumptions:**
- A tail leaf getting more than one thread is correct behaviour (spare cores go to the remaining work), per M63 - Task-Graph Render Scheduler's budget design.

## Phase 135.1: Diagnose and fix

- [x] M135.P1.T1 — Make WideGraphLeavesGetOneThreadEach assert the scheduler's actual guarantee
  - files: `Tests/PoolSharing_Test.cpp`, `Engine/RenderScheduler.h` (only if a test hook is needed)
  - approach: first reproduce. Run the test in a loop (`--gtest_repeat=500`) while loading the host's cores (for example `stress-ng --cpu $(nproc)`, or a parallel ctest), and record which leaves fail. Read `popLocked` (`Engine/RenderScheduler.cpp:972-1056`) to confirm that a late leaf can see `running + ready < poolMax`. If it can, the scheduler is right and the assertion is too strong. Replace it with a deterministic one. The preferred fix records each leaf's budget **together with** the `running`/`ready` it saw: from the render hook, use the scheduler's existing counter snapshot (the function at `:805` that returns running/ready), or a test-only hook. Then assert that the budget equals `computeTaskBudget` for those inputs, and that every leaf popped while at least `poolMax` tasks were running or ready got exactly 1. A fallback, if snapshotting isn't race-free, is to assert 1 for every leaf except the last `poolMax − 1` in render order (record order in the hook). If instead the cause is a scheduler bug, stop and add a `# Open questions` entry rather than changing the test.
  - verify: the reproduction loop that failed before the change passes 500/500 after it, under the same load. The test still fails if `computeTaskBudget` is temporarily changed to give every task `poolMax` threads, so it still guards the wide-graph behaviour. Full `PoolSharing*` suite green.
  - size: M

## Phase 135.2: Deep layers segfault

- [x] M135.P2.T1 — Find and fix the DeepLayersGraphTest segfault
  - files: `Tests/DeepLayers_Test.cpp`, and the Engine deep-layer code the crash leads to (to be found)
  - approach:
    - Seen once: PR CI for M141 - Complete Local Build Environment (run 39591735434, `ctest (legacy)`, debug preset, `ctest -j 4`, inside the new GHCR image). The case segfaulted after 1.67 s and passed on the serial `--rerun-failed -V` that followed. It does not appear in main's last six CI runs.
    - Reproduce in the debug build: `--gtest_repeat` on the case, and on the whole `DeepLayersGraphTest` suite (it may depend on a sibling case once suites share a process), under CPU load, in both `NATRON_RENDER_SCHEDULER` modes. Get a backtrace: core dump or gdb in the container, see the profiling memory for privileges. A TSan or ASan build of `Tests` is in scope if the plain build won't reproduce it.
    - Fix the root cause. If it turns out to be a design-level race in the scheduler or cache, record it and stop for the user, as P1 does.
    - Second sighting (2026-10-21, remote debug run for M141): `DeepRenderPipelineTest.AbortDuringUpstreamRenderLeavesNothingCached` passed, then segfaulted during process teardown with `malloc(): unsorted double linked list corrupted`. Likely the same bug, a shutdown race: something outlives the app at exit. Repeat loops have to launch a fresh process per iteration to exercise exit.
  - verify: the stressed loops (fresh process per iteration) that crashed before pass 500/500 after, in both scheduler modes on the debug preset, for both cases. The `DeepLayers*` and `DeepRenderPipeline*` suites are green.
  - size: L

## Phase 135.3: Other flakes found by M155

- [x] M135.P3.T1 — Make ViewerDisplayTransformOCIO.EightBitOutputStaysWithinOneCodeValue deterministic
  - files: the test's source (find it with `git grep EightBitOutputStaysWithinOneCodeValue`), plus whatever Engine viewer/OCIO code the cause leads to
  - approach: M155's leak agent saw it fail intermittently when run on its own, in both scheduler modes, on a local NDEBUG fast build. Its fixture does not use `BaseTest`, so M155's per-case reset doesn't cover it. Reproduce it with `--gtest_repeat` under CPU load. Find whether the failure is a timing premise in the test or a real race in the 8-bit viewer path, and fix it where it lives.
  - verify: 500/500 under load in both modes; the suite is green.
  - size: M
- [x] M135.P3.T2 — Find the CImgBlur heap corruption in CacheEquivalenceSweepTest shard 4
  - files: `Tests/` sweep shard, `Plugins/` or libs CImg Blur code (to be found)
  - approach: on the 4-core local box, shard 4 of `Shards/CacheEquivalenceSweepTest` aborts with heap corruption inside CImgBlur's OpenMP code. It reproduces on untouched main there, but passes on the remote runners. Get a backtrace and, if needed, an ASan build of the plug-in. A thread-count-dependent overrun is the first suspect. Fix it at the root.
  - verify: shard 4 passes 20/20 locally, in both modes; remote debug gate green.
  - size: L

**Verification gate:** one build per phase. Both stressed repeat loops are 500/500, and full ctest is green apart from known goldens. CI `build-and-test` is green on the PR.
- 2026-10-21 — **Scope widened to every flaky test found (user).** So far that means the DeepLayersGraphTest segfault (Phase 135.2). Renamed from "Pool Sharing Flake Fix". Claimed alongside M141 - Complete Local Build Environment and M155 - Per-Suite Test Processes. M155 is fixing shared-process leaks in four other Deep suites, so the two lanes may touch the same Deep Engine code; conflicts are resolved at merge.
- 2026-10-21 — Phase 135.3 added under the same instruction: two more flakes found by M155's leak agent (ViewerDisplayTransformOCIO 8-bit, CImgBlur heap corruption on 4 cores).
- 2026-10-21 — **Root causes.**
  - **P1:** the test's assumption was wrong, not the scheduler. Over 1,650 instrumented runs, the one leaf that got 2 threads was a tail leaf popped with the pool short (3 running, 0 ready). The test now checks each leaf against the budget inputs the scheduler recorded for it (`getLastTaskBudgetInputsForTests`).
  - **P2:** an Engine race at exit. `clearNodesBlocking` freed the effect while the render watcher started by TearDown's non-blocking destroy was still reading it, crashing in `RenderEngine::quitEngine`. The blocking destroy now waits for the node's watcher and its enclosing groups' watchers.
  - **P3.T1:** a real 8-bit viewer bug. The LUT accuracy check compared clamped values, which hid errors of up to 243 code values in cells crossing 0 or 1. It now compares those cells unclamped.
  - **P3.T2:** stale local plug-in assets, from openfx-misc before M143's EdgeExtend fix. `test.sh` now refuses assets whose stamp doesn't match the pins (`fetch-assets.sh --check-pins`).
