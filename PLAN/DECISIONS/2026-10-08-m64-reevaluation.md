# M64 re-evaluation against native core nodes

2026-10-08, M64.P7.T1. **Preliminary, work-in-progress investigatory notes. M64 is still deferred; nothing here is adopted or scheduled.** **Recommendation: drop the strip-task design (a). Rewrite M64 as option (b), fused native point-op segments, and put a cheap per-node fast path for native point ops first.** Then re-measure. Fusion is gated on what is left after the fast path, measured on this host and on a small-cache 4-core host. Do not cancel (c): the per-node cost of a native point op is still 6–7x what its kernel needs.

## Setup

- Code: `main` at `cb04295b9`, release build rebuilt in `natron-dev` (binary 2026-10-08 22:38, tree clean). The image was built from `mirror.gcr.io` per `tools/ci/local/README.md`. The OFX bundle is the existing `build/assets/Plugins` (openfx-misc `59ae4c26`, not re-fetched at the M73 merge-commit pin). The only OFX node in the measured graphs is the Write.
- Host: AMD Ryzen 7 9800X3D, 8 cores / 16 threads, 96 MiB L3, 8 MiB L2 total, 62 GB RAM. Every configuration started at load1 ≤ 0.49, with cpu/io/memory `some avg10` ≤ 0.05% at start. Mean clocks were 3.7–4.4 GHz.
- Previous numbers came from a 4-core Intel N100 with 6 MB L3. An HD float RGBA frame (33 MB) fits in this host's L3; a UHD frame (133 MB) does not.
- Commands (logs in `build/bench/m64-reeval/`):
  - `stream_bench W H N TILE` at `OMP_NUM_THREADS` 16/8/4 → `stream.log`.
  - `BENCH_RENDER_STATS=1 BENCH_COOLDOWN=30 tools/bench/run_matrix.sh` for HD `chain:30,100 ccchain:10,40 mergechain:10,40 xfchain:10,40` (3 frames), UHD `chain:10,30`, tiny `chain:0,1000`, HD chain 30/100 again with `noRenderThreads=4`, and a second HD chain 30/100 round. The script is `run_chains.sh`, output `chains.log` and `build/bench/results-m64re-*.jsonl`.
  - A 60-sample eu-stack profile of HD chain 100 (`profile_run.sh`) → `profile-analysis.txt`, samples in `build/bench/samples-m64re-chain100.txt`.
  - Minor-fault counts for chain 30 vs 100 (`faults.py`) → `faults.log`.
  - A scratch micro-benchmark, `fuse_bench.cpp` → `fuse.log`. It copies `GradeKernel`'s maths and `NativeImageEffect::render`'s row loop, and runs it node-at-a-time and fused over row bands. It is not a tracked change.

## Numbers

| measure | N100 (earlier) | this host |
|---|---|---|
| `stream_bench` HD node pass, all threads | 5.1 ms (12.9 GB/s) | **0.23 ms** (~290 GB/s, L3-resident) |
| `stream_bench` UHD node pass (DRAM) | — | 6.6 ms (~40 GB/s), i.e. ~1.65 ms per HD-sized frame |
| native Grade, HD marginal (chain 100 − 30)/70 | 21.3 ms | **3.62 / 3.61 ms** (two rounds), 28.6 CPU-ms, parallelism 7.5 |
| native Grade, HD, `noRenderThreads=4` | — | 5.29 ms, 15.3 CPU-ms |
| native Grade, UHD marginal (chain 30 − 10)/20 | — | 21.2 ms |
| ColorCorrect / Merge / Transform, HD marginal | 45 / 20 / 43 ms | 7.4 / 4.2 / 8.3 ms |
| tiny chain per node (per-call fixed cost) | ~0.17 ms | 0.052 ms |
| bandwidth share of a Grade node, HD | ~24% | **~6%** (L3); ~45% if the frame had to come from DRAM |
| bandwidth share, UHD | — | ~31% |
| rss_peak HD chain 30 → 100 | — | 477–521 → 488–582 MB (flat in N) |

Where a native Grade's 3.6 ms goes at HD (eu-stack, busy samples):
- ~40% in `GradeKernel::processRow`, which is scalar double maths with per-channel bit tests;
- ~40% in `NativeImageEffect::render`'s own row loop: the source and divided-row copies, and the per-pixel, per-channel mix/write loop;
- ~5% memmove, ~5% `checkForNaNsAndFix`, ~5% image alloc/fill.

