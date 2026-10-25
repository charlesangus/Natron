# M87 - Point-Op Kernel Fusion

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Fuse connected regions of pixel-wise ops (including merges and masked ops with different masks: each mask becomes an extra per-pixel input, `v = lerp(v, op_i(v), m_i(x)·mix_i)`, and stages are skipped where their mask is 0) into one generated kernel on GPU, and optionally as a CPU row-strip pass. A deliberately low-priority optimization: point ops are memory-bound and cheap, so the gain is mostly memory traffic and `FrameStore` footprint.

Blocked on: evidence from M82 - Render Cost Profiling or the M85 - Native GPU Kernels profiles that long point-op regions are common and fusion would measurably help.

Acceptance sketch:
- Fused output matches unfused output within tolerance.
- A measured gain on a real comp justifies the complexity.

## Evidence from M85 - Native GPU Kernels (2026-10-15)

Point-op runs inside GPU regions on the three M82 comps (UHD, Auto; a run is a chain of point ops, generators included, each feeding only the next):

| comp | regions | point-op runs by length |
|---|---|---|
| keying_grade | 6 | 1 ×1, 2 ×1, 4 ×1 |
| cg_multipass | 9 | 1 ×8, 2 ×3 |
| defocus_retime | 9 | 1 ×8, 2 ×1 |

- That is 23 runs. 17 are a single op, so there is nothing to fuse. Fusing the rest would save about 7 dispatches per frame.
- A point-op dispatch costs ~0.5 ms at UHD (M85 cost seeds), so fusion saves ~3.5 ms of 340–880 ms frames.
- Exposed transfer is 59–84% of region time in these comps, and it is M109 - GPU Region Engine Hardening's to fix.
- **On this evidence fusion would not measurably help the M82 comps.** It stays low priority until a comp with long point-op chains turns up.

## Decisions

- 2026-10-25 — **Cancelled (user).** The gpu PM claimed it for a free lane, then found that its own evidence section answers its blocker negatively: 17 of 23 point-op runs in the M82 comps are single ops, and fusing the rest saves ~3.5 ms of 340–880 ms frames. The user chose to cancel rather than park it. If a comp with long point-op chains turns up, a new milestone can reuse this file's design.
