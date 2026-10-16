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

- [ ] M109.P1.T1 — Time each member with device timestamps and break region wall time into host phases
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
- [ ] M109.P1.T2 — Reproduce the empty-input hang as a failing test that times out
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
- [ ] M109.P1.T3 — Commit the region bench driver and its time breakdown
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

- [ ] M109.P2.T1 — Quick directional profile pass (~1 h) to explain Transform "other", the 24k transfer rise and the kernelMs gap
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
- [ ] M109.P2.T2 — Find the root cause of the empty-input hang
  - files: none (findings in `## Decisions`)
  - approach:
    - Run the P1.T2 disabled cases with `NATRON_RENDER_PROFILE` and the region error log.
    - Where the container's ptrace ban blocks a debugger, render the equivalent `.ntp` with NatronRenderer from the fast AppImage on the host, and take `gdb -batch -ex 'thread apply all bt' -p` while it hangs.
    - Name the waiting thread and what it waits on. Candidates: the GPU kernel reading an empty view, an unscheduled dependency, or a wait in the fallback path.
  - verify: Decisions names the blocking call with `file:line` and the fix P3.T1 makes.
  - size: M

## Phase 109.3: Correctness and fallback budget

- [ ] M109.P3.T1 — Make a member with an empty input render correctly or refuse to the CPU, never hang
  - files: `Engine/GpuRegionTask.cpp`, plus the file P2.T2 names (expected `Engine/RenderScheduler.cpp` or a node's `renderGpu`), `Tests/GpuRegionFallback_Test.cpp`
  - approach:
    - Apply the P2.T2 fix.
    - Also make `plan()` treat an empty or absent input of a non-mask input as either an explicit refusal (`FailureKind::Refused`) or a valid empty view the node accepts. The behaviour must be the same in the unstripped and the strip paths.
  - verify: At the phase build, the P1.T2 cases are re-enabled and pass on lavapipe, RADV and no ICD in both modes, with no timeout.
  - size: M
- [ ] M109.P3.T2 — Give a region's CPU fallback the thread budget a CPU task would get
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

- [ ] M109.P4.T1 — Strip regions that fit whole when pipelining can hide their transfer
  - files: `Engine/GpuRegionStrips.{h,cpp}`, `Engine/GpuRegionTask.cpp` (`planStrips`), `Tests/GpuResidency_Test.cpp`
  - approach:
    - Add a `GpuStripLimits::overlapMinBytes` and an overlap target: when the region is `Whole`, pipelining is allowed, and no output is kept on the device, plan at least `kGpuStripMaxInFlight` strips. This applies when its host I/O bytes are ≥ the threshold (start at 32 MiB, tuned in P4.T4).
    - Strips stay ≥ `kGpuStripMinPipelinedRows`, and at least 4× the largest halo of the region's members. If not, the region stays whole.
    - Count these regions separately in the stats.
  - verify:
    - At the phase build, unit tests on `planGpuStrips` cover: whole region with a large I/O → strips; a kept output → whole; a large halo → whole; no pipelining → whole.
    - A residency test with `GpuStripPipelining::On` on lavapipe and RADV renders a lone Grade at UHD as ≥ 3 strips, bit-equal to whole.
  - size: M
- [ ] M109.P4.T2 — Pin download destinations ahead of time on a helper thread
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
- [ ] M109.P4.T3 — Charge placement the transfer a region exposes, not the transfer it hides (if P2.T1 confirms a misprediction)
  - files: `Engine/GpuCostModel.{h,cpp}` (`regionGainMs`, new `observeTransfer`), `Engine/GpuRegionTask.cpp` (`noteSuccess`), `Tests/GpuPlacement_Test.cpp`
  - approach:
    - The gain counts exposed transfer: max(0, transfer − the kernel it overlaps) for a region that P4.T1 would strip, and full transfer otherwise.
    - The region's measured exposed ms per byte feeds a per-session EMA that corrects the calibrated rate. This lets placement refuse a lone cheap point op whose transfer outweighs its saving on a slower bus.
  - verify: At the phase build, placement tests show:
    - a lone Grade with a halved transfer rate is refused;
    - the same Grade on this bus, pipelined, is accepted;
    - the EMA moves the rate toward observed samples.
  - size: M
- [ ] M109.P4.T4 — Quick directional bench of the overlap phase
  - files: none (results in `## Decisions`)
  - approach:
    - Rerun P2.T1's comps and node/gxg cases at UHD and 8K with `gpu_region_bench.sh`.
    - Tune `overlapMinBytes` and record the chosen value.
  - verify: Exposed transfer share on keying_grade and cg_multipass drops below 30% of region time. No Auto case is slower than Off in any round, and no comp regresses against M85's ratios.
  - size: M

## Phase 109.5: Root-cause fixes and host memory

- [ ] M109.P5.T1 — Remove the cause of plain Transform's "other" time and its 4× exposed transfer
  - files: as P2.T1 finds. Candidates are `Engine/GpuRegionTask.cpp` (buffer sizing and reuse), `Engine/Nodes/Transform/Transform.cpp` (`renderGpu`, source RoI) and `Engine/Gpu/GpuBufferPool.{h,cpp}` (size classes), plus `Tests/GpuResidency_Test.cpp`
  - approach: Fix the named cause without touching M126 - Runtime GPU Shaders' files. If the fix needs `GpuKernel.cpp` (for example, pipeline creation per dispatch), move it to Phase 109.7 and flag it.
  - verify: At the phase build, a test asserts the specific invariant, for example no pool miss on the second frame of an animated Transform. A directional bench shows Transform "other" below 20% of region time at UHD and 16k.
  - size: M
- [ ] M109.P5.T2 — Stop exposed transfer rising at 24k (if P2.T1 traces it to fresh host buffers)
  - files: `Engine/HostImageMemory.{h,cpp}`, `Tests/Image_Test.cpp`
  - approach:
    - Let the pool hold at least one frame's working set of same-size buffers, even when that is above RAM/4. Bound it by `MemAvailable`, probed at each grow, so it never assumes a machine.
    - Or apply the cause P2.T1 found instead.
  - verify: At the phase build, unit tests cover pool growth beyond the default cap when memory is available and refusal when it is not. A directional 24k bench of ChromaKeyer and Erode 40 shows exposed transfer at or below 0.1 ms/Mpx.
  - size: M
- [ ] M109.P5.T3 — Trim idle pooled host memory to a floor
  - files: `Engine/HostImageMemory.{h,cpp}`, `Tests/Image_Test.cpp`
  - approach:
    - Record the time of the last allocate or release.
    - A lazily started reaper thread wakes every few seconds and calls `trimTo(floor)` once the pool has been idle for longer than the idle period. The `GpuContext` free hook then drops the cached imports, which unpins them.
    - Make the floor and idle period constants, with a test hook to shorten them.
    - The thread stops at shutdown.
  - verify: At the phase build, a test fills the pool, waits past a shortened idle period, and sees `pooledBytes()` at the floor. A release during the wait resets the timer.
  - size: M

## Phase 109.6: Retry ladder and Blur narrow pass

- [ ] M109.P6.T1 — Remember the strip height that worked for each region shape
  - files: `Engine/GpuRegionTask.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach:
    - Keep a session map, keyed by device generation and region shape (member plugin IDs and parameters hash, output bounds, nComps), to the last successful `forcedRows`/serial.
    - `runGpuRegion` starts from the remembered entry. The map is cleared on device loss or a runtime rebuild.
  - verify: At the phase build, with the pool budget capped so a heavy-scratch region halves on frame 1, frame 2 has `retries == 0`. This holds on lavapipe and RADV.
  - size: M
- [ ] M109.P6.T2 — Wait briefly for a contending region before halving strips (only if P2.T1 saw contention halvings)
  - files: `Engine/Gpu/GpuBufferPool.{h,cpp}` (release notification), `Engine/GpuRegionTask.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach: On out-of-memory, when another region holds pool bytes, wait on a pool release condition (bounded at ~50 ms) and rerun at the same height before halving.
  - verify: At the phase build, two concurrent regions over a capped budget complete without a halving.
  - size: M
- [ ] M109.P6.T3 — Bring Blur's narrow vertical pass (r ≤ 32) near its horizontal pass
  - files: `Engine/Gpu/GpuBlur.cpp`, `Engine/Gpu/Kernels/blur.slang`, `Tests/Gpu/BlurGpu_Test.cpp`
  - approach: Route r ≤ 32 through the shared-tile column path that already wins for wide σ, or tune the coalesced path, inside the existing `blurPass` entry. Do not add an entry point.
  - verify: At the phase build, the Blur parity tests pass on lavapipe and RADV. GpuBench (`run-host.sh`) σ3 UHD V/H is ≤ 2×, against ~10× before.
  - size: M

## Phase 109.7: Shared-file follow-ups (FLAGGED: touch files other milestones own)

- [ ] M109.P7.T1 — ⚑ Record one edge per input in `collectPreRenderFrames` (touches M100 - Content Node Hashing's `ParallelRenderArgs.cpp`; start after M100 merges)
  - files: `Engine/ParallelRenderArgs.cpp`, `Engine/GpuRegionTask.cpp` (delete the compensation at `:597-615`), `Tests/SchedulerEquivalence_Test.cpp`
  - approach: Key `PreRenderFrames` by (effect, inputNb), so a node feeding two inputs yields two edges, as `FrameGraph::Task` documents. Delete the region's edge re-derivation.
  - verify: At the phase build, a node wired to a member's source and mask gives two dependencies, and CPU and GPU renders are equal in both modes.
  - size: M
- [ ] M109.P7.T2 — ⚑ Add Erode size and Blur filter to `GpuTaskTraits` and seed them (touches `GpuCostSeed.inc`, which M90 - Render Path Serial Overhead regenerates; start after M90 merges)
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
  - verify: The gate below holds. The idle Natron's pooled host bytes, read after 2× the idle period, are at the floor.
  - size: M

**Verification gate:**
- Full ctest passes on lavapipe, RADV and no ICD, in both scheduler modes, and `-DNATRON_GPU=OFF` builds.
- Every P1.T2 case finishes with no timeout.
- On the M82 comps:
  - no region halves its strips after its first frame;
  - contention halvings are 0, or explained in Decisions;
  - region exposed transfer in the comps is under 30% of region time;
  - plain Transform "other" is under 20%.
- Every speed-up is ≥ M85's, and no Auto case is slower than Off in any round.
- CPU-only renders are within ±2% of `main`.
- A forced CPU fallback of a big Blur runs within 15% of Off.
- Per-member times in profiles come from timestamps.

## Decisions

- 2026-10-16 — **Elaborated at claim by the gpu PM** from the stub's follow-up list. The stub's own facts are folded into Scope and the scouted facts. Task files avoid every file of M126 - Runtime GPU Shaders, which runs alongside, and every file in the unchecked tasks of M90 - Render Path Serial Overhead and M100 - Content Node Hashing, except the two flagged Phase 109.7 tasks, which wait for those merges. Multi-plane placement moved Out to M110 - Remaining Native GPU Ports, because it needs `GpuRender.h`.
