# M109 - GPU Region Engine Hardening

M85's benches show where the region engine loses time on real comps. Three items are the largest: exposed transfer in unstripped regions (59–84% of region time), plain Transform's unexplained "other" time, and a rise in exposed transfer at 24k. M85 also left one correctness bug, a region whose member had an empty input slot hung, and several measurement gaps. This milestone first breaks region time down to the phase, using device timestamps and host-phase timers, and finds the root causes nobody has explained yet. It then fixes them in order of measured impact, followed by the cheaper ladder and budget items from M84. Tasks that need a file another running milestone owns come last and are flagged.

## Scope

**In:**
- Per-member GPU time from timestamps, plus a host-phase breakdown in the profile's region record.
- Diagnosing and fixing: Transform "other" time, the 24k rise in exposed transfer, the region `kernelMs` reading 1.7–3× the isolated kernel time, and the empty-input hang.
- Overlapping transfer in regions that fit whole, by stripping them for the pipeline, and charging placement for the transfer it really exposes.
- Importing download destinations on a helper thread, with `resolveImports` doing the pinning outside `importMutex`.
- Giving a CPU fallback the normal thread budget.
- An idle trim for `HostImageMemory`.
- Remembering a strip height per region shape, and waiting on a contending region before halving strips.
- Blur's narrow vertical pass.
- Flagged, last: one edge per input in `collectPreRenderFrames`, and the `GpuTaskTraits` seed detail.

**Out:**
- Multi-plane tasks as placement candidates → **M110 - Remaining Native GPU Ports**. It needs a multi-plane `GpuQuery`/`GpuRenderArgs` in `Engine/Nodes/Image/GpuRender.h`, which M126 - Runtime GPU Shaders edits, and the Shuffle-family nodes in M110 are its only consumer.
- A Preferences knob for `HostImageMemory` → none for now. M109 trims to a fixed floor.
- Device-side (`GpuBufferPool`) idle trim → not requested.
- Faster EXR Write at 16k/24k → not GPU work, no owner.
- New Slang entry points → none. Adding one would edit `Engine/Gpu/CMakeLists.txt`, which M126 - Runtime GPU Shaders owns.

**Edge cases:**
- Strip-for-overlap must not strip a region with an output that another region reads on the device. Stripping downloads every output, so residency would be lost.
- Strip-for-overlap on a device without push descriptors or without a separate transfer family must stay whole (`pipelineAllowed()`).
- Halos: a spatial member whose halo is large next to the strip height must stay whole.
- Idle trim must never free a buffer that is live or imported and in flight. It runs through `trimTo`, so the `GpuContext` free hook drops imports first.
- A helper-thread import racing a `forget()` from the free hook.
- Strip memory must be forgotten on device loss or a runtime rebuild.
- The fallback budget must not oversubscribe the pool while other CPU tasks run.

**Assumptions:**
- Benches use the stripped release AppImage on the target host with `NATRON_RENDER_SCHEDULER=taskgraph`, Auto against Off, interleaved, comparing only pairs from the same boot. Each case probes `MemAvailable` first, and the EXR write-back stays out of the timed part. Run a quick directional pass first and the thorough pass at the end.
- GPU tests run on lavapipe and RADV, in both scheduler modes and with no ICD. Builds are remote (`tools/ci/remote/build.sh`), one per phase.
- Tasks in one phase that share `Engine/GpuRegionTask.cpp` run one after another, not in parallel.

**Scouted facts** (main `7ad45061e`):
- **Fallback thread budget:** a region task gets budget 1 (`Engine/RenderScheduler.cpp:1043-1048`). `runGpuRegion` ignores its `budget` argument (`Engine/GpuRegionTask.cpp:2126`), and `renderMembersOnCpu` (`:122`) runs inside that budget-1 scope (`RenderScheduler.cpp:1217-1218`).
- **Retry ladder:** evict → serial → halve is per call, and nothing is remembered between calls (`GpuRegionTask.cpp:2156-2236`).
- **Region timing:**
  - `wallMs` starts after `plan`/`resolveInputs`/`planStrips` (`:333`).
  - For an unstripped region, exposed transfer is the upload plus download host intervals (`:359-362`).
  - M85's summary computed "other" as wall − kernel − exposed. "Other" therefore contains `renderGpu` recording, `acquireBuffer` and the `submitAndWait` latency beyond device time.
- **`memberNs`** is device time split by cost-model weight (`:641-642`, `:968-971`, `:1947-1953`), and `noteSuccess` feeds it to `observeGpu` (`:2025`).
- **Empty input slot:** in the unstripped path it stays an empty view (`:1086-1090`). The strip path skips it (`:1442`).
- **Import mutex:** `GpuTransfer::Impl::resolveImports` holds `importMutex` across `createHostImported`, which does the pinning (`Engine/Gpu/GpuTransfer.cpp:680-707`).
- **`HostImageMemory` pool:** capped at max(512 MB, RAM/4) (`Engine/HostImageMemory.cpp:58,66-71`). It shrinks only through the Cache's `trimTo`, `trim()` or a failed allocation.
- **Placement:**
  - It requires exactly one plane (`Engine/GpuPlacement.cpp:484`).
  - The gain already subtracts calibrated transfer (`Engine/GpuCostModel.cpp:432-474`).
  - `traitsOf` is at `:235-262`.
- **`collectPreRenderFrames`** keys its map by input effect (`Engine/ParallelRenderArgs.cpp:123,166`). `GpuRegionTask.cpp:597-615` compensates for this.
- **Strip planner:** `GpuStripLimits::forcedRows` already forces strips on a region that fits (`Engine/GpuRegionStrips.h:103`). Pipelining needs push descriptors and a transfer family of its own (`GpuRegionTask.cpp:1611-1628`).
- **M85's bench drivers** (`.local/m85/batchF/bench5/{gb.sh,cases.sh,summ.py}`) are not committed.

## Phase 109.1: Instrumentation

- [x] M109.P1.T1 — Time each member with device timestamps and break region wall time into host phases
  - files: `Engine/GpuRegionTask.cpp`, `Engine/RenderScheduler.h` (`RegionReport`), `Engine/RenderScheduler.cpp` (`executeRegion` copies the fields), `Engine/RenderStats.{h,cpp}` (`GpuRegionTimes`, `formatRegionRecord`), `Tests/GpuResidency_Test.cpp`
  - approach:
    - Write a timestamp after each non-identity member, in both the unstripped and the strip paths: one query pair per member per strip, on the existing per-segment and per-strip pools. Set `memberNs` from those timestamps.
    - Keep the weight split only as a fallback for a device without timestamps. `observeGpu` then learns from measured time.
    - Add host-phase fields to the report and the region record:
      - `recordMs`: time inside `renderGpu` plus `acquireBuffer`;
      - `submitWaitMs`: host time in `submitAndWait`, minus device time;
      - `acquireMs`, and `poolMisses`, which counts fresh `vkAllocateMemory` calls;
      - `importCount` and `newImportBytes`, as deltas of `hostImportCount()`;
      - `planMs`: the time before `wallMs` starts.
    - Keep the record one JSON line, and add fields only.
  - verify:
    - At the phase build, a new `GpuResidencyTest` checks a two-member spatial and point region on lavapipe and RADV: `memberNs` sums to within 5% of `kernelMs`, and the two members differ in the direction their kernels' costs do.
    - The `RegionKernelTimeIsNotCountedTwice…` test still passes.
    - The new fields appear in a `NATRON_RENDER_PROFILE` record.
  - size: L
- [x] M109.P1.T2 — Reproduce the empty-input hang as a failing test that times out
  - files: `Tests/GpuRegionFallback_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Build regions with a member whose input is:
      - connected, but its task has an empty RoI;
      - disconnected;
      - a disabled mask.
    - Use a GPU-capable native node, for example Grade with a mask, plus Keyer/ChromaKeyer with input 1 empty.
    - Render through `RenderScheduler` with a `QFuture` and `kFrameTimeoutMs`, as `GpuResidency_Test.cpp:483` does, so a hang fails instead of blocking ctest. Assert that the result matches the CPU render.
    - Mark the hanging cases `DISABLED_` so the sweep stays green. P3.T1 enables them again.
  - verify: At the phase build, the non-disabled cases pass on lavapipe, RADV and no ICD in both modes, and running with `--gtest_also_run_disabled_tests` shows the hang as a timeout failure on lavapipe and RADV.
  - size: M
- [x] M109.P1.T3 — Commit the region bench driver and its time breakdown
  - files: `tools/bench/gpu_region_bench.sh` (new), `tools/bench/gpu_region_summary.py` (new), `tools/bench/README.md`
  - approach:
    - Port M85's `gb.sh`/`cases.sh`/`summ.py` into the repo:
      - cases for the node and gxg topologies, plus the comps;
      - Auto/Off rounds, interleaved;
      - a `MemAvailable` probe on every case, a CPU-idle wait and a dirty-page drain;
      - outputs deleted outside the timed part;
      - an AppImage path, run with `env -u LD_LIBRARY_PATH`.
    - The summary splits region time per region and per node into: each member's kernel, upload/download hidden and exposed, plan, record, submit-wait, acquire and imports, plus a residual "other".
    - Also print the placement-predicted `transferMs` next to the measured exposed transfer.
  - verify: `python3 -I tools/bench/gpu_region_summary.py --self-test` passes on a bundled sample profile. A dry run (`BENCH_DRY_RUN=1`) lists the planned cases without rendering.
  - size: M

## Phase 109.2: Diagnosis (no code)

- [x] M109.P2.T1 — Quick directional profile pass (~1 h) to explain Transform "other", the 24k transfer rise and the kernelMs gap
  - files: none (results go in `## Decisions`; raw data in `.local/m109/`)
  - approach:
    - Use the release AppImage from the P1 build, on the target host, with RADV.
    - Run node/gxg Transform plain, ChromaKeyer and Erode 40 at UHD, 16k and 24k, plus keying_grade and cg_multipass at UHD. Use 2 rounds, and pair Auto/Off within one boot.
    - Answer each with numbers:
      - which phase holds plain Transform's 55–60% "other", and why its exposed transfer is ~4× other nodes'. Hypotheses to check: pool misses because RoI and buffer sizes change per frame, the source RoI being larger than the output, and record or pipeline-creation time;
      - whether the 24k rise in exposed transfer is fresh `HostImageMemory` buffers and new imports;
      - whether a lone spatial member's timestamped kernel matches GpuBench, or which pass accounts for the 1.7–3×: wrap divide/finish passes, or the halo over cold input;
      - whether the predicted `transferMs` is within 1.5× of the measured exposed transfer for lone-Grade regions.
    - Add one `BENCH_RANGE` frame-parallel run of Erode 40 and Blur σ100 at 16k. It counts halvings and serializations, to show whether P6's ladder items bite.
  - verify: Decisions records a table per question, with a named root cause or "not reproduced", and states whether P4.T3, P5.T1, P5.T2 and P6.T2 are confirmed or dropped.
  - size: M
