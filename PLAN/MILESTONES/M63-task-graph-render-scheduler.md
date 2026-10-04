# Milestone 63: Task-graph render scheduler

> **Elaborated 2026-10-04** (planning consultant, opus) from the 2026-09-25 stub, after M62's re-benchmark. The architecture departs from the stub: a concurrent pull, not an up-front task graph. See `DECISIONS/2026-10-04-parallel-pull-render-scheduler.md` (pending user confirmation).

M62's re-benchmark showed HD renders spend 83.5% of the time with exactly 3 of 4 cores running (mean 2.74). The scout traced most of that to how the global QThreadPool is used rather than to the pull model: a blocking writer render can sit on a pool slot for the whole render, and the OFX multithread suite and host frame threading put their caller to sleep instead of letting it work. This milestone fixes that first. Then it makes the pull concurrent: when `treeRecurseFunctor` pre-renders a node's inputs, the inputs after the first become tasks on the same global pool, joined claim-or-wait. Independent branches render at once, and each node still gets exactly today's RoI, time and arguments, so caching, request-pass and abort behaviour are unchanged. A setting switches between serial and parallel branch rendering so pixels can be compared both ways; it ships off and is flipped once the pixel, abort and GUI gates pass. Targets against the start of M63 (m63base): HD wide/comp 100 and 300 at least 1.3x faster per frame with at least 3.4 mean running threads; HD chain/mixed at least 1.15x; tiny configs no slower. The stub's 1.8x is not reachable on 4 cores at a measured parallelism of 2.55 (ceiling 1.57x) and is dropped.

