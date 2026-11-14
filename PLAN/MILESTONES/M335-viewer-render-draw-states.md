# M335 - Viewer Render Draw States

Viewer renders through Cycles or MoonRay follow the viewer's draw states, so you can look-dev one
asset without paying for the whole scene, with a Render Everything toggle that ignores them.
Design: `PLAN/DESIGN/2026-11-04-scene-graph-design.md`. M327 - Heavy Scene Spike notes
(`PLAN/DESIGN/2026-11-07-heavy-scene-spike.md`): verify the filter binds on delegate engines; this is undocumented.

## Scope

**In:**
- Delegate viewer renders (Cycles, MoonRay) follow the viewer's draw states through a `SceneDrawFilter` on the progressive worker's engine.
- Render Everything is a knob on the Viewer node (`renderEverything`, "Render Everything"), shown in the delegate render row.

**Out:**
- USDRender and USDWrite: they never follow draw states (user).

**Edge cases:**
- Turning Render Everything on loads payloads, the one case of a viewer toggle doing so; the toggle's tooltip says it will.
- Draw changes made while a delegate is active reach the worker and the box overlay; a Storm engine made later starts from the current rules.
- The worker applies changes all at once, never as a paced reveal (each reveal step would restart the delegate). A burst of changes causes one restart (20 ms quiet, 100 ms cap).
- Turning Render Everything off restores the rules without rebuilding the engine and unloads nothing ("closing a row never unloads").
- Lights follow M329 - Viewer Draw States: only Hidden rows remove them. When Hidden rows remove every scene light, delegate viewer renders fall back to a headlight, as Storm does (user).

**Assumptions:**
- Render Everything matches USDRender because the viewer's evaluation load set follows its stage's load rules (`Engine/SceneLoadSet.h` `SceneEvaluationContext`).
- MoonRay respects `HdVisibilitySchema`; P1.T3 checks it, and BBox falls back to removing the prim for MoonRay if not.

## Phase 335.1: Filter on delegate engines

- [x] M335.P1.T1 — Move the filter into Engine and make it opt-in
  - files: `Gui/SceneDrawFilter.{h,cpp}` → `Engine/Nodes/Scene/SceneDrawFilter.{h,cpp}`, `Gui/CMakeLists.txt` (~:25), Engine CMake, `Gui/SceneViewport.cpp` (~:1561-1600), `Engine/Nodes/Scene/HydraRenderImpl.cpp` (`newEngine` ~:282, `RenderThread::renderEngine` ~:1134)
  - approach: the filter is registered for every renderer today (`SceneDrawFilter.cpp` ~:256-270) but nobody seeds it on delegate engines. `appendFilter` (~:67) returns its input unless an RAII `SceneDrawFilter::EngineScope` is active; the scope records the instance id itself so it works on whichever thread builds the engine (Cycles builds inside `tbb::task_arena::execute`, `CyclesAovs.cpp` ~:668, so the per-thread `takeAppendedInstanceId` is unreliable there). `newEngine` takes a `wantFilter` flag; offline engines pass false.
  - verify: `Tests/SceneDrawFilter_Test.cpp` gains: a scoped Cycles engine has a filter `find()` locates, an unscoped one has none; existing cases pass.
  - size: M

- [x] M335.P1.T2 — `SceneDrawFilter::replaceRules(const Rules&)`
  - files: `Engine/Nodes/Scene/SceneDrawFilter.{h,cpp}`, `Tests/SceneDrawFilter_Test.cpp`
  - approach: move from the current rules to a target by sending only the differing hide, show and visibility notices. Empty rules show everything. Used for Render Everything on/off without rebuilding the engine.
  - verify: A → empty → A sends notices only for changed prims, ends identical to a filter seeded with A, and no-op calls send nothing.
  - size: M

- [x] M335.P1.T3 — Confirm the filter takes effect on Cycles and MoonRay
  - files: `Tests/Scene/CyclesDelegate_Test.cpp`, `Tests/Scene/MoonRayDelegate_Test.cpp`
  - approach: three meshes and one light, render with a seeded filter, then apply BBox (held invisible), Hidden (removed) and Hidden on the light's row; read the primId AOV.
  - verify: hidden prims absent from primId; the hidden light changes frame brightness. Print whether MoonRay respects `HdVisibilitySchema`; if not, BBox removes the prim for MoonRay and the test asserts that.
  - size: M

## Phase 335.2: The progressive worker follows draw states

