## Pending

### 2026-11-08T23:40:00Z — change-request
- refs: Tests/SceneViewport_Test.cpp (SceneViewportStorm.ARefinementFailureClearsAtTheNextSceneEdit), Gui/SceneViewport.cpp, Engine/Nodes/Scene/HydraRenderImpl.cpp
SceneViewportStorm.ARefinementFailureClearsAtTheNextSceneEdit hung for ~290 s under full `ctest -j` load on PR #228's debug CI (both runners), past both of its 120 s deadlines, so one render()/syncScene() call blocks — likely dropping the failed Cycles engine/session or waiting on the low-priority refinement worker. Passes alone and on other branches. Pin it with eu-stack under load; add a per-case watchdog so a stall fails fast. Possibly related to M273 - RenderIsolation Exit Hang. A blocking viewport call is also a "UI stays responsive" violation if it can happen in the product.

### 2026-11-09T01:00:00Z — change-request
- refs: Tests/Scene/ProgressiveRender_Test.cpp:654
ProgressiveRenderTest.ThePrimsSettingsAreTheEnginesAndTheCapAppliesOverThem is flaky under debug CI load: frames.back()->samples was 1, expected 12 (PR #228 taskgraph job, run 42533824384 first attempt, AMD EPYC 7763; passed on rerun; branch didn't touch scene rendering). Same family as the SceneViewportStorm refinement-failure hang: Cycles progressive refinement timing under `ctest -j`. Make the test wait on the refinement's own completion signal rather than wall time.

### 2026-11-09T19:00Z — change-request
- refs: Engine/GenericSchedulerThread.cpp (`waitForAbortToComplete_internal`), M219 - Curves Follow Vectors
- `GenericSchedulerThreadPrivate::waitForAbortToComplete_internal` tests `(state == eThreadStateAborted) || GenericSchedulerThread::eThreadStateIdle`. The second operand is a non-zero enum constant, so the condition is always true and every "blocking" abort returns at once. It has done this since the upstream code. Callers that rely on the wait free state the worker still uses: the render engine, the tracker (`abortTracking_blocking`), previews, the backup thread and `Node::abortAnyProcessing_blocking`. M219 hit it as a segfault in its follow task and works around it with its own mutex. A fix makes those callers actually block the main thread, so it needs a deadlock review of each caller and a check that each worker notices an abort quickly (UI-responsiveness rule).

### 2026-11-09T20:30Z — change-request
- refs: charlesangus/cycles fork (src/hydra: HdCyclesRenderBuffer, HdCyclesOutputDriver), Tests/Scene/ProgressiveRender_Test.cpp
- `ProgressiveRenderTest.CoalescesEditsIntoOneRestart` occasionally aborts with `free(): corrupted unsorted chunks` (release gate job 129322983075). The progressive-render fix branch ruled out our holder lifetime, settings locking, stage-edit races and the first buffer resize. Suspect: `HdCyclesRenderBuffer` Map/WritePixels/Allocate take no locks, and `HdCyclesOutputDriver` walks `_aovBindings` while our worker reads buffers. Run the test under ASan/TSan to confirm, then lock in the fork or serialise reads on our side.
