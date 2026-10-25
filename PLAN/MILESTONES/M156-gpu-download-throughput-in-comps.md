# M156 - GPU Download Throughput In Comps

M109 - GPU Region Engine Hardening's thorough bench (M109.P8.T1) measured fused-comp regions exposing 1.6–2.2× the GpuBench-rate transfer floor (keying_grade, cg_multipass, defocus_retime at UHD and 8K), with downloads running at about 6 GB/s. GpuBench's host-import download rate on the same boot was 16.7 GB/s, and single-node 16k/24k regions sit at 0.9–1.08× of that floor. So the bus is not the limit in comps; something in how a fused region downloads is. This milestone finds where that time goes, then fixes the measured causes.

Source: the gpu PM's inbox request (2026-10-20). Data on the GPU host in `.local/m109/p8t1/analysis/final/regions.md` (also `ratios.md`, `pw_gate.txt`); boot `823970cf`, AppImage `4581d52f3`.

Candidates named in the request: many small per-member downloads, an image layout or format conversion on the download path, and an unpipelined last strip.

Scouted facts (main `31636d20d`, 2026-10-20; re-check at promotion):
- Region execution and stripping: `Engine/GpuRegionTask.{h,cpp}`, `Engine/GpuRegionStrips.{h,cpp}`; placement cost: `Engine/GpuCostModel.{h,cpp}`, `Engine/GpuPlacement.{h,cpp}`; buffers and transfers: `Engine/Gpu/GpuBuffer.cpp`, `Engine/Gpu/GpuBufferPool.cpp`, `Engine/Gpu/GpuRuntime.cpp`, `Engine/Gpu/GpuRecord.cpp`.
- Bench tooling: `tools/bench/gpu_region_bench.sh` (kinds `a-cold`, `a-warm`, `bc`, `playback`; Auto vs Off, interleaved), `tools/bench/gpu_region_summary.py` (pipelined floor from `--import-rate`), `tools/bench/comps/run_comps.sh`; GpuBench in `tools/gpu-spike/bench/GpuBench_main.cpp`.
- M109 decisions to respect: the first frame is the workload (fresh-process single frame, same-frame re-renders, a frame jump; playback secondary); fresh-page zeroing is a host floor, owned by M154 - Fresh Buffer Page Zeroing.
- Perf numbers come from the stripped `release` AppImage on the GPU host, never a GitHub runner.

## Scope

**In:**
- A per-download breakdown in the region profile record (count, bytes, rate, queue, whether it overlapped a kernel).
- Diagnosing the comp-vs-single-node download-rate gap on the three comps at UHD and 8K.
- Fixing the measured causes, in order of impact, for uploads as well as downloads (user, 2026-10-24).
- Pricing uploads and downloads at their own GpuBench rates in the transfer floor the gate judges.

**Out:**
- Fresh-buffer page zeroing: M154 - Fresh Buffer Page Zeroing.
- Kernel speed, and placement policy beyond charging the corrected transfer cost: none. (Upload rate moved In, 2026-10-24.)
- Single-node regions, already at 0.9–1.08× of the floor: none.

**Assumptions:**
- GpuBench's 16.7 GB/s host-import download is reachable from Natron's download path on the same device and queue.

## Phase 156.1: Measure

- [x] M156.P1.T1 — Per-download breakdown in the region record
  - files: Engine/GpuRegionTask.cpp, Engine/GpuRegionStrips.cpp, tools/bench/gpu_region_summary.py
  - approach: For each download a region issues, record bytes, device-timestamp duration, queue, source image format/layout, and whether a conversion or staging copy ran, into the existing region profile record. The summary prints, per region, the count, size distribution and effective rate of downloads, and the share of transfer time spent in the last strip.
  - verify: A ctest case runs a two-member fused region on lavapipe and asserts the new fields are present and their byte total equals the region's output bytes; on the GPU host, the summary of one keying_grade UHD run shows the new columns.
  - size: M

