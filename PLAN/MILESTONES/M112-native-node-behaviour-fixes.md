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

- [x] M112.P1.T1 — Turn off the clamps by default and make default nodes identity
  - files: `Engine/Nodes/Color/Grade.cpp`, `ColorCorrect.cpp`, `Saturation.cpp`, `Engine/Gpu/Kernels/gradeops.slang`, `Tests/Native/NativeGrade_Test.cpp`, `NativeColorCorrect_Test.cpp`, `NativeSaturation_Test.cpp`, `NativeGradeGpu_Test.cpp`, `NativeColorCorrectGpu_Test.cpp`, `NativeSaturationGpu_Test.cpp`, `Tests/Gpu/GradeFixture.h`, `Engine/Nodes/README.md`
  - approach:
    - `clampBlack`/`clampWhite` default off (`Grade.cpp:327`, `ColorCorrect.cpp:555`, `Saturation.cpp:214`), with the `readBool(..., true)` fallbacks to match (`Grade.cpp` ~:425, `ColorCorrect.cpp:610,655`, `Saturation.cpp:243,300`); `isIdentityOp` is true at defaults.
    - Clamp gamma to a small positive minimum on the host before `p.gamma`/`p.invGamma` are packed (`Grade.cpp:402-403`); delete the gamma ≤ 0 step branches in CPU `grade()` (`:132-139`) and in `gradeops.slang:8-14, :28`, keeping CPU and GPU bit-matched.
    - Parity fixtures are OFX replays (`Tests/NativeParity.h`); regenerate affected `.f32` files from the native node or turn the cases into analytic asserts; list each under `## Decisions`. Add GPU cases at default knobs and at gamma ≤ 0.
  - verify: default nodes are identity; negatives and values above 1 pass through on CPU and GPU; the Grade/ColorCorrect/Saturation tests and their `*Gpu_Test`s pass.
  - size: L
- [x] M112.P1.T2 — Separate ColorCorrect's tone weights from the output clamps, guard ColorLookup's triangle, and drop the LUT range knobs
  - files: `Engine/Nodes/Color/ColorCorrect.cpp`, `ColorLookup.cpp`, `Engine/Nodes/Image/CurveSnapshot.h`, `Engine/Gpu/Kernels/colorcorrect.slang`, `colorlookup.slang`, `curvesnapshot.slang`, `Tests/Native/NativeColorCorrect_Test.cpp`, `NativeColorLookup_Test.cpp`, `NativeColorCorrectGpu_Test.cpp`, `NativeColorLookupGpu_Test.cpp`, `Engine/Nodes/README.md`
  - approach:
    - Curves are already evaluated directly outside `range` (`ColorLookup.cpp:263-270`, `ColorCorrect.cpp:285-291`); the defects are the weights and `triangle()`.
    - Tone weights always clamp to [0,1] through their own helper, in the CPU LUT build (`:158`), the direct path (`:291`) and `colorcorrect.slang:85`; the output clamp applies only to the final channels.
    - Delete the `range` knob (`ColorCorrect.cpp:505-520`, `ColorLookup.cpp:565-579`): size the table from fixed internal bounds and evaluate directly outside them (or everywhere). Keep `kGpuTableRange`/`CurveSnapshot::packGpuTableRange` consistent; update the README parity paragraphs (~:840, :854).
    - Guard `triangle()` (`ColorLookup.cpp:302-319`, `colorlookup.slang:82-96`) for `a >= 1`; both return the same finite value.
  - verify: values above 1 and below 0; weights in [0,1] with the clamps off; weighted standard at a = 1 and a = 2 gives no inf/NaN; GPU parity at the same points.
  - size: M
- [x] M112.P1.T3 — Fix Clamp's description typo and the Rec.709 comment
  - files: `Engine/Nodes/Color/Clamp.cpp`, `Engine/Nodes/Image/ColorMath.h`
  - approach: `Clamp.cpp:260` "MaxClampTop" → "MaxClampTo". Reword `ColorMath.h:150` to say the Rec.709 helpers deliberately use the BT.2020 constants (1.0993, 0.0181) and that only ChromaKeyer uses them. No constant change (that would change ChromaKeyer CPU and GPU output; it belongs to M95).
  - verify: review the diff; no pixel output changes.
  - size: S
