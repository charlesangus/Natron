# M86 - OFX 1.5 GPU Suites

Make the OFX OpenGL suite the engine's only GL path, and let GL-suite plugins (bundled Shadertoy and the OCIO plugins) join resident Vulkan regions, so a chain like native GPU node → OCIOColorSpace → native GPU node runs without going through host memory. Render GL contexts become per-thread and are created only when a GL-suite plugin actually renders through GL. The shared render pool and the frame-level GL plumbing in `ParallelRenderArgs`, `FrameRenderContext.cpp`, `ViewerInstance.cpp` and `RenderScheduler.cpp` are deleted. On the plain CPU path, GL-suite output, which M108 made a RAM image, is now cached and rendered over the requested window only. Inside a region, a GL-suite member's inputs and output cross the GL/Vulkan boundary at the plugin call. They go through `GlInterop` buffers that Vulkan exports and GL imports, ordered by shared semaphores, with the hostsync fallback. The OFX 1.5 CUDA and OpenCL suites, and the M84 hand-offs they need (exportable pool, out-of-pool memory, `kOfxStatGPUOutOfMemory`), move to a follow-up milestone proposed through the inbox ("OFX CUDA And OpenCL Suites"). Phase 86.1 runs the compute-API availability spike that its elaboration needs.

## Scope

**In:**
- Render GL contexts come only from `GPUContextPool::getOrCreateContextForCurrentThread()`, on both scheduler modes. `attachGLContextToRender`, `releaseGLContextFromRender`, `NATRON_RENDER_SHARED_CONTEXT` and the shared pool are deleted. A `getNumContextsCreated()` counter proves that a project with no GL-suite plugin creates no render context.
- The frame-level GL plumbing is deleted: `ParallelRenderArgs::openGLContext`, the setter's attach and release, the `OSGLContextPtr` parameter of `buildArgsMap`/`setParallelRenderArgsTLS` and its callers, and the scheduler's GL-texture-plane check.
- On the CPU path, a GL-suite render's RAM output is cached like any other, and renders only the requested window. The max-texture-size fallback stays.
- `GlInterop` gains a GL→Vulkan direction (download and acquire) beside the existing Vulkan→GL one.
- New `Engine/GlBoundary.{h,cpp}`:
  - It holds the per-thread-context interop resources.
  - It answers `availability()` without creating a context, by comparing the picked GL renderer's device UUID with the Vulkan device's UUID and checking for latched failures.
  - It logs the rung once per session.
  - `NATRON_GL_BOUNDARY=0` turns it off, for A/B runs.
- `OfxEffectInstance::renderGlAtBoundary` renders one plane from given input textures into a given output texture. `OfxClipInstance::loadTexture` serves those textures from TLS.
- Placement offers eligible GL-suite OFX tasks as region members. The cost model prices them by the transfers they save. The region body runs them across the boundary, and maps `kOfxStatGLOutOfMemory` onto the existing out-of-memory ladder.
- The `GLScheduler_Test`/`OSGLContext_Test` rework and new `GlBoundary_Test`/`GlRegionMember_Test`.

**Out:**
- The OFX 1.5 CUDA, OpenCL and Metal suites, porting `ofxGPURender.h`/HostSupport into the `libs/OpenFX` fork, the exportable buffer pool, device-backed `OfxImage`, and measuring out-of-pool memory for compute plugins (the 1 GiB `kGpuStripOutsidePoolHeadroom`). These belong to the proposed "OFX CUDA And OpenCL Suites" milestone.
- A headless EGL backend for `OSGLContext`, which would give the engine zero-copy GL under ctest. This belongs to M68 - Headless GL EGL.
- A desktop-session proof of the radeonsi zero-copy rung for GL plugins. The user accepted the hostsync rung under Xvfb plus `GlInterop`'s own radeonsi ctest; the full radeonsi check comes with M68.
- Deferring the startup GL probe context (`AppManager::initializeOpenGLFunctionsOnce`, run before `QApplication` because of the X11 `XUniqueContext` issue) and M108's per-renderer UUID probe. They still create and destroy contexts at launch. "No GL context" means no render context.
- Host mask, mix, copy-unprocessed-channels and "(Un)premult by" for a GL-suite member inside a region. Such tasks are not region candidates and stay on M108's CPU path. No bundled plugin uses them.
- NVIDIA validation of the boundary. This belongs to M91 - NVIDIA GPU Validation.
- Viewer and GUI GL. This belongs to M107 - GPU Viewer Hand-off.
- Any edit to `libs/OpenFX`, `Engine/Gpu/GpuRuntime.*`, `GpuBufferPool.*`, `Engine/HostImageMemory.*`, `Engine/Nodes/Image/PassExecutor.*` or `tools/bench/*`.