- [x] M109.P2.T2 — Find the root cause of the empty-input hang
  - files: none (findings in `## Decisions`)
  - approach:
    - Run the P1.T2 disabled cases with `NATRON_RENDER_PROFILE` and the region error log.
    - Where the container's ptrace ban blocks a debugger, render the equivalent `.ntp` with NatronRenderer from the fast AppImage on the host, and take `gdb -batch -ex 'thread apply all bt' -p` while it hangs.
    - Name the waiting thread and what it waits on. Candidates: the GPU kernel reading an empty view, an unscheduled dependency, or a wait in the fallback path.
  - verify: Decisions names the blocking call with `file:line` and the fix P3.T1 makes.
  - size: M

## Phase 109.3: Correctness and fallback budget

- [x] M109.P3.T1 — Make a member with an empty input render correctly or refuse to the CPU, never hang
  - files: `Engine/GpuRegionTask.cpp`, plus the file P2.T2 names (expected `Engine/RenderScheduler.cpp` or a node's `renderGpu`), `Tests/GpuRegionFallback_Test.cpp`
  - approach:
    - Apply the P2.T2 fix.
    - Also make `plan()` treat an empty or absent input of a non-mask input as either an explicit refusal (`FailureKind::Refused`) or a valid empty view the node accepts. The behaviour must be the same in the unstripped and the strip paths.
  - verify: At the phase build, the P1.T2 cases are re-enabled and pass on lavapipe, RADV and no ICD in both modes, with no timeout.
  - size: M
- [x] M109.P3.T2 — Give a region's CPU fallback the thread budget a CPU task would get
  - files: `Engine/RenderScheduler.{h,cpp}`, `Engine/GpuRegionTask.cpp`, `Tests/RenderScheduler_Test.cpp`
  - approach:
    - Pass the region body a callback that computes, under the scheduler mutex at the time of the fallback, `computeTaskBudget(poolMax, running CPU tasks, ready, cores, nThreadsPerEffect)`.
    - `renderMembersOnCpu` opens its own `AppTLS::FrameContextScope` and `OpenMPThreadsScope` with that budget. Check that nested scopes restore correctly.
    - Remove the unused `budget` parameter of the region body instead of keeping it (hard cut).
  - verify:
    - At the phase build, a `RenderScheduler_Test` with a stub body that forces the fallback asserts that the member sees a budget > 1 on a pool of ≥ 2 threads. It also asserts that the budget drops when CPU tasks are running.
    - A quick host bench of a forced fallback (`gScaleKernel = -1` style refusal) of Blur σ25 at UHD runs within 15% of the same Blur at Off.
  - size: M

## Phase 109.4: Transfer overlap (largest measured cost)

- [x] M109.P4.T1 — Strip regions that fit whole when pipelining can hide their transfer
  - files: `Engine/GpuRegionStrips.{h,cpp}`, `Engine/GpuRegionTask.cpp` (`planStrips`), `Tests/GpuResidency_Test.cpp`
  - approach:
    - Add a `GpuStripLimits::overlapMinBytes` and an overlap target: when the region is `Whole`, pipelining is allowed, and no output is kept on the device, plan at least `kGpuStripMaxInFlight` strips. This applies when its host I/O bytes are ≥ the threshold (start at 32 MiB, tuned in P4.T4).
    - Strips stay ≥ `kGpuStripMinPipelinedRows`, and at least 4× the largest halo of the region's members. If not, the region stays whole.
    - Count these regions separately in the stats.
  - verify:
    - At the phase build, unit tests on `planGpuStrips` cover: whole region with a large I/O → strips; a kept output → whole; a large halo → whole; no pipelining → whole.
    - A residency test with `GpuStripPipelining::On` on lavapipe and RADV renders a lone Grade at UHD as ≥ 3 strips, bit-equal to whole.
  - size: M
- [x] M109.P4.T2 — Pin download destinations ahead of time on a helper thread
  - files: `Engine/Gpu/GpuTransfer.{h,cpp}`, `Engine/Gpu/GpuRuntime.{h,cpp}`, `Engine/GpuRegionTask.cpp`, `Tests/Gpu/GpuTransfer_Test.cpp`
  - approach:
    - `resolveImports` creates imports outside `importMutex`. It takes the lock only to look up and insert, and drops its own import if another thread inserted one first.
    - Add a public `prepareImport(HostRange)` that warms the cache.
    - After `plan()`, the region allocates its download images and hands them to a helper thread, which touches them and calls `prepareImport` while uploads and compute run. `issueDownloads` then finds them already imported.
    - The race with `forget()` is resolved by a generation check under the lock.
  - verify:
    - At the phase build, `GpuTransfer_Test` imports one range from 8 threads at once: one cached import, with the rest dropped or reused. It also checks a `forget` during `prepareImport`.
    - A residency test sees no new import at download time on the first frame (`importCount` from P1.T1), on lavapipe and RADV.
  - size: L
- [x] M109.P4.T3 — Charge placement the transfer a region exposes, not the transfer it hides (if P2.T1 confirms a misprediction)
  - files: `Engine/GpuCostModel.{h,cpp}` (`regionGainMs`, new `observeTransfer`), `Engine/GpuRegionTask.cpp` (`noteSuccess`), `Tests/GpuPlacement_Test.cpp`
  - approach:
    - The gain counts exposed transfer: max(0, transfer − the kernel it overlaps) for a region that P4.T1 would strip, and full transfer otherwise.
    - The region's measured exposed ms per byte feeds a per-session EMA that corrects the calibrated rate. This lets placement refuse a lone cheap point op whose transfer outweighs its saving on a slower bus.
  - verify: At the phase build, placement tests show:
    - a lone Grade with a halved transfer rate is refused;
    - the same Grade on this bus, pipelined, is accepted;
    - the EMA moves the rate toward observed samples.
  - size: M
- [x] M109.P4.T5 — Bring region host transfer up to the bus rate: cache-first imports and whole-image source imports
  - files: `Engine/Gpu/GpuTransfer.{h,cpp}` (`resolveImports`), `Engine/GpuRegionTask.cpp` (source prepare at region start, `prepareWaitMs`), `Engine/RenderScheduler.h`/`Engine/RenderStats.{h,cpp}` (the new record field), `Engine/GpuContext.cpp` (staging copy threads), `Tests/Gpu/GpuTransfer_Test.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach:
    - `resolveImports` looks in the cache first and accepts any sub-range that a cached import fully covers, whatever its alignment. Alignment is required only when a new import has to be created.
    - The region prepares its source images whole at region start, as it already does for outputs (`prepareImports` without the touch step). Each strip upload is then a slice of a cached import, with no pinning or staging per strip.
    - Time `awaitPreparedDownloads()` as a new `prepareWaitMs` record field, separate from `downloadMs`, so first-touch faults don't count as transfer.
    - Staging for leftovers uses about 4 copy threads and bigger pieces (`GpuContext.cpp:~429`).
    - Report `TransferPath` and `HostImportStats.atUpload`/`atDownload` in the region record.
  - verify:
    - At the phase build, a `GpuTransfer_Test` case checks that an unaligned sub-range of a cached import makes no new import. A residency test checks that a strip-mode region's second frame has `atUpload == 0` and `atDownload == 0` and no staging on 4096-aligned widths, on lavapipe and RADV.
    - Host bench on RADV: before the change, ChromaKeyer at 16k shows device copy time well below host `uploadMs + downloadMs`. After it, the rate is ≥ 85% of the same-boot GpuBench host-import rate in each direction.
  - size: L
- [x] M109.P4.T6 — Pipeline strips on extra compute-family queues when the device has no transfer-only family
  - files: `Engine/Gpu/GpuDevice.{h,cpp}` (queue creation), `Engine/Gpu/GpuTransfer.{h,cpp}` (`QueueKind`, timelines), `Engine/GpuRegionStrips.cpp` (`gpuStripPipeliningAllowed`), `Engine/GpuRegionTask.cpp` (`pipelineAllowed`), `Engine/GpuPlacement.cpp`, `Tests/Gpu/GpuTransfer_Test.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach:
    - With no transfer-only family, `GpuDevice::create` asks for `min(queueCount, 3)` compute-family queues: compute, upload and download. With only 2 queues, upload and download share the second.
    - `transferUsesComputeQueue` is replaced by `transferSharesComputeQueue`, which is true only when there really is just one `VkQueue`. It gates `gpuStripPipeliningAllowed`, `pipelineAllowed` and placement's pipelining test.
    - `QueueKind::Transfer` splits into Upload and Download, and `transferTL` into separate upload and download timelines, because one timeline signalled from two queues breaks increasing signal order. Ownership transfers within one family stay no-ops.
    - Do not set `RADV_PERFTEST` in-process.
  - verify:
    - At the phase build, the existing pipelined-strip and residency tests pass on lavapipe (1 queue: serial path) and RADV (several queues), in both scheduler modes.
    - On RADV with no environment variable set, a region record shows `stripsInFlight > 1`, and hidden transfer is about equal to kernel time.
    - ChromaKeyer at 16k: wall time is about max(upload, download) + kernel.
  - size: L
- [x] M109.P4.T7 — Run local RADV tests on the default queue layout, and lock every queue around device-wide waits
  - files: `tools/ci/local/devshell.sh`, `tools/gpu-spike/run-host.sh`, `Engine/Gpu/GpuDevice.{h,cpp}`, `Engine/Gpu/GpuTransfer.cpp`
  - approach: Remove `RADV_PERFTEST=transfer_queue` from both scripts (hard cut, no opt-in). Add a `GpuDevice` helper that takes every queue mutex in a fixed order, then calls `vkDeviceWaitIdle`. Use it in `waitIdle`, the transfer destructor, `releaseCachedImports` and the `process()` abort path.
  - verify: At the phase build, the residency and transfer tests pass on RADV with no env var, and `WholeRegionWithLargeTransferRunsAsOverlappedStrips` in Auto mode pipelines.
  - size: M
- [x] M109.P4.T4 — Quick directional bench of the overlap phase (after P4.T5 and P4.T6)
  - files: none (results in `## Decisions`)
  - approach:
    - Rerun P2.T1's comps and node/gxg cases at UHD and 8K with `gpu_region_bench.sh`.
    - Tune `overlapMinBytes` and record the chosen value.
  - verify: Exposed transfer is below 30% of region time on the expensive spatial cases (Erode 40, Blur σ100 at 16k/24k) and on fused comp regions. No Auto case is slower than Off in any round, and no comp regresses against M85's ratios.
  - size: M

- [x] M109.P4.T8 — Prepare download destinations chunk by chunk, ahead of the downloads (after P4.T9; both edit `GpuRegionTask.cpp`)
  - files: `Engine/Gpu/GpuRuntime.{h,cpp}`, `Engine/GpuRegionTask.cpp` (`allocateDownloadImages`, `downloadStrip`), `Tests/Gpu/GpuTransfer_Test.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach:
    - The import job takes ranges aligned to strip interiors and rounded out to 2 MiB, and marks each one ready as it finishes. `downloadStrip` waits only on its own chunk, and the cache-first lookup resolves each strip within one chunk.
    - Touch the size-class headroom too, or stop importing into it.
  - verify: Erode 40 at 16k, a single-frame render in a fresh process and the first GPU render after a jump to a new frame: `prepareWaitMs` ≤ 20 ms. Residency tests pass on lavapipe and RADV.
  - size: M
- [x] M109.P4.T9 — Strip for overlap only when the kernel can hide transfer
  - files: `Engine/GpuRegionStrips.{h,cpp}` (`planOverlap`), `Engine/GpuRegionTask.cpp` (`planStrips` passes the predicted kernel and transfer ms), `Engine/GpuCostModel.cpp` (`pipelinesTransfers`), `Tests/GpuResidency_Test.cpp`
  - approach:
    - Require predicted K ≥ 0.25·T before stripping a region that fits whole. Tune the threshold on the UHD and 8K lone and comp cases.
    - Log which `planOverlap` gate refuses the defocus_retime 8K regions, and loosen the ×4 halo rule if it's the culprit.
  - verify: directional bench: lone cheap UHD regions run whole with exposed transfer ≤ 11 ms; the defocus 8K regions run as overlapped strips; no comp regresses.
  - size: M

- [x] M109.P4.T10 — Find and fix why a fresh process renders its first frame on the CPU
  - files: `Engine/GpuPlacement.cpp`, `Engine/GpuCostModel.{h,cpp}`, `Engine/GpuContext.cpp`, `Tests/GpuResidency_Test.cpp` (exact files depend on the cause)
  - approach:
    - In the P4.T8 check (`.local/m109/p4t8/`), comp frame 1 of a 6-frame Erode 40 16k run rendered on the CPU, and the region ran on the GPU only from frame 2. Find the cause from the profile and code: GPU init not finished when the first frame is placed, a cold cost model refusing, or something else.
    - Fix it so a fresh process's first frame goes to the GPU whenever placement would accept the same frame once warm. If GPU init is asynchronous, the first frame waits for it when the GPU is going to win, rather than falling to the CPU.
  - verify: a single-frame render in a fresh process (`NATRON_RENDER_PROFILE` on) of Erode 40 16k, Blur σ100 16k and one M82 comp at UHD: the region record shows it ran on the GPU, and wall time beats Off. A test pins the first-frame placement.
  - size: M

- [x] M109.P4.T11 — Shorten GPU startup so a fresh process's first frame waits less
  - files: `Engine/GpuContext.{h,cpp}`, `Engine/Gpu/GpuDevice.cpp` (only if device creation itself is the cost), `Tests/GpuContext_Test.cpp`
  - approach:
    - Time each startup phase (loader, instance, device, runtime, each calibration pass) from `GpuContext` construction, and log it once at info level.
    - Skip the staging calibration when host import is supported, since regions then never stage; calibrate staging lazily on first use otherwise. Cut whatever else the timing shows is avoidable before the first placement (e.g. smaller or fewer round trips, or calibration after availability with the seeded rate in the meantime).
  - verify: startup to `isAvailable()` on RADV is reported before and after, and drops well below the 170–190 ms measured in P4.T10. `GpuContext_Test` passes. A single-frame Erode 40 UHD render in a fresh process is no slower than Off.
  - size: M

## Phase 109.5: Root-cause fixes and host memory

- [x] M109.P5.T1 — Remove the cause of plain Transform's "other" time and its 4× exposed transfer
  - files: as P2.T1 finds. Candidates are `Engine/GpuRegionTask.cpp` (buffer sizing and reuse), `Engine/Nodes/Transform/Transform.cpp` (`renderGpu`, source RoI) and `Engine/Gpu/GpuBufferPool.{h,cpp}` (size classes), plus `Tests/GpuResidency_Test.cpp`
  - approach: Fix the named cause without touching M126 - Runtime GPU Shaders' files. If the fix needs `GpuKernel.cpp` (for example, pipeline creation per dispatch), move it to Phase 109.7 and flag it.
  - verify: At the phase build, a test asserts the specific invariant, for example no pool miss on the second frame of an animated Transform. A directional bench shows Transform "other" below 20% of region time at UHD and 16k.
  - size: M
- [ ] ~~M109.P5.T2~~ (dropped, see Decisions 2026-10-19) — Stop exposed transfer rising at 24k (if P2.T1 traces it to fresh host buffers)
  - files: `Engine/HostImageMemory.{h,cpp}`, `Tests/Image_Test.cpp`
  - approach:
    - Let the pool hold at least one frame's working set of same-size buffers, even when that is above RAM/4. Bound it by `MemAvailable`, probed at each grow, so it never assumes a machine.
    - Or apply the cause P2.T1 found instead.
  - verify: At the phase build, unit tests cover pool growth beyond the default cap when memory is available and refusal when it is not. A directional 24k bench of ChromaKeyer and Erode 40 shows exposed transfer at or below 0.1 ms/Mpx.
  - size: M
- [x] M109.P5.T3 — Trim idle pooled host memory to a floor
  - files: `Engine/HostImageMemory.{h,cpp}`, `Tests/Image_Test.cpp`
  - approach:
    - Record the time of the last allocate or release.
    - A lazily started reaper thread wakes every few seconds and calls `trimTo(floor)` once the pool has been idle for longer than the idle period. The `GpuContext` free hook then drops the cached imports, which unpins them.
    - Make the floor and idle period constants, with a test hook to shorten them.
    - The thread stops at shutdown.
  - verify: At the phase build, a test fills the pool, waits past a shortened idle period, and sees `pooledBytes()` at the floor. A release during the wait resets the timer.
  - size: M

## Phase 109.6: Retry ladder and Blur narrow pass

- [x] M109.P6.T1 — Remember the strip height that worked for each region shape
  - files: `Engine/GpuRegionTask.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach:
    - Keep a session map, keyed by device generation and region shape (member plugin IDs and parameters hash, output bounds, nComps), to the last successful `forcedRows`/serial.
    - `runGpuRegion` starts from the remembered entry. The map is cleared on device loss or a runtime rebuild.
  - verify: At the phase build, with the pool budget capped so a heavy-scratch region halves on frame 1, frame 2 has `retries == 0`. This holds on lavapipe and RADV.
  - size: M
- [ ] ~~M109.P6.T2~~ (dropped, see Decisions 2026-10-19) — Wait briefly for a contending region before halving strips (only if P2.T1 saw contention halvings)
  - files: `Engine/Gpu/GpuBufferPool.{h,cpp}` (release notification), `Engine/GpuRegionTask.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach: On out-of-memory, when another region holds pool bytes, wait on a pool release condition (bounded at ~50 ms) and rerun at the same height before halving.
  - verify: At the phase build, two concurrent regions over a capped budget complete without a halving.
  - size: M
- [x] M109.P6.T3 — Bring Blur's narrow vertical pass (r ≤ 32) near its horizontal pass
  - files: `Engine/Gpu/GpuBlur.cpp`, `Engine/Gpu/Kernels/blur.slang`, `Tests/Gpu/BlurGpu_Test.cpp`
  - approach: Route r ≤ 32 through the shared-tile column path that already wins for wide σ, or tune the coalesced path, inside the existing `blurPass` entry. Do not add an entry point.
  - verify: At the phase build, the Blur parity tests pass on lavapipe and RADV. GpuBench (`run-host.sh`) σ3 UHD V/H is ≤ 2×, against ~10× before.
  - size: M

## Phase 109.7: Shared-file follow-ups (FLAGGED: touch files other milestones own)

- [x] M109.P7.T1 — ⚑ Record one edge per input in `collectPreRenderFrames` (touches M100 - Content Node Hashing's `ParallelRenderArgs.cpp`; start after M100 merges)
  - files: `Engine/ParallelRenderArgs.cpp`, `Engine/GpuRegionTask.cpp` (delete the compensation at `:597-615`), `Tests/SchedulerEquivalence_Test.cpp`
  - approach: Key `PreRenderFrames` by (effect, inputNb), so a node feeding two inputs yields two edges, as `FrameGraph::Task` documents. Delete the region's edge re-derivation.
  - verify: At the phase build, a node wired to a member's source and mask gives two dependencies, and CPU and GPU renders are equal in both modes.
  - size: M
- [x] M109.P7.T2 — ⚑ Add Erode size and Blur filter to `GpuTaskTraits` and seed them (touches `GpuCostSeed.inc`, which M90 - Render Path Serial Overhead regenerates; start after M90 merges)
  - files: `Engine/GpuCostModel.{h,cpp}`, `Engine/GpuCostSeed.inc`, `tools/bench/render-profile-report.py`, `Tests/GpuPlacement_Test.cpp`
  - approach:
    - Add `size` for Erode/Dilate and a filter enum for Blur, used in `traitsOf` and the row lookup.
    - Seed rows for: Erode 6/40, box/triangle/quadratic, the Gaussian IIR row apart from Quasi-Gaussian, and motion blur q4. Use P1.T1's timestamped lone-node figures.
  - verify: At the phase build, `Placement|CostModel` passes, and the report self-test passes.
  - size: M

## Phase 109.8: Thorough bench

- [ ] M109.P8.T1 — Thorough Auto/Off bench on the target host
  - files: none (results in `## Decisions`, design note §11 hand-off)
  - approach:
    - Use the P7 release AppImage with `gpu_region_bench.sh`, 3 rounds.
    - Run the M82 comps at UHD and 8K, and the node/gxg cases at UHD, 8K, 16k and 24k.
    - Probe RAM on every case, and keep the write-back out of the timed part.
    - Report medians of ratios from pairs measured on the same boot, set against M85's table.
    - Measure the real workload first, each case Auto against Off, same boot: (a) a single-frame render in a fresh process, with the on-disk pipeline cache warm and, reported separately, cold; (b) re-renders of the same frame after a knob change, in one session; (c) a jump to an arbitrary frame after (b). Multi-frame playback is secondary and reported last.
    - Bench fixes: the driver can run (a)–(c), and `tools/bench/gpu_region_summary.py` gets the pipelined-floor and `prepareWaitMs` columns and labels each record with its case.
  - verify: The gate below holds. The idle Natron's pooled host bytes, read after 2× the idle period, are at the floor.
  - size: M

**Verification gate:**
- Full ctest passes on lavapipe, RADV and no ICD, in both scheduler modes, and `-DNATRON_GPU=OFF` builds.
- Every P1.T2 case finishes with no timeout.
- On the M82 comps:
  - no region halves its strips after its first frame;
  - contention halvings are 0, or explained in Decisions;
  - region exposed transfer, with `prepareWaitMs` excluded from numerator and denominator, is ≤ 1.25× its pipelined floor max(0, T − K·(n−1)/n), where T uses the same-boot GpuBench host-import rate; where K ≥ T it is under 30%. Cheap lone nodes are left to placement;
  - `prepareWaitMs` is ≤ 10% of region wall on the first GPU render of a frame, not just in steady state;
  - plain Transform "other" is under 20%.
- Every speed-up is ≥ M85's. No Auto case is slower than Off on a single-frame render in a fresh process (warm on-disk pipeline cache), on a same-frame re-render after a knob change, or after a frame jump. The cold-pipeline-cache single frame is reported with its compile time.
- A fresh process's first frame runs on the GPU wherever placement accepts the warm frame.
- CPU-only renders are within ±2% of `main`.
- A forced CPU fallback of a big Blur runs within 15% of Off.
- Per-member times in profiles come from timestamps.

## Decisions

- 2026-10-16 — **Elaborated at claim by the gpu PM** from the stub's follow-up list. The stub's own facts are folded into Scope and the scouted facts. Task files avoid every file of M126 - Runtime GPU Shaders, which runs alongside, and every file in the unchecked tasks of M90 - Render Path Serial Overhead and M100 - Content Node Hashing, except the two flagged Phase 109.7 tasks, which wait for those merges. Multi-plane placement moved Out to M110 - Remaining Native GPU Ports, because it needs `GpuRender.h`.
- 2026-10-18 — **M109.P1.T1: new record fields sit between `transferExposedMs` and `fallback`**, because the test helper `regionOutcomeOf` reads from `fallback` to the end of the line. For pipelined strips, `submitWaitMs` is time in `submit()` plus the wait in `retireStrip`, with no device time subtracted, since the two overlap. `poolMisses`, `importCount` and `newImportBytes` are deltas of process-wide counters, so regions running at the same time leak into each other's numbers. The work was recovered from the previous session's transcript after a lane reset deleted it uncommitted, then 3-way applied onto `93339c413` with no conflicts.
- 2026-10-18 — **M109.P1.T2: the disabled cases were chosen by reading the code, not by running it.** The Keyer/ChromaKeyer cases with an empty inside mask and the two empty-RoI cases, whose mask is a `Constant` placed off-window, are `DISABLED_`. Grade with an absent or disabled mask stays enabled. The phase build's `--gtest_also_run_disabled_tests` run settles which cases really hang, and any disabled case that passes is re-enabled there. Not yet confirmed: whether the off-window `Constant` yields a connected task with an empty RoI or gets pruned. M109.P2.T2 checks this.
- 2026-10-18 — **M109.P1.T3: placement's predicted `transferMs` is not emitted anywhere in the profile.** It exists only as `GpuCostModel::transferMs()`, so the summary's `pred xfer` column reads `n/a` until a region record carries a `predictedTransferMs` field. For P2.T1's question on lone-Grade transfer prediction, compute the prediction offline from the calibrated rate, or add the field before P2.T1. In the summary, `other = wallMs − (kernel + exposed + record + acquire + submitWait)`. `planMs` falls outside `wallMs`. `other` can go negative for pipelined regions, because record and submit-wait overlap kernels there. Comps run through the CLI (`-w Write 1-6`); M85's `comp_bench.py` per-frame walls are not ported.
- 2026-10-18 — **Phase 1 build (run 39168234310, `09ad0743d`):** everything passes except `RegionMembersAreTimedOnTheDevice`. Its strip modes forced strips with a 1 MB limit, but on a 512-wide source a size-60 Blur's halo needs about 1.26 MB per strip, so the planner refused and the region fell back. The pipelined mode never ran either, because the serial-mode ASSERT stopped the loop. `44bec9bb7` uses 2 MB and checks that serial runs one strip at a time and pipelined runs several in flight. The same commit adds `predictedTransferMs` (`GpuCostModel::transferMs` of the bytes actually moved; `null` when the model has no bandwidth) before `fallback`, so M109.P2.T1 can compare predicted against measured transfer directly.
- 2026-10-18 — **Phase 1 is green on lavapipe** (run 39173462912, `44bec9bb7`): all of ctest passes in both scheduler modes. RADV runs of P1.T1's test and the `--gtest_also_run_disabled_tests` hang probe are still owed, and P2.T1 needs this build's release AppImage. This session's sandbox can't reach the Azure blob host the artifact downloads from ("no route to host"). Until it can, M109.P3.T2 and the Phase 109.4 implementation tasks go ahead, since they don't depend on the diagnosis.
- 2026-10-18 — **M109.P3.T2: the region body takes a `CpuFallbackLease` instead of a budget.** A fallback leases `cpuTaskBudgetLocked()`, now shared with `popLocked`, and counts in `_fallbackCpuTasks` until the token drops. `renderMembersOnCpu` applies the budget through `ThreadBudgetScope` plus `OpenMPThreadsScope`, not a second `FrameContextScope`. The test uses a second concurrent lease to stand in for a running CPU task. Still owed: the host bench of a forced fallback within 15% of Off, done with the P4.T4 directional pass once the AppImage is reachable.
- 2026-10-18 — **M109.P4.T1: the overlap halo rule uses the chain's cumulative halo, measured on a mid-band strip.** That is stricter than "4× the largest member halo". Overlap planning needs ≥ 3 strips that fit the budget with 3 in flight. `keptOnDevice` is `slot.keep && !slot.download`, so a root output doesn't block it. `overlapMinBytes` defaults to max in `GpuStripLimits` and `planStrips` sets it to 32 MiB; P4.T4 tunes it. `91945efa8` (P3.T2) left `Tests/GpuResidency_Test.cpp` uncompilable because its region-body lambdas still took `int budget`. This commit fixes them.
- 2026-10-18 — **M109.P4.T2: the `forget()` race uses a per-range in-progress mark instead of a global generation.** A global generation would drop good imports whenever the Cache freed any unrelated buffer. `forget()` waits until the marked imports are destroyed. A single runtime-owned helper thread serves every region, so concurrent regions' prepare jobs queue behind each other, and any wait at the first download counts in `downloadMs`. Download images are now allocated at region start, which raises peak RAM a little in whole mode. The forget test uses a 100 ms sleep to release the held import. The per-origin counters are in `hostImportStats()`, not in the region record.
- 2026-10-18 — **M109.P5.T3: floor 256 MiB, idle period 30 s, poll every 5 s** (the implementer's choice; `setIdleTrim` is the test hook). The reaper starts at the first pooled-size release, and a static `ReaperJoiner` joins it at exit. Two things for the review to check. First, hooks are copied under the lock and then invoked, so a hook may run after `removeFreeHook` returns; this predates M109, and the Cache's trim has the same exposure, but the reaper now makes it reachable during static destruction. Second, the timer-restart test steps in 50 ms against a 2 s idle period and could flake on a stalled CI host.
- 2026-10-18 — **M109.P6.T1: the shape key is plugin ID + `Task::nodeHash` + `renderedRoI` + component count per member.** `GpuRuntime::generation()` is new and drawn from a process-wide counter, because `recreate()` keeps the same runtime object. The map holds 256 entries and clears when full or when the generation changes. Only settings from the OOM ladder are remembered, never overlap planning. Known limits: an animated parameter makes each frame a new key, and a remembered setting outlives a raised memory budget until a rebuild.
- 2026-10-18 — **M109.P6.T3: the narrow path gets its own multi-column shared tile rather than the wide-radius column path,** which loads one column per group and wastes most of each cache line at small radii. The tile is 8 columns × (64 + 2r) rows, within the existing `tile[1152]`. If the GpuBench V/H ≤ 2× target misses, tune `kNarrowRows`/`kNarrowColumns` first (16×32 is the alternative), since the halo load is 2× the output at r = 32. Parity and the bench are still owed: the phase build and a host GpuBench run.
- 2026-10-18 — **M109.P2.T2: root cause found, and it is not an empty-input bug.** It was a self-deadlock that `cc7d152af` (P4.T2) introduced. `RegionRun::allocateDownloadImages` takes `Image::WriteAccess` (`Engine/GpuRegionTask.cpp:1074`), then calls `gpuHostTransferBytes` (`Engine/GpuContext.cpp:541`), and that call takes a read lock on the same image (`Engine/Image.h:289`). Qt's `QReadWriteLock` blocks a writer that asks for a read lock, so the thread waits on itself. `issueDownloads` (`:1114`) has the same pattern. The test's post-abort `FrameFuture::wait` (`RenderScheduler.cpp:95`) has no time limit, so a hang wedges ctest instead of failing. Local release build of `b7d54029f`: 26 of 186 filtered tests hang on RADV and lavapipe in both modes. The fix is to compute the bytes before taking the lock, in a `Slot::downloadBytes`. With it applied in a scratch tree, every case passes, including the Keyer, ChromaKeyer and `KeyerWithInsideMaskOfEmptyRoI` disabled cases, which can be re-enabled. Before `cc7d152af` those cases passed too. `DISABLED_GradeWithMaskOfEmptyRoIMatchesCpu` is a broken test: an off-window mask makes Grade an identity, so no region forms, and the test must be rewritten. Also found: `HostImageMemoryTest.IdlePoolIsTrimmedToTheFloor` and `...RestartsTheIdlePeriod` (P5.T3) fail on RADV, because GPU calibration's `HostImageMemory::trim()` (`GpuContext.cpp:389`) races the test. The guard must wait for GPU init. Data is in `.local/m109/` (backtraces `bt-*.txt`), and the release AppImage is `.local/m109/Natron-m109-b7d54029f-release.AppImage`. This unblocks P2.T1 on the target host.
- 2026-10-18 — **The branch was rebased onto `2410277ee` (M126 merged); every earlier code SHA in this file changed.** The only conflict was `GpuRuntime.{h,cpp}`. Teardown order: import preparer, then pipeline cache, then pool, registry, transfer and device.
- 2026-10-18 — **M109.P3.T1:** fixed the lock with `Slot::downloadBytes`. `plan()` refuses an input with an empty `renderedRoI` or a connected non-mask input with no slot. Unstripped `record()` refuses a slot with no buffer. The post-abort wait is bounded at 60 s, after which the test leaks the future. The Keyer, ChromaKeyer and Keyer empty-RoI cases are enabled again. `GradeWithMaskOfEmptyRoIMatchesCpu` was deleted because Grade's only second input is a mask; the Keyer empty-RoI case is the genuine one. `HostImageMemoryGuard` waits for GPU init. Local release build, one test per process: 188/189 pass on RADV legacy and on lavapipe, 187/189 on RADV taskgraph, `GpuTests` 117 pass on RADV, 0 hangs. **Open:** `GpuResidencyTest.LargeCompStaysWithinTheDeviceBudget` fails 1 in 3 on RADV taskgraph: it halves strips instead of evicting, with evictions at 0. That may come from P4.T1 overlap strips or P6.T1 strip memory. It must be investigated before the gate.
- 2026-10-18 — **`LargeCompStaysWithinTheDeviceBudget` flake: P6.T1's strip memory is doing its job; the test's expectation was too strict.** At HEAD the test failed 5/10 in both modes on RADV; with strip memory off it passed 10/10. Its two identical frames render at once, so one twin's learned setting lets the other start stripped and fit without evicting. The test now requires `evictions + thinned > 0`; it passes 10/10 in both modes on RADV. Strip memory is keyed by device generation, so it does not leak between tests that build their own context. **Design note for later:** a setting learned while 16 regions compete is reused when the same shape runs alone, so it may stay stripped when it would fit whole.
- 2026-10-19 — **M109.P2.T1, partial; the unchecked runs are owed.** The answers come from the pre-reboot node/gxg data in `.local/m109/p2t1/main/` (same-boot pairs). After the reboot, a process outside the container held 18–21 GB of VRAM at 87–100% GPU busy, so this boot's runs sit in `boot2/contended/` and are invalid. Still owed on a free GPU: the cut-off `gxg-erode40-24k-off-r2` pair, the comps (the committed driver's comp output dir `build/bench/m82-profile/out` does not exist; use `p2t1/comps` with `compout/`), the Blur against GpuBench comparison, BENCH_RANGE and a fixed-parameter Transform check. All of them are queued in `boot2/gate_and_run.sh`.
  - **Q1, Transform:** "other" is under 1% everywhere, so that part is not reproduced. Its exposed transfer is 1.5× ChromaKeyer's at UHD and 1.75× at 16k/24k, not 4×. Two causes:
    - it moves 8–17% more bytes, because the animated rotate grows the output;
    - it pays a 1.3–1.6× slower per-byte rate. The probable reason is that `HostImageMemory::allocate` reuses only exact-size buffers (`Engine/HostImageMemory.cpp:470-486`), so a growing RoD gets a fresh allocation and a fresh import every frame, plus 3–15 device pool misses per frame at 16k/24k. The fixed-parameter check would confirm it.
  - **Q2, 24k rise:** not reproduced. The rate goes from 1.19–1.22 to 1.21–1.25 ms/Mpx between 16k and 24k, and new imports are 100% of the download at both sizes. **Caveat:** every 16k/24k region ran in serial strips with no overlap. The VRAM occupant cut the pool budget to about 4 GiB, so pipelining never engaged, and that alone is a 15–20× gap in exposed transfer against M85.
  - **Q3, kernelMs gap:** timestamped UHD member kernels from frame 2 on match M85's seeds (ChromaKeyer 0.51–0.66 against 0.514 ms; Transform 0.63–0.87 against 0.670 ms). Blur records no divide or finish pass in these graphs, and the input is device-generated. **Lead:** the first GPU frame runs the members 1.4–3.7× slower, so M85's 1.7–3× may be weight-split warm-up. The GpuBench comparison is owed.
  - **Q4, predicted transfer:** `predictedTransferMs` is 0 in all 159 records, because `GpuDevice.cpp:365-374` marks a discrete ReBAR card as `unifiedMemory` and `GpuCostModel::transferMs` (`:418-420`) then returns 0. Placement charges nothing for transfer on this host.
  - **Verdicts:**
    - **P4.T3 confirmed,** with a first step added: the unified-memory detection, keyed on `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`. `GpuDevice.cpp` is added to its files, and no other running milestone lists it.
    - **P5.T1 re-scoped:** "other" is dropped, and the fix target is buffer reuse within a size class in `HostImageMemory::allocate` and the device pool, gated on the fixed-parameter check.
    - **P5.T2 dropped.**
    - **P6.T2 stays conditional on BENCH_RANGE.** No region retried in any single-frame run. The halving counters never reach the profile, so counts come from `retries`, `strips` and `stripsInFlight` in the region records.
- 2026-10-19 — **M109.P4.T3 (`633842a63`):** unified memory now follows the device type: integrated or CPU-type is unified, and discrete, ReBAR included, is not. No upload or download path read the old flag. `pipelinesTransfers` is the cost model's "would the planner pipeline this?" test. It shares `gpuStripPipeliningAllowed` with `RegionRun::pipelineAllowed`, and it ignores halos and regions that are forced into strips. `observeTransfer(up, down, hidden+exposed ms)` scales the calibrated rate by an EMA of measured over predicted time. It is fed hidden plus exposed time, because exposed time alone would count the overlap twice. Unpipelined samples are host wall time, while pipelined samples are device timestamps. The tests take 50 GB/s as "this bus", because at 25 GB/s a lone UHD Grade is refused even when pipelined. `VIRTUAL_GPU` counts as not unified. Checked with `-fsyntax-only` only; the tests run at the phase build.
- 2026-10-19 — **M109.P2.T1 done. Boot-2 runs on a free GPU** (`.local/m109/p2t1/boot2/`) agree with the pre-reboot data within 1–2%.
  - **Q1, Transform: cause confirmed.** The animated rotate grows the RoD every frame, so `HostImageMemory::allocate`'s exact-size match never reuses a buffer, and the device pool misses too. With fixed parameters at 16k, the same 4440 MB moves with 37% less exposed transfer, and imports and pool misses drop to 0. "Other" is under 1% everywhere. The real ratio against ChromaKeyer is 1.5–1.75×, not 4×. Transform's `planMs` is 14–21 ms at 16k/24k, against about 2 ms for other nodes; it sits outside `wallMs`, and it is noted, not fixed.
  - **Q2, 24k rise:** not reproduced. The rate is 1.19 at 16k against 1.23 ms/Mpx at 24k, and imports are 100% at both sizes.
  - **Q3, kernelMs gap:** only at UHD unstripped. Blur members run 1.9–2.1× GpuBench at σ3/σ25 and 1.5× at σ100; in 16k strips they run 0.86–1.04×. Wrap passes and cold input are both rejected. The cause is not identified: timing H and V separately inside the member would locate it. The first GPU frame runs 1.4–3.7× slower.
  - **Q4, transfer prediction:** the prediction was always 0. With the detection fixed, a bandwidth-based prediction would still be 2–4× too low: regions achieve about 6 GB/s up plus down, and 9–13 GB/s down alone, against GpuBench's 28 GB/s. P4.T3's `observeTransfer` EMA is what corrects this.
  - **BENCH_RANGE=4 at 16k:** Erode 40 and Blur σ100, with and without the 4160 MB cap, show 0 halvings, 0 serializations and 0 retries.
  - **Verdicts:** P4.T3 confirmed (done). P5.T1 confirmed, re-scoped to buffer reuse within a size class in `HostImageMemory` and `GpuBufferPool`. P5.T2 dropped. P6.T2 dropped.
  - **The finding that changes the plan:** strip pipelining never engages on RADV. `gpuStripPipeliningAllowed` needs a dedicated transfer queue, and RADV exposes one only under `RADV_PERFTEST=transfer_queue`. Every 16k/24k region therefore ran serial strips with 0 hidden transfer, and P4.T1's overlap strips never ran on this host. M85's hidden-transfer figures assumed the transfer queue. This puts the gate's "exposed transfer < 30%" out of reach as things stand. It has gone to a consultant.
  - **Bench tooling defects:** the committed `gpu_region_bench.sh` writes comp output to `build/bench/m82-profile/out`, which doesn't exist; `extra.sh` needs an absolute output dir; and the halving and serialization counters never reach the profile.
- 2026-10-19 — **The consultant's answer on RADV pipelining (recovered from the previous session's transcript).** Pipelining is not the main lever. Regions move host data at about 6 GB/s, against GpuBench's 28 GB/s, because strip uploads pin or stage per strip and miss the import cache. Its ranked recommendation:
  1. **Fix the region transfer rate first.** `resolveImports` checks the cache first and accepts any sub-range a cached import covers, whatever its alignment, and requires alignment only for a new import. Source images are prepared whole at region start, like the outputs, so each strip upload slices a cached import. A new `prepareWaitMs` separates first-touch faults from `downloadMs`. Staging gets about 4 copy threads and bigger pieces (`GpuContext.cpp:~429`). Expected: ChromaKeyer 16k exposed transfer about 320 → 80 ms, wall time 347 → 105 ms. Confirm first by comparing device copy time with host `uploadMs`/`downloadMs`.
  2. **Pipeline on extra queues of the compute family** when there is no transfer-only family: up to 3 queues (compute, upload, download). Replace `transferUsesComputeQueue` with `transferSharesComputeQueue` in `GpuRegionStrips.cpp:537`, `pipelineAllowed` and `GpuPlacement.cpp:581`. Split `QueueKind::Transfer` and `transferTL` into separate upload and download timelines.
  3. **Measure the gate's 30% on regions whose kernel time is at least comparable to transfer time** (expensive spatial ops, fused regions, resident chains), as M64 was judged. Placement already keeps cheap lone nodes on the CPU.
  - Rejected: setting `RADV_PERFTEST=transfer_queue` in-process, which is unstable and affects other Vulkan users such as M107's hand-off, and using the graphics family for transfers.
