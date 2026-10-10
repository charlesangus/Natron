# Render cost profile of three synthetic UHD comps (M82)

> **Follow-up (2026-10-10, `cbb5c619a`):** the issues listed under the report-script findings below are fixed. `wall_gain.py` is folded into `render-profile-report.py`, which now reports gain as a share of wall time (`--wall-ms-per-frame`), takes per-node cost from a 1-thread run (`--serial-profile`), and treats writers as region boundaries. Re-running it reproduces the gains in this report. References to `wall_gain.py` below are historical.

Question for M83/M84: do the expensive nodes of a real comp sit close enough to form GPU-resident regions, or are they isolated, so that a few heavy nodes with async transfers is enough?

All numbers below were measured on the Ryzen 7 7800X3D, **16 threads (8 cores / 16 hardware threads, `nproc` = 16)**, Natron `build/release` of branch `milestone/m82-render-cost-profiling`, 3840x2160 float, native nodes where a native exists (Grade, ColorCorrect, Merge, Transform, Keyer, ChromaKeyer, CImgBlur/Erode/Dilate are native in this build; the rest are OFX).

## Caveat: these are synthetic comps

No real comps were available. The three comps are generator-fed stand-ins (Ramp, Rectangle, Radial, CheckerBoard instead of Read), so decode and IO cost are absent and the node mix is a guess. Treat the output as directional.

## Comps (`tools/bench/comps/`)

| Comp | Nodes (plugin, native = N) |
|---|---|
| `keying_grade` (24 tasks/frame) | plate: Ramp + 2 Rectangle + 2 Merge; KeyerPlugin (N), ChromaKeyerPlugin (N), Merge max, CImgErode (N) 6px, CImgBlur (N) 10px, CImgDilate (N) 2px, Despill (OFX), Merge in, 2 Grade, 2 ColorCorrect on the fg, background Ramp + Grade + CImgBlur 24px, Merge over, ColorCorrect, Grade, Write |
| `cg_multipass` (31 tasks/frame) | 8 AOV generators (Radial x2, Ramp x3, Rectangle x2, CheckerBoard), each into a Grade; Merge plus x5, Merge multiply (AO); glow = Grade threshold + CImgBlur 90px + Grade + Merge screen; depth-masked CImgBlur 60px (depth Ramp + 2 Grade as the mask); ColorCorrect, Grade, Write |
| `defocus_retime` (37-38 tasks/frame) | plate (Ramp, CheckerBoard, Rectangle, 2 Merge, Grade, CImgBlur 12px); TimeBlur (5 divisions); Retime (speed 0.5); FrameBlend (-2..+2); CImgBlur 300px and 100px (there is no Defocus plugin in this build, so these stand in for it); ColorCorrect; Transform with motion blur (motionBlur 1.0); Merge over; ColorCorrect; Grade; Write |

Plugins present but unused: PIK, DirBlur, CImgBloom, GodRays, no Defocus. TimeBlur and Retime show ~0 ms themselves; their cost lands in the upstream tasks they request several times per frame (3-4 extra `plate_soften` tasks per frame in `defocus_retime`).

## Method

- Each comp was rendered for frames 1-6 with `NATRON_RENDER_PROFILE` on; the first rendered frame is dropped (cold caches, plugin load), leaving 5 steady frames.
- Two profiles per comp:
  - **16-thread run** (default scheduler): the realistic wall time. Measured 0.9 s/frame (`keying_grade`), 0.9 s/frame (`cg_multipass`), 2.5 s/frame (`defocus_retime`), 16 threads. Two repeats agreed within ~10% on summed task time.
  - **1-thread run** (`--setting noRenderThreads=1`, 3 steady frames): each node's own cost without contention. **All shares and regions below come from this run**, because in the 16-thread run `wallNs` is inflated by contention and the heavy/cheap classification flips between frames (regions of 1-15 nodes, different in each frame; `GradePlugin` heavy ms 1761 vs 202 ms in `cg_multipass`).
- `render-profile-report.py` with its defaults (heavy > 20 ms/Mpx, GPU 10x faster than the CPU), plus `tools/bench/comps/wall_gain.py` (below).
- Bandwidth scenarios, UHD RGBA float = 133 MB per image: staging 13.3 GB/s (20 ms round trip), importable host memory / DMA 26.6 GB/s (5 ms each way), unified memory 0 ms.
- Host: otherwise idle when each batch started (`pgrep` clear and 1-minute load 1.1-2.8, which is my own previous render). The other agent's GpuBench was running during an earlier 20 minute wait, and I waited it out; I did not watch the host during the renders themselves.

### Why `wall_gain.py`

