# M318 - LensCorrect Bounded Input Region

Stub. From M160 - Lensfun Lens Correction (user, 2026-11-02): LensCorrect requests its Source's whole RoD for every render (`getRegionsOfInterest`, LensCorrect.cpp), because the distortion map has no cheap bound. Zoomed viewer regions and tiled renders therefore cost a full-frame upstream render. Compute a bounded input region by evaluating the distortion map over the render window's edges and an interior grid, plus a safety margin, on CPU and GPU (`warpGpuInputRoI`). The "tiled equals full-frame" test must keep passing, including on extreme profiles (fisheye).

Blocked on: M160 - Lensfun Lens Correction merging.

## Phase 318.1: Bounded input region

- [x] M318.P1.T1 — Lattice-based conservative Source bound per output window
  - files: `Engine/Nodes/Transform/LensCorrect.cpp`
  - approach: map the window (plus a one-pixel ring for Jacobians) through the render's own `PixelMapper` on all three channels on a 33→65→129 lattice that always includes the corners (every pixel for small windows), refined while the curvature term exceeds 1 px; margin = 8× the bilinear error bound + filter reach (`max(2, radius) × kernel scale + 2`) + 2 px + 1e-4 × max coordinate; projected onto the Source RoD and merged with the window. Any lattice point without an answer, no metadata or an infinite RoD falls back to window ∪ whole Source.
  - verify: tests below.
  - size: M
- [x] M318.P1.T2 — The CPU RoI and the GPU input RoI (`resampleGpuSourceRoI`) both use the bound
  - files: `Engine/Nodes/Transform/LensCorrect.cpp`
  - size: S
- [x] M318.P1.T3 — Tests: tiled vs whole frame bit-identical across 10 lens cases, filters, edge modes and mip; every read covered by the RoI; a 64x64 window asks for under 1/20 of the Source; GPU RoI equals CPU RoI with small-window parity
  - files: `Tests/Native/NativeLensCorrect_Test.cpp`, `Tests/Native/NativeLensCorrectGpu_Test.cpp`
  - verify: remote gate, both ctest legs.
  - size: M
