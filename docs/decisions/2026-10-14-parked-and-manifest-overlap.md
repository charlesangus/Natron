# Parked milestones and shared manifests don't block claims

Two relaxations of the PLAN-FORMAT §8a file-overlap test, which the user chose on 2026-10-14 when claiming M90 - Render Path Serial Overhead and M100 - Content Node Hashing alongside M85 - Native GPU Kernels:

- **A blocked milestone with no owner (currently M64 - Tiled Rendering) is ignored for file overlap.** It is parked with nobody working it, and waiting on it would stall every render-path milestone indefinitely. If it resumes, it rebases onto whatever has landed by then.
- **Shared manifests and shared directories don't count as overlap**, for example `Tests/CMakeLists.txt`, `Engine/Nodes/README.md`, `Tests/Native/` and `tools/bench/`. Conflicts there are rebase-level and get resolved at rebase. A real shared source file still counts. For M90, that means its `Blur.cpp` and `GpuCostSeed.inc` regeneration tasks run last, after M85 - Native GPU Kernels merges, while the rest of M90 proceeds now.
