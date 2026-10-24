# M113 - Native Warp Nodes

Rewrite the OFX warp nodes users rely on as redesigned native nodes, each with a CPU kernel and a GPU kernel:
- **`IDistort` and `STMap`**: U and V come from any layer of the UV input through the standard layer/channel knob. The openfx-misc "Alpha Channel" UV-coverage multiply and the "Unpremult UV" option are dropped (user, 2026-10-12).
- **`CornerPin`**: CornerPinMasked is merged into it. It has Nuke-like knobs and concatenates like Transform. **The tracker moves to its new knob names.**
- **`Mirror`**: flip and flop.

All four use plain IDs at major 1. Maths is scene-linear, and the host provides mask, mix and premult. There is no compatibility with the OFX versions. Registration, kernel lists, cost seeds, icons, docs and OFX retirement go through the per-node fragment files that M120 - Native Node Port Kit creates (`Engine/Nodes/Transform/<Node>.{reg.inc,costseed.inc}`, `docs/<Node>.md`, `Engine/Gpu/Kernels/<kernel>.cmake`), and one `Engine/OfxRetired/<ofx-plugin-id>` marker file per retired OFX ID. LensDistortion is split off into M132 - Native Lens Distortion.

Deps: M120 - Native Node Port Kit (per-node registration fragments), M85 - Native GPU Kernels (`resample.slang`, `pixelwrap.slang`, `PixelKernelGpu`), M56 - Visual And Menu Polish (it still changes the IDistort/STMap take-over layout this milestone replaces).

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion). OFX sources are under `build/assets/plugin-src/openfx-misc/`.

**IDistort and STMap** (`Distortion/Distortion.cpp`, one template class):
- **IDistort** (`net.sf.openfx.IDistort`):
  - Inputs are Source and UV.
  - U and V are offsets in full-scale pixels: `u = (u - uvOffset) * uvScale`. uvScale is also multiplied by renderScale (`:2519`).
  - The sample point is `x + u + 0.5, y + v + 0.5` (`:1000`).
  - Defaults are uvOffset (0,0) and uvScale (1,1) (`:3699`, `:3713`).
- **STMap** (`net.sf.openfx.STMap`):
  - UV is the first input, and the output format comes from it.
  - Coordinates are normalized: `sx = srcx1 + u*(srcx2-srcx1)`, with no +0.5 (`:981`).
  - Wrap modes are clamp, repeat (`x - floor(x)`) and mirror, applied after offset and scale (`:891`).
  - The format is the source format, or its RoD when the format is empty or `useRoD` is set (`:2523`).
- **Shared:**
  - Filter knobs come from `ofxsFilterDescribeParamsInterpolate2D` (`:4616`). There are 10 filters, Cubic is the default, and `clamp` is off. `black_outside` defaults to true for STMap and false for IDistort.
  - Sampling uses `ofxsFilterInterpolate2DSuper`, with a Jacobian from central differences of UV (impulse skips the Jacobian).
  - A concatenated upstream 3x3 transform is applied to (sx, sy) (`:1100`). There is no motion blur.
  - The dropped alpha pipeline is at `:918-1088`.

**CornerPin** (`CornerPin/CornerPin.cpp`, built on Transform3x3):
- **IDs:** `net.sf.openfx.CornerPinPlugin` and `...CornerPinMaskedPlugin` (`:105`).
- **Knobs:**
  - Points: `to1..4` and `from1..4` (Double2D), plus `enable1..4`. Each enable flag covers both the from and the to point.
  - Overlay and amount: `overlayPoints` (To/From), `transformAmount`.
  - Extra matrix: `transform_row1..3`.
  - Behaviour: `invert`, `filter`, `clamp`, `black_outside`.
  - Motion blur: `motionBlur`, `shutter`, `shutterOffset`, `shutterCustomOffset`.
- **Maths** (`:285-380`):
  - The number k of enabled points picks the solver: 4 is a homography, 3 affine, 2 similarity, 1 translation.
  - The "to" points are lerped from the "from" points by `transformAmount`.
  - The result is `homography * extraMat`, with the sign flipped if z<0 at p0.
