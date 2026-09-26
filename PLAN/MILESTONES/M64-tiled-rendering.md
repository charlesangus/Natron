# Milestone 64: Tiled / fused rendering for bandwidth-bound chains

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

At HD, node-at-a-time rendering writes and re-reads a full frame per node. `tools/bench/stream_bench.c` measured the node-at-a-time vs tiled vs fused cost of a chain of point ops on this machine (~7 GB/s, 6 MB L3): a chain of point operations fits a tile in cache and approaches the ~9 ms/node bandwidth floor only when intermediate images don't round-trip through memory. This milestone would let the M63 scheduler execute a chain of tile-capable nodes tile by tile (each tile flowing through several nodes while it stays in cache), using OFX `kOfxImageEffectPropSupportsTiles` and the RoI machinery that already exists. It would also cut peak memory for long chains (HD comp 300 peaked at 4.4 GB).

Blocked on: M63 shipping (tiles are a scheduling policy on top of the task graph), and on M62/M63's benchmarks showing that plugin-side per-pixel cost, not host overhead or occupancy, is what remains. If per-node plugin compute dominates bandwidth after M63, tiling buys little and this milestone should be cancelled.

Acceptance sketch:
- HD chain 30/100 of tile-capable point ops renders at least 1.5x faster than after M63, with lower peak RSS.
- Non-tile-capable plugins and deep/3D data kinds fall back to whole-image rendering with identical pixels.
- Cache semantics (which intermediate images are kept) stay correct and are covered by tests.
