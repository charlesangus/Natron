# M90 - Render Path Serial Overhead

At UHD float on 16 threads, the work around each native node kernel is still serial, and it caps every native node at 4–6 of 16 threads. M84.P1.T1 measured it; this milestone removes it.

The ~50 ms "memmove" is the plain `memcpy` loop in `Image::pasteFromForDepth<float>`. It is not the `isAllocatedOnTheFly` paste at `EffectInstance.cpp:3163`. It is the paste at `EffectInstance.cpp:3279`, in `renderHandler`'s non-proxy branch. Three things cause it:
- Native image effects are `eRenderSafetyFullySafe`, so `renderRoIInternal` makes one `tiledRenderingFunctor` call over the whole RoI and the paste runs on one thread.
- The bench's generator-fed Blur and Grade are cache-backed (`Node::shouldCacheOutput` caches nodes that don't vary per frame), and cache images use bitmaps.
- `renderHandler` therefore renders into a private, bitmap-free `tmpImage` and copies it back while holding the cached image's write lock.

The same block also runs `checkForNaNsAndFix` (on by default) as a second single-threaded full-frame pass under that write lock.

This milestone parallelises those image passes, then lets native effects render straight into the mapped image so the paste and its full-frame temporary go away. Last, after M85 lands, it trims `writeWindow`'s serial overhead and regenerates M84's cost seed.

- At UHD float on 16 threads, every Blur task spends about 100 ms in serial work that doesn't depend on σ or filter:
  - ~50 ms in a single-threaded `memmove` in `Image::pasteFromForDepth<float>`, called from `EffectInstance::renderHandler` after `render()` returns. An `eu-stack` profile put 72% of busy samples there. Pinned down: it is `downscaleImage->pasteFrom(*tmpImage, …)` at `Engine/EffectInstance.cpp:3279`.
  - ~15 ms in `Blur::writeWindow`. The template is `SpatialFilter::writeWindow` in `Engine/Nodes/Filter/SpatialFilterSupport.h:243-329`, called from `Blur.cpp:840`. It is already banded.
  - ~15 ms in other `renderRoI` work, such as image allocation. Candidates: the serial `checkForNaNsAndFix` pass, `Bitmap::initialize` zeroing the 1-byte-per-pixel trimap twice, and allocating the temporary.
- IIR Blur is flat at ~19 ms/Mpx against ~3.4 ms/Mpx for the bare kernel; FIR at σ3 is 12× its bare kernel. Grade carries ~5 ms/Mpx of the same overhead (9.4 in-graph vs 3.4 bare).
- The overhead caps every native node's in-graph parallelism at roughly 4–6 of 16 threads, and skews the CPU side of M84 - GPU Placement And Residency's cost model.

Phase 90.3 waits for M85 - Native GPU Kernels to merge, because M85 edits `Blur.{h,cpp}` and `NativeImageEffect.{h,cpp}` and regenerates `Engine/GpuCostSeed.inc` (user, 2026-10-14; see `DECISIONS/2026-10-14-parked-and-manifest-overlap.md`).

**Builds:** this host builds remotely (`tools/ci/remote/build.sh`), one build per phase. Bench tasks (P1.T1, P2.T4, P3.T2) measure a stripped `release` AppImage on the user's host, never on a GitHub runner.

## Scope

**In:**
- A row-band parallel helper shared by the image passes, plus banded `Image::pasteFromForDepth` and `Image::checkForNaNsAndFix`.
- Direct rendering into the cache-backed mapped image for native effects that opt in, removing the post-`render()` paste and its full-frame temporary (133 MB at UHD float, 530 MB at 8K).
- Removing the serial `renderRoI` residue that P1.T1 attributes, within `Engine/Image*` and the `renderHandler` allocation and paste block.
- After M85: `writeWindow` overhead, the Blur acceptance bench, and regenerating `Engine/GpuCostSeed.inc`.