- **In-tree solvers:** `Engine/Transform.h:178-199`: `setHomographyFromFourPoints`, `setAffineFromThreePoints`, `setSimilarityFromTwoPoints`, `setTranslationFromOnePoint`.

**Mirror** (`Mirror/Mirror.cpp`, `net.sf.openfx.Mirror`):
- Knobs are `flip` and `flop`.
- It reverses pixels exactly about the source RoD: `xoff = x1+x2-1` (`:431`).
- It has no concatenation and no filter.

**Native resample infrastructure:**
- `Engine/Nodes/Image/Resampler.h`:
  - `FilterEnum` lists the same 10 filters (`:87`), with `filterOptions()` at `:116`.
  - `interpolate(...)` (`:319`) and `interpolateSuper(..., Jxx, Jxy, Jyx, Jyy, ...)` (`:337`) take a per-pixel source coordinate and Jacobian.
  - `SourceImage` is at `:273`.
  - `resampleRow` (`:501`) is the only full-pipeline entry. It maps through a projective 3x3 `Mat3` inverse (`z<=0` gives 0).
- **There is no per-pixel-coordinate row sampler and no wrap/mirror mode.**
- Motion blur:
  - `BlurSettings` (`:391`) and `buildSamplingTransforms` (`:445`) drive it, with `motionBlurPixel` doing stratified sampling.
  - It works for matrix geometry only.
- Concatenation: native Transform implements `getTransform` (`Transform.cpp:850`), fetches with `&job->inputTransform` (`:1065`) and calls `concatenateInputTransform` (`:1113`).
- `Engine/Nodes/Image/TransformMath.h` has `Mat3` and the canonical-to-pixel helpers, and no corner-pin solver.

**GPU (M85 - Native GPU Kernels):**
- M85.P5.T1 creates `Engine/Gpu/Kernels/resample.slang`, porting `filter1D`/`filterWeights` and `interpolate`/`interpolateSuper` for all 10 filters with clamp and black-outside.
- It refuses motion blur and concatenated inputs, which fall back to the CPU. P5.T2 adds `motionBlurPixel`.
- The matrix path should accept a projective matrix. This is unverified.
- No per-pixel-coordinate GPU kernel is planned anywhere.

**Host overlay:**
- `Node::addCornerPinInteract(from1..4, to1..4, enable1..4, overlayPoints, invert, interactive)` (`Engine/Node.h:1446`, `Engine/NodeOverlay.cpp:391`) binds **by knob pointer**. It builds `HostOverlayKnobsCornerPin` (`Engine/HostOverlaySupport.h:118`), drawn by `Gui/HostOverlay.cpp:792` and created at `:1099`.
- `invert` and `interactive` are optional (`NodeOverlay.cpp:429-434`). `overlayPoints` must be a real knob.

**Tracker coupling (by knob name):**
- `Engine/EffectInstance.h:64`: `PLUGINID_OFX_CORNERPIN`.
- `Engine/TrackerContextPrivate.cpp`:
  - `:149` creates the internal CornerPin and throws if Misc.ofx is missing (`:155`).
  - The wiring is Input → CornerPin → Transform → Output (`:174-177`).
  - `:558-598` duplicates `from`, `to`, `from1..4`, `to1..4`, `enable1..4`, `overlayPoints` and `transform`.
  - `:605-624` duplicates `invert`, `filter`, `clamp`, `black_outside`, `motionBlur`, `shutter`, `shutterOffset` and `shutterCustomOffset` (`TrackerContextPrivate.h:275-282, 308`).
  - `createDuplicateKnob` (`:63`) only does `assert(otherNodeKnob)` (`:78-84`). **Asserts are inert in this build (NDEBUG), so a missing knob is dereferenced.**
  - The tracker expects a knob named `transform`, but OFX names it `transform_row1..3`. The lookup is null-checked (`:597`, `TrackerContext.cpp:~897`), so it likely returns null today.