`render-profile-report.py` subtracts transfer time (one serial stream) from **summed task time** (spread over 16 threads), so its "net gain" is not a share of frame time: it said 48-62% of task time, and a task-time share can exceed what a frame could ever save. `wall_gain.py` divides each region's task time by the measured parallelism (summed 1-thread task time / 16-thread wall per frame = 6.2, 5.1 and 6.9 for the three comps) before subtracting transfer. The 10x GPU speedup is then 10x over the 16-thread CPU, which is optimistic; 5x is shown too.

## Results (16 threads, UHD float)

Share of frame time is a share of summed 1-thread task time, which includes the generators and the Write encode.

| | `keying_grade` | `cg_multipass` | `defocus_retime` |
|---|---|---|---|
| 1-thread task time / frame | 5.6 s | 4.6 s | 17.3 s |
| 16-thread wall / frame | 0.90 s | 0.90 s | 2.5 s |
| Heavy share, > 20 ms/Mpx | 75% | 36% | 88% |
| Heavy share, > 50 ms/Mpx | 53% | 23% | 71% |
| Regions at 20 ms/Mpx | 1 of 14 nodes | 1 of 6 nodes | 1 of 19 nodes |
| Region boundary traffic | 664 MB/frame | 531 MB/frame | 550 MB/frame |
| Regions at 50 ms/Mpx | 2 (10 nodes + 1 node) | 1 of 4 nodes | 2 (sizes 1 and 3, or 1, 1, 1, 2 per frame) |
| Regions at 10 ms/Mpx | 1 of 19 nodes (of 24) | 1 of 23 nodes (of 31) | 1 of 20 nodes (of 38) |
| Net gain, regions at 20 ms/Mpx, GPU 10x: staging / DMA / unified | 68% / 71% / 74% of wall | 32% / 34% / 37% | 80% / 80% / 81% |
| same, GPU 5x: staging / DMA / unified | 60% / 63% / 65% | 28% / 30% / 33% | 71% / 71% / 72% |
| Net gain, isolated heavy nodes only (50 ms/Mpx), 10x: staging / DMA / unified | 57% / 60% / 64% | 21% / 23% / 25% | 62% / 63% / 64% |
| Net gain, every node a candidate (10 ms/Mpx), 10x: staging / DMA / unified | 79% / 82% / 84% | 67% / 72% / 77% | 80% / 81% / 82% |

Note on the 50 ms/Mpx rows: per frame, `keying_grade` has a 10-node region plus one isolated node; `defocus_retime` has region instances of sizes 1, 1, 1, 1, 2, 3 over 3 frames, i.e. two pieces per frame.

