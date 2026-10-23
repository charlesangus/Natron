## Pending

### 2026-10-22T08:00:00Z — change-request
- refs: none
- New milestone requested (user, while scoping M86 - OFX 1.5 GPU Suites to the GL suite only): **"OFX CUDA And OpenCL Suites"**, epic E5 - GPU Compute, Deps M86. Also add it to M116 - GPU Region Overlay's Deps. Suggested stub:

  > Host the OpenFX 1.5 CUDA and OpenCL render suites, so third-party compute plugins join a resident region with no host round trip. Port OFX 1.5's `ofxGPURender.h` properties into the `libs/OpenFX` fork: CUDA stream support, OpenCL buffers and images, the optional `OfxOpenCLProgramSuiteV1`, and `kOfxStatGPUOutOfMemory`. Extend HostSupport's render in-args with the CUDA stream. Advertise each suite only when its backend is live. Import exported Vulkan memory and semaphores, through a `dlopen`ed `libcuda.so.1` (driver API: `cuImportExternalMemory`/`cuImportExternalSemaphore`/`cuWait|SignalExternalSemaphoresAsync`) or through OpenCL `cl_khr_external_memory_opaque_fd` + `cl_khr_external_semaphore_opaque_fd`, with the device matched by `cl_khr_device_uuid`. Hand the plugin `kOfxImagePropData` as a device pointer or `cl_mem`, pass the host stream or queue, and never wait for the plugin's work. Reuse M86's region boundary. Carries M84's hand-offs: an exportable `GpuBufferPool` variant, imported once per buffer and recycled; out-of-pool plugin memory, checked against the 1 GiB `kGpuStripOutsidePoolHeadroom` (moved here from M86); `kOfxStatGPUOutOfMemory` mapped onto the evict → serial strips → halve → CPU ladder.
  >
  > Blocked on: M86 - OFX 1.5 GPU Suites (the region boundary) and its P1.T1 spike (which compute runtimes and extensions exist on the dev host and CI); user calls on CUDA-without-NVIDIA (consultant recommends: build it, test without a driver, validate on M91's host) and on a staged OpenCL fallback (consultant recommends: zero-copy where available plus a staged RAM path outside regions, tested on CI with EPEL 9's pocl 3.1); end-to-end CUDA validation needs M91 - NVIDIA GPU Validation's host.
  >
  > Acceptance sketch: an OFX 1.5 OpenCL sample plugin (`libs/OpenFX/Examples/OpenCL`) renders inside a resident region with no host round trip wherever a zero-copy OpenCL runtime exists, and elsewhere follows the agreed fallback; the CUDA path builds in CI, is unit-tested without a driver (clean unavailability, suite not advertised), and is validated end to end on M91's NVIDIA host; a plugin returning `kOfxStatGPUOutOfMemory` takes the out-of-memory ladder, and out-of-pool memory with real plugins is measured against the headroom.

### 2026-10-22T12:00:00Z — info
- refs: none
- Pre-existing bug on main, found by M154 - Fresh Buffer Page Zeroing's first `-DNATRON_GPU=OFF` CI build (run 39770415588) and outside its scope: `StagedImageEffectGpuTest` fails in a GPU-off build because `StagedImageEffect::canRenderOnGpu` returns true for 1–4 components and for the mask/mix/channel-subset query, where the test (`Tests/Native/StagedImageEffectGpu_Test.cpp:511, 528`) expects false without `NATRON_HAVE_GPU`. Either the effect should refuse GPU without `NATRON_HAVE_GPU`, or the test's expectation is wrong. Both came from M137. Repro: `tools/ci/remote/build.sh fast --tests --no-appimage --cmake-arg -DNATRON_GPU=OFF` (available once M154 merges).

### 2026-10-22T19:30:00Z — change-request
- refs: none
- New milestone requested (user, 2026-10-22): **"GL Context Restore On GUI Thread"**, a bug fix, deps `-`. An AppImage of M158 - Node Toolbar As Menu (`8570052b4`, fast preset) segfaulted at startup on an NVIDIA RTX 3070 Ti (proprietary driver, X11). Log: Qt's `QBackingStoreDefaultCompositor` printed "Failed to compile shader" for its `#version 150` shader with an empty info log, then "Failed to build graphics pipeline", then a SIGSEGV in `QPlatformBackingStore::rhiFlush` during the first expose. It is **intermittent**: the same AppImage later launched fine, as did main's beta43 release. Llvmpipe/Xvfb never shows it, because the GL suite is off there.

  > Likely mechanism (diagnosed by reading code; not reproduced): Natron releases the main thread's GL context with a raw `glXMakeCurrent(dpy, None, NULL)` through `OSGLContext_x11::makeContextCurrent(nullptr)` (`Engine/OSGLContext_x11.cpp:~872`) without restoring the previous one. Qt 6 skips `makeCurrent` when it believes its own compositor context is still current, so its shader compile runs with no context bound: `glCreateShader` returns 0 and the log is empty. Unguarded release sites on the main thread:
  > - the one-time `GL_MAX_TEXTURE_SIZE` probe in `Engine/GPUContextPool.cpp:256-259` (no `threadHasACurrentContext()` guard, unlike `:305`)
  > - `Engine/RenderScheduler.cpp:1218`
  > - `EffectInstance::dettachAllOpenGLContexts` (`Engine/EffectInstance.cpp:~4625`)
  > - the `OSGLContext` and `OSGLContext_x11` destructors
  >
  > Timing (when a viewer or preview render first attaches on the main thread relative to the first expose) would explain why it's intermittent.
  >
  > Suggested fix:
  > 1. Seed the pool's texture limit from `OpenGLRendererInfo::maxTextureSize`, which `getGPUInfos` already gathers, and delete the probe's bind/unbind.
  > 2. Make Natron's GLX bind and release save the thread's previous display, drawables and context, and restore them instead of releasing to `None`.
  >
  > Workaround: `NATRON_GPU=off`.
  >
  > Acceptance sketch: after any Natron GL attach and release on the GUI thread, a test checks that `QOpenGLContext::currentContext()` and the GLX current context are what they were before; repeated launches on the NVIDIA desktop never hit the compositor failure.

### 2026-10-23T00:15:41+00:00 — change-request
- refs: M29
- This is defunct now, cancel it. I was able to self serve leaving the fork network and this has been done.
