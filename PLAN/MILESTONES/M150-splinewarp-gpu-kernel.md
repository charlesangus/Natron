# M150 - SplineWarp GPU Kernel

Run SplineWarp's resample on the GPU. The coordinate field is still built on the CPU (by either solver), uploaded as a two-channel texture, and the kernel does a bilinear field lookup, takes the Jacobian from the field differences, maps through the concatenated input matrix, and samples with M113 - Native Warp Nodes's `coordmap.slang`. It goes through M137 - Native Base Class Flexibility's Custom-kind GPU executor, and falls back to the CPU wherever the GPU path declines.

Deps: M147 - SplineWarp Node Core (the field and the node). `coordmap.slang` comes from M113 - Native Warp Nodes, which M147 - SplineWarp Node Core already depends on.

Scouted facts (2026-10-20; re-check at promotion):
- Kernels are listed in `Engine/Gpu/CMakeLists.txt:23-44`, and the `GpuKernelId` enum is generated (`GpuKernelRegistry.h:20-25`). M120 - Native Node Port Kit moves registration to per-kernel `Engine/Gpu/Kernels/<kernel>.cmake` fragments.
- M113.P1.T2 adds `Engine/Gpu/Kernels/coordmap.slang`, with `sampleAt(src, sx, sy, J, filter, clamp, blackOutside, wrapU, wrapV)`.
- GPU parity tests use the NativeGpuParity harness (`Tests/Native/*Gpu_Test.cpp`), and run on RADV at the phase build. Local trees can't compile GPU translation units until slang is configured, so GPU work compiles remotely (see the project memory on Slang for local GPU builds).

## Scope

**In:**
- `splinewarp.slang`: field lookup, Jacobian, the concatenated-matrix map, and `coordmap.slang` sampling. It covers every filter, clamp and black outside.
- Field upload, and the GPU path wired through the Custom-kind GPU executor, with a size-scaled `gpuWeight`.
- A parity test against the CPU path.

**Out:**
- GPU solving or GPU mesh rasterization → none. The field is small, and CPU building keeps the solvers in one place.
- GPU motion blur → M151 - SplineWarp Motion Blur.

**Edge cases:**
- Host mask and mix: handled by the framework, exactly as on the CPU path.
- A field larger than the maximum texture size (a tiny field step on a huge window): decline the GPU path, and fall back to the CPU.

## Phase 150.1: GPU path

- [ ] M150.P1.T1 — `splinewarp.slang` kernel
  - files: `Engine/Gpu/Kernels/splinewarp.slang` (new), `Engine/Gpu/Kernels/splinewarp.cmake` (new), `Tests/Native/NativeSplineWarpGpu_Test.cpp` (new)
  - approach:
    - Inputs: the source texture, the field texture (RG32F) with its origin and step, the inverse concatenated 3x3 matrix, and the filter flags.
    - Per pixel: do a bilinear field lookup, take the Jacobian from neighbouring field samples (the same formula as the CPU path), map through the matrix, and call `sampleAt`.
  - verify: the test's kernel-level cases (a uniform field, an affine field, and each filter) match the CPU `resampleRowFromCoords` within 1e-5 absolute. They run on RADV at the phase build.
  - size: L

- [ ] M150.P1.T2 — Wire the GPU path into SplineWarp
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Tests/Native/NativeSplineWarpGpu_Test.cpp`
  - approach:
    - Implement the Custom-kind GPU hooks: build the field on the CPU, upload it, and dispatch.
    - Decline for motion blur (until M151 - SplineWarp Motion Blur) and for oversized fields.
    - Declare its `gpuWeight` (size-scaled).
  - verify: node-level parity against the CPU path within 1e-5, on M147 - SplineWarp Node Core's gate fixture with both solvers and with Transform concatenation. It runs on RADV.
  - size: M

**Verification gate:**
- The build and the full ctest pass on the remote GPU leg, including `NativeSplineWarpGpu_Test` on RADV.
- The CPU fallback is exercised for an oversized field.

## Decisions

- 2026-10-20 — **A GPU kernel is in scope (user).**
- 2026-10-20 — **Field built on the CPU, sampled on the GPU (planner):** one solver implementation, and the GPU only does the per-pixel work.
