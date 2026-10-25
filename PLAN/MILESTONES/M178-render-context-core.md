# M178 - Render Context Core

The per-context render core shared by E13 - RFV and E15 - Loops And I-Nodes, split out of M71 - RFV Render Context (user, 2026-10-24). Today every render key is per node: the frame args map (`Engine/FrameRenderContext.h:105`), `FrameRequestMap` (`Engine/ParallelRenderArgs.h:271`), `FrameStore::TaskKey` (`FrameRenderContext.h:57-64`), `TaskEdge` (`ParallelRenderArgs.h:202-208`), the per-frame node hash (`ParallelRenderArgs.cpp:576`) and `ActionsCache` (`Engine/EffectInstancePrivate.cpp:460`). So one node cannot render under two parameter contexts in one frame. This milestone makes it possible. A context is an immutable, namespaced variable map; a node's context key is the hash of only the variables its upstream read-set names, so nodes that read nothing keep key 0 and render and cache exactly as today. A node may fan one input out into many contexts, and an edge may target a different context than its source (loop iterations, feedback, fold chains). It runs after M167 - Per-Frame Node Metadata, so metadata — cached in `ActionsCache` under (hash, time, view) and snapshotted per frame beside `nodeHash` — becomes per-context through the folded key (user, 2026-10-25). It needs no M70 - RFV Declaration And Storage and no Python: tests drive it with a test-only node that injects contexts. The Set node overlay and the Python `rfv` object stay in M71 - RFV Render Context.

## Scope

**In:**
- The context value type, with namespaces (`loop.*`, `loops.<name>.*`, plain RFV names).
- A per-knob read-set hook (expressions and native drivers report into it) and the per-node upstream read-set.
- Context fan-out on one input, and explicit target-context keys on request-pass edges and scheduler tasks.
- `(node, ctxKey)` keying of the args map, request map, scheduler tasks, render hash and `_exprRes`.
- `ActionsCache` sized for the contexts in flight.
- Both schedulers. Legacy pulls contexts serially.
- Per-context node metadata (format, frame range, layers, components), on top of M167 - Per-Frame Node Metadata's per-(hash, time, view) cache.

**Out:**
- The Python `rfv` object, the expression read-set scan and the Set node overlay: M71 - RFV Render Context.
- Graph continuation after rendering starts: M179 - Render Graph Continuation.
- Main-thread UI metadata (channel selectors, warnings) under a context: it keeps showing the base context.

**Edge cases:**
- Context key 0 must leave every existing hash and cache key bit-identical, so `NATRON_CACHE_VERSION` stays valid.
- A node reached through two contexts that restrict to the same key renders once.
- A cross-context edge that would point a task at itself (same node, same key) is a request-pass error, not a hang.
- Nested fan-out (a fan-out node inside another's fan-out) multiplies contexts. There is no hard limit: past 4096 contexts in one frame the request pass logs a warning naming the fan-out nodes, and the outermost one shows a warning badge (user). Memory is bounded elsewhere (M180 - Loop Block Core's fold chain).

## Phase 178.1: Context and read-sets