**Edge cases:**
- **Vulkan and GL on different devices** (RADV beside llvmpipe under Xvfb): `availability()` is false, the GL plugin is not a region candidate, and it renders on M108's path.
- **Only the readback rung is reachable:** not a candidate. The boundary would cost what M108's path already costs.
- **Plane is not RGBA, the task is temporal (asks for frames other than the current time), has a concatenated input, uses host mask/mix/channel subset/unpremult, or its window exceeds `GL_MAX_TEXTURE_SIZE`:** not a candidate.
- **Two GL-suite members back to back in one region:** each crosses the boundary on its own, through device-side copies only.
- **The thread-context limit (`kMaxRenderGLContexts`) is reached on the region thread:** the member refuses, the region falls back through `renderMembersOnCpu`, and the plugin renders on M108's path.
- **The plugin returns `kOfxStatGLOutOfMemory`:** `FailureKind::OutOfMemory`, so the existing ladder runs (evict, then serial strips, then halve, then CPU). Any other failure is `Refused`, so CPU fallback.
- **A region with a GL member never pipelines strips.** If the whole region doesn't fit, it fails `TooLarge`, so CPU fallback.
- **The plugin calls `clipLoadTexture` for another time or clip during a boundary render:** it gets null, the render fails, the member refuses.
- **Device loss, or the runtime being recreated, mid-session:** `GlBoundary` drops its interops when the runtime generation changes. The region's existing device-loss handling applies.
- **A cached GL result serves a later CPU render of the same node hash** after the compute mode changes at the next launch: accepted. They differ by at most the plugin's own GPU/CPU tolerance (OCIO about 5e-6).
- **Legacy scheduler mode:** GL renders on per-thread contexts, like taskgraph mode. Regions exist only in taskgraph mode.

**Assumptions:**
- lavapipe and llvmpipe report matching device UUIDs. M107 reached hostsync on that pair, which implies they do. P1.T2 confirms it on the engine's GLX context.
- `glReadPixels` into a `GL_PIXEL_PACK_BUFFER` backed by an imported memory object works on llvmpipe and radeonsi. P1.T2 checks this.
- OCIOColorSpace and Shadertoy call `clipLoadTexture` only for the render time and the input RoI.
- A GL-suite render action can run from a region's scheduler thread on that thread's context, as `GLScheduler` tests already show for scheduler tasks.

**Scouted facts** (main `13f12edca`):
- **OFX headers:** `libs/OpenFX` is the `charlesangus/openfx-natron` fork at `646ab00c`. It has `ofxOpenGLRender.h`, not OFX 1.5's `ofxGPURender.h`. The CUDA, OpenCL and Metal properties exist only under `OFX_EXTENSIONS_RESOLVE` in `ofxImageEffect.h:1438-1600` (pre-1.5 Resolve set). HostSupport puts `OpenCLEnabled`/`CudaEnabled`/`OpenCLCommandQueue` in the render in-args, with no CUDA stream (`ofxhImageEffect.cpp:1376-1378`).
- **GL contexts:** the legacy path attaches a shared pool context on every frame (`ParallelRenderArgs.cpp:1047-1055`, `:1261`, release at `:1304`), so GL contexts are created even with no GL plugin. The scheduler path is already lazy (`EffectInstanceRenderRoI.cpp:730` → `EffectInstance::getRenderGLContext`, `EffectInstance.cpp:756`). Startup probe: `AppManager.cpp:350, 884-950` (through `attachGLContextToRender(false)` at `:895`). `RenderScheduler.cpp:1194-1219` checks GL planes; `task.storage` is always RAM (`RenderScheduler.cpp:1649`, `GpuPlacement.cpp:225`). `ViewerInstance.cpp:431` and `FrameRenderContext.cpp:444` pass a context into `setParallelRenderArgsTLS`.
- **Regions:** members are `NativeEffectBase*` only (`GpuRegionTask.cpp:618`, `GpuPlacement.cpp:496, 600-607`). Segments are submitted with `submitAndWait` (`:1031`), split at downloads (`:1340-1354`). `FailureKind` is at `:410`. The cost model gives an unseeded `eOther` plugin an infinite GPU cost (`GpuCostModel.h:195-201`), so an OFX task is never placed today.
- **Interop:** `GpuBuffer::createExportable` exists (`Engine/Gpu/GpuBuffer.h:52`), and the device enables `VK_KHR_external_memory_fd`/`external_semaphore_fd` when present (`GpuDevice.cpp:348-349`). `GpuDeviceInfo` has no UUID. On M107's branch, `GlInterop` is double-buffered and Vulkan→GL only (`produce`/`upload`, `GlRecordFn(cb, dst)`, timeline waits).
- **Renderer UUIDs:** M108 stores a device UUID per GL renderer (`OpenGLRendererInfo::deviceUuid/hasUuid`, `AppManager::getOpenGLRenderers()`, `AppManager.h:634`).
- **GL test material:** `GLScheduler_Test.cpp` uses OCIOColorSpace with CheckerBoard and Merge; OCIO's GPU and CPU paths agree to about 5e-6. `Tests/gui/run-gl-tests.sh` is the strict runner; CI has no GLX, so GL tests skip there.
- **Compute runtimes:** natron-dev (Rocky 9.8, mesa 25.2.7) ships the CUDA 13.1 toolkit (`/usr/local/cuda`, driver-API `cuda.h`, stub `libcuda.so`), `ocl-icd` and `opencl-headers`, with no OpenCL ICD and no rusticl. The dev host has ROCm 7.2.4 HIP (`/opt/rocm`) but no ROCm OpenCL. Mesa rusticl has `cl_khr_external_memory*` "not started". EPEL 9 has pocl 3.1 (CPU device).
- **GUI bench rule: not triggered.** No `Gui/NodeGraph*`, `NodeGui*`, `Edge*`, `Engine/Knob*` or `NodeGroup*` file is touched.