- [x] M156.P1.T2 — Diagnose the comp download gap
  - files: PLAN/MILESTONES/M156-gpu-download-throughput-in-comps.md
  - approach: On the GPU host, with the stripped `release` AppImage and a same-boot GpuBench run, bench keying_grade, cg_multipass and defocus_retime at UHD and 8K (`a-cold` and `bc` kinds) plus one single-node 16k region as a control. Attribute the gap between each comp's download rate and GpuBench's to the request's candidates (small downloads, layout/format conversion, unpipelined last strip) or anything else the breakdown shows. Record the attribution in Decisions with numbers; the PM then inserts one fix task per cause as M156.P2.T1 onward, largest first, each sized M or larger.
  - verify: The Decisions entry accounts for at least 80% of each comp's measured gap, or says what remains unexplained.
  - size: L

- [x] M156.P1.T3 — Time every region transfer on the device (runs before P1.T2 resumes)
  - files: Engine/GpuRegionTask.cpp, Engine/RenderStats.{h,cpp}, tools/bench/gpu_region_summary.py, Tests/GpuResidency_Test.cpp
  - approach:
    - Give a whole region a timestamp pool with one query pair per download and upload slot. Pass it in `issueDownloads()` (~:1226-1270) and `upload()` (~:849), as `options.timestampPool`/`timestampQuery`, which `GpuTransfer.cpp:672` already honours. Read it in `finishDownloads()` (~:1384). Reset the pool on the device: host reset is off, and the pair is written on the transfer queue.
    - Time serial-strip downloads and uploads too: `downloadStrip()` with `flight == nullptr` (~:1803-1890) and the strip upload (~:1626). Give them a pool of strips × slots.
    - Record pinned downloads (`downloadPinned()`, ~:1276) in `downloads` and `downloadBytes`, flagged as pinned.
    - Add an `uploads` list mirroring `downloads`.
    - `transferExposedMs` stays host-measured: it is the exposure the gate judges. Device copy time is reported beside it.
    - The summary prints upload and download device rates per region, and the timestamped share.
  - verify: remote `fast --tests` green on both legs. The P1.T1 test is extended so that every download and upload entry of a whole and a pipelined region has a non-null `deviceMs` on a device with timestamps. On the GPU host, one keying_grade UHD a-cold run shows ≥ 95% of download and upload bytes timestamped.
  - size: M

## Phase 156.2: Fix

- [x] M156.P2.T1 — Split the cold-copy slowdown between clock ramp and first-touch, without touching GPU clocks
  - files: tools/gpu-spike/bench/GpuBench_main.cpp
  - approach: Add GpuBench modes that copy one UHD image (a) after an idle gap of 0/10/30/60 ms with warm, cached imports, (b) into a freshly `aligned_alloc`'d, newly imported buffer with no warm pass and no idle gap, and (c) both. Report device up/down rates for the first copy against the steady state. **Do not change `power_dpm_force_performance_level` or any clock setting** (user). Instead, when the gap matters, add a mode (d) that keeps the GPU busy with a tiny dummy dispatch during the gap, to show whether a busy GPU removes the penalty.
  - verify: The CSV shows which mode reproduces in-app rates (7–9 down / 5.6–6.8 up, from P1.T2), and Decisions records the split with numbers.
  - size: M

- [ ] M156.P2.T2 — Remove the cold-copy penalty in regions, both directions
  - files: Engine/GpuRegionTask.cpp, Engine/Gpu/GpuTransfer.cpp, Engine/Gpu/GpuBufferPool.cpp
  - approach: Take the branch P2.T1 measures. If it's a ramp after idle: record a region's command buffers before its upload (record runs ~:1395-1430), submit upload, kernels and download back to back so the GPU never idles inside a region, and queue the next region's upload during the current one's host work. If it's first-touch: import and GPU-touch upload sources and download destinations at prepare time (`awaitPreparedDownloads`, ~:1308), and reuse cached imports across regions.
  - verify: On the GPU host, same boot, a-cold and bc device downloads ≥ 13 GB/s and uploads ≥ 9 GB/s on all three comps; no single-node region regresses; ctest green on both scheduler legs.
  - size: L

