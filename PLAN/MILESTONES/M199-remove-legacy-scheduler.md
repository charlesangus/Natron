# M199 - Remove Legacy Scheduler

Delete the Legacy render scheduler mode (`renderSchedulerMode`, `Engine/Settings.cpp:391`, and `NATRON_RENDER_SCHEDULER=legacy`), leaving the task graph from M63 - Task-Graph Render Scheduler as the only path. Two pieces of M63 scaffolding go with it: per-effect thread-local frame args, which the task graph fills on demand from `FrameRenderContext`, and the per-frame `RenderThreadTask` QThreads that only block on the scheduler in task-graph mode, plus the dead `processFrame` path. Moved from M31 - Architectural Cleanup (cancelled 2026-10-25).

## Scope

**In:**
- The Legacy mode is gone: `RenderSchedulerModeEnum` (`Global/Enums.h:549`), the `renderSchedulerMode` knob (`Engine/Settings.cpp:396-413,1947,2762,3028`), the env parsing and API (`Engine/AppManager.cpp:616,1204,3521-3616`, `AppManager.h:477-510`, `AppManagerPrivate.*`), and every mode branch (`Engine/OutputSchedulerThread.cpp:1657,2593,4499`, `Engine/ViewerInstance.cpp:2855,4139`, `Engine/Nodes/Loop/LoopBegin.cpp:450`, `Engine/Nodes/Loop/LoopEnd.cpp:293` `foldsAsTasks()`).
- CI runs one ctest leg: `.github/workflows/remote-build.yml`, `.github/workflows/ci.yml`, `tools/ci/remote/build.sh`, `tools/ci/local/test.sh`, `tools/ci/smoke_test.py`, with their READMEs.
- Tests stop switching modes. The both-ways helpers compare the direct `renderRoI` pull against the scheduler, not Legacy against Task graph.
- Frame args come from the `FrameRenderContext` installed on the thread. `EffectTLSData::frameArgs` is deleted.
- The scheduler owns frame admission for writers and viewer playback. `RenderThreadTask` and the dead `DefaultScheduler::processFrame` are deleted.
- Live docs and bench scripts that mention the setting or the env var.

**Out:**
- The trimap, which M201 - Trimap Removal owns once this lands.
- RenderEngine ownership, which M198 - RenderEngine On Node owns.
- The recursive `renderRoI` path that task-graph fallbacks still run (deep outputs, `taskGraphIneligibility`, viewer deep input), and renaming the `incLegacyFallbacks` stats (`Engine/RenderStats.*`). Not Legacy mode; no milestone yet.
- Historical records: `tools/bench/BASELINE.md`, `docs/perf/interactive-edit-attribution.md`.
- `FrameView_compare_less`: already a strict weak ordering (`Engine/ParallelRenderArgs.h:376-395`), so the carried fix is moot.

**Edge cases:**
- A settings file still holding `renderSchedulerMode`: the unknown knob is ignored on load. No backward compatibility.
- `NATRON_RENDER_SCHEDULER` still set in a user's environment: silently ignored.
- Native goldens and parity refs were recorded with `NATRON_RENDER_SCHEDULER: legacy` (`remote-build.yml:245,296`). If they now differ only by rounding, re-record them. Rounding diffs never reopen the design.
- Nested `ParallelRenderArgsSetter`s (knobChanged analysis while `timelineGoTo` sets viewer args): the stack of frame contexts must restore the outer one, as the `frameArgs` list does now.
- Code that reads `AppTLS::currentFrameContext() != 0` as "on a scheduler task" must keep that meaning once non-scheduler renders install a context too (P2.T1).
- Sequential-preference writers (`OutputSchedulerThread.cpp:1213`): `beginSequenceRender` and ordered completion survive the feeder removal.

**Assumptions:**
- Unconfirmed: with no frame context, a direct `renderRoI` pull in task-graph mode renders the same pixels as the Legacy pull it replaces in the test helpers. P1.T2 checks this before the reference changes; if wrong, the alternative (golden fixtures) goes to the user.

## Phase 199.1: Delete the Legacy mode

