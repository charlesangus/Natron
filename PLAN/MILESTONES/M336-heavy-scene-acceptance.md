# M336 - Heavy Scene Acceptance

The end-to-end proof that M231 - Scene Graph Panel lacked: every budget together on the
fixtures, then the user on a real production scene. Design:
`PLAN/DESIGN/2026-11-04-scene-graph-design.md`.

Notes from M327 - Heavy Scene Spike (2026-11-07), see `PLAN/DESIGN/2026-11-07-heavy-scene-spike.md`:
- Add loaded reveal and cancel, and run Island (119,220 meshes and 30.9M instances, never yet loaded fully in the GUI).

## Scope

**In:**
- One scripted end-to-end run of every budget on both fixtures.
- A UAT with a real scene such as ALab or Moana Island, kept outside the repo.
- Fixes for what those find, as new tasks here.

**Out:**
- Storing a production scene in the repo or LFS: never.
- New features the UAT suggests: new milestones.

**Edge cases:**
- A budget revised after M327 - Heavy Scene Spike applies as revised. No second revision without the user.
- A real scene with no authored bounds opens as markers, by design (user). That's a finding to discuss, not a bug.

**Assumptions:**
- The user has, or can download, a production scene. Which one is the user's choice at UAT.

## Phase 336.1: Budgets

- [x] M336.P1.T1 — End-to-end budget run
  - files: `tools/bench/heavy_scene_bench.py`, `tools/bench/README.md`
  - approach: add a scenario chaining the whole workflow on both fixtures: open, twirl into a payload, set Full on one asset, Full + Load on a block, add a pattern node, switch to another node, select in the inspector, save, reopen. Run on a stripped release build on a GPU host and record the JSON under `## Phase 336.4: Bounded fill

- [x] M336.P4.T1 — Admission mode in SceneDrawFilter
  - files: `Engine/Nodes/Scene/SceneDrawFilter.{h,cpp}`, `Tests/SceneDrawFilter_Test.cpp`
  - approach: an admission mode seeds every rprim and instancer absent (an empty-type placeholder, nothing in Hydra); `admit(order, maxPrims, deadline)` turns absent prims into real inserts following a priority list; `admittedCount()`, `pendingCount()`, `isFilling()`. Loads join the queue through the `_arrivalRoots` path; lights and materials pass through; dirties on absent prims are dropped (admission reads current data). Instancers grow their instance mask per batch. A draw-state change or a new stage clears the queue.
  - verify: gtest — admission order honoured; each batch notices at most N prims; dirties on absent prims dropped; loads join the queue; instancer mask growth.
  - size: L
- [x] M336.P4.T2 — Near-camera order and boxes for unadmitted prims
  - files: `Gui/SceneDrawBoxes.{h,cpp}`, `Tests/SceneDrawBoxes_Test.cpp`
  - approach: the box worker sorts unadmitted prims by projected screen area for the current camera (near and large first), recomputed after the camera is idle 250 ms; stand-in boxes cover only unadmitted prims under the existing stand-in setting; picking an unadmitted prim goes through `SceneDrawBoxes::pick`.
  - verify: gtest — ordering for a known camera; the box set excludes admitted prims.
  - size: M
- [x] M336.P4.T3 — One admitting pass per frame, paced textures
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneRevealPacer*`, `Engine/Nodes/Scene/StormTextureBudget.{h,cpp}`
  - approach: `serviceDrawFilter` calls `admit` once per pass sized by `SceneRevealPacer` (budget = the stand-in budget, 33 ms) with `noteDrawFilterRendered` feeding back the cost; the background frame loop stops after one admitting pass, presents it, and takes the next on frameSwapped. No admission while the camera moves; after idle the order is re-sorted and the fill resumes. `StormTextureBudget::streamTextures` becomes time-paced: only textures bound by admitted geometry, nearest first, with a per-pass byte cap from the same budget.
  - verify: the GUI script below; the existing m336_stand_in, scene_draw_pick, scene_draw_boxes, scene_draw_instances and scene_viewer_navigation scripts pass.
  - size: L
- [x] M336.P4.T4 — Fill progress, completion and the bench
  - files: `Gui/SceneViewport.cpp` (properties), `tools/bench/heavy_scene_bench.py`, new `Tests/gui/m336_bounded_fill.py`
  - approach: publish `natronSceneFillProgress` and `natronSceneFillComplete` (no pending prims, no pending textures, converged); `natronSceneLastPaintMs` counts only a completed fill as converged. The bench reports first-pass and complete times, and an Island-loaded `orbit_fill` scenario records the worst pass, first-pass time, complete time and stall.
  - verify: the GUI script checks progress rises monotonically, completion is set, and a camera nudge mid-fill gets a new frame within a few passes.
  - size: M

## Decisions`.
  - verify: every budget passes; any failure becomes a task here with its numbers.
  - size: M

- [x] M336.P1.T2 — Bench follows M330's Initial Draw and waits for deferred edits
  - files: `tools/bench/heavy_scene_bench.py`
  - approach: open_file sets USDRead `initialDraw` to Full (Full + Load for load_state all), as the GUI tests do since M330 - Graph Draw Policy; the presented-change check waits (bounded) for the viewer's posted count, since queued edits post later; populate() records the draw-state stage timing for the first presented change.
  - verify: the flat workflow runs to its end under Xvfb without "the draw state changed something"; the GPU-host acceptance rerun (user) completes every scenario on every fixture.
  - size: M

## Phase 336.2: Real scene

- [ ] M336.P2.T1 — UAT AppImage and steps for a real scene
  - files: `build/appimages/` (AppImage plus same-named steps `.md`)
  - approach: package a stripped release AppImage after all of M329 - Viewer Draw States, M330 - Graph Draw Policy, M331 - Deferred Pattern Matching and M332 - Scene Graph Points are on main. The steps walk all five goals on the user's scene, and ask for times on open, reveal and orbit.
  - verify: the user's report is recorded here, with any fixes filed as tasks.
  - size: M

## Phase 336.3: Round-6 fixes

From the first valid baseline (VM, cloud-gpu-20261117T045608Z) and its sampled Island pass. The thresholds in each verify are the acceptance budgets for the VM rerun.