Minor faults are the same for chain 30 and 100 (253k vs 256k), so the engine recycles image memory and does not page-fault per node.

Micro-benchmark (`fuse_bench`, ms per node, 8 threads, HD / UHD):
- as the engine runs it, with a fresh image per node: 2.32 / 17.2;
- kernel writing image to image with two reused buffers: **0.55 / 6.55**;
- fused over row bands: **0.55 / 2.01**.

At 4 threads the same three are 2.96 / 0.94 / 0.96 (HD) and 18.8 / 6.5 / 3.8 (UHD). The fresh-image cost in the micro-benchmark is page faulting, which the engine avoids, so the engine-relevant comparison is the real 3.6 ms against ~0.55 ms of kernel.

## Reading

- **On this host, HD is not bandwidth-bound.** A whole-frame pass costs 0.23 ms, about 6% of a native Grade node, so strip-tiling for cache reuse buys almost nothing at HD. Strip pulls would also add the per-call cost (52 µs × nodes × strips) to save bandwidth that isn't being spent. The N100 spike already capped design (a) at 1.05–1.17x.
- **The cost is per-node overhead in the native pipeline, not memory.** The kernel needs ~0.55 ms/node, and the node costs 3.6 ms. About half of the gap is the base class's scalar copy/mix/write loop, which a fast path can remove for the common case (mix 1, no mask, all channels, no divisor: kernel straight into the output row) without any fusion. Estimate by subtraction: 1.5–2x per point-op node on chains, and it helps every graph.
- **Fusion pays where DRAM traffic is real.** That means UHD and larger frames here (fused 2.0 vs reused node-at-a-time 6.5 ms/node, 3.2x), and plausibly HD on a 6 MB-L3 host, where an HD frame behaves like UHD does here. Fusion also removes the per-node fixed costs: the NaN pass, scheduler task, band fork-join and output image. Upper bound on this host for a Grade chain at HD is ~3.6/0.55 ≈ 6.5x per node. After a fast path, the remaining HD gain from fusion here is likely 1.3–2x (estimate), and larger at UHD and on small-cache hosts.
- **Transfer to small-cache hosts.** The conclusion that (a) is not worth building transfers: on the N100 the measured spike gave ≤1.17x. The fusion estimate does not transfer directly, so it has to be measured there. The micro-benchmark and `stream_bench` need no Natron build, so that is a few minutes on the 4-core box.
- **Memory (large frames).** Peak RSS on a Grade chain is already flat in N (one task at a time; consumers release inputs). Tiling a point-op chain can only reach the source-plus-output floor, which at 24k is ~9.7 GB, unless the source joins the segment. Fusion reaches the same floor without strips. So the out-of-RAM case does not by itself justify (a). P1.T3/P1.T4 should still be measured before any memory work is promised.
- **Coverage.** Native point ops with a `PixelKernel` today are Grade, ColorCorrect, Saturation, Clamp, Invert, ColorLookup, the Add/Multiply/Gamma family, Constant and CheckerBoard. Merge has its own render (no kernel), and Transform and Blur are spatial. Fusing Merge needs a two-input kernel, which `RowIO` already allows (`kPixelKernelMaxInputs` 4).

## Effect on Phases 64.1–64.6