## Scout notes (2026-10-04, on milestone/m63-task-graph-render-scheduler at 20aa6f102)
- Bench renders one frame per `app.render(writer, f, f)` (`tools/bench/graph_bench.py:206-253`); the median frame metric never sees parallel frames. `BENCH_RANGE` renders a range in one call. `run_matrix.sh` arg 4 is the range.
- Background renders: `AppInstance::startWritersRendering` runs `startRenderingFullSequence(true, item)` inside `QtConcurrent::blockingMap` (`Engine/AppInstance.cpp:1812-1816`); `BlockingBackgroundRender::blockingRender` sleeps on a condition for the whole render (`Engine/BlockingBackgroundRender.cpp:53-71`). On a pool thread that holds 1 of the 4 slots. Python `app.render` gets there via `PyAppInstance.cpp:340/387`.
- `AppManager::getNCPUsAvailableForEffect` (`Engine/AppManager.cpp:2923-2950`) = min(max − (pool active + `getNRunningThreads`) + 1, perEffect). Render threads count themselves through `RenderThreadTask::notifyIsRunning` (`OutputSchedulerThread.cpp:2123-2134`). With 1 pool slot blocked + 1 render thread, plugins are told 3.
- Pool max from Settings: min(cores, RAM / 3.5 GB) (`Engine/Settings.cpp:2263-2277`); 4 on the N100. `QT_CUSTOM_THREADPOOL` is not defined on Qt 6.8, so pool threads are not `AbortableThread`s; `aborted()` falls back to TLS frame args (`Engine/EffectInstance.cpp:490-535`).
- The suite: `OfxHost::multiThread` uses `QtConcurrent::mapped` + `waitForFinished()` (`Engine/OfxHost.cpp:1350-1371`), so the caller sleeps. `threadFunctionWrapper` TLS scope at `:1210-1243`; `multiThreadIndex`/`IsSpawnedThread` read `threadIndexes` (`:1443-1469`). Host frame threading does the same at `EffectInstanceRenderRoI.cpp:1857-1858`. Deep chunks (`Nodes/NativeEffectBase.cpp:195-210`) and row bands (`ImageCopyChannels.cpp:151-174`) already use caller-participating `blockingMap`. `SpawnedThreadScope` is a no-op when spawner == current thread (`Engine/TLSHolder.cpp:192-206`).
- Viewer current-frame renders are global-pool runnables, with a backup thread when the pool is nearly full (`OutputSchedulerThread.cpp:4063-4078`). Sequence renders use dedicated `RenderThreadTask` QThreads (`NATRON_PLAYBACK_USES_THREAD_POOL` off, `OutputSchedulerThread.h:44`); parallel frames come from `adjustNumberOfThreads` (`:1507-1550`).
- Pull: `renderRoI` (`EffectInstanceRenderRoI.cpp:294`) pre-renders inputs per rect (`:1207-1243`) via `renderInputImagesForRoI` (`EffectInstance.cpp:2256-2300`) → `treeRecurseFunctor` (`ParallelRenderArgs.cpp:47`). Its serial loop over inputs is `:133-305` (RoI from the request at `:257`, `renderRoI` at `:282`, images appended per input in frame order at `:288-292`, abort check `:294`). The render action runs at `:1471` under the instance-safe/unsafe locks (`:1424-1436`).
- Request pass: `getInputsRoIsFunctor` (`ParallelRenderArgs.cpp:311`) merges every request's RoI per (node, time, view) into `finalData.finalRoi` (`:423-431`). `ParallelRenderArgsSetter` installs per-effect TLS for the upstream and expression dependencies (`:684-790`), including the `isFrameVaryingOrAnimated` memo (`:751`); `updateNodesRequest` attaches the request (`:792`). Writer entry: `DefaultRenderFrameRunnable::renderFrame` (`OutputSchedulerThread.cpp:2190-2400`); viewer entry: `ViewerInstance::renderViewer_internal` (`ViewerInstance.cpp:1315`, `renderRoI` at `:1587`).
- `FrameView_compare_less` is not a strict weak ordering (`ParallelRenderArgs.h:221`); multi-view only; goes to M31.
- Concurrent renders of one image: trimap marking and waiting (`EffectInstancePrivate.cpp:589-700`; the wait polls every 50 ms and checks `aborted()`; called from `EffectInstanceRenderRoI.cpp:269`). Nodes with more than one output always cache (`Node.cpp:7185-7215`). `getImage` falls back to pulling when the input isn't pre-rendered (`EffectInstance.cpp:1222-1236`).
- TLS (M62): only `EffectTLSData` is inherited, lazily, along the spawner chain (depth limit 16). `eSpawnKindHostFrameThreading` copies `frameArgs` and counters, not `currentRenderArgs` (`TLSHolderImpl.h:53-73`, `:125-160`). A thread may never run two renders of the same effect at once (`EffectInstance.cpp:2362-2366`).
- Deep: `renderDeepRoI` pulls deep inputs in its own serial loop (`EffectInstanceRenderDeep.cpp:404-450`); the viewer flattens via `renderDeepRoIFlattened` (`ViewerInstance.cpp:1553`).
- Single-threaded per-image passes: `Image::checkForNaNsAndFix` (`Image.cpp:2085`, called at `EffectInstance.cpp:2939`), 4.4% of HD busy time. `copyUnProcessedChannels` is banded only as wide as `getNCPUsAvailableForEffect`.
- Tests: `GraphScalingTLS_Test.cpp` has a `renderWindow` helper (request pass + `renderRoI`, `:106-180`) and a pull-vs-multithreaded memcmp test (`:260-330`; `appPTR->setNumberOfThreads(-1/0)`, `setMaxThreadCount(4)`). Abort tests: `DeepReadWrite_Test.cpp:804`, `DeepRenderPipeline_Test.cpp:312/552/611/820`. Range: `RenderRange_Test.cpp:85`. OFX integration: `tools/ci/smoke_test.py`. GUI: `Tests/gui/run-gui-test.sh` + `guitest.py`, scrub precedent `viewer_error_scrub.py`.