- [x] M335.P2.T1 — Draw rules in `ProgressiveRender`
  - files: `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}` (Implementation ~:2447, `makeEngine` ~:2552, render block ~:3001-3036, `edited()` ~:2516), `Tests/Scene/ProgressiveRender_Test.cpp`
  - approach: `setDrawRules(Rules)`, `applyDrawDeltas(std::vector<SceneDrawDelta>)` and `setRenderEverything(bool)` queue under `mutex` and call `edited()`. `makeEngine` builds inside an `EngineScope` and seeds the filter. The worker applies the queue (revealBatch 0) under `engineMutex` and the stage guard before `renderGuarded`, then `endRender()`. Render Everything calls `replaceRules({})`.
  - verify: new cases — a hidden prim leaves the primId frame; five deltas within 20 ms cause one restart; Render Everything on then off restores the frame.
  - size: L

## Phase 335.3: Viewer wiring

- [x] M335.P3.T4 — Headlight when every light is hidden
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (worker draw-rule application from P2.T1), `Engine/Nodes/Scene/SceneDrawFilter.{h,cpp}`, `Tests/Scene/ProgressiveRender_Test.cpp`
  - approach: when the applied rules leave no scene light, add the same camera-attached headlight Storm's viewer uses (find how `SceneViewport` provides it) to the delegate's scene; remove it when any light returns or Render Everything is on. Offline engines never get it.
  - verify: Cycles progressive render with every light row Hidden is not black and brightness follows camera direction; unhiding one light removes the headlight; USDRender output unchanged.
  - size: M

- [x] M335.P3.T1 — Draw changes reach the worker and the overlay while a delegate is active
  - files: `Gui/SceneViewport.cpp` (`applyPendingDrawDeltas` ~:3096, `serviceDrawFilter` ~:1753, `switchRenderer` ~:1110-1125, `renderProgressive` ~:4661)
  - approach: today `applyPendingDrawDeltas` does nothing without a Storm engine and choosing a delegate drops that engine, so `drawRules` and `boxes->setRules` go stale under Cycles. Without a Storm engine, still fold queued deltas into `drawRules`, `boxes->setRules` and `progressive->applyDrawDeltas`. On a delegate switch call `setDrawRules(drawRules)`. The delegate paint path services the queue too.
  - verify: new `Tests/gui/m335_render_draw_states.py` on a generated four-asset payload city under build/: one asset Load All, the rest BBox, Cycles — picked prims cover only that asset, unrequested payloads stay 0, boxes drawn > 0 over the frame; BBox another row and its prims leave the frame.
  - size: L

- [x] M335.P3.T2 — Render Everything knob and button
  - files: `Engine/ViewerInstance.h` (~:85, `kViewerParamRenderEverything`), `Engine/ViewerInstance.cpp` (~:946-957), `Engine/ViewerInstancePrivate.h` (~:555), `Gui/ViewerTab.cpp` (~:877-909, 1754, 1864), `Gui/SceneViewport.{h,cpp}` (beside `setMaxSamples` ~:5099, `requestStageLoad` ~:3344)
  - approach: KnobBool shown as a checkable button. On: `requestStageLoad({"/"}, true, {})` and `progressive->setRenderEverything(true)`. Tooltip says it loads every payload.
  - verify: the m335 script turns it on — every payload loads, the frame covers all assets, the toggle survives save and load; off — rules return, nothing unloads.
  - size: M

