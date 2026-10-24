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

- [x] M91.P1.T1 — Run the GPU dev container on NVIDIA through nvidia-container-toolkit
  - files: `tools/ci/local/devshell.sh`, `tools/ci/local/README.md` (the "GPU container" section only)
  - approach: In the `--gpu` branch, detect NVIDIA: `/dev/nvidiactl` exists and `docker info --format '{{json .Runtimes}}'` lists `nvidia`. When it does, add `--runtime=nvidia -e NVIDIA_VISIBLE_DEVICES=all -e NVIDIA_DRIVER_CAPABILITIES=compute,graphics,utility,display`. Keep the `/dev/dri` args. Replace the "out of scope" comment. If the image's loader doesn't search the directory where the toolkit drops `nvidia_icd.json`, export `VK_ADD_DRIVER_FILES` to that path; if its glvnd doesn't see the injected `10_nvidia.json`, export `__EGL_VENDOR_LIBRARY_DIRS` — each only when the file exists. An existing container keeps the runtime it was created with, so the README says to use `--recreate` (or a new name such as `NATRON_DEV_CONTAINER=natron-dev-m91-gpu`). README text goes in the GPU container section, not the "RADV GL display" section M107 adds.
  - verify: `NATRON_DEV_CONTAINER=natron-dev-m91-gpu tools/ci/local/devshell.sh --gpu --recreate sh -c 'nvidia-smi -L; eglinfo -B | grep -i renderer'` lists both GPUs and an NVIDIA EGL renderer. A Vulkan device listing inside the container (`vulkaninfo --summary` if present, otherwise `DeviceList` from P1.T4) shows the RTX 3070 Ti, P400 and llvmpipe. `bash -n` and `shellcheck` pass. On a host without NVIDIA the generated `docker run` arguments don't change.
  - size: M

- [x] M91.P1.T2 — Let the GlInterop ctest load whichever EGL vendors the system provides, not only Mesa
  - files: `Tests/Gpu/CMakeLists.txt`
  - approach: replace `__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json` with `__EGL_VENDOR_LIBRARY_DIRS=/usr/share/glvnd/egl_vendor.d`, under the same `if(EXISTS …)` guard. Update the comment. glvnd then reads the directory at run time: Mesa alone on CI and RADV hosts, `10_nvidia.json` first in the NVIDIA container.
  - verify: in the P1.T4 run, `ctest -R '^Gpu\.GlInteropTest' -V` in the NVIDIA container shows an NVIDIA GL renderer and UUID match. Remote CI on lavapipe stays green.
  - size: S

