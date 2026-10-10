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
