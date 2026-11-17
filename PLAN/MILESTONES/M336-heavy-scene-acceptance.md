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
