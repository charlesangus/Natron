# M180 - Loop Block Core

A Houdini-style loop block (user, 2026-10-24): `LoopBegin` and `LoopEnd` nodes around any subgraph. LoopEnd requests the body once per iteration, each under its own render context (M178 - Render Context Core) carrying `loop.iteration`, `loop.count`, `loop.t` (0..1) and `loop.frame`, with outer loops visible as `loops.<name>.*`. Knobs in the body are driven per iteration by a **native driver** (start value at `t`=0, end value at `t`=1, an optional easing curve), a C++ read that never touches Python, since every Python expression takes the GIL on render threads (`Engine/KnobImpl.h:401`). LoopEnd combines the iterations by **Accumulate** (mean, sum, min, max, over), unrolled into a chain of fold tasks so only O(1) iteration images are resident and the result is bit-exact for any thread count. Iterations run by count or over a frame range relative to the current frame. A single-pass debug mode shows one iteration, and a stop-condition expression ends the loop early, evaluated before each iteration (user, 2026-10-25). Loops carry images only in v1. Feedback is M182 - Loop Feedback; Map blend is M183 - Loop Map Blend.

## Scope

**In:**
- `LoopBegin` / `LoopEnd` native nodes (image kind; the body is any subgraph between them), registered at major 1 under a new "Loop" grouping.
- Iteration by count (`iterations`, `start`, `step`) and by frame range (`frameStart`, `frameEnd`, `frameStep`, relative to the current frame).
- Loop variables as namespaced context variables, nested loops via `loops.<name>.*`.
- The native knob driver (`KnobDriver`) as a context read source, with its serialization.
- Accumulate modes (mean, sum, min, max, over, under, with a reverse-order toggle) with the fold chain, single pass, the stop-condition expression, and a max-iterations safety cap.
- CPU and GPU fold kernels.

**Out:**
- The "Drive from loop" knob UI, the node-graph block visuals and Python expression access to loop variables: M181 - Loop Drivers And UI.
- Deep and scene data through a loop: refused at connection time in v1; deep folding would be a later E8 - Deep Compositing milestone.
- Merge's full operator set for Accumulate: no milestone owns it.
- Feedback: M182 - Loop Feedback. Map blend and Make I-Node: M183 - Loop Map Blend, M189 - Make I-Node Action.
- Iterating over layers or IDs (user cut, 2026-10-24).
- Metadata (format, layers) that varies per iteration: stays at base values.

**Edge cases:**
- LoopEnd with no matching LoopBegin upstream, or a LoopBegin reached from two LoopEnds: a node error, rendering black with a persistent message.
- Iterations 0: LoopEnd outputs black with a warning. Iterations 1: the body once at `t`=0.
- `t` for a count loop is `iteration / (count − 1)`, and 0 when count is 1.
- A body node that reads no loop variable keeps context key 0 and renders once, whatever the count.
- The stop condition is a Python boolean expression on LoopEnd, evaluated in the request pass before each iteration with `loop.*` injected into its namespace by LoopEnd itself (no M71 - RFV Render Context dependency). It can read knobs and the frame but never pixels. An exception stops the loop at that iteration and sets a persistent error message.
- The max-iterations cap (default 1024, a LoopEnd knob up to the M178 context cap) fails the render with a message rather than silently truncating.

## Phase 180.1: Nodes and contexts

- [x] M180.P1.T1 — Write LoopBegin and LoopEnd nodes with count iteration
  - files: `Engine/Nodes/Loop/LoopBegin.{h,cpp}`, `Engine/Nodes/Loop/LoopEnd.{h,cpp}`, `Engine/Nodes/Loop/LoopBegin.reg.inc`, `Engine/Nodes/Loop/LoopEnd.reg.inc`
  - approach: Both nodes accept and produce images only; a deep or scene connection is refused with a message. LoopBegin is an identity on its input that marks the body's start and carries a `name` knob (unique per graph scope, default `loop1`). LoopEnd finds its LoopBegin by walking its input's upstream (error if none or ambiguous), and overrides M178's `getInputContexts` to request its input once per iteration, each context overlaying `loop.*` (and the current `loop.*` renamed to `loops.<outerName>.*`). Knobs: `iterations`, `start`, `step`, `mode` (Accumulate for now), `maxIterations`. Read `Engine/Nodes/README.md` first.
  - verify: at the phase build, `ctest -R NativeLoopBlock` (T3) passes.
  - size: L

