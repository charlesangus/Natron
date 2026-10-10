# M83 - GPU Compute Backend Spike

The engine direction is already settled: Vulkan compute, with kernels written in Slang (user decision of 2026-10-10). This spike builds the smallest piece of that stack that M84 - GPU Placement And Residency can later move into `Engine/` as it is. The pieces are:
- headless device and queue setup, with VMA for allocation;
- pinned staging where upload, compute and download overlap, synchronised by timeline semaphores;
- one point kernel (Grade) and one neighbourhood kernel (a separable Gaussian blur), each in Slang, compiled to SPIR-V and to Slang's C++ target, which serves as the CPU twin;
- a zero-copy Vulkan → GL hand-off that the Qt viewer can consume.

It measures all of this on the RX 7900 XTX and checks it for correctness on lavapipe in the `natron-dev` container. GL 4.3 compute and OpenCL 3.0 are reference measurements only. Each has a timebox and can be dropped. The result is a design note and a decisions entry. Together they name the backend, the kernel language, the CPU-fallback strategy and the GL interop path, with measured numbers, the tolerances found, the build and deployment cost, and an API sketch for M84.

Scouted facts this plan relies on (2026-10-10):
- **Container (`natron-dev:2027-clang21.1`):**
  - Present: `vulkan-headers` and `vulkan-loader-devel` 1.4.328, and `mesa-vulkan-drivers` 25.2.7. A probe found lavapipe ("llvmpipe (LLVM 21.1.8)", Vulkan 1.4, `VK_KHR_external_memory_fd`). Also `libEGL_mesa` and SPIRV-Tools libraries.
  - Missing: glslang, slangc, VMA and any OpenCL ICD (only the CUDA ICD loader and the headers are present).
  - Even with `--device=/dev/dri`, the quick probe still enumerated only lavapipe, not RADV.
  - There is no distro package repo, but github.com is reachable.
- **Host:**
  - Present: Debian 12, RADV and radeonsi (Mesa 25.0.7), Slang 2026.19 at `/opt/slang` (it ships a CMake config and `slang-cpp-prelude.h`), glslangValidator and g++ 12.
  - Broken or missing: host `cmake` fails with a libarchive symbol error, there is no OpenCL ICD (ROCm 7.2.4 is installed without its OpenCL runtime), and there are no Qt 6.8 dev files.
- **Slang release tarball:** `slang-2026.19-linux-x86_64.tar.gz` is 83 MB (251 MB unpacked).
- **M82.P2.T1 bench (`build/gpu-transfer-bench/`, results already in):**
  - DMA runs at 24–27 GB/s each way.
  - The cost of a UHD float frame is dominated by the memcpy into and out of staging. Upload is 10 ms memcpy plus 5 ms DMA. Download is 5 ms DMA plus 24 ms memcpy.
  - So skipping the staging copy is worth measuring.
- **Engine:**
  - `PixelKernel::processRow` (`Engine/Nodes/Image/PixelKernel.h`) works on interleaved float rows.
  - `GradeKernel` (`Engine/Nodes/Color/Grade.cpp`, anonymous namespace) computes in double.
  - Native Blur (`Engine/Nodes/Filter/BlurKernels.*`) is IIR (Deriche, van Vliet) plus box.
  - The viewer uploads through two PBOs in `ViewerGL::transferBufferFromRAMtoGPU` (`Gui/ViewerGL.cpp:1557`).
  - Natron's glad is GL 2.0 compat plus a few ARB/EXT extensions, with no compute and no `EXT_memory_object`.

## Phase 83.1: Toolchain and scaffold