- [x] M336.P3.T1 — Fixture check only matches file paths; the stack tool re-symbolizes every Natron frame
  - files: `tools/bench/heavy_scene_fixture_check.py`, `tools/bench/heavy_scene_stack_window.py`, `tools/bench/heavy_scene_gpu_host.sh`
  - approach: the ptex pattern only matches a path, as in `Unable to open ptex (\S*(?:/|\.ptx)\S*) : `, since Storm node names (`HwPtexTexture_1`) are not files. Re-symbolize every frame of a module that has a .syms file, not only `??` frames (eu-stack names Natron.bin frames after the nearest exported symbol). Add a ≥20 Hz sampling mode for stalls under 1 s.
  - verify: re-checking the baseline's Island-loaded records gives judged results; a fixture with a renamed .ptx is still marked incomplete.
  - size: M
- [x] M336.P3.T2 — Paint never waits; overlays are drawn on the main thread
  - files: `Gui/SceneViewport.cpp` (paint ~8940-8990, picks ~8193/8221, overlay draw ~4760-4772)
  - approach: camera, resize and expose paints post the frame through the background-frame path and show the last finished one. Queued draw deltas count as busy. Grid, guides, handles and boxes become a main-thread pass over the copied frame at the current camera. Picks stay asynchronous with the existing busy-and-retry.
  - verify: orbit10k `stall_ms` ≤100 (city 293, Island 127); `twirl_stall_ms` ≤100 (Lobby 566, Island 996); scene_viewer_* and scene_draw_* GUI scripts green.
  - size: L
- [x] M336.P3.T3 — NVIDIA driver-lock spike (result to the user before building)
  - files: `Gui/SceneViewport.cpp` (render context creation ~6225-6245)
  - approach: during Island load_all, the main thread's `QOpenGLWidget::makeCurrent` blocks on a mutex inside NVIDIA's GLX library while the render thread uploads ptex and draws with VRAM saturated. With dense samples, try a render context with no share group (own X connection or EGL) and flushes between passes. Bring the findings back first.
  - verify: Island load_all `stall_ms` ≤100 (16.8 s); Island reveal, draw_budget and cancel stalls ≤100.
  - size: M
- [x] M336.P3.T4 — Box stand-in with an Off / During interaction / Always setting
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneDrawBoxes.h`, Settings, `tools/bench/heavy_scene_bench.py` (new orbit_full scenario)
  - approach: per build/m336-heavy-viewport-research.md. While the user interacts and the last full frame went over budget, draw Natron's instanced boxes on the main thread over the last frame. Start no full frame until idle, then refill all at once. No Storm visibility or render-tag switch. Depends on T2.
  - verify: Island-loaded orbit_full in During interaction: `orbit_fps` ≥30 and `stall_ms` ≤100.
  - size: L
- [x] M336.P3.T5 — Fixed cost of open and connect
  - files: `Engine/EffectInstance.cpp` (~6863-6900), `Engine/GPUContextPool.cpp` (~194-274), `Engine/ParallelRenderArgs.cpp` (~2068)
  - approach: `viewer.connectInput` → `InspectorNode::refreshActiveInputs` → `onKnobValueChanged_public` → `ParallelRenderArgsSetter` → `GPUContextPool::attachGLContextToRender` creates a GLX context on the main thread. Time the connect substeps, then create the context ahead of time or skip it for setters triggered by a knob change.
  - verify: `open_stall_ms` and populate_full `stall_ms` ≤100 on all four scenes (now 200-260).
  - size: M
- [x] M336.P3.T6 — pattern_node and panel reads off the main thread
  - files: `Gui/SceneGraphModel.cpp` (~1003-1106), the viewer input switch
  - approach: sample the synchronous viewer reconnect (683-700 ms on Lobby and Island) and fix what it shows. Gather `readEdits` and `readPrimData` on a worker.
  - verify: `pattern_node_stall_ms` ≤100 everywhere (610-735); flat `inspect_stall_ms` ≤100 (121.5).
  - size: M
- [x] M336.P3.T7 — Reopen tears the old stage down off the main thread
  - files: the project reset path, SceneStage and viewer release
  - approach: release the old UsdStage and engine on a worker or the render thread, without the main thread waiting.
  - verify: `reopen_stall_ms` ≤100 (flat 21.3 s, city 395, Lobby 680, Island 997).
  - round 9: the reset part (1.1 s flat, 0.94 s Lobby) is `SceneGraphModel::resetTo` destroying the item tree on the main thread (via `SceneGraphPanel::releaseNodeStages`); move it to the teardown worker.
  - size: M

- [x] M336.P3.T9 — Island load_all memory under admission
  - files: `Engine/Nodes/Scene/SceneDrawFilter.cpp` (~912-1015, 1259-1335, 1610-1640)
  - approach: round 9 killed Island-loaded load_all at ~89 GiB RSS (round 8: 20 GB); the render thread sat in `sendChanges` → `_SendPrimsAdded`. Find what each admit batch re-announces (prims already in Hydra, instancer masks over 30.9M instances); send dirties, not re-adds. Also the reveal_load_all stall (45 → 200 ms) now that loads join the fill queue.
  - verify: city-loaded and Island-loaded load_all RSS within ~10% of round 8; reveal_load_all stall ≤100.
  - size: L
- [x] M336.P3.T10 — Render context without the main thread waiting
  - files: `Gui/SceneViewport.cpp` (`initializeGL`, `startRenderContext` ~7271, ~10269)
  - approach: connect's 100-145 ms is `initializeGL` waiting on `callOnRenderThread(makeRenderContext)`; post it and hold paints until ready. On reopen, `initializeGL` must not wait behind the old loop destroying its engine. Also orbit10k's cadence (city 18 fps, Lobby exactly 30 fps after P4.T3's frameSwapped-gated loop) and what Lobby's zero boxes mean.
  - verify: open_stall_ms and populate_full stall ≤100; orbit10k ≥30 fps on city and Lobby.
  - size: L

- [x] M336.P3.T8 — GPU texture budget: textures past it are not loaded
  - files: `Gui/StormPtexBudget.cpp` and the Storm texture registry hooks it uses, Settings
  - approach: a total GPU texture budget (default a fraction of the card's VRAM, adjustable in Preferences) across ptex and UV textures. A texture that would go past it is not loaded; the material uses its fallback. Warn once per scene with the count left out. The data on disk and in the scene is never touched. CPU rendering as a fallback is a separate, later option.
  - verify: Island-loaded load_all on the 24 GB VM no longer saturates VRAM (vram_peak below the budget) and its `stall_ms` drops; the warning names the skipped count.
  - size: M

**Verification gate:** end-to-end run passes every budget on the GPU host; the user accepts the behaviour on a real production scene.
- 2026-11-14 — Claimed by bosley-native-nodes. P1.T1's `workflow` scenario written (code on milestone/m336-heavy-scene-acceptance); `heavy_scene_gpu_host.sh acceptance` runs it on flat, city, Lobby and Island. PM calls: save is judged as a main-thread stall over 100 ms, reopen as the stall after `loadProject` plus first frame under 2 s; the pattern is `<branch>//*`. The run itself is on the GPU host (benches stay off the dev host), so P1.T1 stays open until its JSON is recorded here.
- 2026-11-15 — F4 (all other members) merged in (e8c3fd624); release build run 43688632970 produced `build/appimages/Natron-m336-heavy-scene-acceptance.AppImage` + steps .md for the user's GPU-host `heavy_scene_gpu_host.sh acceptance` run. ctest: the task-graph leg failed ProgressiveRenderTest.ACapLoweredAboveTheSamplesLastReadEndsAtIt (precondition cap<kSamples, 257 vs 256), being diagnosed.
- 2026-11-15 — ProgressiveRenderTest.ACapLoweredAboveTheSamplesLastReadEndsAtIt (added by M335) was a deterministic setup failure on fast runners (the 32×32 scene reached all 256 samples in its first pass); fixed on F4 (%h) by rendering under a 1,000,000 cap. Product assertions unchanged; confirmed at the F4 ship gate.
- 2026-11-15 — GPU-host acceptance run (user, maxsettings RX 7900 XTX; build/bench-gpu/maxsettings-20261115T175545Z/heavy): **fails.** All four workflow runs and the reveal_wide/reveal_load_all runs aborted (exit 1, log ends at "Viewer hand-off rung: zerocopy"; "bbox … the draw state changed something" on every scene), so save/reopen/draw-state budgets were never measured. City and Lobby populate/load passed; Island populate_full stall 301.6 ms (>100) and first frame 4.76 s (>2 s), load_all stall 286.9 ms. Being diagnosed; fixes become tasks here.
- 2026-11-15 — Diagnosis: no product crash. The bench's require on a presented draw-state change failed because USDRead's Initial Draw (BBox since M330) was never set, so boxing a row under a boxed top-level prim changed nothing → os._exit(1). Island's numbers are for a BBox first draw and no stage breakdown was recorded; rerun after P1.T2.
- 2026-11-15 — P1.T2 done (bf31c48e1): Initial Draw Full / Full + Load, bounded wait for late-posted edits, populate records sync_to_first_paint_ms and the first change's stage breakdown. Local Xvfb check: the flat workflow runs every step (budgets fail on llvmpipe as expected). GPU-host rerun requested (`BENCHES=heavy .local/bench/run-maxsettings-benches.sh`).

