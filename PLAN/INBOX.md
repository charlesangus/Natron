## Pending

### 2026-11-08T23:40:00Z — change-request
- refs: Tests/SceneViewport_Test.cpp (SceneViewportStorm.ARefinementFailureClearsAtTheNextSceneEdit), Gui/SceneViewport.cpp, Engine/Nodes/Scene/HydraRenderImpl.cpp
SceneViewportStorm.ARefinementFailureClearsAtTheNextSceneEdit hung for ~290 s under full `ctest -j` load on PR #228's debug CI (both runners), past both of its 120 s deadlines, so one render()/syncScene() call blocks — likely dropping the failed Cycles engine/session or waiting on the low-priority refinement worker. Passes alone and on other branches. Pin it with eu-stack under load; add a per-case watchdog so a stall fails fast. Possibly related to M273 - RenderIsolation Exit Hang. A blocking viewport call is also a "UI stays responsive" violation if it can happen in the product.

### 2026-11-09T01:00:00Z — change-request
- refs: Tests/Scene/ProgressiveRender_Test.cpp:654
ProgressiveRenderTest.ThePrimsSettingsAreTheEnginesAndTheCapAppliesOverThem is flaky under debug CI load: frames.back()->samples was 1, expected 12 (PR #228 taskgraph job, run 42533824384 first attempt, AMD EPYC 7763; passed on rerun; branch didn't touch scene rendering). Same family as the SceneViewportStorm refinement-failure hang: Cycles progressive refinement timing under `ctest -j`. Make the test wait on the refinement's own completion signal rather than wall time.
