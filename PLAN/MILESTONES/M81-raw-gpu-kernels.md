# M81 - Raw GPU Kernels

Gives the M80 - Native RAW Support nodes their remaining GPU paths: hot pixels, VNG (Bayer VNG4 and X-Trans), Markesteijn 1- and 3-pass, AMaZE and raw denoise. Kernels are offline Slang under `Engine/Gpu/Kernels/`, like M80's raw kernels and M110's ports; M126's runtime GLSL path is for generated shaders only. They build on M80.P6.T2's raw regions (`RawLayoutGpu`, `RawGpuParams.h:48-56`; `rawlayout.slang:39` `colourAt()`) and follow `RawDemosaic::renderOpGpu` (`RawDemosaic.cpp:1228-1372`): one pass per dispatch over the buffer the CPU runs over (`sourceRegionFor()`, `RawDemosaic.cpp:815-865`), in bands that share one scratch of at most 128 MB. The CPU stays the default and the reference. With no usable device the node falls back silently. Each bound goes in the node's doc (README "GPU parity").

## Scope

**In:**
- GPU paths and parity tests for RawHotPixels; RawDemosaic VNG (both CFAs), Markesteijn 1- and 3-pass, and AMaZE; RawDenoise (Bayer and X-Trans).
- A resident-chain test, and a bench on the GPU host.

**Out:**
- CA-correct apply. The shift fit needs the whole mosaic on the CPU (`RawCACorrect.cpp:273-291`), and a raw run never crosses devices (README "Raw nodes", `placeGraph()`). The default is two iterations (`RawCACorrect.cpp:140`), and the second measures the frame the first corrected (`RawCACorrectKernel.cpp:795-931`), so the CPU would end up doing every apply but the last. It needs a full GPU port with estimation, or a CPU-measure pre-pass in the engine, and that is a new milestone.
- The Phase 80.7 modes: Inpaint Opposed, segments and laplacian highlights, flat field, LMMSE, FDC, dual, capture, green equilibration and smoothing. They go to a follow-up milestone, the user's call.
- Lens correction: M160 - Lensfun Lens Correction.
- A per-node GPU knob. Native nodes have none, and the legacy knob is OpenGL (`Node.cpp:3083`), so it is engine-wide, not raw work.

**Edge cases:**
- Below 33 photosites AMaZE becomes VNG, and below 19 Markesteijn-3 becomes VNG (`RawDemosaic.cpp:691-760`). VNG on the GPU keeps those cases on the device.
- The default Develop chain still renders on the CPU, because of Inpaint Opposed. Tests and the bench use Clip highlights.

**Assumptions:**
- "Selectable" means the Preferences GPU compute mode (`Settings.cpp:461`) plus automatic fallback, as in M80 and M110.
- Push blocks stay ≤ 128 bytes (`RawLayoutGpu` is 64, plus dims).

## Phase 81.1: Hot pixels

- [x] M81.P1.T1 — Record RawHotPixels' pass on the GPU
  - files: new `Engine/Gpu/Kernels/rawhotpixels.slang`, `rawhotpixels.cmake`, `Engine/Nodes/Raw/RawHotPixels.cpp`, `Engine/Nodes/Raw/RawGpuParams.h`, new `Tests/Native/NativeRawHotPixelsGpu_Test.cpp`, `Engine/Nodes/Raw/docs/RawHotPixels.md`, `Tests/Native/NativeGpuCapability_Test.cpp` (only if the source-less node answers true)
  - approach: This is the first raw Passes op on the GPU.
    - Set `list.gpu` in `RawHotPixelsOp::passes` (`RawHotPixels.cpp:250-264`), with a `PassGpuRecorder` (`PassExecutor.h:377-411`), as `edgeDetectGpuRecorder` does (`EdgeDetect.cpp:732-900,1054`).
    - Port `hotPixelsWorker` (`:116`) from the C path: Bayer uses 2-px neighbours and non-CFA a halo of 1. For X-Trans, scan `kXTransSearch` with `colourAt()` in the kernel instead of uploading `xtransNeighbours()`.
    - Write zero outside `sourceValid`.
    - Mark the comparison operands precise, so FMA contraction can't flip a decision.
  - verify: `expectGpuMatchesCpu` (`RawGpuTestSupport.h:170`) on CR2, RAF and linear DNG with injected hot pixels, under `ParityTolerance::exact()` (record any measured exception); identity at strength 0; tiled = full frame.
  - size: M