- [x] M83.P1.T1 — Create the standalone `tools/gpu-spike/` CMake project with a pinned Slang toolchain
  - files: `tools/gpu-spike/CMakeLists.txt`, `tools/gpu-spike/cmake/FetchSlang.cmake`, `tools/gpu-spike/cmake/SlangKernels.cmake`, `tools/gpu-spike/kernels/smoke.slang`, `tools/gpu-spike/tests/Smoke_Test.cpp`
  - approach:
    - A self-contained C++20 project, not added to the root `CMakeLists.txt`. Dependencies: `find_package(Vulkan REQUIRED)`, `find_package(GTest)` (the image has it in `/usr/local/lib/cmake/GTest`), and Slang.
    - `FetchSlang.cmake` uses `SLANG_ROOT` when it is set (`-DSLANG_ROOT=/opt/slang` on the host) and then calls `find_package(slang CONFIG)`.
    - Otherwise it downloads `https://github.com/shader-slang/slang/releases/download/v2026.19/slang-2026.19-linux-x86_64.tar.gz`, checks its SHA256 and extracts it.
    - The download goes into `${NATRON_DEPS_CACHE:-$HOME/.cache/natron-deps}`, so reconfigures and fresh build dirs don't fetch it again.
    - `SlangKernels.cmake` provides `slang_add_kernel(<target> <file.slang> ENTRY <name>)`:
      - an `add_custom_command` runs `slangc -target spirv` into a generated `const uint32_t[]` header, so no Slang runtime is needed;
      - it also runs `slangc -target cpp` into a `.cpp` that compiles against the Slang prelude headers.
    - No `libslang` is linked into any spike binary.
    - Build it in the container with `cmake -S tools/gpu-spike -B build/gpu-spike -G Ninja -DCMAKE_PREFIX_PATH=/usr/local`. It is independent of `tools/ci/local/build.sh`, so it never takes the Natron build slot.
  - verify:
    - A clean configure and build in `natron-dev` fetches and verifies the tarball. This also proves that `slangc` runs on the image's el9 glibc.
    - A second configure in a new build dir makes no network access.
    - `Smoke_Test` checks that the embedded SPIR-V starts with magic `0x07230203` and that the C++-target smoke entry point runs and writes the expected values.
    - Record the configure and build wall time.
  - size: M

- [x] M83.P1.T2 — Vendor Vulkan Memory Allocator
  - files: `libs/VulkanMemoryAllocator/include/vk_mem_alloc.h`, `libs/VulkanMemoryAllocator/LICENSE.txt`, `libs/VulkanMemoryAllocator/VERSION`
  - approach: Copy the single header from tag `v3.3.0` of GPUOpen's VulkanMemoryAllocator (MIT), unmodified, with its licence and a version and SHA note. It goes under `libs/`, next to the other vendored deps, because M84 will include it from `Engine/`. Nothing in the main build references it yet.
  - verify: `sha256sum` matches the upstream tag's file. A one-file compile check (`#define VMA_IMPLEMENTATION`, against the container's Vulkan headers) compiles warning-free with the project's warning flags.
  - size: S

- [x] M83.P1.T3 — Establish the host-GPU run path
  - files: `tools/gpu-spike/run-host.sh`, `tools/gpu-spike/tests/DeviceList_main.cpp`
  - approach:
    - Speed runs need RADV. The quick probe saw only lavapipe inside the container, even with `--device=/dev/dri`. Find out why: device node ownership showed up as 65534 in the probe, and the ICD JSON or libdrm-amdgpu may be missing in the image.
    - If the container can reach RADV, `run-host.sh` runs the spike there. Otherwise it builds in the container and runs the binaries natively on the host.
    - Native running means the binaries may depend only on `libvulkan.so.1`, glibc and el9's base libstdc++ symbols (gcc-toolset links its newer pieces statically). Check this with `objdump -T` and `ldd`.
    - `DeviceList_main` prints every physical device with its driver ID, its queue families (compute, transfer-only) and the extensions the spike cares about.
  - verify: `run-host.sh DeviceList` prints "AMD Radeon RX 7900 XTX (RADV NAVI31)" with a transfer-only queue family. The same binary run in the container prints lavapipe.
  - size: M

## Phase 83.2: Vulkan core (the layer M84 promotes)

