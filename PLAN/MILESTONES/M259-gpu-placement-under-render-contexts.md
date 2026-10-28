# M259 - GPU Placement Under Render Contexts

Split out of M180 - Loop Block Core (2026-10-27). M178 - Render Context Core kept every task under a non-empty render context on the CPU "for now", and GPU regions don't carry the context-request edges between tasks. This lets GpuPlacement put context-carrying tasks on the GPU: lookups go by (node, context key), and their context and frame args are installed while they record. GPU regions carry context-request edges, so LoopEnd's fold chain and the feedback chain stay on the device. Adds the moved M180.P2.T2 fold kernels.

## Scope

**In:**
- Placement and region execution for tasks whose contextKey ≠ 0: loop iterations, RFV Set contexts, fold steps.
- Context-request edges (`kContextRequestInputNb`) as region inputs.
- `loopfold` kernels for mean, sum, min, max, over, under and reverse order; LoopBegin's feedback forward on the GPU.

**Out:**
- Cross-frame cache on the GPU: M190 - Cross-Frame Feedback stays on the CPU and in host RAM.
- Placement changes for context-free tasks.

**Edge cases:**
- The device budget is smaller than one iteration's image: the task falls back to the CPU and the render still succeeds.
- Many contexts: buffer reuse and strip halving (`Engine/GpuRegionStrips.h:182`) bound peak device memory; nothing may exceed the pool budget.
- A context-carrying task whose node cannot render on the GPU splits the region (as today).

## Phase 259.1: Context tasks on the GPU

- [ ] M259.P1.T1 — Look up and render context tasks by (node, context key)
  - files: `Engine/GpuPlacement.cpp`, `Engine/GpuRegionTask.cpp`
  - approach: drop the `contextKey != 0` refusals (GpuPlacement.cpp:601, :707). Use `NodeCtxKey(node, task.key.contextKey)` for `find`/`findFrameViewRequest`/`getArgs` (:609, :643, :711-712). Wrap `canRenderOnGpu`, `getGpuStageSummary` and `renderGpu` per member in `ContextFrameArgsScope` + `ActionContextScope`, mirroring RenderScheduler.cpp:1166, so driven knobs read their context.
  - verify: at the phase build, `ctest -R NativeGpuContext` (T3) passes.
  - size: L
- [ ] M259.P1.T2 — Carry context-request edges in regions
  - files: `Engine/Nodes/Image/GpuRender.h`, `Engine/GpuRegionTask.cpp`, `Engine/GpuPlacement.cpp`
  - approach: add `GpuRenderArgs::contextInputs`, filled in `fvRequest->dependencies` order for −3 edges (edge build at GpuRegionTask.cpp:~783), and count them as external inputs or internal edges when forming regions.
  - verify: at the phase build, `ctest -R NativeGpuContext` passes.
  - size: L
- [ ] M259.P1.T3 — Test context tasks on the GPU
  - files: `Tests/Native/NativeGpuContext_Test.cpp`
  - approach: on lavapipe, a Grade driven by `loop.t` in a 5-iteration Accumulate loop puts the body tasks on `Device::Gpu` and matches the CPU result within tolerance. Under an RFV Set context, each context gets distinct image keys (no hits across contexts).
  - verify: at the phase build, `ctest -R NativeGpuContext` passes.
  - size: M

## Phase 259.2: Fold kernels and residency

- [ ] M259.P2.T1 — Fold kernels for LoopEnd
  - files: `Engine/Gpu/Kernels/loopfold.slang`, `Engine/Gpu/Kernels/loopfold.cmake`, `Engine/Nodes/Loop/LoopEnd.{h,cpp}`
  - approach: one point kernel per `LoopFold::OperationEnum`, matching `foldRow`/`finishRow`. LoopEnd gains `opKind`, `canRenderOnGpu` and `renderGpu` reading step k−1 from `contextInputs`. Add LoopEnd to `gpuCapableIds()` in `Tests/Native/NativeGpuCapability_Test.cpp`.
  - verify: at the phase build, `ctest -R NativeLoopFoldGpu` (T3) passes.
  - size: L
- [ ] M259.P2.T2 — LoopBegin's feedback forward on the GPU
  - files: `Engine/Nodes/Loop/LoopBegin.{h,cpp}`
  - approach: `renderGpu` copies or conforms the tail's previous step from `contextInputs`, so a feedback chain stays in one region.
  - verify: at the phase build, `ctest -R NativeLoopFoldGpu` passes.
  - size: M
- [ ] M259.P2.T3 — Test GPU folding, residency and memory bounds
  - files: `Tests/Native/NativeLoopFoldGpu_Test.cpp`
  - approach: parity with the `NativeGpuParity.h` helpers: min/max bit-exact, mean/sum/over/under within 1e-6 relative, with reverse order. The fold chain and a 10-step feedback chain each run as one region with no host round-trip per step. 256 iterations at 4K under a capped `GpuBufferPool` userBudgetBytes keep peakBytes ≤ budget; a budget below one image falls back to the CPU.
  - verify: at the phase build, `ctest -R NativeLoopFoldGpu` passes.
  - size: M
- [ ] M259.P2.T4 — Document GPU loops
  - files: `Engine/Nodes/Loop/docs/LoopEnd.md`
  - approach: GPU tolerance line, residency, CPU fallback.
  - verify: reviewed at the gate.
  - size: S

**Verification gate:** full build + ctest incl. `NativeGpuContext`, `NativeLoopFoldGpu`, `NativeLoopBlock`, `NativeLoopFeedback`; `tools/ci/remote/build.sh debug --tests` green; a 64-iteration 4K Accumulate render measured on the GPU host (time and peak device bytes vs CPU) and recorded in `## Decisions`.

## Decisions

- 2026-10-28: phases 1–2 green (run 40641533964): NativeGpuContext and NativeLoopFoldGpu pass on lavapipe. Fold helper lives in LoopFold (shared by LoopBegin's exact forward copy); canRenderOnGpu checks only the scheduler mode (frame-store check in renderGpu); a missing step k−1 on the device falls back to CPU; mean not bit-exact (Vulkan division 2.5 ULP), kernels use `precise`. Remaining gate: 64-iteration 4K timing on the GPU host (UAT doc build/appimages/M259-uat.md).
- 2026-10-28: P1 implemented (build pending). ContextFrameArgsScope alone mirrors the CPU path (it opens ActionContextScope). Context inputs reach nodes as GpuContextInput (layer + view), queried with inputNb == kContextRequestInputNb; a node that reads a −3 edge but can't name its layer stays on the CPU.
- 2026-10-28: elaborated (consultant). Branch stacks on M182 - Loop Feedback (feedback chain). Strip partitioning inside a fold region relies on the existing strip halving, with P2.T3's capped-budget test as proof.
