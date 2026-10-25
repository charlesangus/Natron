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
- Devices without timestamps keep the existing `chargeByWeight` path unchanged (`GpuRegionTask.cpp:1261`, used at 2769).
- Identity members and members with an empty strip rect have no stamp (`GpuRegionTask.cpp:2221-2223`).

**Assumptions:**
- The semaphore wait already uses `ALL_COMMANDS` (`Engine/Gpu/GpuTransfer.h:55`), so the begin stamp does not include upload queue wait. The suspected causes of Grade's inflation are transfer contention and halo megapixels; barriers touch only member 0 and `kernelMs`. P1.T3 confirms which.

## Phase 169.1: Instrument and diagnose

- [x] M169.P1.T1 — Record raw per-strip member intervals, the strip's ownership-barrier time and transfer begin/end in the region report
  - files: `Engine/GpuRegionTask.cpp`, `Engine/RenderScheduler.h`, `Engine/RenderScheduler.cpp`, `Engine/RenderStats.h`, `Engine/RenderStats.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach: In `submitStrip` (`GpuRegionTask.cpp:2500-2562`), add one stamp after the acquires (2515-2520) and before the first member (2521); extend the query layout in `createQueries` (`_memberStamp0`/`_queriesPerStrip` 2463-2464). Add `RegionReport::memberIntervals` (strip, member, begin/end ns relative to the region, output rows×width) and `ownershipMs` (pipelined only: acquire plus release intervals; whole and serial segments hold no ownership barrier inside the timed span — acquires precede the begin stamp at 1677/1683, releases follow the end stamp at 1696/1704). Fill intervals in `notePipelineTimes` (2704)/`measureStripMembers` (2779), serial `recordStrip` (2275)/`submit` (~1273), and whole chained segments in `noteSegmentTimes` (~1838) from `segmentDeviceNs` stamps. Pipelined member attribution starts at the new stamp, so `memberNs` stops including the acquires. Do not build new transfer stamps: add region-relative `beginNs`/`endNs` to `GpuTransferRecord` (`RenderStats.h:~250`), fill them in `noteTransferTimes` (1422) and emit them beside `deviceMs` in `appendTransferRecords`. Put the new `RegionReport` fields beside the staged counters (`RenderScheduler.h:~298`), copy them in `RenderScheduler.cpp:1267-1300`, and emit them only when profiling is on / nonempty, following the `stagedReasons` pattern (`RenderStats.cpp:855`).
  - verify: `RegionMembersAreTimedOnTheDevice` (`Tests/GpuResidency_Test.cpp:2060`) checks `memberIntervals` count = strips × non-identity members, that intervals are monotone within each strip, and that the pipelined sum of member time plus `ownershipMs` matches `kernelMs` within 5% — restoring the strict check M91.P2.T5 relaxed (2144-2152). Whole and serial keep their 5% check (2154). Lavapipe CI green.
  - size: M

- [x] M169.P1.T2 — Gate build for the instrumentation
  - files: none in code
  - approach: one `tools/ci/remote/build.sh fast --tests` on the milestone HEAD. Download `build-tree-fast` using the M91 recipe (`.local/m91/p2/`).
  - verify: lavapipe CI legs green. The `build-tree-fast` artifact is available for P1.T3.
  - size: S

- [x] M169.P1.T3 — Classify the inflation on the RTX 3070 Ti and record the fix decision
  - files: this milestone file (`## Decisions` and a findings section); output in `.local/m169/p1/`
  - approach: Ownership barriers cannot explain Grade's (member 1's) inflation: in pipelined strips they affect only member 0 and `kernelMs`. Since M156's chained submission, whole regions are contended too (downloads overlap later segments), so classify all three modes, with serial strips as the uncontended baseline. On `bosley-faux`, run `RegionMembersAreTimedOnTheDevice` 5× and render `tools/bench/comps/keying_grade.ntp` with `NATRON_RENDER_PROFILE`, plus a control run with `NATRON_GPU_KEEPALIVE=0`. For each member interval split the inflation over serial into: (a) acquire/release time now in `ownershipMs` (member 0 only); (b) overlap with transfer-busy intervals, from the profile's `uploads`/`downloads` begin/end (P1.T1); (c) halo rows (strip output rows against task RoI rows); (d) keep-alive effect; (e) the remainder, e.g. `ALL_COMMANDS` stamp semantics at the `computeBarrier`. Record the numbers. Then rewrite or strike P2.T2 and P2.T3: they stay only if transfer contention or halo megapixels is a real share of the gap. If contention stays, put the no-clean-sample policy (skip and count, an occasional serial probe, or correcting contended samples) and the whole-region-without-transfer-stamps policy (see P2.T3) to the user and record them in `## Decisions`.
  - verify: the findings table accounts for at least 90% of the Grade pipelined-serial gap, or names the unexplained remainder and adds a follow-up task.
  - size: M

