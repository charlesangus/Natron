# M196 - Copy-Compute Contention

M156 - GPU Download Throughput In Comps shipped with a residual: in fused comps the in-app device copies run at 7.5–11.7 GB/s down and 5.6–7.9 GB/s up, against GpuBench's 16.1 down / 10.4 up for an isolated host-import copy. GpuBench's overlap-on rows show about 10 / 9.9 whenever a copy runs beside compute. On RADV (RX 7900 XTX, `RADV_PERFTEST=transfer_queue`) copies get no transfer-only queue in practice, so they share the compute queues (`Engine/Gpu/GpuDevice.cpp`, queue-family selection ~:404-430 on main). This milestone measures whether a dedicated transfer/SDMA queue, or scheduling region copies apart from kernels, recovers the isolated rate, and fixes it if it does.

Source: the gpu PM's inbox request (2026-10-25). Data on the GPU host: `.local/m156/p2t2-ka1/`, `.local/m156/p2t1/`, plus M156 - GPU Download Throughput In Comps's `## Decisions`.

Scouted facts (2026-10-25; re-check at promotion):
- Queue and family selection: `Engine/Gpu/GpuDevice.cpp` (`transferFamily`, `uploadQueueIndex`, `downloadQueueIndex`, `transferSharesComputeQueue`); transfers: `Engine/Gpu/GpuTransfer.cpp`; region chains: `Engine/GpuRegionTask.cpp` (`submitChain`, `finishChain`).
- Bench tooling: `tools/bench/gpu_region_bench.sh`, `tools/bench/gpu_region_summary.py`, `tools/bench/comps/run_comps.sh`, `tools/gpu-spike/bench/GpuBench_main.cpp` (overlap modes, `BusyLoad`).
- Perf numbers come from the stripped `release` AppImage on the GPU host, never a GitHub runner.

## Scope

**In:**
- Measure copy rate with and without concurrent compute on the same device, per queue configuration: shared compute queue, a second queue in the compute family, a dedicated transfer family where exposed (`RADV_PERFTEST=transfer_queue`, SDMA).
- Measure scheduling region copies apart from kernels, so no copy overlaps a kernel at region edges.
- Implement whichever of the two recovers the isolated rate in comps, and re-bench.

**Out:**
- Kernel speed and placement policy: M156 - GPU Download Throughput In Comps and M109 - GPU Region Engine Hardening own those.
- Fresh-page zeroing, a host floor owned by M154 - Fresh Buffer Page Zeroing.
- Other vendors' drivers (NVIDIA, Intel): measure and report only if hardware is on hand; M168 - NVIDIA Viewer Hand-off Validation owns NVIDIA validation.

**Edge cases:**
- A device with no transfer-only family: the fix must degrade to today's behaviour with no regression, and `transferSharesComputeQueue` stays accurate.
- Serialising copies apart from kernels may cost more in lost overlap than it recovers; the gate judges net exposed transfer time, not copy rate alone.

**Assumptions:**
- GpuBench's 16.1 / 10.4 GB/s isolated rate is reachable from Natron's copy path (M156 assumed the same for the download path).
- Copies and kernels contending for the same queue or memory controller is the cause; Phase 196.1 confirms or refutes it before any fix.

## Phase 196.1: Measure

- [x] M196.P1.T1 — Bench copy rate against concurrent compute per queue configuration
  - files: `tools/gpu-spike/bench/GpuBench_main.cpp`
  - approach: Add modes that run host-import up and down copies alone, beside a compute load, and with the compute load on a different queue, for each queue configuration the device exposes (shared queue, second compute-family queue, transfer family with and without `RADV_PERFTEST=transfer_queue`). Print one row per configuration with the rate and the queue family and index used.
  - verify: On the GPU host, the isolated rows reproduce about 16.1 down / 10.4 up and the overlap rows reproduce about 10 / 9.9; the build compiles under the existing spike target.
  - size: M