- `Engine/TrackerContext.cpp`:
  - `exportTrackDataFromExportOptions` starts at `:798` and creates the node at `:823-838`.
  - `getCornerPinPoint` is at `:783`.
  - `from` is set at the reference frame (`:858`), and `to` is cloned or slaved. The matrix is cloned from `transform` (`:874`).
- `Gui/MultiInstancePanel.cpp:2306-2412` is a legacy CornerPin export whose call site is commented out (`:2246-2252`).

**Other references:**
- `Engine/OfxEffectInstance.cpp:1279-1287` (`defaultProcessesAllLayers`: CornerPin, CornerPinMasked, Mirror, IDistort, STMap, LensDistortion).
- `Engine/EffectInstance.cpp:2231` (comment).
- Tests:
  - `Tests/ColorViewsRender_Test.cpp:90, 849, 892`
  - `Tests/ProjectSerialization_Test.cpp:554`
  - `Tests/LayerKnobsRender_Test.cpp:805, 842, 922`
  - `Tests/KnobLayerSelect_Test.cpp:695`
  - `Tests/DefaultChannelSet_Test.cpp:59-70`
- No PyPlug or menu references.

## Scope

**In:**
- Native `IDistort`, `STMap`, `CornerPin` and `Mirror`, each with a CPU kernel, a GPU kernel, CPU tests and a GPU parity test.
- A shared per-pixel-coordinate row sampler (CPU) and kernel module (Slang) with wrap modes. M132 - Native Lens Distortion reuses both.
- The tracker's internal CornerPin and its export moved to the native node and its knob names.
- Retiring the OFX IDistort, STMap, CornerPin, CornerPinMasked and Mirror.

**Out:**
- LensDistortion → M132 - Native Lens Distortion.
- Card3D → E9 - USD And 3D.
- The UV alpha-coverage multiply and Unpremult UV. These are dropped (2026-10-12) and owned by no milestone.
- Motion blur for IDistort and STMap. The OFX nodes have none.
- Renaming the existing Transform's IDs → M111 - Native Node Identity Cleanup.

**Edge cases:**
- UV input disconnected: IDistort is the identity on Source. STMap outputs black, since it has no coordinates.
- UV layer with one channel: the missing channel reads zero (layer rule), so V = 0.
- NaN/inf UV values: the sample is treated as outside, giving black (or the edge under clamp wrap). It never propagates NaN.
- CornerPin with 0 enabled points: only the extra matrix applies. Degenerate points (collinear for 4, coincident for 2) fall back to the next-lower solver, and the identity if every solver fails.
- CornerPin with z<0 at p0: the sign is flipped as in OFX. Pixels with z<=0 read black.
- The tracker is created in a project with no OFX bundles: it works, because it no longer needs Misc.ofx.
- Mirror on an odd-sized or offset RoD: the reversal is about the RoD centre, pixel-exact.

**Assumptions:**
- Unconfirmed: Mirror is a flip matrix that concatenates with neighbouring transforms. Standalone it renders impulse-exact (a pure index reversal, no filtering).
- Unconfirmed: STMap's output format is the UV input's format (Nuke behaviour). `useRoD` becomes a "format from: UV format / UV RoD" choice.
- Unconfirmed: IDistort/STMap filtering keeps the Jacobian-from-UV-differences approach, and impulse skips it.
- M85 - Native GPU Kernels's GPU matrix resample path accepts a projective matrix. P3.T3 verifies this, and if it doesn't, extends it.
- `Gui/MultiInstancePanel.cpp`'s CornerPin export is dead code (its call site is commented out), so it is deleted rather than ported.

## Phase 113.1: Shared per-pixel-coordinate sampler