## Findings (P1.T3, 2026-10-25, RTX 3070 Ti, build from run 40188846698)

Transfer contention is the cause. Member intervals that overlap an upload/download run 1.1–8.2× slow (Grade 8.2× at 512², 1.3× at UHD); clean pipelined intervals run at 0.93–1.01× serial, so there is no constant stamp bias.

| set | member | serial ms | pipelined ms | gap | ownership | contention | halo | keep-alive | spikes | rest |
|---|---|---|---|---|---|---|---|---|---|---|
| test graph (10 runs) | Grade | 0.043 | 0.187 | 0.143 | 0 (outside member) | 0.145 (101%) | 0 | −0.022 | 0 | −1% |
| test graph | Blur | 0.364 | 0.601 | 0.237 | 0 | 0.211 (89%) | 0 | −0.111 | 16% | −5% |
| keying_grade | bg_grade | 1.220 | 2.584 | 1.364 | 0 | 82% | 0 | −1.165 | 27% | −9% |
| keying_grade | final_grade | 0.599 | 0.725 | 0.126 | 0 | 116% | 0 | −0.028 | 0 | −20% |
| keying_grade | matte_erode | 9.349 | 19.965 | 10.615 | 0 | 120% | 0 | 3.060 | −14% | −6% |

- Ownership per pipelined region 0.11–0.42 ms (13–37% of `kernelMs`), now outside every member. The strict 5% check passed 20/20 on NVIDIA in both schedulers (with and without keep-alive).
- Halo: strips compute up to +11% more pixels than `megapixelsOf` (bg_grade), equally in serial and pipelined.
- Whole regions: 0 of 343 intervals overlapped a transfer; no multi-segment chained region occurred, so their contention is unmeasured.
- Clean-sample availability: 91% of (region, member) pairs in 4–5-strip pipelined regions, 81% with the planner's 3-strip regions.
- bg_grade's 27% remainder is spikes (2–16× slow intervals with no overlap) also seen serial and whole; the desktop's other GPU users (22–50% utilisation) time-slice the GPU. Follow-up filed.
- Data and scripts: `.local/m169/p1/` (results-test.md, results-comp.md, clean-stats.md).

## Phase 169.2: Attribution fix

- [x] M169.P2.T1 — ~~Split strip ownership-barrier time out of member time in serial and whole segments too~~ (struck 2026-10-25: whole and serial segments hold no ownership barrier inside the timed span; see Decisions)
  - files: none
  - approach: struck.
  - verify: none.
  - size: S

- [x] M169.P2.T2 — Add a pure calibration-sample helper that drops transfer-contended intervals and uses computed megapixels
  - files: `Engine/GpuMemberTiming.h` (new), `Engine/GpuMemberTiming.cpp` (new), `Tests/GpuMemberTiming_Test.cpp` (new), `Tests/CMakeLists.txt` (if the test is not globbed)
  - approach: Input: per-member intervals with pixel counts, plus the transfer-busy union (`unionOf`/`lengthOf`/`overlapOf` move here from `GpuRegionTask.cpp:2655-2701` only in P2.T3; intervals come from the existing pair queries, `queryOf` 2418). Output per member: clean ns, clean megapixels, and how many intervals were skipped. An interval counts as contended when more than X% of it overlaps transfer-busy time. X = 10% (P1.T3: overlap is almost always 0 or >50%; any X in 5–25% classifies the same). Pinned downloads are not timestamped (`createQueries` skips `pinned` slots), so their contention is invisible to the helper; name that in the header. No Vulkan.
  - verify: the gtest checks fully clean, fully contended, partially overlapping, halo-row and empty inputs, and that a 4× inflated contended interval does not move the sample. Runs without a GPU.
  - size: M