- [x] M199.P1.T1 — Run one task-graph ctest leg in CI and the gate scripts
  - files: `.github/workflows/remote-build.yml` (input `schedulers` :65-69; `NATRON_RENDER_SCHEDULER: legacy` in the golden/parity record steps :245,:296; `ctest-legs` job :500-526; matrix and `matrix.scheduler` uses :529-728; cross-scheduler compare :731-776), `.github/workflows/ci.yml` (matrix :340-453; `build-and-test` compare :456-493), `tools/ci/remote/build.sh` (`--scheduler`, `SCHEDULERS` :8-55,120,163,201-207,484,668-670), `tools/ci/local/test.sh` (:13,19,227), `tools/ci/smoke_test.py` (`_compare_or_stash_render` :553-580, `_report_render_scheduler_mode` :986-1018), `tools/ci/local/README.md` (:385,408), `tools/ci/remote/README.md` (:66-72,91,123)
  - approach: Drop the matrix and the `schedulers` input. Keep one ctest/smoke job and keep the check name `Build and test`. Artifact and log names lose the scheduler suffix; `tools/ci/summary.sh` gets one fixed label. Remove the record steps' Legacy env. `build.sh --scheduler` becomes an unknown option. Remove the smoke reference-dir comparison and the mode report. The engine still accepts the env var until P1.T4.
  - verify: actionlint/`yq` parse both workflows; `build.sh --help` no longer lists `--scheduler`; a `build.sh debug --tests` gate shows one ctest leg, green; a PR CI run shows one Test job and a green `Build and test`.
  - size: M

- [x] M199.P1.T2 — Make the both-ways test helpers mode-free
  - files: `Tests/RenderBothWays.h`, `Tests/RenderBothWays.cpp`, `Tests/RenderBothWaysSelf_Test.cpp`, `Tests/CacheStress.h`, `Tests/CacheStress.cpp`
  - approach: The reference pass becomes the direct `renderRoI` pull with no frame context, and no mode switch. For `renderBothWays` (writer), the reference renders the writer's input per frame and view directly and compares to the task-graph EXRs. Rename `RenderMismatch::legacy` to `reference`. Replace `renderNodePlanesInMode(mode)` with `renderNodePlanesScheduled()`, keep `renderNodePlanesDirect()`. For one transition commit the old overload forwards to the new ones. CacheStress loses its mode parameter the same way.
  - verify: `RenderBothWaysSelf_Test` still catches a planted difference; the `renderBothWays`/`renderBothWaysDirect` suites (e.g. `Tests/Native/CacheEquivalence_Test.cpp`, `Tests/SchedulerEquivalence_Test.cpp`) are green at the phase build.
  - size: L

- [ ] M199.P1.T3 — Move every test off the scheduler-mode API
  - files: every `Tests/` file naming `RenderSchedulerMode`/`SchedulerModeGuard` (`rg -l 'RenderSchedulerMode|SchedulerModeGuard' Tests`, about 55 files, e.g. `Tests/RFVRender_Test.cpp`, `Tests/RFVSwitch_Test.cpp`, `Tests/Native/NativeLoopCrossFrame_Test.cpp`, `Tests/Native/NativeLoopFeedback_Test.cpp`, `Tests/GroupDirectRender_Test.cpp`, `Tests/RenderContextRequest_Test.cpp`, `Tests/SchedulerWriters_Test.cpp:120-137`, `Tests/RenderIsolation_Test.cpp:878-896`, `Tests/CacheStressConcurrent_Test.cpp`)
  - approach: Remove mode guards and save/restore fixtures. A Legacy-only case either uses `renderNodePlanesDirect()` (when it tests the direct pull) or is deleted (when it only pinned Legacy behaviour). `InMode` callers move to the P1.T2 functions and the forwarding overload goes. `TEST_P` cases over both modes collapse to `TEST_F`. Leave `RenderSchedulerMode_Test.cpp` for P1.T4.
  - verify: `rg 'RenderSchedulerMode|SchedulerModeGuard' Tests` lists only `RenderSchedulerMode_Test.cpp`; whole ctest green at the phase build.
  - size: M

