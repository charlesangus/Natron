

### 2026-10-24T01:22:11+00:00 — change-request
- refs: M119
- Also for the trackball/global mode control: 
  
  - global mode should make the cursor a crosshair (pitch some designs)
  - the hud can follow the cursor, offset below and to the right
  - would like to try out some various other UI designs for global mode, like the trackball appearing in C mode when you click, and similar images for the other modes that give feedback on what you're doing
  - could also try making the cursor change to that UI (trackball etc) 
  
  Pitch some designs.

### 2026-10-24T11:00:00Z — info
- refs: none
- Pre-existing race on main, found while diagnosing a `NativeWriteReadBackTest` flake in M138 - GUI Papercuts (reproduced about 58 failures in 480 suite runs with 8 parallel copies on 4 cores). `BlockingBackgroundRender::blockingRender` (`Engine/BlockingBackgroundRender.cpp:53-72`) returns when `notifyRenderFinished()` fires inside the scheduler thread's `threadLoopOnce` (`Engine/OutputSchedulerThread.cpp:2823`, `Engine/OutputEffectInstance.cpp:356`), but the thread only marks itself idle after that returns (`Engine/GenericSchedulerThread.cpp:523-526`). In the gap, `RenderEngine::isDoingSequentialRender()` is still true, so `NativeWrite::readBackActive()` (`Engine/Nodes/IO/NativeWrite.cpp:1396`) reports false and `EffectInstance.cpp:521` aborts renders on the Write. M138 worked around it in the test (wait for `isDoingSequentialRender()` to go false). An engine fix — `blockingRender` also waiting for the scheduler to go idle — needs care: if `notifyRenderFinished` starts the next queued sequence, the thread stays active and the wait could hang. Other callers that inspect a Write right after `startWritersRendering` may hit the same race.

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
