# GPU compute backend (M83 - GPU Compute Backend Spike)

Status: proposed, awaiting the user's sign-off (M83.P6.T4). Decision record: `DECISIONS/2026-10-10-gpu-compute-backend-chosen.md`.

Spike code: `tools/gpu-spike/` on `milestone/m83-gpu-backend-spike` (worktree `build/wt/m83-gpu-backend-spike`, head `cb21e6224`). Bench output: `build/gpu-bench/` in that worktree (`summary.txt`, `gpu-bench.csv`, `cpu-baseline.csv`). Per-task evidence is in the `## Decisions` section of `MILESTONES/M83-gpu-compute-backend-spike.md`.

**Hardware, unless a row says otherwise:** GPU = AMD RX 7900 XTX (24 GB) on RADV, Mesa 25.0.7, native on the Debian 12 host. CPU = Ryzen 7 7800X3D, 8 cores / 16 threads, all 16 used. Correctness also runs on lavapipe (llvmpipe, LLVM 21.1.8, Mesa 25.2.7) in `natron-dev:2027-clang21.1`. Images are RGBA float (16 B/px): UHD 3840×2160 = 0.13 GB, 8K 7680×4320 = 0.53 GB, 16k = 16000² = 4.1 GB, 24k = 24000² = 9.2 GB.

## 1. Decision summary

| Question | Decision |
|---|---|
| Backend | **Vulkan 1.3 compute**, headless (no WSI, X, Wayland or EGL). It needs timeline semaphores and synchronization2. Optional extensions are detected: external memory and semaphore fd, external memory host, memory budget, push descriptors. One compute queue plus a transfer-only queue when the device has one. VMA (v3.3.0, vendored) allocates. |
| Kernel language | **Slang, compiled offline to SPIR-V and embedded as `const uint32_t[]`.** Slang does not ship at runtime and `libslang` is never linked. The runtime dependency is the system `libvulkan.so.1`. Slang is pinned to v2026.19, fetched at configure time with its SHA256 checked, and cached in `~/.cache/natron-deps`. |
| Data layout | Storage buffers that hold Natron's interleaved float rows byte for byte. `VkImage` is not used. M85 resamples Transform from storage buffers with the CPU's filter maths instead of sampled images (§10). |
| CPU fallback | **The hand-written CPU kernels stay the primary CPU path.** The Slang C++-target twin is a **test oracle only**: it runs 14–31× slower than native FIR on Blur and fits `PixelKernel::processRow` poorly. |
| Viewer interop | Export a `VkBuffer` (opaque fd), import it in GL as a memory object bound as `GL_PIXEL_UNPACK_BUFFER`, then `glTexSubImage2D` into the viewer texture. The PBO path stays in `ViewerGL`, which only binds a different buffer. Synchronisation uses exported semaphores (`glWaitSemaphoreEXT` / `glSignalSemaphoreEXT`). **Fallbacks:** (1) **hostsync**: the same memory import, ordered by a Vulkan fence and `glFinish` when semaphores are missing; (2) **readback + PBO**: the existing upload path when memory import is missing or the device and driver UUIDs differ. |
| Rejected | GL 4.3 compute performs about the same, but Natron would need a regenerated glad (GL 2.0 compat today, about half a day of work) and 4.3 contexts everywhere. OpenCL has no runtime on the host or in the image, and the user dropped it. The Slang C++ twin as the CPU path is rejected for the reasons in §2.6. |

## 2. Measured numbers

### 2.1 Transfers (RADV, UHD float, 16 frames pipelined, `GpuTransfer_Test`)

| Path | ms/frame wall | Notes |
|---|---|---|
| Staging, 1 copy thread | 35.4 | The host memcpy dominates. |
| Staging, 8 copy threads | 20.1 | The stages sum to 500 ms against 322 ms wall: overlap ×1.55. |
| Host-import (frame already in imported memory) | **9.9** | No memcpy. The device reads and writes the pinned host pages. |
| DMA alone | 4.7–5.6 each way | About 25 GB/s. |
| No-op compute | 0.28 | |

Lavapipe, for correctness only: host-import 470 ms against staging 623 ms over 16 frames.

### 2.2 Break-even: one round trip, upload plus download (bench Copy workload, RADV)

| Size | Staging serial / pipelined ms | Host-import serial / pipelined ms | Round-trip GB/s, staging / import |
|---|---|---|---|
| UHD | 20.0 / 18.6 | 10.8 / 9.5 | 13.2 / 24.5 |
| 8K | 78.2 / 74.3 | 42.8 / 38.4 | 13.6 / 24.8 |
| 16k (tiled) | 603.6 / 571.2 | 329.0 / 292.3 | 13.6 / 24.9 |
| 24k (tiled) | 1337.8 / 1274.9 | 737.4 / 654.5 | 13.8 / 25.0 |

A region pays off when the CPU time it saves exceeds this cost plus its GPU kernel time. The bandwidth is flat across sizes: about 25 GB/s with host-import and 13.6 GB/s with staging. Those two numbers are the cost-model constants for M84.

### 2.3 Kernel time and end to end against the CPU (RADV, ms per image, mean of 2 runs)

"Kernel" is GPU time with the data already resident. "E2E" is upload + kernel + download, pipelined (overlap on). "CPU" is Natron's native node at 16 threads: its marginal cost in a `graph_bench.py` graph. **The CPU Blur here is the IIR Gaussian, because it was measured before M88 - FIR Gaussian Blur merged.** GradeBlur25 on the CPU is Grade + Blur25 added together, not a measured chain; see §7.

| Size | Workload | CPU | GPU kernel | E2E staging | E2E host-import | CPU / E2E import |
|---|---|---|---|---|---|---|
| UHD | Grade | 31.3 | 0.4 | 18.5 | 9.5 | 3.3× |
| UHD | Blur σ3 | 117.4 | 3.3 | 18.9 | 9.5 | 12.4× |
| UHD | Blur σ25 | 127.7 | 21.5 | 35.0 | 25.5 | 5.0× |
| UHD | Blur σ100 | 189.5 | 52.5 | 75.4 | 61.3 | 3.1× |
| UHD | Grade→Blur25 resident | 159.0 | 22.9 | 33.5 | 23.7 | 6.7× |
| 8K | Grade | 123.9 | 2.0 | 74.1 | 38.3 | 3.2× |
| 8K | Blur σ3 | 457.8 | 12.8 | 84.8 | 38.3 | 12.0× |
| 8K | Blur σ25 | 487.9 | 164.2 | 254.2 | 186.3 | 2.6× |
| 8K | Blur σ100 | 574.6 | 286.6 | 395.0 | 342.1 | 1.7× |
| 8K | Grade→Blur25 | 611.8 | 170.0 | 221.7 | 182.3 | 3.4× |
| 16k tiled | Grade | 903.7 | 15.6 | 571.1 | 292.1 | 3.1× |
| 16k tiled | Blur σ3 | 3468.9 | 99.1 | 607.5 | 293.3 | 11.8× |
| 16k tiled | Blur σ25 | 3561.0 | 611.0 | 1130.7 | 669.8 | 5.3× |
| 16k tiled | Blur σ100 | 3834.8 | 2045.9 | 2722.8 | 2146.1 | 1.8× |
| 16k tiled | Grade→Blur25 | 4464.6 | 570.4 | 1123.1 | 649.1 | 6.9× |
| 24k tiled | Grade | 1997.6 | 37.6 | 1273.5 | 653.8 | 3.1× |
| 24k tiled | Blur σ3 | 7892.0 | 220.4 | 1456.8 | 657.2 | 12.0× |
| 24k tiled | Blur σ25 | 7822.3 | 906.7 | 2296.7 | 1048.1 | 7.5× |
| 24k tiled | Blur σ100 | 8247.0 | 3981.8 | 5460.8 | 4130.4 | 2.0× |
| 24k tiled | Grade→Blur25 | 9819.9 | 927.2 | 2238.0 | 1030.7 | **9.5×** |

