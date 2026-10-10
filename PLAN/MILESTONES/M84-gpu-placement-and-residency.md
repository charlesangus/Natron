# M84 - GPU Placement And Residency

The task-graph scheduler learns to place work on the CPU or the GPU, and to keep GPU results resident between nodes. It uses the backend M83 - GPU Compute Backend Spike chose (Vulkan 1.3 compute, Slang kernels compiled offline to SPIR-V, hand-written CPU kernels as the fallback; design note `PLAN/DESIGN/2026-10-10-gpu-compute-backend.md`).

- **Backend:** the spike's backend moves into `Engine/Gpu/` and its tests into `Tests/Gpu/`, built by the main build and covered in CI on lavapipe. The gaps the design note lists get filled: buffers over VMA, async chunked transfers, a kernel registry, a pooled VRAM budget with LRU eviction, and a device-lost/OOM state machine.
- **Host memory:** frames are allocated in importable host memory, and `FrameStore` gains device images.
- **Placement:** a pass after `buildGraph` marks GPU tasks and groups them into resident regions from day one (user decision `2026-10-10-gpu-residency-from-day-one`). It uses a cost model built from measured per-plugin costs and calibrated bandwidth.
- **Execution:** each region runs as one super-task. It has a thread budget of 1, one upload per external input and one download per output that has a CPU consumer. It is tiled into strips on the GPU when a buffer exceeds `maxStorageBufferRange`.
- **Nodes:** to exercise placement, Grade (without mask, mix or unpremult) and the FIR Gaussian Blur get GPU paths on the spike's verified kernels. Every other node, and the viewer hand-off, belong to M85 - Native GPU Kernels (see Decisions).
- **Default mode:** GPU compute is Auto, so it is on whenever a real GPU is present (user).