- [ ] M199.P1.T4 — Delete the mode from the engine
  - files: `Global/Enums.h`, `Engine/Settings.h`, `Engine/Settings.cpp`, `Engine/AppManager.h`, `Engine/AppManager.cpp`, `Engine/AppManagerPrivate.h`, `Engine/AppManagerPrivate.cpp`, `Engine/OutputSchedulerThread.cpp`, `Engine/ViewerInstance.cpp`, `Engine/Nodes/Loop/LoopBegin.cpp`, `Engine/Nodes/Loop/LoopEnd.cpp`, `Tests/RenderSchedulerMode_Test.cpp` (delete), `Tests/CMakeLists.txt:219`
  - approach: Delete what Scope/In lists. Each `taskGraphMode` branch keeps its task-graph arm; the Legacy `else` of `adjustNumberOfThreads` (:1680-1705) and the `ViewerCurrentFrameRequestScheduler` pool branch (:4500-4505) go. `foldsAsTasks()` is deleted and its callers keep their true arm. The fallback `renderRoI` calls stay (Scope/Out).
  - verify: `rg -i 'renderSchedulerMode|NATRON_RENDER_SCHEDULER|SchedulerModeLegacy|SchedulerModeTaskGraph' Engine Global Gui Tests` is empty; whole ctest green, including `SchedulerWriters.*` and `RenderSchedulerAbortTiming.WriterRangeStopsSoonAfterEngineAbort`.
  - size: M

- [ ] M199.P1.T5 — Sweep live docs and bench scripts
  - files: `docs/decisions/2026-10-04-task-graph-render-scheduler.md:17` (add a superseded note, keep the record), `docs/cache/tiered-cache.md:842`, `Engine/Nodes/README.md:1526`, `Engine/Nodes/Loop/docs/LoopEnd.md:279`, `Engine/EffectInstance.h:2479` (comment), `tools/bench/README.md:155-158,202,358`, `tools/bench/run_matrix.sh:10,52`, `tools/bench/edit_latency.sh:62`, `tools/bench/raw_gpu_bench.sh:244`, `tools/bench/gpu_region_bench.sh:202`
  - approach: Remove the mention, or reword "Legacy scheduler" to "direct pull" where the fallback path is meant. Drop the env var from bench run lines.
  - verify: `rg -i 'renderSchedulerMode|NATRON_RENDER_SCHEDULER|legacy scheduler' --glob '!tools/bench/BASELINE.md' --glob '!docs/perf/**' --glob '!.plan/**'` is empty; `bash -n` passes on the four scripts.
  - size: M

## Phase 199.2: Frame args from FrameRenderContext

- [ ] M199.P2.T1 — Separate "frame context installed" from "running a scheduler task"
  - files: `Engine/TLSHolder.h`, `Engine/TLSHolder.cpp` (`FrameContextScope` :256), `Engine/FrameRenderContext.h`, and the sites that test for a context as "on a task": `Engine/EffectInstance.cpp:1343,1453,1538,1868-1885,3358`, `Engine/EffectInstanceRenderRoI.cpp:847,1530,2177`, `Engine/Nodes/Loop/LoopEnd.cpp:3636,3758`, `Engine/Nodes/NativeEffectBase.cpp:686`, `Engine/OfxHost.cpp:1375`, `Engine/RenderScheduler.cpp:1735`
  - approach: Add a scheduled-task marker `RenderScheduler` sets around task execution (e.g. `AppTLS::currentScheduledFrame()` or a flag on the scope). Sites meaning "inside a task" read the marker; sites that only want the frame's args keep `currentFrameContext()`. No behaviour change yet.
  - verify: whole ctest green; `FrameRenderContext_Test` gains a case: a context installed without the marker reports no scheduled frame.
  - size: M

- [ ] M199.P2.T2 — `ParallelRenderArgsSetter` installs a FrameRenderContext; getters read it first
  - files: `Engine/ParallelRenderArgs.h`, `Engine/ParallelRenderArgs.cpp`, `Engine/FrameRenderContext.h`, `Engine/FrameRenderContext.cpp` (`createFromSetter` :270), `Engine/EffectInstance.cpp` (`getParallelRenderArgsTLS` :594)
  - approach: The setter builds its context with `buildArgsMap()` and holds a `FrameContextScope` for its lifetime, nesting as a stack; when the scheduler already installed a context it reuses it. `getParallelRenderArgsTLS()` reads `currentFrameContext()->getArgs(node)` before the TLS list. TLS pushes stay for now. Spawned host-frame-threading threads inherit the context through `SpawnedThreadScope`.
  - verify: whole ctest green; `GraphScalingTLS_Test` and `FrameRenderContext_Test` unchanged and green; a new `FrameRenderContext_Test` case: nested setters restore the outer args.
  - size: L

