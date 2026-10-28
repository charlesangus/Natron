# M268 - Edge-Aware Region Independence

Make every node built on the M209 - Edge-Aware Filter Library produce the same pixels whatever window or tile is rendered, and make that a contract that is written down and tested rather than an accident. The library keeps its whole-image functions, and the contract says the buffer is the node's whole frame. A shared node-side helper builds that frame (clamped to the format when the source is infinite), caches the whole-frame result and checks that the frame fits the GPU. Every EdgeAware/darktable node (DenoiseProfiled, Retouch, ToneEqualizer, ContrastTexture, ColorEqualizer, RawHighlights) gets a regression test that compares the full frame against offset windows and tiles, on CPU and GPU. Along the way: ContrastTexture and Retouch stop recomputing the whole frame on every tile, the GPU frame-size guards move into `canRenderOpOnGpu` everywhere, and DenoiseProfiled's Compute Variance measures the whole frame.

Deps: M209 - Edge-Aware Filter Library (on `main`), M210 - Profiled Denoise, M211 - Wavelet Retouch, M212 - Tone Equalizer, M213 - Contrast And Texture, M215 - Colour Equalizer. M210–M215 are all in UAT and not on `main`. The milestone runs on one branch, `milestone/m268-edge-aware-region-independence`, cut from `origin/main`, and its first task (P1.T1) merges the five branches. M215's branch already contains M214 - Colour Balance RGB's commits, so M214 comes along too.

Scouted facts (2026-10-28, `origin/main` at `9185c8bae` plus the five branch heads; re-check at promotion):
- The library (`Engine/Nodes/Filter/EdgeAware/{BoxFilter,GuidedFilter,Eigf,Wavelets,PoissonHeal,EdgeAwareCommon}.h`) takes `(buf, width, height)` and nothing else. `EdgeAwareGpu.h` mirrors each function as `record*`. In practice the reach is the whole image:
  - the Deriche IIR starts from the edge sample (`Eigf.h`);
  - `interpolateBilinear` samples at `j * widthIn / widthOut` (`GuidedFilter.h`);
  - the colour `guidedFilter`'s 512 px tiles are anchored at the buffer origin;
  - the dwt scales are capped by the image size (`dwtMaxScale`).
- Callers:
  - `Engine/Nodes/Raw/RawHighlightsLaplacian.cpp` (main);
  - `ToneEqualizerKernels.cpp` and `ToneEqualizerGpu.cpp` (m212);
  - `ContrastTexture.cpp` and `ContrastTextureKernels.cpp` (m213);
  - `Retouch.cpp`, `RetouchRender.cpp` and `RetouchAlgorithms.cpp` (m211);
  - `ColorEqualizer.cpp` (m215).
  - DenoiseProfiled has its own wavelets (`DenoiseProfiledWavelets.cpp`) under the same whole-frame need.
- All five UAT nodes already read the whole region of definition on every render.
  - Clamping an infinite RoD to the format: `Retouch::sourceRoD` (`Retouch.cpp:1292`) and `ContrastTexture::sourceRoD` (`ContrastTexture.cpp:278`) do it.
  - **Missing** in DenoiseProfiled (`effectRoDPixels`, `DenoiseProfiled.cpp:193-208`), ToneEqualizer (`ToneEqualizer.cpp:116`, used at 909, 1078, 1154) and ColorEqualizer (`ColorEqualizer.cpp:1518, 1562, 1627`, where `opPasses` sets a halo of `max(rod.width, rod.height)`).
- CPU whole-frame caches exist in:
  - ColorEqualizer (`_frame`, `ColorEqualizer.cpp:1664-1690`);
  - ToneEqualizer (`ToneEqualizer.cpp:951`);
  - DenoiseProfiled wavelets (`DenoiseProfiled.cpp:1048`);
  - RawHighlights (`RawHighlights.cpp:1229`).
  - There is none in ContrastTexture (`CT::computeMap` runs on every `renderOp`, around `ContrastTexture.cpp:396`) or Retouch (`RC::process` on every `renderOp`, around `Retouch.cpp:1470`).
  - No GPU path caches.
