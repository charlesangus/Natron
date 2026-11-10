# M378 - Scene Render Test Stability

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Three scene-render tests fail only under heavy CI load, and all three come from Cycles progressive refinement. Find the root cause of each and fix it, so the tests pass reliably under `ctest -j`. If the product itself can block, fix the product (the "UI stays responsive" rule). Otherwise fix the test.

- `SceneViewportStorm.ARefinementFailureClearsAtTheNextSceneEdit` hung for about 290 s on both runners of PR #228's debug CI, past both of its 120 s deadlines. So one `render()`/`syncScene()` call blocks, probably while dropping the failed Cycles engine or session, or while waiting on the low-priority refinement worker. It passes when run alone. It may be related to M273 - RenderIsolation Exit Hang. Files: `Tests/SceneViewport_Test.cpp`, `Gui/SceneViewport.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`.
- `ProgressiveRenderTest.ThePrimsSettingsAreTheEnginesAndTheCapAppliesOverThem` (`Tests/Scene/ProgressiveRender_Test.cpp:654`) failed once in PR #228's taskgraph job (run 42533824384) with `frames.back()->samples` at 1 instead of 12. The test waits on wall time. It should wait on the refinement's own completion signal.
- `ProgressiveRenderTest.CoalescesEditsIntoOneRestart` sometimes aborts with `free(): corrupted unsorted chunks` (release gate job 129322983075). Our holder lifetime, settings locking, stage-edit races and the first buffer resize are ruled out. The suspect is that `HdCyclesRenderBuffer` Map/WritePixels/Allocate take no locks while `HdCyclesOutputDriver` walks `_aovBindings` and our worker reads the buffers. The fix goes either in the charlesangus/cycles fork, as locks, or on our side, by serialising reads.

Blocked on: reproducing each failure under load (eu-stack on the hang, ASan/TSan on the heap corruption). Which fix to make depends on what those runs show.

Acceptance sketch:
- All three tests pass 20 consecutive times under the full `ctest -j` on both schedulers in a remote debug run.
- A stalled viewport render fails its test quickly through a per-case watchdog instead of hanging the leg.
- If the race is in the Cycles fork, the fork carries the lock and our submodule pin moves to it.
