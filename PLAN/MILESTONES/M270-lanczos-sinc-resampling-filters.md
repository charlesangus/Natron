# M270 - Lanczos Sinc Resampling Filters

Add Lanczos 2/3/4, a Hann-windowed Sinc with a Lobes knob, Blackman-Harris, Gaussian and a Bicubic with B and C knobs to the Filter choice of Transform, TransformMasked and Reformat, and make every kernel filter scale-aware: when a transform minifies, the kernel's support widens by the scale factor, so a downscale is properly prefiltered. This replaces `interpolateSuper`'s 3^n bilinear supersampling. The fork is a clean break (user, 2026-10-28): the existing ten filters are not held to OFX bit parity, and goldens are re-recorded.

## Scope

**In:**
- Seven new Filter choice entries appended after Notch: Lanczos2, Lanczos3, Lanczos4, Sinc, Blackman-Harris, Gaussian, Bicubic.
- Knobs `lobes` ("Lobes", Sinc only, 2 to 8, default 4), `bicubicB` ("Bicubic B") and `bicubicC` ("Bicubic C") (Bicubic only, 0 to 1, defaults 1/3 and 1/3, Mitchell-Netravali), enabled by the Filter choice like Clamp is today.
- One separable weighted-kernel resampler for every filter except Impulse and Box: per-axis kernel scale from the back-transform's Jacobian, tap count from the scaled support, normalised weights.
- The same kernels in the CPU resampler (`Resampler`) and the GPU kernel (`resample.slang`), with CPU/GPU parity.
- Region-of-interest expansion that follows the kernel radius and the minification scale.
- Re-recorded Transform and Reformat goldens, and tests for each new filter.