- [ ] M156.P2.T3 — Price uploads at the upload rate in the floor
  - files: tools/bench/gpu_region_summary.py, tools/bench/gpu_region_bench.sh
  - approach: Add `--import-rate-up` and price `uploadBytes` and `downloadBytes` separately in `pipelined_floor_ms` (`gpu_region_summary.py:96-99`). The bench script passes both GpuBench rates.
  - verify: The in-file self-test covers the two-rate floor, and re-summarising boot `1a6741d9`'s runs gives floors 15–33% higher.
  - size: M

## Phase 156.3: Re-bench

- [ ] M156.P3.T1 — Re-run the comp bench
  - files: PLAN/MILESTONES/M156-gpu-download-throughput-in-comps.md
  - approach: Repeat M156.P1.T2's bench on the same host after the fixes, with a same-boot GpuBench run, and record per comp and resolution the exposed-transfer ratio to the floor and the download rate, against M109.P8.T1's numbers.
  - verify: The table exists for all three comps at UHD and 8K.
  - size: M

**Verification gate:** ctest green on both scheduler legs (lavapipe, `NATRON_TEST_REQUIRE_GPU`); on the GPU host, each of keying_grade, cg_multipass and defocus_retime at UHD and 8K exposes ≤ 1.2× the same-boot GpuBench-rate transfer floor, or the Decisions entry says why a residual can't be closed and the user accepts it; no single-node region regresses against M109.P8.T1.

## Decisions

- 2026-10-23 — **Claimed by the gpu PM as the third lane** beside M107 - GPU Viewer Hand-off and M110 - Remaining Native GPU Ports. Freshness check passed on main `bf781cbcd`: every listed file exists and M109 and M154 are done. Its only overlap with running rows is at directory level (`tools/bench/` with M80 - Native RAW Support and M64 - Tiled Rendering, different files). M86 - OFX 1.5 GPU Suites was passed over because it overlaps M107 on `ViewerInstance.cpp` and `GlInterop.*`. M87 - Point-Op Kernel Fusion was passed over because its stub is blocked on evidence that so far argues against it.
- 2026-10-23 — **M156.P1.T1 code in (`8f415277a`), awaiting the remote build.** Each region record gains `downloads: [{strip, bytes, deviceMs, queueFamily, queueIndex, comps, format, staged}]`, filled only when the profile is on (`RegionReport::recordDownloads`). `deviceMs` comes from the pipelined strip path's existing timestamp queries. It is `null` for whole-region and serial-strip downloads, which have no timestamps, so P1.T2 may need timestamps added there before it can attribute those. There is no conversion flag, because both download paths are plain device→host copies (staged or imported). Beyond the brief, the change touches `Engine/RenderStats.*` and `Engine/RenderScheduler.{h,cpp}` (7 lines of plumbing), `Tests/RenderProfile_Test.cpp` and `tools/bench/gpu-region-profile-sample.jsonl`. The test is `GpuResidency.RegionRecordListsEveryDownloadAndTheirBytesAddUp`, in `Tests/GpuResidency_Test.cpp` to reuse its fused-region fixture.
- 2026-10-23 — **Overlap with M64 - Tiled Rendering on `Engine/RenderScheduler.cpp` accepted (user):** M156 runs, and M64 rebases over these lines if it resumes, as already agreed for M86 - OFX 1.5 GPU Suites.
- 2026-10-23 — **M156.P1.T1 done.** Remote `fast --tests` of `8f415277a` is green on both scheduler legs (run 40051202693). `GpuResidencyTest.RegionRecordListsEveryDownloadAndTheirBytesAddUp` passed in both legs, not skipped, and `RenderProfileTest.*` is unchanged. The GPU-host half of the verify (the summary of one keying_grade UHD run shows the new columns) is folded into P1.T2's first bench run, which builds the release AppImage anyway.
- 2026-10-23 — **M156.P1.T2 stopped at its P1.T1 check** (`.local/m156/p1t2/`; boot `9e915d0b`, RADV GFX1201, AppImage `8f415277a`). The GPU-host half of P1.T1's verify passes: the summary prints the Downloads table. But only 12–33% of comp download bytes are timestamped, because whole regions carry most of them and have no timestamps. Uploads are untimed too, and pinned downloads aren't recorded. M156.P1.T3 was inserted to time them, and P1.T2 resumes after it.
  - **Same-boot GpuBench floor (host import):** UHD up 11.5 / down 12.8 GB/s, 8K 11.0 / 12.0.
  - **Directional, one a-cold run each:** exposed-transfer ratio keying_grade 2.10×, cg_multipass 1.94×, defocus_retime 3.12×. Whole-region host-timed rates are 1.9–8.6 GB/s down and 2.4–8.1 up.
  - **Even timestamped pipelined copies are slow:** 1.9–5.3 GB/s. Every region does 1–3 large (44–133 MB) imported copies, none staged. So the copy itself looks more likely than "many small downloads": imported-page mapping/IOMMU, or contention with compute.
  - **For Phase 2:** compare an import copy with compute idle against GpuBench.
  - **Comp sizes:** the comps' 8K case records the same bytes as UHD (UHD sources), so 8K adds no information for these comps.
  - **Host changes:** RAM is ~62 GB available now (M109 saw ~91 GB), and `/dev/shm` is 64 MB.