## GPU-host run 2026-11-15 (maxsettings-20261115T185231Z, bf31c48e1)
8 pass / 12 fail / 1 abort. Diagnosis:
- Product crash: Island reopen segfaults — a SceneNodeStageSource worker hashes a node whose effect the project reset cleared (Node::getHashInputs, Node.cpp:~1724); workers are joined only at app quit. Fix in progress: join stage-source workers at project reset; lock-copy the effect in the hash walk.
- Bench bugs (fix in progress): failed checks recorded as pass; load_all under load_state all waits 1800 s; unrequested_payloads counted after a Full + Load open; orbit10k measures 300 boxes, not 10k, and orbits before the change is presented; 2 s first-frame budget applied beyond the payload city.
- Real perf failures left after the fixes (need GPU-host stack samples): flat open stall 2965 ms / pattern and reopen ~62 s / 20 GB RSS; per-batch fixed frame cost at 1M prims (city reveal_wide worst frame 2784 ms, reveal_load_all 6929 ms; Island reveal 1938 ms, stalls 500-750 ms); Lobby full_load_block worst frame 158 ms; Lobby cold first present 2.6 s.
- Separate: Storm fails to link ptex shaders for Island xgFronds (GetPatchCoord).
- 2026-11-15 — Build run 43755812218 on d67df10b4 green (ctest both legs, 6 scene GUI scripts). AppImage + Round 3 steps in build/appimages/Natron-m336-heavy-scene-acceptance.*; GPU-host rerun requested (Needs uat).
- 2026-11-15 — Round-3 GPU-host bench (maxsettings-20261116T025624Z; R9700 / 63 GB, previous runs on 7900 XTX / 96 GB): Island reopen crash fixed (reopen passes; exits are budget failures only). 8 pass / 10 fail. Bench bugs still open: Full + Load keeps loading after first frame, so load_all skips and later city scenarios measure a loading stage and count its payloads unrequested; cancel_paced never cancels mid-reveal; draw_budget not scheduled; ISLAND_MIN_RAM_GB too low (Island peaks 42 GB). Real perf failures: per-reveal-step fixed cost at 1M prims (city reveal_load_all 7.25 s worst frame; likely Storm re-gathering draw items on every rprim-index bump, SceneDrawFilter.h:355-361), Island main-thread stalls 1.2-2.1 s, flat open stall 469 ms / pattern 372 ms / reopen 148 ms, Lobby full_load_block 129.6 ms. Next: bench fixes plus opt-in SAMPLE=1 stack sampling (implementer running), then a rerun with samples before the perf fixes are designed.
- 2026-11-15 — Bench fixes plus SAMPLE=1 stack sampling (code: d902884d4); round 4 handed to the user with the same AppImage.
- 2026-11-16 — Round 4 first attempts died on two bench bugs (set -u on launch's stacks path, 3a0201a47; print(flush=) on Natron's stdout, fbae2e6ad). Smoke-run locally under Xvfb on the city fixture: workflow and the loaded chain (populate_full, load_all waiting for background loads, draw_budget with its table, reveal_wide, reveal_cancel_paced cancelling mid-reveal, orbit10k) all complete; failures only on llvmpipe budgets. Round 4 rerun handed back to the user.
- 2026-11-16 — Round 4 (maxsettings-20261116T045801Z, sampled): 14 pass / 10 fail. Passing now: flat open (94.9 ms), flat inspect, Island save, Island populate_full, city-loaded draw budgets. Bench is ~60% of main-thread samples (widgets_of per poll) and Natron.bin is stripped, so stalls are inflated and our frames unnamed. Causes: Island stalls (~1.9-2.1 s) are the main thread's Qt compose waiting (drmSyncobjWait) on the render thread's ~2 s GPU frame via the present fence (SceneViewport.cpp:8725-8736, 6271-6293); city reveal_load_all rebuilds every draw batch per insert step (_RebuildDrawBatches etc.); city reveal_wide pays HdChangeTracker::ResetVaryingState O(all rprims) per frame; flat pattern_node stall in SceneGraphModel readPrimData (GetKind/GetString) on the main thread; flat reopen destroys the old UsdStage on the main thread; ptex GetPatchCoord link failures on Island. Running: bench overhead/records/sampling-rate/debug-symbols fix; ptex link fix. Design questions to the user: non-blocking present, bounding GPU work per frame, reveal-by-visibility.
- 2026-11-16 — **Non-blocking present (user):** poll the present fence; if the GPU frame isn't done, show the last finished frame and retry, so the UI never waits on the GPU.
- 2026-11-16 — **Island GPU frame cost: profile first (user):** bisect the ~2 s GPU frame (lights, ptex/xgFronds materials, render tags) on the GPU host and bring back the cause before choosing a mechanism.
- 2026-11-16 — **Reveal by visibility (user):** the draw filter inserts rprims once as invisible and reveals by visibility dirties instead of inserting per step; refit the pacer so fixed cost isn't per prim. Check pxr's draw-items cache behaviour first.
- 2026-11-16 — Round-5 work committed: bench overhead/records/symbols/VRAM/BISECT (edc8f1d14); reveal by visibility, pacer refit, non-blocking present (two targets, polled fences, one frame on the GPU), Storm Ptex-binding filter for curves/points (0446637e2). GL OOM on Island-loaded was a one-off during a mid-draw_budget upload, not the per-change 2 s cost. Release gate (--tests --debug-symbols, scene_draw_*/scene_viewer_* GUI scripts) running; then round 5 to the user.
- 2026-11-16 — Resume note: M336 gate is GitHub run 43812863223 (release, --tests --debug-symbols, scene_draw_*/scene_viewer_* GUI). If the watcher was lost, fetch results with tools/ci/remote/build.sh --from-run 43812863223 (or gh run watch) instead of rebuilding.
- 2026-11-16 — Round-5 gate run 43812863223 failed to compile: `ScenePresentPacer_Test.cpp` passed `takeReady()`'s std::optional to EXPECT_TRUE/FALSE. Fixed in c865630c6; the gate is re-running (release, --tests, --debug-symbols, scene_draw_*/scene_viewer_* GUI). Round 5 goes to the user once it is green.
- 2026-11-16 — Round-5 gate run 43972140070 on 95e978bd8 green. The acceptance bench runs next on a rented Vast.ai NVIDIA VM (cloud-gpu skill) instead of the user's GPU host.
- 2026-11-17 — Vast.ai acceptance attempt on the round-5 AppImage produced no numbers: the first VM run lacked Tests/gui (run-gui-test.sh), so every run exited 127; a rerun was cancelled by the user and the VM destroyed. P1.T1 still needs the round-5 run (user's GPU host, or a VM on request).
- 2026-11-17 — VM runs, all on Vast.ai RTX 4090 / 98 GB with inputs staged through R2. Earlier exit 127s were the image lacking libOpenGL.so.0, not the missing Tests/gui. The 30-minute workflow stalls on flat and city came from the VM's screen having no monitor attached (`UseDisplayDevice None`): the window was 678x353 and the scene never painted, and after two kills GLX was dead for the remaining labels. Harness changes in e2fd5ceb5: BENCH_TIMEOUT_S can be set, a failed open dumps the viewer and screen state, and a GPU wedged after a label stops the batch. With a connected 1920x1080 monitor the city open probe paints (first frame 922.7 ms; its stall was sampled, so it isn't judged). The full unsampled acceptance run is going on the same VM.
- 2026-11-17 — **Round-5 acceptance run, complete, on the VM** (Vast.ai RTX 4090, driver 580.95.05, 98 GB, unsampled; .local/vast/runs/cloud-gpu-20261117T041339Z/out/heavy-scene-acceptance.jsonl): 7 pass / 13 fail / 4 skipped.
  - Passing: load_all on city, Lobby and Island; draw_budget on city and Island; city reveal_cancel_paced; Island orbit10k.
  - Every open and populate_full stall is 200-265 ms, against a 100 ms limit, on every scene.
  - flat: pattern_node 623 ms, reopen 604 ms, inspect 124 ms, 20.8 GB RSS.
  - city: reveal_wide worst frame 427 ms, reveal_load_all worst frame 456 ms, orbit10k stall 294 ms.
  - Island: full_load_block worst frame 3.6 s; reveal_wide 5.4 s stall and 10.9 s worst frame; reveal_cancel_paced 5.4 s stall.
  - Lobby: its loaded scenarios are skipped ("no such row").
  - A consultant is classifying each failure as product, environment or bench before fix tasks are filed.
