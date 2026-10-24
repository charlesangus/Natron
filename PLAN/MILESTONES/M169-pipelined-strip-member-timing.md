# M169 - Pipelined Strip Member Timing

In pipelined GPU strips, each member's device time is inflated. On the RTX 3070 Ti, Grade measures 0.21 ms pipelined against 0.045 ms serial. `GpuRegionTask::noteSuccess` (`Engine/GpuRegionTask.cpp:2290-2301`) passes those times straight to `GpuCostModel::observeGpu`, and `observeGpu` folds every sample into `kernelMsPerMpx` by EMA (`Engine/GpuCostModel.cpp:769-791`). So pipelined regions skew calibration, and through `predictedKernelMs` (`:666`) they also skew strip planning. In pipelined mode, `measureStripMembers` (`:2207-2229`) charges each member from the stamp before it. Member 0's interval starts at the strip's begin stamp (`submitStrip` `:1942`), so it also covers the `acquireFromTransfer` ownership barriers (`:1944-1949`). Every interval also runs while the transfer queue serves strip k+1's uploads and strip k-1's downloads (`pipelineStrips` `:2031-2081`). Last, `observeGpu(task, ns)` divides by the whole task's megapixels (`megapixelsOf`), but the strips actually compute more rows than that because of halos. M91.P2.T5 only relaxed `RegionMembersAreTimedOnTheDevice` (`Tests/GpuResidency_Test.cpp:1907`) to a nesting check (`DECISIONS/2026-10-24-nvidia-pipelined-strip-timing-assert.md`). Only the barrier cost is evidenced (M91's decision); transfer contention and halo megapixels are suspects read from the code, not measured. This milestone measures the real cause first, fixes the attribution it finds, and adds a test that would catch the inflation. Filed 2026-10-24 at M91 - NVIDIA GPU Validation's request.

Deps: M107 - GPU Viewer Hand-off (its `Engine/GpuRegionTask.cpp` changes) and M91 - NVIDIA GPU Validation (`stagedReasons()` and the relaxed test).

## Scope

**In:**
- Per-strip, per-member raw intervals in `RegionReport` and the profile, plus a diagnosis on `bosley-faux` (NVIDIA).
- Ownership-barrier time split out of member time, and calibration samples that exclude transfer-contended device time and use the megapixels actually computed.
- A test that fails when pipelined calibration diverges from serial.

**Out:**
- Changes to the strip planner (`Engine/GpuRegionStrips.*`), the transfer queue design, and the EMA itself.

**Edge cases:**
- A node that only ever runs in pipelined regions may have no uncontended interval. It then keeps its seed or serial row; the sample is skipped and counted, never forced in.
- Devices without timestamps keep the existing `chargeByWeight` path unchanged (`:2183-2188`).
- Identity members and members with an empty strip rect have no stamp (`:1660-1662`).

**Assumptions:**
- The semaphore wait already uses `ALL_COMMANDS` (`Engine/Gpu/GpuTransfer.h:55`), so the begin stamp does not include upload queue wait. The suspected causes are the acquire barriers, transfer contention and halo megapixels. P1.T3 confirms which.

## Phase 169.1: Instrument and diagnose

- [ ] M169.P1.T1 — Record raw per-strip member intervals and the strip's ownership-barrier stamps in the region report
  - files: `Engine/GpuRegionTask.cpp`, `Engine/RenderScheduler.h`, `Engine/RenderScheduler.cpp`, `Engine/RenderStats.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach: In `submitStrip`, add one stamp after the acquires and before the first member (extend the `_queriesPerStrip` layout in `startPipeline` `:1891-1903`). Add `RegionReport::memberIntervals` (strip, member, begin/end ns relative to the region, output rows×width) and `ownershipMs` (acquire plus release intervals). Fill them in `notePipelineTimes`/`measureStripMembers`. In serial `recordStrip`/`submit`, fill them from the segment stamps. Emit them in the profile region record only when profiling is on. Member attribution starts at the new stamp, so `memberNs` stops including the acquires.
  - verify: `RegionMembersAreTimedOnTheDevice` checks `memberIntervals` count = strips × non-identity members, that intervals are monotone within each strip, and that the pipelined sum of member time plus `ownershipMs` matches `kernelMs` within 5%. That restores the strict check M91.P2.T5 relaxed. Lavapipe CI green.
  - size: M

- [ ] M169.P1.T2 — Gate build for the instrumentation
  - files: none in code
  - approach: one `tools/ci/remote/build.sh fast --tests` on the milestone HEAD. Download `build-tree-fast` using the M91 recipe (`.local/m91/p2/`).
  - verify: lavapipe CI legs green. The `build-tree-fast` artifact is available for P1.T3.
  - size: S

- [ ] M169.P1.T3 — Classify the inflation on the RTX 3070 Ti and record the fix decision
  - files: this milestone file (`## Decisions` and a findings section); output in `.local/m169/p1/`
  - approach: On `bosley-faux`, run `RegionMembersAreTimedOnTheDevice` 5× and render `tools/bench/comps/keying_grade.ntp` with `NATRON_RENDER_PROFILE`. For each member interval, split the inflation over serial into four parts: (a) acquire/release time now in `ownershipMs`; (b) overlap with transfer-busy intervals (compare a member's time in strips with overlap against strips without); (c) halo rows (strip output rows against task RoI rows); (d) the remainder. Record the numbers. Then rewrite or strike P2.T2 and P2.T3 to match: they stay only if transfer contention or halo megapixels is a real share of the gap. If contention stays, put the no-clean-sample policy (skip and count, an occasional serial probe, or correcting contended samples) to the user and record it in `## Decisions`.
  - verify: the findings table accounts for at least 90% of the Grade pipelined-serial gap, or names the unexplained remainder and adds a follow-up task.
  - size: M

## Phase 169.2: Attribution fix

- [ ] M169.P2.T1 — Split strip ownership-barrier time out of member time in serial and whole segments too
  - files: `Engine/GpuRegionTask.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach: In `beginTimedSegment`/`segmentDeviceNs` (`:948-997`), the member-0 interval starts at query 0 and so includes the barrier before it. Mirror P1.T1's post-acquire stamp wherever a segment opens with barriers, so that across all three modes members own only their dispatches and the barriers between them. Barrier time goes to `ownershipMs`.
  - verify: all three modes of `RegionMembersAreTimedOnTheDevice` pass the 5% equality check, now including `ownershipMs`. Lavapipe CI green.
  - size: M

- [ ] M169.P2.T2 — (conditional on P1.T3) Add a pure calibration-sample helper that drops transfer-contended intervals and uses computed megapixels
  - files: `Engine/GpuMemberTiming.h` (new), `Engine/GpuMemberTiming.cpp` (new), `Tests/GpuMemberTiming_Test.cpp` (new), `Tests/CMakeLists.txt` (if the test is not globbed)
  - approach: Input: per-member intervals with pixel counts, plus the transfer-busy union (`unionOf`/`overlapOf` move here from `GpuRegionTask.cpp:2083-2132` only in P2.T3). Output per member: clean ns, clean megapixels, and how many intervals were skipped. An interval counts as contended when more than X% of it overlaps transfer-busy time. P1.T3 sets X, with 10% as the starting value. No Vulkan.
  - verify: the gtest checks fully clean, fully contended, partially overlapping, halo-row and empty inputs, and that a 4× inflated contended interval does not move the sample. Runs without a GPU.
  - size: M

- [ ] M169.P2.T3 — (conditional on P1.T3) Calibrate GpuCostModel from the helper's samples and add the catching test
  - files: `Engine/GpuRegionTask.cpp`, `Engine/RenderScheduler.h`, `Engine/RenderStats.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach: `noteSuccess` calls `observeGpu(traits, cleanNs, cleanMpx)` and skips members that have no clean interval. It counts the skipped samples into a `RenderStats` counter and the region record (`calibrationSkipped`). `memberNs` stays the actual device time spent. Move the interval helpers into `GpuMemberTiming`. New test `PipelinedStripsCalibrateLikeSerial`: same Blur→Grade graph; `GpuCostModel::session().reset()`; run serial strips and read `gpuMs(Grade traits, 1.0)`; reset; run pipelined and read again. Expect the ratio to fall in [0.67, 1.5] when a sample was taken, and expect a nonzero `calibrationSkipped` otherwise. Print the raw `memberNs`/Mpx against the calibrated value.
  - verify: lavapipe CI green. On `bosley-faux` the new test passes 5/5, and its printout shows a raw Grade ratio above 2 alongside a calibrated ratio within bounds. That shows the test exercises the inflation.
  - size: M

## Phase 169.3: Gate

- [ ] M169.P3.T1 — Gate build and hardware runs
  - files: none in code; output in `.local/m169/p3/`
  - approach: one `tools/ci/remote/build.sh fast --tests`. On `bosley-faux`: `ctest -L gpu`, and the engine ctest with `NATRON_GPU=force` under both schedulers. Repeat the keying_grade profile and compare Grade/Blur `kernelMsPerMpx` after pipelined frames against a serial-only run (`setGpuStripPipelining` off). On the user's RADV host: `ctest -L gpu` as a no-regression check.
  - verify: everything passes on lavapipe CI, `bosley-faux` and RADV. On NVIDIA, the pipelined and serial calibrations are within 1.5×.
  - size: M

## Decisions

- 2026-10-24 — **The contention fix waits on measurement:** P2.T1 (barrier time out of member time) is evidenced by M91's NVIDIA results and goes ahead. P2.T2 and P2.T3 are rewritten or struck by P1.T3's findings, and the no-clean-sample policy is decided then (user, grill).
- 2026-10-24 — **Deps M107 and M91:** every task but P2.T2 edits `Engine/GpuRegionTask.cpp`, which M107 changes, and the work builds on M91's P2.T2 and P2.T5 (user, grill).

**Verification gate:** remote `fast --tests` CI green on lavapipe. `ctest -L gpu` and the engine ctest (`NATRON_GPU=force`, both schedulers) pass on `bosley-faux` (RTX 3070 Ti) and on the RADV host. The restored 5% `RegionMembersAreTimedOnTheDevice` check passes 5/5 on `bosley-faux`, and so does `PipelinedStripsCalibrateLikeSerial` if P2.T3 survived P1.T3. keying_grade pipelined calibration is within 1.5× of serial.