**Out:**
- `EffectInstance.cpp` outside `renderHandler`'s tmpImage, paste and NaN block (M100 - Content Node Hashing owns the hash and age code), and all of `Engine/ParallelRenderArgs.cpp`.
- `Blur.{h,cpp}` and `NativeImageEffect.{h,cpp}` before M85 merges.
- Parallelising `convertToFormat` (the component or depth mismatch paste at `:3170`/`:3273`). Native effects match their output format, so they don't hit it. Record it as a follow-up if P1.T1 shows it in another node's profile.
- The proxy path (`renderFullScaleThenDownscale`). Native effects support render scale, so it keeps its temporary.
- OFX plugins. They keep the temporary, because the host can't trust them to write only inside the render window.
- Kernel-pass speed (fill, H and V passes). If 1.5× isn't reached once the overhead is gone, the residual goes to the user, not into this milestone.

**Edge cases:**
- **Host tiling.** A `FullySafeFrame` render with several `rectsToRender` must keep the temporary. Rendering those tiles straight into one image would serialise them on its write lock (`Image::WriteAccess` holds `lockForWrite` for its lifetime).
- **Two renders of one cached image.** When two `renderRoI` calls need disjoint RoIs of the same cached image, they now serialise on its write lock for the whole render instead of only for the paste. Acceptable: trimap marking already makes overlapping RoIs wait.
- **Abort mid-render.** Pixels already written into the cached image stay marked unrendered: `clearBitmap` runs at `:3099`, and the trimap guard is invalidated.
- **Layers allocated on the fly** (`allocateImageLayerAndSetInThreadLocalStorage`, `:3359`). They keep their temporary and benefit from the parallel paste.
- **Storage modes.** Disk-cache (mmap) images and GL textures keep the temporary. Direct rendering applies to RAM storage only.
- **Small RoIs and modest machines.** Below a minimum area (the existing `kMinParallelCopyUnProcessedArea` is 256×256), or with `getNCPUsAvailableForEffect()` at 1, the passes stay serial.
- **Nested parallelism.** These passes already run on pool threads inside parallel regions. They size their bands from `AppTLS` thread budgets via `getNCPUsAvailableForEffect()`, and `parallelForOnGlobalPool` has the caller take part.
- **Frame-varying footage.** Plate-fed nodes may not be cached, so they take the uncached path (`Image(key, params)`, `_useBitmap=false`), where `tmpImage == renderMappedImage` already. This must not regress.

**Assumptions:**
- The bare-kernel references are the `CpuTwinBench` numbers in M84.P1.T1: FIR 1.44 / 5.5 / 19.9 ms/Mpx at σ 3 / 25 / 100; Grade and IIR ~3.4.
- The serial budget is ~12 ms/Mpx at UHD. Removing the paste, the NaN pass and allocation should bring Grade close to 1.5×. Blur IIR may stay above 1.5× because its passes run at 1.3–1.8× bare; P3.T2 reports the gap instead of widening scope.
- Native effects write only inside `args.roi`, and none assumes the output image's bounds equal the RoI. P2.T1 checks this.

## Phase 90.1: Parallel image passes and baseline

All of this phase runs now. It touches neither `Blur.cpp` nor `NativeImageEffect.*`, nor `EffectInstance.cpp`.

