# M380 - Blocking Scheduler Aborts

`GenericSchedulerThreadPrivate::waitForAbortToComplete_internal` (`Engine/GenericSchedulerThread.cpp`) returns at once because its condition is always true (upstream since 1070d8189). The audit (consultant, 2026-11-10) found one live caller that would hang once fixed (a blocking writer render's failure path waits on itself), one destructor wait that can hang on a `Stopped` thread, a main-thread hazard in the viewer schedulers (workers still request main-thread execution after an abort), and several dead blocking APIs. M219's `workMutex` (F8 only) must stay as a data guard; only its wait role is replaced.

## Scope

**In:**
- Make every live caller safe and delete dead blocking-abort paths before or with the fix.
- Make the wait correct (Aborted, Idle or Stopped; never from the worker's own thread; main-thread waiters service pending main-thread requests).
- A unit test and per-worker abort-latency checks.
- Replacing VectorFollowTask's lock-as-wait on F8.

**Out:**
- Quit-path waits (they already block on their own conditions) and the preview abort (own condition variable).

**Edge cases:**
- `notifyRenderFailure` runs on the scheduler thread itself or on render threads: it never waits; `BlockingBackgroundRender` already waits on `_runningCond`.
- `~ViewerCurrentFrameRequestScheduler`: the member's `~GenericSchedulerThread` already quits and joins.
- Deleting `abortAnyProcessing_blocking` makes F8's merge drop its `abortFollow(true)` line (F8 Node.cpp ~3536): a trivial conflict.

## Phase 380.1: Callers and the wait

- [x] M380.P1.T1 — Remove the waits that would hang once the wait is fixed, and delete the dead blocking-abort API
  - files: `Engine/OutputSchedulerThread.{cpp,h}`, `Engine/Node.{cpp,h}`, `Engine/TrackerContext.{cpp,h}`, `Engine/GenericSchedulerThread.{cpp,h}`, `Engine/GenericSchedulerThreadWatcher.{cpp,h}`, a test
  - approach: drop the wait in `OutputSchedulerThread::notifyRenderFailure` (~2048-2050) and the backup-thread abort wait in `~ViewerCurrentFrameRequestScheduler` (~4120-4122). Delete `Node::abortAnyProcessing_blocking`, `TrackerContext::abortTracking_blocking`, `waitForAbortToCompleteQueued_main_thread`, `RenderEngine::waitForAbortToComplete_main_thread` and `engineAborted`, `onWatcherTaskAbortedEmitted`/`taskAborted`, and the watcher tasks `eBlockingTaskWaitForAbort` and `eBlockingTaskAbortAnyProcessing`. Behaviour is unchanged while the wait is still a no-op.
  - verify: `BlockingRenderIdle_Test`, `SchedulerWriters_Test` and `RenderSchedulerAbort_Test` pass; a new case where a blocking writer render fails partway returns.
  - size: M
- [x] M380.P1.T2 — Make the abort wait really block
  - files: `Engine/GenericSchedulerThread.{cpp,h}`, `Tests/GenericSchedulerAbortWait_Test.cpp` (new, registered in `Tests/CMakeLists.txt`)
  - approach: the early return becomes `state ∈ {Aborted, Idle, Stopped}`. On `run()`'s `Stopped` exit, clear `abortRequested` and wake `abortRequestedCond`. Return false when called from the scheduler's own thread. A main-thread waiter runs a pending `executeOnMainThread` itself: the worker wakes `abortRequestedCond` after posting `executionOnMainThreadRequested`, and a request ID makes the late queued slot a no-op (no assert at ~655).
  - verify: the new test covers (a) a worker holding heap state across the abort, freed after the wait (flag-checked); (b) quit then wait returns; (c) waiting on the worker's own thread returns false; (d) a main-thread wait on a worker that requests main-thread execution after the abort returns. Each wait has a watchdog timeout.
  - size: M

## Phase 380.2: Abort latency

- [x] M380.P2.T1 — Abort latency check per worker type
  - files: `Tests/GenericSchedulerAbortWait_Test.cpp` or `Tests/RenderSchedulerAbort_Test.cpp`, `Tests/TrackerLayer_Test.cpp`, and the worker whose polling misses the budget
  - approach: time `abortThreadedTask` to the wait returning while a slow abort-polling effect is mid-render, for the writer (`DefaultScheduler`), viewer playback, the current-frame scheduler plus backup thread, and `TrackScheduler`; generous budget (500 ms) for CI noise. Fix a worker's abort polling here if it misses.
  - verify: the tests pass in PR CI with debug asserts.
  - size: M

## Phase 380.3: M219 workaround

- [ ] M380.P3.T1 — Replace VectorFollowTask's lock-as-wait with the real blocking wait
  - files: `Engine/VectorFollowTask.cpp` (on `feature/f8-motion-vectors`: a PR into F8, or after F8 merges into main)
  - approach: replace the lock at ~202-204 with `waitForAbortToComplete_enforce_blocking()` and drop the comment at ~200-201. Keep `workMutex` and the generation check: they guard run/sampler/job/generation (written on the main thread while the phase reads them) and queued phases. Record that reason in a comment and under Decisions.
  - verify: F8's `VectorFollow_Test`/`PyVectorFollow_Test` and the `m219_follow_abort.py` GUI check pass.
  - size: M

**Verification gate:** one PM build plus ctest; the new abort-wait and latency tests green; PR CI (asserts) green; a GUI smoke on Xvfb (viewer scrub, playback stop, closing a project during playback, deleting a viewer) with no hang. P3 goes through F8's CI.

## Decisions

- 2026-11-10 — Elaborated at claim from a per-caller audit (consultant). Most callers named in the stub are dead code and are deleted. Previews don't use this wait. M219's `workMutex` lives only on F8 and stays as a data guard.
- 2026-11-10 — Phase 1 verified: remote debug build with tests green at 364fac688 (run 42796566643), including `GenericSchedulerAbortWait` and the failing-blocking-render case.
- 2026-11-10 — `SchedulerAbortLatency.ViewerCurrentFrameAbortReturnsWithinBudget` hit a SIGFPE in OCIO's CPU apply on a render thread in the debug build. The viewer display transform and the colour-picking applies now run under `FpTrapGuard` (a8a7c8890). Likely cause: an aborted render leaves tiles unfilled and the viewer colour-processes them. Unconfirmed; if confirmed, the viewer reading unwritten pixels after an abort is its own bug, for an E14 stub.
- 2026-11-10 — The current-frame latency test fired the default abort (`keepOldestRender = true`). By design that leaves the one in-flight viewer render running for scrubbing, so the test waited out its 15 s hook cap. It now fires the full abort (7dd7b0e90). Follow-up for /cat-plan (E14), given the renders-cancel-fast rule: `Gui::abortAllViewers` → `Node::abortAnyProcessing_non_blocking`, which `AppManager::clearAllCaches` and `SceneWriteBarrier::abortViewers` use, also keeps the oldest render. So a lone in-flight current-frame render isn't cancelled on those invalidating actions; they likely want `abortRenderingNoRestart(false)`.