- [x] M169.P2.T3 — Calibrate GpuCostModel from the helper's clean samples and add the catching test
  - files: `Engine/GpuRegionTask.cpp`, `Engine/RenderScheduler.h`, `Engine/RenderStats.cpp`, `Tests/GpuResidency_Test.cpp`
  - approach: `noteSuccess` (`GpuRegionTask.cpp:~2949`, `observeGpu` call ~2972) calls the existing `observeGpu(traits, cleanNs, cleanMpx)` overload (`GpuCostModel.h:240`) with the helper's clean ns and computed megapixels. Pipelined strips: a member with no clean interval feeds no sample and is counted (skip and count). Serial strips: all intervals are clean. Whole regions (no transfer stamps outside profiling): single-segment regions are clean (their download follows the last member); in multi-segment chained regions, a member submitted after an earlier segment's download was issued is skipped and counted, decided from submission order, no timestamps. Do not add a per-region query-pool reset (its host round trip idles the GPU, M156). Count skipped samples into a `RenderStats` counter and the region record (`calibrationSkipped`, emitted only when nonzero). `memberNs` stays the actual device time spent. Move the interval helpers (`unionOf`/`lengthOf`/`overlapOf`) into `GpuMemberTiming`. New test `PipelinedStripsCalibrateLikeSerial`: same Blur→Grade graph; `GpuCostModel::session().reset()`; run serial strips and read `gpuMs(Grade traits, 1.0)`; reset; run pipelined and read again. Expect the ratio in [0.67, 1.5] when a sample was taken, and a nonzero `calibrationSkipped` otherwise. Print the raw `memberNs`/Mpx ratio against the calibrated one (printout only, not a pass condition).
  - verify: lavapipe CI green. On `bosley-faux` the new test passes 5/5 and its printout shows the raw Grade ratio alongside a calibrated ratio within bounds.
  - size: M

## Phase 169.3: Gate

- [ ] M169.P3.T1 — Gate build and hardware runs
  - files: none in code; output in `.local/m169/p3/`
  - approach: one `tools/ci/remote/build.sh fast --tests`. On `bosley-faux`: `ctest -L gpu`, and the engine ctest with `NATRON_GPU=force` under both schedulers. Repeat the keying_grade profile and compare Grade/Blur `kernelMsPerMpx` after pipelined frames against a serial-only run (`setGpuStripPipelining` off). On the user's RADV host: `ctest -L gpu` as a no-regression check.
  - verify: everything passes on lavapipe CI, `bosley-faux` and RADV. On NVIDIA, the pipelined and serial calibrations are within 1.5×.
  - size: M

## Decisions

- 2026-10-24 — **The contention fix waits on measurement:** P2.T1 (barrier time out of member time) is evidenced by M91's NVIDIA results and goes ahead. P2.T2 and P2.T3 are rewritten or struck by P1.T3's findings, and the no-clean-sample policy is decided then (user, grill).
- 2026-10-25 — **Calibration policy (P1.T3):** skip and count when a member has no clean interval (user: no preference; correction is unreliable at 1.0–8.2× slowdown, a serial probe isn't needed yet). Whole regions: single-segment ones are clean; in multi-segment chained ones, members submitted after an earlier segment's download are skipped and counted, by submission order (user, option A). Contention threshold 10%. `PipelinedStripsCalibrateLikeSerial` keeps [0.67, 1.5] but the raw-ratio-above-2 condition becomes a printout (it would fail ~1 run in 5).
- 2026-10-25 — **Freshness check after M107, M156 and M168 merged** (consultant, against main dc8774f9a): line refs in the intro paragraph predate M156 (`noteSuccess` now `GpuRegionTask.cpp:2949-2984`, `observeGpu` `GpuCostModel.cpp:773-802`, `predictedKernelMs` is now `Member::predictedKernelMs` in `GpuRegionTask.cpp:310/869`, `pipelineStrips` 2601-2651, `RegionMembersAreTimedOnTheDevice` `Tests/GpuResidency_Test.cpp:2060`). P2.T1 struck: in whole regions acquires precede the begin stamp and releases follow the end stamp, and serial strips have no ownership barriers, so only pipelined strips need the split, which P1.T1 does. P1.T1 reuses M156's per-transfer timestamp pairs (adds begin/end to `GpuTransferRecord`) instead of new stamps. P1.T3 and P2.T3 now cover whole chained regions, which M156 made contended. Claimed despite file overlap with M64 - Tiled Rendering, M101 - Clean-Sheet Merge Node and M146 - Roto Hosting For Any Node (user, 2026-10-25).
- 2026-10-24 — **Deps M107 and M91:** every task but P2.T2 edits `Engine/GpuRegionTask.cpp`, which M107 changes, and the work builds on M91's P2.T2 and P2.T5 (user, grill).

**Verification gate:** remote `fast --tests` CI green on lavapipe. `ctest -L gpu` and the engine ctest (`NATRON_GPU=force`, both schedulers) pass on `bosley-faux` (RTX 3070 Ti) and on the RADV host. The restored 5% `RegionMembersAreTimedOnTheDevice` check passes 5/5 on `bosley-faux`, and so does `PipelinedStripsCalibrateLikeSerial` if P2.T3 survived P1.T3. keying_grade pipelined calibration is within 1.5× of serial.
- 2026-10-25 — **`PipelinedStripsCalibrateLikeSerial` compares backed-out samples:** one observation moves a seeded EMA row only `kEmaWeight` (0.25) of the way, so raw `gpuMs` ratios would almost always pass. The test recovers the sample as `before + (after − before) / kEmaWeight`. `calibrationSkipped` counts task positions.
