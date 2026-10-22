# M107 - GPU Viewer Hand-off

Main has the pieces for this, but they are not connected. `GlInterop` is never called outside its tests. A GPU region always downloads its root. In the default 8-bit viewer mode, the CPU applies the OCIO display transform. In 32-bit float mode, OpenGL's fragment shader applies it. So the Vulkan OCIO kernel is only needed for 8-bit mode, which is the default. In float mode the GL shader keeps doing the display transform after the hand-off.

Two things block the end-to-end acceptance. First, keeping the root on the device needs a small edit to `Engine/GpuRegionTask.cpp`, which M109 is rewriting heavily, so that task is flagged to run after M109 merges. Second, the RADV "0 downloads" check needs a radeonsi GL display on the bench host. Xvfb only gives llvmpipe, which can only use the readback path. Spike P1.T2 is there to find that display.

**Scouted facts** (origin/main `2410277ee`)

**The viewer pipeline today**
1. `ViewerInstance::renderViewer_internal` sends the frame to the RenderScheduler (`submitViewerFrameToRenderScheduler`, `Engine/ViewerInstance.cpp:476-523`). It then calls `future->wait()` and `future->getRootPlanes()` (`:1665-1667`), which return **RAM** `ImagePtr`s. Frames the scheduler can't take go through `renderRoI(... eStorageModeRAM)` instead (`:1669-1683`).
2. Each tile with no cached copy goes through `renderFunctor` (`:2129-2140`). It calls `scaleToTexture32bits` at **:2135** and `scaleToTexture8bits` at **:2138**. The stub said :2137/:2140; the declarations at :107/:112 are correct.
   - **8-bit** (`scaleToTexture8bits_generic`, `:2273-2448`):
     - converts to float, selects the channel or computes luminance (Y);
     - calls `applyViewerDisplayTransform` (`:139-190`, with the 8-bit LUT shortcut `applyForEightBitOutput` when gamma is 1). That applies gain and offset, then OCIO on the CPU, then gamma (a threshold at gamma ≤ 0);
     - applies the matte overlay, with the display transform also run on the matte (`:2403-2406`);
     - dithers by error diffusion along each row from a `rand()` start (`:2408-2440`), so the CPU output is not deterministic;
     - packs ARGB with `toBGRA` (`:127-136`).
   - **32f** (`scaleToTexture32bitsGeneric`, `:2628-2752`): only channel select, luminance, int→float and `r += matte*0.5`. No gain, gamma or OCIO.
3. Finished tiles go into the viewer texture cache (`FrameKey` includes gain, gamma, the OCIO `cacheHash`, depth and channels; `:1176-1245`, `:1880-1950`). `useTextureCache` is false under auto-contrast, user RoI, paint, partial updates and force-render (`:1469`). Each cache entry also keeps the RAM `colorImage` (`setInternalImage`).
4. On the main thread, `ViewerInstancePrivate::updateViewer` (`:2890-2970`) calls `uiContext->transferBufferFromRAMtoGPU` once per tile (`:2929`), then `endTransferBufferFromRAMToGPU` (`:2959`).
5. `ViewerGL::transferBufferFromRAMtoGPU` is at **`Gui/ViewerGL.cpp:1560-1672`** (stub said ~1557). It uses two rotating PBOs: `glBufferData(NULL)`, map, `memcpy`, then `fillOrAllocateTexture`. The texture is RGBA8 (BGRA / `8_8_8_8_REV`) or RGBA32F, depending on `getBitDepth()` (`:3186`). That is the `texturesBitDepth` setting, **default "8u"** (`Engine/Settings.cpp:1104-1123`, `:1626`); a shader failure forces 8u.
6. OpenGL OCIO is used only when `useShader = getBitDepth() != Byte` (`Gui/ViewerGLPrivate.cpp:251`):
   - `buildShaderRGB` (`:874-1040`) builds `GPU_LANGUAGE_GLSL_1_2` (`:895`) and its LUT textures;
   - `activateShaderRGB` (`:1046-1103`) sets the gain, offset and gamma uniforms plus OCIO's uniforms.
7. `endTransferBufferFromRAMToGPU` (`Gui/ViewerGL.cpp:1452-1530`) stores the RAM image in `lastRenderedTiles[mip]`. The colour picker, pixel info and `Gui/Histogram.cpp:502` all read it through `getLastRenderedImageByMipmapLevel` (`:4171`).

**The answer to the OCIO question:** only 8-bit mode needs OCIO in the Vulkan kernel (OCIO's VK GLSL, compiled through M126's pipeline cache). In 32f mode the kernel does channel select, luminance and matte, and the existing GL shader keeps applying gain, OCIO and gamma to the handed-off float texture.

**Where a resident root lives**
- `RegionRun::plan` (`Engine/GpuRegionTask.cpp:655-679`; `:679` on the M109 branch) sets `cpuConsumer = output.isRoot`, so the root slot always gets `download = true`.
- `storeOutputs` (`:1958-1975`) fails with "the root was not downloaded" if it wasn't, and sets `(*rootPlanes)[layer] = slot.downloaded`.
- Device outputs that other regions read already go through `GpuDeviceImage::create(...)` and `FrameStore::putDevice` (`:1976-2005`). `GpuDeviceImage` (`Engine/GpuContext.h:234-290`) has `pin`/`unpin`, `setEvictable`, `downloadToRam` and `layout()`. Its pixels are float, `nComps` interleaved, rows packed from the bottom (`GpuImageLayout`).
- `RenderScheduler` keeps the root task's planes (`:1340`), then `finalizeFrames` runs `getStore().clear()` and `future->finish(Ok, rootPlanes)` (`:1745-1757`). A device root stored in the FrameStore would be dropped there. It has to travel on the `FrameRenderContext`, which the viewer creates (`ViewerInstance.cpp:505`) and keeps holding.
- Region roots in strip mode are downloaded strip by strip (`stripSlot.interior = keep || download`, `:1164`). M109.P4.T1 also strips regions that fit whole when that hides transfer time.
- `GpuCostModel::regionGain` charges a download for the root (`Engine/GpuCostModel.h:176-177`), so placement under-rates regions that feed the viewer.
- Download accounting already exists:
  - `RenderStats::noteDownload` / `getDownloadCount` (`Engine/RenderStats.h:168,209`);
  - `NATRON_RENDER_PROFILE` writes a JSONL record per region with `downloadBytes` (`RenderScheduler.cpp:186`; sample in `tools/bench/render-profile-sample.jsonl`).

**What M83 left in `GlInterop`** (`Engine/Gpu/GlInterop.{h,cpp}`)
- It is only used by `Tests/Gpu/GlInterop_Test.cpp` (EGL surfaceless) and `tools/gpu-spike/tests/GlInteropQt_main.cpp` (Qt under Xvfb).
- `probe` checks the Vulkan and GL external-memory and semaphore extensions plus matching device and driver UUIDs, and picks `best`.
- `create(device, loader, w, h, path)` makes **one** RGBA32F buffer of a fixed size.
- `produce(record)` (`:593-707`) records the caller's writes. It brackets them with acquire and release barriers to `VK_QUEUE_FAMILY_EXTERNAL`; on the readback path it copies to a host-visible buffer instead. It submits on the compute queue (with semaphores on ZeroCopy) and never waits on a timeline value it is given.
- `upload(texture)` (`:709-770`):
  - **ZeroCopy** waits on the GL semaphore, does `TexSubImage2D` from the imported PBO, signals and flushes;
  - **ZeroCopyHostSync** waits on the fence first and calls `glFinish` at the end;
  - **Readback** invalidates, orphans and maps the PBO, then copies with `memcpy`.
- `produce` and `upload` must strictly alternate (`pendingUpload_`): a second `produce` fails. That is the single-buffer limit M83's review deferred. Uploads are always full-size from (0,0) with `GL_RGBA`/`GL_FLOAT`, so there is no 8-bit format and no sub-rectangle.
- M83 measured on RADV/radeonsi at UHD: 0.79 ms zero-copy, 0.87 ms hostsync, 33.2 ms readback plus PBO. On lavapipe/llvmpipe, hostsync is byte-exact (including Qt under Xvfb) but semaphore zero-copy is skipped.

