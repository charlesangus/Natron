# M170 - Engine Render Races

This milestone fixes two engine races that surfaced as test flakes. In the first, `BlockingBackgroundRender::blockingRender` returns before the Write's scheduler thread has gone idle. During that gap the Write still counts as rendering a sequence, so read-back is off and renders on the Write abort. M138 - GUI Papercuts papered over this in `NativeWriteReadBackTest`. In the second, `AbortableRenderInfo` creates a `QTimer` on whichever thread builds it, and calls `deleteLater()` on that timer from its destructor. When the creating thread is a pool thread that has exited, this crashes inside `QEventDispatcherGlib::wakeUp`. That crash is what M80 - Native RAW Support's debug CI hit in `SchedulerEquivalence.NodeFeedingSourceAndMaskIsTwoEdges` (run 40033444348). Each phase brings a deterministic regression test where one is possible, the engine fix, and a parallel soak that proves the flake is gone. Filed 2026-10-24 from two inbox reports (M138 - GUI Papercuts, 2026-10-24; M80 - Native RAW Support, 2026-10-23).

## Scope

**In:**
- Blocking renders return only once the Write has stopped sequential rendering, or once its next queued sequence has legitimately started.
- An audit of every caller that inspects a Write right after `startWritersRendering`. That includes the afterRender callback, which `DefaultScheduler::onRenderStopped` runs *after* `notifyRenderFinished()` (`Engine/OutputSchedulerThread.cpp` ~:2823-2840).
- Removing M138's polling workaround (`Tests/Native/NativeWriteReadBack_Test.cpp` ~:221-231).
- Making `AbortableRenderInfo` timer ownership safe on any thread, and deciding whether the abort timeout should start working for renders created on pool threads.

**Out:**
- Reworking `GenericSchedulerThread`'s state machine beyond what the idle wait needs.
- Non-blocking GUI render paths (`Gui/RenderDialog.cpp`, `Gui/ProgressTaskInfo.cpp`). They do not wait on `BlockingBackgroundRender`.

**Edge cases:**
- Queued sequences: `OutputEffectInstance::notifyRenderFinished` (`Engine/OutputEffectInstance.cpp` ~:356-373) calls `launchRenderSequence` for the next request from the scheduler thread. That thread then stays active, so a plain "wait for idle" would hang.
- Single-thread mode: when `getNumberOfThreads() == -1`, `blockingRender` clears `_running` straight away (`Engine/BlockingBackgroundRender.cpp` ~:64-65). The new wait must not block there.
- Aborted or failed sequences must also release the wait. The scheduler goes to `eThreadStateAborted` or `eThreadStateStopped` instead of `eThreadStateIdle` (`Engine/GenericSchedulerThread.cpp` ~:515-527).
- Most `AbortableRenderInfo::create` call sites run on pool threads with no event loop (`Engine/Node.cpp` ~:4670, `Engine/NodeInputs.cpp` ~:1615, `Engine/EffectInstance.cpp` ~:1160/5606, `Engine/NodeColorSampler.cpp` ~:179). On those threads the timer `deleteLater()` never runs, so each info also leaks a `QTimer`.

**Assumptions:**
- `RenderEngine::isDoingSequentialRender()` is simply `scheduler->isWorking()` (`Engine/OutputSchedulerThread.cpp` ~:3528). "Idle" therefore means the scheduler thread has left `eThreadStateActive`.
- `onAbortTimerTimeout` already returns early for readers and writers (`Engine/AbortableRenderInfo.cpp` ~:262). A working timeout would only report stalled non-IO plug-ins.

## Phase 170.1: BlockingBackgroundRender idle race

- [x] M170.P1.T1 — Make `blockingRender` wait until the scheduler goes idle or the next queued sequence starts, and add a deterministic regression test
  - files: `Engine/BlockingBackgroundRender.cpp`, `Engine/BlockingBackgroundRender.h`, `Engine/GenericSchedulerThread.cpp`, `Engine/GenericSchedulerThread.h`, `Tests/BlockingRenderIdle_Test.cpp` (new; add it to the Tests CMake list)
  - approach: Add a state-change condition variable to `GenericSchedulerThread`, signalled where `setThreadState` runs (~:124), plus a waiter `waitUntilNotActive(predicate)`, so the wait does not poll. After `_running` goes false, `blockingRender` waits until one of two things holds: `!_writer->isDoingSequentialRender()`, or the writer's `_renderSequenceRequests` holds a request that is not ours, meaning the next sequence has started and the Write really is busy. Expose that second check as a small `OutputEffectInstance` accessor taken under `_outputEffectDataLock`. Skip the wait when `numberOfThreads == -1`. Add a test-only static hook that sleeps between `threadLoopOnce` returning and the idle transition (~:510-527) to widen the gap. The test turns the hook on (200 ms), runs a one-frame blocking render on a NativeWrite, then asserts `!isDoingSequentialRender()` and `readBackActive()` immediately. Add a second case that queues two sequences on one Write and asserts both return and no wait hangs (30 s guard).
  - verify: `tools/ci/remote/build.sh debug --tests`. The new test passes, and fails when the `blockingRender` wait is reverted locally (note that run in the PR). It also passes with the hook on under `--repeat until-fail:50`.
  - size: L

