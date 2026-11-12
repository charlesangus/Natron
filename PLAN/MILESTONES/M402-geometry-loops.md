# M402 - Geometry Loops

Make loop blocks work on native geometry, which they don't today: `LoopBegin`/`LoopEnd` take image inputs only (`Engine/Nodes/Loop/LoopBegin.h:48`, `LoopEnd.h:95`), and neither `evaluateGeometry()` (`Engine/EffectInstanceRenderGeometry.cpp`) nor `GeoDetailKey` carries the M178 - Render Context Core context key, so a geometry node can't vary per iteration. This milestone adds a Houdini-style SOP For-Each block, `GeoForEachBegin`/`GeoForEachEnd`, on the same loop driver and render-context core as the image loop (`PLAN/DECISIONS/2026-10-25-loops-and-i-nodes-architecture.md`). Nodes follow the For-Each SOP block (`PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`).

Member of F18 - Geometry Loops And Solver: branches off and merges into `feature/f18-geometry-loops-and-solver` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers.

## Scope

**In:**
- Render contexts on the geometry pull path: `evaluateGeometry()` and `GeoDetailKey` mix in the render-context key, and only nodes whose upstream read-set names a loop variable re-cook per iteration.
- `GeoForEachBegin` (outputs: Piece, Feedback, Metadata) and `GeoForEachEnd` (inputs: body result, optional Piece source), both with `eDataKindGeometry` edges.
- Iteration modes: by piece (a primitive or point attribute such as `name`/`path`/`class`, or connectivity), by point, by count, and feedback. Gather by merge or feedback. Stop condition. Single-pass (one chosen iteration) for debugging.
- Iteration values two ways: the image loop's `loop.*` variables through the existing knob drivers, and a Metadata output detail with `iteration`, `numiterations`, `value` (and `ivalue`/`svalue` for the piece key) that a wrangle reads through its second input.
- Merge-mode iterations run in parallel on the thread pool; feedback runs in order.

**Out:**
- Scene (USD stage) edges through any loop (user, 2026-11-12: geometry only); they stay refused.
- The cross-frame solver (M404 - Geometry Solver).
- Compiled-block special modes beyond the parallel merge path (none).

**Edge cases:**
- Zero pieces (empty input or attribute missing): `GeoForEachEnd` outputs an empty detail with an info message, not an error.
- Piece attribute missing on the input: node error naming the attribute.
- 10,000+ pieces: contexts are lightweight and nodes outside the read-set cook once; the gate benches it.
- A body that doesn't read the piece or any loop variable: every iteration hashes the same and merges N identical copies, as in Houdini; no special case.
- Nested For-Each blocks: inner contexts extend the outer key, as image loops already do.
- Feedback with a stop condition that never fires: capped by the iteration count knob.
- Cancelling mid-loop: every iteration checks abort, so the viewer stays responsive.

**Assumptions:**
- Unconfirmed: piece order is the sorted order of the piece attribute's unique values (numeric ascending, strings lexical), with connectivity pieces in lowest-point-number order, matching Houdini.
- Unconfirmed: `GeoForEachEnd` merges in iteration order, so the result does not depend on thread count.

## Phase 402.1: Contexts on the geometry path

- [ ] M402.P1.T1 — Mix the render-context key into geometry evaluation and caching
  - files: `Engine/EffectInstanceRenderGeometry.cpp`, `Engine/GeoDetailKey.{h,cpp}`, `Engine/EffectInstance.h`, `Tests/GeoRender_Test.cpp`
  - approach: Thread the current render context through `EvaluateGeometryArgs` the way `renderRoI` carries it for images (`mixRenderContextKey`, `Engine/EffectInstance.cpp:685`/`:7820`, `Engine/ParallelRenderArgs.cpp:922`). Add the context key to `GeoDetailKey` only when the node's upstream read-set names a context variable, so nodes that don't read it keep one cache entry. Knob reads inside `renderGeometry()` must see the context's variable values through the existing driver path.
  - verify: new `GeoRender_Test` cases: a `GeoTransform` whose translate is driven by a test context variable cooks different details under two contexts and hits the cache on a repeat; a node that doesn't read the variable produces one cache entry for both.
  - size: L

## Phase 402.2: The For-Each block

- [ ] M402.P2.T1 — `GeoForEachBegin` and `GeoForEachEnd` nodes with count and feedback modes
  - files: `Engine/Nodes/Loop/GeoForEachBegin.{h,cpp,reg.inc}`, `Engine/Nodes/Loop/GeoForEachEnd.{h,cpp,reg.inc}`, `Engine/CMakeLists.txt`, `Tests/Native/GeoForEach_Test.cpp`
  - approach: Mirror `LoopBegin`/`LoopEnd`'s pairing, driver and `getInputContexts` (`LoopEnd.cpp:3446`) with geometry inputs and outputs. Count mode iterates N times over the full input; feedback passes each iteration's result to the next through Begin's Feedback output. Gather by merge (GeoMerge's attribute promotion rules) or feedback (last iteration). Merge-mode iterations evaluate in parallel on the thread pool and merge in iteration order. Stop condition and single-pass knobs as on `LoopEnd`. Labels match script names.
  - verify: `GeoForEach_Test`: count mode of 5 with a context-driven `GeoTransform` merges 5 offset copies; feedback mode of 5 applies the transform 5 times cumulatively; the stop condition ends early; results are identical with 1 and 8 threads.
  - size: L