## Phase 108.2: Spatial filters

- [x] M112.P2.T1 — Filter the render window plus halo, never shrunk by the mask
  - files: `Engine/Nodes/Filter/SpatialFilterSupport.{h,cpp}`, `Blur.{h,cpp}`, `EdgeDetect.cpp`, `ErodeDilate.{h,cpp}`, `Engine/Gpu/GpuMorphology.{h,cpp}`, `Engine/Gpu/GpuBlur.h`, `Engine/Gpu/Kernels/morphology.slang`, `Tests/Native/NativeBlur_Test.cpp`, `NativeBlurGpu_Test.cpp`, `NativeErodeDilate_Test.cpp`, `NativeErodeDilateGpu_Test.cpp`, `NativeEdgeDetect_Test.cpp`, `Engine/Nodes/README.md` (GPU parity ~:832-835, :858)
  - approach:
    - Delete `shrinkToMask` (`SpatialFilterSupport.cpp:358`), `maskRowIsZero`, `maskColumnIsZero`; `processWindow = roi`, empty only when mix == 0; mask and mix apply in `writeWindow` only. Compatible with M90's fast path (`.h:274`), which masked renders don't take.
    - GPU: `recordFilterWindow` (`GpuMorphology.cpp:123`; callers `Blur.cpp:1091`, `GpuMorphology.cpp:212`) loses its mask argument and the `morph_mask_bbox`/`keptByMask` pass (`morphology.slang:394-460`) goes; ErodeDilate's shrink (`ErodeDilate.cpp:750-785`) goes. M85's `recordGpuFinish` already applies mask/mix after the filter. Update the doc comments at `Blur.h:160-163`, `ErodeDilate.h:157-161`.
    - Keep `writeWindow` as the shared CPU helper (the base class has only the GPU finish).
    - IIR Blur depends on the window (halo `max(3, ceil(1.5*size))`, `Blur.cpp:385`); state that bound in the README.
  - verify: one pixel through two render windows and two masks that are both 1 there is identical for FIR Blur, ErodeDilate and EdgeDetect, and within the stated tolerance for IIR; GPU/CPU parity with a mask unchanged.
  - size: L
- [x] M112.P2.T2 — Fix EdgeDetect's copied normalisation and RMS bugs
  - files: `Engine/Nodes/Filter/EdgeDetect.cpp`, `Tests/Native/NativeEdgeDetect_Test.cpp`
  - approach: normalise the Gaussian by sigma, and make the RMS sign alignment do what its code intends, or delete it if the result is equivalent. Re-record the fixtures.
  - verify: an analytic test with a step edge of known height gives the expected gradient magnitude for each operator.
  - size: M

- [x] M112.P2.T3 — Trim Blur's knobs to a Nuke-like set
  - files: `Engine/Nodes/Filter/Blur.{h,cpp}`, `BlurKernels.{h,cpp}`, `Engine/Gpu/GpuBlur.{h,cpp}`, `Engine/Gpu/Kernels/blur.slang`, `blur_lines.slang`, `Tests/Native/NativeBlur_Test.cpp`, `BlurFIRGaussian_Test.cpp`, `BlurKernels_Test.cpp`, `NativeBlurGpu_Test.cpp`, `PyPlugNativeParams_Test.cpp`, `Gui/Resources/PyPlugs/{DropShadow,Glow,AngleBlur,LightWrap,EdgeBlur,PIKColor}.py`, `Engine/Nodes/README.md`
  - approach: keep size, filter and crop to format. Delete `orderX/orderY`, `alphaThreshold`, `boundary` (black outside), `expandRoD` (always expand) and the hidden `uniform` (`Blur.h:43-59`), and the matching GPU paths (`neumann` in `GpuBlur.h:49,124,199` and the kernels, `alphaThreshold` in the finish ~`Blur.cpp:1006-1019`, the derivative-order CPU fallback). Drop the CImg-rounding claims in `BlurKernels.cpp`. Fix every PyPlug line that sets a removed knob.
  - verify: Blur tests and `NativeBlurGpu_Test` pass per filter; the PyPlug audit is green; the deleted knobs are gone from the panel.
  - size: M
