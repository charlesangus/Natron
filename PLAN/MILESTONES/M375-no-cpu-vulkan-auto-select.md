# M375 - No CPU Vulkan Auto-Select

Auto GPU device selection must never pick a CPU Vulkan device (llvmpipe/lavapipe) (user,
2026-11-08). On AVX-512 CPUs llvmpipe hangs the `dirblur.slang` kernel and zeroes
LensCorrect's sinc path, and a GPU-less machine would get it automatically. GPU-less machines
use Natron's native CPU path. `NATRON_GPU_DEVICE` can still name llvmpipe explicitly. CI must
keep testing Vulkan on lavapipe: the tests select it explicitly rather than through auto, and
CI keeps `GALLIUM_OVERRIDE_CPU_CAPS=avx` (PR #231).

Today `GpuDevice::create`'s ranked pick (`Engine/Gpu/GpuDevice.cpp` ~263) ranks
`VK_PHYSICAL_DEVICE_TYPE_CPU` last but still picks it when nothing else qualifies.
`GpuContext` in `Auto` mode already refuses a CPU-type device after creating the runtime
(`Engine/GpuContext.cpp` ~461), but `Force` mode, the test helpers and any other
`GpuRuntime::create` caller get lavapipe from the ranked pick.

## Scope

**In:**
- The ranked (no selector) pick in `GpuDevice::create` skips CPU-type devices; with only a CPU
  device it fails with a message naming it and saying to set `NATRON_GPU_DEVICE` to use it.
- An explicit selector (option or `NATRON_GPU_DEVICE`, by index or name) still picks a CPU
  device.
- GPU tests select lavapipe explicitly when no hardware device exists, so CI keeps running
  them on lavapipe.

**Out:**
- Changing `GpuContext`'s Auto/Force/Off resolution beyond what the new device pick requires.
- Filing the Mesa lavapipe AVX-512 miscompile upstream — a manual user task once a standalone
  reproducer is confirmed on an AVX-512 machine (harness in `.worktrees/m130/build/m130hang`);
  noted here, not done by this milestone.
- The OpenGL renderer pick (`OSGLContext`, llvmpipe GL) — unrelated to Vulkan device choice.

**Edge cases:**
- `NATRON_GPU=force` with no `NATRON_GPU_DEVICE` on a lavapipe-only machine: no device, so GPU
  compute is off with the new failure message. Force means "accept the explicitly selected
  device even if it is CPU-type", not "pick lavapipe for me".
- `NATRON_TEST_REQUIRE_GPU_HARDWARE=1`: never falls back to lavapipe; fails as today.

## Phase 375.1: Device pick

- [x] M375.P1.T1 — Exclude CPU-type devices from the ranked Vulkan device pick
  - files: Engine/Gpu/GpuDevice.cpp (ranked loop ~263-277, typeRank ~33), Engine/Gpu/GpuDevice.h (~58 comment), Engine/GpuContext.h (~55-58 Force comment), Tests/Gpu/GpuRuntime_Test.cpp or Tests/Native/GpuDeviceList_Test.cpp (new test)
  - approach: In the `selector.empty()` branch skip devices whose `deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU`. When no device qualifies and at least one CPU-type device was skipped, fail with `VK_ERROR_FEATURE_NOT_PRESENT` and a message like "only CPU Vulkan devices (<name>) are present; set NATRON_GPU_DEVICE to use one". The explicit-selector branch is unchanged. Update the header comments to say auto never picks a CPU device. Add a test, on hosts where lavapipe is present (skip otherwise): with `NATRON_GPU_DEVICE` unset, the ranked pick never returns a CPU-type device; with `NATRON_GPU_DEVICE=llvmpipe` it returns lavapipe. Restore the environment afterwards.
  - verify: The new test passes in natron-dev (lavapipe only: ranked pick fails with the new message, explicit selector gets lavapipe).
  - size: M

## Phase 375.2: Tests and CI select lavapipe explicitly

- [x] M375.P2.T1 — Make the GPU test helpers fall back to an explicit lavapipe selection
  - files: Tests/Gpu/GpuTestDevice.h, Tests/Gpu/GpuHarness.h (~22 comment), plus every test that calls `GpuDevice::create`/`GpuRuntime::create` directly (Tests/Gpu/GpuTransfer_Test.cpp, BlurGpu_Test.cpp, PixelWrap_Test.cpp, GpuRuntime_Test.cpp, Tests/Native/NativeGpuParity.cpp, Tests/Native/GpuDeviceList_Test.cpp — confirm with `grep -rln "GpuDevice::create\|GpuRuntime::create" Tests`)
  - approach: In the shared helper, when `NATRON_GPU_DEVICE` is unset and the ranked pick finds no hardware device, retry with an explicit CPU-device selector (the first `VK_PHYSICAL_DEVICE_TYPE_CPU` device's index) unless `NATRON_TEST_REQUIRE_GPU_HARDWARE=1`. Route the direct callers through that helper (or the same fallback) so none depends on auto picking lavapipe. Native GPU tests that go through `GpuContext` with `NATRON_GPU=force` need the same explicit device (set `GpuContextOptions::device` or `NATRON_GPU_DEVICE` in the fixture). Update the GpuHarness comment.
  - verify: In natron-dev (lavapipe only) the full GPU ctest set runs on lavapipe with the same pass/skip counts as on main — no suite newly skipped; with `NATRON_TEST_REQUIRE_GPU_HARDWARE=1` they fail as before.
  - size: M
- [ ] M375.P2.T2 — Keep CI's Vulkan test legs on lavapipe
  - files: .github/workflows/ci.yml (~261), .github/workflows/remote-build.yml (~299-302)
  - approach: Confirm the GPU test legs still run on lavapipe after P2.T1 (they rely on `NATRON_TEST_REQUIRE_GPU=1`, which now needs the explicit fallback to find a device). If any leg runs Natron itself (not a gtest fixture) expecting GPU compute, set `NATRON_GPU_DEVICE=llvmpipe` in its env. Keep `GALLIUM_OVERRIDE_CPU_CAPS=avx` wherever PR #231 set it (check main if it isn't on this branch).
  - verify: Dispatched ci.yml run shows the GPU test legs executing (not skipping) on lavapipe and green.
  - size: M

**Verification gate:** build and ctest green in natron-dev with GPU tests running on lavapipe (no newly skipped suites); PR CI green with GPU legs executing on lavapipe; a launch of Natron in natron-dev with `NATRON_GPU=force` and no `NATRON_GPU_DEVICE` logs the new "only CPU Vulkan devices" message and renders on the CPU path.
