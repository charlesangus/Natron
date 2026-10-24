# M91 - NVIDIA GPU Validation

The aim is to show that the Vulkan 1.3 compute backend works on NVIDIA's proprietary driver, not only on RADV and lavapipe. That covers the offline Slang→SPIR-V kernels, the runtime GLSL shaders (M126), host-import and the Vulkan→GL hand-off. The test host is bosley-faux: RTX 3070 Ti (GPU 0, bus 01:00, 8 GiB, drives the X display), Quadro P400 (GPU 1, 2 GiB, Pascal), driver 580.178.04, Vulkan 1.4.312, plus llvmpipe. CPU is an FX-8350 with 8 threads and 23 GB RAM, about 6 GB free.

Scouting on origin/main `bf781cbcd` turned up three things to fix before a baseline means anything:
- **The dev container can't see an NVIDIA GPU.** `devshell.sh --gpu` passes only `/dev/dri`, and its comment says "NVIDIA needs nvidia-container-toolkit's --gpus instead, out of scope here". The toolkit and the `nvidia` Docker runtime are installed on this host.
- **The GL interop ctest is pinned to Mesa's EGL vendor.** `Tests/Gpu/CMakeLists.txt` sets `__EGL_VENDOR_LIBRARY_FILENAMES=…/50_mesa.json`. Under that, the zero-copy interop path can never be tested on NVIDIA GL.
- **Auto device selection ties on enumeration order.** `GpuDevice::create` picks the first device of the best type rank. Both NVIDIA cards are DISCRETE_GPU, so the 3070 Ti wins only because the loader lists it first.

The rest of the milestone runs everything once, fixes what fails because of the vendor, benchmarks, and records the numbers next to the RX 7900 XTX ones.

**Scouted facts** (origin/main `bf781cbcd`; this host)

**Building and running tests**
- `tools/ci/remote/build.sh fast --tests` downloads only the AppImage and the ctest logs. The workflow also uploads a `build-tree-<preset>` artifact (`build-tree.tar`: the build tree without `.o`/`.a`), kept 7 days. Fetch it with `gh run download <run-id> -n build-tree-fast`. It holds `GpuTests` and `Tests`, configured for the CI path `/__w/Natron/Natron`.
- M107.P6.T1 already ran suites from that artifact inside natron-dev-gpu with no local build. CI runners have no NVIDIA GPU, so every GPU result must come from running that tree on this host.
- `GpuTests` links `-static-libstdc++ -static-libgcc`. It may also run natively on the host, which has glibc 2.39 and the NVIDIA libvulkan/libEGL. `Tests` needs the container's `/usr/local` Qt and Python, so it must run in a container with the NVIDIA runtime.
- `tools/gpu-spike` (`GpuBench`, `ImportCost`, `DeviceList`, `GlInteropQt`, `CpuTwinBench`) is a standalone CMake project that CI never builds. It needs a small local build in the container, then runs natively through `run-host.sh`. That script works for NVIDIA as written (`/dev/dri/renderD128`/`129` have rw ACLs for bosley), but its messages say "RADV". `run-bench.sh` defaults to the image tag `natron-dev:2027-clang21.1`, which may be stale.

**Displays and GL**
- Xvfb gives Mesa llvmpipe GL: `run-gui-test.sh` forces `__GLX_VENDOR_LIBRARY_NAME=mesa` and `LIBGL_ALWAYS_SOFTWARE=1`. Its UUIDs never match NVIDIA Vulkan, so only the Readback rung is reachable there.
- NVIDIA GL is available in two ways:
  - EGL surfaceless or device on the host, where surfaceless resolves to the RTX 3070 Ti;
  - the user's autologin Plasma X11 session on `:0`. `DISPLAY=:0 XAUTHORITY=/home/bosley/.Xauthority glxinfo -B` reports `NVIDIA GeForce RTX 3070 Ti`. Agents can use it from the host, outside any container, and windows appear on the user's desktop.
- EGL's GBM platform resolves to the Quadro P400.

