# M280 - Utility Renderer GPU Path

Run Natron Utility's wavefront on the GPU: the engine's single Vulkan `GpuDevice` gains
optional acceleration-structure and ray-query support, the intersection stage becomes a
SPIR-V ray-query kernel, and the shared Slang ray-generation and shading kernels from M279 -
Utility Renderer Foundation run as SPIR-V. A `Device` render setting chooses Auto / GPU / CPU;
GPUs without ray-tracing hardware fall back to the Embree path. Output stays resident on the
GPU for downstream GPU nodes and the viewer.

## Scope

**In:**
- `GpuDevice` probes and enables `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations` and buffer device address when present; a capability flag says whether ray tracing is usable. Devices without them keep working for every other kernel.
- GPU tracer: per-geometry BLAS (build, refit on dirty points, compaction), instance TLAS rebuild per scene version, an intersection kernel in Slang using `RayQuery` (SPIR-V target only), and a GPU occlusion query for later shadow rays.
- GPU wavefront executor: RayGen → intersect → ShadeFlat as SPIR-V dispatches through `GpuRecordContext`, buffers from `GpuBufferPool`.
- `Device` render setting (`Auto` default, `GPU`, `CPU`) exposed as an `HdRenderSettingDescriptor`, so M253 - Delegate Render Settings makes it a USDRender knob.
- Resident output: USDRender's `renderGpu` takes the delegate's GPU render buffers directly; CPU consumers still `Map()`. The 3D viewer shows the GPU frame through the existing Vulkan→GL interop (`Engine/Gpu/GlInterop`).

**Out:**
- A compute-shader BVH for GPUs without ray-tracing hardware — none (user, 2026-11-01).
- Materials, lights, sampling, subdivision — M281, M283, M284.
- Multi-GPU — none.

**Edge cases:**
- Auto on a device without ray query: CPU path, no warning (the node info panel says which device rendered).
- Forced GPU on a device without ray query: node error naming the missing extension (user, 2026-11-01).
- Auto, scene does not fit in GPU memory (BLAS/TLAS or buffers fail to allocate): that frame re-renders on CPU and the node shows a warning with the shortfall; forced GPU makes it a node error (user, 2026-11-01).
- `NATRON_GPU=off` or Free GPU Memory: the delegate drops its GPU structures; the next render rebuilds them (or goes CPU when GPU is off).
- lavapipe in CI: ray query is advertised since Mesa 24.1 (image has 25.2.7) but with documented gaps; a failing feature on lavapipe is skipped in tests only with the gap named, never silently.

**Assumptions:**
- lavapipe in the natron-dev and CI images exposes `VK_KHR_ray_query` and acceleration structures (Mesa 25.2.7 > 24.1). Probe it in M280.P1.T1 before relying on it.
- Slang's `RayQuery` compiles to SPIR-V through `SPV_KHR_ray_query` with the fork's offline `slang_add_kernel` path and `spirv_1_5` profile (may need `spirv_1_6` or a capability flag).
- CPU/GPU parity is tolerance-based: hit results differ in the last bits (memory: ulp diffs never reopen the architecture).

## Phase 280.1: Device support

- [x] M280.P1.T1 — Ray-tracing capability in GpuDevice
  - files: `Engine/Gpu/GpuDevice.{h,cpp}`, `Engine/Gpu/Vma.cpp`, `Tests/Gpu/GpuDeviceRayTracing_Test.cpp`
  - approach: query `VkPhysicalDeviceAccelerationStructureFeaturesKHR`, `VkPhysicalDeviceRayQueryFeaturesKHR` and `bufferDeviceAddress` in the feature chain; enable them only when all are present, plus the VMA buffer-device-address flag. Expose `GpuDevice::supportsRayQuery()`. No behaviour change for devices without them.
  - verify: gtest on the CI/lavapipe device reports the capability (record what lavapipe advertises in the test output); existing Gpu tests pass unchanged; forcing the capability off via an env override is honoured.
  - size: M

