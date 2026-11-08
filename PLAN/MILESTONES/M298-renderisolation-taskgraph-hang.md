# M298 - RenderIsolation Taskgraph Hang

The `RenderIsolation` gtest sometimes hangs on the taskgraph ctest leg and is killed at 300 s; ctest's re-run then passes in about 1 s. Seen on two unrelated branches (M271 on 2026-10-28, M190 on 2026-11-01), so it is on main and dates from M177 - Renders Isolated From Edits or later.

In both logs a blocking Write render (CheckerBoard → Grade → Write) refuses an edit made through `renderWithActionAtFrame` at frame 3, reports frames up to about 12, and then no frame completes. Every render thread, or the thread that reports frames, stalls. The likely cause is a lock-order problem or a missed wakeup between the render threads, the scheduler and the Python GIL after the refused edit.

## Scope

**In:**
- Reproduce in natron-dev with the taskgraph scheduler: `Tests --gtest_filter=RenderIsolation.* --gtest_repeat=500`.
- Take thread stacks of a hung run with `eu-stack` (needs `-u root --privileged`).
- Fix the deadlock, and add a regression check that repeats the scenario.

**Out:** other RenderIsolation behaviour.

Stub: to be broken down into tasks when claimed.

## Phase 298.1: Diagnose and guard

- [x] M298.P1.T1 — Diagnose the taskgraph RenderIsolation hang
  - files: `Engine/OutputSchedulerThread.cpp`, `Engine/RenderScheduler.cpp`, `Engine/IsolatedJob.cpp`, `Tests/RenderIsolation_Test.cpp`
  - approach: the logged hang (M271 2026-10-28, M190 2026-11-01) matches the render-thread count race fixed by M321 - Render Thread Count Race (c30dff64c, 2026-11-03): two finishing frame threads both added a thread past the cap, quitting threads were still counted, and every later finish stopped another live thread until none was left, leaving queued frames unrendered. The refused edit at frame 3 only widened the window. Current code reviewed for other lost wakeups or pool deadlocks; none found.
  - verify: reasoning recorded here; no product change.
  - size: M
- [x] M298.P1.T2 — Render deadline in the test helper and a stress test
  - files: `Tests/RenderIsolation_Test.cpp`
  - approach: `renderWithActionAtFrame` takes an optional deadline (abort, then `kRenderTimedOut`); `RenderIsolation.RefusedEditRenderStress` loops the refused-edit render under taskgraph with a parallel-render cap below the pool size, within ~50 s worst case.
  - verify: remote gate, both ctest legs.
  - size: S