| task | fate |
|---|---|
| new, first | **Native point-op fast path**: in `NativeImageEffect::render`, write the kernel output directly to the destination row when mix is 1, there is no mask or divisor, and every channel is processed; hoist the per-channel bit tests out of `GradeKernel`/`ColorCorrect` rows. Re-measure HD/UHD chain per-node on both hosts. This could be a small M67 follow-up instead of M64 work. |
| P1.T1 re-baseline | **rewritten**: after the fast path, on this host and a 4-core 6 MB-L3 host, including UHD. Add `stream_bench` and `fuse_bench` on both. |
| P1.T2 strip-pull spike (gate 1) | **dropped**. Replaced by a fused-kernel spike: chain the real nodes' `makeKernel()` kernels over row bands in an env-gated test, compared with the whole-image scheduler. Same 1.3x gate, on HD at the small-cache host or UHD at this host. |
| P1.T3 large-frame bench support | **survives** unchanged. |
| P1.T4 untiled large-frame baseline | **survives**; it decides whether the memory case exists at all. |
| P2.T1 request-pass point-op probe | **dropped**: nodes declare it (`isPointOp()`, promoted to an `EffectInstance` trait). |
| P2.T2 setting, env, stats, strip height | **rewritten**: a `fusedRendering` setting and `NATRON_FUSED_RENDERING`, with fused-segment stats. Row-band size stays per node; there is no cache-topology strip policy. |
| P2.T3 graph partition into tile + join tasks | **rewritten**: segment formation and the Design §1 eligibility rules survive (minus the RoI-probe rule), but a segment becomes one fused task with no tile or join tasks. |
| P3.T1 TileScope per-call pulls | **dropped**. |
| P3.T2 tile/join execution | **rewritten**: execute a fused task (boundary inputs from the `FrameStore`, kernels chained per row with per-node mask/mix/unpremult, tail image stored), with the same accounting, abort and failure tests. |
| P4.T1 equivalence suite | **survives**, as TaskGraphVsFused, bit-exact. |
| P4.T2 cache semantics | **survives**. |
| P4.T3 bench + gate 2 | **rewritten**: fused vs whole-image on both hosts, HD and UHD. Targets are set after the fast path. |
| P4.T4 per-tile overhead trim | **dropped** (no tiles); fold any per-segment cost into P4.T3's follow-up. |
| P4.T5 large-frame tiled vs untiled | **conditional**: only if P1.T4 shows untiled failures on point-op chains. |
| P5.T1 viewer/GUI check | **survives** with the fused setting. |
| P6.T1 decision | **survives**, rewritten for the fused design. |
| P6.T2 default, CI, AppImage | **survives**. |

## Not measured here (offered as follow-ups)

- The same runs on a 4-core 6 MB-L3 host (`stream_bench`, `fuse_bench`, chain 30/100 HD). This is the deciding number for HD fusion on small machines.
- The fast path's real gain, which needs an engine change, so it was out of scope.
- ColorCorrect/Merge/Transform profiles, multi-round medians, and many-core (>16 thread) scaling. At 4 threads a Grade node uses 15 CPU-ms; at 16 it uses 28, so SMT and band fan-out cost CPU on this host.
- Large frames (P1.T3/P1.T4).

## Spatial ops at large frames (follow-up, 2026-10-08)

The user asked whether tiling helps an expensive spatial op at very large frames, tiled against whole-frame on this host only. **It does not help speed. With serial strips it cuts peak memory 4–14x, and Natron's cache cannot get that saving without recomputing halo rows.** The recommendation above stands.

### Setup

- Same host and `build/release` at `cb04295b9`. The release `Tests` objects were linked with a scratch test, `build/wt/m64-spatial/Tests/SpatialTileSpike_Test.cpp`. It is untracked and lives in a detached worktree at `main`, and it adapts the kill-gate-1 spike (`30700800a`). The binaries are `build/bench/m64-spatial/spike_tests{,2}`, built by `compile*.sh`/`link*.sh`. The drivers are `run_one.sh`, `matrix.sh` and `matrix2.sh`, and the logs are `build/bench/m64-spatial/logs/*.log`.
- Graph: CheckerBoard (project extent, `color0` keyed) → 1 or 4 native Blurs: Gaussian (Van Vliet IIR, the default), size 50 or 200, labelled g50, g200 and g50x4. Formats are 8192×4096, 12288×6144, 16384×8192 and 24576×12288, all RGBA float. The node cache is cleared before every run. Each config is best of 2 runs at 8k–16k and a single run at 24k.
- Modes:
  - **whole**: one frame through the M63 scheduler.
  - **ser**: full-width strips, top to bottom, each with its own request pass and `renderRoI(strip)`, on the test thread with the whole pool. The Blur bands its own passes over 16 threads.
  - **par**: one strip per pool thread with a budget of 1, as M64's strip tasks would run.
  - **reuse**: like ser, but `forceCaching` is on for every node except the tail, and the tail does not bypass the cache. A later strip then finds earlier rows in the cached image's bitmap.
- Threads: pool 16. Effective parallelism (CPU/wall) is ~10.4 for whole, 9.5–10 for ser, up to 14.7 for par with ≥16 strips, and 4.5–7 for reuse.
- Peak memory is VmHWM after resetting it (`clear_refs` 5), minus the RSS before the run. The whole-process max RSS was at most 32 GB, which includes the test's two comparison frames. The container was capped at 40 GB with no swap, and nothing hit OOM.
- Host load: every config started at load1 < 1.0, gated, with 55–59 GB available. load5 was 2–5 from the previous config, since there was no long cooldown.
- `upstream_rows_x` counts the rows each node is asked for, summed over strips, divided by the frame height (source/…/tail). `rendered_px_x` counts the pixels each node actually rendered, from in-depth `RenderStats`, divided by the frame's pixels.