- [x] M83.P2.T1 — Headless device, queues and allocator
  - files: `tools/gpu-spike/src/GpuDevice.h`, `tools/gpu-spike/src/GpuDevice.cpp`, `tools/gpu-spike/src/Vma.cpp`, `tools/gpu-spike/tests/GpuDevice_Test.cpp`
  - approach:
    - Create a Vulkan 1.3 instance with no WSI or surface extensions, so it needs no X, Wayland or EGL. `VK_LAYER_KHRONOS_validation` is used when present; neither the image nor the host has it, so its absence is not an error.
    - Pick the device by the `NATRON_GPU_DEVICE` env override (an index or a name substring), otherwise the first discrete GPU, then integrated, then CPU (lavapipe).
    - Required features: `timelineSemaphore`, `synchronization2`, `storageBuffer16BitAccess`, and `shaderFloat16` where available.
    - Optional, detected and reported: `VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore_fd`, `VK_EXT_external_memory_host`, `VK_EXT_memory_budget`, `VK_KHR_push_descriptor`.
    - Queues: one compute queue, plus a dedicated transfer-only family queue when one exists. Otherwise transfers fall back to the compute queue, and a flag records that.
    - Create the VMA allocator with the budget extension when present.
    - Failures return a status string; there are no exceptions across the API and no aborts. `VK_ERROR_DEVICE_LOST` puts the device into a sticky "lost" state that later calls report. M84 needs this for CPU fallback.
    - Each worker thread gets its own `VkCommandPool`, and queue submission sits behind a mutex. This is the threading model to record for M84: GPU tasks run as single-threaded super-tasks.
  - verify: `GpuDevice_Test` passes in the container on lavapipe with `DISPLAY` and `XDG_RUNTIME_DIR` unset, and on the host on RADV. It asserts that a transfer queue was found (RADV) or the fallback flag is set (lavapipe), and that the VMA budget query returns non-zero.
  - size: M

- [ ] M83.P2.T2 — Kernel pipeline and dispatch
  - files: `tools/gpu-spike/src/GpuKernel.h`, `tools/gpu-spike/src/GpuKernel.cpp`, `tools/gpu-spike/kernels/fill.slang`, `tools/gpu-spike/tests/GpuKernel_Test.cpp`
  - approach:
    - Build a compute pipeline from an embedded SPIR-V blob, with a pipeline cache.
    - Bindings are storage buffers bound with `vkCmdPushDescriptorSetKHR`. If push descriptors are missing, a small per-dispatch descriptor pool is used instead. Parameters go in push constants, up to 128 bytes.
    - Use buffers, not `VkImage`s. Natron's images are interleaved float rows, so a buffer matches the RAM layout byte for byte, with no format conversion on upload or download. Sampled images are left to M85, for filtered Transform.
    - The dispatch helper computes group counts from the kernel's reflected `numthreads`, using `slangc -reflection-json` emitted by `slang_add_kernel`.
    - Time each dispatch with timestamp queries.
  - verify: `GpuKernel_Test` dispatches `fill.slang` (writes `x + y*w`) over a 1921×1081 buffer on lavapipe and RADV, reads it back through a host-visible buffer and checks every value. A reported timestamp delta is greater than 0.
  - size: M

- [ ] M83.P2.T3 — Pinned staging ring with transfer/compute overlap
  - files: `tools/gpu-spike/src/GpuTransfer.h`, `tools/gpu-spike/src/GpuTransfer.cpp`, `tools/gpu-spike/tests/GpuTransfer_Test.cpp`
  - approach:
    - Build a VMA staging ring with N slots (default 3): write-combined host memory for upload and `HOST_CACHED` memory for readback.
    - Uploads and downloads go on the transfer queue, with queue-family ownership transfers to and from the compute queue.
    - One timeline semaphore per queue orders upload k → compute k → download k, while upload k+1 and download k−1 overlap with compute k.
    - Add a second upload/download path through `VK_EXT_external_memory_host`, which imports a page-aligned host allocation directly as a `VkBuffer`. M82's numbers show the staging memcpy costs more than the DMA itself, so this path skips that copy. It falls back to staging where the import is unsupported or the pointer is misaligned.
    - Large frames go through in strips, so a single frame never needs a staging buffer as big as itself.
  - verify:
    - `GpuTransfer_Test` round-trips UHD RGBA float buffers through both paths with a no-op compute in between, and checks them byte-exact.
    - A pipelined run of 16 frames records host-side and timestamp timelines. On RADV it shows transfer and compute overlapping: total time is less than the sum of the stages, with the figure recorded.
    - lavapipe gives a correctness pass only.
  - size: L