- [x] M180.P1.T2 — Write the native knob driver
  - files: `Engine/KnobDriver.h`, `Engine/KnobDriver.cpp` (new), `Engine/KnobImpl.h` (`getValueAtTime`), `Engine/Knob.cpp`, the knob serialization under `Serialization/`
  - approach: A `KnobDriver` on a numeric knob dimension names a context variable (default `loop.t`), a start and end value and an easing (linear, smooth, a parametric curve). `getValueAtTime` returns the eased lerp when the current thread's context holds the variable, else the knob's own value. It registers as an M178 read source naming that variable. Save and load it with the knob.
  - verify: gtest: a driven knob reads start at `t`=0, end at `t`=1, the eased midpoint at 0.5, its own value outside a context; a project save/load round-trips the driver.
  - size: L

- [x] M180.P1.T3 — Test the loop block on CPU
  - files: `Tests/Native/NativeLoopBlock_Test.cpp`
  - approach: Constant → Grade (gain driven 1→2 by `loop.t`) between LoopBegin and LoopEnd (Accumulate mean, 5 iterations): the output is the mean gain 1.5. A body node with no driver renders once (counter). A nested loop (outer 2 × inner 3) sees both `loop.t` and `loops.outer.t`. Error cases from `## Scope` render black with their message.
  - verify: at the phase build, `ctest -R NativeLoopBlock` passes.
  - size: M

## Phase 180.2: Accumulate fold chain

- [x] M180.P2.T1 — Unroll Accumulate into a fold chain of tasks
  - files: `Engine/Nodes/Loop/LoopEnd.cpp`, `Engine/Nodes/Loop/LoopFold.h`, `Engine/Nodes/Loop/LoopFold.cpp`
  - approach: LoopEnd declares fold step k as depending on fold k−1 and iteration k (cross-context edges from M178.P2.T3), so iterations render ahead in parallel up to the bytes budget while the fold runs in fixed k order. Modes: mean, sum, min, max, over (iteration k over the accumulator), under (the accumulator over iteration k); `reverseOrder` folds from the last iteration down. Iteration outputs bypass the image cache. On the legacy path, iterations are pulled and folded serially.
  - verify: gtest: 64 iterations of a 4K float body hold at most budget-many iteration images (FrameStore high-water mark), and the result is bit-identical for 1 and N threads and both schedulers.
  - size: L

- [ ] ~~M180.P2.T2~~ — moved 2026-10-27 to M259 - GPU Placement Under Render Contexts (GPU placement refuses context-carrying tasks; needs engine work beyond this milestone). Original:
  M180.P2.T2 — Fold kernels on the GPU
  - files: `Engine/Gpu/Kernels/loopfold.slang`, `Engine/Gpu/Kernels/loopfold.cmake`, `Engine/Nodes/Loop/LoopFold.cpp`, `Tests/Native/NativeLoopFoldGpu_Test.cpp`
  - approach: One point kernel per mode (mean, sum, min, max, over, under; reverse order swaps the fold direction), accumulator resident across steps (M84 - GPU Placement And Residency pools).
  - verify: at the phase build, `ctest -R NativeLoopFoldGpu` passes on lavapipe, bit-exact for min/max and within 1e-6 relative for mean/sum/over.
  - size: M

## Phase 180.3: Frame iteration, single pass and stop