## Phase 81.2: VNG and Markesteijn

- [x] M81.P2.T1 — VNG kernels (Bayer VNG4 and X-Trans) and an outlier-budget parity helper
  - files: new `Engine/Gpu/Kernels/demosaic_vng.slang`, `demosaic_vng.cmake`, `Engine/Nodes/Raw/RawDemosaic.cpp`, `Engine/Nodes/Raw/RawGpuParams.h`, `Tests/Native/RawGpuTestSupport.h`, new `Tests/Native/NativeRawDemosaicXTransGpu_Test.cpp`
  - approach:
    - Port `RawKernels::vng()` and `vngLinearInterpolate()` (`RawKernels.h:170-197`). Check them against darktable's `demosaic_vng.cl` at `DARKTABLE_REF` if present.
    - VNG keeps a halo of 6 (`RawDemosaic.cpp:105-133`). Add VNG to `canRenderOpOnGpu` (`:1168-1191`) and a branch in `renderOpGpu`.
    - Gradient thresholds can flip, so add `expectGpuMatchesCpuWithin(mean, max, outlierBudget)`, following `RawReferenceSupport.h:73-92,333-337`.
    - Update the GPU paragraph of docs/RawDemosaic.md (`:146-160`).
  - verify: VNG on CR2 and RAF, and on a sub-33 crop that falls back to VNG, within the stated mean/max/outlier bound; tiled GPU = full-frame GPU.
  - size: L

- [x] M81.P2.T2 — Markesteijn 1- and 3-pass kernels (after T1)
  - files: new `Engine/Gpu/Kernels/demosaic_markesteijn.slang`, `demosaic_markesteijn.cmake`, `Engine/Nodes/Raw/RawDemosaic.cpp`, `Engine/Nodes/Raw/RawGpuParams.h`, `Tests/Native/NativeRawDemosaicXTransGpu_Test.cpp`, `docs/RawDemosaic.md`
  - approach:
    - Port `RawKernels::markesteijn()` (`RawDemosaicMarkesteijn.cpp`; tile 122, halos 12/18).
    - Each stage is one dispatch over tiles in `z`: green min/max, the rgb directions, YPbPr/Lab, then homogeneity and its argmax. Scratch comes from `ctx.scratch` (`GpuRecord.h:84`).
    - The border uses T1's linear-interpolate kernel. Keep the below-14/19 fallback to VNG.
  - verify: 1-pass and 3-pass on RAF, inside the frame and at corners, within T1's helper (the argmax can flip, so with an outlier budget); tiled = full; all 36 X-Trans phases flat-neutral.
  - size: L

## Phase 81.3: AMaZE

- [x] M81.P3.T1 — AMaZE Slang module from scratch, with kernel-level parity
  - files: new `Engine/Gpu/Kernels/demosaic_amaze.slang`, `demosaic_amaze.cmake`, `Engine/Nodes/Raw/RawGpuParams.h`, new `Tests/Gpu/RawAmazeGpu_Test.cpp`
  - approach:
    - darktable has no OpenCL version, so translate `RawKernels::amaze()` (`RawDemosaicAmaze.cpp`, 1169 lines) stage by stage.
    - One entry per stage, over 128-photosite tiles with the CPU's 144-photosite extent (`RawDemosaic.cpp:142-161`) and its per-tile Nyquist bounds.
    - Keep the same operand order as the C path (README "The GPU side of an op").
  - verify: the test dispatches the entries on a synthetic Bayer mosaic and a CR2 crop and compares them with `RawKernels::amaze()` under a stated mean/max/outlier bound.
  - size: L