- [x] M113.P1.T1 — Add a per-pixel-coordinate row sampler with wrap modes to the resampler
  - files: `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/Image/Resampler.cpp`, `Tests/Native/ResamplerCoordMap_Test.cpp` (new)
  - approach:
    - Add `resampleRowFromCoords(const SourceImage&, const float* sx, const float* sy, const float* jacobian /*nullable*/, int n, filter, clamp, blackOutside, WrapMode wrapU, WrapMode wrapV, float* out, nComps)`. It calls `interpolateSuper` when a Jacobian is given and `interpolate` otherwise.
    - `WrapMode` is `{None, Clamp, Repeat, Mirror}`, applied in source-normalized space before sampling. `None` keeps `blackOutside` semantics.
    - Non-finite coordinates read as outside.
    - Add a helper that computes per-pixel Jacobians from central differences of a coordinate row triple (row above, row, row below) so callers don't each reimplement it.
  - verify: the new test covers an identity coordinate map against a direct copy (bit-exact with impulse), each wrap mode on a 4×4 ramp, a NaN coordinate giving 0, and a Jacobian-scaled minification that matches `resampleRow` with an equivalent scale matrix within 1e-5. It runs at the phase build.
  - size: L

- [x] M113.P1.T2 — Add the Slang coordinate-map sampling module on top of `resample.slang`
  - files: `Engine/Gpu/Kernels/coordmap.slang` (new), `Engine/Gpu/Kernels/coordmap.cmake` (new, per-kernel fragment per M120 - Native Node Port Kit), `Tests/Native/CoordMapGpu_Test.cpp` (new)
  - approach:
    - Import M85 - Native GPU Kernels's `resample.slang` and expose `sampleAt(src, sx, sy, J, filter, clamp, blackOutside, wrapU, wrapV)` with the same wrap and non-finite rules as P1.T1.
    - Add one test-only compute entry that samples a coordinate buffer, so parity can be tested before any node uses it.
  - verify: `CoordMapGpu_Test` runs P1.T1's cases through the GPU entry with the NativeGpuParity harness, tolerance 1e-5 absolute. It runs at the phase build on RADV.
  - size: L

## Phase 113.2: IDistort and STMap

- [x] M113.P2.T1 — Add the native STMap node on the CPU
  - files: `Engine/Nodes/Transform/STMap.{h,cpp}` (new), `Engine/Nodes/Transform/STMap.reg.inc`, `Tests/Native/NativeSTMap_Test.cpp` (new)
  - approach:
    - `STMap` v1. Inputs are UV (0, which sets the format) and Source (1), plus the host mask.
    - Knobs:
      - `uv` layer with `u`/`v` channel selects through `declareLayerKnob` on input 0.
      - `uvOffset` and `uvScale`.
      - `wrapU`/`wrapV` (Clamp default, Repeat, Mirror).
      - `filter`, `clamp`, `blackOutside` (default on).
      - "Format from" (UV format / UV RoD).
    - The kernel reads the UV row (plus the rows above and below for Jacobians) and calls P1.T1's sampler.
    - `getRegionsOfInterest` asks Source for its whole RoD. UV is asked for the window plus one row each way.
    - No alpha multiply, no unpremult UV.
  - verify: the test covers an identity ST map reproducing Source, each wrap mode, offset/scale, U/V from a non-colour layer, UV disconnected giving black, and mask/mix from the base. It runs at the phase build.
  - size: L

- [x] M113.P2.T2 — Add the native IDistort node on the CPU
  - files: `Engine/Nodes/Transform/IDistort.{h,cpp}` (new), `Engine/Nodes/Transform/IDistort.reg.inc`, `Tests/Native/NativeIDistort_Test.cpp` (new)
  - approach:
    - `IDistort` v1. Inputs are Source (0) and UV (1), plus the host mask.
    - U/V layer and channel selects on input 1. `uvOffset` and `uvScale` are in pixels (canonical, so they scale with render scale). `filter`, `clamp` and `blackOutside` (default off).
    - The sample point is `x + 0.5 + u`.
    - UV disconnected, or a zero scale, means `isIdentity` on Source.
    - `getRegionsOfInterest` on Source is conservative: its full RoD. Unconfirmed: no bounding-offset knob.
  - verify: the test covers zero UV giving Source, a constant +2 px U shifting the image by 2 px, uvScale, a render scale of 0.5 matching a downscaled full render within the filter tolerance, and U/V from an arbitrary layer. It runs at the phase build.
  - size: L