- 2026-10-19 — **M109.P5.T1 was implemented in the previous session, but left uncommitted** in `.worktrees/m109`; only `-fsyntax-only` has been run on it. It adds `Engine/Gpu/GpuSizeClass.h`, a header-only file of the class maths shared by `GpuBufferPool` and `HostImageMemory`. `HostImageMemory` allocates at class size plus 1/16 growth room and reuses the smallest pooled buffer up to one class above, and its `live` map records real sizes. `GpuBufferPool::acquire` takes from its own class up to the next. A new `GpuTransferOptions::hostAllocationBytes` makes a cache-miss import run to the end of the host buffer. Tests were added in `Image_Test`, `GpuBufferPool_Test` and `GpuResidency_Test` (`GrowingImageReusesTheBuffersAndImportsOfTheFrameBefore`). Known costs: pinned imports cover up to about a third more than the request, which the Cache doesn't count. A device request that grows across a class boundary still misses once. Growth room on device classes was reverted because it broke three exact-fit residency tests.
- 2026-10-19 — **The user took all three of the consultant's recommendations.** M109.P4.T5 fixes the region transfer rate and M109.P4.T6 adds pipelining on extra compute-family queues; P4.T4's bench follows both. The gate's 30% exposed-transfer target, and P4.T4's, now applies to regions whose kernel time is at least comparable to their transfer, as M64 was judged. Cheap lone nodes are placement's job.
- 2026-10-19 — **M109.P7.T1 (`83390a3b3`):** `PreRenderFrames` is keyed by `(effect, inputNb)`, and the roto bottom-merge entry by `(effect, -1)`. Its two consumers, `EffectInstance::treeRecurseFunctor` and the request-pass visitor, emit one edge per entry. The region's edge re-derivation is deleted. The new `NodeFeedingSourceAndMaskIsTwoEdges` test checks the graph shape and legacy against taskgraph on the CPU only, because `SchedulerEquivalence_Test` has no GPU harness. The CPU-against-GPU case for this wiring is owed in `GpuRegionFallback_Test`. The test assumes Grade's mask knob is `enableMask_<label>`; the phase build confirms it.
- 2026-10-19 — **M109.P4.T5 (`38df6aeb6`):** sources are imported whole by `importSources()` on the region thread, not the helper, so they don't queue behind another region's download touch. Only sources in pooled `HostImageMemory` buffers (`bytesFrom >= bytes`) are imported, since only those reach the free hook's forget. Staging uses 4 copy threads and 32 MB pieces, about 192 MB pinned. The new record fields come after `overlapStrips`: `prepareWaitMs`, `uploadsImported`/`Staged`, `downloadsImported`/`Staged`, `importsAtUpload`/`AtDownload`. `RenderProfile_Test.RegionRecordFormat` was updated. The existing window between a cache lookup and its copy now covers unaligned slices too; it is safe only because no live buffer is freed mid-transfer. `StripTransfersCopyOutOfWholeImageImports` assumes the Constant's bounds are exactly 1024². The bench summary script doesn't show the new fields yet. Checked with `-fsyntax-only` only.
- 2026-10-19 — **M109.P4.T6 (`7d362c6fe`):** `QueueKind` is Compute, Upload and Download, with an index per kind, and each distinct `VkQueue` has its own mutex. With no transfer-only family: 3+ compute-family queues give compute/upload/download on queues 0/1/2, 2 queues share queue 1 between upload and download, and 1 queue (lavapipe) keeps the serial path. A transfer-only family with 2+ queues gets separate upload and download queues too, which changes NVIDIA's path; M91 - NVIDIA GPU Validation should check it. Upload and download timelines are separate, retired imports carry their own timeline, and cached imports track their last upload and last download. **Two follow-ups, M109.P4.T7:** `tools/ci/local/devshell.sh:146` and `tools/gpu-spike/run-host.sh:59` set `RADV_PERFTEST=transfer_queue`, so local RADV tests never run the default layout. And `vkDeviceWaitIdle` runs without the queue locks, in `waitIdle`, the transfer destructor, `releaseCachedImports` and the `process()` abort path. That predates this change, but the device now has three queues. Checked with `-fsyntax-only` only.
- 2026-10-19 — **M109.P7.T2 (`d08c60d4d`), keys done but only Erode 40 newly seeded.** `GpuTaskTraits` gains `size` and a `GpuBlurFilter` enum. GPU seed rows may end with `filter` and `sizeMax`, and a row that names one beats a row that doesn't. Erode 6 keeps its 6.827 ms/UHD row, now tagged `sizeMax` 6. Erode 40 is new at 4.170 ms/UHD: the median steady-state `kernelMs` from p2t1, minus the CheckerBoard's seed. `GpuCostSeed.inc` was edited by hand, because a trial regeneration also changed the Write plugin ID. **Owed to the M109.P4.T4 bench:** Blur box, triangle and quadratic; Gaussian IIR apart from Quasi-Gaussian; motion blur q4; a CPU Erode 40 serial row; and a fresh Erode 6 run in the same environment, since Erode 40 coming out cheaper than Erode 6 is suspect.
- 2026-10-19 — **The owed CPU-against-GPU case for M109.P7.T1 is `GpuRegionFallbackTest.NodeFeedingSourceAndMaskMatchesCpu`** (`b42415fae`): one CheckerBoard, with alphas 0.25/0.75, feeds a Grade's source and its enabled mask. CI's ctest runs it in both scheduler modes.
- 2026-10-19 — **Local phase build of `b42415fae`** (release, `NATRON_DEV_GPU=1`, no `RADV_PERFTEST`; logs in `.local/m109/phase-b42415f/`). The build, the stripped AppImage (`.local/m109/Natron-m109-b42415fae-release.AppImage`, 92 MB; the host has no FUSE, so run it with `--appimage-extract-and-run`) and a `-DNATRON_GPU=OFF` build all succeed. RADV picks compute 1/0, upload 1/1 and download 1/2; lavapipe uses one queue. Full ctest passes 2580/2582 on RADV and lavapipe in both modes, and 2581/2582 with no ICD. Every targeted test passes: overlapped strips pipeline on RADV in Auto mode, `LargeCompStaysWithinTheDeviceBudget` passes 20/20 on RADV taskgraph, and Blur parity, transfer, device, fallback, placement, host memory and profile tests pass. **Two tests fail every time**, alone too: `GpuCostModel.PipelinedLoneGradeIsAcceptedWhereItsTransferHides` (`GpuPlacement_Test.cpp:685`; the unpipelined half still places the Grade on the GPU), and `GpuResidencyTest.MaskInAnotherLayerThanItsProducersPlaneRendersOnTheCpu` (`GpuResidency_Test.cpp:1090`; the producer task has 2 components where 1 is expected). Four Deep tests fail only in a single-process filtered batch and pass under ctest; that is test-order coupling, outside M109.
- 2026-10-19 — **Both phase-build failures were wrong test expectations** (`517d41345`). (1) `PipelinedLoneGradeIsAcceptedWhereItsTransferHides` could never refuse at 50 GB/s. CPU saved 8.29 ms, GPU 0.50, transfer 5.31, so the whole gain is +1.98 ms. It now runs at 35 GB/s: pipelined +0.21 ms is accepted, whole −0.29 ms is refused. Letting the separate upload and download queues overlap in the model was considered and rejected, because it would accept the half-bandwidth case. (2) Since P7.T1, a producer feeding a member's source and its other-layer mask has two components, which is correct. The member is now refused at `GpuRegionTask.cpp:577` ("does not render exactly one plane") rather than by the layer-mismatch check, and the CPU render and fallback assertions are unchanged. Both pass on RADV and lavapipe in both modes, and the placement, cost, residency, fallback and equivalence suites pass 86/86 on RADV in both modes. **For the review:** `CostFixture`'s doc comment ("even a lone Grade at 25 GB/s gains on the GPU") is false.
- 2026-10-19 — **M109.P4.T4 quick directional bench (boot 19:53, `.local/m109/p4t4/`; b42415fae release AppImage, taskgraph, no `RADV_PERFTEST`, VRAM free, 2 rounds).** Pipelining now engages on RADV's default layout: 3 strips in flight against 1 before, and hidden transfer is 53–170 ms on 16k/24k Erode and up to 640 ms on 24k Blur, against 0 before. All transfers are host-imported (0 staged); no retries or halvings.
  - **Ratios Off/Auto:** Erode 40 1.5–1.7 (UHD), 1.7–1.9 (8K), 2.2–2.4 (16k/24k); Blur σ100 2.7–2.8, 3.1–3.4, 3.2–3.7; Transform motion blur 8 and 11 at UHD/8K. Comps at UHD/8K: keying_grade 1.89/2.78, cg_multipass 1.66/1.99, defocus_retime 2.30/4.24, against M85's 2.77/2.90, 1.99/2.09, 2.94/2.88. M85's method was not reproducible; the same CLI method on pre-fix p2t1 gives 1.81 and 1.59, so this is a small gain over pre-fix and the M85 comparison is inconclusive.
  - **Exposed transfer:** Blur σ100 16k/24k 4–10% (pass). Erode 40 16k/24k 52–59%, from 76–82% (fail): download-only, 4096 MB at 16k (~280 ms) against a 98 ms kernel, with `prepareWaitMs` ~110 ms. Fused comp regions 41–69%, from 68–93% (fail), on 10–120 ms of kernel per frame. Lone cheap nodes 80–87% (excluded).
  - **Auto slower than Off once:** lone Transform UHD, round 1, 194 against 180 ms; round 2 was faster.
  - **Forced CPU fallback**, 16k Blur σ100 via a private config with `gpuMemoryBudgetMB=1`: median 13.26 s against Off's 13.26 s; the mean is 13.89 s because of one 18.4 s frame that did not recur (pass). `--setting gpuMemoryBudgetMB` is read too late, and `NATRON_GPU_FAULT=oom@N` with small N kills GPU init instead.
  - **GpuBench Blur V/H at r ≤ 32:** 2.29× at UHD (fail), 1.79× at 16k (pass).
  - **`overlapMinBytes`: keep 32 MiB.** The smallest region moves ~127 MB; whole regions that skip overlap are held back by another `planOverlap` gate, not the byte limit.
  - **Seeds owed to P7.T2** (UHD steady `kernelMs`, includes ~0.5 ms CheckerBoard): Blur box 22.85, triangle 32.79, quadratic 40.20, Gaussian IIR 29.46, Quasi-Gaussian 35.42, FIR σ25 8.01; Transform motion blur q4 48.28; Erode 6 8.80 and Erode 40 5.97 in the same run (Erode 40 cheaper than Erode 6 reproduces). CPU Erode 40 UHD single-thread 0.62 s.
  - **Tooling:** the committed comps hard-code `build/bench/m82-profile/out`; there is no 8K comp variant (`app.addFormat` in-process works).