**Out:**
- A separate Hermite entry: Hermite is the existing Cubic (zero-tangent cubic), and Bicubic with B = 0, C = 0 covers it (decision below).
- A Gaussian width knob: Gaussian is fixed at sigma 0.5 source pixels, radius 3 sigma.
- Non-Resampler nodes (STMap, IDistort, lens distortion, warps, EdgeDetect's own filter enum): a later milestone if wanted.
- Deep images and OFX plug-ins.
- Anisotropic kernels oriented along the Jacobian's axes: the footprint is the axis-aligned bounding box, as `interpolateSuper` does today.

**Edge cases:**
- Kernel scale is `max(1, |source step per destination pixel|)` per axis, so magnification and 1:1 behave as the plain kernel, and a 1:1 Lanczos or Sinc render reproduces the source exactly.
- Per-axis footprint is capped at 32 source pixels; a larger minification clamps to the cap and aliases beyond it (assumption, check at the P1 build against a 64x downscale).
- Weights are normalised to sum to 1 per axis, so a constant image stays constant under any kernel, including Lanczos and Sinc at a frame edge.
- Clamp (to the range of the footprint's samples) applies to every filter with negative lobes: Keys, Simon, Rifman, Mitchell, Lanczos2/3/4, Sinc and Bicubic; Blackman-Harris, Gaussian, Parzen and Notch never overshoot.
- Black Outside and edge pixels: taps outside the source read black or the nearest edge pixel exactly as today, before normalisation.
- Motion blur and the Impulse draft-quality path are unchanged in structure; each blur sample uses the new kernel.

**Assumptions:**
- `Resampler.cpp`'s `interpolateSuper` and `supersampleLevels` have no caller outside Transform, TransformMasked, Reformat and the staged resample executor.
- The GPU kernel can loop up to 64 taps per axis at acceptable cost; if not, lower the cap in P2 and record a decision.

## Decisions

- 2026-10-28: blocked on scope (PM): interpolateSuper/supersampleLevels and filterOptions() are shared by STMap, IDistort, IWarp, LensDistortion (resampleRowFromCoords; coordmap.slang/iwarp.slang use resamplecore.slang) and CornerPin/ITransform/ICornerPin (staged resampleRow), contradicting the Assumptions/Out list. Asked the user to choose replace-everywhere vs split.
- 2026-10-28 — Scale-aware kernels replace supersampling for all filters: no backward compat in the fork (user), so existing ten need not stay bit-identical to OFX; ulp and minification goldens are re-recorded, not treated as a framework change.
- 2026-10-28 — No Hermite entry: Hermite is the zero-tangent cubic Natron already calls Cubic, equal to Bicubic at B = 0, C = 0; adding it would duplicate an entry.
- 2026-10-28 — Lobe count exposed as fixed Lanczos 2/3/4 entries plus a Sinc entry with a Lobes knob (user).

## Phase 270.1: CPU kernels and scale-aware resampler

- [ ] M270.P1.T1 — Add kernel table and the seven new filters
  - files: `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/Image/Resampler.cpp`
  - approach: Extend `FilterEnum` with eFilterLanczos2, eFilterLanczos3, eFilterLanczos4, eFilterSinc, eFilterBlackmanHarris, eFilterGaussian, eFilterBicubic after eFilterNotch; extend `filterOptions()` with label, hint and script ID each (labels match script names: `lanczos2` is "Lanczos2", `blackmanHarris` is "Blackman-Harris"). Add a `FilterKernel { FilterEnum filter; int lobes; double B, C; }` with `radius()` and `weight(double x)` for the new filters: Lanczos a = 2/3/4, sinc(x)·sinc(x/a) for |x| < a; Sinc is sinc(x)·Hann(x/lobes) over `lobes`; Blackman-Harris 4-term window, radius 2; Gaussian sigma 0.5, radius 1.5; Bicubic Mitchell-Netravali B and C. Update `filterTaps`, `filterHonoursClamp` (true for every filter with negative lobes), and keep the existing ten's formulas as they are.
  - verify: `ctest -R Resampler` at the phase build passes the kernel-property tests of T4.
  - size: M

- [ ] M270.P1.T2 — Replace supersampling with a separable scale-aware resampler
  - files: `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/Image/Resampler.cpp`, `Engine/Nodes/Image/TransformMath.h` (only if the Jacobian helper lives there)
  - approach: Rewrite `interpolate` and `interpolateSuper` as one weighted-kernel path for every filter except Impulse and Box (Box keeps its bounding-box integration). Per axis, scale s = max(1, step length from the Jacobian column norms); taps cover radius·s, capped at 32 source pixels; weights are kernel(distance / s), normalised to sum to 1; accumulate in float as today; apply Clamp to the footprint's sample range; honour `blackOutside`. Remove `supersampleLevels`, the bilinear supersamples and the 3^level loop, and fix every caller and header comment. A zero Jacobian samples at s = 1.
  - verify: at the phase build, `ctest -R Resampler` passes, including the 1:1 identity, constant-image and minification tests of T4.
  - size: L

- [ ] M270.P1.T3 — Generalise the region-of-interest expansion
  - files: `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/Image/Resampler.cpp`, `Engine/Nodes/Image/ResampleExecutor.cpp`
  - approach: `filterRoIExpansion` takes the minification scale per axis and returns radius·scale (capped like the footprint), instead of the fixed 0, 0.5 or 1.5. Thread the scale through `RegionParams` and the staged executor's region-of-interest query so an upstream render covers every tap, including under a Transform chain.
  - verify: at the phase build, `ctest -R "Resampler|StagedResample"` passes, including a test that a 4x Lanczos3 downscale of a window reads only inside the reported region of interest.
  - size: M

- [ ] M270.P1.T4 — Test the kernels and the scale-aware resampler
  - files: `Tests/Native/Resampler_Test.cpp`, `Tests/Native/StagedResample_Test.cpp`
  - approach: Per new filter: weights sum to 1 at fractional offsets; Lanczos and Sinc are 1 at 0 and 0 at nonzero integers, so a 1:1 resample reproduces the source exactly; Bicubic at B = 0, C = 0.5 equals Keys; Bicubic at B = 1/3, C = 1/3 equals Mitchell. Resampler: a constant image stays constant under every filter, a 4x downscale of a one-pixel checkerboard gives near-zero alias energy for Lanczos3 (below 1e-2) where Impulse does not, Clamp keeps a step response inside the source range, Lobes 2 to 8 widen the footprint.
  - verify: at the phase build, `ctest -R "Resampler|StagedResample"` passes, and reverting T2's normalisation makes the constant-image test fail.
  - size: M

## Phase 270.2: GPU kernel parity

- [ ] M270.P2.T1 — Mirror the kernels and resampler in the GPU kernel
  - files: `Engine/Gpu/Kernels/resample.slang`, `Engine/Gpu/GpuResample.h`, `Engine/Gpu/GpuResample.cpp`
  - approach: Add the seven filter IDs (`kFilterLanczos2` through `kFilterBicubic`, same numbering as `Resampler::FilterEnum`), raise `kGpuResampleFilterCount` to 17 and add `lobes`, `bicubicB`, `bicubicC` to `GpuResampleParams` (update the `static_assert` size and keep 16-byte padding). Replace the 3^level supersampling path with the same per-axis scaled-kernel loop as P1.T2 (same cap, same normalisation, same Clamp rule); remove `GpuResampleJacobian`'s level fields and `kGpuResampleMaxLevel` if nothing else uses them. Compute weights in float; the CPU's double positions pick the same integer cells, as the file header already requires.
  - verify: `ctest -R "NativeTransformGpu|StagedResample"` at the phase build, on lavapipe, passes with the P2.T2 cases.
  - size: L

- [ ] M270.P2.T2 — Test CPU and GPU parity for the new filters
  - files: `Tests/Native/NativeTransformGpu_Test.cpp`, `Tests/Native/StagedResample_Test.cpp`
  - approach: Add parity cases (the existing helper in `Tests/Native/NativeGpuParity.h`, ulp tolerance as the neighbouring filter cases use) for each new filter at 1:1, 2x magnification and 4x minification, with and without Clamp, Sinc at Lobes 2 and 8, Bicubic at two (B, C) pairs, and motion blur on one case. Keep fixtures small and synthetic.
  - verify: `ctest -R "NativeTransformGpu|StagedResample"` passes on lavapipe at the phase build; with `NATRON_DEV_GPU=1` on a real GPU it passes too when one is available.
  - size: M

## Phase 270.3: Nodes, knobs and goldens

- [ ] M270.P3.T1 — Add the Lobes and Bicubic knobs
  - files: `Engine/Nodes/Transform/Transform.cpp`, `Engine/Nodes/Transform/Transform.h`, `Engine/Nodes/Transform/Reformat.cpp`, `Engine/Nodes/Transform/Reformat.h`, `Engine/Nodes/Image/Resampler.h`
  - approach: In the shared filter-knob builder (`Transform.cpp` around the `kResamplerParamFilterType` knob) add `lobes` (int, 2 to 8, default 4), `bicubicB` and `bicubicC` (double, 0 to 1, default 1/3) beside Filter and Clamp, with `kResamplerParam*` names, labels and hints in `Resampler.h`; labels match script names. Enable each only for its filter in the same knob-changed handler that enables Clamp today (`Transform.cpp` near `filterHonoursClamp`). Fit the 1920x1080 default panel. Read them into `makeResampleParams` and the GPU params.
  - verify: `ctest -R "NativeTransform|NativeReformat"` passes at the phase build; a GUI check under Xvfb shows the knobs enable and disable with the Filter choice, and the screenshot is shared before sign-off.
  - size: M

- [ ] M270.P3.T2 — Wire the new filters through Transform, TransformMasked and Reformat
  - files: `Engine/Nodes/Transform/Transform.cpp`, `Engine/Nodes/Transform/Reformat.cpp`, `Engine/Nodes/Image/ResampleExecutor.h`, `Engine/Nodes/Image/ResampleExecutor.cpp`
  - approach: Carry `lobes`, `bicubicB` and `bicubicC` through each node's param struct, the staged `Resample` settings and the draft-quality path (Impulse still wins in draft quality). Reformat's region-of-interest and any resize-type path uses the scale-aware expansion from P1.T3.
  - verify: `ctest -R "NativeTransform|NativeReformat|StagedResample"` passes at the phase build with the P3.T3 cases.
  - size: M

- [ ] M270.P3.T3 — Re-record goldens and add node tests
  - files: `Tests/Native/NativeTransform_Test.cpp`, `Tests/Native/NativeReformat_Test.cpp`, `Tests/Native/NativeGolden_Test.cpp`, `Tests/fixtures/native-golden/Transform/`, `Tests/fixtures/native-golden/Reformat/`
  - approach: Re-record only the existing goldens whose output changed with the scale-aware path (record the cause in the commit); add one Reformat downscale golden for Lanczos3 and one Transform golden for Sinc at Lobes 3, as ≤256 px half-float compressed crops. Add node tests that each new Filter entry renders without error and a Lanczos3 Reformat of 1/4 scale has lower alias energy than Impulse.
  - verify: `ctest -R "NativeTransform|NativeReformat|NativeGolden"` passes at the phase build; `du -sh Tests/fixtures/native-golden` grows by under 1 MB.
  - size: M

- [ ] M270.P3.T4 — Document the filters
  - files: `docs/` (the Transform and Reformat node pages, found with `grep -ril reformat docs`), the Filter knob hints in `Engine/Nodes/Image/Resampler.cpp`
  - approach: List all seventeen filters with when to use each (Lanczos3 for general down and up scaling, Sinc for maximum sharpness, Gaussian and Blackman-Harris for ringing-free minification, Bicubic for custom B and C), describe the Lobes, B and C knobs, state that the kernel widens when minifying, and describe the footprint cap. Skip the page if `docs/` has no node reference.
  - verify: the docs build step in CI (or `grep -c Lanczos` on the page) shows the new entries.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh release --tests` passes in full, including `ctest -R "Resampler|StagedResample|NativeTransform|NativeReformat|NativeGolden|NativeTransformGpu"` on lavapipe; the GUI bench is not required (no NodeGraph diff); the Xvfb screenshot of Transform's filter panel was shared and approved.
