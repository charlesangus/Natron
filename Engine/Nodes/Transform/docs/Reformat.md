# Reformat

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Reformat | resampling | `kReformatTolerance` | |

GPU tolerance: `resampling()` (1e-5 absolute) for every resize type, centre, flip, flop and turn, every filter with clamp and black outside on and off, preserveBB on and off, at mipmap 0 and 1. The matrix is the CPU's, built on the host. A transform concatenated from upstream stays on the CPU (`Tests/Native/NativeReformatGpu_Test.cpp`).
