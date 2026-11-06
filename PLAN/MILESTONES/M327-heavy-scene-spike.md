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

- [ ] M327.P2.T1 — Prototype the Hydra filter and box overlay
  - files: spike branch: `Gui/SceneViewport.{h,cpp}`, a new scene index filter under `Gui/`, the viewport's guide drawing
  - approach: register a filter per engine, keyed on its `renderInstanceId`, with a mutable set of pruned prefixes. Prune and restore a subtree at runtime, add prims in per-frame batches, and draw boxes as instanced GL lines. Cite `pxr/imaging/hdsi/prefixPathPruningSceneIndex.h` and `usdImagingGL/engine.cpp`. Mark anything that relies on undocumented behaviour.
  - verify: harness numbers on both fixtures: time to prune and restore a branch, batched reveal frame rate, orbit at 10k boxes, stage scene index population time. A check confirms no stage edit happens on a state change.
  - size: L

- [ ] M327.P2.T2 — Prototype session-layer draw mode
  - files: spike branch: `Engine/Nodes/Scene/SceneStage.{h,cpp}`, `Engine/Nodes/Scene/SceneOps.cpp`, `Gui/SceneViewport.cpp`
  - approach: no session layer or LoadNone code exists yet: `SceneOps::openStage` (`SceneOps.cpp:469-480`) passes a null session layer and uses `LoadAll`, so the prototype adds both (`SceneStage.cpp:34-50`, `SceneOps.cpp:479`); M328 - Deferred Viewer Stage owns the real implementation. With payloads unloaded, Author `GeomModelAPI`, `applyDrawMode`, `drawMode=bounds` and `extentsHint` on rows. Measure the same actions as the filter, including a state change on a row with many siblings.
  - verify: harness numbers from the same scenarios as M327.P2.T1, in the same table.
  - size: M

## Phase 327.3: Engine costs

- [ ] M327.P3.T1 — Measure off-lock payload loading
  - files: spike branch: `Engine/Nodes/Scene/SceneStage.cpp`, `Engine/Nodes/Scene/SceneOps.cpp`, `Gui/SceneViewport.cpp`
  - approach: building on the prototype's LoadNone stage from M327.P2.T2, on a worker, open the payload's layers with `SdfLayer::FindOrOpen` and hold references, then take the stage lock for `LoadAndUnload`. Compare lock hold time against loading without pre-opening. Find whether Storm can keep drawing while a load holds the stage, given `SceneViewport`'s paint try-lock, and what orbit does during a load.
  - verify: numbers for one building and for one block, and a stated answer on whether navigation continues during a load, with file and line evidence.
  - size: L

- [ ] M327.P3.T2 — Measure deferred pattern re-matching
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

- 2026-11-05 — M327.P1.T1 verified in natron-dev: city 3,901 prims with LoadNone, 997,501 with LoadAll; flat file 1,000,001 either way; 344 s to generate, 43 MB under build/bench.
- 2026-11-06 — P2.T1 (fd58be8e9) and P2.T2 (1abadeb0e) written on `spike/m327-hydra-filter`, worktree `.worktrees/m327-filter`, both selected at run time by `NATRON_DRAW_STATE_MECHANISM`, so one build gives one comparison table. `usdImagingGL/engine.cpp` isn't on the host or in the container, so engine claims come from library strings and symbols and are marked `UNDOCUMENTED` in the code: that `%p` in `UsdImagingGLEngine_%s_%p` is the engine, that the render index builds synchronously in the constructor, and that the earliest insertion sits ahead of Storm's filters.
