# M71 - RFV Render Context

Reverse-Flow Variables, part 2 of 3 (design in `DECISIONS/2026-10-08-reverse-flow-variables-design.md`, which governs on any ambiguity). Depends on M70 - RFV Declaration And Storage. This milestone makes variables flow up the graph per path, evaluate in expressions through `rfv`, and key every cache correctly. The hard part is that the render machinery is keyed by node (args map, `FrameRequestMap`, `FrameStore::TaskKey`, `ImageKey`, `ActionsCache`) and assumes one context per node per frame. Nodes that read no RFV must collapse to one context so existing graphs render and cache exactly as before.

Scouting notes: `treeRoot` is the viewer/writer requester only, with about 10 setter sites; identity nodes never execute, so the Set node's overlay happens in the request pass (`visitRequestPassItem`, `Engine/ParallelRenderArgs.cpp`), not in `render`. Expression results are cached in `Knob<T>::_exprRes`, keyed by time only (`Engine/KnobImpl.h`). Pool threads get frame args lazily via `createFromFrameContext` (`Engine/TLSHolderImpl.h`).

## Phase 71.1: Context value and read-set

- [ ] M71.P1.T1 — RFV context value type
  - files: `Engine/RFVContext.h`, `Engine/RFVContext.cpp` (new), `Engine/CMakeLists.txt`, `Tests/RFVContext_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: an immutable map name → variant (bool, int64, double, string, vector of double/int/string, matching what the knob types can hold), with `overlay(other)` returning a new context, `restrictTo(nameSet)`, and a stable 64-bit `hash()` (order-independent, bit-exact for doubles). Cheap copy through a shared pointer. `readsAll` sentinel for the restriction.
  - verify: gtest for overlay precedence, restriction, hash stability and hash equality of equal contexts built in different orders.
  - size: M

- [ ] M71.P1.T2 — Expression read-set scan
  - files: `Engine/Knob.cpp`, `Engine/Knob.h` (new `KnobHelper::getRFVReadSet`), `Engine/RFV.cpp`
  - approach: scan each expression's source for `rfv.<name>`, `rfv('<name>')` and `rfv["<name>"]`; any other use of the name `rfv` (passed to a function, `getattr`, computed key) marks the knob as reads-all. Recompute when the expression changes.
  - verify: gtest table of expression strings → expected read-set / reads-all.
  - size: M

- [ ] M71.P1.T3 — Per-node upstream read-set
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/NodeInputs.cpp`
  - approach: `Node::getUpstreamRFVReadSet()` is the union of the node's own knob read-sets, a Set node's own knob expressions, and a per-node hook `Node::getExtraRFVReads()` that a native node overrides to name variables it reads outside expressions (RFVSwitch's variable name; default empty), and the read-sets of all inputs and expression-dependency nodes. Memoize; invalidate with the same events that recompute `computeHashRecursive` (the input connection changes and expression changes). Beware of cycles through expression dependencies: use a visited set.
  - verify: gtest on a three-node chain plus an expression dependency from a side branch: read-sets are as expected, and editing an expression updates it.
  - size: M

## Phase 71.2: Evaluation

- [ ] M71.P2.T1 — Add the `rfv` Python object
  - files: `Engine/Knob.cpp` (`declarePythonVariables`), `Engine/PyRFV.h`, `Engine/PyRFV.cpp` (new), `Engine/CMakeLists.txt`
  - approach: a Python-visible object whose `__getattr__` and `__call__` read from the current thread's RFV context. Resolve lazily at call time, because the preamble is baked at `setExpression`. The context comes from the effect's `ParallelRenderArgs` TLS (`getParallelRenderArgsTLS`); with no render args (GUI/main thread) it reads the project base values at the timeline frame. Unset → `None`. Convert variant → Python int/float/bool/str/list.
  - verify: gtest sets `rfv.a` through the project and evaluates an expression `rfv.a * 2` outside a render (value 2×), and `rfv.missing is None` evaluates True.
  - size: M

- [ ] M71.P2.T2 — Bypass the expression result cache for RFV reads
  - files: `Engine/KnobImpl.h` (`getValueFromExpression`, `evaluateExpression`, the `_exprRes` fill sites), `Engine/Knob.cpp`
  - approach: if a knob's expression has a non-empty read-set or reads-all, never read or write `_exprRes` for it. Everything else keeps the cache. Avoid taking `_valueMutex` for longer than today.
  - verify: gtest evaluates one expression knob under two different contexts back-to-back on one frame and gets two values; a non-RFV expression still hits the cache (a counter on `executeExpression` stays at 1).
  - size: M

## Phase 71.3: Per-path propagation and keying

- [ ] M71.P3.T1 — Carry a context through the request pass and apply Set overlays
  - files: `Engine/ParallelRenderArgs.h`, `Engine/ParallelRenderArgs.cpp` (`visitRequestPassItem`, `getInputsRoIsFunctor`), `Engine/Nodes/RFV/ReverseFlowVariableSet.cpp`
  - approach: request pass items carry an `RFVContextPtr` (root: project base values evaluated at the root's time). When an item passes a Set node, evaluate its declared variables at that item's time **under the incoming context** and push the overlay onto its input edge item. Every other node passes the context along unchanged. Record on each `NodeFrameRequest` the context key `ctx.restrictTo(node->getUpstreamRFVReadSet()).hash()`. No forking of maps yet.
  - verify: gtest builds the user's example graph and inspects the request pass: a reader under Set1 holds a=Set1's value, under Set2 holds Set2's.
  - size: L

- [ ] M71.P3.T2 — Key the args map and request map by (node, context key)
  - files: `Engine/ParallelRenderArgs.h`, `Engine/ParallelRenderArgs.cpp` (`buildArgsMap`, `FrameRequestMap`), `Engine/FrameRenderContext.cpp`
  - approach: introduce `NodeCtxKey {NodePtr, uint64 ctxKey}`; nodes with an empty effective read-set always use ctxKey 0, so their entries are shared as today. Keep a thin lookup overload for callers that have no context.
  - verify: gtest on the diamond graph: the reader appears twice in the request map, a context-free node once.
  - size: L

- [ ] M71.P3.T3 — Key the scheduler tasks by context
  - files: `Engine/RenderScheduler.h`, `Engine/RenderScheduler.cpp`, `Engine/FrameRenderContext.h`
  - approach: add the context key to `FrameStore::TaskKey` and `SharedOutputKey` and to `FrameGraph::Task` identity, so the diamond produces two reader tasks and the shared context-free ancestors produce one. Edges connect each task to the dependency with the matching context key.
  - verify: `Tests/FrameGraphBuild_Test.cpp` gains a case: task counts for the user's example and the diamond, with context-free nodes shared.
  - size: L

- [ ] M71.P3.T4 — Fold the context key into the render hash
  - files: `Engine/EffectInstance.cpp` (`getRenderHash`), `Engine/ParallelRenderArgs.cpp` (`visitRequestPassItem` where `nodeHash` is set), `Engine/ViewerInstance.cpp` (texture cache hash)
  - approach: mix the node's context key into `args->nodeHash` / `NodeFrameRequest::nodeHash`; the existing consumers (`ImageKey`, `ActionsCache`, `DeepImageKey`, the viewer texture cache) take `nodeHash` as input and need no change. Context key 0 must leave the hash unchanged, so existing cache keys and `NATRON_CACHE_VERSION` stay valid.
  - verify: with `CountingTestEffect`, the same reader under two different contexts renders twice and under two equal contexts renders once; a context-free node's hash is unchanged.
  - size: M

- [ ] M71.P3.T5 — Install the context on the executing thread
  - files: `Engine/RenderScheduler.cpp` (`executeTask`), `Engine/TLSHolderImpl.h` (`createFromFrameContext`), `Engine/EffectInstance.cpp` (legacy path's setter)
  - approach: the task body runs under a `FrameContextScope` that selects the args entry for the task's `(node, ctxKey)`, so the `rfv` object on a pool thread sees that entry's context. Do the same on the legacy (non-scheduler) render path, which `Tests/RenderBothWays.h` still compares against. OpenMP workers inside plugins stay unsupported, as for the existing frame args.
  - verify: `renderBothWays` on the user's example is bit-identical across both schedulers and 1 vs N pool threads.
  - size: L

## Phase 71.4: Behaviour tests

- [ ] M71.P4.T1 — User example and project base
  - files: `Tests/RFVRender_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Checkerboard → Grade whose gain is `1 if rfv.var == 'a' else 2` → two branches, each through a Set node (`var='a'`, `var='b'`) to its own Writer. Render each writer and check the pixel scale. Also the project-level base value is overridden by a Set node, and a graph with no Set sees the project value.
  - verify: the new test passes.
  - size: M

- [ ] M71.P4.T2 — Diamond, animation, nesting, unset
  - files: `Tests/RFVRender_Test.cpp`
  - approach: add a diamond (A → Set1, A → Set2 → Merge → Writer) with `CountingTestEffect` asserting A renders twice and its context-free ancestor once; an animated variable that gives different values at frames 1 and 2; a Set knob whose expression reads a downstream `rfv` value; and an unset read that yields `None`.
  - verify: the new cases pass; `renderBothWays` bit-identical for each.
  - size: M

**Verification gate:** full ctest passes, including the new RFV tests and the existing `SchedulerEquivalence`, `FrameGraphBuild` and `GraphScalingTLS` suites unchanged; `tools/bench` shows no regression on a graph with no RFVs (context key 0 path); `git clang-format` against the merge base is clean.
