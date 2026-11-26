# M496 - MoonRay Test Stability

MoonRay suites fail intermittently under parallel ctest on runners and pass when rerun serially. Seen so far:
- `ProgressiveRenderTest` stalls in MoonRay cases (`AMoonRayEditRestartsInPlace`, `AnEngineChangeCutsTheTextureExportThePassWaitsOn`; watchdog abort) — M336 - Heavy Scene Acceptance's PR CI run 44940733888 and its first re-run, legacy and task-graph legs; once on MoonRay's headlight case.
- `ProgressiveRenderTest` segfault inside `UsdImagingGLEngine` construction (task-graph leg, same run).
- `USDRenderMoonRayLiveTextureTest` hitting the 300 s suite timeout under load (same run), and separately hanging after MoonRay's dispatcher exit once all 6 cases had passed in 8 s (M288 - Utility Renderer Projection Modes' gate, run 44195775643, legacy).
- `USDRenderMoonRayTest` segfault: hdMoonray's arras client thread reads a freed socket buffer while the previous render context tears down asynchronously (traced 2026-11-14, an upstream race; candidate fix: wait for the old context in teardown).

Fix the product races in our code or the MoonRay fork (M325 - MoonRay Fork: charlesangus forks of `hdMoonray`, `arras4_core`, …, pinned by `tools/ci/local/build-moonray.sh`), get stacks for the stalls first, and only then decide whether any suite also needs to run apart from others.

## Scope

**In:**
- The arras teardown race, the exit hang, the stalls and the `UsdImagingGLEngine` construction segfault, each fixed at its cause once stacks show it.
- Watchdog coverage (`Tests/TestWatchdog.h`, from M378 - Scene Render Test Stability) for the MoonRay suites that lack it, so the next stall leaves stacks.
- `RESOURCE_LOCK`/`RUN_SERIAL` in `Tests/SuiteProperties.cmake` only for a suite whose remaining failure the stacks show is CPU starvation, not a bug.

**Out:**
- KeyMix's GPU mismatch from the same run (M495 - KeyMix GPU Edge Read Fix).
- Cycles-delegate flakes (fixed in M378 - Scene Render Test Stability).
- Raising timeouts to hide a hang.

**Edge cases:**
- A stall that doesn't reproduce in 20 repeats: record it, keep the watchdog, drop its fix task (as M378 did).

## Phase 496.1: Evidence