- [ ] M196.P1.T2 — Attribute in-app copy rate to queue sharing in comps
  - files: `Engine/GpuRegionTask.cpp`, `tools/bench/gpu_region_summary.py`
  - approach: Using M156's per-download profile record (`queueFamily`, `queueIndex`, `deviceMs`, overlap-with-kernel flag), summarise per comp and resolution the copy rate split by whether the copy ran beside a kernel and by queue. Extend the summary only; add no new profile fields unless the existing record lacks one, and say which.
  - verify: Run on the GPU host over keying_grade, cg_multipass and defocus_retime at UHD and 8K; the summary table shows, per comp, copy rate overlapped vs not, and the Decisions entry states whether contention explains the 7.5–11.7 / 5.6–7.9 GB/s gap.
  - size: M

- [ ] M196.P1.T3 — Prototype the two candidate fixes behind a switch and measure
  - files: `Engine/Gpu/GpuDevice.cpp`, `Engine/GpuRegionTask.cpp`, `Engine/Gpu/GpuTransfer.cpp`
  - approach: Behind an environment switch (default off), (a) request a dedicated transfer or SDMA queue where the device exposes one and route region copies to it, and (b) order a region's submissions so copies at region edges do not overlap kernels. Run each over the three comps and record exposed-transfer ratio against the same-boot GpuBench floor.
  - verify: Results table in `## Decisions` for baseline, (a), (b) and both, per comp and resolution, same boot, three rounds.
  - size: L

## Phase 196.2: Fix

- [ ] M196.P2.T1 — Land the winning configuration as the default
  - files: `Engine/Gpu/GpuDevice.cpp`, `Engine/GpuRegionTask.cpp`, `Engine/Gpu/GpuTransfer.cpp`, `Tests/Gpu/` (queue-selection test)
  - approach: Make the configuration that Phase 196.1 shows recovers the isolated rate the default and remove the switch and the losing prototype. If neither recovers it, land nothing, record the numbers and why in `## Decisions`, and skip to the re-bench. Keep the no-transfer-family fallback unchanged, and add a lavapipe-runnable test that the queue-selection logic picks the expected families for a synthetic family list.
  - verify: ctest green on both scheduler legs (lavapipe, `NATRON_TEST_REQUIRE_GPU`); the new queue-selection test passes.
  - size: M

## Phase 196.3: Re-bench

- [ ] M196.P3.T1 — Re-run the comps bench and record the result
  - files: none (results recorded in `## Decisions`)
  - approach: On the GPU host, rerun M156's comps bench with a same-boot GpuBench run and compare exposed-transfer ratio and copy rates with M156.P3.T1's numbers.
  - verify: A Decisions entry with the table, or the measured reason the residual cannot be closed.
  - size: S

**Verification gate:** ctest green on both scheduler legs (lavapipe, `NATRON_TEST_REQUIRE_GPU`); on the GPU host, keying_grade, cg_multipass and defocus_retime at UHD and 8K show exposed transfer no worse than M156's shipped numbers and, if a fix landed, in-app copy rates within 10% of the same-boot GpuBench overlap-off rates; or the Decisions entry shows why the residual cannot be closed and the user accepts it.

## Decisions

- 2026-10-27 — **P1.T1 queue bench (gpu, AMD Radeon AI PRO R9700, RADV GFX1201, PCIe 5; 256 MB host-import copies, wall time submit to fence):** the host changed since M156 (RX 7900 XTX), so its 16.1 / 10.4 GB/s figures don't carry over.
  - Without `RADV_PERFTEST`: no transfer-only family. Compute-family copies run 10.6 up and 11.7–12.1 down alone. Beside a compute load on another compute queue they fall to 0.3–3.6 up and 4.8 down. On the same queue as the load they keep 9.6–10.9 up and 11.9 down (serialised).
  - With `RADV_PERFTEST=transfer_queue`: a 2-queue transfer-only family appears. Copies on it run **44.7 up and 44.8 down alone, and 44.8 up and 41.5 down beside compute**.
  - So contention on the compute queues is real, and the SDMA queue is about 4× faster than any compute-queue copy and barely slowed by compute. Candidate (a) in P1.T3 is the lead. Its catch is that RADV exposes the queue only behind `RADV_PERFTEST`, so the fix has to decide whether Natron sets it (before instance creation) or documents it.