- 2026-10-23 — **`transferExposedMs` stays host-measured** (PM). It measures exposure, which is what the gate judges; device copy times are added beside it, not in place of it.
- 2026-10-23 — **M156.P1.T3 code in (`d4eb41d68`), awaiting the remote build.**
  - **Pools:** the timestamp pool code is factored into `createQueries(nStrips, timeMembers)`, and the pipelined layout is unchanged. Whole regions and serial strips get pools only when profiling, reset on the compute queue before any copy, and read once at the end.
  - **Record:** `GpuDownloadRecord` → `GpuTransferRecord` (+`pinned`), and `recordDownloads` → `recordTransfers`. A new `uploads` list comes first. Pinned downloads are recorded (`deviceMs` null) and counted in `downloadBytes`. `ownDownloadBytes()` excludes them, so the cost model's `observeTransfer`/`predictedTransferMs` see exactly what they did before.
  - **Test:** renamed to `RegionRecordListsEveryTransferAndTheirBytesAddUp`, plus a sibling `StripRegionRecordTimesEveryTransfer`.
  - **Caveats for P1.T2:** a staged copy's `deviceMs` includes ring-wait idle time, so its rate is a lower bound. Pinned downloads stay untimed, so the ≥ 95% timestamped check excludes pinned bytes.
- 2026-10-23 — **M156.P1.T3 done.**
  - **CI:** remote `fast --tests` of `d4eb41d68` is green on both legs (run 40056237899). `RegionRecordListsEveryTransferAndTheirBytesAddUp` and `StripRegionRecordTimesEveryTransfer` ran in both legs, and lavapipe reports timestamps supported, so the `deviceMs` assertions were exercised.
  - **Release AppImage:** run 40056240260, at `.worktrees/m156/build/remote/release/`.
  - **Folded into P1.T2:** the GPU-host "≥ 95% timestamped" check (pinned bytes excluded) moves to P1.T2's resumed first run. P1.T2 resumes when M107.P6.T3's bench frees the host.