**Other facts the plan depends on**
- `GpuContext::holdRuntime()` stops `recoverIfLost()` from recreating the runtime while a token is held. `recreate()` invalidates every handle (`Engine/GpuRuntime.h`). `appPTR->getGpuContext()` is at `Engine/AppManager.h:430`.
- `GpuRuntime::pipelines()` is M126's `GpuPipelineCache::get(source, stage, entry)` (thread-safe, cached in memory and on disk). `GpuKernel::recordBindings` binds reflected UBOs, storage buffers and `GpuTexture`s on a raw `VkCommandBuffer`, which is what `GlInterop::produce` hands out.
- `GpuDevice::submit` is mutex-guarded (`Engine/Gpu/GpuDevice.h:~148`), so a viewer submit from the main thread is safe next to region submits.
- `Engine/*.cpp` and `Gui/*.cpp` are globbed, so new files need no CMake edit. `Tests/Native/*_Test.cpp` is globbed with `CONFIGURE_DEPENDS` (`Tests/CMakeLists.txt:122`). `Tests/CMakeLists.txt`'s explicit list is touched by both M100 and M109.
- `NATRON_HAVE_GPU` is a PUBLIC define of NatronEngine, so Gui code can `#ifdef` on it.
- `OpenGLViewerI` (`Engine/OpenGLViewerI.h`) has exactly one implementor, `ViewerGL`.
- M127 (todo) plans `Engine/Nodes/OCIO/OcioGpuShader.{h,cpp}` (M127.P3.T1) for the same OCIO VK work: `GPU_LANGUAGE_GLSL_VK_4_6`, LUTs as `GpuTexture`, uniforms as a UBO. M107 should own that helper and M127 should reuse it.
- `Tests/gui/run-gui-test.sh` with `guitest.py` (PASS/FAIL/SHOT lines) is the Xvfb GUI harness. `tools/bench/gui/` does not exist on main.

## Scope

**In:**
- `GlInterop` double-buffered slots, with capacity-based sizing, RGBA32F and BGRA8 formats, sub-rectangle uploads and a timeline wait before produce.
- A runtime-GLSL viewer-process compute kernel covering:
  - channel display (RGB/R/G/B/A/Y) and the matte overlay;
  - gain and offset, the OCIO display/view/look (OCIO's `GLSL_VK_4_6` through M126's pipeline cache) and gamma, including the threshold at gamma ≤ 0;
  - 8-bit packing with dither (8u mode only);
  - float output for 32f mode, where the existing GL shader keeps gain, OCIO and gamma.
- A shared OCIO→Vulkan shader helper, which M127 reuses, with a per-processor LUT texture cache. This lifts M126's re-upload-every-render limit for the viewer.
- An Engine-side `ViewerHandoff` that owns the interop slots per viewer input (A/B). It picks a fallback rung: ZeroCopy → ZeroCopyHostSync → Readback (GPU kernel, then a download of the 4 B/px or 16 B/px display buffer) → the CPU path.
- `ViewerGL` wiring: `QOpenGLContext::currentContext()->getProcAddress`, a device-image upload entry on `OpenGLViewerI`, and teardown with the context current.
- A device root delivered on `FrameRenderContext`. The viewer consumes it and skips `scaleToTexture*`.
- On-demand download of the displayed device root for the colour picker, pixel info and histogram.
- The region root kept on the device when the viewer asks for it (flagged task, after M109).
- Fallback reasons through the existing `RenderStats::incLegacyFallbacks("viewer handoff: …")`.
- A diagnostic rung override, `NATRON_VIEWER_HANDOFF=zerocopy|hostsync|readback|cpu`, for tests and benches. It is not a setting.
- A UAT script under Xvfb, then a quick directional bench before a thorough one.

**Out:**
- GPU min/max for auto-contrast. Auto-contrast frames take the CPU path; follow-up milestone, none owns it yet.
- A GPU viewer process for roots that live on the CPU (uploading a RAM root to run the kernel). Follow-up, unowned.
- Partial updates (RotoPaint/tracking), deep input and paint strokes. The scheduler doesn't take these frames (`ViewerInstance.cpp:453-465`), so they stay on the CPU.
- Matte overlay whose alpha layer differs from the colour layer. A region renders one plane (`makeGpuQueryForTask`), so these stay on the CPU.
- The engine GL host path: M108 - Retire Engine GL Host Path.
- The node-graph overlay of regions feeding the viewer: M116 - GPU Region Overlay. Its "still a download" note is resolved by this milestone.
- OCIO nodes on Vulkan: M127 - Native OCIO Nodes, which reuses the P3.T1 helper.
- NVIDIA interop validation: M91 - NVIDIA GPU Validation.

**Edge cases:**
- The viewport grows (pan or zoom out): the slot is too small, so that frame falls back to Readback or the CPU and the main thread grows the slots for the next frame.
- A/B wipe: one slot ring per `textureIndex`.
- A frame dropped by the age check in `updateViewer`: release the pinned root without uploading.
- Texture pixels outside the root's bounds are processed as (0,0,0,0) through the full transform, as the CPU path does with `src_pixels == null`.
- Display transform changes, or `shaderFailed` flips the mode to 8u: the pipeline and LUT cache rebuild, keyed on the processor `cacheID` and the mode.
- An OCIO VK compile failure: CPU path for that processor, one log line, no retry until the processor changes.
- Device lost (`NATRON_GPU_FAULT=lost@N`): `ViewerHandoff` drops its interop and device images on the main thread and releases its `holdRuntime` token, the viewer uses the CPU path, and it rebuilds after `recoverIfLost`.
- The pool is under pressure while the viewer holds the displayed root: the root is evictable, and the picker reads its RAM copy.
- The colour picker, pixel info or histogram is open: a download happens only when they ask for pixels, and the gate measures with them closed.
- Interop available but the device and driver UUIDs differ, for example Xvfb's llvmpipe GL next to RADV Vulkan: use the Readback rung.
- The ViewerGL widget is destroyed before the render thread's frame arrives.
- Proxy or mipmap levels and pixel aspect ratio: the texture rectangle comes from `roiRoundedToTileSize` as today.

**Assumptions:**
- The 8-bit tolerance is ±1 code value against the CPU path. CPU dithering is already random, and an ordered or hash dither on the GPU stays within ±1 of the same float, so the GPU kernel uses a parallel ordered dither, not row-serial error diffusion.
- Alpha must match exactly. 32f must match within 1e-6·max(1,|v|).
- A region returns only after its compute work has completed, or the hand-off can wait on the timeline value it publishes. P1.T1 and P5.T1 check this.
- One shared Vulkan device and pool: the viewer's command buffers go through `GlInterop`'s own pool on the compute family, the same family the region used, so no ownership transfer is needed.
- No new user setting (hard cut). The hand-off runs whenever the GPU context is available and a rung works.

## Phase 107.1: Spikes

- [x] M107.P1.T1 — Spike: compile OCIO's `GLSL_VK_4_6` display shader inside a compute wrapper through `GpuPipelineCache`
  - files: `Tests/Native/OcioVkSpike_Test.cpp` (new, scratch; folded into P3.T1 or deleted), milestone Decisions entry
  - approach:
    - Build a `GpuShaderDesc` with `GPU_LANGUAGE_GLSL_VK_4_6`. Try `setDescriptorSetIndex(0, <first texture binding>)` with a function name and resource prefix, for the studio config's "sRGB - Display" / ACES 2.0 SDR view (as in `Tests/ViewerDisplayTransform_Test.cpp`), and for a config with a 3D LUT (a `.cube` look).
    - Wrap it in `#version 460` compute code: a storage buffer in, a storage buffer out, push constants.
    - Compile with `runtime.pipelines().get`, then check M126's reflection: uniform block names and offsets (`UniformData::m_bufferOffset`), and the sampler bindings at 1D/2D/3D.
    - Dispatch it on a ramp and compare with `processor->getDefaultCPUProcessor()`.
    - Also confirm, by reading code and running one instrumented case, whether `RegionRun::run` returns after compute has completed (`record()` and `submit`).
  - verify: on lavapipe and RADV (one local container build, `NATRON_DEV_GPU=1`), the shader compiles, both configs bind, and the max error against OCIO's CPU path is recorded (target ≤ 1e-4). A Decisions entry records:
    - the binding scheme;
    - whether dynamic properties come through as a UBO;
    - the compile time when cold and when the disk cache is warm;
    - the answer on region completion.
  - size: M