The 24k frame as a single image cannot run its kernels, because the frame exceeds `maxStorageBufferRange` (§4). Only its staging Copy ran: 1323.7 ms, with 18.7 GB of device-local memory in use. The tiled runs use strips of 2048 rows with blur halos. Tiled output and whole-frame output differ by exactly 0.

Overlap off → on, host-import e2e, in ms: UHD Blur3 13.8 → 9.5; UHD Blur25 34.0 → 25.5; 24k Blur25 1781.6 → 1048.1. Pipelining hides most of the transfer whenever the kernel time is comparable to it.

Reading:
- **Grade, and point ops in general, are transfer-bound.** Kernel time is negligible (0.4 ms at UHD), so isolated they gain only about 3×. They pay off only inside a resident region.
- **A resident chain gains up to 9.5× at 24k.** It costs one boundary round trip, and the extra GPU work is nearly free.
- **Wide blurs are kernel-bound.** At σ100 the GPU gains only 1.7–3.1×, and the vertical pass dominates (§2.4).

Run-to-run: 2 runs; median |diff| 0.6%; 5 of 180 cells outside ±10%, mostly the 2 ms 8K Grade kernel.

### 2.4 Blur pass split (resident, ms per image)

| | σ3 H / V | σ25 H / V | σ100 H / V |
|---|---|---|---|
| UHD | 0.99 / 2.30 | 5.94 / 15.52 | 22.10 / 30.37 |
| 8K | 4.00 / 8.79 | 23.44 / **140.77** | 87.86 / 198.89 |
| 16k | 31.27 / 67.88 | 190.42 / 420.36 | 855.37 / 1190.21 |
| 24k | 68.75 / 151.57 | 428.83 / 477.82 | 1961.41 / 2020.48 |

For r ≤ 32 the vertical pass reads coalesced rows, with one thread per column over a block of 32 columns. That fix took UHD σ3 V from 11.0 ms to 2.3 ms. Wider radii use `groupshared` tiles, and V runs 1.4–2.6× slower than H. **8K σ25 V is 6× H**, probably stride aliasing at a 7680-float row pitch; nobody chased it.

### 2.5 Interop hand-off (UHD float, end to end)

| Path | RADV / radeonsi | lavapipe / llvmpipe |
|---|---|---|
| Zero-copy with semaphores | **0.79 ms**, byte-exact | skipped: lavapipe has no `VK_KHR_external_semaphore_fd` and llvmpipe has no `GL_EXT_semaphore(_fd)` |
| Zero-copy hostsync (fence + `glFinish`) | 0.87 ms, byte-exact | 39 ms, byte-exact (including Qt `QOpenGLWidget` under Xvfb) |
| Readback + PBO | 33.2 ms | 125 ms |

The device and driver UUIDs match on RADV/radeonsi. The test runs on an EGL surfaceless context. The Qt variant (`GlInteropQt_main`, compat-profile `QOpenGLWidget`, xcb/Xvfb) proves the import works on a Qt-provided context. Direct import as a texture (`glTexStorageMem2DEXT`) was not measured.

### 2.6 GL 4.3 compute reference (radeonsi, EGL surfaceless, UHD, median of 21)

| Kernel | GL 4.3 | Vulkan kernel (bench) |
|---|---|---|
| Grade | 0.41–0.55 ms | 0.4 ms |
| Blur σ3 | 12.95 ms (H 1.10 / V 11.86) | 3.3 ms after the coalesced-V fix (V was 11.0 before it) |
| Blur σ25 | 22.47 ms (H 6.47 / V 16.00) | 21.5 ms |

Both ran the same kernel layout at parity. GL was not re-run after the V fix or the switch to push constants. Slang's GLSL output needed two fix-ups: `#version 450` → 430, and `controlBarrier` → `memoryBarrierShared(); barrier();`.

### 2.7 CPU twin against native (P6.T2; 16 threads, UHD/8K, ms/Mpx)

| Kernel | Slang C++ twin | Native |
|---|---|---|
| Blur σ3 / σ25 / σ100 | 20.6 / 158 / 616 | FIR 1.44 / 5.5 / 19.9; IIR ~3.4 and box ~2.8, both flat in σ |
| Grade | 2.9 (gcc); about 1.5× faster than native with clang | 3.4 (computed in double) |

The twin does not vectorise. Each pixel is a scalar call inside an 8×8 group loop, the FP reduction cannot be reordered, and every tap resolves its own bounds. Its entry point has the shape of a dispatch (`k(ComputeVaryingInput*, entryParams, globalParams)`), and its Params layout must be kept in sync by hand. It needs only the prelude headers.

### 2.8 Build and deployment cost (P6.T3, container)

| Item | Cost |
|---|---|
| Slang tarball | 79 MiB, 238 MiB unpacked, downloaded once per cache |
| Configure | 3.3 s cold (with the download), 0.83 s cached |
| Full spike build | 8.9 s at -j4 |
| `slangc` | 0.16–0.21 s per kernel per target; 4.1 s for all 22 invocations |
| Embedded SPIR-V | 67 KiB for 10 kernels |
| Runtime `NEEDED` | `libvulkan.so.1` plus glibc. The GL tools also need libEGL/libOpenGL, and the Qt variant needs Qt6 and GLX. |
| Packaging | `package.sh` needs no change, and `excludelist.txt` already excludes `libvulkan.so.1`. The loader and ICDs come from the user's system. |
| Licences | Slang is Apache-2.0 WITH LLVM-exception and VMA is MIT; both are GPL-compatible. |
| CI | None: the spike is local-only. CI coverage arrives when M84 - GPU Placement And Residency moves the code into `Engine/`. |

