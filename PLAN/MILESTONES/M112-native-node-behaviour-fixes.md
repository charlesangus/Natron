# M112 - Native Node Behaviour Fixes

The behaviour and maths that the M67 - Native Core Nodes ports copied from OFX on purpose, and that are wrong for a scene-linear compositor:
- default clamps that clip HDR
- LUT `range` knobs and maths that assume [0,1]
- spatial filters whose output depends on the mask and the render window
- Reformat reading the timeline's current frame
- upstream EdgeDetect bugs
- Crop smearing edge pixels
- a hard-coded format table with a typo

The README's OFX-parity rule is retired (`DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md`), so each fix re-records its regression fixtures and notes the change under `## Decisions`.

Out of scope:
- **The keyers:** M95 - Knob Cleanup And Keyer Redesign covers Keyer and, since 2026-10-12, ChromaKeyer.
- **`hidpi`/`interactive` and the footer move:** also M95.
- **IDs, siblings and dead knobs:** M111 - Native Node Identity Cleanup.

Deps: none. The overlap with M95 on ExtentKnobs and the colour nodes, and with M111 on Blur, ColorLookup, Crop and Reformat, is file overlap, which the parallel-lane rule handles. It is not a logical dependency.

Scouted facts (2026-10-12, read-only audit on `milestone/m45-native-tab-menu`; re-check line numbers at promotion):
- **Default clamps:**
  - `clampBlack` defaults to on in Grade (`Color/Grade.cpp:292`), ColorCorrect (`ColorCorrect.cpp:469`) and Saturation (`Saturation.cpp:189`).
  - Because of it, a default node is never treated as identity (`Grade.cpp:332`, `ColorCorrect.cpp:535`, `Saturation.cpp:226`).
  - Grade's gamma ≤ 0 becomes a step function around 1 (`Grade.cpp:115-122`).
- **ColorCorrect:**
  - The output clamps also clamp the tone-range curve weights (`ColorCorrect.cpp:118, 205`).
  - The `range` knob only sizes a LUT, defaulting to [0,1], and the curves are fixed to [0,1] (`:421-445`). ColorLookup does the same (`ColorLookup.cpp:507-538`).
- **ColorLookup:** the "weighted standard" `triangle()` divides by `(1-a)` (`ColorLookup.cpp:~250-260`).
- **Clamp:** its description says "MaxClampTop" (`Clamp.cpp:207`).
- **Spatial filters:**
  - Blur, EdgeDetect and ErodeDilate shrink the filtered window to skip mask-zero rows and columns, "exactly as" CImg does (`Filter/SpatialFilterSupport.cpp:358`; `Blur.cpp:727`; `EdgeDetect.cpp:704`; `ErodeDilate.cpp:493`).
  - They write mask/mix/premult back through their own `writeWindow` (`SpatialFilterSupport.h:245-329`).
- **EdgeDetect:** it normalises the Gaussian with 1/2.4 "where sigma would be expected, as in the OpenFX plug-in". Its RMS sign alignment never triggers (`EdgeDetect.cpp:588-592, 874-875`).
- **Blur's CImg knobs:** `orderX/orderY`, `boundary`, `expandRoD`, `cropToFormat`, `alphaThreshold`, and a hidden `uniform` (`Filter/Blur.h:43-59`, `Blur.cpp:191-276, 283`). The IIR filters copy CImg's float/double rounding (`BlurKernels.cpp:36-37`). The FIR Gaussian is the default since M88 - FIR Gaussian Blur.
- **Reformat:**
  - It takes the source format from `getNearestNonIdentity(getCurrentTime())` (`Reformat.cpp:715-719`).
  - `getPreferredMetadata` evaluates at time 0 (`:1119`).
  - The source is cut to the requested window, so edge clamping happens there (`:1203-1207`).