- [x] M107.P1.T2 — Spike: find which hand-off rung the viewer's actual Qt GL context reaches, under Xvfb and on the bench host's RADV
  - files: `tools/gpu-spike/tests/GlInteropQt_main.cpp` (print `GlInteropCaps` for a compatibility-profile `QOpenGLWidget`), `tools/ci/local/README.md` (a short "RADV GL display for GUI checks" recipe)
  - approach:
    - Run `GlInteropQt` in natron-dev under Xvfb on lavapipe and llvmpipe. Expect HostSync to be best.
    - Run it on the bench host on RADV. Try, in order:
      - an existing desktop session;
      - `weston --backend=headless --renderer=gl` with `QT_QPA_PLATFORM=wayland`;
      - Xorg with a dummy or modesetting config on the 7900 XTX.
    - The goal is a radeonsi GL context, so `uuidMatch` is true and ZeroCopy is best. Record the setup that works. Use `env -u LD_LIBRARY_PATH` because Nuke's Qt shadows ours.
  - verify: each environment's best rung and its `missing` string are recorded in the milestone, with a byte-exact checksum. If no environment reaches ZeroCopy or HostSync on RADV, stop and raise open question 3 before P6.
  - size: M

- [x] M107.P1.T3 — A self-contained script the user runs in a desktop session to check the hand-off rung on radeonsi
  - files: `tools/gpu-spike/desktop-handoff-check.sh` (new), `tools/ci/local/README.md` (short section)
  - approach:
    - Run on the user's AMD host inside a real desktop session (X11 or Wayland), outside any container, with `env -u LD_LIBRARY_PATH`.
    - Arguments: the path to a built `GlInteropQt` binary, or a directory of CI artifacts. Later phases add the release AppImage and the resident comp.
    - Print the session type, `glxinfo -B`/`eglinfo` renderer and `vulkaninfo --summary` device. Run `GlInteropQt` under the desktop's own platform (xcb, then wayland), and collect its `RESULT best=… checksum=… PASS|FAIL` and `missing` lines.
    - Write a one-file report (`handoff-report.txt`) for the user to paste back. It must say plainly whether zero-copy was reached.
    - Fail cleanly, with an explanation, when there is no display, the GL renderer isn't radeonsi, or a binary is missing.
  - verify: `bash -n` and `shellcheck` pass. A dry run in natron-dev under Xvfb produces a report showing llvmpipe and `best=zero-copy-hostsync`. On the user's desktop the report shows radeonsi and `best=zero-copy`, or names what is missing.
  - size: M

## Phase 107.2: GlInterop for the viewer

- [x] M107.P2.T1 — Double-buffer `GlInterop` and make it size- and format-flexible
  - files: `Engine/Gpu/GlInterop.h`, `Engine/Gpu/GlInterop.cpp`, `Tests/Gpu/GlInterop_Test.cpp`
  - approach:
    - Replace the single buffer, semaphores, command buffer and fence with `kSlots = 2` slots. Each slot has its own exported buffer and GL memory object, readback buffer, command buffer, fence and semaphore pair, plus its own release/acquire and `pendingUpload` state.
    - `create(device, loader, capacityBytes, path)`.
    - `GpuStatus produce(const GlFrame& frame, record, std::span<const TimelineValue> waits, int* slot)`, where `GlFrame` is `{width, height, GlPixelFormat::Rgba32F | Bgra8}`. It picks the slot GL is not reading, fails with `VK_ERROR_OUT_OF_POOL_MEMORY` when the frame exceeds the capacity, and waits on the given timeline values in the submit.
    - `upload(slot, texture, x, y)` sets the format and type: `GL_RGBA`/`GL_FLOAT`, or `GL_BGRA`/`GL_UNSIGNED_INT_8_8_8_8_REV`.
    - Keep the queue-family-external bracketing for each slot.
    - HostSync waits on the fence of the slot being uploaded, so producing frame N+1 never waits on GL reading frame N.
  - verify: `GlInteropTest.*` gains two tests for all three paths:
    - `DoubleBufferedProduceAheadByteExact`: produce N+1 before uploading N, byte-exact;
    - `Bgra8SubRectByteExact`, which also covers the capacity-refusal case.
    They pass on lavapipe/llvmpipe (EGL, ZeroCopy skipped with its reason) and on RADV/radeonsi. `UhdLatency` does not regress against M83's 0.79 / 0.87 / 33.2 ms.
  - size: L

## Phase 107.3: Viewer-process kernel

