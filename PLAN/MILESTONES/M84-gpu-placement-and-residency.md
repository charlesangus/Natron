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

- [x] M84.P1.T1 — Re-measure the in-graph CPU Blur cost against the bare FIR kernel, and find where the threads go
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

- [x] M84.P1.T2 — Measure host-import registration cost and churn on RADV
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

- [x] M84.P1.T3 — Make RADV work inside the natron-dev container
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

- [x] M84.P1.T4 — Fix the stale GL statement in the M63 decision record
  - files: `.plan/PLAN/DECISIONS/2026-10-04-task-graph-render-scheduler.md`
  - approach:
    - In its Risks paragraph, take OpenGL out of the "stay on the legacy path in v1" list.
    - Add one sentence saying that GL-capable nodes render inside tasks on per-thread private contexts (`GPUContextPool::getOrCreateContextForCurrentThread`, M63.P5.T3, `9d5b0997f`), and that a task fails if a GL texture would reach the RAM-only store.
    - Roto strokes, analysis renders and deep-output writers stay listed.
  - verify: the paragraph matches M63's P5.T3 entry, and the `INDEX.md` summary is still accurate.
  - size: S

## Phase 84.2: Promote the backend into the main build

- [x] M84.P2.T1 — Build `Engine/Gpu/` (`NatronGpu`) from the spike sources, with Slang in the root build
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

- [x] M84.P2.T2 — `Tests/Gpu/` as the host-runnable `GpuTests` binary
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

- [x] M84.P2.T3 — GPU coverage in CI on lavapipe
  - files: `.github/workflows/ci.yml`
  - approach:
    - Cache `.natron-deps` with `actions/cache`, keyed on the Slang version and SHA, and set `NATRON_DEPS_CACHE` to it.
    - Set `VK_ICD_FILENAMES` to lavapipe and `NATRON_TEST_REQUIRE_GPU=1` on both ctest steps.
    - Keep everything in the same job.
  - verify: CI on the branch is green, the `GpuTests` cases show as passed in the log (not skipped), and the job's wall-time change is recorded in Decisions.
  - size: M

- [x] M84.P2.T4 — Rebuild the spike's tools on the promoted code
  - files: `tools/gpu-spike/CMakeLists.txt`; delete `tools/gpu-spike/{src,cmake,tests/*_Test.cpp,tests/*.h,ref}` and the moved kernels
  - approach:
    - The spike project includes the root `cmake/` helpers and `add_subdirectory(../../Engine/Gpu)`.
    - These stay in the spike, built against `NatronGpu`: `GpuBench`, `CpuTwinBench`, `GlCompute`, `DeviceList`, `GlInteropQt`, `ImportCost` (P1.T2), `run-host.sh` and `run-bench.sh`.
    - Run this after P1.T2, since both edit the spike's CMakeLists.
  - verify: the spike configures and builds in the container, and `GpuBench --sizes uhd` runs on lavapipe.
  - size: M

## Phase 84.3: Finish the backend (design note §5 "new" items)

- [x] M84.P3.T1 — Device limits and the `GpuBuffer` RAII layer
  - files: `Engine/Gpu/GpuDevice.{h,cpp}`, `Engine/Gpu/GpuBuffer.{h,cpp}` (new), `Tests/Gpu/GpuBuffer_Test.cpp`, `Tests/Gpu/CMakeLists.txt`
  - approach:
    - `GpuDeviceInfo` gains `maxStorageBufferRange`, `minImportedHostPointerAlignment`, the device-local heap size, and `unifiedMemory` (integrated GPU, or a device-local and host-visible heap that covers device memory).
    - `GpuDeviceOptions` gains test overrides for `maxStorageBufferRange` and the copy chunk size.
    - `GpuBuffer {VkBuffer, VmaAllocation, size, kind}` comes in four kinds: device-local, host-imported, exportable (with its own VMA pool) and staging.
    - Creation returns a `GpuStatus`. No exceptions.
  - verify: on lavapipe and RADV, the test creates, maps and destroys each kind; a misaligned import returns a status rather than crashing; and the overrides read back.
  - size: M

- [x] M84.P3.T2 — Async `GpuTransfer` streams with timeline values and built-in chunking
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

- [x] M84.P3.T3 — `GpuKernelRegistry` and a recording helper
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

- [x] M84.P3.T4 — Ref-counted device pool with a VRAM budget and LRU eviction
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

- [x] M84.P3.T5 — Device state machine with fault injection and re-creation
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

- [x] M84.P4.T1 — Page-aligned, importable RAM image buffers
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

- [x] M84.P4.T2 — App-wide `GpuContext`, settings and mode resolution
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

- [x] M84.P4.T3 — `FrameStore` entries that hold device images
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

- [x] M84.P4.T4 — Transfer and placement counters in `RenderStats` and the profile
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

- [x] M84.P5.T1 — GPU-capability interface on `NativeImageEffect`
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

- [x] M84.P5.T2 — Grade on the GPU (no mask, mix 1, no unpremult)
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

- [x] M84.P5.T3 — FIR Gaussian Blur on the GPU, for regions of interest
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

- [x] M84.P6.T1 — Device and region fields, and the region-forming placement pass
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

- [x] M84.P6.T2 — Emit the per-plugin cost table from profiles
  - files: `tools/bench/render-profile-report.py`, `Engine/GpuCostSeed.inc` (generated, checked in)
  - approach:
    - `--emit-cost-table` writes per-plugin CPU ms/Mpx from the M82 serial profiles, the measured parallelism, and P1.T1's in-situ Blur numbers. If the serial profiles (`build/bench/m82-profile/*-serial.steady.jsonl`) are missing on this host, re-run `run_comps.sh` with `noRenderThreads=1`.
    - It also writes GPU ms/Mpx for `grade` and `blur` from M83 §2.3/§2.4, by σ bucket.
    - The output is a C++ initialiser list.
    - The self-test covers the emitter.
  - verify: `RenderProfileReportSelfTest` is green, and the `.inc` matches the inputs listed in its header comment.
  - size: M