- [ ] M113.P2.T3 — Run STMap and IDistort on the GPU
  - files: `Engine/Gpu/Kernels/stmap.slang` (new), `Engine/Nodes/Transform/STMap.cpp`, `Engine/Nodes/Transform/IDistort.cpp`, `Engine/Gpu/Kernels/stmap.cmake`, `Tests/Native/NativeWarpGpu_Test.cpp` (new)
  - approach:
    - One Slang file with two entries (`stmap_main`, `idistort_main`), both built on `coordmap.slang` and wrapped by M85 - Native GPU Kernels's `pixelwrap.slang` for mask, mix and channels.
    - The UV input is a second storage-buffer view, which is possible because `GpuRenderArgs::inputs` is indexed by input number.
    - `canRenderOnGpu` refuses only what the CPU kernel would also skip.
    - Add the cost-seed rows to the Transform family seed file from M120.
  - verify: `NativeWarpGpu_Test` runs P2.T1's and P2.T2's fixtures CPU against GPU, tolerance 1e-5 absolute (filters other than impulse) and exact for impulse. It runs at the phase build on RADV.
  - size: L

## Phase 113.3: CornerPin

- [x] M113.P3.T1 — Add the native CornerPin node on the CPU, Masked merged in
  - files: `Engine/Nodes/Transform/CornerPin.{h,cpp}` (new), `Engine/Nodes/Transform/CornerPinKnobs.h` (new: shared knob script names), `Engine/Nodes/Transform/{CornerPin,CornerPinKnobs}.reg.inc`, `Tests/Native/NativeCornerPin_Test.cpp` (new)
  - approach:
    - `CornerPin` v1. Source input plus the host mask (one node, no Masked variant).
    - Knobs, all named in `CornerPinKnobs.h`:
      - `to1..4`, `from1..4` and `enable1..4`.
      - `overlayPoints` (To/From), `amount`, `extraMatrix` (one 3×3 knob) and `invert`.
      - `filter`, `clamp`, `blackOutside`.
      - `motionBlur`, `shutter`, `shutterOffset`, `shutterCustomOffset`.
      - "Set to input" and "copy from/to" buttons.
    - Build the matrix with the `Engine/Transform.h:178-199` solvers by enabled-point count, lerping to-from by `amount`, times the extra matrix, with the z<0 sign flip and degenerate fallback (see Scope).
    - Implement `getTransform` so it concatenates like Transform, and render through `resampleRow` and `motionBlurPixel`.
    - Declare `addCornerPinInteract` with real knobs.
  - verify: the test covers 4/3/2/1/0 enabled points against hand-computed matrices, invert, amount 0 giving identity, a degenerate fallback, concatenation with a following Transform matching one combined resample, and motion blur on an animated pin. It runs at the phase build.
  - size: L

- [ ] M113.P3.T2 — Move the tracker's internal CornerPin and export to the native node
  - files: `Engine/TrackerContextPrivate.cpp`, `Engine/TrackerContextPrivate.h`, `Engine/TrackerContext.cpp`, `Engine/EffectInstance.h` (`PLUGINID_OFX_CORNERPIN` → the native ID), `Gui/MultiInstancePanel.cpp` (delete the dead CornerPin export at `:2306-2412` and its commented call site)
  - approach:
    - Create the native `CornerPin` by ID, and drop the "requires Misc.ofx.bundle" error path.
    - Duplicate knobs through the `CornerPinKnobs.h` names. The extra-matrix knob is now real, so the `transform` lookup resolves.
    - Replace `assert(otherNodeKnob)` in `createDuplicateKnob` with a real check that logs and skips, because asserts are inert in this build.
    - The export's `getCornerPinPoint`, `from`/`to` set-up and matrix clone use the new names.
  - verify: an existing or new tracker test (`Tests/Tracker*_Test.cpp`, or add `Tests/TrackerCornerPin_Test.cpp`) creates a tracker, checks every duplicated knob is non-null, and exports to a CornerPin whose `to` points follow the track. Xvfb check at the gate: track a fixture, export CornerPin, and the overlay draws.
  - size: L

