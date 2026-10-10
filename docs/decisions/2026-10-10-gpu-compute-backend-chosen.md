# GPU compute backend chosen: Vulkan compute, offline Slang, native CPU fallback, PBO interop

2026-10-10, proposed by M83 - GPU Compute Backend Spike, pending the user's sign-off. Natron's single engine-wide GPU backend is headless **Vulkan 1.3 compute** with VMA. Kernels are written in **Slang and compiled offline to embedded SPIR-V**; no Slang ships at runtime, so the only runtime dependency is the system `libvulkan.so.1`. **The hand-written CPU kernels stay the primary CPU path and fallback**, and the Slang C++ twin is a test oracle only. **The viewer hand-off** exports a Vulkan buffer, imports it as a GL PBO and uses `glTexSubImage2D`, ordered by shared semaphores. It falls back first to hostsync (fence + `glFinish`), then to readback + PBO.

Rationale, measured on the RX 7900 XTX (RADV, Mesa 25.0.7) and the Ryzen 7800X3D (16 threads):
- Kept-resident chains run up to 9.5× faster end to end than the CPU at 24k. Isolated point ops gain only about 3×, because they are transfer-bound, which supports residency from day one.
- Host-import gives about 25 GB/s round trips, against 13.6 GB/s through staging.
- The zero-copy hand-off takes 0.79 ms at UHD, against 33.2 ms for readback.
- GL 4.3 compute only matched Vulkan, and would cost a glad regeneration plus 4.3 contexts.
- The C++ twin is 14–31× slower than native FIR Blur.
- Grade and Blur meet their tolerances on RADV and lavapipe.

M84 - GPU Placement And Residency must design around these hard limits: frames over 4.29 GB must be tiled on the GPU, and copies must be chunked. See `PLAN/DESIGN/2026-10-10-gpu-compute-backend.md`.