Top plugins by 1-thread time, ms/frame (16 threads' worth of work, summed):

| `keying_grade` | | `cg_multipass` | | `defocus_retime` | |
|---|---|---|---|---|---|
| ColorCorrect | 1243 | Grade (11 nodes) | 1862 | Transform (motion blur) | 9468 |
| CImgBlur | 1062 | CImgBlur | 1081 | CImgBlur | 2664 |
| Grade | 787 | Merge | 530 | FrameBlend | 2253 |
| ChromaKeyer | 548 | ColorCorrect | 378 | Grade | 827 |
| CImgErode | 466 | Ramp | 240 | ColorCorrect | 714 |
| CImgDilate | 418 | Radial | 196 | Merge | 524 |
| Merge | 411 | Rectangle | 150 | Ramp | 244 |

Per-node speeds, 1 thread, ms/Mpx: CImgBlur 57-66, CImgErode/Dilate 50-55, ChromaKeyer 64, ColorCorrect 33-71, Grade 13-33, Merge 6-15, Keyer 18, Despill 9, generators 4-10, FrameBlend 21, Transform with motion blur 1014.

### Reading the table

- **`keying_grade`:** the heavy nodes (ChromaKeyer, Erode, Blur, Dilate, ColorCorrect) sit in one chain from the plate to the final Grade, so the whole post-plate graph (14 of 24 nodes) forms one region with 664 MB of boundary traffic: plate in, and the sky and Write out. Resident beats isolated by about 10 points (68-74% vs 57-64%).
- **`cg_multipass`:** only the glow, defocus and final correction are heavy by the 20 ms/Mpx rule, and they form a 6-node region worth 32-37%. The 8-pass rebuild is Grade and Merge, each 6-18 ms/Mpx: individually cheap (a Grade is ~110-150 ms on one thread, ~20-30 ms on 16 threads, so saving ~20-25 ms for a 10-20 ms round trip is roughly break-even), but together Grade+Merge are 52% of the comp's task time. Counting every node as a candidate (10 ms/Mpx) raises the gain from ~34% to ~72%, and this only works if the passes stay resident.
- **`defocus_retime`:** a single motion-blurred Transform is 55% of task time, which dominates the result. The rest (blurs, FrameBlend) is a chain from TimeBlur to the Merge, which is one 19-node region.

## Conclusion

1. **Resident regions are available in all three comps.** With a heavy cut at 20 ms/Mpx, 70-90% of the non-generator nodes join one connected region per frame (6-19 nodes); the nodes between heavy ones are cheap point ops (Grade, Merge) and the graphs are dense chains, not isolated spikes.
2. **But most of the gain is already captured by isolated heavy nodes.** At UHD float a node's round trip is 10 ms (DMA/importable) to 20 ms (staging), against 100-1000 ms of CPU work in a 50 ms/Mpx node. Isolated heavy nodes alone get 57-64% (keying), 21-25% (CG), 62-64% (defocus), versus 68-74%, 32-37%, 80-81% for resident regions: residency adds about 8-18 points of frame time, and the difference between staging and DMA is under 4 points everywhere.
3. **Residency changes the result only for chains of cheap point ops.** In the CG multipass comp the 11 Grades and 6 Merges are half of the task time but each is near break-even alone; they pay (72% vs 23-34%) only if they run on the GPU without transfers between them. Plates arriving from Read are CPU-resident in any case, so the boundary stays at one upload per input.
4. Recommendation for the go/no-go: M83/M84 can start with heavy nodes (Blur, Erode/Dilate, ChromaKeyer, ColorCorrect, motion-blurred Transform, FrameBlend) with async transfers, which captures most of the gain even with staging copies. Planning residency for point-op chains (Grade, Merge, ColorCorrect) is worth doing next, mainly for multipass comps with many inputs, but it is a second step, not a prerequisite.

All these figures assume a 10x (or 5x) GPU speedup over the 16-thread CPU; neither was measured here.

## Bugs, oddities and fixes

Nothing in the profiler or report script was changed.

- **`wallNs` is thread time under contention.** In 16-thread runs the same node costs more and the heavy/cheap classification differs from frame to frame; use a `noRenderThreads=1` profile for classification (see `run_comps.sh`). Summed task time is 2.7-9 s/frame vs 0.9-2.5 s of wall.
- **Report script mixes task time and wall time.** Its "net GPU gain" (48-62% of task time) is not a share of the frame; `wall_gain.py` rescales it.
- **Write is a region member.** The report puts `internalEncoderNode` (the Write encode) inside regions (seen in the 16-thread keying run and in every 10 ms/Mpx region); encode and file IO cannot run on the GPU.
- **`time` is not usable as a frame key.** In `defocus_retime` records carry times such as -1073741824 (a sentinel for some time-offset requests) and times offset by Retime/TimeBlur. The `frame` field (render index from 0) is the key, and `run_comps.sh` drops the first frame by it.
- **Transform motion blur is very slow.** `motionBlur 1.0` with 14 px/frame translation and 0.8 deg/frame rotation costs 1014 ms/Mpx (9.4 s/frame on one thread). Worth a look independently of the GPU question.
- The report script's region rule assumes every cheap node between two heavy ones can run on the GPU, without checking that a GPU version exists.

## Files

In the repository (untracked, not committed):

- `tools/bench/comps/keying_grade.ntp`, `cg_multipass.ntp`, `defocus_retime.ntp`: the comps (UHD float, frames 1-8, writer `Write` to `build/bench/m82-profile/out/`).
- `tools/bench/comps/build_comps.py` and `build_comps.sh`: regenerate the `.ntp` files in the natron-dev container.
- `tools/bench/comps/run_comps.sh`: waits for a free host, renders with profiling, writes `<comp>[-<TAG>].steady.jsonl`.
- `tools/bench/comps/wall_gain.py`: wall-clock rescaling of the report's gain.
- This report, in the ignored `.plan/`.

Ignored, under `build/bench/m82-profile/`: raw `*.jsonl.<pid>`, `*.steady.jsonl`, `*-serial.*` (1-thread runs), `run1/` (first 16-thread repeat), logs, `host.log`.

Reproduce:

```
tools/bench/comps/build_comps.sh
tools/bench/comps/run_comps.sh 1 6
TAG=serial RENDERER_ARGS="--setting noRenderThreads=1" tools/bench/comps/run_comps.sh 1 4
python3 tools/bench/render-profile-report.py build/bench/m82-profile/keying_grade.steady.jsonl --serial-profile build/bench/m82-profile/keying_grade-serial.steady.jsonl --wall-ms-per-frame 900
```