- [x] M84.P6.T3 — StarPU-style cost model and region acceptance
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

- [x] M84.P7.T1 — Per-task output storage, and regions contracted into super-tasks in the scheduler
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

- [x] M84.P7.T2 — The region super-task body: upload once, record all members, download per output
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

- [x] M84.P7.T3 — GPU-aware admission and thread budget
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

- [x] M84.P7.T4 — Fall back to the CPU on out-of-memory and device loss
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

- [x] M84.P7.T5 — GPU strip tiling for oversized regions
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

- [x] M84.P7.T6 — Acceptance: the VRAM budget holds under a large comp
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

- [x] M84.P7.T7 — Reactive GPU memory admission: the pool enforces the budget, out-of-memory halves the strips
  - files: `Engine/RenderScheduler.{h,cpp}`, `Engine/GpuRegionTask.cpp`, `Engine/GpuRegionStrips.{h,cpp}`, `Engine/GpuContext.cpp`, `Tests/GpuResidency_Test.cpp`, `Tests/RenderScheduler_Test.cpp`
  - approach:
    - Admission no longer gates GPU regions on estimated device bytes. They are throttled only by `gpuMaxConcurrentTasks`, and the pool enforces the budget by evicting LRU buffers. Host-byte admission for downloaded outputs is unchanged. "A branch continuation always runs" stays.
    - Strips are planned from the exactly known input and output buffer sizes only, with a fixed headroom under the device's real memory for allocations outside the pool (OFX plugins later).
    - On out-of-memory, a region evicts and retries. If it still fails, it retries with half-height strips, down to the 4-row minimum, before the CPU fallback. Each step is counted.
  - verify:
    - `LargeCompStaysWithinTheDeviceBudget` asserts evictions > 0, along with its existing checks (peak ≤ budget, no budget fallback, pixels match), and passes 10/10.
    - A test forces an out-of-memory that strip-halving resolves without a CPU fallback.
    - Host and device reservations return to 0.
    - Full ctest passes in both modes on lavapipe.
  - size: L

- [x] M84.P7.T8 — Pipelined strips: stream uploads and downloads while the GPU computes
  - files: `Engine/GpuRegionTask.cpp`, `Engine/GpuRegionStrips.{h,cpp}`, `Engine/Gpu/GpuTransfer.{h,cpp}` (only if needed), `Tests/GpuResidency_Test.cpp`
  - approach:
    - A stripped region runs as a pipeline over the async transfer streams (P3.T2) with timeline semaphores. While strip k computes, strip k+1's inputs upload and strip k−1's outputs download. The host waits only when it must reuse a buffer, not after every strip.
    - Buffers rotate as at least double buffers (triple where the budget allows) per strip slot. The strip planner sizes strips so that the in-flight set (slots × strip working set) fits the budget, and keeps one-slot serial as the last step before strip-halving or the CPU fallback.
    - It falls back to the serial path when the device has no separate transfer queue family, so lavapipe stays correct. On RADV it uses the dedicated transfer family (`RADV_PERFTEST=transfer_queue`).
    - Abort, out-of-memory and device-loss handling keep their P7.T4/P7.T7 behaviour mid-pipeline. In-flight work is drained before any buffer is returned to the pool.
    - Per-region stats record the overlap: transfer time hidden behind compute versus exposed.
  - verify:
    - Tiled output still matches untiled exactly (diff 0) on lavapipe with the 1 MB override, with pipelining both forced on and off.
    - On RADV in the container (`devshell.sh --gpu`), a stripped Grade → Blur σ25 → Grade at 16k shows queue overlap: total region wall time is clearly below the sum of upload + compute + download, recorded in Decisions.
    - Reservations return to 0, and abort mid-pipeline is clean.
    - Full ctest passes in both modes on lavapipe.
  - size: L

- [x] M84.P7.T9 — Make strip pipelining overlap in real renders
  - files: `Engine/GpuRegionTask.cpp`, `Engine/GpuRegionStrips.{h,cpp}`, `Engine/Gpu/GpuTransfer.{h,cpp}`, `Engine/HostImageMemory*` (if the cause is pinning), `Tests/GpuResidency_Test.cpp`
  - approach: in the M84.P8.T2 release bench on RADV (`.local/m84p8/graph_bench_gbg.py`, G-B-G σ25 at 16384² and 24000²), stripped regions report `stripsInFlight` 3 but `transferHidden` 0–20 ms against 295–810 ms exposed, while P7.T8's in-test benchmark hid 274 ms. Find why (candidates: first-touch or pinning of fresh download images on the region thread every frame, host-import vs staging, the pipeline falling back, or a measurement bug), fix it, and re-run the 16k and 24k G-B-G bench.
  - verify:
    - in the release bench on RADV, a 16k and a 24k stripped region show most of the transfer time hidden;
    - wall time improves on the P8.T2 numbers (16k 4.44 s, 24k 9.10 s);
    - tiled still matches untiled exactly;
    - full ctest passes in both modes on lavapipe.
  - size: L

- [x] M84.P7.T10 — Fix the release-only segfault in the device-loss test on lavapipe
  - files: `Engine/GpuPlacement.cpp`, `Engine/GpuContext.cpp`, `Engine/RenderScheduler.cpp`, `Engine/GpuRegionTask.cpp` (wherever the cause is), `Tests/GpuResidency_Test.cpp`
  - approach: `GpuResidencyTest.DeviceLossRendersTheFrameOnTheCpuAndTheNextRenderRecovers` segfaults in `placeRenderGraph` in the **release** build on lavapipe, 3 of 3 runs. It also fails on the tip before P7.T9. It passes in debug and on RADV in release. Find the root cause (a likely suspect is a use-after-free or a race around `recreate()` and `recoverIfLost()` invalidating handles that placement or the cost model still holds), and fix it in the engine, not the test.
  - verify:
    - the test passes 20/20 in release on lavapipe in both modes, and in debug;
    - if available, an ASan or TSan run of the test is clean;
    - full ctest passes in both modes in debug and release on lavapipe.
  - size: L