## 3. Tolerances per kernel and driver

| Kernel / check | Bound | lavapipe | RADV | NVIDIA |
|---|---|---|---|---|
| `fill` (x + y·w, 1921×1081) | exact | pass | pass | pass |
| Transfer round trip (UHD, staging and host-import) | byte-exact | pass | pass | pass |
| Grade, C++ twin against the double reference | ≤ 4 ULP, or abs ≤ 1e-6 near 0. Plus a per-element conditioned term for reverse grade: 4 + 2·\|ln y\| ULP of the pow term, plus subtraction and division rounding. | CPU only: about 5.5k of 27M values need the conditioned term. That is float against double, not a kernel bug. | — | — |
| Grade, GPU against the double reference | The same bounds, plus a GPU-only slack from the SPIR-V precision table (pow = exp2·log2, unfused fma). Subnormal inputs may return either the value or 0. | worst use of the conditioned bound 0.487. pow/log2 on subnormals are garbage, hence the value-or-0 rule. | worst use of the conditioned bound 0.244; gamma curves ≤ 31 ULP; all 6033 subnormals match | pass at the RADV bounds |
| Blur, C++ twin against the double FIR reference | abs ≤ 2e-6 on [0, 1], σ ∈ {0.5, 3, 25, 100}, odd and tiny sizes | pass | — | — |
| Blur, GPU against the double FIR reference | abs ≤ 2e-6 | same values as RADV | max abs 1.4e-7 / 2.2e-7 / 5.8e-7 / 1.21e-6 at σ 0.5 / 3 / 25 / 100 | pass at the RADV bounds |
| `Chain<Grade, Invert>` against two passes | bit-exact | pass (C++ twin) | — | — |
| GL 4.3 compute (Grade, Blur) | the P3 bounds above | — | pass on radeonsi | pass |
| GL interop | byte-exact | hostsync pass | zero-copy and hostsync pass | zero-copy and hostsync pass (§11) |
| Tiled strips against the whole frame (bench) | — | — | max abs diff 0 | — |
| Linear-filter LUT in runtime GLSL (`Lut3DLinear`, `Lut2DLinear`) | the exact-weight 1e-5 where the device allows; otherwise `kTolerance` plus a per-element bound of 2^-`subTexelPrecisionBits` times the texel differences across the cell (`subTexelPrecisionBits` is 8 on NVIDIA) | pass | pass | worst 1.55e-4 (3D) and 1.26e-5 (2D); legal under the Vulkan filtering precision rules |

On NVIDIA every other suite passed at the RADV tolerances (§11). Kernel fix found along the way: SPIR-V `Pow` gives NaN for inf^0, so reverse grade now returns 1 explicitly for pow(v, 0).

## 4. Hard limits and gotchas M84 must design around