- [x] M178.P1.T1 — Write the render context value type
  - files: `Engine/RenderContext.h`, `Engine/RenderContext.cpp` (new), `Engine/CMakeLists.txt`, `Tests/RenderContext_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: An immutable map from a namespaced name (`ns`, `name`) to a variant (bool, int64, double, string, vectors of double/int/string). `overlay(other)` returns a new context; `restrictTo(readSet)` keeps only named entries (a reads-all sentinel keeps everything); `hash()` is a stable, order-independent 64-bit hash, bit-exact for doubles, and the empty context hashes to 0. Shared-pointer copies. This replaces M71.P1.T1.
  - verify: gtest covers overlay precedence, restriction, namespace separation (`loop.t` vs `t`), hash stability, equal hashes for equal contexts built in different orders, and empty → 0.
  - size: M

- [x] M178.P1.T2 — Per-knob and per-node upstream read-sets
  - files: `Engine/Knob.h`, `Engine/Knob.cpp`, `Engine/Node.h`, `Engine/Node.cpp`, `Engine/NodeInputs.cpp`
  - approach: `KnobI::getContextReadSet()` returns the union of sources registered on the knob (a `ContextReadSource` interface that expressions, in M71, and native drivers, in M180, implement; none by default). `Node::getUpstreamContextReadSet()` is the union of its own knobs' read-sets, a virtual `Node::getExtraContextReads()`, its inputs' and expression-dependency nodes' read-sets. Memoize and invalidate on the events that recompute `computeHashRecursive`; use a visited set against expression cycles. This replaces M71.P1.T3, minus the expression scan.
  - verify: gtest on a three-node chain plus a side-branch expression dependency, with a test read source on one knob: read-sets are as expected, and removing the source updates them.
  - size: M

## Phase 178.2: Request pass and keys

- [x] M178.P2.T1 — Carry contexts through the request pass, with fan-out and cross-context edges
  - files: `Engine/ParallelRenderArgs.h`, `Engine/ParallelRenderArgs.cpp` (`visitRequestPassItem`, `getInputsRoIsFunctor`), `Engine/EffectInstance.h`
  - approach: Request items carry a `RenderContextPtr` (root: empty). Add a virtual `EffectInstance::getInputContexts(inputNb, time, ctx)` returning the list of contexts to request that input under (default: `{ctx}`), and a way for a node to request another node's output under a named target context (for M182 - Loop Feedback's back-edge). Record on each `NodeFrameRequest` its key `ctx.restrictTo(node->getUpstreamContextReadSet()).hash()`. Reject a request whose target equals its own (node, key).
  - verify: gtest with a test-only `ContextFanOutTestEffect` that fans its input into 3 contexts: the request pass holds 3 entries for a reading node and 1 for a non-reading ancestor; a self-targeting request fails with an error.
  - size: L

- [x] M178.P2.T2 — Key the args map and request map by (node, context key)
  - files: `Engine/ParallelRenderArgs.h`, `Engine/ParallelRenderArgs.cpp` (`buildArgsMap`, `FrameRequestMap`), `Engine/FrameRenderContext.h`, `Engine/FrameRenderContext.cpp`
  - approach: Introduce `NodeCtxKey {NodePtr, uint64 ctxKey}`. Key 0 entries are shared as today. Keep a thin lookup overload for callers without a context. This replaces M71.P3.T2.
  - verify: gtest on the fan-out graph: the reader appears 3 times in the request map, the context-free ancestor once.
  - size: L

- [x] M178.P2.T3 — Key scheduler tasks and edges by target context
  - files: `Engine/RenderScheduler.h`, `Engine/RenderScheduler.cpp`, `Engine/FrameRenderContext.h`
  - approach: Add the context key to `FrameStore::TaskKey`, `SharedOutputKey` and `FrameGraph::Task` identity. Each `TaskEdge` carries the dependency's target context key explicitly, so an edge may cross contexts (fan-out, feedback, fold chain); do not assume edges connect equal keys. This replaces M71.P3.T3, whose "matching context key" rule is wrong for loops.
  - verify: `Tests/FrameGraphBuild_Test.cpp` gains cases: the fan-out graph's task counts, a chain where task k depends on task k−1 under a different key, and context-free nodes shared.
  - size: L

- [x] M178.P2.T4 — Fold the context key into the render hash
  - files: `Engine/EffectInstance.cpp` (`getRenderHash`), `Engine/ParallelRenderArgs.cpp`, `Engine/ViewerInstance.cpp`
  - approach: Mix the node's context key into `args->nodeHash` / `NodeFrameRequest::nodeHash`; `ImageKey`, `ActionsCache`, `DeepImageKey` and the viewer texture cache take it unchanged. Key 0 leaves the hash unchanged. This replaces M71.P3.T4.
  - verify: with `CountingTestEffect`, a reader under two different keys renders twice and under two equal keys once; a context-free node's hash is bit-identical to before.
  - size: M

- [x] M178.P2.T5 — Install the context on the executing thread, on both schedulers
  - files: `Engine/RenderScheduler.cpp` (`executeTask`), `Engine/TLSHolderImpl.h` (`createFromFrameContext`), `Engine/EffectInstance.cpp` (legacy path)
  - approach: The task body runs under a `FrameContextScope` that selects the args entry for `(node, ctxKey)`, and exposes the current context through `getParallelRenderArgsTLS`. The legacy path pulls each context's input serially under the same scope. This replaces M71.P3.T5.
  - verify: `renderBothWays` on the fan-out graph is bit-identical across both schedulers and 1 vs N pool threads.
  - size: L

## Phase 178.3: Caches

- [ ] M178.P3.T1 — Key the expression result cache by context
  - files: `Engine/KnobImpl.h` (`getValueFromExpression`, `evaluateExpression`, `_exprRes` fill sites), `Engine/Knob.cpp`
  - approach: Key `_exprRes` by (time, view, restricted context key) instead of time only, for knobs with a non-empty read-set; others keep today's key. Don't hold `_valueMutex` longer than today. This replaces M71.P2.T2's bypass, which would re-run Python per read.
  - verify: gtest evaluates one expression-like test knob under two keys on one frame (two values) and repeats each (no re-evaluation; a counter stays at 2); a context-free knob still hits once.
  - size: M

- [ ] M178.P3.T2 — Size the actions cache to the contexts in flight
  - files: `Engine/EffectInstancePrivate.cpp`, `Engine/EffectInstancePrivate.h`
  - approach: The LRU holds max(kMin, 2×threads) hashes (`EffectInstancePrivate.cpp:460`), which N contexts thrash. Grow its capacity per frame to the node's context count from the request pass (bounded by the M178 context cap), shrinking back after the frame.
  - verify: gtest with a 64-context fan-out: each context's RoD/RoI actions run once per frame (counter), not repeatedly.
  - size: M

- [ ] M178.P3.T3 — Snapshot metadata per (node, context)
  - files: `Engine/ParallelRenderArgs.cpp`, `Engine/EffectInstance.cpp` (`getMetadata(time, view)` from M167), `Tests/RenderContextMetadata_Test.cpp`
  - approach: The per-frame metadata snapshot M167 - Per-Frame Node Metadata stores beside `nodeHash` is taken per `(node, ctxKey)`; since the context key is folded into the hash, `ActionsCache` keys it per context with no further change. Main-thread UI keeps reading the base context.
  - verify: gtest: a fan-out test node whose contexts drive a Reformat-like test node's output size reports a different format per context downstream, and a context-free node's metadata is computed once.
  - size: M

- [ ] M178.P3.T4 — Warn on runaway context counts
  - files: `Engine/ParallelRenderArgs.cpp`, `Engine/Node.cpp` (persistent message)
  - approach: Count contexts per frame in the request pass; past 4096, log one warning per render naming the fan-out nodes and set a persistent warning badge on the outermost one, cleared when the count drops.
  - verify: gtest with a 2×64×64 nested test fan-out: the render completes and the warning is logged once.
  - size: S

## Decisions

- 2026-10-25 — **Request pass under contexts (bosley-native-nodes):** the shared-output key needs no separate context key because the task's node hash already mixes it in; the empty context keys to 0 and leaves every hash unchanged. Tasks under a non-empty context stay on the CPU for now. Known gaps carried into Phase 178.3: request-pass actions (metadata, warm-cache RoD) still read empty-context state under the mixed hash; a fan-out node's read-set includes the variables it sets; only direct self-requests are rejected; named-target outputs are planned but not yet fetchable in render.
- 2026-10-25 — **Freshness check (bosley-native-nodes):** references re-verified against the base branch; corrections in `PLAN/DESIGN/2026-10-25-freshness-m122-m192-m202-m178-m185-m20.md` override stale paths, lines and APIs in this file. M178 rebases onto M202 when that lands.
- 2026-10-25 — **Split from M71 - RFV Render Context (user):** the per-context core ships first and alone, so E15 - Loops And I-Nodes does not wait on RFV declarations or Python.
- 2026-10-25 — **No hard context cap (user):** a per-context entry costs ~1–2 KB and ~10–50 µs of actions, far below an unrolled graph of real nodes (tens to hundreds of KB each plus O(N²) graph hotspots), and the fold chain bounds pixel memory; a warning past 4096 contexts is enough.
- 2026-10-25 — **Per-context metadata in scope, after M167 (user):** M167 - Per-Frame Node Metadata keys metadata by (hash, time, view), so folding the context key into the hash makes it per-context almost for free.

**Verification gate:** the full build and ctest pass, including the new context tests and the existing `SchedulerEquivalence`, `FrameGraphBuild` and `GraphScalingTLS` suites unchanged. `tools/bench` shows no regression on graphs with no contexts (key 0 path). `git clang-format` against the merge base is clean.
