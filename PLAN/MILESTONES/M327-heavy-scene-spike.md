# M327 - Heavy Scene Spike

Generated heavy fixtures and a budget harness, which M231 - Scene Graph Panel never had, and
the numbers that settle how draw states reach Hydra. Design:
`PLAN/DESIGN/2026-11-04-scene-graph-design.md`; evidence:
`PLAN/DESIGN/2026-11-04-scene-graph-research.md`.

## Scope

**In:**
- A script generating a 1M-prim payload city and a 1M-prim flat file, plus a harness measuring the design's four budgets.
- Prototypes of both mechanisms on those fixtures: a per-engine Hydra filter with a GL box overlay, and session-layer draw mode.
- Measurements of off-lock payload loading and of deferred pattern re-matching.
- A results note, and the user's mechanism decision.

**Out:**
- Shipping either mechanism — M329 - Viewer Draw States.
- Merging prototype code. Only fixtures and the harness merge; prototypes stay on spike branches.
- Real production scenes — M336 - Heavy Scene Acceptance.

**Edge cases:**
- Timings from a GitHub runner or llvmpipe don't count. Main-thread stall and file-open checks are still valid under Xvfb; frame rates need the GPU host.
- Fixtures never enter the repo or LFS; only the generator does.

**Assumptions:**
- `HdSceneIndexPluginRegistry::RegisterSceneIndexForRenderer` callbacks receive a `renderInstanceId` that identifies one engine (from source; undocumented, not run).
- The stage scene index still walks pruned prims. Its cost at 1M prims is unmeasured.
- Crate payloads read prim structure without paging in geometry arrays until values are requested.

## Phase 327.1: Fixtures and harness

- [x] M327.P1.T1 — Heavy scene fixture generator
  - files: `tools/bench/heavy_scene_make.py` (new), `tools/bench/README.md`
  - approach: deterministic generator writing under `build/bench`. Payload city: about 300 blocks of buildings, each building a payload to its own `.usdc`, with `assembly` and `component` kinds, about 1M prims once all are loaded, `extentsHint` authored outside the payload on 90% of payload prims, and none on the rest so markers get exercised. Flat file: 1M prims, no payloads. Reuse `scene_edit_bench.py make-file` where it fits.
  - verify: inside natron-dev, a pxr script opens each with `LoadNone` and `LoadAll` and prints prim counts: the city's composed count before loading is under 100k, and about 1M once loaded; the flat file is about 1M either way.
  - size: M

- [x] M327.P1.T2 — Budget harness
  - files: `tools/bench/heavy_scene_bench.py` (new), `tools/bench/README.md`
  - approach: a Natron GUI script run under the existing bench conventions. Heartbeat `QTimer` for the longest main-thread gap, time from file set to first boxes drawn, `Sdf.Layer.GetLoadedLayers()` before and after an action to catch unrequested payload files, scripted orbit for frame rate. Prints one JSON line per scenario with pass or fail against the budgets in the design note. Scenarios take the action as a parameter, so later milestones add theirs.
  - verify: run against main with the flat file under Xvfb; it prints stall and layer numbers, and reports frame rate as not valid on llvmpipe.
  - size: M

## Phase 327.2: Mechanism prototypes

- [x] M327.P2.T1 — Prototype the Hydra filter and box overlay
  - files: spike branch: `Gui/SceneViewport.{h,cpp}`, a new scene index filter under `Gui/`, the viewport's guide drawing
  - approach: register a filter per engine, keyed on its `renderInstanceId`, with a mutable set of pruned prefixes. Prune and restore a subtree at runtime, add prims in per-frame batches, and draw boxes as instanced GL lines. Cite `pxr/imaging/hdsi/prefixPathPruningSceneIndex.h` and `usdImagingGL/engine.cpp`. Mark anything that relies on undocumented behaviour.
  - verify: harness numbers on both fixtures: time to prune and restore a branch, batched reveal frame rate, orbit at 10k boxes, stage scene index population time. A check confirms no stage edit happens on a state change.
  - size: L

- [x] M327.P2.T2 — Prototype session-layer draw mode
  - files: spike branch: `Engine/Nodes/Scene/SceneStage.{h,cpp}`, `Engine/Nodes/Scene/SceneOps.cpp`, `Gui/SceneViewport.cpp`
  - approach: no session layer or LoadNone code exists yet: `SceneOps::openStage` (`SceneOps.cpp:469-480`) passes a null session layer and uses `LoadAll`, so the prototype adds both (`SceneStage.cpp:34-50`, `SceneOps.cpp:479`); M328 - Deferred Viewer Stage owns the real implementation. With payloads unloaded, Author `GeomModelAPI`, `applyDrawMode`, `drawMode=bounds` and `extentsHint` on rows. Measure the same actions as the filter, including a state change on a row with many siblings.
  - verify: harness numbers from the same scenarios as M327.P2.T1, in the same table.
  - size: M

## Phase 327.3: Engine costs

- [x] M327.P3.T1 — Measure off-lock payload loading
  - files: spike branch: `Engine/Nodes/Scene/SceneStage.cpp`, `Engine/Nodes/Scene/SceneOps.cpp`, `Gui/SceneViewport.cpp`
  - approach: building on the prototype's LoadNone stage from M327.P2.T2, on a worker, open the payload's layers with `SdfLayer::FindOrOpen` and hold references, then take the stage lock for `LoadAndUnload`. Compare lock hold time against loading without pre-opening. Find whether Storm can keep drawing while a load holds the stage, given `SceneViewport`'s paint try-lock, and what orbit does during a load.
  - verify: numbers for one building and for one block, and a stated answer on whether navigation continues during a load, with file and line evidence.
  - size: L

