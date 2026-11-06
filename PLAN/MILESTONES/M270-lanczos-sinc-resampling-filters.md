# M270 - Lanczos Sinc Resampling Filters

Add Lanczos 2/3/4, a Hann-windowed Sinc with a Lobes knob, Blackman-Harris, Gaussian and a Bicubic with B and C knobs to the Filter choice of every node that shares the resampler, and make every kernel filter scale-aware: when a node minifies, the kernel widens by the scale factor. This replaces 3^n bilinear supersampling. The fork is a clean break (user, 2026-10-28): the existing ten filters are not held to OFX bit parity, and goldens are re-recorded.

## Scope

**In:**
- Nodes: Transform, ITransform, Reformat, CornerPin, ICornerPin, AutoStraighten, STMap, IDistort, LensDistortion, LensCorrect, SplineWarp.
- Seven new Filter entries after Notch, ids and labels: `lanczos2` "Lanczos 2", `lanczos3` "Lanczos 3", `lanczos4` "Lanczos 4", `sinc` "Sinc", `blackmanHarris` "Blackman Harris", `gaussian` "Gaussian", `bicubic` "Bicubic".
- Knobs on all those nodes: `lobes` "Lobes" (Sinc only, 2 to 8, default 4), `bicubicB` "Bicubic B", `bicubicC` "Bicubic C" (Bicubic only, 0 to 1, default 1/3 each), enabled by the Filter choice like Clamp.
- One weighted-kernel path for every filter except Impulse and Box, on CPU (`Resampler`) and GPU (`resamplecore.slang`), used by the matrix and per-pixel-coordinate forms, with CPU/GPU parity.
- Region-of-interest expansion that follows the kernel radius and minification.
- Re-recorded goldens, per-filter tests and node docs.

**Out:**
- A Hermite entry: it is the existing Cubic, and Bicubic at B = 0, C = 0 (decision below).
- A Gaussian width knob: fixed at sigma 0.5 source pixels, radius 1.5.
- Filters that are not this resampler: BlurKernels users (Blur, Bloom, EdgeDetect), Glow's `glow_resample`, NeuralUpscale's own Lanczos and its Cubic `filterWeights` call, Mirror (always Impulse), TimeBlur (shutter helpers only).
- Deep images and OFX plug-ins.
- Anisotropic kernels oriented along the Jacobian's axes: footprints are per source axis.
- Faster two-pass resizing for axis-aligned matrices: no owner yet; open a milestone from the P1 bench numbers if wanted.

