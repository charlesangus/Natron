## Pending

### 2026-11-08T23:40:00Z — change-request
- refs: Tests/SceneViewport_Test.cpp (SceneViewportStorm.ARefinementFailureClearsAtTheNextSceneEdit), Gui/SceneViewport.cpp, Engine/Nodes/Scene/HydraRenderImpl.cpp
SceneViewportStorm.ARefinementFailureClearsAtTheNextSceneEdit hung for ~290 s under full `ctest -j` load on PR #228's debug CI (both runners), past both of its 120 s deadlines, so one render()/syncScene() call blocks — likely dropping the failed Cycles engine/session or waiting on the low-priority refinement worker. Passes alone and on other branches. Pin it with eu-stack under load; add a per-case watchdog so a stall fails fast. Possibly related to M273 - RenderIsolation Exit Hang. A blocking viewport call is also a "UI stays responsive" violation if it can happen in the product.