- [x] M107.P3.T1 — Shared OCIO→Vulkan shader helper with a LUT texture cache
  - files: `Engine/OcioVulkanShader.h` (new), `Engine/OcioVulkanShader.cpp` (new), `Tests/Native/OcioVulkanShader_Test.cpp` (new, absorbs P1.T1's spike)
  - approach:
    - `OcioVulkanShader::build(const OCIO::ConstGPUProcessorRcPtr&, functionName, firstBinding)` returns the GLSL text, the texture descriptions and the UBO layout, using the binding scheme P1.T1 found.
    - `OcioVulkanShader::Resources` holds the created `gpu::GpuTexture`s keyed on the processor `cacheID`, created once on the runtime's device. It also has `fillUniforms(std::vector<std::byte>&)` for each frame's dynamic values.
    - Dynamic properties are excluded from the shader hash, so changing exposure doesn't recompile.
    - This is the helper M127.P3.T1 is meant to reuse (see Coordination).
  - verify: the test compiles through `pipelines()` and matches OCIO's CPU path within 1e-4 for the studio display/view, the 3D-LUT look and an identity processor. A second `Resources` lookup with the same `cacheID` makes no new `GpuTexture`. Runs on lavapipe (CI) and RADV.
  - size: M

- [x] M107.P3.T2 — Viewer-process compute kernel with a CPU-reference parity test for every display mode
  - files: `Engine/ViewerGpuProcess.h` (new), `Engine/ViewerGpuProcess.cpp` (new), `Engine/ViewerInstance.h`, `Engine/ViewerInstance.cpp`, `Tests/Native/ViewerGpuProcess_Test.cpp` (new)
  - approach:
    - A GLSL compute template is put together from:
      - the input fetch from a `GpuImageView` (nComps 1–4, `rowStrideFloats`, bounds; (0,0,0,0) outside the bounds);
      - channel select and luminance (0.299/0.587/0.114);
      - the matte, read from the same plane;
      - **8u:** gain and offset, `OCIODisplay(...)` from P3.T1 (or a pass-through), gamma or the threshold, the display transform also applied to the matte, `r += matte/2` as an integer clamp, a hash-based ordered dither to `floatToInt<0xff01>`-equivalent codes, and ARGB packing that matches `toBGRA`;
      - **32f:** channel, luminance and matte only (`r += matte*0.5`), float RGBA out.
    - `ViewerGpuProcess::prepare(processor, mode)` warms the pipeline and LUTs. It is callable from the render thread.
    - `ViewerGpuProcess::record(cmd, input, outBuffer, GlFrame, params)` records the dispatch over the texture rectangle.
    - A compile failure returns a status, and the caller takes the CPU path.
    - Expose the existing CPU tile path as a free function, `viewerProcessRectOnCpu(...)` in `ViewerInstance.h`. It is a thin wrapper over `renderFunctor`, in a hunk away from M100's lines ~583 and ~1426.
  - verify: the parity test sweeps:
    - `DisplayChannels` RGB/R/G/B/A/Y/Matte;
    - nComps 1–4;
    - gain 0.5/4, offset ±0.2, gamma 1/2.2/0;
    - no OCIO, the studio ACES display, and the 3D-LUT look;
    - inputs with ramps, negatives, values above 1 and an off-bounds band.
    In 8u, every RGB code is within ±1 and alpha is exact. In 32f, values are within 1e-6·max(1,|v|). A deliberately broken OCIO source returns a failed status. Passes on lavapipe and RADV.
  - size: L

## Phase 107.4: Viewer integration

- [x] M107.P4.T1 — `ViewerHandoff`: interop ownership, fallback ladder, device loss
  - files: `Engine/ViewerHandoff.h` (new), `Engine/ViewerHandoff.cpp` (new), `Tests/Native/ViewerHandoff_Test.cpp` (new; EGL surfaceless loader copied from `GlInterop_Test.cpp`)
  - approach:
    - A GPU-API-free interface for Gui:
      - `create(GlProcLoader)` probes on the GL thread and picks the rung: ZeroCopy, then HostSync, then Readback, honouring `NATRON_VIEWER_HANDOFF`;
      - `rung()` is atomic, so the render thread can read it;
      - `present(textureIndex, const GpuDeviceImagePtr& root, params, glTexture, GlFrame)` pins the root, runs `produce` (with P3.T2's `record` and the region's timeline wait), runs `upload`, then unpins once the fence signals;
      - `ensureCapacity(bytes)` reallocates slots on the GL thread;
      - `release()` runs with the GL context current.
    - Hold a `GpuContext::holdRuntime()` token only while interop exists. On `runtime->isLost()`, drop everything and report `cpu` until recovery.
    - The returned reason strings feed `incLegacyFallbacks`.
  - verify: headless test on llvmpipe/lavapipe (HostSync and Readback) and on RADV/radeonsi (all three rungs):
    - `present` of a hand-made `GpuDeviceImage` into a GL texture matches `viewerProcessRectOnCpu` within the P3.T2 bounds;
    - a frame over capacity is refused with a reason;
    - `NATRON_GPU_FAULT=lost@N` takes it to `cpu` with no crash or leak (`leakedAllocationsAtLastTeardown() == 0`).
  - size: L

- [x] M107.P4.T2 — Deliver a device root on `FrameRenderContext` and let `ViewerInstance` consume it
  - files: `Engine/FrameRenderContext.h`, `Engine/FrameRenderContext.cpp`, `Engine/ViewerInstancePrivate.h`, `Engine/ViewerInstance.cpp`, `Tests/Native/ViewerDeviceRoot_Test.cpp` (new)
  - approach:
    - `FrameRenderContext::setWantsRootOnDevice(bool)`, `wantsRootOnDevice()`, `setRootDevicePlanes(map<ImageLayerDesc, DeviceImagePtr>)` and `takeRootDevicePlanes()`. This is separate from `FrameStore`, so `finalizeFrames`' `store.clear()` leaves it alone.
    - In `submitViewerFrameToRenderScheduler`, set the flag when `uiContext->handoffRung() != cpu` and the frame qualifies: no auto-contrast, not Matte with a separate alpha layer, not a partial rect.
    - After `future->wait()`, prefer the context's device planes. Put them in `UpdateViewerParams::deviceColor`, skip allocating CPU tile buffers and skip `renderFunctor`, and call `ViewerGpuProcess::prepare` on the render thread so the main thread never compiles.
    - Empty RAM planes with a device plane count as success, not `eViewerRenderRetCodeBlack`.
    - Keep the edits in `renderViewer_internal` (~1655-1720, ~1760-2070) and the submit helper, away from M100's hunks.
  - verify: the test installs a fake scheduler result. A context whose root device plane was set by the test makes `renderViewer_internal` produce params with `deviceColor` set and no RAM tiles. With the flag off, or under auto-contrast, the RAM path is byte-identical to main (existing `ViewerScheduler_Test` and `ViewerDisplayTransform_Test` still pass).
  - size: L

- [x] M107.P4.T3 — `ViewerGL` uploads device roots through `ViewerHandoff`, and the picker and histogram download on demand
  - files: `Engine/OpenGLViewerI.h`, `Gui/ViewerGL.h`, `Gui/ViewerGL.cpp`, `Gui/ViewerGLPrivate.h`, `Engine/ViewerInstance.cpp`
  - approach:
    - `OpenGLViewerI` gains `handoffRung()` and `transferDeviceImageToGPU(root, roiRounded, roi, textureIndex, params)`.
    - `ViewerGL::initializeGL` (`:1205`) creates the `ViewerHandoff`, under `#ifdef NATRON_HAVE_GPU`, with a loader wrapping `QOpenGLContext::currentContext()->getProcAddress`.
    - `transferDeviceImageToGPU` picks the `Texture` data type from `getBitDepth()`, calls `ensureTextureHasSize(roiRounded)` and `handoff->present(...)` into `displayTextures[i].texture`. On refusal it returns false; `updateViewer` then downloads the root once and runs the existing CPU path for that frame.
    - `ViewerGL::~ViewerGL` (`:140`) releases the handoff with the context current.
    - `endTransferBufferFromRAMToGPU` keeps a `DeviceImagePtr` beside `lastRenderedTiles`. `getLastRenderedImageByMipmapLevel` (`:4171`) downloads it lazily, once per frame, under `lastRenderedImageMutex`.
    - `updateViewer` (`ViewerInstance.cpp:2890-2970`) branches on `params->deviceColor`.
    - The 32f GL shader path is untouched.
  - verify: the fast build passes. An Xvfb smoke test (`run-gui-test.sh` with an existing script) shows no regression with `NATRON_GPU=off`. `ViewerHandoff_Test` still passes. The full in-app device path is checked in P6, after P5.
  - size: L

- [x] M107.P4.T4 — Viewer texture cache for handed-off frames (shape depends on open question 2)
  - files: `Engine/ViewerInstance.cpp`, `Engine/ViewerHandoff.cpp`, `Engine/ViewerHandoff.h`, `Tests/Native/ViewerHandoff_Test.cpp`
  - approach:
    - Recommended option (a): when `useTextureCache` holds, `present` also records a copy of the display buffer into the slot's host readback buffer. After the fence signals (next `updateViewer`, or a zero-timer on the main thread), cut it into tiles of `tileSize` and write them into the `FrameEntry`s found with `getTextureOrCreate`, with internal image null. The root itself is still never downloaded.
    - Option (b): skip cache insertion for handed-off frames, and document that already-seen frames render again.
  - verify, option (a): a second display of the same frame is served from the viewer cache with no render, the cached tiles match the displayed texture byte for byte, and the region record still shows `downloadBytes == 0`. Option (b): the test asserts that nothing is inserted.
  - size: M

## Phase 107.5: Root stays on the device (⚑ start after M109 - GPU Region Engine Hardening merges)

- [x] M107.P5.T1 — ⚑ Keep a GPU region's root on the device when the viewer asks for it
  - files: `Engine/GpuRegionTask.cpp`, `Engine/GpuCostModel.h`, `Engine/GpuCostModel.cpp`, `Engine/GpuRegionStrips.cpp`, `Tests/Native/ViewerRootOnDevice_Test.cpp` (new)
  - approach:
    - In `plan()`, when `_context->wantsRootOnDevice()` and the region runs whole, `isRoot` alone no longer sets `cpuConsumer`.
    - In `storeOutputs`, wrap the root slot with `GpuDeviceImage::create`, make it evictable, and call `_context->setRootDevicePlanes`. `rootPlanes` stays empty.
    - Strip runs keep downloading the root, with a "viewer handoff: strips" reason.
    - `planGpuStrips` no longer chooses strips just to hide transfer time when the root stays on the device.
    - `GpuCostModel::regionGain` does not charge the root's download under the flag.
    - The CPU-restart path (`renderMembersOnCpu`) still returns RAM planes. `ViewerInstance` already handles both.
  - verify: a CPU Read feeding Grade, Blur and ColorCorrect on the GPU into the root, on lavapipe (`NATRON_GPU=force`) and RADV:
    - with the flag, `RenderStats::getDownloadCount() == 0`, the region JSONL record has `downloadBytes == 0`, and the device plane's pixels equal the RAM root rendered without the flag;
    - a forced strip height still downloads and is counted;
    - placement accepts the region at a size where it was rejected before.
    M109's `GpuResidency_Test` and `GpuRegionFallback_Test` still pass.
  - size: L

## Phase 107.6: Verification

- [x] M107.P6.T1 — UAT under Xvfb: every display mode, every rung, CPU fallback
  - files: `Tests/gui/m107_viewer_handoff.py` (new)
  - approach:
    - Fast preset in natron-dev under Xvfb. Load the resident comp from P5.T1.
    - For 8u and 32f, × RGB/R/G/B/A/Y/Matte, × gain 2 / gamma 2.2 / gamma 0, × an ACES display/view: grab the viewer framebuffer (`ViewerGL::grabFramebuffer`) under `NATRON_GPU=off` (the reference) and under `NATRON_GPU=force` with `NATRON_VIEWER_HANDOFF=hostsync` and `=readback`.
    - Then: a mid-session OCIO change, an A/B wipe, a pan that grows the viewport, colour picker values, and the histogram drawing.
    - Also run once with `NATRON_GPU_FAULT=lost@40`.
  - verify:
    - `results.txt` has `SUMMARY 0 failure(s)`;
    - each GPU grab is within ±1 code value of the CPU grab;
    - the picker matches the CPU values;
    - after a device loss the viewer keeps updating on the CPU path;
    - with `NATRON_GPU=off` the log has no interop errors (silent fallback);
    - SHOT lines are kept for review.
    GUI bench rule: not triggered. The diff touches `Gui/ViewerGL*` only, none of `NodeGraph*`, `NodeGui*`, `Edge*`, knob expressions or node naming.
  - size: M

- [ ] M107.P6.T2 — Quick directional bench (~1 h) on RADV
  - files: milestone Results section (data in `.local/m107/`)
  - approach:
    - Release AppImages of main and of the M107 branch, run on the same boot (probe `MemAvailable` per case; `env -u LD_LIBRARY_PATH`), in P1.T2's radeonsi display setup.
    - The P5 comp at UHD and 8K, 8u and 32f.
    - Measure viewer latency (request to `imageChanged`) and 48-frame playback fps with the viewer cache cleared, with `NATRON_RENDER_PROFILE` on.
    - Compare the rungs zerocopy, hostsync, readback and cpu.
  - verify: a table of ms per frame and fps for each case and rung, and root region `downloadBytes == 0` on zerocopy and hostsync. A direction call: hand-off beats main by X ms at UHD. If it's slower, stop and diagnose before P6.T3.
  - size: M

- [ ] M107.P6.T3 — Thorough bench matrix on the target host
  - files: milestone Results section
  - approach: 2K, UHD, 8K and the largest texture the viewer allows; 8u and 32f; single input and A/B wipe; cold and warm pipeline cache (cold-compile latency for the OCIO display); with the colour picker or histogram open (on-demand download cost); and option (a)'s cache readback cost if chosen. Pairs from the same boot only.
  - verify: the matrix is recorded with medians over 3 runs. No case is slower than main beyond noise without an explanation written down.
  - size: M

**Verification gate:**
- **CI:** `tools/ci/remote/build.sh fast --tests` passes on lavapipe, including `GlInteropTest.*`, `OcioVulkanShader_Test`, `ViewerGpuProcess_Test`, `ViewerHandoff_Test`, `ViewerDeviceRoot_Test` and `ViewerRootOnDevice_Test`, with the unchanged `ViewerScheduler_Test` and `ViewerDisplayTransform_Test`.
- **RADV:** the local container build (`NATRON_DEV_GPU=1 tools/ci/local/build.sh`) runs the same suites on RADV/radeonsi with ZeroCopy exercised.
- **UAT:** `Tests/gui/m107_viewer_handoff.py` passes under Xvfb on the fast preset. GPU output is within ±1 code value of the CPU path in 8u, within 1e-6 relative before the GL shader in 32f, and alpha is exact, for every display mode.
- **0 root downloads:** on RADV in the radeonsi display setup, the release AppImage displays the resident comp with `NATRON_RENDER_PROFILE` showing `downloadBytes == 0` for the root region and `RenderStats` download count 0 (picker and histogram closed).
- **Silent fallback:** with `NATRON_GPU=off`, or without interop, the viewer displays correctly with no error dialog or log spam.
- **Benches:** P6.T2 and P6.T3 numbers are recorded.
- **Merges:** P5.T1 lands only after M109 merges, and the branch is rebased on main after M100 merges.
- **Not applicable:** GUI bench (`tools/bench/gui/run.sh`), because no `Gui/NodeGraph*`, `NodeGui*`, `Edge*`, knob-expression or naming files change.

## Overlaps with running milestones

| M107 file | Running owner | Handling |
|---|---|---|
| `Engine/GpuRegionTask.cpp` | M109 (+391/−59 on its branch, including `plan()` and `storeOutputs`) | Can't be avoided: the root's download decision lives there. It is kept to one task, **P5.T1 ⚑**, which starts after M109 merges. P3 and P4 are tested with a hand-injected `GpuDeviceImage` root, so they don't need it. |
| `Engine/GpuCostModel.{h,cpp}`, `Engine/GpuRegionStrips.cpp` | M109 | Moved into P5.T1 ⚑, for the same reason. |
| `Engine/RenderScheduler.{h,cpp}` | M109 | **Avoided.** The device root travels on `FrameRenderContext`, which the viewer owns, not on `FrameFuture`. `finalizeFrames`' `store.clear()` doesn't touch it. |
| `Engine/RenderStats.*` | M109 | **Avoided.** Only the existing `incLegacyFallbacks` and `getDownloadCount` are called, and the profile JSONL already carries `downloadBytes`. |
| `Engine/Gpu/GpuRuntime.*`, `GpuBufferPool.*`, `GpuTransfer.*` | M109 | **Avoided.** The existing `pipelines()`, `pool()` and `holdRuntime()` are used. The LUT texture cache lives in `OcioVulkanShader`, not `GpuRuntime`. |
| `Engine/ViewerInstance.cpp` | M100 (6 lines on its branch, at ~583 and ~1426; all M100 tasks are checked and it is in review round 2) | Edited in P3.T2, P4.T2, P4.T3 and P4.T4, in hunks away from M100's (renderFunctor wrapper, the submit helper, ~1655-2070, `updateViewer`). Can proceed in parallel; **rebase over M100 when it merges, before the gate.** |
| `Tests/CMakeLists.txt` | M100 and M109 | **Avoided.** All new tests go in globbed `Tests/Native/`. GlInterop tests stay in the existing `Tests/Gpu/GlInterop_Test.cpp`, and `Tests/Gpu/CMakeLists.txt` is unchanged. |
| `Engine/CMakeLists.txt`, `Engine/Settings.*`, `Engine/AppManager.h` | M100 | **Avoided.** Engine sources are globbed, there is no new setting, and the existing `getGpuContext()` is used. |
| `Engine/Gpu/CMakeLists.txt` | none (M126 merged) | Not needed. The kernel is runtime GLSL, so no Slang kernel is added. |
| `Engine/FrameRenderContext.*`, `Engine/OpenGLViewerI.h`, `Engine/ViewerInstancePrivate.h`, `Gui/ViewerGL*`, `Engine/Gpu/GlInterop.*` | none (checked against both branches) | Free. |

**Coordination (no user decision needed):** M127.P3.T1 should be amended to reuse `Engine/OcioVulkanShader.{h,cpp}` from M107.P3.T1 instead of creating `Engine/Nodes/OCIO/OcioGpuShader.*`. M116's "region feeding the viewer is still a download" note becomes stale once P5.T1 lands.

## Decisions

- 2026-10-18 — **The viewer's OCIO display transform on Vulkan compiles OCIO's own generated GLSL at runtime** (user, at promotion), through M126 - Runtime GPU Shaders' pipeline cache. This matches the M127 - Native OCIO Nodes decision and supersedes the stub's "without runtime shader compilation" options, a baked 3D LUT plus shaper or ported OCIO ops. If the compile fails, the viewer falls back to the CPU display path.
- 2026-10-18 — **Claimed by the gpu PM as the second lane** beside M109 - GPU Region Engine Hardening (user). M108 - Retire Engine GL Host Path would collide with M100 - Content Node Hashing on `Engine/EffectInstance.cpp`. M87 - Point-Op Kernel Fusion would make M109 wait and has little measured gain. M91 - NVIDIA GPU Validation has no host.
- 2026-10-18 — **Elaborated at claim by a consultant**, scouted on `2410277ee`. The user answered its three questions. **Both texture modes are kept:** in 8u the Vulkan kernel applies OCIO's VK shader and dithers, and in 32f the kernel only selects channels while the existing GL shader applies OCIO. **Handed-off frames fill the viewer cache from an asynchronous readback of the display buffer** (P4.T4 option (a)). **The "0 root downloads" gate needs a radeonsi GL display:** if P1.T2 finds none on the bench host, stop and ask the user to set one up rather than accept Readback.
- 2026-10-18 — **M107.P5.T1 is flagged ⚑ and waits for M109 - GPU Region Engine Hardening to merge** (`GpuRegionTask.cpp`, `GpuCostModel.*`, `GpuRegionStrips.cpp`). P3 and P4 test with a hand-made `GpuDeviceImage` root. The `Engine/ViewerInstance.cpp` hunks avoid M100 - Content Node Hashing's lines, and the branch rebases over M100 before the gate. The shared OCIO helper is `Engine/OcioVulkanShader.{h,cpp}`; M127 - Native OCIO Nodes should reuse it instead of creating `Engine/Nodes/OCIO/OcioGpuShader.*`. That change to M127 is plan-scoped and goes through the inbox.
- 2026-10-18 — **M107.P2.T1: the record callback is `GlRecordFn(VkCommandBuffer, VkBuffer dst)`,** because `produce` picks the slot itself. It uses the existing `TimelineValue` from `GpuTransfer.h`. EGL's X11 header defines `None`, which clashes with `ComputeHandoff::None`, so the test includes project headers before EGL, and P4's ViewerGL work must watch include order. The phase build still owes three checks: `UhdLatency` against M83's 0.79 / 0.87 / 33.2 ms, now with two UHD buffers per path; the new tests; and the timeline-wait test, whose host-write-after-submit visibility is not strictly guaranteed by the spec.
- 2026-10-19 — **M107.P1.T1 and P1.T2 code is on the branch but not verified** (`24001a605` spike test, `2a86220e3` caps print). Both wait on one local container build together with P2.T1's owed checks. `GlInteropQt` now asks for Qt's default format instead of an explicit 2.1 compatibility profile, because the app sets no `QSurfaceFormat` and `ViewerGL` gets the default. It prints the context's actual version and profile, plus a `RESULT best=… checksum=… PASS|FAIL` line.
- 2026-10-19 — **Local RADV container build of `2a86220e3` (recovered from the previous session's build agent).**
  - **M107.P2.T1 verified:** `GlInteropTest.*` 14/14 on lavapipe+llvmpipe (EGL surfaceless; best rung hostsync, zero-copy skipped for missing semaphore extensions) and 14/14 on RADV+radeonsi (best rung zero-copy). Double-buffered produce-ahead, including the timeline-wait gate, and Bgra8SubRect are byte-exact on every path. UHD end to end on RADV: zero-copy 0.79 ms, hostsync 0.81 ms, readback 22.59 ms, against M83's 0.79 / 0.87 / 33.2 ms. No regression.
  - **M107.P1.T1, partial:** the OCIO `GLSL_VK_4_6` shader compiles and reflects on lavapipe and RADV. Binding scheme: set 0, UBO reserved at binding 0, OCIO textures from binding 1 (the studio SDR view has two 1D samplers, 363×1, nearest), input storage buffer at 24, output at 25, 8 bytes of push constants. Studio cold compile 135 ms (lavapipe) / 192 ms (RADV), warm disk load 48 / 0.3 ms. Studio dispatch at 2560 px: 0.21 / 0.36 ms. Code reading says `RegionRun::run` returns after compute completes, on both the whole and the pipelined-strip paths; the instrumented case was not run. **Two test defects block the verify:** (1) the CPU reference calls `applyRGBA(float*)`, which transforms one pixel, so only pixel 0 is compared (the worst reported pixel, 11.5 → 0.963989 on the GPU, is 1.5e-6 from PyOpenColorIO's CPU result); fix it with a `PackedImageDesc`. (2) The 3D-LUT config's source colour space is `raw`, which OCIO marks as data, so the display transform becomes a no-op: 127-byte shader, no textures, no uniforms. It needs a non-data source, and a case with dynamic properties so the UBO path is exercised.
  - **M107.P1.T2: stop condition hit.** Under Xvfb, lavapipe with llvmpipe GL reaches hostsync (PASS), and RADV Vulkan with llvmpipe GL reaches only readback because the UUIDs differ (PASS). No environment on this host gives the viewer's `QOpenGLWidget` a radeonsi context: there is no desktop session, no weston or Xorg on the host or in natron-dev, `/dev/dri/card1` is 0660 `nobody`, and the container Qt has only the xcb, offscreen, minimal, linuxfb, vnc and vkkhrdisplay platforms. `xcb_egl` and zink were tried and failed. radeonsi does reach zero-copy through EGL surfaceless, but not through Qt. Per the 2026-10-18 promotion decision, the user is asked to provide a display.
- 2026-10-19 — **The user will run a script in a desktop session to supply the radeonsi check** (answering the P1.T2 stop condition). M107.P1.T3 writes it. P3 and P4 continue meanwhile, and the zero-copy and 0-root-download gates are verified from the user's report.
- 2026-10-19 — **M107.P1.T1 is folded into P3.T1** (`ede5f63d3`), which deleted the spike. `OcioVulkanShader::build` returns a bool, with the shader as an out-parameter, so failures carry an error string. `fillUniforms` and `dynamicProperty()` are on `OcioVulkanShader`, not on `Resources`: OCIO keeps dynamic properties on each GPU shader description, so a caller keeps the shader it dispatches and changes values on it. `glsl()` has no `#version` line, so the caller's wrapper owns the version and its own bindings. The test's LUT case uses a non-data source with a 1D shaper and a 33³ cube, and a dynamic `ExposureContrastTransform` case checks that the kernel is not recompiled. The std140 stride for OCIO's vector uniforms is inferred from the gap before the next uniform, and is the least certain part. Nothing has run on a GPU yet; the phase build verifies it.
- 2026-10-19 — **M107.P1.T3 (`9757efa60`):** `tools/gpu-spike/desktop-handoff-check.sh --glinterop <binary|dir> [--out <dir>]` writes `handoff-report.txt`, whose first line is a `VERDICT`. It counts zero-copy and zero-copy-hostsync both as reached, and names which one. Only `bash -n` has run, because shellcheck isn't in the image. The Xvfb dry run used a stale `GlInteropQt` build that predates the `RESULT` line, so the script falls back to the old output lines; parsing a current build's output is untested. `--appimage`/`--comp` are accepted but not yet supported; P6 adds the 0-root-download check.
- 2026-10-19 — **M107.P3.T2 (`78b67fd8a`):** one pipeline per output format and OCIO `cacheID`, with the rest in an 80-byte push-constant block. Failed compiles are remembered so the CPU fallback doesn't recompile every frame. The 8u codes match `floatToInt<0xff01>` plus a per-pixel hash dither shifted right by 8, computed `precise` (unfused), so the GPU and the CPU's error diffusion are always within one code. **For the review:** with an OCIO processor, gamma 0's threshold is `0.99999`, not 1.0, because OCIO's GPU output for ACES lands just under 1.0 where the CPU lands exactly on it (2.6% of codes flipped). In 32f with 1–3 channels, out-of-bounds alpha is 1, matching the CPU's 32f path. The test reference turns off the 8-bit LUT shortcut. A matte from a separate image stays on the CPU. A device without push descriptors must call `releaseDescriptors()`. A standalone run against a hand copy of the CPU path was within ±1 on RADV and lavapipe, with exact alpha and 32f error ≤ 1.8e-7. The full sweep runs at the phase build.
- 2026-10-19 — **M107.P4.T1 (`0e241dbc0`):** `ViewerHandoff.h` includes no Vulkan, so it has its own `ViewerHandoffRung/Format/Frame/Params/Result` types. `rung()` and `prepare()` work from any thread; every other call, the destructor included, must run on the GL thread with the context current. Each rung is tried with a 4 KB interop, and a slot build that fails later drops to the next rung. A ring is built at its first frame's size. A larger frame is refused with a reason, and the slots grow at the next `present`. A root stays pinned until a non-blocking fence check. **P4.T3 must call `maintain()` on every draw**, even on CPU-path frames, or a device loss is never noticed and the runtime token blocks recovery. **P5.T1:** `present` waits on no timeline value, because `GpuDeviceImage` carries none. That relies on regions returning only after compute completes, which P1.T1 confirmed by reading the code. The loss test uses `injectFault(DeviceLost, 1)` rather than `NATRON_GPU_FAULT`, since calibration uses an unknown number of operations. EGL is `dlopen`ed in the test, to keep `Tests/CMakeLists.txt` untouched. Checked with `-fsyntax-only` only.
- 2026-10-19 — **M107.P4.T2 (`eb0c53adc`):** `UpdateViewerParams` lives in `Engine/UpdateViewerParams.h`, so that file was edited instead of `ViewerInstancePrivate.h`. `OpenGLViewerI` gains `handoffRung()`, defaulting to Cpu, and `prepareHandoff(processor, format)`, defaulting to a refusal, so the render thread reaches the GL viewer's `ViewerHandoff`. **P4.T3 overrides both.** A frame qualifies when the rung isn't Cpu, auto-contrast is off, there is no separate matte layer, the frame isn't a partial update, and it has `nbCachedTile == 0`. That last condition is new: a partial root would drop the cached tiles. A refused prepare counts as a fallback and downloads the root to the CPU path. The test seam is `ViewerInstance::setSchedulerResultHookForTests`. **For P4.T3:** under DEBUG, `OutputSchedulerThread.cpp` (~:3820) asserts every tile has a `ramBuffer`, and device frames now have none. **For P5.T1:** nothing in the scheduler calls `setRootDevicePlanes` yet. Checked with `-fsyntax-only` only.
- 2026-10-19 — **M107.P4.T3 (`dedbb35f2`):** `transferDeviceImageToGPU` returns a `ViewerHandoffResult` rather than a bool, so a refusal's reason can be counted. `UpdateViewerParams` gains `deviceProcess` (`ViewerHandoffParams`) and `deviceStats`. The `ViewerHandoff` is constructed in the `ViewerGL` constructor, so `handoffRung()` is safe from any thread, and `initializeGL` creates its interop. `paintGL` calls `maintain()`. The handoff is released on the context's `aboutToBeDestroyed` and in `~ViewerGL` after `makeCurrent()`. On a refusal, `drawDeviceRootOnCpu` downloads the root once and runs `viewerProcessRectOnCpu` over the part of the RoI the image covers, then uploads through the RAM path. The lazy picker download takes the root out of `lastRenderedDeviceTiles[mip]`, so it happens once per frame. The DEBUG assert in `OutputSchedulerThread.cpp` accepts device frames. Checked with `-fsyntax-only` (NDEBUG, DEBUG and no-GPU); the test files aren't in the fast compile commands.
- 2026-10-19 — **M107.P4.T4 (`5c9e85301`), option (a):** `GlInterop::produce(..., hostCopy)` copies the exported buffer into a host-visible buffer created lazily per slot, on every rung. `readHostCopy` returns `VK_TIMEOUT` while the copy is in flight. `ViewerHandoffParams::readbackToken` asks for a copy, and `takeCompletedReadbacks()` collects finished ones without blocking. `ViewerInstance` polls at each `updateViewer` and on a 25 ms timer, keeps at most 4 pending fills and drops one after 200 polls. Tiles are keyed with the CPU path's `FrameKey` arguments, and their internal image is null. Ring rebuilds or device loss discard pending copies. **Owed to P6.T1's UAT:** whether the colour picker shows values for a frame served from these cache entries. With a null internal image, `endTransferBufferFromRAMToGPU` gets no image. Also owed: the end-to-end check that a second display comes from the cache with no render. Checked with `-fsyntax-only` only.
- 2026-10-19 — **Local phase build of `5c9e85301` (recovered from the previous session's run, `.local/m107/phase-5c9e853/`):** release build succeeds. `GlInteropTest.*` passes 14/14 on lavapipe and RADV in both modes. Seven new tests fail every time, in both modes: on both ICDs `ViewerDeviceRoot.{ADeviceRootReplacesTheRamTiles, AViewerOnTheCpuRungKeepsTheRamPath, AutoContrastKeepsTheRamPath, ARefusedPrepareDownloadsTheDeviceRoot}`, `ViewerGpuProcess.EightBitMatchesTheCpuInEveryDisplayMode` and `ViewerHandoff.ReadbackMatchesTheTextureOnEveryRung`; on RADV only `OcioVulkanShader.ShaperAnd3DLutLook`. The full ctest run was cut off at 651/2556. The desktop hand-off script's Xvfb run in the container reports readback (UUID mismatch, llvmpipe GL), as expected; the user's desktop run of the script is still owed. A fixer is diagnosing the seven failures; remote CI run 39344529046 checks the same commit on lavapipe.
- 2026-10-19 — **The seven phase-build failures were test defects** (`99903f3cb`; no engine bug found). (1) `ViewerDeviceRoot.*` never passed SetUp: `AppManager.cpp:1693` registers the Viewer only outside background mode, so the test registers it itself, as internal-use-only so plugin sweeps skip it. The test bodies have never executed before this fix. (2) `EightBitMatchesTheCpuInEveryDisplayMode`: every mismatch was gamma 0 with OCIO, CPU 0 and GPU 255, in the agreed [0.99999, 1) band. The test skips and counts channels whose pre-threshold CPU value is within [0.99999, 1.00001]; everything else is held to ±1 with exact alpha. (3) `ReadbackMatchesTheTextureOnEveryRung`: the float pass hit the designed one-frame refusal while ring 0 grew; the test now expects it. (4) `ShaperAnd3DLutLook` on RADV: 1.16e-4 against 1e-4 is the 8-bit subtexel precision of linear filtering on the steep 1D shaper (lavapipe gives 1.19e-7). The LUT case's tolerance adds the largest texel step × 2^−subTexelPrecisionBits, about 4.9e-4. Checked with `-fsyntax-only`; lavapipe runs on remote CI, RADV locally after M109's bench.
- 2026-10-19 — **Remote CI of `99903f3cb` (run 39347232956) is green on lavapipe:** 2554/2554 in both scheduler modes, with `ViewerDeviceRoot.*` (4, running for the first time), `ViewerGpuProcess.*` and `OcioVulkanShader.*` passing. CI has no EGL, so `ViewerHandoff` present/readback/capacity/device-loss and `GlInteropTest.*` skip there; they are owed to the local RADV and llvmpipe run. The artifact download failed again (blob host unreachable from this sandbox).
- 2026-10-20 — **Local run of `99903f3cb` (recovered, `.local/m107/phase-99903f3/`):** `GlInteropTest.*` 14/14 and `ViewerDeviceRoot.*`, `ViewerGpuProcess.*`, `OcioVulkanShader.*` (including `ShaperAnd3DLutLook` on RADV) pass on lavapipe+llvmpipe and RADV+radeonsi in both modes. `ViewerHandoff.ReadbackMatchesTheTextureOnEveryRung` still fails everywhere: at `ViewerHandoff_Test.cpp:755` the float pass's `present` succeeds where the test expects the one-frame refusal, so the earlier fix's assumption was wrong. A fixer is on it.
- 2026-10-20 — **`ViewerHandoff.ReadbackMatchesTheTextureOnEveryRung` fixed in the test (`2dfb54c56`), syntax-checked.** The engine was right. The guard at `:754` also fired on the first (Bgra8) pass, where `capacity(0)` is 0 because no ring exists yet and `present` builds one at the frame's size and accepts it. The test now expects the first pass to build the ring and the float pass to be refused once ("over"), with capacity unchanged, then grown by the next `present`.
- 2026-10-20 — **The desktop check can't use the container-built `GlInteropQt`:** the host has no Qt6, so its Qt libraries are not found. A bundling script that ships the probe with the container's Qt libraries and platform plugins (host GL, EGL, Vulkan and drivers excluded) is being added beside `desktop-handoff-check.sh`.
- 2026-10-20 — **Desktop probe bundle (`990bb8099`).** `tools/gpu-spike/make-probe-bundle.sh` runs in natron-dev and packs `GlInteropQt` with its Qt closure, the xcb platform plugin and xcb GLX integration (RUNPATH via patchelf, `qt.conf`), skipping `tools/release/excludelist.txt` plus libstdc++, libgcc_s and libvulkan, so the host's GL, EGL, Vulkan and drivers are used. `desktop-handoff-check.sh --glinterop` takes the bundle dir or its `.tar.gz`. On host Xvfb it starts and reports `best=readback-pbo` PASS (llvmpipe GL beside RADV Vulkan, UUID mismatch). **The container's Qt has no Wayland platform plugin and no xcb EGL integration,** so the probe is xcb/GLX only; a Wayland session is checked through XWayland. That matches the app, which ships the same Qt.
- 2026-10-20 — **M107.P1.T2 done: the viewer's Qt GL context reaches zero-copy on the user's desktop** (`.local/m107/desktop-check/2026-10-20-x11/`). X11 session, `DISPLAY=:0`, Qt xcb, GL 4.6 compatibility profile on radeonsi (Mesa 26.2.3, kisak PPA) beside RADV Vulkan. Every Vulkan and GL external memory and semaphore extension is present, device and driver UUIDs match, and all three rungs are available; best is zero-copy, with the texture and render checksums byte-exact (max diff 0). With the earlier Xvfb results (lavapipe+llvmpipe: hostsync; RADV+llvmpipe: readback), every rung is now recorded. The stop condition is cleared: P6's zero-copy and 0-root-download gates run in this desktop session, through the same script extended with the release AppImage and the resident comp.
- 2026-10-20 — **Local run of `990bb8099` (`.local/m107/phase-990bb80/`, fast preset): every M107 suite is green on both ICDs, in both modes.** `ViewerHandoff.*`, `ViewerDeviceRoot.*`, `ViewerGpuProcess.*` and `OcioVulkanShader.*` pass 16/16 on RADV and lavapipe, with no skips. `GlInteropTest.*` passes 14/14 on RADV; on lavapipe its 4 zero-copy cases skip for missing semaphore extensions. `ViewerHandoff` ran zero-copy on RADV+radeonsi (EGL) and hostsync on lavapipe+llvmpipe, and the readback test passes on every rung. The RAM path is unchanged: `ViewerScheduler*` and `ViewerDisplayTransform*` pass 18/18 on each ICD. Remaining work: P5.T1 ⚑ (after M109 merges), then P6.
- 2026-10-20 — **M107.P5.T1 (`a9fcf5b28`), recovered from the previous session's implementer, which built and verified it before the session ended.** `ViewerRootOnDevice.*` (6 cases), `GpuResidency`, `GpuRegionFallback`, `GpuPlacement`, `GpuCostModel`, `GpuRegionStrips`, `ViewerHandoff` and `ViewerDeviceRoot` pass 105/105 on lavapipe and RADV in both scheduler modes (`.local/m107/p5t1/`). Deviations from the brief: `GpuDeviceImage::evictToRam()` now keeps its RAM copy and `downloadToRam()` returns it, because the viewer holds the root outside the frame store and an evicted root would otherwise lose its pixels (`Engine/GpuContext.{h,cpp}`). `planGpuStrips` is now `planOverlap`, which already refuses to strip when a slot is `keptOnDevice`, so `GpuRegionStrips.cpp` needed no change, only the doc in `GpuRegionStrips.h`. `GpuCostModel::regionGainMs` takes a required `rootOnDevice` (hard cut), passed from `makeRenderPlacementInputs` in `Engine/GpuPlacement.cpp`; the two `GpuPlacement_Test` calls pass `false`. An identity root sharing an input's slot still downloads. "viewer handoff: strips" is counted only on a successful run.
- 2026-10-20 — **P6.T1 first UAT run (script uncommitted).** Build green. Every config failed the same 15 `32f-sweep` checks, including the CPU reference with `NATRON_GPU=off`: after the A/B wipe case, a Grade `multiply` change no longer re-renders the viewer. The device-loss run passed, but nothing confirmed a loss happened. The active rung is not observable from the log. Disconnecting B after the wipe leaves the viewer black, and the 200% zoom doesn't repaint until a mouse event. A debugger is checking lane vs main (M100 - Content Node Hashing is a suspect) and adding a rung log line.
- 2026-10-21 — **P6.T1 UAT green, pending a tolerance sign-off** (`f0e21186b`..`0034da8c0` on milestone/m107-gpu-viewer-hand-off, rebased on `1c0b5ab3b`; `.local/m107/p6t1c/full/`). cpu 372, hostsync (lavapipe) 502, readback (RADV) 502 and `lost@40` 73 checks, all `SUMMARY 0 failure(s)`; the M107 ctest suites 54/54 on RADV and lavapipe. The three first-run symptoms: (1) the sweep "not re-rendering" was a script defect — connecting B to viewer input 1 makes it the active A input, so later frames showed B; the script now sets A back to input 0. (2) Disconnecting the active input leaves A at -1 and the viewer black even with input 0 connected — a pre-existing bug on main in `Engine/NodeInputs.cpp`, outside this milestone, avoided by the UAT (repro `.local/m107/p6t1c/viewer_ab_zoom_repro.py`). (3) `ViewerGL::zoomSlot` never called `update()` — on main too, fixed here (`2773bbbe0`). The earlier fault run had no Vulkan device; it now asserts from the new rung log line that hostsync drops to `cpu (device lost)` and all 25 later sweep steps change the picture. P4.T4's owed checks pass: a second display is served from the hand-off cache with no render, and the picker reads correct values on it. **Found:** on a cache hit the viewer's last rendered image is still the previous device frame's root, so output picking (`getColorAt`) and probably the histogram would read the wrong frame; a fixer is on it. **Deviation needing the user:** 8u GPU grabs differ from the CPU grab by 2 on 0.01–0.13% of pixels (dither: the CPU diffuses error from a random start column per row, the GPU hashes by position; GPU 8u is within 1 of the CPU's 32f grab), so the script allows ±2 on ≤ 0.5% of pixels in 8u; 32f stays ±1.
- 2026-10-21 — **8u UAT tolerance accepted (user):** an 8u GPU grab may differ from the CPU grab by 2 code values on at most 0.5% of pixels (`M107_DITHER_SHARE`), because the two dithers differ; 32f stays ±1. This relaxes the gate's "±1 code value in 8u" accordingly. The `NodeInputs.cpp` active-input bug goes to the inbox for planning (user).
- 2026-10-22 — **The cache-hit picker fix is committed (`0f5cb8204`; recovered from the previous session's fixer, run data in `.local/m107/p6t1d/`).** A cache hit whose tiles have no image now clears the last rendered images. Picking, pixel info, `getColorAt` and the histogram then render the displayed frame on demand through `NodeColorSampler::renderNodeImage`. The bug was on the CPU path too. The UAT now checks output picking on the cache hit and compares the histogram per pixel with the CPU run.
  - **The full UAT of this exact tree passes for cpu, hostsync and readback:** `SUMMARY 0 failure(s)`, cache-hit output pick 0.61643 as on the CPU, histogram 0 of 109220 pixels differ.
  - **Still owed:**
    - the M107 ctest suites on RADV and lavapipe;
    - the `lost@40` fault config, which fails (the loss never fires) only with the fresh `XDG_CACHE_HOME` that `run.sh` creates and passes with the container's cache dir;
    - a check that the GPU-rung runs really rendered on the device, since a loss that never fires suggests little device work.
  - **Accepted cost:** on a hand-off cache hit, an open histogram or output picking renders the viewed node synchronously on the main thread, as the input picker already does.
- 2026-10-22 — **Rebased onto main `3c2ba44fc` (M120, M141, M155) as `22f88b57e`.** One conflict, in `tools/ci/local/README.md`: M141's "Worktree lanes" section and this branch's "RADV GL display for GUI checks" section both stay, main's first. No C++ conflicts. Remote `fast --tests` (the lavapipe CI gate line) and a `release` AppImage for P6.T2 are dispatched together.
- 2026-10-22 — **Gate line "CI" passes on `22f88b57e`** (remote `fast --tests`, run 39716306181, lavapipe): 329/329 ctest entries in both scheduler modes, among them `Gpu.GlInteropTest`, `OcioVulkanShader`, `ViewerGpuProcess`, `ViewerHandoff`, `ViewerDeviceRoot`, `ViewerRootOnDevice`, `ViewerScheduler`, `ViewerDisplayTransform` and `ViewerDisplayTransformOCIO`. Still owed locally: the same suites on RADV, the `lost@40` config under a fresh `XDG_CACHE_HOME`, the device-work check, and P6.T2/P6.T3.
- 2026-10-22 — **M107.P6.T1 done (`46604dcba`; `.local/m107/p6t1e/`).** **RADV gate line:** the 9 M107 suites pass on RADV NAVI31/radeonsi in both scheduler modes, 54 cases each, none skipped, all 14 GlInterop cases including the six ZeroCopy/ZeroCopyHostSync ones (best path zero-copy). They ran from the remote `build-tree-fast` artifact of `22f88b57e` inside natron-dev-gpu, with no local build. **UAT** on the remote fast AppImage, fresh home and cache: cpu, hostsync (lavapipe), readback (RADV) and `lost@40` (RADV) each `SUMMARY 0 failure(s)`. **`lost@40` was a script defect, not cache-dependent:** on lavapipe the cost model learns the device is slower than the CPU after the first region and moves the sweep to the CPU, so fewer than 20 device operations happen and the fault never fires. On RADV, 31 of 33 fault frames ran on the device and the rung went hostsync → cpu (device lost) → hostsync. The script now says to run the fault config on a hardware device. **Device work:** readback on RADV ran 41 GPU regions, and all 39 root regions downloaded 0 bytes. Hostsync on lavapipe was device-rooted on frame 0 only. Zerocopy can't be reached under Xvfb (llvmpipe GL), so ctest covers it. **Accepted by the PM, flagged to the user in the run report:** hostsync's device-root UAT coverage on lavapipe is one frame, and its per-frame hand-off is covered by `ViewerHandoff`/`ViewerRootOnDevice` on both ICDs. **Environment note:** natron-dev's own `Natron.conf` holds a stale `gpuDevice` UUID, so a run without its own `XDG_CONFIG_HOME` silently gets no GPU compute.