- 2026-11-17 — Round-5 VM triage (consultant): the run is not valid for acceptance.
  - Lobby `Source/` and Island `textures/` were not staged (PM staging error), so Lobby had 0 gprims and its loaded scenarios skipped, and Island ran without ptex.
  - load_all and draw_budget "pass" while Full + Load is still queuing payloads (loads_settled reports early).
  - Main finding: paint still blocks the main thread until the render thread's Storm render returns (SceneViewport.cpp:8958 callOnRenderThread → renderIfIdle). Round 5 removed only the wait on GPU completion. This is the likely cause of Island's 5.4 s stalls and part of the ~230 ms open stall. It goes to the user with the trade-offs.
- 2026-11-17 — **Open budget is judged warm (user):** the judged open runs with an already-populated GL shader cache after a warm-up; the cold first-ever open is recorded, not judged.
- 2026-11-17 — **Island acceptance includes its textures (user):** the full Island, `textures/` included, is staged for the VM run.
- 2026-11-17 — Viewer heavy-frame research (consultant; build/m336-heavy-viewport-research.md, sent to the user): the other tools limit work by what is loaded, expanded or counted (Houdini's Scene Polygon Limit and distance culling to boxes; Katana and Gaffer expansion), and none uses a time-budget fallback. Recommended: paint never waits, plus a box stand-in drawn on the main thread over the last frame (Natron's own instanced boxes, no Storm switch, since render-tag and visibility switches re-gather batches), plus an all-at-once refill after idle.
- 2026-11-17 — **Stand-in mode is a setting (user):** Off / During interaction / Always.
- 2026-11-17 — **Explore Houdini-style viewer budgets (user):** user-controllable polygon budgets, distance-based culling or reduction to bounding boxes, and drawing only a fraction of the polygons beyond a distance. These are new features outside M336's Scope ("New features the UAT suggests: new milestones"), so they go to /cat-plan as a new milestone. M336 keeps the paint wait and the stand-in once the sampled Island profile scopes them.
- 2026-11-17 — **First valid round-5 baseline** (VM RTX 4090 24 GB / 98 GB; complete Lobby, textured Island, warm shader cache; bench b956b3777; .local/vast/runs/cloud-gpu-20261117T045608Z): 8 pass / 15 fail, plus the warm-up.
  - Every Lobby loaded scenario passes.
  - The open stall is still 200-250 ms with a warm cache, so it is not shader compiles.
  - flat: pattern_node 610 ms; reopen 21.3 s, now including the project reset; 21 GB RSS.
  - city: wide time to picture 2.2 s; reveal worst frames ~0.5 s; orbit stall 293 ms.
  - Lobby and Island workflows: twirl 0.6-1 s, pattern_node ~0.7 s.
  - Island: Full + Load on a block, worst frame 5.9 s. Loaded: load_all stall 16.8 s at 61 GB RSS with VRAM saturated (24.1 of 24.6 GB); time to picture 2.5-2.7 s; populate, reveal and cancel stalls ~250 ms.
  - Island-loaded records are tagged fixture_incomplete by two HwPtexTexture_1 lines, probably a false positive.
  - A sampled Island pass (1412 stacks) is with a consultant for stall locations and the M336.P3 task scope.
