# M214 - Colour Balance RGB

A native `ColorBalanceRGB` node ported from darktable's `src/iop/colorbalancergb.c` (about 2.1k lines, with OpenCL) at `DARKTABLE_REF`. It takes unbounded linear RGB in.

What it offers:
- **4-ways:** global offset, shadows lift, highlights gain and midtones power, each as luminance, hue and chroma.
- **Linear chroma grading.**
- **Perceptual saturation and brilliance grading:** global, shadows, midtones and highlights, in darktable UCS or JzAzBz.
- **Global vibrance, contrast around a grey fulcrum, and a hue shift.**
- **Luminance masks:** shadows and highlights weights, plus the white and grey fulcrums.

Its unique value next to Grade and ColorCorrect is the perceptual saturation and brilliance tools. The colour-space helpers it needs (darktable UCS, JzAzBz, and the Filmlight Yrg-style LMS conversions) go in a shared header that M215 - Colour Equalizer reuses. The 4-ways use M44 - Trackball Colour Editing's trackball widget in a hue/chroma mode where it fits.

Every user-facing control and darktable default is ported (`port-means-every-control`).

Deps: none. See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`ColorBalanceRGB`** in `Engine/Nodes/Color/`, plain ID, major 1. Inputs: Source and an optional Mask (the host MaskMix).
- **Every control** of the module's newest params version and GUI:
  - master: `hueShift`, `vibrance`, `contrast`, and linear chroma global/shadows/midtones/highlights
  - perceptual saturation and brilliance, global/shadows/midtones/highlights
  - 4-ways: global offset, shadows lift, highlights gain and power, each as luminance, hue and chroma
  - masks: `saturationFormula` (JzAzBz, darktable UCS), `whiteFulcrum`, `contrastGreyFulcrum`, `shadowsWeight`, `highlightsWeight` and `maskGreyFulcrum`
  - darktable's presets ("basic colorfulness" and others) as Natron presets
- **`output`:** Result, Shadows Mask, Midtones Mask or Highlights Mask. This replaces darktable's darkroom mask checker.
- **Shared `Engine/Nodes/Color/DtColorSpaces.h`:** darktable UCS 22, JzAzBz, the XYZ↔LMS conversions and the gamut helpers, ported from `src/common/colorspaces_inline_conversions.h` and `chromatic_adaptation.h`.
- **Working-space handling:** the node converts the project working space to linear Rec.2020 (darktable's pipeline space) through OCIO and back.
- CPU and GPU paths, darktable reference parity, a params-to-knob audit, a doc, an icon.

**Out:**
- Display-referred concerns, such as darktable's gamut mapping to the display at export: none. The node's own gamut handling on output is in scope, because it is part of the module.

**Edge cases:**
- **Negative and out-of-gamut input:** handled the way darktable's code handles it, and documented.
- **Trackball fit:** if M44's widget cannot express separate luminance, hue and chroma for a wheel, the 4-ways fall back to sliders laid out per wheel, and the decision is recorded.
- **Alpha:** passes through untouched.

## Phase 214.1: Node

- [ ] M214.P1.T1 — Shared darktable colour-space helpers
  - files: new `Engine/Nodes/Color/DtColorSpaces.h`, new `Tests/Native/DtColorSpaces_Test.cpp`
  - approach: port the inline conversions the module uses (XYZ↔JzAzBz, darktable UCS 22 forward and inverse, Rec.2020↔XYZ↔LMS) as constexpr inline functions shared by CPU code, plus a Slang twin `Engine/Gpu/Kernels/dtcolorspaces.slang` for the GPU kernels.
  - verify: `ctest -R DtColorSpaces` round-trips each forward and inverse pair within 1e-5, and matches darktable's published test values where they exist.
  - size: M

- [ ] M214.P1.T2 — ColorBalanceRGB node (CPU)
  - files: new `Engine/Nodes/Color/ColorBalanceRGB.h`, `Engine/Nodes/Color/ColorBalanceRGB.cpp`, `Engine/Nodes/Color/ColorBalanceRGB.reg.inc`
  - approach:
    - Port `process` and `commit_params` (the derived matrices and mask weights) from `colorbalancergb.c`, on `DtColorSpaces.h`.
    - Add every knob, preset and `output` mode.
    - Do the OCIO conversion between the working space and Rec.2020 per row with `CPUProcessor`, as M127 - Native OCIO Nodes plans to.
  - verify: P1.T5's reference jobs pass.
  - size: L

- [ ] M214.P1.T3 — 4-ways on the trackball widget
  - files: `Engine/Nodes/Color/ColorBalanceRGB.cpp`, `Gui/` trackball files from M44 (read `PLAN/MILESTONES/M44-trackball-colour-editing.md` and M119's panel layout first)
  - approach: bind each wheel's hue and chroma to a trackball in polar mode, and the luminance to its slider. If the widget has no polar mode, add one when it takes a few lines. Otherwise fall back per the Scope's edge case.
  - verify: a gtest sets a wheel's trackball value through its knob API and checks the hue and chroma knobs. The gate's Xvfb screenshot shows the four wheels.
  - size: M

- [ ] M214.P1.T4 — GPU path
  - files: new `Engine/Gpu/Kernels/colorbalancergb.slang`, `colorbalancergb.cmake`, `Engine/Nodes/Color/ColorBalanceRGB.cpp`, new `Tests/Native/NativeColorBalanceRGBGpu_Test.cpp`
  - approach: a point op from darktable's `data/kernels/colorbalancergb.cl`, with the OCIO conversion as a matrix when the working space is a matrix space. Otherwise the node runs CPU-only.
  - verify: `ctest -R NativeColorBalanceRGBGpu` passes on lavapipe within 1e-4 relative.
  - size: M

- [ ] M214.P1.T5 — Reference parity, knob audit and docs
  - files: `tools/raw/make-reference.py`, `Tests/fixtures/raw/reference/` (new), new `Tests/Native/NativeColorBalanceRGB_Test.cpp`, `Engine/Nodes/Color/docs/ColorBalanceRGB.md`
  - approach:
    - darktable-cli jobs: each preset, each 4-way, saturation and brilliance per range under both formulas, contrast and vibrance.
    - Audit `dt_iop_colorbalancergb_params_t` against the knobs.
    - Run identity checks, then write the doc and add an icon (`Gui/Resources/Images/NativeNodes/`).
  - verify: `ctest -R NativeColorBalanceRGB` passes within the doc's `Tolerance:`.
  - size: L

**Verification gate:**
- The full build and ctest pass, including both ColorBalanceRGB suites on lavapipe and RADV.
- Under Xvfb, the panel with its four wheels and the mask outputs is captured, and a grade on a plate is compared with darktable's render. Screenshots are shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.
