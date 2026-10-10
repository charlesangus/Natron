# M87 - Point-Op Kernel Fusion

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Fuse connected regions of pixel-wise ops (including merges and masked ops with different masks: each mask becomes an extra per-pixel input, `v = lerp(v, op_i(v), m_i(x)·mix_i)`, and stages are skipped where their mask is 0) into one generated kernel on GPU, and optionally as a CPU row-strip pass. A deliberately low-priority optimization: point ops are memory-bound and cheap, so the gain is mostly memory traffic and `FrameStore` footprint.

Blocked on: evidence from M82 - Render Cost Profiling or the M85 - Native GPU Kernels profiles that long point-op regions are common and fusion would measurably help.

Acceptance sketch:
- Fused output matches unfused output within tolerance.
- A measured gain on a real comp justifies the complexity.
