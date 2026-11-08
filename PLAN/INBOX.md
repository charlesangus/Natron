## Pending


### 2026-11-08T23:10:00Z — change-request
- refs: Engine/Gpu/GpuDevice.cpp:239-266, Engine/Gpu/GpuDevice.cpp:448-462
Auto GPU device selection must never pick a CPU Vulkan device (llvmpipe/lavapipe) (user, 2026-11-08). On AVX-512 CPUs llvmpipe hangs the dirblur.slang kernel and zeroes LensCorrect's sinc path; a GPU-less machine would get it automatically. GPU-less machines use Natron's native CPU path; NATRON_GPU_DEVICE can still name llvmpipe explicitly. CI must keep testing Vulkan on lavapipe (user): the tests select it explicitly (env or test fixture) rather than through auto, and CI keeps GALLIUM_OVERRIDE_CPU_CAPS=avx (PR #231). Also file the lavapipe AVX-512 miscompile upstream with Mesa once a standalone reproducer is confirmed on an AVX-512 machine (harness in .worktrees/m130/build/m130hang).