- [x] M170.P1.T2 — Audit the code that inspects a Write right after `startWritersRendering`, and make the afterRender callback run before a blocking render returns
  - files: `Engine/OutputSchedulerThread.cpp`, `Engine/AppInstance.cpp`, `Engine/PyAppInstance.cpp`, `Engine/DiskCacheNode.cpp`, `Engine/PrecompNode.cpp`
  - approach: Go through each caller (`Engine/AppInstance.cpp` ~:680/683, `Engine/Node.cpp` ~:6635, `Engine/DiskCacheNode.cpp` ~:170, `Engine/PrecompNode.cpp` ~:714, `Engine/PyAppInstance.cpp` ~:343/390, plus the `Tests/` callers). Record which ones read Write state or results afterwards. In `DefaultScheduler::onRenderStopped`, move `effect->notifyRenderFinished()` after the afterRender Python callback. That way a blocking `app.render()` returns only after its callback has run, and the T1 wait covers the rest. List the audit results in the PR body. Fix only real ordering bugs here, and spin off anything larger.
  - verify: Add a gtest that registers an afterRender callback setting a flag, runs a blocking render, and asserts the flag is set as soon as the render returns. Run `tools/ci/remote/build.sh debug --tests`.
  - size: M

- [x] M170.P1.T3 — Remove M138's `isDoingSequentialRender` polling from `NativeWriteReadBackTest` and soak the suite
  - files: `Tests/Native/NativeWriteReadBack_Test.cpp`
  - approach: Delete the deadline loop and its comment in `writeSequence()` (~:221-231), keeping a single `ASSERT_FALSE(effect->isDoingSequentialRender())` as the contract check. Remove the `<chrono>` and `<thread>` includes if nothing else uses them.
  - verify: Rerun the M138 reproduction, 480 suite runs with 8 parallel copies on 4 cores, against a debug build: `ctest -R NativeWriteReadBack --repeat until-fail:60` in 8 concurrent shells. Expect 0 failures, compared with the baseline of about 58 in 480.
  - size: M

## Phase 170.2: AbortableRenderInfo timer ownership