| # | Limit | Consequence for M84 |
|---|---|---|
| 1 | `maxStorageBufferRange` is **4.29 GB** on RADV. | A frame bigger than about 4 GB cannot be bound whole: 24k (9.2 GB) failed as a single frame, while 16k (4.1 GB) just fits. **The GPU must tile oversized frames itself**, as strips with halos (2048 rows in the bench). This overrides the M84 stub's "fine tiling stays a CPU concern" for anything over the limit. Read the limit from the device; never assume it. |
| 2 | A single `vkCmdCopyBuffer` over about **2.5 GiB corrupts data** on RADV. | Every copy is chunked; the bench uses 512 MB. The chunking lives in `GpuBench_main.cpp` today and must move into the promoted `GpuTransfer`. |
| 3 | A single **host-import copy over 4 GiB is wrong**. | The same chunking applies to imported buffers. That is also why the 24k single-frame host-import cells are empty. |
| 4 | **Host-import alignment:** pointer and size must be multiples of `minImportedHostPointerAlignment`. | The RAM frame allocator must hand out page-aligned, padded buffers (§5.5). Strip interiors are kept to multiples of 4 rows so strip pointers stay 4096-aligned. When alignment fails, transfers fall back to staging. |
| 5 | **The vertical blur pass is stride-bound.** | Naive 1D column loads made V 10× slower than H. Coalesced loads fixed r ≤ 32, and wider radii use shared tiles (V 1.4–2.6× H). 8K σ25 still shows V at 6× H (aliasing). M85 - Native GPU Kernels needs a 2D-tile or transpose pass for large radii. |
| 6 | **lavapipe has no external semaphores**, and llvmpipe has no `GL_EXT_semaphore`. | Software and CI runs exercise only the hostsync interop path. The semaphore path is tested only on real hardware. |
| 7 | After a host reboot, `/dev/dri/*` appeared as **65534 inside this host's user namespace**, so RADV could not open the device. | The user made the render nodes world-rw. That probably does not survive a reboot (udev), so re-check after every reboot. `run-host.sh` now exits 1 when no render node opens, so runs no longer fall back silently to llvmpipe (`GPU_SPIKE_ALLOW_SOFTWARE=1` overrides it). radeonsi also prints a harmless `os_same_file_description couldn't determine…` warning under this namespace. |
| 8 | **The container does not see RADV** even with `--device=/dev/dri`. | GPU runs build in the container and run natively on the host, so binaries may depend only on `libvulkan.so.1`, glibc and el9 base libstdc++. Engine ctest on a real GPU needs the same arrangement, or a fix to the image. |
| 9 | **Push constants are capped at 128 B, and uniform buffers are unsupported.** | Kernel parameters must fit 128 B. Bigger tables, such as Blur weights, go in a storage buffer. |
| 10 | Validation layers are on neither the image nor the host. | Without them, misuse such as the copy corruption shows up only as wrong output. Install the layers for M84 development. |
| 11 | NVIDIA: `maxComputeWorkGroupCount` is {2³¹−1, **65535**, **65535**} (RADV: 2³²−1 in every dimension). | `GpuKernel::record` fails rather than splitting a dispatch that exceeds it. Kernels must keep Y and Z within 65535 groups. |
| 12 | NVIDIA: `maxComputeSharedMemorySize` is **48 KiB** (RADV 64 KiB). | The current kernels' groupshared use fits. |
| 13 | NVIDIA: `shaderDenormPreserveFloat32` is **false**. | Float compares on denormals are not reliable, which is why §10 keys compares by integer. |
| 14 | NVIDIA without ReBAR: the device-local, host-visible heap (BAR) is **246 MiB**. | Do not assume the VRAM is host-visible. |
| 15 | NVIDIA: subgroup size 32. Queue families: compute-only ×8 (family 2), transfer-only ×2 (family 1), plus the graphics family. | No kernel relies on subgroups. The code picks compute family 2 and transfer family 1, with ownership transfers between them. |

## 5. API sketch for M84 - GPU Placement And Residency

The spike API exists as described below unless the section says **(new)**. Namespace `gpu`. No exceptions cross the API: everything returns `GpuStatus {VkResult, message}`. No Engine or Python headers are included.

### 5.1 `GpuDevice` (exists)
- `create(GpuDeviceOptions, unique_ptr&)` and `listDevices(vector<GpuPhysicalDeviceDesc>&)`. Selection order: the `NATRON_GPU_DEVICE` override (an index or a name substring), then discrete, integrated and CPU devices.
- `info()` reports: the queue families, `transferUsesComputeQueue`, and the `externalMemoryFd`, `externalSemaphoreFd`, `externalMemoryHost`, `memoryBudget`, `pushDescriptor` and `shaderFloat16` flags.
- `commandPool(QueueKind)` gives one pool per (thread, family), created lazily.
- `submit(QueueKind, span<VkSubmitInfo2>, fence)` locks a mutex per queue.
- `createTimelineSemaphore` and `waitSemaphore` are the timeline primitives.
- `queryBudget(vector<VmaBudget>&)` returns budget and usage per heap through `VK_EXT_memory_budget`. M84's VRAM budget and admission read it.
- `check(VkResult, what)` latches device loss; `isLost()` reads the latch.

### 5.2 Buffers (new: a thin RAII layer over VMA)
- The spike hands out the raw `VmaAllocator`. M84 adds a `GpuBuffer {VkBuffer, VmaAllocation, size, usage}` with these kinds:
  - device-local;
  - host-imported (wrapping a caller's pointer, cached by (pointer, size));
  - exportable (dedicated, opaque fd, for interop);
  - staging.
- Allocations go to a ref-counted pool and are counted against `queryBudget`. Under pressure, LRU eviction moves them to RAM.

### 5.3 `GpuTransfer` streams (exists as a blocking batch; M84 makes it asynchronous)
- Today, `process(span<TransferFrame>, ComputeRecordFn, TransferTimeline*)` pipelines N frames through an N-slot ring and blocks until done. Its behaviour:
  - one timeline semaphore per queue orders upload k → compute k → download k;
  - queue-family ownership transfers between the transfer and compute families;
  - partial downloads (`dstOffset` / `dstBytes`) for strip interiors;
  - a host-import path with staging fallback;
  - `cacheHostImports`.
- **(new)** Split it into `upload(src, GpuBuffer&) → TimelineValue`, `download(GpuBuffer&, dst) → TimelineValue` and `wait(TimelineValue)`. The scheduler can then chain region boundaries without blocking, and the compute stream waits on the transfer timeline value.
- **(new)** Move the 512 MB copy chunking (§4 #2, #3) inside the class.

### 5.4 Kernels and the registry
- **Exists:** `GpuKernel::create(device, GpuKernelDesc{spirv, words, entry, storageBufferCount, pushConstantBytes, groupSize})`, then `record(cmd, buffers, push, threads)`.
  - Bindings come through push descriptors, falling back to a descriptor pool.
  - Group counts come from the `numthreads` that `slangc -reflection-json` reports.
  - Each kernel has a pipeline cache.
  - `GpuTimer` provides timestamp pairs.
- **Kernel convention:** storage buffers in set 0, bindings 0..N-1 in declaration order, and one `[[vk::push_constant]]` block of at most 128 B.
- **(new) `GpuKernelRegistry`:** a static table from kernel ID to the generated `GpuKernelDesc`, built from the `slang_add_kernel` outputs. Pipelines are created lazily for each device and are invalidated on device loss. Native nodes advertise GPU support by owning a registry ID (`EffectInstance::supportsOpenGLRender` is separate and stays for the OFX GL suite).

### 5.5 Residency from day one (user decision `2026-10-10-gpu-residency-from-day-one`)
- **A resident region runs as one super-task.** Its inputs are uploaded once and its intermediates never leave the GPU; one download happens per region output, or per output that has more than one consumer. A super-task records all its kernels into command buffers on the calling thread and submits through `GpuDevice::submit`.
- **Frames live in importable, page-aligned host memory.** The RAM image allocator behind `FrameStore` and the image buffers hands out page-aligned buffers whose size is padded to the import alignment. Host-import then replaces the staging memcpy: 9.9 ms against 20.1–35.4 ms per UHD frame. Import registrations are cached for each pooled buffer, so pinning is paid once per buffer, not once per frame. Staging remains the fallback for unaligned or foreign memory.
- Oversized frames and regions are tiled on the GPU in strips with halos, using the same strip machinery the bench already verifies (§4 #1).

### 5.6 Threading model
- GPU tasks are **single-threaded super-tasks with a thread budget of 1** in admission. They record on their own thread, using that thread's command pool, and submit behind the per-queue mutex.
- There is no shared command buffer and no per-queue thread affinity.
- Host memcpy (staging path only) uses the transfer object's own copy-thread pool. With host-import that pool is idle, and the budget stays at 1.

### 5.7 Device-lost and out-of-memory state machine

| State | Condition | Behaviour |
|---|---|---|
| `Ready` | normal | Placement may assign GPU. |
| `Degraded` | `VK_ERROR_OUT_OF_DEVICE_MEMORY` or budget exhausted | Evict LRU to RAM and retry once. If that fails, run the region on the CPU. Placement shrinks its VRAM budget. |
| `Lost` (sticky) | any `VK_ERROR_DEVICE_LOST` passed through `check()` | Every call returns the lost status. In-flight regions re-run on the CPU from their CPU-resident inputs, so a region's inputs must stay in RAM until the region completes. Placement assigns only CPU. |
| `Recreate` **(new, untried)** | user action or the next render | Tear down and call `GpuDevice::create` again. Registry pipelines and imports are rebuilt lazily. |

Silent CPU fallback also covers the case of no device at all: `create` fails, and placement never assigns GPU.

## 6. Recommendations for downstream milestones

### M68 - Headless GL EGL
- Native GPU work does not need it. Vulkan compute is headless with no EGL or WSI, and the viewer hand-off uses the GL context Qt already creates.
- **Re-scope M68 to one job:** an EGL surfaceless/device backend for `OSGLContext`, so that **OFX plugins using the OpenGL render suite** (Shadertoy, the OCIO GL plugins) and the GL ctest cases get real GL in `NatronRenderer` and ctest with no display.
- That role still holds, because M86 - OFX 1.5 GPU Suites keeps the OpenGL suite working through GL/Vulkan interop.
- The spike de-risks it: EGL surfaceless (`EGL_MESA_platform_surfaceless`) worked on radeonsi and llvmpipe for `GlInterop_Test` and the GL 4.3 reference.
- M68 stays deferred until the user gives the go-ahead.

### M81 - Raw GPU Kernels (OpenCL C → Slang)
The M80.P6.T1 raw-kernel requirements note does not exist yet (that task is still open), and the OpenCL reference (M83.P5.T2) was dropped. So no OpenCL porting time was measured. Mapping notes:

| darktable OpenCL | Slang on this backend |
|---|---|
| `__kernel void f(...)`, `get_global_id(0/1)` | `[shader("compute")] [numthreads(X,Y,1)]` with `SV_DispatchThreadID`. The host takes group counts from the reflected `numthreads`. |
| `__local` buffers, sized through `dt_opencl_local_buffer_t` | `groupshared` arrays with a fixed tile size; the work-group size cannot be tuned at runtime. |
| `barrier(CLK_LOCAL_MEM_FENCE)` | `GroupMemoryBarrierWithGroupSync()`. Slang's GLSL output needs the `barrier()` fix-up (§2.6), but SPIR-V does not. |
| scalar kernel arguments | one push-constant struct of at most 128 B. Larger tables (lens-correction coefficients, denoise LUTs) go in a storage buffer. |
| `image2d_t` with `read_imagef` and a clamp sampler | **Not in the spike.** Either rewrite as storage buffers with explicit index clamping, as `blur.slang` does, or wait for M85's sampled-image path. The mosaic is single-channel, so a buffer is natural for demosaic. |
| `native_*` and `half_*` maths | Expect the pow = exp2·log2 precision slack and garbage results on subnormals, as with Grade (§3). Budget ULP-conditioned tolerances, not flat ones, and flush or zero-check subnormal sensor values. |

- Demosaic → white balance → highlights → denoise should run as one resident region (§5.5).
- Wide neighbourhoods (denoise, AMaZE) will meet the vertical-stride issue (§4 #5).

### M85 - Native GPU Kernels
- **FIR against IIR:** since M88 - FIR Gaussian Blur, the CPU Blur defaults to the FIR Gaussian for every Blur under the hard cut. M88 used the same maths: σ = size/2.4, radius ceil(3σ), host-normalised weights, Neumann clamp or zero edges.
  - The GPU FIR is within 1.21e-6 of the double reference, and M88's CPU FIR within 1e-6, so GPU and CPU Blur should agree within the 2e-6 FIR bound.
  - **That has not been measured directly**: M85's first Blur test should compare the native node and the GPU kernel on the same fixture.
  - The IIR Gaussian and box filters stay selectable on the CPU and have no GPU kernel. Placement treats those settings as CPU-only, unless M85 ports them.
- **Per-pixel wrapping (P3.T3, `kernels/pointops.slang`):**
  - Ops implement `interface IPixelOp { float4 apply(float4 v, uint2 xy); }`.
  - `Masked<Op>` wraps one: `lerp(v, op(v), mask·mix)`, with unpremultiply before and premultiply after, and the process-channels mask as a per-channel select.
  - `Chain<A, B>` composes two ops, and `PointKernel<Op>` is the entry point.
  - Interface generics inline fully: a single `OpFunction` with no calls or dynamic dispatch.
  - This is the backend-neutral description to place next to `makeKernel()`.
- **Blur performance:** wide radii are kernel-bound (σ100 UHD 52.5 ms, V > H). A 2D-tile or transpose vertical pass, or a different large-σ strategy, is the first optimisation. See also the CPU comparison in §7.
- Filtered Transform needs `VkImage` and sampled images, which the spike did not touch.

### M87 - Point-Op Kernel Fusion
- Slang generics fuse well: `Chain<Grade, Invert>` is bit-exact against two passes, with 12% fewer SPIR-V instructions, one dispatch and no intermediate buffer.
- **But every op combination needs its own compiled entry point.** With Slang offline-only (no runtime Slang), fusing an arbitrary runtime graph needs one of three approaches, all untried:
  - (a) Slang **link-time specialisation** of precompiled modules (untried, and it may still need `libslang` at runtime);
  - (b) an ahead-of-time set of common fused chains;
  - (c) an "uber-kernel" that interprets an op list from push constants or a storage buffer.
- Measure (c) and (a) first. If only runtime Slang works, that reopens the no-runtime-Slang decision.

### M86 - OFX 1.5 GPU Suites
- CUDA interop goes through the same exportable buffers (`VK_KHR_external_memory_fd`) the GL hand-off uses. Exportable allocations need their own VMA pool, as `GlInterop` already does.
- The spike made no OpenCL or CUDA measurements.

## 7. Promotion plan

| From `tools/gpu-spike/` | To | Notes |
|---|---|---|
| `src/GpuDevice.{h,cpp}`, `src/GpuKernel.{h,cpp}` (with `GpuTimer`), `src/GpuTransfer.{h,cpp}`, `src/Vma.cpp` | `Engine/Gpu/` | Engine-free already. Add the §5 **(new)** items: `GpuBuffer`, asynchronous transfers, chunking inside the class, `GpuKernelRegistry`, the state machine. |
| `src/GlInterop.{h,cpp}` | `Engine/Gpu/` | It has no Qt dependency: it takes a `GlProcLoader`. `Gui/ViewerGL` passes `QOpenGLContext::getProcAddress` and binds `glBuffer()` in `transferBufferFromRAMtoGPU`. |
| `cmake/FetchSlang.cmake`, `cmake/SlangKernels.cmake` | root `cmake/` | Keep `-target cpp` as an option, for oracle tests. `slang_add_glsl` stays in tools. |
| `kernels/grade.slang`, `kernels/blur.slang`, `kernels/pointops.slang`, `kernels/gradeops.slang` | `Engine/Gpu/Kernels/` (or beside each node in M85) | `fill.slang` and `smoke.slang` go to the tests. |
| `tests/GpuDevice_Test`, `GpuKernel_Test`, `GpuTransfer_Test`, `GlInterop_Test`, `GradeGpu_Test`, `BlurGpu_Test`, `Tolerance.h`, `GpuHarness.h` | `Tests/Gpu/` | Each test skips with a reason when no device is present. CI runs them on lavapipe; semaphore interop skips there. GPU kernels are compared against the **native CPU kernels**, replacing `ref/GradeRef.h` and `ref/BlurRef.h`. Keep the double references for ULP diagnostics. |
| `bench/`, `ref-gl/`, `run-host.sh`, `run-bench.sh`, `tests/DeviceList_main.cpp`, `tests/GlInteropQt_main.cpp`, `bench/CpuTwinBench_main.cpp`, `bench/build-cost.sh` | stay in `tools/gpu-spike/` | measurement tools only |
| `libs/VulkanMemoryAllocator/` (v3.3.0, SHA256 recorded in `VERSION`) | already in place | Include it from `Engine/Gpu/`. Exactly one TU defines `VMA_IMPLEMENTATION` (`Vma.cpp`). |

The main build gains `find_package(Vulkan)` behind an option that defaults to on, and builds without the GPU when Vulkan is absent.

## 8. Open risks

- **The CPU baseline for Blur is in doubt.** The bench's CPU Blur column (marginal cost in a Natron graph, IIR) is 117–190 ms at UHD, about 14–23 ms/Mpx. The P6.T2 harness measured native IIR at ~3.4 ms/Mpx and native FIR at 1.44 / 5.5 / 19.9 ms/Mpx at σ 3 / 25 / 100, on the same 16 threads. For Grade the two harnesses agree: 31.3 ms against 3.4 ms/Mpx × 8.3 Mpx ≈ 28 ms. Against the native FIR kernel, UHD host-import e2e Blur gains only **~1.3× / 1.8× / 2.7×** (σ 3 / 25 / 100), not the 12× / 5× / 3× in §2.3. M84's cost model needs per-node CPU costs measured in situ. Separately, the Blur node's in-graph overhead may itself be a CPU-side bug worth chasing.
- **The Blur GPU kernel is far from roofline:** 21.5 ms for σ25 at UHD on a 61 TFLOP card, and the 8K σ25 vertical-pass anomaly. GPU gains for heavy spatial ops depend on M85 fixing this.
- **The cost of registering a host-import is unmeasured.** The 9.9 ms figure assumes the frame is already in imported memory. The per-buffer pin and unpin cost, and how it behaves with a churning `FrameStore`, decide whether residency-from-day-one gets its transfer number.
- **Device-lost recovery is untested.** The latch exists, but nothing has exercised re-running on the CPU or re-creating the device.
- **Vendors and drivers.** NVIDIA proprietary is now validated on Ampere (RTX 3070 Ti, driver 580.178.04) as well as RADV and lavapipe (§11). Intel, Pascal (the Quadro P400 was not run, by user decision) and macOS (MoltenVK, where GL interop is not an option) are untested. The limits in §4 differ per driver, so read them from the device.
- **Runtime fusion against offline-only Slang** (M87, §6) may force a runtime-compiler decision.
- **Slang release churn:** v2026.19 is pinned. GLSL output already needed fix-ups, and a compiler bump must re-run the tolerance suite.
- **Host access to `/dev/dri`** after reboots (§4 #7), and no RADV inside the container (§4 #8), make real-GPU CI hard. CI will be lavapipe-only, so the semaphore interop path is never run in CI.

## 9. Outcomes from M84 - GPU Placement And Residency (2026-10-11)

- **Host-import registration on RADV:**
  - pinning touched memory: UHD 2.4–3.6 ms, 8K 12–29 ms;
  - pinning untouched memory: ~30 / ~135 ms;
  - an import of an exact multiple of 4 GiB fails (a 32-bit truncation). The engine never submits one and treats −13 as "use staging".
  - **Recycling is what makes host-import pay:** the `HostImageMemory` pool (default cap max(512 MB, RAM/4); 2 MB-aligned `MADV_HUGEPAGE` buffers ≥ 32 MB).
- **Blur baseline, in-graph CPU** (UHD float, 16 threads): FIR 13.2 / 21.0 / 43.4 ms/Mpx at σ3/25/100, after leaving its buffer uninitialised. About 100 ms per UHD task of serial overhead went to M90 - Render Path Serial Overhead.
- **Limits that changed from §4/§5:**
  - admission is **reactive**: there is no device-byte admission; the pool enforces the budget and out-of-memory walks evict → serial strips → halve strips → CPU;
  - strips are planned from I/O sizes against the pool budget, capped at device memory − 1 GiB;
  - device loss re-runs the whole frame on the CPU;
  - stripped regions download every output, so nothing resident crosses a stripped boundary.
- **Strip pipeline:** 3 strips in flight on the dedicated transfer family, with `ComputeHandoff::Record`. It needs push descriptors.
- **Bench results** (release, RADV RX 7900 XTX, Grade → Blur σ25 → Grade, Auto vs Off):

  | Size | Off | Auto | Speedup |
  |---|---|---|---|
  | UHD | 0.279 s | 0.143 s | 1.95× |
  | 8192² | 2.70 s | 1.49 s | 1.82× |
  | 16384² | 8.2 s | 3.0 s | ~2.7× |
  | 24000² | 17.8 s | 6.5 s | ~2.7× |

  - At 16k and 24k the region is compute-bound: 276 of 314 ms of transfer is hidden at 16k, and 627 of 705 ms at 24k.
  - keying_grade is 1.12× and cg_multipass 1.26× at UHD. No comp was slower under Auto.
  - CPU-only renders are 10–43% faster than `main`.

## 10. Outcomes from M85 - Native GPU Kernels (2026-10-15)

- **Ported:** Grade, Blur (FIR, quasi-Gaussian, Gaussian IIR, box, triangle and quadratic, order 0), Invert, Clamp, Add/Multiply/Gamma, Saturation, ColorCorrect, ColorLookup, Constant/Solid, CheckerBoard, Keyer, ChromaKeyer, Erode/Dilate and Transform/TransformMasked, motion and directional blur included. `NativeGpuCapability_Test`'s allowlist is exactly this set.
- **Wrap layout (`Kernels/pixelwrap.slang`):**
  - Mask, mix, process channels and "(Un)premult by" reproduce `NativeImageEffect::render()` in its order: divide where the divisor is usable, op, multiply back by `(0 < d) ? d : 0`, blend by `mix · mask`, pass unprocessed channels through from the undivided source. The blend uses `precise`.
  - `WrapParams` (116 B) carries the wrap geometry in push constants. Op parameters go in a storage buffer, read with `wrapParamFloat`/`wrapParamFloat4`, because 128 B can't hold both.
  - Flags include `applyMask` and Blur's alpha threshold, which `wrap_finish` applies.
  - Spatial nodes run divide → filter → finish passes from the same module.
  - `slang_add_kernel` has no include path, so the shared ops (`GradeOp`, `Chain`) live inside `pixelwrap.slang`.
  - "(Un)premult by" runs on the GPU only when the divisor is in the task's plane. Masks may come from any layer.
- **Tolerances:** every ported node's bound is in `Engine/Nodes/README.md` "GPU parity". Notable:
  - compares are integer-keyed (Clamp, Erode/Dilate), because devices flush denormals in float compares;
  - unpremultiplied cases hold 2e-6 absolute plus 1e-6 relative;
  - ColorLookup's weighted and luminance modes hold 5e-4 and 3e-5 relative;
  - unpremultiplied ChromaKeyer holds 1e-3 relative;
  - motion blur holds `iir()`, with up to one pixel in 250 allowed `motionBlurMaxError()`;
  - double-float accumulators use Veltkamp/Dekker splitting, because Vulkan doesn't guarantee a single-rounding `fma`.
- **Resampling from storage buffers, not `VkImage` (supersedes §1's data-layout note and §6's "Filtered Transform needs `VkImage`"):** hardware filtering has coarse fixed-point weights and only bilinear, so it can't meet the 1e-5 resampling bound or Natron's ten filters. Sample positions travel as two floats.
- **Blur vertical pass:** a single-dispatch shared tile (float4 per pixel, column-order groups) beat transpose → H → transpose at every size. V/H is now 1.01–1.23× at σ25/σ100 (was 3–9×). The narrow σ3 path (r ≤ 32) is still V ≈ 10× H.
- **Erode/Dilate wide pass:** van Herk/Gil-Werman above half-width 16, register-blocked at 16 outputs per thread with adjacent columns per group. A one-load-per-row-pitch column walk aliased onto one memory channel at power-of-two widths (7.6 → 0.5 ms/Mpx).
- **Bench (release, RADV 7900 XTX, taskgraph, Auto vs Off; median of per-round ratios):**

  | | UHD | 8K | 16k | 24k |
  |---|---|---|---|---|
  | keying_grade | 2.77× | 2.90× | – | – |
  | cg_multipass | 1.99× | 2.09× | – | – |
  | defocus_retime | 2.94× | 2.88× | – | – |
  | Transform, alone / between Grades | 1.36× / 1.72× | 1.45× / 1.84× | 1.65× / 2.30× | 1.79× / 2.50× |
  | Transform motion blur | 8.5× / 8.7× | 12× / 12× | 20× / 20× | 21× / 21× |
  | Blur FIR σ25 | 2.18× / 2.64× | 2.56× / 3.07× | 3.11× / 3.73× | 3.22× / 3.48× |
  | Erode 40 | 2.52× / 2.91× | 2.93× / 3.48× | 3.63× / 4.20× | 2.99× / 3.59× |
  | ChromaKeyer | 1.93× / 2.29× | 2.05× / 2.54× | 2.46× / 3.18× | 2.18× / 2.81× |

  No Auto case is slower than Off in any round. CPU-only renders are within ±2% of `main`. The full tables are in M85's decisions.
- **What limits it now:**
  - unstripped regions expose their transfer (~0.5 ms/Mpx, ~2 ms/Mpx for Transform), which is 59–84% of region time in the comps;
  - stripped regions hide it;
  - at 16k and 24k the uncompressed EXR Write is 60–90% of an Auto frame.
- **The bench host is transient:** its hardware can change across a reboot. Compare only Auto/Off pairs measured on one boot.

## 11. Outcomes from M91 - NVIDIA GPU Validation (2026-10-24)

NVIDIA proprietary driver 580.178.04 (Vulkan 1.4.312) on an RTX 3070 Ti works for the offline kernels, the runtime shaders, host-import and the Vulkan→GL hand-off. Two tests were vendor-specific and were fixed in the tests, not the engine.

**Host differences (read every number below with these)**

| | RADV reference host | M91 host (bosley-faux) |
|---|---|---|
| GPU | RX 7900 XTX, 24 GiB VRAM, ReBAR | RTX 3070 Ti, 8 GiB VRAM (about 4 GiB held by desktop apps, 3.86 GiB headroom), no ReBAR (246 MiB BAR heap) |
| PCIe link | not recorded (about 25 GB/s DMA, i.e. gen4 x16) | **gen 2 x16**, the platform's maximum; about 6 GB/s per direction |
| CPU | 7800X3D, 16 threads | AMD FX-8350, 8 threads, 4.0 GHz max |
| Host RAM | 24 GiB-class | 23 GiB, about 6 GB free, swap in use |
| Driver | RADV | NVIDIA 580.178.04 |

Transfer rows are dominated by the gen 2 link, so they are not comparable as GPU speed. Kernel rows are comparable as GPU compute. NVIDIA figures are the mean of 2 runs (median run-to-run difference 0.2%). Only UHD and 8K were run. The CPU columns of §2.3 were not re-measured (they need `graph_bench`, and the CPU is the busy FX-8350).

### 11.1 Transfers (cf. §2.1; UHD float, `GpuTransfer_Test`, ms/frame wall)

NVIDIA ran 6 frames at 8 strips per frame (RADV 16 frames), on a possibly loaded host, so treat it as indicative.

| Path | RADV | NVIDIA | Notes |
|---|---|---|---|
| Staging, 1 copy thread | 35.4 | 60.5 | overlap ×2.57 (RADV ×1.55 with 8 threads) |
| Staging, 8 copy threads | 20.1 | 60.8 | overlap ×2.41; link-bound, extra copy threads do not help |
| Host-import (pin per frame) | 9.9 | 52.0 | |
| Host-import, cached registration | n/a | 37.2 | pin cost removed |
| DMA alone, each way | 4.7–5.6 | 20.3–22.8 | about 5.7 GB/s (RADV about 25 GB/s) |
| No-op compute | 0.28 | 0.51–0.57 | |

### 11.2 Break-even (cf. §2.2; one round trip, serial / pipelined ms)

| Size | Staging RADV | Staging NVIDIA | Host-import RADV | Host-import NVIDIA | GB/s staging / import, RADV | GB/s staging / import, NVIDIA |
|---|---|---|---|---|---|---|
| UHD | 20.0 / 18.6 | 74.3 / 72.7 | 10.8 / 9.5 | 43.9 / 36.7 | 13.2 / 24.5 | 3.6 / 6.0 |
| 8K | 78.2 / 74.3 | 281.7 / 275.4 | 42.8 / 38.4 | 174.6 / 150.5 | 13.6 / 24.8 | 3.8 / 6.1 |
| 16k (tiled) | 603.6 / 571.2 | skipped | 329.0 / 292.3 | skipped | 13.6 / 24.9 | skipped |
| 24k (tiled) | 1337.8 / 1274.9 | skipped | 737.4 / 654.5 | skipped | 13.8 / 25.0 | skipped |

Bandwidth is flat across sizes again: about 6 GB/s with host-import and 3.7 GB/s with staging on the gen 2 link (RADV: 25 and 13.6).

### 11.3 Kernel time and end to end (cf. §2.3; ms per image, pipelined)

| Size | Workload | RADV kernel | NVIDIA kernel | RADV E2E staging | NVIDIA E2E staging | RADV E2E import | NVIDIA E2E import |
|---|---|---|---|---|---|---|---|
| UHD | Grade | 0.4 | 0.5 | 18.5 | 68.1 | 9.5 | 36.7 |
| UHD | Blur s3 | 3.3 | 1.2 | 18.9 | 68.2 | 9.5 | 36.7 |
| UHD | Blur s25 | 21.5 | 5.9 | 35.0 | 68.7 | 25.5 | 37.3 |
| UHD | Blur s100 | 52.5 | 19.3 | 75.4 | 68.1 | 61.3 | 39.3 |
| UHD | Grade→Blur25 | 22.9 | 6.5 | 33.5 | 68.1 | 23.7 | 37.5 |
| 8K | Grade | 2.0 | 2.1 | 74.1 | 280.3 | 38.3 | 150.3 |
| 8K | Blur s3 | 12.8 | 4.8 | 84.8 | 272.5 | 38.3 | 151.4 |
| 8K | Blur s25 | 164.2 | 23.8 | 254.2 | 274.7 | 186.3 | 156.3 |
| 8K | Blur s100 | 286.6 | 78.1 | 395.0 | 280.2 | 342.1 | 170.5 |
| 8K | Grade→Blur25 | 170.0 | 25.9 | 221.7 | 270.6 | 182.3 | 156.3 |

Up to Blur s25 every NVIDIA cell is pure transfer time (E2E equals Copy). Blur kernels are 2.5–7× faster than on RADV. Overlap off, UHD (staging / import): Copy 74.3 / 43.9, Blur25 77.0 / 49.4, Blur100 89.7 / 62.6; 8K: Copy 281.7 / 174.6, Blur25 299.4 / 195.6, Blur100 352.6 / 249.9.

Blur pass split (cf. §2.4; resident, ms per image, H / V):

| | s3 RADV | s3 NVIDIA | s25 RADV | s25 NVIDIA | s100 RADV | s100 NVIDIA |
|---|---|---|---|---|---|---|
| UHD | 0.99 / 2.30 | 0.58 / 0.63 | 5.94 / 15.52 | 2.19 / 3.72 | 22.10 / 30.37 | 8.38 / 10.87 |
| 8K | 4.00 / 8.79 | 2.34 / 2.49 | 23.44 / 140.77 | 8.81 / 14.96 | 87.86 / 198.89 | 34.11 / 44.02 |

NVIDIA V/H is 1.07–1.70 everywhere. The RADV 8K s25 outlier (V = 6× H) does not appear (1.7×).

### 11.4 Interop hand-off (cf. §2.5; UHD float, end to end, median of 10)

| Path | RADV / radeonsi | NVIDIA / NVIDIA GL |
|---|---|---|
| Zero-copy with semaphores | 0.79 ms | 1.24 ms (produce 0.81, hand-off 0.64) |
| Zero-copy hostsync (fence + `glFinish`) | 0.87 ms | 1.43 ms |
| Readback + PBO | 33.2 ms | 67.12 ms (gen 2 link) |

All three paths are byte-exact on NVIDIA, with device and driver UUIDs matching.

### 11.5 ImportCost (NVIDIA only; no RADV figures)

Alignment 4096 B. Median ms of 7.

| Buffer | alloc+touch | pin (touched) | unpin | pin (cold) |
|---|---|---|---|---|
| 1 MB | 0.081 | 0.159 | 0.312 | 0.147 |
| UHD 126.6 MB | 79.8 | 11.5 | 5.8 | 66.4 |
| 8K 506 MB | 323.5 | 49.0 | 27.5 | 270.0 |
| 4 GB | `VK_ERROR_OUT_OF_DEVICE_MEMORY` in `vkAllocateMemory` | | | |

Churn, 32 UHD round trips, median ms/frame: fresh alloc+touch+import+transfer+free 235.7; recycled with cached import 43.7; recycled staging 90.7. Fresh allocation costs 192 ms/frame (439% of the cached transfer), which confirms the cached-registration requirement (§5.5).

### 11.6 End-to-end release render (3 comps, frames 1–6, taskgraph, release AppImage, per-host memory cap)

Median of 3 rounds, whole-process wall in seconds, auto against `NATRON_GPU=off`. All 21 renders exited 0, with no OOM kills.

| Comp | Auto | Off | Speed-up | GPU-placed task records | Region records | Fallbacks | Transfers |
|---|---|---|---|---|---|---|---|
| keying_grade | 7.3 | 15.1 | 2.07× | 78 of 144 | 36 | none | all host-import |
| cg_multipass | 8.7 | 15.0 | 1.72× | 60 of 186 (75 in r3) | 18 (33 in r3) | none | all host-import |
| defocus_retime | 23.9 | 63.9 | 2.67× | 96 of 228 | 54 | none | all host-import |

Auto is not slower than off on any comp. There were no host-import failures. The run selected the RTX 3070 Ti over the Quadro P400 and llvmpipe, ready about 330–360 ms after start. Exposed transfer, not kernel time, dominates the GPU regions (keying_grade, first round: upload 1032 ms, kernel 129 ms, download 929 ms). Placement is not deterministic: cg_multipass r3 placed 15 more tasks and was still the fastest auto run. An earlier attempt without the memory cap swapped and was discarded.

### 11.7 Test suites

P2.T6 (build f2cb62759, CI run 40058465161), all green on the RTX 3070 Ti:

| Run | Total | Pass | Fail |
|---|---|---|---|
| `ctest -L gpu` (REQUIRE_GPU, REQUIRE_GPU_HARDWARE) | 47 | 47 | 0 |
| engine ctest, `NATRON_GPU=force`, legacy | 335 | 335 | 0 |
| engine ctest, `NATRON_GPU=force`, taskgraph | 335 | 335 | 0 |

`GlInterop_Test` is zero-copy on NVIDIA EGL (UHD medians: zero-copy 1.23 ms, hostsync 1.47 ms, readback 66.96 ms; byte-exact on all three). `GlInteropQt` on `:0` with NVIDIA GL 4.6.0 is zero-copy, checksums match; under Xvfb (llvmpipe GL) it correctly falls back to readback-pbo. Vendor-specific failures found in the baseline and fixed in the tests: the linear-LUT tolerance (§3) and the pipelined-strip timing assert (decision `nvidia-pipelined-strip-timing-assert`). `GpuTransferTest.PipelinedFramesTimeline` failed once under RAM pressure (staging fallback, correct) and passed afterwards.

### 11.8 Capacity and cost-seed notes

- 16k and 24k were dropped as host capacity: the 4.1 GB image with staging plus host buffers pushed a host with about 5 GiB available into swap, and the first 16k staging cell took 293.7 s. That data is not valid.
- The 4 GB ImportCost case hit `VK_ERROR_OUT_OF_DEVICE_MEMORY` with 3.86 GiB of VRAM headroom (cause not investigated).
- The GPU cost seeds were measured on the RADV and 7800X3D host. The CPU rows are too cheap for the FX-8350 (Grade seeded at 11.5 ms/Mpx, observed about 45–50 under load), so the model undervalues offload. In cg_multipass this keeps five single-node island Grades on the CPU (about 133 MB each way predicted at about 45 ms). That errs in the safe direction (toward CPU). Predicted transfer cost tracked the measured exposed transfer, within about 2–20%.

### 11.9 Follow-ups

- Viewer hand-off validation on NVIDIA once M107 is merged (Phase 91.3 was skipped: M107 was still in progress).
- Pipelined member-time inflation: the compute→transfer release barriers inside the timed strip interval are charged real time on NVIDIA, so member timestamps undercount strip time.
- Per-vendor and per-host cost seeds, including a CPU-seed correction for slow hosts.