- [x] M335.P3.T3 — USDRender and USDWrite unaffected; Render Everything matches them
  - files: `Tests/Scene/USDRenderCycles_Test.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: with a live `ProgressiveRender` holding rules in the same process, render through USDRender and write through USDWrite.
  - verify: output bit-identical to a run with no viewer; a fully loaded viewer stage's prim list equals USDRender's (default evaluation context).
  - size: M

**Verification gate:** `SceneDrawFilter_Test`, `ProgressiveRender_Test`, `CyclesDelegate_Test`, `MoonRayDelegate_Test`, `USDRenderCycles_Test`, `USDWrite_Test` pass; GUI scripts via explicit `--gui`: `Tests/gui/m335_render_draw_states.py`, `Tests/gui/m254_progressive_viewer.py`, `Tests/gui/scene_draw_boxes.py`, `Tests/gui/scene_draw_column.py`. Screenshots and UAT go to F4 - Heavy Scene Viewer's UAT.

## Decisions

- 2026-11-13 — Elaborated after the claim (consultant). No `Deps` on M364 - Delegate Cancel Responsiveness: the filter is applied on the worker thread between renders, which holds whether the worker renders the shared stage or its own. But M364's worker builds its `SceneStage` with the default `LoadAll`; whichever of F4 and F9 merges second must make the worker snapshot carry the viewer stage's `UsdStageLoadRules`, or delegate renders open every payload.
- 2026-11-13 — P1.T1 implemented (037de0a43). Both `newEngine` call sites pass `wantFilter=false` for now; P2.T1 flips the viewer worker to true. The scope is process-wide (a static mutex), so scoped engine builds serialise.
- 2026-11-13 — Delegate viewer renders fall back to a headlight when Hidden rows remove every scene light, as Storm does (user); P3.T4 added. Turning Render Everything off unloads nothing (PM, per "closing a row never unloads").
- 2026-11-13 — Phase 1 build (run 43440936742) failed two tests: `replaceRules` A→empty→A doesn't restore Hidden rows (a real bug), and the MoonRay light check assumed darker without the light (MoonRay's frame was brighter). Fixes in progress. `USDRenderMoonRayTest` timed out in one leg.
- 2026-11-13 — Phase 1 build failures were test errors (a8f40dce3). `replaceRules` A→empty→A matches a fresh filter in what is drawn, but boxed prims Hydra has held stay held invisible rather than removed, as `SceneDrawFilter` is designed to (Storm flips visibility instead of rebuilding batches); "identical" in P1.T2's verify means the same drawn result and rules (PM). hdMoonray turns on its own white dome light ("defaultLight") when the scene has no lights, and maps UsdLux `normalize` (default false) so the test light was nearly black; the light checks now use a red key light. P3.T4 adds the camera headlight for both Cycles and MoonRay, replacing MoonRay's dome fallback, so every renderer matches Storm (PM, per the user's headlight choice).
- 2026-11-13 — P2.T1 implemented (27cfa594b). Deltas are folded into the worker's rule copy (`applyToRules`) and the worker sends the difference with `replaceRules`, never paced; a change during a pass is caught by comparing `drawEdits`. Phase 1+2 build running.
- 2026-11-13 — P3.T1 implemented (a7ee1b24b). Found: `SceneViewport::pickRect` has no delegate branch and calls `ensureEngine`, so a rectangle pick under Cycles/MoonRay builds a GL engine; outside this task, the script clicks single points. Follow-up to file as an E14 stub if confirmed.
- 2026-11-13 — P3.T3 and P3.T4 implemented (03bbd3178). A scene with no lights at all never gets the headlight, as in Storm's viewer. The headlight is HdxTaskController's distant light, placed at the camera's view inverse. All tasks implemented; full gate running (the phase 1+2 build was cancelled as superseded).
- 2026-11-13 — Gate on 03bbd3178: ctest and three GUI scripts green; `m335_render_draw_states.py` failed 9 checks. Two product bugs: Render Everything queued the load of `/` without waking the load thread, and the box overlay kept winning picks while a delegate rendered everything (boxes hidden then, PM call). Script fixes: Load All under a drawn parent sets no rule, and the click grid could miss small buildings, so it now clicks the loaded prims' projected positions. Fixed in 3b738a4ae; re-gating.
- 2026-11-13 — Gate on 3b738a4ae: ProgressiveRender tests failed under load. A slow Cycles makes the worker remake its engine after 2 s, which these tests counted as draw-change rebuilds, and the headlight test looped (a remade engine's first render lacked the headlight, restarted too soon to be seen, and was remade again). Fix: a remade engine carries its predecessor's headlight, and the tests allow only that unseen remake (a new `unseenRemakes` counter). Cherry-picked M376's 44e08361c. GUI "Cycles draws a frame (drawn -1)" is unexplained; it passed on 03bbd3178. Re-gating (221af478a).
- 2026-11-13 — The gate on 221af478a was green apart from one check: Cycles picking found nothing. Cause (product): lowering Max Samples below the samples an unconverged Cycles render had already reached left Cycles stuck at sample 0 with empty AOVs. The worker now remakes the engine in that case, with a regression gtest. Only Cycles is covered, since there is no evidence for MoonRay. F4 (main and M401) merged in at 436d66926. Re-gating.
- 2026-11-14 — Same check failed on 436d66926. Cause: the remake condition used the samples count from the worker's last read, but Cycles keeps sampling in between, so it could already be past the new cap. Now any lowering on an unconverged Cycles engine that has drawn remakes it, and the remade engine keeps the headlight. New gtest. Re-gating.
- 2026-11-14 — Gate green on 84b133340 (run 43566540635). F4 had moved (M330 - Graph Draw Policy merged); merging it in hit conflicts in HydraRenderImpl.cpp and two tests, being resolved. Then PR into F4, review, and a re-gate on the merged tree.
- 2026-11-14 — F4 merged in (4fc6af35d): M398's `makeEngine` builds filterless offline engines; the Cycles and USDWrite tests keep both sides. PR #281 into F4 opened; Codex review and re-gate running.