- 2026-11-17 — **City time-to-picture and reveal-frame budgets are not judged for now (user):** "Ok for now. Will revisit later with different techniques." M336 judges UI stalls on the city; the 1M-prim change cost is left for later work.
- 2026-11-17 — **Island's ~2 s GPU frame is accepted (user):** M336 judges Island on UI stalls only; the frame cost goes to the Houdini-style viewer-budgets milestone.
- 2026-11-17 — **VRAM is a real limit (user):** if the textures won't fit, don't load the ones past the cap (or render on the CPU). The rule against size caps is about trimming user data, not GPU memory. This becomes P3.T8, with not loading past the budget as the mechanism.
- 2026-11-17 — **Stand-in looks (user):** wireframe boxes over a cleared view. The other defaults stand as proposed (33 ms budget, one box per model-kind prim else per draw-state row, refill after 250 ms idle, default mode During interaction), each a Preference.
- 2026-11-17 — P3.T1 done (9cf673ce8). Re-judged baseline: 7 pass / 17 fail / 1 warm-up (heavy-scene-acceptance.rejudged.jsonl); the six Island-loaded records are plain fails. Dense sampling: eu-stack costs ~170 ms per whole-process sample; SAMPLE_TID_COMM samples named threads only.
- 2026-11-17 — P3.T5 done (a1759cbfb): ParallelRenderArgsSetter with setUpstreamArgs=false (knob change, input change) attaches no GL context. Test KnobChangeGLContext_Test (needs GL; run-gl-tests.sh). Not covered: ~40 ms of USDRead initialDraw/filename setValue (computeFrameRangeForReader, the reader's knobChanged) and possible synchronous layer reads on layerListRefreshed.
- 2026-11-17 — P3.T7 code in 0b5c8223d: SceneTeardown hands a closed scene's stages and layers to one worker (GUI only; background runs and tests destroy at once), and quit waits for it. Follow-ups: the open path reloads layers whose files changed on disk while a teardown is pending (implementer running); the Storm engine's destruction in ~SceneViewport stays on the main thread until P3.T2 lands. Unbuilt; the gate checks it.
- 2026-11-17 — P3.T2 code in 39d5e2d24 (unbuilt): paintGL never renders. View, size and expose changes render as background frames, and the last frame is shown stretched until the matching one lands. Overlays are drawn on the main thread at the current camera, try-locking the viewer and falling back to the last read. Queued draw deltas count as busy. natronSceneLastPaintMs advances only for a converged frame of the current view. PM call: when the shown frame's camera differs from the current one, overlays skip the frame's depth (folded into P3.T4).
- 2026-11-17 — Stale layer on reopen during pending teardown (consultant): never reload or reuse a stale pending layer in place. Reloading would send change notices that make the dying stages recompose on the writing thread (a main-thread stall), and every open path holds a ReadScope. Instead, the open path detects a stale pending layer (FileStampRegistry::matchesDisk), defers that node's evaluation with an uncached 'Waiting for the previous scene to close' result, and re-keys the node when the teardown queue drains. No main-thread wait. Implementer running.
- 2026-11-17 — P3.T8 code in 5ed197d6d (unbuilt; the accounting unit test passes on the host): a Storm material filter sizes each texture from its file header with Storm's own sizing before Storm opens it. A texture past the budget has its node replaced by its fallback, and one warning gives the count and the budget. Preference gpuTextureBudget (MB, 0 = half the card's memory, read via the NVX, ATI or Mesa queries, 4 GiB if unknown). The budget is shared across the process; a change applies to textures loaded afterwards; a refused normal map falls back to the geometry normal.
- 2026-11-17 — P3.T4 code in e755bb36d (unbuilt):
  - Preferences: sceneStandInMode (Off / During Interaction / Always, default During Interaction), sceneStandInBudget (33 ms), sceneStandInBoxes (Per Model, or per draw-state row), sceneStandInRefillDelay (250 ms).
  - A second box worker feeds instanced wireframe boxes from drawOverlays.
  - A full frame's cost runs from its start to its fence being seen, with hysteresis at 0.8×.
  - Properties: natronSceneStandIn, natronSceneLastFullFrameMs, natronSceneStandInBoxes, natronScenePaintCount.
  - orbit_full bench scenario; Tests/gui/m336_stand_in.py.
  - PM calls: Per Model boxes every model-kind prim. Under Always, a pick still takes in pending stage changes with the main thread waiting; that is a follow-up, since Always never renders.
  - The GUI harness defaults the stand-in to Off, so llvmpipe scripts aren't affected; scripts and the bench opt in (implementer running).