- [ ] M113.P3.T3 — Run CornerPin on the GPU
  - files: `Engine/Nodes/Transform/CornerPin.cpp`, `Engine/Nodes/Transform/CornerPin.costseed.inc` (new), `Tests/Native/NativeCornerPinGpu_Test.cpp` (new)
  - approach:
    - Reuse M85 - Native GPU Kernels's Transform matrix GPU path (`resample.slang`) with CornerPin's projective matrix, and motion blur through M85.P5.T2's port.
    - If that path assumes affine (`z == 1`), add the projective divide and the z<=0 → black rule there. Do it there, not in a copy.
    - Add the cost seed. No new kernel fragment: the matrix path's `resample.slang` fragment is M85 - Native GPU Kernels's.
  - verify: parity on P3.T1's homography, motion-blur and concatenated fixtures, tolerance 1e-5 absolute. It runs at the phase build on RADV.
  - size: M

## Phase 113.4: Mirror

- [x] M113.P4.T1 — Add the native Mirror node as a concatenating flip
  - files: `Engine/Nodes/Transform/Mirror.{h,cpp}` (new), `Engine/Nodes/Transform/Mirror.reg.inc`, `Tests/Native/NativeMirror_Test.cpp` (new)
  - approach:
    - `Mirror` v1 with `flip` and `flop`. `getTransform` returns the reflection about the source RoD centre, so it concatenates.
    - Standalone render is an exact index reversal, with no filtering.
    - Identity when both are off.
    - GPU: an index-reversal point op through `makeGpuKernel`, or the matrix path with impulse. Whichever is exact.
  - verify: the test covers flip, flop, both, an odd-width RoD, offset RoDs, Mirror + Transform concatenation matching one resample, and GPU parity exact. It runs at the phase build.
  - size: M

## Phase 113.5: Retire the OFX warps

- [x] M113.P5.T1 — Retire the OFX IDistort, STMap, CornerPin, CornerPinMasked and Mirror and fix their references
  - files: `Engine/OfxRetired/<ofx-plugin-id>` (five new marker files, per M120 - Native Node Port Kit), `Engine/OfxEffectInstance.cpp` (`:1279-1287` entries), `Tests/DefaultChannelSet_Test.cpp`, `Tests/LayerKnobsRender_Test.cpp`, `Tests/ColorViewsRender_Test.cpp`
  - approach:
    - Add one `Engine/OfxRetired/<ofx-plugin-id>` marker per OFX ID (IDistort, STMap, CornerPin, CornerPinMasked and Mirror), each naming its native replacement, so the host skips them.
    - Drop their `defaultProcessesAllLayers` entries.
    - Move the test cases to the native IDs where the behaviour still applies, and delete them where it was OFX-only (e.g. the multiplane UV picker).
    - `Tests/ProjectSerialization_Test.cpp:554` and `Tests/KnobLayerSelect_Test.cpp:695` are P5.T2.
  - verify: a test asserts each retired ID fails to load and its native replacement does. The full ctest passes at the gate.
  - size: M

- [ ] M113.P5.T2 — Move the remaining warp references in tests to the native IDs
  - files: `Tests/ProjectSerialization_Test.cpp`, `Tests/KnobLayerSelect_Test.cpp`
  - approach: swap the OFX warp IDs and knob names for the native ones where the test's point still applies. Delete the cases that only covered OFX plumbing (the multiplane UV picker).
  - verify: both tests pass at the gate.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including every new `Native*` and GPU parity test on RADV.
- Xvfb GUI check, with screenshots shared before sign-off:
  - STMap and IDistort with U/V from a non-colour layer.
  - The CornerPin overlay.
  - A tracker export to CornerPin that follows the track.
- An AppImage built with `package.sh` for UAT.
- The retired OFX IDs no longer load.

## Decisions