- [x] M90.P1.T1 — Measure the baseline on current `main` and split the ~100 ms serial time by function
  - files: none (results go in this file's `## Decisions`); `tools/bench/` only if a flag is missing
  - approach:
    - Use a stripped release AppImage of `main` on the user's host, 16 threads.
    - Run `graph_bench.py` at `BENCH_RES=uhd`, `BENCH_N=1`, with `NATRON_RENDER_PROFILE=build/bench/m90/base-<case>.jsonl`, for:
      - `BENCH_TOPO=chain` (Grade);
      - `BENCH_TOPO=blurchain` with `BENCH_BLUR_SIZE` 7.2 / 60 / 240 (σ 3 / 25 / 100, FIR);
      - `BENCH_TOPO=blurchain` with `BENCH_BLUR_FILTER` set to the Gaussian (IIR) option, the same one M84.P1.T1 used.
    - Run every case once in each scheduler mode (`NATRON_RENDER_SCHEDULER=legacy|taskgraph`).
    - Sample with `eu-stack` (memory: natron-profiling-in-container) and `tools/bench/sample_stacks.sh` / `analyze_stacks.py`. Attribute the serial time to `pasteFromForDepth`; `checkForNaNsAndFix`; `Bitmap::initialize` / `markForRendered` / `getRestToRender`; `HostImageMemory::allocate` and first-touch page faults; `writeWindow`.
    - Confirm the paste comes from `:3279` (the non-proxy, non-on-the-fly branch), not `:3172`.
    - Run one plate-fed `readchain` case to check that the uncached path has no paste.
  - verify: a Decisions entry with per-task ms/Mpx per case and mode, the per-function split of the serial time, and the bare-kernel ratios. P3.T2 reuses these as its "before" numbers.
  - size: M

- [x] M90.P1.T2 — Factor out a shared row-band helper and band `Image::pasteFromForDepth`
  - files: `Engine/PoolParallelFor.h`, `Engine/ImageCopyChannels.cpp`, `Engine/Image.cpp`, `Tests/ImagePasteBands_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Move `forEachCopyUnProcessedRowBand` (`ImageCopyChannels.cpp:152-176`, anonymous namespace) into `PoolParallelFor.h` as a general `forEachRowBand(const RectI&, qint64 minArea, const std::function<void(int y1, int y2)>&)`. It keeps the band sizing on `appPTR->getNCPUsAvailableForEffect()` and the `kMinParallelCopyUnProcessedArea`-style threshold, and dispatches through `parallelForOnGlobalPool`. Switch `ImageCopyChannels.cpp` over to it with no change in behaviour.
    - In `pasteFromForDepth<PIX>` (`Image.cpp:881-940`), keep the `QWriteLocker`/`QReadLocker`, the intersection and `copyBitmapPortion` on the calling thread. Replace only the row `memcpy` loop with `forEachRowBand`. Helpers compute their row pointers from the base `src`/`dst` and the row strides, and take no locks, the same way `ImageCopyChannels` helpers do.
    - Use `std::ptrdiff_t` for the strides. Today they are `int`, which overflows past roughly 2^31 elements at 16K.
  - verify:
    - The new gtest pastes byte, short and float images through partial overlaps, offset bounds, src ⊂ dst and dst ⊂ src. Cases above and below the area threshold must match a reference serial copy bit for bit.
    - A large (≥ 2048²) case runs with a 1-thread budget (`AppTLS::ThreadBudgetScope`) and with the full pool.
    - `ImageCopyUnProcessedChannelsTest.*` in `Tests/Image_Test.cpp` still passes.
  - size: M

- [x] M90.P1.T3 — Band `Image::checkForNaNsAndFix` and `checkForNaNsNoLock`
  - files: `Engine/Image.cpp`, `Tests/ImageNaNBands_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - In `checkForNaNsAndFix` (`Image.cpp:2085`), keep the write lock on the caller and run the row scan through `forEachRowBand` from P1.T2. Each band writes its own `hasnan` flag, and the flags are ORed after the join.
    - Do the same for `checkForNaNsNoLock` (`:2118`).
    - Keep the replacement value (1.0) and the `std::isnan` test unchanged.
    - Depends on P1.T2's helper: dispatch after P1.T2 lands.
  - verify:
    - The new gtest puts NaNs in the first row, the last row and on a band boundary of a large image. It checks the return value, that exactly those values become 1, and that a NaN-free image returns false and stays unchanged.
    - `Tests/Native/NativeNaNWarning_Test.cpp` passes.
  - size: M

- [x] M90.P1.T4 — Remove the serial `renderRoI` residue that P1.T1 attributes to image and bitmap setup
  - files: `Engine/Image.h` (`Bitmap`), `Engine/Image.cpp`, `Tests/Image_Test.cpp` (the `BitmapTest` cases), plus at most one more `Engine/Image*.cpp` named by P1.T1
  - approach:
    - Always: `Bitmap::initialize` (`Image.h:91-97`) zeroes the map twice, once in `_map.resize()` and again in `std::fill`. Zero it once.
    - Also always: replace the per-byte fill in `Bitmap::setTo1` and in the large-area `markFor` / `clearBitmap` paths with row `memset`s. Band them with `forEachRowBand` only above the threshold.
    - Anything else P1.T1 measures above ~2 ms per UHD task in `Engine/Image*` (for example `getRestToRender`) goes in here.
    - If P1.T1 points outside `Engine/Image*` and outside `renderHandler`'s block, record it for the user instead of fixing it.
  - verify: `BitmapTest.*` in `Tests/Image_Test.cpp` passes, plus a new case that checks `getRestToRender` after `markForRendered` / `clearBitmap` on a large bitmap.
  - size: M

- [x] M90.P1.T5 — Remove the render-path NaN check app-wide
  - files: `Engine/Image.{h,cpp}` (`checkForNaNsAndFix`, `checkForNaNsNoLock`), `Engine/EffectInstance.cpp` (the call in `renderHandler`'s tmpImage block only), `Engine/Settings.{h,cpp}` (the NaN-handling preference), any other callers, `Tests/ImageNaNBands_Test.cpp` (delete), `Tests/Native/NativeNaNWarning_Test.cpp` and `Tests/CMakeLists.txt`
  - approach: per `DECISIONS/2026-10-14-drop-nan-check.md`, delete the NaN scan/replace, its call, the preference and its settings plumbing (and any UI that exposes it), plus the banded version P1.T3 added. Find every caller first. Keep any per-node NaN *warning* UI only if it doesn't depend on the scan; otherwise remove it and its test, and report what went.
  - verify: `grep -rn checkForNaNs` finds nothing; the full ctest passes at the phase build.
  - size: M

**Phase gate:** one remote build with `--tests`; full ctest in both scheduler modes (`NATRON_RENDER_SCHEDULER=legacy` and `taskgraph`), no new failures (the known CImg-asset failure excepted).

## Phase 90.2: Render native effects straight into the mapped image

This phase runs now. It edits only `renderHandler`'s tmpImage decision block in `EffectInstance.cpp` (`:2966-2994`), which M100 doesn't touch, plus `EffectInstance.h` and `NativeEffectBase.h`, which M85 doesn't touch. It stays out of `NativeImageEffect.*` and `Blur.cpp`.

- [x] M90.P2.T1 — Check that every native effect writes only inside `args.roi`
  - files: read-only across `Engine/Nodes/**`; results go in `## Decisions`. Opt-out overrides go only in classes outside M85's file list.
  - approach:
    - For every class under `Engine/Nodes` that reaches `NativeEffectBase`, check that `render()` writes only inside `args.roi` and never sizes a loop from the output image's `getBounds()`. That covers `NativeImageEffect` and its subclasses, and `NativeEffectBase` direct subclasses: `Shuffle`, `AddLayers`, `RemoveLayers`, `TypedPassthrough`, `NativeWrite`, and the Deep nodes.
    - Grep for `getBounds()` on output images, `fillZero`, and writes through `outputLayers`.
    - A class that fails is listed with file:line and opted out in P2.T2. If the offending class is `NativeImageEffect` or `Blur`, the opt-out moves to Phase 90.3.
  - verify: a Decisions entry lists each class with its verdict and evidence; P2.T2 consumes the opt-out list.
  - size: M

- [ ] M90.P2.T2 — Let opted-in effects render into the cache-backed mapped image, with no temporary and no paste
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp` (the `renderHandler` block at `:2966-2994` only), `Engine/Nodes/NativeEffectBase.h`, plus the opt-out overrides from P2.T1
  - approach:
    - Add a virtual `EffectInstance::rendersOnlyInsideRenderWindow() const`, default `false`. Override it to `true` in `NativeEffectBase`, and to `false` in each class P2.T1 flagged.
    - In `renderHandler`, set `it->second.tmpImage = it->second.renderMappedImage` when all hold: the effect opts in; `!layers.useOpenGL`; `renderMappedImage->getStorageMode() == eStorageModeRAM`; `prefComp == renderMappedImage->getComponents()`; `outputClipPrefDepth == renderMappedImage->getBitDepth()`; `layers.rectsToRender.size() == 1` (host-tiled `FullySafeFrame` renders keep the temporary).
    - With `tmpImage == downscaleImage`, the existing `tmpImage != downscaleImage` guard at `:3263` skips the paste.
    - Comment at the decision point why this is safe: the trimap marks the RoI as being rendered, so readers wait; the effect's `Image::WriteAccess` holds the image's write lock, so a concurrent `ensureBounds` can't reallocate the buffer under the kernel; abort clears the bitmap at `:3099`.
    - Leave the paste blocks at `:3160-3174` and `:3176-3259` unchanged.
  - verify: P2.T3's tests; plus `Tests/Native/*` and `RenderBothWaysSelf_Test` passing in both scheduler modes at the phase build.
  - size: M

- [ ] M90.P2.T3 — Test that direct output is correct, in both schedulers and under concurrency
  - files: `Tests/Native/NativeDirectOutput_Test.cpp` (new; picked up by the `Native/*_Test.cpp` GLOB), and a new test-only effect header if needed. Don't edit `Tests/CountingTestEffect.h`, so parallel lanes don't share a file.
  - approach:
    - **Path taken.** A test effect with `rendersOnlyInsideRenderWindow()` true records `args.outputLayers.front().second->usesBitMap()` in `render()`. With `forceCaching` on, it must see a bitmap-backed image (temporaries are created with `useBitmap=false`). With the override false, or with two rects, it must see a temporary.
    - **Pixels.** Render native Grade, Invert and CPU Blur at an offset, non-power-of-two RoI with `forceCaching` on. Compare bit for bit with `renderBothWaysDirect` / `renderNodePlanesDirect` (both bypass the cache), and check the cached image's bitmap is fully marked over the RoI.
    - **Both schedulers.** `renderBothWays` on a cached Constant→Grade→Write chain with pool sizes {1, 4, 16}.
    - **Concurrency.** Two threads render disjoint RoIs of the same cached node at once. Both finish, the pixels are correct, and there is no deadlock (use a timeout).
    - **Abort.** Abort mid-render, then check the bitmap over the RoI is cleared and a re-render produces correct pixels.
  - verify: `ctest -R NativeDirectOutput` passes with `NATRON_RENDER_SCHEDULER=legacy` and with `taskgraph`.
  - size: L

- [ ] M90.P2.T4 — Measure the Grade half of the acceptance criteria
  - files: none (results go in `## Decisions`)
  - approach:
    - On a stripped release AppImage of the Phase 90.2 branch, rerun P1.T1's `chain` (Grade) and `blurchain` cases under `NATRON_RENDER_PROFILE`, in both scheduler modes.
    - Report per-task ms/Mpx against the bare 3.4, using `python3 tools/bench/render-profile-report.py build/bench/m90/p2-<case>.jsonl`.
    - Re-sample once with `eu-stack` to confirm `pasteFromForDepth` is gone from the native path and that the NaN pass is banded.
    - Run a `readchain` (uncached) case to confirm it hasn't regressed.
  - verify: a Decisions entry showing in-graph Grade ≤ ~5.1 ms/Mpx (1.5 × 3.4) at UHD on 16 threads, the new Blur numbers, and no regression on `readchain` beyond ±5%.
  - size: M

**Phase gate:** one remote build with `--tests`; full ctest in both scheduler modes, no new failures.

## Phase 90.3: After M85 merges — `writeWindow`, Blur acceptance, cost seed

This phase starts only after M85 - Native GPU Kernels has merged into `main`. M85 rewrites `Engine/Nodes/Filter/Blur.{h,cpp}` and `NativeImageEffect.{h,cpp}`, and its GPU finish pass must stay bit-exact with `SpatialFilter::writeWindow`. It also regenerates `Engine/GpuCostSeed.inc`, which this phase regenerates again on top of M85's version. Rebase onto the post-M85 `main` before starting.

- [ ] M90.P3.T1 — Cut `writeWindow`'s overhead without changing its output
  - files: `Engine/Nodes/Filter/SpatialFilterSupport.h`, `Engine/Nodes/Filter/SpatialFilterSupport.cpp` (`readSourceRow` / `readChannelRow`), `Engine/Nodes/Filter/Blur.cpp` (only if the call at `:840` must change), plus any Phase 90.2 opt-out that P2.T1 assigned to `NativeImageEffect` or `Blur`
  - approach:
    - First profile `writeWindow` on the post-M85 build.
    - Hoist the per-band `std::vector` scratch rows out of the row loop. Compute the per-channel `planes.planeOfChannel` lookup once per band.
    - Add a fast path for rows fully inside both `processWindow` and `bufferRect` with no divisor, no mask and `mix == 1`. It writes from the planar buffer straight to `dstPix` without the per-pixel branches, and reads the source row only when an unprocessed channel needs it.
    - Keep the operation order (output → premultiply → mix) exactly as it is. M85's `recordGpuFinish` mirrors it.
    - Don't touch `fillBuffer` or the H and V passes.
    - Also cut `Blur::render`'s planar buffer allocate/zero/free (26% of busy samples in P1.T1's σ3 profile): reuse or skip zeroing where every element is written.
  - verify: `NativeBlur_Test`, `NativeErodeDilate_Test`, `NativeEdgeDetect_Test`, `NativeBlurGpu_Test` and `BlurFIRGaussian_Test` pass bit for bit. The `eu-stack` share of `writeWindow` per UHD task drops below ~5 ms (recorded in Decisions).
  - size: M

- [ ] M90.P3.T2 — Measure the Blur half of the acceptance criteria
  - files: none (results go in `## Decisions`)
  - approach:
    - Stripped release AppImage. Rerun P1.T1's `blurchain` cases (FIR σ 3/25/100 and IIR) and `chain`, at UHD on 16 threads, in both scheduler modes, under `NATRON_RENDER_PROFILE`.
    - Also run 8K FIR σ25, and run with `--setting noRenderThreads=1` for the serial half.
    - GPU placement off, so the CPU path is what gets measured.
  - verify: a before/after Decisions table against P1.T1: in-graph UHD Blur IIR and FIR σ3 within ~1.5× of bare (IIR ≤ ~5.1, FIR σ3 ≤ ~2.2 ms/Mpx); Grade still ≤ ~5.1; any case above 1.5× is listed with its remaining profile split and put to the user, not fixed here.
  - size: M

- [ ] M90.P3.T3 — Regenerate `Engine/GpuCostSeed.inc` from the new CPU numbers
  - files: `tools/bench/render-profile-report.py` (`BLUR_SIGMA_MEASURED`, `:270-275`), `Engine/GpuCostSeed.inc` (regenerated)
  - approach:
    - Replace the hard-coded Blur σ rows (serial and threaded ms/Mpx per bucket) with P3.T2's numbers.
    - Re-run `tools/bench/comps/run_comps.sh` in threaded mode and with `noRenderThreads=1`, so the CPU rows reflect the reduced overhead for every native node. Keep M85's GPU rows (`GPU_KERNEL_UHD_MS`).
    - Run `tools/bench/render-profile-report.py --serial-profile …=… … --emit-cost-table Engine/GpuCostSeed.inc`, and update its `--self-test` expectations if they pin the old values.
  - verify: `render-profile-report.py --self-test` and `ctest -R "GpuPlacement|GpuCostModel"` pass, and the diff of `GpuCostSeed.inc` shows lower CPU ms/Mpx for Grade and Blur with the GPU rows unchanged.
  - size: M

**Verification gate:** full ctest passes in both `NATRON_RENDER_SCHEDULER=legacy` and `taskgraph`, with no new failures (CImg-asset failure excepted). `NativeDirectOutput`, `ImagePasteBands` and `ImageNaNBands` pass. The P2.T4 and P3.T2 Decisions entries show in-graph UHD Grade and Blur at 16 threads within ~1.5× of their bare-kernel ms/Mpx (`graph_bench.py` `chain`/`blurchain` under `NATRON_RENDER_PROFILE`), or put any measured exception to the user. `Engine/GpuCostSeed.inc` is regenerated on top of M85's version.

## Decisions

- 2026-10-14 — **Elaborated at claim** (consultant scouting `main` at `e93abc919`): the paste is `EffectInstance.cpp:3279`, not `:3163`; it is serial because native effects are `FullySafe` and render the whole RoI in one call, and it exists because cached images carry a bitmap. Fix order: band the image passes, then render natives straight into the mapped image, then (post-M85) `writeWindow` and the cost seed.
- 2026-10-14 — **Bench host** (user): this PM's host has 4 cores / 15 GB. Benches run here at 4 threads to steer the work, judged as ratios to the bare kernel; the user re-runs P2.T4 and P3.T2 on their 16-thread host before the gate.
- 2026-10-14 — **P2.T1 audit: no opt-outs.** Every `NativeEffectBase` class that writes an `Image` (NativeImageEffect and its point ops, Blur, ErodeDilate, EdgeDetect via `SpatialFilter::writeWindow`, Merge, Dissolve, Keyer, ChromaKeyer, Crop, Reformat, Position, Transform, NativeGenerator, Shuffle, AddLayers, RemoveLayers, NativeRead, NativeWrite, DeepToImage) writes only inside `args.roi` or a rect clipped to it. Deep nodes write `DeepImage`s, outside the cached `Image` path. The audit flagged that whole-image `WriteAccess`/`fillZero` locks serialise disjoint-RoI renders of one cached image; that is the accepted edge case in `## Scope`.
- 2026-10-14 — **P1.T1 baseline (4-thread Intel N100, base AppImage `e93abc91` run inside natron-dev, UHD, 3-repeat medians; taskgraph per-task ms/Mpx, frame 0 dropped).** Grade chain: Grade 15.5. Blur FIR σ3 33.7, σ25 132.3, σ100 414.9; IIR 71.3. readchain: Read 23.7, Grade 15.3. Legacy wall (no per-task profile in legacy): chain 403 ms, fir3 706, fir25 1538, fir100 3888, iir 1009, read 738. IIR scales ~linearly from the 16-thread figure (71 vs 19); Grade doesn't (15.5 vs 9.4), consistent with a serial residue. No bare-kernel bench here (`CpuTwinBench` needs building). **Serial split** (eu-stack, legacy, symbolised but older M83-era NatronRenderer, so indicative only; thread-samples, which understate serial wall share): Grade: paste 46%, `checkForNaNsAndFix` 35%, kernel 19%. Blur FIR σ3: `Blur::render` plane memset/munmap 26%, `checkForNaNsAndFix` 20%, `writeWindow` outside kernel 20%, kernel 18%, paste 17%. Bitmap init/mark/getRestToRender <1%, so P1.T4 needs nothing beyond its fixed part. Blur's plane allocate/zero/free lives in `Blur.cpp`; it joins P3.T1. Rerun scripts: `build/bench/m90/{run.sh,matrix.sh,an.py,samp-rel.sh,split.py}` (paths hard-coded to this host).
- 2026-10-14 — **NaN check dropped app-wide** (user; `DECISIONS/2026-10-14-drop-nan-check.md`): P1.T3's banding is superseded by P1.T5, which deletes the pass.
- 2026-10-14 — **P1.T5** (code: 1da538805): removing the preference also removed the `doNansHandling` render arg from `Engine/ParallelRenderArgs.{h,cpp}` and `ViewerInstance.cpp`, and the scan's warning plumbing from `Engine/Node.{h,cpp}` / `NodePrivate.h` (`setNaNWarning`, `clearStaleNaNWarning`, `nanWarningHash`). That breaks this milestone's `Out` on `ParallelRenderArgs.cpp`, a file M100 - Content Node Hashing also edits (P3.T3, `buildArgsMap`). The edits are small deletions, so whichever lands second resolves them at rebase. `Tests/Native/NativeNaNWarning_Test.cpp` and `Tests/ImageNaNBands_Test.cpp` are deleted.
- 2026-10-14 — **Phase 90.1 gate green** at b378b0e41 (remote run 38663940820): 2171/2171 in legacy and taskgraph. P1.T2 0d13f81a5, P1.T3+T4 92ffc6870 (P1.T3's banded NaN scan was later deleted by P1.T5).
