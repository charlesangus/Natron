# M328 - Deferred Viewer Stage

The viewer's stage opens with payloads unloaded, on a worker, and the heavy walks leave the
main thread. Today every payload loads on the first sync and bounds are measured afterwards.
Design: `PLAN/DESIGN/2026-11-04-scene-graph-design.md`. Line references below are at
`b4f4fe783` and will have moved.

Notes from M327 - Heavy Scene Spike (2026-11-07), see `PLAN/DESIGN/2026-11-07-heavy-scene-spike.md`:
- The first render took 1.3-1.9 s even with the root pruned and 11 s loaded, so it can't stay on the main thread if the flat stall gate is to pass; move it or say why not.
- Test load continuity with larger loads: the spike's windows allowed only 2-3 mouse moves.

## Scope

**In:**
- Viewer stage opened `LoadNone` on a worker, with a viewer status while composing.
- Payload loads through one worker path: open layers off-lock, then a short locked `LoadAndUnload`.
- File closure, framing, counting, cameras and guides moved off the main thread.
- Tree lists unloaded prims and badges payload rows. Twirling a payload row loads its own payload only. An Unload row action.

**Out:**
- Draw states and boxes — M329 - Viewer Draw States. Until then the viewer draws all composed prims, and unloaded payloads draw nothing.
- Query stages, picker stage and pattern matching — M331 - Deferred Pattern Matching.
- Stages for non-viewed nodes — M332 - Scene Graph Points.
- USDRender and USDWrite stages, which stay fully loaded by design.

**Edge cases:**
- A load requested while one is running is coalesced into the next batch.
- A load that fails to open a file shows a row badge and the USD error; the rest of the stage is unaffected.
- Twirling an already-loaded payload does no work; closing a row never unloads.
- A viewer switching node mid-load cancels what hasn't started and keeps what has.
- Resyncs insert and remove rows incrementally. A load must not rebuild the tree, which is usdview's weakness.

## Phase 328.1: Stage opens unloaded

- [x] M328.P1.T1 — Open the viewer stage unloaded on a worker
  - files: `Gui/SceneViewport.{h,cpp}`, `Engine/Nodes/Scene/SceneStage.{h,cpp}`, `Engine/Nodes/Scene/SceneOps.cpp`, `Tests/Scene/SceneStage_Test.cpp`
  - approach: `SceneStage` gains a load-policy argument; the viewer passes `LoadNone`, while query and render callers keep `LoadAll` (`SceneOps.cpp:411`). Compose on the existing viewport worker (`SceneViewport.cpp:1202-1223`), and show a composing status in the viewer until the first sync lands.
  - verify: gtest — a stage over the city fixture's root opened this way has no building layer in `SdfLayer::GetLoadedLayers()`; harness — no main-thread gap over 100 ms during open of the city.
  - size: M

- [x] M328.P1.T2 — One load path, opened off-lock
  - files: `Engine/Nodes/Scene/SceneStage.{h,cpp}`, `Gui/SceneViewport.{h,cpp}`, `Tests/Scene/SceneStage_Test.cpp`
  - approach: `SceneStage::requestLoad(paths, withDescendants)` and `requestUnload(paths)`. Using M327 - Heavy Scene Spike's findings, a worker opens the payload layers and holds references without the lock, then a short locked `LoadAndUnload` batches every pending request. Never `SetLoadRules`, which recomposes from root.
  - verify: gtest — two requests batch into one `LoadAndUnload`, `UsdNotice::ObjectsChanged` names only those subtrees; harness — lock hold time for one building is reported and the main thread stays within budget.
  - size: L

## Phase 328.2: Heavy walks off main

- [ ] M328.P2.T1 — Move the file closure walk off the main thread
  - files: `Engine/Nodes/Scene/USDRead.cpp`, `Engine/Nodes/Scene/SceneFileSource.cpp`, `Engine/Nodes/Scene/SceneOps.cpp`, `Tests/Scene/USDRead_Test.cpp`
  - approach: `knobChanged` no longer calls `SdfLayer::FindOrOpen` or walks sublayers and specs (`USDRead.cpp:314-316`, `SceneFileSource.cpp:191`, `SceneOps.cpp:126,242-244,463-469`). That work happens on the evaluation path, and the closure walk must not open payload arcs.
  - verify: gtest — setting the file knob returns without opening the file, and evaluation doesn't open any building layer; harness — no gap over 100 ms when the file knob is set.
  - size: M

- [ ] M328.P2.T2 — Move framing, counting, cameras and guides off the main thread
  - files: `Gui/SceneViewport.{h,cpp}`, `Gui/SceneGuides.{h,cpp}`, `Tests/gui/scene_viewer_framing.py`
  - approach: the traversals at `SceneViewport.cpp:1409,1712,1752,1866` and `SceneGuides.cpp:251` run on the worker and post results. `onSynced` (`:3179-3185`) never traverses. Framing uses a `UsdGeomBBoxCache` that includes unloaded prims. Drop the prim count unless something still reads it.
  - verify: harness — no main-thread gap over 100 ms from first sync on the flat file; GUI test — framing a loaded scene still frames it, and cameras still list.
  - size: L

## Phase 328.3: Payload rows

- [ ] M328.P3.T1 — List unloaded prims and load on twirl
  - files: `Gui/SceneGraphModel.{h,cpp}`, `Gui/SceneGraphPanel.cpp`, `Tests/Scene/SceneGraphModel_Test.cpp`, `Tests/gui/scene_graph_payloads.py`
  - approach: children come through `GetFilteredChildren` with a predicate that omits `IsLoaded`, as usdview does (`usdviewq/appController.py:3492-3508`). Payload rows get a badge from `HasAuthoredPayloads`, `IsLoaded`. Expanding an unloaded payload row calls `requestLoad` without descendants and shows a loading row until the resync arrives. Rows change incrementally.
  - verify: gtest with `QAbstractItemModelTester` — an unloaded payload row reports children possible and nothing else resets on its load; GUI script on the city — twirling one building opens only that building's layer, and other rows keep their expansion.
  - size: M

- [ ] M328.P3.T2 — Explicit Unload action
  - files: `Gui/SceneGraphPanel.cpp`, `Tests/gui/scene_graph_payloads.py`
  - approach: row context menu Load, Load With Descendants, Unload on the selection, through `requestLoad` and `requestUnload`. Unload closes the row's descendants' rows.
  - verify: GUI script — Unload removes the building's prims from the stage and the row keeps its payload badge; undo is not offered because this isn't a scene edit.
  - size: M

**Verification gate:** release gate green; harness on the payload city and flat file passes main-thread stall and unrequested-file budgets on the GPU host; `scene_graph_payloads.py` passes; existing panel GUI tests pass.

## Decisions

- 2026-11-07 — Phase 328.1 at f26c43835, unbuilt. First render moved off the main thread (PM-accepted implementer's design, not in the plan): the engine lives on a per-viewport render thread with a shared GL context. A frame taking in a stage change renders there with main free; any other frame blocks main while it draws, then blits. Without threaded GL the engine stays on main.
