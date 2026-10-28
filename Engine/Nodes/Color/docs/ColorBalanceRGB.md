# ColorBalanceRGB

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ColorBalanceRGB | darktable reference | `kReferenceMean` 2e-4, `kReferenceMax` 5e-3 | mean and largest absolute error over the crop, relative to the reference's mean level, each sample's beyond the half float reference's rounding (`RawReference::halfFloatError`), against darktable-cli renders (`NativeColorBalanceRGB_Test.cpp`, `MatchesDarktable`). Provisional until the references are rendered |
| ColorBalanceRGB | identity | 2e-4 relative | defaults on low-chroma colours, both formulas (`DefaultsKeepLowChromaColours`) |
| ColorBalanceRGB | gpu | `ParityTolerance::make(0., 1e-4)`, JzAzBz `1e-3` | kernel against the CPU row, both formulas, every control, mask outputs and presets (`NativeColorBalanceRGBGpu_Test.cpp`). JzAzBz's PQ curve amplifies float rounding: a float round trip drifts up to 2.6e-4 (CPU) and 2.8e-4 (lavapipe) from its input, and the two disagree by up to 3.75e-4 |

The node ports darktable's color balance rgb (`src/iop/colorbalancergb.c` and
`data/kernels/colorbalancergb.cl`, release-5.6.2) with every parameter of
`dt_iop_colorbalancergb_params_t` v5 and darktable's defaults. The colour-space helpers it shares
with other darktable ports live in `Engine/Nodes/Color/DtColorSpaces.h` and
`Engine/Gpu/Kernels/dtcolorspaces.slang`.

## Parameters

| darktable field | Knob | Tab |
|---|---|---|
| hue_angle | hueShift | Master |
| vibrance | vibrance | Master |
| contrast | contrast | Master |
| chroma_global, chroma_shadows, chroma_midtones, chroma_highlights | chromaGlobal, chromaShadows, chromaMidtones, chromaHighlights | Master |
| saturation_global, saturation_shadows, saturation_midtones, saturation_highlights | saturationGlobal, saturationShadows, saturationMidtones, saturationHighlights | Master |
| brilliance_global, brilliance_shadows, brilliance_midtones, brilliance_highlights | brillianceGlobal, brillianceShadows, brillianceMidtones, brillianceHighlights | Master |
| global_Y, global_H, global_C | offsetLuminance, offsetHue, offsetChroma | 4 Ways |
| shadows_Y, shadows_H, shadows_C | liftLuminance, liftHue, liftChroma | 4 Ways |
| highlights_Y, highlights_H, highlights_C | gainLuminance, gainHue, gainChroma | 4 Ways |
| midtones_Y, midtones_H, midtones_C | powerLuminance, powerHue, powerChroma | 4 Ways |
| saturation_formula | saturationFormula (default darktable UCS) | Masks |
| shadows_weight | shadowsWeight | Masks |
| mask_grey_fulcrum | maskGreyFulcrum | Masks |
| highlights_weight | highlightsWeight | Masks |
| white_fulcrum | whiteFulcrum (EV) | Masks |
| grey_fulcrum | contrastGreyFulcrum | Masks |
| (GUI mask preview) | output: Result, Shadows Mask, Midtones Mask, Highlights Mask | Masks |
| (presets) | preset and applyPreset | top |

Values are darktable's internal ones: darktable's GUI shows most sliders as percentages, the
knobs hold the fraction (25 % is 0.25). The presets are darktable's four "basic colorfulness"
presets; Apply writes every control, resetting the ones a preset does not name.

## Differences from darktable

- **Fixed grading space.** The node converts the working space to linear Rec.2020, darktable's
  pipeline space, grades, and converts back. A matrix working space uses a 3x3 matrix; any other
  working space goes through OCIO via XYZ D65 and renders on the CPU only.
- **Negative and out-of-gamut input.** As in darktable, negative Rec.2020 values are clipped to 0
  before grading and the output is clipped at 0; the saturation is gamut-mapped against the
  Rec.2020 boundary. Even at its defaults the node is therefore not an identity for negative or
  highly saturated colours, and it never reports itself as one.
- **4 Ways as wheels.** As in darktable, the four wheels sit two to a row: Global Offset and
  Shadows Lift, then Highlights Gain and Power. Each has its title above, its luminance slider
  beneath and a readout of hue, chroma and luminance. On a wheel, angle is hue and distance from
  the centre is chroma; the outer edge is the chroma knob's soft maximum, and scrolling zooms it.
  Drag the ring to change the hue alone, and double-click to reset the chroma. A drag is one undo
  step. To type values, open the tab's collapsed Values group, which holds the twelve knobs.
- **Not ported:** the colour pickers (white fulcrum, grey fulcrum, neutralize), the colour checker
  and the GUI-only collapsible sections. Mask previews are the Output choice.
- **Alpha** passes through. Premult, mask and mix are the host's stages.

## GPU

The kernel (`Engine/Gpu/Kernels/colorbalancergb.slang`) follows the CPU statement for statement
and reads the same gamut LUT. It runs only in a matrix working space; an OCIO working space keeps
the node on the CPU. Its JzAzBz conversions use their own pow(), since a Vulkan pow() may carry a log2
error that the PQ exponent of 134 amplifies 134-fold.