- 2026-10-20 — **P4.T4's misses are mostly a bus limit; the gate was amended (user, on the consultant's analysis).** On compute-queue copies RADV moves ~17 GB/s down and 10–11 up (`.local/m109/p4t4/gb/gpubench.csv`); regions reach ~90% of that. The pipelined floor max(0, T − K·(n−1)/n) puts Erode 40 at 16k/24k at 66–68% exposed, and the measured 209 ms is within 8% of the 194 ms floor; 30% would need ~33.5 GB/s, beyond PCIe 4 x16. The real defect is `prepareWaitMs` (~110 of 406 ms at 16k): one helper thread faults and pins the whole 4 GiB destination (plus 1 GiB size-class headroom) before the first download → new P4.T8. Lone cheap UHD regions lose throughput when stripped with nothing to hide (127 MB in 15–18 ms vs ~10 ms whole), and the kernel-heavy defocus_retime 8K regions run whole with 0 hidden, probably refused by the ×4 halo rule → new P4.T9. The Blur V/H 2.29× at UHD (0.47 ms absolute; 16k passes at 1.79×) is accepted. The lone-Transform round-1 slowdown was a 264 ms cold pipeline build, so first-frame builds are reported, not gated; P8.T1's bench warms the GPU path. P4.T5's "≥ 20 GB/s" check becomes ≥ 85% of the same-boot GpuBench rate.
- 2026-10-20 — **M109.P4.T8 stopped at its first step; no code change.** Erode 40 at 16k for 6 frames (`.local/m109/p4t8/`, b42415fae release AppImage, MemAvailable ~122 GiB, idle GPU): the region's first two GPU frames each import 5 GiB new (the 4 GiB destination plus ~1 GiB of size-class headroom), with `prepareWaitMs` 0 and 110 ms. From the third GPU frame on, `newImportBytes` is 0, `prepareWaitMs` is 14–15 ms and wall is 297–299 ms, inside the task's own ≤ 20 ms / ≤ 310 ms targets. Chunking would only shorten one warm-up frame. P2.T1/P4.T4's 2-frame 16k/24k runs never reached steady state, which is why they showed the 110 ms wait; P8.T1 runs ≥ 4 frames. The re-import on the first two GPU frames is probably the host pool settling on reusable buffers (inferred from code, not traced).
- 2026-10-20 — **The first frame is the workload (user); P4.T8 reopened, P4.T10 added, gate and P8.T1 reworked.** Artists park on one frame and re-render it after knob and topology changes, then jump to an arbitrary frame; farm machines render one frame, sometimes 2–5. One-time costs are therefore part of the measurement, not warm-up: first-frame placement, pipeline compile, host import and pinning, and pool warm-up. This reverses today's earlier P4.T8 stop, which judged it by steady-state frames: comp frames 2 and 3 import 5 GiB each, and frame 3 waits 110 ms. It also reverses the "first-frame pipeline build is reported, not gated" amendment. The same check showed comp frame 1 of a fresh process rendering on the CPU; P4.T10 finds why. P8.T1 now measures a fresh-process single frame, same-frame re-renders, and a frame jump, with playback secondary.
- 2026-10-20 — **M109.P4.T9 (`97b4ae54e`), syntax-checked only; verify owed to the phase build and bench.** The defocus_retime 8K regions were refused by the ×4 cumulative-halo rule: region 6 (Blur 300 → Grade) has a 450-row halo, so it needed 1800-row strips against ~1560-row thirds of the 4685-row band; region 7 (Blur 100 → CC → Transform motion blur) has ~430 rows cumulative. The fixed multiple is gone. The planner now prices the candidate strips at the pipelined floor K′ + max(0, T′ − K′(n−1)/n), with K′ and T′ including re-uploaded halo rows and recomputed earlier-member rows, and strips only if that beats K + T whole. A whole region is also stripped only when K ≥ 0.25·T (`kGpuStripOverlapMinKernelRatio`); `pipelinesTransfers` applies the same rule, so placement and planning agree. Strip height now rounds up to the 4-row quantum, which removes a 5-row sliver strip that carried a full halo. The estimates are ~137 vs 152 ms for region 6 and ~141 vs 149 ms for region 7 (thin margin). `GpuCostModel::setGpuSeed` is new, for tests. `Tests/GpuPlacement_Test.cpp` was also edited, since the 0.25 rule changed three of its cases. The bench must check: the defocus regions show `overlapStrips:1` with 3 strips; no comp regresses (8K Grade+soften, the lone CheckerBoards and the final CC+Grade now run whole); lone Erode 40 at UHD/8K sits near the threshold (K/T 0.3–0.5). To tune 0.25, run with overlap forced on and forced off and put the threshold at the K/T crossover.
- 2026-10-20 — **M109.P4.T10 (`f7a058ecb`), syntax-checked only.** Cause: `GpuContext`'s constructor (`Engine/GpuContext.cpp:130`) initializes on its own thread: Vulkan device, then calibration (2 paths × 2 × 64 MB round trips). A fresh process places its first frame before that finishes (~170–190 ms after start on RADV), so `isGpuPlacementAvailable()` is false and the whole frame goes to the CPU with no region formed. Profile: frame 0 is all `"device":"cpu"` and took 12.25 s against ~3.8 s warm. Single-frame runs of Blur σ100 at 16k and keying_grade at UHD also went all-CPU. Fix: while startup is pending, `placeRenderGraph` makes a trial placement with transfers free (`GpuCostModel::regionGainBoundMs`), and waits for startup for at most that best-case gain, capped at 10 s after context creation. The real placement then runs with calibrated rates. `GpuContextOptions::beforeInit` is a test hook. Tests: `FirstFrameWaitsForTheGpuWhenItWouldWin` and `FirstFrameTheGpuCannotWinDoesNotWait`. Known cost: a frame the GPU only narrowly wins (lone Erode 40 at UHD, ~100 ms gain against ~180 ms of startup left) waits out its gain and still renders on the CPU, ~100 ms slower than before → new P4.T11 shortens startup. The implementer's edit to `Engine/Nodes/Image/NativeImageEffect.cpp` (accept runtime shaders while startup is pending) was backed out: M137 - Native Base Class Flexibility owns that file, and no node uses runtime shaders yet. Until it lands, runtime-shader nodes don't count towards the wait.
- 2026-10-20 — **M109.P4.T11 (`1cb23fcbb`), syntax-checked only.** Calibration now measures only the path regions take: import when host import is supported (RADV), otherwise staging; the other path's rates stay 0. The import round trip uses 16 MB; staging keeps 64 MB, because its 4 copy threads and 32 MB pieces need several pieces to show their rate. Calibration drops from ~8 × 64 MB to 4 × 16 MB on RADV, an estimated ~70–90 ms to ready. One `qInfo` line reports total startup, the runtime phase (loader, instance, device and runtime as one number, since splitting it needs `GpuRuntime.*`) and each calibration. It doesn't go through `infoLog`, because tests count those lines. The context is not made available before calibration finishes: renders would contend with the calibration transfers, and calibration's `trim()` already raced the idle-trim tests. Phase build to check: the log's real split on RADV; whether the 16 MB import rate matches the old 64 MB one (if low, raise `kImportCalibrationBytes` to 32 MB); `GpuContext_Test`; and a fresh-process single-frame Erode 40 UHD against Off.
- 2026-10-20 — **M109.P4.T8 (`4581d52f3`), syntax-checked only.**
  - **Readiness:** `GpuImportJob` keeps a count of ranges prepared so far, in order, and `waitFor(count)` blocks until it is reached. Strip k's chunk ends just after its last interior row, rounded up to 2 MiB, so strip k waits only for `_stripPrepared[k]`, and a later chunk never touches a page an earlier strip's download writes. The last strip waits for everything. Whole regions wait for the whole destination as before. Each chunk is an ordinary `prepareImport`, so `forget()`'s per-range mark and the cache-first lookup are unchanged.
  - **Size-class headroom is no longer imported (hard cut, reversing part of P5.T1):** `GpuTransferOptions::hostAllocationBytes` and the stretch-to-end-of-buffer in `resolveImports` are gone. A growing image (animated Transform) now imports only its growth each frame. `GrowingImageReusesTheBuffersAndImportsOfTheFrameBefore` now bounds the bytes imported instead of requiring 0 imports on frame 3.
  - **New test:** `GpuTransferTest.StripDownloadProceedsOnceItsChunkIsPrepared`, synchronised with condition variables only. It would hang rather than fail if strip downloads went back to waiting on the whole job.
  - **Bench to check (Erode 40 16k):** `prepareWaitMs` ≤ 20 ms, `newImportBytes` ~4 GiB rather than 5, and `downloadsStaged`/`importsAtDownload` 0.
