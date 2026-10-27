# ColorEqualizer

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ColorEqualizer | darktable reference | `kReferenceMean` 1e-3, `kReferenceMax` 2e-2 | mean and largest absolute error over the crop's interior, a `halo()` in from each edge, relative to the reference's mean level, each sample's beyond the half float reference's rounding (`RawReference::halfFloatError`), against darktable-cli renders (`NativeColorEqualizer_Test.cpp`, `MatchesDarktable`). darktable filters the whole developed image, so the crop's border is not comparable. Provisional until the references are rendered |
| ColorEqualizer | identity | 1e-4 + 1e-3 relative | defaults on flat in-gamut colours, with and without the filter (`DefaultsAreTheIdentityOnFlatColours`) |
| ColorEqualizer | render | 1e-4 + 1e-4 relative | the node's render against `process()` (`RenderMatchesTheKernel`) |
| ColorEqualizer | gpu | `ParityTolerance::make(2e-3, 1e-3)` | kernels against the CPU render, every control, downsampled guided filters, every output and preset (`NativeColorEqualizerGpu_Test.cpp`). Provisional until measured |

The node ports darktable's color equalizer (`src/iop/colorequal.c` and
`data/kernels/colorequal.cl`, release-5.6.2) with every parameter of
`dt_iop_colorequal_params_t` v4 and darktable's defaults. The colour-space helpers it shares with
other darktable ports live in `Engine/Nodes/Color/DtColorSpaces.h` and
`Engine/Gpu/Kernels/dtcolorspaces.slang`; the guided filters are built on the EdgeAware blurs.

## Parameters

| darktable field | Knob | Tab |
|---|---|---|
| hue[8] | hueRed, hueOrange, hueYellow, hueGreen, hueCyan, hueBlue, hueLavender, hueMagenta (degrees) | Hue |
| sat[8] | saturationRed ... saturationMagenta (gain) | Saturation |
| bright[8] | brightnessRed ... brightnessMagenta (gain) | Brightness |
| hue_shift | nodePlacement | Options |
| smoothing_hue | hueCurveSmoothing | Options |
| threshold | saturationThreshold | Options |
| contrast | contrast | Options |
| white_level | whiteLevel (EV) | Options |
| use_filter | useFilter | Options |
| chroma_size | chromaSize | Options |
| param_size | paramSize | Options |
| (GUI mask preview) | output: Result, Hue, Saturation, Brightness, Saturation Threshold, Brightness Threshold | Options |
| (presets) | preset and applyPreset | top |
| (graph) | curves (read-only preview) | top |

Values are darktable's internal ones: darktable's GUI shows saturation and brightness as
percentages around 0 %, the knobs hold the gain (0 % is 1, +50 % is 1.5). The presets are
darktable's four built-in looks; Apply writes every control but Output.

## Differences from darktable

- **Sliders, not a draggable graph.** darktable edits the three curves by dragging points on a
  graph whose x positions are fixed at the node hues, so each point is one value. The node keeps a
  knob per point, eight per tab, and draws the three interpolated curves, the hue ramp and the node
  positions in a read-only Curves preview above the tabs that redraws as the knobs move.
- **Fixed working space.** The node converts the working space to linear Rec.2020, darktable's
  pipeline space, equalizes, and converts back. A matrix working space uses a 3x3 matrix; any other
  working space goes through OCIO via XYZ D65 and renders on the CPU only.
- **Guided filter reach.** The node reads a halo about the render window large enough for the
  filters' gaussians; darktable's tiling reach is computed differently, so results within a few
  pixels of a tile edge may differ slightly. The downsampled filter grid is anchored at the buffer
  origin.
- **Overlays.** The threshold outputs draw their weighting curve across the region of definition.
  The node never reports itself as an identity: gamut mapping applies even at the defaults.
- **Not ported:** the colour picker that selects a node from the image and the on-image hue
  selection. They are a follow-up for "M165 - Rich Viewer Overlays", which provides the viewer
  interaction they need.
- **Alpha** passes through. Premult, mask and mix are the host's stages.

## GPU

The kernels (`Engine/Gpu/Kernels/colorequal.slang`) follow the CPU passes: chromaticity prepare,
covariance, the two guided filters (on a downsampled grid when the sizes allow) through the
EdgeAware GPU blurs, corrections and the final composite with every output. They read the same
LUTs and saturation weights. The GPU runs only in a matrix working space.