## Phase 108.3: Transform family

- [x] M112.P3.T1 — Fix Reformat's frame and edge handling
  - files: `Engine/Nodes/Transform/Reformat.cpp`, `Tests/Native/NativeReformat_Test.cpp`
  - approach:
    - Take the source format from the input's metadata at the render or metadata time, not the timeline's current time.
    - `getPreferredMetadata` evaluates at the metadata time, not 0.
    - Clamp filter edges at the real image bounds, not at the requested window.
  - verify: tests check an input whose format changes over time, rendered at a frame other than the current one; an animated box size past frame 0; and identical edge pixels for two request windows.
  - size: M

- [x] M112.P3.T2 — Crop reads black outside its source; Transform/Reformat disable `clamp` where it does nothing
  - files: `Engine/Nodes/Transform/Crop.cpp`, `Transform.cpp`, `Reformat.cpp`, `Engine/Nodes/Image/Resampler.{h,cpp}`, `Tests/Native/NativeCrop_Test.cpp`, `NativeTransform_Test.cpp`, `NativeReformat_Test.cpp`, `Resampler_Test.cpp`
  - approach: Crop reads black, not the nearest edge pixel, outside the source (`Crop.cpp:153-190`). Disable the `clamp` knob when `filterHonoursClamp(filter)` (`Resampler.h:129`) is false, so Cubic output is unchanged on CPU and GPU. A Transform or Reformat that does no resampling is identity whatever `clamp` says (`Transform.cpp:1028-1030`, `Reformat.cpp:659-664`).
  - verify: a crop box larger than the source; a no-op Transform and Reformat with `clamp` on is identity; `clamp` is disabled for Cubic.
  - size: M
## Phase 108.4: Generators

- [x] M112.P4.T1 — Use the project's formats and range in generators, Crop and Reformat
  - files: `Engine/Nodes/Image/ExtentKnobs.{h,cpp}`, `Engine/Nodes/Image/NativeGenerator.cpp`, `Engine/Nodes/Transform/Reformat.cpp`, `Tests/Native/NativeConstant_Test.cpp`
  - approach:
    - Fill the format choice from the project's format list. This deletes the copied openfx-misc table and its "HD_720 1280x1720" typo, in both copies.
    - The "Default" extent is the full project format, including its offset.
    - A generator's `frameRange` defaults to the project range.
    - M95 removes `hidpi`/`interactive` from the same file. Whichever lands second rebases.
  - verify: a test with a project format at an offset gets a matching default extent. The format list equals the project's. A new Constant spans the project range.
  - size: M

## Phase 108.5: Close-out

- [x] M112.P5.T2 — Point the M76 Write GUI script at the tabtabtab node menu
  - files: `Tests/gui/m76_write.py`
  - approach: lines ~181/183 still look up `gt.widgets_of("NodeCreationDialog")`, which M45 - Native Tab Menu removed (the Tab menu is `TabTabTabWidget`, the completer `CompleterLineEdit`). Drive `TabTabTabWidget` the way `Tests/gui/m75_read.py` (~428–430) does.
  - verify: `Tests/gui/run-gui-test.sh Tests/gui/m76_write.py` passes under Xvfb.
  - size: M
- [ ] M112.P5.T1 — UAT
  - files: none (user acceptance)
  - approach: hand the user a build and a checklist:
    - a default Grade, ColorCorrect and Saturation pass ACEScg negatives and values above 1 untouched
    - ColorCorrect and ColorLookup curves above 1
    - Blur, EdgeDetect and ErodeDilate don't change when the viewer region or mask changes
    - Reformat on a variable-format input
    - Crop larger than the source
    - the format lists match the project
    - CPU and GPU agree for the changed nodes, including Blur with a mask on the GPU
    - a bundled PyPlug that uses Blur (Glow, DropShadow) still loads after the knob trim
  - verify: the user signs off (on a `package.sh` AppImage). Record the findings under `## Decisions`.
  - size: S