Scouted facts this plan relies on (2026-10-10, `main` at `938e77662`, spike at `3cefeff73` on PR #48):
- **Scheduler (`Engine/RenderScheduler.{h,cpp}`):**
  - `FrameGraph::Task` is at `RenderScheduler.h:60-99`. It has no device or region fields.
  - `buildGraph` is at `RenderScheduler.cpp:461-589`. The stub's 238-366 is stale.
  - `executeTask` (`:909`):
    - hard-codes `eStorageModeRAM` in its `RenderRoIArgs` (`:933`);
    - fails a task whose planes are GL textures (`:957-967`);
    - then calls `store.release`/`store.put` (`:988-1002`).
  - `popLocked` (`:827`) computes the thread budget via `computeTaskBudget` (`:727`, call at `:888`).
  - `admitGatedLocked` (`:1151`) admits against a single `_bytesBudget`.
  - `bytesInFlightLocked` (`:1185`) sums `FrameStore::bytesInFlight()` over the active frames.
  - `_bytesBudget` is half the cache's RAM share (`:446-451`).
  - The profile sink is `ProfileSink` (`:161`). `formatTaskRecord` (`:322-375`) writes `frame/task/plugin/node/time/view/mipmapLevel/roiPixels/components/bitDepth/deps/estimatedBytes/wallNs/pointOp/glSupport/ok`.
- **`FrameStore`** is in `Engine/FrameRenderContext.h:53-143`. Entries hold `std::map<ImageLayerDesc, ImagePtr>`, so they are RAM only. CPU consumers read them via `EffectInstance::lookupFrameStore` (`Engine/EffectInstance.cpp:865`; callers at `:921` and `:1381`).
- **The CPU/GPU/GL decision in `renderRoI`** is at `Engine/EffectInstanceRenderRoI.cpp:694-786`. The `visitsCount > 1` refusal is at `:708`, and GL output skips the cache at `:787-792`.
- **The RAM image allocator** is `RamBuffer::resize` (`Engine/CacheEntry.h:107-123`). It uses plain `malloc`, which is not page-aligned.
- **Native nodes:**
  - `NativeImageEffect::isPointOp` is at `Engine/Nodes/Image/NativeImageEffect.h:128` and `makeKernel` at `:152`. `PixelKernel` is row-based.
  - `GradeKernel` is in `Engine/Nodes/Color/Grade.cpp:59`, and `Grade::makeKernel` at `:306`.
  - Blur is `PLUGINID_NATRON_BLUR "net.sf.cimg.CImgBlur"`. Its knobs include `filter` (FIR Gaussian by default since M88), `orderX/Y`, `boundary`, `alphaThreshold`, `expandRoD` and `cropToFormat`.
  - `Blur::render` (`Blur.cpp:572`) takes its threads from `getNCPUsAvailableForEffect()` (`:738`).
  - `EffectInstance::supportsOpenGLRender` defaults to None (`Engine/EffectInstance.h:1770`).
- **The viewer** does its display conversion on the CPU (`scaleToTexture8bits`/`32bits`, `Engine/ViewerInstance.cpp:106/111`) before `ViewerGL::transferBufferFromRAMtoGPU` (`Gui/ViewerGL.cpp:1557`).
- **Build:**
  - The root build is C++20 (`CMakeLists.txt:29`) and calls `add_subdirectory(Engine)` at `:154`. `cmake/` holds only `NatronBundleAssets.cmake`.
  - `Engine/CMakeLists.txt:36-44` globs `Engine/*.cpp` non-recursively plus `Nodes/**`, so `Engine/Gpu/` needs its own CMake file.
  - `Tests/CMakeLists.txt` uses an explicit source list (`:21-110`) plus a `Native/*_Test.cpp` glob (`:114`). Tests use the bundled `google-test/` and are discovered with `PRE_TEST` (`:188-189`).
- **CI and image:**
  - CI (`.github/workflows/ci.yml:26-28`) runs in `aswf/ci-vfxall:2027-clang21.1`, the same base image as natron-dev (`tools/ci/local/Dockerfile` is a bare `FROM`). The ctest steps run debug in both scheduler modes (`:157-166`).
  - The image has `vulkan-headers`/`vulkan-loader` 1.4.328, `mesa-vulkan-drivers` 25.2.7 (`lvp_icd.x86_64.json`) and gtest 1.11. It has no `slangc`.
  - RADV fails inside the container because the clang `libLLVM` in `/usr/local` lacks the AMDGPU target and shadows the system copy. The spike's GL test worked around this with `LD_PRELOAD=/usr/lib64/libLLVM.so.21.1`.
- **Spike (`tools/gpu-spike/`, PR #48):**
  - `src/GpuDevice.h` exposes queue families and extension flags, but no device limits.
  - `maxStorageBufferRange` is read only in `bench/GpuBench_main.cpp:761`, and the 512 MB copy chunking lives only there (`:547`).
  - Strip units with halos are built by `makeUnits` (`:93`), and tiled output is checked against whole-frame output by `verifyTiling` (`:677`).
  - `GpuTransfer` reads `minImportedHostPointerAlignment` (`src/GpuTransfer.cpp:345`) and exposes only a blocking `process()`.
  - `kernels/grade.slang` takes `a/b/gamma/invGamma/nComps/channelMask/flags`, with no mask or mix.
  - `kernels/blur.slang` takes whole-image `width/height/channels/radius/vertical/neumann`, with no src/dst offsets or row stride. `kMaxRadius` is 512.
  - The spike binaries link libstdc++ statically so they run on the host.
- **The stale GL statement:** the Risks paragraph of `DECISIONS/2026-10-04-task-graph-render-scheduler.md` still says "OpenGL … stay on the legacy path in v1", but M63.P5.T3 (`9d5b0997f`) moved GL nodes into tasks on per-thread private contexts.

## Phase 84.1: Early measurements and environment

- [ ] M84.P1.T1 — Re-measure the in-graph CPU Blur cost against the bare FIR kernel, and find where the threads go
  - files: `tools/bench/` (only if a bench script needs a flag); results go in this file's `## Decisions`
  - approach:
    - Use a release build of `main` in natron-dev with 16 threads.
    - Run `graph_bench.py` with `BENCH_TOPO=blurchain BENCH_N=1` at UHD and 8K, `BENCH_BLUR_SIZE` for σ 3/25/100 (FIR is the default), `NATRON_RENDER_PROFILE` and `BENCH_RENDER_STATS=1`. Also run once with `noRenderThreads=1` to get the serial cost.
    - Compare against the bare FIR numbers from `CpuTwinBench`: 1.44 / 5.5 / 19.9 ms/Mpx.
    - Sample thread occupancy with `tools/bench/sample_states.sh`. Find where the gap comes from: the thread budget (`computeTaskBudget`), the serial parts of `fillBuffer`/`writeWindow`, `renderRoI` overhead, or chunking in `forEachLineChunk`.
    - Take about 1 hour. Diagnose, don't fix. A fix goes in here only if it is a few lines in `Blur.cpp`. Anything bigger becomes its own milestone (user, 2026-10-10).
  - verify: a Decisions entry records:
    - the in-situ ms/Mpx per σ and size;
    - the thread occupancy;
    - the serial cost;
    - the likely cause;
    - the per-plugin CPU numbers that P6.T2's cost table will use.
  - size: M

- [ ] M84.P1.T2 — Measure host-import registration cost and churn on RADV
  - files: `tools/gpu-spike/bench/ImportCost_main.cpp` (new), `tools/gpu-spike/CMakeLists.txt`
  - approach:
    - Allocate page-aligned buffers of 1 MB, 133 MB (UHD), 530 MB (8K) and 4 GB.
    - For each size, time the import (`vkAllocateMemory` with `VkImportMemoryHostPointerInfoEXT`, plus buffer create and bind) and the free.
    - Compare three cases:
      - (a) fresh `malloc`, first touch, then import, every frame;
      - (b) a recycled buffer whose import is cached;
      - (c) the same with staging.
    - Run a 32-frame churn loop and report the per-frame overhead against the 9.9 ms UHD host-import transfer.
    - Run it on the host through `run-host.sh`, and once on lavapipe.
  - verify: a Decisions entry records pin and unpin time per size and churn per frame, and decides P4.T1's allocator policy:
    - if pin plus unpin takes under 1 ms per UHD buffer, cache imports per live allocation;
    - otherwise, keep a recycling pool for large aligned buffers.
  - size: M

- [ ] M84.P1.T3 — Make RADV work inside the natron-dev container
  - files: `tools/ci/local/Dockerfile`, `tools/ci/local/devshell.sh`, `tools/ci/local/README.md`
  - approach:
    - Run a second container with `--device /dev/dri` and the render group.
    - First try `LD_PRELOAD=/usr/lib64/libLLVM.so.21.1`. If that isn't clean (GL, Vulkan and the full ctest all working), fix the image in `tools/ci/local/Dockerfile` instead (user, 2026-10-10). For example, give Mesa its own loader path or relocate the clang `libLLVM`. The clang toolchain and the CI image must keep working.
    - Add a `--gpu` option to `devshell.sh` that sets all of this up, and document it in the README.
    - The host's `/dev/dri` permissions don't survive a reboot (see the design note). Document the host udev rule.
  - verify:
    - The spike's `DeviceList` lists the RX 7900 XTX with a transfer-only family inside the container.
    - A full engine ctest runs there with RADV selected, and a CPU-only ctest is unchanged.
    - The CI image is unaffected: CI is green.
  - size: L

- [ ] M84.P1.T4 — Fix the stale GL statement in the M63 decision record
  - files: `.plan/PLAN/DECISIONS/2026-10-04-task-graph-render-scheduler.md`
  - approach:
    - In its Risks paragraph, take OpenGL out of the "stay on the legacy path in v1" list.
    - Add one sentence saying that GL-capable nodes render inside tasks on per-thread private contexts (`GPUContextPool::getOrCreateContextForCurrentThread`, M63.P5.T3, `9d5b0997f`), and that a task fails if a GL texture would reach the RAM-only store.
    - Roto strokes, analysis renders and deep-output writers stay listed.
  - verify: the paragraph matches M63's P5.T3 entry, and the `INDEX.md` summary is still accurate.
  - size: S

## Phase 84.2: Promote the backend into the main build

- [ ] M84.P2.T1 — Build `Engine/Gpu/` (`NatronGpu`) from the spike sources, with Slang in the root build
  - files: `CMakeLists.txt`, `cmake/FetchSlang.cmake`, `cmake/SlangKernels.cmake`, `Engine/Gpu/CMakeLists.txt`, `Engine/CMakeLists.txt`, plus the moved `Engine/Gpu/{GpuDevice,GpuKernel,GpuTransfer,GlInterop}.{h,cpp}`, `Engine/Gpu/Vma.cpp` and `Engine/Gpu/Kernels/{grade,blur,pointops,gradeops}.slang` (moved as they are, not rewritten)
  - approach:
    - In the root build, add `option(NATRON_GPU "Vulkan compute backend" ON)` and `find_package(Vulkan)`. If Vulkan is missing, print a status message and turn the option off.
    - Copy the cmake helpers into `cmake/` and rename `GPU_SPIKE_*` to `NATRON_SLANG_*`. Keep the pinned v2026.19 SHA256, `NATRON_DEPS_CACHE` and `SLANG_ROOT`.
    - `slang_add_kernel` emits SPIR-V only by default, with a `CPP` flag to opt into the C++ target for oracle tests. `slang_add_glsl` stays in tools.
    - `Engine/Gpu/CMakeLists.txt` builds `NatronGpu`:
      - a static, engine-free C++20 library: no Qt, Python or Engine headers;
      - `libs/VulkanMemoryAllocator/include` as a system include;
      - exactly one TU with `VMA_IMPLEMENTATION`;
      - it must also build when included from `tools/gpu-spike` (P2.T4).
    - `NatronEngine` links `NatronGpu` PUBLIC and gets `NATRON_HAVE_GPU=1`.
  - verify:
    - A debug build in natron-dev is green, and `-DNATRON_GPU=OFF` also configures and builds.
    - `ldd NatronRenderer` shows `libvulkan.so.1` as the only new dependency.
    - A targeted existing ctest (`RenderScheduler*|FrameStore*`) is unchanged.
  - size: M

- [ ] M84.P2.T2 — `Tests/Gpu/` as the host-runnable `GpuTests` binary
  - files: `Tests/Gpu/CMakeLists.txt`, `Tests/Gpu/*_Test.cpp` (moved from the spike: GpuDevice, GpuKernel, GpuTransfer, GlInterop, GradeGpu, BlurGpu, PointOpCompose, GradeCpu, BlurCpu), `Tests/Gpu/{GpuHarness,Tolerance,GradeFixture,BlurFixture}.h`, `Tests/Gpu/ref/{GradeRef,BlurRef}.h`, `Tests/Gpu/kernels/{fill,smoke}.slang`, `Tests/CMakeLists.txt`
  - approach:
    - Build one `GpuTests` executable that links only `NatronGpu` and the bundled `Tests/google-test`, with `-static-libstdc++ -static-libgcc`, so it runs on the host.
    - Every test skips with a reason when there is no device. `NATRON_TEST_REQUIRE_GPU=1` turns those skips into failures, like `NATRON_TEST_REQUIRE_GL` does (`Tests/GLScheduler_Test.cpp:90`).
    - Carry over the spike's GlInterop test environment.
    - Keep the double references for ULP diagnostics. Parity with native nodes moves to P5.T2 and P5.T3.
  - verify:
    - In the container: `VK_ICD_FILENAMES=…/lvp_icd.x86_64.json NATRON_TEST_REQUIRE_GPU=1 ctest -R '^Gpu'` is green.
    - On the host: `run-host.sh build/debug/Tests/Gpu/GpuTests` is green on RADV, including semaphore interop.
  - size: M

- [ ] M84.P2.T3 — GPU coverage in CI on lavapipe
  - files: `.github/workflows/ci.yml`
  - approach:
    - Cache `.natron-deps` with `actions/cache`, keyed on the Slang version and SHA, and set `NATRON_DEPS_CACHE` to it.
    - Set `VK_ICD_FILENAMES` to lavapipe and `NATRON_TEST_REQUIRE_GPU=1` on both ctest steps.
    - Keep everything in the same job.
  - verify: CI on the branch is green, the `GpuTests` cases show as passed in the log (not skipped), and the job's wall-time change is recorded in Decisions.
  - size: S

- [ ] M84.P2.T4 — Rebuild the spike's tools on the promoted code
  - files: `tools/gpu-spike/CMakeLists.txt`; delete `tools/gpu-spike/{src,cmake,tests/*_Test.cpp,tests/*.h,ref}` and the moved kernels
  - approach:
    - The spike project includes the root `cmake/` helpers and `add_subdirectory(../../Engine/Gpu)`.
    - These stay in the spike, built against `NatronGpu`: `GpuBench`, `CpuTwinBench`, `GlCompute`, `DeviceList`, `GlInteropQt`, `ImportCost` (P1.T2), `run-host.sh` and `run-bench.sh`.
    - Run this after P1.T2, since both edit the spike's CMakeLists.
  - verify: the spike configures and builds in the container, and `GpuBench --sizes uhd` runs on lavapipe.
  - size: S

## Phase 84.3: Finish the backend (design note §5 "new" items)

- [ ] M84.P3.T1 — Device limits and the `GpuBuffer` RAII layer
  - files: `Engine/Gpu/GpuDevice.{h,cpp}`, `Engine/Gpu/GpuBuffer.{h,cpp}` (new), `Tests/Gpu/GpuBuffer_Test.cpp`, `Tests/Gpu/CMakeLists.txt`
  - approach:
    - `GpuDeviceInfo` gains `maxStorageBufferRange`, `minImportedHostPointerAlignment`, the device-local heap size, and `unifiedMemory` (integrated GPU, or a device-local and host-visible heap that covers device memory).
    - `GpuDeviceOptions` gains test overrides for `maxStorageBufferRange` and the copy chunk size.
    - `GpuBuffer {VkBuffer, VmaAllocation, size, kind}` comes in four kinds: device-local, host-imported, exportable (with its own VMA pool) and staging.
    - Creation returns a `GpuStatus`. No exceptions.
  - verify: on lavapipe and RADV, the test creates, maps and destroys each kind; a misaligned import returns a status rather than crashing; and the overrides read back.
  - size: M

- [ ] M84.P3.T2 — Async `GpuTransfer` streams with timeline values and built-in chunking
  - files: `Engine/Gpu/GpuTransfer.{h,cpp}`, `Tests/Gpu/GpuTransfer_Test.cpp`
  - approach:
    - New calls: `upload(HostRange, GpuBuffer&, offset) → TimelineValue`, `download(GpuBuffer&, offset, HostRange) → TimelineValue` and `wait(TimelineValue)`, plus a way for compute submissions to wait on a transfer value.
    - Queue-family ownership transfers between the transfer and compute queues.
    - Every copy is chunked at `chunkBytes` (512 MB by default) on both the staging and host-import paths (design note §4).
    - Host-import keeps a registration cache keyed by allocation, with `forgetHostAllocation(ptr)`. Misaligned or foreign memory falls back to staging.
    - Partial row-range downloads, so strip interiors can be read back.
    - `process()` is rebuilt on these streams.
  - verify, on lavapipe and RADV:
    - With a 1 MB chunk override, a 9 MB buffer round-trips byte-exact on both paths.
    - Upload → compute (fill) → download completes in order with no host wait in between.
    - A host range that is freed and then reallocated gets re-imported, not served stale.
  - size: L

- [ ] M84.P3.T3 — `GpuKernelRegistry` and a recording helper
  - files: `cmake/SlangKernels.cmake`, `Engine/Gpu/GpuKernelRegistry.{h,cpp}` (new), `Engine/Gpu/GpuRecord.{h,cpp}` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Gpu/GpuKernelRegistry_Test.cpp`
  - approach:
    - `slang_add_kernel` also appends to a generated `GpuKernels.inc`: an enum of kernel IDs and a static table of `GpuKernelDesc` (SPIR-V, entry point, binding count, push-constant bytes, group size).
    - Pipelines are created lazily per device, thread-safe, and the registry can `invalidate()` them on device loss or re-creation.
    - `GpuRecordContext` wraps a command buffer from the calling thread's pool and offers `dispatch(kernelId, buffers, push, threads)` and compute→compute barriers. Node code never touches raw Vulkan objects.
  - verify, on lavapipe and RADV:
    - Lookup by ID works.
    - Each pipeline is created once across 8 threads, and again after `invalidate()`.
    - A fill dispatched through `GpuRecordContext` is correct.
  - size: M

- [ ] M84.P3.T4 — Ref-counted device pool with a VRAM budget and LRU eviction
  - files: `Engine/Gpu/GpuBufferPool.{h,cpp}` (new), `Tests/Gpu/GpuBufferPool_Test.cpp`, `Tests/Gpu/CMakeLists.txt`
  - approach:
    - Free lists per size class of device-local `GpuBuffer`s. Handles are `shared_ptr`s that return their buffer to the pool on release.
    - Budget = min(`queryBudget` heap × 0.8, the user setting, the test override). Usage counts both live and pooled bytes.
    - Evictable buffers carry an owner callback that downloads them to RAM and drops them.
    - Allocation order:
      1. take from a free list;
      2. allocate from VMA;
      3. trim the free lists;
      4. evict the least-recently-used buffer and retry;
      5. if all of that fails, return the `VK_ERROR_OUT_OF_DEVICE_MEMORY` status.
    - The pool stays engine-free.
  - verify: on lavapipe with a 32 MB budget:
    - usage never exceeds the budget;
    - eviction follows LRU order and each callback runs exactly once;
    - the last release returns the buffer to the pool;
    - a request bigger than the budget returns OOM without evicting everything.
  - size: M

- [ ] M84.P3.T5 — Device state machine with fault injection and re-creation
  - files: `Engine/Gpu/GpuRuntime.{h,cpp}` (new), `Engine/Gpu/GpuDevice.{h,cpp}`, `Tests/Gpu/GpuRuntime_Test.cpp`
  - approach:
    - `GpuRuntime` owns the device, the transfer streams, the registry and the pool.
    - States are `Ready`, `Degraded` and `Lost`, plus `recreate()` (design note §5.7).
    - OOM moves to `Degraded` and shrinks the budget by the failed request.
    - `Lost` is sticky: every call returns the lost status.
    - `recreate()` tears everything down, creates the device again, and invalidates the registry and the import caches.
    - Faults are injected through `GpuDeviceOptions` or `NATRON_GPU_FAULT=lost@N|oom@N`.
  - verify: on lavapipe:
    - an injected loss latches `Lost` and later calls fail fast;
    - `recreate()` returns to `Ready` and dispatches work again;
    - an injected OOM moves to `Degraded` with a smaller budget;
    - the debug allocator reports no leaks.
  - size: M

## Phase 84.4: Host memory, device images and accounting (Engine)

- [ ] M84.P4.T1 — Page-aligned, importable RAM image buffers
  - files: `Engine/CacheEntry.h`, `Engine/HostImageMemory.{h,cpp}` (new), `Tests/Image_Test.cpp`
  - approach:
    - `RamBuffer` allocates through `HostImageMemory`: aligned to the page size, with sizes padded to the import alignment (4096 until a device reports a larger one).
    - Before memory is freed, it calls hooks that GPU code registers (the import cache's `forget`). No Vulkan types appear in this header.
    - The allocator policy follows P1.T2. If that calls for a recycling pool for buffers of 1 MB and up, keep it bounded, and count its bytes against the cache's RAM limit.
    - Memory-mapped and disk tiles are unchanged; they use the staging fallback.
  - verify:
    - An `Image_Test` case asserts the alignment and padding.
    - Full ctest is green in both scheduler modes.
    - A quick CPU bench (`graph_bench.py` with `BENCH_TOPO=comp`, UHD, 3 frames) is within noise of `main`.
  - size: M

- [ ] M84.P4.T2 — App-wide `GpuContext`, settings and mode resolution
  - files: `Engine/GpuContext.{h,cpp}` (new), `Engine/AppManager.{h,cpp}`, `Engine/Settings.{h,cpp}`, `Tests/GpuContext_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - `AppManager` owns one `GpuContext` around `GpuRuntime`. When the mode isn't Off, it initialises on a background thread at startup and registers the host-memory free hook.
    - At startup, calibrate with one 64 MB round trip each through host-import and staging.
    - New settings on the threading page:
      - `gpuComputeMode`: Off or Auto, **default Auto** (user, 2026-10-10);
      - `gpuDevice`;
      - `gpuMemoryBudgetMB`, where 0 means automatic.
    - `NATRON_GPU=off|auto|force` overrides the setting.
    - Auto never picks a CPU-type Vulkan device such as lavapipe. Force accepts any device, for tests and benches.
    - With `NATRON_HAVE_GPU` off, the stub is never available.
  - verify:
    - The mode-resolution tests pass.
    - With no ICD, or with Auto on lavapipe, the context reports unavailable and logs no error.
    - `NATRON_GPU=force` on lavapipe gives a Ready context with calibrated bandwidth.
    - An injected fault at startup degrades silently to CPU.
  - size: M

- [ ] M84.P4.T3 — `FrameStore` entries that hold device images
  - files: `Engine/FrameRenderContext.{h,cpp}`, `Engine/DeviceImage.h` (new interface), `Engine/EffectInstance.cpp`, `Tests/FrameStoreHandoff_Test.cpp`
  - approach:
    - `DeviceImage` is an abstract interface: `bytes()`, `bounds()`, `nComps()`, `downloadTo(Image&)` and `evictToRam()`. The Vulkan implementation comes in P7.T2.
    - An `Entry` gains `deviceLayers`, plus `putDevice` and `findDevice`.
    - A RAM `find()` on an entry that only has device layers downloads once under the entry lock, keeps the RAM layers, and counts the download. `lookupFrameStore` is unchanged.
    - `bytesInFlight()` splits into `hostBytesInFlight()` and `deviceBytesInFlight()`.
    - Eviction replaces the device layers with RAM layers. Consumer counts are unchanged.
  - verify: with a fake `DeviceImage` and no device:
    - two finds download only once;
    - host and device accounting are separate and both return to 0;
    - an evicted entry serves finds without another download.
  - size: M

- [ ] M84.P4.T4 — Transfer and placement counters in `RenderStats` and the profile
  - files: `Engine/RenderStats.{h,cpp}`, `Engine/RenderScheduler.cpp` (`formatTaskRecord`), `tools/bench/render-profile-report.py`, `Tests/RenderProfile_Test.cpp`
  - approach:
    - New counters:
      - upload and download counts and bytes;
      - evictions;
      - GPU regions run;
      - GPU tasks run per node;
      - CPU fallbacks, by reason;
      - peak device bytes.
    - Task records gain `"device"` and `"regionId"`. A new `"kind":"region"` record lists the region's members with upload, kernel and download ms and bytes.
    - The report script reads the new fields and skips region records in the CPU analysis. Its self-test covers both.
  - verify: `RenderProfile_Test` checks the new fields on a CPU render, and `RenderProfileReportSelfTest` is green.
  - size: M

## Phase 84.5: GPU-capable nodes (the minimum that exercises placement)

- [ ] M84.P5.T1 — GPU-capability interface on `NativeImageEffect`
  - files: `Engine/Nodes/Image/GpuRender.h` (new), `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, `Tests/Native/NativeImageEffect_Test.cpp`
  - approach:
    - New types:
      - `GpuQuery`: time, view, mapped scale, components, depth, mask presence, mix, process channels and premult selector.
      - `GpuImageView`: buffer, byte offset, bounds, row stride in floats, and nComps.
      - `GpuRenderArgs`: the query, the output rect, the input and output views, and a `GpuRecordContext`.
    - New virtuals:
      - `canRenderOnGpu(const GpuQuery&) const`, default false;
      - `getGpuInputRoI(int input, const RectI& out, const RenderScale&) const`, default `out`, used for halos and strips;
      - `renderGpu(const GpuRenderArgs&)`.
    - A node's GPU support is expressed through registry kernel IDs. `supportsOpenGLRender` stays as it is, for the OFX GL suite.
  - verify: it builds with and without `NATRON_HAVE_GPU`, and a test asserts that every registered native node except Grade and Blur reports `canRenderOnGpu == false`.
  - size: M

- [ ] M84.P5.T2 — Grade on the GPU (no mask, mix 1, no unpremult)
  - files: `Engine/Nodes/Color/Grade.{h,cpp}`, `Tests/Native/NativeGradeGpu_Test.cpp`, `Tests/Gpu/Tolerance.h` (shared into the Tests binary)
  - approach:
    - Eligible for float images with 1, 3 or 4 components, no mask, mix 1 and no premult.
    - Map the knobs to `GradeParams` exactly as `GradeKernel` does: compute in double and cast once, including the reverse and clamp flags and the channel mask.
    - `renderGpu` dispatches `grade` with offsets and strides, adding those params to `grade.slang` if needed.
  - verify:
    - On lavapipe, GPU output matches the native CPU output on the M83 Grade fixtures (reverse, clamps, gamma curves, 1-channel alpha) within the M83 ULP-conditioned bound.
    - Ineligible settings report false.
    - On RADV inside the container (P1.T3).
  - size: M

- [ ] M84.P5.T3 — FIR Gaussian Blur on the GPU, for regions of interest
  - files: `Engine/Nodes/Filter/Blur.{h,cpp}`, `Engine/Gpu/Kernels/blur.slang`, `Engine/Nodes/Filter/BlurKernels.{h,cpp}` (expose the FIR weight function), `Tests/Native/NativeBlurGpu_Test.cpp`
  - approach:
    - Eligible only when filter is FIR Gaussian, `orderX == orderY == 0`, `alphaThreshold == 0`, there is no mask, mix is 1, there is no premult, and the radius is ≤ 512.
    - `blur.slang` gains src and dst rect origins and row strides.
    - Black and nearest edges are resolved against the input's RoD, as `fillBuffer` does.
    - Weights come from the same host function as the CPU FIR and are passed in a storage buffer.
    - The two passes use a scratch buffer from the pool.
    - `getGpuInputRoI` calls `getSourceRoI`.
  - verify: on lavapipe and RADV (in the container), GPU and CPU FIR agree within 2e-6 for:
    - σ 0.5, 3, 25 and 100;
    - black and nearest edges;
    - a RoI inside the RoD;
    - odd sizes;
    - `expandRoD` on and off.
  - size: L

## Phase 84.6: Placement and cost model

- [ ] M84.P6.T1 — Device and region fields, and the region-forming placement pass
  - files: `Engine/RenderScheduler.h`, `Engine/GpuPlacement.{h,cpp}` (new), `Tests/GpuPlacement_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - `FrameGraph::Task` gains `device` (Cpu or Gpu) and `regionId` (-1 by default).
    - `FrameGraph` gains `regions`. Each region records its members in topological order, its external inputs, its outputs with internal and external consumer counts, and its estimated device working set.
    - `placeGraph(FrameGraph&, const PlacementInputs&)` takes capability and cost callbacks, so tests need no device.
    - Candidates are tasks whose effect answers `canRenderOnGpu` (with frame TLS installed). Identity members alias their input, and a task whose output is already cached stays on the CPU.
    - Candidates form connected components, which are split until every region is convex.
    - The pass runs after `buildGraph` on the writer and viewer task-graph paths, only when `GpuContext` is available. The legacy scheduler never places.
  - verify: hand-built graphs check that:
    - a chain becomes one region;
    - a diamond with a CPU node on one side splits into convex regions;
    - a multi-consumer GPU node stays one member, with the right consumer counts;
    - a cache hit stays on the CPU;
    - with no context, the graph is unchanged.
  - size: L

- [ ] M84.P6.T2 — Emit the per-plugin cost table from profiles
  - files: `tools/bench/render-profile-report.py`, `Engine/GpuCostSeed.inc` (generated, checked in)
  - approach:
    - `--emit-cost-table` writes per-plugin CPU ms/Mpx from the M82 serial profiles, the measured parallelism, and P1.T1's in-situ Blur numbers. If the serial profiles (`build/bench/m82-profile/*-serial.steady.jsonl`) are missing on this host, re-run `run_comps.sh` with `noRenderThreads=1`.
    - It also writes GPU ms/Mpx for `grade` and `blur` from M83 §2.3/§2.4, by σ bucket.
    - The output is a C++ initialiser list.
    - The self-test covers the emitter.
  - verify: `RenderProfileReportSelfTest` is green, and the `.inc` matches the inputs listed in its header comment.
  - size: M

- [ ] M84.P6.T3 — StarPU-style cost model and region acceptance
  - files: `Engine/GpuCostModel.{h,cpp}` (new), `Engine/GpuPlacement.cpp`, `Tests/GpuPlacement_Test.cpp`
  - approach:
    - CPU cost is the seed ms/Mpx × Mpx, scaled to the pool size. During a session it is refined by an EMA of the measured `wallNs × budget / Mpx` from `executeTask`.
    - GPU cost is the seed, refined by `GpuTimer` kernel times.
    - Boundary cost is (input bytes + CPU-consumed output bytes) / bandwidth. Bandwidth comes from P4.T2's calibration and is 0 when `unifiedMemory` is set; tests can override it.
    - Accept a region when saved CPU time − GPU time − boundary cost > margin.
    - Otherwise shrink it greedily: drop the least profitable boundary member, re-check convexity, and stop after a bounded number of steps. Rejected members go to the CPU.
    - Costs are not kept across sessions (user, 2026-10-10: deferred).
  - verify:
    - At 25 GB/s an isolated Grade between CPU nodes stays on the CPU, and under the unified model it moves to the GPU.
    - A 10-Grade chain forms one region at 25 GB/s.
    - A lone σ25 Blur is accepted at UHD.
    - Halving the bandwidth rejects a marginal region.
  - size: L

## Phase 84.7: Execution: region super-tasks, admission, failure, tiling

- [ ] M84.P7.T1 — Per-task output storage, and regions contracted into super-tasks in the scheduler
  - files: `Engine/RenderScheduler.{h,cpp}`, `Tests/RenderScheduler_Test.cpp`
  - approach:
    - `executeTask` takes the task's storage from placement instead of the hard-coded `eStorageModeRAM` (`:933`).
    - Each region becomes one schedulable unit:
      - its dependencies are the union of its members' external dependencies, and its consumers the union of their external consumers;
      - members are never pushed on their own;
      - when it finishes, it releases each external input once per member edge and decrements each external consumer.
    - The region body is a hook (`std::function`) that P7.T2 sets; tests install a fake.
    - A CPU-only graph takes exactly today's path.
  - verify:
    - Tests with a fake region body cover ordering, store release counts, and abort and failure inside a region, and `getReservedBytesForTests() == 0` afterwards.
    - Full ctest is green in both scheduler modes.
  - size: L

- [ ] M84.P7.T2 — The region super-task body: upload once, record all members, download per output
  - files: `Engine/GpuRegionTask.{h,cpp}` (new), `Engine/GpuContext.cpp` (the `DeviceImage` implementation), `Engine/RenderScheduler.cpp` (installs the hook), `Tests/GpuResidency_Test.cpp`, `Tests/CMakeLists.txt`
  - approach:
    - Runs on the pool thread with budget 1, inside `FrameContextScope`.
    - RAM inputs are uploaded by host-import, falling back to staging. Device-image inputs are used directly.
    - Member outputs come from the pool.
    - Each member's `renderGpu` is recorded in topological order, with barriers between members. Identity members alias their input. Abort is checked between members.
    - Each output with a CPU consumer gets exactly one download into a page-aligned RAM `Image`, issued as soon as its producer is submitted.
    - Outputs consumed only by other regions go to the store as evictable `DeviceImage`s.
    - Intermediates are released after their last internal consumer.
    - GPU outputs bypass the image cache.
    - It emits profile and region records and updates the counters.
  - verify: with `NATRON_GPU=force` on lavapipe:
    - Constant → Grade → Blur → Grade → CPU sink records uploads == 1 and downloads == 1, and the pixels match the CPU render within the Blur bound.
    - A Grade feeding two GPU Blurs and one CPU node renders the Grade once and downloads only the outputs that have CPU consumers.
    - A convex split carries one device image between regions with no extra transfer.
  - size: L

- [ ] M84.P7.T3 — GPU-aware admission and thread budget
  - files: `Engine/RenderScheduler.{h,cpp}`, `Tests/RenderScheduler_Test.cpp`
  - approach:
    - A super-task's host `estimatedBytes` covers only its downloaded outputs. A new `estimatedDeviceBytes` holds its peak working set.
    - `admitGatedLocked` checks host and device budgets separately: `_reservedDeviceBytes` against the pool budget. `bytesInFlightLocked` splits the same way.
    - GPU super-tasks get budget 1 and don't count as sharers in `computeTaskBudget`.
    - At most `gpuMaxConcurrentTasks` (default 2) run at once; the rest wait.
    - A super-task that continues a branch always runs, and eviction handles the memory pressure.
  - verify:
    - With no regions, CPU budgets are unchanged.
    - GPU tasks get budget 1.
    - The concurrency cap holds.
    - Host and device reservations return to 0 after mixed frames and after an abort.
  - size: M

- [ ] M84.P7.T4 — Fall back to the CPU on out-of-memory and device loss
  - files: `Engine/GpuRegionTask.cpp`, `Engine/RenderScheduler.{h,cpp}`, `Engine/GpuContext.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach:
    - **Degraded (OOM):**
      1. Evict LRU buffers and retry the region once.
      2. If that fails, run the members on the CPU via `renderRoI` in topological order, inside the super-task, after downloading any device inputs.
      3. Placement then uses the shrunk budget.
    - **Lost:** the scheduler re-runs the whole frame CPU-only. It clears the store, re-places every task on the CPU and re-queues the leaves, without completing the `FrameFuture`.
    - **Recovery:** `GpuContext` tries `recreate()` at the start of the next render.
    - Every fallback is counted, with its reason.
  - verify: on lavapipe with `NATRON_GPU_FAULT`:
    - OOM at the second region completes the frame, matches the CPU pixels, and counts one fallback;
    - loss mid-frame completes the frame CPU-only with matching pixels;
    - the next render recreates the device and uses the GPU again.
  - size: L

- [ ] M84.P7.T5 — GPU strip tiling for oversized regions
  - files: `Engine/GpuRegionTask.cpp`, `Engine/GpuRegionStrips.{h,cpp}` (new), `Tests/GpuResidency_Test.cpp`
  - approach:
    - Strip a region when any member buffer exceeds `maxStorageBufferRange`, or its working set exceeds the device budget.
    - Strips are horizontal, with heights a multiple of 4 rows (default 2048) so pointers stay 4096-aligned.
    - Halos accumulate backwards through `getGpuInputRoI`. Inputs are uploaded with their halos, and only interiors are downloaded.
    - If not even one strip fits, fall back to the CPU.
  - verify:
    - On lavapipe, with a 1 MB `maxStorageBufferRange` override, tiled Grade → Blur σ25 → Grade matches untiled exactly (max abs diff 0).
    - On RADV, a 24k RGBA float Blur region renders (P8.T2).
  - size: L

- [ ] M84.P7.T6 — Acceptance: the VRAM budget holds under a large comp
  - files: `Tests/GpuResidency_Test.cpp`
  - approach: on lavapipe, with the pool budget at 64 MB:
    - build 8 branches of Constant → Grade → Blur σ10 → Grade at 1024²;
    - join them with CPU Merges;
    - include one convex split, so device images sit in the store across branches;
    - render with 2 frames in flight.
  - verify:
    - Both frames complete.
    - Evictions > 0, and peak device bytes stay ≤ the budget.
    - No fallback happens for budget reasons.
    - The pixels match the CPU render.
  - size: M

## Phase 84.8: Benches and close-out

- [ ] M84.P8.T1 — Quick directional bench: no regression in CPU-only renders
  - files: none (results go in Decisions)
  - approach:
    - Release builds of `main` (pre-M84) and the milestone, both with `NATRON_GPU=off`.
    - Run `graph_bench.py` with `chain`, `comp` and `blurchain` at UHD, 3 frames each, plus `keying_grade`.
    - About 1 hour.
  - verify: every case is within ±5% of `main` (the noise band, from two repeats). Diagnose any regression before the gate.
  - size: M

- [ ] M84.P8.T2 — Quick directional GPU bench on RADV, up to 24k
  - files: none (results go in Decisions)
  - approach:
    - Run in the RADV container (P1.T3), comparing GPU Auto against Off.
    - Workloads:
      - `blurchain` N=1 and Grade → Blur σ25 → Grade, at UHD, 8K, 16k and 24k (`BENCH_SIZE`);
      - `cg_multipass` and `keying_grade` at UHD.
    - Record wall time per frame, the regions formed, the transfer counts and bytes, and the placement decisions.
    - About 1 hour.
  - verify: a Decisions table. The direction must hold: Grade → Blur → Grade at 24k is faster on the GPU, and no comp is slower with Auto than with Off. Any comp that is slower is explained by the cost-model inputs, and those inputs are fixed.
  - size: M

- [ ] M84.P8.T3 — Record outcomes and hand-offs
  - files: `.plan/PLAN/DESIGN/2026-10-10-gpu-compute-backend.md` (a follow-up section), `.plan/PLAN/MILESTONES/M85-native-gpu-kernels.md`, `.plan/PLAN/MILESTONES/M86-ofx-gpu-suites.md`
  - approach:
    - The design note gets the registration cost, the Blur baseline, the bench results and any limits that changed.
    - The M85 stub gets:
      - the `ViewerGL` interop wiring and a GPU viewer-process kernel;
      - mask, mix and premult wrapping for Grade and Blur;
      - GPU IIR and box Blur, or a decision to keep them CPU-only;
      - work on the Blur vertical pass;
      - double-buffering in `GlInterop` (deferred from the M83 review).
    - The M86 stub gets the exportable buffer pool.
  - verify: the M85 and M86 stubs name these hand-offs.
  - size: S

**Dependency map**
- **Start in parallel:** P1.T1, P1.T2, P1.T3, P1.T4 and P2.T1. Builds share the one build slot: P1.T1 needs a release build of `main`, and P2.T1 a debug build of the branch.
- **After P2.T1:**
  - P2.T2, then P2.T3;
  - P2.T4, which also needs P1.T2;
  - P3.T1;
  - P4.T3 and P4.T4.
- **After P3.T1, in parallel:** P3.T2, P3.T3 and P3.T4. P3.T5 follows all three.
- **P4.T1:** after P1.T2.
- **P4.T2:** after P3.T5 and P4.T1.
- **Phase 5:** P5.T1 after P3.T3; then P5.T2 and P5.T3 in parallel, both after P5.T1 and P4.T2.
- **Phase 6:**
  - P6.T1 after P5.T1;
  - P6.T2 after P1.T1;
  - P6.T3 after P6.T1, P6.T2 and P4.T2.
- **Phase 7:**
  - P7.T1 after P6.T1.
  - P7.T2 after P7.T1, P5.T2, P5.T3, P4.T3, P4.T4 and P3.T2.
  - P7.T3 after P7.T1 and P3.T4, in parallel with P7.T2.
  - P7.T4 after P7.T2 and P3.T5.
  - P7.T5 after P7.T2.
  - P7.T6 after P7.T3 and P7.T4.
- **Phase 8:**
  - P8.T1 after Phase 7.
  - P8.T2 after Phase 7 and P6.T3.
  - P8.T3 last.
- **Critical path:** P2.T1 → P3.T1 → P3.T3 → P5.T1 → P6.T1 → P7.T1 → P7.T2 → P7.T4 → P7.T6 → P8.T2.

**Verification gate:**
- **Container, lavapipe:** full ctest is green in both scheduler modes in natron-dev, with `VK_ICD_FILENAMES` set to lavapipe and `NATRON_TEST_REQUIRE_GPU=1`. That includes `GpuTests`, `GpuPlacement_Test`, `GpuResidency_Test` (transfers only at region boundaries, multi-consumer rendered once, VRAM budget held, OOM and device-loss fallback, tiled = untiled) and Grade/Blur GPU parity, none skipped.
- **Container, no Vulkan ICD:** full ctest is also green.
- **Container, RADV:** the same suite is green with RADV (P1.T3).
- **Host:** `GpuTests` is green on RADV through `run-host.sh`, including semaphore interop.
- **CI:** green, with GPU tests run, not skipped.
- **Build:** `-DNATRON_GPU=OFF` builds.
- **Benches:** P8.T1 shows no CPU-only regression beyond ±5%, and P8.T2 shows the expected direction up to 24k.
- **Records:** the M63 record is corrected and the hand-offs are recorded.

## Decisions

- 2026-10-10 — Elaborated from the stub by a planning consultant after the M83 design note was signed off. The user answered its four open questions:
  - default GPU mode is **Auto**;
  - **fix the dev image** so engine tests run on RADV in the container (P1.T3), rather than relying on host AppImages;
  - a CPU Blur threading bug found by P1.T1 gets **its own milestone** unless the fix is a few lines;
  - **no cost history across sessions** in M84.
- 2026-10-10 — **M84 / M85 boundary.**
  - **M84 ships:**
    - the GPU-capability interface;
    - Grade on the GPU, without mask, mix or premult;
    - Blur on the GPU, FIR Gaussian only, order 0, with no mask, mix or alpha threshold.
  - **M85 owns:**
    - the backend-neutral kernel description next to `makeKernel()`;
    - per-pixel mask, mix, process-channel and premult wrapping;
    - every other node;
    - per-node tolerance suites;
    - kernel performance work.
  - **Why:** placement and residency can only be proven end to end with real kernels whose tolerances are known. Grade is the cheap point op that residency depends on, and Blur is the heavy spatial op that needs halos and tiling.
- 2026-10-10 — **Viewer interop:** the code is promoted, but the wiring is deferred to M85.
  - **Why:** the viewer's display processing runs on the CPU (`ViewerInstance.cpp:106/111`), so a region output would be downloaded anyway. Zero-copy pays only once that processing becomes a GPU kernel.
  - **In M84:** viewer frames place their upstream regions and download the root.
- 2026-10-10 — **CI coverage is lavapipe, in the existing job.**
  - Slang is cached, and `NATRON_TEST_REQUIRE_GPU=1` makes skipped GPU tests fail.
  - Semaphore interop never runs in CI. RADV coverage is a manual gate step.
- 2026-10-10 — **Two test binaries:**
  - `GpuTests` is engine-free and statically linked, so it runs on the host.
  - The engine-level GPU tests live in the main `Tests` binary and reach RADV through the fixed container (P1.T3).
- 2026-10-10 — **Auto mode ignores CPU-type Vulkan devices such as lavapipe,** so CPU renders and CI placement stay deterministic. Tests use `force`.
- 2026-10-10 — **GPU outputs bypass the image cache,** as GL renders do. A task whose output is already cached stays on the CPU.
- 2026-10-10 — **Regions are convex.** A super-task can't wait on a CPU task that waits on it. Device images cross regions only at the split points.
- 2026-10-10 — **Device loss re-runs the whole frame CPU-only,** a departure from design note §5.7.
  - **Why:** device images die with the device, and losses are rare.
  - **OOM stays per-region:** evict, retry once, then fall back to the CPU.
- 2026-10-10 — **Cost model:**
  - a checked-in seed table, built from the M82 serial profiles, P1.T1 and the M83 kernel times;
  - refined during the session by an EMA of measured times;
  - bandwidth calibrated at device init;
  - not persisted across sessions.
- 2026-10-10 — **GPU tiling overrides the stub's "fine tiling stays a CPU concern"** for buffers over `maxStorageBufferRange` or the device budget.
- 2026-10-10 — **Placement runs only in task-graph mode.** The legacy scheduler stays CPU-only.
