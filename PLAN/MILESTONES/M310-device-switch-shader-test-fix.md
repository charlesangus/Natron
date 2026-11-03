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

- [ ] M310.P1.T1 — Assert per-device rejection and per-device compiles instead of the global glslang counter
  - files: `Tests/Gpu/CacheStressGpu_Test.cpp`
  - approach: in `DeviceSwitchRetriesRejectedShader`, drop the expectation that `gpu::glslCompileCountForTesting()` rises on the second device. Instead assert that `gate.isRejected(second, shader)` holds after the retry, and that the device-B pipeline cache's `pipelines().stats_.compiles` (`Engine/Gpu/GpuPipelineCache.cpp:332`) records the attempt. Read the current test and those APIs before editing; line numbers may have moved. Leave `compileGlsl`'s process-global memo alone. Keep the single-device skip at lines 792-796.
  - verify: the test passes on a host with two Vulkan devices (RADV + lavapipe, via the gpu host's RADV gate) and still skips its switch half under lavapipe alone; the rest of `CacheStressGpuTest` passes.
  - size: M

**Verification gate:** `CacheStressGpuTest` passes in PR CI and on a two-device Vulkan host (gpu PM's RADV gate).