- 2026-11-17 — Round-6 code complete on 26d84f773: P3.T2, T4, T5, T7 (with the stale-layer deferral, 89a8ac725 and 89bee6a6e) and T8, plus the GUI harness stand-in default. Syntax check clean. Gate run 44032201649 (release, --tests, --debug-symbols, every scene_draw_*, scene_viewer_* and m336_* GUI script). VM acceptance rerun next; P3.T3 (driver-lock spike) and P3.T6 (pattern_node) wait for its numbers.
- 2026-11-17 — **Round-6 VM acceptance** (RTX 4090, AppImage 0d1acb8b7; .local/vast/runs/cloud-gpu-20261117T135449Z): 10 pass / 17 fail; the sampled Island pass is 2 pass / 2 fail.
  - Better: open and populate stalls 115-167 ms (were 200-260); city reveals and the paced cancel pass; orbit_full passes on city and Lobby; Lobby loaded passes except orbit10k; Island load_all 5.3 s (was 16.8 s).
  - Regressions: flat pattern_node stall 42.9 s (was 610 ms); Island-loaded change stalls 5.2-5.5 s (were ~250 ms) with time to picture 10.9 s; Island orbit_full stall 688 ms; orbit10k 0.2 fps, probably a measuring artifact of the stand-in.
  - Unchanged: flat reopen 20 s, Island VRAM ~24 GB despite the texture budget.
  - A consultant is diagnosing the regressions.
  - The first VM attempt failed in VM prep: apt upgraded NVIDIA user space past the kernel module. vm-prep now holds *nvidia* packages.
- 2026-11-17 — Round-6 regressions (consultant):
  - Island change stalls: the main thread is blocked in NVIDIA's GLX mutex while Qt makes the shared widget context current, with the render thread in a long Storm render. That is P3.T3's driver lock, made worse by a frame roughly 5x slower than round 5, probably VRAM residency at ~23.5 of 24.5 GB.
  - The texture budget engages (266 textures refused at 12282 MB), but geometry takes another 9-11 GB.
  - Flat reopen: the Storm engine release still runs through callOnRenderThread while the main thread waits; P3.T7 moved only the UsdStage. The reaper fix is with an implementer.
  - orbit10k 0.2 fps is a bench artifact: it counts only full frames, which the stand-in suppresses. Fixed in the bench by running it with the stand-in off.
  - Flat pattern_node 42.9 s is not settled; it needs a sampled flat run at ≥20 Hz.
