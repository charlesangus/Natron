# M81 - Raw GPU Kernels

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Ports the GPU variants of the M80 - Native RAW Support nodes that M80 leaves on the CPU: demosaic AMaZE (darktable has no OpenCL for it, so it is written from scratch), Markesteijn and VNG (X-Trans), hot pixels, the CA-correct apply step (shift estimation stays on the CPU), and raw denoise. Kernels are Slang on the Vulkan backend chosen by M83 - GPU Compute Backend Spike, and they build on M80's raw GPU region plumbing (M80.P6.T2). The core chain's kernels (Prepare, Temperature, Highlights, PPG, RCD, InputColor) already ship in M80 - Native RAW Support. Lens correction's GPU path is in M160 - Lensfun Lens Correction. CPU stays the default and the reference. The GPU result must match it within a tolerance, and a machine with no usable device falls back silently.

Blocked on: M80 - Native RAW Support finishing. These kernels port its CPU nodes and reuse its Phase 80.6 raw-region plumbing and parity pattern, so the briefs can't name real files until that lands.

Acceptance sketch:
- Each remaining module has a GPU path selectable per node and by the project GPU setting, falling back to CPU on failure.
- GPU and CPU outputs agree within a stated tolerance on the M80 fixtures.
- A measured speed-up on Markesteijn and AMaZE on a real GPU.
