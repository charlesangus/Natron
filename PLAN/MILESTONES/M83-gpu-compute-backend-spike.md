# M83 - GPU Compute Backend Spike

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Pick the engine-wide GPU compute backend once, for native nodes, the task graph and M81 - Raw GPU Kernels. Leading candidate: Vulkan compute (VMA for allocation, a dedicated transfer queue, timeline semaphores) with kernels in Slang, whose C++ target doubles as the CPU twin for point and gather kernels. Prototype headless device init (no X, no EGL), one point kernel and one neighbourhood kernel (a separable blur), upload and download through pinned staging with overlap, and zero-copy hand-off to the Qt GL viewer through external memory. Compare against GL 4.3 compute on the existing context and OpenCL 3.0 on the same kernels. Measure, check CPU/GPU tolerance, and record build and deployment cost.

Blocked on: M82 - Render Cost Profiling's go/no-go, and the user confirming the backend direction.

Acceptance sketch:
- A project-wide decision naming the backend, kernel language and CPU-fallback strategy, with measured numbers.
- The prototype runs headless in the container (llvmpipe or lavapipe acceptable for correctness) and on the host GPU for speed.
- CPU/GPU tolerance for the prototype kernels stated and met.
