# M86 - OFX 1.5 GPU Suites

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Host the OpenFX 1.5 GPU render suites, so third-party GPU plugins can join a resident region instead of forcing a round trip:
- CUDA streams through Vulkan external-memory interop; OpenCL if the backend allows.
- Pass the host queue or stream to the plugin, and don't wait for the plugin's work to finish.
- Keep the existing OpenGL suite working for Shadertoy and the OCIO plugins, as the **only
  remaining engine GL path** (2026-10-11, user). A GL-suite plugin's inputs and outputs
  cross a GL/Vulkan interop boundary at the plugin call (exported Vulkan memory imported
  as GL textures or PBOs, ordered by shared semaphores with the hostsync fallback from
  M83). Shrink `OSGLContext*` and `GPUContextPool` to lazily created contexts used only for
  those plugins — none are created when a project has no GL-suite plugin — and delete the
  GL scheduling plumbing in `RenderScheduler.cpp`, `FrameRenderContext.cpp` and
  `ParallelRenderArgs.*` that no longer has a consumer. Rework or retire
  `Tests/GLScheduler_Test.cpp` and `Tests/OSGLContext_Test.cpp` to match. M68 - Headless
  GL EGL then reduces to giving this narrowed context an EGL surfaceless backend.
- **Hand-offs from M84 - GPU Placement And Residency** (2026-10-11):
  - **Exportable buffer pool:** `GpuBufferPool` hands out plain device buffers. CUDA/OpenCL interop needs a pool variant whose allocations are exportable (`VK_KHR_external_memory_fd`) and imported once per buffer on the plugin's API side, recycled like the M84 pool.
  - **Out-of-pool memory:** plugin allocations are invisible to the pool. M84 plans strips against the pool budget capped at device memory − 1 GiB (`kGpuStripOutsidePoolHeadroom`) for that reason. Measure whether that headroom holds with real GPU plugins.
  - **Reactive admission covers plugins too:** an out-of-memory raised by a plugin's render should map to the same ladder (evict → serial strips → halve → CPU).

Blocked on: M84 - GPU Placement And Residency.

Acceptance sketch:
- An OFX 1.5 CUDA or OpenCL sample plugin renders inside a resident region with no host round trip.
- The existing GL plugins still render.
- With no GL-suite plugin in the project, a render creates no GL context.
- GL is used in the engine only at the OFX OpenGL-suite boundary; everything else on the
  GPU runs on Vulkan.
