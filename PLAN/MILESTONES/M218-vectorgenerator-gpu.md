# M218 - VectorGenerator GPU

Run M217 - VectorGenerator Node's two classical methods on the GPU with Slang kernels: the pyramid, DIS search and densification, variational refinement, TV-L1, the consistency confidence, matte separation and temporal smoothing. The node keeps one code path for its knobs, and only the solver moves. Flow is iterative, and the GPU's red-black ordering differs from the CPU's SOR ordering, so the gate checks the GPU against ground truth and against the CPU within a tolerance, never bit-exactly.

Deps:
- M217 - VectorGenerator Node: the CPU library, its tests and its fixtures.
- M124 - Native Time Remaps: GPU input views keyed by (input, time), and placement that keeps a resident region through multi-time nodes.

Scouted facts (2026-10-25, `origin/main` at `3c8553ec`; re-check at promotion):
- Kernels are `Engine/Gpu/Kernels/*.slang`, each with a `.cmake`.
- There is no pyramid, downsample or mip kernel. `Engine/Gpu/GpuTexture.cpp:126` uses `mipLevels = 1` (`GpuTexture.h:28`: "no mipmaps"), so pyramid levels are separate textures.
- Reusable kernels:
  - `Engine/Gpu/GpuBlur.{h,cpp}` with `Kernels/blur.slang`: separable, radius capped at `kGpuBlurMaxRadius`.
  - `Engine/Gpu/GpuResample.{h,cpp}` with `Kernels/resample.slang`.
- Kernels must never take `%` of a possibly negative signed value (`DECISIONS/2026-10-24-nvidia-signed-remainder.md`).

## Scope

**In:**
- GPU kernels for every M217 stage, a GPU path in `VectorGenerator` taken when placement puts it on the GPU, parity tests on lavapipe, and its `gpuWeight` (Heavy).

**Out:**
- The neural method on the GPU → M221 - Neural Optical Flow (runs through ONNX Runtime, CPU-side tensors).
- New CPU features → none. Behaviour stays M217's.

**Edge cases:**
- **VRAM:** a full-frame pyramid of three or five frames, plus the fields, has to fit. If the allocation fails, the node falls back to the CPU path for that render, with a log line and no error.
- **Odd level sizes:** handled exactly as on the CPU (M217.P1.T1's test cases).
- **Abort:** checked between dispatch batches.

**Assumptions:**
- M124's (input, time) views can deliver the neighbour frames' full RoD to one dispatch.

## Phase 218.1: Kernels

- [ ] M218.P1.T1 — Pyramid and gradient kernels
  - files: `Engine/Gpu/Kernels/flow_pyramid.slang` (new), `Engine/Gpu/Kernels/flow_pyramid.cmake` (new), `Engine/Gpu/GpuFlow.{h,cpp}` (new), `Tests/Gpu/GpuFlowPyramid_Test.cpp` (new)
  - approach:
    - Grey conversion, a 2× downsample after a `GpuBlur` pre-blur, and gradients, matching `FlowPyramid`.
    - `GpuFlow` owns the per-level textures and the dispatch order.
  - verify: `ctest -R GpuFlowPyramid` passes on lavapipe, matching `FlowPyramid` within 1e-5 per level.
  - size: M

- [ ] M218.P1.T2 — DIS search and densification kernels
  - files: `Engine/Gpu/Kernels/flow_dis.slang` (new), `Engine/Gpu/Kernels/flow_dis.cmake` (new), `Engine/Gpu/GpuFlow.cpp`, `Tests/Gpu/GpuDisFlow_Test.cpp` (new)
  - approach: one thread per patch for the inverse search, with the Gauss-Newton loop in-thread. Densification is one thread per pixel, gathering the overlapping patches.
  - verify: `ctest -R GpuDisFlow` passes. The GPU stays within the CPU's recorded EPE thresholds on the M217 synthetic cases, and within 0.05 px mean of the CPU's field.
  - size: L

- [ ] M218.P1.T3 — Variational refinement kernel
  - files: `Engine/Gpu/Kernels/flow_refine.slang` (new), `Engine/Gpu/Kernels/flow_refine.cmake` (new), `Engine/Gpu/GpuFlow.cpp`, `Tests/Gpu/GpuDisFlow_Test.cpp`
  - approach: compute the data and smoothness terms per pixel, then run red-black SOR with two dispatches per iteration.
  - verify: `ctest -R GpuDisFlow` passes with refinement on, under the same thresholds as T2.
  - size: L

- [ ] M218.P1.T4 — TV-L1 kernels
  - files: `Engine/Gpu/Kernels/flow_tvl1.slang` (new), `Engine/Gpu/Kernels/flow_tvl1.cmake` (new), `Engine/Gpu/GpuFlow.cpp`, `Tests/Gpu/GpuTvL1Flow_Test.cpp` (new)
  - approach: the warp, thresholding, primal and dual updates, and the median filter, each one thread per pixel.
  - verify: `ctest -R GpuTvL1Flow` passes, within the CPU's EPE thresholds and within 0.05 px mean of the CPU.
  - size: L

- [ ] M218.P1.T5 — Confidence, matte and temporal kernels
  - files: `Engine/Gpu/Kernels/flow_confidence.slang` (new), `Engine/Gpu/Kernels/flow_confidence.cmake` (new), `Engine/Gpu/GpuFlow.cpp`, `Tests/Gpu/GpuFlowConfidence_Test.cpp` (new)
  - approach:
    - The round-trip confidence.
    - The masked data-term weights for the two matte passes, and their composite.
    - The temporal warp-and-blend.
    - All of them mirror `FlowConfidence`, `FlowEstimate` and `FlowTemporal`.
  - verify: `ctest -R GpuFlowConfidence` passes, matching the CPU within 1e-3 on M217's moving-square and constant-velocity cases.
  - size: M

## Phase 218.2: Node wiring

- [ ] M218.P2.T1 — GPU path in VectorGenerator
  - files: `Engine/Nodes/Time/VectorGenerator.cpp`, `Tests/Native/NativeVectorGeneratorGpu_Test.cpp` (new)
  - approach:
    - Declare GPU support and the per-input times for placement.
    - Feed `GpuFlow` from the (input, time) views. Write Forward, Backward and `vectorConfidence` to the output textures.
    - Fall back to the CPU when allocation fails.
    - Declare `gpuWeight` Heavy.
  - verify: `ctest -R NativeVectorGeneratorGpu` passes on lavapipe. Every M217 node case runs on the GPU path within the tolerances above, and a forced allocation failure takes the CPU path and still matches.
  - size: M

**Verification gate:**
- The build and the full ctest pass on lavapipe, including every `GpuFlow*`, `GpuDisFlow`, `GpuTvL1Flow` and `NativeVectorGeneratorGpu` test.
- The release AppImage, run on the target host's GPU, records DIS and TV-L1 Medium timings on a 2K frame next to M217's CPU numbers in `## Decisions`.
- An Xvfb screenshot of the Forward layer from the GPU path, shared before sign-off.
- An AppImage built with `package.sh` for UAT.

## Decisions

- 2026-10-25 — **The GPU is checked against ground truth plus a tolerance to the CPU, never bit-exactly (planner):** red-black SOR and parallel reductions order operations differently from the CPU. This is the iterative-solver case of the ulp-diff rule.