**Device facts**
- Auto ranks devices by type: discrete, integrated, virtual, CPU. Auto refuses CPU-type devices (`GpuContext::isDeviceAllowed`).
- Overrides: `NATRON_GPU_DEVICE` (an index or a name substring) and the Preferences `gpuDevice` (a UUID, mapped to an index).
- The "GPU compute ready" log line in `Engine/GpuContext.cpp` doesn't name the chosen device or say why it was chosen.
- Limits where the RTX 3070 Ti differs from RADV:
  - `maxComputeWorkGroupCount` = {2³¹−1, **65535, 65535**}. RADV allows 2³²−1 in every dimension. `GpuKernel::record` fails, rather than splitting, when a dispatch exceeds this (`GpuKernel.cpp:626`).
  - `maxComputeSharedMemorySize` = 48 KiB (RADV 64 KiB). The current kernels' groupshared use is small.
  - `shaderDenormPreserveFloat32 = false`.
  - `subgroupSize` 32. No kernel relies on subgroups.
  - `maxPushConstantsSize` 256.
  - `minImportedHostPointerAlignment` 0x1000, same as RADV.
  - No ReBAR: the device-local, host-visible heap is 246 MiB.
- Queue families: 0 = graphics, compute and transfer (×16); 1 = transfer-only (×2); 2 = compute-only (×8). The code picks compute family 2 and transfer family 1, with ownership transfers in `GpuTransfer.cpp:144`.
- The P400 has `shaderFloat16 = false`.
- No Khronos validation layer is installed on the host. `GpuTestDevice.h` counts validation errors only when the layer is present.

**GlInterop on main**
- It is still M83's single-buffer version, used only by `GlInterop_Test` (EGL surfaceless) and `GlInteropQt`. M107's viewer integration (`ViewerHandoff`, the device root, the `ViewerGL` wiring, the `m107_viewer_handoff.py` UAT, `desktop-handoff-check.sh`) is only on `milestone/m107-gpu-viewer-hand-off`.
- GlInterop uses OPAQUE_FD for both memory and semaphores, which NVIDIA supports. It uses binary semaphores for GL and checks device and driver UUIDs across GL devices.
- Allocation: a dedicated VMA pool allocation with `GL_DEDICATED_MEMORY_OBJECT_EXT = TRUE`, imported with size `ai.offset + ai.size`, and `kImportSlack = 256` added to the buffer size.
- `glWaitSemaphoreEXT` and `glSignalSemaphoreEXT` pass the buffer and no textures.

**Where the RADV numbers live (plan branch)**
- `PLAN/DESIGN/2026-10-10-gpu-compute-backend.md`: §2.1 transfers, §2.2 break-even, §2.3 kernels, §2.5 interop, §3 tolerances per driver, §4 limits, §8 risk "One vendor and one driver", §9/§10 outcomes from M84/M85.
- `PLAN/DESIGN/2026-10-10-host-gpu-transfer-bench.md`. Its bench code was gitignored and is not on this host. `GpuTransfer_Test`, `ImportCost` and `GpuBench` stand in for it.
- The GPU cost seeds (`Engine/GpuCostSeeds/inputs.txt`) were measured on the RADV and 7800X3D host.

**Environment traps**
- natron-dev's `Natron.conf` can hold a stale `gpuDevice` UUID, which silently disables GPU compute (M107 environment note). Always use a fresh `XDG_CONFIG_HOME`.
- The NVIDIA driver keeps its own shader disk cache (`__GL_SHADER_DISK_CACHE`). For cold-start timings, disable it as well as clearing `XDG_CACHE_HOME`.

