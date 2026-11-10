# M379 - GpuContext Test Segfault

`GpuContextTest` (`Tests/GpuContext_Test.cpp`) segfaulted once, after 5.15 s, on one scheduler leg of M331 - Deferred Pattern Matching's release gate (run 42709980437). It passed on retry and on the other scheduler. Find the root cause and fix it.

Evidence (job 129553888682, "ctest (legacy)", `ctest -j 4`, llvmpipe LLVM 21.1.8 / Mesa 25.2.7): `GpuContextTest.LostRuntimeIsRecreatedOnceNothingHoldsIt` crashed with "faulty address is 0x27ffa16 from 0x50": the instruction at 0x27ffa16 in the `Tests` binary read field +0x50 of a null pointer, on a thread started with `std::thread` (frames libstdc++ → libc clone). The fault is in our code (or VMA compiled into it), not in `libvulkan_lvp.so`. "GPU compute ready" never printed, so the crash happened during that context's start-up or on a thread left over from the previous test's teardown. Candidate threads: `GpuContext::initialize` (`Engine/GpuContext.cpp` ~137, ~478-582), the `CopyPool` workers and `drainThread` (`Engine/Gpu/GpuTransfer.cpp`), `GpuImportPreparer` and `GpuKeepAlive` (`Engine/Gpu/GpuRuntime.cpp`), and the `HostImageMemory` reaper calling the GPU free hook (`Engine/HostImageMemory.cpp` ~166-224). No core dump exists (the runner uses apport).

## Phase 379.1: Reproduce and locate

- [x] M379.P1.T1 — Symbolise the recorded crash from run 42709980437
  - files: none (findings go in `## Decisions`)
  - approach: `gh run download 42709980437 -n build-tree-release` (2.2 GB) and keep only `build/release/Tests/Tests` (non-PIE, has debug info). Run `addr2line -f -C -i -e Tests 0x27ffa16` and `objdump -dC --start-address=0x27ff9e0 --stop-address=0x27ffa40` to find which pointer's field +0x50 was read. Blob-storage downloads can be blocked; retry later if so.
  - verify: the notes name the function, file:line and the null pointer, and which of the candidate threads it is.
  - size: S
- [x] M379.P1.T2 — Add a mode that runs one test many times under load
  - files: `tools/ci/local/test.sh`, `tools/ci/remote/build.sh`, `.github/workflows/remote-build.yml`
  - approach: add `test.sh repeat <ctest-name> <N> [--parallel P]`: take the test's command from `ctest --show-only=json-v1`, run it N times, P at a time (default 4), while `ctest -j 4 -E <name>` runs in the background as load; stop at the first crash and keep its output (the built-in SIGSEGV handler's instruction pointer is enough with `addr2line`; an optional `--gdb` wraps each run in `gdb -batch -ex run -ex 'thread apply all bt'`). Add `build.sh --repeat-test <name>:<N>` (implies `--tests`), passed through a new workflow input to the ctest legs, and upload the log. M377 - Readable CI Names is renaming jobs and steps in the same workflow; keep this to a new input and one step so the rebase stays small.
  - verify: a remote fast run with `--repeat-test GpuContextModeTest:5` passes and its log shows 5 runs beside the load; a deliberately failing repeat fails the leg.
  - size: M
- [ ] ~~M379.P1.T3 — Run GpuContextTest repeatedly until it crashes (PM, no code)~~ (cancelled: P1.T1 found the site)
  - files: none
  - approach: a remote release run with `--repeat-test GpuContextTest:200`.
  - verify: a crash is captured and agrees with T1, or 200 clean runs are recorded as "not reproduced" and Phase 2 proceeds on T1 alone.
  - size: S

## Phase 379.2: Fix

- [x] M379.P2.T1 — Fix the crash site found in Phase 1
  - files: whichever Phase 1 points at (`Engine/GpuContext.cpp`, `Engine/Gpu/GpuRuntime.cpp`, `Engine/Gpu/GpuTransfer.cpp` or `Engine/HostImageMemory.cpp`), `Tests/GpuContext_Test.cpp`. Not `Engine/Gpu/GpuDevice.cpp`'s device pick, `Tests/Gpu/GpuTestDevice.h` or `Tests/Gpu/GpuHarness.h` (M375 - No CPU Vulkan Auto-Select).
  - approach: our code: fix the order in which the thread starts or stops relative to the object it reads (join or stop it before the pointer is cleared, or publish the pointer before it starts). Add a regression case to `Tests/GpuContext_Test.cpp` that creates and destroys contexts in a loop with `injectFault(DeviceLost)` and `recoverIfLost()`, aimed at the window the trace shows. Lavapipe: a minimal raw-Vulkan repro under `build/m379/`, a Mesa issue, and the narrowest workaround, citing the issue.
  - verify: the new case passes; `--repeat-test GpuContextTest:50` passes remotely.
  - size: M

**Verification gate:** a remote release run with `--tests --repeat-test GpuContextTest:50`: both ctest legs green plus 50 repeats under load. PR CI green (debug asserts on). `## Decisions` carries the stack trace from P1.T1/T3.

## Decisions

- 2026-11-10 — Elaborated at claim (consultant): the crash is in our binary on a `std::thread`, not in lavapipe, so Phase 1 symbolises the recorded address before building any repro.
- 2026-11-10 — P1.T1 result: the fault is `Engine/GpuContext.cpp:549-550` in `GpuContext::initialize()` on the context's own init thread: after `shared.runtime = std::move(runtime); finish(true);` it builds the "GPU compute ready" `qInfo` from `shared.runtime->device().info()` (`driverVersion`, `selectionReason`). `finish(true)` has already woken the test, whose `recoverIfLost()` → `GpuRuntime::recreate()` → `teardown()` resets `device_`, so the init thread reads a null `GpuDevice` (mov 0x1c0 → null, then +0x50). Our race, not lavapipe. P1.T3 is dropped; P1.T2's repeat mode is kept for the gate's 50 repeats.
