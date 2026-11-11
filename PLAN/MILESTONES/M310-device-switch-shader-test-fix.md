# M310 - Device-Switch Shader Test Fix

`CacheStressGpuTest.DeviceSwitchRetriesRejectedShader` fails 3/3 at
`Tests/Gpu/CacheStressGpu_Test.cpp:800` on any host with two or more Vulkan devices (found by
the gpu PM's RADV gate for M240 - Float Trap And Denormal Audit: RADV R9700 + lavapipe). The
test expects `gpu::glslCompileCountForTesting()` to rise by 1 when the rejected shader is
retried on the second device, but `compileGlsl` (`Engine/Gpu/GpuShaderCompiler.cpp:122-177`)
memoises results, failures included, in a process-global map keyed by stage/entry/source with
no device, so the retry is a cache hit and the counter stays flat. Per-device rejection is
still recorded (`Engine/Nodes/Image/StagePlan.cpp:1667`). This is a wrong test assumption, not
a production bug. PR CI's lavapipe-only runner skips the switch half (lines 792-796), which is
why it never showed there.

## Phase 310.1: Test

- [x] M310.P1.T1 — Assert per-device rejection and per-device compiles instead of the global glslang counter
  - files: `Tests/Gpu/CacheStressGpu_Test.cpp`
  - approach: in `DeviceSwitchRetriesRejectedShader`, drop the expectation that `gpu::glslCompileCountForTesting()` rises on the second device. Instead assert that `gate.isRejected(second, shader)` holds after the retry, and that the device-B pipeline cache's `pipelines().stats_.compiles` (`Engine/Gpu/GpuPipelineCache.cpp:332`) records the attempt. Read the current test and those APIs before editing; line numbers may have moved. Leave `compileGlsl`'s process-global memo alone. Keep the single-device skip at lines 792-796.
  - verify: the test passes on a host with two Vulkan devices (RADV + lavapipe, via the gpu host's RADV gate) and still skips its switch half under lavapipe alone; the rest of `CacheStressGpuTest` passes.
  - size: M

**Verification gate:** `CacheStressGpuTest` passes in PR CI and on a two-device Vulkan host (gpu PM's RADV gate).
- 2026-11-10 — PR #252 is open. Release run 42803460920 is green, after a re-run for an unrelated ProgressiveRenderTest flake. The review round closed with no blocking findings. The merge waits on the two-device half passing on the GPU host's RADV gate, run by the GPU-host PM or the user: `CacheStressGpuTest` on RADV plus lavapipe at milestone/m310-device-switch-shader-test-fix.
- 2026-11-11 — The two-device check is now an `m310` step in the GPU-host session. It runs CacheStressGpuTest from run 42803460920's prebuilt Tests binary with OFX plug-ins, packaged as m310-cachestress.tar.gz (790 MB), in natron-dev-gpu with RADV and lavapipe visible. A local dry run on lavapipe passed 6/6 with the switch half skipped, as expected. Merge when the host log shows the switch half ran and passed.
- 2026-11-11 — The first GPU-host m310 run failed before testing: the existing natron-dev-gpu container mounted another checkout. run-bench.sh now uses natron-dev-gpu-bench, with a mount guard. m310 needs a rerun.
