## Pending

### 2026-10-09T22:08:44-04:00 — change-request
- refs: M82, M83, M84, M85, M86, M87
- Add six new milestones (user-requested, from a 2026-10-09 side-session investigation of GPU rendering). Suggested board position: after M80 - Native RAW Support, with M82 and M83 ahead of M81 - Raw GPU Kernels (see the separate change-request about M80.P6.T1). Investigation findings that apply to all six:
  - **Current state.** GPU means OpenGL only, through the OFX GL suite. Only Shadertoy and the openfx-io OCIO plugins use it; no native node has a GPU path. In task-graph mode `RenderScheduler::executeTask` hard-codes `eStorageModeRAM` (`Engine/RenderScheduler.cpp:711`) and fails any task that leaks a texture or leaves a context bound (`:727-751`). Per-thread contexts don't share. So every GL node is an island: upload, render, then `glFinish` + `glReadPixels` back to RAM. The legacy path's GL-chain residency is lost. `renderRoI` refuses GL for multi-consumer nodes, time offsets and force-cache (`Engine/EffectInstanceRenderRoI.cpp:704-712`), because GL output is never cached. Textures are always RGBA32F. The M63 decision record's claim that "GL stays on the legacy path" is stale.
  - **Economics.** A 4K RGBA float frame is about 133 MB. A PCIe 4.0 round trip is about 11 ms (PCIe 5.0 about 5 ms). A point op is about 0.3–0.5 ms on GPU and 3–5 ms on a many-core CPU. One round trip costs as much as two or three CPU point ops, so per-node offload pays only for heavy nodes. The win is **residency**: keep connected GPU-capable regions on the device and pay transfers only at region boundaries. Fusion is second-order. Point ops need GPU versions mainly so they don't break a region. Masks do not block fusion: each mask is an extra per-pixel input, `v = lerp(v, op(v), m(x)·mix)`. Unified-memory machines (APUs) collapse the transfer cost, so the cost model must take bus bandwidth as a measured parameter (design-for-hardware-range rule).
  - **Prior art.** Blender's GPU compositor fuses pixel ops into one shader and uses a ref-counted texture pool, aiming for minimal CPU/GPU differences, not bit-exact. Houdini Copernicus is GPU-resident on OpenCL, and its VRAM exhaustion bugs show budgeting and eviction are needed from day one. darktable has a per-module CPU/GPU cost model and tiles on low VRAM. StarPU's dmda/dmdar does placement by exec+transfer cost with locality preference. Core Image fuses kernels across the graph. Nuke has no graph-level residency as of 17.
  - **Backend lean** (needs the user's confirmation in M83): Vulkan compute with kernels in Slang (Khronos; one source compiles to SPIR-V, CUDA, Metal and C++ for a CPU twin). Reasons: explicit memory, transfer queues, timeline semaphores, headless operation, float controls, external-memory interop with the GL viewer and OFX CUDA plugins. Flame and Houdini moved this way. Rejected: OpenGL (no explicit memory or queues, sync transfers); OpenCL (GEGL driver-stability history, flat vendor investment); CUDA as the core (vendor lock; fine as an optional OFX/ML path); WebGPU (weak precision guarantees); SYCL (heavy toolchain); Qt RHI (no transfer or memory control; keep it for the viewer). CPU/GPU parity is tolerance-based, never bit-exact.

  **New milestone file `PLAN/MILESTONES/M82-render-cost-profiling.md`:**

  # M82 - Render Cost Profiling

  Measure where render time actually goes on real comps, and what host↔GPU transfers cost on real hardware, before committing to any GPU architecture. The output is a go/no-go and a shape for M83 - GPU Compute Backend Spike and M84 - GPU Placement And Residency: do expensive nodes sit close enough to form resident regions, or are they isolated, in which case a few heavy nodes with async transfers is enough?

  ## Phase 82.1: Instrumentation

  - [ ] M82.P1.T1 — Record per-task render cost in the task-graph scheduler
    - files: `Engine/RenderScheduler.cpp`, `Engine/RenderScheduler.h`, a new `Tests/RenderProfile_Test.cpp`
    - approach: In `RenderScheduler::executeTask`, when a profiling switch is on (env var `NATRON_RENDER_PROFILE=<path>`; off by default, zero cost when off), record per task: plugin ID, node script name, time, view, mipmap level, RoI pixel count, components and bit depth, input-dependency task IDs, `estimatedBytes`, wall time of `renderRoI`, and whether the node is a pure point op (native `PixelKernel` effect) or reports GL support. Append records to a JSON-lines file per process, buffered and mutex-guarded so pool threads don't contend on I/O.
    - verify: a gtest renders a small Read → Grade → Blur → Merge graph with the variable set and checks one record per task with a non-zero time and correct dependency edges.
    - size: M
  - [ ] M82.P1.T2 — Region analysis report script
    - files: a new `tools/bench/render-profile-report.py`
    - approach: Read the JSON-lines output. Aggregate time per plugin. Mark nodes as heavy (above a configurable ms/Mpx threshold) or cheap. Find connected regions of "GPU-candidate" nodes (heavy nodes plus the cheap nodes linking them). For each region, compare its CPU time with its boundary bytes ÷ a bandwidth parameter (default 25 GB/s; unified memory as a second scenario). Print a per-comp summary: share of frame time in heavy nodes, number and size of regions, estimated net GPU gain.
    - verify: runs on M82.P1.T1's test output and on a synthetic file with a known answer.
    - size: M

  ## Phase 82.2: Measurement

  - [ ] M82.P2.T1 — Host↔GPU transfer microbenchmark
    - files: spike code under `build/` only; results into a `PLAN/DESIGN/` note
    - approach: Measure upload and download of a UHD RGBA float frame (and half, and 8-bit) through the existing GL PBO path, and through plain Vulkan with pinned staging if that's quick to stand up. Runs on the host GPU (the container has no usable GPU under Xvfb). Record PCIe generation and width.
    - verify: a design note with measured GB/s per direction and per path.
    - size: M
  - [ ] M82.P2.T2 — Profile representative comps
    - files: a `PLAN/DESIGN/` report
    - approach: Profile at least three comps with M82.P1.T1 and M82.P1.T2: a keying-and-grade comp, a CG multi-pass merge comp, and a defocus/retime-heavy comp. Use user-supplied comps if available, otherwise build plausible ones. The dev host is 4-core, so state the core count next to every number and prefer a faster host if one is available. Feed in M82.P2.T1's bandwidth.
    - verify: the user reviews the report and gives a go/no-go plus scope for M83 - GPU Compute Backend Spike.
    - size: M

  **Verification gate:** `ctest` green including `RenderProfile_Test`; profiling off by default with no measurable overhead; the transfer note and comp report exist in `PLAN/DESIGN/`; the user has given a go/no-go on M83 - GPU Compute Backend Spike.

  **New milestone file `PLAN/MILESTONES/M83-gpu-compute-backend-spike.md`:**

  # M83 - GPU Compute Backend Spike

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Pick the engine-wide GPU compute backend once, for native nodes, the task graph and M81 - Raw GPU Kernels. Leading candidate: Vulkan compute (VMA for allocation, a dedicated transfer queue, timeline semaphores) with kernels in Slang, whose C++ target doubles as the CPU twin for point and gather kernels. Prototype headless device init (no X, no EGL), one point kernel and one neighbourhood kernel (a separable blur), upload and download through pinned staging with overlap, and zero-copy hand-off to the Qt GL viewer through external memory. Compare against GL 4.3 compute on the existing context and OpenCL 3.0 on the same kernels. Measure, check CPU/GPU tolerance, and record build and deployment cost.

  Blocked on: M82 - Render Cost Profiling's go/no-go, and the user confirming the backend direction.

  Acceptance sketch:
  - A project-wide decision naming the backend, kernel language and CPU-fallback strategy, with measured numbers.
  - The prototype runs headless in the container (llvmpipe or lavapipe acceptable for correctness) and on the host GPU for speed.
  - CPU/GPU tolerance for the prototype kernels stated and met.

  **New milestone file `PLAN/MILESTONES/M84-gpu-placement-and-residency.md`:**

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

  **New milestone file `PLAN/MILESTONES/M85-native-gpu-kernels.md`:**

  # M85 - Native GPU Kernels

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Give native nodes GPU implementations on the M83 backend.
  - Heavy nodes first, ranked by M82's profile (likely Blur, Defocus/Convolve, Transform with filtering, and the OCIO and colour pipeline).
  - Then the cheap point ops (Grade, ColorCorrect, Merge, Shuffle, Copy, Premult/Unpremult, Clamp and the like) — mainly so they don't break a resident region.
  - Add a backend-neutral kernel description next to `makeKernel()` (`Engine/Nodes/Image/PixelKernel.h`). Mask, mix, process-channels and premult wrapping become per-pixel parts of each kernel.
  - Native nodes report GPU support (today `EffectInstance::supportsOpenGLRender` defaults to None for them, `EffectInstance.h:1772`).

  Blocked on: M84 - GPU Placement And Residency, and the ranked node list from M82 - Render Cost Profiling.

  Acceptance sketch:
  - Each ported node matches its CPU output within a stated tolerance on fixtures.
  - A measured end-to-end speed-up on the M82 comps on a real GPU.
  - Silent CPU fallback with no device.

  **New milestone file `PLAN/MILESTONES/M86-ofx-gpu-suites.md`:**

  # M86 - OFX 1.5 GPU Suites

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Host the OpenFX 1.5 GPU render suites, so third-party GPU plugins can join a resident region instead of forcing a round trip:
  - CUDA streams through Vulkan external-memory interop; OpenCL if the backend allows.
  - Pass the host queue or stream to the plugin, and don't wait for the plugin's work to finish.
  - Keep the existing OpenGL suite working through GL/Vulkan interop for Shadertoy and the OCIO plugins.

  Blocked on: M84 - GPU Placement And Residency.

  Acceptance sketch:
  - An OFX 1.5 CUDA or OpenCL sample plugin renders inside a resident region with no host round trip.
  - The existing GL plugins still render.

  **New milestone file `PLAN/MILESTONES/M87-point-op-kernel-fusion.md`:**

  # M87 - Point-Op Kernel Fusion

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Fuse connected regions of pixel-wise ops (including merges and masked ops with different masks: each mask becomes an extra per-pixel input, `v = lerp(v, op_i(v), m_i(x)·mix_i)`, and stages are skipped where their mask is 0) into one generated kernel on GPU, and optionally as a CPU row-strip pass. A deliberately low-priority optimization: point ops are memory-bound and cheap, so the gain is mostly memory traffic and `FrameStore` footprint.

  Blocked on: evidence from M82 - Render Cost Profiling or the M85 - Native GPU Kernels profiles that long point-op regions are common and fusion would measurably help.

  Acceptance sketch:
  - Fused output matches unfused output within tolerance.
  - A measured gain on a real comp justifies the complexity.

### 2026-10-09T22:08:44-04:00 — change-request
- refs: M80.P6.T1, M81
- Align the raw-kernel backend choice with the engine-wide GPU backend instead of choosing one only for raw kernels. Edit the approach of M80.P6.T1 (M80 - Native RAW Support): defer the decision to M83 - GPU Compute Backend Spike, or include Vulkan compute + Slang as a third option alongside OpenCL and GL shaders. The engine should end up with one GPU backend, not an OpenCL raw path plus a separate engine backend. darktable's OpenCL kernels are C-like and should port to Slang with modest effort. Add M83 - GPU Compute Backend Spike to M81 - Raw GPU Kernels's `Blocked on:` line. Ideally M81 also lands after M84 - GPU Placement And Residency, so raw kernels feed a resident region (demosaic → WB → highlights → denoise is exactly a chain that should stay on the device).

### 2026-10-09T22:08:44-04:00 — info
- refs: M68
- M68 - Headless GL EGL may shrink if M83 - GPU Compute Backend Spike picks Vulkan, which is headless by design. Headless GL is still needed for the existing OFX GL plugins (Shadertoy, OCIO) in background renders and for GL test coverage in CI, so don't cancel it. Re-scope it when M83 lands. No plan edit needed now; still deferred pending the user's go-ahead.