## Phase 84.8: Benches and close-out

- [x] M84.P8.T1 — Quick directional bench: no regression in CPU-only renders
  - files: none (results go in Decisions)
  - approach:
    - Release builds of `main` (pre-M84) and the milestone, both with `NATRON_GPU=off`.
    - Run `graph_bench.py` with `chain`, `comp` and `blurchain` at UHD, 3 frames each, plus `keying_grade`.
    - About 1 hour.
  - verify: every case is within ±5% of `main` (the noise band, from two repeats). Diagnose any regression before the gate.
  - size: M

- [x] M84.P8.T2 — Quick directional GPU bench on RADV, up to 24k
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

- [x] M84.P8.T3 — Record outcomes and hand-offs
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
  - P7.T7 after P7.T6; P7.T6 is checked off together with it.
  - P7.T8 after P7.T7 (same files).
  - P7.T9 after P8.T2 (found by it).
  - P7.T10 found by P7.T9; it blocks the gate.
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
- 2026-10-10 — M84.P1.T4 done (plan-only change). The corrected `2026-10-04-task-graph-render-scheduler.md` must be re-published to `docs/decisions/` at the M84 gate.
- 2026-10-10 — M84.P1.T1 done (code `336b0ce81`, `592682281`). **In-graph CPU cost** (UHD float, 16 threads, `blurchain` N=1, per-task ms/Mpx):

  | Filter | σ3 | σ25 | σ100 |
  |---|---|---|---|
  | FIR | 16.9 | 25.1 | 47.7 |
  | FIR, serial | 46 | 100 | 267 |
  | IIR | 18.9 (flat) | | |
  | IIR, serial | 71 | | |

  8K matches. The thread budget is fine (16 threads). About 100 ms per UHD task is serial and independent of σ:
  - ~35 ms zeroing the 133 MB buffer on one thread;
  - ~50 ms in a single-threaded `memmove` in `Image::pasteFromForDepth` called from `renderHandler`;
  - ~15 ms in `writeWindow`.

  The kernel passes themselves run at 1.3–1.8× the bare kernel.
  - **Fix kept (a few lines):** leave the Blur buffer uninitialised. FIR becomes 13.2 / 21.0 / 43.4 ms/Mpx. Full ctest passes, except the known CImg-asset test.
  - **Split off:** the rest went to M90 - Render Path Serial Overhead (user rule).
  - **Cost-model inputs:** Blur FIR is ~12 ms/Mpx fixed + ~1.5× the bare kernel; Grade is 9.4 ms/Mpx in-graph (24.8 serial).
  - **Also added:** `BENCH_SIZE`/`BENCH_BLUR_SIZE`/`BENCH_BLUR_FILTER` in `graph_bench.py`. This overlaps M83's own `BENCH_SIZE` change, so it may conflict when M83 merges.
- 2026-10-10 — M84.P1.T3 done (`9eeba79ba`). **RADV in the container.**
  - **Fix:** `tools/ci/local/Dockerfile` uses `patchelf` to repoint Mesa's LLVM-linked libraries (gallium, radv, lvp, OSMesa) at `/usr/lib64/libLLVM.so.21.1` by absolute path.
  - **Why not `LD_PRELOAD`:** it was rejected because it would also swap the clang toolchain's same-soname `libLLVM` in every build process.
  - **Usage:** `devshell.sh --gpu` (or `NATRON_DEV_GPU=1` for `build.sh`/`test.sh`) runs `natron-dev-gpu` with `/dev/dri` and `RADV_PERFTEST=transfer_queue`. The env var is required for the transfer-only family on this Mesa.
  - **Results:** full ctest in the GPU container is 1274/1275; the only failure is the known CImg asset. Lavapipe is still selectable with `VK_ICD_FILENAMES`.
  - **CI:** unaffected, because the workflows use the base image directly.
  - **Host follow-up:** the host still needs the udev rule (`KERNEL=="renderD*", SUBSYSTEM=="drm", MODE="0666"`) for this to survive reboots.
- 2026-10-10 — The M84 branch is stacked on `milestone/m83-gpu-backend-spike` while PR #48 waits on CI (user: work ahead). Rebase onto `main` after #48 squash-merges, dropping the M83 commits. M84 commits so far after the stacking rebase: `62bc42d6a` Blur buffer, `3e29ed34b` graph_bench (merged with M83's `BENCH_SIZE`), `d0995e115` dev container.
- 2026-10-10 — M84.P1.T2 done. **Host-import cost on RADV.** **Decision for P4.T1: a recycling pool for large page-aligned buffers**, not per-allocation import caching.

  | Size | pin (touched) | unpin | pin (untouched, page faults) | fresh `aligned_alloc` + touch | free |
  |---|---|---|---|---|---|
  | UHD | 2.4–3.6 ms | 0.14 ms | ~30 ms | 40–44 ms | ~6 ms |
  | 8K | 12–29 ms | 0.45 ms | ~135 ms | ~157 ms | ~23 ms |

  - **Above 4 GiB:** a 4 GB import fails on RADV (`VK_ERROR_UNKNOWN`), so imports must stay under that size.
  - **UHD round-trip per frame:**
    - fresh buffers: 105–126 ms;
    - recycled buffers with cached imports: 10.7–11.2 ms;
    - recycled buffers through staging: 19.5–20.4 ms.
  - **Why a pool:** fresh allocation and first touch dominate, so only recycling makes host-import pay off.
