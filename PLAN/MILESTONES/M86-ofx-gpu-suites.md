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

Blocked on: M84 - GPU Placement And Residency.

Acceptance sketch:
- An OFX 1.5 CUDA or OpenCL sample plugin renders inside a resident region with no host round trip.
- The existing GL plugins still render.
- With no GL-suite plugin in the project, a render creates no GL context.
- GL is used in the engine only at the OFX OpenGL-suite boundary; everything else on the
  GPU runs on Vulkan.
