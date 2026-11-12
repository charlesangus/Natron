# M332 - Scene Graph Points

Explore the hierarchy at any node without blocking: each panel follows the viewer, the node
selection or a pin, independently. A panel on another node gets its own unloaded stage,
composed on a worker, and recent stages stay cached. Design:
`PLAN/DESIGN/2026-11-04-scene-graph-design.md`.

## Scope

**In:**
- Stages for non-viewed nodes opened unloaded on a worker, with a small least-recently-used cache.
- The old tree stays up greyed until the new one is ready; selection-follow is debounced.
- Twirl state per panel by path, carried across nodes. Scroll preserved.
- Follow and pin per panel, driven by signals in place of 250 ms polling.

**Out:**
- The draw column's editing — M329 - Viewer Draw States. It always edits the linked viewer's states.
- Pattern matching on these stages — M331 - Deferred Pattern Matching.
- Comparing two graph points in one view. Changed Here is in M333 - Prim Inspector; two panels cover the rest.

**Edge cases:**
- A panel following the viewer shares its stage, at no extra cost.
- Rapid clicks across nodes compose only the last, and in-progress work is abandoned.
- A cached stage goes stale when an upstream node changes; it syncs on reuse, as the viewer's does.
- A node with no scene shows "no scene" and keeps any pin. A deleted pinned node unpins, as in M231 - Scene Graph Panel.
- A path that doesn't exist at the new node keeps its twirl state silently for when it returns.
- Twirling a payload row on a non-viewed stage loads only on that panel's stage; the viewer is untouched.

## Phase 332.1: Stages for any graph point

- [x] M332.P1.T1 — Open non-viewed stages unloaded with a small cache
  - files: `Gui/SceneNodeStageSource.{h,cpp}`, `Gui/SceneGraphPanel.cpp`, `Tests/gui/scene_graph_points.py`
  - approach: panel sources for non-viewed nodes open `LoadNone` on a worker and sit in a small least-recently-used cache keyed by node, syncing when reused. Share the load path from M328 - Deferred Viewer Stage. Remove `loadNothing` special-casing that this supersedes (`SceneGraphPanel.cpp:258-260`).
  - verify: GUI script on the city — pinning two nodes in two panels shows each one's tree, returning to a cached node composes nothing, and neither panel opens a building layer until a twirl.
  - size: L

- [x] M332.P1.T2 — Non-blocking switch with a greyed tree
  - files: `Gui/SceneGraphPanel.cpp`, `Tests/gui/scene_graph_points.py`
  - approach: on a source change, grey the current tree and label the pending node, then swap models when the worker finishes. Debounce Follow Node Selection. A newer request cancels an older one. Replace the 250 ms polling (`SceneGraphPanel.cpp:102-104`) with node-selection and viewer signals.
  - verify: harness — no main-thread gap over 100 ms switching between nodes on the flat file; GUI script — clicking through four nodes quickly composes only the last and no timer polls while idle.
  - size: M

## Phase 332.2: State across nodes

- [x] M332.P2.T1 — Keep twirl state and scroll across nodes
  - files: `Gui/SceneGraphPanel.{h,cpp}`, `Gui/SceneGraphModel.{h,cpp}`, `Tests/gui/scene_graph_points.py`
  - approach: each panel holds its open paths and restores them lazily on the new model, without forcing loads: a payload row that isn't loaded on this stage stays closed until the user twirls it. Scroll restores to the top visible path, or its nearest ancestor. Twirl state stays unsaved.
  - verify: GUI script — open rows and scroll survive a viewer switch to a downstream node and back; a row whose path is missing reappears open on return; nothing loads as a result of restoring.
  - size: M

**Verification gate:** release gate green; `scene_graph_points.py` passes under Xvfb; harness budgets pass for node switching on the GPU host; accepted from the UAT AppImage.

## Decisions

- 2026-11-12 — Audit: all three tasks were already implemented (436afd897, 30fa6db80, 3e7fb51c1) but never checked off. The last release gates failed only on `USDRenderCyclesTest` aborting under the taskgraph scheduler (run 42757353797). F4 and M331 were merged in at d8391e71d, and the gate with `scene_graph_points.py` is re-running (run 43155051418).