- 2026-10-20 — **Local phase build of `4581d52f3` (`.local/m109/phase-4581d52/`; release, `NATRON_DEV_GPU=1`, no `RADV_PERFTEST`; AppImage `.local/m109/Natron-m109-4581d52f3-release.AppImage`).**
  - **Build:** the release build passes. The `-DNATRON_GPU=OFF` build fails: `VkDeviceSize` is unguarded at `GpuRegionTask.cpp:100` (P4.T8).
  - **Full ctest:** 2589/2589 on RADV and lavapipe in both modes. With no ICD, taskgraph is 2589/2589 and legacy 2587/2589. The two failures (`PyPlugExportTest.UserLayerSelectWithChannelButtonsSurvivesProjectRoundTrip`, `DimensionsFoldStateProjectTest.AnimatedFoldedKnobSavesOneCurveForEveryDimension`, both /tmp project I/O) pass alone, so they look like parallel-run flakes.
  - **Order coupling:** `GpuResidencyTest.FirstFrameWaitsForTheGpuWhenItWouldWin` fails when it runs after `GpuPlacement*` in one process. It passes under ctest and alone. Leaked cost-model state is suspected.
  - **Fresh-process single frame, Auto against Off, same boot, idle GPU:** every first frame formed a GPU region (P4.T10 confirmed).

    | Case | Off | Auto | Speed-up |
    |---|---|---|---|
    | Erode 40 16k | 8.27 s | 3.75–3.86 s | 2.2× |
    | Erode 40 UHD | 0.287 s | 0.203–0.207 s | 1.4× |
    | Blur σ100 16k | 13.24 s | 4.02–4.11 s | 3.2× |

    GPU startup is 43–45 ms (runtime 19–22 ms, import calibration 16–17 ms, staging 0), down from ~170–190 ms (P4.T11 confirmed). Erode 40 16k `newImportBytes` is exactly 4 GiB, with no headroom import, and 0 staged or at-download imports. **But `prepareWaitMs` is still ~100–106 ms against P4.T8's ≤ 20 ms target** (Blur σ100 16k: 46–55 ms). `overlapStrips` reads 0 on the pipelined 16k Erode. A fixer is on the GPU-off build, the test isolation and the prepare wait.