- [ ] M199.P2.T3 — Route the render entry points' own frame args through the context
  - files: `Engine/EffectInstanceRenderRoI.cpp:561-578`, `Engine/EffectInstanceRenderDeep.cpp:130,266-284,681`, `Engine/EffectInstanceRenderScene.cpp:76,187-205`, `Engine/EffectInstance.cpp:439-600` (set/invalidate, roto-paint args), `Engine/FrameRenderContext.*`
  - approach: Where a pull with no args builds local frame args and pushes them on TLS, add them to the current context (or a scoped child context) instead. `setDuringPaintStrokeCreation`/`getRotoPaintNodes` read and write the context's args.
  - verify: whole ctest green, including `GroupDirectRender_Test`, `Tests/Native/NativeLoop*`, `RenderContinuation_Test`; `rg 'frameArgs\.push_back' Engine` lists only `TLSHolderImpl.h`.
  - size: L

- [ ] M199.P2.T4 — Delete per-effect TLS frame args
  - files: `Engine/EffectInstance.h:3724,3750` (`EffectTLSData::frameArgs`), `Engine/TLSHolder.h`, `Engine/TLSHolderImpl.h:164-185` (`createFromFrameContext`), `Engine/EffectInstance.cpp` (TLS set/invalidate API), `Tests/GraphScalingTLS_Test.cpp`, `Tests/FrameRenderContext_Test.cpp`
  - approach: Remove the list, the on-demand materialisation and the TLS frame-args API; frame args come only from the context. Rewrite the two suites to the new model: args per context, inheritance across spawned threads, nesting, no args outside a context.
  - verify: `rg 'frameArgs' Engine/EffectInstance.h Engine/TLSHolder*` is empty; whole ctest green.
  - size: L

- [ ] M199.P2.T5 — Rename `getParallelRenderArgsTLS` to `getFrameArgs`
  - files: callers from `rg -l getParallelRenderArgsTLS Engine Gui Tests` (`Engine/EffectInstance.*`, `ParallelRenderArgs.*`, `EffectInstanceRenderRoI.cpp`, `RenderScheduler.cpp`, `PyRFV.cpp`, `OfxEffectInstance.cpp`, `Node.cpp`, `Nodes/Loop/LoopBegin.cpp`, `Nodes/Loop/LoopEnd.cpp`, `Nodes/RFV/RFVSwitch.cpp`, `Nodes/Transform/SplineWarp/SplineWarp.cpp`)
  - approach: Mechanical rename, no behaviour change; update the doc comment.
  - verify: `rg getParallelRenderArgsTLS` is empty; `tools/ci/local/syntax-check.py` clean on the touched TUs; whole ctest green.
  - size: M

## Phase 199.3: Scheduler owns frame admission

- [ ] M199.P3.T1 — Delete the dead `DefaultScheduler::processFrame` path
  - files: `Engine/OutputSchedulerThread.h:346,499`, `Engine/OutputSchedulerThread.cpp:2706-2819` and the commented `appendToBuffer` block (:2676-2685)
  - approach: Writers always notify `eSchedulingPolicyFFA` (:2542,2686), so nothing calls `DefaultScheduler::processFrame`. Give `processFrame` a default empty body, keep `ViewerDisplayScheduler`'s override (:3041), keep `appendToBuffer` for the viewer.
  - verify: whole ctest green, including `SchedulerWriters.*` and `Tests/Native/NativeWriteRange_Test.cpp`.
  - size: M

- [ ] M199.P3.T2 — Give `FrameFuture` a completion notification
  - files: `Engine/RenderScheduler.h` (`FrameFuture`, `wait()` :257), `Engine/RenderScheduler.cpp`, `Tests/RenderScheduler_Test.cpp`
  - approach: A one-shot callback, called once on whichever thread finishes or aborts the frame, or immediately when registered after completion. It must never run on a pool thread that then waits on the scheduler.
  - verify: new `RenderScheduler_Test` cases: callback after success, after abort, registered late; exactly one call under a parallel stress.
  - size: M

- [ ] M199.P3.T3 — Writers: the OutputSchedulerThread admits frames itself
  - files: `Engine/OutputSchedulerThread.h`, `Engine/OutputSchedulerThread.cpp` (`startTasks` :1037-1090, `adjustNumberOfThreads` :1644, `DefaultRenderFrameRunnable` :2385-2690, `notifyThreadAboutToQuit` :1135)
  - approach: Use the load test from `adjustNumberOfThreads` (`getLoad` against pool max, capped by `noRenderThreads`). The scheduler thread builds, places and submits frames; the P3.T2 callback calls `notifyFrameRendered`. `writesFrameRangeAtOnce` and fallback frames (deep, ineligible) run on one dedicated fallback worker owned by the OutputSchedulerThread, never a pool thread. Abort cancels in-flight futures.
  - verify: whole ctest green; `RenderSchedulerAbortTiming.WriterRangeStopsSoonAfterEngineAbort`, `SchedulerAbortLatency.WriterAbortReturnsWithinBudget` and `SchedulerWriters.DeepOutputFramesFallBackToLegacyAndNameTheReason` green; a writer range creates no `RenderThreadTask` (counter or assert in the test).
  - size: L

