# M85 - Native GPU Kernels

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Give native nodes GPU implementations on the M83 backend.
- Heavy nodes first, ranked by M82's profile (likely Blur, Defocus/Convolve, Transform with filtering, and the OCIO and colour pipeline).
- Then the cheap point ops (Grade, ColorCorrect, Merge, Shuffle, Copy, Premult/Unpremult, Clamp and the like) — mainly so they don't break a resident region.
- Add a backend-neutral kernel description next to `makeKernel()` (`Engine/Nodes/Image/PixelKernel.h`). Mask, mix, process-channels and premult wrapping become per-pixel parts of each kernel.
- Native nodes report GPU support (today `EffectInstance::supportsOpenGLRender` defaults to None for them, `EffectInstance.h:1772`).
- **Retire the engine GL render path for host work** (2026-10-11, user). The host's GL
  wrappers around plugin renders — mask/mix, copy-channels, premult and storage conversion
  on `eStorageModeGLTex` images (`ImageMaskMix.cpp`, `ImageCopyChannels.cpp`,
  `ImagePremult.cpp`, `ImageConvert.cpp`, GL paths in `Image.cpp`, `GLShader.*`) — are
  replaced by the backend-neutral kernels above running on Vulkan. GPU device selection
  and the on/off switch in `Settings.cpp` move from the GL renderer list to the Vulkan
  device list. After M85 nothing native or host-side renders through GL; the remaining GL
  engine code serves only the OFX OpenGL suite and is narrowed by M86.

Blocked on: M84 - GPU Placement And Residency, and the ranked node list from M82 - Render Cost Profiling.

Acceptance sketch:
- Each ported node matches its CPU output within a stated tolerance on fixtures.
- A measured end-to-end speed-up on the M82 comps on a real GPU.
- Silent CPU fallback with no device.
- No host-side image operation renders through GL: the `eStorageModeGLTex` mask/mix,
  copy-channels, premult and convert paths are gone or reduced to the M86 interop boundary.
- The Preferences GPU setting lists Vulkan devices, not GL renderers.
