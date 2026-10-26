# M122 - Native Colour Utilities

Native, redesigned replacements for the remaining openfx-misc colour point ops:
- `Premult` and `Unpremult`
- one `Colorspace` node in place of the 18 `ColorTransform` converters
- `ColorMatrix`
- a scene-linear `HSVTool`
- `HueCorrect`
- a clean-sheet trapezoid `HueKeyer`
- a clean-sheet `Despill` that absorbs ColorSuppress
- `Threshold`
- `Posterize` in place of Quantize

Log2Lin and PLogLin are retired without a native replacement; the OCIO nodes cover log conversion. Every node is a point op with a CPU kernel, a GPU kernel and tests, so a colour chain stays inside one resident GPU region.

Shared conventions:
- Plain IDs at major 1, Nuke-like knob sets and scene-linear maths, with clamps off by default. The nodes don't copy OFX behaviour (`DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md`), and there is no project compatibility (`DECISIONS/2026-09-18-clean-break-no-project-compat.md`).
- Mask, mix and "(Un)premult by" come from the `NativeImageEffect` base.
- Registration, kernels, cost seeds, icons, docs and retirement go through M120 - Native Node Port Kit's per-node fragment files (`Engine/Nodes/Color/<Node>.{reg.inc,costseed.inc}`, `docs/<Node>.md`; `Keyer` for HueKeyer and Despill), and one `Engine/OfxRetired/<ofx-plugin-id>` marker per retired ID.
- Each node gets `Tests/Native/Native<Node>_Test.cpp` and `Tests/Native/Native<Node>Gpu_Test.cpp`.

Deps: M120 - Native Node Port Kit. Through it, this also waits on M85 - Native GPU Kernels for `PixelKernelGpu`, `makeGpuKernel` and `pixelwrap.slang`.

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion). OFX paths are under `openfx-misc/`:
- **Templates:**
  - `Engine/Nodes/Color/Saturation.cpp` is the point-op template: traits around `:170` (`hostUnPremult`, `defaultChannels[3] = false`), the kernel class at `:40-100`, `makeKernel` at `:205`, `isIdentityOp` at `:221`.
  - Mask and mix come from `addMaskMixKnobs(page)`.
  - Grade's colour knobs: `addGradeColorKnob` (`Grade.cpp:322-328`).
- **Curves:** `ColorLookup.cpp:524` creates a `KnobParametric`, control points are added at `:541`, and a `CurveSnapshot` is taken per curve at `:670-673`. `CurveSnapshot.h:69` has `valueAt`. ColorCorrect's two curves are at `:438, :513`.
- **Maths helpers:** `Engine/Nodes/Image/ColorMath.h` has:
  - `luminance()` (`:93`), `addLuminanceMathKnob()` (`:124`)
  - `acesCgLumaWeights()` (`:73`), `acesCgToXYZ()` (`:81`)
  - transfer helpers (`:153-202`)
  - Project working-space access is through `ProjectColorManagement::getWorkingLuma` (`Engine/ProjectColorManagement.h:114-169`).
- **GPU (M85 - Native GPU Kernels branch):**
  - `PixelKernel.h:141` has `struct PixelKernelGpu { int kernel; std::vector<std::byte> params; setParams<P>() }`, and `NativeImageEffect.h:214` has `makeGpuKernel`.
  - `Saturation.cpp:268` and `Engine/Gpu/Kernels/saturation.slang` (`IPixelOp` `load`/`apply`, `runWrapped<>`) are the templates.
  - `ColorLookup::makeGpuKernel` (`:873`) packs a LUT into uint words, which is the template for the curve nodes.
- **OFX Premult/Unpremult** (`Premult/Premult.cpp:53,63`):
  - processRGB on, A off, `premultChannel` default A.
  - Unpremult skips `alpha <= FLT_EPSILON`, and both flip the output premult state (`:766`).
  - Users: `DropShadow.py:245`, `PIKColor.py:420,432,551,612,949`, `LightWrap.py:1092,1187`, `Fill.py:158,229`, all pinned at major 2.
  - `Engine/OfxEffectInstance.cpp:677-678` is an `inputPlane` take-over table that goes dead once these are retired.
