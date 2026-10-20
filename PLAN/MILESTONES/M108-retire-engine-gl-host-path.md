# M108 - Retire Engine GL Host Path

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Nothing native or host-side renders through GL any more:
- **Host wrappers:** the host's wrappers around OFX GL-suite renders stop running on `eStorageModeGLTex` images: `Image::applyMaskMix` (`ImageMaskMix.cpp:216`), `copyUnProcessedChannels` (`ImageCopyChannels.cpp:483`), the premult (`ImagePremult.cpp:103`), convert, and the `fillZero` GL path (`Image.cpp:1445`), called from `EffectInstance.cpp:3278/3281`. A GL-suite plugin's output crosses to RAM (or to Vulkan, once M86 adds interop) right after the plugin call. The wrappers then run on the CPU or as `pixelwrap` kernels.
- **Shaders:** delete the mask-mix, copy-channels and fill shaders (`OSGLContext.cpp:509-645`), and with them `GLShader.*` where nothing else uses it.
- **Preferences:** the GL renderer chooser, the context count and the "OpenGL Rendering" switch (`Settings.cpp:556-600`) are replaced by a GPU page. It lists Vulkan devices (enumerated, replacing the free-text `gpuDevice` at `:425`), with the compute mode and budget moved from the Threading page. The OFX GL suite follows the compute mode and picks the GL renderer that matches the chosen Vulkan device's UUID. Hard cut: old settings are ignored.

Blocked on:
- deciding with M86 - OFX 1.5 GPU Suites' elaboration where the GL/Vulkan boundary at the plugin call lives, since both edit the same GL branch of `EffectInstance::renderHandler`. The user chose (2026-10-12) to keep this separate from M86 and run it before M86.
- M85 merged, if the wrappers are to run as Vulkan kernels.

Acceptance sketch:
- `grep` finds no GL-texture branch in `ImageMaskMix.cpp`, `ImageCopyChannels.cpp`, `ImagePremult.cpp` or `ImageConvert.cpp`, and no mask-mix or copy-channels shader.
- A GL-suite OFX plugin with mask, mix and a channel subset matches its CPU render.
- Preferences lists Vulkan devices, not GL renderers, and choosing one takes effect at the next launch.

## Decisions

- 2026-10-20 — **Claimed by the gpu PM as the third lane, overlap with M64 accepted (user).** M108 will touch `Engine/Settings.cpp` and `Engine/EffectInstance.cpp`, which M64 - Tiled Rendering (blocked, dropped for now) also lists. The user chose to run M108 anyway; M64 rebases over it if it resumes. The M86 boundary blocker resolves as the stub already says: M108 runs first, and a GL-suite plugin's output crosses to RAM right after the plugin call, leaving Vulkan interop to M86 - OFX 1.5 GPU Suites. M85 is merged, so the wrappers may run as `pixelwrap` kernels.