- [x] M81.P3.T2 — Wire AMaZE into RawDemosaic's GPU path
  - files: `Engine/Nodes/Raw/RawDemosaic.cpp`, new `Tests/Native/NativeRawDemosaicAmazeGpu_Test.cpp`, `docs/RawDemosaic.md`
  - approach: Allow AMaZE in `canRenderOpOnGpu`. `getOpGpuInputRoI` already returns the tile-aligned span. Band on the 128 grid, so a band's result equals the CPU tile's.
  - verify: CR2 inside and at corners, tiled = full; `MethodsWithoutAGpuPathStayOnTheCpu` updated to the remaining methods.
  - size: M

## Phase 81.4: Raw denoise

- [x] M81.P4.T1 — RawDenoise wavelet kernels, Bayer and X-Trans
  - files: new `Engine/Gpu/Kernels/rawdenoise.slang`, `rawdenoise.cmake`, `Engine/Nodes/Raw/RawDenoise.cpp`, `Engine/Nodes/Raw/RawGpuParams.h`, new `Tests/Native/NativeRawDenoiseGpu_Test.cpp`, `docs/RawDenoise.md`
  - approach: This fits the GPU: there is no whole-frame statistic, just 5 levels of separable hat passes with soft thresholds (`RawDenoiseKernel.cpp:101-203`).
    - Kernels: sqrt plane extraction; `dwtDenoiseVert`/`Horiz` per level, with the CPU's mirrored edges; the X-Trans row repair; write-back.
    - The host computes the noise per band (`computeChannelNoise`) into the push.
    - Override `canRenderOpOnGpu`/`getOpGpuInputRoI`/`renderOpGpu` as RawDemosaic does, with the buffer = `sourceRegionFor()` (halos 64/66, `RawDenoise.cpp:77-78,201-207`).
  - verify: CR2 and RAF under `transcendental()` or a measured bound; tiled = full; identity unchanged.
  - size: L

## Phase 81.5: Chain and bench

- [x] M81.P5.T1 — Resident-chain test for the new paths
  - files: new `Tests/Native/NativeRawChainGpuExtended_Test.cpp`
  - approach: Like `DevelopChainIsOneResidentRegion` (`NativeRawChainGpu_Test.cpp:142-160`), run `renderEndToEnd()` on two chains: RAF Prepare→HotPixels→Denoise→Temperature→Highlights (Clip)→Markesteijn→InputColor, and the same with AMaZE on CR2.
  - verify: every node `ranInRegion`, one upload, and the end-to-end result within the loosest stated bound.
  - size: M

- [x] M81.P5.T2 — GPU on/off bench of Markesteijn and AMaZE on the GPU host
  - files: new `tools/bench/raw_gpu_bench.sh`, `tools/bench/README.md`, `docs/RawDemosaic.md`
  - approach:
    - Interleave `NATRON_GPU=off` and `auto` as `gpu_region_bench.sh` does.
    - Run the P5.T1 chains on CR2 and RAF, plus a ~100 MP file.
    - Time the first frame cold, with no warm-up, using the stripped release AppImage and `env -u LD_LIBRARY_PATH`.
    - Probe MemAvailable per case, and compare same-boot pairs only.
    - Do a quick directional pass first, and a matrix only if asked.
  - verify: results.jsonl shows the first-frame GPU speed-up for Markesteijn-1/3 and AMaZE, with the numbers in the doc and the milestone Decisions.
  - size: M

- [x] M81.P5.T3 — Parallelise Markesteijn's minmax, exactly (inserted after P5.T2's bench)
  - files: `Engine/Gpu/Kernels/demosaic_markesteijn.slang`, `Engine/Nodes/Raw/RawDemosaic.cpp`, `Tests/Native/NativeRawDemosaicXTransGpu_Test.cpp`
  - approach: P5.T2 shows that `markesteijn_minmax` takes ~70–85% of the GPU demosaic time (≈12.6 ms per batch, 54 or 129 batches). Each batch runs 1–51 threads, each walking a 122² tile serially (`demosaic_markesteijn.slang:305-361`). The walk's writes are a pure function of position (`lo`/`hi` reset at each outer-row start at :313, rows r and r+1 only, hops at :341-357 depending only on geometry, data only through `hi == 0` at :326), and walk order decides which write wins. Run one thread per (tile, start row): each thread first simulates the start-row sequence with no memory reads, skips rows that aren't start rows, and doesn't write row r+1 when r+1 is itself a start row. Then raise the batch cap (`RawDemosaic.cpp:1434`) as far as the device budget allows, so fewer batches run.
  - verify: an exhaustive comparison test of the parallel minmax against the serial walk, bit-identical, over all 36 phases, edge `mrow`/`mcol` values and tiles with all-zero greens, plus the existing Markesteijn tests on RADV. Re-bench: Markesteijn-1 and -3 GPU first frame both faster than the CPU's.
  - size: L