- **OFX converters** (`ColorTransform/ColorTransform.cpp:35-140`):
  - RGB↔HSV/HSL/HSI/YCbCr/YPbPr/YUV (601/709), RGB709↔XYZ/Lab, XYZ↔Lab/xyY.
  - Lab uses the old 0.412453 sRGB matrix and is divided by 100 (`:429-455`).
  - HSV is in `SupportExt/ofxsLut.cpp:131`.
- **OFX HSVTool** (`HSVTool/HSVTool.cpp`, maths `:519-610`):
  - Hue, saturation and brightness ranges with rolloffs, plus adjustments with gains.
  - brightnessRange defaults to 0..1 and saturation is clamped to 0..1, so V>1 falls out of the default range.
  - clampBlack is on by default. `outputAlpha` defaults to Hue.
  - Used by `Glow.py:1167`.
- **OFX HueCorrect** (`HueCorrect/HueCorrect.cpp:340-420`): 10 hue curves (sat, lum, r, g, b, r_sup, g_sup, b_sup, sat_thrsh, hue), plus `luminanceMath` and `mixLuminance`. It is mostly scale-invariant.
- **OFX HueKeyer**: amount and sat_thrsh curves (`:1158+`). It is replaced by a simpler design, below.
- **OFX ColorMatrix**: four RGBA rows, with clampBlack on by default.
- **OFX ColorSuppress** (`ColorSuppress/ColorSuppress.cpp:285-415`) does an ordered Y, M, C, R, G, B suppress.
- **OFX Despill** (`Despill/Despill.cpp:270-300`): `spill = max(G − (R·mix + B·(1−mix))·(1−expand), 0)`, then per-channel scale plus brightness. clampBlack is on. It sits in `tools/bench/comps/keying_grade.ntp` and in the cost seeds.
- **OFX Threshold** (`Threshold.cpp:248`): at softness 0, `v == level` gives 0, which contradicts its own description.
- **OFX Quantize** (`:417`): `ceil(v·N − 1)/(N − 1)`, with dither (None, Bayer 2/4/8, void-and-cluster 14/25, Random) and a seed.
- **Tests referencing these IDs:**
  - M120 - Native Node Port Kit moves the generic fixtures.
  - Left here: any test that checks the node itself, and `tools/bench/comps/build_comps.py:189` and `keying_grade.ntp:1948` (Despill).

## Scope

**In:**
- The nine native node types above.
- GPU kernels and tests for each.
- PyPlug, bench-comp and engine reference updates.
- Retiring `Premult`, `Unpremult`, `HSVToolPlugin`, `HueCorrect`, `HueKeyer`, `ColorMatrixPlugin`, `ColorSuppress`, `Despill`, `Threshold`, `Quantize`, `Log2Lin`, `PLogLin` and every `ColorTransform` converter.

**Out:**
- Log conversion. Log2Lin and PLogLin are retired, and the OCIO nodes milestone's OCIOColorSpace and its LogConvert preset cover it.
- The Glow PyPlug's rewrite: the Sharpen/Glow filter milestone. This milestone only keeps HSVTool working for it.
- Renaming existing colour nodes and the `OfxEffectInstance` dead-table cleanup: M111 - Native Node Identity Cleanup. The table is trivially deleted here only if nothing else uses it.
- Keyer and ChromaKeyer: M95 - Knob Cleanup And Keyer Redesign.

**Edge cases:**
- Unpremult where alpha ≤ FLT_EPSILON passes colour through unchanged, as the base's divisor rule does (`Image::unPremultDivisorIsUsable`).
- Colorspace HSV/HSL on negative or >1 values stays defined, with no NaN. Hue is undefined at zero chroma and set to 0.
- Lab and xyY take the working space's white point. At Y = 0, xyY outputs the white point's xy.
- HSVTool with V > 1 is selected by the default range, which is open-ended.
- Despill with the screen colour set to grey has no spill axis, so it is identity.
- Posterize with levels < 2 is identity. Values outside the range keep stepping.
- Threshold at softness 0: `v ≥ level` gives 1.