## Phase 83.3: Slang kernels and CPU twins

- [ ] M83.P3.T1 — Grade point kernel in Slang, with a C++ twin and a reference
  - files: `tools/gpu-spike/kernels/grade.slang`, `tools/gpu-spike/ref/GradeRef.h`, `tools/gpu-spike/tests/GradeCpu_Test.cpp`, `tools/gpu-spike/CMakeLists.txt`
  - approach:
    - Port `GradeKernel`'s maths from `Engine/Nodes/Color/Grade.cpp` to Slang in float: the A/B precompute on the host, grade and inverse grade, the gamma ≤ 0 edge cases, clamp black and white, and the per-channel process mask over 1/3/4-channel interleaved pixels.
    - `GradeRef.h` transcribes the same maths in double, with a comment citing `Grade.cpp`. The engine version can't be linked here: it sits in an anonymous namespace and pulls in `Python.h`.
    - Compile the Slang kernel through `slang_add_kernel`, which produces both the SPIR-V and the C++ target.
    - This task tests only the CPU side: the Slang C++ twin against `GradeRef` over a value sweep of [−1, 8], NaN and inf, plus random knob sets.
  - verify: `GradeCpu_Test` passes in the container, and reports and asserts max abs and relative error and the ULP distribution (proposed bound: ≤ 4 ULP of float, or abs ≤ 1e-6 near 0). The float-vs-double gap is stated as the expected cost of computing in float.
  - size: M

- [x] M83.P3.T2 — Separable Gaussian blur kernels in Slang, with a C++ twin and a reference
  - files: `tools/gpu-spike/kernels/blur.slang`, `tools/gpu-spike/ref/BlurRef.h`, `tools/gpu-spike/tests/BlurCpu_Test.cpp`
  - approach:
    - Write horizontal and vertical gather passes of a truncated Gaussian FIR (radius ceil(3σ)), with weights normalised on the host.
    - Each pass loads a tile plus halo into `groupshared` memory before the gather.
    - Edges are clamp (Neumann) or zero, matching the `neumann` flag of `BlurKernels`. Input is interleaved float with 1–4 channels.
    - `BlurRef.h` is a direct double-precision two-pass FIR.
    - Natron's own Blur is IIR (Deriche, van Vliet). The test reports, but does not assert, the difference from an IIR result. That number tells M85 how close FIR comes to the current node output.
  - verify: `BlurCpu_Test` checks the C++ twin against `BlurRef` for σ ∈ {0.5, 3, 25, 100}, odd sizes and tiny images (smaller than the radius), with the tolerance asserted (proposed: abs ≤ 2e-6 on [0, 1] inputs).
  - size: M