- [x] M280.P1.T2 — Acceleration structure wrapper
  - files: `Engine/Gpu/GpuAccel.{h,cpp}` (new), `Engine/Gpu/CMakeLists.txt`, `Tests/Gpu/GpuAccel_Test.cpp`
  - approach: thin RAII types for BLAS (triangle geometry from vertex/index `GpuBuffer`s, build, refit with `ALLOW_UPDATE`, compaction) and TLAS (instance buffer with transform and custom index), with scratch from `GpuBufferPool` and a size query before allocation so out-of-memory is reported, not crashed. Natron-agnostic: no Hydra types.
  - verify: gtest builds a BLAS for one quad and a TLAS with two instances on lavapipe; a size query larger than the device budget returns an out-of-memory status.
  - size: L

## Phase 280.2: GPU tracer and executor

- [x] M280.P2.T1 — GPU tracer behind the Tracer interface
  - files: `Engine/Nodes/Scene/Utility/GpuTracer.{h,cpp}`, `Engine/Nodes/Scene/Utility/Kernels/Intersect.slang`, `Engine/Nodes/Scene/Utility/CMakeLists.txt`, `Tests/Scene/NatronUtilityTracer_Test.cpp`
  - approach: implement M279's `Tracer` on `GpuAccel`: upload dirty geometry, refit or rebuild BLAS per dirty bit, rebuild TLAS on scene version change. `Intersect.slang` (SPIR-V only, no CPP twin) reads the ray batch, runs `RayQuery` and writes the same hit layout the Embree tracer produces; an occlusion entry point answers shadow batches.
  - verify: the tracer test suite runs against both backends (parameterised); hit t within 1e-5 relative, ids identical except on edge-grazing rays, which the test excludes by construction.
  - size: L