- 2026-10-24 — **Resumed by the gpu PM after a host crash and a reboot onto different hardware** (boot `d728f1eb`, RX 7900 XTX / Navi 31, ~193 GB available, against P1.T2's first run on an R9700 / GFX1201). P1.T2's attribution and the gate compare only against a same-boot GpuBench floor, so earlier numbers stay directional. Rebased onto main `d82d560f6` (M159, no shared files) as `1f66ff36c`. P1.T2 still waits for M107.P6.T3's bench to free the host.
- 2026-10-24 — **Rebased onto main `121a8289c` as `d762dc13f`** (M16, M170, M134, M94, M80, M138; no shared files). The host rebooted again (boot `1a6741d9`, same RX 7900 XTX). M16 moved the comps to `.ntpz`, which the rebased branch loads. A release AppImage of `d762dc13f` is building for P1.T2. `fast --tests` waits for the Phase 2 fixes, so one gate build covers them.
- 2026-10-24 — **M156.P1.T2 attribution** (boot `1a6741d9`, RX 7900 XTX, AppImage `d762dc13f`, GpuBench import 16.1 down / 10.5 up; `.local/m156/p1t2/run-1a6741d9/`). 100% of transfer bytes are timestamped and none pinned. Exposed/floor: keying 2.51× a-cold / 1.87× bc, cg_multipass 2.14 / 2.03, defocus 2.70 / 2.31. Device downloads run 7.9–11.1 GB/s, against 14.2–14.7 for the 16k control. The gap splits into: slow device download 27–49%, slow device upload 29–48%, the floor pricing uploads at the download rate 15–33%, and a host-side residual on defocus of 15–21%. Ruled out: small copies (one 133 MB copy per whole region), format conversion (plain `vkCmdCopyBuffer`, the same `GpuTransfer` code GpuBench uses), compute contention (sub-2 ms-kernel regions are equally slow, and GpuBench Copy ≈ Blur100), and the last strip (20–27%, in 1–2 regions). Copies speed up with GPU busy time: first copy 7.2–7.9 GB/s, later 9.0–10.4, and a strip ramp of 7.1→10.5. GpuBench never measures the cold state (cached imports, an unmeasured warm pass). So the cause is a ramp after idle or first-touch of fresh imports, and P2.T1 splits the two.
- 2026-10-24 — **Uploads are in scope** (user), since they make up half the gap and the ≤1.2× gate is out of reach without them. **The floor takes two rates** (user), uploads priced at GpuBench's upload rate (P2.T3). **No GPU clock forcing** (user: "I don't want to mess with the GPU clocks"), so P2.T1 tells ramp from first-touch with idle-gap, fresh-import and keep-busy modes only.
- 2026-10-24 — **M156.P2.T1 code in (`43113404e`)**: GpuBench `--cold-modes` / `--cold-only` adds idle-gap, fresh-import, fresh+gap and busy-gap (a 16 MiB device copy loop) modes, written to a sibling `-cold.csv` with first-copy and steady-state device rates plus a per-copy ramp. No clock or power settings are touched. It is built in natron-dev and run on the host after M107's sweep frees the GPU (~3 min).
- 2026-10-24 — **M156.P2.T1 done: the cold copy is a ramp after idle, not first-touch** (GpuBench `4c34420c3` `--cold-modes`, boot `1a6741d9`, UHD RGBA32F 132.7 MB, 3 reps × 8 copies, device timestamps; `.local/m156/p2t1/gpu-bench-cold.csv`). Back-to-back copies run at 10.4 GB/s up / 16.0 down. An idle gap of ≥ 10 ms before each copy drops the steady state to 5.7–6.0 up / 10.7–12.0 down, and the first copy after the 500 ms settle is always cold (6.0 / 11.9). With the GPU kept busy through the same gaps by a 16 MiB device copy loop, rates stay at 10.4 / 16.0 for every gap. Fresh, newly imported buffers are no slower, and if anything faster (first 8.4 / 14.1, steady 10.3 / 16.0). So P2.T2 takes the ramp branch: no GPU idle inside a region (record before submitting, then upload, kernels and download back to back), and cover the host's record/prepare time so the GPU isn't idle when a region's first copy starts. No clock settings were touched. The GpuBench build had rotted when M137 deleted the Slang grade kernel; `4c34420c3` restores a spike-local copy.
