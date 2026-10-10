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
