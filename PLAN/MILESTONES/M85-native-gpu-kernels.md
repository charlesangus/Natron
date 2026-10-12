# M85 - Native GPU Kernels

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Give native nodes GPU implementations on the M83 backend.
- Heavy nodes first, ranked by M82's profile (likely Blur, Defocus/Convolve, Transform with filtering, and the OCIO and colour pipeline).
- Then the cheap point ops (Grade, ColorCorrect, Shuffle, Copy, Premult/Unpremult, Clamp and the like) — mainly so they don't break a resident region.
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
- **Hand-offs from M84 - GPU Placement And Residency** (2026-10-11):
  - **Viewer:** wire `GlInterop` into `ViewerGL`, and add a GPU viewer-process kernel so a region's root output can stay on the device. Today the viewer downloads the root because display conversion runs on the CPU (`ViewerInstance.cpp` `scaleToTexture*`). Add double-buffering in `GlInterop`, deferred from the M83 review.
  - **Grade and Blur:** add mask, mix and premult wrapping to their GPU paths. M84 ships them without mask or mix, with no unpremult and FIR-only.
  - **Blur:** GPU IIR and box Blur, or a recorded decision to keep them CPU-only. Speed up the vertical pass.
  - **Per-kernel scratch:** M84 plans strips from I/O sizes only and handles kernel scratch reactively (evict → serial strips → halve → CPU). New kernels need no scratch declaration, but heavy-scratch kernels will hit that ladder on their first frame. Remember a strip height that worked per region shape.
  - **Contention:** two regions sharing the pool can halve each other into thin strips. Consider waiting briefly for the other region before halving.
  - **First-frame imports:** pinning fresh download destinations costs ~50 ms per 512 MB strip on the region thread. Import ahead of time on a helper thread, which needs `GpuTransfer::resolveImports` outside `importMutex`.
  - **CPU fallback:** a region that falls back runs on the CPU with thread budget 1, so it is slow for a big Blur. Give the fallback a real budget.
  - **Profiles:** members that fell back are still labelled `device:gpu`, and the region record has no fallback field.
  - **Single-node regions:** in keying_grade and cg_multipass they are mostly exposed transfer (~11 of ~12 ms). Porting the neighbouring keyers and morphology is what makes those comps resident; Merge's GPU path comes from M101 - Clean-Sheet Merge Node.
  - **`HostImageMemory`:** the pool can hold up to RAM/4 plus pinned imports while idle. Add an idle trim or a setting.
  - **Timeouts:** a `submitAndWait` timeout without a device loss can return buffers to the pool while the device may still use them.
  - **Per-member GPU time** is estimated from the cost model, not measured with per-member timestamps.

Blocked on: M84 - GPU Placement And Residency, and the ranked node list from M82 - Render Cost Profiling.

Acceptance sketch:
- Each ported node matches its CPU output within a stated tolerance on fixtures.
- A measured end-to-end speed-up on the M82 comps on a real GPU.
- Silent CPU fallback with no device.
- No host-side image operation renders through GL: the `eStorageModeGLTex` mask/mix,
  copy-channels, premult and convert paths are gone or reduced to the M86 interop boundary.
- The Preferences GPU setting lists Vulkan devices, not GL renderers.

## Decisions

- 2026-10-12 — **Merge is out of scope** (user): M101 - Clean-Sheet Merge Node replaces the old `MergeNode` with a new node that ships its own GPU kernel, so porting the old one would be throwaway work. Not a dependency: M85 runs without it.