- [ ] M199.P3.T4 — Viewer playback: the same admission for `ViewerDisplayScheduler`
  - files: `Engine/OutputSchedulerThread.cpp` (`ViewerRenderFrameRunnable` :3098-3260), `Engine/ViewerInstance.cpp` (`submitViewerFrameToRenderScheduler` caller near :4268)
  - approach: Playback frames are submitted by the scheduler thread; completion feeds `appendToBuffer`, so the main-thread `processFrame` display keeps its order. Deep-input fallback frames use the P3.T3 fallback worker.
  - verify: whole ctest green; `SchedulerAbortLatency.ViewerPlaybackAbortReturnsWithinBudget` and `ViewerScheduler_Test` green; an Xvfb GUI check: playback of a 50-frame comp loops, stops and scrubs cleanly.
  - size: L

- [ ] M199.P3.T5 — Delete `RenderThreadTask` and the feeder-thread bookkeeping
  - files: `Engine/OutputSchedulerThread.h` (:45,93-119,287 and the `NATRON_PLAYBACK_USES_THREAD_POOL` ifdefs), `Engine/OutputSchedulerThread.cpp` (:148,445, `RenderThreadTask` :2132-2290, `getNRenderThreads`/`getNActiveRenderThreads`/`stopRenderThreads` :2050-2080, `renderThreadsMutex`), `Engine/Settings.cpp:358,1942,3991,4003`, `Engine/AppManager.cpp:3666`
  - approach: Remove the class, the runnable list, the quit flags and the dead ifdefs. Keep `noRenderThreads` as the admission cap.
  - verify: `rg 'RenderThreadTask|NATRON_PLAYBACK_USES_THREAD_POOL' Engine Gui Tests` is empty; whole ctest green.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests` green with the single task-graph ctest leg (whole suite), and PR CI green. The writer abort tests pass: `RenderSchedulerAbortTiming.WriterRangeStopsSoonAfterEngineAbort`, `SchedulerAbortLatency.*`, `SchedulerWriters.*`. `rg -i 'renderSchedulerMode|NATRON_RENDER_SCHEDULER|RenderThreadTask'` outside historical docs and `.plan` is empty. A before/after bench of writer range renders (`tools/bench/run_matrix.sh` hd chain/wide/comp presets, trimmed to the runtime budget) run in one sitting on the GPU host or a cloud VM, never GitHub runners, shows no slowdown; the result is recorded here.

## Decisions

- 2026-10-25 — **Scheduler hand-offs become Legacy removal:** both scheduler modes still ship, and the shim and feeder threads only become removable once Legacy goes (user, at M31 triage).
- 2026-11-25 — **Go-ahead (user):** the task graph has proven itself; drop the Legacy scheduler. Gates stop running the legacy ctest leg now (PM runs --scheduler taskgraph only), ahead of the removal.
- 2026-11-25 — Elaborated (opus consultant) from origin/main 00d15bf9a. Findings: task-graph fallbacks still use the direct `renderRoI` pull (ineligible frames, deep writer outputs, viewer deep input), so only the mode branches are deleted and the pull stays (Scope/Out); the both-ways parity helpers lose their Legacy reference and switch to the direct pull (P1.T2, assumption to confirm); viewer playback has feeder threads too (P3.T4); `FrameView_compare_less` is already a valid ordering, so that carried fix is dropped.
- 2026-11-25 (PM) — P1.T2 in 2ba946ea29: the direct pull matches the Legacy pull except in LoopEnd/LoopBegin, which plan only the last fold step without a frame context (render() still folds every step, same order; earlier iterations' RoI/RoD are computed on the fly, and an identity LoopEnd hops to the last iteration's context). NativeLoopCrossFrame/NativeLoopFeedback direct pulls now take that path — watch them at the phase build. The writer reference pulls the writer itself through renderRoI on the test thread (frame args make it write its own EXRs) rather than the writer's input, so the data-window/channel/colour checks stay.