**Edge cases:**
- Kernel scale per source axis is `max(1, hypot(Jxx, Jxy))` for x and `max(1, hypot(Jyx, Jyy))` for y (row norms). Magnification, 1:1 and pure rotation use the plain kernel. A zeroed row or a zero Jacobian gives scale 1.
- A 1:1 Lanczos or Sinc render at integer offsets reproduces the source exactly: weights return exactly 1 at 0 and 0 at nonzero integers, never `sin(pi n)`.
- Tap budget: 81 taps per axis (main's level-4 maximum). Past it, the 81 taps spread evenly over the whole footprint with bilinear reads. The footprint and region of interest are never shrunk, and no source size is capped.
- Weights are normalised over all taps, including those outside the source. With Black Outside off, a constant image stays constant at a frame edge; with it on, the edge fades to black as today.
- Every weighted kernel reaches 0 at its support edge: Gaussian and Blackman-Harris subtract their edge value before normalising, so the tap set changes without a jump.
- Clamp applies to Keys, Simon, Rifman, Mitchell, Lanczos 2/3/4, Sinc and Bicubic, to the range of the footprint's samples, so clamped Keys, Simon, Rifman and Mitchell change even at 1:1 (re-recorded). Cubic stays unclamped; Blackman-Harris, Gaussian, Parzen and Notch never overshoot.
- Wrap modes fold the centre coordinate only; taps read black or the edge pixel per Black Outside, as today. An axis eWrapClamp moved gets scale 1.
- A UV, Map or lens discontinuity produces a huge Jacobian, so that pixel averages a wide footprint within the tap budget.
- LensCorrect's lateral chromatic aberration samples R and B with the G Jacobian (`lenscorrect.slang:558-572`), unchanged.
- Motion blur and draft Impulse are unchanged in structure; each blur sample uses the new kernel.

**Assumptions:**
- `SplineWarpField` can report its largest kernel scale from the walk behind `getSourceBounds()`; if not, P3.T9 reads the whole Source as IDistort does.
- GPU kernels tolerate a dynamic loop of 81 taps per axis on lavapipe and real drivers; main's worst case is already 81 x 81 bilinear samples.

## Decisions

- 2026-11-07 — Black Outside edge under slow translate at 4x down (0.01 output px per frame, 201 frames): max one-frame pixel change is 0.338 for main's cubic, 0.380 for the branch's Cubic, 0.392 for Lanczos 3, against 0.010 for an interior step; Lanczos 3 also rings to -0.118 at the edge. The jump exists on main too, once per output pixel crossed. Whether it shows in playback is unchecked: for the user at the gate.
- 2026-11-07 — Extends "a tap on the support edge is dropped": Keys, Simon, Rifman, Mitchell, Sinc and Bicubic weigh 0 within 2^-26 of the edge, on CPU and GPU. The CPU's polynomials rounded to zero there unpredictably, moving the clamp range by up to 0.04 against the GPU; mirroring that would need double multiplication per tap. Weights in the band are under 2^-50, so no visible change. Sinc's window is `cos²` to match. Phase 270.3 factors Bicubic's outer lobe, since non-default B and C arrive with its knobs.
- 2026-11-06 — Edge band measured (4x Lanczos 3 of a 64 px source): one-pixel stripes with Black Outside off go 0.57 then 1.0 at the first pixel past the source, the ramp 0.98 then 1.0; with it on, 0.94 then 0.02 on this grid. No band wider than one output pixel, so the zeroed-row rule stands. But a Black Outside edge sliding sub-pixel jumps (a replica reads 0.377 at fx = 64.49 and -0.006 at 64.51, ringing to -0.11 at 65), which could flicker under animation: check at the Xvfb stage and raise with the user at the panel screenshots if it shows.
- 2026-11-06 — P1 triage: no product bug in the CPU suites. Golden and parity diffs in IDistort, STMap, ITransform, ICornerPin, Reformat and Transform come from the widened kernel, not rounding (max 0.41 on noise UVs); re-record in P3.T3. `NativeSplineWarpGpuTest` is the only timeout.
- 2026-11-06 — P1.T1 and T2: CPU clamp range counts black out-of-source reads, otherwise a fading edge clamps back up, and skips zero-weight taps; spread taps count their interpolated read. The support is an open interval, so a tap on its edge is dropped. Spread taps sit at `(k + 0.5) * 2R / 81 - R`. Lobes clamp to 2-8. Phase 270.2 mirrors all of these. Keeping the zeroed-row rule means a pixel centred past the source uses the plain kernel, so the kernel narrows at the edge under minification: P1.T4 checks for a visible band, and the user hears about it if it shows.
- 2026-11-06 — Re-elaborated for "Same filters everywhere": added AutoStraighten, LensCorrect and SplineWarp's kernel; dropped TransformMasked (M111 - Native Node Identity Cleanup landed).
- 2026-11-06 — Row-norm kernel scale replaces column norms: column norms mis-scale a 90° anisotropic transform and blur pure rotations.
- 2026-11-06 — Tap budget of 81 per axis replaces the 32 px footprint cap: per-pixel Jacobians from data make unbounded tap counts a hang, and the budget matches main's worst case without clipping the footprint. Raised to the user at the gate, as is the CPU cost: at 4x down, Lanczos 3 reads about 625 taps per pixel where main takes 9 samples.
- 2026-11-06 — Gaussian and Blackman-Harris taper to zero, and every clamped filter takes the exact GPU entries: both keep CPU/GPU parity at tap boundaries.
- 2026-11-02: audit. The user's answer (next entry, option a) had been appended after the gate; moved here. It unblocks the milestone, but `## Scope` (Out: STMap, IDistort, warps, lens distortion; the Assumption that only Transform, TransformMasked, Reformat and the staged executor call interpolateSuper) and the task list still describe the old split. Before P1.T1, re-elaborate: add tasks for resampleRowFromCoords users (STMap, IDistort, IWarp, LensDistortion; coordmap.slang, iwarp.slang, resamplecore.slang) and the staged resampleRow users (CornerPin, ITransform, ICornerPin), and drop TransformMasked if M111 - Native Node Identity Cleanup lands first (it folds it into Transform). Worktree .worktrees/m270 exists with no commits.
- 2026-11-02 — **Same filters everywhere (user):** option (a), the new kernels and Filter entries apply to every node sharing the resampler.
- 2026-10-28: blocked on scope (PM): interpolateSuper/supersampleLevels and filterOptions() are shared by STMap, IDistort, IWarp, LensDistortion (resampleRowFromCoords; coordmap.slang/iwarp.slang use resamplecore.slang) and CornerPin/ITransform/ICornerPin (staged resampleRow), contradicting the Assumptions/Out list. Asked the user to choose replace-everywhere vs split.
- 2026-10-28 — Scale-aware kernels replace supersampling for all filters: no backward compat in the fork (user), so existing ten need not stay bit-identical to OFX; ulp and minification goldens are re-recorded, not treated as a framework change.
- 2026-10-28 — No Hermite entry: Hermite is the zero-tangent cubic Natron already calls Cubic, equal to Bicubic at B = 0, C = 0; adding it would duplicate an entry.
- 2026-10-28 — Lobe count exposed as fixed Lanczos 2/3/4 entries plus a Sinc entry with a Lobes knob (user).
- 2026-11-02 — Takes over M274.P5.T2: SplineWarp gets the new Lanczos/Sinc filters too (user). Add it when re-elaborating.

## Phase 270.1: CPU kernels and scale-aware resampler

- [x] M270.P1.T1 — Add the kernel table and the seven new filters
  - files: `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/Image/Resampler.cpp`
  - approach: Extend `FilterEnum` after eFilterNotch and `filterOptions()` as in Scope; hints never mention other applications. Add `struct FilterSpec { FilterEnum filter; int lobes = 4; double B = 1./3, C = 1./3; }`, implicitly constructible from `FilterEnum` so Mirror and existing tests still compile, with `radius()` and `weight(double x)`. Existing filters derive `weight` from `filterWeights()` (`Resampler.h:261`) by mapping |x| to a tap and d. Lanczos a = 2/3/4: sinc(x)·sinc(x/a). Sinc: sinc(x)·Hann(x/lobes) over `lobes`. Blackman-Harris: 4-term, radius 2. Gaussian: sigma 0.5, radius 1.5. Bicubic: Mitchell-Netravali. Apply the edge-value and exact-integer rules from Scope. Update `filterTaps` and `filterHonoursClamp`.
  - verify: `ctest -R Resampler` at the phase build passes the kernel-property tests of T4.
  - size: M

- [x] M270.P1.T2 — Replace supersampling with the scale-aware kernel path
  - files: `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/Image/Resampler.cpp`
  - approach: Rewrite `interpolate` and `interpolateSuperT` as one runtime weighted path taking `FilterSpec` (still templated on nComps), for every filter except Impulse and Box. Box keeps its bounding-box integration. Per pixel, compute each axis's scale (row norms), tap positions and weights into stack arrays of 81, normalise, then read row by row. Accumulate in float; apply the footprint clamp and `blackOutside`. Thread `FilterSpec` through `ResampleParams`, `makeResampleParams`, `resampleRow`, `motionBlurPixel` and `resampleRowFromCoords`. Remove the bilinear supersamples and fix the header comments, including the claim of reproducing OFX to rounding. Keep `supersampleLevels` for `ResampleExecutor.cpp:412` until P2.T1 removes it.
  - verify: at the phase build, `ctest -R Resampler -E Gpu` passes, including the identity, constant-image and minification tests of T4.
  - size: L

- [x] M270.P1.T3 — Generalise the region-of-interest expansion
  - files: `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/Image/Resampler.cpp`, `Engine/Nodes/Image/ResampleExecutor.cpp`, `Engine/Nodes/Image/ResampleExecutor.h`
  - approach: `filterRoIExpansion` takes `FilterSpec` and per-axis scale and returns radius·scale − 0.5 (today's 0.5 and 1.5 at scale 1), 0 for Impulse and Box. `RegionParams` and `ResampleSettings` carry the spec. `getRegionOfInterest` takes scale from the matrix's Jacobian at the window corners, over every blur matrix; a corner at or behind z = 0 takes the source's whole region. Update the reference path at `ResampleExecutor.cpp:726`. No cap.
  - verify: at the phase build, `ctest -R "Resampler|StagedResample" -E Gpu` passes, including a test that a 4x Lanczos 3 downscale of a window reads only inside the reported region.
  - size: M

- [x] M270.P1.T4 — Test the kernels and the matrix path
  - files: `Tests/Native/Resampler_Test.cpp`, `Tests/Native/StagedResample_Test.cpp`
  - approach: Per new filter: weights sum to 1 at fractional offsets; Lanczos and Sinc reproduce the source at 1:1; each kernel is 0 at its support edge; Bicubic at (0, 0.5) equals Keys and at (1/3, 1/3) equals Mitchell. Resampler: a constant image stays constant under every filter with Black Outside off; a 4x downscale of a one-pixel checkerboard has alias energy below 1e-2 for Lanczos 3 where Impulse does not; a 64x downscale reads the full footprint within budget; clamp holds a step inside the source range; Lobes 2 to 8 widen the footprint; J = [[0,4],[1,0]] scales (4,1) and a 45° rotation (1,1). Replace the assertions at `Resampler_Test.cpp:218,291-327,352`.
  - verify: at the phase build, these pass and reverting T2's normalisation makes the constant-image test fail.
  - size: M

- [x] M270.P1.T5 — Test the per-pixel coordinate path on the CPU
  - files: `Tests/Native/ResamplerCoordMap_Test.cpp`
  - approach: Cases through `resampleRowFromCoords` for each new filter: a null Jacobian samples the plain kernel; a minifying Jacobian widens it; eWrapClamp and out-of-source rows give scale 1; a non-finite or huge Jacobian stays finite and within budget; wrap modes fold the centre only.
  - verify: `ctest -R ResamplerCoordMap` passes at the phase build.
  - size: M

## Phase 270.2: GPU kernel parity

- [x] M270.P2.T1 — Mirror the kernels in the shared core and the matrix path
  - files: `Engine/Gpu/Kernels/resamplecore.slang`, `Engine/Gpu/Kernels/resample.slang`, `Engine/Gpu/GpuResample.h`, `Engine/Gpu/GpuResample.cpp`, `Engine/Nodes/Image/ResampleExecutor.cpp`
  - approach: Add the seven IDs, same numbering as `FilterEnum`; `kGpuResampleFilterCount` becomes 17. Add a `FilterSpec` struct in the core and the P1.T2 loop computed in float with FF positions. Remove `interpolateSuperAt`, level states, `kGpuResampleMaxLevel`, `GpuResampleJacobian`'s level fields and finally `Resampler::supersampleLevels`; `states` keeps only Box's zero bit. Add lobes, B, C and padding to `GpuResampleParams` and the blur entries, updating the `static_assert`. Extend `exactPositions` to every clamped filter so exact entries decide tap ranges as the CPU's doubles do; affine radius·scale arrives as double bits, projective stays two-float. Keep a `uint filter` overload of `sampleAt` so coord-map kernels build until T3 and T4.
  - verify: `ctest -R "NativeTransformGpu|StagedResample"` on lavapipe passes at the phase build with the T2 cases.
  - size: L

- [x] M270.P2.T2 — Test matrix-path CPU and GPU parity for the new filters
  - files: `Tests/Native/NativeTransformGpu_Test.cpp`, `Tests/Native/NativeReformatGpu_Test.cpp`, `Tests/Native/NativeCornerPinGpu_Test.cpp`, `Tests/Native/NativeAutoStraightenGpu_Test.cpp`, `Tests/Native/StagedResample_Test.cpp`
  - approach: Using `NativeGpuParity.h` and the neighbouring tolerance, cover each new filter at 1:1, 2x up, 4x and 64x down, with and without Clamp, on exact and inexact scales (3x up lands near tap boundaries); Sinc at Lobes 2 and 8; Bicubic at two (B, C) pairs; one motion-blur case; one projective case. The filter loops at `NativeTransformGpu_Test.cpp:583`, `NativeReformatGpu_Test.cpp:431`, `NativeCornerPinGpu_Test.cpp:580` and `NativeAutoStraightenGpu_Test.cpp:422` assert `eFilterNotch + 1 == filters.size()` and instantiate up to it: raise them, and `CoordMapGpu_Test.cpp:519` in P2.T5. Small synthetic fixtures only.
  - verify: `ctest -R "NativeTransformGpu|NativeReformatGpu|NativeCornerPinGpu|NativeAutoStraightenGpu|StagedResample"` passes on lavapipe at the phase build.
  - size: M

- [x] M270.P2.T3 — Carry the kernel through coordmap, STMap, IDistort and the variable warps
  - files: `Engine/Gpu/Kernels/coordmap.slang`, `Engine/Gpu/Kernels/stmap.slang`, `Engine/Gpu/Kernels/iwarp.slang`, `Engine/Nodes/Transform/WarpGpu.h`, `Engine/Nodes/Transform/IWarp.cpp`
  - approach: `sampleAt` takes the core's `FilterSpec`; drop `jacobianState`. Append lobes, B, C to `WarpGpuParams` (43 to 46 words, update the `static_assert`) and to IWarp's header (`IWarp.cpp:1141`, read at `iwarp.slang:47`), filled from `FilterSpec(settings.filter)` until Phase 270.3. Kernels read the same words the C++ writes.
  - verify: `ctest -R "CoordMapGpu|NativeIWarpGpu|NativeIDistort"` passes on lavapipe at the phase build.
  - size: M

- [x] M270.P2.T4 — Carry the kernel through the lens and SplineWarp kernels
  - files: `Engine/Gpu/Kernels/lensdistortion.slang`, `Engine/Gpu/Kernels/lenscorrect.slang`, `Engine/Gpu/Kernels/splinewarp.slang`, `Engine/Nodes/Transform/SplineWarp/SplineWarpGpu.h`, `Engine/Nodes/Transform/LensDistortion.cpp`, `Engine/Nodes/Transform/LensCorrect.cpp`
  - approach: As T3 for each params block (`LensDistortion.cpp:1234`, `LensCorrect.cpp:1446`, `SplineWarpGpuParams` 20 to 23 words). `SplineWarp.cpp:2183` fills the new words too. Then remove the `uint filter` overload of `sampleAt` once `coordmap_test.slang` is updated.
  - verify: `ctest -R "NativeLensDistortionGpu|NativeLensCorrectGpu|NativeSplineWarpGpu"` passes on lavapipe at the phase build.
  - size: M

- [x] M270.P2.T5 — Test coord-map CPU and GPU parity for the new filters
  - files: `Tests/Native/CoordMapGpu_Test.cpp`, `Tests/Native/NativeIWarpGpu_Test.cpp`, `Tests/Native/NativeLensDistortionGpu_Test.cpp`, `Tests/Native/NativeLensCorrectGpu_Test.cpp`, `Engine/Gpu/Kernels/coordmap_test.slang`
  - approach: Replace the supersampling cases at `CoordMapGpu_Test.cpp:479` with minifying Jacobians, axis-aligned and rotated, for all seventeen filters with Clamp, Sinc lobes and Bicubic (B, C) set. Add per-filter loops, as `NativeTransformGpu_Test.cpp:583` does, to the three node tests.
  - verify: `ctest -R "CoordMapGpu|NativeIWarpGpu|NativeLensDistortionGpu|NativeLensCorrectGpu"` passes on lavapipe at the phase build.
  - size: M

## Phase 270.3: Knobs and node wiring

- [x] M270.P3.T1 — Add the shared filter knob helper and move Transform and ITransform to it
  - files: `Engine/Nodes/Image/FilterKnobs.h` (new), `Engine/Nodes/Image/FilterKnobs.cpp` (new), `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/Transform/Transform.cpp`, `Engine/Nodes/Transform/ITransform.cpp`
  - approach: A helper creates `lobes`, `bicubicB` and `bicubicC` beside Filter and Clamp on a page, with names, labels and hints in `Resampler.h`; updates enabled state (replacing `updateClampEnabled`'s body); and reads a `FilterSpec` at time and view. Transform's `TransformOp` uses it (`Transform.cpp:192,511`), so ITransform gets it through `_op` (`ITransform.cpp:145`). Fit the default panel at 1920x1080.
  - verify: `ctest -R "NativeTransform|NativeIWarp"` passes at the phase build; an Xvfb check shows the knobs enable with the Filter choice, including after project load.
  - size: M

- [x] M270.P3.T2 — Wire Transform and Reformat through the staged path
  - files: `Engine/Nodes/Transform/Transform.cpp`, `Engine/Nodes/Transform/Transform.h`, `Engine/Nodes/Transform/Reformat.cpp`, `Engine/Nodes/Transform/Reformat.h`, `Engine/Nodes/Image/ResampleExecutor.cpp`
  - approach: Params structs hold `FilterSpec`; `ResampleSettings` carries it into `makeResampleParams` and the GPU record, including a concatenated chain (the last node's spec wins, as its filter does today). Reformat's knobs use the helper (`Reformat.cpp:432,523`). Draft quality still forces Impulse.
  - verify: `ctest -R "NativeTransform|NativeReformat|StagedResample"` passes at the phase build, including a test that changing Lobes changes a Sinc render on CPU and GPU.
  - size: M

- [x] M270.P3.T5 — Wire CornerPin, ICornerPin and AutoStraighten
  - files: `Engine/Nodes/Transform/CornerPin.cpp`, `Engine/Nodes/Transform/CornerPin.h`, `Engine/Nodes/Transform/ICornerPin.cpp`, `Engine/Nodes/Transform/AutoStraighten.cpp`, `Engine/Nodes/Transform/AutoStraighten.h`
  - approach: Use the helper at `CornerPin.cpp:285,761`, `ICornerPin.cpp:130` and `AutoStraighten.cpp:854,921`; params and settings hold `FilterSpec` (`AutoStraighten.h:105`). CornerPin's projective matrix relies on P2.T1's per-pixel Jacobian.
  - verify: `ctest -R "NativeCornerPin|NativeIWarp|NativeAutoStraighten"` passes at the phase build with a Lobes-changes-the-render case per node.
  - size: M

- [x] M270.P3.T6 — Wire STMap and IDistort
  - files: `Engine/Nodes/Transform/STMap.cpp`, `Engine/Nodes/Transform/STMap.h`, `Engine/Nodes/Transform/IDistort.cpp`, `Engine/Nodes/Transform/IDistort.h`, `Engine/Nodes/Transform/WarpGpu.h`
  - approach: Use the helper (`STMap.cpp:245`, `IDistort.cpp:168`); `Settings` holds `FilterSpec` for the CPU `resampleRowFromCoords` calls and the GPU words (`STMap.cpp:913`, `IDistort.cpp:871`). Replace the `eFilterNotch` range check at `IDistort.cpp:258` with the last enum value. Both already read the whole Source, so their regions don't change.
  - verify: `ctest -R "NativeSTMap|NativeIDistort|CoordMapGpu"` passes at the phase build, including a test that IDistort honours a new filter (it would fall back to the default under the old range check).
  - size: M

- [x] M270.P3.T7 — Wire LensDistortion and LensCorrect
  - files: `Engine/Nodes/Transform/LensDistortion.cpp`, `Engine/Nodes/Transform/LensDistortion.h`, `Engine/Nodes/Transform/LensCorrect.cpp`, `Engine/Nodes/Transform/LensCorrect.h`
  - approach: Use the helper (`LensDistortion.cpp:391`, `LensCorrect.cpp:452`); `Settings` holds `FilterSpec` for `resampleRowFromCoords` (`LensDistortion.cpp:1116`, `LensCorrect.cpp:1266,1272`) and the GPU words. Regions are unchanged: both read the whole Source. Enable the new knobs wherever the Filter knob is enabled.
  - verify: `ctest -R "NativeLensDistortion|NativeLensCorrect"` passes at the phase build with a new-filter render case per node.
  - size: M

- [x] M270.P3.T8 — Wire SplineWarp
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarp.h`, `Tests/Native/SplineWarpKnobs_Test.cpp`
  - approach: Use the helper (`SplineWarp.cpp:482,640`); `Settings` holds `FilterSpec` for the CPU calls at `SplineWarp.cpp:1994,2060` and the GPU words at `:2183`. Replace the range check at `:1212`. Add the three knobs to the knob test.
  - verify: `ctest -R "SplineWarpKnobs|NativeSplineWarp"` passes at the phase build.
  - size: M

- [x] M270.P3.T9 — Make the variable-warp and SplineWarp regions cover the footprint
  - files: `Engine/Nodes/Transform/IWarp.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarpField.h`, `Engine/Nodes/Transform/SplineWarp/SplineWarpField.cpp`, `Tests/Native/NativeIWarp_Test.cpp`
  - approach: Output must not depend on the render window. IWarp with no Map expands by the full transform's scale (P1.T3). With a Map connected the gradient bounds nothing, so read the whole Source, as IDistort does (`IDistort.cpp:345`) and as its GPU path already does (`IWarp.cpp:1074`). SplineWarp grows `getSourceBounds()` by radius·(largest field scale) − 0.5 at `SplineWarp.cpp:1744`, or reads the whole Source if the field can't report it.
  - verify: at the phase build, a test renders Lanczos 3 under a steep Map and a strong SplineWarp minification in two tiles and whole, and the results match exactly.
  - size: M

## Phase 270.4: Goldens, docs and sign-off

- [x] M270.P3.T3 — Re-record goldens and add node tests
  - files: `Tests/Native/NativeGolden_Test.cpp`, `Tests/Native/NativeReformat_Test.cpp`, `Tests/fixtures/native-golden/` (Transform, ITransform, ICornerPin, Reformat, STMapMap, IDistortMap, SplineWarp), `Tests/fixtures/native-parity/{Transform,Reformat}/` (rotate30-scale05-cubic, skew-yx, mask-mix05, project, format-par, box-fit; `NATRON_PARITY_RECORD_DIR`), `Tests/Native/NativeTransform_Test.cpp` and `NativeReformat_Test.cpp` tolerance comments
  - approach: Re-record only goldens whose output changed; rounding-only diffs are re-recorded without reopening the design, and the commit records the cause per directory. Add one Reformat downscale golden for Lanczos 3, one Transform golden for Sinc at Lobes 3 and one STMap minifying golden for Lanczos 3, as ≤256 px half-float compressed crops. Add a test that a 1/4 Lanczos 3 Reformat has lower alias energy than Impulse.
  - verify: `ctest -R "NativeTransform|NativeReformat|NativeGolden"` passes at the phase build; `du -sh Tests/fixtures/native-golden` grows by under 1 MB.
  - size: M

- [x] M270.P3.T4 — Document the filters on the Transform and Reformat pages
  - files: `Engine/Nodes/Transform/docs/Transform.md`, `Engine/Nodes/Transform/docs/Reformat.md`, the Filter hints in `Engine/Nodes/Image/Resampler.cpp`
  - approach: List all seventeen filters with when to use each; describe Lobes, Bicubic B and Bicubic C; state that the kernel widens when minifying and how the tap budget spreads samples. Name only the filters themselves as references, never other applications.
  - verify: `git grep -c "Lanczos 3" -- Engine/Nodes/Transform/docs` shows both pages.
  - size: M

- [x] M270.P3.T10 — Point the other node pages at the filter list
  - files: `Engine/Nodes/Transform/docs/` (AutoStraighten, CornerPin, ICornerPin, IDistort, ITransform, LensCorrect, LensDistortion, STMap, SplineWarp)
  - approach: One sentence per page that the node shares the Filter list and the three knobs, linking `Transform.md`. Replace supersampling wording (for example `STMap.md:16-18`) and check the parity tables still state what the tests do (`STMap.md:47-50`).
  - verify: `git grep -il supersampl -- Engine/Nodes/Transform/docs` returns nothing.
  - size: M

- [x] M270.P9.T1 — SplineWarp offers the new filters
  - files: `Tests/Native/NativeSplineWarpGpu_Test.cpp` (`EachFilterMatchesTheCpu`, `:498`; `TpsFrameMatchesTheCpuForEachFi...`, `:855`)
  - approach: Moved from M274 - SplineWarp Remaining Controls. The menu fills from `filterOptions()`; confirm both tests cover the new entries, including set lobes and (B, C). The Lanczos6 mentioned in M274 stays Sinc-only territory: there is no entry for it.
  - verify: both tests pass on lavapipe at the phase build; an Xvfb check lists the new entries on SplineWarp.
  - size: M

**Verification gate:** Build mode remote, one build per phase, with `tools/ci/local/syntax-check.py` first. `tools/ci/remote/build.sh release --tests` passes in full, and so does `tools/ci/remote/build.sh debug --tests`, including every Resampler, StagedResample, CoordMapGpu, per-node Gpu and NativeGolden test on lavapipe. Every node in Scope renders each new entry. No golden over 256 px. `git grep -i nuke` over the new docs and hints is empty. A local CPU bench of a 4x Reformat (Cubic and Lanczos 3, main vs branch) is reported in the PR but not gated. Xvfb screenshots of Transform's and STMap's filter panels are shared and the user approves them.