- [x] M170.P2.T1 — Decide who owns the abort timer, and get the behaviour change approved by the user
  - files: `Engine/AbortableRenderInfo.cpp`, `Engine/AbortableRenderInfo.h` (read only; the decision goes in this milestone's `## Decisions`)
  - approach: Compare three options and recommend one. (a) Create the timer lazily inside `setAborted()` only, owned by the main thread (`moveToThread(qApp->thread())`), and start and stop it through queued calls to the main thread. This fixes the crash and makes the timeout work for pool-created renders. (b) Have one app-wide watchdog on the main thread that scans aborted renders older than `NATRON_ABORT_TIMEOUT_MS`, with no `QTimer` per info. (c) Keep today's inert behaviour on pool threads: use no timer when the creating thread has no event loop, and delete the timer directly in the destructor. For each option, state the user-visible change: does the stalled-render dialog now appear for plug-ins that stall while being aborted from viewer or pool renders? Also state the cost under heavy abort churn on many-core machines, and the behaviour in background mode, where there is no dialog.
  - verify: The user approves the chosen option before T2 starts, and the decision is recorded.
  - size: M

- [x] M170.P2.T2 — Make the timer's lifetime safe on any thread, per the T1 decision, with a deterministic thread-exit test
  - files: `Engine/AbortableRenderInfo.cpp`, `Engine/AbortableRenderInfo.h`, `Tests/AbortableRenderInfo_Test.cpp` (new; add it to the Tests CMake list)
  - approach: Implement the approved option. Remove the eager `new QTimer` from `AbortableRenderInfoPrivate` (~:87), and make the destructor (~:107-113) safe when the creating thread has exited. Remove the `deleteLater()` in `onAbortTimerTimeout` that nulls the pointer, which races with the destructor (~:229-230), or put it under `timerMutex`. Tests, all deterministic: (1) create an info on a `QThread` with no event loop, end the thread, then drop the last `shared_ptr` on a second thread. (2) Do the same with `setAborted()` called from a third thread before the drop. (3) Loop 10k create/abort/destroy cycles across `QThreadPool` resizes 16/4/1, as `Tests/SchedulerEquivalence_Test.cpp` ~:303 does.
  - verify: `tools/ci/remote/build.sh debug --tests`. The new test passes, and case 1 or 3 crashes when the old destructor is restored locally (note this in the PR). Run `SchedulerEquivalence` with 8 parallel copies and `--repeat until-fail:100`: no segfault.
  - size: L

- [x] M170.P2.T3 — Test the abort-timeout behaviour that T1 chose
  - files: `Engine/AbortableRenderInfo.cpp`, `Engine/AbortableRenderInfo.h`, `Tests/AbortableRenderInfo_Test.cpp`
  - approach: Make the timeout injectable for tests, with a static override of `NATRON_ABORT_TIMEOUT_MS`. Under option (a) or (b), assert three things. A render created on a pool thread, aborted, and kept registered past the timeout reaches `onAbortTimerTimeout` on the main thread. Unregistering every thread before the timeout suppresses it. Readers and writers never report. Under option (c), assert that pool-created infos never start a timer, and that main-thread ones still do.
  - verify: Run `tools/ci/remote/build.sh debug --tests`. The test passes with `--repeat until-fail:50`.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests` passes all of ctest. `NativeWriteReadBackTest` shows 0 failures in 480 suite runs with 8 parallel copies on 4 cores, with M138's workaround removed. `SchedulerEquivalence` and `AbortableRenderInfo_Test` show 0 failures in 100 repeats with 8 parallel copies.

## Decisions

- 2026-10-24 — **P1.T1 revert proof (PM):** a debug build with the `blockingRender` wait removed (scratch branch, since deleted) fails `BlockingRenderIdleTest.TheWriteIsIdleAsSoonAsABlockingRenderReturns` and `TwoSequencesQueuedOnOneWriteBothReturn` on both ctest legs; the lane's own build passes them, and they passed 104 soak runs with the idle-transition hook on.
- 2026-10-24 — **Gate soak results (PM):** on the debug build (32543a146, 391/391 on both ctest legs), 8 parallel copies on 4 cores in natron-dev: NativeWriteReadBack 480 runs, AbortableRenderInfo 104, BlockingRenderIdle 104, SchedulerEquivalence 104. Every one of the 792 gtest runs passed (M138 baseline: about 58 failures in 480 NativeWriteReadBack runs). 79 runs exited non-zero only because `xvfb-run -a` failed to kill an already-gone Xvfb under 8-way contention; each of those logs ends in `[  PASSED  ]` with no failure, and 20 sequential runs of the three suites all exited 0.
- 2026-10-24 — **P2.T2 revert check replaced (PM):** the old per-render `QTimer` destructor cannot be restored under the new test, because the test drives `AbortWatchdog` hooks and `getStallReport`, which do not exist in the old design. The crash path (a `QTimer` owned by an exited thread) is gone by construction. The thread-exit cases plus the soak stand as the evidence. P1.T1's revert run (blockingRender wait removed) is on `scratch/m170-revert-wait`.
- 2026-10-24 — **User decision: one app-wide abort watchdog (option b).** `AbortableRenderInfo` loses its per-render `QTimer`, `ownerThread`, `timerMutex` and the start signal. `setAborted()` adds a weak pointer and a timestamp, under a mutex, to one main-thread watchdog, whose timer runs only while that list is non-empty. One scan can raise a single merged stalled-render dialog and skips renders that already have one open. This removes the `QEventDispatcherGlib::wakeUp` crash by design, along with the race between `onAbortTimerTimeout` and the destructor. The stalled-render dialog now works for renders created on pool threads: it fires 5 s after an abort when a non-IO plug-in is still inside an action. Background mode is unchanged: there the main thread is blocked and the watchdog only logs. Rejected: (a) a per-render main-thread timer, which costs more under abort churn and can stack dialogs; (c) leaving pool renders inert, which keeps the two-thread design.