## Phase 63.1: Decision, baseline, and shared-pool discipline
- [ ] M63.P1.T1 — Publish the decision and update the M64/M31 entries
  - files: `PLAN/DECISIONS/2026-10-04-parallel-pull-render-scheduler.md`, `PLAN/DECISIONS/INDEX.md`, `PLAN.md`, `PLAN/MILESTONES/M64-tiled-rendering.md`, `PLAN/MILESTONES/M31-architectural-cleanup.md`, `docs/decisions/` copy (PM work, not a subagent)
  - approach: After the user confirms the direction: INDEX line; M64's premise becomes "tiles inside the pull", still blocked on M63's after-bench; M31 Phase 31.1 gains the per-render context object and the `FrameView_compare_less` fix (speculative, with file:line); copy the decision into `docs/decisions/` on the milestone branch.
  - verify: INDEX links the decision; M64 and M31 cite this milestone; `docs/decisions/` has the copy.
  - size: S
- [ ] M63.P1.T2 — Add a render-window occupancy tool and record the M63 baseline
  - files: `tools/bench/states_run.sh` (new), `tools/bench/README.md`, `tools/bench/BASELINE.md`
  - approach: `states_run.sh <name> <interval> VAR=…` starts `graph_bench.py` in the container like `profile_run.sh`, samples `/proc/<pid>/task/*/stat` (no ptrace) from "[bench] built" until "RESULT", and prints the running-thread histogram, the mean, and the share of samples with 4 running. Then, on `build/release` at `20aa6f102`, run `run_matrix.sh m63base hd 3 0 chain:30,100 mixed:100 wide:100,300 comp:100,300`, `run_matrix.sh m63base-range hd 1 8 chain:100 wide:100 comp:100`, and `states_run.sh` on chain:30, wide:100 and comp:100 at HD. Write a "Before M63" section.
  - verify: shellcheck clean; the section has the table, range per-frame figures and the three histograms; chain:30 HD mean within ±0.3 of M62's 2.74.
  - size: M