**Verification gate:** PR CI is green (debug with asserts, both scheduler legs, lavapipe), and every new `*Gpu_Test` passes. The RADV run on the RX 7900 XTX (`NATRON_DEV_GPU=1 tools/ci/local/build.sh fast --tests`, `NATRON_TEST_REQUIRE_GPU=1`) passes. Each node doc states its GPU bound. P5.T2 records a measured speed-up for Markesteijn and AMaZE.

## Decisions

- 2026-10-25 — **Elaborated at claim by the gpu PM** (consultant, against main `9ab236449`; the stub's blocker, M80 - Native RAW Support, merged as `9b6196a5c`). **Kernels stay offline Slang:** every M80 raw kernel and M110 port is a `Kernels/<k>.slang` plus `.cmake` (`Engine/Gpu/CMakeLists.txt:23-25` globs them), and M126's runtime GLSL is for generated shaders only. **CA-correct apply is moved to Scope Out** (see Scope): it re-measures between iterations, so a GPU apply alone leaves the CPU doing every apply but the last. **Overlap at claim:** `RawDemosaic.cpp` is shared with M167 - Per-Frame Node Metadata (its hunks at :514, :781 and :891-906, against M81's GPU functions), and `NativeGpuCapability_Test.cpp`'s allowlist with M113 - Native Warp Nodes. Both rows are later on the board, so M81 proceeds (§8a), and Phase 81.2 rebases onto M167 if that merges first. **Risks:** AMaZE and Markesteijn may need outlier budgets wider than CPU-only parity uses, so the bound is measured in P3.T1 before it is fixed. P1.T1 is the first raw run through the generic Passes GPU executor.
- 2026-10-25 — **M81.P1.T1 code in (`1c4c887b7`); draft PR #97 opened early**, so PR CI (debug with asserts, both legs) tests each push and serves as the gate's CI line. `rawhotpixels_fill` copies the source into a scratch of the CPU's domain (zero outside `sourceValid`), and `rawhotpixels_detect` ports `hotPixelsWorker`, including the 2-photosite unexamined border (1 without a CFA) and the X-Trans `kXTransSearch` scan via `colourAt()`. `RawHotPixelsGpuParams` is 128 bytes. The tests are `MatchesTheCpuOnEveryFixture` (CR2, RAF, linear DNG; 13 injected hot pixels; 7 windows; `exact()`), `StrengthZeroIsTheIdentity` and `TilesMatchTheFullFrame`. **Watch:** possible denormal flushing in `value * multiplier` at near-zero strength. The RADV run follows when the GPU host frees up.
- 2026-10-25 — **M81.P2.T1 code in (`e42a78065`):** `demosaic_vng.slang` (`vng_linear`, `vng_gradient`, `vng_write`; 8 floats per photosite of scratch in ≤ 128 MB bands; `RawVngGpuPush` 128 bytes). VNG on period 2 or 6 is now GPU-capable, including the small-image fallbacks from AMaZE and Markesteijn. `expectGpuMatchesCpuWithin(mean, max, outlierBudget)` is added. The stated bound is mean 1e-4, max 1e-4, and 1 pixel per 1000 in the window, an estimate that the GPU run confirms. Draft PR #97 CI covers P1.T1 and P2.T1.
- 2026-10-25 — **M81.P2.T2 code in (`c2722bccc`):** Markesteijn 1/3-pass runs on the GPU as 12 Slang entries per band, with `vng_linear` filling the border. The tile stages run 8×8 over 122² tiles in z, using an RGBA plane (≤ 64 MB) plus a tile buffer (≤ 64 MB), batched at 51 tiles for 1-pass and 26 for 3-pass. `RawMarkesteijnGpuPush` is 112 bytes. The bound is the same as VNG: Vulkan division is 2.5 ulp, so a threshold can flip. New tests: `MarkesteijnMatchesTheCpuOnTheRaf` and `MarkesteijnKeepsAFlatGreyNeutralAtEveryPhase` (36 phases). **Phase 81.1–81.2 build:** a RADV run from a detached snapshot worktree at `c2722bccc` (`.local/m81-snap`), so P3.T1 can be implemented in the lane at the same time. PR #97 CI covers lavapipe.
- 2026-10-25 — **Phases 81.1–81.2 pass on RADV** (snapshot `c2722bccc`, local `NATRON_DEV_GPU=1` build, `NATRON_TEST_REQUIRE_GPU=1`, RX 7900 XTX, boot `1a6741d9`): 433/433. `NativeRawHotPixelsGpuTest` (exact parity), `NativeRawDemosaicGpuTest` and `NativeRawDemosaicXTransGpuTest` (VNG and Markesteijn within mean 1e-4 / max 1e-4 / 1‰ outliers, with flat grey neutral at all 36 phases) all pass. The tasks get checked off when PR #97 CI (lavapipe) agrees.
- 2026-10-25 — **M81.P3.T1 code in (`63e261f56`):** 20 AMaZE Slang entries over 160² tiles, one per z. Each tile has 15 planes plus a Nyquist header (~1.5 MB), batched at ≤ 43 tiles (~66 MB). The CPU's lower-border overflow and its `nyquist2` aliasing are reproduced byte for byte. The dispatcher is `recordAmazeGpu()` (`RawAmazeGpu.h`) and `RawAmazeGpuPush` is 96 bytes. `RawAmazeGpu_Test` covers 4 synthetic mosaics and 2 CR2 crops, with a provisional bound of mean 1e-4, max 1e-4 and 1‰ outliers. The first push also carried three unrelated M172 `Gui/` files, reformatted because `git-clang-format` ran against the moved `origin/main`. They were removed by amend before review, and format now runs against the merge base. **M81.P3.T2 code in (`21bcc278c`):** AMaZE is wired into `RawDemosaic`, banded on the CPU's 128 grid, with new `NativeRawDemosaicAmazeGpu_Test`. Phase 81.3's RADV check runs from the snapshot after the kernel-only run.
- 2026-10-25 — **AMaZE kernel passes on RADV the first time** (snapshot `63e261f56`, `NATRON_TEST_REQUIRE_GPU=1`): 434/434, with `RawAmazeGpuTest.MatchesTheCpuOnASyntheticMosaic` and `MatchesTheCpuOnACr2Crop` within the provisional bound. The P3.T2 node-wiring check (`21bcc278c`) is running.
- 2026-10-25 — **M81.P4.T1 code in (`dc369cbfe`):** RawDenoise runs on the GPU: per colour, an extract, 5 levels of vertical then horizontal hat passes, and a write-back (48 dispatches per band on Bayer, 36 on X-Trans). The X-Trans extract is a per-pixel gather that matches the CPU's sequential fill. Scratch is 3 planes in ≤ 128 MB bands. The host computes noise per band via `RawKernels::rawDenoiseChannelNoise`, moved out of an anonymous namespace with its body unchanged. The bound is `transcendental()` (only sqrt is inexact; the soft threshold is continuous), with pass-through and a threshold of 0 exact.
- 2026-10-25 — **Phase 81.3 passes on RADV** (snapshot `21bcc278c`): 435/435, with `NativeRawDemosaicAmazeGpuTest` included. **M81.P5.T1 code in (`33c4566dc`):** `NativeRawChainGpuExtended_Test`, with RAF Markesteijn-3 and CR2 AMaZE Develop chains, Highlights Clip, one region and one upload, and the demosaic bound applied to the end-to-end result.
- 2026-10-25 — **M81.P5.T2 tooling in (`55e0351af`):** `tools/bench/raw_gpu_bench.sh` (off/auto interleaved, a fresh process for each cold first frame, MemAvailable gate, region count from the profile). The bench runs once the release AppImage lands.
- 2026-10-25 — **P5.T2 directional bench** (release AppImage of `33c4566dc`, boot `1a6741d9`, RX 7900 XTX, cold first frame in a fresh process, 3 rounds, off/auto interleaved; `.local/m81/bench-1a6741d9/`). Every chain is one GPU region with 8 tasks. Median first frame, off → auto: CR2 AMaZE 1039 → 504 ms (**2.06×**), RAF Markesteijn-1 1615 → 1450 ms (1.11×), RAF Markesteijn-3 1901 → 2410 ms (**0.79×, slower than the CPU**). The gate needs a speed-up on Markesteijn, so a consultant is diagnosing from the profiles before a fix task is inserted.
- 2026-10-25 — **Markesteijn diagnosis** (consultant, from `.local/m81/bench-1a6741d9/` profiles). Demosaic is ~98% of region kernel time on X-Trans: GPU 906 ms against CPU 433 ms for 1-pass, and 1852 against 721 ms for 3-pass. The fit `t = batches·12.6 ms + 225 ms` matches both cases, so the serial per-tile `minmax` dominates. RawDenoise is 48× faster on the GPU (14 against 690 ms) and RawHotPixels takes 3 ms. Markesteijn-1 only comes out ahead because of RawDenoise. **M81.P5.T3 is inserted** (the exact parallel `minmax` and a larger batch cap). Also found: `gpu_region_summary.py` crashes on the bench'"'"'s marks files (`KeyError: 'profileFrame'`, :127), so it gets a small fix in P5.T3'"'"'s re-bench or follows separately.
- 2026-10-25 — **RADV on `aac5433d6` (P5.T3 included): 443/444.** `NativeRawDemosaicXTransGpuMinmax.ParallelWalksMatchTheSerialWalk` passes (bit-identical over 36 phases × 1/3 passes × 8 sizes), as do the denoise suite and `XTransChainWithMarkesteijnIsOneResidentRegion` (4 of 3072 outliers, mean 9.8e-6). `BayerChainWithAmazeIsOneResidentRegion` fails at 607 of 3072 pixels over 1e-4 relative (mean 3.05e-5), against a budget of 4. Every per-node test passes alone, so compounding (denoise sqrt differences flipping AMaZE thresholds) is the leading hypothesis. Being diagnosed.
- 2026-10-25 — **CR2 AMaZE chain outliers are compounding, not a defect** (diagnosis on RADV). Without denoise, the max error is 5.8e-7. Prepare→Denoise→AMaZE alone reproduces the outliers, and RCD in place of AMaZE gives a max of 9.2e-6. **Deciding check, on the CPU only:** a ±1-ulp nudge to the denoised input moves 29% of CPU AMaZE pixels by > 1e-4 (worst 8.7e-3), against 11 pixels on the prepared input, so denoise leaves AMaZE's direction and Nyquist tests near their thresholds. The outliers follow image content, not the 128-photosite tile edges. **Change (`b017f08f4`):** `kBayerChainMaxBound = 5e-3` for the Bayer chain only. The mean stays 1e-4 and the outlier budget 1‰. Measured max is 1.08e-3 (1.7e-3 in the worst nudged run), with 0 pixels over 5e-3 in the window. The X-Trans chain keeps 1e-4. 6/6 related tests pass on RADV. The bound was measured on one GPU and driver.
- 2026-10-25 — **P5.T3 re-bench** (release AppImage of `b017f08f4`-era HEAD with the parallel `minmax`, boot `1a6741d9`, 3 rounds, cold first frame; `.local/m81/bench2-1a6741d9/`). Off → auto: RAF Markesteijn-1 1618 → 570 ms (**2.84×**, was 1.11×), RAF Markesteijn-3 1892 → 618 ms (**3.06×**, was 0.79×), CR2 AMaZE 1045 → 515 ms (2.03×). Each case is one region, with 18/18 runs clean. The gate's measured speed-up is met for Markesteijn and AMaZE. All tasks are checked off: RADV passed on `aac5433d6`, and on `b017f08f4` for the chain bound. Rebased onto main `325c87422` as `f0afa72d5`; `git-clang-format` is clean. Next: the PR goes ready, then the Codex review.
- 2026-10-25 — **Codex review round 1 (PR #97): 2 findings, both fixed in `948e29709`.** (Major) The fixed 256 MB Markesteijn batch could exceed `maxStorageBufferRange` and bind invalid descriptors. `GpuRecordContext::maxStorageBufferBytes()`/`maxScratchBytes()` now account for pool size-class rounding. Every raw scratch is clamped (demosaic work buffer, Markesteijn batch, AMaZE batch, RawDenoise buffer), the node returns `eStatusFailed` (CPU fallback) when one tile can't fit, and `dispatch()`/`dispatchBindings()` refuse an oversized binding. New tests: `ASmallStorageRangeBatchesTheTilesWithTheSameResult` (bit-identical under an 8 MB cap) and `ATileOverTheStorageRangeFallsBackToTheCpu`. Targeted RADV filters pass. (Nit) Two narrating comments are removed. Threads replied to. The fix touches the shared dispatch path every GPU node uses, so a **second, fix-only Codex round** and a full RADV ctest are running. **Follow-up, not this milestone:** the region strip planner checks image buffers against the pool's size class, not the next class up that the pool may also hand back, so near the limit a region image could now be refused (falling back to the CPU) where it used to bind invalidly. RawHotPixels' whole-domain scratch isn't banded; the dispatch check guards it.
- 2026-10-25 — **Full RADV ctest of `948e29709`: 461/463.** Every M81 suite passes, including both storage-range tests. The 2 failures are `Gpu.` and `GpuNoPushDescriptor.CurveSnapshotPrecisionTest`, from M171 - GPU Correctness Fixes (merged `325c87422`). That test only ran on lavapipe in M171's PR CI. On RADV, Dekker's partial products for tiny |p| fall below FLT_MIN and are flushed, although the residual (~1.7e-38) is just above FLT_MIN, so the test's "exact whenever residual ≥ FLT_MIN" rule is too strict for real hardware. **Fixed in this branch** as a separate test-only commit (PM call; it blocks M81's RADV gate line and M171 is closed). PR #97 lint-ci also failed: shellcheck now covers `tools/bench/*.sh`, and M81's bench script had SC1007/SC2034 warnings, which are being fixed too.
- 2026-10-25 — **Fixes pushed:** `4f3e643ba` (bench-script shellcheck clean at the pinned v0.10.0) and `4a4606ad7` (`CurveSnapshotPrecisionTest`: exactness is asserted only for |p| ≥ FLT_MIN·2^48, where no Dekker partial can be denormal, and below that the error must be finite and within 4·FLT_MIN). That test now passes on RADV in both suites. All other suites passed in the full RADV run of `948e29709`; these two commits touch only that test and the bench script. PR CI is re-running, and M81 merges when it is green.
- 2026-10-25 — **PR CI of `4a4606ad7`: two failures.** (1) `NativeRawHotPixelsGpuTest` hits the 300 s ctest timeout on both debug lavapipe legs, and every other suite passes. It takes 54 s even on a RADV fast build (44 s in `MatchesTheCpuOnEveryFixture`, for a 96×64 window). Earlier PR runs were all cancelled before reaching it, so this is the first time it showed up. An implementer is finding where the time goes, a per-render product cost or the test's per-iteration setup, and fixing it. (2) lint-ci: `check_kernel_fma.sh` still allowlists `resample.slang`, which no longer has an fma. This also fails on main since `41d21705c`. The one-line allowlist fix goes in this branch so the PR can go green.
- 2026-10-25 — **Timeout fixed in `570b7d64a` (test-only, plus lint).** The slowness was in the test, not the product. Each CPU render bypasses the cache, so it calls `clearAllCaches()`, and that re-decoded the fixture's 22–24 MP raw Read, which was still in the project. Recomputing the Read's metadata needs a full decode, because rawspeed has no header-only parse. The test now removes the Read and RawPrepare once it has their window, with the same coverage. On RADV the HotPixels suites take **0.86 s, down from 54 s**, with 11/11 passing. The fma allowlist is now empty, and the lint passes. **Product note (not this milestone):** Clear All Caches fully re-decodes every raw Read in a project. PR CI is re-running.