- 2026-10-10 — **Correction to M84.P1.T2's "4 GB import fails":** the failure happens only at sizes that are an **exact multiple of 4 GiB**. Imports of 4 GiB, 8, 12 and 16 GiB return −13; 4 GiB ± 4 KiB, 5 GiB, 6 GiB and 20 GiB + 4 KiB succeed. This points to a 32-bit size truncation in RADV's userptr path. The cause is not GTT (46.95 GiB), memlock, or VRAM.
  - RADV advertises `maxMemoryAllocationSize` = 4 GiB − 4 but doesn't enforce it: 9 GiB device-local allocations succeed.
  - Host-pointer imports map to memory type 5 only (the GTT heap, cached).
  - **Engine rule:**
    - clamp each import to `maxMemoryAllocationSize`;
    - never submit an exact multiple of 4 GiB;
    - treat −13 from an import as "fall back to staging", not fatal;
    - split frames bigger than that into per-strip imports. This follows already from `maxStorageBufferRange`.
  - The earlier M83 finding that "a single host-import copy over 4 GiB is wrong" is likely the same truncation.
  - Probe: the scratchpad `import-probe/`, which was not committed.
- 2026-10-10 — #48 merged. Once the in-flight tasks commit, rebase M84 with `git rebase --onto origin/main 87dcb403a`; `87dcb403a` is the M83 tip the branch is stacked on.
- 2026-10-10 — M84.P4.T3 done. `DeviceImage` exposes `downloadToRam()`/`evictToRam()` returning an `ImagePtr` (rather than `downloadTo(Image&)`); `bytesInFlight()` returns the host figure, so the scheduler is unchanged. Device bytes are counted per entry, not de-duplicated across keys; revisit if identity tasks store device images (P7.T2). Full ctest 1280/1280.
- 2026-10-10 — M84.P2.T1 done (`85ea0d6b7`). `NatronGpu` builds from the root, and `-DNATRON_GPU=OFF` builds. The ldd difference is only `libvulkan.so.1`. Targeted ctest passed 30/30. `slang_add_kernel` emits a header-only SPIR-V target by default, and `CPP` gives the C++-twin library. Kernel target names (grade, blur, pointop_*) match the spike's, so P2.T4 must resolve that collision. `tools/ci/local/test.sh` has no ctest filter; worth adding when convenient.
- 2026-10-10 — M84.P2.T2 done (`1583f1ee8`). `GpuTests` gives every ctest name a `Gpu.` prefix (`GpuNoPushDescriptor.` for the second registration), so `ctest -R '^Gpu'` selects all of them. Results: 82/82 on RADV in the container. On lavapipe with `NATRON_TEST_REQUIRE_GPU=1`, everything passes except four GL-interop zero-copy cases, which skip for want of interop rather than a device; that matches the CI-coverage decision. On the host (`run-host.sh`, RADV) 45/45 pass, including every GlInterop and semaphore case.
- 2026-10-10 — M84.P4.T4 done (`0a165a17f`). The counters and the region record format exist, but nothing feeds them yet: P7.T2 must call the `note*` methods and `formatRegionRecord`. P6.T1 must stop `formatTaskRecord` hard-coding `"device":"cpu","regionId":-1`. The report self-test asserts that region records are skipped and the new fields are read.
- 2026-10-10 — M84.P4.T1 done (`11c1f9f49` after the rebase). **Pool accounting:** the pool is process-global, so its bytes are not added to a cache's size. Instead the cache trims the pool to its headroom (`limit − memoryCacheSize`) before evicting. That counts idle pooled bytes against the limit without double-counting them or evicting live entries. **Hooks under the lock:** `trimTo` runs the free hooks while the cache lock is held, so P4.T2's import-cache `forget` hook must not take a lock that a cache caller can hold. **Bench:** the quick CPU bench is folded into P8.T1, which compares against `main` anyway.
- 2026-10-10 — **Floating-point trap fix** (`7fe6a04ea`). In taskgraph mode, `GLScheduler.GLNodeRendersInsideATaskAndStoresRAM` failed intermittently with SIGFPE in llvmpipe's shader compile. Pool threads inherited the debug FP traps from whichever thread spawned them. `NodeTask::run` now sets the traps itself, and `OSGLContext` masks them while it creates the context. Full ctest passes 1370/1370 in both modes.
- 2026-10-10 — **Rebased onto `main`** (`6b0ec1f42`, M75) with `--onto origin/main 87dcb403a`. It was clean, and the GPU paths are byte-identical to before. Old tip kept as local branch `m84-pre-rebase-backup`.
- 2026-10-10 — **Open build issue** (fix in P2.T3): `cmake/FetchSlang.cmake` defaults the deps cache to `$HOME/.cache/natron-deps`, and the CPP-kernel targets bake that absolute path. `natron-dev` and `natron-dev-gpu` share `build/debug` but have different `HOME`s, so `build.sh debug` from the other container fails to find `slang-cpp-prelude.h`.
- 2026-10-10 — M84.P2.T3 committed (`fb3718b86`). The default deps cache is now the repo-local `.natron-deps` (fixing the cross-container build), and CI and release cache it. Both ctest steps use lavapipe with `NATRON_TEST_REQUIRE_GPU=1`. The task is checked off once a dispatched CI run on the branch is green with GpuTests passing.
- 2026-10-10 — M84.P2.T4 done (`3289f9613`). The spike builds against `NatronGpu` using the root cmake helpers. Its CPU twins are named `grade_twin`/`blur_twin` to avoid the collision, and `slang_add_glsl` moved to `tools/gpu-spike/SlangGlsl.cmake`. `GpuBench` accepts a software device under `GPU_SPIKE_ALLOW_SOFTWARE=1`. `GpuBench --sizes uhd` and `CpuTwinBench` run on lavapipe.
- 2026-10-10 — M84.P3.T1 done (`e97868b96`). `createHostImported` **rejects** sizes above `maxMemoryAllocationSize` (`fallbackToStaging`) instead of clamping them; callers split. GpuTests: 100/100 on lavapipe (4 GL zero-copy skips) and on RADV in the container; 54/54 on the host.
- 2026-10-10 — M84.P3.T2 (`7fb410354`), P3.T3 (`32fcb9fee`) and P3.T4 (`080727d46`) done. GpuTests: 138/138 on lavapipe (4 GL zero-copy skips) and on RADV in the container, and 73/73 on the host. No Vulkan validation layer is installed in the container or on the host, so the ownership-transfer barriers are proven only by results on RADV's separate transfer family.
  - **Transfer API:** calls return `GpuStatus`, with `TimelineValue` as an out-param. `ComputeHandoff` is `Submit`, `Record` or `None`. After a download, a range belongs to the transfer family. The host-import cache is **opt-in** (`cacheHostImports`) until P4.T2 wires `forgetHostAllocation` as the HostImageMemory free hook. `forgetHostAllocation` may wait on the device when an import is still in use, so it must not be called under the cache lock (see P4.T1's note).
  - **Registry:** binding count, push bytes and group size come from Slang reflection. `slang_add_kernel(... REGISTRY NatronGpu)` plus `slang_write_kernel_table` generates `GpuKernels.inc`. Test kernels register at runtime. `GpuRecordContext` is thread-bound.
  - **Pool:** size classes go up in quarter-power-of-two steps (≤25% waste). Eviction callbacks run outside the lock and may fire after the owner dropped its handle. A concurrent `acquire` can report OOM early while another thread's callback is still running.
- 2026-10-10 — M84.P6.T2 done (`5a7e29fd2`). `Engine/GpuCostSeed.inc` defines three macros: CPU rows (18 plugins, serial ms/Mpx and speedup), Blur σ rows (in situ, hard-coded) and GPU rows (UHD kernel-only, RX 7900 XTX). P6.T3 should note two things. The GPU rows are UHD-only, so wide blurs at 8K+ are under-estimated. Transform's serial cost of 996 ms/Mpx is as measured.
- 2026-10-10 — M84.P3.T5 (`2b44cc6ce`), P5.T1 (`7da9d9e31`) and P6.T1 (`5335e5c5e`) done. Full ctest passes in both modes once two test fixes are in (a non-user-creatable plugin filter, and a fixed pool budget in the GpuRuntime tests, since RADV's live budget drifts). GpuTests: 164/164 on lavapipe and on RADV, 82/82 on the host.
  - **Runtime:** fault ops count from 1 and are every `GpuDevice::submit` plus every `GpuRuntime::acquire`. `recreate()` invalidates all outstanding handles and contexts. `Degraded` clears only on `recreate()`.
  - **Placement deviations:** convexity forbids a cycle through **any** outside task, including another region, not just CPU tasks. `acceptRegion` takes `(const FrameGraph&, const Region&)`. GPU availability is a function-pointer hook, `setGpuPlacementAvailableFunc`, which is null today. **Enable it only together with P7.T2's region body**, or tasks get labelled `gpu` but still render on the CPU. Identity tasks never start a region. Multi-plane tasks are never candidates.
- 2026-10-11 — **GPU tests are gated on availability (user).** Most development happens on hosts without a GPU, and CI has none. GPU tests skip with a reason whenever what they need is missing. Vulkan compute tests run on any Vulkan device, **lavapipe included** (user chose this over skipping on software devices), and skip only when no device exists. CI keeps `NATRON_TEST_REQUIRE_GPU=1` on lavapipe. GL interop availability is separate: a missing EGL/GL device skips even under `NATRON_TEST_REQUIRE_GPU`.
- 2026-10-11 — First branch CI run (38104592140, at `e97868b96`): 92 GPU tests passed on lavapipe in both scheduler modes. Three `GlInteropTest` readback cases segfault because CI has no EGL device; a fix is in progress. ctest takes about 10 min per mode.
- 2026-10-11 — M84.P4.T2 done (`50b6cf2bd`) and M84.P7.T1 done (code on the branch tip). Full ctest passes in both modes, at 775 s against 737 s before; the per-process `GpuContext` startup cost is small.
  - **`GpuContext` notes:** the free hook stays synchronous, so a stall under the cache lock is bounded by in-flight device work. Settings apply at the next launch. The `Tests` binary brings up a context in Auto mode. Placement is **still not registered**.
  - **Region notes:** a region's stand-in tasks are appended to the frame's copy of `graph.tasks`. With no body hook, a region fails its frame. Regions write no profile record yet and get the normal thread budget (P7.T2 and P7.T3 fix both). Shared-cache de-duplication doesn't cover region members.
- 2026-10-11 — **GL interop gating fixed.** The bundled gtest has no `GTEST_SKIP`, so `GPUTEST_SKIP()` in `SetUp` let test bodies run. Every GlInterop case now checks readiness. `GlInterop::probe()` fails when no GL context is current, so the engine won't crash on GPU-less hosts. **Watch for the same pattern elsewhere:** any `GPUTEST_SKIP` in a fixture's `SetUp` must be re-checked in each test body.
- 2026-10-11 — M84.P2.T3 verified: CI run 38110360851 is green, with 165 GPU tests passing on lavapipe in both modes.
- 2026-10-11 — M84.P7.T3 (`c302bfb61`), P6.T3 (`c6966f0bb`), P5.T2 (`4702401a2`) and P5.T3 (`ef075e4c6`) done. Full ctest passes 1635/1635 in both modes. On lavapipe 211/211 pass; on RADV 190/190; on the host 83/83.
  - **Cost model:** a lone UHD Grade goes to the GPU even at 25 GB/s with the measured seeds (CPU in-graph about 12 ms/Mpx against about 1.3 ms/Mpx of transfers), which agrees with M83. The "stays on the CPU" test pins CPU Grade to 1 ms/Mpx. `acceptRegion` became `regionGain` (numeric). The margin is 0.5 ms and the EMA weight 0.25.
  - **Blur:** the GPU result is bit-identical to the CPU FIR. Channel subsets are refused. `getGpuInputRoI` has no time or view, so it reads knobs at the current time.
  - **For P7.T2:**
    - wire `GpuCostModel::session().observeCpu/observeGpu` into `executeTask` and the region body;
    - wire `setDeviceBytesBudget` from the pool budget;
    - register `setGpuPlacementAvailableFunc`.
  - **Follow-ups:**
    - `cmake/FetchSlang.cmake` caches `_slangc` and `_slang_inc`, so stale build dirs keep old paths. Use `NO_CACHE` or unset the variables before searching.
    - `tools/gpu-spike/bench/GpuBench_main.cpp` passes the full Blur weight table to the new half-kernel layout, so its pixels are wrong, though the timings still hold.
    - After a `submitAndWait` timeout, scratch buffers return to the pool while the device may still use them.
- 2026-10-11 — **Paused: usage limit.** Next is P7.T2 (the region body, L), then P7.T4, P7.T5, P7.T6, Phase 84.8 and the gate.
- 2026-10-11 — **Resumed** after the usage-limit pause, in a GPU-only PM session running alongside other PM sessions.
- 2026-10-11 — M84.P7.T2 done (`5bb32cbb2`). Placement is now **live**: the scheduler's constructor installs the region body and registers `setGpuPlacementAvailableFunc` together. Full ctest passes 1639/1639 in both modes (CI environment, lavapipe). GpuResidency 4/4 in both modes.
  - **Deviations:**
    - a download is issued once the output's producer *and* every later member that reads it are submitted, because after a download the buffer belongs to the transfer family;
    - freed intermediates go back to the pool only after their submission finishes;
    - an output read by both the CPU and another region is downloaded and stored in RAM only, and the other region uploads it;
    - `RegionBody` takes a `RegionReport*` so the scheduler writes the profile records;
    - `AppManager::swapGpuContextForTests` forces a context in tests, since Auto refuses lavapipe;
    - `observeCpu` skips image-cache hits (`EffectInstance::getRenderActionsOnThisThread()`);
    - the convex-split test assigns its two regions by hand, because placement can't produce region→region with single-input GPU nodes.
  - **Bug fixed:** downloaded images carry the task's node, hash, time and view. With an empty key, OFX clips reused a wrong-time image, so FrameBlend got the wrong frame.
  - **For P7.T4:** any region-body failure currently fails the frame, for example a Grade input that doesn't cover its window, or an empty window.
  - **Known gaps:**
    - per-member GPU time is estimated from the cost model, not measured with timestamps;
    - under `NATRON_GPU=force`, 9 Legacy-vs-taskgraph bit-exact tests differ by one float rounding step (expected; not a CI configuration).
  - **Verify note:** running ctest directly needs `OFX_PLUGIN_PATH=build/assets/Plugins` and `xvfb-run`, as `test.sh` sets them.
- 2026-10-11 — M84.P7.T4 done (`06d9859db`). Full ctest passes 1649/1649 in both modes (lavapipe, `NATRON_TEST_REQUIRE_GPU=1`).
  - **Behaviour:**
    - OOM: evict, retry once on the GPU, then render the members on the CPU in the super-task.
    - Every other refusal or GPU error also falls back to the CPU per region, instead of failing the frame.
    - Device loss restarts the frame CPU-only without completing the `FrameFuture`.
    - `GpuContext::recoverIfLost()` runs from `placeRenderGraph`. It recreates only once no frame holds the runtime, and backs off 10 s after a failed recreate.
    - Fallback reasons: "out of device memory", "GPU refused", "GPU error", "no GPU", "device lost". Retries are counted separately (`noteGpuRegionRetry`).
  - **Deviations:**
    - small additions in `Engine/Gpu/`: `GpuDevice::injectFault(kind, n)` counts from now, because `NATRON_GPU_FAULT` counts from creation, calibration included; `GpuDevice::drainQueues()`; and `GpuBufferPool::evict(bytes)`;
    - `RenderScheduler::renderTaskOnCpu` is factored out;
    - a one-shot OOM succeeds on the retry, so the "one fallback" check uses a pool budget forced to 1 byte;
    - the shrunk budget applies through admission only; placement itself ignores the device budget, and P7.T5's stripping covers oversized regions.
  - **Known gaps:**
    - a CPU fallback runs with thread budget 1, so it's slow for a big Blur;
    - fallen-back members are still profiled as `device:gpu`, and the region record has no fallback field;
    - tasks dropped by a loss restart count as purged.
- 2026-10-11 — M84.P7.T5 done (`f13cbf53d`). Tiled Grade → Blur σ25 → Grade matches the untiled render exactly (diff 0) on lavapipe with a 1 MB range override. Full ctest passes 1656/1656 in both modes.
  - **Design:**
    - `planGpuStrips` mirrors the body's real allocation pattern, including Blur's held intermediate and weight buffers.
    - Strips are full-width bands, 4-row multiples of up to 2048 rows, binary-searched for the tallest that fits.
    - Inputs are uploaded with halos rounded to 4 rows. Only band rows are downloaded.
    - A stripped region downloads **every** output to RAM, since the output never exists whole on the device.
    - New fallback reason "too large for the GPU" (no retry) for the storage-buffer range. Over budget takes the "out of device memory" path and marks the runtime Degraded.
    - The test hook is `setGpuStripOverridesForTests`.
  - **Follow-ups (for P8.T2 or M85):**
    - strips run serially (upload, compute, download, wait), with no overlap;
    - offset halo uploads mostly go through staging, not host import;
    - the spatial-node scratch cost is a Blur-shaped heuristic;
    - admission reserves a stripped region's whole working set, so it is admitted only when the scheduler is idle; cap it at the budget;
    - transfer counts are per strip.

- 2026-10-11 — M84.P7.T6's test is committed (`5704714b1`), but the task is left unchecked. The budget holds (pool peak exactly 64 MB), there are no fallbacks and the pixels match, but **evictions are 0**. Admission estimates a region at ~2× its real peak (60.8 MB against 32 MB), so GPU regions never overlap, and a continuation runs straight after its producer.
- 2026-10-11 — **Reactive GPU memory admission (user).** An estimate can't be trusted for arbitrary kernels: OFX GPU plugins allocate outside our pool, and native-kernel scratch would need hand-written declarations. Correctness already rests on the pool's eviction and the P7.T4 retry/fallback, not on prediction. So device-byte admission is dropped in favour of the concurrency cap plus pool enforcement. Strips are planned from known I/O sizes with headroom, and out-of-memory halves the strip height before the CPU fallback. This **supersedes the device-byte half of the P7.T3 approach**. "Continuations always run" is kept. New task M84.P7.T7.
- 2026-10-11 — **Strips must stream (user):** a stripped region must overlap transfers with compute rather than run upload → compute → download → wait per strip. This was a P7.T5 follow-up, and is now in M84 as M84.P7.T8, ahead of the P8.T2 bench, so the 24k numbers reflect it.
- 2026-10-11 — M84.P7.T7 done (`054aa9f91`), and M84.P7.T6 checked off with it. Full ctest passes 1660/1660 in both modes. `LargeCompStaysWithinTheDeviceBudget` passes 10/10: pool peak exactly 64 MB, 1–2 evictions per run, no CPU fallbacks, pixels match.
  - **Admission:** device-byte admission is removed (`estimatedDeviceBytes`, `_reservedDeviceBytes`, the budget setter). Store device-byte reporting, `notePeakDeviceBytes` and `Region::workingSetBytes` (the evict amount before a retry) are kept.
  - **Headroom:** strips are planned from I/O sizes against the pool budget, capped at device memory − 1 GiB (or half the device on small cards). See `kGpuStripOutsidePoolHeadroom`.
  - **Out-of-memory ladder:** evict and retry once, then halve the strip height down to 4 rows, then the CPU fallback (`noteGpuStripHalving`).
  - **Deviations in `Engine/Gpu/`:**
    - pool refusals caused by contention (`GpuStatus::budgetContention`) no longer shrink the budget or mark the runtime Degraded. Driver refusals, oversize requests and injected faults still do;
    - `GpuRecordContext::ranOutOfMemory()` makes a scratch out-of-memory halve the strips instead of reading as "GPU refused".
  - **Test changes:** LargeComp no longer asserts `retries == 0` / `uploads == 8` / `store downloads == 0`. `WorkingSetOverTheBudgetRunsAsStrips` now uses a 4 MB budget. A new test, `OutOfMemoryHalvesTheStripsInsteadOfFallingBack`, is added.
  - **Follow-ups:**
    - two contending regions can halve each other into thin strips; consider waiting briefly for the other region before halving;
    - remember a working strip height per region shape;
    - the error log is noisy under contention.
- 2026-10-11 — M84.P7.T8 done (`1c06ea503`). Full ctest passes 1668/1668 in both modes on lavapipe. On RADV, GpuResidency + GpuRegionStrips + `Gpu.*` pass 113/113 in each mode.
  - **RADV overlap** (debug build, Grade → Blur σ25 → Grade at 16384², 8 strips, second render):

    | Mode | Wall | Device upload + compute + download | Transfer hidden / exposed |
    |---|---|---|---|
    | serial | 930.5 ms | 212 + 461 + 257 ms | 0 / 470 ms |
    | pipelined, 3 strips in flight | **504.3 ms** | 162 + 460 + 152 ms | 274 / 39 ms |

    On a first render, pinning freshly allocated destination memory costs 100–250 ms per strip on the region thread: serial 2471 ms, pipelined 1670 ms. Reproduce with `NATRON_GPU_PIPELINE_BENCH=16384` and the filter `GpuResidencyTest.PipelinedStripsOverlapBenchmark`, which is skipped by default.
  - **Design:**
    - order per strip: retire strip k−D, upload k, submit compute k, download k−1;
    - each run has its own compute timeline semaphore, and `ComputeHandoff::Record` folds the ownership acquire and release into the strip's compute command buffer;
    - the planner takes 3 in flight at the serial height if that fits, otherwise 2 at ≥ 64 rows, otherwise serial;
    - it pipelines only with a separate transfer family **and** push descriptors. A test hook forces it on or off.
  - **Out-of-memory ladder** is now: evict and retry → serial strips (`noteGpuStripPipelineSerialized`) → halve strips → CPU.
  - **Stats:** GPU timestamps fill the region record (`wallMs`, `strips`, `stripsInFlight`, `transferHiddenMs`, `transferExposedMs`) and `RenderStats`.
  - **Deviations:** also touched `GpuRecord` (non-blocking timeline submit, `Record` handoff, timestamps), `RenderScheduler`/`RenderStats`/`RenderProfile_Test` (stats; `formatRegionRecord` takes `GpuRegionTimes`) and `setGpuStripSubmittedHookForTests`.
  - **Follow-ups:**
    - first-frame pinning of fresh download images on the region thread; pre-fault, or import on a helper thread;
    - a wait timeout without loss can return buffers in use (pre-existing);
    - contending regions on the out-of-memory ladder.
- 2026-10-11 — M84.P8.T1 done: **no CPU-only regression; the milestone is faster.** Release (RelWithDebInfo) `main` `cd0f3f5f2` against the tip `1c06ea503`, `NATRON_GPU=off`, task-graph mode, UHD, interleaved, 3 rounds:

  | Case | `main` | Milestone | Change |
  |---|---|---|---|
  | chain 30 | 1.12 s | 0.74 s | −34% |
  | comp 50 | 1.76 s | 1.31 s | −25% |
  | blurchain 10 | 1.11 s | 0.63 s | −43% |
  | keying_grade, 12 frames | 13.26 s | 11.92 s | −10% |

  The noise band is ≤ 2% (3.8% on keying_grade). The likely cause, not profiled, is the `HostImageMemory` pool plus the uninitialised Blur buffer. Logs are in the untracked `.local/m84p8/`; the `main` build is in the worktree `build/wt/main`.
- 2026-10-11 — M84.P8.T2 run: **the direction holds.** RADV container, release tip, Auto vs Off, per-frame median including the Write. G-B-G is CheckerBoard → Grade → Blur σ25 → Grade, added in a local copy of `graph_bench.py`:

  | Case | Off | Auto | Speedup |
  |---|---|---|---|
  | G-B-G, UHD | 0.279 s | 0.143 s | 1.95× |
  | G-B-G, 8192² | 2.70 s | 1.49 s | 1.82× |
  | G-B-G, 16384² (9 strips, 3 in flight) | 10.18 s | 4.44 s | 2.30× |
  | G-B-G, 24000² (12 strips, 3 in flight) | 22.02 s | 9.10 s | 2.42× |
  | blurchain σ25, UHD | | | 1.57× |
  | blurchain σ25, 8192² | | | 1.49× |
  | blurchain σ25, 16384² | | | 1.85× |
  | blurchain σ25, 24000² | | | 1.96× |
  | blurchain σ1.25, UHD | | | 1.38× |
  | keying_grade, UHD, 12 frames | 12.15 s | 10.84 s | 1.12× |
  | cg_multipass, UHD, 12 frames | 12.2 s | 9.7 s | 1.26× |

  No comp is slower under Auto. The comp runs overlapped load of 4–6 from other sessions.
  - **Placement:** keying_grade forms 55 regions, 44 of them single-node (Grade and Blur on the GPU, keyers and morphology on the CPU). cg_multipass forms 110 regions, 88 single-node. A single-node region spends about 12 ms of wall, about 11 ms of it exposed transfer around a ~2 ms kernel. If a slower link ever regresses, check the bandwidth calibration first. The warm-up frame runs fully on the CPU.
  - **Finding:** the stripped regions at 16k and 24k hide only 0–20 ms of transfer against 295–810 ms exposed. **The strip pipeline is not overlapping in real renders.** New task M84.P7.T9 fixes this, and M84.P8.T2 is checked off once the 16k and 24k rows are re-run after it.
- 2026-10-11 — M84.P7.T9 done (`67dba4f4a`), and M84.P8.T2 checked off with its re-run.
  - **Root cause:** `HostImageMemory`'s pool cap was 512 MB, so 16k and 24k frame buffers (4.3 and 9.2 GB) never recycled. Every frame pinned fresh, untouched memory on the region thread, at 94–106 ms per strip download, so the queues serialised. P7.T8's benchmark hid this by raising the cap itself.
  - **Fix:**
    - the default cap is max(512 MB, RAM/4);
    - buffers ≥ 32 MB are 2 MB-aligned with `MADV_HUGEPAGE`.
    - `GpuRegionTask` and `GpuTransfer` are unchanged.
  - **RADV release, G-B-G σ25, interleaved, 2 rounds:**

    | Case | Off before | Off after | Auto before | Auto after |
    |---|---|---|---|---|
    | 16k | 10.3 s | 8.2 s | 4.5–4.7 s | **3.0–3.1 s** |
    | 24k | 21.9–22.0 s | 17.7–18.0 s | 9.7–9.9 s | **6.4–6.6 s** |

    Region transfer hidden / exposed is now 276/38 ms at 16k and 627/78 ms at 24k (before: ~20/300 and 0/805). The region is compute-bound, and the remaining exposed time is the first upload plus the last download.
  - **Rest of the frame:** at 24k, CheckerBoard (~1 s) and the Write (~4.4 s) on the CPU now dominate. Off mode is also 18–21% faster.
  - **Follow-ups:**
    - first-frame imports of fresh destinations take ~50 ms per 512 MB strip on the region thread; import ahead on a helper thread, which needs `resolveImports` outside `importMutex`;
    - the pool can hold up to RAM/4 plus pins when idle; consider an idle trim or a setting;
    - ≤ 64 GB machines won't fully recycle 24k frames.
  - **Found:** a pre-existing release-only segfault in the device-loss test on lavapipe → new task M84.P7.T10.
- 2026-10-11 — M84.P7.T10 done (`a9048b4ec`). **The cause was in the test, not the engine.** `GpuResidencyTest::SetUp`'s forced context logged through a lambda that captured a `SetUp` stack local by reference. `recoverIfLost()` logs through that sink after `SetUp` returns, which corrupts the stack, and the release build crashed on it. It shows only on lavapipe, because RADV uses the app's own context. The fix holds the sink by `shared_ptr`, and the `GpuContextOptions::infoLog` doc now says it is called for the context's whole life. The test passes 20/20 in release and debug in both modes. Full ctest passes 1669/1669 in debug and release, in both modes.
- 2026-10-11 — M84.P8.T3 done (plan-only). The hand-offs are in M85 - Native GPU Kernels and M86 - OFX 1.5 GPU Suites, and the design note has a §9 outcomes section.
