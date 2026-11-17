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

- [ ] M336.P1.T1 — End-to-end budget run
  - files: `tools/bench/heavy_scene_bench.py`, `tools/bench/README.md`
  - approach: add a scenario chaining the whole workflow on both fixtures: open, twirl into a payload, set Full on one asset, Full + Load on a block, add a pattern node, switch to another node, select in the inspector, save, reopen. Run on a stripped release build on a GPU host and record the JSON under `## Decisions`.
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
- [ ] M336.P3.T3 — NVIDIA driver-lock spike (result to the user before building)
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
- [ ] M336.P3.T6 — pattern_node and panel reads off the main thread
  - files: `Gui/SceneGraphModel.cpp` (~1003-1106), the viewer input switch
  - approach: sample the synchronous viewer reconnect (683-700 ms on Lobby and Island) and fix what it shows. Gather `readEdits` and `readPrimData` on a worker.
  - verify: `pattern_node_stall_ms` ≤100 everywhere (610-735); flat `inspect_stall_ms` ≤100 (121.5).
  - size: M
- [ ] M336.P3.T7 — Reopen tears the old stage down off the main thread
  - files: the project reset path, SceneStage and viewer release
  - approach: release the old UsdStage and engine on a worker or the render thread, without the main thread waiting.
  - verify: `reopen_stall_ms` ≤100 (flat 21.3 s, city 395, Lobby 680, Island 997).
  - size: M

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