## Phase 86.1: Spikes

- [ ] M86.P1.T1 — Spike: which compute-API interop paths exist on the dev host and CI (input to the CUDA/OpenCL follow-up)
  - files: none in the repo (scratch probes and outputs under `.local/m86/p1/`), milestone Decisions entry
  - approach: Evidence, not docs:
    - **CUDA:** in natron-dev, confirm `/usr/local/cuda/include/cuda.h` declares `cuImportExternalMemory`, `cuExternalMemoryGetMappedBuffer`, `cuImportExternalSemaphore` and `cuWait/SignalExternalSemaphoresAsync`. Build a ~60-line probe that `dlopen`s `libcuda.so.1` and resolves them, without linking cudart. Run it, and expect a clean "no driver" result. CI uses the same `aswf/ci-vfxall:2027-clang21.1` base.
    - **HIP (host, outside the container, `/opt/rocm` 7.2.4):** with `hipcc`, build a probe that exports a RADV `VkBuffer` (opaque fd) and a binary semaphore, imports both with `hipImportExternalMemory`/`hipImportExternalSemaphore`, has a kernel wait on the semaphore, write a ramp and signal back, then lets Vulkan read it. This records whether HIP can test the shared interop protocol in CUDA's place.
    - **OpenCL:** record that natron-dev has no ICD and Rocky's mesa has no rusticl. Ask the user, through the PM, whether they will install `rocm-opencl-runtime` 7.2.4 on the host; if so, record `clinfo` for `cl_khr_external_memory_opaque_fd`, `cl_khr_external_semaphore_opaque_fd`, `cl_khr_device_uuid` and `cl_khr_gl_sharing`. On a throwaway branch, run `dev-image.yml` with `dnf install -y pocl` added to `tools/ci/local/Dockerfile` (never merged), and record pocl's device and extensions on a runner.
  - verify: a Decisions table, API × {dev host RADV, natron-dev, CI runner} → compile / run / zero-copy import / staged only / none, with each probe's output saved under `.local/m86/p1/`.
  - size: M