**Build mode:** remote (this host's default). Engine and test builds: `tools/ci/remote/build.sh fast --tests` once per phase (P1.T4, P2.T6, P3.T2) and `release` once (P4.T2); run the downloaded `build-tree-fast` in the NVIDIA lane container `NATRON_DEV_CONTAINER=natron-dev-m91-gpu` (created with `--gpu --recreate` after P1.T1), mounted at `/__w/Natron/Natron`. Only `tools/gpu-spike` builds locally, once, in that container.

## Scope

**In:**
- NVIDIA passthrough for the GPU dev container, so both the engine ctest and `GpuTests` can run on the RTX 3070 Ti.
- GL interop ctest that picks up the NVIDIA EGL vendor whenever it is present.
- Deterministic device selection on hosts with several GPUs, and a log line naming the chosen device and the reason.
- A baseline on the RTX 3070 Ti:
  - `GpuTests` (both registrations plus GlInterop);
  - the full engine ctest with `NATRON_GPU=force`, in both scheduler legs;
  - a check that auto mode and host-import work;
  - GL interop through EGL and through a real NVIDIA X display.
- Fixes for failures that are specific to the vendor, or follow-ups filed for large ones.
- In-app viewer hand-off on NVIDIA GL. This happens after M107 merges (see Phase 91.3).
- `GpuBench`, `ImportCost`, transfer and interop timings, and an end-to-end release render profile on NVIDIA, written into the GPU design note next to RADV. A decision entry for every vendor-specific workaround.

**Out:**
- The viewer hand-off itself, and its double-buffered `GlInterop`: M107 - GPU Viewer Hand-off. M91 only validates it.
- OFX GPU suites: M86 - OFX GPU Suites. Raw OpenCL→Slang kernels: M81 - Raw GPU Kernels. Point-op fusion: M87 - Point-Op Kernel Fusion. Validate these in a follow-up if they land after M91.
- Full validation on the Quadro P400. It is an optional data point only; a follow-up, unowned.
- Tuning kernel performance for NVIDIA, and re-measuring the GPU cost seeds per vendor. Follow-ups, unowned.
- An NVIDIA CI runner, and validation layers in the image or CI. Unowned.
- Intel, macOS and MoltenVK.

**Edge cases:**
- **Enumeration order puts the P400 first** (another host, or a changed loader). Covered by the selection tie-break in P1.T3.
- **llvmpipe is present.** Force accepts it, but it ranks last. Auto refuses it.
- **EGL picks a different GPU from Vulkan.** For example, `NATRON_GPU_DEVICE=1` selects the P400 while EGL surfaceless gives the 3070 Ti. The UUIDs mismatch, so the readback rung must be chosen and the reason logged. That counts as a pass, not a failure.
- **The host runs out of capacity rather than hitting a vendor bug.** About 6 GB of host RAM is free, VRAM is 8 GiB against RADV's 24 GiB, and there is no ReBAR. Big-transfer and 16k/24k cases may fail with out-of-memory. Each such failure is classified as a host-capacity issue, not a vendor bug, and the bench sizes are cut to fit.
- **A failure that also reproduces on lavapipe in CI, or on RADV**, is not vendor-specific. Record it and route it to the owning milestone or a follow-up.
- **First-run pipeline compile on NVIDIA is slow when the cache is cold.** A timeout on the first run is retried once with a warm cache before it counts as a failure.
- **The container's `Natron.conf` holds a stale `gpuDevice` UUID.** Every engine run uses a fresh `XDG_CONFIG_HOME`.
- **The `:0` session is logged out or locked.** The in-app zero-copy check then falls back to EGL tests plus `desktop-handoff-check.sh`, run by the user.
- **Denormals are not preserved** (`shaderDenormPreserveFloat32 = false`). Comparisons that need exact subnormals can differ in ways the spec allows.

**Assumptions:**
- Driver 580.178.04 and the Docker `nvidia` runtime stay installed. The dev image (aswf ci-vfxall, CUDA-based) accepts NVIDIA runtime injection of the Vulkan ICD and GLX/EGL libraries through `NVIDIA_DRIVER_CAPABILITIES`.
- The remote `build-tree-fast` artifact runs in a local container mounted at the CI workspace path, as M107.P6.T1 proved.
- The existing test tolerances are the bar RADV met. Widening one needs a decision entry showing the NVIDIA result is legal under the SPIR-V/Vulkan precision rules.
- Agents may briefly open windows on the user's `:0` desktop for the interop and UAT checks.

## Phase 91.1: Bring-up and baseline

- [ ] M91.P1.T1 — Run the GPU dev container on NVIDIA through nvidia-container-toolkit
  - files: `tools/ci/local/devshell.sh`, `tools/ci/local/README.md` (the "GPU container" section only)
  - approach: In the `--gpu` branch, detect NVIDIA: `/dev/nvidiactl` exists and `docker info --format '{{json .Runtimes}}'` lists `nvidia`. When it does, add `--runtime=nvidia -e NVIDIA_VISIBLE_DEVICES=all -e NVIDIA_DRIVER_CAPABILITIES=compute,graphics,utility,display`. Keep the `/dev/dri` args. Replace the "out of scope" comment. If the image's loader doesn't search the directory where the toolkit drops `nvidia_icd.json`, export `VK_ADD_DRIVER_FILES` to that path; if its glvnd doesn't see the injected `10_nvidia.json`, export `__EGL_VENDOR_LIBRARY_DIRS` — each only when the file exists. An existing container keeps the runtime it was created with, so the README says to use `--recreate` (or a new name such as `NATRON_DEV_CONTAINER=natron-dev-m91-gpu`). README text goes in the GPU container section, not the "RADV GL display" section M107 adds.
  - verify: `NATRON_DEV_CONTAINER=natron-dev-m91-gpu tools/ci/local/devshell.sh --gpu --recreate sh -c 'nvidia-smi -L; eglinfo -B | grep -i renderer'` lists both GPUs and an NVIDIA EGL renderer. A Vulkan device listing inside the container (`vulkaninfo --summary` if present, otherwise `DeviceList` from P1.T4) shows the RTX 3070 Ti, P400 and llvmpipe. `bash -n` and `shellcheck` pass. On a host without NVIDIA the generated `docker run` arguments don't change.
  - size: M

- [ ] M91.P1.T2 — Let the GlInterop ctest load whichever EGL vendors the system provides, not only Mesa
  - files: `Tests/Gpu/CMakeLists.txt`
  - approach: replace `__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json` with `__EGL_VENDOR_LIBRARY_DIRS=/usr/share/glvnd/egl_vendor.d`, under the same `if(EXISTS …)` guard. Update the comment. glvnd then reads the directory at run time: Mesa alone on CI and RADV hosts, `10_nvidia.json` first in the NVIDIA container.
  - verify: in the P1.T4 run, `ctest -R '^Gpu\.GlInteropTest' -V` in the NVIDIA container shows an NVIDIA GL renderer and UUID match. Remote CI on lavapipe stays green.
  - size: S

- [ ] M91.P1.T3 — Make auto device selection deterministic on hosts with several GPUs, and log the choice
  - files: `Engine/Gpu/GpuDevice.h`, `Engine/Gpu/GpuDevice.cpp`, `Engine/GpuContext.cpp`, `Tests/Gpu/GpuDevice_Test.cpp`
  - approach: Move the auto pick into a pure function, `pickAutoDevice(const std::vector<GpuDeviceCandidate>&)`, where a candidate holds the index, type, largest device-local heap and whether it meets requirements. Order by type rank, then larger device-local heap, then lower index. Record a short `selectionReason` in `GpuDeviceInfo`, e.g. `auto: discrete, 8 GiB, over Quadro P400 (2 GiB), llvmpipe (cpu)`, or `NATRON_GPU_DEVICE=1`. Add device name, driver version and reason to the existing "GPU compute ready" line in `GpuContext.cpp` (one line, a different hunk from M107's `evictedCopy` change). Explicit selectors (index, name, Preferences UUID) behave as before.
  - verify: new `GpuDeviceTest` cases feed fake candidates (3070 Ti, P400, llvmpipe) in every permutation and always get the 3070 Ti; plus only-llvmpipe, larger-device-fails-requirements, and two identical discrete GPUs tie-breaking to the lower index. In the P1.T4 run, the engine log names the RTX 3070 Ti with that reason.
  - size: M

- [ ] M91.P1.T4 — Baseline run on the RTX 3070 Ti: one remote build, then every GPU suite and the engine ctest locally
  - files: none in code. Raw output goes to `.local/m91/baseline/`. A "Baseline findings" section goes in this milestone file.
  - approach: Push the milestone branch with P1.T1–T3. Run `tools/ci/remote/build.sh fast --tests` once (also gives the fast AppImage). Fetch the build tree with `gh run download <run-id> -n build-tree-fast`; mount or symlink it at `/__w/Natron/Natron` in the P1.T1 container, following M107.P6.T1's recipe. In the container, with fresh `XDG_CONFIG_HOME` and `XDG_CACHE_HOME`: (a) `NATRON_TEST_REQUIRE_GPU=1 NATRON_TEST_REQUIRE_GPU_HARDWARE=1 ctest -L gpu --output-on-failure` — check each suite's `vulkan device:` line names the RTX 3070 Ti, record the GlInterop best path and `missing` string; (b) the full engine ctest with `NATRON_GPU=force NATRON_TEST_REQUIRE_GPU=1` under `xvfb-run`, once per scheduler (`NATRON_RENDER_SCHEDULER=legacy` and `taskgraph`), as `tools/ci/local/test.sh ctest` does; (c) optional: `GpuTests` natively on the host. Locally build `tools/gpu-spike` (`cmake -S tools/gpu-spike -B build/gpu-spike -G Ninja -DCMAKE_BUILD_TYPE=Release` in the container); fix bit-rot only if it doesn't build. Run `DeviceList` through `run-host.sh`. If a Khronos validation layer can be had without sudo (LunarG SDK tarball via `VK_ADD_LAYER_PATH`), run `GpuTests` once more with it. Optionally run `NATRON_GPU_DEVICE=1` (P400) `GpuTests` once. For every failure, record suite, case, assert text, and whether it reproduces on lavapipe (remote CI logs) and on RADV (M107/M109 records).
  - verify: the findings section lists every gpu-labelled and engine suite as pass, fail or skip, on the named device, for both schedulers; holds the GlInterop path line; classifies each failure as vendor / host-capacity / not-vendor. No suite reports "no usable Vulkan device".
  - size: M

- [ ] M91.P1.T5 — Check auto mode, host-import and GL interop on a real NVIDIA display
  - files: none in code. Output goes to `.local/m91/baseline/`, findings appended to the same section.
  - approach: Run the P1.T4 fast AppImage on the host (`env -u LD_LIBRARY_PATH`, fresh config) with `NATRON_GPU=auto`, rendering `tools/bench/comps/keying_grade.ntp` headless through NatronRenderer with `NATRON_RENDER_PROFILE` on. From the log record: the "GPU compute ready" line (device, reason, host-import against staging calibration: `hostImport` true, import alignment 4096); that regions are placed on the GPU; any `incLegacyFallbacks` reasons. Run `GlInteropQt` (from P1.T4's `build/gpu-spike`) under `DISPLAY=:0 XAUTHORITY=/home/bosley/.Xauthority` and under Xvfb; record `best`, `missing` and checksum (zero-copy expected on `:0`, readback under Xvfb). If `:0` isn't available, note it and rely on the EGL GlInterop result.
  - verify: the findings section shows auto picking the RTX 3070 Ti with host-import active (or the logged reason why not), region placement in the render profile, and `GlInteropQt` best rungs on `:0` and Xvfb with byte-exact checksums (or the `missing` string).
  - size: M

- [ ] M91.P1.T6 — Triage the baseline and turn Phase 91.2's template tasks into concrete tasks
  - files: this milestone file (plan branch)
  - approach: Map each vendor-specific failure to one P2 template, or file it as a follow-up milestone stub when the fix is large (more than about one M task, or design work). Delete the P2 templates nothing triggered. Route non-vendor failures to their owner. Route GlInterop and viewer failures per P2.T1's sequencing rule.
  - verify: every failure in the findings section has a P2 task, a follow-up stub or a written "not vendor-specific → <owner>" line. P2 contains only triggered tasks.
  - size: M

## Phase 91.2: Vendor-specific fixes (template tasks; instantiate only what P1.T6 triggers)

- [ ] M91.P2.T1 — (if GlInterop zero-copy or hostsync fails under NVIDIA GL) Fix the GL import or semaphore path
  - files: `Engine/Gpu/GlInterop.cpp`, `Tests/Gpu/GlInterop_Test.cpp` (**both are M107 files**)
  - approach: **Sequencing:** while M107 - GPU Viewer Hand-off is in progress, send the finding and a proposed patch to M107's owner through the inbox rather than editing these files; once M107 has merged, fix it here on top of its double-buffered `GlInterop`. Suspects, in order: (1) GL requires a dedicated import, so the Vulkan allocation must carry `VkMemoryDedicatedAllocateInfo` naming the buffer — check the VMA pool path with `pMemoryAllocateNext` really emits it; (2) the `glImportMemoryFdEXT` size must equal the `VkDeviceMemory` allocation size (NVIDIA is strict) — compare `ai.offset + ai.size`, including `kImportSlack`, with `VkMemoryRequirements.size`; (3) `glWaitSemaphoreEXT` / `glSignalSemaphoreEXT` buffer lists and layouts; (4) binary semaphore reuse order across `produce`/`upload`; (5) the right `GL_DEVICE_UUID_EXT` index with two NVIDIA GPUs.
  - verify: `Gpu.GlInteropTest.*` passes every path on NVIDIA EGL, zero-copy best and byte-exact. `GlInteropQt` on `:0` gives `best=zero-copy`. Lavapipe CI stays green.
  - size: M

- [ ] M91.P2.T2 — (if host-import fails, or falls back silently) Fix host-pointer import on NVIDIA
  - files: `Engine/Gpu/GpuTransfer.cpp`, `Engine/HostImageMemory.cpp`, `Tests/Gpu/GpuTransfer_Test.cpp`
  - approach: Suspects: memory type choice from `vkGetMemoryHostPointerPropertiesEXT`'s `memoryTypeBits` (NVIDIA offers only host-visible, non-device-local types); importing `aligned_alloc` huge-page buffers (`HostImageMemory.cpp:245`); size padding to 4096; the 4 GiB per-copy chunk on imported buffers; the cached-import free hook. Any fallback to staging must be logged once with its reason.
  - verify: the `GpuTransfer_Test` host-import cases pass on NVIDIA. P1.T5's render shows `hostImport` true, or a logged fallback reason. Lavapipe CI stays green.
  - size: M

- [ ] M91.P2.T3 — (if GPU-vs-CPU parity fails) Make the failing kernel agree, or justify a tolerance the spec allows
  - files: `Engine/Gpu/Kernels/<kernel>.slang` (whichever failed), `Tests/Gpu/Tolerance.h` or `Tests/Native/NativeGpuParity.{h,cpp}`, plus the failing test file
  - approach: Suspects: fp32 denormals (NVIDIA can't preserve them); FMA contraction where the reference assumes unfused arithmetic; `pow`/`exp2`/`log2` precision at the SPIR-V precision-table limits; inf/NaN edge cases (`shaderSignedZeroInfNanPreserveFloat32` is true). If the result is legal under the precision rules, extend the existing GPU slack the same way as Grade's "value-or-0 for subnormals" rule, record a decision, and add an NVIDIA column to the design note's §3. Otherwise fix the kernel explicitly, as with the earlier `pow(v, 0)` fix. Never widen a tolerance just because RADV would still pass.
  - verify: the failing parity suites pass on NVIDIA and still pass on lavapipe (CI). Any widened bound is listed in the decision with its worst-case NVIDIA value.
  - size: M

- [ ] M91.P2.T4 — (if a device-limit or memory-heap assumption fails) Respect NVIDIA's limits
  - files: `Engine/Gpu/GpuKernel.cpp`, `Engine/Gpu/GpuBuffer.cpp`, `Engine/Gpu/GpuBufferPool.cpp`, `Engine/Gpu/GpuTexture.cpp`, plus a matching `Tests/Gpu/*_Test.cpp`
  - approach: Suspects: (1) a dispatch with Y or Z groups over 65535 — `GpuKernel::record` errors rather than splitting; fold extra rows into Z, or split with a base offset; (2) `VMA_MEMORY_USAGE_AUTO` + `HOST_ACCESS` buffers (`GpuKernel.cpp:136`, `GpuTexture.cpp:198`) landing in the 246 MiB BAR heap and exhausting it; (3) budget and eviction under 8 GiB VRAM; (4) compute/transfer queue-family assumptions. Read every limit from the device; never hard-code one.
  - verify: the failing case passes on NVIDIA. A test forcing a small device limit covers the split path and runs on lavapipe in CI.
  - size: M

- [ ] M91.P2.T5 — (if timeouts, hangs, corruption or validation errors appear) Fix the synchronization bug NVIDIA exposes
  - files: `Engine/Gpu/GpuTransfer.cpp`, `Engine/Gpu/GpuRecord.cpp`, `Engine/Gpu/GpuRuntime.cpp`, `Engine/GpuRegionTask.cpp` (only after M107.P5 has merged; an M107/M109 hotspot), plus the failing test
  - approach: suspects are a missing queue-family ownership release/acquire between transfer family 1 and compute family 2, timeline-semaphore wait values, and `synchronization2` stage/access masks RADV happens to tolerate. Use the P1.T4 validation-layer run if one was obtained.
  - verify: the failing suite passes 3 times in a row on NVIDIA, and on lavapipe CI.
  - size: M

- [ ] M91.P2.T6 — Gate build for the Phase 2 fixes
  - files: none in code. Output goes to `.local/m91/p2/`.
  - approach: one remote `fast --tests` on the milestone HEAD carrying every P2 fix. Rerun P1.T4 (a) and (b) and the P1.T5 checks from the new build tree and AppImage.
  - verify: every gpu-labelled suite and the engine ctest (both schedulers, `NATRON_GPU=force`) pass on the RTX 3070 Ti. The remote CI legs on lavapipe are green. Failures filed as follow-ups are listed.
  - size: M

## Phase 91.3: Viewer hand-off on NVIDIA GL (⚑ start after M107 - GPU Viewer Hand-off merges)

If M107 hasn't merged when Phase 91.2 finishes, skip this phase and file it as a follow-up milestone. The acceptance line for the viewer path is then met by `GlInterop_Test` (NVIDIA EGL) and `GlInteropQt` on `:0`, and a decision records that.

- [ ] M91.P3.T1 — Let the GUI test harness run on an existing host display with that display's GL
  - files: `Tests/gui/run-gui-test.sh` (M107 also changes this file, hence the ⚑)
  - approach: Add a mode, `GUI_TEST_HOST_DISPLAY=:0`, that skips Xvfb, the `/usr/lib64` GL symlink directory and the `mesa`/`LIBGL_ALWAYS_SOFTWARE` exports, running the extracted AppImage on the host against the system's glvnd with `env -u LD_LIBRARY_PATH`. Fail with a clear message when the display can't be opened, or when the GL renderer isn't hardware (that check takes an opt-out). The default Xvfb mode stays byte-identical.
  - verify: `GUI_TEST_HOST_DISPLAY=:0 XAUTHORITY=… Tests/gui/run-gui-test.sh Tests/gui/<existing smoke>.py <fast AppImage>` passes and logs an NVIDIA renderer. The same script still passes under Xvfb in the container. `bash -n` and `shellcheck` pass.
  - size: M

- [ ] M91.P3.T2 — Run M107's suites and UAT on NVIDIA, on every rung
  - files: none in code. Output goes to `.local/m91/p3/`, results in this milestone file.
  - approach: One remote `fast --tests` on M91's branch rebased onto main after M107 merged. In the NVIDIA container, run `ViewerHandoff_Test`, `ViewerRootOnDevice_Test`, `ViewerGpuProcess_Test`, `OcioVulkanShader_Test` and `GlInterop_Test`, in both scheduler modes. Run `Tests/gui/m107_viewer_handoff.py`: cpu reference first; then `NATRON_VIEWER_HANDOFF=zerocopy` and `hostsync` on `:0` (P3.T1); `readback` under Xvfb; `NATRON_GPU_FAULT=lost@40` on the hardware device. Run `tools/gpu-spike/desktop-handoff-check.sh` on `:0`. Check via `NATRON_RENDER_PROFILE` that root regions report `downloadBytes == 0` on zerocopy and hostsync.
  - verify: all suites pass on the RTX 3070 Ti. Each UAT config shows `SUMMARY 0 failure(s)`. The handoff report says zero-copy was reached on NVIDIA GL, or names what is missing and the logged fallback rung. Viewer-path failures are fixed under P2.T1's rules or filed as follow-ups.
  - size: M

## Phase 91.4: Benchmarks and documentation

- [ ] M91.P4.T1 — Microbenchmarks on the RTX 3070 Ti: GpuBench, ImportCost, transfers and interop
  - files: none in code (unless `run-bench.sh`/`run-host.sh` need their RADV wording or stale image tag fixed). Output goes to `.local/m91/bench/`.
  - approach: Run `tools/gpu-spike/run-bench.sh --no-cpu --sizes uhd,8k,16k --runs 2` (24k needs ~9.2 GB host RAM this host doesn't have free). Run `ImportCost` through `run-host.sh`. Take the `GpuTransfer_Test` pipelined UHD timings (§2.1 source) and `GlInterop_Test` per-path timings (§2.5) from the P2.T6 or P3.T2 runs. Use the release preset for the gpu-spike build; run with no other load; record `nvidia-smi -q -d CLOCK,PCIE` next to the numbers.
  - verify: CSVs and summaries exist for each size. A table mirrors §2.1, §2.2, §2.3 and §2.5 with NVIDIA values, plus the host's PCIe link, CPU and RAM.
  - size: M

- [ ] M91.P4.T2 — End-to-end release render on NVIDIA: placement sanity and gain over the CPU
  - files: none in code. Output goes to `.local/m91/bench/e2e/`.
  - approach: One remote `release` build (stripped AppImage). Render `tools/bench/comps/{keying_grade,cg_multipass,defocus_retime}.ntp` headless on the host, with `NATRON_GPU=auto` then `off`, 3 interleaved rounds, with `NATRON_RENDER_PROFILE`. Summarise with `tools/bench/render-profile-report.py`. Record which regions went to the GPU and the gain; note where RADV-derived cost seeds misplace work on this GPU and slow CPU, and file a follow-up if they do.
  - verify: a median-of-3 table per comp for auto and off, a GPU-region count, and an explicit statement that auto is not slower than off beyond noise — or a follow-up stub saying why.
  - size: M

- [ ] M91.P4.T3 — Record the NVIDIA numbers and decisions in the GPU design note
  - files: `PLAN/DESIGN/2026-10-10-gpu-compute-backend.md`, `PLAN/DECISIONS/<date>-nvidia-*.md` (one per workaround, only if any). Plan branch only.
  - approach: Add §11, "Outcomes from M91 - NVIDIA GPU Validation", with the P4.T1 and P4.T2 tables beside the RADV figures, stating host differences up front (FX-8350 vs 7800X3D, PCIe link, 8 vs 24 GiB, no ReBAR). Add an NVIDIA column to §3; NVIDIA rows to §4 (dispatch Y/Z limit, 48 KiB shared memory, no fp32 denormal preservation, 246 MiB BAR heap). Update §8's "One vendor and one driver" risk. One decision entry per vendor-specific workaround or widened tolerance.
  - verify: §11 exists with numbers for every bench in P4.T1 and P4.T2. Every P2 workaround has a decision file. §8 no longer says NVIDIA is untested.
  - size: M

**Verification gate:** On the RTX 3070 Ti, from the milestone's final remote `fast --tests` build tree run in the NVIDIA container: every `gpu`-labelled ctest suite (`Gpu.*`, `GpuNoPushDescriptor.*`, `Gpu.GlInteropTest.*` with zero-copy and semaphores reached through NVIDIA EGL) passes with `NATRON_TEST_REQUIRE_GPU=1 NATRON_TEST_REQUIRE_GPU_HARDWARE=1`, and the full engine ctest passes with `NATRON_GPU=force` under both legacy and taskgraph schedulers; parity suites pass at RADV tolerances or at bounds widened only by a justifying decision; the P1.T3 log line shows auto choosing the RTX 3070 Ti with host-import active; `GlInteropQt` on `:0` reaches zero-copy or logs its fallback reason; if M107 merged, the Phase 91.3 suites pass and UAT configs show `SUMMARY 0 failure(s)`; remote CI (lavapipe) is green; §11 and decision entries are committed on the plan branch; every unfixed failure is filed as a follow-up stub.

## Decisions

- 2026-10-23 — Stub elaborated after the claim on bosley-faux (RTX 3070 Ti + Quadro P400, driver 580.178.04), which resolved the "no NVIDIA host" blocker.
- 2026-10-23 — Overlap with M107 - GPU Viewer Hand-off (doing, owner `gpu`): hard overlap only on `Engine/Gpu/GlInterop.{h,cpp}`, `Tests/Gpu/GlInterop_Test.cpp`, `Engine/ViewerHandoff.*`, `Engine/GpuRegionTask.cpp` and `Tests/gui/run-gui-test.sh`; M91 routes findings there through the inbox and touches them only after M107 merges (P2.T1, P2.T5, Phase 91.3). Same-file different-hunk overlap on `tools/ci/local/README.md` and `Engine/GpuContext.cpp`.
- 2026-10-23 — P1.T6 resized S → M: triage needs judgement.
- 2026-10-23 — User chose to proceed despite the §8a overlap with M107 - GPU Viewer Hand-off (earlier row): run Phases 91.1, 91.2 and 91.4 now, leave M107-owned files untouched until it merges, send interop findings to its owner through the inbox, and the second to merge rebases.
- 2026-10-23 — User allowed agents to briefly open test windows on the `:0` Plasma session for the NVIDIA GL checks (P1.T5, Phase 91.3).