- [x] M327.P3.T2 — Measure deferred pattern re-matching
  - files: spike branch: `Engine/Nodes/Scene/SceneQueryPool.cpp`, `Engine/Nodes/Scene/USDPrune.cpp`, `Engine/Nodes/Scene/SceneStage.cpp`
  - approach: a chain of ten pattern nodes below the city. Load one block, re-match on a query stage with the same load set, swap the changed layers. Measure re-match and sublayer swap separately, count `ObjectsChanged` resynced paths, and check whether a swap hits the whole-stage resync recorded in M19 - USD/Hydra Foundation (15–16 s on 100k cubes).
  - verify: numbers for one matching and one non-matching node, and a recommendation for M331 - Deferred Pattern Matching if the swap cost breaks the 100 ms budget.
  - size: M

## Phase 327.4: Decision

- [ ] M327.P4.T1 — Write up results and get the mechanism decision
  - files: `PLAN/DESIGN/<date>-heavy-scene-spike.md` (new, plan branch), this milestone's `## Decisions`
  - approach: one table comparing the mechanisms on every budget, plus the engine-cost answers, a recommendation, any budget revision proposed (one is allowed), and what M329 - Viewer Draw States should change. Ask the user under `# Open questions`.
  - verify: the user's decision is recorded here and in the design note.
  - size: M

**Verification gate:** fixtures and harness merged; both mechanisms and both engine costs have numbers from a stripped release build on a GPU host; the user has chosen a mechanism; prototypes are not merged.

## Decisions

- 2026-11-07 — Settle run, session on the loaded 1M city (AMD Radeon AI PRO R9700): peak RSS 12.27 GB with 46 GB free, so memory didn't cause the earlier hang. Open 14.7 s; prune 1896 ms, restore 39 ms; sibling set 10-16 ms; reveal first frame 1344 ms; cancel 1402 ms. `orbit10k` hung Natron, and the user killed it after 145 s; the machine stayed up this time. `sibling_shot` changed 0 pixels for both rows, with no filter control to judge it. Lobby was skipped because its path differs on that host.

- 2026-11-07 — Host script rewritten on spike/m327-hydra-filter (user): no system PySide, fixtures in the dev image, Lobby and Island added (Lobby 2,786 prims loaded; Island 30.9M instances, skipped below 24 GB free, an estimate), per-scenario limits record `timed_out`, `settle` mode with a pixel check. `first_boxes_ms` renamed `first_frame_ms`. Tested under llvmpipe only; Island in the GUI is untested.

- 2026-11-07 — GPU host run received (87 of 92 records; synthetic fixtures). `flat-session-all` and `flat-session-restorevalue` were killed by the user in `noedit`, after about 53 and 10 min (inferred from timestamps). **Recommendation, awaiting the user's choice:** draw states reach Hydra through a per-engine scene index filter with a GL box overlay, not session-layer draw mode. The filter finished reveal at 2 fps and cancelled in 120-1207 ms. Open: session on a loaded city is unmeasured, and the filter's 1.2-1.7 s state change at 1M loaded fails the 100 ms budget. A run of about 5 minutes closes this.

- 2026-11-05 — M327.P1.T1 verified in natron-dev: city 3,901 prims with LoadNone, 997,501 with LoadAll; flat file 1,000,001 either way; 344 s to generate, 43 MB under build/bench.
- 2026-11-06 — P2.T1 (fd58be8e9) and P2.T2 (1abadeb0e) written on `spike/m327-hydra-filter`, worktree `.worktrees/m327-filter`, both selected at run time by `NATRON_DRAW_STATE_MECHANISM`, so one build gives one comparison table. `usdImagingGL/engine.cpp` isn't on the host or in the container, so engine claims come from library strings and symbols and are marked `UNDOCUMENTED` in the code: that `%p` in `UsdImagingGLEngine_%s_%p` is the engine, that the render index builds synchronously in the constructor, and that the earliest insertion sits ahead of Storm's filters.
- 2026-11-06 — M327.P1.T2 verified on a 100k-prim flat file against a main-based AppImage under Xvfb: one JSON line per scenario. Today's code on 100k prims, llvmpipe: open stalls the main thread 12.4 s, an edit 10.8 s (17 s total), orbit 2 fps (not valid), no unrequested payloads. The 1M flat file ran natron-dev out of its 12 GB limit on this 15 GB host, so 1M runs need the GPU host.
- 2026-11-07 — Prototypes built and run at 20k prims on llvmpipe (relative only; results `build/m327-run/m327-p2.jsonl` in the spike worktree, numbers in ms). Flat: filter prune 127, restore 1611, sibling edit 230-330 with 0 resyncs, no stage edits; session prune 1905, restore 476, sibling edit 53-65 with 4 resyncs, 7 stage edits, a 17.8 s stall on `noedit`. City: filter prune 237, restore 589; session 78 and 63 but over only 13 prims. Payload load hold 5-8 with no paints during it, so the preopen comparison says little here. Re-match no-match 0.08; the offset control shows 1 root resync, so the counter works. On code reading, a load freezes the picture with input live; hover, picks and panel walks block for the whole hold. No mechanism choice from this: P3's boxes tick and the 1M run need a GPU host, via `tools/bench/heavy_scene_gpu_host.sh` (bc748f682).