- [ ] M402.P2.T2 — Piece and point iteration modes
  - files: `Engine/Nodes/Loop/GeoForEachBegin.{h,cpp}`, `Engine/Geo/GeoConnectivity.{h,cpp}`, `Tests/Native/GeoForEach_Test.cpp`
  - approach: By-piece mode splits the input by a primitive or point attribute's unique values, or by connected components (union-find over `GeoReverseTopology`, in a reusable `GeoConnectivity` helper M415 - Geometry Attribute Ops' Connectivity node will reuse). The Piece output is that subset, extracted by building a detail with only those elements (pages shared where possible). By-point mode yields one point per iteration. `GeoForEachEnd`'s optional Piece-source input lets the user extract pieces from a different stream, as the SOP's "Fetch Piece" source does.
  - verify: `GeoForEach_Test`: three boxes merged, iterated by `name` and by connectivity, each piece scaled about its own centroid, give the expected bounds per box; a missing attribute errors with its name; zero pieces gives an empty detail and an info message.
  - size: L
- [ ] M402.P2.T3 — Metadata output and loop variables
  - files: `Engine/Nodes/Loop/GeoForEachBegin.{h,cpp}`, `Tests/Native/GeoForEach_Test.cpp`, `Engine/Nodes/Loop/docs/GeoForEach.md`
  - approach: Begin's Metadata output is a detail-only `GeoDetail` with `iteration`, `numiterations`, `value`, `ivalue` and `svalue` detail attributes for the current context. Publish the same values as the `loop.*` variables the image loop's drivers use, so knobs read them directly. Write the node docs fresh (no SideFX text).
  - verify: `GeoForEach_Test`: a `GeoWrangle` reading `detail(1, "iteration")` from Metadata on its second input, and a `GeoTransform` knob driven by `loop.iteration`, both produce the same per-piece offsets.
  - size: M

## Phase 402.3: Proof

- [ ] M402.P3.T1 — Scale bench and GUI check
  - files: `tools/bench/geo/foreach_pieces.py`, `Tests/gui/m402_geo_foreach.py`
  - approach: Bench a For-Each over 10,000 connectivity pieces (a scattered box field) with a per-piece transform, reporting cook time at 1 and all threads; no threshold. GUI script builds `GeoPrimitive → GeoForEachBegin → GeoTransform → GeoForEachEnd → GeoToUSD` and checks the viewer shows every piece, then cancels a long loop and checks the UI stays responsive.
  - verify: the bench runs headless through a gtest-style harness that cooks `GeoForEachEnd` directly (geometry has no headless render target until M403 - Geometry File Cache); the GUI script passes under Xvfb.
  - size: M

**Verification gate:** full ctest green including `GeoRender_Test` and `GeoForEach_Test`; `Tests/gui/m402_geo_foreach.py` passes under Xvfb; the 10k-piece bench numbers recorded in `## Decisions`; existing `NativeLoop*` tests unchanged; the GUI bench rule (Context) if `Engine/Knob*` was touched.

## Decisions

- 2026-11-12 — **Separate `GeoForEachBegin`/`GeoForEachEnd` nodes (user, /cat-plan grill):** edges stay statically typed; reusing `LoopBegin`/`LoopEnd` would need inputs whose kind follows the connection, which the graph has no precedent for.
- 2026-11-12 — **Loops carry geometry only (user):** scene edges stay refused, as in M180 - Loop Block Core.
- 2026-11-12 — **Full For-Each SOP mode set (user):** by piece (attribute or connectivity), by point, by count, feedback; gather merge or feedback; metadata; stop condition.
- 2026-11-12 — **Iteration values both ways (user):** `loop.*` variables through the existing knob drivers and a Metadata output detail for wrangles.
- 2026-11-12 — **Merge-mode iterations run in parallel (user):** feedback stays serial; a 10k-piece bench joins the gate.
- 2026-11-12 — **Ships in F18 - Geometry Loops And Solver with M403 - Geometry File Cache and M404 - Geometry Solver (user):** they share the iteration and cache core.
- 2026-11-12 — Code finding: loops are image-only today (`LoopBegin.h:48`, `LoopEnd.h:95`), and `evaluateScene`/`evaluateGeometry` don't carry the render-context key.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