- **Transform/Reformat `clamp`:** it is ignored for the default Cubic filter (`Resampler.h:124-129`), and turning it on blocks identity (`Transform.cpp:975-978`, `Reformat.cpp:658-664`).
- **Crop:** outside the source it repeats the nearest edge pixel (`Crop.cpp:152-190`).
- **Format tables:** the openfx-misc table, with the typo "HD_720 1280x1720", is copied into `Image/ExtentKnobs.cpp:54-77` (typo at `:61-62`) and `Reformat.cpp:67-89` (typo at `:75`).
- **Generators:**
  - The "Default" extent uses the Source clip or the project format from (0,0), ignoring its offset (`NativeGenerator.cpp:286-299`).
  - `frameRange` defaults to 1..1 (`ExtentKnobs.cpp:259-266`).

## Phase 108.1: Colour nodes

- [ ] M112.P1.T1 — Turn off the clamps by default and make default nodes identity
  - files: `Engine/Nodes/Color/Grade.cpp`, `ColorCorrect.cpp`, `Saturation.cpp`, `Tests/Native/NativeGrade_Test.cpp`, `NativeColorCorrect_Test.cpp` (and `NativeSaturation_Test.cpp`)
  - approach:
    - `clampBlack` and `clampWhite` default off on all three. The knobs stay, and M95 moves them below the line.
    - `isIdentity` is true at default values.
    - Grade's gamma ≤ 0 clamps gamma to a small positive minimum instead of the step function.
  - verify: a default Grade, ColorCorrect and Saturation are identity, and negatives and values above 1 pass through untouched. Re-record the fixtures.
  - size: M

- [ ] M112.P1.T2 — Drop the LUT `range` knobs and decouple ColorCorrect's weight clamping
  - files: `Engine/Nodes/Color/ColorCorrect.cpp`, `Engine/Nodes/Color/ColorLookup.cpp`, `Tests/Native/NativeColorCorrect_Test.cpp`, `Tests/Native/NativeColorLookup_Test.cpp`
  - approach:
    - Evaluate the tone-range and lookup curves directly. Alternatively, size an internal LUT from the image's actual range and fall back to direct evaluation outside it. Delete the user-facing `range` knob.
    - ColorCorrect's shadow/midtone/highlight weights are always clamped to [0,1], independent of the output clamps.
    - Guard ColorLookup's weighted-standard `triangle()` for a ≥ 1.
  - verify: tests check curves at values above 1 and below 0, weights in range with the output clamps off, and the weighted-standard mode at a = 1 and a = 2 without inf or NaN.
  - size: M

- [ ] M112.P1.T3 — Fix Clamp's description typo
  - files: `Engine/Nodes/Color/Clamp.cpp`
  - approach: "MaxClampTop" → the knob's real label. Check `ColorMath.h:150`'s claim that "Rec. 709 uses Rec. 2020 constants", and fix either the constants or the comment.
  - verify: review the diff.
  - size: S

## Phase 108.2: Spatial filters

- [ ] M112.P2.T1 — Filter a fixed region, then apply the mask
  - files: `Engine/Nodes/Filter/SpatialFilterSupport.{h,cpp}`, `Blur.cpp`, `EdgeDetect.cpp`, `ErodeDilate.cpp`
  - approach:
    - The filtered window is the render window plus the filter halo, never shrunk by the mask. The mask and mix only apply after the filter.
    - Where possible, route the mask/mix/premult write-back through the base class's helper instead of `writeWindow`. Otherwise keep one shared helper and say why in the report.
  - verify: a new test renders the same pixel through two different render windows and with two masks that are both 1 at that pixel, and the results are identical (IIR and FIR Blur, EdgeDetect, ErodeDilate).
  - size: L

- [ ] M112.P2.T2 — Fix EdgeDetect's copied normalisation and RMS bugs
  - files: `Engine/Nodes/Filter/EdgeDetect.cpp`, `Tests/Native/NativeEdgeDetect_Test.cpp`
  - approach: normalise the Gaussian by sigma, and make the RMS sign alignment do what its code intends, or delete it if the result is equivalent. Re-record the fixtures.
  - verify: an analytic test with a step edge of known height gives the expected gradient magnitude for each operator.
  - size: M

