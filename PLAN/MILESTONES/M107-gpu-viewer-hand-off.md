# M107 - GPU Viewer Hand-off

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Let a region's root output stay on the device all the way to the screen:
- Wire `GlInterop` (`Engine/Gpu/GlInterop.h`) into `ViewerGL::transferBufferFromRAMtoGPU` (`Gui/ViewerGL.cpp:1557`), passing `QOpenGLContext::getProcAddress`. The `ViewerGL` PBO path then binds the interop buffer instead.
- Add double-buffering in `GlInterop`, deferred from the M83 review.
- Add a GPU viewer-process kernel to replace `scaleToTexture8bits`/`32bits` (`Engine/ViewerInstance.cpp:107/112`, called at `:2137/2140`): gain, gamma, channel display, matte overlay, the display LUT and dithering for 8-bit.

The viewer then stops downloading the root, and the hostsync and readback fallbacks remain.

Blocked on:
- how the OCIO display transform runs on Vulkan (the user decides at promotion, 2026-10-12) without runtime shader compilation: a baked 3D LUT plus shaper in a fixed Slang kernel, or a fixed set of OCIO ops ported to Slang. This needs a short spike, and the user's choice on the precision trade-off;
- M85 merged (the `pixelwrap.slang` conventions and the registry).

Acceptance sketch:
- On RADV, a resident comp displays with no device-to-host download of the root; the profile shows 0 root downloads.
- The viewer's GPU output matches the CPU display path within a stated tolerance (8-bit: ±1 code value) for every display mode.
- Without interop (lavapipe, CI) the viewer falls back silently and displays correctly.

## Decisions

- 2026-10-18 — **The viewer's OCIO display transform on Vulkan compiles OCIO's own generated GLSL at runtime** (user, at promotion), through M126 - Runtime GPU Shaders' pipeline cache. This matches the M127 - Native OCIO Nodes decision and supersedes the stub's "without runtime shader compilation" options, a baked 3D LUT plus shaper or ported OCIO ops. If the compile fails, the viewer falls back to the CPU display path.
- 2026-10-18 — **Claimed by the gpu PM as the second lane** beside M109 - GPU Region Engine Hardening (user). M108 - Retire Engine GL Host Path would collide with M100 - Content Node Hashing on `Engine/EffectInstance.cpp`. M87 - Point-Op Kernel Fusion would make M109 wait and has little measured gain. M91 - NVIDIA GPU Validation has no host.