- [x] M91.P1.T3 — Make auto device selection deterministic on hosts with several GPUs, and log the choice
  - files: `Engine/Gpu/GpuDevice.h`, `Engine/Gpu/GpuDevice.cpp`, `Engine/GpuContext.cpp`, `Tests/Gpu/GpuDevice_Test.cpp`
  - approach: Move the auto pick into a pure function, `pickAutoDevice(const std::vector<GpuDeviceCandidate>&)`, where a candidate holds the index, type, largest device-local heap and whether it meets requirements. Order by type rank, then larger device-local heap, then lower index. Record a short `selectionReason` in `GpuDeviceInfo`, e.g. `auto: discrete, 8 GiB, over Quadro P400 (2 GiB), llvmpipe (cpu)`, or `NATRON_GPU_DEVICE=1`. Add device name, driver version and reason to the existing "GPU compute ready" line in `GpuContext.cpp` (one line, a different hunk from M107's `evictedCopy` change). Explicit selectors (index, name, Preferences UUID) behave as before.
  - verify: new `GpuDeviceTest` cases feed fake candidates (3070 Ti, P400, llvmpipe) in every permutation and always get the 3070 Ti; plus only-llvmpipe, larger-device-fails-requirements, and two identical discrete GPUs tie-breaking to the lower index. In the P1.T4 run, the engine log names the RTX 3070 Ti with that reason.
  - size: M

- [x] M91.P1.T4 — Baseline run on the RTX 3070 Ti: one remote build, then every GPU suite and the engine ctest locally
  - files: none in code. Raw output goes to `.local/m91/baseline/`. A "Baseline findings" section goes in this milestone file.
  - approach: Push the milestone branch with P1.T1–T3. Run `tools/ci/remote/build.sh fast --tests` once (also gives the fast AppImage). Fetch the build tree with `gh run download <run-id> -n build-tree-fast`; mount or symlink it at `/__w/Natron/Natron` in the P1.T1 container, following M107.P6.T1's recipe. In the container, with fresh `XDG_CONFIG_HOME` and `XDG_CACHE_HOME`: (a) `NATRON_TEST_REQUIRE_GPU=1 NATRON_TEST_REQUIRE_GPU_HARDWARE=1 ctest -L gpu --output-on-failure` — check each suite's `vulkan device:` line names the RTX 3070 Ti, record the GlInterop best path and `missing` string; (b) the full engine ctest with `NATRON_GPU=force NATRON_TEST_REQUIRE_GPU=1` under `xvfb-run`, once per scheduler (`NATRON_RENDER_SCHEDULER=legacy` and `taskgraph`), as `tools/ci/local/test.sh ctest` does; (c) optional: `GpuTests` natively on the host. Locally build `tools/gpu-spike` (`cmake -S tools/gpu-spike -B build/gpu-spike -G Ninja -DCMAKE_BUILD_TYPE=Release` in the container); fix bit-rot only if it doesn't build. Run `DeviceList` through `run-host.sh`. If a Khronos validation layer can be had without sudo (LunarG SDK tarball via `VK_ADD_LAYER_PATH`), run `GpuTests` once more with it. For every failure, record suite, case, assert text, and whether it reproduces on lavapipe (remote CI logs) and on RADV (M107/M109 records).
  - verify: the findings section lists every gpu-labelled and engine suite as pass, fail or skip, on the named device, for both schedulers; holds the GlInterop path line; classifies each failure as vendor / host-capacity / not-vendor. No suite reports "no usable Vulkan device".
  - size: M

- [x] M91.P1.T5 — Check auto mode, host-import and GL interop on a real NVIDIA display
  - files: none in code. Output goes to `.local/m91/baseline/`, findings appended to the same section.
  - approach: Run the P1.T4 fast AppImage on the host (`env -u LD_LIBRARY_PATH`, fresh config) with `NATRON_GPU=auto`, rendering `tools/bench/comps/keying_grade.ntp` headless through NatronRenderer with `NATRON_RENDER_PROFILE` on. From the log record: the "GPU compute ready" line (device, reason, host-import against staging calibration: `hostImport` true, import alignment 4096); that regions are placed on the GPU; any `incLegacyFallbacks` reasons. Run `GlInteropQt` (from P1.T4's `build/gpu-spike`) under `DISPLAY=:0 XAUTHORITY=/home/bosley/.Xauthority` and under Xvfb; record `best`, `missing` and checksum (zero-copy expected on `:0`, readback under Xvfb). If `:0` isn't available, note it and rely on the EGL GlInterop result.
  - verify: the findings section shows auto picking the RTX 3070 Ti with host-import active (or the logged reason why not), region placement in the render profile, and `GlInteropQt` best rungs on `:0` and Xvfb with byte-exact checksums (or the `missing` string).
  - size: M

- [x] M91.P1.T6 — Triage the baseline and turn Phase 91.2's template tasks into concrete tasks
  - files: this milestone file (plan branch)
  - approach: Map each vendor-specific failure to one P2 template, or file it as a follow-up milestone stub when the fix is large (more than about one M task, or design work). Delete the P2 templates nothing triggered. Route non-vendor failures to their owner. Route GlInterop and viewer failures per P2.T1's sequencing rule.
  - verify: every failure in the findings section has a P2 task, a follow-up stub or a written "not vendor-specific → <owner>" line. P2 contains only triggered tasks.
  - size: M

## Baseline findings (P1.T4, 2026-10-23, build d338d2af1, CI run 40052482940)

How to repeat: download `build-tree-fast` to tmpfs (`/tmp/m91`; the repo disk had ~130 MB free); rebuild test assets from the CI AppImage (`--appimage-extract`, copy `Plugins/OFX/Natron`) or `tools/ci/local/fetch-assets.sh` — the local `build/assets` was stale against the pins and made `Shards/CacheEquivalenceSweepTest` fail (Sketch non-deterministic, then munmap_chunk crash), which is not vendor-related. Run in a plain `docker run --runtime=nvidia` container from the dev image with the checkout at `/__w/Natron/Natron` and the build tree mounted over its `build/`, because devshell.sh has no extra-mount option. Use a fresh `XDG_CONFIG_HOME`/`XDG_CACHE_HOME` per run; scripts are in `.local/m91/baseline/`.

| Run | Total | Pass | Fail | Device |
|---|---|---|---|---|
| `ctest -L gpu` (REQUIRE_GPU, REQUIRE_GPU_HARDWARE) | 47 | 45 | 2 | RTX 3070 Ti |
| engine ctest, `NATRON_GPU=force`, legacy | 335 | 332 | 3 | RTX 3070 Ti |
| engine ctest, `NATRON_GPU=force`, taskgraph | 335 | 332 | 3 | RTX 3070 Ti |
| host-native `GpuTests` (no GlInterop) | 121 cases | 119 | 2 | RTX 3070 Ti |
| host-native `GpuTests` with Khronos validation (SDK 1.4.363) | 121 cases | 119 | 2 | zero real validation errors |

- `GpuDeviceAutoPick.*` (5 cases) pass. The engine log shows: `GPU compute ready on NVIDIA GeForce RTX 3070 Ti (driver 580.178.4, selected by auto: discrete, 8 GiB, over Quadro P400 (2 GiB), llvmpipe (...) (cpu)) after 987.8 ms (... host import calibration 26.3 ms ...)`.
- GlInterop under Xvfb in the NVIDIA container used NVIDIA GL (`4.6.0 NVIDIA 580.178.04`): `best path: zero-copy`, `missing: (none)`, uuidMatch=1, byte-exact on zero-copy, hostsync and readback. UHD RGBA32F end-to-end medians: zero-copy 1.23 ms, hostsync 1.47 ms, readback 66.96 ms. 8/8 pass.

Failures (none of them reproduces on the lavapipe CI legs):
- `GpuRuntimeShaderTest.Lut3DLinear` / `Lut2DLinear` (both registrations), `Tests/Gpu/GpuRuntimeShader_Test.cpp:344`: worst 1.55e-4 / 1.26e-5 against kTolerance 1e-5. Deterministic. **Vendor-specific**: NVIDIA's hardware linear filter evidently doesn't give the exact result the test assumes (the comment at :149-151 assumes fixed-point weights exact on a 1/32 grid).
- `GpuResidencyTest.RegionMembersAreTimedOnTheDevice`, `Tests/GpuResidency_Test.cpp:1992`, pipelined-strips mode: kernelMs 0.945 against a member-timestamp sum of 0.877 (≈7%, against 5% allowed). Flaky (passed on one rerun, failed twice). **Vendor-specific timing**; cause unconfirmed.
- `GpuTransferTest.PipelinedFramesTimeline`, `GpuTransfer_Test.cpp:404-405`: expected HostImport, got Staging on all frames. Failed only while host RAM was tight, then passed 3/3 standalone and in later runs. **Host-capacity (probable)**. The engine's staging fallback worked; only the strict assert tripped.
- `Shards/CacheEquivalenceSweepTest`: stale local assets. **Not vendor**; passes with pinned assets.

P1.T5 (2026-10-23, fast AppImage d338d2af1 on the host; artifacts in `.local/m91/baseline/t5/`):
- Auto mode: NatronRenderer rendering `keying_grade.ntp` (Write path redirected to /tmp), taskgraph, frames 1-6, exit 0. `GPU compute ready on NVIDIA GeForce RTX 3070 Ti (driver 580.178.4, selected by auto: discrete, 8 GiB, over Quadro P400 (2 GiB), llvmpipe (...) (cpu)) after 456.0 ms (... host import calibration 14.1 ms, staging calibration 0.0 ms)`.
- Placement: 36 region records (6 regions per frame), all ok, empty `fallback`, 0 retries. 78 GPU and 66 CPU node records; the CPU ones are Rectangle, Ramp, Merge and Write (no GPU support).
- Host-import transfers: uploads 52 HostImport / 4 Staged, downloads 49 HostImport / 7 Staged. The staged ones fall in frames 1-3 across regions, and no reason is logged. Triage this in P1.T6.
- GlInteropQt (in the NVIDIA container): on `:0` with NVIDIA GL 4.6.0 580.178.04 the path is **zero-copy**, 0.244 ms at 256x192, checksums MATCH. Under Xvfb (llvmpipe GL) it falls back to **readback-pbo** with logged blockers (no GL_EXT_semaphore(_fd), UUID mismatch), checksums MATCH. That is the expected rung.

## Phase 91.2: Vendor-specific fixes (triaged by P1.T6)

Triage of the baseline: P2.T1 (GlInterop) and P2.T4 (device limits) were not triggered and are dropped. Zero-copy and every limit held on NVIDIA. `GpuTransferTest.PipelinedFramesTimeline` is classed host-capacity: it failed only under RAM pressure, the engine's staging fallback was correct, and it passed on every rerun. It gets no task, but P2.T6 must see it pass. `Shards/CacheEquivalenceSweepTest` was caused by stale local assets and is not vendor-related. The diagnosis for each task below was done read-only on the RTX 3070 Ti.

- [x] M91.P2.T2 — Record why a transfer took the staging path instead of host-import, and log import failures once
  - files: `Engine/Gpu/GpuTransfer.cpp`, `Engine/Gpu/GpuTransfer.h`, `Engine/RenderScheduler.h`, `Engine/RenderStats.cpp`, `Tests/Gpu/GpuTransfer_Test.cpp`. **Not** `Engine/GpuRegionTask.cpp`, which M107 is changing.
  - approach: In the keying_grade render, 11 of 112 region transfers were staged with `importCount` 0 and no reason recorded. `resolveImports` (~755-868) has three silent exits: an unaligned or uncached range (expected); `createHostImported` failing with its `GpuStatus` discarded (~785, ~821); and a pending import forgotten mid-flight. Make `resolveImports` report a reason enum (Unaligned, ImportFailed with the VkResult/message, Forgotten, Disabled). Expose per-reason staged counters on GpuTransfer and fold them into the region report / profile JSON (`stagedReasons`) from RenderScheduler/RenderStats, without going through `GpuRegionTask.cpp`'s `notePath`. Log ImportFailed once per run (rate-limited) with its VkResult. Unaligned stays silent apart from the counter.
  - verify: a `GpuTransfer_Test` case forces each reason (an unaligned pointer, and a failing import if it can be provoked, e.g. a non-host pointer) and checks the counter. Remote CI (lavapipe) is green. P2.T6 re-runs the t5 render and records the reasons for the staged transfers. If they are ImportFailed on NVIDIA (e.g. `VK_ERROR_INVALID_EXTERNAL_HANDLE` from overlapping or still-registered pages), file a follow-up to chase `forget()` and pool reuse rather than fixing it here.
  - size: M

- [x] M91.P2.T3 — Derive the linear-LUT test tolerance from the device's subTexelPrecisionBits
  - files: `Tests/Gpu/GpuRuntimeShader_Test.cpp`
  - approach: `Lut3DLinear`/`Lut2DLinear` fail on NVIDIA with worst errors of 1.55e-4 / 1.26e-5 against `kTolerance` 1e-5. That is legal under Vulkan: filter weights need only `subTexelPrecisionBits` (8) fractional bits, and the per-sample spec bounds are 2.57e-4 / 3.12e-4. The reference uses exact fp32 weights, and lavapipe and RADV happen to match it exactly. The test-comment premise (a 1/32 grid) is also wrong: the inputs sit on a 1/256 grid. Fix the test, not the engine. Production OCIO/runtime GLSL calls `texture()` itself, and the viewer's GL_LINEAR path has the same error class. In `SetUp` read `VkPhysicalDeviceProperties::limits.subTexelPrecisionBits` via `vkGetPhysicalDeviceProperties(dev->physicalDevice(), …)`. Have the reference also return a per-element bound for Linear: per channel `strength * |tint[ch]| * 2^-bits * Σ_axes max over the cell's corners of |texel(hi_a) - texel(lo_a)|`, added to `kTolerance`. Nearest keeps `kTolerance` alone. `expectMatches` checks `d <= bound[i]` and prints the worst d/bound ratio. Rewrite the comment at ~149-151 to state the real rationale.
  - verify: `GpuRuntimeShaderTest.*` passes on the RTX 3070 Ti (both registrations) and on lavapipe CI. The worst d/bound ratio is printed and below 1. A deliberately swapped channel or axis (an error around 0.1) would still fail; check by reasoning or a temporary local edit that is not committed.
  - size: M

- [x] M91.P2.T5 — Make the pipelined-strip residency timing test assert what the design guarantees
  - files: `Tests/GpuResidency_Test.cpp`
  - approach: `RegionMembersAreTimedOnTheDevice` fails 12/12 on NVIDIA in pipelined-strips mode with a near-constant gap of about 0.07-0.09 ms (6-9%). Whole and serial strips are within 1.2%. The cause is correct device time that no member owns. Pipelined strips record the compute→transfer queue-ownership release barriers inside the timed strip interval, after the last member's timestamp (`GpuRegionTask.cpp` `submitStrip` ~1931-1975, handoff `Record` vs `Submit` ~1602/1807). NVIDIA charges real time for that release; lavapipe and RADV make it nearly free. Keep the 5% equality check for whole and serial. For pipelined, assert nesting, `EXPECT_LE(sumMs, report.kernelMs + eps)` with eps = 2 × strips × timestampPeriod, plus a sanity floor `EXPECT_GT(sumMs, 0.5 * report.kernelMs)`. Add a short why-comment about the release barriers. No engine change, which also avoids the M107 file.
  - verify: the test passes 3 times in a row on the RTX 3070 Ti and on lavapipe CI.
  - size: M

- [x] M91.P2.T6 — Gate build for the Phase 2 fixes
  - files: none in code. Output goes to `.local/m91/p2/`.
  - approach: one remote `fast --tests` on the milestone HEAD carrying every P2 fix. Rerun P1.T4 (a) and (b) and the P1.T5 checks from the new build tree and AppImage.
  - verify: every gpu-labelled suite and the engine ctest (both schedulers, `NATRON_GPU=force`) pass on the RTX 3070 Ti. The remote CI legs on lavapipe are green. Failures filed as follow-ups are listed.
  - size: M

P2.T6 result (2026-10-24, build f2cb62759, CI run 40058465161; artifacts in `.local/m91/p2/`, same recipe as P1.T4):

| Run | Total | Pass | Fail | Device |
|---|---|---|---|---|
| `ctest -L gpu` (REQUIRE_GPU, REQUIRE_GPU_HARDWARE) | 47 | 47 | 0 | RTX 3070 Ti |
| engine ctest, `NATRON_GPU=force`, legacy | 335 | 335 | 0 | RTX 3070 Ti |
| engine ctest, `NATRON_GPU=force`, taskgraph | 335 | 335 | 0 | RTX 3070 Ti |

- Lut2DLinear/Lut3DLinear pass in both registrations. `RegionMembersAreTimedOnTheDevice` passes 3/3 standalone and in both full runs. The new GpuTransfer staged-reason tests pass. `PipelinedFramesTimeline` passed in every run.
- The t5 render (keying_grade, auto, taskgraph, frames 1-6) used host-import for all 46 uploads and 46 downloads, with 0 staged and no `host import ... failed` line. The earlier 11 staged transfers did not recur, and no driver import refusal was seen.

## Phase 91.3: Viewer hand-off on NVIDIA GL (⚑ start after M107 - GPU Viewer Hand-off merges)

If M107 hasn't merged when Phase 91.2 finishes, skip this phase and file it as a follow-up milestone. The acceptance line for the viewer path is then met by `GlInterop_Test` (NVIDIA EGL) and `GlInteropQt` on `:0`, and a decision records that.

- [ ] ~~M91.P3.T0 — Surface the staged-transfer reasons in the region profile~~ (moved to a follow-up milestone: M107 not merged when Phase 91.2 finished)
  - files: `Engine/GpuRegionTask.cpp`, `Engine/RenderScheduler.h`, `Engine/RenderScheduler.cpp`, `Engine/RenderStats.h`, `Engine/RenderStats.cpp`
  - approach: P2.T2 added `GpuTransfer::stagedReasons()`, but `RegionReport` is filled only in `GpuRegionTask.cpp`, which is an M107 file. After M107 merges: snapshot `stagedReasons()` beside `importStatsBefore` (~l.353) and write the deltas into new `RegionReport` staged* fields next to `importsAtUpload` (~l.394). Copy them into `GpuRegionTimes` in `RenderScheduler.cpp` (~l.1303). In `formatRegionRecord`, write `"stagedReasons":{"disabled":N,"unaligned":N,"importFailed":N,"forgotten":N}` only when a count is nonzero, so the exact-match string in `RenderProfile_Test.cpp` (~l.336) is unchanged.
  - verify: a `RenderProfile_Test` case with a nonzero staged count emits the object. The t5 render's profile shows reasons for every staged transfer.
  - size: M

- [ ] ~~M91.P3.T1 — Let the GUI test harness run on an existing host display with that display's GL~~ (moved to a follow-up milestone: M107 not merged when Phase 91.2 finished)
  - files: `Tests/gui/run-gui-test.sh` (M107 also changes this file, hence the ⚑)
  - approach: Add a mode, `GUI_TEST_HOST_DISPLAY=:0`, that skips Xvfb, the `/usr/lib64` GL symlink directory and the `mesa`/`LIBGL_ALWAYS_SOFTWARE` exports, running the extracted AppImage on the host against the system's glvnd with `env -u LD_LIBRARY_PATH`. Fail with a clear message when the display can't be opened, or when the GL renderer isn't hardware (that check takes an opt-out). The default Xvfb mode stays byte-identical.
  - verify: `GUI_TEST_HOST_DISPLAY=:0 XAUTHORITY=… Tests/gui/run-gui-test.sh Tests/gui/<existing smoke>.py <fast AppImage>` passes and logs an NVIDIA renderer. The same script still passes under Xvfb in the container. `bash -n` and `shellcheck` pass.
  - size: M

- [ ] ~~M91.P3.T2 — Run M107's suites and UAT on NVIDIA, on every rung~~ (moved to a follow-up milestone: M107 not merged when Phase 91.2 finished)
  - files: none in code. Output goes to `.local/m91/p3/`, results in this milestone file.
  - approach: One remote `fast --tests` on M91's branch rebased onto main after M107 merged. In the NVIDIA container, run `ViewerHandoff_Test`, `ViewerRootOnDevice_Test`, `ViewerGpuProcess_Test`, `OcioVulkanShader_Test` and `GlInterop_Test`, in both scheduler modes. Run `Tests/gui/m107_viewer_handoff.py`: cpu reference first; then `NATRON_VIEWER_HANDOFF=zerocopy` and `hostsync` on `:0` (P3.T1); `readback` under Xvfb; `NATRON_GPU_FAULT=lost@40` on the hardware device. Run `tools/gpu-spike/desktop-handoff-check.sh` on `:0`. Check via `NATRON_RENDER_PROFILE` that root regions report `downloadBytes == 0` on zerocopy and hostsync.
  - verify: all suites pass on the RTX 3070 Ti. Each UAT config shows `SUMMARY 0 failure(s)`. The handoff report says zero-copy was reached on NVIDIA GL, or names what is missing and the logged fallback rung. Viewer-path failures are fixed under P2.T1's rules or filed as follow-ups.
  - size: M

## Phase 91.4: Benchmarks and documentation

- [x] M91.P4.T1 — Microbenchmarks on the RTX 3070 Ti: GpuBench, ImportCost, transfers and interop
  - files: none in code (unless `run-bench.sh`/`run-host.sh` need their RADV wording or stale image tag fixed). Output goes to `.local/m91/bench/`.
  - approach: Run `tools/gpu-spike/run-bench.sh --no-cpu --sizes uhd,8k,16k --runs 2` (24k needs ~9.2 GB host RAM this host doesn't have free). Run `ImportCost` through `run-host.sh`. Take the `GpuTransfer_Test` pipelined UHD timings (§2.1 source) and `GlInterop_Test` per-path timings (§2.5) from the P2.T6 or P3.T2 runs. Use the release preset for the gpu-spike build; run with no other load; record `nvidia-smi -q -d CLOCK,PCIE` next to the numbers.
  - verify: CSVs and summaries exist for each size. A table mirrors §2.1, §2.2, §2.3 and §2.5 with NVIDIA values, plus the host's PCIe link, CPU and RAM.
  - size: M

- [x] M91.P4.T2 — End-to-end release render on NVIDIA: placement sanity and gain over the CPU
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
- 2026-10-23 — P1.T4: tools/gpu-spike had bit-rotted (standalone grade.slang deleted in M137); the spike now carries its own copy of the old grade_main kernel staged beside the engine gradeops module (code: 2c03da4d3 on milestone/m91-nvidia-gpu-validation). GpuBench smoke on the RTX 3070 Ti ran with hostImport=1.
- 2026-10-23 — User: no Quadro P400 runs at all, not even the optional small-VRAM data point (P1.T4 and later).
- 2026-10-23 — P1.T6 triage: Phase 91.2 keeps P2.T2 (staged-reason logging), P2.T3 (spec-derived LUT tolerance, test-only) and P2.T5 (pipelined timing assertion, test-only); P2.T1 and P2.T4 were dropped as not triggered. Follow-up observed, not M91 scope: in pipelined strips, per-member times are inflated (Grade 0.21 ms vs 0.045 ms serial), which skews GpuCostModel calibration samples. It needs a follow-up milestone, to be filed by /cat-plan.
- 2026-10-23 — P2.T2 scoped down: the counters, the once-per-run ImportFailed log and the tests landed. Wiring the counts into the region profile needs `GpuRegionTask.cpp` (an M107 file), so it moved to P3.T0, after M107 merges. ImportFailed has no test because no deterministic way to make a driver reject an aligned pointer was found. The P2.T6 rerun reads the stderr log to see whether NVIDIA rejects imports.
- 2026-10-24 — P4.T1 ran at uhd and 8k only. 16k was dropped as host-capacity: the host swapped (about 5 GiB free, 26 GB of swap in use) and one cell took 294 s. CPU columns were not run because they need graph_bench. The link is PCIe gen2 x16, the platform maximum, so transfers are 4x slower than on RADV while blur kernels are 2.5-7x faster. Desktop apps hold about 4 GiB of VRAM, and the 4 GB ImportCost case hit VK_ERROR_OUT_OF_DEVICE_MEMORY (host-capacity). Tables are in `.local/m91/bench/nvidia-tables.md` and get folded into the design note by P4.T3.
- 2026-10-24 — User folded the per-host memory cap (`tools/ci/local/memcap.sh`, devshell.sh `--memory`, run-bench.sh) into M91 (code: 2177f4671 on milestone/m91-nvidia-gpu-validation): the first P4.T2 attempt swapped the host and died mid cg_multipass. User chose to discard the partial P4.T2 runs (kept in `.local/m91/bench/e2e-aborted/`) and rerun all 3 comps × 3 rounds under the cap.
- 2026-10-24 — Phase 91.3 skipped per its own rule: M107 - GPU Viewer Hand-off was still `doing` (owner `gpu`) when Phase 91.2 finished. P3.T0–T2 are struck through here and handed to /cat-plan through the inbox as a follow-up milestone. The gate's viewer-path line is met by `Gpu.GlInteropTest.*` on NVIDIA EGL (zero-copy, P2.T6) and `GlInteropQt` on `:0` (zero-copy, P1.T5).
- 2026-10-24 — P4.T2 (release AppImage from run 40060640897, all 21 renders under memcap.sh, exit 0, no OOM): median wall auto/off — keying_grade 7.3/15.1 s (2.07x), cg_multipass 8.7/15.0 s (1.72x), defocus_retime 23.9/63.9 s (2.67x). Auto is not slower than off on any comp. Every transfer was host-import, with no import failures. The RADV CPU seeds are too cheap for the FX-8350 (Grade seeded at 11.5 ms/Mpx, measured 45-50), which keeps five island Grades in cg_multipass on the CPU. That errs in the safe direction and is an optional follow-up. Details are in `.local/m91/bench/e2e/summary.md`.