- GPU frame guards:
  - DenoiseProfiled (`DenoiseProfiled.cpp:1268-1282`) and ToneEqualizer (`ToneEqualizer.cpp:1069-1084`) refuse frames over `UINT32_MAX` floats in `canRenderOpOnGpu`.
  - ContrastTexture (`ContrastTexture.cpp:508`) and Retouch (`Retouch.cpp:1704`) check only inside the recorder.
  - ColorEqualizer has no guard; its GPU path goes through `PassList::gpu` (`ColorEqualizer.cpp:1531`).
- Compute Variance uses `bufferRect = window` (`DenoiseProfiled.cpp:989-993`) and measures at 1044-1046. Its tooltip says "over the last region rendered" (`DenoiseProfiled.cpp:555`).
- Existing region tests:
  - `NativeRawHighlights*_Test.cpp`: `expectTilesMatchTheFullFrame`, scale 1, `ASSERT_EQ`.
  - `NativeColorEqualizer_Test.cpp`: `OutputDoesNotDependOnTheRequestedRegion`, two windows, 1e-6.
  - `NativeDenoiseProfiledGpu_Test.cpp`: `TiledGpuRenderEqualsTheWholeGpuRender`, `ParityTolerance::exact()`.
- Test helpers: `renderNodePlanesDirect` (`Tests/RenderBothWays.h:135`), `GpuParityRuntime` and `runRenderGpu` (`Tests/Native/NativeGpuParity.h`), and `RawNodeSupport::setFrameComputedHookForTests` with `noteFrameComputed` (`Engine/Nodes/Raw/RawNodeSupport.h:181-187`).
- M130 finding: M209's running-sum box and guided kernels round differently depending on where a pixel sits in the buffer, so the same pixel computed in a tile and in the full frame can differ in the last bits.

## Scope

**In:**
- The whole-frame contract in the library headers, including the M130 rounding caveat.
- `Engine/Nodes/Image/WholeFrame.{h,cpp}`. The frame rect (source RoD, intersected with the output format when infinite, pixel-enclosing at the render scale), a one-entry keyed CPU result cache with compute-once-and-wait, and a GPU fit check.
- The format clamp adopted in DenoiseProfiled, ToneEqualizer and ColorEqualizer. Retouch and ContrastTexture switch their private `sourceRoD` to the helper.
- The ContrastTexture map cache and the Retouch result cache.
- GPU frame guards in `canRenderOpOnGpu` for ContrastTexture, Retouch and ColorEqualizer.
- Compute Variance measures the whole frame.
- A shared region-independence test support header, and per-node CPU and GPU region tests for all six nodes.

**Out:**
- Frame-origin parameters on the library functions, or windowed EdgeAware computation. Rejected, see Decisions.
- Making the box and guided running sums independent of buffer position (the M130 finding). Under the whole-frame contract it cannot change node output; it is documented, not fixed.
- A device-side (GPU) whole-frame cache → open question 2, a follow-up milestone if wanted.
- Moving the existing ColorEqualizer, ToneEqualizer, DenoiseProfiled and RawHighlights caches onto the new helper. They work; only their frame rect changes.
- A memory cap for huge *finite* RoDs. Only infinite RoDs are clamped.
- M130's own nodes (median, bilateral and guided), which do not use M209's kernels.

**Edge cases:**
- Infinite source RoD:
  - The frame is the format at the render scale.
  - Output pixels outside it pass the source through unchanged, as ContrastTexture's row function already does.
  - The node's own RoD stays the source's.