- [x] M280.P2.T2 — GPU wavefront executor and device choice
  - files: `Engine/Nodes/Scene/Utility/WavefrontGpu.{h,cpp}`, `Engine/Nodes/Scene/Utility/UtilityRenderPass.cpp`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.cpp`, `Tests/Scene/NatronUtility_Test.cpp`
  - approach: dispatch the SPIR-V RayGen, Intersect and ShadeFlat kernels through `GpuRecordContext`. The render pass picks the executor from the `Device` setting (descriptor registered in the delegate) and `supportsRayQuery()`; on an allocation failure in Auto it re-runs the frame on the CPU executor and posts a warning through the delegate's render-stat dictionary for the node to show.
  - verify: gtest — the red-cube render matches between CPU and GPU executors within tolerance; Device=GPU with the capability forced off fails with the extension named; an injected allocation failure in Auto renders on CPU and reports the warning.
  - size: L

## Phase 280.3: Resident output

- [x] M280.P3.T1 — USDRender GPU output from Natron Utility buffers
  - files: `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}`, `Engine/Nodes/Scene/Utility/UtilityRenderBuffer.{h,cpp}`, `Tests/Scene/USDRender_Test.cpp`
  - approach: `UtilityRenderBuffer::GetResource` returns a handle to its `GpuBuffer` when GPU-resident; HydraRenderImpl passes it to USDRender's `renderGpu`, which copies or aliases it into the output region without a host round-trip. `Map()` downloads lazily for CPU consumers. Readback stays the path for every other delegate.
  - verify: gtest — a USDRender (Natron Utility, GPU) → GPU Grade chain records zero host downloads (transfer counter); the CPU-consumer path still returns identical pixels.
  - size: L

- [x] M280.P3.T2 — Viewer shows the GPU frame through GL interop
  - files: `Gui/SceneViewport.cpp`, `Engine/Gpu/GlInterop.{h,cpp}`, `Tests/gui/natron_utility_viewer.py`
  - approach: when the viewer's delegate is Natron Utility on GPU, import its colour buffer through the existing Vulkan→GL interop with semaphores instead of uploading a mapped CPU buffer; fall back to the upload when interop is unavailable.
  - verify: the M279 viewer GUI script, extended to force GPU, passes under the strict Xvfb runner and its screenshot matches the CPU one; screenshot shared with the user.
  - size: M

**Verification gate:** full build and ctest green on lavapipe (tracer parity, executor parity, residency counters); the viewer GUI script passes with GPU forced; a manual AMD and NVIDIA check (Auto picks GPU, a frame renders) reported by the user.

## Decisions

- 2026-11-01 — **Auto plus override (user):** a `Device: Auto / GPU / CPU` setting; forced GPU where unavailable is an error.
- 2026-11-01 — **GPU out-of-memory falls back to CPU in Auto, with a warning (user).**
- 2026-11-01 — **Resident output in this milestone (user):** matches 2026-10-10-gpu-residency-from-day-one.
- 2026-11-01 — **No compute-shader BVH (user):** GPUs without ray-tracing hardware use the CPU path.
- 2026-11-12 — P1.T1 implemented (fd227125d); F7 took main (0c83f0148, one HydraRenderImpl conflict: main's publish/finished rule kept with the Utility delegate's whole-frame rule on top) and is merged into this branch. lavapipe's driver library names all three extensions; whether it sets the feature bits prints on the first test run.
- 2026-11-13 — P2.T2 (e95fc6253), the Device menu and node warnings (b2a644320), and P3.T1 (7bbb2f936) implemented. Auto also falls back to the CPU on a lost device (PM call, as for out-of-memory). The device that rendered shows as an info persistent message on USDRender. The colour film stays on the device while depth, primId and instance id still read back per batch. P3.T2 and the once-per-frame strip fix are in progress.
- 2026-11-13 — Gate green (run 43419744078 on 6f19477d5): build, both ctest legs and `natron_utility_viewer.py` 84/84. The stripped-region test now caps the strip buffer so the region really strips. PR #270 is open into F7 and under review. Screenshot sign-off and the AMD/NVIDIA check go to F7's UAT.
- 2026-11-13 — **Handoff (session restart):** the PR #270 review fixes are local WIP commit 42bba1830 on the branch, also pushed as `wip/m280-review-fixes` and not on the PR branch yet. It does not build yet.
  - Done: findings 3–7, 9, 11–14.
  - Partial:
    - #1: the film keeps its hold, because `GpuRuntime::recreate` invalidates handles. Loss releasers (`GpuContext::addLossReleaser`) release films, interop and the delegate instead.
    - #2: ctest left.
    - #8: rewrite `USDRender::canRenderOnGpu` (~938) to be non-blocking and use `HydraRender::deviceFrameFellBack(owner, gpu->generation(), utilityGpuBudget())`.
    - #15: `USDRender_Test.cpp:1292/1298` and `NatronUtility_Test.cpp:853/859` still call the removed hook; move them to the `swapGpuContextForTests` pattern.
  - Not started: #10, the strict interop requirement in `natron_utility_viewer.py`, which must not turn lavapipe CI red.
  - Then: un-WIP the commit message, syntax-check, gate, re-review.
- 2026-11-13 — PR #270 review fixes finished (b3380b12b): finding #2 exposed Free GPU Memory emptying the pool before the Utility engine returned its buffers, now fixed; #10's viewer check requires the GL hand-off only where one can be built. Gating with `natron_utility_viewer.py`.
- 2026-11-13 — Gate green on b3380b12b (run 43439305717, with `natron_utility_viewer.py`). Round 1 closed on PR #270; review round 2 (final) running.
- 2026-11-13 — Review round 2 (final): round 1 11 fixed, 4 partly. 7 new findings: one high (test GPU-context swaps can free a context the Utility engine still uses); the 3D viewer's Utility engine escapes loss releasers and Free GPU Memory; the interop GUI check could pass on interop-capable hardware. All being fixed, then the gate decides.
- 2026-11-13 — Round 2 fixes (071eb7f5a); round 2 closed on the PR. Final gate running with `natron_utility_viewer.py`; merges into F7 if green.
- 2026-11-13 — Final gate on 071eb7f5a: all green including `natron_utility_viewer.py`, except `AnAutoFallbackDoesNotRefuseAForcedGpuRender`. Real bug: an Auto fallback inside a CPU-path render (after the scheduler's GPU task can't fit) was never recorded, so `canRenderOnGpu` kept answering true. Now recorded from the shared stats path (b518cef5b); ctest re-running.
