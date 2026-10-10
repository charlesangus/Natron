# M81 - Raw GPU Kernels

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Ports the GPU variants of the M80 - Native RAW Support nodes: darktable's OpenCL kernels for demosaic (PPG, RCD, AMaZE, Markesteijn), white balance, highlights, hot pixels, denoise and lens correction, onto the backend M80's spike chooses. CPU stays the default and the reference; the GPU result must match it within a tolerance, and a machine with no usable device falls back silently.

Blocked on: M80 - Native RAW Support finishing (including the raw-kernel requirements note, M80.P6.T1), the engine-wide backend decision in M83 - GPU Compute Backend Spike and the user's confirmation of it, and ideally M84 - GPU Placement And Residency, so demosaic → white balance → highlights → denoise runs as one resident region.

Acceptance sketch:
- Each ported module has a GPU path selectable per node and by the project GPU setting, falling back to CPU on failure.
- GPU and CPU outputs agree within a stated tolerance on the M80 fixtures.
- A measured speed-up on at least the demosaic step on a real GPU.
