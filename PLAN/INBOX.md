## Pending

- 2026-10-20 — change-request — from: gpu — refs: none
  Propose a follow-up E5 - GPU Compute milestone, after M109 - GPU Region Engine Hardening: find why GPU region downloads in fused comps run at about 6 GB/s against GpuBench's 16.7 GB/s host-import download rate on the same boot. M109.P8.T1 measured comp exposed transfer at 1.6–2.2× the GpuBench-rate floor (keying_grade, cg_multipass, defocus_retime at UHD/8K); single-node 16k/24k regions are at 0.9–1.08×. Data in `.local/m109/p8t1/analysis/final/regions.md`. Candidates: small per-member downloads, image layout or format conversion, an unpipelined last strip.