### Results

ms is wall time, x is the speed-up against whole (above 1 means tiling wins), and MB is peak above the pre-run RSS. Source rows is `upstream_rows_x` for the CheckerBoard.

| size | graph | whole ms / MB | ser 768: ms, x, MB, source rows | best ser: rows, ms, x, MB | best par: rows, ms, x, MB | best reuse: rows, ms, x, MB |
|---|---|---|---|---|---|---|
| 8k | g50 | 447 / 1537 | 566, 0.79, 324, 1.18 | 3072, 511, 0.87, 1169 | 256, 567, 0.79, 1648 | 3072, 677, 0.67, 1169 |
| 8k | g200 | 449 / 1537 | 762, 0.59, 436, 1.72 | 3072, 557, 0.81, 1225 | 256, 1250, 0.36, 3283 | 3072, 723, 0.61, 1225 |
| 8k | g50x4 | 1636 / 1537 | 2379, 0.69, 493, 1.72 | 3072, 1870, 0.87, 1253 | 256, 3277, 0.50, 3810 | 3072, 2395, 0.70, 2475 |
| 12k | g50 | 947 / 3454 | 1236, 0.77, 487, 1.17 | 3072, 1119, 0.85, 1755 | 512, 1183, 0.80, 3209 | — |
| 12k | g200 | 963 / 3458 | 1681, 0.57, 656, 1.68 | 3072, 1175, 0.82, 1839 | 512, 1837, 0.52, 4935 | — |
| 12k | g50x4 | 3530 / 3455 | 5257, 0.67, 740, 1.68 | 3072, 3849, 0.92, 1881 | 512, 5626, 0.63, 5357 | — |
| 16k | g50 | 2008 / 6142 | 2306, 0.87, 649, 1.18 | 1536, 2186, 0.92, 1225 | 512, 2028, 0.99, 5894 | — |
| 16k | g200 | 1989 / 6142 | 3181, 0.63, 874, 1.73 | 3072, 2308, 0.86, 2602 | 512, 3468, 0.57, 8723 | — |
| 16k | g50x4 | 6872 / 6147 | 10028, 0.69, 987, 1.73 | 3072, 7642, 0.90, 2715 | 512, 8935, 0.77, 10583 | — |
| 24k | g50 | 4369 / 13829 | 5283, 0.83, 975, 1.18 | 3072, 4787, 0.91, 3567 | 768, 3983, **1.04**¹, 12964 | 3072, 6832, 0.64, 7472 |
| 24k | g200 | 4170 / 13829 | 7343, 0.57, 1313, 1.73 | 3072, 5120, 0.81, 3904 | 768, 5760, 0.74, 16441 | 3072, 7196, 0.58, 7646 |
| 24k | g50x4 | 15296 / 13829 | 22682, 0.67, 1482, 1.73 | 3072, 16994, 0.90, 4073 | 768, 18866, 0.81, 19906 | 3072, 25234, 0.61, 22160 |

¹ Single run against its own process's whole run (4158 ms). The other 24k process measured whole at 4369 ms. Within noise.

Full curves (heights 128–3072) are in the logs.
- ser falls off steeply below ~768 rows: at 24k g50x4, 768 rows is 0.67x and 3072 rows is 0.90x.
- par only reaches the pool's width at ≥16 strips. At 2–6 strips it runs 0.1–0.4x.
- Reuse was 0.30–0.35x at 768 rows and 0.58–0.64x at 3072 rows.

### Reading

