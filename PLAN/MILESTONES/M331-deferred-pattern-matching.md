# M331 - Deferred Pattern Matching

A pattern matches what its evaluator has loaded: a viewer matches its load set, while USDRender
and USDWrite match everything. A wildcard reads no files until a load or a render, and renders
stay correct. The user's idea; see `PLAN/DESIGN/2026-11-04-scene-graph-design.md`. Katana and
Gaffer get this free from per-location evaluation; on our layer-per-node design it's an
evaluation input.

Notes from M327 - Heavy Scene Spike (2026-11-07), see `PLAN/DESIGN/2026-11-07-heavy-scene-spike.md`:
- Measure swap at 1M: the spike's 1.6-2.8 ms came from a stage of a few thousand prims.
- Explain `rematch_calls` of 10 when only the last node changed, against the harness doc.

## Scope

**In:**
- The load set as an evaluation input, passed by viewers and panels; absent means everything loaded.
- Query stages matching that load set; `SceneLayerMemo` slots per evaluation context.
- Pattern nodes re-matching only when a load can reach them, applied together with the load.
- The unsearched note in place of the zero-match warning.
- An engine-side Search Unloaded query. Its UI is in M334 - Knob Prim Tools.

**Out:**
- Knob and picker UI — M334 - Knob Prim Tools.
- The pattern language itself — M52 - USD Layer-Stack Completion's `PrimSelector`, whose evaluator this calls.
- Bounds updated for edits inside an unloaded payload: a known limit.

**Edge cases:**
- Literal paths never need a query, so they apply to unloaded content straight away.
- A pattern's literal prefix is the prefix test for skipping: `/city/block5//x` isn't re-matched when block3 loads. A pattern with no literal prefix always re-matches.
- A node whose matches didn't change keeps its layer, through `eUpstreamReadFacts`, and nothing swaps.
- A load and its new layers apply in one locked step, so a pruned prim never flashes.
- Two viewers with different load sets hold separate evaluations of the same node; slots are evicted least recently used and re-author on demand.
- An invalid pattern is still a persistent node error, immediately.

**Assumptions:**
- A sublayer swap reports only the paths whose opinions changed, and stays under budget at 1M prims. M327 - Heavy Scene Spike measures it and may change Phase 331.2's approach.

## Phase 331.1: Load set as an evaluation input

- [x] M331.P1.T1 — Pass the load set through scene evaluation
  - files: `Engine/ViewerInstance.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Engine/Nodes/Scene/SceneQueryPool.{h,cpp}`, `Tests/Scene/SceneQueryPool_Test.cpp`
  - approach: a small value type (a sorted path list and its hash) that viewers pass when evaluating. `SceneQueryPool::borrow(stack, loadSet)` returns a stage with those payloads loaded, preferring the closest load set, and applying the difference with `LoadAndUnload`. USDRender and USDWrite pass nothing and get `LoadAll` as today.
  - verify: gtest — borrowing with a set loads only that set's layers; borrowing without one loads all; returning and re-borrowing the same set does no load.
  - size: L

- [x] M331.P1.T2 — One memo slot per evaluation context
  - files: `Engine/Nodes/Scene/SceneLayerMemo.h`, `Engine/Nodes/Scene/SceneOps.cpp`, `Tests/Scene/SceneLayerMemo_Test.cpp`
  - approach: `SceneLayerMemo` holds a few least-recently-used slots keyed by context, with the existing `Key` inside each slot, so viewer and render evaluations stop evicting each other. A layer already handed out is still never edited.
  - verify: gtest — alternating viewer and render evaluations of a pattern node authors each layer once; a knob edit invalidates both.
  - size: M

## Phase 331.2: Pattern nodes

- [x] M331.P2.T1 — Match per context and skip by prefix
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.cpp`, `Engine/Nodes/Scene/USDPrune.cpp`, `Engine/Nodes/Scene/USDEditTransform.cpp`, `Engine/Nodes/Scene/USDAssignMaterial.cpp`, `Tests/Scene/ScenePatternContext_Test.cpp`
  - approach: pattern nodes borrow with the context's load set. Patterns made only of literal paths skip the query. A load re-evaluates only nodes whose literal prefix reaches a loaded path; add the prefix helper beside `PrimSelector`.
  - verify: gtest on the city — `/city/block3//car*` matches nothing unloaded, the right cars once block3 is in the load set, and all cars with no load set; loading block5 doesn't re-query that node; a literal path matches while unloaded.
  - size: L

- [x] M331.P2.T2 — Apply a load and its layers together
  - files: `Engine/Nodes/Scene/SceneStage.{h,cpp}`, `Gui/SceneViewport.cpp`, `Tests/Scene/SceneStage_Test.cpp`
  - approach: on a load request, evaluate downstream pattern nodes against the future load set on a query stage first, then in one locked step swap the changed sublayers and call `LoadAndUnload`, so Hydra's next sync sees both. Follow M327 - Heavy Scene Spike's recommendation on swap cost.
  - verify: gtest — a USDPrune targeting a prim inside a payload never exposes that prim to a stage listener during the load; harness — the main thread stays within budget with ten pattern nodes downstream.
  - size: L

## Phase 331.3: Honest results

- [x] M331.P3.T1 — Replace the zero-match warning with the unsearched note
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`, `Engine/Nodes/Scene/USDPrune.cpp`, `Tests/Scene/ScenePatternContext_Test.cpp`
  - approach: when a viewer-context match leaves unloaded payload prims the pattern could reach, the node's message reads "0 matches, N unloaded branches not searched". The plain warning stays when everything reachable is loaded, and in render context.
  - verify: gtest — the three message cases; an invalid pattern stays a persistent error in every context.
  - size: M

- [x] M331.P3.T2 — Search Unloaded query
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Tests/Scene/ScenePatternContext_Test.cpp`
  - approach: an asynchronous, cancellable API returning a pattern's matches on a private `LoadAll` query stage over the node's input, for previews only. Results never feed evaluation and no viewer stage changes.
  - verify: gtest — it finds matches inside unloaded payloads, the viewer's load set is unchanged, and cancelling stops it mid-way.
  - size: M

**Verification gate:** release gate green; on the city a pattern node's viewer result and USDRender result agree once everything is loaded; a wildcard node opens no payload file in the viewer until a load; harness budgets pass with ten pattern nodes downstream on the GPU host.
- 2026-11-10 — P2.T2 and P3.T2 completed with tests (ebfadf3bd): a pruned prim inside a payload never shows during its load; Search Unloaded leaves the viewer's stage and the node's result alone. Release gate green (run 42789360725). Still open: the city agreement check and the ten-pattern-node harness budgets on the GPU host.