- [ ] M86.P1.T2 — Spike: GL↔Vulkan rungs on the engine's own GL context
  - files: `Tests/Native/GlBoundarySpike_Test.cpp` (new, scratch; deleted or folded into P4.T2's test), milestone Decisions entry
  - approach: Under `Tests/gui/run-gl-tests.sh` in natron-dev, on the context `GPUContextPool::getOrCreateContextForCurrentThread()` returns, with the session `GpuRuntime` on lavapipe, then on RADV:
    1. Run `GlInterop::probe` and record `best`, `missing` and the UUIDs.
    2. **Vulkan→GL:** produce an RGBA32F ramp at 1920×1080, upload it into a texture, read it back with `glGetTexImage` and compare.
    3. **GL→Vulkan:** make a `GpuBuffer::createExportable` buffer and export its fd; import it with `glCreateMemoryObjectsEXT`/`glImportMemoryFdEXT`/`glNamedBufferStorageMemEXT`, loaded through the context's proc loader; bind it as `GL_PIXEL_PACK_BUFFER` and `glReadPixels` from an FBO holding a known texture, with `glFinish` on hostsync; download it on Vulkan and compare bytes.
    4. Time steps 2 and 3 at UHD on each rung.
    5. Render bundled OCIOColorSpace and Shadertoy once through M108's path in the same runner.
    `GlInterop` is used read-only, as it is on main (M107 rewrites it).
  - verify: a Decisions entry with the rung per (Vulkan ICD × GL renderer), byte-exactness of both directions, the UHD timings, and whether the pack-buffer import works on llvmpipe. Expected: lavapipe+llvmpipe reaches hostsync, and RADV+llvmpipe reaches readback (UUID mismatch). If the pack-buffer import fails on llvmpipe, stop and replan P4.T1.
  - size: M

## Phase 86.2: GL contexts only for GL-suite renders

- [ ] M86.P2.T1 — Make per-thread contexts the only render contexts and delete the shared pool
  - files: `Engine/GPUContextPool.h`, `Engine/GPUContextPool.cpp`, `Engine/AppManager.cpp`, `Engine/EffectInstance.cpp`, `Tests/OSGLContext_Test.cpp`
  - approach:
    - Delete `attachGLContextToRender`, `releaseGLContextFromRender`, `NATRON_RENDER_SHARED_CONTEXT` and the `glContextPool`, `attachedGLContexts`, `lastUsedGLContext`, `glShareContext` and `glContextPoolEmpty` members.
    - Move the one-time `GL_MAX_TEXTURE_SIZE` read into `getOrCreateContextForCurrentThread` (first context made current), or take it from the picked renderer's `OpenGLRendererInfo::maxTextureSize`.
    - Add `std::size_t getNumContextsCreated() const` (an atomic, bumped on each creation).
    - `AppManager::initializeOpenGLFunctionsOnce` builds its probe `OSGLContext` directly (same config, version and default renderer), makes it current, loads glad, checks the version and destroys it. Drop `renderingContextPool->clear()` there.
    - `EffectInstance::getRenderGLContext` always returns the thread context. The parameter stays until P2.T2.
    - Delete `GPUContextPool.DISABLED_Basic` in `OSGLContext_Test.cpp`, which calls the deleted attach, so the build stays green.
  - verify:
    - Phase build; CI ctest green (GL tests skip).
    - In natron-dev, `Tests/gui/run-gl-tests.sh 'GLScheduler.*:GlHostWrap.*'` passes with no skips, under both `renderSchedulerMode` values. Legacy mode now renders GL on thread contexts.
    - `git grep -n attachGLContextToRender Engine Gui Tests` is empty.
  - size: L

- [ ] M86.P2.T2 — ⚑ Delete the frame-level GL context plumbing (start after M107 - GPU Viewer Hand-off merges: `Engine/ViewerInstance.cpp`, `Engine/FrameRenderContext.*`)
  - files: `Engine/ParallelRenderArgs.h`, `Engine/ParallelRenderArgs.cpp`, `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp`, `Engine/FrameRenderContext.cpp`, `Engine/ViewerInstance.cpp`
  - approach: a mechanical removal. Delete `ParallelRenderArgs::openGLContext`; `ParallelRenderArgsSetter::_openGLContext` with its attach (`ParallelRenderArgs.cpp:1047-1055, 1261-1262`) and release (`:1304`); the `OSGLContextPtr` parameter of `buildArgsMap` and of `EffectInstance::setParallelRenderArgsTLS`, with its arguments at `FrameRenderContext.cpp:444` and `ViewerInstance.cpp:431`. `getRenderGLContext()` loses its parameter, and `getThreadLocalOpenGLContext` calls it plainly. `currentOpenglSupport` stays.
  - verify:
    - Phase build.
    - `git grep -nE '\bopenGLContext\b|_openGLContext' Engine` is empty.
    - ctest `RenderScheduler*`, `ViewerScheduler*` and `GLScheduler.*` green on CI; the strict GL run is green.
  - size: M

- [ ] M86.P2.T3 — Delete the scheduler's GL-texture plane check
  - files: `Engine/RenderScheduler.cpp`
  - approach:
    - Delete the `glPlanes` detection, its failure (`:1194-1204`) and the texture-deletion branch (`:1212-1215`). Task storage is always RAM since M108.
    - Keep the bind-count check and `OSGLContext::unsetCurrentContextNoRender()`.
    - Add an `assert` that no plane is `eStorageModeGLTex`.
  - verify: phase build. ctest `RenderScheduler*` and `GLScheduler.*` are green.
  - size: M

- [ ] M86.P2.T4 — Rework the GL tests around per-thread contexts, and prove there is no context without a GL-suite plugin
  - files: `Tests/GLScheduler_Test.cpp`, `Tests/OSGLContext_Test.cpp`
  - approach:
    - Add `GLScheduler.NoGlContextWithoutAGlSuitePlugin`: force the GL suite on (`setRenderingEnabledForTests(true)` plus the project knob); render CheckerBoard → Merge → native Grade in taskgraph mode, then in legacy mode; assert that `getNumContextsCreated()` is unchanged.
    - Add `GLScheduler.LegacyModeGlNodeUsesItsThreadContext`: legacy mode renders OCIOColorSpace via GL (`getNumThreadContextsUsedForRender() > 0`), and the result matches CPU within 5e-6.
    - Turn `OSGLContext.DISABLED_Basic` into a test of a per-thread context: create it, make it current, `glGetString(GL_RENDERER)` is non-null, unbind it.
    - All of these use the existing `skipWithoutOpenGL` pattern.
  - verify: `Tests/gui/run-gl-tests.sh 'GLScheduler.*:OSGLContext.*'` passes with no skips in natron-dev, and CI prints skips and passes.
  - size: M

## Phase 86.3: GL-suite output on the CPU path

- [ ] M86.P3.T1 — Cache GL-suite output and render only the requested window
  - files: `Engine/EffectInstanceRenderRoI.cpp`, `Tests/GLScheduler_Test.cpp`
  - approach:
    - Delete the GL no-cache branch (~`:801-807`) and the full-RoI forcing (~`:1082`). The output is a RAM layer image since M108.
    - Keep render scale, the max-texture-size fallback and `readBackOpenGLRender`. The cache key is unchanged.
  - verify: new `GLScheduler` cases: rendering the same OCIOColorSpace frame twice runs the render action once (the `getRenderActionsOnThisThread` counter); a request for a sub-window yields an image whose bounds cover that window and not the full RoD; results match CPU within 5e-6. Strict GL run green, CI green.
  - size: M

## Phase 86.4: GL-suite plugins inside GPU regions

- [ ] M86.P4.T1 — ⚑ GL→Vulkan direction in `GlInterop` (start after M107 - GPU Viewer Hand-off merges: `Engine/Gpu/GlInterop.*`, `Tests/Gpu/GlInterop_Test.cpp`)
  - files: `Engine/Gpu/GlInterop.h`, `Engine/Gpu/GlInterop.cpp`, `Tests/Gpu/GlInterop_Test.cpp`
  - approach: Add a reverse pair on the existing slots.
    - `download(slot, texture, x, y, const GlFrame&)` (GL side): bind the slot's imported buffer as `GL_PIXEL_PACK_BUFFER`; read the texture rectangle through an internal FBO (`glReadPixels`, `GL_RGBA`/`GL_FLOAT`); finish by rung — **ZeroCopy:** signal a new per-slot GL→Vulkan semaphore and `glFlush`; **HostSync:** `glFinish`; **Readback:** read into host memory.
    - `acquire(slot, GlRecordFn record, TimelineValue& done)` (Vulkan side): wait on that semaphore (ZeroCopy), or stage the host copy (Readback); acquire from `VK_QUEUE_FAMILY_EXTERNAL`, then run `record(cb, src)`, where the caller copies out; signal and return a compute timeline value.
    - Each slot strictly alternates `download` and `acquire`, as `produce` and `upload` do.
  - verify:
    - `GlInterop_Test` round trip on every rung is byte-exact: produce ramp → upload → download → acquire copy → host compare.
    - An interleave case: produce slot B while slot A is downloading.
    - Runs on lavapipe+llvmpipe and RADV+radeonsi (natron-dev `NATRON_DEV_GPU=1`, the radeonsi GL display setup M107 uses for its GlInterop cases), with the UHD download time logged.
  - size: L

- [ ] M86.P4.T2 — `GlBoundary`: per-context interop resources and availability without a context
  - files: `Engine/GlBoundary.h` (new), `Engine/GlBoundary.cpp` (new), `Engine/OSGLContext.h`, `Engine/OSGLContext.cpp`, `Tests/Native/GlBoundary_Test.cpp` (new)
  - approach:
    - `OSGLContext` owns an opaque `std::unique_ptr` resource slot, destroyed in `~OSGLContext` with the context current.
    - `GlBoundary::forCurrentContext()` lazily creates, in that slot, a `GlInterop` for inputs and one for the output. They are sized up when a frame exceeds capacity, and dropped when `GpuContext`'s runtime generation changes.
    - `static bool availability()`: needs the GL suite enabled (`GPUContextPool::isRenderingEnabled()`); needs the picked renderer's `deviceUuid`, found by `getRendererID()` in `AppManager::getOpenGLRenderers()`, to equal the Vulkan physical device UUID (read once through `vkGetPhysicalDeviceProperties2`); needs no latched runtime refusal (for example, an interop `create` failed); `NATRON_GL_BOUNDARY=0` turns it off. It never creates a context. It logs the rung once per session (`GL boundary: zero-copy|hostsync|off (<reason>)`).
    - `uploadInputs(device views → RGBA32F textures)` and `downloadOutput(texture → device buffer, returns TimelineValue)` wrap P4.T1's API.
    - Everything sits under `#ifdef NATRON_HAVE_GPU`. New Engine files need the phase build's fresh configure.
  - verify: phase build. `GlBoundary_Test` passes under the strict runner: a device ramp goes buffer → texture → buffer and stays byte-exact; `availability()` is true on lavapipe+llvmpipe, and false with the reason "device mismatch" on RADV+llvmpipe. On CI it skips.
  - size: L

- [ ] M86.P4.T3 — OFX GL-suite render at the boundary
  - files: `Engine/OfxEffectInstance.h`, `Engine/OfxEffectInstance.cpp`, `Engine/OfxClipInstance.h`, `Engine/OfxClipInstance.cpp`, `Tests/Native/GlBoundary_Test.cpp`
  - approach:
    - Add `StatusEnum OfxEffectInstance::renderGlAtBoundary(const GlBoundaryRender&)`, taking time, view, mapped scale, render window, plane, input textures by input index (id, target, bounds) and the output texture. It mirrors the GL branch of `tiledRenderingFunctor`: `attachOpenGLContext_public` on the thread context; an FBO with the output attached, viewport and scissor set to the window; begin, render and end sequence render with OpenGL enabled; detach.
    - `OfxClipInstance::loadTexture` checks a TLS boundary map first: an input clip at the render time gets a `Texture` built from the given texture (float, RGBA, its bounds) without `getImage`; the output clip gets the output texture; anything else gets null.
    - `bool canRenderGlAtBoundary(const GpuQuery&) const` requires GL support Yes or Needed; `nComps == 4`; no host mask or mix, all four channels processed, no unpremult-by; no concatenated input; `getFramesNeeded` asking only the current time; a window within the max texture size.
  - verify: `GlBoundary_Test`, strict GL: OCIOColorSpace and Shadertoy (default shader) rendered through `renderGlAtBoundary` from textures uploaded from a RAM ramp are bit-exact with the same node rendered on M108's path.
  - size: L

- [ ] M86.P4.T4 — ⚑ Placement offers GL-suite OFX tasks as region members (start after M107 merges: `Engine/GpuPlacement.cpp` and `Tests/GpuPlacement_Test.cpp` are edited on its branch)
  - files: `Engine/GpuPlacement.cpp`, `Engine/GlBoundary.h`, `Tests/GpuPlacement_Test.cpp`
  - approach:
    - `makeGpuQueryForTask` accepts an `OfxEffectInstance` whose current GL support is not None when `GlBoundary::availability()` holds. It fills the query from the task and plane, with an empty stage summary (no host stages).
    - `makeRenderPlacementInputs.canRenderOnGpu` dispatches to `NativeEffectBase::canRenderOnGpu` or `OfxEffectInstance::canRenderGlAtBoundary`.
    - Add a test override `setGlBoundaryAvailableFuncForTests`.
  - verify: `GpuPlacement_Test`, with the override and a fake OFX-GL task: native → GL → native forms one region; with availability off, the GL task splits the chain; a temporal or host-masked GL task is excluded.
  - size: M

- [ ] M86.P4.T5 — ⚑ Cost model prices GL-suite members by the transfers they save (start after M107 merges: `Engine/GpuCostModel.*`)
  - files: `Engine/GpuCostModel.h`, `Engine/GpuCostModel.cpp`, `Tests/GpuPlacement_Test.cpp`
  - approach:
    - Add `GpuCostClass::eGlSuite`, set by `traitsOf` for OFX GL tasks.
    - `cpuMs` is the observed M108-path wall time, which `observeCpu` already records and which includes the upload and the readback.
    - `gpuMs = max(point-op default, observed cpu − modelled input uploads − modelled output download at the calibrated rates)`. With no observation yet, use the point-op default, so a first frame can place.
    - No `observeGpu` for this class.
  - verify: unit cases: a transfer-dominated GL member gives a positive region gain; an unknown plugin uses the default; existing cost-model cases are unchanged.
  - size: M

- [ ] M86.P4.T6 — ⚑ Region body runs GL-suite members across the boundary (start after M107 merges: `Engine/GpuRegionTask.cpp`)
  - files: `Engine/GpuRegionTask.cpp`
  - approach:
    - `Member` gains `OfxEffectInstance* glEffect`, and `plan()` accepts it when `canRenderGlAtBoundary` holds. A region with a GL member runs whole, never as pipelined strips. Too large means `TooLarge`, so the existing CPU fallback.
    - In `record()`, at a GL member: (1) submit the open segment; (2) `GlBoundary::uploadInputs` from the member's input slots (copy slot → interop buffer, then texture); (3) allocate the output texture over `renderedRoI`, and call `renderGlAtBoundary`; (4) `downloadOutput` into the member's out-slot buffer, and make the next segment wait on the returned value. On ZeroCopy the CPU never waits on GL, only `glFlush`.
    - Status mapping: `kOfxStatGLOutOfMemory` (and `eStatusGLOutOfMemory`) maps to `FailureKind::OutOfMemory`, so the ladder runs. Any other failure, a null thread context (the context limit) or an interop failure is `Refused`, so `renderMembersOnCpu`; the plugin then renders on M108's path.
    - Check for an abort before the plugin call. Add the GL member's time to `recordMs`.
  - verify: phase build. P4.T7's tests.
  - size: L

- [ ] M86.P4.T7 — End-to-end tests for GL-suite members in regions
  - files: `Tests/Native/GlRegionMember_Test.cpp` (new), `Tests/gui/run-gl-tests.sh`
  - approach:
    - The runner gains `GL_TEST_VK_ICD=lavapipe|radv`, which exports the matching `VK_ICD_FILENAMES` and `NATRON_GPU=force`.
    - Cases in taskgraph mode: (a) native CheckerBoard → OCIOColorSpace → native Grade at 1920×1080 form one region of 3 members; `RenderStats` counts one download (the root); the result equals the same graph with GPU placement off within 1e-6. (b) Two OCIOColorSpace nodes back to back in one region give the same parity. (c) With `GL_TEST_VK_ICD=radv`, OCIO is not a region member, and parity holds. (d) A test hook makes the boundary report GL out-of-memory once; the ladder retries and the frame matches. (e) An abort mid-region leaves no context bound and no leaked interop.
    - Skip pattern on CI.
  - verify: in natron-dev, `GL_TEST_VK_ICD=lavapipe Tests/gui/run-gl-tests.sh 'GlRegionMember.*:GlBoundary.*'` and the `radv` run both pass with no skips. The lavapipe run's log shows the `GL boundary: hostsync` rung.
  - size: M

**Verification gate:**
- Phase builds are green on remote CI (`tools/ci/remote/build.sh fast --tests`, lavapipe, both scheduler legs). GL tests print skips there.
- In natron-dev, these pass with no skips: `Tests/gui/run-gl-tests.sh 'GLScheduler.*:GlHostWrap.*:OSGLContext.*'`; `GL_TEST_VK_ICD=lavapipe Tests/gui/run-gl-tests.sh 'GlRegionMember.*:GlBoundary.*'` (hostsync rung); the same with `radv`; `GlInterop_Test` on lavapipe+llvmpipe and RADV+radeonsi, including the new GL→Vulkan ZeroCopy round trip.
- `GLScheduler.NoGlContextWithoutAGlSuitePlugin` passes in both scheduler modes.
- `git grep -nE 'attachGLContextToRender|releaseGLContextFromRender|NATRON_RENDER_SHARED_CONTEXT|\bopenGLContext\b|_openGLContext' Engine Gui Tests` is empty.
- GUI bench: not triggered (record that in Decisions).

## Overlaps with running milestones

| M86 file | Running owner | Tasks | Handling |
|---|---|---|---|
| `Engine/Gpu/GlInterop.{h,cpp}`, `Tests/Gpu/GlInterop_Test.cpp` | M107 (P2.T1, rewritten on its branch) | P4.T1 | ⚑ start after M107 merges. P1.T2 only reads main's `GlInterop`. |
| `Engine/GpuRegionTask.cpp` | M107 (P5.T1) | P4.T6 | ⚑ start after M107 merges. |
| `Engine/GpuCostModel.{h,cpp}` | M107 (P5.T1) | P4.T5 | ⚑ start after M107 merges. |
| `Engine/GpuPlacement.cpp`, `Tests/GpuPlacement_Test.cpp` | M107's branch edits them although its task lists don't name them | P4.T4, P4.T5 | ⚑ start after M107 merges. |
| `Engine/ViewerInstance.cpp`, `Engine/FrameRenderContext.cpp` | M107 (P3.T2–P4.T4, P4.T2) | P2.T2 | ⚑ start after M107 merges. |
| `Engine/HostImageMemory.*`, `Engine/Nodes/Image/PassExecutor.*`, `tools/bench/*` | M154 | none | Avoided. M154's branch diff touches none of M86's files. |
| `Engine/EffectInstance.{h,cpp}`, `Engine/ParallelRenderArgs.{h,cpp}`, `Engine/FrameRenderContext.cpp`, `Engine/RenderScheduler.cpp`, `Engine/AppManager.cpp` | M64 (blocked, dropped for now) | P2.T1–P2.T3 | **Accepted (user, 2026-10-22):** M86 runs; M64 rebases over it if it resumes. M86 edits the GL plumbing, not M64's functions. |
| `Engine/GPUContextPool.*`, `Engine/OSGLContext.*`, `Tests/OSGLContext_Test.cpp` | none running (M68, todo, lists them) | P2.T1, P4.T2, P2.T4 | Free. M68 later rebases onto the narrowed pool. |
| `Engine/OfxEffectInstance.*`, `Engine/OfxClipInstance.*`, `Engine/EffectInstanceRenderRoI.cpp`, `Tests/GLScheduler_Test.cpp`, `Tests/gui/run-gl-tests.sh`, new files | none | various | Free. |

**Order:** P1, P2.T1/T3/T4 and P3 can run now. P2.T2 and P4.T1/T4/T5/T6 wait for M107. P4.T2/T3/T7 don't touch M107 files, but depend on P4.T1's API.

## Decisions

- 2026-10-22 — **Claimed by the gpu PM for the lane M108 freed; elaborated at claim by a consultant on main `13f12edca`.** Stubs pass the claim's overlap test; the recheck after elaboration finds M107 overlaps (handled by ⚑ flags, M107 being earlier on the board) and M64 overlaps (accepted by the user, below). No overlap with M154.
- 2026-10-22 — **GL suite only; CUDA and OpenCL go to a follow-up milestone (user).** No host here can run CUDA. OpenCL zero-copy with Vulkan needs `cl_khr_external_memory_opaque_fd`, which rusticl has not started, and ROCm OpenCL is not installed. The GL boundary is fully testable and covers the bundled GL plugins; the follow-up reuses its region boundary. Proposed through the inbox for `/cat-plan` to file, with its stub text. P1.T1's spike feeds its elaboration.
- 2026-10-22 — **The M64 file overlap is accepted (user),** as it was for M108: M86 runs, M64 rebases over it if it resumes.
- 2026-10-22 — **GL-suite output on the CPU path is cached and renders only the requested window (user; M108 deferred this to M86).** The output has been a RAM image since M108, so the texture-era reasons no longer apply.
- 2026-10-22 — **The radeonsi zero-copy rung is proven by hostsync under Xvfb plus `GlInterop`'s radeonsi ctest (user).** No desktop-session script; the full engine-context radeonsi check comes with M68 - Headless GL EGL. The device-memory headroom measurement moves to the CUDA/OpenCL follow-up, where out-of-pool plugin memory is in scope.
- 2026-10-22 — **The GL boundary copies through per-context `GlInterop` buffers rather than an exportable region pool (planner).** Thread contexts share nothing, and GL objects must be deleted on their own context, so importing pool buffers once per buffer would need a cache per context. A device-side copy of a UHD float plane costs about 0.15 ms on the 7900 XTX. This also avoids `GpuRuntime.*`/`GpuBufferPool.*`.
- 2026-10-22 — **"No GL context without a GL-suite plugin" means render contexts (planner).** The startup probe context, which must exist before `QApplication` because of X11, and M108's per-renderer UUID probe stay.
- 2026-10-22 — **Placement decides boundary availability without creating a context (planner),** from M108's stored renderer UUID against the Vulkan UUID. Extension gaps found at run time latch it off.
- 2026-10-22 — **GL-suite members in a region are RGBA-only, have no host mask/mix/channel/unpremult stages and never pipeline strips (planner).** No bundled plugin needs the host stages, which therefore couldn't be tested. Everything else stays on M108's path.
- 2026-10-22 — **GL-suite tasks are priced by the transfers they save (planner).** Their GPU work runs on both paths; the M108-path wall time minus the modelled transfers stands in for the GPU time.
- 2026-10-22 — **P2.T3 sized M, not S (PM):** it deletes logic in `executeTask` and adds an assert, beyond a spelled-out mechanical edit.
- 2026-10-22 — **Paused and released (user).** No task is checked. `milestone/m86-ofx-gpu-suites` is pushed at main `13f12edca` with no commits of its own, and the lane worktree `.worktrees/m86` is kept. P1.T1's spike was stopped mid-run: its unreviewed probe sources and logs are in `.local/m86/p1/` on the gpu host (CUDA in natron-dev, a HIP↔RADV probe, four pocl-on-CI attempts). The throwaway `spike/m86-pocl` branch, which has a temporary worktree at `.local/m86/p1/wt`, is still on the remote and must be deleted, never merged. The next claimant re-runs or finishes P1.T1 from that data.