- [ ] M63.P1.T3 — Stop blocking writer renders from holding a pool slot
  - files: `Engine/AppInstance.cpp`, `Engine/BlockingBackgroundRender.cpp`, `Engine/ThreadPool.h`, `Engine/ThreadPool.cpp`, `Tests/BlockingRenderPool_Test.cpp` (new) + `Tests/CMakeLists.txt`
  - approach: Add `ScopedRenderThreadIdle` in ThreadPool.{h,cpp}: on a global-pool thread (marked by a thread_local set in our runnables, the blockingMap lambda and P2's task wrapper) it calls `releaseThread()`/`reserveThread()`; on a counted render thread it calls `fetchAndAddNRunningThreads(-1/+1)`; otherwise nothing. Use it around the condition wait in `blockingRender`. For a single queued item, run `startRenderingFullSequence(true, …)` directly on the calling thread instead of through `blockingMap`.
  - verify: a native test effect records `getNCPUsAvailableForEffect()` and `QThreadPool::globalInstance()->activeThreadCount()` inside its render during a blocking `startWritersRendering` with pool max 4; expects 4 CPUs and 0 active pool threads (before the fix it shows 3; record both). Full debug ctest green; `smoke_test.py` green.
  - size: M
- [ ] M63.P1.T4 — Make the OFX suite and host frame threading put the caller to work
  - files: `Engine/OfxHost.cpp`, `Engine/EffectInstanceRenderRoI.cpp`, `Tests/SuiteParticipation_Test.cpp` (new)
  - approach: In `OfxHost::multiThread`, replace `mapped`+`waitForFinished` with `QtConcurrent::blockingMapped` so the caller runs indices. `threadFunctionWrapper` already skips the TLS scope when spawner == current thread. The caller's push/pop of `threadIndexes` must leave its `-1` action-caller marker intact, and `multiThreadIndex` must return the index the caller is running. Same for host frame threading at `:1857`. Keep the `useThreadPool=false` `OfxThread` path unchanged.
  - verify: the test saturates the global pool with 4 runnables blocked on a semaphore, then renders a 512² CheckerBoard→Grade×10 from the main thread: it must finish (generous 30 s bound; before the change it hangs until release) and match the single-threaded render bit for bit. `GraphScalingTLS.*` passes with `--gtest_repeat=3`. `states_run.sh` on chain:30 HD shows 4 running in at least 50% of samples.
  - size: M
- [ ] M63.P1.T5 — Band `checkForNaNsAndFix` across the shared pool
  - files: `Engine/Image.cpp`, `Engine/ImageCopyChannels.cpp` (generalise `forEachCopyUnProcessedRowBand` into a shared `forEachRowBand`), `Engine/Image.h`, `Tests/Image_Test.cpp`
  - approach: Split the NaN scan into row bands with the same 256×256 threshold and sizing from `getNCPUsAvailableForEffect`, via caller-participating `blockingMap`. Keep `_entryLock` held across the call and OR the per-band found flags.
  - verify: an `Image_Test` case puts NaNs in scattered rows of a 1024² float image and gets the same fixed pixels and return value banded and unbanded; full ctest green.
  - size: M
- [ ] M63.P1.T6 — Bench the pool-discipline fixes
  - files: `tools/bench/BASELINE.md`
  - approach: Re-run P1.T2's matrix, range and states runs as `m63p1`; `compare.py` against m63base.
  - verify: HD chain:30/100 and mixed:100 at least 1.15x faster per frame; chain:30 HD mean running threads ≥3.3; nothing flagged at 1.3; tiny chain:1000 and wide:1000 no slower than 1.1x. Record the wide/comp means (Phase 2's input). If chain gains are below 1.1x, stop and re-scout before Phase 2.
  - size: M
**Phase gate:** P1.T3–T5 tests green in debug ctest (TLS tests 3×), `smoke_test.py` green, P1.T6 targets met and recorded.

## Phase 63.2: Parallel branches inside the pull (default off)
- [ ] M63.P2.T1 — Add `RenderTaskGroup`: fork-join with claim-or-wait on the global pool
  - files: `Engine/RenderTaskGroup.h` (new), `Engine/RenderTaskGroup.cpp` (new), `Engine/TLSHolder.h`, `Engine/TLSHolderImpl.h`, `Tests/RenderTaskGroup_Test.cpp` (new)
  - approach: `fork(fn, depth)` wraps `fn` in a non-autodelete runnable with an atomic claimed flag and starts it on the global pool with priority = depth (depth-first). `join()` claims each unclaimed child in fork order and runs it inline (`tryTake` to free the queue entry), waits under `ScopedRenderThreadIdle` for children already running, and returns results in fork order. `cancel()` claims and drops unstarted children. A runnable that loses the claim returns immediately. Each task enters `SpawnedThreadScope(rootThread, eSpawnKindRenderTask)`, a new kind copying only `frameArgs` (zero counters, empty `currentRenderArgs`); `rootThread` is the forking thread's own root when it is itself a task, so chains stay ≤2 deep. A thread_local `ScopedNoFork` depth makes `fork()` run inline. Exceptions are caught in the task and rethrown at `join()`.
  - verify: run 3×: nested fork-join 6 levels deep with fan-out 3 at pool max 1, 2 and 4 completes with a deterministic result vector; `cancel()` after an abort flag leaves no child running later; a task 20 levels deep still sees the root's `frameArgs` (pointer identity); `ScopedNoFork` runs children inline; a child exception surfaces at `join()`.
  - size: L
- [ ] M63.P2.T2 — Add the parallel-branches setting and a bench switch
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`, `Engine/AppManager.h`, `Engine/AppManager.cpp`, `tools/bench/graph_bench.py`, `tools/bench/run_matrix.sh`, `tools/bench/states_run.sh`, `tools/bench/compare.py`
  - approach: Threading-page `KnobBool` "Render independent branches in parallel", script name `parallelBranchRender`, default false, mirrored into an `AppManager` atomic (`getParallelBranchRendering()`, `setParallelBranchRendering(bool)` for tests). Env `NATRON_PARALLEL_BRANCHES=0|1` overrides at startup so ctest runs both ways. `graph_bench.py` sets the param when `BENCH_BRANCHES` is set and records `branches` in the JSON; `compare.py` adds it to the key; the shell scripts forward it.
  - verify: the toggle from Python and from the env is visible to `appPTR`; `compare.py` keys separate the two modes; ctest green.
  - size: M
- [ ] M63.P2.T3 — Fork input pre-renders in `treeRecurseFunctor`
  - files: `Engine/ParallelRenderArgs.cpp`, `Engine/EffectInstanceRenderRoI.cpp`, `Engine/EffectInstance.cpp`, `Engine/EffectInstancePrivate.cpp`
  - approach: In the render functor (`ParallelRenderArgs.cpp:133-305`), when the setting is on, at least 2 inputs have work and no `ScopedNoFork` is active, turn each `framesToRender` entry into one task that runs that input's whole frame/view loop into its own preallocated `ImageList` (order within an input unchanged). Input 0 runs inline; the others fork. Don't fork a non-`eDataKindImage` input, any input when `renderStorageMode` is GL or the frame has a GL context, an analysis/rotopaint-internal/paint-stroke frame, or a pixel RoI under 256×256. Hold `NotifyInputNRenderingStarted_RAII` inside each task. Join and return the first non-OK code in input order; on Aborted/Failed `cancel()` the rest. Put `ScopedNoFork` around `renderRoIInternal` (`EffectInstanceRenderRoI.cpp:1471`) so a `getImage` fallback inside a plugin action never forks while plugin locks are held. Wrap the trimap wait (`EffectInstancePrivate.cpp:626`) in `ScopedRenderThreadIdle`.
  - verify: setting on: full debug ctest, `GraphScalingTLS.*` 3× and `smoke_test.py` with `NATRON_PARALLEL_BRANCHES=1` green. Setting off: no behaviour change (ctest green; tiny chain within noise).
  - size: L
- [ ] M63.P2.T4 — Prove fan-out renders once and identity/reroutes behave under forks
  - files: `Tests/ParallelBranchesFanout_Test.cpp` (new), a counting test effect header (new or `Tests/DataKindTestEffect.h`)
  - approach: A counting native effect feeds a Dot consumed by 4 Merges reduced to one root; render 512² with branches on. Same with a disabled (identity) node in one branch, and a Transform→Transform→Merge where concatenation reroutes the input.
  - verify: the counted node renders its RoI exactly once per frame in both modes; pixels memcmp-equal between modes; 3×.
  - size: M
- [ ] M63.P2.T5 — Compare pixels from the serial and parallel pull across topologies
  - files: `Tests/ParallelBranches_Test.cpp` (new), `Tests/CMakeLists.txt` (registers P2.T1, P2.T4, P2.T5, P2.T6 tests)
  - approach: Reuse the `GraphScalingTLS` `renderWindow` pattern plus a Write-driven range render (`startWritersRendering`, as `RenderRange_Test.cpp`). Graphs: wide (16 CheckerBoard→Grade leaves, merge tree); seeded comp DAG with Dot fan-out and ColorCorrect/Transform/Merge; masked Merge; TimeOffset/FrameHold branch; Switch; multi-plane Shuffle branch; deep branch through DeepToImage into a Merge; Grade expressions referencing another branch's animated knob with the timeline parked elsewhere. Render each with branches off (single- and multithreaded) and on (pool max 2 and 4); clear caches between runs. memcmp everything except graphs with host-frame-threaded CImg plugins (max abs diff ≤1e-5, with the reason in the test).
  - verify: all equal; `--gtest_repeat=3` green; the whole ctest runs once with `NATRON_PARALLEL_BRANCHES=1` and once with `=0`.
  - size: M
- [ ] M63.P2.T6 — Test that an abort cancels promptly under forks
  - files: `Tests/ParallelBranchesAbort_Test.cpp` (new)
  - approach: Render a wide graph (64 leaves of a slow native test effect that polls `aborted()`) on a helper thread with branches on; `abortInfo->setAborted()` after the first leaf starts; also abort a Write range render through `engine->abortRenderingNoRestart()` as in `DeepReadWrite_Test.cpp:804`.
  - verify: `renderRoI` returns Aborted within 2 s; no leaf starts after the abort (counter); no image stays marked (a follow-up render matches a clean render); `activeThreadCount()` returns to 0; 3×.
  - size: M
**Phase gate:** P2.T1–T6 green; whole ctest green with the setting off and on (TLS/threading tests 3×); `smoke_test.py` green both ways.

## Phase 63.3: Measure, viewer, and turn it on
- [ ] M63.P3.T1 — Bench parallel branches against the baseline
  - files: `tools/bench/BASELINE.md`
  - approach: Run the P1.T2 matrix, range and states runs with `BENCH_BRANCHES=1` as `m63p3`; profile comp:100 HD; record `rss_peak_mb`; `compare.py` against m63base and m63p1.
  - verify, against m63base: wide/comp HD 100 and 300 ≥1.3x faster per frame with `states_run.sh` mean ≥3.4 on wide:100 and comp:100; chain/mixed HD ≥1.15x (no worse than m63p1 beyond noise); tiny configs no slower than 1.15x; range per-frame no slower; HD comp 300 peak RSS ≤1.5x m63base. If wide/comp miss 1.3x, record the histogram, fill P3.T2's measurements, and bring it to the user before P3.T4.
  - size: M
- [ ] M63.P3.T2 — Fix frame-overlap scheduling only if range renders fall short
  - files: `Engine/OutputSchedulerThread.cpp` (`adjustNumberOfThreads`), `tools/bench/BASELINE.md`
  - approach: Act only if P3.T1 shows mean running below 3.6 during a range render, or range per-frame no better than single-frame: make `adjustNumberOfThreads` count idle-released render threads correctly (it reads `getNRunningThreads` + pool active) so a frame thread starts while another frame's branches leave cores idle. Otherwise record "no change needed" with the numbers.
  - verify: `RenderRange_Test` and the abort tests green; range mean ≥3.6, or the task closed as not needed with evidence.
  - size: M
- [ ] M63.P3.T3 — Check the viewer under Xvfb with branches on
  - files: `Tests/gui/parallel_branches_scrub.py` (new), `Tests/gui/guitest.py` if a helper is missing
  - approach: Wide graph (8 CheckerBoard→Grade leaves merged, HD) → Viewer with the setting on, via `Tests/gui/run-gui-test.sh` (pre-seed checkForUpdates=false). Scrub quickly across 20 frames, stop; play 2 s, stop; change a leaf Grade mid-render. Also run `viewer_error_scrub.py` with the setting on.
  - verify: after each seek the viewer shows the playhead frame within the poll timeout and keeps it; nothing stale flips back; no crash or hang; below 4 busy threads once idle. Screenshots shared with the user, who signs off before the box is ticked.
  - size: M
- [ ] M63.P3.T4 — Turn the setting on by default and keep it as a kill switch
  - files: `Engine/Settings.cpp`, `tools/bench/README.md`, `.github/workflows/ci.yml` (one ctest leg with `NATRON_PARALLEL_BRANCHES=0`)
  - approach: Default true after P3.T1–T3 pass; CI runs the main leg with the default and one leg with it off so the serial pull stays tested.
  - verify: CI green on both legs; bench defaults match m63p3 within noise.
  - size: S
**Phase gate:** P3.T1 targets met (or a user decision recorded); GUI check signed off; CI green with the setting on and off.

## Phase 63.4: Close out
- [ ] M63.P4.T1 — Record the After-M63 benchmark and decisions
  - files: `tools/bench/BASELINE.md`, this file's `## Decisions`, `PLAN.md`
  - approach: "After M63" section: full matrix, range runs, histograms, profiles, gate checks with ratios to m63base and to After M62, and the input for M64 (plugin pixel work vs bandwidth per node).
  - verify: every gate line PASS/FAIL with numbers; M64's "Blocked on" points here.
  - size: S
- [ ] M63.P4.T2 — Package the AppImage and write the UAT script
  - files: `build/appimages/M63-<sha>.AppImage`, `build/appimages/M63-uat.md`
  - approach: Release `package.sh` (never debug). UAT: open a wide comp, scrub and play in the viewer, render a range to EXR with progress and abort, run `NatronRenderer -w` on the same project and compare its output against the setting turned off (`oiiotool --diff`), render with Python `app.render()`, watch `htop` show 4 cores busy during the CLI render.
  - verify: the AppImage launch-checks under Xvfb; the UAT is deferred to the user.
  - size: M

**Verification gate:** every task's verify holds; whole ctest green with the setting off and on; `smoke_test.py` green; CI green on both legs; AppImage packaged; user UAT signed off.

## Moved elsewhere
- M31: a per-render context object (args map, request, abort info) replacing per-effect TLS `frameArgs` and spawner-chain inheritance; fix `FrameView_compare_less` (`ParallelRenderArgs.h:221`). M31.P1.T1 (`RenderEngine` on `Node`) is not needed by M63.
- M64: tiles/fusion inside the pull; an explicit graph is reconsidered there only if fusion needs it.
- Not in scope: forking the deep input loop (`EffectInstanceRenderDeep.cpp:404`); OpenGL branch concurrency.

Execution notes:
- Stacked PR on `milestone/m62-render-scaling-hotspots`; retarget to `main` once M62 merges.
- Order: P1.T1 → P1.T2 (bench on the unmodified `20aa6f102` release build, container exclusive) → batch A {P1.T3, P1.T4, P1.T5} in parallel with one detached debug build + ctest (P1.T3 alone edits `Tests/CMakeLists.txt`, registering `BlockingRenderPool_Test.cpp` and `SuiteParticipation_Test.cpp`; P1.T5 extends `Image_Test.cpp`) → release build → P1.T6.
- Batch B {P2.T1, P2.T2} in parallel (disjoint files) → P2.T3 alone → batch C {P2.T4, P2.T5, P2.T6} in parallel, each with its own test file; P2.T5 alone edits `Tests/CMakeLists.txt`. One build per batch. P2.T1 lands before P2.T3 is briefed.
- Container single-tenant: never a bench during a build; detached builds with a done marker; `pgrep -x ninja` must be 0 before relaunching. Pin the libs/OpenFX fork branch before building.
- Debug build defines NDEBUG: tests use EXPECT/ASSERT. Concurrency tests run with `--gtest_repeat=3` and pool max 1, 2 and 4. Timing bounds are generous structural limits.
- Every pixel comparison runs both ways through the setting (`appPTR->setParallelBranchRendering`, env `NATRON_PARALLEL_BRANCHES`); `appPTR->clearAllCaches()` between modes.
- Bench against "Before M63" (m63base), quoting After-M62 ratios alongside; noise up to 2x, so medians and ratios; concurrency only from `states_run.sh`/`sample_states.sh`.
- No backward compatibility work: the new setting needs no migration.

## Decisions
- 2026-10-04 — **Elaborated** (planning consultant, opus). Architecture: concurrent pull, not an up-front task graph; shared-pool discipline first. Targets re-based to the 4-core ceiling (1.3x wide/comp, 1.15x chains; the stub's 1.8x dropped). Two scout findings stand on their own: CLI/Python renders probably lose one core to a blocked pool slot (unconfirmed until P1.T3's probe), and the suite/host frame threading put the caller to sleep. Awaiting the user's confirmation of the direction before P1.T2.
