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
- Fixing the measured causes, in order of impact.

**Out:**
- Fresh-buffer page zeroing: M154 - Fresh Buffer Page Zeroing.
- Upload rate, kernel speed, and placement policy beyond charging the corrected transfer cost: none.
- Single-node regions, already at 0.9–1.08× of the floor: none.

**Assumptions:**
- GpuBench's 16.7 GB/s host-import download is reachable from Natron's download path on the same device and queue.

## Phase 156.1: Measure

- [ ] M156.P1.T1 — Per-download breakdown in the region record
  - files: Engine/GpuRegionTask.cpp, Engine/GpuRegionStrips.cpp, tools/bench/gpu_region_summary.py
  - approach: For each download a region issues, record bytes, device-timestamp duration, queue, source image format/layout, and whether a conversion or staging copy ran, into the existing region profile record. The summary prints, per region, the count, size distribution and effective rate of downloads, and the share of transfer time spent in the last strip.
  - verify: A ctest case runs a two-member fused region on lavapipe and asserts the new fields are present and their byte total equals the region's output bytes; on the GPU host, the summary of one keying_grade UHD run shows the new columns.
  - size: M

- [ ] M156.P1.T2 — Diagnose the comp download gap
  - files: PLAN/MILESTONES/M156-gpu-download-throughput-in-comps.md
  - approach: On the GPU host, with the stripped `release` AppImage and a same-boot GpuBench run, bench keying_grade, cg_multipass and defocus_retime at UHD and 8K (`a-cold` and `bc` kinds) plus one single-node 16k region as a control. Attribute the gap between each comp's download rate and GpuBench's to the request's candidates (small downloads, layout/format conversion, unpipelined last strip) or anything else the breakdown shows. Record the attribution in Decisions with numbers; the PM then inserts one fix task per cause as M156.P2.T1 onward, largest first, each sized M or larger.
  - verify: The Decisions entry accounts for at least 80% of each comp's measured gap, or says what remains unexplained.
  - size: L

## Phase 156.2: Fix

Tasks are inserted from M156.P1.T2's attribution.

## Phase 156.3: Re-bench

- [ ] M156.P3.T1 — Re-run the comp bench
  - files: PLAN/MILESTONES/M156-gpu-download-throughput-in-comps.md
  - approach: Repeat M156.P1.T2's bench on the same host after the fixes, with a same-boot GpuBench run, and record per comp and resolution the exposed-transfer ratio to the floor and the download rate, against M109.P8.T1's numbers.
  - verify: The table exists for all three comps at UHD and 8K.
  - size: M

**Verification gate:** ctest green on both scheduler legs (lavapipe, `NATRON_TEST_REQUIRE_GPU`); on the GPU host, each of keying_grade, cg_multipass and defocus_retime at UHD and 8K exposes ≤ 1.2× the same-boot GpuBench-rate transfer floor, or the Decisions entry says why a residual can't be closed and the user accepts it; no single-node region regresses against M109.P8.T1.
