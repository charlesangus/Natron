# M84 - GPU Placement And Residency

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Teach the task-graph scheduler to place work on CPU or GPU, and keep GPU results resident between tasks. Hook points:
- A placement pass after `RenderScheduler::buildGraph` (`Engine/RenderScheduler.cpp:238-366`), adding `device` and `regionId` fields to `FrameGraph::Task` (`RenderScheduler.h:59-98`). This moves the CPU/GPU decision out of `renderRoI` (`EffectInstanceRenderRoI.cpp:694-786`).
- A StarPU-style cost model: measured per-plugin CPU and GPU cost (from M82's instrumentation) plus calibrated bus bandwidth. Accept a connected region only when the CPU time it saves exceeds its boundary transfer cost.
- Per-task output storage instead of the hard-coded RAM (`RenderScheduler.cpp:711`).
- A `FrameStore` that can hold device images, so multi-consumer nodes can stay on the GPU (drops the visits>1 refusal).
- Run each resident region as one super-task, so context or queue affinity isn't a problem.
- A ref-counted device memory pool with a VRAM budget and LRU eviction to RAM, with GPU bytes counted separately in admission (`admitGatedLocked`, `bytesInFlightLocked`).
- Pinned staging buffers with async upload and readback overlapping compute.
- Full-frame or region-of-interest GPU units (fine tiling stays a CPU concern).
- GPU tasks get a thread budget of 1.
- Graceful fallback to CPU on out-of-memory or device loss.
- Fix the stale GL statement in the M63 decision record.

Blocked on: M83 - GPU Compute Backend Spike's backend decision.

Acceptance sketch:
- A chain of GPU-capable nodes renders with transfers only at the region boundary (asserted by transfer counters in a test).
- Multi-consumer GPU nodes render once.
- The VRAM budget is respected under a large comp, with eviction and no failure.
- Placement adapts when the bandwidth parameter models unified memory.
- No regression in CPU-only renders.