- **The whole-frame Blur scales linearly.** It costs 13–15 ns per pixel at every size from 8k to 24k, and there is no cache cliff at 24k widths. The Blur is separable: a horizontal pass over rows, then a vertical pass one column at a time with a stride of `bufferWidth`. Each thread walks a run of adjacent columns, so the cache lines a column touches (12288 × 64 B ≈ 0.8 MB at 24k) stay in L2/L3 for the next column. Strips leave nothing for locality to win.
- **Naive strips lose to their halo.** A strip's Blur buffer is `getSourceRoI(strip) ∩ RoD`, the strip plus 1.5×size rows on each side, and every upstream node renders that too. At 768 rows that is 1.18x the frame's rows for size 50 and 1.73x for size 200, and it compounds down a chain (1.73/1.55/1.37/1.18 for g50x4). CPU time tracks it, and the best strip heights are the largest. Speed only gets close to whole (0.9x) when strips are so tall that the memory saving shrinks.
- **Memory is the real benefit, and only for ser.** Whole-frame peak is ~2.9 frames (13.8 GB at 24k), flat in chain length. Serial strips hold one strip plus its halos per node: at 24k that is 1.0–1.5 GB at 768 rows and 3.6–4.1 GB at 3072 rows, for 0.6–0.9x the speed. par keeps 16 haloed strips in flight, so it peaks at or above the whole frame (13–20 GB at 24k).
- **Reuse through the engine cache (option 1) works for rows, not for memory.**
  - It works with the engine as it stands. With interior nodes cached and the tail not bypassing, `renderRoI` finds the earlier strip's cached image, `getRestToRender`/the trimap gives only the missing rows, and `rendered_px_x` drops to exactly 1.000 per node.
  - It needs the tail call not to bypass the cache. The tail's `byPassCache` is forwarded to its input renders (`EffectInstanceRenderRoI.cpp` around 1228), and a bypassing lookup evicts the entry (around 880). The spike's first reuse attempt therefore re-rendered 1.549x.
  - **Cost:** each strip grows the cached image with `Image::ensureBounds` (`Image.cpp:1117`), which reallocates to the union and copies everything so far, single-threaded. That is O(strips²) copying, and parallelism falls to 4.5–7.
  - The cache keeps every interior node's whole frame, so peak is 7.5–24 GB at 24k, no lower than whole.
  - Reuse also does not remove the Blur's own halo work: its fill and both passes still run over strip + halo rows, because that is its filter warm-up.
  - So reuse ran 0.6–0.7x at best. A sliding row window (option 2: a per-node ring of ~halo × width rows that later strips read) was not built. It would get serial-strip memory with source rows at 1.0, but the Blur's per-strip warm-up rows would remain.
- **Pixels: strips never match the whole frame for the Gaussian Blur, in any mode, reuse included.** Max abs difference is 3.7–5.7e-3 on values near 1 (≈1 8-bit code). The differing share grows from 10% of samples at 2 strips to 100% at 32. This is a finding about the node: **the native Gaussian/quasi-Gaussian Blur is not RoI-invariant.**
  - Where: `Blur::render` sizes its buffer to `getSourceRoI(processWindow) ∩ RoD` (`Engine/Nodes/Filter/Blur.cpp:687`), fills it (`:705`), and runs the vertical IIR over the buffer's height (`:740`). `LineFilter::applyVanVliet`/`applyDeriche` (`Engine/Nodes/Filter/BlurKernels.cpp:282`/`:239`) start the recursion from a zero (Black) or edge (Nearest) state at the buffer's first row and apply the Triggs end condition at its last row.
  - The recursion therefore starts 1.5×size = 3.6σ from the strip, not at the image edge. An IIR has infinite support, so no finite halo makes it exact.
  - This is inherited from CImgBlur's behaviour under host tiling. The Box filter is bit-identical under strips (checked at 8k, size 50, 256 rows), so the FIR modes are RoI-invariant.
  - Bit-exact tiling of Gaussian Blurs would need the vertical pass's recursion state carried between strips (inherently serial top to bottom), or an acceptance tolerance.

### Effect on the M64 options

- **(a) Strip-task design:** confirmed dropped. Its Design §7 already excluded halo nodes, and adding them with overlap recompute is 0.5–0.8x at the heights that keep memory low, with par (its threading model) the worst for memory. No configuration from 8k to 24k gave a speed-up beyond noise.
- **(b) Fused point-op segments after a fast path:** unchanged. Spatial nodes end a segment, as already planned.
- **(c) Cancel:** still not recommended, for the point-op reasons above.
- **Large-frame memory (P1.T3/P1.T4):** the measured case is real but bounded. Untiled 24k peaks at ~2.9 frames (13.8 GB) regardless of chain length, so it fits this host. On a 16–32 GB machine, or with several branches alive at once, it would not. If that matters later, the design that pays is a **serial, bounded-memory strip mode**: strips top to bottom, all threads inside each node, per-node sliding row windows instead of cache images, and IIR vertical state carried between strips. It is not a parallel strip-task scheduler. Expect it to be ~0.8–0.9x the speed for a 4–10x lower peak. It is a separate, lower-priority item from M64's speed goal. The INDEX line is unchanged.

### Not measured

- Option 2 (the ring-buffer sliding window).
- Transform or other spatial nodes; only Blur was run.
- 24k medians: one run per config.
- A host with less RAM, where whole-frame 24k would actually fail.
- Strip heights above 3072, where ser approaches whole speed and loses its memory edge.