- Source RoD not at the origin, and odd sizes not divisible by the subsampling factor: the frame and grid are anchored at the RoD's corner. Tested with origin (−37, 21) and odd sizes.
- Windows partly outside the RoD: the inside matches the full frame, and the outside follows the node's existing out-of-RoD behaviour.
- Render scale 0.5: compared against the full frame at 0.5, not at 1. Only for nodes that support render scale (the raw nodes do not).
- Tiles rendered by one node instance hit the cache, so the test also renders each window from a *fresh* node, otherwise it would only be testing the cache.
- Retouch heal on GPU: if its convergence reduction is not deterministic from run to run, the GPU tile test pins Max Heal Iterations below convergence (P4.T2).
- A frame over the GPU limit: `canRenderOpOnGpu` refuses and the CPU renders it, rather than the recorder failing mid-render.
- UAT fixes landing later on M210–M215: merge them into this branch again before its PR (P1.T1's procedure).

**Assumptions:**
- The CPU library results do not depend on how the work is spread over threads (stated in `EdgeAwareCommon.h`), so whole-frame CPU renders are bit-identical across windows.
- GPU dispatches over the same whole frame are bit-identical between tiles on the same device. The only possible exception is a float-atomic reduction, which P4 checks.
- A raw source's RoD is always finite, so RawHighlights needs no clamp.
- The node's render hash covers Retouch's shapes. P3.T3 checks this; if it doesn't, the shapes' hash goes into the key.

## Phase 268.1: Integration branch

- [x] M268.P1.T1 — Merge M210, M211, M212, M213 and M215 onto one branch
  - files: the branch `milestone/m268-edge-aware-region-independence` (from `origin/main`). Expected conflicts:
    - `Tests/Native/RawReferenceSupport.h` (m210, m212, m213, m215);
    - `Tests/Native/NativeGpuCapability_Test.cpp` (m213, m215);
    - `Engine/EffectInstance.h` (m211 and m212's wheel hook);
    - `Tests/Native/RawTestSource.h` (m212);
    - `tools/raw/make-reference.py` and `Tests/fixtures/raw/README.md` (m213's `DARKTABLE_MASTER_REF`, m214 and m215);
    - `Engine/CMakeLists.txt`, `Tests/SuiteProperties.cmake`, the `Engine/Gpu/Kernels/*.cmake` lists and `Engine/GpuPlacement.cpp`.
  - approach:
    - `git submodule sync`.
    - Merge the five branches in order with `--no-ff`, committing each merge with `--no-verify` (the format hook rewrites vendored files).
    - Resolve every conflict as a union. Each branch only adds helpers, allowlist entries, kernels and harness jobs; keep both sides and dedupe identical helpers.
    - Leave the five branches untouched. Record their merged heads in Decisions so later UAT fixes can be merged in again.
  - verify:
    - `tools/ci/local/syntax-check.py --root <wt>` on every file that had a conflict.
    - The phase build and full ctest (run by the PM) pass with the union of the five branches' suites, each as green as on its own branch.
  - size: M

## Phase 268.2: Contract and shared support

- [x] M268.P2.T1 — Document the whole-frame contract in the library
  - files: `Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h`, `BoxFilter.h`, `GuidedFilter.h`, `Eigf.h`, `Wavelets.h`, `PoissonHeal.h`, `EdgeAwareGpu.h`
  - approach:
    - In the `EdgeAwareCommon.h` namespace comment, state that every function treats its buffer as the whole image. Its subsampling grids, tile grids, border rules and recursions are anchored to the buffer's top-left corner and size. A caller that passes anything but the node's whole frame (`WholeFrame.h`) gets output that depends on the window.
    - In `BoxFilter.h` and `GuidedFilter.h`, note the M130 finding: the running sums round by position in the buffer, so the results are not translation-invariant to the last bit.
    - Docs only; no signature changes.
  - verify: the build passes. Review checks the wording against P2.T2's helper.
  - size: S

- [x] M268.P2.T2 — `WholeFrame` helper: frame rect, format clamp, result cache and GPU fit
  - files: `Engine/Nodes/Image/WholeFrame.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/Native/WholeFrame_Test.cpp` (new), the tests CMake list
  - approach:
    - `bool wholeFramePixels(const EffectInstance&, int sourceInput, double time, ViewIdx, const RenderScale&, RectI* pixels, RectI* fullScalePixels)`. It takes the source's RoD at identity scale. If the RoD is infinite, it intersects it with `getOutputFormat()` as canonical (the `ContrastTexture::sourceRoD` logic). The result is pixel-enclosing at the render scale and at full scale, honouring the source's PAR. Null → false.
    - `class WholeFrameCache` holds one `shared_ptr<const std::vector<float>>` under a string key. `get(key, compute)` locks, computes once and serves concurrent windows the same result; this is ColorEqualizer's `_frameMutex` pattern made generic. Also:
      - `clear()`;
      - the key builder `wholeFrameKey(effect, sourceInput, time, view, scale, rect, layerId, nComps, extra)`, which includes the node's and the source's render hashes;
      - `RawNodeSupport::noteFrameComputed(what, …)` on every compute, so tests can count computes.
    - `bool wholeFrameFitsGpu(const RectI&, int floatsPerPixel, uint64_t maxScratchBytes)`: at most `UINT32_MAX` floats and within the scratch budget.
  - verify: `WholeFrame_Test`:
    - The clamp on an infinite-RoD source equals the format at scales 1 and 0.5, and a finite RoD passes unchanged.
    - Concurrent `get` from 8 threads computes once.
    - A key change recomputes.
    - `wholeFrameFitsGpu` boundaries.
  - size: M

- [x] M268.P2.T3 — Region-independence test support
  - files: `Tests/Native/RegionIndependenceSupport.h` (new)
  - approach: `expectRegionIndependent(makeNode, options)`.
    - `makeNode` builds a fresh source and node chain.
    - The source is synthetic and in memory: seeded white `Noise` cropped to an RoD at origin (−37, 21) with odd size, 203×157 by default. Options override the size where a node needs more to subsample, and choose the plane, the scales and the GPU runtime.
    - It renders the full frame once. Then each window from a **fresh** chain, and each window again in sequence on **one** chain (the cache path). The windows:
      - a 3×3 uneven tiling of the frame;
      - two overlapping windows offset by (5, 3);
      - a window at each corner;
      - a 1 px row and a 1 px column;
      - a window that extends past the RoD on two sides.
    - Each window is compared with the matching full-frame pixels: **bit-identical** (`memcmp` of the float bits, with any NaN equal to any NaN), and the first mismatch is reported.
    - GPU variant: the same windows through `runRenderGpu`, GPU tile against GPU whole, `ParityTolerance::exact()`.
    - Variant against the CPU full frame: within the node's existing GPU parity tolerance, passed in by the caller.
    - `expectInfiniteRoDClamped(makeNode)` uses an infinite-RoD source (a shipped generator if one returns an infinite RoD, else a test-only effect beside `RawTestSource`). A window straddling the format edge must equal, inside, the same node over the format-cropped source, and must be the source outside.
  - verify: `WholeFrame_Test` gains a self-test that runs the helper on a pointwise node (Grade), which passes, and on a deliberately windowed stub filter, which must report a mismatch.
  - size: M

## Phase 268.3: Node fixes

- [ ] M268.P3.T1 — The format clamp in every whole-frame node
  - files: `Engine/Nodes/Filter/DenoiseProfiled.cpp`, `Engine/Nodes/Filter/DenoiseProfiledGpu.cpp`, `Engine/Nodes/Color/ToneEqualizer.cpp`, `Engine/Nodes/Color/ColorEqualizer.cpp`, `Engine/Nodes/Filter/Retouch.cpp`, `Engine/Nodes/Filter/ContrastTexture.cpp`
  - approach:
    - Replace `effectRoDPixels` (DenoiseProfiled, ToneEqualizer), the RoD lookups in ColorEqualizer (`opPasses` halo, `getRegionsOfInterest`, `renderOp`, `recordGpu`'s `io.rod` frame) and the private `sourceRoD`s (Retouch, ContrastTexture) with `wholeFramePixels`.
    - Derive the full-scale sizes that set the scale counts from the clamped rect: DenoiseProfiled `fullSize`, ToneEqualizer `fullWidth`/`fullHeight`, ContrastTexture `fullSize`.
    - The RoI asks for the clamped frame.
    - Pixels outside the frame pass the source through.
  - verify: each node's region test (P4) includes `expectInfiniteRoDClamped`. Existing suites unchanged.
  - size: M

- [ ] M268.P3.T2 — ContrastTexture caches its full-frame map
  - files: `Engine/Nodes/Filter/ContrastTexture.{h,cpp}`
  - approach:
    - Hold a `WholeFrameCache` member.
    - The key is `wholeFrameKey` (node and source render hashes, time, view, scale, frame rect, layer, nComps).
    - `renderOp` fetches or computes `CT::computeMap` through it. The source read moves inside the compute.
    - Clear the cache on purge or Clear Caches, as `RawHighlights` does.
    - The GPU path is unchanged.
  - verify: a test in `Tests/Native/NativeContrastTexture_Test.cpp` renders a 3×3 tiling on one node and counts exactly one compute via the frame-computed hook. A knob change gives a second compute.
  - size: M

- [ ] M268.P3.T3 — Retouch caches its whole-frame result
  - files: `Engine/Nodes/Filter/Retouch.{h,cpp}`
  - approach:
    - Same as P3.T2, around the whole-source read and `RC::process` (the processed 4-channel frame).
    - First confirm that the node's render hash changes when a shape, its source offset or its keyframes change; if not, add the shapes' hash to the key's `extra`.
  - verify: a test in `Tests/Native/NativeRetouch_Test.cpp` checks one compute per tiling, and a recompute after moving a shape's source.
  - size: M

- [ ] M268.P3.T4 — GPU frame guards in `canRenderOpOnGpu`
  - files: `Engine/Nodes/Filter/ContrastTexture.cpp`, `Engine/Nodes/Filter/Retouch.cpp`, `Engine/Nodes/Color/ColorEqualizer.{h,cpp}`
  - approach:
    - Each node refuses the GPU when `!wholeFrameFitsGpu(frame, floatsPerPixel, query.maxScratchBytes)`.
    - `floatsPerPixel` is the largest per-pixel buffer the recorder allocates: ContrastTexture's lowpass, highpass and corrections; Retouch's image and bands; ColorEqualizer's guide and coefficients.
    - ColorEqualizer gains a `canRenderOpOnGpu` override, or the guard goes in its `opPasses` GPU lambda's availability, whichever `StagedImageEffect` consults before choosing the device.
    - The recorders keep their checks as a backstop.
  - verify: per-node GPU tests use a test source with a 70000×70000 RoD and expect `canRenderOnGpu` false, with no allocation.
  - size: S

- [ ] M268.P3.T5 — Compute Variance measures the whole frame
  - files: `Engine/Nodes/Filter/DenoiseProfiled.{h,cpp}`, `Engine/Nodes/Filter/docs/DenoiseProfiled.md`
  - approach (as recommended in open question 1):
    - In variance mode the RoI is the whole clamped frame.
    - `measureVariance` runs over the whole frame once per `wholeFrameKey`, through a `WholeFrameCache` of the three values.
    - The output passes through.
    - The readout reads "R G B variance over the whole frame (W×H at S%)". Update the tooltip and the docs to match.
    - Still CPU only.
  - verify: a test in `Tests/Native/NativeDenoiseProfiled_Test.cpp`:
    - Two fresh nodes rendering different windows give an identical `lastVariance()`.
    - It equals the variance of the stabilised synthetic uniform-noise frame computed in the test.
    - The GUI gate checks the readout after panning.
  - size: M

## Phase 268.4: Region regression suites

Each task adds `RegionIndependent*` cases to that node's existing test files, so no two implementers share a file. Each case runs at scales 1 and 0.5 where the node supports render scale, and includes `expectInfiniteRoDClamped` where P3.T1 applies.

- [ ] M268.P4.T1 — DenoiseProfiled
  - files: `Tests/Native/NativeDenoiseProfiled_Test.cpp`, `Tests/Native/NativeDenoiseProfiledGpu_Test.cpp`
  - approach: run the CPU helper for Wavelets (every colour mode) and NLM. Run the GPU helper for both. The existing `TiledGpuRenderEqualsTheWholeGpuRender` becomes a call to the helper.
  - verify: the cases pass in the phase build.
  - size: S

- [ ] M268.P4.T2 — Retouch
  - files: `Tests/Native/NativeRetouch_Test.cpp`, `Tests/Native/NativeRetouchGpu_Test.cpp`, `Tests/Native/RetouchTestSupport.h`
  - approach:
    - Cover clone, heal, blur and fill, with one shape crossing a tile seam and a shape sitting at a coarser wavelet scale, through the CPU and GPU helpers.
    - Run GPU heal whole twice first. If the two runs differ, set Max Heal Iterations to 50 so that convergence never stops it early, and record that in Decisions.
  - verify: the cases pass.
  - size: M

- [ ] M268.P4.T3 — ToneEqualizer
  - files: `Tests/Native/NativeToneEqualizer_Test.cpp`, `Tests/Native/NativeToneEqualizerGpu_Test.cpp`
  - approach: cover the four filters (fast guided or EIGF, each with linear or geomean blending) and filter off, through the CPU and GPU helpers.
  - verify: the cases pass.
  - size: S

- [ ] M268.P4.T4 — ContrastTexture
  - files: `Tests/Native/NativeContrastTexture_Test.cpp`, `Tests/Native/NativeContrastTextureGpu_Test.cpp`
  - approach: cover the defaults plus two detail and halo-control settings that use every level, through the CPU and GPU helpers.
  - verify: the cases pass.
  - size: S

- [ ] M268.P4.T5 — ColorEqualizer
  - files: `Tests/Native/NativeColorEqualizer_Test.cpp`, `Tests/Native/NativeColorEqualizerGpu_Test.cpp`
  - approach:
    - Replace `OutputDoesNotDependOnTheRequestedRegion` with the helper.
    - Use a 641×479 frame at origin (−37, 21) with the Kodachrome 64-like preset, which subsamples by 4 (assert the factor), plus one weight-output view mode, on CPU and GPU.
    - The comparison tightens from 1e-6 to bit-identical.
  - verify: the cases pass.
  - size: S

- [ ] M268.P4.T6 — RawHighlights
  - files: `Tests/Native/NativeRawHighlights_Test.cpp`, `NativeRawHighlightsSegments_Test.cpp`, `NativeRawHighlightsInpaint_Test.cpp`, `NativeRawHighlightsLaplacian_Test.cpp`, `NativeRawHighlightsGpu_Test.cpp`
  - approach:
    - Swap each `expectTilesMatchTheFullFrame` for the helper's window set over `RawTestSource` at an offset origin with odd size, for every mode (clip, opposed, LCh, segments, inpaint, laplacian), on GBRG/RGGB and X-Trans.
    - The GPU helper covers Clip and LCh, the modes that have GPU paths.
    - Scale 1 only.
  - verify: the cases pass.
  - size: S

**Verification gate:**
- The phase build and the full ctest pass on the merged branch: every suite from M210–M215, plus `WholeFrame_Test` and every new `RegionIndependent*` case. Run it remotely in both forms: `tools/ci/remote/build.sh debug --tests`, which reproduces PR CI's asserts, and the release gate.
- The GPU cases run on a GPU-capable runner: the published image's GL/GPU leg, or the GPU host. None of them may skip silently, and the log shows each GPU case executed.
- A GUI check via the M269 remote GUI checks, with screenshots shared before sign-off. Script: `Tests/gui/m268_region_independence.py`.
  - The DenoiseProfiled Compute Variance readout must not change when the viewer is panned or zoomed at 100%.
  - A ContrastTexture viewer pan over a 4K synthetic plate must show no tile seams.
- An AppImage built with `package.sh` for UAT.

## Open questions (for the user)

1. **What should Compute Variance measure?** Recommendation: the whole frame at the render scale, with a readout that names the size and scale it measured ("over 4000×3000 at 100%"). It is then independent of pan and zoom, and a zoomed-out viewer is visibly a different measurement. The alternative is to keep it per region and label it "visible region". It is planned as recommended in P3.T5.
2. **Should the GPU paths get a device-side whole-frame cache?** Today every GPU tile recomputes the whole frame, in all five nodes. Recommendation: not in this milestone. It needs a cross-node GPU residency design, and the CPU caches plus the size guards cover correctness. File a follow-up after a bench on a modest GPU.
3. **In what order should this land against the five UAT PRs?** Recommendation: let M210–M215 finish UAT and merge as they are, then merge `main` into this branch and open its own PR. This milestone doesn't hold up their UAT, and only its diff needs review.

## Decisions

- 2026-10-28: Phases 1–2 implemented (build pending). Merged heads: M210 aea7e922c, M211 a6c396ecc, M212 1b6bfba6f, M213 51b26e630, M215 27952108b (merges 441b5946e…3b78dd2a6); only make-reference.py and fixtures/raw/README.md conflicted (union). Test source is an in-memory noise source (finite or infinite extent), not Noise+Crop (Noise is format-bound; RawTestSource ignores render scale; no shipped generator is infinite). In-sequence renders keep the cache (bypassing clears the node cache). The infinite-RoD 'outside equals source' check holds for whole-frame nodes only; the Grade self-test uses defaults.
- 2026-10-28: elaborated (consultant).
- 2026-10-28 — **A whole-frame contract, not frame-origin parameters (consultant):**
  - The library's filters reach the whole image: Deriche IIR edge starts, a bilinear resample ratio that depends on the full size, colour guided-filter tiles anchored at the origin, and dwt scale caps. With a frame origin and full size, a window would still need whole-frame data to be exact, so the API would promise more than it can deliver.
  - It would also have to be threaded through five Slang kernels and `EdgeAwareGpu.h`.
  - darktable processes the whole image too.
  - `ColorEqualizerFrame` stays: it positions the threshold weight overlay, which is a node concern, not the library's.
  - The contract is enforced by the shared `WholeFrame` helper and the per-node regression tests, not by `assert()`, which is inert in the local NDEBUG debug build.
- 2026-10-28 — **Tolerances (consultant):**
  - CPU windows against the CPU full frame: bit-identical, because under the contract the same buffer goes through deterministic code.
  - GPU tiles against the GPU whole: bit-identical.
  - GPU windows against the CPU full frame: the node's existing GPU parity tolerance, unchanged.
  - Any non-zero difference is a bug, never an ulp budget. The only allowed exception is Retouch heal's iteration pin in P4.T2.
- 2026-10-28 — **Scope additions found while scouting (consultant):**
  - ColorEqualizer also lacked the infinite-RoD clamp, so it joins P3.T1.
  - Retouch also recomputed the whole frame on every tile, so it gets P3.T3 alongside ContrastTexture's map cache.
  - ContrastTexture's and Retouch's GPU guards move from the recorder into `canRenderOpOnGpu`, as M210 and M212 already have them, so an oversized frame falls back to the CPU instead of failing.
- 2026-10-28 — **The M130 running-sum rounding is documented, not fixed:** under the whole-frame contract the buffer never changes between windows, so the rounding cannot reach node output. Fixing it would need frame anchoring, which the contract rejects.
