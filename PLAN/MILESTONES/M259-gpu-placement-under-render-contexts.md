# M259 - GPU Placement Under Render Contexts

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Split out of M180 - Loop Block Core (2026-10-27). M178 - Render Context Core kept every task under a non-empty render context on the CPU "for now" (`Engine/GpuPlacement.cpp:601`, `:707` on M180's branch), and GPU regions (`GpuRegionTask`) don't carry the context-request edges between tasks. So loop iterations, LoopEnd's fold-chain steps and anything under an RFV Set can't run on the GPU, and a loop accumulator can't stay resident between steps.

Scope sketch:
- Let GpuPlacement place context-carrying tasks: placement keys and residency pools keyed by (node, context key), as the frame store already is.
- Carry context-request edges (M178 cross-context edges, LoopEnd's step k ← step k−1) inside GPU regions, so a fold chain can stay on the device.
- M180.P2.T2, moved here: one GPU point kernel per fold operation (mean, sum, min, max, over, under; reverse order swaps direction), with the accumulator resident across steps (M84 - GPU Placement And Residency pools), and CPU/GPU parity tests.
- Check against the hardware range: many contexts must not exhaust device memory (bytes budget, eviction to host).