- [x] M496.P1.T1 — Watchdog and phase reporting for the MoonRay suites
  - files: `Tests/Scene/USDRenderMoonRay_Test.cpp` (`USDRenderMoonRayTest`, `USDRenderMoonRayLiveTextureTest` fixtures), `Tests/TestWatchdog.h` (reuse, no change expected)
  - approach: start the M378 watchdog in each fixture's `SetUp` (150 s, as `ProgressiveRender_Test.cpp` does) and report phases around render, abort and teardown; extend the watchdog to cover fixture `TearDown` and static/process teardown if it doesn't already (arm it again in `TearDown`, and from an `atexit` or test-environment `TearDown` so an exit hang dumps stacks instead of running into ctest's timeout).
  - verify: the suites pass at the phase build; the watchdog death test still passes
  - size: M
- [x] M496.P1.T2 — Repeat the MoonRay suites under load to capture stacks (PM runs)
  - files: none
  - approach: `tools/ci/remote/build.sh debug --tests --repeat-test <suite>:20` for `ProgressiveRenderTest`, `USDRenderMoonRayTest` and `USDRenderMoonRayLiveTextureTest`, both schedulers; collect watchdog stacks and any core/segfault backtrace from the logs. Record which failures reproduced, with stacks, as a milestone decision.
  - verify: logs collected and summarised in `## Decisions`
  - size: S

## Phase 496.2: Fixes

- [x] ~~M496.P2.T1 — Wait for the previous MoonRay render context before tearing down its arras client~~
  - files: MoonRay fork (`hdMoonray` render delegate/render pass teardown, `arras4_core` `Client.cc` as needed), `tools/ci/local/build-moonray.sh` pin
  - approach: consultant-confirmed design first (opus): find where hdMoonray starts the new context while the old one tears down asynchronously, and make teardown join the old context's arras client thread (or keep its socket buffers alive until that thread exits) before freeing them. Commit on the fork's `natron/...` branch, then move the pin and checksum in `build-moonray.sh`.
  - verify: the fork builds in the dev image job; `ctest -R USDRenderMoonRay` passes
  - size: L
- [ ] M496.P2.T2 — Fix the hang after MoonRay's dispatcher exit
  - files: per P1.T2's stacks — likely `Engine/Nodes/Scene/MoonRayRuntime.cpp` or the fork's dispatcher/arras shutdown
  - approach: from the exit-hang stacks, find the thread that never joins (arras client, dispatcher, TBB arena) and make shutdown end it. If P1.T2 never reproduces the exit hang, record that and drop this task.
  - verify: `USDRenderMoonRayLiveTextureTest` exits promptly 20/20 in P3.T1
  - size: M
- [ ] M496.P2.T3 — Fix the ProgressiveRenderTest MoonRay stalls
  - files: per P1.T2's stacks — likely `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/MoonRayRuntime.cpp`, or the fork's `hdMoonray`
  - approach: from the stall stacks, fix the wait that never returns (a restart or texture-export wait missing a cancel or wake-up). If the stacks show only starvation (all threads progressing, slowly), put the MoonRay suites under one `RESOURCE_LOCK` in `Tests/SuiteProperties.cmake` with a comment saying why. If no stall reproduces, record that and drop this task.
  - verify: `ProgressiveRenderTest` 20/20 in P3.T1
  - size: M
- [ ] M496.P2.T4 — Fix the segfault in UsdImagingGLEngine construction
  - files: per P1.T2's backtrace — the test fixture in `Tests/Scene/ProgressiveRender_Test.cpp` or the engine setup in `Engine/Nodes/Scene/`
  - approach: from the backtrace, find what construction races with (a previous engine's teardown, plugin registry or GL context on another thread) and serialise or order it. If it never reproduces, record that and drop this task.
  - verify: `ProgressiveRenderTest` 20/20 in P3.T1
  - size: M

## Phase 496.3: Gate (PM runs)

- [ ] M496.P3.T1 — Repeat under load
  - files: none
  - approach: as P1.T2, 20 each for `ProgressiveRenderTest`, `USDRenderMoonRayTest`, `USDRenderMoonRayLiveTextureTest`, both schedulers
  - verify: 20/20 per suite per scheduler, no watchdog abort, no segfault
  - size: S

**Verification gate:** P3.T1 green on both schedulers; the dev image builds on the new MoonRay pin; full debug PR CI green.
- 2026-11-24 — Two more ProgressiveRenderTest segfaults: PR #346 (M510) task-graph leg run 45154296436, and m554-gate4 legacy leg; both passed on the in-job rerun.
- 2026-11-24 — USDRenderStormTest failed once more on m559-gate7 (task-graph leg), passing on rerun; m106-gate5 and m559-gate5 saw the same.

## Decisions
- 2026-11-24 (PM, opus consultant) — P2.T1 struck: the arras client join-before-free fix is already pinned (Dockerfile:70 NATRON_MOONRAY_COMMIT 9cc77b5, arras4_core d673d57, PR #301, 2026-11-17; Client::shutdownConnection joins mThread/mSendThread before deleting endpoints). Today's USDRenderMoonRayTest 300 s timeouts are hangs, not crashes. Hypothesis to confirm with stacks: endUpdate holds ArrasRenderer::mMutex while reconnect joins the delivery thread, which waits on mMutex in messageHandler. rep2 repeats USDRenderMoonRayTest and ProgressiveRenderTest ×20 with the watchdog to capture stacks before P2.T2/P2.T3.
- 2026-11-25 (PM) — rep3/rep4 (watchdog build, 6c18ea6ad): USDRenderMoonRayTest ×20 and ProgressiveRenderTest ×20, 4 at a time, passed 20/20 on both schedulers — no hang reproduced under that load, so no stacks yet. The timeouts keep appearing in other branches' full ctest gates (-j4 with the whole suite), which lack the watchdog. Options: land the P1.T1 watchdog on main early so every gate dumps stacks on a stuck MoonRay test, or repeat under heavier load. rep4's task-graph leg also crashed in USDRenderCyclesSubdivisionTest (SEGFAULT) during the normal ctest run — a new sighting.
- 2026-11-25 (PM) — New evidence against the P2.T1 strike: M434 gate2 (run 45535986152, task-graph leg) segfaulted in RenderDelegates.AbortingAMoonRayRenderMidPassReturnsWithinASecond on MoonRay's client thread: arras4::network::BufferedSource::nextFrame ← MessageReader::read ← Client::deliverMessages ← Client::threadProc, right after 'Message Dispatcher … exiting: socket was disconnected'. That is the freed-socket-buffer race P2.T1 described, still happening with the d673d57 join fix pinned — possibly the self-join-guard path the consultant flagged (disconnect() from the delivery thread skips the join and frees endpoints under a live thread). P2.T1 needs reopening with this stack.
- 2026-11-25 — **Flake diagnostics on main first (user):** land the watchdog (P1.T1) on main now so every gate and PR CI run dumps stacks on the known flakes, ahead of the fixes. Branch milestone/m496-flake-diagnostics off main with 6c18ea6ad cherry-picked (c4f6ae1db), extended to a process-wide fatal-signal dump and a per-test stall dump that fires before ctest's 300 s timeout; its own PR into main. This milestone's branch merges main afterwards.
- 2026-11-25 (PM, user) — Picked up orphan PR #313 (fix/moonray-livetexture-exit-hang, no board row): ~AppManager releases CPU render engines and ends leftover MoonRay execComp children before exit, the suspected cause of the USDRenderMoonRay* 300 s timeouts and the 'shutdown stalled 30 s' exits. Main merged in (20338b65c), gate and Claude review running; it lands on main on its own PR.
- 2026-11-25 (PM) — The flake diagnostics (#380: fatal-signal and pre-timeout stack dumps) and #313 (MoonRay children end with the app) are on main. Next: collect stacks from the next MoonRay timeouts or segfaults in any main-based gate or PR CI, then take P2.T2-T4 from them. If #313 alone stops the USDRenderMoonRay* timeouts, note that against P2.T2.
- 2026-11-26 (PM, opus consultant) — **First watchdog stacks:** USDRenderMoonRayTest.PlainTexturesMatchTheirMaketxConversion stalled 150 s in 'render' on f16-gate3. The dump is at `.local/tmp/recover/m496-stall-f16gate3.txt`.
  - **Cause:** a Natron-side livelock, not a mutex deadlock. HydraRenderImpl's renderEngine polls IsConverged() every 10 ms with no liveness bound (`:1675-1738`; MoonRay has no restart watch, so the kRestartUnseen remake never fires).
  - **Why it never converges:** hdMoonray's ArrasRenderer never sees a FINISHED frame (the Arras client sits idle in recv), and the session's death or stall isn't reported through sessionFailure.
  - **Ruled out:** the earlier endUpdate-mutex hypothesis.
  - **Leading cause of the silence:** endUpdate returns early when !isEngineReady(), leaving the update pending. A stall in execComp's prep is also possible; it needs the child's stacks.
  - **Fix:**
    - a MoonRay no-progress bound (GetRenderStats signature unchanged for 30 s → remake the engine once, then fail with "MoonRay's render session stopped answering"), in the refine path too, for P2.T3;
    - hdMoonray status for "waiting for engine";
    - the watchdog dumps execComp children's stacks and the last render stats;
    - a SIGSTOP-execComp test proving the bound.
  - P2.T2 (exit hang) stays open on its own evidence.
- 2026-11-25 (PM) — Liveness fix committed (5cb591fb10), gate m496-gate5 (NO_GUI, task-graph). MoonRay-only no-progress bound in renderEngine and the progressive path (30 s, `NATRON_DELEGATE_STALL_S`; one engine remake, then "MoonRay's render session stopped answering"); the watchdog dumps execComp children (eu-stack/gdb, else thread state and wchan when ptrace is refused); new test `ARenderProcessThatStopsAnsweringFailsTheRender` SIGSTOPs execComp. Open risks: MoonRay may not update stats within 30 s during heavy-scene prep; teardown with a stopped execComp may block. Proposed hdMoonray patch (not applied): in `plugin/hd_moonray/ArrasRenderer.cc:379`, when connected but the engine isn't ready, set `mStatusString = "Waiting for engine"` before returning, so a never-ready engine shows in the status.
- 2026-11-25 (PM) — m496-gate5 green (run 45736916501, task-graph): the new `ARenderProcessThatStopsAnsweringFailsTheRender` passes in 8.2 s, ProgressiveRenderTest and all USDRenderMoonRay* suites pass. P2.T3 checked off. P3.T1 repeat run m496-rep5 queued (the three suites ×20, task-graph only since the legacy scheduler is being removed).
- 2026-11-25 (PM) — P3.T1 repeats on 5cb591fb10 (one suite per run, since test.sh repeat takes one case):
  - **USDRenderMoonRayLiveTextureTest:** 20/20 (rep8).
  - **USDRenderMoonRayTest** (rep7): failed at iteration 4 of 20. The new stall test lost its race under 4× parallel load: the process was stopped before it connected, so the session timed out instead of the bound firing. The next test then segfaulted in arras `Client::localTermination` ← `LocalSession::onTerminate` ← `ProcessManager::handleChildExit` on the exit-monitor thread while a new session was being created, likely a late child exit reported to a destroyed client. Being fixed (stop only once rendering; always continue and reap; a Natron-side guard if sound; arras fork patch written up).
  - **ProgressiveRenderTest:** its own repeat (rep6) is still running. In rep8's normal ctest leg it timed out once (passed on the serial rerun): a detached progressive worker sat in `ProgressiveRender::Implementation::refine` on a bare `condition_variable::wait` while Cycles' session thread waited for work, and `waitForDetachedWorkers` gave up after 60 s. That is a Cycles-path wait with no wake on detach, separate from the MoonRay bound; P2.T3 reopened for it. Stack: `.worktrees/m496/build/rep8-progressive-stall.txt`.
- 2026-11-25 (PM) — rep6: ProgressiveRenderTest 20/20 under load on 5cb591fb10 (task-graph). The Cycles refine-wait hang seen in rep8's normal leg did not recur in the repeats; P2.T3 stays open for it.
- 2026-11-25 (PM) — f6bd4f5f61: the stall test stops a process only once MoonRay reports progress (`HydraRender::getMoonRayProgressReads()`), and a scope guard always resumes and reaps. **rep7 segfault cause (arras4_core d673d57):** when `LocalSession::start()` throws after spawning (20 s connect timeout, `readRegistration`), the session is never registered, its process keeps running, and the session stays alive through `ProcessManager → Process::mObserver → LocalSession`; its terminate callback holds the Client's raw `this`, so the orphan's later exit calls `Client::localTermination` on a freed Client. Any real connect timeout can crash Natron later the same way. No sound Natron-side guard. **PM call:** fix it in the fork (abandon + stop on every post-spawn failure), together with the hdMoonray "Waiting for engine" status, as one MoonRay pin bump in M496. The openmoonray branch tip c4edab6 carries an unpinned moonray deep-output bump from another lane, so the M496 super-repo commit goes on a side branch off ee4955e (`natron/v2026.29.1-m496`), and whichever lands second merges the two. Cycles progressive refine hang (P2.T3) being fixed.
- 2026-11-25 (PM) — Fork fixes pushed: arras4_core 3b3e189d9a (`LocalSession::start` abandons and stops the spawned process on any post-spawn failure; `abandon()` clears the terminate callback under its mutex, so no callback reaches a freed Client), hdMoonray 334633efd1 ("Waiting for engine" while connected and the engine isn't ready; an IDLE backend stat leaves it showing until a non-idle stat or frame). Super-repo `natron/v2026.29.1-m496` f57c136d3e off ee4955e; Dockerfile pin bump in the M496 lane follows (image rebuild). Not covered: `spawnProcess()` itself throwing leaves `mConnectThread` joinable (follow-up if seen); `Process::mObserver` stays a shared_ptr.
- 2026-11-25 (PM) — P2.T3 Cycles hang cause: a lost wakeup in `refine`. The thread-count check drops the mutex around `askThreads`; `~ProgressiveRender` set `quit` and notified inside that window, and the worker then went into the bare done wait without reading `quit`. Fixed in a42fd97fef (every bare wait in `refine` is guarded by `!quit`; no timeouts added) with `ARenderEndedWhileItsWorkerAsksForThreadsEndsItsWorker`. m496-gate6 (NO_GUI, rebuilds the image for the MoonRay pin 909f699eae) queued; P3.T1 repeats follow.
- 2026-11-25 (PM) — m496-gate6 green on a42fd97fef with the rebuilt image natron-dev:9d7eb361a806cc8c (MoonRay pin f57c136d3e): USDRenderMoonRay* suites pass, `ARenderEndedWhileItsWorkerAsksForThreadsEndsItsWorker` passes. P3.T1 repeats rep9-11 (one suite each, ×20) queued.
