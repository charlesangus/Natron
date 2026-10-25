# M179 - Render Graph Continuation

Neither scheduler can add work once a frame starts rendering: the request pass runs on actions only (`Engine/ParallelRenderArgs.cpp:654-733`), and a `FrameGraph`'s context "must stay unchanged until the future finished" (`Engine/RenderScheduler.h:340-360`). The only dynamic path is the serial render-time pull in `renderInputOrTakeFromStore` (`Engine/EffectInstance.cpp:908-934`), which runs on the requesting task's thread and is counted as unplanned. M183 - Loop Map Blend needs data-dependent requests from the start (user, 2026-10-24): render the Map tile, then request only the iterations that tile's Map range spans. This milestone adds a **continuation** API to the task-graph scheduler: a finished probe task may extend its frame's request map and graph with new (node, context, region) requests, which are scheduled in parallel under the same budget, and a dependent "continuation task" runs once they are done.

## Scope

**In:**
- A probe → extend → continue API on the task-graph scheduler, keyed by context (M178 - Render Context Core).
- Request-pass reuse: extension requests run the same RoI/RoD/identity actions as the planned pass and merge into the frame's maps.
- Budget, cancellation (abort) and progress accounting for extended tasks.
- On the legacy scheduler and the ineligible-frame fallbacks (`Engine/OutputSchedulerThread.cpp:2240-2263`), the same API degrades to serial render-time pulls.

**Out:**
- Any particular caller: M183 - Loop Map Blend is the first. The API is public to native nodes but not exposed to OpenFX or Python.
- Changing how ordinary nodes are planned.

**Edge cases:**
- An extension naming work already planned or rendered in the frame reuses it (no duplicate task).
- An extension requested after the frame was aborted is dropped and the continuation task does not run.
- Recursive extension (a continuation task that itself probes) is allowed up to a depth of 8, then fails the frame with a message.
- Extended work counts against the frame's bytes budget (`RenderScheduler.h:413`); a continuation never waits on memory held by its own pending inputs.

## Phase 179.1: API

- [ ] M179.P1.T1 — Design and document the continuation API
  - files: `Engine/RenderScheduler.h`, `Engine/EffectInstance.h`, `Engine/Nodes/README.md`
  - approach: A node opts in with `EffectInstance::getRenderContinuation(args)`; its planned task (the probe) returns a list of `(node, ctxKey, RoI)` requests and a continuation callback. Document the lifetime, threading and abort rules in the header and README. No implementation in this task beyond declarations and a stub that rejects use.
  - verify: reviewed at the gate; the tree builds.
  - size: M

- [ ] M179.P1.T2 — Extend a frame's request map from a finished task
  - files: `Engine/ParallelRenderArgs.h`, `Engine/ParallelRenderArgs.cpp`, `Engine/FrameRenderContext.cpp`
  - approach: Factor the request-pass visit so it can run incrementally against an existing frame's `FrameRequestMap` and args map: new entries are added under the frame's lock, existing entries' RoIs are unioned, and only newly needed regions produce new requests.
  - verify: gtest runs a planned pass, then an extension over a node already in the map with a disjoint RoI: the map holds the union and only the new region is reported as new work.
  - size: L

- [ ] M179.P1.T3 — Schedule extension tasks and the continuation task
  - files: `Engine/RenderScheduler.h`, `Engine/RenderScheduler.cpp`
  - approach: When a probe task finishes, build tasks for the new requests into the live `FrameGraph` (new nodes and edges only), plus a continuation task depending on them and on the probe's output; enqueue under the frame's budget and priority. Abort drops pending extension tasks.
  - verify: gtest with a `ProbeTestEffect` that requests a data-dependent subset of 16 contexts of its input: only that subset renders (counter), in parallel on N threads, and the result matches a full render.
  - size: L

- [ ] M179.P1.T4 — Degrade to serial pulls on the legacy path
  - files: `Engine/EffectInstance.cpp` (`renderInputOrTakeFromStore`), `Engine/RenderScheduler.cpp`
  - approach: Where the task graph is not in use, run the probe, then pull each requested `(node, ctxKey, RoI)` serially through the existing render-time pull, then run the continuation inline.
  - verify: `renderBothWays` on the `ProbeTestEffect` graph is bit-identical across both schedulers.
  - size: M

## Phase 179.2: Robustness

- [ ] M179.P2.T1 — Abort, recursion and budget tests
  - files: `Tests/RenderContinuation_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Cover abort during extension (no continuation runs, no leak), recursion to depth 8 and the depth-9 failure, an extension that only reuses planned work, and a bytes-budget-limited frame that still completes.
  - verify: the new suite passes, under the debug build's asserts (`tools/ci/remote/build.sh debug --tests`).
  - size: M

## Decisions

- 2026-10-25 — **Continuation from the start (user):** Map-blend culling uses graph continuation, not serial render-time pulls, so heavy loop bodies stay parallel.
- 2026-10-25 — **Public to native nodes (user):** documented in `Engine/Nodes/README.md` so later adaptive or tile-skipping nodes can use it; not exposed to OpenFX or Python.

**Verification gate:** the full build and ctest pass, including `RenderContinuation` and the existing scheduler suites unchanged; `tools/ci/remote/build.sh debug --tests` passes; `tools/bench` shows no regression on ordinary graphs.