- 2026-11-17 — **P3.T3: prototype both and measure (user):** (a) a render context with no share group (own X connection or EGL) with the frame handed across, and (b) holding widget repaints while a long render-thread job runs. Both are measured on the Island-loaded VM run before choosing.
- 2026-11-17 — **Texture budget is free memory minus headroom (user):** a texture is admitted only while the card's currently free VRAM stays above a headroom, so geometry is accounted for implicitly. This replaces half of total.
- 2026-11-17 — Free-memory budget code committed. The automatic headroom is max(1 GiB, total/16), 1.5 GiB on a 24 GB card. A reading also notes the bytes admitted so far; until the first reading the half-of-card fallback applies. A non-zero Preference is a fixed cap on textures alone.
- 2026-11-17 — **Textures stream in after geometry (user):** the first frame draws with fallback colours, and textures are admitted over the following frames against free memory measured after geometry is resident, so the first sync no longer over-admits. Free memory is read before each render (the viewer and HydraRenderImpl). This follows P3.T3's prototype, since both edit the render loop.
- 2026-11-17 — Round-7 code complete: Storm engine teardown off the main thread (bd215fe08), free-memory texture budget (50b8899e5), P3.T3 prototypes behind NATRON_SCENE_RENDER_CONTEXT=shared|unshared|holdrepaint (d12e9c2c2), textures streaming after geometry in batches of ≤512 MB or 64 textures per frame (latest). PM calls: offline renders decide textures at first sync, since their output can't show fallbacks; textures from material edits also wait one frame. The gate is running, then the VM run: VARIANTS on Island loaded, plus a flat run sampled at ≥20 Hz.
- 2026-11-17 — The round-7 gate (44121797621) compiled but failed packaging on AppStream validation: the lane lacked main's #297. main merged straight into the M336 branch (679cb23dd, no conflicts). Merging main into feature/f4-heavy-scene-viewer conflicts in 19 files, so it waits for the F4 ship, which brings M336's resolutions with it. Gate re-running (44149101933).
- 2026-11-17 — Round-7 VM run (RTX 3090, cloud-gpu-20261117T201054Z) on the 679cb23dd build: 11 of 27 pass. orbit10k reads 0.2 fps on every scene (f8ed3e99f did not fix the artifact). Flat pattern_node still stalls 44.7 s; the sampled flat pass is under out-flat/. Island variants, worst main-thread stall: shared 1.7–4.0 s; unshared 0.2–0.46 s but load_all 685 s (vs 230 s) and VRAM at the 24 GB cap; holdrepaint 26–86 ms in draw_budget and reveal, load_all 190 s, populate_full 614 ms, orbit10k 504 ms.
- 2026-11-17 — **P3.T3: holdrepaint (user).** The viewer shares the render context and holds widget paints while a render-thread job runs past 50 ms; the unshared prototype and the NATRON_SCENE_RENDER_CONTEXT switch are removed. Next: the orbit10k artifact, holdrepaint's populate_full and orbit10k stalls, and P3.T6 from the flat samples.
- 2026-11-17 — **Round 8** (RTX 3090, e35a2f47e, run cloud-gpu-20261118T020609Z): 11 pass / 16 fail. pattern_node on flat fixed (44.7 s → 58 ms); Island-loaded populate_full 614 → 191 ms; Lobby orbit10k 59 fps. Bench bugs: the workflow twirl waits for a converged paint a moving camera never makes (900 s walls); reveal worst-frame timed from converged paints only; decisions on city ttp/reveal and Island ttp not applied in verdicts; the all-thread sampler managed one sample per ~1.5 s. Real: flat reopen 5.5 s (SceneDrawBoxes join and bbox-cache frees, and a timer-driven ~UsdStage, on the main thread); Island change stalls 1.2–3.1 s in one run only, with pollPresent's makeCurrent round-trips the main busy stack; Island orbit10k shows nothing for 5 s while a 22.8 s frame renders; pattern_node reconnect (P3.T6) and reopen reset (P3.T7) remain; open connect 104–141 ms. Fixes dispatched for the bench, reopen teardown, present polling and orbit cancel.
- 2026-11-18 — **Bound the Storm pass (user):** a pass can't be stopped midway, so on the loaded Island orbiting froze the picture for up to one ~23 s pass. Storm's render index is filled in prim batches so each pass covers a bounded slice of the stage and ends within about one budget (33 ms target); the first full picture builds up progressively and a camera change gets a new frame quickly. Chosen over predicting cost with the stand-in, a separate stand-in GL context, or accepting the freeze.
- 2026-11-18 — **Bound the fill only (user):** M336 bounds passes while the scene fills (prims and textures, nearest first, paused while the camera moves; Phase 336.4, design by consultant: admission in SceneDrawFilter, prims absent from Hydra until admitted). After the fill a pass still draws the whole scene (~2-4 s on Island, accepted 2026-11-17); the steady draw belongs to M481 - Viewer Polygon Budgets, where drawing only stand-in boxes while the camera moves on an over-budget scene is recorded as a possible follow-up. Slicing the steady draw into accumulated passes was rejected (unverified Storm internals, re-gather per slice).
- 2026-11-19 — **Island steady-frame composite stalls go to M481 (user):** on the loaded Island a steady GPU frame takes 1.5-6 s, and any other widget's repaint makes Qt composite the window behind it (main-thread stalls of 0.8-4.3 s in draw_budget, reveal_wide and reveal_cancel_paced). M336 waives those Island-loaded stall checks with this reason; M481 - Viewer Polygon Budgets bounds the steady frame. Rejected: holding all window compositing while a frame is on the GPU (the window looks frozen), and pulling the steady-draw budget into M336.
- 2026-11-18 — Phase 336.4 in 2f222fabe (with 6fcdb7965's reopen teardown, present-fence watch, view-change frame stop and bench fixes); gate relaunched. PM calls: stand-in mode Always renders no frame, so no fill runs there (as that mode means); textures for materials bound only outside the ordered rows wait until the fill completes; the fill reuses the reveal pacer (first batch 1024 prims) and a reveal never runs while admitting; 'clears the queue' resets the order cursor; newly added prims are queued (bypassing the reveal for loads). natronSceneSynced and converged paints now wait for the fill.
- 2026-11-18 — Phase 336.4 gate green on f48cb66dd (run 44386092853, m336 GUI rerun 44402586874 after the city doubled to 40 blocks; WholeFrameTest one-off, passed on rerun). The m364 first-Storm-frame failures were variance: the probe shows 1 pass and a complete fill on the first frame after a switch, like steady frames. Round-9 VM acceptance launched (build/vast-logs/m336-r9.sh, AppImage from 44386092853).
- 2026-11-18 — **Round 9** (RTX 3090, f48cb66dd, cloud-gpu-20261118T213824Z): 8 pass / 13 fail; Island-loaded killed at ~89 GiB RSS in load_all (new, P3.T9). Diagnosis (consultant): open/populate stalls are the render-context wait (P3.T10); reopen is the model reset plus initializeGL waiting on the old engine teardown (P3.T7); pattern_node/inspect/twirl unchanged (P3.T6); reveal_cancel_paced is obsolete under the fill (a BBox→Full row's prims queue for the fill, so no reveal runs) and becomes a fill-cancel check; orbit10k cadence regressed (P3.T10). Flat first frame 239 s (115 s in round 8), mostly worker-thread composition. Bench: record out_of_memory, a per-second RSS line, sample only main and render threads.
- 2026-11-19 — **Round 10** (RTX 3090, d5f4f856d, cloud-gpu-20261119T032728Z; commit inferred from timing): 10 pass / 15 fail / 2 timed out (flat workflow, Island-loaded orbit_full). Island load_all RSS is back to 12.9 GB, so the round-9 OOM is fixed (P3.T9). City and Lobby orbit10k: 51.8 and 59.7 fps. Fails: open 111–131 ms; pattern_node city 156.7, Lobby 853.7, Island 308.3; reopen 360–944; populate_full 104–140; city load_all 269.4; reveal_load_all 360.1; Island save 598.7 (once only). reveal_cancel_paced on city and Lobby finished before the cancel landed (a bench bug, fixed). The Island-loaded composite stalls are waived to M481. The Island-loaded orbit10k stall of 181.2 is not covered by any waiver.
- 2026-11-19 — **Round 11** (RTX 3090, c4b3fee9b, cloud-gpu-20261119T114434Z): 17 pass / 3 fail / 4 aborted. Every workflow aborted because the bench read Scene Graph rows before the worker delivered them (fixed in 828cf15c4). populate_full passes on all four scenes (72–99 ms). Fails: city load_all 280.4, reveal_load_all 105.7, Lobby time_to_picture 103.1.
- 2026-11-19 — **Round 12** (RTX 3090, 7df47341a, cloud-gpu-20261119T154147Z): 14 pass / 10 fail, none waived. Met: flat inspect 83.9; orbit10k city 53.4 and Lobby 59.7 fps (P3.T10's orbit part); load_all RSS within round 8's (city 13.0 GB, Island 19.5 GB; P3.T9's memory part). Fails: open 105–122; populate_full 103–139; pattern_node city 129, Lobby 803, Island 375 (P3.T6, Lobby unchanged since round 8); reopen flat 1236, city 366, Lobby 974, Island 489 (P3.T7); city load_all 428.6; reveal_load_all 354.0. Half the flat reopen samples join a thread inside widget deletion, which 9f1afcfd7 hands to the teardown worker.
- 2026-11-20 — 9f1afcfd7 gate green (44587451262), but it had no symbols. A release --debug-symbols rebuild for round 13 is running. Round 13 samples Lobby and Island too (accept-lobby, accept-island; SAMPLE_INTERVAL_S 0.05), because P3.T6's ~800 ms Lobby reconnect has never been sampled. If open and populate stay at 100–140 ms after round 13, the user decides whether that is acceptable.
- 2026-11-20 — Round 13 setup failed twice: the VM's R2 download of the textures tar was reset (curl 56), on 3090 offers 60679268 and 60202942. No RTX 3090 with at least 128 GB RAM and reliable bandwidth is left, so round 13 runs on an RTX 4090 with 128 GB (≤ $1/h, ≥ 500 Mb/s down), like round 6. Compare its numbers with the 3090 rounds with that in mind.
- 2026-11-20 — **Round 13** (RTX 4090, 9f1afcfd7 with symbols, cloud-gpu-20261120T204549Z) is invalid. Partway through accept-flat the VM swapped NVIDIA user space to 580.178.04 under kernel module 580.95.05 (dmesg `NVRM: API mismatch`). Every later GL client was refused, and the harness recorded each later label as gpu_wedged. Natron exited normally, and no Xid errors appeared. The flat results taken before or around the swap: reopen 401 (round 12: 1236), open 247 (122), pattern_node 393 (22), inspect 42 (84); not trusted given the swap. The VM prep now stops and masks apt-daily and unattended-upgrades, waits for dpkg, and fails if the NVML and kernel versions differ. Round 13 is re-queued after the M153 bench.
- 2026-11-20 — **Round 13** (RTX 3090, 9f1afcfd7 with symbols, cloud-gpu-20261120T223311Z; driver consistent, no API mismatch): 18 pass / 6 fail. Island-loaded draw_budget 3966 and reveal_wide 3920 are waived to M481.
  - **Met:** open on city/Lobby/Island (90–93); Lobby populate 87.4; reveal_load_all 45.1; Island load_all at 13.1 GB; orbit10k 50.9/59.9 fps.
  - **Fails:** flat open 100.8; populate city-loaded 122.4 and Island-loaded 106.7; pattern_node city 107, Lobby 720, Island 329; reopen flat 660, city 360, Lobby 780, Island 373.
  - **Bench artifact:** city load_all's pass doesn't count. It settled before its load edit, and the loads ran inside draw_budget at 18.8 GB.
  - **Samples** (3–4 Hz, not 20):
    - pattern_node is the viewer flipping to the 2D viewer on disconnect (ViewerTab::onActiveInputDataKindChanged → QOpenGLWidget::initialize → glXCreatePbuffer).
    - reopen is restoreDefaultLayout rebuilding the layout (wipeLayout/TabWidget::setParent → GL widget re-init) because the layout no longer matches the default, plus ~ViewerGL.
    - populate waits on the GIL.
  - Open/populate at 100–122 goes to the user. Fixes dispatched: no 2D flip on an empty input, a structural layout-kept test with deferred viewer deletion, and the bench's load-settle, poll timer and sampler.
- 2026-11-20 — **Open and populate at 100–122 ms are within tolerance (user):** flat open 100.8, populate city-loaded 122.4 and Island-loaded 106.7 count as met. With orbit10k already met, P3.T10 is met. What remains: pattern_node (P3.T6), reopen (P3.T7) and the city load_all re-measure (P3.T9).
- 2026-11-21 — The df10e4a65 no-flip change broke scene_viewer_flip and scene_graph_follow. An empty input reported Image, so a later 2D input sent no kind change, and the Scene Graph panel didn't re-pick its source. Fixed in e282175d4: the viewer also signals when input A connects or disconnects, and the panel re-applies its source on every viewed-node change. Gate green (release, ctest 916/916, all 24 scene GUI scripts, symbols). Round 14 launched on the same 3090 host class as round 13 (offer 47687661).
- 2026-11-21 — **Round 14** (RTX 3090, e282175d4 with symbols, cloud-gpu-20261121T060139Z; driver consistent, no API mismatch): 18 pass / 6 fail.
  - **Met:**
    - pattern_node: flat 24.6, city 36.2, Lobby 30.1, Island 22.6 (P3.T6).
    - city load_all 57.4 at 13.07 GB, measured validly (the load edit came 4.6 s in and settled after 89.7 s); reveal_load_all 53.8 (P3.T9).
    - Island-loaded reveal_wide 39.4.
  - **Tolerated:** populate city-loaded 115.7 and Island-loaded 116.4. **Waived:** Island-loaded draw_budget 148 (M481).
  - **Fails:** reopen flat 395, city 217, Lobby 1264, Island 641 (P3.T7; Lobby and Island are worse than round 13). Flat open was 170.1 in one run (95.0 sampled, no stacks).
  - **Samples:**
    - Every reset still rebuilds the layout: the structural kept test never matches, so flat reopen runs wipeLayout.
    - The deferred ~ViewerTab destroys the SceneViewport's QOpenGLContext on the main thread, where the VAO teardown creates a pbuffer (glXCreatePbuffer, 3 of 4 Lobby samples).
    - The new viewer's ViewerHandoff interop probe also costs time.
  - **Next:** log the structure difference, release or reuse the viewer GL context off the reopen path, cache the handoff path, then round 15.
- 2026-11-21 — Reopen fixes:
  - **f7856e0d7:** a reset closes only the panes added since the default (the twirl's split made the old kept test always fail); closing a pane moves its neighbour straight into the parent splitter; a dying GL context is left current so Qt's VAO teardown makes no pbuffer; the handoff path is cached per device.
  - **Latest commit:** a split inserts the new splitter in place first, so GL widgets never leave the window. Floating-pane splits still detach (FloatingWidget::takeWidget); that's a follow-up.

  Gate running, then round 15.
- 2026-11-21 — **Ship it (user): "don't worry about small residual misses, I want to get it shipping. Finish up what's in progress and fix bugs only."**
  - Round 14's remaining budget misses are accepted, and P1.T1 is closed on round 14.
  - P3.T7 closes once its in-progress fixes (f7856e0d7, dd577db69, c46c7ed61) pass the gate, whatever the reopen budget.
  - No VM round 15 and no new performance work; only bugs get fixed.
  - Then: PR into F4, one review round (bugs only), merge. P2.T1's real-scene UAT happens at F4's UAT.
- 2026-11-21 — P3.T7 closed per the "ship it" decision: PR #330 CI green on ab6c0039e with f7856e0d7, dd577db69 and c46c7ed61 (after format/shellcheck fixes in 6071c05d2 and ab6c0039e, and two re-runs of MoonRay/ProgressiveRender/KeyMix flakes that pass serially and are not M336 code; logged under # Open questions). The round-9 resetTo teardown was already moved off the main thread in d5f4f856d. P2.T1 (UAT AppImage on a real scene) goes to F4's Feature UAT. Next: the review round on #330, then merge into feature/f4-heavy-scene-viewer.