- 2026-10-20 — **Phase-build fixes (`82dd5c063`) and the prepare-wait floor.**
  - **GPU-off build:** `kPrepareChunkAlignment` moved inside the `NATRON_HAVE_GPU` block. A `-fsyntax-only` sweep with the `build/gpuoff` compile commands over every touched `.cpp` (9 Engine, 8 Tests) is clean.
  - **Test isolation:** `GpuPlacement.NoAvailabilityLeavesGraphUnchanged` ended with `setGpuPlacementAvailableFunc(nullptr)`, wiping the function `RenderScheduler` installs for the whole process. This leak already exists on main. `setGpuPlacementAvailableFunc` now returns the function it replaces, and the test restores it through a scope guard. The 207-test targeted filter passes in one process on RADV.
  - **Prepare wait is a host page-zeroing floor, not a defect.** Instrumented (`.local/m109/fix/`): chunking works on the budget-strip path, with 8 × 512 MiB ranges and strip k waiting only for range k. Per chunk, faulting costs 24–34 ms and pinning ~3 ms, so the helper prepares at ~16 GB/s while downloads run. In one 5-frame process, frame 1's region took 306 ms and frames 2–5 took 170 ms. Frame 2 still imports a new 4 GiB, but its pages are already faulted, and its wait is 8.6 ms. The whole ~136 ms fresh-process penalty is the kernel zeroing 4 GiB of new pages. More helper threads don't help: a standalone fault test tops out at 21.7 GB/s on 1 thread and ~24 GB/s on 2–8 (7800X3D single-CCD write bandwidth). Inside Natron, 2–4 threads stay at ~16.5 GB/s while downloads compete for memory. The CPU path also writes its output into fresh pages, so Off pays this zeroing too. `prepareWaitMs` sums per-strip waits that overlap downloads still in flight, so it overstates its wall cost. Its value also varies by boot (106 vs 256 ms) with the download rate. The ≤ 20 ms target can't be met for a fresh 4 GiB destination on this host.
  - **`overlapStrips` = 0 is correct** for a region stripped for budget; its pipelining shows as `stripsInFlight: 3`.