- 2026-10-12 — **Created from M69 - Minor Cleanup (user):** instead of giving the OFX IDistort and STMap a UV-layer dropdown (the cancelled M69.P9.T1), rewrite them as native nodes, drop the alpha channel feature, and use this milestone for further native-node rewrites.
- 2026-10-13 — **Scope is the warp family (user, native-node port grill):** IDistort, STMap, CornerPin and Mirror. Redesigned, not ported for parity. Plain IDs, scene-linear maths, GPU kernels in this milestone.
- 2026-10-13 — **LensDistortion gets its own milestone (user):** M132 - Native Lens Distortion, with all models. It reuses this milestone's per-pixel-coordinate sampler.
- 2026-10-13 — **CornerPin is redesigned and the tracker moves to it (user):** one node with Masked merged in, Nuke-like knobs, concatenation. The tracker's internal node and export are edited to the new knob names through a shared knob-name header, rather than keeping OFX script names.
- 2026-10-13 — **Card3D is out:** it belongs with E9 - USD And 3D.
- 2026-10-24 — **Claimed by bosley-native-nodes** as the next eligible E3 milestone while M16 and M138 wait on UAT (user's rule for this run). Skipped: M79 (stub blocked on a decoding-library choice), M101 (overlaps M110 on `Engine/GpuRegionTask.cpp`, `Engine/GpuPlacement.cpp`, `Engine/Nodes/Image/Op.h`), M111 (deps M101), M95 (overlaps M110 on `Engine/Nodes/Transform/Position.h`). M113 has no file overlap with active rows. Its three Unconfirmed assumptions (Mirror concatenates; STMap format from the UV input; IDistort/STMap keep the Jacobian filter) follow Nuke and are checked at UAT.
- 2026-10-24 — **P1.T1/P1.T2 notes for P2.T3:** the CPU sampler takes an optional `wrapDomain` (STMap wraps on the source format, not the fetched bounds) and the GPU `sampleAt` takes the same `domain`. Clamp clamps to edge pixel centres and zeroes that axis's Jacobian row; any non-finite coordinate after wrapping samples 0. Jacobians are central differences one pixel wider than the window. For P2.T3: `resample.slang` declares a push-constant block and unbound buffers, which clash with `pixelwrap.slang`'s bindings 0–3 in one kernel — split `resample.slang` into a parameter-free module; there is no GPU `coordJacobians` yet. Plugin IDs are plain (`Mirror`, `STMap`, `IDistort`, `CornerPin`) per DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md.
- 2026-10-24 — **P5 (code in the commit after 116f5b8c6):** markers for `net.sf.openfx.{IDistort,STMap,CornerPinPlugin,CornerPinMaskedPlugin,Mirror}` (Mirror's OFX ID has no `Plugin` suffix). The five tests P5.T1/P5.T2 listed don't reference the retired IDs: they exercise OFX multiplane host-twin plumbing through the test bundle's `fr.natron.test.MatrixWarp`, so they stay. `Tests/gui/m56_visual.py` dropped its OFX-only IDistort/STMap host-channel checks. P3.T2: `Gui/MultiInstancePanel.cpp`'s CornerPin export is live (the legacy TrackerPM panel, `Gui/NodeGui.cpp:702`), not dead as assumed; it now creates the native CornerPin, and `PLUGINID_OFX_CORNERPIN` is gone. STMap's icon is openfx-misc's.
- 2026-10-24 — **Host reboot mid-run; state reconstructed from the branch:** P1.T1, P1.T2, P2.T1, P2.T2, P3.T1, P3.T3, P4.T1 and P5.T1 are implemented (6197839b0, 434d80492, 116f5b8c6, 0dbb779bc, ce4111d50, 30eb6fa78, 899bbfba2, c97bb73f4, e802072b7); P3.T2 is partial (5073e10d1); P2.T3 is not started. The first remote fast run on e802072b7 failed CornerPin (CPU, GPU, GPU filter), GpuCapability, IDistort, Mirror and GoldenXml; boxes are checked once those pass.