- [x] M180.P3.T1 — Iterate over a relative frame range
  - files: `Engine/Nodes/Loop/LoopEnd.cpp`, `Tests/Native/NativeLoopBlock_Test.cpp`
  - approach: An `iterateOver` choice (Count, Frames). Frames requests the body at `time + offset` per iteration with `loop.frame` set; time is already in every key, so iterations that read no loop variable share caches with ordinary renders of those frames. Frames needed for the body are reported through the frames-needed action.
  - verify: a test with a frame-numbered Constant body and frames −2..2 Accumulate mean gives the mean of the five frame numbers; `ctest -R NativeLoopBlock` passes.
  - size: M

- [x] M180.P3.T2 — Single pass and stop condition
  - files: `Engine/Nodes/Loop/LoopEnd.cpp`, `Tests/Native/NativeLoopBlock_Test.cpp`
  - approach: `singlePass` (bool) plus `singlePassIteration` output only that iteration, unfolded. `stopCondition` is a Python boolean expression knob, evaluated in the request pass before each iteration k with `loop.iteration`, `loop.t`, `loop.frame` injected; the first true ends iteration before k. Both change the context list LoopEnd requests.
  - verify: single pass at k=2 equals the body rendered at `t`=2/(n−1); `loop.iteration >= 3` with 10 iterations renders 3 (counter); a raising expression stops with its error shown; tests pass.
  - size: M

## Phase 180.4: Documentation

- [ ] M180.P4.T1 — Document the loop block
  - files: `Engine/Nodes/Loop/docs/LoopBegin.md`, `Engine/Nodes/Loop/docs/LoopEnd.md`
  - approach: Variables, nesting names, the modes, the fold order, the caps, the metadata limitation, and the GPU tolerance line.
  - verify: reviewed at the gate against the node code.
  - size: S

## Decisions

- 2026-10-27 — Gate green (99a0de9d4). AppImage build/appimages/Natron-m180-loop-block-core.AppImage; render benches vs main running before UAT.
- 2026-10-27 — **Fold chain on the CPU only (bosley-native-nodes):** P2.T1 unrolls Accumulate into per-step LoopEnd tasks under `loopFold.step`, shared bit-exact fold arithmetic (LoopFold) for task-graph and legacy, loop outputs bypass the image cache, and the request pass walks context requests before inputs (so iteration memory stays within budget). GPU fold kernels moved to M259, since GPU placement keeps context tasks on the CPU (M178).
- 2026-10-27 — **Phase 1 knob choices (bosley-native-nodes):** `loop.iteration = start + k*step`; Iterations defaults to 4; LoopEnd has a `mode` choice (Accumulate now, Single Pass in Phase 3) and a separate `operation` choice for the fold (mean, sum, min, max; over and under join in Phase 2). A knob driver wins over an expression; linear easing extrapolates outside 0..1, smooth and curve clamp; `clone()` does not copy drivers.
- 2026-10-26: `loop.*` context variables stay plain values, never RFV bindings (M71 Phase 71.5 introduced time-resolved bindings for animated Set variables).
- 2026-10-25 — **Houdini-style loop block on render contexts (user):** chosen over hidden clones (no cache sharing, graph edits mid-render, O(N²) graph hotspots) and over knob-override args (the same core without read-sets); see M178 - Render Context Core.
- 2026-10-25 — **Native drivers, no Python on the hot path:** every Python expression takes the GIL on render threads (`Engine/KnobImpl.h:401`) and the tree has no exprtk.
- 2026-10-25 — **Stop condition is an expression per iteration (user):** Houdini's model; evaluated in the request pass, never per pixel.
- 2026-10-25 — **Accumulate is a fixed operator set (user):** mean, sum, min, max, over, under, reverse toggle; no dependency on M101 - Clean-Sheet Merge Node.
- 2026-10-25 — **Images only in v1 (user).**
- 2026-10-25 — **Layer and ID iteration cut (user);** frame iteration, single pass and stop kept.

**Verification gate:** the full build and ctest pass, including `NativeLoopBlock` and `NativeLoopFoldGpu`; `renderBothWays` is bit-identical on every loop test graph; a 64-iteration 4K Accumulate render stays within the bytes budget on the 15 GB dev host. Under Xvfb, a project with a blur-driven Accumulate loop and a frame-range echo is rendered; screenshots are shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
