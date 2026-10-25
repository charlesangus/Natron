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

- [ ] M81.P1.T1 — Record RawHotPixels' pass on the GPU
  - files: new `Engine/Gpu/Kernels/rawhotpixels.slang`, `rawhotpixels.cmake`, `Engine/Nodes/Raw/RawHotPixels.cpp`, `Engine/Nodes/Raw/RawGpuParams.h`, new `Tests/Native/NativeRawHotPixelsGpu_Test.cpp`, `Engine/Nodes/Raw/docs/RawHotPixels.md`, `Tests/Native/NativeGpuCapability_Test.cpp` (only if the source-less node answers true)
  - approach: This is the first raw Passes op on the GPU.
    - Set `list.gpu` in `RawHotPixelsOp::passes` (`RawHotPixels.cpp:250-264`), with a `PassGpuRecorder` (`PassExecutor.h:377-411`), as `edgeDetectGpuRecorder` does (`EdgeDetect.cpp:732-900,1054`).
    - Port `hotPixelsWorker` (`:116`) from the C path: Bayer uses 2-px neighbours and non-CFA a halo of 1. For X-Trans, scan `kXTransSearch` with `colourAt()` in the kernel instead of uploading `xtransNeighbours()`.
    - Write zero outside `sourceValid`.
    - Mark the comparison operands precise, so FMA contraction can't flip a decision.
  - verify: `expectGpuMatchesCpu` (`RawGpuTestSupport.h:170`) on CR2, RAF and linear DNG with injected hot pixels, under `ParityTolerance::exact()` (record any measured exception); identity at strength 0; tiled = full frame.
  - size: M

## Phase 81.2: VNG and Markesteijn

- [ ] M81.P2.T1 — VNG kernels (Bayer VNG4 and X-Trans) and an outlier-budget parity helper
  - files: new `Engine/Gpu/Kernels/demosaic_vng.slang`, `demosaic_vng.cmake`, `Engine/Nodes/Raw/RawDemosaic.cpp`, `Engine/Nodes/Raw/RawGpuParams.h`, `Tests/Native/RawGpuTestSupport.h`, new `Tests/Native/NativeRawDemosaicXTransGpu_Test.cpp`
  - approach:
    - Port `RawKernels::vng()` and `vngLinearInterpolate()` (`RawKernels.h:170-197`). Check them against darktable's `demosaic_vng.cl` at `DARKTABLE_REF` if present.
    - VNG keeps a halo of 6 (`RawDemosaic.cpp:105-133`). Add VNG to `canRenderOpOnGpu` (`:1168-1191`) and a branch in `renderOpGpu`.
    - Gradient thresholds can flip, so add `expectGpuMatchesCpuWithin(mean, max, outlierBudget)`, following `RawReferenceSupport.h:73-92,333-337`.
    - Update the GPU paragraph of docs/RawDemosaic.md (`:146-160`).
  - verify: VNG on CR2 and RAF, and on a sub-33 crop that falls back to VNG, within the stated mean/max/outlier bound; tiled GPU = full-frame GPU.
  - size: L

- [ ] M81.P2.T2 — Markesteijn 1- and 3-pass kernels (after T1)
  - files: new `Engine/Gpu/Kernels/demosaic_markesteijn.slang`, `demosaic_markesteijn.cmake`, `Engine/Nodes/Raw/RawDemosaic.cpp`, `Engine/Nodes/Raw/RawGpuParams.h`, `Tests/Native/NativeRawDemosaicXTransGpu_Test.cpp`, `docs/RawDemosaic.md`
  - approach:
    - Port `RawKernels::markesteijn()` (`RawDemosaicMarkesteijn.cpp`; tile 122, halos 12/18).
    - Each stage is one dispatch over tiles in `z`: green min/max, the rgb directions, YPbPr/Lab, then homogeneity and its argmax. Scratch comes from `ctx.scratch` (`GpuRecord.h:84`).
    - The border uses T1's linear-interpolate kernel. Keep the below-14/19 fallback to VNG.
  - verify: 1-pass and 3-pass on RAF, inside the frame and at corners, within T1's helper (the argmax can flip, so with an outlier budget); tiled = full; all 36 X-Trans phases flat-neutral.
  - size: L

## Phase 81.3: AMaZE

- [ ] M81.P3.T1 — AMaZE Slang module from scratch, with kernel-level parity
  - files: new `Engine/Gpu/Kernels/demosaic_amaze.slang`, `demosaic_amaze.cmake`, `Engine/Nodes/Raw/RawGpuParams.h`, new `Tests/Gpu/RawAmazeGpu_Test.cpp`
  - approach:
    - darktable has no OpenCL version, so translate `RawKernels::amaze()` (`RawDemosaicAmaze.cpp`, 1169 lines) stage by stage.
    - One entry per stage, over 128-photosite tiles with the CPU's 144-photosite extent (`RawDemosaic.cpp:142-161`) and its per-tile Nyquist bounds.
    - Keep the same operand order as the C path (README "The GPU side of an op").
  - verify: the test dispatches the entries on a synthetic Bayer mosaic and a CR2 crop and compares them with `RawKernels::amaze()` under a stated mean/max/outlier bound.
  - size: L