- [ ] M83.P3.T3 — Check Slang composition for point-op descriptions (feeds M85 and M87)
  - files: `tools/gpu-spike/kernels/pointops.slang`, `tools/gpu-spike/tests/PointOpCompose_Test.cpp`
  - approach:
    - Define `interface IPixelOp { float4 apply(float4 v, uint2 xy); }`. Grade (reusing `grade.slang`) and a trivial Invert implement it.
    - Write a generic `PointKernel<Op : IPixelOp>` entry point, a `Chain<A, B>` fusion and a `Masked<Op>` wrapper (`lerp(v, op(v), mask·mix)` plus premult and unpremult). This is the per-pixel wrapping M85 wants next to `makeKernel()`.
    - Compile specialised entry points to SPIR-V and C++. Inspect the SPIR-V (`spirv-dis`, from the image's SPIRV-Tools libraries or the host) to confirm everything is fully inlined, with no dynamic dispatch.
  - verify: `PointOpCompose_Test` (CPU via the C++ twin) checks that `Chain<Grade, Invert>` equals Invert applied after Grade, and that `Masked` matches a hand-written reference. A recorded note gives the SPIR-V instruction counts for fused and unfused versions.
  - size: M

- [ ] M83.P3.T4 — GPU correctness for both kernels against the references
  - files: `tools/gpu-spike/tests/GradeGpu_Test.cpp`, `tools/gpu-spike/tests/BlurGpu_Test.cpp`, `tools/gpu-spike/tests/Tolerance.h`
  - approach:
    - Dispatch the P3.T1 and P3.T2 kernels through `GpuKernel` and `GpuTransfer` on the same fixtures.
    - `Tolerance.h` compares a GPU buffer with the double reference and reports max abs, max relative, ULP histogram and the location of the worst pixel.
    - Run on lavapipe (container) and RADV (host). Record per-driver differences, especially `pow`/`exp` precision in gamma.
  - verify: both tests pass on both drivers within the bounds stated in P3.T1 and P3.T2. If RADV needs a looser bound, the new bound is written into the test with a reason.
  - size: M

## Phase 83.4: Hand-off to the GL viewer

- [ ] M83.P4.T1 — Zero-copy Vulkan → GL hand-off through external memory
  - files: `tools/gpu-spike/src/GlInterop.h`, `tools/gpu-spike/src/GlInterop.cpp`, `tools/gpu-spike/tests/GlInterop_Test.cpp`, `tools/gpu-spike/tests/GlInteropQt_main.cpp`
  - approach:
    - Export a VMA-allocated device-local `VkBuffer` (dedicated allocation, `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`) and a binary semaphore pair (`VK_KHR_external_semaphore_fd`).
    - In GL, check that `GL_DEVICE_UUID_EXT` and `GL_DRIVER_UUID_EXT` match `VkPhysicalDeviceIDProperties`. Then import the memory: `glCreateMemoryObjectsEXT` → `glImportMemoryFdEXT` → `glBufferStorageMemEXT`.
    - Bind the imported buffer as a `GL_PIXEL_UNPACK_BUFFER` and `glTexSubImage2D` into an RGBA32F texture. This mirrors the PBO path in `ViewerGL::transferBufferFromRAMtoGPU`, so the viewer change in M84 or M85 means binding a different buffer object. The copy stays on the GPU at roughly 0.5 ms for UHD float, per M82's PBO→texture number.
    - Sync with `glWaitSemaphoreEXT` and `glSignalSemaphoreEXT`. Load the entry points with `eglGetProcAddress` or `QOpenGLContext::getProcAddress`, because Natron's glad has no `EXT_memory_object`.
    - The measured test uses EGL surfaceless (`EGL_MESA_platform_surfaceless`), so it is headless and needs no Qt. That lets it run on the host on radeonsi and in the container on llvmpipe with lavapipe.
    - `GlInteropQt_main` repeats the import inside a compatibility-profile `QOpenGLWidget` under xcb/Xvfb in the container. That proves it works on the same kind of context Qt gives the viewer.
    - Importing directly as a texture (`glTexStorageMem2DEXT`) is a stretch measurement only, because tiling compatibility is driver-specific.
    - Fallback: when the extensions are missing or the UUIDs don't match, read back to the host and upload through the existing PBO path.
  - verify:
    - `GlInterop_Test` fills the buffer with a Vulkan kernel, imports it, `glGetTexImage`s the texture and compares byte-exact on RADV/radeonsi. It also runs on lavapipe/llvmpipe, or skips with the missing extension named, and that outcome is recorded.
    - The Qt variant renders under Xvfb and dumps a matching pixel checksum.
    - Record the hand-off latency against the readback-and-PBO fallback at UHD.
  - size: L

## Phase 83.5: Reference backends (timeboxed, can be dropped)

- [ ] M83.P5.T1 — GL 4.3 compute reference on the same kernels
  - files: `tools/gpu-spike/ref-gl/GlCompute_main.cpp`, `tools/gpu-spike/CMakeLists.txt`
  - approach:
    - Timebox: one working day. Emit GLSL 430 from the same `grade.slang` and `blur.slang` with `slangc -target glsl`. The build step gets added to `slang_add_kernel` behind an argument.
    - Run on an EGL surfaceless 4.3 core context with SSBOs and `glDispatchCompute`, timed with `GL_TIME_ELAPSED` queries.
    - It doesn't run on Natron's existing contexts: their glad loader is GL 2.0 compat, and the note will say what regenerating it would cost.
    - Record whether Slang's GLSL output works as it stands, and the timings against Vulkan.
  - verify: the output matches `GradeRef` and `BlurRef` within the P3 bounds on radeonsi. Timings land in the bench CSV. If the timebox runs out, write down why it was dropped instead.
  - size: M

- [x] M83.P5.T2 — ~~OpenCL 3.0 reference on the same kernels (only if a runtime is available)~~ (dropped 2026-10-10, user)
  - files: `tools/gpu-spike/ref-cl/grade.cl`, `tools/gpu-spike/ref-cl/blur.cl`, `tools/gpu-spike/ref-cl/OpenCl_main.cpp`
  - approach:
    - There is no OpenCL ICD on the host or in the image (verified). This task runs only if the user installs one on the host: Mesa rusticl (`mesa-opencl-icd` from bookworm-backports, `RUSTICL_ENABLE=radeonsi`) or ROCm's OpenCL runtime.
    - Slang has no OpenCL C target, so write the two kernels by hand in OpenCL C, structured like the Slang versions. The porting time is itself a data point for M81, whose source kernels are darktable OpenCL. Use the M80.P6.T1 note if it exists.
    - Time with profiling events.
  - verify: matches the references within the P3 bounds, and timings land in the bench CSV. If no runtime is installed, the task closes as dropped with that reason.
  - size: M

## Phase 83.6: Measurement and decision

- [ ] M83.P6.T1 — GPU benchmark driver at UHD through 24k
  - files: `tools/gpu-spike/bench/GpuBench_main.cpp`, `tools/gpu-spike/bench/run-bench.sh`
  - approach:
    - Sizes: UHD, 8K, 16k and 24k RGBA float. At 16k and 24k the frame goes through in strips with blur halos, as a tiled GPU unit, and 24k is also measured as a single frame if VRAM allows (about 9.2 GB).
    - For Grade, Blur (σ = 3, 25, 100) and Grade→Blur kept resident on the GPU, report:
      - kernel time;
      - upload and download through staging and through the host-pointer import;
      - pipelined end-to-end time with overlap on and off.
    - CPU baselines at the same sizes on all 16 threads: Natron's native Grade and Blur through the existing `tools/bench` harness (`graph_bench.py`), run from a Natron build already present.
    - Output is CSV and a short summary table. Runs on the host on RADV.
  - verify: `run-bench.sh` produces a CSV with every cell filled and a second run within ±10%. It reports the break-even point: the CPU time a region must save to pay for one round trip at the measured bandwidth.
  - size: M

- [ ] M83.P6.T2 — Evaluate the Slang C++ target as the CPU fallback
  - files: `tools/gpu-spike/bench/CpuTwinBench_main.cpp`
  - approach:
    - Time the Slang C++-target Grade and Blur, compiled at `-O2 -march=x86-64-v3` and run over 16 threads in row strips, against Natron's hand-written `GradeKernel` and the IIR and box Blur on the same inputs.
    - Inspect whether the generated code vectorises (`-Rpass=loop-vectorize`).
    - Check what the twin needs at build time (prelude headers only) and whether it can be called from a `PixelKernel::processRow`.
  - verify: a recorded table of twin vs native ms/Mpx with a recommendation: twin as the CPU path, twin as test oracle only, or hand-written CPU kept as primary.
  - size: M

- [ ] M83.P6.T3 — Record build and deployment cost
  - files: `tools/gpu-spike/bench/build-cost.sh`
  - approach: Measure and print:
    - Slang tarball size and configure time, cold and cached;
    - `slangc` time per kernel and target;
    - embedded SPIR-V bytes;
    - spike binary size and runtime `NEEDED` libraries (expected: `libvulkan.so.1` only);
    - packaging implications for `tools/ci/local/package.sh`: the loader and ICDs come from the user's system, nothing Slang ships at runtime, and the GPL compatibility of Slang (Apache-2.0 with LLVM exception) and VMA (MIT);
    - added CI time if P6.T5 lands.
  - verify: the script runs in the container and its output is pasted into the design note.
  - size: S

- [ ] M83.P6.T4 — Write the design note and decision record
  - files: `.plan/PLAN/DESIGN/2026-10-xx-gpu-compute-backend.md`, `.plan/PLAN/DECISIONS/2026-10-xx-gpu-compute-backend-chosen.md`, `.plan/PLAN/DECISIONS/INDEX.md`
  - approach: Measured numbers from P6.T1–T3, P4.T1 and P5. The tolerance table per kernel and driver. The CPU-fallback strategy from P6.T2. The interop path and its fallback. Then the handover:
    - **M84 API sketch:** `GpuDevice`, VMA-backed buffers, `GpuTransfer` streams with timeline values, the kernel registry, the threading model (submits behind a mutex, command pool per thread, thread budget 1), the device-lost state machine and the budget query.
    - **Recommendations:** what M68 is re-scoped to (EGL kept only for OFX GL plugins), and porting notes for M81 (OpenCL C → Slang, cross-referencing M80.P6.T1).
    - **For M85:** the FIR-vs-IIR gap and the per-pixel wrapping pattern from P3.T3.
    - **Promotion plan:** which files move from `tools/gpu-spike/src/` to `Engine/Gpu/`.
  - verify: the user reviews and signs off on the decision. M84's stub cites the note.
  - size: M

- [x] M83.P6.T5 — ~~CI step that runs the spike on lavapipe~~ (dropped 2026-10-10, user: spike stays local-only)
  - files: `.github/workflows/ci.yml`
  - approach:
    - After the existing ctest steps, cache `~/.cache/natron-deps` with `actions/cache` keyed on the Slang version and SHA.
    - Configure and build `tools/gpu-spike` with Ninja and ccache, then run its ctest with `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`.
    - Tests that need the GL extensions skip themselves when the extensions are missing.
    - It runs in the existing job, so there is no second image pull.
  - verify: a CI run on the milestone PR is green with the spike steps visible. Their wall time goes into P6.T3's numbers.
  - size: M

**Dependency map**
- **Start at once:** P1.T1 and P1.T2. They touch separate files and P1.T2 is header-only.
- **After P1.T1, all in parallel:**
  - P1.T3;
  - P3.T1, P3.T2 and P6.T3 (their CPU sides need only the toolchain);
  - P2.T1 (needs P1.T2 too).
- **After P3.T1:** P3.T3 and the CPU-twin bench P6.T2 (P6.T2 needs P3.T2 too).
- **After P2.T1, in parallel:** P2.T2, P2.T3 and P4.T1 (P4.T1 does its own export allocation), plus P6.T5.
- **After P2.T2:** P5.T1.
- **After P2.T2 + P2.T3 + P3.T1 + P3.T2:** P3.T4.
- **P5.T2:** independent of the Vulkan work. It can start whenever an OpenCL runtime exists, after P3.T1 and P3.T2 for the shared fixtures.
- **After P1.T3 + P2.T3 + P3.T4:** P6.T1.
- **Last:** P6.T4, after everything else (P5 tasks may close as dropped).
- No task builds Natron, so the spike never competes for the one-at-a-time Natron build slot. Spike builds take seconds to minutes, so two can run side by side if needed.

**Verification gate:**
- `ctest` in `build/gpu-spike` is green on lavapipe in `natron-dev`, with no DISPLAY for everything except the Qt interop variant, which runs under Xvfb.
- The same suite is green on the RX 7900 XTX through `run-host.sh`.
- The Grade and Blur GPU outputs meet their stated tolerances on both drivers.
- The interop test passes byte-exact on RADV/radeonsi.
- The bench CSV covers UHD through 24k, with overlap demonstrated.
- The GL compute and OpenCL references are either measured or recorded as dropped, with a reason.
- The main Natron build and ctest are unchanged.
- The design note and decision record exist, and the user has signed off on the backend, the kernel language, the CPU-fallback strategy and the interop path.

## Decisions

- 2026-10-10 — **Where the spike lives:** a standalone CMake project in `tools/gpu-spike/`, not an option in the root build.
  - It builds in seconds without touching the Natron build slot or main-build configure time, and runs natively on the host.
  - It carries zero risk to the main build and can be dropped whole.
  - The reusable layer (`src/Gpu*`) is written engine-free (std plus Vulkan plus VMA, no Engine or Python headers) so M84 can move it into `Engine/Gpu/` as it is.
- 2026-10-10 — **How Slang is acquired:** the pinned release tarball (v2026.19, matching the host's `/opt/slang`) is downloaded at configure time with SHA256 verification and cached in `~/.cache/natron-deps`. `-DSLANG_ROOT` overrides it.
  - Vendoring 83 MB / 251 MB is unreasonable, the image has no Slang, and github.com is the only reachable source.
  - Kernels are compiled offline to embedded SPIR-V, so no Slang library ships at runtime. The runtime dependency is the system Vulkan loader only.
- 2026-10-10 — **VMA is vendored** under `libs/VulkanMemoryAllocator/` (single MIT header, pinned tag): it is tiny, works offline, and M84 needs it in the engine anyway.
- 2026-10-10 — **Storage buffers, not images**, for the prototype kernels: they match Natron's interleaved float RAM layout, so there's no format conversion on transfer. Sampled images are deferred to M85's filtered Transform.
- 2026-10-10 — **Interop via an exported buffer imported as a GL PBO**, then a GPU-side `glTexSubImage2D`, rather than importing an image directly: it avoids driver-specific tiling compatibility and maps onto ViewerGL's existing PBO upload. Direct image import is a stretch measurement only.
- 2026-10-10 — **The neighbourhood kernel is a FIR Gaussian.** It is the GPU-natural gather kernel. The gap to Natron's IIR Blur is reported for M85 to decide, not asserted.
- 2026-10-10 — **GL 4.3 compute and OpenCL are timeboxed references** that can be dropped (user direction: build toward Vulkan + Slang). OpenCL runs only if a host runtime is installed, since none exists today.
- 2026-10-10 — **The bench covers up to 24k**, with strip tiling, per the project's tiling target workload.
- 2026-10-10 — Blocker resolved: the user confirmed Vulkan compute + Slang and asked for parallel GPU work, so M83 starts while M82 - Render Cost Profiling finishes its comp profiling (see DECISIONS/2026-10-10-vulkan-slang-backend-and-parallel-gpu-milestones.md). Lane: worktree `build/wt/m83-gpu-backend-spike` off `main`, PR against `main`.
- 2026-10-10 — User answers: **OpenCL reference dropped** (M83.P5.T2) — no runtime on host or image, and Vulkan + Slang is decided. **Spike stays local-only** (M83.P6.T5 dropped); CI coverage arrives when M84 moves the code into `Engine/`. **FIR Gaussian is acceptable for GPU Blur**, and the user wants FIR added to the native CPU Blur as its default filter too — split out as M88 - FIR Gaussian Blur so it ships independently; M83.P3.T2's FIR maths should match what M88 puts in the CPU node.
- 2026-10-10 — The VMA vendor commit bypassed the comment gate: all ten flags were in upstream's unmodified `vk_mem_alloc.h`, which is not ours to rewrite.
- 2026-10-10 — M83.P3.T2: no IIR-vs-FIR difference numbers — linking `BlurKernels` into the standalone spike drags in Engine/Python headers. M88 - FIR Gaussian Blur's tests can report that gap from inside the engine instead.