- [ ] M112.P2.T3 — Trim Blur's knobs to a Nuke-like set
  - files: `Engine/Nodes/Filter/Blur.{h,cpp}`, `Tests/Native/NativeBlur_Test.cpp`, `Tests/Native/BlurFIRGaussian_Test.cpp`, any bundled PyPlug that sets the removed knobs
  - approach:
    - Keep size, filter and crop to format.
    - Delete `orderX/orderY` (derivatives belong to EdgeDetect), `alphaThreshold`, the `boundary` choice (black outside), `expandRoD` (always expand) and the hidden `uniform`.
    - Keep the IIR kernels only as filter choices. Drop the claim that they match CImg's rounding.
  - verify: the Blur tests pass for each filter. The deleted knobs are gone from the panel.
  - size: M

## Phase 108.3: Transform family

- [ ] M112.P3.T1 — Fix Reformat's frame and edge handling
  - files: `Engine/Nodes/Transform/Reformat.cpp`, `Tests/Native/NativeReformat_Test.cpp`
  - approach:
    - Take the source format from the input's metadata at the render or metadata time, not the timeline's current time.
    - `getPreferredMetadata` evaluates at the metadata time, not 0.
    - Clamp filter edges at the real image bounds, not at the requested window.
  - verify: tests check an input whose format changes over time, rendered at a frame other than the current one; an animated box size past frame 0; and identical edge pixels for two request windows.
  - size: M

- [ ] M112.P3.T2 — Make Crop read black outside its source, and make Transform/Reformat honour `clamp`
  - files: `Engine/Nodes/Transform/Crop.cpp`, `Engine/Nodes/Image/Resampler.h`, `Engine/Nodes/Transform/Transform.cpp`, `Reformat.cpp`, `Tests/Native/NativeCrop_Test.cpp`
  - approach:
    - Crop reads black, not the nearest edge pixel, outside the source.
    - `clamp` either applies to Cubic, or is disabled in the UI when the filter ignores it. Pick one and record it.
    - A Transform or Reformat that does no resampling is identity whatever `clamp` says.
  - verify: tests check a crop box larger than the source, and `clamp` with Cubic overshoot. A no-op Transform with `clamp` on is identity.
  - size: M

## Phase 108.4: Generators

- [ ] M112.P4.T1 — Use the project's formats and range in generators, Crop and Reformat
  - files: `Engine/Nodes/Image/ExtentKnobs.{h,cpp}`, `Engine/Nodes/Image/NativeGenerator.cpp`, `Engine/Nodes/Transform/Reformat.cpp`, `Tests/Native/NativeConstant_Test.cpp`
  - approach:
    - Fill the format choice from the project's format list. This deletes the copied openfx-misc table and its "HD_720 1280x1720" typo, in both copies.
    - The "Default" extent is the full project format, including its offset.
    - A generator's `frameRange` defaults to the project range.
    - M95 removes `hidpi`/`interactive` from the same file. Whichever lands second rebases.
  - verify: a test with a project format at an offset gets a matching default extent. The format list equals the project's. A new Constant spans the project range.
  - size: M

## Phase 108.5: Close-out

- [ ] M112.P5.T1 — UAT
  - files: none (user acceptance)
  - approach: hand the user a build and a checklist:
    - a default Grade, ColorCorrect and Saturation pass ACEScg negatives and values above 1 untouched
    - ColorCorrect and ColorLookup curves above 1
    - Blur, EdgeDetect and ErodeDilate don't change when the viewer region or mask changes
    - Reformat on a variable-format input
    - Crop larger than the source
    - the format lists match the project
  - verify: the user signs off. Record the findings under `## Decisions`.
  - size: S

**Verification gate:**
- One build. Full ctest green in both scheduler modes, and CI green.
- Every re-recorded fixture is listed under `## Decisions` with the reason.
- The user signs off the UAT.
