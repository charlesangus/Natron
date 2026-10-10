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

Design input: `PLAN/DESIGN/2026-10-10-gpu-compute-backend.md`, from M83 - GPU Compute Backend Spike. It covers the API sketch (§5), the hard limits (§4) and the promotion plan (§7). One of those limits overrides the "fine tiling stays a CPU concern" bullet above: frames over `maxStorageBufferRange` (4.29 GB on RADV) must be tiled on the GPU.

Blocked on: the user's sign-off on that design note. The backend is decided (Vulkan compute, offline Slang → SPIR-V, native CPU fallback, exported-buffer PBO interop; `DECISIONS/2026-10-10-gpu-compute-backend-chosen.md`).

Acceptance sketch:
- A chain of GPU-capable nodes renders with transfers only at the region boundary (asserted by transfer counters in a test).
- Multi-consumer GPU nodes render once.
- The VRAM budget is respected under a large comp, with eviction and no failure.
- Placement adapts when the bandwidth parameter models unified memory.
- No regression in CPU-only renders.