**Verification gate:**
- One build. Full ctest green in both scheduler modes, and CI green.
- Every re-recorded fixture is listed under `## Decisions` with the reason.
- The user signs off the UAT.

## Decisions

- 2026-10-16 — **Freshness check at claim**: line numbers re-located on main after M85 and M90. P1.T1, P1.T2, P1.T3, P2.T1, P2.T3 and P3.T2 rewritten in place, mainly to add the GPU twins (kernels, GPU tests and README GPU-parity text) that must change with any CPU output change. P1.T1 re-sized M→L. Cross-cutting: native parity fixtures are OFX replays that can no longer be re-recorded from OFX, so every changed case is regenerated from the native node or turned into an analytic assertion, and listed here.
- 2026-10-16 — **P1.T3 is comment-only** (PM): the Rec.709 helpers' BT.2020 constants are used only by ChromaKeyer, which belongs to M95 and has a GPU twin.
- 2026-10-16 — **P3.T2 disables `clamp` where the filter ignores it** (PM): Cubic output stays the same on CPU and GPU, rather than changing Cubic and its GPU resampler.
- 2026-10-16 — **Claimed with a trivial overlap** (user rule for trivial overlaps): `Tests/gui/m76_write.py` (P5.T2) is also edited by M100 - Content Node Hashing; resolved at rebase.
- 2026-10-16 — **P1.T1 implemented** (code on milestone/m112-native-node-behaviour-fixes, unbuilt). Gamma is floored at 0.01 on the host: at 0.001 the GPU's float rounding, magnified by 1/gamma, exceeds Grade's 1e-5 parity bound. The Grade GPU bound gains a 1/gamma ulp term (checked against a float model over every fixture case). **Fixtures:** none regenerated; the OFX-recorded cases now set Clamp Black on explicitly (Grade multiply-offset and mask-mix, ColorCorrect tone-ranges, Saturation saturation-0, saturation-2.5, luminance-rec2020 and mask-mix). **Build risk:** `ColorViewsRender_Test` `DefaultGradeOverAlphaKeepsAlphaExactly` now goes through the identity path.
- 2026-10-16 — **All implementation tasks green** at 5718f330f (remote fast run 38906738996, 2338/2338 in legacy and taskgraph). Code: P1.T1 e6b017dc4, P1.T2 822f098ef, P1.T3 e6d5cec51, P2.T1 6e9611c7e, P2.T2 0a2b17608, P2.T3 d98c3d69b, P3.T1 9bb474d97, P3.T2 f5d5312c1, P4.T1 76d1aa58e, P5.T2 0fbac2cb3; build fixes 31b51dee0, 258cfc4cc, 5718f330f. **Fixtures changed:**
  - deleted EdgeDetect parity replays `gaussian-separate-erode2-nms`, `gaussian-tensor-3` and `mask-mix` (and their `.mip1`): OFX output with the 1/2.4 normalisation and the mask-shrunk window, both now fixed; covered by the analytic step-edge tests;
  - deleted ColorLookup `luminance-range`: the `range` knob is gone; luminance mode is asserted analytically;
  - `Tests/Gpu/ref/GradeRef.h`: the gamma ≤ 0 step branch is gone;
  - golden projects `cg_multipass`, `defocus_retime`, `keying_grade` and `synthetic-generator-grade-write`: the trimmed Blur knobs and the project-list formats change what is saved.
  
  The Blur render tests moved to a box filter, because the window-dependent IIR filter no longer matches a mask-shrunk OFX replay.
- 2026-10-16 — **Review round (Codex, PR #66)**: 7 findings, all fixed in d747cfdb4 — clamp greying now follows an animated filter in Reformat and Transform; the alpha-threshold GPU path (`recordGpuFinish`, `GpuWrap`, `pixelwrap.slang`) and the `writeWindow` output callback and `alphaPlane` left over from the Blur trim are removed; four restating comments are deleted.
