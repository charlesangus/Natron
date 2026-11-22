# M509 - Cancel-Aware GPU Fence Wait

`GpuRecordContext::submitFenced` (`Engine/Gpu/GpuRecord.cpp:1254`) blocks in one `vkWaitForFences(timeoutNs)` (:1276), 30 s by default. On timeout its only exit is `abandonWork` → `GpuDevice::abandonInFlightWork`, which declares the device lost. `submitAndWait(timeoutNs, stop)` (:1215) already checks `stop` between split batches (`runBatchesBeforeLast`, :1331) and during semaphore waits (`waitBeforeBatches`, :1297). It does not check it during any fence wait, and `RenderScheduler.cpp:1507` and `GpuRegionTask.cpp:3359` pass no stop at all. Make the fence wait cancel-aware: on stop, abandon the batch and return at once. Whatever the batch uses must stay alive until its fence signals, and the device must stay usable.

## Scope

**In:**
- A fence wait that polls in `kStopPollNs` slices and checks `stop`.
- A device-level list that keeps an abandoned batch's resources until its fence signals, then frees them.
- Holding the caller-bound buffers, so the pool cannot reuse them while the batch runs.
- Passing a stop at every `submitAndWait` caller that renders.

**Out:**
- Changing the 30 s timeout or the device-lost path; those stay as they are.
- Preempting work already on the GPU. Vulkan cannot do it; later submits on the queue simply queue behind the abandoned batch.

**Edge cases:**
- Stop and fence signal at the same time: if the fence has signalled, take the normal path, so the result is kept.
- Device lost while a batch is retiring: the list moves its holdings to `parkForever`, as `abandonInFlightWork` does for in-flight work.
- An app exit with batches retiring: `drainQueues` reaps them after the wait.

**Assumptions:**
- Fence waits are where an aborted render currently blocks (see `Tests/gui/m364_cancel_responsiveness.py`).

## Phase 509.1: Design

- [ ] M509.P1.T1 — How an abandoned batch keeps what it uses alive
  - files: read-only: `Engine/Gpu/GpuRecord.cpp`/`.h` (`parkInFlightWork`, `held_`, `heldObjects_`, `hold()`), `Engine/Gpu/GpuDevice.cpp`/`.h` (:233-244), `Engine/Gpu/GpuBufferPool.cpp`, `Engine/Gpu/GpuBuffer.cpp:60`, `Engine/GpuRegionTask.cpp` (~:1727)
  - approach: consultant (opus) lists everything a submitted batch touches: context-owned resources, the command pool and fence, and the buffers and textures the caller bound and will release when the call returns. It then picks how the caller-bound ones are held: the context captures a shared reference at bind time, or the buffer pool quarantines handles until a fence signals. It names where retired batches are reaped (each `GpuDevice::submit`, `drainQueues`, destruction) and records the choice with its trade-offs as a milestone decision.
  - verify: a decision naming the mechanism, every resource class it covers, and the reap points
  - size: L

## Phase 509.2: Implement

- [ ] M509.P2.T1 — Device list of batches retiring on their fence
  - files: `Engine/Gpu/GpuDevice.h`, `Engine/Gpu/GpuDevice.cpp`, `Tests/Gpu/GpuDevice_Test.cpp`
  - approach: add `GpuDevice::retireWhenSignalled(VkFence, std::shared_ptr<void> holdings)`. `vkGetFenceStatus` reaps finished entries at the P1.T1 points and destroys the fence. On `abandonInFlightWork` the list moves to `parkForever`. Test it with an unsignalled fence (the holdings stay alive) and then a signalled one (released after a reap). Bundled gtest is old: no `GTEST_SKIP`, use `INSTANTIATE_TEST_CASE_P`, no `ASSERT_TRUE(ptr)`.
  - verify: `ctest -R GpuDevice` passes at the phase build
  - size: M
- [ ] M509.P2.T2 — Poll the fence and abandon the batch on stop
  - files: `Engine/Gpu/GpuRecord.cpp`, `Engine/Gpu/GpuRecord.h`, `Tests/Gpu/` (new `GpuRecordCancel_Test.cpp` or the existing record test file)
  - approach: `submitFenced` takes `stop` and waits in `kStopPollNs` slices. On stop it moves the context's holdings, command pool and fence into `retireWhenSignalled` and returns `stopped()` without touching the lost-device latch. Thread `stop` through `runBatchesBeforeLast` and the final submit. Update the header comment (:263-268). For the test, `waitExternalSemaphore` on a binary semaphore nobody signals keeps the fence pending deterministically. Stop after 50 ms and expect a return within 0.5 s, `GpuStatus` stopped, and `!device.hasAbandonedWork()`. Then signal the semaphore with an empty submit and check the holdings are released after a reap. Same gtest limits apply.
  - verify: `ctest -R "GpuRecord|GpuDevice"` passes at the phase build
  - size: L
- [ ] M509.P2.T3 — Hold caller-bound buffers until the fence signals
  - files: per P1.T1, likely `Engine/Gpu/GpuRecord.cpp` (bind paths) or `Engine/Gpu/GpuBufferPool.cpp`
  - approach: implement the P1.T1 choice, so that a buffer the caller releases after a stopped `submitAndWait` goes back to the pool only once the batch's fence has signalled. Extend the P2.T2 test: release the bound output buffer after the stop, and check the pool does not hand it out again before the semaphore is signalled.
  - verify: `ctest -R "GpuRecord|GpuDevice|GpuBufferPool"` passes at the phase build
  - size: M
- [ ] M509.P2.T4 — Pass the render's stop at every submitAndWait caller
  - files: `Engine/RenderScheduler.cpp` (:1507), `Engine/GpuRegionTask.cpp` (:3359; check :1727)
  - approach: pass the abort check (`stopWhenAborted()` in `GpuRegionTask`, the equivalent from the render's `AbortableRenderInfo` in `RenderScheduler`), so a stopped wait takes the existing aborted-render return, not the failed-GPU path.
  - verify: phase build; `ctest -R "Gpu|RenderScheduler"` passes
  - size: M

## Phase 509.3: Gate (PM runs)

- [ ] M509.P3.T1 — Cancel responsiveness under Xvfb
  - files: none
  - approach: run `Tests/gui/m364_cancel_responsiveness.py` under Xvfb on the `fast` build with GPU placement on, plus the GPU ctest subset
  - verify: the script passes, and no "treated as lost" line appears in the log
  - size: S

**Verification gate:** P3.T1 green; `ctest -R Gpu` passes; full debug PR CI green.