- [ ] M81.P3.T2 — Wire AMaZE into RawDemosaic's GPU path
  - files: `Engine/Nodes/Raw/RawDemosaic.cpp`, new `Tests/Native/NativeRawDemosaicAmazeGpu_Test.cpp`, `docs/RawDemosaic.md`
  - approach: Allow AMaZE in `canRenderOpOnGpu`. `getOpGpuInputRoI` already returns the tile-aligned span. Band on the 128 grid, so a band's result equals the CPU tile's.
  - verify: CR2 inside and at corners, tiled = full; `MethodsWithoutAGpuPathStayOnTheCpu` updated to the remaining methods.
  - size: M

## Phase 81.4: Raw denoise

- [ ] M81.P4.T1 — RawDenoise wavelet kernels, Bayer and X-Trans
  - files: new `Engine/Gpu/Kernels/rawdenoise.slang`, `rawdenoise.cmake`, `Engine/Nodes/Raw/RawDenoise.cpp`, `Engine/Nodes/Raw/RawGpuParams.h`, new `Tests/Native/NativeRawDenoiseGpu_Test.cpp`, `docs/RawDenoise.md`
  - approach: This fits the GPU: there is no whole-frame statistic, just 5 levels of separable hat passes with soft thresholds (`RawDenoiseKernel.cpp:101-203`).
    - Kernels: sqrt plane extraction; `dwtDenoiseVert`/`Horiz` per level, with the CPU's mirrored edges; the X-Trans row repair; write-back.
    - The host computes the noise per band (`computeChannelNoise`) into the push.
    - Override `canRenderOpOnGpu`/`getOpGpuInputRoI`/`renderOpGpu` as RawDemosaic does, with the buffer = `sourceRegionFor()` (halos 64/66, `RawDenoise.cpp:77-78,201-207`).
  - verify: CR2 and RAF under `transcendental()` or a measured bound; tiled = full; identity unchanged.
  - size: L

## Phase 81.5: Chain and bench

- [ ] M81.P5.T1 — Resident-chain test for the new paths
  - files: new `Tests/Native/NativeRawChainGpuExtended_Test.cpp`
  - approach: Like `DevelopChainIsOneResidentRegion` (`NativeRawChainGpu_Test.cpp:142-160`), run `renderEndToEnd()` on two chains: RAF Prepare→HotPixels→Denoise→Temperature→Highlights (Clip)→Markesteijn→InputColor, and the same with AMaZE on CR2.
  - verify: every node `ranInRegion`, one upload, and the end-to-end result within the loosest stated bound.
  - size: M

- [ ] M81.P5.T2 — GPU on/off bench of Markesteijn and AMaZE on the GPU host
  - files: new `tools/bench/raw_gpu_bench.sh`, `tools/bench/README.md`, `docs/RawDemosaic.md`
  - approach:
    - Interleave `NATRON_GPU=off` and `auto` as `gpu_region_bench.sh` does.
    - Run the P5.T1 chains on CR2 and RAF, plus a ~100 MP file.
    - Time the first frame cold, with no warm-up, using the stripped release AppImage and `env -u LD_LIBRARY_PATH`.
    - Probe MemAvailable per case, and compare same-boot pairs only.
    - Do a quick directional pass first, and a matrix only if asked.
  - verify: results.jsonl shows the first-frame GPU speed-up for Markesteijn-1/3 and AMaZE, with the numbers in the doc and the milestone Decisions.
  - size: M

**Verification gate:** PR CI is green (debug with asserts, both scheduler legs, lavapipe), and every new `*Gpu_Test` passes. The RADV run on the RX 7900 XTX (`NATRON_DEV_GPU=1 tools/ci/local/build.sh fast --tests`, `NATRON_TEST_REQUIRE_GPU=1`) passes. Each node doc states its GPU bound. P5.T2 records a measured speed-up for Markesteijn and AMaZE.

## Decisions

- 2026-10-25 — **Elaborated at claim by the gpu PM** (consultant, against main `9ab236449`; the stub's blocker, M80 - Native RAW Support, merged as `9b6196a5c`). **Kernels stay offline Slang:** every M80 raw kernel and M110 port is a `Kernels/<k>.slang` plus `.cmake` (`Engine/Gpu/CMakeLists.txt:23-25` globs them), and M126's runtime GLSL is for generated shaders only. **CA-correct apply is moved to Scope Out** (see Scope): it re-measures between iterations, so a GPU apply alone leaves the CPU doing every apply but the last. **Overlap at claim:** `RawDemosaic.cpp` is shared with M167 - Per-Frame Node Metadata (its hunks at :514, :781 and :891-906, against M81's GPU functions), and `NativeGpuCapability_Test.cpp`'s allowlist with M113 - Native Warp Nodes. Both rows are later on the board, so M81 proceeds (§8a), and Phase 81.2 rebases onto M167 if that merges first. **Risks:** AMaZE and Markesteijn may need outlier budgets wider than CPU-only parity uses, so the bound is measured in P3.T1 before it is fixed. P1.T1 is the first raw run through the generic Passes GPU executor.