**Assumptions:**
- Unconfirmed: the Colorspace models are RGB, HSV, HSL, HSI, YCbCr/YPbPr/YUV (601/709/2020), XYZ, Lab and xyY, with RGB primaries from the project working space.
- Unconfirmed: HueCorrect keeps all ten OFX curves but drops `HueCorrect1` and the clamps.
- Unconfirmed: Despill's default limit is the average of the two non-screen channels (Nuke's default), with luma restore off by default.
- Unconfirmed: Posterize dither modes are None, Ordered (Bayer 8), Blue noise and Random, with a seed and a static-seed toggle.
- Unconfirmed: GPU tolerance is 1e-6 absolute for the matrix/HSV nodes, and one LUT step for the curve nodes.

## Phase 121.1: Premult, Unpremult, ColorMatrix, Threshold

- [x] M122.P1.T1 — Native Premult and Unpremult
  - files: `Engine/Nodes/Color/Premult.h`, `Engine/Nodes/Color/Premult.cpp`, `Engine/Nodes/Color/Premult.reg.inc`, `Tests/Native/NativePremult_Test.cpp`
  - approach:
    - Two classes sharing one kernel: multiply or divide the selected channels (default RGB) by an alpha-channel picker (any layer's channel, default A).
    - Divide only where `alpha > FLT_EPSILON`.
    - Each sets the output premult state in metadata: Premultiplied, or Unpremultiplied.
    - `hostUnPremult` is off: these nodes are the premult operation.
  - verify: at the phase build, the test covers known pixels, the epsilon pass-through, the metadata premult flag on output, and an alternate alpha channel.
  - size: M

- [x] M122.P1.T2 — Native ColorMatrix and Threshold
  - files: `Engine/Nodes/Color/ColorMatrix.h`, `Engine/Nodes/Color/ColorMatrix.cpp`, `Engine/Nodes/Color/Threshold.h`, `Engine/Nodes/Color/Threshold.cpp`, `Tests/Native/NativeColorMatrixThreshold_Test.cpp`
  - approach:
    - ColorMatrix: a 4×4 matrix knob set (rows R, G, B, A over r, g, b, a), identity by default, with clamps present but off. It is identity at the identity matrix.
    - Threshold: per-channel `level` and `softness`. Output 0 below `level − softness`, 1 at or above `level + softness`, and a linear ramp between. At softness 0, `v ≥ level` gives 1.
    - Register both in the family list.
  - verify: at the phase build, the test covers matrix maths with HDR values, identity detection, Threshold's boundary at softness 0, and the ramp.
  - size: M

- [x] M122.P1.T3 — GPU kernels for Premult, Unpremult, ColorMatrix and Threshold
  - files: `Engine/Gpu/Kernels/colourutil.slang`, `Engine/Gpu/Kernels/colourutil.cmake`, `Engine/Nodes/Color/{Premult,ColorMatrix,Threshold}.costseed.inc`, `Tests/Native/NativeColourUtilGpu_Test.cpp`
  - approach:
    - One Slang file with four `IPixelOp`s on `runWrapped<>`.
    - `makeGpuKernel` in each node packs its params, as `Saturation.cpp:268` does: matrix 64 B, Threshold 32 B, alpha-channel index.
    - Add cost seeds by analogy with Saturation. Premult and Unpremult share `Premult.costseed.inc`, since they are one class.
  - verify: at the phase build, parity on RADV within 1e-6 absolute for all four, including masked and mixed cases.
  - size: M

## Phase 121.2: Colorspace

- [x] M122.P2.T1 — Native Colorspace node, CPU
  - files: `Engine/Nodes/Color/Colorspace.h`, `Engine/Nodes/Color/Colorspace.cpp`, `Engine/Nodes/Image/ColorMath.h`, `Engine/Nodes/Color/Colorspace.reg.inc`, `Tests/Native/NativeColorspace_Test.cpp`
  - approach:
    - `colorspace_in` and `colorspace_out` model choices. The input model converts to linear RGB, then RGB converts to the output model.
    - The RGB↔XYZ matrices come from the project working space's primaries and white point. Add a helper beside `acesCgToXYZ` that takes the working space from `ProjectColorManagement` and falls back to ACEScg.
    - Lab is unscaled (L 0..100 as standard), with a cube root that handles negatives via the linear segment.
    - Hue is in 0..1 turns.
    - It is identity when in == out.
  - verify: at the phase build, the test checks round-trips for every model within 1e-5 on HDR and negative inputs, the XYZ of a working-space white equals the white point, and Rec.709 primaries map to the expected Lab reference values.
  - size: L

- [x] M122.P2.T2 — Colorspace GPU kernel
  - files: `Engine/Gpu/Kernels/colorspace.slang`, `Engine/Gpu/Kernels/colorspace.cmake`, `Engine/Nodes/Color/Colorspace.cpp`, `Tests/Native/NativeColorspaceGpu_Test.cpp`, `Engine/Nodes/Color/Colorspace.costseed.inc`
  - approach: the same model functions in Slang, selected by two ints. The 3×3 matrices go in params. Keep the code paths identical to the CPU, so there is no fast-math divergence.
  - verify: at the phase build, parity on RADV within 1e-5 absolute for every in/out pair on a test ramp that includes HDR and negatives.
  - size: M

## Phase 121.3: HSVTool, HueCorrect, HueKeyer

- [x] M122.P3.T1 — Native HSVTool, scene-linear
  - files: `Engine/Nodes/Color/HSVTool.h`, `Engine/Nodes/Color/HSVTool.cpp`, `Engine/Nodes/Color/HSVTool.reg.inc`, `Tests/Native/NativeHSVTool_Test.cpp`
  - approach:
    - Hue range with rolloff, saturation range with rolloff, and value range with rolloff. The value range defaults to 0..∞ (upper bound "none"), so HDR values are selected.
    - Saturation is computed as chroma/max, guarded against max ≤ 0 and not clamped.
    - Hue rotation with gain, plus saturation and value adjustment with gains. Clamps exist but are off.
    - Output alpha choice: None, Hue, Saturation, Value or All (the selection mask). This keeps the hue-to-alpha use.
  - verify: at the phase build, the test covers the default being identity, V > 1 selected by the default range, hue rotation wrapping, the rolloff ramp, and the alpha outputs.
  - size: M

- [x] M122.P3.T2 — Native HueCorrect
  - files: `Engine/Nodes/Color/HueCorrect.h`, `Engine/Nodes/Color/HueCorrect.cpp`, `Engine/Nodes/Color/HueCurves.h`, `Engine/Nodes/Color/HueCorrect.reg.inc`, `Tests/Native/NativeHueCorrect_Test.cpp`
  - approach:
    - One `KnobParametric` with the curves (sat, lum, r, g, b, r_sup, g_sup, b_sup, sat_thrsh) over a periodic hue domain, following the ColorLookup pattern.
    - `HueCurves.h` holds the shared hue/periodic-curve sampling, which HueKeyer and the GPU LUT packing reuse.
    - Luminance restore uses `addLuminanceMathKnob`. There are no clamps.
  - verify: at the phase build, the test covers flat curves being identity, a sat curve dip desaturating only that hue, suppress curves, and HDR values passing through scaled, not clipped.
  - size: M

- [x] M122.P3.T3 — Clean-sheet HueKeyer
  - files: `Engine/Nodes/Keyer/HueKeyer.h`, `Engine/Nodes/Keyer/HueKeyer.cpp`, `Engine/Nodes/Keyer/HueKeyer.reg.inc`, `Tests/Native/NativeHueKeyer_Test.cpp`
  - approach:
    - Two four-value trapezoids, in M95 - Knob Cleanup And Keyer Redesign's trapezoid-keyer style:
      - hue (in turns, wrapping across 0/1): `hueLow`, `hueLowFull`, `hueHighFull`, `hueHigh`;
      - saturation: `satLow`, `satLowFull`, `satHighFull`, `satHigh`.
    - alpha = hueKey × satKey (user). Saturation is scene-linear-safe, as in HSVTool.
    - The output is alpha only. RGB passes through, matching M95 - Knob Cleanup And Keyer Redesign's Keyer redesign.
    - An invert option.
  - verify: at the phase build, the test covers the trapezoid edges, hue wrapping across red, the product intersection, and zero saturation giving a satKey of 0 unless satLow ≤ 0.
  - size: M

- [x] M122.P3.T4 — GPU kernels for HSVTool, HueCorrect and HueKeyer
  - files: `Engine/Gpu/Kernels/hue.slang`, `Engine/Gpu/Kernels/hue.cmake`, `Engine/Nodes/Color/HSVTool.cpp`, `Engine/Nodes/Color/HueCorrect.cpp`, `Engine/Nodes/Keyer/HueKeyer.cpp`
  - approach:
    - HSVTool and HueKeyer are analytic `IPixelOp`s.
    - HueCorrect packs its curves into a storage-buffer LUT, as `ColorLookup::makeGpuKernel` (`:873`) does, sampled periodically with linear interpolation.
    - Add cost seeds to `Engine/Nodes/Color/{HSVTool,HueCorrect}.costseed.inc`, `Engine/Nodes/Keyer/HueKeyer.costseed.inc` (or `keyer.inc`).
  - verify: at the phase build, `NativeHueGpu_Test.cpp` (new) on RADV: HSVTool and HueKeyer within 1e-6, HueCorrect within one LUT step of the CPU.
  - size: M

## Phase 121.4: Despill

- [x] M122.P4.T1 — Clean-sheet Despill, CPU
  - files: `Engine/Nodes/Keyer/Despill.h`, `Engine/Nodes/Keyer/Despill.cpp`, `Engine/Nodes/Keyer/Despill.reg.inc`, `Tests/Native/NativeDespill_Test.cpp`
  - approach:
    - Screen colour picker (any colour, default pure green) defines the spill axis: its dominant channel against the other two.
    - Generalise to any hue by rotating into the screen's hue frame and computing spill there, so a custom screen colour or a ColorSuppress-style R/G/B/C/M/Y suppress is the same maths.
    - `limit`: average, max, min or custom weights of the two other channels.
    - `expand`/`contract` offset, and `strength`.
    - The removed spill is replaced with a target colour, default grey/luma-neutral, scaled by the spill amount.
    - `luma restore` amount adds back the removed luminance (working-space luma).
    - The spill map is written to alpha or a chosen target layer.
    - No clamps.
  - verify: at the phase build, the test covers:
    - a green-screen pixel reducing to its limit;
    - blue and custom screen colours;
    - a ColorSuppress-equivalent magenta suppress;
    - luma restore keeping luminance;
    - the spill map output;
    - a grey screen colour being identity;
    - HDR values staying unclipped.
  - size: L

- [x] M122.P4.T2 — Despill GPU kernel and bench comp
  - files: `Engine/Gpu/Kernels/despill.slang`, `Engine/Gpu/Kernels/despill.cmake`, `Engine/Nodes/Keyer/Despill.cpp`, `Tests/Native/NativeDespillGpu_Test.cpp`, `tools/bench/comps/build_comps.py`
  - approach:
    - An `IPixelOp` with the hue-frame rotation matrix precomputed on the host.
    - When the spill map targets another layer, take a multi-plane output, or have `canRenderOnGpu` refuse that case if M109 - GPU Region Engine Hardening's multi-plane regions aren't in. Say which in the commit.
    - Switch `build_comps.py:189` and regenerate `keying_grade.ntp` to the native Despill.
  - verify: at the phase build, parity on RADV within 1e-6. `tools/bench/comps/build_comps.py` regenerates the comps without error.
  - size: M

## Phase 121.5: Posterize

- [x] M122.P5.T1 — Native Posterize with dither, CPU and GPU
  - files: `Engine/Nodes/Color/Posterize.h`, `Engine/Nodes/Color/Posterize.cpp`, `Engine/Gpu/Kernels/posterize.slang`, `Tests/Native/NativePosterize_Test.cpp`, `Tests/Native/NativePosterizeGpu_Test.cpp`
  - approach:
    - `levels` (default 16) over `rangeMin..rangeMax` (default 0..1). Values outside step on at the same spacing.
    - Dither: None, Ordered (8×8 Bayer), Blue noise (a small tiled texture in a storage buffer) or Random (position hash with seed, plus frame unless static).
    - The position hash is shared between CPU and GPU so the output matches.
    - Add its `Posterize.reg.inc`, `posterize.cmake` and `Posterize.costseed.inc` fragments.
  - verify: at the phase build, the CPU test covers step positions, values above range, every dither mode being deterministic per seed, and levels < 2 being identity. GPU parity is bit-exact for None/Ordered/Random and within 1e-6 for blue noise.
  - size: M

## Phase 121.6: Retire and update users

- [x] M122.P6.T1 — Move PyPlugs to native Premult, Unpremult and HSVTool
  - files: `Gui/Resources/PyPlugs/DropShadow.py`, `Gui/Resources/PyPlugs/PIKColor.py`, `Gui/Resources/PyPlugs/LightWrap.py`, `Gui/Resources/PyPlugs/Fill.py`, `Gui/Resources/PyPlugs/Glow.py`
  - approach:
    - Swap `net.sf.openfx.Premult`/`Unpremult` (major 2) and `HSVToolPlugin` (major 1) for the native IDs at major 1, and map any knob names the scripts set to the new names.
    - Glow's HSVTool usage should keep its behaviour. The Glow rewrite itself is M123 - Native Sharpen And Glow's.
  - verify: at the phase build, `PyPlugInstantiate_Test` and `PyPlugNativeParams_Test` pass, and the PyPlugs render on a test plate without errors.
  - size: M

- [x] M122.P6.T2 — Retired markers and per-node docs
  - files: `Engine/OfxRetired/<ofx-plugin-id>` (one per retired ID), `Engine/Nodes/Color/docs/{Premult,ColorMatrix,Threshold,Colorspace,HSVTool,HueCorrect,Posterize}.md`, `Engine/Nodes/Keyer/docs/{HueKeyer,Despill}.md`, `Engine/GpuCostSeeds/ofx/<retired-id>.inc` (deleted, one per retired ID), `Engine/OfxEffectInstance.cpp`
  - approach:
    - Add an `Engine/OfxRetired/<ofx-plugin-id>` marker for every retired ID from `## Scope` In.
    - Document each node, its maths and its GPU tolerance.
    - Delete the Premult/Unpremult take-over rows at `OfxEffectInstance.cpp:677-678` only if nothing else reads that table. Otherwise leave it to M111.
  - verify: at the phase build, `OfxRetired_Test` and `NativePluginList_Test` pass, and `getPluginIDs()` contains no retired ID.
  - size: S

## Phase 121.7: UAT round 1

- [ ] M122.P7.T1 — Premult "all" breaks the node permanently
  - files: `Engine/Nodes/Color/Premult.{h,cpp}` and whatever holds the stuck state
  - approach: user repro: Constant → Shuffle to a second layer → Copy a blurred alpha in → Premult → Unpremult is fine. Set Premult's channels to "all" and the Unpremult gives very bright edges; setting Premult back to rgba, refreshing the viewer and clearing caches doesn't recover. Find the persistent state (a knob, metadata, a premult flag, a layer selection) and fix it.
  - verify: a gtest of the repro: after the round trip all → rgba the output equals a fresh node's; plus the GUI check.
  - size: L

- [ ] M122.P7.T2 — ColorMatrix matrix interface
  - files: ColorMatrix, a matrix knob display in Gui
  - approach: a grid of sliderless float fields (4×4, with a 3×3 mode that hides the alpha row and column and leaves them identity), in place of four RGBA rows.
  - verify: Xvfb screenshot shared with the user; gtests for 3×3 mode keeping alpha untouched.
  - size: L

- [ ] M122.P7.T3 — HueKeyer becomes HSVKeyer
  - files: `Engine/Nodes/Keyer/HSVKeyer.*` (rename), a trapezoid range knob and its Gui widget
  - approach: pick a source colour, which centres the ranges; one trapezoid (outer low, inner low, inner high, outer high) per hue, saturation and value/luma, edited on a custom widget; a toggle for each component's contribution to the alpha; a colour-model choice. Hue ranges wrap.
  - verify: gtests per component and model, plus the wrap; Xvfb screenshots shared with the user.
  - size: L

## Decisions

- 2026-10-26 — **Premult and ColorMatrix (user):** (un)premultiplying never touches the divisor channel, so rgb and rgba selections give the same result; ColorMatrix clampBlack stays off by default; knob labels match their script names, made readable ("Matrix Size", "Matrix").
- 2026-10-26 — **HSVKeyer design (user):** models HSV, HSL and OkLCh (from the working space); picking or editing the source colour recentres each trapezoid on it, keeping its widths, in one undo step, with a Recentre button for animated or expression colours; the hue contribution fades out at low chroma so greys never match a hue range; output modes Alpha / Premultiplied / Matte. Ranges are 4-dimension KnobDoubles shown by a trapezoid range widget (a display hint, not a new knob type), with per-component toggles, multiplied together. Design: `PLAN/DESIGN/2026-10-26-hsvkeyer.md`.
- 2026-10-26 — **UAT round 1 (user):** Premult breaks permanently after its channels are set to "all"; ColorMatrix needs a proper matrix grid of sliderless fields, possibly with 3×3 and 4×4 modes; HueKeyer becomes HSVKeyer with a picked source colour, trapezoid controls (outer/inner/inner/outer) for hue, saturation and luma on custom knobs, per-component alpha toggles, and a colour-model choice.
- 2026-10-25 — **Colorspace premultiplication (user):** Colorspace keeps the standard "(Un)premult by" knob, off by default; with it set, it divides before the conversion and multiplies after, like every other colour node.
- 2026-10-25 — **Despill and Posterize (bosley-native-nodes):** Despill's Expand scales the limit by (1−expand) as OFX did; Replace Color adds only the replacement's chroma in proportion to the spill and Luma Restore adds the removed luminance back as grey; the spill map goes to alpha only, since a staged node cannot write another layer. Posterize ports every Quantize control (all seven dithers, seed, static seed) and adds a Range Min/Max; the plan's blue noise is OFX's void-and-cluster matrices.
- 2026-10-25 — **Phase 121.3 departures (bosley-native-nodes):** HSVTool ports all 8 OFX alpha outputs (the plan listed 5) and HueCorrect keeps clampBlack/clampWhite (default off), because a port means every control. HueKeyer's saturation defaults (0.1/0.3/1/1) leave greys unkeyed. GPU parity uses the transcendental tolerance (1e-5) for HSVTool and HueKeyer, since GPU division error is scaled by chroma and ramp slopes, and one table step for HueCorrect. `net.sf.openfx.HueCorrect1` (OFX's deprecated nine-curve v1) is retired with the others.
- 2026-10-25 — **Premult nodes (bosley-native-nodes):** Premult and Unpremult pick their alpha from any layer through a new `Stage::Divisor` pipeline stage (None makes the node an identity); a divisor from another layer renders on the CPU. The host carries no premultiplication state in metadata, so there is no output premult flag to set or test. Where the divisor has no pixel, the native nodes read 1 (OFX read 0). Threshold at softness 0 maps a value equal to the level to 1, per the plan.
- 2026-10-25 — **Freshness check (bosley-native-nodes):** references re-verified against the base branch; corrections in `PLAN/DESIGN/2026-10-25-freshness-m122-m192-m202-m178-m185-m20.md` override stale paths, lines and APIs in this file.
- 2026-10-13 — Premult and Unpremult stay two separate nodes (user): visible in the graph, the same reasoning as Add/Multiply, and the PyPlugs keep working.
- 2026-10-13 — One `Colorspace` node replaces the 18 converters (user), with primaries from the project working space and Lab unscaled. Its name sits beside the OCIO family's `OCIOColorSpace`, which converts between config spaces.
- 2026-10-13 — HSVTool is redesigned with an unbounded value range and scene-linear-safe saturation, clamps off (user).
- 2026-10-13 — Despill is clean-sheet (user): any screen colour, a target colour, strength, luma restore, a spill map out, limit and expand controls. It absorbs ColorSuppress.
- 2026-10-13 — HueKeyer is a simpler clean-sheet keyer (user): trapezoids on hue and saturation, intersected by multiplication.
- 2026-10-13 — Log2Lin and PLogLin are dropped with no native replacement (user); the OCIO nodes cover log conversion.
- 2026-10-13 — Quantize is ported as `Posterize` (user), with dither kept.

**Verification gate:**
- Full build and ctest pass, including every node's CPU test and the GPU parity tests on RADV.
- A Grade → Colorspace → HSVTool → Despill → Grade chain places as one GPU region (profile label check).
- The bundled PyPlugs that used Premult, Unpremult or HSVTool render.
- Xvfb screenshots of each new node's panel, including HueCorrect's curves and the HueKeyer trapezoids, are shared before sign-off.
- The AppImage is built with `package.sh` for UAT.
- No retired OFX ID loads.
Shipped: phase 6 at 0c1fe3168 (release gate green, run 40239056993).
Review round (Codex, PR #114): Threshold icon and CPU cost seeds fixed at 137eaaafa; release gate green. GPU seeds pending the GPU host; UAT pending.
